/* Backend 2: capture syscalls by rewriting the syscall instructions in the
 * process image, instead of replacing libc symbols.
 *
 * Why a second backend exists at all: LD_PRELOAD interposes libc symbols, so it
 * cannot see a write issued by inline assembly. That is not hypothetical — the
 * repo's own raw-syscall writer is missed by backend 1, and Go runtimes and
 * statically linked binaries issue syscalls the same way.
 *
 * Route A, per the build report: dlopen the transformer, register the Capstone
 * x86 architecture, call setup_syscall_tracer(), then install a plain C function
 * pointer with set_call_hook(). "Skip the original syscall and return my own
 * value" is expressed by returning from the hook without calling the saved
 * original. Nothing here needs an eBPF program to be compiled or loaded.
 *
 * These are the parts that can be tested with no bpftime present and no root:
 * which syscalls we claim, how their arguments map onto the decision functions,
 * and that everything is refused when the transformer is unavailable. */
#include "tap.h"
#include "exitos_bpftime.h"
#include "exitos_frontend_admission.h"
#include "exitos_frontend_config.h"
#include "exitos_frontend_fallocate.h"
#include "exitos_intercept.h"
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>

static exitos_decision g_sync_decision;
static int g_sync_result;
static int g_raw_sync_rc;
static int g_raw_sync_calls;
static int g_note_calls;
static int g_kernel_write_notes;
static int g_kernel_metadata_notes;
static int g_unregister_calls;
static int g_truncate_decisions;
static int g_orig_calls;
static int g_orig_fallocate_calls;
static int64_t g_orig_rc;
static int g_write_note_seen_at_orig;
static int g_global_poison_calls;
static int g_invalidate_calls;
static int g_frontend_fallocate_exec_calls;
static int g_frontend_fallocate_create_calls;
static int g_frontend_fallocate_destroy_calls;
static int g_frontend_fallocate_takeover;
static int g_frontend_fallocate_rc;
static int g_frontend_fallocate_ctl_depth;
static char g_prepare_ready_line[512];
static char g_prepare_outcome_line[512];

/* Shared tentative definitions with the private source copy below. */
static struct exitos_ctx *g_ctx;
static __thread int __attribute__((tls_model("initial-exec"))) g_ctl_depth;

static exitos_decision mock_on_fdatasync(struct exitos_ctx *c, int fd, int *result)
{
    (void)c;
    (void)fd;
    if (g_sync_decision == EXITOS_TAKEOVER)
        *result = g_sync_result;
    return g_sync_decision;
}

static int mock_flush_raw_debt(struct exitos_ctx *c, int fd)
{
    (void)c;
    (void)fd;
    g_raw_sync_calls++;
    return g_raw_sync_rc;
}

static void mock_note_kernel_sync(struct exitos_ctx *c, int fd)
{
    (void)c;
    (void)fd;
    g_note_calls++;
}

static void mock_note_kernel_write(struct exitos_ctx *c, int fd)
{
    (void)c; (void)fd;
    g_kernel_write_notes++;
}

static exitos_decision mock_on_ftruncate(struct exitos_ctx *c, int fd,
                                         off_t len, int *result)
{
    (void)c; (void)fd; (void)len; (void)result;
    g_truncate_decisions++;
    return EXITOS_PASS;
}

static int mock_txn_begin(struct exitos_ctx *c, int fd,
                          struct exitos_fd_txn *tx)
{
    memset(tx, 0, sizeof(*tx));
    tx->ctx = c;
    tx->fd = fd;
    tx->held = 1;
    tx->registered = 1;
    return 0;
}

static int mock_txn_registered(const struct exitos_fd_txn *tx)
{
    return tx && tx->registered;
}

static int mock_txn_prepare_forget(struct exitos_fd_txn *tx)
{
    (void)tx;
    return g_raw_sync_rc;
}

static int mock_txn_forget(struct exitos_fd_txn *tx)
{
    if (tx && tx->registered) {
        tx->registered = 0;
        g_unregister_calls++;
    }
    return 0;
}

static void mock_txn_abort_forget(struct exitos_fd_txn *tx)
{
    (void)tx;
}

static void mock_txn_end(struct exitos_fd_txn *tx)
{
    if (tx)
        tx->held = 0;
}

static exitos_decision mock_txn_on_write(struct exitos_fd_txn *tx,
                                         const void *buf, size_t n, off_t off,
                                         ssize_t *result)
{
    (void)tx; (void)buf; (void)n; (void)off; (void)result;
    return EXITOS_PASS;
}

static exitos_decision mock_txn_on_fdatasync(struct exitos_fd_txn *tx,
                                             int *result)
{
    return mock_on_fdatasync(tx ? tx->ctx : NULL, tx ? tx->fd : -1, result);
}

static int mock_txn_flush(struct exitos_fd_txn *tx)
{
    return mock_flush_raw_debt(tx ? tx->ctx : NULL, tx ? tx->fd : -1);
}

static exitos_decision mock_txn_on_ftruncate(struct exitos_fd_txn *tx,
                                             off_t len, int *result)
{
    return mock_on_ftruncate(tx ? tx->ctx : NULL, tx ? tx->fd : -1,
                             len, result);
}

static void mock_txn_note_write(struct exitos_fd_txn *tx)
{
    mock_note_kernel_write(tx ? tx->ctx : NULL, tx ? tx->fd : -1);
}

static void mock_txn_note_metadata(struct exitos_fd_txn *tx)
{
    (void)tx;
    g_kernel_metadata_notes++;
}

static void mock_txn_note_sync(struct exitos_fd_txn *tx)
{
    mock_note_kernel_sync(tx ? tx->ctx : NULL, tx ? tx->fd : -1);
}

static void mock_txn_invalidate(struct exitos_fd_txn *tx)
{
    (void)tx;
    g_invalidate_calls++;
}

static int mock_alias_begin(struct exitos_ctx *c, int source_fd, int target_fd,
                            struct exitos_alias_txn *tx)
{
    memset(tx, 0, sizeof(*tx));
    tx->ctx = c;
    tx->source_fd = source_fd;
    tx->target_fd = target_fd;
    tx->held = 1;
    tx->source_registered = 1;
    tx->target_registered = target_fd >= 0;
    return 0;
}

static int mock_alias_prepare(struct exitos_alias_txn *tx)
{
    (void)tx;
    return g_raw_sync_rc;
}

static void mock_alias_abort(struct exitos_alias_txn *tx)
{
    (void)tx;
}

static void mock_alias_commit(struct exitos_alias_txn *tx)
{
    (void)tx;
    g_unregister_calls++;
}

static void mock_alias_end(struct exitos_alias_txn *tx)
{
    if (tx)
        tx->held = 0;
}

static int mock_flush_all_and_poison(struct exitos_ctx *c)
{
    (void)c;
    g_global_poison_calls++;
    return g_raw_sync_rc;
}

static void mock_ctx_poison(struct exitos_ctx *c)
{
    (void)c;
    g_global_poison_calls++;
}

/* Wrap the real shared factory at the private active-bpftime seam.  A missing
 * transformer is a cheap, device-free failure path: configuration must still
 * be validated/snapshotted first and then destroyed, proving this adapter did
 * not grow its own EXITOS_* parsing. */
