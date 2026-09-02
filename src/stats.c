#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "exitos_stats.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#ifdef EXITOS_STATS_GLOBAL_BASELINE

static _Atomic uint64_t g_counter[EXITOS_STAT_MAX];

void exitos_stat_inc(exitos_stat_id id)
{
    if ((int)id < 0 || id >= EXITOS_STAT_MAX)
        return;
    atomic_fetch_add_explicit(&g_counter[id], 1, memory_order_relaxed);
}

uint64_t exitos_stat_get(exitos_stat_id id)
{
    if ((int)id < 0 || id >= EXITOS_STAT_MAX)
        return 0;
    return atomic_load_explicit(&g_counter[id], memory_order_relaxed);
}

__attribute__((visibility("hidden")))
size_t exitos_stats_test_shard_capacity(void)
{
    return 0;
}

__attribute__((visibility("hidden")))
const void *exitos_stats_test_current_shard(void)
{
    return NULL;
}

#else

#define STATS_CACHELINE 64u
#define STATS_SHARD_COUNT 64u

/* One thread owns a shard at a time.  A relaxed RMW keeps a signal-handler
 * reentry on that same thread exact; unlike the old global counter, the
 * cacheline is not shared with ordinary increments from other threads. */
struct __attribute__((aligned(STATS_CACHELINE))) stats_shard {
    _Atomic uint64_t counter[EXITOS_STAT_MAX];
    _Atomic int claimed;
};

_Static_assert(_Alignof(struct stats_shard) >= STATS_CACHELINE,
               "stats shards must be cacheline aligned");
_Static_assert(sizeof(struct stats_shard) % STATS_CACHELINE == 0,
               "adjacent stats shards must not share a cacheline");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2,
               "signal-safe stats require lock-free native atomics");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2,
               "signal-safe stats require lock-free pointer atomics");

static struct stats_shard g_shards[STATS_SHARD_COUNT];
static struct stats_shard g_acquiring_marker;
static struct stats_shard g_overflow_marker;
static _Atomic uint64_t g_overflow[EXITOS_STAT_MAX];
/* The interposers already require Linux/GNU TLS.  initial-exec keeps even a
 * thread's first signal-path increment off the general-dynamic
 * __tls_get_addr() slow path, which may allocate and is not async-signal
 * safe. */
static __thread _Atomic(struct stats_shard *) g_tls_shard
    __attribute__((tls_model("initial-exec")));
static _Atomic int g_sharding_ready;
static _Atomic(void (*)(int)) g_test_cold_hook;

enum {
    STATS_TEST_AFTER_EMPTY_TLS_READ = 1,
    STATS_TEST_AFTER_OWNER_CLAIM = 2,
};

static void stats_test_cold_point(int point)
{
    void (*hook)(int) = atomic_load_explicit(&g_test_cold_hook,
                                              memory_order_relaxed);

    if (hook)
        hook(point);
}

static void stats_after_fork_child(void)
{
    struct stats_shard *survivor =
        atomic_load_explicit(&g_tls_shard, memory_order_relaxed);

    atomic_store_explicit(&g_tls_shard, &g_acquiring_marker,
                          memory_order_relaxed);
    for (size_t i = 0; i < STATS_SHARD_COUNT; i++)
        atomic_store_explicit(&g_shards[i].claimed, 0,
                              memory_order_relaxed);
    if (survivor && survivor != &g_acquiring_marker &&
        survivor != &g_overflow_marker) {
        atomic_store_explicit(&survivor->claimed, 1,
                              memory_order_relaxed);
        atomic_store_explicit(&g_tls_shard, survivor,
                              memory_order_relaxed);
    } else {
        atomic_store_explicit(&g_tls_shard, NULL, memory_order_relaxed);
    }
}

static void __attribute__((constructor(101))) stats_shard_init(void)
{
    if (pthread_atfork(NULL, NULL, stats_after_fork_child) == 0)
        atomic_store_explicit(&g_sharding_ready, 1, memory_order_release);
}

static struct stats_shard *stats_shard_acquire(void)
{
    if (!atomic_load_explicit(&g_sharding_ready, memory_order_acquire))
        return NULL;

    /* A slot belongs to one thread lifetime and is never recycled in this
     * process.  Retaining exited-thread counters makes aggregation exact and
     * avoids TID-reuse/ESRCH ABA protocols.  After 64 lifetime claims, new
     * threads use the exact sticky overflow counter. */
    for (size_t i = 0; i < STATS_SHARD_COUNT; i++) {
        int expected = 0;

        if (atomic_load_explicit(&g_shards[i].claimed,
                                 memory_order_relaxed))
            continue;
        if (atomic_compare_exchange_strong_explicit(
                &g_shards[i].claimed, &expected, 1,
                memory_order_acq_rel, memory_order_relaxed))
            return &g_shards[i];
    }
    return NULL;
}

static __attribute__((cold, noinline))
struct stats_shard *stats_shard_acquire_slow(void)
{
    struct stats_shard *shard = stats_shard_acquire();

    if (shard)
        stats_test_cold_point(STATS_TEST_AFTER_OWNER_CLAIM);
    return shard;
}

