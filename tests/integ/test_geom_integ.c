/* Tier-1 integration tests for the geom module.
 *
 * Safety rules obeyed here:
 *   - the only device this test creates is a loop device made by
 *     tests/harness/loopfix.sh over a private image file, torn down at the end;
 *   - nothing is ever written to a physical block device: the checks against
 *     the host's own root filesystem read /proc/mounts and /sys only, and the
 *     raw-offset verification reads the loop image *file*, never /dev/sd*;
 *   - a unique EXITOS_IMG / EXITOS_MNT per process keeps parallel test runs
 *     from colliding.
 *
 * Needs root (loop device + mount). Without root everything is skipped, not
 * failed.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/fiemap.h>
#include <linux/fs.h>

#include "exitos_geom.h"
#include "tap.h"

#ifndef FS_IOC_FIEMAP
#define FS_IOC_FIEMAP _IOWR('f', 11, struct fiemap)
#endif

#define SENTINEL 0xDEADBEEFCAFEBABEULL

/* Private, per-process fixture paths: "/tmp/exitos-geom-<pid>.img" and
 * ".mnt", so parallel test binaries never share an image or a mountpoint. */
static char g_img[128];
static char g_mnt[128];
static char g_loop[64];   /* "/dev/loopN" as reported by loopfix.sh */

/* ---- small helpers --------------------------------------------------- */

static int read_first_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static int read_sysfs_u64(const char *path, uint64_t *out)
{
    char line[64];
    if (read_first_line(path, line, sizeof line) != 0)
        return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(line, &end, 10);
    if (errno != 0 || end == line)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            n++;
    }
    closedir(d);
    return n == 0;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Disk that owns a block-device node name, e.g. "sda4" -> "sda",
 * "nvme0n1p1" -> "nvme0n1", "loop0" -> "loop0". Uses the sysfs topology
 * instead of guessing from the name. */
static int disk_of(const char *devname, char *out, size_t n)
{
    char link[PATH_MAX], target[PATH_MAX];
    snprintf(link, sizeof link, "/sys/class/block/%s", devname);
    ssize_t k = readlink(link, target, sizeof target - 1);
    if (k < 0)
        return -1;
    target[k] = '\0';
    /* .../block/sda/sda4  -> parent basename is the disk; for a whole disk
     * .../block/loop0 -> parent is "block", so the disk is the device itself */
    char *slash = strrchr(target, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    const char *parent = base_name(target);
    if (strcmp(parent, "block") == 0)
        snprintf(out, n, "%s", devname);
    else
        snprintf(out, n, "%s", parent);
    return 0;
}

static int path_exists(const char *p) { return access(p, F_OK) == 0; }

static int is_mounted(const char *target)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;
    char a[PATH_MAX], b[PATH_MAX], c[64];
    int hit = 0;
    while (fscanf(f, "%1023s %1023s %63s %*s %*d %*d\n", a, b, c) == 3)
        if (strcmp(b, target) == 0)
            hit = 1;
    fclose(f);
    return hit;
}

/* A detached loop device loses /sys/block/<name>/loop/backing_file. */
static int loop_is_attached(const char *devpath)
{
    char p[PATH_MAX];
    if (!devpath || !devpath[0])
        return 0;
    snprintf(p, sizeof p, "/sys/block/%s/loop/backing_file",
             devpath + (strncmp(devpath, "/dev/", 5) == 0 ? 5 : 0));
    return path_exists(p);
}

/* Expected translation, recomputed independently of the module. */
static uint64_t want_lba(const struct exitos_geom *g, uint64_t blk)
{
    __uint128_t start_bytes = (__uint128_t)g->part_start_sect * 512u;
    return (uint64_t)(start_bytes / g->lbs) +
           blk * (uint64_t)(g->fs_bs / g->lbs);
}

/* ---- fixture --------------------------------------------------------- */

static const char *find_loopfix(void)
{
    static const char *cands[] = {
        "tests/harness/loopfix.sh",
        "./tests/harness/loopfix.sh",
        "../harness/loopfix.sh",
        "../../tests/harness/loopfix.sh",
    };
    const char *env = getenv("EXITOS_LOOPFIX");
    if (env && access(env, X_OK) == 0)
        return env;
    for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++)
        if (access(cands[i], X_OK) == 0)
            return cands[i];
    return NULL;
}