static unsigned g_bpftime_frontend_config_calls;
static unsigned g_bpftime_frontend_destroy_calls;

/* A loader-only active-start fixture.  It lets the white-box copy exercise
 * symbol preflight without executing a real transformer or rewriting this
 * test process. */
typedef int64_t (*fake_hook_fn)(int64_t, int64_t, int64_t, int64_t,
                                int64_t, int64_t, int64_t);
static int g_fake_loader;
static unsigned g_fake_csreg_calls;
static unsigned g_fake_setup_calls;
static unsigned g_fake_getk_calls;
static unsigned g_fake_setk_calls;
static unsigned g_fake_dlclose_calls;
static int g_fake_orig_available;
static int g_fake_capture_publish;
static int g_fake_trace_at_publish;
static int g_fake_noop_at_publish;
static int g_fake_admission_at_publish;
/* Forward tentative declarations for the private source copy below. */
static int g_trace;
static int g_noop;
static _Atomic int g_block_frontend_admission;
static _Atomic int g_frontend_admission_entered;
static _Atomic int g_release_frontend_admission;
static _Atomic int g_frontend_quiesce_entered;
static _Atomic int g_frontend_stop_done;

static void fake_csreg(void) { g_fake_csreg_calls++; }
static void fake_setup(void) { g_fake_setup_calls++; }
static int64_t fake_orig(int64_t nr, int64_t a1, int64_t a2, int64_t a3,
                         int64_t a4, int64_t a5, int64_t a6)
{
    (void)nr; (void)a1; (void)a2; (void)a3;
    (void)a4; (void)a5; (void)a6;
    return 0;
}
static fake_hook_fn fake_getk(void)
{
    g_fake_getk_calls++;
    return g_fake_orig_available ? fake_orig : NULL;
}
static void fake_setk(fake_hook_fn hook)
{
    (void)hook;
    g_fake_setk_calls++;
    if (g_fake_capture_publish) {
        g_fake_trace_at_publish = g_trace;
        g_fake_noop_at_publish = g_noop;
        g_fake_admission_at_publish = exitos_frontend_admission_enter();
        if (g_fake_admission_at_publish)
            exitos_frontend_admission_leave();
        g_fake_capture_publish = 0;
    }
}

static void *wb_dlopen(const char *path, int flags)
{
    if (g_fake_loader) {
        (void)path;
        (void)flags;
        return (void *)(uintptr_t)0x1234;
    }
    return dlopen(path, flags);
}

static void *wb_dlsym(void *handle, const char *name)
{
    if (!g_fake_loader)
        return dlsym(handle, name);
    if (strcmp(name, "_frida_cs_arch_register_x86") == 0)
        return (void *)fake_csreg;
    if (strcmp(name, "_ZN7bpftime20setup_syscall_tracerEv") == 0)
        return (void *)fake_setup;
    if (strcmp(name, "_ZN7bpftime13get_call_hookEv") == 0)
        return (void *)fake_getk;
    if (strcmp(name,
               "_ZN7bpftime13set_call_hookEPFllllllllE") == 0)
        return (void *)fake_setk;
    return NULL;
}

static int wb_dlclose(void *handle)
{
    if (g_fake_loader) {
        (void)handle;
        g_fake_dlclose_calls++;
        return 0;
    }
    return dlclose(handle);
}

static int wb_frontend_config_from_env(struct exitos_frontend_config **out)
{
    g_bpftime_frontend_config_calls++;
    return exitos_frontend_config_from_env(out);
}

static void wb_frontend_config_destroy(struct exitos_frontend_config *config)
{
    g_bpftime_frontend_destroy_calls++;
    exitos_frontend_config_destroy(config);
}

static int wb_frontend_admission_enter(void)
{
    int admitted = exitos_frontend_admission_enter();

    if (admitted && atomic_load_explicit(&g_block_frontend_admission,
                                         memory_order_acquire)) {
        atomic_store_explicit(&g_frontend_admission_entered, 1,
                              memory_order_release);
        while (!atomic_load_explicit(&g_release_frontend_admission,
                                     memory_order_acquire))
            sched_yield();
    }
    return admitted;
}

static void wb_frontend_admission_leave(void)
{
    exitos_frontend_admission_leave();
}

static void wb_frontend_admission_enable(void)
{
    exitos_frontend_admission_enable();
}

static void wb_frontend_admission_quiesce(void)
{
    atomic_store_explicit(&g_frontend_quiesce_entered, 1,
                          memory_order_release);
    exitos_frontend_admission_quiesce();
}

static int64_t mock_orig(int64_t nr, int64_t a1, int64_t a2, int64_t a3,
                         int64_t a4, int64_t a5, int64_t a6)
{
    (void)a4; (void)a5; (void)a6;
#ifdef SYS_write
    /* internal_log writes readiness diagnostics to stderr.  Do not treat an
     * arbitrary application write buffer as a NUL-terminated log message. */
    if (nr == SYS_write && a1 == STDERR_FILENO && a2 != 0 && a3 > 0) {
        size_t amount = (size_t)a3;
        const char *source = (const char *)(uintptr_t)a2;
        char *destination = amount >= 16 &&
                                    memcmp(source, "PREPARE_OUTCOME ", 16) == 0 ?
                                g_prepare_outcome_line : g_prepare_ready_line;
        if (amount >= sizeof g_prepare_ready_line)
            amount = sizeof g_prepare_ready_line - 1;
        memcpy(destination, source, amount);
        destination[amount] = '\0';
        return a3;
    }
#else
    (void)a2;
    (void)a3;
#endif
#ifdef SYS_fallocate
    if (nr == SYS_fallocate)
        g_orig_fallocate_calls++;
#else
    (void)nr;
#endif
    g_write_note_seen_at_orig = g_kernel_write_notes;
    g_orig_calls++;
    return g_orig_rc;
}

static int wb_frontend_fallocate_runtime_create(
    struct exitos_frontend_fallocate_runtime **out,
    const struct exitos_frontend_config *config, struct exitos_ctx *ctx)
{
    (void)config;
    (void)ctx;
    g_frontend_fallocate_create_calls++;
    *out = (struct exitos_frontend_fallocate_runtime *)(uintptr_t)0x8888;
    return 0;
}

static void wb_frontend_fallocate_runtime_destroy(
    struct exitos_frontend_fallocate_runtime *runtime)
{
    if (runtime)
        g_frontend_fallocate_destroy_calls++;
}

static int wb_frontend_fallocate_execute(
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
                result->chunks = 4;
            } else {
                result->outcome = EXITOS_FALLOCATE_ELIGIBLE_FAILED;
                result->stage = EXITOS_FALLOCATE_STAGE_DONOR_PREPARE;
            }
        }
        return g_frontend_fallocate_rc;
    }
    if (mock_txn_begin(g_ctx, fd, &tx) != 0)
        return native_call(native_opaque, fd, mode, off, len);
    rc = mock_txn_flush(&tx);
    if (rc == 0) {
        mock_txn_invalidate(&tx);
        rc = native_call(native_opaque, fd, mode, off, len);
        if (rc == 0)
            mock_txn_note_metadata(&tx);
    }
    mock_txn_end(&tx);
    if (result) {
        result->outcome = rc == 0 ? EXITOS_FALLOCATE_NATIVE_SKIP :
                                    EXITOS_FALLOCATE_CONTROL_FAILED;
        result->stage = rc == 0 ? EXITOS_FALLOCATE_STAGE_NATIVE :
                                  EXITOS_FALLOCATE_STAGE_FLUSH;
        result->rc = rc;
    }
    return rc;
}

