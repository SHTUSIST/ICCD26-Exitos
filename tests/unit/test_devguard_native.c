#include "tap.h"

/* Including the public header twice must be harmless.  Disk pin declarations
 * used to sit below the include guard and redefined struct devguard_pin. */
#include "exitos_devguard.h"
#include "exitos_devguard.h"
#include "exitos_iopath.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fs.h>

static int mock_block_fd = -1;
static dev_t mock_block_rdev;
static int mock_block_fd2 = -1;
static dev_t mock_block_rdev2;
static int mock_short_pread_fd = -1;
static unsigned mock_short_pread_calls;
static int mock_direct_pread_fd = -1;
static size_t mock_direct_pread_alignment;
static unsigned mock_direct_pread_calls;
static unsigned mock_misaligned_pread_calls;
static uint64_t mock_block_diskseq_override;
static unsigned mock_block_diskseq_calls;
static unsigned mock_block_diskseq_fail_call;
static unsigned mock_block_diskseq_change_call;
static uint64_t mock_block_diskseq_changed_value;
static char deny_absolute_metadata_root[PATH_MAX];
static unsigned denied_absolute_metadata_opens;

struct sysfs_generation_swap {
    int armed;
    int attempted;
    int swapped;
    int b_size_opened;
    int retained_start_opened;
    int retained_size_opened;
    int restored;
    char trigger_path[PATH_MAX];
    char dev_link[PATH_MAX];
    char b_size_path[PATH_MAX];
    char a_dir[PATH_MAX];
    char a_target[PATH_MAX];
    char b_target[PATH_MAX];
};

static struct sysfs_generation_swap sysfs_generation_swap;

struct pin_parent_swap {
    int armed;
    int attempted;
    int succeeded;
    char parent[PATH_MAX];
    char moved[PATH_MAX];
    char attacker[PATH_MAX];
};

static struct pin_parent_swap pin_parent_swap;

struct pin_load_parent_swap {
    int armed;
    int attempted;
    int succeeded;
    char pin_path[PATH_MAX];
    char leaf[NAME_MAX + 1];
    char parent[PATH_MAX];
    char moved[PATH_MAX];
    char attacker[PATH_MAX];
};

static struct pin_load_parent_swap pin_load_parent_swap;

/* Pure fake-sysfs ABA seam.  The initial fd resolver has already retained the
 * A serial file when the /dev/block link is switched to B.  If code later
 * opens B's size by pathname, restore the link to A while that B fd remains
 * open.  A pathname-only post-check therefore sees A again. */
int exitos_internal_open_call(const char *path, int flags, mode_t mode)
{
    int fd;
    int saved;

    if (path && deny_absolute_metadata_root[0] != '\0') {
        size_t root_len = strlen(deny_absolute_metadata_root);
        if (strncmp(path, deny_absolute_metadata_root, root_len) == 0 &&
            strcmp(path + root_len, "/devices") != 0 &&
            strncmp(path + root_len, "/devices/", 9) == 0) {
            denied_absolute_metadata_opens++;
            errno = EPERM;
            return -1;
        }
    }
    fd = (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
    saved = errno;

    if (fd >= 0 && sysfs_generation_swap.armed && path) {
        if (sysfs_generation_swap.swapped &&
                   !sysfs_generation_swap.restored &&
                   strcmp(path, sysfs_generation_swap.b_size_path) == 0) {
            sysfs_generation_swap.b_size_opened = 1;
            if (unlink(sysfs_generation_swap.dev_link) == 0 &&
                symlink(sysfs_generation_swap.a_target,
                        sysfs_generation_swap.dev_link) == 0)
                sysfs_generation_swap.restored = 1;
        }
    }
    errno = saved;
    return fd;
}

/* Move the dev_t link only after the helper's first canonical-inode check.
 * A retained-dirfd implementation still opens A/start and A/size.  Restoring
 * on the second retained read creates a real A->B->A window: pathname-based
 * mutants read B geometry while the hardened implementation returns A. */
static void maybe_swap_sysfs_generation_at(int dirfd, const char *path)
{
    char proc[64], actual[PATH_MAX];
    ssize_t n;

    if (!sysfs_generation_swap.armed || !path ||
        (strcmp(path, "start") != 0 && strcmp(path, "size") != 0))
        return;
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", dirfd);
    n = readlink(proc, actual, sizeof actual - 1);
    if (n <= 0)
        return;
    actual[n] = '\0';
    if (strcmp(actual, sysfs_generation_swap.a_dir) != 0)
        return;
    if (!sysfs_generation_swap.attempted && strcmp(path, "start") == 0) {
        sysfs_generation_swap.attempted = 1;
        sysfs_generation_swap.retained_start_opened = 1;
        if (unlink(sysfs_generation_swap.dev_link) == 0 &&
            symlink(sysfs_generation_swap.b_target,
                    sysfs_generation_swap.dev_link) == 0)
            sysfs_generation_swap.swapped = 1;
    } else if (sysfs_generation_swap.swapped &&
               !sysfs_generation_swap.restored && strcmp(path, "size") == 0) {
        sysfs_generation_swap.retained_size_opened = 1;
        if (unlink(sysfs_generation_swap.dev_link) == 0 &&
            symlink(sysfs_generation_swap.a_target,
                    sysfs_generation_swap.dev_link) == 0)
            sysfs_generation_swap.restored = 1;
    }
}

static void maybe_swap_pin_load_parent(int dirfd, const char *path)
{
    char proc[64], actual[PATH_MAX];
    ssize_t n;

    if (!pin_load_parent_swap.armed || pin_load_parent_swap.attempted || !path)
        return;
    if (dirfd == AT_FDCWD) {
        if (strcmp(path, pin_load_parent_swap.pin_path) != 0)
            return;
    } else {
        if (strcmp(path, pin_load_parent_swap.leaf) != 0)
            return;
        snprintf(proc, sizeof proc, "/proc/self/fd/%d", dirfd);
        n = readlink(proc, actual, sizeof actual - 1);
        if (n <= 0)
            return;
        actual[n] = '\0';
        if (strcmp(actual, pin_load_parent_swap.parent) != 0)
            return;
    }
    pin_load_parent_swap.attempted = 1;
    if (syscall(SYS_renameat, AT_FDCWD, pin_load_parent_swap.parent,
                AT_FDCWD, pin_load_parent_swap.moved) == 0 &&
        symlink(pin_load_parent_swap.attacker,
                pin_load_parent_swap.parent) == 0)
        pin_load_parent_swap.succeeded = 1;
}

/* The old loader calls fopen(path), while the hardened loader opens a retained
 * parent and then calls openat(parent_fd, leaf).  Exercising the race at both
 * syscall shapes proves the regression test fails before the implementation
 * and keeps testing the actual post-fix boundary. */
FILE *fopen(const char *path, const char *mode)
{
    int flags, fd, saved;

    maybe_swap_pin_load_parent(AT_FDCWD, path);
    if (!mode || mode[0] == '\0') { errno = EINVAL; return NULL; }
    if (mode[0] == 'r') flags = strchr(mode, '+') ? O_RDWR : O_RDONLY;
    else if (mode[0] == 'w') flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) |
                                    O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) |
                                    O_CREAT | O_APPEND;
    else { errno = EINVAL; return NULL; }
    flags |= O_CLOEXEC;
    fd = (int)syscall(SYS_openat, AT_FDCWD, path, flags, 0666);
    if (fd < 0) return NULL;
    FILE *result = fdopen(fd, mode);
    if (result) return result;
    saved = errno;
    close(fd);
    errno = saved;
    return NULL;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    va_list ap;

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    maybe_swap_sysfs_generation_at(dirfd, path);
    maybe_swap_pin_load_parent(dirfd, path);
    return (int)syscall(SYS_openat, dirfd, path, flags, mode);
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (request == BLKGETDISKSEQ &&
        (fd == mock_block_fd || fd == mock_block_fd2)) {
        uint64_t seq;
        dev_t rdev = fd == mock_block_fd ? mock_block_rdev : mock_block_rdev2;
        mock_block_diskseq_calls++;
        if (mock_block_diskseq_fail_call == mock_block_diskseq_calls) {
            errno = ENOTTY;
            return -1;
        }
        seq = mock_block_diskseq_override ? mock_block_diskseq_override :
              (minor(rdev) == 1 ? UINT64_C(202) : UINT64_C(101));
        if (mock_block_diskseq_change_call != 0 &&
            mock_block_diskseq_calls >= mock_block_diskseq_change_call)
            seq = mock_block_diskseq_changed_value;
        if (!arg) { errno = EFAULT; return -1; }
        *(uint64_t *)arg = seq;
        return 0;
    }
    return (int)syscall(SYS_ioctl, fd, request, arg);
}

/* Pure unit race seam: when pin_save fsyncs its completed temporary file,
 * replace the pathname of its parent with an attacker-controlled symlink and
 * plant the same temporary basename there.  A pathname-based rename then
 * overwrites the attacker's victim; a rename relative to the already-opened
 * parent dirfd stays on the original directory object. */
