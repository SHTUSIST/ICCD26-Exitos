/* Background space preparer. See include/exitos_donor_async.h for why.
 *
 * Correctness is inherited: every actual donation goes through donor_extend(),
 * the synchronous path that is already covered by its own tests. This file adds
 * only the scheduling — who calls it, when, and on which thread — so that the
 * write path pays nothing.
 *
 * The write path touches exactly two things here, both lock-free:
 *   donor_async_runway()  - one atomic load
 *   donor_async_advance() - one atomic store, plus a condvar signal ONLY when
 *                           the runway has fallen below the low watermark
 * Neither issues a syscall. Every ioctl and every byte of initialisation
 * happens on the preparer thread.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "exitos_donor_async.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

struct donor_async {
    struct donor_pool *pool;
    int                fd;
    uint64_t           chunk;
    uint64_t           low_water;

    _Atomic uint64_t   prepared_to;   /* bytes of the file made ready       */
    _Atomic uint64_t   consumed_to;   /* how far the writer has reached     */
    _Atomic uint64_t   rounds;        /* donations completed                */
    _Atomic uint64_t   last_write;    /* most recent append size, for sizing */
    _Atomic int        stop;
    _Atomic int        last_error;   /* why preparation stopped, 0 while running */
    uint64_t           blk;          /* filesystem block size donor_extend wants */

    pthread_t          th;
    pthread_mutex_t    mu;
    pthread_cond_t     cv;
    int                started;
};

static uint64_t runway_of(const struct donor_async *a)
{
    uint64_t p = atomic_load_explicit(&a->prepared_to, memory_order_acquire);
    uint64_t c = atomic_load_explicit(&a->consumed_to, memory_order_relaxed);
    return p > c ? p - c : 0;
}

static void *preparer(void *arg)
{
    struct donor_async *a = arg;

    for (;;) {
        pthread_mutex_lock(&a->mu);
        while (!atomic_load_explicit(&a->stop, memory_order_acquire) &&
               runway_of(a) >= a->low_water)
            pthread_cond_wait(&a->cv, &a->mu);
        int stopping = atomic_load_explicit(&a->stop, memory_order_acquire);
        pthread_mutex_unlock(&a->mu);
        if (stopping)
            break;

        /* Donate the next chunk at the end of what is already prepared. The
         * synchronous path does the MOVE_EXT and initialises the range; that is
         * the expensive part, and it is happening here rather than under an
         * append, which is the entire point of this file. */
        /* Never donate below where the writer has reached. donor_extend swaps
         * the target's blocks for the donor's and then zeroes the moved range,
         * so donating over live bytes destroys them outright -- and the header
         * tells callers to write through the ordinary kernel path whenever the
         * runway is zero, which puts live data above prepared_to by design.
         * consumed_to is exactly where that data ends, so the floor is the
         * larger of the two, rounded up to a block because donor_extend refuses
         * an unaligned file offset. */
        uint64_t at = atomic_load_explicit(&a->prepared_to, memory_order_relaxed);
        uint64_t reached = atomic_load_explicit(&a->consumed_to, memory_order_acquire);
        if (reached > at)
            at = reached;
        if (a->blk && at % a->blk) {
            uint64_t pad = a->blk - (at % a->blk);
            /* Rounding up must not wrap. It did: after advance(UINT64_MAX) the
             * rounded offset came back as 0 and the preparer donated over the
             * head of the file -- the same destruction this rounding was added
             * to prevent, reintroduced one line later. */
            if (pad > UINT64_MAX - at) {
                atomic_store_explicit(&a->last_error, -EOVERFLOW,
                                      memory_order_release);
                break;
            }
            at += pad;
        }
        /* chunk == 0 means "size it from what is actually being written":
         * donor_next_chunk implements the paper's rule of four times the moving
         * average of observed write sizes. Deciding it here, on this thread,
         * keeps even that arithmetic off the append. */
        uint64_t want = a->chunk;
        if (want == 0) {
            uint64_t seen = atomic_load_explicit(&a->last_write, memory_order_relaxed);
            want = donor_next_chunk(a->pool, seen ? seen : 4096);
            if (want == 0) want = 4u << 20;
        }
        if (want > UINT64_MAX - at) {   /* the donated range would wrap too */
            atomic_store_explicit(&a->last_error, -EOVERFLOW, memory_order_release);
            break;
        }
        int64_t got = donor_extend(a->pool, a->fd, at, want);
        if (got > 0) {
            /* Store, do not accumulate: `at` may have been lifted above
             * prepared_to just now, and adding to the stale value would leave
             * prepared_to claiming space that was never prepared. */
            atomic_store_explicit(&a->prepared_to, at + (uint64_t)got,
                                  memory_order_release);
            atomic_fetch_add_explicit(&a->rounds, 1, memory_order_relaxed);
        } else {
            /* Record why we stopped. Leaving only a runway of 0 behind makes a
             * transient or caller-caused rejection (-EINVAL from a bad offset)
             * indistinguishable from a genuinely exhausted pool, and the caller
             * has no way to tell that preparation has quietly ended forever. */
            atomic_store_explicit(&a->last_error, got ? (int)got : -EIO,
                                  memory_order_release);
            break;
        }
    }
    return NULL;
}

