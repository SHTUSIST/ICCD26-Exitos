/* geom: filesystem block -> device LBA translation, plus the refusal rules
 * that keep that translation from being applied where it is meaningless.
 *
 * The translation itself is one line:
 *
 *     lba = part_start_sect * 512 / lbs + fs_block * (fs_bs / lbs)
 *
 * It is only correct when the filesystem sits *directly* on a partition (or a
 * whole disk, or a loop device), because only then is the mapping from the
 * filesystem's address space to the device's address space a constant shift.
 * On device-mapper, md RAID, or anything we failed to identify, the mapping is
 * arbitrary and a raw write at the computed LBA lands on unrelated data. Those
 * cases are refused with -EPERM, and the caller's output variable is left
 * untouched so that a caller which ignores the return code cannot silently use
 * a stale or half-written LBA.
 *
 * Everything this file reads from the system is read-only: stat(), statvfs(),
 * and files under /sys. No block device is ever opened here.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include "exitos_geom.h"
#include "exitos_iopath.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ------------------------------------------------------------------ *
 * small, allocation-free helpers
 * ------------------------------------------------------------------ */

/* Bounded copy that never truncates silently into a wrong device name:
 * callers check the length first, this only guarantees NUL termination. */
static void copy_str(char *dst, size_t n, const char *src)
{
    size_t len = strlen(src);
    if (len >= n)
        len = n - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int path_exists(const char *p)
{
    return access(p, F_OK) == 0;
}

/* Linux exposes a partition's start attribute in 512-byte sectors regardless
 * of the device's native logical-block size.  Convert that ABI unit without a
 * potentially overflowing `start * 512`, and reject a partition boundary that
 * cannot be represented as a whole native LBA. */
static int partition_start_lba(const struct exitos_geom *g, uint64_t *out)
{
    uint64_t a, b, gcd, denominator, multiplier, q;

    if (!g || !out || g->lbs == 0)
        return -EINVAL;
    a = 512;
    b = g->lbs;
    while (b != 0) {
        uint64_t r = a % b;
        a = b;
        b = r;
    }
    gcd = a;
    denominator = (uint64_t)g->lbs / gcd;
    multiplier = 512 / gcd;
    if (g->part_start_sect % denominator != 0)
        return -EINVAL;
    q = g->part_start_sect / denominator;
    if (q > UINT64_MAX / multiplier)
        return -EOVERFLOW;
    *out = q * multiplier;
    return 0;
}

/* Read the first line of a small sysfs file. Returns 0 on success. */
static int read_first_line(const char *path, char *buf, size_t n)
{
    int fd = exitos_internal_open_call(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    ssize_t k;
    do {
        k = read(fd, buf, n - 1);
    } while (k < 0 && errno == EINTR);

    if (exitos_internal_close_call(fd) != 0 && k >= 0) {
        /* A failing close() on a read-only sysfs file is not expected; treat
         * it as a failed read rather than trusting the buffer. */
        return -1;
    }
    if (k <= 0)
        return -1;

    buf[k] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static int read_sysfs_u64(const char *path, uint64_t *out)
{
    char line[64];
    char *end = NULL;

    if (read_first_line(path, line, sizeof line) != 0)
        return -1;

    errno = 0;
    unsigned long long v = strtoull(line, &end, 10);
    if (errno != 0 || end == line)
        return -1;

    *out = (uint64_t)v;
    return 0;
}

/* 1 = empty, 0 = has entries, -1 = cannot tell. */
static int dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return -1;

    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            n++;
    }
    closedir(d);
    return n == 0;
}

/* Read an unsigned number out of "<dir>/<leaf>". Returns 0 on success. */
static int read_sysfs_u64_from(const char *dir, const char *leaf, uint64_t *out)
{
    char p[PATH_MAX];
    int n = snprintf(p, sizeof p, "%s/%s", dir, leaf);
    if (n < 0 || (size_t)n >= sizeof p)
        return -1;
    return read_sysfs_u64(p, out);
}

/* "<dir>/<leaf>" exists? */
static int sub_exists(const char *dir, const char *leaf)
{
    char p[PATH_MAX];
    int n = snprintf(p, sizeof p, "%s/%s", dir, leaf);
    if (n < 0 || (size_t)n >= sizeof p)
        return 0;
    return path_exists(p);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* ------------------------------------------------------------------ *
 * probe
 * ------------------------------------------------------------------ */

/* Split the sysfs symlink target of a block device into its own kernel name
 * and the name of the disk that owns it:
 *
 *   ../../devices/.../block/sda/sda4  -> name "sda4", disk "sda"
 *   ../../devices/virtual/block/loop3 -> name "loop3", disk "loop3"
 *
 * The parent directory is what distinguishes a partition from a whole device:
 * for a whole device the parent is literally "block".  `target` is modified.
 */
static int split_dev_target(char *target, char *name, size_t nname,
                            char *disk, size_t ndisk)
{
    const char *leaf = base_name(target);
    if (leaf[0] == '\0' || strlen(leaf) >= nname)
        return -1;
    copy_str(name, nname, leaf);

    char *slash = strrchr(target, '/');
    if (!slash) {
        /* No parent component at all: treat the device as its own disk. */
        if (strlen(name) >= ndisk)
            return -1;
        copy_str(disk, ndisk, name);
        return 0;
    }
    *slash = '\0';

    const char *parent = base_name(target);
    const char *d = (strcmp(parent, "block") == 0) ? name : parent;
    if (d[0] == '\0' || strlen(d) >= ndisk)
        return -1;
    copy_str(disk, ndisk, d);
    return 0;
}

/* Stacking test for one sysfs directory (either the device itself or the disk
 * that owns it).  Returns the class implied by that directory, or
 * EXITOS_DEV_OK when it shows no sign of stacking. */
static exitos_dev_class class_of_sysfs_dir(const char *dir)
{
    if (sub_exists(dir, "dm"))
        return EXITOS_DEV_DM;
    if (sub_exists(dir, "md"))
        return EXITOS_DEV_MD;
    if (sub_exists(dir, "loop"))
        return EXITOS_DEV_LOOP;
    return EXITOS_DEV_OK;
}

int exitos_geom_probe(const char *path, struct exitos_geom *out)
{
    struct stat st;
    struct statvfs vfs;
    char devdir[PATH_MAX], diskdir[PATH_MAX], target[PATH_MAX], tmp[PATH_MAX];
    char name[64], disk[64];
    uint64_t v = 0;
    int n;

    if (!out)
        return -EINVAL;

    /* Fail closed: even on an error return the caller sees a struct that
     * refuses translation rather than uninitialised stack bytes. */
    memset(out, 0, sizeof *out);
    out->devclass = EXITOS_DEV_UNKNOWN;
    out->writable_raw = 0;

    if (!path || path[0] == '\0')
        return -EINVAL;

    if (stat(path, &st) != 0)
        return -errno;
    if (statvfs(path, &vfs) != 0)
        return -errno;

    out->fs_bs = (uint32_t)vfs.f_bsize;

    /* Anonymous device numbers (major 0) belong to filesystems with no block
     * device behind them: procfs, sysfs, tmpfs, overlayfs, network mounts.
     * There is nothing to translate to, so the geom stays UNKNOWN. */
    unsigned int maj = major(st.st_dev);
    unsigned int min = minor(st.st_dev);
    if (maj == 0)
        return 0;

    n = snprintf(devdir, sizeof devdir, "/sys/dev/block/%u:%u", maj, min);
    if (n < 0 || (size_t)n >= sizeof devdir)
        return 0;

    ssize_t k = readlink(devdir, target, sizeof target - 1);
    if (k < 0)
        return 0;                       /* no sysfs entry -> cannot identify */
    target[k] = '\0';

    if (split_dev_target(target, name, sizeof name, disk, sizeof disk) != 0)
        return 0;                       /* name too long to store faithfully */

    n = snprintf(diskdir, sizeof diskdir, "/sys/block/%s", disk);
    if (n < 0 || (size_t)n >= sizeof diskdir)
        return 0;

    /* part_name / disk_path.  A truncated name would name the wrong device,
     * so refuse instead of storing a prefix. */
    n = snprintf(tmp, sizeof tmp, "/dev/%s", disk);
    if (n < 0 || (size_t)n >= sizeof tmp ||
        strlen(name) >= sizeof out->part_name ||
        strlen(tmp) >= sizeof out->disk_path)
        return 0;
    copy_str(out->part_name, sizeof out->part_name, name);
    copy_str(out->disk_path, sizeof out->disk_path, tmp);

    /* Partition start.  A whole device (loop0, nvme0n1, sda) has no "start"
     * file at all; 0 is then the only correct offset. */
    if (read_sysfs_u64_from(devdir, "start", &v) == 0)
        out->part_start_sect = v;
    else
        out->part_start_sect = 0;

    /* Logical block size lives on the disk's request queue; partitions do not
     * have a queue/ directory of their own. */
    if (read_sysfs_u64_from(diskdir, "queue/logical_block_size", &v) == 0 ||
        read_sysfs_u64_from(devdir, "queue/logical_block_size", &v) == 0)
        out->lbs = (uint32_t)v;
    else
        out->lbs = 0;

    /* Classification.  Check the device itself first (a filesystem mounted on
     * /dev/dm-0 or /dev/md0 is stacked no matter what lies underneath), then
     * the disk that owns it (a partition of an md array is stacked too). */
    exitos_dev_class c = class_of_sysfs_dir(devdir);
    if (c == EXITOS_DEV_OK)
        c = class_of_sysfs_dir(diskdir);

    /* A holder is another block device layered on top of this one - dm-crypt,
     * LVM, md.  We cannot know whether the caller means this device or the
     * stack above it, so we decline to guess. */
    if (c == EXITOS_DEV_OK || c == EXITOS_DEV_LOOP) {
        char holders[PATH_MAX];
        n = snprintf(holders, sizeof holders, "%s/holders", devdir);
        if (n < 0 || (size_t)n >= sizeof holders || dir_is_empty(holders) != 1)
            c = EXITOS_DEV_UNKNOWN;
    }
    if (c == EXITOS_DEV_OK || c == EXITOS_DEV_LOOP) {
        char holders[PATH_MAX];
        n = snprintf(holders, sizeof holders, "%s/holders", diskdir);
        if (n < 0 || (size_t)n >= sizeof holders || dir_is_empty(holders) != 1)
            c = EXITOS_DEV_UNKNOWN;
    }

    /* An allowed class is worth nothing if the geometry cannot express the
     * translation: lbs must be known and fs_bs must be a whole multiple of it,
     * otherwise fs_block * (fs_bs / lbs) is not the file's real offset. */
    if (c == EXITOS_DEV_OK || c == EXITOS_DEV_LOOP) {
        uint64_t start_lba;
        if (out->lbs == 0 || out->fs_bs == 0 ||
            out->fs_bs % out->lbs != 0 ||
            partition_start_lba(out, &start_lba) != 0)
            c = EXITOS_DEV_UNKNOWN;
    }

    out->devclass = c;
    out->writable_raw = (c == EXITOS_DEV_OK || c == EXITOS_DEV_LOOP) ? 1 : 0;
    return 0;
}

/* ------------------------------------------------------------------ *
 * translation
 * ------------------------------------------------------------------ */

int exitos_geom_fsblock_to_lba(const struct exitos_geom *g,
                               uint64_t fs_block, uint64_t *lba)
{
    if (!g || !lba)
        return -EINVAL;

    /* The permission gate runs before anything else, including the division:
     * a refused geom may legitimately carry lbs = 0. */
    if (g->writable_raw != 1)
        return -EPERM;
    if (g->devclass != EXITOS_DEV_OK && g->devclass != EXITOS_DEV_LOOP)
        return -EPERM;   /* the header's invariant, enforced a second time */

    if (g->lbs == 0 || g->fs_bs == 0 || g->fs_bs % g->lbs != 0)
        return -EINVAL;

    uint64_t factor = (uint64_t)g->fs_bs / (uint64_t)g->lbs;   /* >= 1 here */
    uint64_t start_lba;
    int rc = partition_start_lba(g, &start_lba);
    if (rc != 0)
        return rc;

    /* Wrapping past 2^64 would produce a small, plausible-looking LBA that
     * points at the start of the device. Refuse instead. */
    if (fs_block > UINT64_MAX / factor)
        return -EOVERFLOW;
    uint64_t off = fs_block * factor;
    if (off > UINT64_MAX - start_lba)
        return -EOVERFLOW;

    *lba = start_lba + off;
    return 0;
}

const char *exitos_devclass_str(exitos_dev_class c)
{
    switch (c) {
    case EXITOS_DEV_OK:      return "EXITOS_DEV_OK";
    case EXITOS_DEV_DM:      return "EXITOS_DEV_DM";
    case EXITOS_DEV_MD:      return "EXITOS_DEV_MD";
    case EXITOS_DEV_LOOP:    return "EXITOS_DEV_LOOP";
    case EXITOS_DEV_UNKNOWN: return "EXITOS_DEV_UNKNOWN";
    }
    /* Not one of the five values the contract defines; never claim it is
     * writable by returning one of the allowed labels. */
    return "EXITOS_DEV_INVALID";
}