static __attribute__((cold, noinline))
void stats_stat_inc_cold(exitos_stat_id id)
{
    struct stats_shard *shard;
    struct stats_shard *expected;
    struct stats_shard *published;

retry:
    shard = atomic_load_explicit(&g_tls_shard, memory_order_relaxed);
    if (shard == &g_acquiring_marker || shard == &g_overflow_marker) {
        atomic_fetch_add_explicit(&g_overflow[id], 1,
                                  memory_order_relaxed);
        return;
    }
    if (!shard) {
        stats_test_cold_point(STATS_TEST_AFTER_EMPTY_TLS_READ);
        expected = NULL;
        if (!atomic_compare_exchange_strong_explicit(
                &g_tls_shard, &expected, &g_acquiring_marker,
                memory_order_relaxed, memory_order_relaxed))
            goto retry;
        shard = stats_shard_acquire_slow();
        published = shard ? shard : &g_overflow_marker;
        expected = &g_acquiring_marker;
        if (!atomic_compare_exchange_strong_explicit(
                &g_tls_shard, &expected, published,
                memory_order_relaxed, memory_order_relaxed))
            goto retry;
    }
    if (!shard) {
        atomic_fetch_add_explicit(&g_overflow[id], 1,
                                  memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&shard->counter[id], 1, memory_order_relaxed);
}

void exitos_stat_inc(exitos_stat_id id){
    struct stats_shard *shard;

    if ((int)id < 0 || id >= EXITOS_STAT_MAX) return;
    shard = atomic_load_explicit(&g_tls_shard, memory_order_relaxed);
    if (__builtin_expect(shard != NULL &&
                         shard != &g_acquiring_marker &&
                         shard != &g_overflow_marker, 1)) {
        atomic_fetch_add_explicit(&shard->counter[id], 1,
                                  memory_order_relaxed);
        return;
    }
    stats_stat_inc_cold(id);
}
/* Each atomic load is race-free and a quiescent total is exact.  A live get is
 * a monotonic per-counter sample, not a linearizable snapshot across shards;
 * dump likewise does not promise one cross-counter instant. */
uint64_t exitos_stat_get(exitos_stat_id id){
    uint64_t total;

    if ((int)id < 0 || id >= EXITOS_STAT_MAX) return 0;
    total = atomic_load_explicit(&g_overflow[id], memory_order_relaxed);
    for (size_t i = 0; i < STATS_SHARD_COUNT; i++)
        total += atomic_load_explicit(&g_shards[i].counter[id],
                                      memory_order_relaxed);
    return total;
}

__attribute__((visibility("hidden")))
size_t exitos_stats_test_shard_capacity(void)
{
    return STATS_SHARD_COUNT;
}

__attribute__((visibility("hidden")))
const void *exitos_stats_test_current_shard(void)
{
    struct stats_shard *shard =
        atomic_load_explicit(&g_tls_shard, memory_order_relaxed);

    return !shard || shard == &g_acquiring_marker ||
                   shard == &g_overflow_marker
               ? NULL : shard;
}

__attribute__((visibility("hidden")))
int exitos_stats_test_current_uses_overflow(void)
{
    return atomic_load_explicit(&g_tls_shard, memory_order_relaxed) ==
           &g_overflow_marker;
}

__attribute__((visibility("hidden")))
void exitos_stats_test_set_cold_hook(void (*hook)(int))
{
    atomic_store_explicit(&g_test_cold_hook, hook, memory_order_relaxed);
}

__attribute__((visibility("hidden")))
size_t exitos_stats_test_claimed_shards(void)
{
    size_t claimed = 0;

    for (size_t i = 0; i < STATS_SHARD_COUNT; i++)
        claimed += atomic_load_explicit(&g_shards[i].claimed,
                                        memory_order_relaxed) != 0;
    return claimed;
}

#endif

/* Expand a process id at dump time, not at constructor time.  fio's
 * process-mode jobs inherit one environment string and then fork; expanding
 * here gives every child its own file instead of letting the last destructor
 * truncate all earlier evidence.  Paths without %p retain their exact legacy
 * meaning. */
static int stats_path_expand(const char *path, char *out, size_t out_n)
{
    char pid[32];
    size_t used = 0;
    int pid_n;

    if (!path || !out || out_n == 0)
        return -1;
    pid_n = snprintf(pid, sizeof pid, "%ld", (long)getpid());
    if (pid_n <= 0 || (size_t)pid_n >= sizeof pid)
        return -1;
    while (*path) {
        const char *src = path;
        size_t n = 1;
        if (path[0] == '%' && path[1] == 'p') {
            src = pid;
            n = (size_t)pid_n;
            path += 2;
        } else {
            path++;
        }
        if (n >= out_n - used) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out + used, src, n);
        used += n;
    }
    out[used] = '\0';
    return 0;
}

int exitos_stats_dump(const char *path){
    char expanded[PATH_MAX];
    int saved_errno = 0;

    if (stats_path_expand(path, expanded, sizeof expanded) != 0) return -1;
    FILE *f = fopen(expanded, "w");
    if (!f) return -1;
    for (int i = 0; i < EXITOS_STAT_MAX; i++) {
        errno = 0;
        if (fprintf(f, "%llu\n",
                    (unsigned long long)exitos_stat_get(
                        (exitos_stat_id)i)) < 0) {
            saved_errno = errno ? errno : EIO;
            break;
        }
    }
    errno = 0;
    if (fclose(f) != 0 && !saved_errno)
        saved_errno = errno ? errno : EIO;
    if (saved_errno) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}
int exitos_stats_load(const char *path, uint64_t *out, int n){
    int saved_errno = 0;

    if (!path || !out || n < 1) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    for (int i = 0; i < n; i++) {
        unsigned long long v = 0;

        errno = 0;
        if (fscanf(f, "%llu", &v) != 1) {
            saved_errno = errno ? errno : EINVAL;
            break;
        }
        out[i] = (uint64_t)v;
    }
    errno = 0;
    if (fclose(f) != 0 && !saved_errno)
        saved_errno = errno ? errno : EIO;
    if (saved_errno) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}