struct donor_async *donor_async_start(struct donor_pool *p, int target_fd,
                                      uint64_t chunk, uint64_t low_water)
{
    struct donor_async *a;

    /* chunk may be 0: that selects moving-average sizing (see the header). */
    if (!p || target_fd < 0)
        return NULL;

    a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->pool = p;
    a->fd = target_fd;
    a->chunk = chunk;
    a->low_water = low_water ? low_water : (chunk ? chunk / 2 : (2u << 20));
    /* chunk == 1 makes chunk/2 truncate to 0, and a watermark of 0 satisfies the
     * wait predicate forever: the thread sleeps before donating anything and
     * advance() never signals it, so preparation silently never happens. */
    if (a->low_water == 0)
        a->low_water = 1;
    /* Preparation starts at the target's current end of file. Starting at 0
     * would donate over -- and zero -- whatever the file already holds; the
     * constructor is never told the file is empty and must not assume it. */
    {
        struct stat st;
        uint64_t start = 0;
        if (fstat(target_fd, &st) == 0 && st.st_size > 0)
            start = (uint64_t)st.st_size;
        a->blk = donor_pool_blocksize(p);
        if (a->blk && start % a->blk)
            start += a->blk - (start % a->blk);
        atomic_store(&a->prepared_to, start);
        atomic_store(&a->consumed_to, start);
    }
    atomic_store(&a->rounds, 0);
    atomic_store(&a->stop, 0);
    atomic_store(&a->last_error, 0);

    if (pthread_mutex_init(&a->mu, NULL) != 0) { free(a); return NULL; }
    if (pthread_cond_init(&a->cv, NULL) != 0) {
        pthread_mutex_destroy(&a->mu); free(a); return NULL;
    }
    if (pthread_create(&a->th, NULL, preparer, a) != 0) {
        pthread_cond_destroy(&a->cv); pthread_mutex_destroy(&a->mu); free(a); return NULL;
    }
    a->started = 1;
    return a;
}

int donor_async_last_error(const struct donor_async *a)
{
    return a ? atomic_load_explicit(&a->last_error, memory_order_acquire) : 0;
}

void donor_async_stop(struct donor_async *a)
{
    if (!a)
        return;
    if (a->started) {
        pthread_mutex_lock(&a->mu);
        atomic_store_explicit(&a->stop, 1, memory_order_release);
        pthread_cond_broadcast(&a->cv);
        pthread_mutex_unlock(&a->mu);
        pthread_join(a->th, NULL);
    }
    pthread_cond_destroy(&a->cv);
    pthread_mutex_destroy(&a->mu);
    free(a);
}

uint64_t donor_async_runway(const struct donor_async *a, uint64_t off)
{
    uint64_t p;
    if (!a)
        return 0;
    p = atomic_load_explicit(&a->prepared_to, memory_order_acquire);
    return p > off ? p - off : 0;
}

void donor_async_advance(struct donor_async *a, uint64_t off)
{
    uint64_t prev;
    if (!a)
        return;
    prev = atomic_exchange_explicit(&a->consumed_to, off, memory_order_relaxed);
    if (off > prev)
        atomic_store_explicit(&a->last_write, off - prev, memory_order_relaxed);
    /* Signal only when the runway is actually short: taking the lock on every
     * append would put contention back on the write path. */
    if (runway_of(a) < a->low_water) {
        pthread_mutex_lock(&a->mu);
        pthread_cond_signal(&a->cv);
        pthread_mutex_unlock(&a->mu);
    }
}

uint64_t donor_async_prepared_to(const struct donor_async *a)
{
    return a ? atomic_load_explicit(&a->prepared_to, memory_order_acquire) : 0;
}

uint64_t donor_async_rounds(const struct donor_async *a)
{
    return a ? atomic_load_explicit(&a->rounds, memory_order_relaxed) : 0;
}
