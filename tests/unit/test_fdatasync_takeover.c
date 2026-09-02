/* Sync-debt state machine, with no block device.
 *
 * A registered descriptor can owe durability to two independent owners:
 *
 *   kernel_holds  a write went through the filesystem and needs a real
 *                 fdatasync/fsync;
 *   unflushed     a raw write completed into a volatile device cache and
 *                 needs iopath_flush().
 *
 * The old implementation conflated them: after a raw FLUSH failed on a mixed
 * descriptor it passed fdatasync to the kernel, then note_kernel_sync() cleared
 * BOTH flags.  A successful filesystem sync thereby hid a failed raw flush and
 * returned success to the application.  These tests include the real core and
 * LD_PRELOAD wrapper under private names, replacing only the device FLUSH and
 * real libc sync calls.  No device path is opened and no I/O is submitted. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include "tap.h"
#include "exitos_durability.h"
#include "exitos_intercept.h"
#include "exitos_iopath.h"

static int g_flush_rc[8];
static int g_flush_nr;
static int g_flush_calls;
static int g_close_calls;

/* Structural performance oracle for the registration table.  A read lock is
 * logically shared, but every rdlock/unlock still modifies the same rwlock
 * cache line.  Recording the lock address lets the test distinguish a real
 * fd-sharded table from a single process-wide lock without relying on a noisy
 * wall-clock threshold. */
static pthread_rwlock_t *g_rdlock_seen[8];
static pthread_rwlock_t *g_wrlock_seen[128];
static int g_rdlock_seen_n;
static int g_wrlock_seen_n;
static int g_record_table_locks;

static int record_rwlock_rdlock(pthread_rwlock_t *rw)
{
    if (g_record_table_locks && g_rdlock_seen_n < (int)(sizeof g_rdlock_seen /
                                                        sizeof g_rdlock_seen[0]))
        g_rdlock_seen[g_rdlock_seen_n++] = rw;
    return (pthread_rwlock_rdlock)(rw);
}

static int record_rwlock_wrlock(pthread_rwlock_t *rw)
{
    if (g_record_table_locks && g_wrlock_seen_n < (int)(sizeof g_wrlock_seen /
                                                        sizeof g_wrlock_seen[0]))
        g_wrlock_seen[g_wrlock_seen_n++] = rw;
    return (pthread_rwlock_wrlock)(rw);
}

static int mock_iopath_flush(struct iopath *p)
{
    int i = g_flush_calls++;
    (void)p;
    return i < g_flush_nr ? g_flush_rc[i] : 0;
}

static void mock_iopath_close(struct iopath *p)
{
    (void)p;
    g_close_calls++;
}

int wb_exitos_fdatasync_needed(int durability_policy);

/* Include the production state machine under private names.  The opaque
 * structs become visible here so a test can construct only the four fields a
 * sync uses instead of registering a real filesystem/device pair. */
#define iopath_flush                         mock_iopath_flush
#define iopath_close                         mock_iopath_close
#define exitos_ctx_create                    wb_exitos_ctx_create
#define exitos_ctx_create_with_options       wb_exitos_ctx_create_with_options
#define exitos_ctx_destroy                   wb_exitos_ctx_destroy
#define exitos_register_fd                   wb_exitos_register_fd
#define exitos_unregister_fd                 wb_exitos_unregister_fd
#define exitos_on_write                      wb_exitos_on_write
#define exitos_on_fdatasync                  wb_exitos_on_fdatasync
#define exitos_flush_raw_debt                wb_exitos_flush_raw_debt
#define exitos_fd_all_writes_took_fast_path  wb_exitos_fd_all_writes_took_fast_path
#define exitos_on_ftruncate                  wb_exitos_on_ftruncate
#define exitos_lba_in_bounds                 wb_exitos_lba_in_bounds
#define exitos_fdatasync_needed              wb_exitos_fdatasync_needed
#define exitos_note_kernel_write             wb_exitos_note_kernel_write
#define exitos_note_kernel_sync              wb_exitos_note_kernel_sync
#define exitos_ctx_verify_identity           wb_exitos_ctx_verify_identity
#define exitos_ctx_strict                    wb_exitos_ctx_strict
#define pthread_rwlock_rdlock                 record_rwlock_rdlock
#define pthread_rwlock_wrlock                 record_rwlock_wrlock
#include "../../src/intercept.c"
#undef pthread_rwlock_wrlock
#undef pthread_rwlock_rdlock
#undef iopath_flush
#undef iopath_close
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

