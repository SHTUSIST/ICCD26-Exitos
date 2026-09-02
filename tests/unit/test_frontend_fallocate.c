#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tap.h"
#include "exitos_donor.h"
#include "exitos_frontend_config.h"
#include "exitos_frontend_fallocate.h"
#include "exitos_intercept.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef EXT4_SUPER_MAGIC
#define EXT4_SUPER_MAGIC 0xEF53
#endif

static int g_enabled;
static int g_registered;
static const char *g_dir = "/safe/donors";
static unsigned g_files = 2;
static int g_txn_begin_rc;
static int g_flush_rc;
static int g_native_rc;
static int g_native_calls;
static int g_native_mode;
static off_t g_native_off;
static off_t g_native_len;
static int g_pool_create_calls;
static int g_pool_create_at_calls;
static int g_legacy_pool_create_calls;
static int g_pool_create_errno;
static int g_pool_create_fail;
static int g_mutate_target_on_pool_create;
static uint64_t g_pool_bytes;
static int g_exact_calls;
static int g_exact_rc;
static struct donor_prepare_report g_exact_report;
static int g_metadata_notes;
static int g_sync_notes;
static int g_refresh_calls;
static int g_refresh_rc;
static int g_invalidate_calls;
static int g_end_calls;
static char g_order[64];
static size_t g_order_len;
static struct stat g_target;
static int g_target_fstat_errno;
static struct stat g_dir_stat;
static struct statfs g_target_fs;
static struct statfs g_dir_fs;
static int g_fd_flags;
static int g_lstat_rc;
static int g_lstat_calls;
static int g_statfs_calls;
static int g_dir_open_calls;
static int g_dir_close_calls;
static int g_dir_open_errno;
static int g_dirfd = 77;
static int g_alias_open_calls;
static int g_alias_close_calls;
static int g_alias_open_errno;
static int g_alias_close_errno;
static int g_alias_identity_mismatch;
static int g_alias_fd_flags;
static int g_alias_fd = 78;
static int g_expected_prepare_fd = 41;
static uid_t g_euid = 1000;
static struct donor_pool *const g_fake_pool =
    (struct donor_pool *)(uintptr_t)0x1234;

static void order(char c)
{
    if (g_order_len + 1 < sizeof g_order) {
        g_order[g_order_len++] = c;
        g_order[g_order_len] = '\0';
    }
}

static int ff_config_donor(const struct exitos_frontend_config *config)
{
    (void)config;
    return g_enabled;
}

static const char *ff_config_dir(const struct exitos_frontend_config *config)
{
    (void)config;
    return g_dir;
}

static unsigned ff_config_files(const struct exitos_frontend_config *config)
{
    (void)config;
    return g_files;
}

static int ff_txn_begin(struct exitos_ctx *ctx, int fd,
                        struct exitos_fd_txn *tx)
{
    order('B');
    memset(tx, 0, sizeof *tx);
    tx->ctx = ctx;
    tx->fd = fd;
    tx->held = g_txn_begin_rc == 0;
    tx->registered = g_registered;
    return g_txn_begin_rc;
}

static int ff_txn_registered(const struct exitos_fd_txn *tx)
{
    return tx && tx->registered;
}

static int ff_txn_flush(struct exitos_fd_txn *tx)
{
    (void)tx;
    order('F');
    return g_flush_rc;
}

static void ff_txn_invalidate(struct exitos_fd_txn *tx)
{
    (void)tx;
    order('I');
    g_invalidate_calls++;
}

static void ff_txn_note_metadata(struct exitos_fd_txn *tx)
{
    (void)tx;
    order('M');
    g_metadata_notes++;
}

static void ff_txn_note_sync(struct exitos_fd_txn *tx)
{
    (void)tx;
    order('S');
    g_sync_notes++;
}

static int ff_txn_refresh(struct exitos_fd_txn *tx, off_t off, off_t len)
{
    (void)tx;
    T_EQ(off, 0, "refresh starts at the intercepted fallocate offset");
    T_EQ(len, (off_t)g_pool_bytes,
         "refresh covers the complete prepared range");
    order('R');
    g_refresh_calls++;
    return g_refresh_rc;
}

static void ff_txn_end(struct exitos_fd_txn *tx)
{
    order('E');
    g_end_calls++;
    tx->held = 0;
}

