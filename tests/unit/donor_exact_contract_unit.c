/* Device-free contract test for donor_prepare_exact().
 * MOVE_EXT and FIEMAP are deterministic fakes; all file descriptor validation,
 * allocation, pwrite and fdatasync calls use real temporary regular files.
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

#include <linux/fiemap.h>
#include <linux/fs.h>

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
#ifndef FS_IOC_FIEMAP
#define FS_IOC_FIEMAP _IOWR('f', 11, struct fiemap)
#endif

enum fiemap_mode {
    MAP_VALID,
    MAP_HOLE,
    MAP_UNWRITTEN,
    MAP_UNSAFE,
    MAP_IOCTL_ERROR
};

enum move_mode {
    MOVE_VALID,
    MOVE_ZERO,
    MOVE_ERROR,
    MOVE_PARTIAL_ERROR
};

static unsigned g_checks, g_failures;
static unsigned g_move_calls, g_target_syncs, g_fiemap_calls;
static unsigned g_fiemap_without_sync;
static int g_target_fd = -1;
static int g_fail_target_sync;
static enum fiemap_mode g_map_mode = MAP_VALID;
static enum move_mode g_move_mode = MOVE_VALID;
static uint64_t g_block_size = 4096;

static int make_case(const char *root, const char *name, int donors,
                     uint64_t donor_bytes, char *dir, size_t dir_n,
                     char *target, size_t target_n,
                     struct donor_pool **pool_out);
static void finish_case(struct donor_pool *pool, const char *dir,
                        const char *target);

#define CHECK(cond, fmt, ...) do {                                           \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
        g_failures++;                                                        \
        fprintf(stderr, "not ok %u - " fmt "\n", g_checks, ##__VA_ARGS__); \
    } else {                                                                 \
        printf("ok %u - " fmt "\n", g_checks, ##__VA_ARGS__);             \
    }                                                                        \
} while (0)

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
        if (g_move_mode == MOVE_ERROR) {
            errno = EIO;
            return -1;
        }
        if (g_move_mode == MOVE_PARTIAL_ERROR) {
            me->moved_len = me->len / 2;
            errno = EIO;
            return -1;
        }
        if (g_move_mode == MOVE_ZERO) {
            me->moved_len = 0;
            return 0;
        }
        me->moved_len = me->len;
        return 0;
    }
    if (req == FS_IOC_FIEMAP) {
        struct fiemap *fm = arg;
        struct fiemap_extent *fe = fm->fm_extents;
        g_fiemap_calls++;
        if (!(fm->fm_flags & FIEMAP_FLAG_SYNC))
            g_fiemap_without_sync++;
        if (g_map_mode == MAP_IOCTL_ERROR) {
            errno = EIO;
            return -1;
        }
        if (fm->fm_extent_count < 1) {
            errno = EINVAL;
            return -1;
        }
        memset(fe, 0, sizeof *fe);
        fm->fm_mapped_extents = 1;
        fe->fe_logical = fm->fm_start;
        fe->fe_length = fm->fm_length;
        fe->fe_physical = 16 * g_block_size;
        fe->fe_flags = FIEMAP_EXTENT_LAST;
        if (g_map_mode == MAP_HOLE) {
            fe->fe_logical += g_block_size;
            if (fe->fe_length > g_block_size)
                fe->fe_length -= g_block_size;
        } else if (g_map_mode == MAP_UNWRITTEN) {
            fe->fe_flags |= FIEMAP_EXTENT_UNWRITTEN;
        } else if (g_map_mode == MAP_UNSAFE) {
            fe->fe_flags |= FIEMAP_EXTENT_DELALLOC;
        }
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
    return (ssize_t)syscall(SYS_pwrite64, fd, buf, len, off);
}
int exitos_internal_fdatasync_call(int fd)
{
    if (fd == g_target_fd) {
        g_target_syncs++;
        if (g_fail_target_sync) {
            errno = EIO;
            return -1;
        }
    }
    return (int)syscall(SYS_fdatasync, fd);
}
void exitos_internal_fd_enter(int fd) { (void)fd; }
void exitos_internal_fd_leave(int fd) { (void)fd; }

static int raw_open(const char *path, int flags, mode_t mode)
{
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

static void reset_case(enum fiemap_mode mode)
{
    g_map_mode = mode;
    g_move_mode = MOVE_VALID;
    g_move_calls = 0;
    g_target_syncs = 0;
    g_fiemap_calls = 0;
    g_fiemap_without_sync = 0;
    g_fail_target_sync = 0;
}

static void run_fallocate_only_mutation(const char *root, const char *name,
                                        enum move_mode move_mode, int expected)
{
    char dir[512], target[512];
    struct donor_pool *pool = NULL;
    struct donor_prepare_report report;
    int rc;

    CHECK(make_case(root, name, 1, 16 * 1024,
                    dir, sizeof dir, target, sizeof target, &pool) == 0,
          "%s fixture created", name);
    if (!pool || g_target_fd < 0) {
        finish_case(pool, dir, target);
        return;
    }
    reset_case(MAP_VALID);
    g_move_mode = move_mode;
    rc = donor_prepare_exact(pool, g_target_fd, 0, 16 * 1024, &report);
    CHECK(rc == expected, "%s returns %d (rc=%d)", name, expected, rc);
    CHECK((report.flags & DONOR_PREPARE_MUTATED) &&
              report.prepared_bytes == 0 && report.chunks == 0,
          "%s marks target fallocate mutation without claiming MOVE bytes", name);
    CHECK(!(report.flags & (DONOR_PREPARE_SYNCED | DONOR_PREPARE_VERIFIED)) &&
              g_target_syncs == 0 && g_fiemap_calls == 0,
          "%s failure does not claim sync or verification", name);
    finish_case(pool, dir, target);
}

static void run_partial_move_error(const char *root)
{
    char dir[512], target[512];
    struct donor_pool *pool = NULL;
    struct donor_prepare_report report;
    unsigned moves_before;
    int rc;

    CHECK(make_case(root, "move-partial-error", 1, 16 * 1024,
                    dir, sizeof dir, target, sizeof target, &pool) == 0,
          "partial MOVE-error fixture created");
    if (!pool || g_target_fd < 0) {
        finish_case(pool, dir, target);
        return;
    }
    reset_case(MAP_VALID);
    g_move_mode = MOVE_PARTIAL_ERROR;
    rc = donor_prepare_exact(pool, g_target_fd, 0, 16 * 1024, &report);
    CHECK(rc == -EIO, "partial MOVE error preserves ioctl errno (rc=%d)", rc);
    CHECK((report.flags & DONOR_PREPARE_MUTATED) &&
              report.prepared_bytes == 8 * 1024 && report.chunks == 1,
          "partial MOVE error reports the 8 KiB already exchanged");
    CHECK(!(report.flags & (DONOR_PREPARE_SYNCED | DONOR_PREPARE_VERIFIED)) &&
              g_target_syncs == 0 && g_fiemap_calls == 0,
          "partial MOVE error cannot claim completed prepare");

    reset_case(MAP_VALID);
    moves_before = g_move_calls;
    rc = donor_prepare_exact(pool, g_target_fd, 0, 4096, &report);
    CHECK(rc == -EUCLEAN && g_move_calls == moves_before,
          "a pool with ambiguous partial MOVE state is permanently isolated");
    CHECK(report.prepared_bytes == 0 && report.flags == 0,
          "isolated pool performs no further target mutation");
    finish_case(pool, dir, target);
}

static int make_case(const char *root, const char *name, int donors,
                     uint64_t donor_bytes, char *dir, size_t dir_n,
                     char *target, size_t target_n,
                     struct donor_pool **pool_out)
{
    snprintf(dir, dir_n, "%s/%s-pool", root, name);
    snprintf(target, target_n, "%s/%s-target", root, name);
    if (mkdir(dir, 0700) != 0)
        return -1;
    *pool_out = donor_pool_create(dir, donors, donor_bytes);
    if (!*pool_out)
        return -1;
    g_block_size = donor_pool_blocksize(*pool_out);
    g_target_fd = raw_open(target,
                           O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    return g_target_fd < 0 ? -1 : 0;
}

static void finish_case(struct donor_pool *pool, const char *dir,
                        const char *target)
{
    if (g_target_fd >= 0)
        (void)syscall(SYS_close, g_target_fd);
    g_target_fd = -1;
    donor_pool_destroy(pool);
    (void)unlink(target);
    (void)rmdir(dir);
}

static void run_map_rejection(const char *root, const char *name,
                              enum fiemap_mode mode, int expected)
{
    char dir[512], target[512];
    struct donor_pool *pool = NULL;
    struct donor_prepare_report report;
    int rc;

    CHECK(make_case(root, name, 1, 16 * 1024,
                    dir, sizeof dir, target, sizeof target, &pool) == 0,
          "%s fixture created", name);
    if (!pool || g_target_fd < 0) {
        finish_case(pool, dir, target);
        return;
    }
    reset_case(mode);
    rc = donor_prepare_exact(pool, g_target_fd, 0, 16 * 1024, &report);
    CHECK(rc == expected, "%s is rejected with %d (rc=%d)",
          name, expected, rc);
    CHECK(report.prepared_bytes == 16 * 1024 && report.chunks == 1 &&
              (report.flags & DONOR_PREPARE_MUTATED),
          "%s reports the mutation already performed before verification", name);
    CHECK((report.flags & DONOR_PREPARE_SYNCED) &&
              !(report.flags & DONOR_PREPARE_VERIFIED),
          "%s reports synced but not verified", name);
    CHECK(g_target_syncs == 1 && g_fiemap_calls == 1 &&
              g_fiemap_without_sync == 0,
          "%s used one target sync and FIEMAP_FLAG_SYNC", name);
    finish_case(pool, dir, target);
}

int main(void)
{
    char root[] = "/tmp/exitos-donor-exact-XXXXXX";
    char dir[512], target[512];
    struct donor_pool *pool = NULL;
    struct donor_prepare_report report;
    int rc;

    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }

    CHECK(make_case(root, "success", 2, 16 * 1024,
                    dir, sizeof dir, target, sizeof target, &pool) == 0,
          "two-donor exact-success fixture created");
    if (pool && g_target_fd >= 0) {
        reset_case(MAP_VALID);
        rc = donor_prepare_exact(pool, g_target_fd, 0, 32 * 1024, &report);
        CHECK(rc == 0, "exact 32 KiB preparation succeeds (rc=%d)", rc);
        CHECK(report.requested_bytes == 32 * 1024 &&
                  report.prepared_bytes == 32 * 1024 && report.chunks == 2,
              "report records two partial donor handouts totaling exactly 32 KiB");
        CHECK((report.flags & (DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED |
                               DONOR_PREPARE_VERIFIED)) ==
                  (DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED |
                   DONOR_PREPARE_VERIFIED),
              "successful report records mutation, persistence and verification");
        CHECK(g_move_calls == 2 && g_target_syncs == 1 && g_fiemap_calls == 1,
              "partial donors loop to exact, then one sync and one FIEMAP");
        CHECK(g_fiemap_without_sync == 0,
              "successful coverage verification sets FIEMAP_FLAG_SYNC");
    }
    finish_case(pool, dir, target);
    pool = NULL;

    run_map_rejection(root, "hole", MAP_HOLE, -ENXIO);
    run_map_rejection(root, "unwritten", MAP_UNWRITTEN, -EUCLEAN);
    run_map_rejection(root, "unsafe", MAP_UNSAFE, -EUCLEAN);

    run_fallocate_only_mutation(root, "move-zero", MOVE_ZERO, -ENOSPC);
    run_fallocate_only_mutation(root, "move-error", MOVE_ERROR, -EIO);
    run_partial_move_error(root);

    CHECK(make_case(root, "sync-error", 1, 16 * 1024,
                    dir, sizeof dir, target, sizeof target, &pool) == 0,
          "fdatasync-error fixture created");
    if (pool && g_target_fd >= 0) {
        reset_case(MAP_VALID);
        g_fail_target_sync = 1;
        rc = donor_prepare_exact(pool, g_target_fd, 0, 16 * 1024, &report);
        CHECK(rc == -EIO, "target fdatasync failure is returned (rc=%d)", rc);
        CHECK((report.flags & DONOR_PREPARE_MUTATED) &&
                  !(report.flags & (DONOR_PREPARE_SYNCED |
                                    DONOR_PREPARE_VERIFIED)),
              "sync failure report preserves mutation without claiming sync/verify");
        CHECK(g_target_syncs == 1 && g_fiemap_calls == 0,
              "FIEMAP is not attempted after fdatasync fails");
    }
    finish_case(pool, dir, target);
    pool = NULL;

    CHECK(make_case(root, "partial", 1, 16 * 1024,
                    dir, sizeof dir, target, sizeof target, &pool) == 0,
          "partial-capacity fixture created");
    if (pool && g_target_fd >= 0) {
        reset_case(MAP_VALID);
        rc = donor_prepare_exact(pool, g_target_fd, 0, 32 * 1024, &report);
        CHECK(rc == -ENOSPC, "partial pool is failure, never exact success (rc=%d)", rc);
        CHECK(report.prepared_bytes == 16 * 1024 && report.chunks == 1 &&
                  (report.flags & DONOR_PREPARE_MUTATED),
              "partial failure reports exactly the 16 KiB already mutated");
        CHECK(!(report.flags & DONOR_PREPARE_VERIFIED),
              "partial preparation never claims FIEMAP verification");
    }
    finish_case(pool, dir, target);
    pool = NULL;

    CHECK(make_case(root, "alignment", 1, 16 * 1024,
                    dir, sizeof dir, target, sizeof target, &pool) == 0,
          "alignment-refusal fixture created");
    if (pool && g_target_fd >= 0) {
        reset_case(MAP_VALID);
        rc = donor_prepare_exact(pool, g_target_fd, 1, 4096, &report);
        CHECK(rc == -EINVAL && report.flags == 0 && report.prepared_bytes == 0,
              "misaligned exact request is rejected before mutation");
        CHECK(g_move_calls == 0 && g_target_syncs == 0 && g_fiemap_calls == 0,
              "invalid request performs no MOVE, sync or FIEMAP");
    }
    finish_case(pool, dir, target);

    (void)rmdir(root);
    printf("1..%u  (%u failed)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