/* Bring the fixture up; returns 0 and fills g_loop on success. */
static int fixture_up(void)
{
    const char *fix = find_loopfix();
    if (!fix)
        return -1;

    snprintf(g_img, sizeof g_img, "/tmp/exitos-geom-%d.img", (int)getpid());
    snprintf(g_mnt, sizeof g_mnt, "/tmp/exitos-geom-%d.mnt", (int)getpid());
    unlink(g_img);   /* our own private name; loopfix refuses if it exists */

    char cmd[PATH_MAX * 3];
    snprintf(cmd, sizeof cmd,
             "EXITOS_IMG=%s EXITOS_MNT=%s EXITOS_SIZE_MB=64 %s up 2>&1",
             g_img, g_mnt, fix);
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    char line[512];
    g_loop[0] = '\0';
    while (fgets(line, sizeof line, p)) {
        char *s = strstr(line, "LOOP=");
        if (s) {
            if (sscanf(s, "LOOP=%63s", g_loop) != 1)
                g_loop[0] = '\0';
        }
    }
    int rc = pclose(p);
    if (rc != 0 || g_loop[0] == '\0')
        return -1;
    return 0;
}

/* Targeted teardown: only ever touches the loop device this process created,
 * so a parallel test's fixture is never detached by accident. */
static void fixture_down(void)
{
    char cmd[512];
    int r;
    snprintf(cmd, sizeof cmd, "umount %s 2>/dev/null", g_mnt);
    r = system(cmd);
    (void)r;
    if (g_loop[0]) {
        snprintf(cmd, sizeof cmd, "losetup -d %s 2>/dev/null", g_loop);
        r = system(cmd);
        (void)r;
    }
    unlink(g_img);
    rmdir(g_mnt);
}

/* ---- tests on the loop fixture (whole device, start = 0) ------------- */

static void test_loop_probe(struct exitos_geom *g)
{
    const char *loopname = base_name(g_loop);
    char p[PATH_MAX];
    uint64_t sys_lbs = 0;

    memset(g, 0xA5, sizeof *g);
    int rc = exitos_geom_probe(g_mnt, g);
    T_EQ(rc, 0, "probe(%s) succeeds (rc=%d)", g_mnt, rc);
    if (rc != 0)
        return;

    T_EQ(g->devclass, EXITOS_DEV_LOOP, "loop mount is classified EXITOS_DEV_LOOP (got %s)",
         exitos_devclass_str(g->devclass) ? exitos_devclass_str(g->devclass) : "(null)");
    T_EQ(g->writable_raw, 1, "loop mount has writable_raw = 1");

    T_OK(strcmp(g->part_name, loopname) == 0,
         "part_name is \"%s\" (got \"%s\")", loopname, g->part_name);
    T_OK(strcmp(g->disk_path, g_loop) == 0,
         "disk_path is \"%s\" (got \"%s\")", g_loop, g->disk_path);

    /* A whole device has no /sys/class/block/<name>/start file at all, so the
     * only correct value is 0 - anything else shifts every write. */
    snprintf(p, sizeof p, "/sys/class/block/%s/start", loopname);
    T_OK(!path_exists(p), "%s does not exist (whole device, not a partition)", p);
    T_OK(g->part_start_sect == 0,
         "part_start_sect is 0 for a whole device (got %llu)",
         (unsigned long long)g->part_start_sect);

    snprintf(p, sizeof p, "/sys/block/%s/queue/logical_block_size", loopname);
    if (read_sysfs_u64(p, &sys_lbs) == 0)
        T_OK(g->lbs == sys_lbs, "lbs matches %s (%llu, got %u)", p,
             (unsigned long long)sys_lbs, g->lbs);
    else
        T_SKIP("cannot read %s", p);

    T_EQ(g->fs_bs, 4096, "fs_bs is 4096 (mkfs.ext4 -b 4096), got %u", g->fs_bs);
    struct statvfs vfs;
    if (statvfs(g_mnt, &vfs) == 0)
        T_OK(g->fs_bs == (uint32_t)vfs.f_bsize,
             "fs_bs matches statvfs f_bsize (%lu, got %u)",
             (unsigned long)vfs.f_bsize, g->fs_bs);
    else
        T_SKIP("statvfs(%s) failed: %s", g_mnt, strerror(errno));

    T_OK(g->lbs != 0 && g->fs_bs % g->lbs == 0,
         "fs_bs %u is a whole multiple of lbs %u", g->fs_bs, g->lbs);
}

