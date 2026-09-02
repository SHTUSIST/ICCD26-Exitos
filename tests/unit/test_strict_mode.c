/* Strict mode (Exitos-S): the zero-length-write permission probe.
 *
 * Design under test: with strict mode on, every write that is about to be
 * taken over first issues a zero-length pwrite on the intercepted fd through
 * the internal real-syscall channel.  The kernel runs exactly its per-write
 * permission gate on that probe -- the FMODE_WRITE check and
 * security_file_permission, the hook SELinux and AppArmor use per I/O --
 * with no data-path side effect.  A refusal sends the write down the ordinary
 * path so the kernel itself reports the error, byte-identical to a system
 * without Exitos.
 *
 * Like test_fdatasync_takeover.c this file includes the production core and
 * preload frontend under private names.  Only iopath entry points and the
 * real-syscall pointers are replaced; registration state is built by hand so
 * no device or filesystem is touched. */
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
#include "exitos_maco.h"
#include "exitos_stats.h"

static unsigned g_seq;
static unsigned g_iopw_calls;
static unsigned g_iopw_seq;
static size_t g_iopw_len;
static int g_iopw_rc;

struct iopath;
static int mock_iopath_write_fn(struct iopath *p, uint64_t lba,
                                const void *buf, size_t len)
{
    (void)p; (void)lba; (void)buf;
    g_iopw_calls++;
    g_iopw_seq = ++g_seq;
    g_iopw_len = len;
    return g_iopw_rc;
}

static int mock_iopath_flush_fn(struct iopath *p) { (void)p; return 0; }
static void mock_iopath_close_fn(struct iopath *p) { (void)p; }

int wb_exitos_fdatasync_needed(int durability_policy);

#define iopath_write                         mock_iopath_write_fn
#define iopath_flush                         mock_iopath_flush_fn
#define iopath_close                         mock_iopath_close_fn
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
#include "../../src/intercept.c"
#undef iopath_write
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

#define exitos_frontend_config_from_env       wb_frontend_config_from_env_impl
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

/* The preload adapter must delegate environment interpretation to this one
 * shared seam.  Keep the implementation real, but count adapter calls around
 * it so a future copy of getenv/boolean parsing in preload.c cannot satisfy
 * the behavioral assertions accidentally. */
static unsigned g_preload_frontend_config_calls;
static int wb_frontend_config_from_env(struct exitos_frontend_config **out)
{
    g_preload_frontend_config_calls++;
    return wb_frontend_config_from_env_impl(out);
}

/* Include the LD_PRELOAD frontend too, so the EXITOS_STRICT environment
 * hookup can be exercised by re-running its init with a controlled
 * environment.  Its libc names become private to this executable. */
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
#define exitos_ctx_verify_identity          wb_exitos_ctx_verify_identity
#define exitos_ctx_strict                   wb_exitos_ctx_strict
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
#undef exitos_ctx_strict
#undef exitos_frontend_config_from_env
#undef exitos_frontend_config_destroy
#undef exitos_frontend_config_enabled
#undef exitos_frontend_config_files
#undef exitos_frontend_config_ctx
#undef exitos_frontend_config_path_selected

enum { TEST_FD = 41 };

/* The probe reaches the kernel through exitos_internal_pwrite_call, which the
 * included preload frontend routes through its o_pwrite pointer.  Swapping
 * that pointer is therefore exactly the seam a real frontend uses. */
static unsigned g_probe_calls;
static unsigned g_probe_seq;
static int g_probe_fd;
static size_t g_probe_len = (size_t)-1;
static off_t g_probe_off = -1;
static int g_probe_rc;
static int g_probe_errno;

static ssize_t mock_probe_pwrite(int fd, const void *buf, size_t len, off_t off)
{
    (void)buf;
    g_probe_calls++;
    g_probe_seq = ++g_seq;
    g_probe_fd = fd;
    g_probe_len = len;
    g_probe_off = off;
    if (g_probe_rc != 0) {
        errno = g_probe_errno;
        return -1;
    }
    return 0;
}

static void reset_mocks(void)
{
    g_seq = 0;
    g_iopw_calls = 0;
    g_iopw_seq = 0;
    g_iopw_len = 0;
    g_iopw_rc = 0;
    g_probe_calls = 0;
    g_probe_seq = 0;
    g_probe_fd = -1;
    g_probe_len = (size_t)-1;
    g_probe_off = -1;
    g_probe_rc = 0;
    g_probe_errno = 0;
    g_preload_frontend_config_calls = 0;
    o_pwrite = mock_probe_pwrite;
    g_enabled = 1;
    g_in_hook = 0;
    exitos_frontend_admission_enable();
}

