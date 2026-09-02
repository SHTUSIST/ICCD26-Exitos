/* Registration/sync lifecycle and concurrency regressions.
 *
 * This is deliberately a white-box, device-free test.  It includes the real
 * interceptor and LD_PRELOAD frontend under private names, replacing only
 * geometry discovery, extent discovery, the iopath, and libc syscalls.  The
 * blocking mocks make the race windows deterministic: no loop device and no
 * physical device is opened or written. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "tap.h"
#include "exitos_durability.h"
#include "exitos_extent.h"
#include "exitos_frontend_admission.h"
#include "exitos_frontend_fallocate.h"
#include "exitos_geom.h"
#include "exitos_intercept.h"
#include "exitos_iopath.h"
#include "exitos_maco.h"

enum { TEST_FD = 37, SOURCE_FD = 38, BS = 4096 };

static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_block_raw_write;
static int g_raw_entered;
static int g_release_raw;
static int g_block_kernel_pwrite;
static int g_kernel_write_entered;
static int g_release_kernel_write;
static int g_block_kernel_dup;
static int g_kernel_dup_entered;
static int g_release_kernel_dup;
static int g_block_frontend_admission;
static int g_frontend_admission_entered;
static int g_release_frontend_admission;
static int g_frontend_quiesce_entered;

static int g_flush_rc;
static int g_flush_calls;
static int g_flush_fail_call;
static _Atomic int g_close_calls;
static int g_open_calls;
static iopath_backend g_last_open_backend;
static char g_last_open_path[256];
static int g_open_fd_calls;
static int g_last_open_fd;
static iopath_backend g_last_open_fd_backend;
static uint32_t g_last_open_fd_lbs;
static int g_open_fd_fail;
static int g_fake_block_fstat;
static dev_t g_fake_block_rdev;
static int g_policy;
static int g_iopath_write_rc;
static int g_registration_helper_io;
static int g_registration_wrapper_io;
static int g_registration_mapped;

/* The interposed entry points, under their private names in this executable.
 * mock_geom_probe needs them before src/preload.c is included below. */
int lc_preload_open(const char *path, int flags, ...);
int lc_preload_close(int fd);

static int mock_iopath_write(struct iopath *p, uint64_t lba,
                             const void *buf, size_t len)
{
    (void)p; (void)lba; (void)buf; (void)len;
    pthread_mutex_lock(&g_gate_mu);
    if (g_block_raw_write) {
        g_raw_entered = 1;
        pthread_cond_broadcast(&g_gate_cv);
        while (!g_release_raw)
            pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    }
    pthread_mutex_unlock(&g_gate_mu);
    return g_iopath_write_rc;
}

static int mock_iopath_flush(struct iopath *p)
{
    (void)p;
    g_flush_calls++;
    if (g_flush_fail_call > 0 && g_flush_calls == g_flush_fail_call)
        return -EIO;
    return g_flush_rc;
}

static void mock_iopath_close(struct iopath *p)
{
    (void)p;
    g_close_calls++;
}

static struct iopath *mock_iopath_open(const char *path, iopath_backend b,
                                       uint32_t lbs)
{
    (void)lbs;
    g_open_calls++;
    g_last_open_backend = b;
    snprintf(g_last_open_path, sizeof g_last_open_path, "%s",
             path ? path : "(null)");
    return (struct iopath *)(uintptr_t)(0x2000 + g_open_calls * 0x100);
}

static struct iopath *mock_iopath_open_fd(int fd, iopath_backend b,
                                          uint32_t lbs)
{
    g_open_fd_calls++;
    g_last_open_fd = fd;
    g_last_open_fd_backend = b;
    g_last_open_fd_lbs = lbs;
    if (g_open_fd_fail)
        return NULL;
    return (struct iopath *)(uintptr_t)(0x6000 + g_open_fd_calls * 0x100);
}

static int mock_core_fstat(int fd, struct stat *st);

static int mock_iopath_set_fua(struct iopath *p, int on)
{
    (void)p; (void)on;
    return 0;
}

/* The core declares exclusive ownership on every handle it opens; the fake
 * handle above is not a real struct iopath, so the real setter must not
 * write into it. */
static int mock_iopath_set_exclusive(struct iopath *p, int on)
{
    (void)p; (void)on;
    return 0;
}

static int mock_geom_probe(const char *path, struct exitos_geom *g)
{
    int helper_fd;
    (void)path;
    if (g_registration_helper_io) {
        helper_fd = exitos_internal_open_call("/sys/mock/helper", O_RDONLY, 0);
        if (helper_fd >= 0)
            (void)exitos_internal_close_call(helper_fd);
    }
    /* Not every call registration makes can be routed through the direct-call
     * API above. libc issues the open and close behind opendir() and fopen()
     * from inside its own wrappers, and src/iopath.c's staging probe opens
     * /proc/self/pagemap outright, so those arrive at the interposer instead. */
    if (g_registration_wrapper_io) {
        helper_fd = lc_preload_open("/sys/mock/helper", O_RDONLY);
        if (helper_fd >= 0)
            (void)lc_preload_close(helper_fd);
    }
    memset(g, 0, sizeof(*g));
    snprintf(g->part_name, sizeof(g->part_name), "loop-test");
    snprintf(g->disk_path, sizeof(g->disk_path), "/dev/loop-test");
    g->lbs = BS;
    g->fs_bs = BS;
    g->devclass = EXITOS_DEV_LOOP;
    g->writable_raw = 1;
    return 0;
}

static int mock_extent_read_sync(int fd, struct extent_run *out, int max_runs,
                                 int want_sync)
{
    (void)fd; (void)want_sync;
    if (g_registration_mapped && max_runs > 0) {
        memset(out, 0, sizeof(*out));
        out[0].file_off = 0;
        out[0].fs_block = 100;
        out[0].len = BS;
        return 1;
    }
    return 0;
}

static exitos_durability mock_policy_for(const char *path)
{
    (void)path;
    return (exitos_durability)g_policy;
}

int lc_fdatasync_needed(int durability_policy);

/* Private copy of the production core. */
#define iopath_write                         mock_iopath_write
#define iopath_flush                         mock_iopath_flush
#define iopath_close                         mock_iopath_close
#define iopath_open                          mock_iopath_open
#define iopath_open_fd                       mock_iopath_open_fd
#define iopath_set_fua                       mock_iopath_set_fua
#define iopath_set_exclusive                 mock_iopath_set_exclusive
#define exitos_geom_probe                    mock_geom_probe
#define exitos_extent_read_sync              mock_extent_read_sync
#define exitos_durability_policy_for         mock_policy_for
#define exitos_ctx_create                    lc_ctx_create
#define exitos_ctx_create_with_options       lc_ctx_create_with_options
#define exitos_ctx_destroy                   lc_ctx_destroy
#define exitos_register_fd                   lc_register_fd
#define exitos_unregister_fd                 lc_unregister_fd
#define exitos_on_write                      lc_on_write
#define exitos_on_fdatasync                  lc_on_fdatasync
#define exitos_flush_raw_debt                lc_flush_raw_debt
#define exitos_fd_all_writes_took_fast_path  lc_all_fast
#define exitos_on_ftruncate                  lc_on_ftruncate
#define exitos_lba_in_bounds                 lc_lba_in_bounds
#define exitos_fdatasync_needed              lc_fdatasync_needed
#define exitos_note_kernel_write             lc_note_kernel_write
#define exitos_note_kernel_sync              lc_note_kernel_sync
#define exitos_ctx_verify_identity           lc_verify_identity
#define exitos_ctx_strict                    lc_ctx_strict
#define exitos_ctx_is_poisoned               lc_ctx_is_poisoned
#define exitos_ctx_flush_all_and_poison       lc_ctx_flush_all_and_poison
#define exitos_ctx_poison                     lc_ctx_poison
#define exitos_txn_invalidate_mapping          lc_txn_invalidate_mapping
#define fstat                                  mock_core_fstat
#include "../../src/intercept.c"
#undef fstat
#undef iopath_write
#undef iopath_flush
#undef iopath_close
#undef iopath_open
#undef iopath_open_fd
#undef iopath_set_fua
#undef iopath_set_exclusive
#undef exitos_geom_probe
#undef exitos_extent_read_sync
#undef exitos_durability_policy_for
#undef exitos_ctx_create
#undef exitos_ctx_create_with_options
#undef exitos_ctx_destroy
#undef exitos_register_fd
#undef exitos_unregister_fd
#undef exitos_on_write
#undef exitos_on_fdatasync
#undef exitos_flush_raw_debt
#undef exitos_fd_all_writes_took_fast_path
#undef exitos_on_ftruncate
#undef exitos_lba_in_bounds
#undef exitos_fdatasync_needed
#undef exitos_note_kernel_write
#undef exitos_note_kernel_sync
#undef exitos_ctx_verify_identity
#undef exitos_ctx_strict
#undef exitos_ctx_is_poisoned
#undef exitos_ctx_flush_all_and_poison
#undef exitos_ctx_poison
#undef exitos_txn_invalidate_mapping

/* Private copy of the shared frontend configuration.  It must construct the
 * private white-box core above, not a second production context from the
 * linked archive. */
#define exitos_frontend_config_from_env       lc_frontend_config_from_env
#define exitos_frontend_config_destroy        lc_frontend_config_destroy
#define exitos_frontend_config_enabled        lc_frontend_config_enabled
#define exitos_frontend_config_files          lc_frontend_config_files
#define exitos_frontend_config_ctx            lc_frontend_config_ctx
#define exitos_frontend_config_path_selected  lc_frontend_config_path_selected
#define exitos_ctx_create_with_options         lc_ctx_create_with_options
#define exitos_ctx_destroy                     lc_ctx_destroy
#define exitos_ctx_verify_identity             lc_verify_identity
#define exitos_ctx_strict                      lc_ctx_strict
#include "../../src/frontend_config.c"
#undef exitos_frontend_config_from_env
#undef exitos_frontend_config_destroy
#undef exitos_frontend_config_enabled
#undef exitos_frontend_config_files
#undef exitos_frontend_config_ctx
#undef exitos_frontend_config_path_selected
#undef exitos_ctx_create_with_options
#undef exitos_ctx_destroy
#undef exitos_ctx_verify_identity
#undef exitos_ctx_strict

static int g_kernel_pwrite_rc;
static int g_kernel_pwrite_errno;
static int g_kernel_pwrite_calls;
static int g_kernel_write_calls;
static int g_kernel_writev_calls;
static int g_kernel_pwritev_calls;
static void (*g_kernel_pwrite_reentry)(void);
static int g_kernel_fdatasync_rc;
static int g_kernel_fdatasync_errno;
static int g_kernel_fdatasync_calls;
static void (*g_kernel_fdatasync_reentry)(void);
static int g_kernel_fsync_calls;
static int g_kernel_ftruncate_rc;
static int g_kernel_ftruncate_errno;
static int g_kernel_ftruncate_calls;
static int g_kernel_fallocate_rc;
static int g_kernel_fallocate_errno;
static int g_kernel_fallocate_calls;
static int g_kernel_fallocate_mode;
static off_t g_kernel_fallocate_off;
static off_t g_kernel_fallocate_len;
static int g_kernel_dup_rc;
static int g_kernel_dup_errno;
static int g_kernel_dup_calls;
static int g_kernel_dup_newfd;
static int g_kernel_fcntl_rc;
static int g_kernel_fcntl_errno;
static int g_kernel_fcntl_calls;
static int g_kernel_fcntl_cmd;
static unsigned long g_kernel_fcntl_arg;
static int g_kernel_close_rc;
static int g_kernel_close_errno;
static int g_kernel_close_calls;
static int g_kernel_open_calls;
static int g_kernel_open_fd;
static int g_kernel_open_flags;
static char g_kernel_open_path[256];
static void (*g_kernel_open_reentry)(void);
static int g_kernel_pwritev2_calls;
static int g_kernel_close_range_calls;
static int g_kernel_close_range_rc;
static int g_kernel_close_range_errno;
static int g_kernel_freopen_calls;
static FILE *g_kernel_freopen_result;
static int g_kernel_execve_calls;
static int g_kernel_execveat_calls;
static int g_kernel_fexecve_calls;
static int g_kernel_exec_rc;
static int g_kernel_exec_errno;
static int g_frontend_fallocate_exec_calls;
static int g_frontend_fallocate_create_calls;
static int g_frontend_fallocate_destroy_calls;
static int g_frontend_fallocate_takeover;
static int g_frontend_fallocate_rc;
static int g_frontend_fallocate_ctl_depth;
static char g_prepare_ready_line[512];
static char g_prepare_outcome_line[512];

/* Tentative declarations shared with the private preload source included
 * below.  They let the mock preserve the pre-existing lifecycle assertions
 * while separately proving the adapter delegates to the shared runtime. */
static struct exitos_ctx *g_ctx;
static __thread int __attribute__((tls_model("initial-exec"))) g_ctl_depth;

static int mock_frontend_fallocate_runtime_create(
    struct exitos_frontend_fallocate_runtime **out,
    const struct exitos_frontend_config *config, struct exitos_ctx *ctx)
{
    (void)config;
    (void)ctx;
    g_frontend_fallocate_create_calls++;
    *out = (struct exitos_frontend_fallocate_runtime *)(uintptr_t)0x7777;
    return 0;
}