/* Field-by-field equality (not memcmp: the char arrays may legitimately carry
 * different bytes after the terminating NUL). */
static void same_geom(const char *what, const struct exitos_geom *g,
                      const struct exitos_geom *ref)
{
    T_OK(strcmp(g->part_name, ref->part_name) == 0 &&
             strcmp(g->disk_path, ref->disk_path) == 0 &&
             g->part_start_sect == ref->part_start_sect &&
             g->lbs == ref->lbs && g->fs_bs == ref->fs_bs &&
             g->devclass == ref->devclass &&
             g->writable_raw == ref->writable_raw,
         "%s yields the same geom as the mountpoint "
         "(part=%s disk=%s start=%llu lbs=%u fs_bs=%u class=%d wr=%d)",
         what, g->part_name, g->disk_path,
         (unsigned long long)g->part_start_sect, g->lbs, g->fs_bs,
         (int)g->devclass, g->writable_raw);
}

/* Probing any path inside the filesystem must describe the same device. */
static void test_probe_inside_mount(const struct exitos_geom *ref)
{
    char sub[PATH_MAX], file[PATH_MAX];
    struct exitos_geom g;

    snprintf(sub, sizeof sub, "%s/subdir", g_mnt);
    snprintf(file, sizeof file, "%s/subdir/f.bin", g_mnt);
    if (mkdir(sub, 0755) != 0 && errno != EEXIST) {
        T_SKIP("cannot mkdir %s: %s", sub, strerror(errno));
        return;
    }
    int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        T_SKIP("cannot create %s: %s", file, strerror(errno));
        return;
    }
    (void)!write(fd, "x", 1);
    close(fd);

    memset(&g, 0x5A, sizeof g);
    int rc = exitos_geom_probe(sub, &g);
    T_EQ(rc, 0, "probe(<mount>/subdir) succeeds (rc=%d)", rc);
    if (rc == 0)
        same_geom("probe of a subdirectory", &g, ref);

    memset(&g, 0x5A, sizeof g);
    rc = exitos_geom_probe(file, &g);
    T_EQ(rc, 0, "probe(<mount>/subdir/f.bin) succeeds (rc=%d)", rc);
    if (rc == 0)
        same_geom("probe of a regular file", &g, ref);
}

static void test_translate_with_probed_geom(const struct exitos_geom *g)
{
    static const uint64_t blks[] = { 0, 1, 2, 1000, 65536 };
    for (size_t i = 0; i < sizeof blks / sizeof blks[0]; i++) {
        uint64_t lba = SENTINEL;
        int rc = exitos_geom_fsblock_to_lba(g, blks[i], &lba);
        uint64_t want = want_lba(g, blks[i]);
        T_EQ(rc, 0, "probed loop geom: translate blk=%llu (rc=%d)",
             (unsigned long long)blks[i], rc);
        T_OK(lba == want, "probed loop geom: blk=%llu -> lba=%llu (got %llu)",
             (unsigned long long)blks[i], (unsigned long long)want,
             (unsigned long long)lba);
    }

    /* Same geometry, permission withdrawn: must refuse and not write out. */
    struct exitos_geom ro = *g;
    ro.writable_raw = 0;
    uint64_t lba = SENTINEL;
    int rc = exitos_geom_fsblock_to_lba(&ro, 1000, &lba);
    T_EQ(rc, -EPERM, "probed geom with writable_raw=0 refuses (-EPERM, got %d)", rc);
    T_OK(lba == SENTINEL, "refusal leaves *lba untouched (got 0x%llx)",
         (unsigned long long)lba);
}

/* End-to-end: a file's ext4 block number, translated to an LBA, must point at
 * that file's bytes. The loop device maps byte X of the device to byte X of
 * the backing image, so we verify by reading the image file - no device node
 * is opened, and nothing is ever written outside the image. */
