/* Buffered-fd coherence.
 *
 * The paper never conditions interception on O_DIRECT, and its MySQL
 * default-configuration evaluation implies buffered redo writes, so the
 * registration gate must accept buffered fds.  Accepting them creates the
 * page-cache hazard: a raw device write leaves stale clean pages that a
 * buffered reader would trust, and a dirty page written back later would
 * clobber raw data on the device.  The design (verified against v6.18-rc5
 * mm/fadvise.c + mm/truncate.c):
 *
 *   - takeover on a buffered registration requires kernel_holds == 0, so no
 *     dirty page can exist in the file when raw writes and invalidations run;
 *   - registration itself settles pre-existing state with one real fdatasync
 *     plus a whole-file POSIX_FADV_DONTNEED;
 *   - after every successful raw write the written range, widened to full
 *     page boundaries (DONTNEED deliberately preserves partial pages), is
 *     invalidated -- through an asynchronous queue when the invalidator
 *     thread runs, inline otherwise, and inline again when the queue is full.
 *
 * Includes the production core and preload frontend under private names. */
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

static unsigned g_iopw_calls;
static int g_iopw_rc;

struct iopath;
static int mock_iopath_write_fn(struct iopath *p, uint64_t lba,
                                const void *buf, size_t len)
{
    (void)p; (void)lba; (void)buf; (void)len;
    g_iopw_calls++;
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

enum { TEST_FD = 43 };

/* The invalidation seam.  A strong definition here overrides the weak default
 * in iopath.o, exactly as a frontend would. */
static unsigned g_fadv_calls;
static int g_fadv_fd;
static off_t g_fadv_off = -1;
static off_t g_fadv_len = -1;
static int g_fadv_advice = -1;
static off_t g_fadv_offs[8];
static off_t g_fadv_lens[8];

int exitos_internal_fadvise_call(int fd, off_t off, off_t len, int advice)
{
    if (g_fadv_calls < 8) {
        g_fadv_offs[g_fadv_calls] = off;
        g_fadv_lens[g_fadv_calls] = len;
    }
    g_fadv_calls++;
    g_fadv_fd = fd;
    g_fadv_off = off;
    g_fadv_len = len;
    g_fadv_advice = advice;
    return 0;
}

static unsigned g_kernel_fdatasync_calls;
static int g_kernel_fdatasync_rc;

static int mock_kernel_fdatasync(int fd)
{
    (void)fd;
    g_kernel_fdatasync_calls++;
    if (g_kernel_fdatasync_rc != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

ssize_t exitos_internal_pwrite_call(int fd, const void *buf, size_t len,
                                    off_t off)
{ (void)fd; (void)buf; (void)off; return len == 0 ? 0 : -1; }

int exitos_internal_fdatasync_call(int fd)
{
    return mock_kernel_fdatasync(fd);
}

static void reset_mocks(void)
{
    g_iopw_calls = 0;
    g_iopw_rc = 0;
    g_fadv_calls = 0;
    g_fadv_fd = -1;
    g_fadv_off = g_fadv_len = -1;
    g_fadv_advice = -1;
    memset(g_fadv_offs, 0, sizeof g_fadv_offs);
    memset(g_fadv_lens, 0, sizeof g_fadv_lens);
    g_kernel_fdatasync_calls = 0;
    g_kernel_fdatasync_rc = 0;
}

static struct exitos_ctx *write_ctx(int buffered)
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
    r->buffered = buffered;
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
    return c;
}

static void drop_ctx(struct exitos_ctx *c)
{
    wb_exitos_ctx_destroy(c);
}

int main(void)
{
    static unsigned char buf[8192] __attribute__((aligned(4096)));
    struct exitos_ctx *c;
    ssize_t res;
    int rc;

    /* 1: the registration gate accepts buffered fds and still refuses the
     * modes whose semantics the shortcut cannot carry. */
    {
        char tmpl[] = "/tmp/exitos-bufgate-XXXXXX";
        int fd = mkstemp(tmpl);
        int rofd;
        T_OK(fd >= 0, "created a scratch file for the gate tests");
        unlink(tmpl);
        c = wb_exitos_ctx_create();
        rc = wb_exitos_register_fd(c, fd);
        T_OK(rc != -EOPNOTSUPP,
             "a plain buffered fd is no longer refused for lacking O_DIRECT");
        rofd = open("/dev/null", O_RDONLY);
        T_EQ(wb_exitos_register_fd(c, rofd), -EBADF,
             "a read-only fd is still refused");
        close(rofd);
        rofd = open("/dev/null", O_WRONLY | O_APPEND);
        T_EQ(wb_exitos_register_fd(c, rofd), -EOPNOTSUPP,
             "an O_APPEND fd is still refused");
        close(rofd);
        rofd = open("/dev/null", O_WRONLY | O_SYNC);
        T_EQ(wb_exitos_register_fd(c, rofd), -EOPNOTSUPP,
             "an O_SYNC fd is still refused");
        close(rofd);
        close(fd);
        wb_exitos_ctx_destroy(c);
    }

    /* 2: inline invalidation after a taken-over write on a buffered fd,
     * widened to full page boundaries. */
    reset_mocks();
    c = write_ctx(1);
    T_OK(c != NULL, "constructed a buffered takeover-capable registration");
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, 4096, 4096, &res),
         EXITOS_TAKEOVER, "buffered registration takes a clean write over");
    T_EQ(g_iopw_calls, 1, "the raw write was submitted");
    T_EQ(g_fadv_calls, 1, "one invalidation follows the raw write");
    T_EQ(g_fadv_fd, TEST_FD, "invalidation targets the intercepted fd");
    T_OK(g_fadv_off == 4096 && g_fadv_len == 4096,
         "page-aligned range is invalidated exactly");
    T_EQ(g_fadv_advice, POSIX_FADV_DONTNEED, "advice is POSIX_FADV_DONTNEED");
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, 512, 512, &res),
         EXITOS_TAKEOVER, "sub-page buffered write is taken over");
    T_EQ(g_fadv_calls, 2, "sub-page write also invalidates");
    T_OK(g_fadv_off == 0 && g_fadv_len == 4096,
         "sub-page range is widened to cover its full page");
    drop_ctx(c);

    /* 3: O_DIRECT registrations never invalidate. */
    reset_mocks();
    c = write_ctx(0);
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, 4096, 0, &res),
         EXITOS_TAKEOVER, "direct registration takes the write over");
    T_EQ(g_fadv_calls, 0, "no invalidation for O_DIRECT registrations");
    drop_ctx(c);

    /* 4: a failed raw write invalidates nothing. */
    reset_mocks();
    c = write_ctx(1);
    g_iopw_rc = -EIO;
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, 4096, 0, &res),
         EXITOS_PASS, "failed raw write passes to the kernel");
    T_EQ(g_fadv_calls, 0, "failed raw write invalidates nothing");
    drop_ctx(c);

    /* 5: kernel-side debt blocks buffered takeover until a proven sync. */
    reset_mocks();
    c = write_ctx(1);
    c->r[0]->kernel_holds = 1;
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, 4096, 0, &res),
         EXITOS_PASS, "buffered takeover is refused while the kernel holds "
                      "an unsynced write (dirty pages may exist)");
    T_EQ(g_iopw_calls, 0, "no raw write while the kernel holds debt");
    wb_exitos_note_kernel_sync(c, TEST_FD);
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, 4096, 0, &res),
         EXITOS_TAKEOVER, "a proven kernel sync re-enables the shortcut");
    drop_ctx(c);

    /* 5b: an O_DIRECT registration keeps the old behavior -- kernel debt
     * does not block its takeover (no dirty pages can exist). */
    reset_mocks();
    c = write_ctx(0);
    c->r[0]->kernel_holds = 1;
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, 4096, 0, &res),
         EXITOS_TAKEOVER, "direct registration is not blocked by kernel debt");
    drop_ctx(c);

    /* 6: the asynchronous queue coalesces and drains. */
    reset_mocks();
    c = write_ctx(1);
    c->coh_thread_on = 1;               /* pretend the invalidator runs */
    res = -999;
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, 4096, 0, &res),
         EXITOS_TAKEOVER, "queued-mode write is taken over");
    T_EQ(wb_exitos_on_write(c, TEST_FD, buf, 4096, 4096, &res),
         EXITOS_TAKEOVER, "second adjacent write is taken over");
    T_EQ(g_fadv_calls, 0, "queued mode defers invalidation");
    coherence_drain(c);
    T_EQ(g_fadv_calls, 1, "adjacent queued ranges drain as one invalidation");
    T_OK(g_fadv_off == 0 && g_fadv_len == 8192,
         "the drained invalidation covers both writes");
    coherence_drain(c);
    T_EQ(g_fadv_calls, 1, "an empty queue drains nothing");
    c->coh_thread_on = 0;
    drop_ctx(c);

    /* 7: registration-time settlement for buffered fds. */
    reset_mocks();
    {
        struct reg fake;
        memset(&fake, 0, sizeof fake);
        fake.fd = TEST_FD;
        fake.buffered = 1;
        reg_buffered_init_sync(TEST_FD, &fake);
        T_EQ(g_kernel_fdatasync_calls, 1,
             "buffered registration settles with one real fdatasync");
        T_EQ(fake.kernel_holds, 0, "successful settlement leaves no debt");
        T_EQ(g_fadv_calls, 1, "settlement drops the whole file from cache");
        T_OK(g_fadv_off == 0 && g_fadv_len == 0,
             "whole-file invalidation uses the zero-length convention");
        reset_mocks();
        g_kernel_fdatasync_rc = -1;
        memset(&fake, 0, sizeof fake);
        fake.fd = TEST_FD;
        fake.buffered = 1;
        reg_buffered_init_sync(TEST_FD, &fake);
        T_EQ(fake.kernel_holds, 1,
             "failed settlement records kernel debt instead");
        T_EQ(g_fadv_calls, 0, "failed settlement drops nothing");
    }

    T_DONE();
}