static __attribute__((unused)) struct donor_pool *
ff_pool_create(const char *dir, int nfiles, uint64_t bytes)
{
    order('P');
    g_pool_create_calls++;
    g_legacy_pool_create_calls++;
    T_OK(strcmp(dir, g_dir) == 0, "pool uses immutable configured directory");
    T_EQ(nfiles, (int)g_files, "pool uses immutable configured width");
    g_pool_bytes = bytes;
    if (g_pool_create_fail) {
        errno = g_pool_create_errno;
        return NULL;
    }
    if (g_mutate_target_on_pool_create)
        g_target.st_size = 4096;
    return g_fake_pool;
}

static struct donor_pool *ff_pool_create_at(int dirfd, int nfiles,
                                            uint64_t bytes)
{
    order('P');
    g_pool_create_calls++;
    g_pool_create_at_calls++;
    T_EQ(dirfd, g_dirfd,
         "pool receives the same descriptor that directory validation used");
    T_EQ(nfiles, (int)g_files, "pool uses immutable configured width");
    g_pool_bytes = bytes;
    if (g_pool_create_fail) {
        errno = g_pool_create_errno;
        return NULL;
    }
    if (g_mutate_target_on_pool_create)
        g_target.st_size = 4096;
    return g_fake_pool;
}

static void ff_pool_destroy(struct donor_pool *pool)
{
    if (pool)
        T_OK(pool == g_fake_pool, "runtime destroys the pool it created");
}

static int ff_prepare_exact(struct donor_pool *pool, int fd,
                            uint64_t off, uint64_t len,
                            struct donor_prepare_report *report)
{
    order('D');
    g_exact_calls++;
    T_OK(pool == g_fake_pool, "exact preparation uses the frozen pool");
    T_EQ(fd, g_expected_prepare_fd,
         "exact preparation uses the required read-write inode descriptor");
    T_EQ(off, 0, "exact preparation preserves zero offset");
    T_EQ(len, g_pool_bytes, "exact preparation covers frozen length");
    *report = g_exact_report;
    return g_exact_rc;
}

static int ff_fstat(int fd, struct stat *st)
{
    if (fd == 41 && g_target_fstat_errno) {
        errno = g_target_fstat_errno;
        return -1;
    }
    *st = fd == g_dirfd ? g_dir_stat : g_target;
    if (fd == g_alias_fd && g_alias_identity_mismatch)
        st->st_ino++;
    return 0;
}

static __attribute__((unused)) int ff_lstat(const char *path, struct stat *st)
{
    g_lstat_calls++;
    T_OK(strcmp(path, g_dir) == 0, "directory validation uses configured path");
    if (g_lstat_rc) {
        errno = -g_lstat_rc;
        return -1;
    }
    *st = g_dir_stat;
    return 0;
}

static int ff_fstatfs(int fd, struct statfs *st)
{
    *st = fd == g_dirfd ? g_dir_fs : g_target_fs;
    return 0;
}

static __attribute__((unused)) int ff_statfs(const char *path,
                                             struct statfs *st)
{
    g_statfs_calls++;
    T_OK(strcmp(path, g_dir) == 0,
         "filesystem validation uses configured directory");
    *st = g_dir_fs;
    return 0;
}

static int ff_internal_open(const char *path, int flags, mode_t mode)
{
    const int required = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    char alias_path[64];
    (void)mode;
    snprintf(alias_path, sizeof alias_path, "/proc/self/fd/%d", 41);
    if (strcmp(path, g_dir) == 0) {
        g_dir_open_calls++;
        T_OK((flags & required) == required && !(flags & (O_CREAT | O_TRUNC)),
             "directory open is O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC");
        if (g_dir_open_errno) {
            errno = g_dir_open_errno;
            return -1;
        }
        return g_dirfd;
    }
    g_alias_open_calls++;
    T_OK(strcmp(path, alias_path) == 0,
         "write-only target is reopened through its exact proc-fd identity");
    T_OK((flags & O_ACCMODE) == O_RDWR && (flags & O_CLOEXEC) &&
             (flags & O_DIRECT) && !(flags & (O_CREAT | O_TRUNC)),
         "target alias is O_RDWR|O_DIRECT|O_CLOEXEC without create or truncate");
    if (g_alias_open_errno) {
        errno = g_alias_open_errno;
        return -1;
    }
    return g_alias_fd;
}