int fsync(int fd)
{
    if (pin_parent_swap.armed && !pin_parent_swap.attempted) {
        char proc[64], actual[PATH_MAX], planted[PATH_MAX];
        const char *base;
        size_t parent_len = strlen(pin_parent_swap.parent);
        ssize_t n;

        snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
        n = readlink(proc, actual, sizeof actual - 1);
        if (n > 0) {
            actual[n] = '\0';
            base = strrchr(actual, '/');
            if (base && strncmp(actual, pin_parent_swap.parent, parent_len) == 0 &&
                actual[parent_len] == '/' &&
                strncmp(base + 1, ".exitos-pin-", 12) == 0) {
                int planted_fd;

                pin_parent_swap.attempted = 1;
                if (snprintf(planted, sizeof planted, "%s/%s",
                             pin_parent_swap.attacker, base + 1) <
                        (int)sizeof planted) {
                    int planted_ok = 0;

                    planted_fd = open(planted, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                    if (planted_fd >= 0) {
                        planted_ok = write(planted_fd, "attacker-temp", 13) == 13;
                        if (close(planted_fd) != 0) planted_ok = 0;
                        planted_fd = -1;
                    }
                    if (planted_ok &&
                        syscall(SYS_renameat, AT_FDCWD, pin_parent_swap.parent,
                                AT_FDCWD, pin_parent_swap.moved) == 0 &&
                        symlink(pin_parent_swap.attacker,
                                pin_parent_swap.parent) == 0)
                        pin_parent_swap.succeeded = 1;
                }
            }
        }
    }
    return (int)syscall(SYS_fsync, fd);
}

/* Pure unit seam: only the explicitly selected scratch descriptor is made to
 * look like a block device.  No device node is opened and no block write can
 * occur in this test. */
int fstat(int fd, struct stat *st)
{
    int rc = (int)syscall(SYS_fstat, fd, st);
    if (rc == 0 && fd == mock_block_fd) {
        st->st_mode = (st->st_mode & ~S_IFMT) | S_IFBLK;
        st->st_rdev = mock_block_rdev;
    } else if (rc == 0 && fd == mock_block_fd2) {
        st->st_mode = (st->st_mode & ~S_IFMT) | S_IFBLK;
        st->st_rdev = mock_block_rdev2;
    }
    return rc;
}

/* Observable short-read seam for pin sampling.  The backing file remains
 * large enough for every requested sample; only this selected descriptor's
 * actual pread is shortened, proving the capture reached and rejected I/O. */
ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    if (fd == mock_direct_pread_fd) {
        mock_direct_pread_calls++;
        if (mock_direct_pread_alignment == 0 ||
            (uintptr_t)buf % mock_direct_pread_alignment != 0 ||
            count % mock_direct_pread_alignment != 0 ||
            (uint64_t)offset % mock_direct_pread_alignment != 0) {
            mock_misaligned_pread_calls++;
            errno = EINVAL;
            return -1;
        }
    }
    if (fd == mock_short_pread_fd && count > 0) {
        mock_short_pread_calls++;
        count--;
    }
    return (ssize_t)syscall(SYS_pread64, fd, buf, count, offset);
}