static void test_lba_points_at_the_data(const struct exitos_geom *g)
{
    char file[PATH_MAX];
    unsigned char pattern[4096], got[4096];
    snprintf(file, sizeof file, "%s/needle.bin", g_mnt);

    for (size_t i = 0; i < sizeof pattern; i++)
        pattern[i] = (unsigned char)(0x40 + ((i + (size_t)getpid()) % 61));
    memcpy(pattern, "EXITOS-GEOM-NEEDLE", 18);

    int fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        T_SKIP("cannot create %s: %s", file, strerror(errno));
        return;
    }
    if (write(fd, pattern, sizeof pattern) != (ssize_t)sizeof pattern ||
        fsync(fd) != 0) {
        T_SKIP("cannot write/fsync %s: %s", file, strerror(errno));
        close(fd);
        return;
    }

    char buf[sizeof(struct fiemap) + sizeof(struct fiemap_extent)]
        __attribute__((aligned(8)));
    struct fiemap *fm = (struct fiemap *)(void *)buf;
    memset(buf, 0, sizeof buf);
    fm->fm_start = 0;
    fm->fm_length = sizeof pattern;
    fm->fm_flags = FIEMAP_FLAG_SYNC;
    fm->fm_extent_count = 1;
    if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0 || fm->fm_mapped_extents < 1) {
        T_SKIP("FIEMAP unavailable on this fs: %s", strerror(errno));
        close(fd);
        return;
    }
    close(fd);
    sync();

    struct fiemap_extent *fe = &fm->fm_extents[0];
    if (fe->fe_flags & (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DATA_INLINE |
                        FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DELALLOC)) {
        T_SKIP("first extent is not a plain on-disk extent (flags 0x%x)",
               fe->fe_flags);
        return;
    }

    uint64_t fs_block = fe->fe_physical / g->fs_bs;
    uint64_t lba = SENTINEL;
    int rc = exitos_geom_fsblock_to_lba(g, fs_block, &lba);
    T_EQ(rc, 0, "translate the file's own ext4 block %llu (rc=%d)",
         (unsigned long long)fs_block, rc);
    if (rc != 0)
        return;
    T_OK(lba == want_lba(g, fs_block),
         "file block %llu -> lba %llu (got %llu)", (unsigned long long)fs_block,
         (unsigned long long)want_lba(g, fs_block), (unsigned long long)lba);

    int ifd = open(g_img, O_RDONLY);
    if (ifd < 0) {
        T_SKIP("cannot reopen image %s: %s", g_img, strerror(errno));
        return;
    }
    off_t off = (off_t)(lba * (uint64_t)g->lbs);
    ssize_t n = pread(ifd, got, sizeof got, off);
    close(ifd);
    T_EQ(n, (ssize_t)sizeof got, "read 4096 B at byte offset %lld of the image",
         (long long)off);
    if (n == (ssize_t)sizeof got)
        T_OK(memcmp(got, pattern, sizeof got) == 0,
             "bytes at the translated LBA are exactly the file's contents");
}

/* ---- host checks: read-only, no device is ever written --------------- */

/* If the host root filesystem sits on a plain partition, geom must report it
 * as EXITOS_DEV_OK with the partition's real start sector. This is the only
 * place a non-zero part_start_sect is checked against reality. */