static int ff_internal_close(int fd)
{
    T_OK(fd == g_dirfd || fd == g_alias_fd,
         "frontend closes only a descriptor it opened internally");
    if (fd == g_dirfd)
        g_dir_close_calls++;
    else {
        g_alias_close_calls++;
        if (g_alias_close_errno) {
            errno = g_alias_close_errno;
            return -1;
        }
    }
    return 0;
}

static int ff_fcntl(int fd, int cmd, ...)
{
    T_EQ(cmd, F_GETFL, "eligibility checks descriptor access mode");
    return fd == g_alias_fd ? g_alias_fd_flags : g_fd_flags;
}

static uid_t ff_geteuid(void)
{
    return g_euid;
}

static int ff_native(void *opaque, int fd, int mode, off_t off, off_t len)
{
    int *seen_fd = opaque;
    order('N');
    g_native_calls++;
    *seen_fd = fd;
    g_native_mode = mode;
    g_native_off = off;
    g_native_len = len;
    return g_native_rc;
}

#define exitos_frontend_config_donor_fallocate ff_config_donor
#define exitos_frontend_config_donor_dir       ff_config_dir
#define exitos_frontend_config_donor_files     ff_config_files
#define exitos_fd_txn_begin                    ff_txn_begin
#define exitos_fd_txn_registered               ff_txn_registered
#define exitos_txn_flush_raw_debt              ff_txn_flush
#define exitos_txn_invalidate_mapping          ff_txn_invalidate
#define exitos_txn_note_kernel_metadata        ff_txn_note_metadata
#define exitos_txn_note_kernel_sync            ff_txn_note_sync
#define exitos_txn_refresh_mapping_range       ff_txn_refresh
#define exitos_fd_txn_end                      ff_txn_end
#define donor_pool_create                      ff_pool_create
#define donor_pool_create_at                   ff_pool_create_at
#define donor_pool_destroy                     ff_pool_destroy
#define donor_prepare_exact                    ff_prepare_exact
#define exitos_internal_open_call              ff_internal_open
#define exitos_internal_close_call             ff_internal_close
#define fstat                                  ff_fstat
#define lstat                                  ff_lstat
#define fstatfs                                ff_fstatfs
#define statfs(...)                            ff_statfs(__VA_ARGS__)
#define fcntl                                  ff_fcntl
#define geteuid                                ff_geteuid
#define exitos_frontend_fallocate_runtime_create ff_runtime_create
#define exitos_frontend_fallocate_runtime_destroy ff_runtime_destroy
#define exitos_frontend_fallocate_execute      ff_execute
#define exitos_frontend_fallocate_outcome_name ff_outcome_name
#define exitos_frontend_fallocate_stage_name   ff_stage_name
#include "../../src/frontend_fallocate.c"
#undef exitos_frontend_config_donor_fallocate
#undef exitos_frontend_config_donor_dir
#undef exitos_frontend_config_donor_files
#undef exitos_fd_txn_begin
#undef exitos_fd_txn_registered
#undef exitos_txn_flush_raw_debt
#undef exitos_txn_invalidate_mapping
#undef exitos_txn_note_kernel_metadata
#undef exitos_txn_note_kernel_sync
#undef exitos_txn_refresh_mapping_range
#undef exitos_fd_txn_end
#undef donor_pool_create
#undef donor_pool_create_at
#undef donor_pool_destroy
#undef donor_prepare_exact
#undef exitos_internal_open_call
#undef exitos_internal_close_call
#undef fstat
#undef lstat
#undef fstatfs
#undef statfs
#undef fcntl
#undef geteuid
#undef exitos_frontend_fallocate_runtime_create
#undef exitos_frontend_fallocate_runtime_destroy
#undef exitos_frontend_fallocate_execute
#undef exitos_frontend_fallocate_outcome_name
#undef exitos_frontend_fallocate_stage_name