/* Keep the shared frontend factory on the same private white-box core.  If
 * this source instead resolves the archive's public factory, its ctx_destroy
 * dependency pulls a second production intercept.o into this executable. */
#define exitos_frontend_config_from_env       wb_frontend_config_from_env
#define exitos_frontend_config_destroy        wb_frontend_config_destroy
#define exitos_frontend_config_enabled        wb_frontend_config_enabled
#define exitos_frontend_config_files          wb_frontend_config_files
#define exitos_frontend_config_ctx            wb_frontend_config_ctx
#define exitos_frontend_config_path_selected  wb_frontend_config_path_selected
#define exitos_ctx_create_with_options         wb_exitos_ctx_create_with_options
#define exitos_ctx_destroy                     wb_exitos_ctx_destroy
#define exitos_ctx_verify_identity             wb_exitos_ctx_verify_identity
#define exitos_ctx_strict                      wb_exitos_ctx_strict
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

static int g_kernel_fdatasync_rc;
static int g_kernel_fdatasync_errno;
static int g_kernel_fdatasync_calls;
static int g_kernel_fsync_rc;
static int g_kernel_fsync_errno;
static int g_kernel_fsync_calls;

static int mock_kernel_fdatasync(int fd)
{
    (void)fd;
    g_kernel_fdatasync_calls++;
    if (g_kernel_fdatasync_rc != 0) {
        errno = g_kernel_fdatasync_errno;
        return -1;
    }
    return 0;
}

static int mock_kernel_fsync(int fd)
{
    (void)fd;
    g_kernel_fsync_calls++;
    if (g_kernel_fsync_rc != 0) {
        errno = g_kernel_fsync_errno;
        return -1;
    }
    return 0;
}

/* Include the actual LD_PRELOAD wrappers too.  Their public libc names are
 * private in this executable; the core calls are wired to the white-box copy
 * above and the two real-sync function pointers are replaced in reset(). */
#define open                                wb_preload_open
#define openat                              wb_preload_openat
#define write                               wb_preload_write
#define pwrite                              wb_preload_pwrite
#define fdatasync                           wb_preload_fdatasync
#define fsync                               wb_preload_fsync
#define ftruncate                           wb_preload_ftruncate
#define close                               wb_preload_close
#define dup2                                wb_preload_dup2
#define dup3                                wb_preload_dup3
#define dup                                 wb_preload_dup
#define fcntl                               wb_preload_fcntl
#define fcntl64                             wb_preload_fcntl64
#define fclose                              wb_preload_fclose
#define open64                              wb_preload_open64
#define openat64                            wb_preload_openat64
#define pwrite64                            wb_preload_pwrite64
#define writev                              wb_preload_writev
#define pwritev                             wb_preload_pwritev
#define exitos_ctx_create                   wb_exitos_ctx_create
#define exitos_ctx_destroy                  wb_exitos_ctx_destroy
#define exitos_register_fd                  wb_exitos_register_fd
#define exitos_unregister_fd                wb_exitos_unregister_fd
#define exitos_on_write                     wb_exitos_on_write
#define exitos_on_fdatasync                 wb_exitos_on_fdatasync
#define exitos_flush_raw_debt               wb_exitos_flush_raw_debt
#define exitos_on_ftruncate                 wb_exitos_on_ftruncate
#define exitos_note_kernel_write            wb_exitos_note_kernel_write
#define exitos_note_kernel_sync             wb_exitos_note_kernel_sync
#define exitos_ctx_verify_identity           wb_exitos_ctx_verify_identity
#define exitos_frontend_config_from_env      wb_frontend_config_from_env
#define exitos_frontend_config_destroy       wb_frontend_config_destroy
#define exitos_frontend_config_enabled       wb_frontend_config_enabled
#define exitos_frontend_config_files         wb_frontend_config_files
#define exitos_frontend_config_ctx           wb_frontend_config_ctx
#define exitos_frontend_config_path_selected wb_frontend_config_path_selected
#include "../../src/preload.c"
#undef open
#undef openat
#undef write
#undef pwrite
#undef fdatasync
#undef fsync
#undef ftruncate
#undef close
#undef dup2
#undef dup3
#undef dup
#undef fcntl
#undef fcntl64
#undef fclose
#undef open64
#undef openat64
#undef pwrite64
#undef writev
#undef pwritev
#undef exitos_ctx_create
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
#undef exitos_frontend_config_from_env
#undef exitos_frontend_config_destroy
#undef exitos_frontend_config_enabled
#undef exitos_frontend_config_files
#undef exitos_frontend_config_ctx
#undef exitos_frontend_config_path_selected