static void mock_frontend_fallocate_runtime_destroy(
    struct exitos_frontend_fallocate_runtime *runtime)
{
    if (runtime)
        g_frontend_fallocate_destroy_calls++;
}

static int mock_frontend_fallocate_execute(
    struct exitos_frontend_fallocate_runtime *runtime, int fd, int mode,
    off_t off, off_t len, exitos_frontend_native_fallocate_fn native_call,
    void *native_opaque, struct exitos_frontend_fallocate_result *result)
{
    struct exitos_fd_txn tx;
    int rc;

    (void)runtime;
    g_frontend_fallocate_exec_calls++;
    g_frontend_fallocate_ctl_depth = g_ctl_depth;
    if (result)
        memset(result, 0, sizeof *result);
    if (g_frontend_fallocate_takeover) {
        if (result) {
            result->used_rdwr_alias = 1;
            result->rc = g_frontend_fallocate_rc;
            if (g_frontend_fallocate_rc == 0) {
                result->outcome = EXITOS_FALLOCATE_PREPARED;
                result->stage = EXITOS_FALLOCATE_STAGE_COMPLETE;
                result->prepared = 1;
                result->prepared_bytes = (uint64_t)len;
                result->chunks = 3;
            } else {
                result->outcome = EXITOS_FALLOCATE_ELIGIBLE_FAILED;
                result->stage = EXITOS_FALLOCATE_STAGE_DONOR_PREPARE;
            }
        }
        return g_frontend_fallocate_rc;
    }
    if (exitos_fd_txn_begin(g_ctx, fd, &tx) != 0)
        return native_call(native_opaque, fd, mode, off, len);
    rc = exitos_txn_flush_raw_debt(&tx);
    if (rc == 0) {
        lc_txn_invalidate_mapping(&tx);
        rc = native_call(native_opaque, fd, mode, off, len);
        if (rc == 0)
            exitos_txn_note_kernel_metadata(&tx);
    }
    exitos_fd_txn_end(&tx);
    if (result) {
        result->outcome = rc == 0 ? EXITOS_FALLOCATE_NATIVE_SKIP :
                                    EXITOS_FALLOCATE_CONTROL_FAILED;
        result->stage = rc == 0 ? EXITOS_FALLOCATE_STAGE_NATIVE :
                                  EXITOS_FALLOCATE_STAGE_FLUSH;
        result->rc = rc;
    }
    return rc;
}

static const char *mock_frontend_fallocate_outcome_name(
    enum exitos_frontend_fallocate_outcome outcome)
{
    switch (outcome) {
    case EXITOS_FALLOCATE_NATIVE_SKIP: return "native-skip";
    case EXITOS_FALLOCATE_ELIGIBLE_FAILED: return "eligible-attempt-failed";
    case EXITOS_FALLOCATE_PREPARED: return "prepared";
    case EXITOS_FALLOCATE_CONTROL_FAILED: return "control-failed";
    default: return "unknown";
    }
}

static const char *mock_frontend_fallocate_stage_name(
    enum exitos_frontend_fallocate_stage stage)
{
    switch (stage) {
    case EXITOS_FALLOCATE_STAGE_NATIVE: return "native";
    case EXITOS_FALLOCATE_STAGE_FLUSH: return "flush";
    case EXITOS_FALLOCATE_STAGE_DONOR_PREPARE: return "donor-prepare";
    case EXITOS_FALLOCATE_STAGE_COMPLETE: return "complete";
    default: return "other";
    }
}

static int mock_preload_fprintf(FILE *stream, const char *format, ...)
{
    va_list ap;
    int n;
    char *destination;

    (void)stream;
    destination = strncmp(format, "PREPARE_OUTCOME ", 16) == 0 ?
                      g_prepare_outcome_line : g_prepare_ready_line;
    va_start(ap, format);
    n = vsnprintf(destination, sizeof g_prepare_ready_line, format, ap);
    va_end(ap);
    return n;
}

/* Only the synthetic fd returned for /dev/block/M:m is a fake block object.
 * Application fds still use the real fstat, so this cannot make an ordinary
 * registration pass by globally lying about its file type. */
static int mock_core_fstat(int fd, struct stat *st)
{
    if (g_fake_block_fstat && fd == g_kernel_open_fd) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFBLK | 0600;
        st->st_rdev = g_fake_block_rdev;
        return 0;
    }
    return fstat(fd, st);
}

static ssize_t mock_kernel_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    (void)fd; (void)buf; (void)off;
    g_kernel_pwrite_calls++;
    {
        void (*reentry)(void) = g_kernel_pwrite_reentry;
        g_kernel_pwrite_reentry = NULL;
        if (reentry)
            reentry();
    }
    pthread_mutex_lock(&g_gate_mu);
    if (g_block_kernel_pwrite) {
        g_kernel_write_entered = 1;
        pthread_cond_broadcast(&g_gate_cv);
        while (!g_release_kernel_write)
            pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    }
    pthread_mutex_unlock(&g_gate_mu);
    if (g_kernel_pwrite_rc < 0) {
        errno = g_kernel_pwrite_errno;
        return -1;
    }
    return (ssize_t)n;
}

static ssize_t mock_kernel_write(int fd, const void *buf, size_t n)
{
    (void)fd;
    (void)buf;
    g_kernel_write_calls++;
    return (ssize_t)n;
}

static ssize_t mock_kernel_writev(int fd, const struct iovec *iov, int cnt)
{
    ssize_t total = 0;
    (void)fd;
    g_kernel_writev_calls++;
    for (int i = 0; i < cnt; ++i)
        total += (ssize_t)iov[i].iov_len;
    return total;
}

static ssize_t mock_kernel_pwritev(int fd, const struct iovec *iov, int cnt,
                                   off_t off)
{
    (void)off;
    g_kernel_pwritev_calls++;
    return mock_kernel_writev(fd, iov, cnt);
}

static int mock_kernel_fdatasync(int fd)
{
    (void)fd;
    g_kernel_fdatasync_calls++;
    {
        void (*reentry)(void) = g_kernel_fdatasync_reentry;
        g_kernel_fdatasync_reentry = NULL;
        if (reentry)
            reentry();
    }
    if (g_kernel_fdatasync_rc < 0) {
        errno = g_kernel_fdatasync_errno;
        return -1;
    }
    return 0;
}

static int mock_kernel_fsync(int fd)
{
    (void)fd;
    g_kernel_fsync_calls++;
    return 0;
}

static int mock_kernel_ftruncate(int fd, off_t len)
{
    (void)fd; (void)len;
    g_kernel_ftruncate_calls++;
    if (g_kernel_ftruncate_rc < 0) {
        errno = g_kernel_ftruncate_errno;
        return -1;
    }
    return 0;
}

static int mock_kernel_fallocate(int fd, int mode, off_t off, off_t len)
{
    (void)fd;
    g_kernel_fallocate_calls++;
    g_kernel_fallocate_mode = mode;
    g_kernel_fallocate_off = off;
    g_kernel_fallocate_len = len;
    if (g_kernel_fallocate_rc < 0) {
        errno = g_kernel_fallocate_errno;
        return -1;
    }
    return 0;
}

static int mock_kernel_dup2(int oldfd, int newfd)
{
    (void)oldfd;
    g_kernel_dup_calls++;
    pthread_mutex_lock(&g_gate_mu);
    if (g_block_kernel_dup) {
        g_kernel_dup_entered = 1;
        pthread_cond_broadcast(&g_gate_cv);
        while (!g_release_kernel_dup)
            pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    }
    pthread_mutex_unlock(&g_gate_mu);
    if (g_kernel_dup_rc < 0) {
        errno = g_kernel_dup_errno;
        return -1;
    }
    return newfd;
}

static int mock_kernel_dup(int oldfd)
{
    (void)oldfd;
    g_kernel_dup_calls++;
    if (g_kernel_dup_rc < 0) {
        errno = g_kernel_dup_errno;
        return -1;
    }
    return g_kernel_dup_newfd;
}

static int mock_kernel_dup3(int oldfd, int newfd, int flags)
{
    (void)oldfd; (void)flags;
    return mock_kernel_dup2(oldfd, newfd);
}

static int mock_kernel_close(int fd)
{
    (void)fd;
    g_kernel_close_calls++;
    if (g_kernel_close_rc < 0) {
        errno = g_kernel_close_errno;
        return -1;
    }
    return 0;
}

static int mock_kernel_open(const char *path, int flags, ...)
{
    void (*reentry)(void) = g_kernel_open_reentry;
    g_kernel_open_calls++;
    g_kernel_open_flags = flags;
    snprintf(g_kernel_open_path, sizeof g_kernel_open_path, "%s",
             path ? path : "(null)");
    /* Clear before invoking it so a signal-style nested open cannot recurse
     * forever through this deterministic mock. */
    g_kernel_open_reentry = NULL;
    if (reentry)
        reentry();
    return path && strcmp(path, "selected-registration-file") == 0
         ? TEST_FD : g_kernel_open_fd;
}

static int mock_kernel_openat(int dfd, const char *path, int flags, ...)
{
    (void)dfd;
    return mock_kernel_open(path, flags);
}

static int mock_kernel_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    (void)fd;
    g_kernel_fcntl_calls++;
    g_kernel_fcntl_cmd = cmd;
    if (cmd != F_GETFL && cmd != F_GETFD) {
        va_start(ap, cmd);
        g_kernel_fcntl_arg = va_arg(ap, unsigned long);
        va_end(ap);
    }
    if (g_kernel_fcntl_rc < 0) {
        errno = g_kernel_fcntl_errno;
        return -1;
    }
    return g_kernel_fcntl_rc;
}

static ssize_t mock_kernel_pwritev2(int fd, const struct iovec *iov, int cnt,
                                    off_t off, int flags)
{
    (void)fd; (void)iov; (void)cnt; (void)off; (void)flags;
    g_kernel_pwritev2_calls++;
    return 17;
}

static int mock_kernel_close_range(unsigned first, unsigned last, int flags)
{
    (void)first; (void)last; (void)flags;
    g_kernel_close_range_calls++;
    if (g_kernel_close_range_rc < 0) {
        errno = g_kernel_close_range_errno;
        return -1;
    }
    return 0;
}

static FILE *mock_kernel_freopen(const char *path, const char *mode, FILE *f)
{
    (void)path; (void)mode; (void)f;
    g_kernel_freopen_calls++;
    return g_kernel_freopen_result;
}

static int mock_kernel_execve(const char *path, char *const argv[],
                              char *const envp[])
{
    (void)path; (void)argv; (void)envp;
    g_kernel_execve_calls++;
    errno = g_kernel_exec_errno;
    return g_kernel_exec_rc;
}

static int mock_kernel_execveat(int dirfd, const char *path,
                                char *const argv[], char *const envp[],
                                int flags)
{
    (void)dirfd; (void)path; (void)argv; (void)envp; (void)flags;
    g_kernel_execveat_calls++;
    errno = g_kernel_exec_errno;
    return g_kernel_exec_rc;
}

static int mock_kernel_fexecve(int fd, char *const argv[], char *const envp[])
{
    (void)fd; (void)argv; (void)envp;
    g_kernel_fexecve_calls++;
    errno = g_kernel_exec_errno;
    return g_kernel_exec_rc;
}

/* Private copy of the production LD_PRELOAD frontend. */
static int mock_frontend_admission_enter(void)
{
    int admitted = exitos_frontend_admission_enter();

    if (!admitted)
        return 0;
    pthread_mutex_lock(&g_gate_mu);
    if (g_block_frontend_admission) {
        g_frontend_admission_entered = 1;
        pthread_cond_broadcast(&g_gate_cv);
        while (!g_release_frontend_admission)
            pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    }
    pthread_mutex_unlock(&g_gate_mu);
    return 1;
}

static void mock_frontend_admission_leave(void)
{
    exitos_frontend_admission_leave();
}

static void mock_frontend_admission_enable(void)
{
    exitos_frontend_admission_enable();
}

static void mock_frontend_admission_quiesce(void)
{
    pthread_mutex_lock(&g_gate_mu);
    g_frontend_quiesce_entered = 1;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
    exitos_frontend_admission_quiesce();
}