static void reset(void)
{
    memset(&g_exact_report, 0, sizeof g_exact_report);
    memset(&g_target, 0, sizeof g_target);
    memset(&g_dir_stat, 0, sizeof g_dir_stat);
    memset(&g_target_fs, 0, sizeof g_target_fs);
    memset(&g_dir_fs, 0, sizeof g_dir_fs);
    g_enabled = 0;
    g_registered = 1;
    g_files = 2;
    g_txn_begin_rc = 0;
    g_flush_rc = 0;
    g_native_rc = 0;
    g_native_calls = 0;
    g_native_mode = 0;
    g_native_off = 0;
    g_native_len = 0;
    g_pool_create_calls = 0;
    g_pool_create_at_calls = 0;
    g_legacy_pool_create_calls = 0;
    g_pool_create_errno = 0;
    g_pool_create_fail = 0;
    g_mutate_target_on_pool_create = 0;
    g_pool_bytes = 0;
    g_exact_calls = 0;
    g_exact_rc = 0;
    g_metadata_notes = 0;
    g_sync_notes = 0;
    g_refresh_calls = 0;
    g_refresh_rc = 0;
    g_invalidate_calls = 0;
    g_end_calls = 0;
    g_order_len = 0;
    g_order[0] = '\0';
    g_target.st_mode = S_IFREG | 0600;
    g_target_fstat_errno = 0;
    g_target.st_uid = g_euid;
    g_target.st_dev = 7;
    g_target.st_ino = 99;
    g_target.st_size = 0;
    g_target.st_blocks = 0;
    g_target.st_blksize = 4096;
    g_dir_stat.st_mode = S_IFDIR | 0700;
    g_dir_stat.st_uid = g_euid;
    g_dir_stat.st_dev = g_target.st_dev;
    g_target_fs.f_type = EXT4_SUPER_MAGIC;
    g_target_fs.f_bsize = 4096;
    g_dir_fs = g_target_fs;
    g_fd_flags = O_RDWR;
    g_lstat_rc = 0;
    g_lstat_calls = 0;
    g_statfs_calls = 0;
    g_dir_open_calls = 0;
    g_dir_close_calls = 0;
    g_dir_open_errno = 0;
    g_alias_open_calls = 0;
    g_alias_close_calls = 0;
    g_alias_open_errno = 0;
    g_alias_close_errno = 0;
    g_alias_identity_mismatch = 0;
    g_alias_fd_flags = O_RDWR | O_DIRECT;
    g_expected_prepare_fd = 41;
}

static struct exitos_frontend_fallocate_runtime *new_runtime(void)
{
    struct exitos_frontend_fallocate_runtime *runtime = NULL;
    int rc = ff_runtime_create(
        &runtime, (const struct exitos_frontend_config *)(uintptr_t)1,
        (struct exitos_ctx *)(uintptr_t)2);
    T_EQ(rc, 0, "runtime construction succeeds without eager pool I/O");
    T_OK(runtime != NULL, "runtime construction returns an object");
    T_EQ(g_pool_create_calls, 0, "runtime construction is lazy");
    return runtime;
}