static void test_host_root_partition(void)
{
    char src[PATH_MAX] = "", fstype[64] = "";
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        T_SKIP("cannot open /proc/mounts");
        return;
    }
    char a[PATH_MAX], b[PATH_MAX], c[64];
    while (fscanf(f, "%1023s %1023s %63s %*s %*d %*d\n", a, b, c) == 3) {
        if (strcmp(b, "/") == 0) {
            snprintf(src, sizeof src, "%s", a);
            snprintf(fstype, sizeof fstype, "%s", c);
        }
    }
    fclose(f);

    struct stat st;
    if (src[0] != '/' || stat(src, &st) != 0 || !S_ISBLK(st.st_mode)) {
        T_SKIP("host root (%s, %s) is not backed by a block device", src, fstype);
        return;
    }

    char name[64], disk[64], p[PATH_MAX];
    snprintf(name, sizeof name, "%s", base_name(src));
    snprintf(p, sizeof p, "/sys/class/block/%s/start", name);
    uint64_t start = 0;
    if (read_sysfs_u64(p, &start) != 0) {
        T_SKIP("host root %s is not a partition (no %s)", src, p);
        return;
    }
    if (disk_of(name, disk, sizeof disk) != 0) {
        T_SKIP("cannot resolve the disk behind %s", name);
        return;
    }
    snprintf(p, sizeof p, "/sys/block/%s/holders", disk);
    if (dir_is_empty(p) != 1) {
        T_SKIP("host root disk %s has holders; not a plain partition", disk);
        return;
    }
    snprintf(p, sizeof p, "/sys/block/%s/dm", disk);
    if (path_exists(p)) {
        T_SKIP("host root disk %s is device-mapper", disk);
        return;
    }
    snprintf(p, sizeof p, "/sys/block/%s/md", disk);
    if (path_exists(p)) {
        T_SKIP("host root disk %s is md raid", disk);
        return;
    }

    struct exitos_geom g;
    memset(&g, 0x3C, sizeof g);
    int rc = exitos_geom_probe("/", &g);
    T_EQ(rc, 0, "probe(\"/\") succeeds on a plain partition (rc=%d)", rc);
    if (rc != 0)
        return;
    T_EQ(g.devclass, EXITOS_DEV_OK, "host root partition %s is EXITOS_DEV_OK", src);
    T_EQ(g.writable_raw, 1, "host root partition has writable_raw = 1");
    T_OK(strcmp(g.part_name, name) == 0, "part_name is \"%s\" (got \"%s\")",
         name, g.part_name);
    char want_disk[PATH_MAX];
    snprintf(want_disk, sizeof want_disk, "/dev/%s", disk);
    T_OK(strcmp(g.disk_path, want_disk) == 0, "disk_path is \"%s\" (got \"%s\")",
         want_disk, g.disk_path);
    T_OK(g.part_start_sect == start,
         "part_start_sect equals /sys/class/block/%s/start = %llu (got %llu)",
         name, (unsigned long long)start,
         (unsigned long long)g.part_start_sect);

    snprintf(p, sizeof p, "/sys/block/%s/queue/logical_block_size", disk);
    uint64_t sys_lbs = 0;
    if (read_sysfs_u64(p, &sys_lbs) == 0)
        T_OK(g.lbs == sys_lbs, "lbs matches %s (%llu, got %u)", p,
             (unsigned long long)sys_lbs, g.lbs);
    else
        T_SKIP("cannot read %s", p);
    T_OK(g.fs_bs > 0 && g.lbs > 0 && g.fs_bs % g.lbs == 0,
         "host root fs_bs %u is a whole multiple of lbs %u", g.fs_bs, g.lbs);

    uint64_t lba = SENTINEL;
    rc = exitos_geom_fsblock_to_lba(&g, 1, &lba);
    T_EQ(rc, 0, "host root geom translates blk=1 (rc=%d)", rc);
    T_OK(lba == want_lba(&g, 1),
         "host root blk=1 -> start*512/%u + %u/%u = %llu (got %llu)",
         g.lbs, g.fs_bs, g.lbs,
         (unsigned long long)want_lba(&g, 1), (unsigned long long)lba);
}

/* Any mounted dm/md device present on this host must be refused. Nothing is
 * created here: hard rule - the only device this test may make is the loop
 * fixture. If the host has no stacked device, the case is skipped. */