enum { TEST_FD = 37 };

static void reset_mocks(void)
{
    memset(g_flush_rc, 0, sizeof g_flush_rc);
    g_flush_nr = 0;
    g_flush_calls = 0;
    g_close_calls = 0;
    g_kernel_fdatasync_rc = 0;
    g_kernel_fdatasync_errno = 0;
    g_kernel_fdatasync_calls = 0;
    g_kernel_fsync_rc = 0;
    g_kernel_fsync_errno = 0;
    g_kernel_fsync_calls = 0;
    o_fdatasync = mock_kernel_fdatasync;
    o_fsync = mock_kernel_fsync;
    g_enabled = 1;
    g_in_hook = 0;
    exitos_frontend_admission_enable();
}

static struct exitos_ctx *sync_ctx(int policy, int kernel_debt, int raw_debt)
{
    struct exitos_ctx *c = wb_exitos_ctx_create();
    if (!c)
        return NULL;
    c->r = calloc(1, sizeof(*c->r));
    if (!c->r) {
        wb_exitos_ctx_destroy(c);
        return NULL;
    }
    c->r[0] = calloc(1, sizeof(*c->r[0]));
    if (!c->r[0]) {
        wb_exitos_ctx_destroy(c);
        return NULL;
    }
    c->n = c->cap = 1;
    pthread_mutex_init(&c->r[0]->mu, NULL);
    c->r[0]->active = 1;
    c->r[0]->fd = TEST_FD;
    c->r[0]->io = (struct iopath *)(uintptr_t)1;
    c->r[0]->dur = policy;
    c->r[0]->kernel_holds = kernel_debt;
    c->r[0]->unflushed = raw_debt;
    g_ctx = c;
    return c;
}

static void drop_ctx(struct exitos_ctx *c)
{
    g_ctx = NULL;
    wb_exitos_ctx_destroy(c);
}

static struct exitos_ctx *two_fd_ctx(void)
{
    struct exitos_ctx *c = wb_exitos_ctx_create();
    int fds[2] = { TEST_FD, TEST_FD + 1 };

    if (!c)
        return NULL;
    c->r = calloc(2, sizeof(*c->r));
    if (!c->r) {
        wb_exitos_ctx_destroy(c);
        return NULL;
    }
    c->n = c->cap = 2;
    for (int i = 0; i < 2; i++) {
        c->r[i] = calloc(1, sizeof(*c->r[i]));
        if (!c->r[i]) {
            wb_exitos_ctx_destroy(c);
            return NULL;
        }
        pthread_mutex_init(&c->r[i]->mu, NULL);
        c->r[i]->active = 1;
        c->r[i]->fd = fds[i];
        c->r[i]->io = (struct iopath *)(uintptr_t)(i + 1);
        c->r[i]->dur = EXITOS_DUR_NONE;
    }
    return c;
}