int main(void)
{
    struct exitos_frontend_fallocate_runtime *runtime =
        (struct exitos_frontend_fallocate_runtime *)(uintptr_t)0xdead;
    struct exitos_frontend_fallocate_result result;
    int seen_fd = -1;
    int rc;

    rc = ff_runtime_create(&runtime, NULL,
                           (struct exitos_ctx *)(uintptr_t)2);
    T_EQ(rc, -EINVAL, "runtime rejects a NULL immutable config");
    T_OK(runtime == NULL,
         "runtime creation clears caller output before argument rejection");

    reset();
    runtime = new_runtime();
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, 0, "disabled donor policy preserves native success");
    T_EQ(seen_fd, 41, "native callback receives original fd");
    T_EQ(g_native_mode, 0, "native callback preserves mode");
    T_EQ(g_native_off, 0, "native callback preserves offset");
    T_EQ(g_native_len, 8192, "native callback preserves length");
    T_OK(strcmp(g_order, "BFINME") == 0,
         "PASS order is begin, flush, invalidate, native, metadata, end");
    T_EQ(result.prepared, 0, "ordinary fallocate is not reported prepared");
    T_EQ(result.outcome, EXITOS_FALLOCATE_NATIVE_SKIP,
         "ordinary fallocate has stable native-skip outcome");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_NATIVE,
         "ordinary fallocate reports the native stage");
    T_EQ(result.rc, 0, "successful native-skip reports zero diagnostic rc");
    T_EQ(result.used_rdwr_alias, 0,
         "ordinary fallocate reports no internal read-write alias");
    ff_runtime_destroy(runtime);

    reset();
    runtime = new_runtime();
    g_native_rc = -ENOSPC;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -ENOSPC, "native negative errno is preserved");
    T_OK(strcmp(g_order, "BFINE") == 0,
         "failed native call creates no metadata debt");
    ff_runtime_destroy(runtime);

    reset();
    runtime = new_runtime();
    g_flush_rc = -EIO;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -EIO, "raw-debt flush failure is returned");
    T_EQ(g_native_calls, 0, "flush failure prevents native mutation");
    T_EQ(g_invalidate_calls, 0, "flush failure keeps retryable mapping");
    T_OK(strcmp(g_order, "BFE") == 0,
         "flush failure ends the transaction without mutation");
    T_EQ(result.outcome, EXITOS_FALLOCATE_CONTROL_FAILED,
         "flush failure is distinguished as a control failure");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_FLUSH,
         "flush failure records its stable stage");
    T_EQ(result.rc, -EIO, "control failure records a negative errno");
    ff_runtime_destroy(runtime);

    reset();
    runtime = new_runtime();
    g_txn_begin_rc = -ENOMEM;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, 0, "transaction allocation failure preserves native behavior");
    T_EQ(g_native_calls, 1, "transaction failure calls native exactly once");
    T_OK(strcmp(g_order, "BN") == 0,
         "transaction failure does not pretend to hold lifecycle state");
    T_EQ(result.outcome, EXITOS_FALLOCATE_CONTROL_FAILED,
         "transaction admission failure remains visible despite native fallback");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_TXN_BEGIN,
         "transaction admission failure records its stable stage");
    T_EQ(result.rc, -ENOMEM,
         "transaction admission failure retains its negative control errno");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_mutate_target_on_pool_create = 1;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -EBUSY,
         "target that becomes nonfresh while pool is built fails closed");
    T_EQ(g_exact_calls, 0,
         "freshness is revalidated immediately before donor mutation");
    T_EQ(g_native_calls, 0,
         "lost setup exclusivity never falls back to native fallocate");
    T_EQ(g_invalidate_calls, 1,
         "lost setup exclusivity leaves the old mapping invalidated");
    ff_runtime_destroy(runtime);

    /* fio 3.41 opens its fresh setup descriptor O_WRONLY before issuing the
     * default native fallocate.  EXT4_IOC_MOVE_EXT requires FMODE_READ and
     * FMODE_WRITE on the original file, so the shared frontend must obtain a
     * verified O_RDWR alias without changing the application's descriptor. */
    reset();
    g_enabled = 1;
    g_fd_flags = O_WRONLY;
    g_expected_prepare_fd = g_alias_fd;
    runtime = new_runtime();
    g_exact_report.requested_bytes = 8192;
    g_exact_report.prepared_bytes = 8192;
    g_exact_report.chunks = 1;
    g_exact_report.flags = DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED |
                           DONOR_PREPARE_VERIFIED;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, 0, "fio-shaped O_WRONLY fallocate is donor-prepared");
    T_EQ(g_alias_open_calls, 1,
         "O_WRONLY setup opens one exact read-write alias");
    T_EQ(g_alias_close_calls, 1,
         "successful setup closes its internal read-write alias once");
    T_EQ(g_exact_calls, 1,
         "donor exchange receives the verified read-write alias");
    T_EQ(g_native_calls, 0,
         "fio-shaped setup never falls back to native fallocate");
    T_EQ(result.prepared, 1,
         "fio-shaped O_WRONLY setup publishes preparation success");
    T_EQ(result.outcome, EXITOS_FALLOCATE_PREPARED,
         "successful donor setup has the prepared outcome");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_COMPLETE,
         "successful donor setup reaches the complete stage");
    T_EQ(result.rc, 0, "successful donor setup records zero diagnostic rc");
    T_EQ(result.used_rdwr_alias, 1,
         "fio-shaped preparation reports its verified O_RDWR alias");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    g_fd_flags = O_WRONLY;
    g_expected_prepare_fd = g_alias_fd;
    g_alias_close_errno = EIO;
    runtime = new_runtime();
    g_exact_report.requested_bytes = 8192;
    g_exact_report.prepared_bytes = 8192;
    g_exact_report.chunks = 1;
    g_exact_report.flags = DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED |
                           DONOR_PREPARE_VERIFIED;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -EIO, "read-write alias close failure is fail closed");
    T_EQ(result.prepared, 0,
         "alias close failure never leaves a PREPARE_READY claim");
    T_EQ(result.outcome, EXITOS_FALLOCATE_ELIGIBLE_FAILED,
         "alias close failure is an eligible-attempt failure");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_RDWR_ALIAS_CLOSE,
         "alias close failure records its stable stage");
    T_EQ(result.rc, -EIO,
         "alias close failure records its negative errno");
    T_EQ(result.used_rdwr_alias, 1,
         "alias close failure still records that preparation used the alias");
    T_EQ(g_invalidate_calls, 2,
         "alias close failure withdraws the mapping published before close");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    g_fd_flags = O_WRONLY;
    g_alias_open_errno = EACCES;
    runtime = new_runtime();
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -EACCES,
         "failure to obtain required O_RDWR alias is fail closed");
    T_EQ(g_exact_calls, 0,
         "alias-open failure cannot enter MOVE_EXT preparation");
    T_EQ(g_alias_close_calls, 0,
         "failed alias open owns no descriptor to close");
    T_EQ(g_native_calls, 0,
         "alias-open failure cannot silently restore native UNWRITTEN setup");
    T_EQ(result.outcome, EXITOS_FALLOCATE_ELIGIBLE_FAILED,
         "alias-open failure is distinguished from a native skip");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_RDWR_ALIAS,
         "alias-open failure records its stable stage");
    T_EQ(result.rc, -EACCES,
         "eligible alias-open failure records a negative errno");
    T_EQ(result.used_rdwr_alias, 0,
         "failed alias acquisition is not reported as an alias in use");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    g_fd_flags = O_WRONLY;
    g_alias_identity_mismatch = 1;
    runtime = new_runtime();
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -ESTALE,
         "proc-fd alias with a different dev/inode identity is rejected");
    T_EQ(g_exact_calls, 0,
         "identity mismatch is rejected before donor mutation");
    T_EQ(g_alias_close_calls, 1,
         "rejected identity alias is still closed exactly once");
    T_EQ(g_native_calls, 0,
         "identity mismatch remains fail closed");
    T_EQ(result.outcome, EXITOS_FALLOCATE_ELIGIBLE_FAILED,
         "unverified alias is an eligible preparation failure");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_RDWR_ALIAS,
         "identity mismatch remains attributable to alias acquisition");
    T_EQ(result.rc, -ESTALE,
         "identity mismatch preserves its negative errno");
    T_EQ(result.used_rdwr_alias, 0,
         "an identity-mismatched descriptor is never reported in use");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_exact_report.requested_bytes = 8192;
    g_exact_report.prepared_bytes = 8192;
    g_exact_report.chunks = 2;
    g_exact_report.flags = DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED |
                           DONOR_PREPARE_VERIFIED;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, 0, "eligible native fallocate is replaced by exact donor setup");
    T_EQ(g_native_calls, 0, "successful donor setup does not call native fallocate");
    T_EQ(g_pool_create_calls, 1, "first eligible call lazily creates one pool");
    T_EQ(g_pool_create_at_calls, 1,
         "pool is constructed from the validated directory descriptor");
    T_EQ(g_legacy_pool_create_calls, 0,
         "eligible setup never reopens the validated directory pathname");
    T_EQ(g_dir_open_calls, 1, "eligible setup opens directory once");
    T_EQ(g_dir_close_calls, 1,
         "frontend releases its borrowed dirfd after pool duplicates it");
    T_EQ(g_lstat_calls, 0,
         "directory validation does not perform path-based lstat");
    T_EQ(g_statfs_calls, 0,
         "directory filesystem validation is descriptor-bound");
    T_EQ(g_exact_calls, 1, "eligible call invokes exact donor preparation once");
    T_EQ(result.prepared, 1, "result identifies donor preparation");
    T_EQ(result.prepared_bytes, 8192, "result exposes exact prepared bytes");
    T_EQ(result.chunks, 2, "result exposes donor chunk count");
    T_EQ(result.outcome, EXITOS_FALLOCATE_PREPARED,
         "read-write setup reports the same prepared outcome");
    T_EQ(result.used_rdwr_alias, 0,
         "an application O_RDWR descriptor needs no internal alias");
    T_OK(strcmp(g_order, "BFIPDMSRE") == 0,
         "donor success records metadata, sync, then refreshes mapping");

    g_order_len = 0;
    g_order[0] = '\0';
    g_exact_calls = 0;
    g_metadata_notes = g_sync_notes = g_refresh_calls = 0;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, 0, "same static length reuses frozen pool");
    T_EQ(g_pool_create_calls, 1, "same length never recreates pool");
    T_EQ(g_exact_calls, 1, "same length prepares another target");

    g_order_len = 0;
    g_order[0] = '\0';
    g_exact_calls = 0;
    rc = ff_execute(runtime, 41, 0, 0, 16384, ff_native, &seen_fd, &result);
    T_EQ(rc, -EINVAL, "different later length fails instead of adapting pool");
    T_EQ(g_exact_calls, 0, "size mismatch does not consume donor space");
    T_EQ(g_native_calls, 0, "eligible size mismatch fails closed");
    T_OK(strcmp(g_order, "BFIE") == 0,
         "size mismatch leaves mapping conservatively invalidated");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_exact_report.requested_bytes = 8192;
    g_exact_report.prepared_bytes = 8192;
    g_exact_report.chunks = 1;
    g_exact_report.flags = DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED |
                           DONOR_PREPARE_VERIFIED;
    g_refresh_rc = -ENXIO;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -ENXIO, "post-prepare mapping refresh failure is returned");
    T_EQ(g_invalidate_calls, 2,
         "refresh failure clears any partially installed mapping again");
    T_EQ(result.prepared, 0,
         "refresh failure is never published as preparation-ready");
    T_OK(strcmp(g_order, "BFIPDMSRIE") == 0,
         "refresh failure invalidates after the failed refresh before unlock");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_exact_rc = -EIO;
    g_exact_report.requested_bytes = 8192;
    g_exact_report.prepared_bytes = 4096;
    g_exact_report.chunks = 1;
    g_exact_report.flags = DONOR_PREPARE_MUTATED;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -EIO, "partial donor failure is returned without native fallback");
    T_EQ(g_metadata_notes, 1, "partial irreversible mutation creates metadata debt");
    T_EQ(g_sync_notes, 0, "failed donor preparation is not marked synced");
    T_EQ(g_refresh_calls, 0, "failed donor preparation is not mapped");
    T_EQ(g_native_calls, 0, "partial mutation never falls back to native");
    T_EQ(result.outcome, EXITOS_FALLOCATE_ELIGIBLE_FAILED,
         "partial donor error is an eligible-attempt failure");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_DONOR_PREPARE,
         "partial donor error identifies the donor-prepare stage");
    T_EQ(result.rc, -EIO,
         "partial donor error records its negative errno");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_exact_report.requested_bytes = 8192;
    g_exact_report.prepared_bytes = 8192;
    g_exact_report.flags = DONOR_PREPARE_MUTATED | DONOR_PREPARE_SYNCED;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -EIO, "success without VERIFIED proof fails closed");
    T_EQ(g_metadata_notes, 1, "unverified mutation remains accounted");
    T_EQ(g_sync_notes, 0, "incomplete proof is not published as synced");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_pool_create_fail = 1;
    g_pool_create_errno = ENOSPC;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -ENOSPC, "pool creation reports its concrete errno");
    T_EQ(g_pool_create_at_calls, 1,
         "ENOSPC came from descriptor-bound pool construction");
    T_EQ(g_dir_close_calls, 1,
         "pool creation failure still closes frontend validation dirfd");
    T_EQ(g_native_calls, 0, "pool creation failure is fail closed");
    T_EQ(result.outcome, EXITOS_FALLOCATE_ELIGIBLE_FAILED,
         "pool creation failure is distinguished from native skip");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_DONOR_POOL,
         "pool creation failure identifies the pool stage");
    T_EQ(result.rc, -ENOSPC,
         "pool creation failure records its negative errno");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_dir_open_errno = EACCES;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, NULL);
    T_EQ(rc, -EACCES, "directory open preserves its concrete errno");
    T_EQ(g_pool_create_calls, 0,
         "failed directory open cannot construct a pool");
    T_EQ(g_dir_close_calls, 0,
         "failed directory open has no descriptor to close");
    T_EQ(g_native_calls, 0,
         "eligible directory-open failure remains fail closed");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_dir_stat.st_mode = S_IFDIR | 0750;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, NULL);
    T_EQ(rc, -EPERM, "group-accessible donor directory is rejected");
    T_EQ(g_native_calls, 0, "unsafe directory is fail closed once eligible");
    g_dir_stat.st_mode = S_IFDIR | 0700;
    g_dir_stat.st_uid = g_euid + 1;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, NULL);
    T_EQ(rc, -EPERM, "donor directory must be owned by effective uid");
    g_dir_stat.st_uid = g_euid;
    g_dir_stat.st_dev++;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, NULL);
    T_EQ(rc, -EXDEV, "donor directory must share target device");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_registered = 0;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, NULL);
    T_EQ(rc, 0, "unregistered fd follows ordinary fallocate path");
    T_EQ(g_pool_create_calls, 0, "unregistered fd cannot create donor pool");
    g_registered = 1;
    g_target.st_size = 1;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, NULL);
    T_EQ(rc, 0, "nonempty target follows ordinary fallocate path");
    g_target.st_size = 0;
    g_target.st_blocks = 8;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, NULL);
    T_EQ(rc, 0, "already allocated target follows ordinary fallocate path");
    g_target.st_blocks = 0;
    g_fd_flags = O_RDONLY;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, NULL);
    T_EQ(rc, 0, "read-only target follows kernel error semantics");
    g_fd_flags = O_RDWR;
    rc = ff_execute(runtime, 41, 1, 0, 8192, ff_native, &seen_fd, NULL);
    T_EQ(rc, 0, "nonzero mode is never donor-taken-over");
    T_EQ(g_native_mode, 1, "nonzero mode reaches native callback unchanged");
    rc = ff_execute(runtime, 41, 0, 4096, 8192, ff_native, &seen_fd, NULL);
    T_EQ(rc, 0, "nonzero offset is never donor-taken-over");
    T_EQ(g_native_off, 4096, "nonzero offset reaches native callback unchanged");
    rc = ff_execute(runtime, 41, 0, 0, 4097, ff_native, &seen_fd, NULL);
    T_EQ(rc, 0, "unaligned length is never donor-taken-over");
    T_EQ(g_pool_create_calls, 0,
         "all structurally ineligible calls avoid donor creation");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_target_fstat_errno = EIO;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -EIO,
         "configured candidate fstat error is not disguised as native skip");
    T_EQ(g_native_calls, 0,
         "candidate inspection error remains fail closed");
    T_EQ(result.outcome, EXITOS_FALLOCATE_ELIGIBLE_FAILED,
         "candidate inspection error has eligible-attempt-failed outcome");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_CANDIDATE,
         "candidate inspection error retains candidate stage");
    T_EQ(result.rc, -EIO,
         "candidate inspection error retains its negative errno");
    ff_runtime_destroy(runtime);

    reset();
    g_enabled = 1;
    runtime = new_runtime();
    g_target_fs.f_type = 0x1234;
    rc = ff_execute(runtime, 41, 0, 0, 8192, ff_native, &seen_fd, &result);
    T_EQ(rc, -EOPNOTSUPP, "eligible target must reside on ext4");
    T_EQ(g_native_calls, 0, "wrong filesystem fails configured preparation closed");
    T_EQ(result.outcome, EXITOS_FALLOCATE_ELIGIBLE_FAILED,
         "unsupported candidate is a configured eligible failure");
    T_EQ(result.stage, EXITOS_FALLOCATE_STAGE_CANDIDATE,
         "unsupported filesystem identifies candidate validation stage");
    T_EQ(result.rc, -EOPNOTSUPP,
         "unsupported filesystem records its negative errno");
    ff_runtime_destroy(runtime);

    T_OK(strcmp(ff_outcome_name(EXITOS_FALLOCATE_NATIVE_SKIP),
                "native-skip") == 0,
         "native-skip outcome spelling is schema-stable");
    T_OK(strcmp(ff_outcome_name(EXITOS_FALLOCATE_ELIGIBLE_FAILED),
                "eligible-attempt-failed") == 0,
         "eligible failure outcome spelling is schema-stable");
    T_OK(strcmp(ff_outcome_name(EXITOS_FALLOCATE_PREPARED), "prepared") == 0,
         "prepared outcome spelling is schema-stable");
    T_OK(strcmp(ff_outcome_name(EXITOS_FALLOCATE_CONTROL_FAILED),
                "control-failed") == 0,
         "control failure outcome spelling is schema-stable");
    T_OK(strcmp(ff_stage_name(EXITOS_FALLOCATE_STAGE_RDWR_ALIAS),
                "rdwr-alias") == 0,
         "alias stage spelling is schema-stable");

    T_DONE();
}