static void test_host_stacked_devices_are_refused(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        T_SKIP("cannot open /proc/mounts");
        return;
    }
    char a[PATH_MAX], b[PATH_MAX], c[64], p[PATH_MAX];
    int found = 0;
    while (fscanf(f, "%1023s %1023s %63s %*s %*d %*d\n", a, b, c) == 3) {
        struct stat st;
        if (strncmp(a, "/dev/", 5) != 0 || stat(a, &st) != 0 ||
            !S_ISBLK(st.st_mode))
            continue;
        int is_dm = 0, is_md = 0;
        snprintf(p, sizeof p, "/sys/dev/block/%u:%u/dm",
                 major(st.st_rdev), minor(st.st_rdev));
        is_dm = path_exists(p);
        snprintf(p, sizeof p, "/sys/dev/block/%u:%u/md",
                 major(st.st_rdev), minor(st.st_rdev));
        is_md = path_exists(p);
        if (!is_dm && !is_md)
            continue;

        found = 1;
        struct exitos_geom g;
        memset(&g, 0x11, sizeof g);
        int rc = exitos_geom_probe(b, &g);
        /* Failing the probe outright is an acceptable way to refuse; what is
         * never acceptable is reporting the stacked device as writable. */
        T_OK(rc != 0 || g.writable_raw == 0,
             "stacked device %s (%s) is never reported writable_raw=1 (rc=%d)",
             a, b, rc);
        if (rc != 0)
            continue;
        T_OK(g.devclass == (is_dm ? EXITOS_DEV_DM : EXITOS_DEV_MD),
             "%s (%s) is classified %s", b, a,
             is_dm ? "EXITOS_DEV_DM" : "EXITOS_DEV_MD");
        T_EQ(g.writable_raw, 0, "stacked device %s has writable_raw = 0", a);
        uint64_t lba = SENTINEL;
        rc = exitos_geom_fsblock_to_lba(&g, 1, &lba);
        T_EQ(rc, -EPERM, "stacked device %s refuses translation (-EPERM, got %d)",
             a, rc);
        T_OK(lba == SENTINEL, "stacked device %s leaves *lba untouched", a);
    }
    fclose(f);
    if (!found)
        T_SKIP("no mounted device-mapper/md device on this host; "
               "creating one is forbidden by the tier-1 safety rules");
}

/* A filesystem with no block device behind it must never be reported as raw
 * writable. */
static void test_non_block_backed_fs(void)
{
    struct exitos_geom g;
    memset(&g, 0x77, sizeof g);
    int rc = exitos_geom_probe("/proc/self", &g);
    T_OK(rc != 0 || g.writable_raw == 0,
         "probe(/proc/self) on procfs never reports writable_raw=1 "
         "(rc=%d, writable_raw=%d)", rc, rc == 0 ? g.writable_raw : 0);
    if (rc == 0) {
        uint64_t lba = SENTINEL;
        int r2 = exitos_geom_fsblock_to_lba(&g, 1, &lba);
        T_EQ(r2, -EPERM, "procfs geom refuses translation (-EPERM, got %d)", r2);
        T_OK(lba == SENTINEL, "procfs refusal leaves *lba untouched");
    }
}

static void test_bad_paths(void)
{
    struct exitos_geom g;
    char miss[PATH_MAX];
    snprintf(miss, sizeof miss, "/tmp/exitos-no-such-path-%d", (int)getpid());

    memset(&g, 0x22, sizeof g);
    int rc = exitos_geom_probe(miss, &g);
    T_OK(rc != 0, "probe of a non-existent path fails (rc=%d)", rc);
    if (rc == 0)
        T_OK(g.writable_raw == 0,
             "a probe that returns 0 for a non-existent path must not be writable");

    memset(&g, 0x22, sizeof g);
    rc = exitos_geom_probe("", &g);
    T_OK(rc != 0 || g.writable_raw == 0,
         "probe(\"\") does not report a writable device (rc=%d)", rc);
}

int main(void)
{
    if (!t_need_root()) {
        T_SKIP("tier-1 geom tests need root (loop device + mount)");
        T_DONE();
    }
    if (fixture_up() != 0) {
        T_SKIP("loop fixture could not be brought up (loopfix.sh up failed)");
        T_DONE();
    }

    struct exitos_geom loopgeom;
    memset(&loopgeom, 0, sizeof loopgeom);

    test_loop_probe(&loopgeom);
    if (loopgeom.lbs != 0 && loopgeom.fs_bs != 0) {
        test_probe_inside_mount(&loopgeom);
        test_translate_with_probed_geom(&loopgeom);
        test_lba_points_at_the_data(&loopgeom);
    } else {
        T_SKIP("probe of the loop mount did not yield a usable geom; "
               "skipping the tests that build on it");
    }

    test_host_root_partition();
    test_host_stacked_devices_are_refused();
    test_non_block_backed_fs();
    test_bad_paths();

    fixture_down();
    T_OK(access(g_img, F_OK) != 0, "fixture image %s removed", g_img);
    T_OK(!is_mounted(g_mnt), "fixture mountpoint %s unmounted", g_mnt);
    T_OK(!loop_is_attached(g_loop), "loop device %s detached", g_loop);
    T_DONE();
}