#define exitos_frontend_admission_enter     mock_frontend_admission_enter
#define exitos_frontend_admission_leave     mock_frontend_admission_leave
#define exitos_frontend_admission_enable    mock_frontend_admission_enable
#define exitos_frontend_admission_quiesce   mock_frontend_admission_quiesce
#define open                                lc_preload_open
#define openat                              lc_preload_openat
#define write                               lc_preload_write
#define pwrite                              lc_preload_pwrite
#define fdatasync                           lc_preload_fdatasync
#define fsync                               lc_preload_fsync
#define ftruncate                           lc_preload_ftruncate
#define ftruncate64                         lc_preload_ftruncate64
#define fallocate                           lc_preload_fallocate
#define fallocate64                         lc_preload_fallocate64
#define close                               lc_preload_close
#define dup2                                lc_preload_dup2
#define dup3                                lc_preload_dup3
#define dup                                 lc_preload_dup
#define fcntl                               lc_preload_fcntl
#define fcntl64                             lc_preload_fcntl64
#define fclose                              lc_preload_fclose
#define open64                              lc_preload_open64
#define openat64                            lc_preload_openat64
#define __open_2                            lc_preload___open_2
#define __open64_2                          lc_preload___open64_2
#define __openat_2                          lc_preload___openat_2
#define __openat64_2                        lc_preload___openat64_2
#define pwrite64                            lc_preload_pwrite64
#define writev                              lc_preload_writev
#define pwritev                             lc_preload_pwritev
#define pwritev2                            lc_preload_pwritev2
#define pwritev64                           lc_preload_pwritev64
#define pwritev64v2                         lc_preload_pwritev64v2
#define close_range                         lc_preload_close_range
#define freopen                             lc_preload_freopen
#define freopen64                           lc_preload_freopen64
#define execve                              lc_preload_execve
#define execveat                            lc_preload_execveat
#define fexecve                             lc_preload_fexecve
#define exitos_ctx_create                   lc_ctx_create
#define exitos_ctx_create_with_options      lc_ctx_create_with_options
#define exitos_ctx_destroy                  lc_ctx_destroy
#define exitos_register_fd                  lc_register_fd
#define exitos_unregister_fd                lc_unregister_fd
#define exitos_on_write                     lc_on_write
#define exitos_on_fdatasync                 lc_on_fdatasync
#define exitos_flush_raw_debt               lc_flush_raw_debt
#define exitos_on_ftruncate                 lc_on_ftruncate
#define exitos_note_kernel_write            lc_note_kernel_write
#define exitos_note_kernel_sync             lc_note_kernel_sync
#define exitos_ctx_verify_identity          lc_verify_identity
#define exitos_ctx_is_poisoned              lc_ctx_is_poisoned
#define exitos_ctx_flush_all_and_poison      lc_ctx_flush_all_and_poison
#define exitos_ctx_poison                    lc_ctx_poison
#define exitos_txn_invalidate_mapping         lc_txn_invalidate_mapping
#define exitos_frontend_config_from_env      lc_frontend_config_from_env
#define exitos_frontend_config_destroy       lc_frontend_config_destroy
#define exitos_frontend_config_enabled       lc_frontend_config_enabled
#define exitos_frontend_config_files         lc_frontend_config_files
#define exitos_frontend_config_ctx           lc_frontend_config_ctx
#define exitos_frontend_config_path_selected lc_frontend_config_path_selected
#define exitos_frontend_fallocate_runtime_create mock_frontend_fallocate_runtime_create
#define exitos_frontend_fallocate_runtime_destroy mock_frontend_fallocate_runtime_destroy
#define exitos_frontend_fallocate_execute mock_frontend_fallocate_execute
#define exitos_frontend_fallocate_outcome_name mock_frontend_fallocate_outcome_name
#define exitos_frontend_fallocate_stage_name mock_frontend_fallocate_stage_name
#define fprintf                             mock_preload_fprintf
#include "../../src/preload.c"
#undef exitos_frontend_admission_enter
#undef exitos_frontend_admission_leave
#undef exitos_frontend_admission_enable
#undef exitos_frontend_admission_quiesce
#undef open
#undef openat
#undef write
#undef pwrite
#undef fdatasync
#undef fsync
#undef ftruncate
#undef ftruncate64
#undef fallocate
#undef fallocate64
#undef close
#undef dup2
#undef dup3
#undef dup
#undef fcntl
#undef fcntl64
#undef fclose
#undef open64
#undef openat64
#undef __open_2
#undef __open64_2
#undef __openat_2
#undef __openat64_2
#undef pwrite64
#undef writev
#undef pwritev
#undef pwritev2
#undef pwritev64
#undef pwritev64v2
#undef close_range
#undef freopen
#undef freopen64
#undef execve
#undef execveat
#undef fexecve
#undef exitos_ctx_create
#undef exitos_ctx_create_with_options
#undef exitos_ctx_destroy
#undef exitos_register_fd
#undef exitos_unregister_fd
#undef exitos_on_write
#undef exitos_on_fdatasync
#undef exitos_flush_raw_debt
#undef exitos_on_ftruncate
#undef exitos_note_kernel_write
#undef exitos_note_kernel_sync
#undef exitos_ctx_verify_identity
#undef exitos_ctx_is_poisoned
#undef exitos_ctx_flush_all_and_poison
#undef exitos_ctx_poison
#undef exitos_txn_invalidate_mapping
#undef exitos_frontend_config_from_env
#undef exitos_frontend_config_destroy
#undef exitos_frontend_config_enabled
#undef exitos_frontend_config_files
#undef exitos_frontend_config_ctx
#undef exitos_frontend_config_path_selected
#undef exitos_frontend_fallocate_runtime_create
#undef exitos_frontend_fallocate_runtime_destroy
#undef exitos_frontend_fallocate_execute
#undef exitos_frontend_fallocate_outcome_name
#undef exitos_frontend_fallocate_stage_name
#undef fprintf

static void reset_all(void)
{
    pthread_mutex_lock(&g_gate_mu);
    g_block_raw_write = 0;
    g_raw_entered = 0;
    g_release_raw = 0;
    g_block_kernel_pwrite = 0;
    g_kernel_write_entered = 0;
    g_release_kernel_write = 0;
    g_block_kernel_dup = 0;
    g_kernel_dup_entered = 0;
    g_release_kernel_dup = 0;
    g_block_frontend_admission = 0;
    g_frontend_admission_entered = 0;
    g_release_frontend_admission = 0;
    g_frontend_quiesce_entered = 0;
    pthread_mutex_unlock(&g_gate_mu);
    g_flush_rc = 0;
    g_flush_calls = 0;
    g_flush_fail_call = 0;
    g_close_calls = 0;
    g_open_calls = 0;
    g_last_open_backend = IOPATH_PWRITE;
    g_last_open_path[0] = '\0';
    g_open_fd_calls = 0;
    g_last_open_fd = -1;
    g_last_open_fd_backend = IOPATH_PWRITE;
    g_last_open_fd_lbs = 0;
    g_open_fd_fail = 0;
    g_fake_block_fstat = 0;
    g_fake_block_rdev = 0;
    g_registration_helper_io = 0;
    g_registration_wrapper_io = 0;
    g_registration_mapped = 0;
    g_policy = EXITOS_DUR_FLUSH;
    g_iopath_write_rc = 0;
    g_kernel_pwrite_rc = 0;
    g_kernel_pwrite_errno = 0;
    g_kernel_pwrite_calls = 0;
    g_kernel_write_calls = 0;
    g_kernel_writev_calls = 0;
    g_kernel_pwritev_calls = 0;
    g_kernel_pwrite_reentry = NULL;
    g_kernel_fdatasync_rc = 0;
    g_kernel_fdatasync_errno = 0;
    g_kernel_fdatasync_calls = 0;
    g_kernel_fdatasync_reentry = NULL;
    g_kernel_fsync_calls = 0;
    g_kernel_ftruncate_rc = 0;
    g_kernel_ftruncate_errno = 0;
    g_kernel_ftruncate_calls = 0;
    g_kernel_fallocate_rc = 0;
    g_kernel_fallocate_errno = 0;
    g_kernel_fallocate_calls = 0;
    g_kernel_fallocate_mode = 0;
    g_kernel_fallocate_off = 0;
    g_kernel_fallocate_len = 0;
    g_kernel_dup_rc = 0;
    g_kernel_dup_errno = 0;
    g_kernel_dup_calls = 0;
    g_kernel_dup_newfd = TEST_FD;
    g_kernel_fcntl_rc = TEST_FD;
    g_kernel_fcntl_errno = 0;
    g_kernel_fcntl_calls = 0;
    g_kernel_fcntl_cmd = 0;
    g_kernel_fcntl_arg = 0;
    g_kernel_close_rc = 0;
    g_kernel_close_errno = 0;
    g_kernel_close_calls = 0;
    g_kernel_open_calls = 0;
    g_kernel_open_fd = 905;
    g_kernel_open_flags = 0;
    g_kernel_open_path[0] = '\0';
    g_kernel_open_reentry = NULL;
    g_kernel_pwritev2_calls = 0;
    g_kernel_close_range_calls = 0;
    g_kernel_close_range_rc = 0;
    g_kernel_close_range_errno = 0;
    g_kernel_freopen_calls = 0;
    g_kernel_freopen_result = NULL;
    g_kernel_execve_calls = 0;
    g_kernel_execveat_calls = 0;
    g_kernel_fexecve_calls = 0;
    g_kernel_exec_rc = -1;
    g_kernel_exec_errno = ENOENT;
    g_frontend_fallocate_exec_calls = 0;
    g_frontend_fallocate_create_calls = 0;
    g_frontend_fallocate_destroy_calls = 0;
    g_frontend_fallocate_takeover = 0;
    g_frontend_fallocate_rc = 0;
    g_frontend_fallocate_ctl_depth = 0;
    g_prepare_ready_line[0] = '\0';
    g_prepare_outcome_line[0] = '\0';
    o_write = mock_kernel_write;
    o_pwrite = mock_kernel_pwrite;
    o_fdatasync = mock_kernel_fdatasync;
    o_fsync = mock_kernel_fsync;
    o_ftruncate = mock_kernel_ftruncate;
    o_ftruncate64 = mock_kernel_ftruncate;
    o_fallocate = mock_kernel_fallocate;
    o_fallocate64 = mock_kernel_fallocate;
    o_dup2 = mock_kernel_dup2;
    o_dup3 = mock_kernel_dup3;
    o_dup = mock_kernel_dup;
    o_fcntl = mock_kernel_fcntl;
    o_fcntl64 = mock_kernel_fcntl;
    o_close = mock_kernel_close;
    o_open = mock_kernel_open;
    o_openat = mock_kernel_openat;
    o_pwritev2 = mock_kernel_pwritev2;
    o_writev = mock_kernel_writev;
    o_pwritev = mock_kernel_pwritev;
    o_close_range = mock_kernel_close_range;
    o_freopen = mock_kernel_freopen;
    o_execve = mock_kernel_execve;
    o_execveat = mock_kernel_execveat;
    o_fexecve = mock_kernel_fexecve;
    g_enabled = 1;
    g_fastpath = 1;
    g_in_hook = 0;
    exitos_frontend_admission_enable();
}

static struct exitos_ctx *arm_test_frontend(const char *files)
{
    int rc;

    g_enabled = 0;
    g_ctx = NULL;
    if (g_config) {
        lc_frontend_config_destroy(g_config);
        g_config = NULL;
    }
    setenv("EXITOS_FILES", files, 1);
    unsetenv("EXITOS_IOPATH");
    unsetenv("EXITOS_STRICT");
    unsetenv("EXITOS_VERIFY_IDENTITY");
    rc = lc_frontend_config_from_env(&g_config);
    unsetenv("EXITOS_FILES");
    if (rc != 0)
        return NULL;
    g_enabled = lc_frontend_config_enabled(g_config);
    g_ctx = lc_frontend_config_ctx(g_config);
    if (g_enabled)
        exitos_frontend_admission_enable();
    return g_ctx;
}

static void disarm_test_frontend(void)
{
    exitos_frontend_admission_quiesce();
    g_enabled = 0;
    g_ctx = NULL;
    lc_frontend_config_destroy(g_config);
    g_config = NULL;
}

static struct exitos_ctx *fake_ctx_fd(int fd, int dur, int kernel_debt,
                                      int raw_debt, int mapped)
{
    struct exitos_ctx *c = lc_ctx_create();
    struct reg *r;

    if (!c)
        return NULL;
    c->r = calloc(1, sizeof(*c->r));
    if (!c->r) {
        lc_ctx_destroy(c);
        return NULL;
    }
    r = calloc(1, sizeof(*r));
    if (!r) {
        free(r);
        lc_ctx_destroy(c);
        return NULL;
    }
    c->n = c->cap = 1;
    c->r[0] = r;
    pthread_mutex_init(&r->mu, NULL);
    r->active = 1;
    r->fd = fd;
    r->io = (struct iopath *)(uintptr_t)0x1000;
    r->dur = dur;
    r->kernel_holds = kernel_debt;
    r->unflushed = raw_debt;
    r->g.lbs = BS;
    r->g.fs_bs = BS;
    r->isize = BS;
    if (mapped) {
        r->m = maco_create(BS);
        r->iv = calloc(1, sizeof(*r->iv));
        if (!r->m || !r->iv || maco_insert(r->m, 0, 100, BS) != 0) {
            lc_ctx_destroy(c);
            return NULL;
        }
        r->iv[0].lo = 100;
        r->iv[0].hi = 101;
        r->niv = 1;
    }
    g_ctx = c;
    return c;
}

static struct exitos_ctx *fake_ctx(int dur, int kernel_debt, int raw_debt,
                                    int mapped)
{
    return fake_ctx_fd(TEST_FD, dur, kernel_debt, raw_debt, mapped);
}

