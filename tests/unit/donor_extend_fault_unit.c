/* Device-free fault-injection contract for donor_extend().
 * Built by test_donor_extend_faults.sh with allocator/ioctl names rewritten in
 * src/donor.c only. Real temporary regular files supply fstat/fcntl semantics;
 * no block device or mount is opened.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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

enum write_fault {
    WRITE_NORMAL,
    WRITE_EINTR_THEN_SHORT,
    WRITE_ZERO,
    WRITE_ERROR
};

static unsigned g_checks, g_failures;
static unsigned g_move_calls, g_target_writes, g_bad_alignment;
static int g_target_fd = -1;
static uint64_t g_block_size = 4096;
static enum write_fault g_write_fault;
static int g_fail_realloc_once;
static int g_fail_posix_once;

#define CHECK(cond, fmt, ...) do {                                           \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
        g_failures++;                                                        \
        fprintf(stderr, "not ok %u - " fmt "\n", g_checks, ##__VA_ARGS__); \
    } else {                                                                 \
        printf("ok %u - " fmt "\n", g_checks, ##__VA_ARGS__);             \
    }                                                                        \
} while (0)

void *donor_test_realloc(void *ptr, size_t size)
{
    if (g_fail_realloc_once) {
        g_fail_realloc_once = 0;
        errno = ENOMEM;
        return NULL;
    }
    return realloc(ptr, size);
}

int donor_test_posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (g_fail_posix_once) {
        g_fail_posix_once = 0;
        return ENOMEM;
    }
    return posix_memalign(memptr, alignment, size);
}

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

int exitos_internal_open_call(const char *path, int flags, mode_t mode)
{
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}
int exitos_internal_openat_call(int dirfd, const char *path, int flags,
                                mode_t mode)
{
    return (int)syscall(SYS_openat, dirfd, path, flags, mode);
}
int exitos_internal_close_call(int fd) { return (int)syscall(SYS_close, fd); }
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
    size_t issue = len;
    if (fd != g_target_fd)
        return (ssize_t)syscall(SYS_pwrite64, fd, buf, len, off);

    g_target_writes++;
    if ((uintptr_t)buf % g_block_size)
        g_bad_alignment++;
    if (g_write_fault == WRITE_EINTR_THEN_SHORT && g_target_writes == 1) {
        errno = EINTR;
        return -1;
    }
    if (g_write_fault == WRITE_ZERO)
        return 0;
    if (g_write_fault == WRITE_ERROR) {
        errno = EIO;
        return -1;
    }
    if (g_write_fault == WRITE_EINTR_THEN_SHORT && issue > g_block_size)
        issue = (size_t)g_block_size;
    return (ssize_t)syscall(SYS_pwrite64, fd, buf, issue, off);
}
int exitos_internal_fdatasync_call(int fd)
{
    return (int)syscall(SYS_fdatasync, fd);
}
void exitos_internal_fd_enter(int fd) { (void)fd; }
void exitos_internal_fd_leave(int fd) { (void)fd; }

static int raw_open(const char *path, int flags, mode_t mode)
{
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

static void reset_target(enum write_fault fault)
{
    g_write_fault = fault;
    g_target_writes = 0;
    g_bad_alignment = 0;
}

static struct donor_pool *make_pool(const char *dir, uint64_t bytes)
{
    if (mkdir(dir, 0700) != 0)
        return NULL;
    return donor_pool_create(dir, 1, bytes);
}

int main(void)
{
    char root[] = "/tmp/exitos-donor-extend-fault-XXXXXX";
    char pool1[512], pool2[512], target1[512], target2[512];
    struct donor_pool *p1 = NULL, *p2 = NULL;
    int64_t rc;
    unsigned moves_before;

    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(pool1, sizeof pool1, "%s/pool1", root);
    snprintf(pool2, sizeof pool2, "%s/pool2", root);
    snprintf(target1, sizeof target1, "%s/target1", root);
    snprintf(target2, sizeof target2, "%s/target2", root);

    p1 = make_pool(pool1, 64 * 1024);
    CHECK(p1 != NULL, "64 KiB device-free pool created");
    if (!p1)
        goto out;
    g_block_size = donor_pool_blocksize(p1);
    g_target_fd = raw_open(target1,
                           O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(g_target_fd >= 0, "fault-test target opened");
    if (g_target_fd < 0)
        goto out;

    reset_target(WRITE_EINTR_THEN_SHORT);
    rc = donor_extend(p1, g_target_fd, 0, 16 * 1024);
    CHECK(rc == 16 * 1024,
          "donor_extend succeeds after EINTR and repeated short writes (rc=%lld)",
          (long long)rc);
    CHECK(g_target_writes >= 5,
          "EINTR was retried and four block-sized short writes completed (calls=%u)",
          g_target_writes);
    CHECK(g_bad_alignment == 0,
          "every target initialization buffer is filesystem-block aligned");

    reset_target(WRITE_ZERO);
    rc = donor_extend(p1, g_target_fd, 16 * 1024, 16 * 1024);
    CHECK(rc == -EIO,
          "zero-progress target pwrite is an error, never false success (rc=%lld)",
          (long long)rc);

    reset_target(WRITE_ERROR);
    rc = donor_extend(p1, g_target_fd, 32 * 1024, 16 * 1024);
    CHECK(rc == -EIO,
          "target pwrite failure is returned as negative errno (rc=%lld)",
          (long long)rc);

    reset_target(WRITE_NORMAL);
    g_fail_posix_once = 1;
    rc = donor_extend(p1, g_target_fd, 48 * 1024, 16 * 1024);
    CHECK(rc == -ENOMEM,
          "aligned zero-buffer allocation failure is returned (rc=%lld)",
          (long long)rc);

    (void)syscall(SYS_close, g_target_fd);
    g_target_fd = -1;

    /* A separate fresh pool makes its first chunk-table growth observable. */
    p2 = make_pool(pool2, 16 * 1024);
    CHECK(p2 != NULL, "fresh pool for metadata-allocation failure created");
    if (!p2)
        goto out;
    g_block_size = donor_pool_blocksize(p2);
    g_target_fd = raw_open(target2,
                           O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(g_target_fd >= 0, "metadata-failure target opened");
    reset_target(WRITE_NORMAL);
    moves_before = g_move_calls;
    g_fail_realloc_once = 1;
    rc = donor_extend(p2, g_target_fd, 0, 16 * 1024);
    CHECK(rc == -ENOMEM,
          "chunk-table OOM is reported before mutation (rc=%lld)",
          (long long)rc);
    CHECK(g_move_calls == moves_before,
          "MOVE_EXT was not issued after chunk metadata reservation failed");

out:
    if (g_target_fd >= 0)
        (void)syscall(SYS_close, g_target_fd);
    donor_pool_destroy(p1);
    donor_pool_destroy(p2);
    (void)unlink(target1);
    (void)unlink(target2);
    (void)rmdir(pool1);
    (void)rmdir(pool2);
    (void)rmdir(root);
    printf("1..%u  (%u failed)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
