#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "exitos_frontend_fallocate.h"

#include "exitos_donor.h"
#include "exitos_frontend_config.h"
#include "exitos_intercept.h"
#include "exitos_iopath.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef EXT4_SUPER_MAGIC
#define EXT4_SUPER_MAGIC 0xEF53
#endif

struct exitos_frontend_fallocate_runtime {
    const struct exitos_frontend_config *config;
    struct exitos_ctx *ctx;
    pthread_mutex_t pool_mu;
    struct donor_pool *pool;
    uint64_t bytes_each;
};

const char *exitos_frontend_fallocate_outcome_name(
    enum exitos_frontend_fallocate_outcome outcome)
{
    switch (outcome) {
    case EXITOS_FALLOCATE_NATIVE_SKIP:
        return "native-skip";
    case EXITOS_FALLOCATE_ELIGIBLE_FAILED:
        return "eligible-attempt-failed";
    case EXITOS_FALLOCATE_PREPARED:
        return "prepared";
    case EXITOS_FALLOCATE_CONTROL_FAILED:
        return "control-failed";
    default:
        return "unknown";
    }
}

const char *exitos_frontend_fallocate_stage_name(
    enum exitos_frontend_fallocate_stage stage)
{
    switch (stage) {
    case EXITOS_FALLOCATE_STAGE_ENTRY:
        return "entry";
    case EXITOS_FALLOCATE_STAGE_TXN_BEGIN:
        return "txn-begin";
    case EXITOS_FALLOCATE_STAGE_FLUSH:
        return "flush";
    case EXITOS_FALLOCATE_STAGE_CANDIDATE:
        return "candidate";
    case EXITOS_FALLOCATE_STAGE_NATIVE:
        return "native";
    case EXITOS_FALLOCATE_STAGE_DONOR_DIR:
        return "donor-dir";
    case EXITOS_FALLOCATE_STAGE_DONOR_POOL:
        return "donor-pool";
    case EXITOS_FALLOCATE_STAGE_RDWR_ALIAS:
        return "rdwr-alias";
    case EXITOS_FALLOCATE_STAGE_REVALIDATE:
        return "revalidate";
    case EXITOS_FALLOCATE_STAGE_DONOR_PREPARE:
        return "donor-prepare";
    case EXITOS_FALLOCATE_STAGE_DONOR_PROOF:
        return "donor-proof";
    case EXITOS_FALLOCATE_STAGE_MAP_REFRESH:
        return "map-refresh";
    case EXITOS_FALLOCATE_STAGE_RDWR_ALIAS_CLOSE:
        return "rdwr-alias-close";
    case EXITOS_FALLOCATE_STAGE_COMPLETE:
        return "complete";
    default:
        return "unknown";
    }
}

static int target_candidate(
    const struct exitos_frontend_fallocate_runtime *runtime,
    const struct exitos_fd_txn *tx, int fd, int mode, off_t off, off_t len,
    struct stat *target_st, struct statfs *target_fs)
{
    int flags;

    if (!exitos_frontend_config_donor_fallocate(runtime->config) ||
        !exitos_fd_txn_registered(tx) || mode != 0 || off != 0 || len <= 0)
        return 0;
    if (fstat(fd, target_st) != 0)
        return -(errno ? errno : EIO);
    if (!S_ISREG(target_st->st_mode) || target_st->st_size != 0 ||
        target_st->st_blocks != 0)
        return 0;
    flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return -(errno ? errno : EIO);
    if ((flags & O_ACCMODE) == O_RDONLY)
        return 0;
    if (fstatfs(fd, target_fs) != 0)
        return -errno;
    if ((unsigned long)target_fs->f_type !=
        (unsigned long)EXT4_SUPER_MAGIC)
        return -EOPNOTSUPP;
    if (target_fs->f_bsize <= 0 ||
        (uint64_t)len % (uint64_t)target_fs->f_bsize != 0)
        return 0;
    if ((off_t)(uint64_t)len != len)
        return 0;
    return 1;
}