static struct exitos_ctx *fake_ctx_two(int first_fd, int second_fd,
                                       int raw_debt)
{
    struct exitos_ctx *c = fake_ctx_fd(first_fd, EXITOS_DUR_FLUSH, 0,
                                       raw_debt, 0);
    struct reg *r;
    if (!c)
        return NULL;
    r = calloc(1, sizeof(*r));
    if (!r) {
        g_ctx = NULL;
        lc_ctx_destroy(c);
        return NULL;
    }
    pthread_mutex_init(&r->mu, NULL);
    r->active = 1;
    r->fd = second_fd;
    r->io = (struct iopath *)(uintptr_t)0x3000;
    r->dur = EXITOS_DUR_FLUSH;
    r->unflushed = raw_debt;
    {
        struct reg **nr = realloc(c->r, 2 * sizeof(*c->r));
        if (!nr) {
            reg_release(r);
            g_ctx = NULL;
            lc_ctx_destroy(c);
            return NULL;
        }
        c->r = nr;
    }
    if (first_fd < second_fd) {
        c->r[1] = r;
    } else {
        c->r[1] = c->r[0];
        c->r[0] = r;
    }
    c->n = c->cap = 2;
    return c;
}

static void drop_fake(struct exitos_ctx *c)
{
    g_ctx = NULL;
    lc_ctx_destroy(c);
}

static void test_uringwritepoll_selection(void)
{
    char path[] = "/tmp/exitos-block-backend-XXXXXX";
    struct stat st;
    struct exitos_ctx *c;
    char expected[128];
    int fd = mkstemp(path);

    T_OK(fd >= 0, "created ordinary-file registration fixture for block backend selection");
    if (fd < 0)
        return;
    T_EQ(fstat(fd, &st), 0, "captured the intercepted file's exact st_dev");
    snprintf(expected, sizeof expected, "/dev/block/%u:%u",
             major(st.st_dev), minor(st.st_dev));

    reset_all();
    setenv("EXITOS_IOPATH", "uringwritepoll", 1);
    g_registration_mapped = 1;
    g_fake_block_fstat = 1;
    g_fake_block_rdev = st.st_dev;
    c = lc_ctx_create();
    T_EQ(lc_register_fd(c, fd), 0,
         "explicit uringwritepoll registration accepts the exact block object");
    T_EQ(g_open_fd_calls, 1,
         "uringwritepoll passes one retained block fd to iopath_open_fd");
    T_EQ(g_last_open_fd, g_kernel_open_fd,
         "iopath_open_fd receives the object returned by the exact block open");
    T_EQ(g_last_open_fd_backend, IOPATH_URING_WRITE_POLL,
         "retained object is opened with the distinct ordinary-WRITE backend");
    T_EQ(g_last_open_fd_lbs, BS, "retained object carries the probed native LBS");
    T_OK(strcmp(g_kernel_open_path, expected) == 0,
         "block object path derives from file st_dev, not geom disk basename (%s)",
         g_kernel_open_path);
    T_EQ(g_kernel_open_flags, O_RDWR | O_DIRECT | O_CLOEXEC,
         "exact block object is opened O_RDWR|O_DIRECT|O_CLOEXEC");
    T_EQ(g_open_calls, 0,
         "explicit block backend never enters pathname passthrough fallback");
    drop_fake(c);

    reset_all();
    setenv("EXITOS_IOPATH", "uringwritepoll", 1);
    g_registration_mapped = 1;
    g_fake_block_fstat = 1;
    g_fake_block_rdev = st.st_dev;
    g_open_fd_fail = 1;
    c = lc_ctx_create();
    T_OK(lc_register_fd(c, fd) != 0,
         "explicit uringwritepoll refuses when its retained backend cannot open");
    T_EQ(g_open_fd_calls, 1,
         "backend-open failure occurs after the exact block object is retained");
    T_EQ(g_open_calls, 0,
         "retained backend failure cannot fall back to passthrough or pwrite");
    drop_fake(c);

    reset_all();
    setenv("EXITOS_IOPATH", "uringwritepoll", 1);
    g_registration_mapped = 1;
    g_fake_block_fstat = 1;
    g_fake_block_rdev = makedev(major(st.st_dev), minor(st.st_dev) + 1u);
    c = lc_ctx_create();
    T_OK(lc_register_fd(c, fd) != 0,
         "block object whose opened rdev differs from file st_dev is refused");
    T_EQ(g_open_fd_calls, 0,
         "rdev mismatch is rejected before constructing an iopath handle");
    T_EQ(g_open_calls, 0,
         "explicit uringwritepoll failure never silently falls back");
    drop_fake(c);

    reset_all();
    unsetenv("EXITOS_IOPATH");
    g_registration_mapped = 1;
    c = lc_ctx_create();
    T_EQ(lc_register_fd(c, fd), 0,
         "registration with EXITOS_IOPATH unset keeps the fallback selection");
    T_EQ(g_open_calls, 1, "unset-mode selection uses the pathname backend opener");
    T_EQ(g_last_open_backend, IOPATH_URING_CMD_POLL,
         "unset-mode selection starts at uringpoll passthrough");
    T_EQ(g_open_fd_calls, 0, "unset-mode selection does not enter block-WRITE opener");
    drop_fake(c);

    unsetenv("EXITOS_IOPATH");
    close(fd);
    unlink(path);
}

static void test_iomode_table_and_context_snapshot(void)
{
    static const struct {
        const char *name;
        iopath_backend backend;
    } modes[] = {
        { "pwrite",         IOPATH_PWRITE },
        { "nvme",           IOPATH_NVME_IOCTL },
        { "uring",          IOPATH_URING_CMD },
        { "uringpoll",      IOPATH_URING_CMD_POLL },
        { "uringwritepoll", IOPATH_URING_WRITE_POLL },
    };
    char first_path[] = "/tmp/exitos-iomode-first-XXXXXX";
    char second_path[] = "/tmp/exitos-iomode-second-XXXXXX";
    int first_fd = mkstemp(first_path);
    int second_fd = mkstemp(second_path);
    struct stat st;
    struct exitos_ctx *c;
    size_t i;

    T_OK(first_fd >= 0 && second_fd >= 0,
         "created two ordinary-file fixtures for immutable I/O-mode selection");
    if (first_fd < 0 || second_fd < 0)
        goto out;
    T_EQ(fstat(first_fd, &st), 0,
         "captured the fixture device identity for block-WRITE selection");

    /* This table is deliberately exercised through registration rather than
     * merely through a string parser.  It proves that the one accepted name is
     * the backend actually handed to the iopath construction boundary. */
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        reset_all();
        setenv("EXITOS_IOPATH", modes[i].name, 1);
        g_registration_mapped = 1;
        g_fake_block_fstat = 1;
        g_fake_block_rdev = st.st_dev;
        c = lc_ctx_create();
        T_OK(c != NULL, "mode %s creates a context", modes[i].name);
        if (!c)
            continue;
        T_EQ(lc_register_fd(c, first_fd), 0,
             "mode %s registers through its requested backend", modes[i].name);
        if (modes[i].backend == IOPATH_URING_WRITE_POLL) {
            T_EQ(g_open_fd_calls, 1,
                 "mode %s uses the retained exact-block-fd opener", modes[i].name);
            T_EQ(g_last_open_fd_backend, modes[i].backend,
                 "mode %s maps to the expected retained-fd backend", modes[i].name);
            T_EQ(g_open_calls, 0,
                 "mode %s never enters the pathname opener", modes[i].name);
        } else {
            T_EQ(g_open_calls, 1,
                 "mode %s uses the pathname backend opener", modes[i].name);
            T_EQ(g_last_open_backend, modes[i].backend,
                 "mode %s maps to the expected pathname backend", modes[i].name);
            T_EQ(g_open_fd_calls, 0,
                 "mode %s never enters the retained block-fd opener", modes[i].name);
        }
        drop_fake(c);
    }

    /* The mode is process/context configuration, not a per-registration (and
     * especially not a per-I/O-size) decision.  The current implementation
     * re-reads EXITOS_IOPATH in reg_build(), so the second assertion is the
     * intentional RED that forces the snapshot into struct exitos_ctx. */
    reset_all();
    setenv("EXITOS_IOPATH", "pwrite", 1);
    g_registration_mapped = 1;
    c = lc_ctx_create();
    T_OK(c != NULL, "created a context while pwrite was selected");
    if (c) {
        T_EQ(lc_register_fd(c, first_fd), 0,
             "first fd registers under the mode present at context creation");
        T_EQ(g_last_open_backend, IOPATH_PWRITE,
             "first fd uses the snapshotted pwrite backend");
        setenv("EXITOS_IOPATH", "uring", 1);
        T_EQ(lc_register_fd(c, second_fd), 0,
             "second fd still registers after the process environment changes");
        T_EQ(g_last_open_backend, IOPATH_PWRITE,
             "second fd keeps the context's immutable pwrite backend");
        drop_fake(c);
    }

    /* Invalid explicit configuration is rejected at context construction,
     * before registration can reach either backend-opening seam. */
    reset_all();
    setenv("EXITOS_IOPATH", "", 1);
    errno = 0;
    c = lc_ctx_create();
    T_OK(c == NULL, "empty explicit mode rejects context creation");
    T_EQ(errno, EINVAL, "empty explicit mode reports EINVAL");
    T_EQ(g_open_calls + g_open_fd_calls, 0,
         "empty explicit mode opens no backend");

    reset_all();
    setenv("EXITOS_IOPATH", "unknown-static-mode", 1);
    errno = 0;
    c = lc_ctx_create();
    T_OK(c == NULL, "unknown explicit mode rejects context creation");
    T_EQ(errno, EINVAL, "unknown explicit mode reports EINVAL");
    T_EQ(g_open_calls + g_open_fd_calls, 0,
         "unknown explicit mode opens no backend");

out:
    unsetenv("EXITOS_IOPATH");
    if (first_fd >= 0) {
        close(first_fd);
        unlink(first_path);
    }
    if (second_fd >= 0) {
        close(second_fd);
        unlink(second_path);
    }
}

static int wait_flag(int *flag, long millis)
{
    struct timespec ts;
    int rc = 0;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (millis % 1000) * 1000000L;
    ts.tv_sec += millis / 1000 + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&g_gate_mu);
    while (!*flag && rc == 0)
        rc = pthread_cond_timedwait(&g_gate_cv, &g_gate_mu, &ts);
    rc = *flag ? 1 : 0;
    pthread_mutex_unlock(&g_gate_mu);
    return rc;
}

struct call_arg {
    struct exitos_ctx *c;
    void *buf;
    int done;
    int rc;
    ssize_t sres;
    int oldfd;
    int newfd;
};

static void mark_done(struct call_arg *a)
{
    pthread_mutex_lock(&g_gate_mu);
    a->done = 1;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}

static void *raw_writer(void *opaque)
{
    struct call_arg *a = opaque;
    a->rc = lc_on_write(a->c, TEST_FD, a->buf, BS, 0, &a->sres);
    mark_done(a);
    return NULL;
}

static void *raw_flusher(void *opaque)
{
    struct call_arg *a = opaque;
    a->rc = lc_flush_raw_debt(a->c, TEST_FD);
    mark_done(a);
    return NULL;
}

static void *unregisterer(void *opaque)
{
    struct call_arg *a = opaque;
    a->rc = lc_unregister_fd(a->c, TEST_FD);
    mark_done(a);
    return NULL;
}

static void *preload_pwriter(void *opaque)
{
    struct call_arg *a = opaque;
    a->sres = lc_preload_pwrite(TEST_FD, (char *)a->buf + 1, BS, 0);
    mark_done(a);
    return NULL;
}

static void *preload_finisher(void *opaque)
{
    struct call_arg *a = opaque;
    exitos_preload_fini();
    mark_done(a);
    return NULL;
}

static void *preload_syncer(void *opaque)
{
    struct call_arg *a = opaque;
    a->rc = lc_preload_fdatasync(TEST_FD);
    mark_done(a);
    return NULL;
}

static void *preload_duper(void *opaque)
{
    struct call_arg *a = opaque;
    a->rc = lc_preload_dup2(a->oldfd, a->newfd);
    mark_done(a);
    return NULL;
}

static void signal_close_during_internal_open(void)
{
    (void)lc_preload_close(TEST_FD);
}

static void signal_write_during_internal_open(void)
{
    char b = 0;
    (void)lc_preload_pwrite(TEST_FD, &b, 1, 0);
}

static void signal_open_during_internal_open(void)
{
    (void)lc_preload_open("signal-handler-file", O_RDONLY);
}

#ifndef EXITOS_TEST_OLD_MARKER_RED
static void signal_sync_during_internal_call(void)
{
    (void)lc_preload_fdatasync(TEST_FD);
}
#endif

