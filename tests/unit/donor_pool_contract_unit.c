/* Device-free donor pool contract test.
 *
 * This file is deliberately not named test_*.c: the companion shell test
 * builds it together with src/donor.c so the direct-call seam and MOVE_EXT can
 * be observed without linking the rest of libexitos or touching a device.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "exitos_donor.h"

struct test_move_extent {
    uint32_t reserved;
    uint32_t donor_fd;
    uint64_t orig_start;
    uint64_t donor_start;
    uint64_t len;
    uint64_t moved_len;
};

#ifndef EXT4_IOC_MOVE_EXT
#define EXT4_IOC_MOVE_EXT _IOWR('f', 15, struct test_move_extent)
#endif

static unsigned g_checks;
static unsigned g_failures;
static unsigned g_open_calls;
static unsigned g_openat_calls;
static unsigned g_child_open_calls;
static unsigned g_close_calls;
static unsigned g_fallocate_calls;
static unsigned g_ftruncate_calls;
static unsigned g_pwrite_calls;
static unsigned g_fdatasync_calls;
static unsigned g_move_calls;
static unsigned g_bad_open_flags;
static unsigned g_mutex_init_calls;
static unsigned g_mutex_destroy_calls;
static uint64_t g_forced_available;

#define CHECK(cond, fmt, ...) do {                                           \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
        g_failures++;                                                        \
        fprintf(stderr, "not ok %u - " fmt "\n", g_checks, ##__VA_ARGS__); \
    } else {                                                                 \
        printf("ok %u - " fmt "\n", g_checks, ##__VA_ARGS__);             \
    }                                                                        \
} while (0)

static int raw_open(const char *path, int flags, mode_t mode)
{
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

int exitos_internal_open_call(const char *path, int flags, mode_t mode)
{
    const int required = O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    g_open_calls++;
    if ((flags & required) != required || (flags & (O_CREAT | O_TRUNC)))
        g_bad_open_flags++;
    return raw_open(path, flags, mode);
}

int exitos_internal_openat_call(int dirfd, const char *path, int flags,
                                mode_t mode)
{
    int required = O_NOFOLLOW | O_CLOEXEC;
    g_openat_calls++;
    if (!strcmp(path, ".")) {
        required |= O_DIRECTORY;
        if ((flags & required) != required || (flags & (O_CREAT | O_TRUNC)))
            g_bad_open_flags++;
    } else {
        required |= O_EXCL;
        g_child_open_calls++;
        if ((flags & required) != required || (flags & O_TRUNC))
            g_bad_open_flags++;
    }
    return (int)syscall(SYS_openat, dirfd, path, flags, mode);
}

int exitos_internal_close_call(int fd)
{
    g_close_calls++;
    return (int)syscall(SYS_close, fd);
}

int exitos_internal_ftruncate_call(int fd, off_t len)
{
    g_ftruncate_calls++;
    return (int)syscall(SYS_ftruncate, fd, len);
}

int exitos_internal_fallocate_call(int fd, int mode, off_t off, off_t len)
{
    int rc;
    g_fallocate_calls++;
    rc = (int)syscall(SYS_fallocate, fd, mode, off, len);
    if (rc == 0 || mode != 0)
        return rc;
    /* The test only needs owned bytes, not a particular host filesystem's
     * fallocate implementation. Keep the direct-call boundary observable. */
    if (errno == EOPNOTSUPP || errno == ENOSYS)
        return (int)syscall(SYS_ftruncate, fd, off + len);
    return rc;
}

ssize_t exitos_internal_pwrite_call(int fd, const void *buf, size_t len,
                                    off_t off)
{
    g_pwrite_calls++;
    return (ssize_t)syscall(SYS_pwrite64, fd, buf, len, off);
}

int exitos_internal_fdatasync_call(int fd)
{
    g_fdatasync_calls++;
    return (int)syscall(SYS_fdatasync, fd);
}

void exitos_internal_fd_enter(int fd) { (void)fd; }
void exitos_internal_fd_leave(int fd) { (void)fd; }