static int make_dir(const char *p)
{
    return mkdir(p, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static int put_text(const char *p, const char *s)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t n = strlen(s);
    int ok = fd >= 0 && write(fd, s, n) == (ssize_t)n;
    if (fd >= 0) close(fd);
    return ok ? 0 : -1;
}

struct fake_sysfs {
    char root[128];
};

static uint64_t test_off_t_max(void)
{
    unsigned bits = (unsigned)(sizeof(off_t) * CHAR_BIT);
    unsigned value_bits = bits - (((off_t)-1 < (off_t)0) ? 1u : 0u);
    uint64_t max = 0;
    unsigned i;

    if (value_bits > 64u) value_bits = 64u;
    for (i = 0; i < value_bits; i++)
        max = (max << 1) | UINT64_C(1);
    return max;
}

static int fake_sysfs_make(struct fake_sysfs *f)
{
    char p[512];
    snprintf(f->root, sizeof f->root, "/tmp/exitos-sysfs-XXXXXX");
    if (!mkdtemp(f->root)) return -1;

#define MD(s) do { snprintf(p, sizeof p, "%s/%s", f->root, (s)); if (make_dir(p)) return -1; } while (0)
    MD("dev"); MD("dev/block"); MD("dev/char"); MD("devices");
    MD("devices/controller-a"); MD("devices/controller-a/ns5");
    MD("devices/controller-a/ns5/queue");
    MD("devices/controller-a/ns5/ns5p1");
    MD("devices/controller-a/ns6"); MD("devices/controller-a/ns6/queue");
    MD("devices/controller-a/ng-a");
    MD("devices/controller-b"); MD("devices/controller-b/ns5");
    MD("devices/controller-b/ns5/queue");
    MD("devices/controller-b/ng-b");
    MD("devices/controller-a/ns-part"); MD("devices/controller-a/ng-part");
#undef MD

#define TEXT(rel, s) do { snprintf(p, sizeof p, "%s/%s", f->root, (rel)); if (put_text(p, (s))) return -1; } while (0)
    TEXT("devices/controller-a/ns5/nsid", "5\n");
    TEXT("devices/controller-a/ns5/dev", "259:0\n");
    TEXT("devices/controller-a/ns5/queue/logical_block_size", "4096\n");
    TEXT("devices/controller-a/ns5/queue/io_poll", "1\n");
    TEXT("devices/controller-a/ns5/diskseq", "101\n");
    TEXT("devices/controller-a/ns6/nsid", "6\n");
    TEXT("devices/controller-a/ns6/dev", "259:5\n");
    TEXT("devices/controller-a/ns6/queue/logical_block_size", "4096\n");
    TEXT("devices/controller-a/ns6/queue/io_poll", "0\n");
    TEXT("devices/controller-a/ns6/diskseq", "103\n");
    TEXT("devices/controller-a/serial", "SERIAL-A\n");
    TEXT("devices/controller-a/ns5/ns5p1/dev", "259:4\n");
    TEXT("devices/controller-a/ns5/ns5p1/partition", "1\n");
    TEXT("devices/controller-a/ns5/ns5p1/start", "80\n");
    TEXT("devices/controller-a/ns5/ns5p1/size", "32\n");
    TEXT("devices/controller-a/ng-a/dev", "240:0\n");
    TEXT("devices/controller-b/ns5/nsid", "5\n");
    TEXT("devices/controller-b/ns5/dev", "259:1\n");
    TEXT("devices/controller-b/ns5/queue/logical_block_size", "512\n");
    TEXT("devices/controller-b/ns5/queue/io_poll", "1\n");
    TEXT("devices/controller-b/ns5/diskseq", "202\n");
    TEXT("devices/controller-b/serial", "SERIAL-B\n");
    TEXT("devices/controller-b/ng-b/dev", "240:1\n");
    TEXT("devices/controller-a/ns-part/nsid", "8\n");
    TEXT("devices/controller-a/ns-part/dev", "259:2\n");
    TEXT("devices/controller-a/ns-part/diskseq", "104\n");
    TEXT("devices/controller-a/ng-part/dev", "240:2\n");
    TEXT("devices/controller-a/ns-part/partition", "1\n");
#undef TEXT

#define SL(target, rel) do { snprintf(p, sizeof p, "%s/%s", f->root, (rel)); if (symlink((target), p)) return -1; } while (0)
    SL("..", "devices/controller-a/ns5/device");
    SL("..", "devices/controller-a/ns6/device");
    SL("..", "devices/controller-a/ng-a/device");
    SL("..", "devices/controller-b/ns5/device");
    SL("..", "devices/controller-b/ng-b/device");
    SL("..", "devices/controller-a/ns-part/device");
    SL("..", "devices/controller-a/ng-part/device");
    SL("../../devices/controller-a/ns5", "dev/block/259:0");
    SL("../../devices/controller-a/ns6", "dev/block/259:5");
    SL("../../devices/controller-b/ns5", "dev/block/259:1");
    SL("../../devices/controller-a/ns5/ns5p1", "dev/block/259:4");
    SL("../../devices/controller-a/ns-part", "dev/block/259:2");
    SL("../../devices/controller-a/ns5", "dev/block/259:3");
    SL("../../devices/does-not-exist", "dev/block/259:9");
    SL("../../devices/controller-a/ng-a", "dev/char/240:0");
    SL("../../devices/controller-b/ng-b", "dev/char/240:1");
    SL("../../devices/controller-a/ng-part", "dev/char/240:2");
    SL("../../devices/controller-a/ng-a", "dev/char/240:3");
#undef SL
    return 0;
}

static int remove_tree_entry(const char *path, const struct stat *st, int type,
                             struct FTW *ftw)
{
    (void)st; (void)type; (void)ftw;
    return remove(path);
}

static void test_sysfs_identity(void)
{
    struct fake_sysfs f;
    T_EQ(fake_sysfs_make(&f), 0, "created an isolated fake sysfs identity tree");

    T_EQ(exitos_devguard_nvme_pair_at(f.root, 259, 0, 240, 0, 5), 1,
         "device numbers resolving to one controller and namespace pair are accepted");
    T_EQ(exitos_devguard_nvme_pair_at(f.root, 259, 0, 240, 0, 6), 0,
         "character ioctl NSID must exactly equal the block namespace NSID");
    T_EQ(exitos_devguard_nvme_pair_at(f.root, 259, 0, 240, 1, 5), 0,
         "equal NSIDs on different controllers are not a pair");
    T_EQ(exitos_devguard_nvme_pair_at(f.root, 259, 9, 240, 0, 5), 0,
         "a broken /sys/dev/block symlink fails closed");
    T_EQ(exitos_devguard_nvme_pair_at(f.root, 259, 3, 240, 0, 5), 0,
         "a major:minor symlink pointing at another real device is refused");
    T_EQ(exitos_devguard_nvme_pair_at(f.root, 259, 0, 240, 3, 5), 0,
         "a wrong /sys/dev/char major:minor symlink target is refused");
    T_EQ(exitos_devguard_nvme_pair_at(f.root, 259, 2, 240, 2, 8), 0,
         "a partition can never impersonate a whole NVMe namespace");

    T_EQ(exitos_devguard_whole_block_at(f.root, 259, 0), 1,
         "whole-block classification follows /sys/dev/block major:minor");
    T_EQ(exitos_devguard_whole_block_at(f.root, 259, 2), 0,
         "partition marker reached through major:minor is refused");
    T_EQ(exitos_devguard_whole_block_at(f.root, 259, 3), 0,
         "whole-block gate rejects a wrong major:minor symlink target");
    T_EQ(exitos_devguard_whole_block_at(f.root, 259, 9), 0,
         "unresolvable major:minor is refused");
    T_EQ(nftw(f.root, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS), 0,
         "removed isolated fake sysfs tree");
}

static void test_fd_resolver_and_mountinfo(void)
{
    struct fake_sysfs f;
    struct devguard_device_info info;
    char backing[] = "/tmp/exitos-resolve-fd-XXXXXX";
    char part_backing[] = "/tmp/exitos-resolve-part-fd-XXXXXX";
    char mountinfo[] = "/tmp/exitos-mountinfo-XXXXXX";
    char p[512], long_line[400];
    char ns5_identity[EXITOS_DEVGUARD_IDENTITY_MAX];
    char serial[EXITOS_DEVGUARD_IDENTITY_MAX];
    char why[256];
    int fd, part_fd, mfd;

    T_EQ(fake_sysfs_make(&f), 0, "created fake sysfs for fd identity resolution");
    fd = mkstemp(backing);
    part_fd = mkstemp(part_backing);
    mfd = mkstemp(mountinfo);
    T_OK(fd >= 0 && part_fd >= 0 && mfd >= 0,
         "created resolver and mountinfo fixtures");
    if (fd < 0 || part_fd < 0 || mfd < 0) goto out;
    close(mfd);

    mock_block_fd = fd;

    mock_block_rdev = makedev(259, 0);
    memset(&info, 0, sizeof info);
    T_EQ(exitos_devguard_resolve_fd_at(fd, f.root, &info), 0,
         "fd resolver follows fstat rdev through /sys/dev/block");
    T_EQ(info.dev_major, 259, "resolver records the opened object's major");
    T_EQ(info.dev_minor, 0, "resolver records the opened object's minor");
    T_EQ(info.logical_block_size, 4096,
         "resolver reads the namespace's native logical block size");
    T_EQ(info.is_partition, 0, "whole namespace is not marked as a partition");
    T_OK(strcmp(info.identity, "SERIAL-A#nsid=5") == 0,
         "resolver combines controller identity with the actual namespace ID");
    T_OK(strcmp(info.controller_serial, "SERIAL-A") == 0,
         "resolver keeps operator controller serial separate from namespace identity");
    snprintf(ns5_identity, sizeof ns5_identity, "%s", info.identity);

    mock_block_rdev = makedev(259, 5);
    memset(&info, 0, sizeof info);
    T_EQ(exitos_devguard_resolve_fd_at(fd, f.root, &info), 0,
         "resolved a second namespace on the same controller");
    T_OK(strcmp(info.identity, "SERIAL-A#nsid=6") == 0,
         "second namespace identity includes its own NSID");
    T_OK(strcmp(info.identity, ns5_identity) != 0,
         "controller siblings can never collapse to one resolver identity");

    mock_block_rdev = makedev(259, 4);
    memset(&info, 0, sizeof info);
    T_EQ(exitos_devguard_resolve_fd_at(fd, f.root, &info), 0,
         "fd resolver accepts a canonical partition target");
    T_EQ(info.is_partition, 1, "partition marker is resolved from sysfs");
    T_EQ(info.parent_major, 259, "partition parent major comes from parent dev");
    T_EQ(info.parent_minor, 0, "partition parent minor comes from parent dev");
    T_EQ(info.logical_block_size, 4096,
         "partition inherits native LBS from its actual parent");
    T_OK(strcmp(info.identity, "SERIAL-A#nsid=5") == 0,
         "partition inherits the namespace-stable parent identity");

    mock_block_rdev = makedev(259, 3);
    T_OK(exitos_devguard_resolve_fd_at(fd, f.root, &info) != 0,
         "resolver rejects a major:minor alias whose target dev disagrees");

    mock_block_rdev = makedev(259, 0);
    memset(long_line, 'X', sizeof long_line);
    long_line[sizeof long_line - 1] = '\0';
    snprintf(p, sizeof p, "%s/devices/controller-a/serial", f.root);
    T_EQ(put_text(p, long_line), 0, "wrote an overlong unterminated serial");
    T_OK(exitos_devguard_resolve_fd_at(fd, f.root, &info) != 0,
         "identity truncation fails closed instead of pinning a prefix");
    T_EQ(put_text(p, "SERIAL-A\n"), 0, "restored exact serial fixture");

    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/queue/logical_block_size",
             f.root);
    T_EQ(put_text(p, "4096xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"),
         0, "wrote malformed/truncated native LBS data");
    T_OK(exitos_devguard_resolve_fd_at(fd, f.root, &info) != 0,
         "malformed native LBS fails closed");
    T_EQ(put_text(p, "4096\n"), 0, "restored exact LBS fixture");

    T_EQ(put_text(mountinfo,
                  "41 30 259:4 / /some/alias rw,relatime - ext4 /dev/disk/by-id/x rw\n"),
         0, "wrote mountinfo with an aliased partition mountpoint");
    T_EQ(exitos_devguard_mounted_at(f.root, mountinfo, 259, 0), 1,
         "mount gate matches a child partition by dev_t, not pathname text");
    mock_block_rdev = makedev(259, 4);
    T_EQ(exitos_devguard_check_fd_at(fd, f.root, mountinfo, "SERIAL-A",
                                     NULL, 0), DEVGUARD_MOUNTED,
         "mounted partition is rejected before an acceptable partition verdict");
    mock_block_rdev = makedev(259, 0);
    T_EQ(exitos_devguard_check_fd_at(fd, f.root, mountinfo, "SERIAL-A",
                                     NULL, 0), DEVGUARD_MOUNTED,
         "whole disk with a mounted child is rejected before has-partitions verdict");
    T_EQ(put_text(mountinfo,
                  "41 30 8:1 / /dev/nvme-looking-name rw - ext4 /dev/sda1 rw\n"),
         0, "wrote unrelated dev_t with a misleading mountpoint name");
    T_EQ(exitos_devguard_mounted_at(f.root, mountinfo, 259, 0), 0,
         "mount gate ignores path aliases when dev_t differs");
    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/vanished-child", f.root);
    T_EQ(symlink("no-such-child", p), 0,
         "created a disappearing sysfs-child race fixture");
    T_OK(exitos_devguard_mounted_at(f.root, mountinfo, 259, 0) < 0,
         "unreadable sysfs child fails mount query closed instead of being skipped");
    T_EQ(unlink(p), 0, "removed disappearing sysfs-child fixture");
    T_EQ(put_text(mountinfo, "malformed mountinfo\n"), 0,
         "wrote malformed mountinfo input");
    T_OK(exitos_devguard_mounted_at(f.root, mountinfo, 259, 0) < 0,
         "malformed mountinfo fails closed");
    T_EQ(put_text(mountinfo, ""), 0, "wrote an empty mountinfo fixture");
    T_OK(exitos_devguard_mounted_at(f.root, mountinfo, 259, 0) < 0,
         "empty mountinfo cannot prove that a device is unmounted");
    T_OK(exitos_devguard_mounted_at(f.root, "/no/such/mountinfo", 259, 0) < 0,
         "mountinfo open failure is distinguishable from not mounted");

    mock_block_rdev = makedev(259, 1);
    T_EQ(put_text(mountinfo,
                  "41 30 8:1 / /unrelated rw - ext4 /dev/sda1 rw\n"), 0,
         "prepared an unmounted fd check fixture");
    memset(serial, 0, sizeof serial);
    snprintf(p, sizeof p, "%s/devices/controller-b/ns5/wwid", f.root);
    T_EQ(put_text(p, "eui.0000000000000005\n"), 0,
         "added a namespace-specific WWID beside the controller serial");
    memset(&info, 0, sizeof info);
    T_EQ(exitos_devguard_resolve_fd_at(fd, f.root, &info), 0,
         "resolver accepts namespace WWID plus controller serial");
    T_OK(strcmp(info.identity, "eui.0000000000000005") == 0,
         "namespace-stable identity prefers the namespace WWID");
    T_OK(strcmp(info.controller_serial, "SERIAL-B") == 0,
         "operator identity remains the actual controller serial");
    T_EQ(exitos_devguard_check_fd_at(fd, f.root, mountinfo, "SERIAL-B",
                                     serial, sizeof serial), DEVGUARD_OK,
         "controller serial expectation succeeds when a namespace WWID exists");
    T_OK(strcmp(serial, "SERIAL-B") == 0,
         "fd check reports the operator-facing controller serial");
    T_EQ(exitos_devguard_check_fd_at(fd, f.root, mountinfo, "SERIAL-A",
                                     NULL, 0), DEVGUARD_SERIAL_MISMATCH,
         "wrong controller serial is rejected despite a valid namespace WWID");
    T_OK(exitos_devguard_check_fd_at(fd, f.root, "/no/such/mountinfo", NULL,
                                    NULL, 0) != DEVGUARD_OK,
         "fd check refuses when the mount query cannot be completed");

    mock_block_rdev = makedev(259, 0);
    T_EQ(exitos_devguard_passthru_ok_fd_at(fd, f.root), 1,
         "fd passthrough gate accepts an actual whole namespace");
    mock_block_rdev = makedev(259, 4);
    T_EQ(exitos_devguard_passthru_ok_fd_at(fd, f.root), 0,
         "fd passthrough gate rejects an actual partition namespace");

    mock_block_rdev = makedev(259, 0);
    mock_block_fd2 = part_fd;
    mock_block_rdev2 = makedev(259, 4);
    T_EQ(exitos_devguard_window_in_partition_fds_at(fd, part_fd, f.root,
                                                    10, 2, 4096,
                                                    why, sizeof why), 0,
         "partition window binds both disk and partition to opened dev_t objects");
    T_EQ(exitos_devguard_window_in_partition_fds_at(fd, part_fd, f.root,
                                                    9, 2, 4096,
                                                    why, sizeof why), -ERANGE,
         "fd-bound partition window refuses bytes before the actual partition");
    T_OK(exitos_devguard_window_in_partition_fds_at(fd, part_fd, f.root,
                                                    10, 2, 512,
                                                    why, sizeof why) != 0,
         "partition window refuses caller LBS that differs from actual disk LBS");

out:
    mock_block_fd = -1;
    mock_block_fd2 = -1;
    if (fd >= 0) close(fd);
    if (part_fd >= 0) close(part_fd);
    unlink(backing);
    unlink(part_backing);
    unlink(mountinfo);
    nftw(f.root, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void test_fd_bound_lba_span(void)
{
    struct fake_sysfs f;
    char backing[] = "/tmp/exitos-lba-span-fd-XXXXXX";
    char p[512];
    uint64_t base, count;
    int fd = -1, rc;

    T_EQ(fake_sysfs_make(&f), 0,
         "created fake sysfs for fd-bound LBA span resolution");
    fd = mkstemp(backing);
    T_OK(fd >= 0, "created ordinary-file backing for the mocked block fd");
    if (fd < 0) goto out;

    mock_block_fd = fd;
    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/size", f.root);
    T_EQ(put_text(p, "160\n"), 0, "gave the 4Kn whole device 160 sysfs sectors");
    snprintf(p, sizeof p, "%s/devices/controller-b/ns5/size", f.root);
    T_EQ(put_text(p, "33\n"), 0, "gave the 512-byte whole device 33 sysfs sectors");
    snprintf(p, sizeof p, "%s/devices/controller-b/ns5/ns5p9", f.root);
    T_EQ(make_dir(p), 0, "created a second-generation partition object");
    snprintf(p, sizeof p, "%s/devices/controller-b/ns5/ns5p9/dev", f.root);
    T_EQ(put_text(p, "259:4\n"), 0,
         "gave generation B the same partition device number");
    snprintf(p, sizeof p, "%s/devices/controller-b/ns5/ns5p9/partition", f.root);
    T_EQ(put_text(p, "9\n"), 0, "marked generation B as a partition");
    snprintf(p, sizeof p, "%s/devices/controller-b/ns5/ns5p9/start", f.root);
    T_EQ(put_text(p, "800\n"), 0, "gave generation B a distinct start");
    snprintf(p, sizeof p, "%s/devices/controller-b/ns5/ns5p9/size", f.root);
    T_EQ(put_text(p, "80\n"), 0, "gave generation B a distinct size");

    mock_block_rdev = makedev(259, 0);
    base = 71;
    count = 72;
    T_EQ(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count), 0,
         "whole-device span resolves through the opened fd's canonical object");
    T_EQ(base, 0, "whole-device span always starts at native LBA zero");
    T_EQ(count, 20, "4Kn whole-device size converts from fixed 512-byte sectors");

    mock_block_rdev = makedev(259, 4);
    base = 73;
    count = 74;
    T_EQ(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count), 0,
         "partition span resolves through the opened partition fd");
    T_EQ(base, 10, "4Kn partition start converts from 80 sysfs sectors");
    T_EQ(count, 4, "4Kn partition size converts from 32 sysfs sectors");

    snprintf(deny_absolute_metadata_root, sizeof deny_absolute_metadata_root,
             "%s", f.root);
    denied_absolute_metadata_opens = 0;
    base = 77;
    count = 78;
    T_EQ(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count), 0,
         "span uses only retained relative metadata reads after canonical open");
    T_EQ(denied_absolute_metadata_opens, 0,
         "span never reopens absolute device metadata paths");
    T_EQ(base, 10, "retained-only span preserves the exact partition base");
    T_EQ(count, 4, "retained-only span preserves the exact partition count");
    deny_absolute_metadata_root[0] = '\0';

    mock_block_diskseq_calls = 0;
    mock_block_diskseq_fail_call = 1;
    base = 81;
    count = 82;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "BLKGETDISKSEQ failure refuses the span before trusting sysfs");
    T_EQ(base, 81, "diskseq ioctl failure preserves the base sentinel");
    T_EQ(count, 82, "diskseq ioctl failure preserves the count sentinel");
    mock_block_diskseq_fail_call = 0;

    mock_block_diskseq_calls = 0;
    mock_block_diskseq_change_call = 2;
    mock_block_diskseq_changed_value = 202;
    base = 83;
    count = 84;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "fd generation changing during span capture fails closed");
    T_OK(mock_block_diskseq_calls >= 2,
         "span rechecks the block fd generation before committing output");
    T_EQ(base, 83, "mid-span generation drift preserves the base sentinel");
    T_EQ(count, 84, "mid-span generation drift preserves the count sentinel");
    mock_block_diskseq_change_call = 0;
    mock_block_diskseq_changed_value = 0;

    mock_block_diskseq_calls = 0;
    mock_block_diskseq_override = 202;
    base = 79;
    count = 80;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "same dev_t from a different gendisk generation fails closed");
    T_OK(mock_block_diskseq_calls > 0,
         "fd generation is read through BLKGETDISKSEQ");
    T_EQ(base, 79, "diskseq mismatch leaves base output unchanged");
    T_EQ(count, 80, "diskseq mismatch leaves count output unchanged");
    mock_block_diskseq_override = 0;

    mock_block_rdev = makedev(259, 1);
    base = 75;
    count = 76;
    T_EQ(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count), 0,
         "512-byte native spans preserve the sysfs sector count");
    T_EQ(base, 0, "512-byte whole-device base is zero");
    T_EQ(count, 33, "512-byte whole-device size needs no scale change");

    mock_block_rdev = makedev(259, 4);
    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/ns5p1/start", f.root);
    T_EQ(put_text(p, "81\n"), 0, "made partition start non-integral in 4Kn LBAs");
    base = 101;
    count = 102;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "non-integral native-LBA partition start fails closed");
    T_EQ(base, 101, "failed start conversion leaves base output unchanged");
    T_EQ(count, 102, "failed start conversion leaves count output unchanged");
    T_EQ(put_text(p, "80\n"), 0, "restored integral partition start");

    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/ns5p1/size", f.root);
    T_EQ(put_text(p, "31\n"), 0, "made partition size non-integral in 4Kn LBAs");
    base = 103;
    count = 104;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "non-integral native-LBA partition size fails closed");
    T_EQ(base, 103, "failed size conversion leaves base output unchanged");
    T_EQ(count, 104, "failed size conversion leaves count output unchanged");
    T_EQ(put_text(p, "32\n"), 0, "restored integral partition size");

    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/ns5p1/start", f.root);
    T_EQ(put_text(p, "not-a-sector\n"), 0, "made partition start malformed");
    base = 105;
    count = 106;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "malformed partition start fails closed");
    T_EQ(base, 105, "malformed start leaves base output unchanged");
    T_EQ(count, 106, "malformed start leaves count output unchanged");
    T_EQ(unlink(p), 0, "removed the malformed partition start");
    base = 107;
    count = 108;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "missing partition start fails closed");
    T_EQ(base, 107, "missing start leaves base output unchanged");
    T_EQ(count, 108, "missing start leaves count output unchanged");
    T_EQ(put_text(p, "80\n"), 0, "restored required partition start");

    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/ns5p1/size", f.root);
    T_EQ(put_text(p, "0\n"), 0, "made partition size zero");
    base = 109;
    count = 110;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "zero partition size fails closed");
    T_EQ(base, 109, "zero size leaves base output unchanged");
    T_EQ(count, 110, "zero size leaves count output unchanged");
    T_EQ(put_text(p, "18446744073709551616\n"), 0,
         "made partition size overflow uint64 parsing");
    base = 111;
    count = 112;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "overflowing decimal partition size fails closed");
    T_EQ(base, 111, "parse overflow leaves base output unchanged");
    T_EQ(count, 112, "parse overflow leaves count output unchanged");
    T_EQ(unlink(p), 0, "removed the overflowing partition size");
    base = 113;
    count = 114;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "missing partition size fails closed");
    T_EQ(base, 113, "missing size leaves base output unchanged");
    T_EQ(count, 114, "missing size leaves count output unchanged");

    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/ns5p1/start", f.root);
    T_EQ(put_text(p, "36028797018963976\n"), 0,
         "made integral 4Kn partition start overflow sector-to-byte conversion");
    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/ns5p1/size", f.root);
    T_EQ(put_text(p, "8\n"), 0,
         "kept overflowing-start partition size integral and nonzero");
    base = 115;
    count = 116;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "overflowing partition start conversion fails closed");
    T_EQ(base, 115, "partition-start overflow leaves base output unchanged");
    T_EQ(count, 116, "partition-start overflow leaves count output unchanged");

    mock_block_rdev = makedev(259, 1);
    snprintf(p, sizeof p, "%s/devices/controller-b/ns5/size", f.root);
    T_EQ(put_text(p, "36028797018963969\n"), 0,
         "made whole-device sector-to-byte conversion overflow");
    base = 117;
    count = 118;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "overflowing whole-device conversion fails closed");
    T_EQ(base, 117, "whole conversion overflow leaves base output unchanged");
    T_EQ(count, 118, "whole conversion overflow leaves count output unchanged");
    T_EQ(put_text(p, "18446744073709551617\n"), 0,
         "made whole-device size overflow strict decimal parsing");
    base = 125;
    count = 126;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "overflowing whole-device decimal fails closed");
    T_EQ(base, 125, "whole parse overflow leaves base output unchanged");
    T_EQ(count, 126, "whole parse overflow leaves count output unchanged");

    mock_block_rdev = makedev(259, 3);
    base = 119;
    count = 120;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count) != 0,
         "major:minor alias to a different canonical object fails closed");
    T_EQ(base, 119, "wrong object identity leaves base output unchanged");
    T_EQ(count, 120, "wrong object identity leaves count output unchanged");

    mock_block_rdev = makedev(259, 0);
    base = 121;
    count = 122;
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, &base, NULL) != 0,
         "missing count output is rejected");
    T_EQ(base, 121, "missing count output cannot modify the base output");
    T_OK(exitos_devguard_lba_span_fd_at(fd, f.root, NULL, &count) != 0,
         "missing base output is rejected");
    T_EQ(count, 122, "missing base output cannot modify the count output");

    base = 123;
    count = 124;
    T_OK(exitos_devguard_lba_span_fd(-1, &base, &count) != 0,
         "production wrapper rejects an invalid fd without consulting a device");
    T_EQ(base, 123, "wrapper failure leaves base output unchanged");
    T_EQ(count, 124, "wrapper failure leaves count output unchanged");

    /* Restore every geometry field after the overflow mutants above.  The ABA
     * result must now be attributable only to object binding, never to a stale
     * malformed/overflow fixture. */
    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/ns5p1/start", f.root);
    T_EQ(put_text(p, "80\n"), 0, "restored valid generation A start before ABA");
    snprintf(p, sizeof p, "%s/devices/controller-a/ns5/ns5p1/size", f.root);
    T_EQ(put_text(p, "32\n"), 0, "restored valid generation A size before ABA");
    snprintf(p, sizeof p, "%s/devices/controller-b/ns5/ns5p9/start", f.root);
    T_EQ(put_text(p, "800\n"), 0, "kept valid generation B start before ABA");
    snprintf(p, sizeof p, "%s/devices/controller-b/ns5/ns5p9/size", f.root);
    T_EQ(put_text(p, "80\n"), 0, "restored valid generation B size before ABA");

    memset(&sysfs_generation_swap, 0, sizeof sysfs_generation_swap);
    snprintf(sysfs_generation_swap.dev_link,
             sizeof sysfs_generation_swap.dev_link,
             "%s/dev/block/259:4", f.root);
    snprintf(sysfs_generation_swap.b_size_path,
             sizeof sysfs_generation_swap.b_size_path,
             "%s/devices/controller-b/ns5/ns5p9/size", f.root);
    snprintf(sysfs_generation_swap.a_dir,
             sizeof sysfs_generation_swap.a_dir,
             "%s/devices/controller-a/ns5/ns5p1", f.root);
    snprintf(sysfs_generation_swap.a_target,
             sizeof sysfs_generation_swap.a_target,
             "../../devices/controller-a/ns5/ns5p1");
    snprintf(sysfs_generation_swap.b_target,
             sizeof sysfs_generation_swap.b_target,
             "../../devices/controller-b/ns5/ns5p9");
    mock_block_rdev = makedev(259, 4);
    base = 127;
    count = 128;
    sysfs_generation_swap.armed = 1;
    rc = exitos_devguard_lba_span_fd_at(fd, f.root, &base, &count);
    sysfs_generation_swap.armed = 0;
    T_EQ(sysfs_generation_swap.attempted, 1,
         "swapped the dev_t link after the initial resolver retained A metadata");
    T_EQ(sysfs_generation_swap.swapped, 1,
         "same-dev_t generation B replaced generation A");
    T_EQ(sysfs_generation_swap.retained_start_opened, 1,
         "late swap occurs at the retained start read after the first identity check");
    T_EQ(sysfs_generation_swap.retained_size_opened, 1,
         "retained size read completes the A-to-B-to-A window");
    T_EQ(sysfs_generation_swap.restored, 1,
         "the late generation seam actually restores A before the final check");
    T_EQ(sysfs_generation_swap.b_size_opened, 0,
         "span never follows the rebound pathname into generation B");
    T_EQ(rc, 0,
         "retained generation A remains a valid snapshot across pathname ABA");
    T_EQ(base, 10, "ABA snapshot returns generation A base exactly");
    T_EQ(count, 4, "ABA snapshot returns generation A count exactly");
    if (sysfs_generation_swap.swapped && !sysfs_generation_swap.restored) {
        rc = unlink(sysfs_generation_swap.dev_link);
        if (rc == 0)
            rc = symlink(sysfs_generation_swap.a_target,
                         sysfs_generation_swap.dev_link);
        T_EQ(rc, 0, "restored generation A after an early refusal");
    }
    memset(&sysfs_generation_swap, 0, sizeof sysfs_generation_swap);