int main(void)
{
    struct exitos_ctx *c;
    int rc, res;

    T_EQ(wb_exitos_fdatasync_needed(EXITOS_DUR_NONE), 0,
         "write-through policy has no raw FLUSH debt");
    T_EQ(wb_exitos_fdatasync_needed(EXITOS_DUR_FUA), 0,
         "FUA policy has no raw FLUSH debt");
    T_EQ(wb_exitos_fdatasync_needed(EXITOS_DUR_FLUSH), 1,
         "volatile cache without FUA requires raw FLUSH");

    /* Different fds must not bounce one process-wide table-lock cache line on
     * every write/sync.  Conversely a table writer must freeze every shard so
     * the existing stable-registration lifetime contract still holds. */
    c = two_fd_ctx();
    T_OK(c != NULL, "constructed two independent registered fds");
    if (c) {
        struct exitos_fd_txn a, b;

        T_OK((uintptr_t)c % _Alignof(struct exitos_ctx) == 0,
             "context allocation satisfies its over-aligned table-shard type");

        memset(g_rdlock_seen, 0, sizeof g_rdlock_seen);
        g_rdlock_seen_n = 0;
        g_record_table_locks = 1;
        T_EQ(exitos_fd_txn_begin(c, TEST_FD, &a), 0,
             "first fd table transaction begins");
        exitos_fd_txn_end(&a);
        T_EQ(exitos_fd_txn_begin(c, TEST_FD + 1, &b), 0,
             "second fd table transaction begins");
        exitos_fd_txn_end(&b);
        g_record_table_locks = 0;
        T_EQ(g_rdlock_seen_n, 2, "one table read lock is acquired per transaction");
#ifdef EXITOS_TABLE_GLOBAL_BASELINE
        T_OK(g_rdlock_seen_n == 2 && g_rdlock_seen[0] == g_rdlock_seen[1],
             "compile-time global baseline maps different fds to one table lock");
#else
        T_OK(g_rdlock_seen_n == 2 && g_rdlock_seen[0] != g_rdlock_seen[1],
             "different fds use different table-lock cache lines");
        T_OK(g_rdlock_seen_n == 2 &&
             ((uintptr_t)g_rdlock_seen[0] > (uintptr_t)g_rdlock_seen[1]
              ? (uintptr_t)g_rdlock_seen[0] - (uintptr_t)g_rdlock_seen[1]
              : (uintptr_t)g_rdlock_seen[1] - (uintptr_t)g_rdlock_seen[0]) >= 64,
             "different fd shards are separated by at least one cache line");
#endif

        memset(g_wrlock_seen, 0, sizeof g_wrlock_seen);
        g_wrlock_seen_n = 0;
        g_record_table_locks = 1;
        wb_exitos_ctx_verify_identity(c, 1);
        g_record_table_locks = 0;
        T_EQ(g_wrlock_seen_n, EXITOS_TABLE_SHARDS,
             "a table writer freezes every lock shard before changing state");
        {
            int aligned = g_wrlock_seen_n == EXITOS_TABLE_SHARDS;

            for (int i = 0; aligned && i < g_wrlock_seen_n; i++)
                if ((uintptr_t)g_wrlock_seen[i] % 64 != 0)
                    aligned = 0;
            T_OK(aligned, "every table lock begins on its own cache line");
        }
        drop_ctx(c);
    }

    /* A: a mixed fd owes BOTH domains.  A failed raw flush must fail this
     * fdatasync before a successful kernel call can hide it. */
    reset_mocks();
    c = sync_ctx(EXITOS_DUR_FLUSH, 1, 1);
    T_OK(c != NULL, "A: constructed an in-memory mixed-debt registration");
    if (c) {
        g_flush_rc[0] = -EIO;
        g_flush_nr = 1;
        errno = 0;
        rc = wb_preload_fdatasync(TEST_FD);
        T_EQ(rc, -1, "A: raw FLUSH failure fails fdatasync");
        T_EQ(errno, EIO, "A: internal -EIO becomes libc -1/errno=EIO");
        T_EQ(g_kernel_fdatasync_calls, 0,
             "A: failed raw FLUSH is not handed to kernel fdatasync to mask");
        T_EQ(c->r[0]->unflushed, 1, "A: raw debt remains after failure");
        T_EQ(c->r[0]->kernel_holds, 1, "A: kernel debt remains after failure");

        /* Retry: raw succeeds, then the required kernel sync succeeds. */
        g_flush_rc[1] = 0;
        g_flush_nr = 2;
        errno = 0;
        rc = wb_preload_fdatasync(TEST_FD);
        T_EQ(rc, 0, "A: retry succeeds once raw and kernel sync both succeed");
        T_EQ(g_flush_calls, 2, "A: retry issued the raw FLUSH again");
        T_EQ(g_kernel_fdatasync_calls, 1,
             "A: retry ran exactly one real kernel fdatasync");
        T_EQ(c->r[0]->unflushed, 0, "A: successful raw retry clears raw debt");
        T_EQ(c->r[0]->kernel_holds, 0,
             "A: successful real fdatasync clears kernel debt");
        drop_ctx(c);
    }

    /* The completion note is deliberately one-domain-only.  A caller may
     * report a successful filesystem sync after a raw flush failed. */
    reset_mocks();
    c = sync_ctx(EXITOS_DUR_FLUSH, 1, 1);
    if (c) {
        wb_exitos_note_kernel_sync(c, TEST_FD);
        T_EQ(c->r[0]->kernel_holds, 0,
             "note_kernel_sync clears only the kernel debt");
        T_EQ(c->r[0]->unflushed, 1,
             "note_kernel_sync cannot claim an unrelated raw FLUSH succeeded");
        drop_ctx(c);
    }

    /* B: fsync must retain its metadata syscall while also paying raw debt.
     * Even when the raw half fails, run the real fsync; return the raw error,
     * keep raw debt, and clear kernel debt only if the real fsync succeeded. */
    reset_mocks();
    c = sync_ctx(EXITOS_DUR_FLUSH, 1, 1);
    if (c) {
        g_flush_rc[0] = -EIO;
        g_flush_nr = 1;
        errno = 0;
        rc = wb_preload_fsync(TEST_FD);
        T_EQ(rc, -1, "B: fsync reports a failed raw FLUSH");
        T_EQ(errno, EIO, "B: fsync exposes the raw failure as libc errno");
        T_EQ(g_kernel_fsync_calls, 1,
             "B: fsync still runs the real metadata sync when raw FLUSH fails");
        T_EQ(c->r[0]->unflushed, 1, "B: failed raw FLUSH remains owed");
        T_EQ(c->r[0]->kernel_holds, 0,
             "B: successful real fsync independently clears kernel debt");

        g_flush_rc[1] = 0;
        g_flush_nr = 2;
        rc = wb_preload_fsync(TEST_FD);
        T_EQ(rc, 0, "B: fsync retry succeeds after raw FLUSH succeeds");
        T_EQ(g_flush_calls, 2, "B: fsync retry repeats the failed raw FLUSH");
        T_EQ(g_kernel_fsync_calls, 2,
             "B: every fsync call runs the real metadata sync");
        T_EQ(c->r[0]->unflushed, 0, "B: retry clears raw debt");
        drop_ctx(c);
    }

    /* A successful raw FLUSH does not excuse a failed filesystem sync.  The
     * raw domain is clean, but kernel debt remains and the errno is libc's. */
    reset_mocks();
    c = sync_ctx(EXITOS_DUR_FLUSH, 1, 1);
    if (c) {
        g_kernel_fsync_rc = -1;
        g_kernel_fsync_errno = ENOSPC;
        errno = 0;
        rc = wb_preload_fsync(TEST_FD);
        T_EQ(rc, -1, "B: real fsync failure is returned after raw success");
        T_EQ(errno, ENOSPC, "B: real fsync errno is preserved");
        T_EQ(c->r[0]->unflushed, 0, "B: successful raw FLUSH stays paid");
        T_EQ(c->r[0]->kernel_holds, 1, "B: failed real fsync retains kernel debt");
        drop_ctx(c);
    }

    /* D: once raw FLUSH succeeds it is not repeated merely because the real
     * fdatasync failed; only the kernel debt is retried. */
    reset_mocks();
    c = sync_ctx(EXITOS_DUR_FLUSH, 1, 1);
    if (c) {
        g_kernel_fdatasync_rc = -1;
        g_kernel_fdatasync_errno = EDQUOT;
        errno = 0;
        rc = wb_preload_fdatasync(TEST_FD);
        T_EQ(rc, -1, "D: real fdatasync failure is returned");
        T_EQ(errno, EDQUOT, "D: real fdatasync errno is preserved");
        T_EQ(g_flush_calls, 1, "D: raw FLUSH succeeded once");
        T_EQ(c->r[0]->unflushed, 0, "D: successful raw debt stays paid");
        T_EQ(c->r[0]->kernel_holds, 1,
             "D: failed real fdatasync retains only kernel debt");

        g_kernel_fdatasync_rc = 0;
        rc = wb_preload_fdatasync(TEST_FD);
        T_EQ(rc, 0, "D: retry succeeds when real fdatasync succeeds");
        T_EQ(g_flush_calls, 1, "D: retry does not repeat an already-paid FLUSH");
        T_EQ(g_kernel_fdatasync_calls, 2,
             "D: retry repeats the still-owed kernel sync");
        T_EQ(c->r[0]->kernel_holds, 0, "D: successful retry clears kernel debt");
        drop_ctx(c);
    }

    /* C: FUA, write-through, and no-write cases do not issue an extra FLUSH. */
    reset_mocks();
    c = sync_ctx(EXITOS_DUR_FUA, 0, 1);
    if (c) {
        T_EQ(wb_preload_fdatasync(TEST_FD), 0, "C: FUA fdatasync succeeds locally");
        T_EQ(g_flush_calls, 0, "C: FUA issues no FLUSH");
        drop_ctx(c);
    }
    reset_mocks();
    c = sync_ctx(EXITOS_DUR_NONE, 0, 1);
    if (c) {
        T_EQ(wb_preload_fdatasync(TEST_FD), 0,
             "C: write-through fdatasync succeeds locally");
        T_EQ(g_flush_calls, 0, "C: write-through issues no FLUSH");
        drop_ctx(c);
    }
    reset_mocks();
    c = sync_ctx(EXITOS_DUR_FLUSH, 0, 0);
    if (c) {
        T_EQ(wb_preload_fdatasync(TEST_FD), 0, "C: no-write fdatasync is a no-op");
        T_EQ(g_flush_calls, 0, "C: no-write case issues no FLUSH");
        drop_ctx(c);
    }

    /* Core ABI: errors stay negative until a frontend converts them. */
    reset_mocks();
    c = sync_ctx(EXITOS_DUR_FLUSH, 0, 1);
    if (c) {
        g_flush_rc[0] = -EREMOTEIO;
        g_flush_nr = 1;
        res = 1234;
        T_EQ(wb_exitos_on_fdatasync(c, TEST_FD, &res), EXITOS_TAKEOVER,
             "D: unsafe fallback is suppressed on core raw-FLUSH failure");
        T_EQ(res, -EREMOTEIO, "D: core returns the internal negative errno");
        T_EQ(c->r[0]->unflushed, 1, "D: core failure is retryable");
        drop_ctx(c);
    }

    T_DONE();
}