static void test_preload_shutdown_admission(void *buf)
{
    struct call_arg writer = { .buf = buf };
    struct call_arg finisher = { 0 };
    pthread_t write_thread, fini_thread;
    int admitted;

    reset_all();
    T_OK(arm_test_frontend("shutdown-admission") != NULL,
         "armed shared config for preload shutdown admission test");
    exitos_frontend_admission_quiesce();
    exitos_frontend_admission_enable();
    pthread_mutex_lock(&g_gate_mu);
    g_block_frontend_admission = 1;
    pthread_mutex_unlock(&g_gate_mu);

    T_EQ(pthread_create(&write_thread, NULL, preload_pwriter, &writer), 0,
         "started preload caller in the check/use window");
    admitted = wait_flag(&g_frontend_admission_entered, 500);
    T_OK(admitted,
         "preload entry holds shared admission before borrowing its context");
    if (!admitted) {
        pthread_join(write_thread, NULL);
        exitos_frontend_admission_quiesce();
        disarm_test_frontend();
        return;
    }

    T_EQ(pthread_create(&fini_thread, NULL, preload_finisher, &finisher), 0,
         "started preload destructor while a caller is admitted");
    T_OK(wait_flag(&g_frontend_quiesce_entered, 500),
         "preload destructor closes the shared admission gate first");
    pthread_mutex_lock(&g_gate_mu);
    T_EQ(finisher.done, 0,
         "preload destructor does not free config while a caller is admitted");
    g_release_frontend_admission = 1;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);

    pthread_join(write_thread, NULL);
    pthread_join(fini_thread, NULL);
    T_EQ(finisher.done, 1,
         "preload destructor completes after the admitted caller leaves");
    T_OK(g_config == NULL && g_ctx == NULL,
         "preload destructor clears ownership only after quiescence");
}