out:
    memset(&sysfs_generation_swap, 0, sizeof sysfs_generation_swap);
    mock_block_fd = -1;
    mock_block_diskseq_override = 0;
    mock_block_diskseq_calls = 0;
    if (fd >= 0) close(fd);
    unlink(backing);
    nftw(f.root, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void test_fd_bound_poll_available(void)
{
    struct fake_sysfs f;
    char backing[] = "/tmp/exitos-poll-fd-XXXXXX";
    char path[512], link[512];
    int fd = -1;

    T_EQ(fake_sysfs_make(&f), 0,
         "created fake sysfs for fd-bound polling capability");
    fd = mkstemp(backing);
    T_OK(fd >= 0, "created ordinary backing for mocked polling block fd");
    if (fd < 0) goto out;
    mock_block_fd = fd;

    snprintf(path, sizeof path, "%s/devices/controller-b/ns5/ns5p9", f.root);
    T_EQ(make_dir(path), 0,
         "created generation B partition for polling identity test");
    snprintf(path, sizeof path,
             "%s/devices/controller-b/ns5/ns5p9/dev", f.root);
    T_EQ(put_text(path, "259:4\n"), 0,
         "gave generation B polling partition the same dev_t");
    snprintf(path, sizeof path,
             "%s/devices/controller-b/ns5/ns5p9/partition", f.root);
    T_EQ(put_text(path, "9\n"), 0,
         "marked generation B polling object as a partition");

    mock_block_rdev = makedev(259, 0);
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 1,
         "whole-device polling is proved from the fd-bound parent generation");
    mock_block_rdev = makedev(259, 4);
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 1,
         "partition polling is proved from its retained whole-device parent");
    snprintf(deny_absolute_metadata_root, sizeof deny_absolute_metadata_root,
             "%s", f.root);
    denied_absolute_metadata_opens = 0;
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 1,
         "poll capability uses only retained relative metadata reads");
    T_EQ(denied_absolute_metadata_opens, 0,
         "poll capability never reopens absolute device metadata paths");
    deny_absolute_metadata_root[0] = '\0';

    mock_block_diskseq_calls = 0;
    mock_block_diskseq_fail_call = 1;
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 0,
         "poll capability refuses when BLKGETDISKSEQ is unavailable");
    mock_block_diskseq_fail_call = 0;
    mock_block_diskseq_calls = 0;
    mock_block_diskseq_change_call = 2;
    mock_block_diskseq_changed_value = 202;
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 0,
         "poll capability refuses fd generation drift before returning true");
    T_OK(mock_block_diskseq_calls >= 2,
         "poll capability rechecks the block fd generation");
    mock_block_diskseq_change_call = 0;
    mock_block_diskseq_changed_value = 0;

    snprintf(path, sizeof path,
             "%s/devices/controller-a/ns5/queue/io_poll", f.root);
    T_EQ(put_text(path, "0\n"), 0, "disabled polling in the retained parent");
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 0,
         "io_poll=0 fails closed");
    T_EQ(put_text(path, "2\n"), 0, "made polling capability non-boolean");
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 0,
         "non-one polling capability fails closed");
    T_EQ(put_text(path, "not-one\n"), 0,
         "made polling capability malformed");
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 0,
         "malformed polling capability fails closed");
    T_EQ(unlink(path), 0, "removed required polling capability");
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 0,
         "missing polling capability fails closed");
    T_EQ(put_text(path, "1\n"), 0, "restored exact polling capability");

    mock_block_rdev = makedev(259, 0);
    mock_block_diskseq_override = 202;
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 0,
         "polling cannot be borrowed from the same dev_t in another generation");
    mock_block_diskseq_override = 0;
    mock_block_diskseq_fail_call = 0;
    mock_block_diskseq_change_call = 0;
    mock_block_diskseq_changed_value = 0;
    deny_absolute_metadata_root[0] = '\0';
    denied_absolute_metadata_opens = 0;

    snprintf(link, sizeof link, "%s/dev/block/259:4", f.root);
    T_EQ(unlink(link), 0, "removed generation A partition link for poll race");
    T_EQ(symlink("../../devices/controller-b/ns5/ns5p9", link), 0,
         "rebound the same partition dev_t to generation B");
    mock_block_rdev = makedev(259, 4);
    T_EQ(exitos_devguard_poll_available_fd_at(fd, f.root), 0,
         "fd generation A cannot borrow generation B polling support");
    T_EQ(unlink(link), 0, "removed generation B partition link");
    T_EQ(symlink("../../devices/controller-a/ns5/ns5p1", link), 0,
         "restored generation A partition link");