static const char *wb_frontend_fallocate_outcome_name(
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

static const char *wb_frontend_fallocate_stage_name(
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

/* White-box the actual syscall dispatch under private names.  The production
 * object is still built and linked by the normal rule; only the assembly shim
 * is omitted from this second, private copy to avoid defining its fixed symbol
 * twice. */
#define EXITOS_BPFTIME_NO_ASM 1
/* Exercise the dormant dispatcher as pure logic even though the production
 * DSO is hard-frozen until its assembly boundary preserves complete xstate. */
#define EXITOS_BPFTIME_TEST_DORMANT_DISPATCH 1
#define exitos_bpftime_orig       wb_bpftime_orig
#define exitos_claim_tbl          wb_claim_tbl
#define exitos_xstate_use         wb_xstate_use
#define exitos_xstate_lo          wb_xstate_lo
#define exitos_xstate_hi          wb_xstate_hi
#define exitos_xstate_frame       wb_xstate_frame
#define exitos_bpftime_claims     wb_bpftime_claims
#define exitos_syscall_hook       wb_syscall_hook
#define exitos_syscall_hook_c     wb_syscall_hook_c
#define exitos_bpftime_available  wb_bpftime_available
#define exitos_bpftime_started    wb_bpftime_started
#define exitos_bpftime_start      wb_bpftime_start
#define exitos_bpftime_entries    wb_bpftime_entries
#define exitos_bpftime_claimed    wb_bpftime_claimed
#define exitos_bpftime_stop       wb_bpftime_stop
#define exitos_on_fdatasync       mock_on_fdatasync
#define exitos_flush_raw_debt     mock_flush_raw_debt
#define exitos_note_kernel_sync   mock_note_kernel_sync
#define exitos_note_kernel_write  mock_note_kernel_write
#define exitos_on_ftruncate       mock_on_ftruncate
#define exitos_fd_txn_begin       mock_txn_begin
#define exitos_fd_txn_registered  mock_txn_registered
#define exitos_fd_txn_forget      mock_txn_forget
#define exitos_fd_txn_end         mock_txn_end
#define exitos_fd_txn_prepare_forget mock_txn_prepare_forget
#define exitos_fd_txn_abort_forget mock_txn_abort_forget
#define exitos_txn_on_write       mock_txn_on_write
#define exitos_txn_on_fdatasync   mock_txn_on_fdatasync
#define exitos_txn_flush_raw_debt mock_txn_flush
#define exitos_txn_on_ftruncate   mock_txn_on_ftruncate
#define exitos_txn_note_kernel_write mock_txn_note_write
#define exitos_txn_note_kernel_metadata mock_txn_note_metadata
#define exitos_txn_note_kernel_sync  mock_txn_note_sync
#define exitos_txn_invalidate_mapping mock_txn_invalidate
#define exitos_alias_txn_begin       mock_alias_begin
#define exitos_alias_txn_prepare     mock_alias_prepare
#define exitos_alias_txn_abort       mock_alias_abort
#define exitos_alias_txn_commit      mock_alias_commit
#define exitos_alias_txn_end         mock_alias_end
#define exitos_ctx_flush_all_and_poison mock_flush_all_and_poison
#define exitos_ctx_poison               mock_ctx_poison
#define exitos_internal_fd_enter         wb_internal_fd_enter
#define exitos_internal_fd_leave         wb_internal_fd_leave
#define exitos_internal_open_call        wb_internal_open_call
#define exitos_internal_openat_call      wb_internal_openat_call
#define exitos_internal_close_call       wb_internal_close_call
#define exitos_internal_ftruncate_call   wb_internal_ftruncate_call
#define exitos_internal_fallocate_call   wb_internal_fallocate_call
#define exitos_internal_pwrite_call      wb_internal_pwrite_call
#define exitos_internal_fdatasync_call   wb_internal_fdatasync_call
#define exitos_frontend_config_from_env  wb_frontend_config_from_env
#define exitos_frontend_config_destroy   wb_frontend_config_destroy
#define exitos_frontend_fallocate_runtime_create wb_frontend_fallocate_runtime_create
#define exitos_frontend_fallocate_runtime_destroy wb_frontend_fallocate_runtime_destroy
#define exitos_frontend_fallocate_execute wb_frontend_fallocate_execute
#define exitos_frontend_fallocate_outcome_name wb_frontend_fallocate_outcome_name
#define exitos_frontend_fallocate_stage_name wb_frontend_fallocate_stage_name
#define exitos_frontend_admission_enter  wb_frontend_admission_enter
#define exitos_frontend_admission_leave  wb_frontend_admission_leave
#define exitos_frontend_admission_enable wb_frontend_admission_enable
#define exitos_frontend_admission_quiesce wb_frontend_admission_quiesce
#define dlopen                            wb_dlopen
#define dlsym                             wb_dlsym
#define dlclose                           wb_dlclose
#include "../../src/bpftime_hook.c"
#undef EXITOS_BPFTIME_NO_ASM
#undef EXITOS_BPFTIME_TEST_DORMANT_DISPATCH
#undef exitos_bpftime_orig
#undef exitos_claim_tbl
#undef exitos_xstate_use
#undef exitos_xstate_lo
#undef exitos_xstate_hi
#undef exitos_xstate_frame
#undef exitos_bpftime_claims
#undef exitos_syscall_hook
#undef exitos_syscall_hook_c
#undef exitos_bpftime_available
#undef exitos_bpftime_started
#undef exitos_bpftime_start
#undef exitos_bpftime_entries
#undef exitos_bpftime_claimed
#undef exitos_bpftime_stop
#undef exitos_on_fdatasync
#undef exitos_flush_raw_debt
#undef exitos_note_kernel_sync
#undef exitos_note_kernel_write
#undef exitos_on_ftruncate
#undef exitos_fd_txn_begin
#undef exitos_fd_txn_registered
#undef exitos_fd_txn_forget
#undef exitos_fd_txn_end
#undef exitos_fd_txn_prepare_forget
#undef exitos_fd_txn_abort_forget
#undef exitos_txn_on_write
#undef exitos_txn_on_fdatasync
#undef exitos_txn_flush_raw_debt
#undef exitos_txn_on_ftruncate
#undef exitos_txn_note_kernel_write
#undef exitos_txn_note_kernel_metadata
#undef exitos_txn_note_kernel_sync
#undef exitos_txn_invalidate_mapping
#undef exitos_alias_txn_begin
#undef exitos_alias_txn_prepare
#undef exitos_alias_txn_abort
#undef exitos_alias_txn_commit
#undef exitos_alias_txn_end
#undef exitos_ctx_flush_all_and_poison
#undef exitos_ctx_poison
#undef exitos_internal_fd_enter
#undef exitos_internal_fd_leave
#undef exitos_internal_open_call
#undef exitos_internal_openat_call
#undef exitos_internal_close_call
#undef exitos_internal_ftruncate_call
#undef exitos_internal_fallocate_call
#undef exitos_internal_pwrite_call
#undef exitos_internal_fdatasync_call
#undef exitos_frontend_config_from_env
#undef exitos_frontend_config_destroy
#undef exitos_frontend_fallocate_runtime_create
#undef exitos_frontend_fallocate_runtime_destroy
#undef exitos_frontend_fallocate_execute
#undef exitos_frontend_fallocate_outcome_name
#undef exitos_frontend_fallocate_stage_name
#undef exitos_frontend_admission_enter
#undef exitos_frontend_admission_leave
#undef exitos_frontend_admission_enable
#undef exitos_frontend_admission_quiesce
#undef dlopen
#undef dlsym
#undef dlclose

int64_t wb_syscall_hook(int64_t nr, int64_t a1, int64_t a2, int64_t a3,
                        int64_t a4, int64_t a5, int64_t a6)
{
    return mock_orig(nr, a1, a2, a3, a4, a5, a6);
}

static void reset_dispatch(void)
{
    exitos_frontend_admission_enable();
    g_sync_decision = EXITOS_PASS;
    g_sync_result = 0;
    g_raw_sync_rc = 0;
    g_raw_sync_calls = 0;
    g_note_calls = 0;
    g_kernel_write_notes = 0;
    g_kernel_metadata_notes = 0;
    g_unregister_calls = 0;
    g_truncate_decisions = 0;
    g_orig_calls = 0;
    g_orig_fallocate_calls = 0;
    g_write_note_seen_at_orig = 0;
    g_global_poison_calls = 0;
    g_invalidate_calls = 0;
    g_frontend_fallocate_exec_calls = 0;
    g_frontend_fallocate_create_calls = 0;
    g_frontend_fallocate_destroy_calls = 0;
    g_frontend_fallocate_takeover = 0;
    g_frontend_fallocate_rc = 0;
    g_frontend_fallocate_ctl_depth = 0;
    g_prepare_ready_line[0] = '\0';
    g_prepare_outcome_line[0] = '\0';
    g_orig_rc = 0;
    wb_bpftime_orig = mock_orig;
    g_ctx = (struct exitos_ctx *)(uintptr_t)1;
    g_fastpath = 1;
    g_in_hook = 0;
    g_ctl_depth = 0;
    g_noop = 0;
}

static int wait_atomic_flag(_Atomic int *flag)
{
    for (int i = 0; i < 100000; ++i) {
        if (atomic_load_explicit(flag, memory_order_acquire))
            return 1;
        sched_yield();
    }
    return 0;
}

static void *bpftime_admitted_caller(void *unused)
{
    (void)unused;
    (void)wb_syscall_hook_c(SYS_pwrite64, 41, 0, 1, 0, 0, 0);
    return NULL;
}

static void *bpftime_stopper(void *unused)
{
    (void)unused;
    wb_bpftime_stop();
    atomic_store_explicit(&g_frontend_stop_done, 1, memory_order_release);
    return NULL;
}

static void test_bpftime_shutdown_admission(void)
{
    pthread_t caller, stopper;
    int admitted;

    setenv("EXITOS_FILES", "shutdown-admission", 1);
    setenv("EXITOS_IOPATH", "pwrite", 1);
    g_config = NULL;
    T_EQ(exitos_frontend_config_from_env(&g_config), 0,
         "built shared config for active-bpftime shutdown admission test");
    unsetenv("EXITOS_FILES");
    unsetenv("EXITOS_IOPATH");
    g_ctx = exitos_frontend_config_ctx(g_config);
    g_started = 1;
    g_so = (void *)(uintptr_t)0x1234;
    wb_bpftime_orig = mock_orig;
    g_fake_loader = 1;
    atomic_store(&g_block_frontend_admission, 1);
    atomic_store(&g_frontend_admission_entered, 0);
    atomic_store(&g_release_frontend_admission, 0);
    atomic_store(&g_frontend_quiesce_entered, 0);
    atomic_store(&g_frontend_stop_done, 0);
    exitos_frontend_admission_quiesce();
    exitos_frontend_admission_enable();

    T_EQ(pthread_create(&caller, NULL, bpftime_admitted_caller, NULL), 0,
         "started active-bpftime caller in the check/use window");
    admitted = wait_atomic_flag(&g_frontend_admission_entered);
    T_OK(admitted,
         "active-bpftime hook holds admission before borrowing its context");
    if (!admitted) {
        pthread_join(caller, NULL);
        wb_bpftime_stop();
        exitos_frontend_admission_quiesce();
        g_fake_loader = 0;
        return;
    }

    T_EQ(pthread_create(&stopper, NULL, bpftime_stopper, NULL), 0,
         "started active-bpftime stop while a caller is admitted");
    T_OK(wait_atomic_flag(&g_frontend_quiesce_entered),
         "active-bpftime stop closes shared admission first");
    T_EQ(atomic_load_explicit(&g_frontend_stop_done, memory_order_acquire), 0,
         "active-bpftime stop cannot free config while a hook is admitted");
    atomic_store_explicit(&g_release_frontend_admission, 1,
                          memory_order_release);
    pthread_join(caller, NULL);
    pthread_join(stopper, NULL);
    T_EQ(atomic_load_explicit(&g_frontend_stop_done, memory_order_acquire), 1,
         "active-bpftime stop completes after the admitted hook leaves");
    T_OK(g_config == NULL && g_ctx == NULL,
         "active-bpftime clears ownership only after quiescence");
    g_fake_loader = 0;
}

static void test_bpftime_start_publication_order(void)
{
    g_started = 0;
    g_so = NULL;
    g_ctx = NULL;
    g_config = NULL;
    g_fake_loader = 1;
    g_fake_orig_available = 1;
    g_fake_capture_publish = 1;
    g_fake_trace_at_publish = -1;
    g_fake_noop_at_publish = -1;
    g_fake_admission_at_publish = -1;
    g_trace = 0;
    g_noop = 0;
    g_frontend_fallocate_create_calls = 0;
    g_frontend_fallocate_destroy_calls = 0;
    exitos_frontend_admission_quiesce();
    setenv("EXITOS_FILES", "/tmp/exitos-bpftime-config-*", 1);
    setenv("EXITOS_IOPATH", "pwrite", 1);
    setenv("EXITOS_TRACE", "1", 1);
    setenv("EXITOS_BPFTIME_NOOP", "7", 1);
    unsetenv("EXITOS_BPFTIME_NOHOOK");

    T_EQ(wb_bpftime_start("tests/unit/test_bpftime_backend.c"), 0,
         "fake transformer reaches active hook publication");
    T_EQ(g_fake_trace_at_publish, 1,
         "trace policy is frozen before the active hook is published");
    T_EQ(g_fake_noop_at_publish, 7,
         "noop policy is frozen before the active hook is published");
    T_EQ(g_fake_admission_at_publish, 1,
         "active hook publication observes an enabled admission gate");
    T_EQ(g_frontend_fallocate_create_calls, 1,
         "active bpftime creates one process-private fallocate runtime before publication");

    wb_bpftime_stop();
    T_EQ(g_frontend_fallocate_destroy_calls, 1,
         "active bpftime destroys its fallocate runtime after quiescence");
    g_fake_loader = 0;
    g_fake_orig_available = 0;
    unsetenv("EXITOS_FILES");
    unsetenv("EXITOS_IOPATH");
    unsetenv("EXITOS_TRACE");
    unsetenv("EXITOS_BPFTIME_NOOP");
}

int main(void)
{
    /* Production activation is hard-frozen: claiming any syscall would enter
     * a C ABI that cannot preserve all xstate a real syscall preserves.  The
     * private copy below still exercises dormant dispatch logic in isolation. */
    T_EQ(exitos_bpftime_claims(SYS_write), 0,      "hard freeze does not claim write");
    T_EQ(exitos_bpftime_claims(SYS_pwrite64), 0,   "hard freeze does not claim pwrite64");
    T_EQ(exitos_bpftime_claims(SYS_writev), 0,     "hard freeze does not claim writev");
    T_EQ(exitos_bpftime_claims(SYS_pwritev), 0,    "hard freeze does not claim pwritev");
#ifdef SYS_pwritev2
    T_EQ(exitos_bpftime_claims(SYS_pwritev2), 0,   "hard freeze does not claim pwritev2");
#endif
    T_EQ(exitos_bpftime_claims(SYS_fdatasync), 0,  "hard freeze does not claim fdatasync");
    T_EQ(exitos_bpftime_claims(SYS_fsync), 0,      "hard freeze does not claim fsync");
    T_EQ(exitos_bpftime_claims(SYS_openat), 0,     "hard freeze does not claim openat");
    T_EQ(exitos_bpftime_claims(SYS_close), 0,      "hard freeze does not claim close");
    T_EQ(exitos_bpftime_claims(SYS_dup), 0,        "hard freeze does not claim dup");
    T_EQ(exitos_bpftime_claims(SYS_fcntl), 0,      "hard freeze does not claim fcntl");
    T_EQ(exitos_bpftime_claims(SYS_ftruncate), 0,  "hard freeze does not claim ftruncate");
#ifdef SYS_fallocate
    T_EQ(exitos_bpftime_claims(SYS_fallocate), 0,
         "hard freeze does not claim fallocate");
#endif
#ifdef SYS_close_range
    T_EQ(exitos_bpftime_claims(SYS_close_range), 0,
         "hard freeze does not claim close_range");
#endif

    /* Claiming fsync does not mean taking it over: the hook must still invoke
     * the original syscall so the filesystem can persist metadata. */
    T_EQ(exitos_bpftime_claims(SYS_read), 0,  "does NOT claim read");
    T_EQ(exitos_bpftime_claims(SYS_mmap), 0,  "does NOT claim mmap");
    T_EQ(exitos_bpftime_claims(-1), 0,        "does NOT claim a negative syscall number");
    T_EQ(exitos_bpftime_claims(999999), 0,    "does NOT claim an out-of-range number");

    /* fdatasync: the core's internal negative errno is already in raw-syscall
     * ABI form.  It must not be converted to libc's -1 here and must not fall
     * through to an unrelated kernel sync. */
    reset_dispatch();
    g_sync_decision = EXITOS_TAKEOVER;
    g_sync_result = -EREMOTEIO;
    T_EQ(wb_syscall_hook_c(SYS_fdatasync, 41, 0, 0, 0, 0, 0), -EREMOTEIO,
         "fdatasync returns core negative errno in raw syscall ABI");
    T_EQ(g_orig_calls, 0,
         "fdatasync raw-debt failure cannot be masked by the original syscall");

    /* fsync is claimed only so raw debt can be handled.  Metadata still makes
     * the original syscall mandatory on every call. */
    reset_dispatch();
    g_raw_sync_rc = -EIO;
    g_orig_rc = 0;
    T_EQ(wb_syscall_hook_c(SYS_fsync, 41, 0, 0, 0, 0, 0), -EIO,
         "fsync returns raw FLUSH failure in raw syscall ABI");
    T_EQ(g_orig_calls, 1,
         "fsync still invokes the original metadata syscall after raw failure");
    T_EQ(g_note_calls, 1,
         "successful original fsync independently reports kernel debt paid");

    reset_dispatch();
    g_raw_sync_rc = 0;
    g_orig_rc = -ENOSPC;
    T_EQ(wb_syscall_hook_c(SYS_fsync, 41, 0, 0, 0, 0, 0), -ENOSPC,
         "fsync preserves original raw-syscall errno after raw success");
    T_EQ(g_orig_calls, 1, "fsync always invokes the original syscall");
    T_EQ(g_note_calls, 0,
         "failed original fsync does not clear kernel debt");

    reset_dispatch();
    T_EQ(wb_syscall_hook_c(SYS_fsync, 41, 0, 0, 0, 0, 0), 0,
         "fsync succeeds only after both durability domains succeed");
    T_EQ(g_orig_calls, 1, "successful fsync invoked the original syscall once");
    T_EQ(g_note_calls, 1, "successful fsync clears kernel debt once");

    reset_dispatch();
    g_in_hook = 1;
    g_orig_rc = 44;
    T_EQ(wb_syscall_hook_c(SYS_pwrite64, 41, 0x1000, 4096, 0, 0, 0), 44,
         "signal-style reentrant pwrite still reaches the kernel");
    T_EQ(g_global_poison_calls, 1,
         "signal-style reentrant pwrite poisons takeover state");

    reset_dispatch();
    g_in_hook = 1;
    g_orig_rc = 91;
    T_EQ(wb_syscall_hook_c(SYS_openat, AT_FDCWD,
                           (int64_t)(uintptr_t)"signal-open", O_RDONLY,
                           0, 0, 0), -EIO,
         "signal-style reentrant openat fails closed");
    T_EQ(g_orig_calls, 0,
         "reentrant user openat cannot masquerade as internal control I/O");
    T_EQ(g_global_poison_calls, 1,
         "reentrant user openat poisons takeover state");

    /* Registration reads sysfs through opendir() and fopen(); libc issues the
     * open and close from inside those wrappers, where the direct-call API
     * cannot reach, so under instruction rewriting they arrive here as nested
     * syscalls.  Classifying them as asynchronous application reentry poisoned
     * the context and made every backend-2 registration fail with -EPERM.
     * While the library is synchronously
     * inside its own control path, a nested syscall is ours by construction
     * and must pass through untouched; no raw write is in flight there, which
     * is the thing the poison exists to protect. */
    reset_dispatch();
    g_in_hook = 1;
    g_ctl_depth = 1;
    g_orig_rc = 91;
    T_EQ(wb_syscall_hook_c(SYS_openat, AT_FDCWD,
                           (int64_t)(uintptr_t)"/sys/class/block/x/wwid",
                           O_RDONLY, 0, 0, 0), 91,
         "a nested openat inside the library's own control path is forwarded");
    T_EQ(g_orig_calls, 1, "the control-path openat reaches the kernel");
    T_EQ(g_global_poison_calls, 0,
         "the control-path openat does not poison takeover state");

    reset_dispatch();
    g_in_hook = 1;
    g_ctl_depth = 1;
    g_orig_rc = 0;
    T_EQ(wb_syscall_hook_c(SYS_close, 7, 0, 0, 0, 0, 0), 0,
         "a nested close inside the control path is forwarded");
    T_EQ(g_global_poison_calls, 0,
         "the control-path close does not poison takeover state");

    /* The exemption must be exactly as wide as the control path: once it ends,
     * the same syscall is asynchronous reentry again. */
    reset_dispatch();
    g_in_hook = 1;
    g_ctl_depth = 0;
    g_orig_rc = 91;
    T_EQ(wb_syscall_hook_c(SYS_openat, AT_FDCWD,
                           (int64_t)(uintptr_t)"/sys/class/block/x/wwid",
                           O_RDONLY, 0, 0, 0), -EIO,
         "outside the control path the same openat still fails closed");
    T_EQ(g_global_poison_calls, 1,
         "outside the control path the same openat still poisons");

    reset_dispatch();
    g_in_hook = 1;
    T_EQ(wb_syscall_hook_c(SYS_fdatasync, 41, 0, 0, 0, 0, 0), -EIO,
         "signal-style reentrant fdatasync fails closed in raw syscall ABI");
    T_EQ(g_orig_calls, 0,
         "reentrant fdatasync cannot return a misleading kernel success");
    T_EQ(g_global_poison_calls, 1,
         "reentrant fdatasync poisons takeover state");
    g_in_hook = 0;
    g_sync_decision = EXITOS_PASS;
    g_orig_rc = 0;
    T_EQ(wb_syscall_hook_c(SYS_fdatasync, 41, 0, 0, 0, 0, 0), 0,
         "later ordinary fdatasync reaches the durability transaction");
    T_EQ(g_orig_calls, 1,
         "later ordinary fdatasync reaches the kernel once");

    reset_dispatch();
    g_in_hook = 1;
    T_EQ(wb_syscall_hook_c(SYS_fsync, 42, 0, 0, 0, 0, 0), -EIO,
         "signal-style reentrant fsync fails closed in raw syscall ABI");
    T_EQ(g_orig_calls, 0,
         "reentrant fsync cannot return a misleading kernel success");
    T_EQ(g_global_poison_calls, 1,
         "reentrant fsync poisons takeover state");
    g_in_hook = 0;
    T_EQ(wb_syscall_hook_c(SYS_fsync, 42, 0, 0, 0, 0, 0), 0,
         "later ordinary fsync pays the durability transaction");
    T_EQ(g_raw_sync_calls, 1,
         "later ordinary fsync pays raw debt after poison");
    T_EQ(g_orig_calls, 1,
         "later ordinary fsync reaches the kernel after raw debt");

    /* Scatter/gather calls are deliberately PASS-only, but they still create
     * kernel debt before the real syscall while the fd transaction is pinned.
     * Otherwise a concurrent fdatasync can clear the old debt immediately
     * before these bytes reach the kernel. */
    reset_dispatch();
    g_orig_rc = 123;
    T_EQ(wb_syscall_hook_c(SYS_writev, 41, 0x1000, 2, 0, 0, 0), 123,
         "writev is forwarded unchanged");
    T_EQ(g_kernel_write_notes, 1, "writev records kernel debt");
    T_EQ(g_write_note_seen_at_orig, 1,
         "writev records debt before entering the real syscall");

    reset_dispatch();
    g_orig_rc = 124;
    T_EQ(wb_syscall_hook_c(SYS_pwritev, 41, 0x1000, 2, 7, 0, 0), 124,
         "pwritev is forwarded unchanged");
    T_EQ(g_write_note_seen_at_orig, 1,
         "pwritev records debt before entering the real syscall");

#ifdef SYS_pwritev2
    reset_dispatch();
    g_orig_rc = 125;
    T_EQ(wb_syscall_hook_c(SYS_pwritev2, 41, 0x1000, 2, 7, 0, RWF_DSYNC), 125,
         "pwritev2 including durability flags is forwarded unchanged");
    T_EQ(g_write_note_seen_at_orig, 1,
         "pwritev2 records debt before entering the real syscall");
#endif

    /* Descriptor replacement is committed only after the real raw syscall
     * succeeds. A failed dup2/dup3 leaves the target meaning unchanged. */
    reset_dispatch();
    g_orig_rc = -EBADF;
    T_EQ(wb_syscall_hook_c(SYS_dup2, -1, 41, 0, 0, 0, 0), -EBADF,
         "failed raw dup2 returns its kernel errno");
    T_EQ(g_unregister_calls, 0,
         "failed raw dup2 preserves the target registration and debt");

    reset_dispatch();
    g_orig_rc = 41;
    T_EQ(wb_syscall_hook_c(SYS_dup3, 9, 41, O_CLOEXEC, 0, 0, 0), 41,
         "successful raw dup3 returns the new fd");
    T_EQ(g_unregister_calls, 1,
         "successful raw dup3 removes the overwritten target registration");

    reset_dispatch();
    g_raw_sync_rc = -EREMOTEIO;
    g_orig_rc = 41;
    T_EQ(wb_syscall_hook_c(SYS_dup2, 9, 41, 0, 0, 0, 0), -EREMOTEIO,
         "raw dup2 stops before rebind when alias debt preparation fails");
    T_EQ(g_orig_calls, 0,
         "raw dup2 does not execute after durability preparation failure");
    T_EQ(g_unregister_calls, 0,
         "raw dup2 preparation failure preserves both registrations");

    reset_dispatch();
    g_orig_rc = 57;
    T_EQ(wb_syscall_hook_c(SYS_dup, 9, 0, 0, 0, 0, 0), 57,
         "successful raw dup returns its new fd");
    T_EQ(g_unregister_calls, 1,
         "successful raw dup disables registered source fast path");

    reset_dispatch();
    g_orig_rc = 61;
    T_EQ(wb_syscall_hook_c(SYS_fcntl, 9, F_DUPFD_CLOEXEC, 60, 0, 0, 0), 61,
         "raw fcntl(F_DUPFD_CLOEXEC) returns its new alias fd");
    T_EQ(g_unregister_calls, 1,
         "raw fcntl alias disables registered source fast path");

    reset_dispatch();
    g_orig_rc = O_RDWR;
    T_EQ(wb_syscall_hook_c(SYS_fcntl, 9, F_GETFL, 0, 0, 0, 0), O_RDWR,
         "non-alias raw fcntl is forwarded unchanged");
    T_EQ(g_unregister_calls, 0,
         "non-alias raw fcntl does not alter registrations");

    reset_dispatch();
    g_orig_rc = 0;
    T_EQ(wb_syscall_hook_c(SYS_fcntl, 9, F_SETFL, O_NONBLOCK, 0, 0, 0), 0,
         "successful raw F_SETFL is forwarded");
    T_EQ(g_unregister_calls, 1,
         "successful raw F_SETFL disables the registered fast path");

    reset_dispatch();
    g_orig_rc = -EINVAL;
    T_EQ(wb_syscall_hook_c(SYS_fcntl, 9, F_SETFL, O_NONBLOCK, 0, 0, 0), -EINVAL,
         "failed raw F_SETFL preserves kernel errno");
    T_EQ(g_unregister_calls, 0,
         "failed raw F_SETFL preserves registration");

    reset_dispatch();
    g_raw_sync_rc = -EIO;
    T_EQ(wb_syscall_hook_c(SYS_fcntl, 9, F_SETFL, O_NONBLOCK, 0, 0, 0), -EIO,
         "raw F_SETFL stops when debt preparation fails");
    T_EQ(g_orig_calls, 0,
         "raw F_SETFL does not mutate flags after preparation failure");

    reset_dispatch();
    g_orig_rc = -EIO;
    T_EQ(wb_syscall_hook_c(SYS_close, 41, 0, 0, 0, 0, 0), -EIO,
         "raw close preserves a late kernel EIO");
    T_EQ(g_orig_calls, 1, "raw close executed exactly once");
    T_EQ(g_unregister_calls, 1,
         "Linux late close error still removes released-fd registration");

    reset_dispatch();
    g_raw_sync_rc = -EREMOTEIO;
    T_EQ(wb_syscall_hook_c(SYS_close, 41, 0, 0, 0, 0, 0), -EREMOTEIO,
         "raw close reports pre-close debt FLUSH failure");
    T_EQ(g_orig_calls, 0,
         "raw close leaves fd open when pre-close debt cannot be paid");
    T_EQ(g_unregister_calls, 0,
         "pre-close durability failure retains registration for retry");

#ifdef SYS_close_range
    reset_dispatch();
    g_orig_rc = 0;
    T_EQ(wb_syscall_hook_c(SYS_close_range, 3, 100, 0, 0, 0, 0), 0,
         "raw close_range is forwarded after global preparation");
    T_EQ(g_global_poison_calls, 1,
         "raw close_range flushes all debt and poisons before mutation");
    T_EQ(g_orig_calls, 1, "raw close_range reaches the kernel once");

    reset_dispatch();
    g_raw_sync_rc = -EIO;
    T_EQ(wb_syscall_hook_c(SYS_close_range, 3, 100, 0, 0, 0, 0), -EIO,
         "raw close_range refuses unpaid raw debt");
    T_EQ(g_orig_calls, 0,
         "raw close_range does not recycle fds after preparation failure");
#endif

    /* Truncation pays old raw debt before it can release extents, then
     * invalidates before the syscall. Metadata debt exists only on success. */
    reset_dispatch();
    g_raw_sync_rc = -EIO;
    T_EQ(wb_syscall_hook_c(SYS_ftruncate, 41, 0, 0, 0, 0, 0), -EIO,
         "raw ftruncate refuses an unpaid preflight FLUSH");
    T_EQ(g_orig_calls, 0,
         "raw ftruncate does not release extents after FLUSH failure");
    T_EQ(g_truncate_decisions, 0,
         "failed preflight retains mapping for retry");

    /* Truncation invalidates before the syscall, but metadata debt exists only
     * if the kernel actually changed the inode. */
    reset_dispatch();
    g_orig_rc = 0;
    T_EQ(wb_syscall_hook_c(SYS_ftruncate, 41, 4096, 0, 0, 0, 0), 0,
         "successful raw ftruncate returns success");
    T_EQ(g_truncate_decisions, 1, "ftruncate invalidates its map before syscall");
    T_EQ(g_kernel_metadata_notes, 1,
         "successful raw ftruncate creates kernel/metadata debt");
    T_EQ(g_kernel_write_notes, 0,
         "successful raw ftruncate is not counted as a PASS data write");

    reset_dispatch();
    g_orig_rc = -EINVAL;
    T_EQ(wb_syscall_hook_c(SYS_ftruncate, 41, -1, 0, 0, 0, 0), -EINVAL,
         "failed raw ftruncate returns kernel errno");
    T_EQ(g_kernel_metadata_notes, 0,
         "failed raw ftruncate creates no kernel/metadata debt");

#ifdef SYS_fallocate
    reset_dispatch();
    g_raw_sync_rc = -EREMOTEIO;
    T_EQ(wb_syscall_hook_c(SYS_fallocate, 41,
                           FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                           0, 4096, 0, 0), -EREMOTEIO,
         "raw fallocate refuses unpaid raw debt");
    T_EQ(g_orig_calls, 0,
         "raw fallocate does not mutate extents after FLUSH failure");
    T_EQ(g_invalidate_calls, 0,
         "failed fallocate preflight retains mapping for retry");
    T_OK(strstr(g_prepare_outcome_line,
                "outcome=control-failed stage=flush rc=-121 return_rc=-121 "
                "return_errno=0 "
                "rdwr_alias=0") != NULL,
         "active bpftime logs a control failure before preparation: %s",
         g_prepare_outcome_line);

    reset_dispatch();
    g_orig_rc = 0;
    T_EQ(wb_syscall_hook_c(SYS_fallocate, 41,
                           FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                           0, 4096, 0, 0), 0,
         "raw fallocate proceeds after successful preflight");
    T_EQ(g_raw_sync_calls, 1, "raw fallocate pays raw debt first");
    T_EQ(g_invalidate_calls, 1,
         "raw fallocate fully invalidates mapping before syscall");
    T_EQ(g_orig_calls, 1, "raw fallocate reaches kernel exactly once");
    T_EQ(g_kernel_metadata_notes, 1,
         "successful raw fallocate creates metadata/kernel debt");
    T_OK(strstr(g_prepare_outcome_line,
                "outcome=native-skip stage=native rc=0 return_rc=0 "
                "return_errno=0 "
                "rdwr_alias=0") != NULL,
         "active bpftime logs every ordinary fallocate as native-skip: %s",
         g_prepare_outcome_line);

    reset_dispatch();
    g_orig_rc = -ENOSPC;
    T_EQ(wb_syscall_hook_c(SYS_fallocate, 41, 0, 0, 4096, 0, 0), -ENOSPC,
         "failed submitted fallocate preserves kernel errno");
    T_EQ(g_invalidate_calls, 1,
         "failed submitted fallocate remains conservatively invalidated");
    T_EQ(g_kernel_metadata_notes, 0,
         "failed submitted fallocate creates no metadata debt");

    reset_dispatch();
    g_fallocate_runtime =
        (struct exitos_frontend_fallocate_runtime *)(uintptr_t)0x8888;
    g_frontend_fallocate_takeover = 1;
    T_EQ(wb_syscall_hook_c(SYS_fallocate, 41, 0, 0, 8192, 0, 0), 0,
         "active bpftime exposes shared donor preparation as raw success");
    T_EQ(g_frontend_fallocate_exec_calls, 1,
         "active bpftime delegates fallocate exactly once to shared runtime");
    T_EQ(g_orig_fallocate_calls, 0,
         "active bpftime donor takeover skips original fallocate");
    T_OK(g_frontend_fallocate_ctl_depth > 0,
         "active bpftime donor I/O runs under control-depth recursion guard");
    T_OK(strstr(g_prepare_outcome_line,
                "PREPARE_OUTCOME v=1 frontend=bpftime") != NULL,
         "active bpftime emits the versioned fallocate outcome schema: %s",
         g_prepare_outcome_line);
    T_OK(strstr(g_prepare_outcome_line,
                "outcome=prepared stage=complete rc=0 return_rc=0 "
                "return_errno=0 "
                "rdwr_alias=1") != NULL,
         "active bpftime outcome records alias-backed preparation: %s",
         g_prepare_outcome_line);
    T_OK(strstr(g_prepare_ready_line,
                "fiemap=safe unsafe_flags=0") != NULL,
         "active bpftime readiness marker carries explicit safe-FIEMAP proof fields: %s",
         g_prepare_ready_line);
    g_frontend_fallocate_rc = -EREMOTEIO;
    g_prepare_outcome_line[0] = '\0';
    g_prepare_ready_line[0] = '\0';
    T_EQ(wb_syscall_hook_c(SYS_fallocate, 41, 0, 0, 8192, 0, 0),
         -EREMOTEIO,
         "active bpftime preserves shared negative errno raw ABI");
    T_EQ(g_orig_fallocate_calls, 0,
         "active bpftime configured donor failure remains fail closed");
    T_OK(strstr(g_prepare_outcome_line,
                "outcome=eligible-attempt-failed stage=donor-prepare "
                "rc=-121 return_rc=-121 return_errno=0 rdwr_alias=1") != NULL,
         "active bpftime logs failed eligible setup with stable diagnostics: %s",
         g_prepare_outcome_line);
    T_EQ(g_prepare_ready_line[0], '\0',
         "active bpftime failure emits no PREPARE_READY claim");
    g_fallocate_runtime = NULL;
#endif

    reset_dispatch();
    g_orig_rc = 0;
    T_EQ(wb_internal_ftruncate_call(41, 4096), 0,
         "bpftime internal ftruncate seam uses saved original syscall");
    T_EQ(g_orig_calls, 1,
         "bpftime internal ftruncate seam calls original exactly once");
#ifdef SYS_fallocate
    g_orig_calls = 0;
    T_EQ(wb_internal_fallocate_call(41, 0, 0, 4096), 0,
         "bpftime internal fallocate seam uses saved original syscall");
    T_EQ(g_orig_calls, 1,
         "bpftime internal fallocate seam calls original exactly once");
#endif

    reset_dispatch();
    g_fastpath = 0;
    g_orig_rc = 73;
    T_EQ(wb_syscall_hook_c(SYS_pwrite64, 41, 0x1000, 4096, 0, 0, 0), 73,
         "active bpftime FASTPATH=0 forwards pwrite directly");
    T_EQ(wb_syscall_hook_c(SYS_write, 41, 0x1000, 4096, 0, 0, 0), 73,
         "active bpftime FASTPATH=0 forwards write directly");
    T_EQ(wb_syscall_hook_c(SYS_writev, 41, 0x1000, 1, 0, 0, 0), 73,
         "active bpftime FASTPATH=0 forwards writev directly");
    T_EQ(wb_syscall_hook_c(SYS_pwritev, 41, 0x1000, 1, 0, 0, 0), 73,
         "active bpftime FASTPATH=0 forwards pwritev directly");
    T_EQ(wb_syscall_hook_c(SYS_fdatasync, 41, 0, 0, 0, 0, 0), 73,
         "active bpftime FASTPATH=0 forwards fdatasync directly");
    T_EQ(wb_syscall_hook_c(SYS_fsync, 41, 0, 0, 0, 0, 0), 73,
         "active bpftime FASTPATH=0 forwards fsync directly");
    T_EQ(g_orig_calls, 6,
         "FASTPATH=0 issues exactly one original syscall per timed operation");
    T_EQ(g_kernel_write_notes, 0,
         "FASTPATH=0 creates no PASS/write accounting");
    T_EQ(g_raw_sync_calls, 0,
         "FASTPATH=0 syncs do not enter raw durability lifecycle");

    test_bpftime_start_publication_order();

    /* get_call_hook is a reversible preflight.  A NULL original handler must
     * reject and unload before setup_syscall_tracer irreversibly rewrites any
     * executable text. */
    g_started = 0;
    g_so = NULL;
    g_ctx = NULL;
    g_config = NULL;
    g_fake_loader = 1;
    g_fake_csreg_calls = 0;
    g_fake_setup_calls = 0;
    g_fake_getk_calls = 0;
    g_fake_setk_calls = 0;
    g_fake_dlclose_calls = 0;
    setenv("EXITOS_FILES", "/tmp/exitos-bpftime-config-*", 1);
    setenv("EXITOS_IOPATH", "pwrite", 1);
    T_OK(wb_bpftime_start("tests/unit/test_bpftime_backend.c") != 0,
         "NULL original hook rejects active startup");
    T_EQ(g_fake_getk_calls, 1,
         "active startup queries the original handler once");
    T_EQ(g_fake_setup_calls, 0,
         "NULL original hook is refused before irreversible text rewriting");
    T_EQ(g_fake_csreg_calls, 0,
         "NULL original hook is refused before transformer setup begins");
    T_EQ(g_fake_setk_calls, 0,
         "failed preflight installs no Exitos hook");
    T_EQ(g_fake_dlclose_calls, 1,
         "pre-rewrite failure may safely unload the transformer");
    T_OK(g_config == NULL && g_ctx == NULL && g_so == NULL,
         "pre-rewrite failure releases all active-start ownership");
    g_fake_loader = 0;
    unsetenv("EXITOS_FILES");
    unsetenv("EXITOS_IOPATH");

    /* The opt-in active backend shares the exact same immutable environment
     * loader as preload.  Validate/build it before probing the transformer so
     * malformed mode/boolean settings have deterministic precedence; on this
     * missing-transformer path its owning config must be released exactly
     * once.  No dlopen or instruction rewriting is reached. */
    g_started = 0;
    g_so = NULL;
    g_ctx = NULL;
    g_config = NULL;
    g_bpftime_frontend_config_calls = 0;
    g_bpftime_frontend_destroy_calls = 0;
    setenv("EXITOS_FILES", "/tmp/exitos-bpftime-config-*", 1);
    setenv("EXITOS_IOPATH", "pwrite", 1);
    unsetenv("EXITOS_STRICT");
    unsetenv("EXITOS_VERIFY_IDENTITY");
    T_OK(wb_bpftime_start("/definitely/not/here.so") != 0,
         "active start fails when the transformer is missing");
    T_EQ(g_bpftime_frontend_config_calls, 1,
         "active bpftime start calls the shared config factory exactly once");
    T_EQ(g_bpftime_frontend_destroy_calls, 1,
         "missing-transformer failure destroys the shared config exactly once");
    T_EQ(wb_bpftime_started(), 0,
         "active missing-transformer failure leaves the backend stopped");
    T_OK(g_config == NULL && g_ctx == NULL,
         "active failure leaves no owned config or borrowed context behind");
    unsetenv("EXITOS_FILES");
    unsetenv("EXITOS_IOPATH");

    /* Availability must be reported, never assumed. The transformer needs root
     * (it maps a page at address 0) and a built .so; when either is missing the
     * backend must decline rather than half-install itself. */
    T_EQ(exitos_bpftime_available("/definitely/not/here.so"), 0,
         "reports unavailable for a missing transformer");
    T_EQ(exitos_bpftime_available(0), 0, "reports unavailable for a NULL path");

    /* Starting with no transformer must fail cleanly and leave nothing behind. */
    T_OK(exitos_bpftime_start("/definitely/not/here.so") != 0,
         "start() fails when the transformer is missing");
    T_EQ(exitos_bpftime_started(), 0, "a failed start leaves the backend not started");

    /* Stop must be safe whether or not start ever succeeded. */
    exitos_bpftime_stop();
    T_EQ(exitos_bpftime_started(), 0, "stop() is safe when never started");

    test_bpftime_shutdown_admission();

    T_DONE();
}