/* A hand-built registration whose 4 KiB write at offset 0 qualifies for
 * takeover: mapped by the Maco structure, covered by one device interval,
 * aligned to the 512-byte logical block size, no append/size/identity gate. */
static struct exitos_ctx *write_ctx(void)
{
    struct exitos_ctx *c = wb_exitos_ctx_create();
    struct reg *r;

    if (!c)
        return NULL;
    c->r = calloc(1, sizeof(*c->r));
    if (!c->r) { wb_exitos_ctx_destroy(c); return NULL; }
    c->r[0] = calloc(1, sizeof(*c->r[0]));
    if (!c->r[0]) { wb_exitos_ctx_destroy(c); return NULL; }
    c->n = c->cap = 1;
    r = c->r[0];
    pthread_mutex_init(&r->mu, NULL);
    r->active = 1;
    r->fd = TEST_FD;
    r->io = (struct iopath *)(uintptr_t)1;
    r->g.lbs = 512;
    r->g.fs_bs = 4096;
    r->dur = EXITOS_DUR_NONE;
    r->m = maco_create(512);
    if (!r->m || maco_insert(r->m, 0, 1000, 1u << 20) != 0) {
        wb_exitos_ctx_destroy(c);
        return NULL;
    }
    r->iv = calloc(1, sizeof(*r->iv));
    if (!r->iv) { wb_exitos_ctx_destroy(c); return NULL; }
    r->iv[0].lo = 1000;
    r->iv[0].hi = 1000 + (1u << 20) / 512;
    r->niv = 1;
    g_ctx = c;
    return c;
}

static void drop_ctx(struct exitos_ctx *c)
{
    g_ctx = NULL;
    wb_exitos_ctx_destroy(c);
}

static void drop_preload_config(void)
{
    g_enabled = 0;
    g_ctx = NULL; /* borrowed; the owning config destroys it exactly once */
    wb_frontend_config_destroy(g_config);
    g_config = NULL;
}