static int validate_donor_directory(
    const struct exitos_frontend_fallocate_runtime *runtime,
    const struct stat *target_st, const struct statfs *target_fs,
    int *dirfd_out)
{
    const char *dir = exitos_frontend_config_donor_dir(runtime->config);
    struct stat dir_st;
    struct statfs dir_fs;
    int dirfd;
    int rc = 0;

    if (!dirfd_out)
        return -EINVAL;
    *dirfd_out = -1;
    if (!dir || !*dir)
        return -EINVAL;

    /* This descriptor, rather than a pathname checked with lstat/statfs and
     * reopened later, is the identity handed to donor_pool_create_at().
     * O_NOFOLLOW rejects a configured final-component symlink and subsequent
     * rename/path replacement cannot redirect validation or child creation. */
    dirfd = exitos_internal_open_call(
        dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    if (dirfd < 0)
        return -(errno ? errno : EIO);
    if (fstat(dirfd, &dir_st) != 0) {
        rc = -(errno ? errno : EIO);
        goto fail;
    }
    if (!S_ISDIR(dir_st.st_mode)) {
        rc = -ENOTDIR;
        goto fail;
    }
    if (dir_st.st_uid != geteuid() ||
        (dir_st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        rc = -EPERM;
        goto fail;
    }
    if (dir_st.st_dev != target_st->st_dev) {
        rc = -EXDEV;
        goto fail;
    }
    if (fstatfs(dirfd, &dir_fs) != 0) {
        rc = -(errno ? errno : EIO);
        goto fail;
    }
    if ((unsigned long)dir_fs.f_type != (unsigned long)EXT4_SUPER_MAGIC ||
        (unsigned long)target_fs->f_type !=
            (unsigned long)EXT4_SUPER_MAGIC) {
        rc = -EOPNOTSUPP;
        goto fail;
    }
    *dirfd_out = dirfd;
    return 0;

fail:
    {
        int saved_errno = errno;
        (void)exitos_internal_close_call(dirfd);
        errno = saved_errno;
    }
    return rc;
}

static int revalidate_target_fresh(int fd, const struct stat *original_st,
                                   const struct statfs *original_fs)
{
    struct stat current_st;
    struct statfs current_fs;
    int flags;

    /* Pool creation can be long enough for an uncooperative alias to touch the
     * inode.  Repeat every property immediately before MOVE_EXT/zero-fill;
     * otherwise setup could overwrite data that arrived after the first
     * freshness check. */
    if (fstat(fd, &current_st) != 0)
        return -errno;
    flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return -errno;
    if (fstatfs(fd, &current_fs) != 0)
        return -errno;
    if (current_st.st_dev != original_st->st_dev ||
        current_st.st_ino != original_st->st_ino)
        return -ESTALE;
    if (!S_ISREG(current_st.st_mode) ||
        (flags & O_ACCMODE) == O_RDONLY || current_st.st_size != 0 ||
        current_st.st_blocks != 0)
        return -EBUSY;
    if ((unsigned long)current_fs.f_type !=
            (unsigned long)EXT4_SUPER_MAGIC ||
        current_fs.f_type != original_fs->f_type ||
        current_fs.f_bsize != original_fs->f_bsize)
        return -ESTALE;
    return 0;
}

/* fio's native-fallocate setup descriptor is O_WRONLY.  ext4's
 * EXT4_IOC_MOVE_EXT, however, requires FMODE_READ | FMODE_WRITE on the
 * original (target) file.  dup() cannot add read permission, so reopen the
 * exact proc-fd identity for the duration of preparation and prove that it is
 * still the same regular inode before handing it to donor.c.
 *
 * O_DIRECT is intentional.  The zero-fill performed by donor preparation
 * must not leave clean buffered pages that could later hide raw-device writes
 * from a buffered reader.  The application descriptor itself is untouched. */
static int acquire_prepare_fd(int fd, const struct stat *expected_st,
                              const struct statfs *expected_fs,
                              int *prepare_fd_out, int *owned_out)
{
    char path[64];
    struct stat alias_st;
    struct statfs alias_fs;
    int alias_fd = -1;
    int flags;
    int n;
    int rc = 0;

    if (!expected_st || !expected_fs || !prepare_fd_out || !owned_out)
        return -EINVAL;
    *prepare_fd_out = -1;
    *owned_out = 0;

    flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return -(errno ? errno : EIO);
    if ((flags & O_ACCMODE) == O_RDWR) {
        *prepare_fd_out = fd;
        return 0;
    }
    if ((flags & O_ACCMODE) != O_WRONLY)
        return -EBADF;

    n = snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    if (n < 0 || (size_t)n >= sizeof path)
        return -EOVERFLOW;
    alias_fd = exitos_internal_open_call(
        path, O_RDWR | O_DIRECT | O_CLOEXEC, 0);
    if (alias_fd < 0)
        return -(errno ? errno : EIO);
    if (fstat(alias_fd, &alias_st) != 0) {
        rc = -(errno ? errno : EIO);
        goto fail;
    }
    flags = fcntl(alias_fd, F_GETFL);
    if (flags < 0) {
        rc = -(errno ? errno : EIO);
        goto fail;
    }
    if (fstatfs(alias_fd, &alias_fs) != 0) {
        rc = -(errno ? errno : EIO);
        goto fail;
    }
    if ((flags & O_ACCMODE) != O_RDWR || !(flags & O_DIRECT)) {
        rc = -EBADF;
        goto fail;
    }
    if (!S_ISREG(alias_st.st_mode) ||
        alias_st.st_dev != expected_st->st_dev ||
        alias_st.st_ino != expected_st->st_ino) {
        rc = -ESTALE;
        goto fail;
    }
    if (alias_st.st_size != 0 || alias_st.st_blocks != 0) {
        rc = -EBUSY;
        goto fail;
    }
    if ((unsigned long)alias_fs.f_type !=
            (unsigned long)EXT4_SUPER_MAGIC ||
        alias_fs.f_type != expected_fs->f_type ||
        alias_fs.f_bsize != expected_fs->f_bsize) {
        rc = -ESTALE;
        goto fail;
    }

    *prepare_fd_out = alias_fd;
    *owned_out = 1;
    return 0;

fail:
    {
        int saved_errno = errno;
        (void)exitos_internal_close_call(alias_fd);
        errno = saved_errno;
    }
    return rc;
}

static int ensure_pool(struct exitos_frontend_fallocate_runtime *runtime,
                       int donor_dirfd, uint64_t bytes)
{
    unsigned files =
        exitos_frontend_config_donor_files(runtime->config);
    int lock_rc;
    int rc = 0;

    lock_rc = pthread_mutex_lock(&runtime->pool_mu);
    if (lock_rc != 0)
        return -lock_rc;
    if (runtime->bytes_each != 0 && runtime->bytes_each != bytes) {
        rc = -EINVAL;
        goto out;
    }
    if (runtime->bytes_each == 0)
        runtime->bytes_each = bytes; /* first eligible size is immutable */
    if (!runtime->pool) {
        errno = 0;
        runtime->pool = donor_pool_create_at(
            donor_dirfd, (int)files, runtime->bytes_each);
        if (!runtime->pool)
            rc = -(errno ? errno : EIO);
    }
out:
    (void)pthread_mutex_unlock(&runtime->pool_mu);
    return rc;
}

int exitos_frontend_fallocate_runtime_create(
    struct exitos_frontend_fallocate_runtime **out,
    const struct exitos_frontend_config *config, struct exitos_ctx *ctx)
{
    struct exitos_frontend_fallocate_runtime *runtime;
    unsigned files;
    int rc;

    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!config || !ctx)
        return -EINVAL;
    files = exitos_frontend_config_donor_files(config);
    if (exitos_frontend_config_donor_fallocate(config) &&
        (files == 0 || files > (unsigned)INT_MAX))
        return -EINVAL;
    runtime = calloc(1, sizeof *runtime);
    if (!runtime)
        return -ENOMEM;
    rc = pthread_mutex_init(&runtime->pool_mu, NULL);
    if (rc != 0) {
        free(runtime);
        return -rc;
    }
    runtime->config = config;
    runtime->ctx = ctx;
    *out = runtime;
    return 0;
}

void exitos_frontend_fallocate_runtime_destroy(
    struct exitos_frontend_fallocate_runtime *runtime)
{
    if (!runtime)
        return;
    donor_pool_destroy(runtime->pool);
    (void)pthread_mutex_destroy(&runtime->pool_mu);
    free(runtime);
}

int exitos_frontend_fallocate_execute(
    struct exitos_frontend_fallocate_runtime *runtime, int fd, int mode,
    off_t off, off_t len, exitos_frontend_native_fallocate_fn native_call,
    void *native_opaque, struct exitos_frontend_fallocate_result *result)
{
    struct donor_prepare_report report;
    struct exitos_fd_txn tx;
    struct stat target_st;
    struct statfs target_fs;
    enum exitos_frontend_fallocate_outcome outcome =
        EXITOS_FALLOCATE_CONTROL_FAILED;
    enum exitos_frontend_fallocate_stage stage = EXITOS_FALLOCATE_STAGE_ENTRY;
    uint64_t prepared_bytes = 0;
    uint32_t prepared_chunks = 0;
    int candidate;
    int diagnostic_rc = 0;
    int donor_dirfd = -1;
    int prepare_fd = -1;
    int prepare_fd_owned = 0;
    int used_rdwr_alias = 0;
    int transaction_begun = 0;
    int rc;

    if (result)
        memset(result, 0, sizeof *result);
    if (!runtime || !native_call) {
        rc = -EINVAL;
        diagnostic_rc = rc;
        goto done;
    }
    stage = EXITOS_FALLOCATE_STAGE_TXN_BEGIN;
    rc = exitos_fd_txn_begin(runtime->ctx, fd, &tx);
    if (rc != 0) {
        /* Preserve the historical native fallback ABI, but do not erase why
         * the configured control path could not inspect the call. */
        diagnostic_rc = rc;
        rc = native_call(native_opaque, fd, mode, off, len);
        goto done;
    }
    transaction_begun = 1;
    stage = EXITOS_FALLOCATE_STAGE_FLUSH;
    rc = exitos_txn_flush_raw_debt(&tx);
    if (rc != 0) {
        diagnostic_rc = rc;
        goto out;
    }

    stage = EXITOS_FALLOCATE_STAGE_CANDIDATE;
    candidate = target_candidate(runtime, &tx, fd, mode, off, len,
                                 &target_st, &target_fs);
    /* Every fallocate mode can change extent ownership.  Invalidation stays
     * before both the ordinary syscall and the donor transaction. */
    exitos_txn_invalidate_mapping(&tx);
    if (candidate < 0) {
        outcome = EXITOS_FALLOCATE_ELIGIBLE_FAILED;
        rc = candidate;
        diagnostic_rc = rc;
        goto out;
    }
    if (candidate == 0) {
        outcome = EXITOS_FALLOCATE_NATIVE_SKIP;
        stage = EXITOS_FALLOCATE_STAGE_NATIVE;
        rc = native_call(native_opaque, fd, mode, off, len);
        diagnostic_rc = rc;
        if (rc == 0)
            exitos_txn_note_kernel_metadata(&tx);
        goto out;
    }

    outcome = EXITOS_FALLOCATE_ELIGIBLE_FAILED;
    stage = EXITOS_FALLOCATE_STAGE_DONOR_DIR;
    rc = validate_donor_directory(runtime, &target_st, &target_fs,
                                  &donor_dirfd);
    if (rc != 0) {
        diagnostic_rc = rc;
        goto out;
    }
    stage = EXITOS_FALLOCATE_STAGE_DONOR_POOL;
    rc = ensure_pool(runtime, donor_dirfd, (uint64_t)len);
    {
        int saved_errno = errno;
        (void)exitos_internal_close_call(donor_dirfd);
        donor_dirfd = -1;
        errno = saved_errno;
    }
    if (rc != 0) {
        diagnostic_rc = rc;
        goto out;
    }
    stage = EXITOS_FALLOCATE_STAGE_RDWR_ALIAS;
    rc = acquire_prepare_fd(fd, &target_st, &target_fs, &prepare_fd,
                            &prepare_fd_owned);
    if (rc != 0) {
        diagnostic_rc = rc;
        goto out;
    }
    used_rdwr_alias = prepare_fd_owned;
    stage = EXITOS_FALLOCATE_STAGE_REVALIDATE;
    rc = revalidate_target_fresh(fd, &target_st, &target_fs);
    if (rc != 0) {
        diagnostic_rc = rc;
        goto out;
    }

    memset(&report, 0, sizeof report);
    stage = EXITOS_FALLOCATE_STAGE_DONOR_PREPARE;
    rc = donor_prepare_exact(runtime->pool, prepare_fd, (uint64_t)off,
                             (uint64_t)len, &report);
    if ((report.flags & DONOR_PREPARE_MUTATED) != 0)
        exitos_txn_note_kernel_metadata(&tx);
    if (rc != 0) {
        diagnostic_rc = rc;
        goto out;
    }
    stage = EXITOS_FALLOCATE_STAGE_DONOR_PROOF;
    if (report.requested_bytes != (uint64_t)len ||
        report.prepared_bytes != (uint64_t)len ||
        (report.flags & (DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED |
                         DONOR_PREPARE_VERIFIED)) !=
            (DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED |
             DONOR_PREPARE_VERIFIED)) {
        rc = -EIO;
        diagnostic_rc = rc;
        goto out;
    }
    exitos_txn_note_kernel_sync(&tx);
    stage = EXITOS_FALLOCATE_STAGE_MAP_REFRESH;
    rc = exitos_txn_refresh_mapping_range(&tx, off, len);
    if (rc != 0) {
        /* refresh may replace the old map before discovering that the
         * requested range is not fully covered.  A caller may ignore the
         * fallocate error, so leave no partial map that could later take over
         * only a prefix of its writes. */
        exitos_txn_invalidate_mapping(&tx);
        diagnostic_rc = rc;
        goto out;
    }
    outcome = EXITOS_FALLOCATE_PREPARED;
    stage = EXITOS_FALLOCATE_STAGE_COMPLETE;
    prepared_bytes = report.prepared_bytes;
    prepared_chunks = report.chunks;
out:
    if (prepare_fd_owned && prepare_fd >= 0) {
        int close_rc;
        int saved_errno = errno;

        close_rc = exitos_internal_close_call(prepare_fd);
        prepare_fd = -1;
        prepare_fd_owned = 0;
        if (close_rc != 0 && rc == 0) {
            rc = -(errno ? errno : EIO);
            diagnostic_rc = rc;
            outcome = EXITOS_FALLOCATE_ELIGIBLE_FAILED;
            stage = EXITOS_FALLOCATE_STAGE_RDWR_ALIAS_CLOSE;
            /* Mapping publication happened before the owned alias could be
             * released.  A caller may ignore this fallocate error, so remove
             * the fast-path eligibility while the fd transaction is held. */
            exitos_txn_invalidate_mapping(&tx);
        }
        errno = saved_errno;
    }
    if (donor_dirfd >= 0) {
        int saved_errno = errno;
        (void)exitos_internal_close_call(donor_dirfd);
        errno = saved_errno;
    }
    if (transaction_begun)
        exitos_fd_txn_end(&tx);
done:
    if (diagnostic_rc == 0 && rc < 0)
        diagnostic_rc = rc;
    if (result) {
        result->outcome = outcome;
        result->stage = stage;
        result->rc = diagnostic_rc;
        result->used_rdwr_alias = used_rdwr_alias != 0;
        if (outcome == EXITOS_FALLOCATE_PREPARED && rc == 0) {
            result->prepared = 1;
            result->prepared_bytes = prepared_bytes;
            result->chunks = prepared_chunks;
        }
    }
    return rc;
}