out:
    mock_block_fd = -1;
    mock_block_rdev = 0;
    mock_block_diskseq_override = 0;
    if (fd >= 0) close(fd);
    unlink(backing);
    nftw(f.root, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void test_4kn_zero_window(void)
{
    char path[] = "/tmp/exitos-zero-4kn-XXXXXX";
    int fd = mkstemp(path);
    unsigned char one = 1;
    uint64_t bad = UINT64_MAX;
    uint64_t off_max = test_off_t_max();

    T_OK(fd >= 0, "created ordinary-file backing for a write-free 4Kn test");
    if (fd < 0) return;
    T_EQ(ftruncate(fd, 4 * 4096), 0, "sized 4Kn test backing");
    T_EQ(pwrite(fd, &one, 1, 2 * 4096 + 37), 1,
         "placed a nonzero byte in the second requested native block");
    close(fd);

    T_EQ(exitos_devguard_window_is_zero_checked(path, 1, 1, 4096, &bad), 0,
         "first 4Kn native block is entirely zero");
    T_EQ(exitos_devguard_window_is_zero_checked(path, 1, 2, 4096, &bad), 1,
         "4Kn scan covers the complete requested byte window");
    T_EQ(bad, 2, "first bad LBA is reported in native 4Kn units");
    T_EQ(exitos_devguard_window_is_zero_checked(path, 3, 2, 4096, &bad), -EIO,
         "a short read at the end of the requested window fails closed");
    T_EQ(exitos_devguard_window_is_zero_checked(path, UINT64_MAX, 1, 4096, &bad),
         -EOVERFLOW, "lba times native LBS overflow is refused before pread");
    T_EQ(exitos_devguard_window_is_zero_checked(path, 0, UINT64_MAX, 4096, &bad),
         -EOVERFLOW, "count times native LBS overflow is refused before pread");
    if (off_max < UINT64_MAX) {
        T_EQ(exitos_devguard_window_is_zero_checked(path, off_max / 4096 + 1,
                                                    1, 4096, &bad),
             -EOVERFLOW, "an offset outside this ABI's off_t is refused");
    } else {
        T_SKIP("off_t can represent all uint64_t offsets on this ABI");
    }
    T_EQ(exitos_devguard_window_is_zero_checked(path, UINT64_MAX - 7, 8, 1, &bad),
         -EOVERFLOW,
         "start plus total overflow is refused when neither multiplication overflows");
    unlink(path);
}

static void test_pin_strictness(void)
{
    char short_path[] = "/tmp/exitos-pin-short-XXXXXX";
    char pin_path[] = "/tmp/exitos-pin-file-XXXXXX";
    struct devguard_pin p;
    char why[256];
    int fd = mkstemp(short_path);
    int pfd = mkstemp(pin_path);

    T_OK(fd >= 0 && pfd >= 0, "created ordinary-file pin fixtures");
    if (fd >= 0) {
        T_EQ(ftruncate(fd, 4095), 0, "made every 4096-byte pin sample short");
        close(fd);
        T_OK(exitos_devguard_pin_capture(short_path, 4, 8, &p) != 0,
             "pin capture refuses a short sampled pread");
    }
    if (pfd >= 0) close(pfd);

    T_EQ(put_text(pin_path,
                  "serial=SERIAL A\nsize=1048576\nhash=7\nzlba=40\nzcnt=8\nn=64\n"), 0,
         "wrote the legacy pin format without a native LBS");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "legacy pin format fails closed because its LBA units are unknown");
    T_EQ(put_text(pin_path,
                  "serial=SERIAL A\nsize=1048576\nhash=7\nzlba=40\nzcnt=8\nlbs=4096\nn=64\n"), 0,
         "wrote a complete native-LBS pin file");
    T_EQ(exitos_devguard_pin_load(&p, pin_path), 0,
         "strict parser accepts the exact saved format");
    T_EQ(p.zero_lba_start, 40, "strict parser loaded zlba exactly");
    T_EQ(p.logical_block_size, 4096,
         "strict parser preserves the native LBA byte size exactly");
    T_EQ(exitos_devguard_pin_verify_window("/no/device/needed", pin_path, 41, 8,
                                           why, sizeof why), -ERANGE,
         "pin verify binds this request's starting LBA before device access");
    T_OK(strstr(why, "window") != NULL, "pin mismatch explanation names the window");
    T_EQ(exitos_devguard_pin_verify_window("/no/device/needed", pin_path, 40, 9,
                                           why, sizeof why), -ERANGE,
         "pin verify binds this request's count exactly");

    T_EQ(put_text(pin_path,
                  "serial=S\nsize=1junk\nhash=7\nzlba=40\nzcnt=8\nlbs=4096\nn=64\n"), 0,
         "wrote numeric trailing junk");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects numeric trailing junk");
    T_EQ(put_text(pin_path,
                  "serial=S\nsize= -1\nhash=7\nzlba=40\nzcnt=8\nlbs=4096\nn=64\n"), 0,
         "wrote a negative number hidden behind leading whitespace");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects whitespace followed by a minus sign");
    T_EQ(put_text(pin_path,
                  "serial=S\nsize= +1\nhash=7\nzlba=40\nzcnt=8\nlbs=4096\nn=64\n"), 0,
         "wrote a positive sign hidden behind leading whitespace");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects whitespace followed by a plus sign");
    T_EQ(put_text(pin_path,
                  "serial=S\nsize= 1\nhash=7\nzlba=40\nzcnt=8\nlbs=4096\nn=64\n"), 0,
         "wrote an unsigned number with leading whitespace");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects leading numeric whitespace");
    T_EQ(put_text(pin_path,
                  "serial=S\nsize=1 \nhash=7\nzlba=40\nzcnt=8\nlbs=4096\nn=64\n"), 0,
         "wrote an unsigned number with trailing whitespace");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects trailing numeric whitespace");
    T_EQ(put_text(pin_path,
                  "serial=S\nsize=1\nhash=7\nzlba=40\nzcnt=8\nlbs=4096\nn=64\nextra=x\n"), 0,
         "wrote an unknown extra field");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects unknown or extra fields");
    T_EQ(put_text(pin_path,
                  "serial=S\nsize=1\nhash=7\nzlba=40\nzcnt=8\nlbs=4096\n"), 0,
         "wrote a pin missing its sample count");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects missing fields");
    T_EQ(put_text(pin_path,
                  "serial=S\nsize=1\nhash=7\nzlba=40\nzcnt=8\nlbs=4096\nn=63\n"), 0,
         "wrote a pin claiming a different sampling scheme");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects an unsupported sample count");
    T_EQ(put_text(pin_path,
                  "serial=S\nsize=1\nhash=7\nzlba=40\nzcnt=0\nlbs=4096\nn=64\n"), 0,
         "wrote a pin claiming an empty certified window");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects an empty certified window");
    T_EQ(put_text(pin_path,
                  "serial=S\nsize=1\nhash=7\nzlba=40\nzcnt=8\nlbs=0\nn=64\n"), 0,
         "wrote a pin with an empty native LBS");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects a zero native LBS");
    T_EQ(put_text(pin_path,
                  "serial=S\nsize=1\nhash=7\nzlba=40\nzcnt=8\nlbs=4096junk\nn=64\n"), 0,
         "wrote a pin with trailing junk in its native LBS");
    T_OK(exitos_devguard_pin_load(&p, pin_path) != 0,
         "strict parser rejects malformed native LBS data");

    unlink(short_path);
    unlink(pin_path);
}