int main(void)
{
    struct exitos_ctx *c;
    void *buf = NULL;
    char tmp[] = "/tmp/exitos-lifecycle-a-XXXXXX";
    char tmp2[] = "/tmp/exitos-lifecycle-b-XXXXXX";
    int tfd;

    T_EQ(posix_memalign(&buf, BS, BS), 0, "allocated aligned fake write buffer");
    if (!buf)
        T_DONE();
    memset(buf, 0xa5, BS);

    test_uringwritepoll_selection();
    test_iomode_table_and_context_snapshot();
    test_preload_shutdown_admission(buf);

    /* Registration is an authority boundary, not a hint.  The raw backend is
     * writable even when the selected application fd is not, and it bypasses
     * buffered/synchronous open-file-description semantics.  Refuse those
     * modes before geometry or an iopath can be armed. */
    {
        char gate_path[] = "/tmp/exitos-mode-gate-XXXXXX";
        int seed = mkstemp(gate_path);
        int fd;

        T_OK(seed >= 0, "created mode-gate fixture");
        if (seed >= 0) {
            close(seed);

            reset_all();
            c = lc_ctx_create();
            fd = open(gate_path, O_RDONLY | O_DIRECT);
            T_OK(fd >= 0, "opened read-only O_DIRECT fixture");
            if (fd >= 0) {
                T_EQ(lc_register_fd(c, fd), -EBADF,
                     "central gate refuses a read-only O_DIRECT fd");
                g_ctx = c;
                g_kernel_pwrite_rc = -1;
                g_kernel_pwrite_errno = EBADF;
                errno = 0;
                T_EQ(lc_preload_pwrite(fd, buf, BS, 0), -1,
                     "read-only pwrite remains on the kernel path");
                T_EQ(errno, EBADF,
                     "read-only pwrite preserves required EBADF semantics");
                close(fd);
            }
            drop_fake(c);

            reset_all();
            c = lc_ctx_create();
            fd = open(gate_path, O_RDWR);
            T_OK(fd >= 0, "opened buffered writable fixture");
            if (fd >= 0) {
                /* The paper never conditions interception on O_DIRECT, so a
                 * buffered writable fd is no longer refused at the gate; it
                 * opts into the page-cache coherence machinery instead. */
                T_OK(lc_register_fd(c, fd) != -EOPNOTSUPP,
                     "central gate accepts a writable fd without O_DIRECT");
                close(fd);
            }
            drop_fake(c);

            reset_all();
            c = lc_ctx_create();
            fd = open(gate_path, O_RDWR | O_DIRECT | O_APPEND);
            T_OK(fd >= 0, "opened append O_DIRECT fixture");
            if (fd >= 0) {
                T_EQ(lc_register_fd(c, fd), -EOPNOTSUPP,
                     "central gate refuses O_APPEND");
                close(fd);
            }
            drop_fake(c);

            reset_all();
            c = lc_ctx_create();
            fd = open(gate_path, O_RDWR | O_DIRECT | O_DSYNC);
            T_OK(fd >= 0, "opened synchronous O_DIRECT fixture");
            if (fd >= 0) {
                T_EQ(lc_register_fd(c, fd), -EOPNOTSUPP,
                     "central gate refuses per-write O_DSYNC semantics");
                close(fd);
            }
            drop_fake(c);

            unlink(gate_path);
        }
    }

    /* Registration performs control-plane opens of sysfs attributes and the
     * retained iopath.  Explicit internal I/O must bypass the interposer; a
     * consumable "next open" TLS token would be stealable by a signal. */
    reset_all();
    {
        char reg_path[] = "/tmp/exitos-register-helper-XXXXXX";
        int regfd = mkstemp(reg_path);
        ssize_t wr = -1;

        T_OK(regfd >= 0, "created registration helper fixture");
        if (regfd >= 0) {
            T_OK(fcntl(regfd, F_SETFL, fcntl(regfd, F_GETFL) | O_DIRECT) == 0,
                 "made registration helper fixture O_DIRECT");
            T_EQ(dup2(regfd, TEST_FD), TEST_FD,
                 "installed registration helper fixture at stable fd");
            close(regfd);
            c = arm_test_frontend("selected-registration-file");
            T_OK(c != NULL, "shared frontend config armed helper fixture");
            g_registration_helper_io = 1;
            g_registration_mapped = 1;
            T_EQ(lc_preload_open("selected-registration-file",
                                 O_RDWR | O_DIRECT), TEST_FD,
                 "registration succeeds through synchronous internal helper I/O");
            T_EQ(lc_ctx_is_poisoned(c), 0,
                 "registration helper open/close does not poison context");
            T_EQ(lc_on_write(c, TEST_FD, buf, BS, 0, &wr), EXITOS_TAKEOVER,
                 "first mapped write after registration remains TAKEOVER");
            disarm_test_frontend();
            close(TEST_FD);
            unlink(reg_path);
        }
    }

    /* The same registration, with its nested libc call arriving through an
     * interposed wrapper rather than the direct-call API. That call is
     * synchronous library control code, not a signal that interrupted a raw
     * transaction, and no device write is in flight, so it must pass straight
     * through. Counting it as reentry poisoned the context during the very
     * first registration under LD_PRELOAD; every later exitos_register_fd()
     * then returned -ECANCELED from the poison gate and printed "declined
     * (normal kernel path)", while every write went to the kernel. The bpftime
     * frontend had the same defect and fixed it with a control-path depth. */
    reset_all();
    {
        char reg_path[] = "/tmp/exitos-register-wrapper-XXXXXX";
        int regfd = mkstemp(reg_path);
        int again;

        T_OK(regfd >= 0, "created control-path reentry fixture");
        if (regfd >= 0) {
            T_OK(fcntl(regfd, F_SETFL, fcntl(regfd, F_GETFL) | O_DIRECT) == 0,
                 "made control-path reentry fixture O_DIRECT");
            T_EQ(dup2(regfd, TEST_FD), TEST_FD,
                 "installed control-path reentry fixture at stable fd");
            close(regfd);
            c = arm_test_frontend("selected-registration-file");
            T_OK(c != NULL, "shared frontend config armed wrapper fixture");
            g_registration_wrapper_io = 1;
            g_registration_mapped = 1;
            T_EQ(lc_preload_open("selected-registration-file",
                                 O_RDWR | O_DIRECT), TEST_FD,
                 "registration completes when its own call reaches the interposer");
            T_EQ(lc_ctx_is_poisoned(c), 0,
                 "a libc call made BY registration is not signal reentry");
            again = lc_register_fd(c, TEST_FD);
            T_OK(again != -ECANCELED,
                 "the poison gate is not what refuses the next registration");
            disarm_test_frontend();
            close(TEST_FD);
            unlink(reg_path);
        }
    }

    /* Signal mutation in the middle of direct internal open cannot steal an
     * internal token, because this design has no such token. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 0, 0);
    g_in_hook = 1;
    g_kernel_open_reentry = signal_close_during_internal_open;
    T_EQ(exitos_internal_open_call("/sys/mock/helper", O_RDONLY, 0), 905,
         "internal open completes around signal close injection");
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "signal close during internal open poisons context");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 0, 0);
    g_in_hook = 1;
    g_kernel_open_reentry = signal_write_during_internal_open;
    T_EQ(exitos_internal_open_call("/sys/mock/helper", O_RDONLY, 0), 905,
         "internal open completes around signal write injection");
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "signal write during internal open poisons context");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 0, 0);
    g_in_hook = 1;
    g_kernel_open_reentry = signal_open_during_internal_open;
    T_EQ(exitos_internal_open_call("/sys/mock/helper", O_RDONLY, 0), 905,
         "internal open completes around signal open injection");
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "signal open during internal open poisons context");
    drop_fake(c);

    /* A backend error is not proof that the device wrote zero bytes: pwrite
     * can short-write before a later error and native submission/completion
     * failures can be ambiguous.  Debt therefore begins before submission and
     * survives both a raw error and a failed filesystem fallback. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 0, 1);
    g_iopath_write_rc = -EIO;
    g_kernel_pwrite_rc = -1;
    g_kernel_pwrite_errno = ENOSPC;
    errno = 0;
    T_EQ(lc_preload_pwrite(TEST_FD, buf, BS, 0), -1,
         "ambiguous raw failure followed by fallback failure is reported");
    T_EQ(errno, ENOSPC, "fallback pwrite errno remains visible to caller");
    T_EQ(c->r[0]->unflushed, 1,
         "raw submission error conservatively retains raw durability debt");
    T_EQ(c->r[0]->kernel_holds, 1,
         "attempted filesystem fallback conservatively retains kernel debt");
    g_flush_rc = -EREMOTEIO;
    errno = 0;
    T_EQ(lc_preload_fdatasync(TEST_FD), -1,
         "next sync propagates ambiguous raw write's FLUSH failure");
    T_EQ(errno, EREMOTEIO, "raw FLUSH errno is preserved");
    T_EQ(c->r[0]->unflushed, 1,
         "failed FLUSH leaves ambiguous raw debt retryable");
    T_EQ(g_kernel_fdatasync_calls, 0,
         "kernel sync cannot mask the raw durability failure");
    g_flush_rc = 0;
    T_EQ(lc_preload_fdatasync(TEST_FD), 0,
         "retry sync succeeds after raw FLUSH recovers");
    T_EQ(c->r[0]->unflushed, 0, "successful retry clears raw debt");
    T_EQ(g_kernel_fdatasync_calls, 1,
         "successful retry also settles conservative kernel debt");
    drop_fake(c);

    /* 1. Re-registering must settle debt under the OLD policy before replacing
     * the record.  In particular, switching to FUA cannot erase old FLUSH debt. */
    reset_all();
    tfd = mkstemp(tmp);
    T_OK(tfd >= 0, "created ordinary fd used only for registration metadata");
    if (tfd >= 0) {
        T_OK(fcntl(tfd, F_SETFL, fcntl(tfd, F_GETFL) | O_DIRECT) == 0,
             "made replacement fixture O_DIRECT");
        T_EQ(dup2(tfd, TEST_FD), TEST_FD, "installed registration fixture at stable fd");
        close(tfd);
        c = fake_ctx(EXITOS_DUR_FLUSH, 1, 1, 0);
        g_policy = EXITOS_DUR_FUA;
        g_flush_rc = -EIO;
        T_EQ(lc_register_fd(c, TEST_FD), -EIO,
             "re-register fails when old FLUSH-policy raw debt cannot be paid");
        T_EQ(c->n, 1, "failed re-register keeps the old registration");
        T_EQ(c->r[0]->dur, EXITOS_DUR_FLUSH,
             "failed re-register keeps the old durability policy");
        T_EQ(c->r[0]->unflushed, 1, "failed re-register keeps raw debt retryable");
        T_EQ(c->r[0]->kernel_holds, 1, "failed re-register keeps kernel debt");
        T_EQ(g_flush_calls, 1, "re-register used the old FLUSH policy");
        drop_fake(c);
        close(TEST_FD);
        unlink(tmp);
    }

    reset_all();
    tfd = mkstemp(tmp2);
    if (tfd >= 0) {
        T_OK(fcntl(tfd, F_SETFL, fcntl(tfd, F_GETFL) | O_DIRECT) == 0,
             "made recreated fixture O_DIRECT");
        T_EQ(dup2(tfd, TEST_FD), TEST_FD, "recreated registration fixture");
        close(tfd);
        c = fake_ctx(EXITOS_DUR_FUA, 1, 1, 0);
        g_policy = EXITOS_DUR_FLUSH;
        T_EQ(lc_register_fd(c, TEST_FD), 0,
             "old FUA-policy completion needs no FLUSH before replacement");
        T_EQ(lc_ctx_is_poisoned(c), 0,
             "normal registration and internal cleanup do not poison context");
        T_EQ(g_flush_calls, 0, "new FLUSH policy is not applied retroactively");
        T_EQ(c->r[0]->kernel_holds, 1, "successful replacement migrates kernel debt");
        T_EQ(c->r[0]->unflushed, 0, "new registration starts without old paid debt");
        drop_fake(c);
        close(TEST_FD);
        unlink(tmp2);
    }

    /* 2. Failed dup2/dup3 did not rebind the target, so its exact registration
     * and both debts must remain live. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 1, 1, 0);
    g_kernel_dup_rc = -1;
    g_kernel_dup_errno = EBADF;
    errno = 0;
    T_EQ(lc_preload_dup2(-1, TEST_FD), -1, "failed dup2 is returned to caller");
    T_EQ(errno, EBADF, "failed dup2 preserves libc errno");
    T_EQ(c->n, 1, "failed dup2 preserves target registration");
    T_EQ(c->r[0]->kernel_holds, 1, "failed dup2 preserves kernel debt");
    T_EQ(c->r[0]->unflushed, 1, "failed dup2 preserves raw debt");
    T_EQ(g_flush_calls, 1,
         "dup2 temporarily settles raw debt while target identity is stable");
    drop_fake(c);

    /* Reentrant mutation is treated as an asynchronous signal, not as a
     * harmless recursive call.  Only the exact fd marked by iopath may pass,
     * and a signal nested inside that one real call is detected by a second
     * TLS state bit. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_in_hook = 1;
    (void)lc_preload_pwrite(TEST_FD, buf, BS, 0);
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "signal-style reentrant pwrite on registered fd poisons context");
    T_EQ(lc_preload_fdatasync(TEST_FD), 0,
         "sync after signal poison succeeds through the kernel path");
    T_EQ(g_flush_calls, 1,
         "sync after poison still pays pre-existing raw debt first");
    T_EQ(g_kernel_fdatasync_calls, 1,
         "poisoned context never answers sync locally");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_in_hook = 1;
    errno = 0;
    T_EQ(lc_preload_fdatasync(TEST_FD), -1,
         "signal-style reentrant fdatasync fails closed");
    T_EQ(errno, EIO, "reentrant fdatasync reports EIO, not false durability");
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "reentrant fdatasync permanently poisons takeover state");
    T_EQ(g_kernel_fdatasync_calls, 0,
         "reentrant fdatasync does not return a misleading kernel success");
    T_EQ(lc_preload_fdatasync(TEST_FD), 0,
         "later ordinary fdatasync succeeds after poison");
    T_EQ(g_flush_calls, 1,
         "later ordinary fdatasync pays pre-existing raw debt");
    T_EQ(g_kernel_fdatasync_calls, 1,
         "later ordinary fdatasync also reaches the kernel");
    drop_fake(c);

    reset_all();
    c = fake_ctx_fd(SOURCE_FD, EXITOS_DUR_FLUSH, 0, 1, 0);
    g_in_hook = 1;
    errno = 0;
    T_EQ(lc_preload_fdatasync(TEST_FD), -1,
         "reentrant fdatasync on another fd fails closed");
    T_EQ(errno, EIO, "other-fd reentrant fdatasync reports EIO");
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "other-fd reentrant fdatasync poisons the context");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_in_hook = 1;
    errno = 0;
    T_EQ(lc_preload_fsync(TEST_FD), -1,
         "signal-style reentrant fsync fails closed");
    T_EQ(errno, EIO, "reentrant fsync reports EIO, not false durability");
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "reentrant fsync permanently poisons takeover state");
    T_EQ(g_kernel_fsync_calls, 0,
         "reentrant fsync does not return a misleading kernel success");
    T_EQ(lc_preload_fsync(TEST_FD), 0,
         "later ordinary fsync succeeds after poison");
    T_EQ(g_flush_calls, 1, "later ordinary fsync pays raw debt first");
    T_EQ(g_kernel_fsync_calls, 1, "later ordinary fsync reaches the kernel");
    drop_fake(c);

    reset_all();
    c = fake_ctx_fd(SOURCE_FD, EXITOS_DUR_NONE, 0, 0, 0);
    g_in_hook = 1;
    (void)lc_preload_close(TEST_FD);
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "reentrant mutation of another fd poisons registered context");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_NONE, 0, 0, 0);
    g_kernel_fcntl_rc = 0;
    g_in_hook = 1;
    (void)lc_preload_fcntl(TEST_FD, F_SETFL, O_NONBLOCK);
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "signal-style reentrant F_SETFL poisons context");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_NONE, 0, 0, 0);
    exitos_internal_fd_enter(899);
    g_in_hook = 1;
    (void)lc_preload_pwrite(899, buf, BS, 0);
    g_in_hook = 0;
    exitos_internal_fd_leave(899);
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "signal before an old exact-fd wrapper cannot steal its marker");
    drop_fake(c);

#ifndef EXITOS_TEST_OLD_MARKER_RED
    reset_all();
    c = fake_ctx(EXITOS_DUR_NONE, 0, 0, 0);
    g_in_hook = 1;
    T_EQ(exitos_internal_pwrite_call(900, buf, BS, 0), BS,
         "direct internal pwrite reaches resolved libc without wrapper");
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 0,
         "normal direct internal pwrite does not poison context");
    g_in_hook = 1;
    g_kernel_pwrite_reentry = signal_write_during_internal_open;
    T_EQ(exitos_internal_pwrite_call(900, buf, BS, 0), BS,
         "direct internal pwrite completes around injected signal write");
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "signal during direct internal pwrite still poisons context");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_NONE, 0, 0, 0);
    g_in_hook = 1;
    T_EQ(exitos_internal_fdatasync_call(904), 0,
         "direct internal fdatasync reaches resolved libc without wrapper");
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 0,
         "normal direct internal fdatasync does not poison context");
    g_in_hook = 1;
    g_kernel_fdatasync_reentry = signal_sync_during_internal_call;
    T_EQ(exitos_internal_fdatasync_call(904), 0,
         "direct internal fdatasync completes around injected signal sync");
    g_in_hook = 0;
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "signal during direct internal fdatasync still poisons context");
    drop_fake(c);
#endif

    reset_all();
    {
        FILE *stream = tmpfile();
        T_OK(stream != NULL, "created freopen lifecycle stream");
        if (stream) {
            int stream_fd = fileno(stream);
            c = fake_ctx_fd(stream_fd, EXITOS_DUR_FLUSH, 0, 1, 0);
            g_kernel_freopen_result = stream;
            T_EQ((uintptr_t)lc_preload_freopen("ignored", "w", stream),
                 (uintptr_t)stream,
                 "freopen is forwarded after global preparation");
            T_EQ(g_flush_calls, 1, "freopen pays all raw debt before rebinding");
            T_EQ(g_kernel_freopen_calls, 1, "freopen reaches libc once");
            T_EQ(lc_ctx_is_poisoned(c), 1,
                 "freopen permanently disables stale fd-number takeover");
            T_EQ((uintptr_t)lc_preload_freopen64("ignored", "w", stream),
                 (uintptr_t)stream,
                 "visible freopen64 ABI alias follows the guarded path");
            drop_fake(c);
            fclose(stream);
        }
    }

    /* Bulk descriptor mutation cannot be represented by per-fd table edits.
     * Settle all raw debt, then permanently poison this context before the
     * kernel can recycle any number in the range. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 1);
    T_EQ(lc_preload_close_range(3, 100, 0), 0,
         "close_range succeeds after global debt preparation");
    T_EQ(g_flush_calls, 1, "close_range pays every existing raw debt first");
    T_EQ(g_kernel_close_range_calls, 1, "close_range reaches libc once");
    T_EQ(lc_ctx_is_poisoned(c), 1,
         "close_range permanently poisons fd-number takeover state");
    {
        ssize_t wr = -1;
        T_EQ(lc_on_write(c, TEST_FD, buf, BS, 0, &wr), EXITOS_PASS,
             "poisoned context can never TAKEOVER again");
    }
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_flush_rc = -EIO;
    errno = 0;
    T_EQ(lc_preload_close_range(3, 100, 0), -1,
         "close_range refuses to discard unpaid raw debt");
    T_EQ(errno, EIO, "close_range exposes pre-mutation FLUSH error");
    T_EQ(g_kernel_close_range_calls, 0,
         "close_range is not called after debt preparation failure");
    T_EQ(lc_ctx_is_poisoned(c), 0,
         "failed preflight leaves the original context usable for retry");
    drop_fake(c);

    /* An inherited fd must not cross into a new image while completed raw
     * writes remain only in the device's volatile cache.  Exec is attempted
     * only after a global preflight; a failed preflight leaves both image and
     * retryable debt unchanged. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_flush_rc = -EIO;
    {
        char *const av[] = { (char *)"new-image", NULL };
        char *const ev[] = { NULL };
        errno = 0;
        T_EQ(lc_preload_execve("/new-image", av, ev), -1,
             "execve refuses to cross image boundary with unpaid raw debt");
        T_EQ(errno, EIO, "execve exposes pre-exec raw FLUSH failure");
        T_EQ(g_kernel_execve_calls, 0,
             "failed execve preflight does not enter libc");
        T_EQ(c->r[0]->unflushed, 1,
             "failed execve preflight retains retryable raw debt");
    }
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    {
        char *const av[] = { (char *)"new-image", NULL };
        char *const ev[] = { NULL };
        errno = 0;
        T_EQ(lc_preload_execveat(AT_FDCWD, "/new-image", av, ev, 0), -1,
             "execveat reaches libc only after raw debt is settled");
        T_EQ(errno, ENOENT, "failed real execveat preserves libc errno");
        T_EQ(g_flush_calls, 1, "execveat pays raw debt before real call");
        T_EQ(g_kernel_execveat_calls, 1, "execveat enters libc exactly once");
        T_EQ(c->r[0]->unflushed, 0,
             "successful preflight clears raw debt even when exec itself fails");
        T_EQ(lc_ctx_is_poisoned(c), 1,
             "failed real exec cannot re-enable the old fast-path snapshot");
    }
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    {
        char *const av[] = { (char *)"fd-image", NULL };
        char *const ev[] = { NULL };
        T_EQ(lc_preload_fexecve(12, av, ev), -1,
             "fexecve shares the guarded image-boundary path");
        T_EQ(g_flush_calls, 1, "fexecve pays raw debt before real call");
        T_EQ(g_kernel_fexecve_calls, 1, "fexecve enters libc exactly once");
    }
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_in_hook = 1;
    {
        char *const av[] = { (char *)"signal-image", NULL };
        char *const ev[] = { NULL };
        errno = 0;
        T_EQ(lc_preload_execve("/signal-image", av, ev), -1,
             "signal-style reentrant execve fails closed");
        T_EQ(errno, EIO, "reentrant execve reports fail-closed errno");
        T_EQ(g_kernel_execve_calls, 0,
             "reentrant execve cannot cross the interrupted transaction");
        T_EQ(lc_ctx_is_poisoned(c), 1,
             "reentrant execve permanently poisons takeover");
    }
    g_in_hook = 0;
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_NONE, 0, 0, 0);
    {
        struct iovec iov = { .iov_base = buf, .iov_len = BS };
        T_EQ(lc_preload_pwritev2(TEST_FD, &iov, 1, 0, RWF_DSYNC), 17,
             "pwritev2 PASS preserves the real libc result");
        T_EQ(g_kernel_pwritev2_calls, 1,
             "pwritev2 executes the real syscall exactly once");
        T_EQ(c->r[0]->kernel_holds, 1,
             "pwritev2 publishes kernel debt before leaving its transaction");
    }
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 1, 1, 0);
    g_kernel_dup_rc = -1;
    g_kernel_dup_errno = EINVAL;
    T_EQ(lc_preload_dup3(-1, TEST_FD, O_CLOEXEC), -1,
         "failed dup3 is returned to caller");
    T_EQ(c->n, 1, "failed dup3 preserves target registration and debt");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_flush_rc = -EIO;
    errno = 0;
    T_EQ(lc_preload_dup2(9, TEST_FD), -1,
         "dup2 refuses to discard target raw debt when pre-rebind FLUSH fails");
    T_EQ(errno, EIO, "dup2 exposes pre-rebind FLUSH failure as libc errno");
    T_EQ(g_kernel_dup_calls, 0,
         "dup2 does not rebind the target after durability preparation fails");
    T_EQ(c->n, 1, "failed durability preparation retains target registration");
    T_EQ(c->r[0]->unflushed, 1,
         "failed durability preparation retains retryable raw debt");
    drop_fake(c);

    reset_all();
    c = fake_ctx_two(SOURCE_FD, TEST_FD, 1);
    g_flush_fail_call = 2;
    errno = 0;
    T_EQ(lc_preload_dup2(SOURCE_FD, TEST_FD), -1,
         "dup2 stops when the target's second FLUSH fails");
    T_EQ(errno, EIO, "second FLUSH failure is returned through libc ABI");
    T_EQ(g_flush_calls, 2, "source then target debt are prepared in order");
    T_EQ(g_kernel_dup_calls, 0,
         "second preparation failure prevents descriptor replacement");
    T_EQ(c->n, 2, "second FLUSH failure preserves both registrations");
    T_EQ(c->r[0]->unflushed, 1,
         "second FLUSH failure restores source raw-debt state");
    T_EQ(c->r[1]->unflushed, 1,
         "second FLUSH failure keeps target raw debt retryable");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    T_EQ(lc_preload_dup2(9, TEST_FD), TEST_FD,
         "dup2 succeeds after target raw debt is settled");
    T_EQ(g_flush_calls, 1, "successful dup2 pays target raw debt once");
    T_EQ(g_kernel_dup_calls, 1, "successful dup2 executes one real syscall");
    T_EQ(c->n, 0, "successful dup2 removes overwritten target registration");
    drop_fake(c);

    reset_all();
    c = fake_ctx_two(SOURCE_FD, TEST_FD, 0);
    if (c) {
        pthread_t a_thread, b_thread;
        struct call_arg a = { .c = c, .oldfd = SOURCE_FD, .newfd = TEST_FD };
        struct call_arg b = { .c = c, .oldfd = TEST_FD, .newfd = SOURCE_FD };
        g_block_kernel_dup = 1;
        pthread_create(&a_thread, NULL, preload_duper, &a);
        T_OK(wait_flag(&g_kernel_dup_entered, 1000),
             "cross-dup first transaction reached entered barrier");
        pthread_create(&b_thread, NULL, preload_duper, &b);
        T_EQ(wait_flag(&b.done, 200), 0,
             "opposite dup waits instead of deadlocking on reverse lock order");
        pthread_mutex_lock(&g_gate_mu);
        g_release_kernel_dup = 1;
        pthread_cond_broadcast(&g_gate_cv);
        pthread_mutex_unlock(&g_gate_mu);
        pthread_join(a_thread, NULL);
        pthread_join(b_thread, NULL);
        T_OK(a.done && b.done, "both crossing dup2 calls complete");
        T_EQ(a.rc, TEST_FD, "first crossing dup2 returns target fd");
        T_EQ(b.rc, SOURCE_FD, "second crossing dup2 returns target fd");
        drop_fake(c);
    }

    /* A new descriptor aliases the source open-file description. The minimal
     * safe policy is to settle source raw debt and disable its fast path before
     * exposing an unregistered alias; otherwise fdatasync(newfd) could report
     * success while a later raw write(oldfd) remains volatile. */
    reset_all();
    c = fake_ctx_fd(SOURCE_FD, EXITOS_DUR_FLUSH, 0, 1, 1);
    T_EQ(lc_preload_dup2(SOURCE_FD, TEST_FD), TEST_FD,
         "dup2 creates an alias of a registered source");
    T_EQ(g_flush_calls, 1, "alias creation settles source raw debt");
    T_EQ(c->n, 0,
         "successful alias creation disables the registered source fast path");
    {
        ssize_t wr = -1;
        T_EQ(lc_on_write(c, SOURCE_FD, buf, BS, 0, &wr), EXITOS_PASS,
             "old source fd cannot create untracked raw debt after alias creation");
    }
    T_EQ(lc_preload_fdatasync(SOURCE_FD), 0,
         "fdatasync on old alias uses the real kernel path");
    T_EQ(lc_preload_fdatasync(TEST_FD), 0,
         "fdatasync on new alias uses the real kernel path");
    T_EQ(g_kernel_fdatasync_calls, 2,
         "neither alias can falsely answer durability after fast-path disable");
    drop_fake(c);

    reset_all();
    c = fake_ctx_fd(SOURCE_FD, EXITOS_DUR_FLUSH, 0, 1, 0);
    g_kernel_dup_rc = -1;
    g_kernel_dup_errno = EMFILE;
    T_EQ(lc_preload_dup2(SOURCE_FD, TEST_FD), -1,
         "failed alias creation returns kernel error");
    T_EQ(g_flush_calls, 1,
         "failed alias creation prepared source while its identity was stable");
    T_EQ(c->n, 1, "failed alias creation preserves source registration");
    T_EQ(c->r[0]->unflushed, 1,
         "failed alias creation restores source raw-debt state");
    drop_fake(c);

    /* F_SETFL can change the open-file-description assumptions after
     * registration.  Pay raw debt before the real mutation; only a successful
     * mutation permanently disables takeover. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_kernel_fcntl_rc = 0;
    T_EQ(lc_preload_fcntl(TEST_FD, F_SETFL, O_NONBLOCK), 0,
         "successful F_SETFL is forwarded");
    T_EQ(g_flush_calls, 1, "F_SETFL pays raw debt before changing flags");
    T_EQ(c->n, 0, "successful F_SETFL permanently disables the fast path");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_kernel_fcntl_rc = -1;
    g_kernel_fcntl_errno = EINVAL;
    errno = 0;
    T_EQ(lc_preload_fcntl(TEST_FD, F_SETFL, O_NONBLOCK), -1,
         "failed F_SETFL is returned");
    T_EQ(errno, EINVAL, "failed F_SETFL preserves libc errno");
    T_EQ(g_flush_calls, 1, "failed F_SETFL prepared debt while fd was stable");
    T_EQ(c->n, 1, "failed F_SETFL retains registration");
    T_EQ(c->r[0]->unflushed, 1, "failed F_SETFL restores raw debt state");
    drop_fake(c);

    reset_all();
    c = fake_ctx_fd(SOURCE_FD, EXITOS_DUR_FLUSH, 0, 1, 0);
    T_EQ(lc_preload_dup(SOURCE_FD), TEST_FD,
         "plain dup creates an alias of a registered source");
    T_EQ(g_flush_calls, 1, "plain dup settles source raw debt");
    T_EQ(c->n, 0, "plain dup disables the source fast path");
    drop_fake(c);

    reset_all();
    c = fake_ctx_fd(SOURCE_FD, EXITOS_DUR_FLUSH, 0, 1, 0);
    T_EQ(lc_preload_fcntl(SOURCE_FD, F_DUPFD, 100), TEST_FD,
         "fcntl(F_DUPFD) creates an alias through the guarded path");
    T_EQ(g_kernel_fcntl_calls, 1, "fcntl alias command reaches libc once");
    T_EQ(g_kernel_fcntl_cmd, F_DUPFD, "fcntl preserves alias command");
    T_EQ(g_kernel_fcntl_arg, 100, "fcntl preserves minimum-fd argument");
    T_EQ(g_flush_calls, 1, "fcntl alias settles source raw debt");
    T_EQ(c->n, 0, "fcntl alias disables the source fast path");
    drop_fake(c);

#ifdef F_DUPFD_CLOEXEC
    reset_all();
    c = fake_ctx_fd(SOURCE_FD, EXITOS_DUR_FLUSH, 0, 1, 0);
    T_EQ(lc_preload_fcntl(SOURCE_FD, F_DUPFD_CLOEXEC, 100), TEST_FD,
         "fcntl(F_DUPFD_CLOEXEC) creates an alias through the guarded path");
    T_EQ(g_flush_calls, 1,
         "fcntl(F_DUPFD_CLOEXEC) settles source raw debt");
    T_EQ(c->n, 0,
         "fcntl(F_DUPFD_CLOEXEC) disables the source fast path");
    drop_fake(c);
#endif

    reset_all();
    c = fake_ctx_fd(SOURCE_FD, EXITOS_DUR_FLUSH, 0, 1, 0);
    g_kernel_fcntl_rc = O_RDWR;
    T_EQ(lc_preload_fcntl(SOURCE_FD, F_GETFL), O_RDWR,
         "non-alias no-argument fcntl is forwarded with its ABI intact");
    T_EQ(g_kernel_fcntl_calls, 1, "non-alias fcntl reaches libc once");
    T_EQ(g_flush_calls, 0, "non-alias fcntl does not alter raw debt");
    T_EQ(c->n, 1, "non-alias fcntl retains source registration");
    drop_fake(c);

    /* Linux close releases the fd before late EINTR/EIO reporting. EBADF means
     * the registration was stale already. Neither error may leave an old map
     * attached to a number that can be reused. A pre-close raw FLUSH failure is
     * different: the syscall has not run, so retaining the registration is safe. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_kernel_close_rc = -1;
    g_kernel_close_errno = EBADF;
    errno = 0;
    T_EQ(lc_preload_close(TEST_FD), -1, "close propagates EBADF");
    T_EQ(errno, EBADF, "close preserves EBADF after lifecycle cleanup");
    T_EQ(g_flush_calls, 1, "close settles target raw debt before losing sync entry");
    T_EQ(c->n, 0, "EBADF drops a stale target registration");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_kernel_close_rc = -1;
    g_kernel_close_errno = EIO;
    errno = 0;
    T_EQ(lc_preload_close(TEST_FD), -1, "close propagates late EIO");
    T_EQ(errno, EIO, "close preserves late EIO after lifecycle cleanup");
    T_EQ(c->n, 0,
         "Linux late close error still removes released-fd registration");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_kernel_close_rc = -1;
    g_kernel_close_errno = EINTR;
    errno = 0;
    T_EQ(lc_preload_close(TEST_FD), -1, "close propagates EINTR");
    T_EQ(errno, EINTR, "close preserves EINTR after lifecycle cleanup");
    T_EQ(c->n, 0,
         "Linux EINTR close still removes released-fd registration");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_flush_rc = -EREMOTEIO;
    errno = 0;
    T_EQ(lc_preload_close(TEST_FD), -1,
         "close fails before syscall when raw debt cannot be settled");
    T_EQ(errno, EREMOTEIO, "pre-close FLUSH error is exposed through libc ABI");
    T_EQ(g_kernel_close_calls, 0,
         "pre-close durability failure leaves the real close uncalled");
    T_EQ(c->n, 1, "pre-close durability failure retains valid registration");
    T_EQ(c->r[0]->unflushed, 1,
         "pre-close durability failure leaves raw debt retryable");
    drop_fake(c);

    /* Extent-releasing metadata cannot run while old raw data is merely in the
     * device's volatile cache.  Flush before invalidating or entering libc. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 1);
    g_flush_rc = -EREMOTEIO;
    errno = 0;
    T_EQ(lc_preload_ftruncate(TEST_FD, 0), -1,
         "ftruncate fails before extent release when raw FLUSH fails");
    T_EQ(errno, EREMOTEIO, "ftruncate exposes preflight FLUSH errno");
    T_EQ(g_kernel_ftruncate_calls, 0,
         "failed preflight never calls real ftruncate");
    T_EQ(c->r[0]->unflushed, 1, "failed preflight retains raw debt");
    {
        uint64_t lba = 0, contig = 0;
        T_EQ(maco_lookup(c->r[0]->m, 0, &lba, &contig), 0,
             "failed preflight retains the old mapping for retry");
    }
    g_flush_rc = 0;
    T_EQ(lc_preload_ftruncate(TEST_FD, 0), 0,
         "ftruncate retries after raw FLUSH recovers");
    T_EQ(g_flush_calls, 2, "retry pays raw debt before real ftruncate");
    T_EQ(g_kernel_ftruncate_calls, 1, "real ftruncate runs exactly once");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 1);
    g_flush_rc = -EIO;
    errno = 0;
    T_EQ(lc_preload_ftruncate64(TEST_FD, 0), -1,
         "ftruncate64 shares the raw-debt preflight transaction");
    T_EQ(errno, EIO, "ftruncate64 preserves preflight FLUSH errno");
    T_EQ(g_kernel_ftruncate_calls, 0,
         "ftruncate64 does not bypass failed raw-debt preflight");
    g_flush_rc = 0;
    T_EQ(lc_preload_ftruncate64(TEST_FD, 0), 0,
         "ftruncate64 retries after raw FLUSH recovers");
    T_EQ(g_kernel_ftruncate_calls, 1,
         "ftruncate64 invokes its underlying ABI exactly once");
    drop_fake(c);

    /* fallocate can allocate, free, zero or move extents depending on flags.
     * Invalidate the complete snapshot before every submitted operation; a
     * later miss can safely refresh it. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 1);
    g_flush_rc = -EIO;
    errno = 0;
    T_EQ(lc_preload_fallocate(TEST_FD, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                              0, BS), -1,
         "fallocate refuses extent mutation when raw FLUSH fails");
    T_EQ(errno, EIO, "fallocate exposes raw FLUSH failure");
    T_EQ(g_kernel_fallocate_calls, 0,
         "failed fallocate preflight does not enter libc");
    T_OK(strstr(g_prepare_outcome_line,
                "outcome=control-failed stage=flush rc=-5 return_rc=-1 "
                "return_errno=5 "
                "rdwr_alias=0") != NULL,
         "preload logs a control failure even when preparation never begins: %s",
         g_prepare_outcome_line);
    {
        uint64_t lba = 0, contig = 0;
        T_EQ(maco_lookup(c->r[0]->m, 0, &lba, &contig), 0,
             "failed fallocate preflight preserves mapping and debt");
    }
    g_flush_rc = 0;
    T_EQ(lc_preload_fallocate(TEST_FD, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                              0, BS), 0,
         "fallocate proceeds after raw FLUSH succeeds");
    T_EQ(g_kernel_fallocate_calls, 1, "real fallocate runs exactly once");
    T_EQ(g_kernel_fallocate_mode, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
         "fallocate preserves mode flags");
    T_EQ(g_kernel_fallocate_off, 0, "fallocate preserves offset");
    T_EQ(g_kernel_fallocate_len, BS, "fallocate preserves length");
    T_OK(strstr(g_prepare_outcome_line,
                "outcome=native-skip stage=native rc=0 return_rc=0 "
                "return_errno=0 "
                "rdwr_alias=0") != NULL,
         "preload logs every ordinary intercepted fallocate as native-skip: %s",
         g_prepare_outcome_line);
    {
        uint64_t lba = 0, contig = 0;
        T_OK(maco_lookup(c->r[0]->m, 0, &lba, &contig) != 0,
             "submitted fallocate leaves no stale mapping");
    }
    T_EQ(c->r[0]->kernel_holds, 1,
         "successful fallocate creates metadata/kernel debt");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_NONE, 0, 0, 1);
    g_kernel_fallocate_rc = -1;
    g_kernel_fallocate_errno = ENOSPC;
    errno = 0;
    T_EQ(lc_preload_fallocate64(TEST_FD, 0, BS, BS), -1,
         "fallocate64 forwards real failure");
    T_EQ(errno, ENOSPC, "fallocate64 preserves libc errno");
    T_EQ(g_kernel_fallocate_calls, 1,
         "fallocate64 invokes the underlying operation exactly once");
    {
        uint64_t lba = 0, contig = 0;
        T_OK(maco_lookup(c->r[0]->m, 0, &lba, &contig) != 0,
             "failed submitted fallocate remains conservatively invalidated");
    }
    T_EQ(c->r[0]->kernel_holds, 0,
         "failed real fallocate creates no successful metadata debt");
    drop_fake(c);

    /* The frontend is only an ABI adapter now.  Setup-time replacement and
     * lifecycle ordering live in the shared runtime tested separately. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_NONE, 0, 0, 1);
    g_frontend_fallocate_takeover = 1;
    g_fallocate_runtime =
        (struct exitos_frontend_fallocate_runtime *)(uintptr_t)0x7777;
    T_EQ(lc_preload_fallocate(TEST_FD, 0, 0, 2 * BS), 0,
         "preload exposes successful donor preparation as fallocate success");
    T_EQ(g_frontend_fallocate_exec_calls, 1,
         "preload delegates fallocate exactly once to shared runtime");
    T_EQ(g_kernel_fallocate_calls, 0,
         "donor takeover skips the libc fallocate operation");
    T_OK(g_frontend_fallocate_ctl_depth > 0,
         "preload donor control I/O is protected by control-depth recursion guard");
    T_OK(strstr(g_prepare_outcome_line,
                "PREPARE_OUTCOME v=1 frontend=ld_preload") != NULL,
         "preload emits the versioned fallocate outcome schema");
    T_OK(strstr(g_prepare_outcome_line,
                "outcome=prepared stage=complete rc=0 return_rc=0 "
                "return_errno=0 "
                "rdwr_alias=1") != NULL,
         "preload outcome records successful alias-backed preparation: %s",
         g_prepare_outcome_line);
    T_OK(strstr(g_prepare_ready_line,
                "fiemap=safe unsafe_flags=0") != NULL,
         "preload readiness marker carries explicit safe-FIEMAP proof fields");
    g_frontend_fallocate_rc = -EREMOTEIO;
    g_prepare_outcome_line[0] = '\0';
    g_prepare_ready_line[0] = '\0';
    errno = 0;
    T_EQ(lc_preload_fallocate64(TEST_FD, 0, 0, 2 * BS), -1,
         "preload converts shared negative errno to libc failure");
    T_EQ(errno, EREMOTEIO,
         "preload preserves shared donor failure errno");
    T_EQ(g_kernel_fallocate_calls, 0,
         "configured donor failure remains fail closed in preload adapter");
    T_OK(strstr(g_prepare_outcome_line,
                "outcome=eligible-attempt-failed stage=donor-prepare "
                "rc=-121 return_rc=-1 return_errno=121 rdwr_alias=1") != NULL,
         "preload logs failed eligible setup with stable stage and errno: %s",
         g_prepare_outcome_line);
    T_EQ(g_prepare_ready_line[0], '\0',
         "preload failure emits no PREPARE_READY claim");
    g_fallocate_runtime = NULL;
    drop_fake(c);

    reset_all();
    T_EQ(exitos_internal_ftruncate_call(TEST_FD, BS), 0,
         "preload internal ftruncate seam reaches resolved libc directly");
    T_EQ(g_kernel_ftruncate_calls, 1,
         "preload internal ftruncate seam invokes libc exactly once");
    T_EQ(exitos_internal_fallocate_call(TEST_FD, 0, 0, BS), 0,
         "preload internal fallocate seam reaches resolved libc directly");
    T_EQ(g_kernel_fallocate_calls, 1,
         "preload internal fallocate seam invokes libc exactly once");

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 1);
    g_fastpath = 0;
    {
        struct iovec iov = { .iov_base = buf, .iov_len = BS };
        T_EQ(lc_preload_write(TEST_FD, buf, BS), BS,
             "FASTPATH=0 sends write directly to libc");
        T_EQ(lc_preload_pwrite(TEST_FD, buf, BS, 0), BS,
             "FASTPATH=0 sends pwrite directly to libc");
        T_EQ(lc_preload_writev(TEST_FD, &iov, 1), BS,
             "FASTPATH=0 sends writev directly to libc");
        T_EQ(lc_preload_pwritev(TEST_FD, &iov, 1, 0), BS,
             "FASTPATH=0 sends pwritev directly to libc");
    }
    T_EQ(lc_preload_fdatasync(TEST_FD), 0,
         "FASTPATH=0 sends fdatasync directly to libc");
    T_EQ(lc_preload_fsync(TEST_FD), 0,
         "FASTPATH=0 sends fsync directly to libc");
    T_EQ(g_kernel_write_calls, 1, "FASTPATH=0 executes one kernel write");
    T_EQ(g_kernel_pwrite_calls, 1, "FASTPATH=0 executes one kernel pwrite");
    T_EQ(g_kernel_writev_calls, 2,
         "FASTPATH=0 executes writev and the pwritev mock transfer");
    T_EQ(g_kernel_pwritev_calls, 1, "FASTPATH=0 executes one kernel pwritev");
    T_EQ(g_kernel_fdatasync_calls, 1,
         "FASTPATH=0 executes one kernel fdatasync");
    T_EQ(g_kernel_fsync_calls, 1, "FASTPATH=0 executes one kernel fsync");
    T_EQ(c->r[0]->passed, 0,
         "FASTPATH=0 does not increment per-registration PASS accounting");
    T_EQ(c->r[0]->kernel_holds, 0,
         "FASTPATH=0 does not mutate fast-path durability accounting");
    T_EQ(g_flush_calls, 0,
         "FASTPATH=0 sync calls do not enter raw fast-path lifecycle");
    drop_fake(c);

    /* 3. ftruncate is filesystem metadata work.  Only a successful real call
     * creates kernel debt; the next fdatasync must therefore be real. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_NONE, 0, 0, 0);
    T_EQ(lc_preload_ftruncate(TEST_FD, BS), 0, "successful real ftruncate returns 0");
    T_EQ(c->r[0]->kernel_holds, 1, "successful ftruncate creates metadata debt");
    T_EQ(c->r[0]->passed, 0,
         "ftruncate metadata debt is not miscounted as a passed data write");
    T_EQ(lc_preload_fdatasync(TEST_FD), 0, "fdatasync after ftruncate succeeds");
    T_EQ(g_kernel_fdatasync_calls, 1,
         "fdatasync after successful ftruncate reaches the real syscall");
    T_EQ(c->r[0]->kernel_holds, 0,
         "only successful real fdatasync clears ftruncate debt");
    drop_fake(c);

    reset_all();
    c = fake_ctx(EXITOS_DUR_NONE, 0, 0, 0);
    g_kernel_ftruncate_rc = -1;
    g_kernel_ftruncate_errno = EINVAL;
    T_EQ(lc_preload_ftruncate(TEST_FD, -1), -1, "failed real ftruncate is returned");
    T_EQ(c->r[0]->kernel_holds, 0, "failed ftruncate creates no metadata debt");
    drop_fake(c);

    /* 4a. A flush racing a raw write must linearize after that write, not inspect
     * unflushed before completion and return while the write later creates debt. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 0, 1);
    if (c) {
        pthread_t wt, ft;
        struct call_arg wa = { .c = c, .buf = buf };
        struct call_arg fa = { .c = c };
        g_block_raw_write = 1;
        pthread_create(&wt, NULL, raw_writer, &wa);
        T_OK(wait_flag(&g_raw_entered, 1000), "raw writer reached the blocked iopath");
        pthread_create(&ft, NULL, raw_flusher, &fa);
        T_EQ(wait_flag(&fa.done, 200), 0,
             "concurrent raw flush waits for the in-flight raw write transaction");
        pthread_mutex_lock(&g_gate_mu);
        g_release_raw = 1;
        pthread_cond_broadcast(&g_gate_cv);
        pthread_mutex_unlock(&g_gate_mu);
        pthread_join(wt, NULL);
        pthread_join(ft, NULL);
        T_EQ(fa.rc, 0, "flush after concurrent raw write succeeds");
        T_EQ(g_flush_calls, 1, "flush observes and pays the completed raw write debt");
        T_EQ(c->r[0]->unflushed, 0, "no raw debt is lost by write/flush race");
        drop_fake(c);
    }

    /* 4b. PASS decision, real write, and completion bookkeeping form one
     * transaction.  A real fdatasync cannot slip between decision and syscall. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_NONE, 0, 0, 1);
    if (c) {
        pthread_t wt, st;
        struct call_arg wa = { .c = c, .buf = buf };
        struct call_arg sa = { .c = c };
        g_block_kernel_pwrite = 1;
        pthread_create(&wt, NULL, preload_pwriter, &wa);
        T_OK(wait_flag(&g_kernel_write_entered, 1000),
             "PASS writer reached the blocked real pwrite");
        pthread_create(&st, NULL, preload_syncer, &sa);
        T_EQ(wait_flag(&sa.done, 200), 0,
             "real fdatasync cannot clear debt before PASS write syscall finishes");
        pthread_mutex_lock(&g_gate_mu);
        g_release_kernel_write = 1;
        pthread_cond_broadcast(&g_gate_cv);
        pthread_mutex_unlock(&g_gate_mu);
        pthread_join(wt, NULL);
        pthread_join(st, NULL);
        T_EQ(g_kernel_pwrite_calls, 1, "PASS path executed one real pwrite");
        T_EQ(g_kernel_fdatasync_calls, 1,
             "serialized sync executed after the real pwrite");
        T_EQ(c->r[0]->kernel_holds, 0,
             "post-write real sync clears debt only after persisting it");
        drop_fake(c);
    }

    /* 4c. Unregister must wait for an in-flight write before closing/freeing the
     * stable registration object. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 0, 1);
    if (c) {
        pthread_t wt, ut;
        struct call_arg wa = { .c = c, .buf = buf };
        struct call_arg ua = { .c = c };
        g_block_raw_write = 1;
        pthread_create(&wt, NULL, raw_writer, &wa);
        T_OK(wait_flag(&g_raw_entered, 1000),
             "write/unregister writer reached blocked iopath");
        pthread_create(&ut, NULL, unregisterer, &ua);
        T_EQ(wait_flag(&ua.done, 200), 0,
             "unregister waits for the in-flight write");
        T_EQ(g_close_calls, 0, "iopath is not closed while write is using it");
        pthread_mutex_lock(&g_gate_mu);
        g_release_raw = 1;
        pthread_cond_broadcast(&g_gate_cv);
        pthread_mutex_unlock(&g_gate_mu);
        pthread_join(wt, NULL);
        pthread_join(ut, NULL);
        T_EQ(ua.rc, 0, "unregister succeeds after writer quiesces");
        T_EQ(g_flush_calls, 1,
             "unregister pays the completed raw write debt before removal");
        T_EQ(g_close_calls, 1, "iopath closes exactly once after writer exits");
        T_EQ(c->n, 0, "registration is absent after serialized unregister");
        drop_fake(c);
    }

    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    if (c) {
        g_flush_rc = -EIO;
        T_EQ(lc_unregister_fd(c, TEST_FD), -EIO,
             "failed unregister FLUSH reports its device error");
        T_EQ(c->n, 1, "failed unregister retains the registration");
        T_EQ(c->r[0]->unflushed, 1,
             "failed unregister retains retryable raw debt");
        T_EQ(g_close_calls, 0,
             "failed unregister does not close an iopath that still owns debt");
        drop_fake(c);
    }

    /* Destroy has no error return, but it must at least make a best-effort raw
     * FLUSH before it releases the only iopath capable of paying that debt. */
    reset_all();
    c = fake_ctx(EXITOS_DUR_FLUSH, 0, 1, 0);
    g_ctx = NULL;
    lc_ctx_destroy(c);
    T_EQ(g_flush_calls, 1,
         "ctx destroy attempts to pay raw debt before releasing iopath");
    T_EQ(g_close_calls, 1,
         "ctx destroy closes iopath only after its best-effort FLUSH");

    free(buf);
    T_DONE();
}