int donor_test_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    (void)fd;
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (req == EXT4_IOC_MOVE_EXT) {
        struct test_move_extent *me = arg;
        g_move_calls++;
        me->moved_len = me->len;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

int donor_test_fstatvfs(int fd, struct statvfs *vfs)
{
    if (fstatvfs(fd, vfs) != 0)
        return -1;
    if (g_forced_available) {
        unsigned long unit = vfs->f_frsize ? vfs->f_frsize : vfs->f_bsize;
        vfs->f_bavail = (fsblkcnt_t)(g_forced_available / unit);
    }
    return 0;
}

int donor_test_mutex_init(pthread_mutex_t *mu,
                          const pthread_mutexattr_t *attr)
{
    g_mutex_init_calls++;
    return pthread_mutex_init(mu, attr);
}

int donor_test_mutex_destroy(pthread_mutex_t *mu)
{
    g_mutex_destroy_calls++;
    return pthread_mutex_destroy(mu);
}

static int read_all(const char *path, char *buf, size_t cap)
{
    int fd = raw_open(path, O_RDONLY | O_CLOEXEC, 0);
    ssize_t n;
    if (fd < 0)
        return -1;
    n = (ssize_t)syscall(SYS_read, fd, buf, cap);
    (void)syscall(SYS_close, fd);
    return (int)n;
}

static void reset_counts(void)
{
    g_open_calls = 0;
    g_openat_calls = 0;
    g_child_open_calls = 0;
    g_close_calls = 0;
    g_fallocate_calls = 0;
    g_ftruncate_calls = 0;
    g_pwrite_calls = 0;
    g_fdatasync_calls = 0;
    g_move_calls = 0;
    g_bad_open_flags = 0;
    g_mutex_init_calls = 0;
    g_mutex_destroy_calls = 0;
}

int main(void)
{
    char dir[] = "/tmp/exitos-donor-pool-contract-XXXXXX";
    char donor0[512], donor1[512], target[512];
    const char sentinel[] = "owned-by-someone-else";
    char got[sizeof sentinel] = {0};
    struct donor_pool *p;
    char nested[12][PATH_MAX];
    char longdir[PATH_MAX];
    unsigned nested_count = 0;
    int fd;
    int n;
    int64_t moved;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(donor0, sizeof donor0, "%s/donor-0000.dat", dir);
    snprintf(donor1, sizeof donor1, "%s/donor-0001.dat", dir);
    snprintf(target, sizeof target, "%s/target.dat", dir);

    /* MOVE_EXT swaps donor blocks with blocks allocated to each target, so a
     * pool whose free-space check covers donors only can fail halfway through
     * setup. 48 KiB fits 2x16 KiB donors, but not donors plus targets (64 KiB). */
    reset_counts();
    g_forced_available = 48 * 1024;
    p = donor_pool_create(dir, 2, 16 * 1024);
    CHECK(p == NULL,
          "capacity precheck reserves donors plus target-side swap blocks");
    CHECK(g_child_open_calls == 0,
          "insufficient two-sided capacity is rejected before creating files");
    if (p)
        donor_pool_destroy(p);
    g_forced_available = 0;

    /* Child creation is relative to a retained dirfd, so a valid directory
     * path no longer has to fit an arbitrary combined-path scratch buffer. */
    snprintf(longdir, sizeof longdir, "%s", dir);
    while (strlen(longdir) < 1015 && nested_count < 12) {
        char component[112];
        size_t dl, cl;
        memset(component, 'a' + (int)(nested_count % 20), sizeof component);
        component[100] = '\0';
        dl = strlen(longdir);
        cl = strlen(component);
        if (dl + 1 + cl + 1 > sizeof nested[nested_count])
            break;
        memcpy(nested[nested_count], longdir, dl);
        nested[nested_count][dl] = '/';
        memcpy(nested[nested_count] + dl + 1, component, cl + 1);
        if (mkdir(nested[nested_count], 0700) != 0)
            break;
        snprintf(longdir, sizeof longdir, "%s", nested[nested_count]);
        nested_count++;
    }
    CHECK(strlen(longdir) >= 1015,
          "constructed a valid directory longer than the old path buffer");
    reset_counts();
    p = donor_pool_create(longdir, 1, 16 * 1024);
    CHECK(p != NULL, "long valid directory works through relative openat");
    CHECK(g_child_open_calls == 1,
          "long path creates exactly one child below the retained dirfd");
    if (p) {
        donor_pool_destroy(p);
        p = NULL;
    }
    CHECK(g_mutex_init_calls == 1 && g_mutex_destroy_calls == 1,
          "long-path pool initializes and destroys its mutex exactly once");
    while (nested_count > 0)
        (void)rmdir(nested[--nested_count]);

    /* Put the collision at index 1 so failure cleanup must remove donor 0 but
     * must not unlink or truncate the pre-existing donor 1. */
    fd = raw_open(donor1, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0 || syscall(SYS_write, fd, sentinel, sizeof sentinel) !=
                     (long)sizeof sentinel) {
        perror("create sentinel");
        return 2;
    }
    (void)syscall(SYS_close, fd);

    reset_counts();
    errno = 0;
    p = donor_pool_create(dir, 2, 16 * 1024);
    CHECK(p == NULL && errno == EEXIST,
          "pool collision refuses and preserves EEXIST");
    if (p)
        donor_pool_destroy(p);
    n = read_all(donor1, got, sizeof got);
    CHECK(n == (int)sizeof sentinel && !memcmp(got, sentinel, sizeof sentinel),
          "collision file remains byte-for-byte intact");
    CHECK(access(donor0, F_OK) != 0,
          "failure cleanup removes only the donor file it created");
    CHECK(g_child_open_calls == 2,
          "both donor opens crossed the direct-call seam (calls=%u)",
          g_child_open_calls);
    CHECK(g_bad_open_flags == 0,
          "donor opens use O_EXCL|O_NOFOLLOW|O_CLOEXEC and never O_TRUNC");
    CHECK(g_mutex_init_calls == 1 && g_mutex_destroy_calls == 1,
          "collision failure destroys its initialized mutex exactly once");

    (void)unlink(donor1);
    reset_counts();
    p = donor_pool_create(dir, 2, 16 * 1024);
    CHECK(p != NULL, "fresh pool creation succeeds");
    if (!p)
        goto out;
    CHECK(g_child_open_calls == 2 && g_fallocate_calls == 2 &&
              g_pwrite_calls >= 2 && g_fdatasync_calls == 2,
          "pool open/allocate/initialize/sync all use direct calls");

    fd = raw_open(target, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0, "device-free regular-file target opened");
    if (fd >= 0) {
        unsigned falloc_before = g_fallocate_calls;
        unsigned pwrite_before = g_pwrite_calls;
        moved = donor_extend(p, fd, 0, 8 * 1024);
        CHECK(moved == 8 * 1024 && g_move_calls > 0,
              "fake MOVE_EXT lets donor_extend exercise its control path");
        CHECK(g_fallocate_calls > falloc_before &&
                  g_pwrite_calls > pwrite_before,
              "target allocation and initialization use direct calls");
        CHECK(donor_reclaim(p, fd, 0) == 0 && g_ftruncate_calls > 0,
              "reclaim truncation uses the direct-call seam");
        (void)syscall(SYS_close, fd);
    }

    /* ext4's EXT4_IOC_MOVE_EXT requires the original file description to
     * carry both FMODE_READ and FMODE_WRITE.  Accepting O_WRONLY here would
     * first fallocate the target and only then fail the ioctl with EBADF. */
    fd = raw_open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0, "write-only rejection target opened");
    if (fd >= 0) {
        struct stat before, after;
        unsigned falloc_before = g_fallocate_calls;
        unsigned pwrite_before = g_pwrite_calls;
        unsigned move_before = g_move_calls;
        unsigned ftruncate_before = g_ftruncate_calls;

        CHECK(fstat(fd, &before) == 0,
              "write-only target state captured before donor call");
        moved = donor_extend(p, fd, 0, 8 * 1024);
        CHECK(fstat(fd, &after) == 0,
              "write-only target state captured after donor call");
        CHECK(moved == -EBADF,
              "write-only target is rejected with EBADF before MOVE_EXT (rc=%lld)",
              (long long)moved);
        CHECK(g_fallocate_calls == falloc_before &&
                  g_pwrite_calls == pwrite_before &&
                  g_move_calls == move_before &&
                  g_ftruncate_calls == ftruncate_before &&
                  before.st_size == after.st_size &&
                  before.st_blocks == after.st_blocks,
              "write-only rejection performs no target mutation");
        (void)syscall(SYS_close, fd);
    }

    {
        unsigned closes_before = g_close_calls;
        donor_pool_destroy(p);
        p = NULL;
        CHECK(g_close_calls >= closes_before + 2,
              "pool destruction closes owned donor fds through the seam");
        CHECK(g_mutex_init_calls == 1 && g_mutex_destroy_calls == 1,
              "normal pool destruction destroys its mutex exactly once");
    }

out:
    if (p)
        donor_pool_destroy(p);
    (void)unlink(target);
    (void)unlink(donor0);
    (void)unlink(donor1);
    (void)rmdir(dir);
    printf("1..%u  (%u failed)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