static void test_fd_bound_pin_zero_and_save(void)
{
    struct fake_sysfs f;
    struct devguard_pin pin, other_ns_pin, loaded_pin, short_pin;
    char backing[] = "/tmp/exitos-pin-fd-XXXXXX";
    char moved[256] = {0}, pin_path[] = "/tmp/exitos-pin-atomic-XXXXXX";
    char victim[] = "/tmp/exitos-pin-victim-XXXXXX", lbs_path[512];
    char link_path[] = "/tmp/exitos-pin-link-XXXXXX";
    char short_path[] = "/tmp/exitos-pin-fd-short-XXXXXX";
    unsigned char one = 1;
    char victim_buf[16] = {0}, pin_text[512] = {0};
    uint64_t bad = UINT64_MAX;
    struct stat st;
    struct iopath *io = NULL;
    void *write_buf = NULL;
    mode_t old_umask;
    int fd = -1, pfd = -1, vfd = -1, lfd = -1, sfd = -1;

    T_EQ(fake_sysfs_make(&f), 0, "created fake sysfs for retained-fd guards");
    fd = mkstemp(backing);
    pfd = mkstemp(pin_path);
    vfd = mkstemp(victim);
    lfd = mkstemp(link_path);
    sfd = mkstemp(short_path);
    T_OK(fd >= 0 && pfd >= 0 && vfd >= 0 && lfd >= 0 && sfd >= 0,
         "created retained-fd and atomic-save fixtures");
    if (fd < 0 || pfd < 0 || vfd < 0 || lfd < 0 || sfd < 0) goto out;
    close(pfd); pfd = -1;
    close(lfd); lfd = -1;
    T_EQ(ftruncate(fd, 4 * 4096), 0, "sized retained 4Kn namespace fixture");
    T_EQ(ftruncate(sfd, 4 * 4096), 0,
         "sized retained fixture beyond every pin sample's short-read gate");

    mock_block_fd = fd;
    mock_block_rdev = makedev(259, 0);
    mock_direct_pread_fd = fd;
    mock_direct_pread_alignment = 4096;
    mock_direct_pread_calls = mock_misaligned_pread_calls = 0;
    T_EQ(exitos_devguard_pin_capture_fd_at(fd, f.root, 10, 2, &pin), 0,
         "pin capture binds identity and samples to the supplied fd");
    T_EQ(mock_direct_pread_calls, 64,
         "pin capture performs every planned retained-fd sample read");
    T_EQ(mock_misaligned_pread_calls, 0,
         "pin capture buffers, offsets, and lengths satisfy retained O_DIRECT alignment");
    mock_direct_pread_fd = -1;
    mock_block_rdev = makedev(259, 5);
    T_EQ(exitos_devguard_pin_capture_fd_at(fd, f.root, 10, 2, &other_ns_pin), 0,
         "pin capture resolves a sibling namespace on the same controller");
    T_OK(strcmp(pin.serial, other_ns_pin.serial) != 0,
         "pin identities distinguish namespaces that share a controller serial");
    mock_block_rdev = makedev(259, 0);

    unlink(pin_path);
    old_umask = umask(0);
    T_EQ(exitos_devguard_pin_save(&pin, pin_path), 0,
         "pin save atomically creates a new pin");
    umask(old_umask);
    T_EQ(stat(pin_path, &st), 0, "saved pin exists");
    T_EQ(st.st_mode & 0777, 0600, "saved pin mode is exactly 0600 despite umask");
    pfd = open(pin_path, O_RDONLY | O_CLOEXEC);
    if (pfd >= 0) {
        ssize_t n = read(pfd, pin_text, sizeof pin_text - 1);
        if (n >= 0) pin_text[n] = '\0';
        T_OK(n > 0 && strstr(pin_text, "lbs=4096\n") != NULL,
             "saved 4Kn pin records native LBA units on disk");
        close(pfd); pfd = -1;
    } else {
        T_OK(0, "opened saved pin to inspect its native LBS");
    }
    T_EQ(exitos_devguard_pin_load(&loaded_pin, pin_path), 0,
         "4Kn pin survives a strict save/load round trip");
    T_EQ(loaded_pin.logical_block_size, 4096,
         "4Kn pin round trip retains its native LBA units");
    snprintf(lbs_path, sizeof lbs_path,
             "%s/devices/controller-a/ns5/queue/logical_block_size", f.root);
    T_EQ(put_text(lbs_path, "512\n"), 0,
         "changed the actual namespace LBS after certification");
    T_OK(exitos_devguard_pin_verify_window_fd_at(fd, f.root, pin_path, 10, 2,
                                                 NULL, 0) != 0,
         "pin verification rejects an actual native LBS change");
    T_EQ(put_text(lbs_path, "4096\n"), 0,
         "restored the certified namespace LBS");

    snprintf(moved, sizeof moved, "%s.moved", backing);
    T_EQ(rename(backing, moved), 0, "renamed the pathname behind the retained fd");
    pfd = open(backing, O_RDWR | O_CREAT | O_TRUNC, 0600);
    T_OK(pfd >= 0, "rebound the original pathname to a different regular file");
    T_EQ(exitos_devguard_pin_verify_window_fd_at(fd, f.root, pin_path, 10, 2,
                                                 NULL, 128), 0,
         "pin verify stays on retained fd and permits a NULL explanation buffer");
    T_EQ(exitos_devguard_pin_verify_window_fd_at(fd, f.root, pin_path, 11, 2,
                                                 NULL, 128), -ERANGE,
         "NULL explanation is safe on exact-window mismatch");

    T_EQ(posix_memalign(&write_buf, 4096, 4096), 0,
         "allocated aligned retained-fd write buffer");
    if (write_buf) memset(write_buf, 0x6b, 4096);
    io = iopath_open_fd(fd, IOPATH_PWRITE, 4096);
    T_OK(io != NULL, "iopath can retain the already-validated storage object");
    if (io && write_buf)
        T_EQ(iopath_write(io, 3, write_buf, 4096), 0,
             "retained iopath writes the original object after pathname rebinding");
    T_EQ(pread(fd, victim_buf, 1, 3 * 4096), 1,
         "read back retained-fd iopath byte from original object");
    T_EQ((unsigned char)victim_buf[0], 0x6b,
         "retained-fd iopath did not follow the replacement pathname");

    mock_direct_pread_fd = fd;
    mock_direct_pread_alignment = 4096;
    mock_direct_pread_calls = mock_misaligned_pread_calls = 0;
    T_EQ(exitos_devguard_window_is_zero_fd_at(fd, f.root, 1, 1, &bad), 0,
         "fd zero gate derives and scans one complete native 4Kn block");
    T_OK(mock_direct_pread_calls > 0,
         "retained zero gate reaches its O_DIRECT-compatible pread");
    T_EQ(mock_misaligned_pread_calls, 0,
         "zero scan buffer, offsets, and lengths satisfy retained O_DIRECT alignment");
    mock_direct_pread_fd = -1;
    T_EQ(pwrite(fd, &one, 1, 2 * 4096 + 31), 1,
         "placed one nonzero byte in the next native block");
    T_EQ(exitos_devguard_window_is_zero_fd_at(fd, f.root, 1, 2, &bad), 1,
         "fd zero gate scans the whole requested 4Kn byte window");
    T_EQ(bad, 2, "fd zero gate reports bad LBA in native units");

    mock_block_fd = sfd;
    mock_short_pread_fd = sfd;
    mock_short_pread_calls = 0;
    T_OK(exitos_devguard_pin_capture_fd_at(sfd, f.root, 10, 2, &short_pin) != 0,
         "every short sampled pread on a retained fd fails closed");
    T_OK(mock_short_pread_calls > 0,
         "retained pin short-read fixture reaches the sampled pread");
    mock_short_pread_fd = -1;

    T_EQ(write(vfd, "sentinel", 8), 8, "wrote symlink-victim sentinel");
    close(vfd); vfd = -1;
    unlink(link_path);
    T_EQ(symlink(victim, link_path), 0, "made a hostile pin destination symlink");
    mock_block_fd = fd;
    T_OK(exitos_devguard_pin_save(&pin, link_path) != 0,
         "pin save refuses an existing symlink destination");
    vfd = open(victim, O_RDONLY);
    T_EQ(read(vfd, victim_buf, sizeof victim_buf), 8,
         "symlink victim length remains unchanged");
    T_OK(memcmp(victim_buf, "sentinel", 8) == 0,
         "symlink victim contents remain unchanged");

out:
    mock_block_fd = -1;
    mock_short_pread_fd = -1;
    mock_direct_pread_fd = -1;
    iopath_close(io);
    free(write_buf);
    if (fd >= 0) close(fd);
    if (pfd >= 0) close(pfd);
    if (vfd >= 0) close(vfd);
    if (lfd >= 0) close(lfd);
    if (sfd >= 0) close(sfd);
    unlink(backing);
    if (moved[0]) unlink(moved);
    unlink(pin_path);
    unlink(victim);
    unlink(link_path);
    unlink(short_path);
    nftw(f.root, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void test_pin_parent_dirfd_safety(void)
{
    struct devguard_pin pin = {
        .serial = "PIN-DIRFD",
        .size_bytes = 4096,
        .sample_hash = 7,
        .zero_lba_start = 8,
        .zero_lba_count = 2,
        .logical_block_size = 4096,
        .nsamples = 64,
    };
    struct devguard_pin loaded;
    char root[] = "/tmp/exitos-pin-parent-XXXXXX";
    char parent[256], moved[256], attacker[256], link[256];
    char linked_pin[PATH_MAX], attacker_linked_pin[PATH_MAX];
    char pin_path[PATH_MAX], moved_pin[PATH_MAX], victim[PATH_MAX];
    char victim_buf[32] = {0};
    int fd = -1, rc;

    T_OK(mkdtemp(root) != NULL, "created isolated pin-parent race fixture");
    snprintf(parent, sizeof parent, "%s/parent", root);
    snprintf(moved, sizeof moved, "%s/parent-moved", root);
    snprintf(attacker, sizeof attacker, "%s/attacker", root);
    snprintf(link, sizeof link, "%s/parent-link", root);
    snprintf(linked_pin, sizeof linked_pin, "%s/pin-via-link", link);
    snprintf(attacker_linked_pin, sizeof attacker_linked_pin,
             "%s/pin-via-link", attacker);
    snprintf(pin_path, sizeof pin_path, "%s/pin", parent);
    snprintf(moved_pin, sizeof moved_pin, "%s/pin", moved);
    snprintf(victim, sizeof victim, "%s/pin", attacker);
    T_EQ(make_dir(parent), 0, "created the intended pin parent");
    T_EQ(make_dir(attacker), 0, "created an attacker-controlled directory");
    T_EQ(symlink(attacker, link), 0,
         "placed a symlink in the destination's parent path");

    T_OK(exitos_devguard_pin_save(&pin, linked_pin) != 0,
         "pin save rejects a symlink anywhere in the parent path");
    T_OK(access(attacker_linked_pin, F_OK) != 0,
         "a symlinked parent cannot make pin save escape into its target");

    T_EQ(put_text(victim, "victim-sentinel"), 0,
         "prepared an attacker victim for the parent replacement race");
    memset(&pin_parent_swap, 0, sizeof pin_parent_swap);
    pin_parent_swap.armed = 1;
    snprintf(pin_parent_swap.parent, sizeof pin_parent_swap.parent, "%s", parent);
    snprintf(pin_parent_swap.moved, sizeof pin_parent_swap.moved, "%s", moved);
    snprintf(pin_parent_swap.attacker, sizeof pin_parent_swap.attacker,
             "%s", attacker);
    rc = exitos_devguard_pin_save(&pin, pin_path);
    pin_parent_swap.armed = 0;

    T_EQ(pin_parent_swap.attempted, 1,
         "replaced the parent after the completed temporary pin was fsynced");
    T_EQ(pin_parent_swap.succeeded, 1,
         "installed the attacker parent and colliding temporary basename");
    T_EQ(rc, 0, "pin save remains bound to its originally opened parent dirfd");
    fd = open(victim, O_RDONLY | O_CLOEXEC);
    T_OK(fd >= 0, "attacker victim still exists after the parent replacement");
    if (fd >= 0) {
        ssize_t n = read(fd, victim_buf, sizeof victim_buf);
        T_EQ(n, 15, "attacker victim length remains unchanged");
        T_OK(n == 15 && memcmp(victim_buf, "victim-sentinel", 15) == 0,
             "parent replacement cannot overwrite the attacker victim");
        close(fd);
        fd = -1;
    }
    T_EQ(exitos_devguard_pin_load(&loaded, moved_pin), 0,
         "the saved pin stays in the originally opened directory object");
    T_OK(strcmp(loaded.serial, pin.serial) == 0,
         "the dirfd-bound save contains the intended pin data");

    if (fd >= 0) close(fd);
    memset(&pin_parent_swap, 0, sizeof pin_parent_swap);
    nftw(root, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void test_pin_load_dirfd_safety(void)
{
    static const char trusted[] =
        "serial=TRUSTED-NS\nsize=4096\nhash=7\nzlba=8\nzcnt=2\nlbs=4096\nn=64\n";
    static const char attacker_pin[] =
        "serial=ATTACKER-NS\nsize=4096\nhash=9\nzlba=8\nzcnt=2\nlbs=4096\nn=64\n";
    struct devguard_pin loaded;
    char root[] = "/tmp/exitos-pin-load-XXXXXX";
    char trusted_target[PATH_MAX], final_link[PATH_MAX];
    char attacker[256], attacker_pin_path[PATH_MAX];
    char parent_link[256], linked_pin[PATH_MAX];
    char parent[256], moved[256], pin_path[PATH_MAX];

    T_OK(mkdtemp(root) != NULL, "created isolated pin-load path fixture");
    snprintf(trusted_target, sizeof trusted_target, "%s/trusted.pin", root);
    snprintf(final_link, sizeof final_link, "%s/final-link.pin", root);
    snprintf(attacker, sizeof attacker, "%s/attacker", root);
    snprintf(attacker_pin_path, sizeof attacker_pin_path, "%s/pin", attacker);
    snprintf(parent_link, sizeof parent_link, "%s/parent-link", root);
    snprintf(linked_pin, sizeof linked_pin, "%s/pin", parent_link);
    snprintf(parent, sizeof parent, "%s/parent", root);
    snprintf(moved, sizeof moved, "%s/parent-moved", root);
    snprintf(pin_path, sizeof pin_path, "%s/pin", parent);

    T_EQ(make_dir(attacker), 0, "created attacker pin directory");
    T_EQ(make_dir(parent), 0, "created trusted pin directory");
    T_EQ(put_text(trusted_target, trusted), 0, "wrote trusted final-link target");
    T_EQ(put_text(attacker_pin_path, attacker_pin), 0,
         "wrote attacker-controlled pin");
    T_EQ(put_text(pin_path, trusted), 0, "wrote trusted race pin");

    T_EQ(symlink(trusted_target, final_link), 0,
         "made a final-component pin symlink");
    T_OK(exitos_devguard_pin_load(&loaded, final_link) != 0,
         "pin load refuses a final-component symlink");

    T_EQ(symlink(attacker, parent_link), 0,
         "made a symlink in the pin parent path");
    T_OK(exitos_devguard_pin_load(&loaded, linked_pin) != 0,
         "pin load refuses a symlink in any parent component");

    memset(&pin_load_parent_swap, 0, sizeof pin_load_parent_swap);
    pin_load_parent_swap.armed = 1;
    snprintf(pin_load_parent_swap.pin_path,
             sizeof pin_load_parent_swap.pin_path, "%s", pin_path);
    snprintf(pin_load_parent_swap.leaf, sizeof pin_load_parent_swap.leaf, "pin");
    snprintf(pin_load_parent_swap.parent,
             sizeof pin_load_parent_swap.parent, "%s", parent);
    snprintf(pin_load_parent_swap.moved,
             sizeof pin_load_parent_swap.moved, "%s", moved);
    snprintf(pin_load_parent_swap.attacker,
             sizeof pin_load_parent_swap.attacker, "%s", attacker);
    T_EQ(exitos_devguard_pin_load(&loaded, pin_path), 0,
         "pin load succeeds across a parent pathname replacement");
    pin_load_parent_swap.armed = 0;
    T_EQ(pin_load_parent_swap.attempted, 1,
         "pin-load race replaced the parent at the open boundary");
    T_EQ(pin_load_parent_swap.succeeded, 1,
         "pin-load race installed the attacker parent");
    T_OK(strcmp(loaded.serial, "TRUSTED-NS") == 0,
         "pin load remains bound to the originally opened trusted parent");

    memset(&pin_load_parent_swap, 0, sizeof pin_load_parent_swap);
    nftw(root, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void test_iopath_gate(void)
{
    char path[] = "/tmp/exitos-pwrite-safe-XXXXXX";
    char dir[] = "/tmp/exitos-passthru-decoy-XXXXXX";
    char decoy[256];
    int fd = mkstemp(path);
    int dfd = -1;
    struct iopath *p;
    if (fd >= 0) {
        T_EQ(ftruncate(fd, 8192), 0, "sized pwrite gate fixture");
        close(fd);
    }
    p = iopath_open(path, IOPATH_PWRITE, 4096);
    T_OK(p != NULL, "pwrite backend remains available for ordinary byte storage");
    iopath_close(p);
    T_OK(iopath_open(path, IOPATH_NVME_IOCTL, 4096) == NULL,
         "native passthrough refuses a non-block target at open");
    if (mkdtemp(dir)) {
        snprintf(decoy, sizeof decoy, "%s/nvme0n1", dir);
        dfd = open(decoy, O_RDWR | O_CREAT | O_TRUNC, 0600);
        T_OK(dfd >= 0, "created a regular-file whole-namespace basename decoy");
        if (dfd >= 0) close(dfd);
        T_EQ(exitos_devguard_passthru_ok(decoy), 0,
             "path passthrough gate cannot be fooled by a whole-device basename");
        unlink(decoy);
        rmdir(dir);
    } else {
        T_SKIP("could not create passthrough decoy directory");
    }
    unlink(path);
}

int main(void)
{
    test_sysfs_identity();
    test_fd_resolver_and_mountinfo();
    test_fd_bound_lba_span();
    test_fd_bound_poll_available();
    test_4kn_zero_window();
    test_pin_strictness();
    test_fd_bound_pin_zero_and_save();
    test_pin_parent_dirfd_safety();
    test_pin_load_dirfd_safety();
    test_iopath_gate();
    T_DONE();
}