int main(void)
{
    static unsigned char buf[4096] __attribute__((aligned(4096)));
    struct exitos_ctx *c;
    ssize_t res;
    int sres;
    uint64_t pass0;

    /* 1: fast mode never probes. */
    reset_mocks();
    c = write_ctx();
    T_OK(c != NULL, "constructed an in-memory takeover-capable registration");
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, sizeof buf, 0, &res),
         EXITOS_TAKEOVER, "fast mode takes the qualifying write over");
    T_EQ(res, (ssize_t)sizeof buf, "takeover reports the full byte count");
    T_EQ(g_iopw_calls, 1, "fast mode submitted exactly one raw write");
    T_EQ(g_probe_calls, 0, "fast mode never issues the permission probe");
    drop_ctx(c);

    /* 2: strict on, kernel allows: probe precedes the raw submit. */
    reset_mocks();
    c = write_ctx();
    wb_exitos_ctx_strict(c, 1);
    T_EQ(c->strict, 1, "exitos_ctx_strict arms strict mode");
    T_EQ(c->verify_identity, 1, "strict mode implies the fd identity check");
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, sizeof buf, 4096, &res),
         EXITOS_TAKEOVER, "strict mode still takes the write over when allowed");
    T_EQ(g_probe_calls, 1, "strict mode probes exactly once per taken-over write");
    T_EQ(g_probe_len, 0, "the probe is a zero-length write");
    T_EQ(g_probe_fd, TEST_FD, "the probe targets the intercepted fd");
    T_EQ((long long)g_probe_off, 4096, "the probe uses the write's offset");
    T_OK(g_probe_seq < g_iopw_seq, "the probe runs before the raw submit");
    drop_ctx(c);

    /* 3: strict on, kernel refuses: no raw write, ordinary path instead. */
    reset_mocks();
    c = write_ctx();
    wb_exitos_ctx_strict(c, 1);
    g_probe_rc = -1;
    g_probe_errno = EACCES;
    pass0 = exitos_stat_get(EXITOS_STAT_PASS);
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, sizeof buf, 0, &res),
         EXITOS_PASS, "a refused probe sends the write down the ordinary path");
    T_EQ(res, -999, "the refused write's result is left for the kernel to set");
    T_EQ(g_iopw_calls, 0, "no raw write is submitted after a refused probe");
    T_EQ(exitos_stat_get(EXITOS_STAT_PASS), pass0 + 1,
         "a refused probe counts as a PASS");
    T_EQ(c->r[0]->unflushed, 0,
         "a refused probe leaves no raw durability debt behind");
    T_EQ(c->r[0]->kernel_holds, 1,
         "the kernel-side reissue is tracked like every passed write");
    drop_ctx(c);

    /* 4: strict mode never probes fdatasync (the vanilla fdatasync path runs
     * no LSM file hook, so probing would be stricter than the baseline). */
    reset_mocks();
    c = write_ctx();
    wb_exitos_ctx_strict(c, 1);
    sres = -999;
    (void)wb_exitos_on_fdatasync(c, TEST_FD, &sres);
    T_EQ(g_probe_calls, 0, "fdatasync never issues the permission probe");
    drop_ctx(c);

    /* 5: unregistered fds neither probe nor submit. */
    reset_mocks();
    c = write_ctx();
    wb_exitos_ctx_strict(c, 1);
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD + 1, buf, sizeof buf, 0, &res),
         EXITOS_PASS, "an unregistered fd passes");
    T_EQ(g_probe_calls, 0, "an unregistered fd is never probed");
    drop_ctx(c);

    /* 6: the EXITOS_STRICT environment variable arms strict mode through the
     * preload frontend's init, exactly like EXITOS_VERIFY_IDENTITY. */
    reset_mocks();
    setenv("EXITOS_FILES", "/tmp/exitos-strict-test-*", 1);
    setenv("EXITOS_STRICT", "1", 1);
    exitos_preload_init();
    T_EQ(g_preload_frontend_config_calls, 1,
         "preload init delegates exactly once to the shared config loader");
    T_OK(g_ctx != NULL, "frontend armed a context from EXITOS_FILES");
    T_EQ(g_ctx ? g_ctx->strict : -1, 1,
         "EXITOS_STRICT=1 arms strict mode at frontend init");
    T_EQ(g_ctx ? g_ctx->verify_identity : -1, 1,
         "EXITOS_STRICT=1 also arms the fd identity check");
    drop_preload_config();

    /* Exact zero is off; presence alone must not select Exitos-S. */
    g_preload_frontend_config_calls = 0;
    setenv("EXITOS_STRICT", "0", 1);
    exitos_preload_init();
    T_EQ(g_ctx ? g_ctx->strict : -1, 0,
         "EXITOS_STRICT=0 leaves strict mode off");
    T_EQ(g_ctx ? g_ctx->verify_identity : -1, 0,
         "EXITOS_STRICT=0 does not implicitly enable identity verification");
    T_EQ(g_preload_frontend_config_calls, 1,
         "strict-zero preload init still uses one shared config load");
    drop_preload_config();

    /* Identity is an independent policy bit unless strict promotes it. */
    g_preload_frontend_config_calls = 0;
    unsetenv("EXITOS_STRICT");
    setenv("EXITOS_VERIFY_IDENTITY", "1", 1);
    exitos_preload_init();
    T_EQ(g_ctx ? g_ctx->strict : -1, 0,
         "identity-only configuration does not select Exitos-S");
    T_EQ(g_ctx ? g_ctx->verify_identity : -1, 1,
         "EXITOS_VERIFY_IDENTITY=1 enables identity verification");
    T_EQ(g_preload_frontend_config_calls, 1,
         "identity-one preload init uses one shared config load");
    drop_preload_config();

    g_preload_frontend_config_calls = 0;
    setenv("EXITOS_VERIFY_IDENTITY", "0", 1);
    exitos_preload_init();
    T_EQ(g_ctx ? g_ctx->strict : -1, 0,
         "EXITOS_VERIFY_IDENTITY=0 leaves strict mode off");
    T_EQ(g_ctx ? g_ctx->verify_identity : -1, 0,
         "EXITOS_VERIFY_IDENTITY=0 leaves identity verification off");
    T_EQ(g_preload_frontend_config_calls, 1,
         "identity-zero preload init uses one shared config load");
    drop_preload_config();

    unsetenv("EXITOS_STRICT");
    unsetenv("EXITOS_VERIFY_IDENTITY");
    unsetenv("EXITOS_FILES");

    T_DONE();
}
