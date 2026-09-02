/* Contract tests for donor-pool construction errors and directory identity.
 *
 * This helper intentionally runs on any host filesystem.  The companion
 * script replaces only donor.c's fstatfs() result with ext4's magic; all
 * descriptor, capacity, rename, openat and unlinkat behaviour is real.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "exitos_donor.h"

#ifndef EXT4_SUPER_MAGIC
#define EXT4_SUPER_MAGIC 0xEF53
#endif

static unsigned g_checks;
static unsigned g_failures;
static int g_fail_next_calloc;

#define CHECK(cond, fmt, ...) do {                                           \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
        g_failures++;                                                        \
        fprintf(stderr, "not ok %u - " fmt "\n", g_checks, ##__VA_ARGS__); \
    } else {                                                                 \
        printf("ok %u - " fmt "\n", g_checks, ##__VA_ARGS__);             \
    }                                                                        \
} while (0)

void *donor_test_calloc(size_t n, size_t size)
{
    if (g_fail_next_calloc) {
        g_fail_next_calloc = 0;
        errno = ENOMEM;
        return NULL;
    }
    return calloc(n, size);
}

int donor_test_fstatfs(int fd, struct statfs *st)
{
    if (fstatfs(fd, st) != 0)
        return -1;
    st->f_type = EXT4_SUPER_MAGIC;
    return 0;
}

int exitos_internal_open_call(const char *path, int flags, mode_t mode)
{
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

int exitos_internal_openat_call(int dirfd, const char *path, int flags,
                                mode_t mode)
{
    return (int)syscall(SYS_openat, dirfd, path, flags, mode);
}

int exitos_internal_close_call(int fd)
{
    return (int)syscall(SYS_close, fd);
}

int exitos_internal_ftruncate_call(int fd, off_t len)
{
    return (int)syscall(SYS_ftruncate, fd, len);
}

int exitos_internal_fallocate_call(int fd, int mode, off_t off, off_t len)
{
    int rc = (int)syscall(SYS_fallocate, fd, mode, off, len);
    if (rc == 0 || mode != 0)
        return rc;
    if (errno == EOPNOTSUPP || errno == ENOSYS)
        return (int)syscall(SYS_ftruncate, fd, off + len);
    return rc;
}

ssize_t exitos_internal_pwrite_call(int fd, const void *buf, size_t len,
                                    off_t off)
{
    return (ssize_t)syscall(SYS_pwrite64, fd, buf, len, off);
}

int exitos_internal_fdatasync_call(int fd)
{
    return (int)syscall(SYS_fdatasync, fd);
}

void exitos_internal_fd_enter(int fd) { (void)fd; }
void exitos_internal_fd_leave(int fd) { (void)fd; }

static int open_raw(const char *path, int flags, mode_t mode)
{
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

static int write_sentinel(const char *path, const char *text)
{
    size_t len = strlen(text);
    int fd = open_raw(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    ssize_t wrote;

    if (fd < 0)
        return -1;
    wrote = (ssize_t)syscall(SYS_write, fd, text, len);
    (void)syscall(SYS_close, fd);
    return wrote == (ssize_t)len ? 0 : -1;
}

static int contents_equal(const char *path, const char *expected)
{
    char got[128] = {0};
    size_t len = strlen(expected);
    int fd = open_raw(path, O_RDONLY | O_CLOEXEC, 0);
    ssize_t n;

    if (fd < 0)
        return 0;
    n = (ssize_t)syscall(SYS_read, fd, got, sizeof got);
    (void)syscall(SYS_close, fd);
    return n == (ssize_t)len && memcmp(got, expected, len) == 0;
}

int main(void)
{
    char root[] = "/tmp/exitos-donor-dirfd-XXXXXX";
    char original[256];
    char parked[256];
    char replacement_donor[PATH_MAX];
    char parked_donor[PATH_MAX];
    char *too_long;
    struct donor_pool *pool = NULL;
    struct statvfs vfs;
    uint64_t blk;
    uint64_t avail;
    uint64_t impossible;
    int dirfd = -1;

    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(original, sizeof original, "%s/pool", root);
    snprintf(parked, sizeof parked, "%s/pool-renamed", root);
    snprintf(replacement_donor, sizeof replacement_donor,
             "%s/donor-0000.dat", original);
    snprintf(parked_donor, sizeof parked_donor,
             "%s/donor-0000.dat", parked);

    errno = 0;
    CHECK(donor_pool_create(NULL, 1, 4096) == NULL && errno == EINVAL,
          "NULL directory reports EINVAL");
    errno = 0;
    CHECK(donor_pool_create(root, 0, 4096) == NULL && errno == EINVAL,
          "non-positive donor count reports EINVAL");
    errno = 0;
    CHECK(donor_pool_create(root, 1, 0) == NULL && errno == EINVAL,
          "zero donor size reports EINVAL");
    errno = 0;
    CHECK(donor_pool_create(root, 1, UINT64_MAX) == NULL &&
              errno == EOVERFLOW,
          "alignment overflow reports EOVERFLOW");
    errno = 0;
    CHECK(donor_pool_create_at(-1, 1, 4096) == NULL && errno == EBADF,
          "invalid borrowed dirfd preserves EBADF");
    errno = 0;
    CHECK(donor_pool_create("/definitely/no/such/exitos-directory", 1,
                            4096) == NULL && errno == ENOENT,
          "path-open failure preserves the underlying errno");

    too_long = malloc((size_t)PATH_MAX + 2);
    if (!too_long)
        return 2;
    too_long[0] = '/';
    memset(too_long + 1, 'x', PATH_MAX);
    too_long[PATH_MAX + 1] = '\0';
    errno = 0;
    CHECK(donor_pool_create(too_long, 1, 4096) == NULL &&
              errno == ENAMETOOLONG,
          "overlong wrapper path preserves ENAMETOOLONG");
    free(too_long);

    CHECK(mkdir(original, 0700) == 0, "private donor directory created");
    dirfd = open_raw(original,
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    CHECK(dirfd >= 0, "verified directory fd opened");

    CHECK(fstatvfs(dirfd, &vfs) == 0, "real descriptor capacity read");
    blk = vfs.f_frsize ? (uint64_t)vfs.f_frsize : (uint64_t)vfs.f_bsize;
    if (blk == 0)
        blk = 4096;
    avail = (uint64_t)vfs.f_bavail > UINT64_MAX / blk ? UINT64_MAX :
            (uint64_t)vfs.f_bavail * blk;
    impossible = avail / 2u;
    if (impossible <= UINT64_MAX - blk)
        impossible += blk;
    impossible -= impossible % blk;
    errno = 0;
    CHECK(donor_pool_create_at(dirfd, 1, impossible) == NULL &&
              errno == ENOSPC,
          "real fstatvfs capacity refusal reports ENOSPC");
    CHECK(access(replacement_donor, F_OK) != 0,
          "capacity refusal creates no donor child");

    g_fail_next_calloc = 1;
    errno = 0;
    CHECK(donor_pool_create_at(dirfd, 1, blk) == NULL && errno == ENOMEM,
          "pool allocation failure reports ENOMEM");
    CHECK(access(replacement_donor, F_OK) != 0,
          "allocation failure creates no donor child");

    errno = 0;
    pool = donor_pool_create_at(dirfd, 1, blk);
    CHECK(pool != NULL, "dirfd pool creation succeeds (errno=%d)", errno);
    if (pool) {
        CHECK(rename(original, parked) == 0,
              "validated directory may be renamed after construction");
        CHECK(mkdir(original, 0700) == 0,
              "attacker-visible replacement pathname created");
        CHECK(write_sentinel(replacement_donor, "replacement-owned") == 0,
              "replacement contains same-name sentinel");
        donor_pool_destroy(pool);
        pool = NULL;
        CHECK(access(parked_donor, F_OK) != 0,
              "destroy unlinks donor from the original directory identity");
        CHECK(contents_equal(replacement_donor, "replacement-owned"),
              "destroy never follows replacement pathname or removes sentinel");
    }

    if (dirfd >= 0)
        (void)syscall(SYS_close, dirfd);
    (void)unlink(replacement_donor);
    (void)rmdir(original);
    (void)unlink(parked_donor);
    (void)rmdir(parked);
    (void)rmdir(root);
    printf("1..%u  (%u failed)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
