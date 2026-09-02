#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sched.h>
#include <stdatomic.h>

#include "exitos_frontend_admission.h"

/* A global refcount would put two locked RMW operations from every intercepted
 * call on one cache line, recreating the same-process thread bottleneck this
 * project just removed.  A thread chooses a shard once; its hot-path RMWs then
 * stay off other threads' cache lines in the normal case. */
#define EXITOS_FRONTEND_ADMISSION_SHARDS 64u

struct admission_shard {
    _Atomic unsigned refs;
} __attribute__((aligned(64)));

static _Atomic unsigned g_accepting;
static _Atomic unsigned g_next_shard;
static struct admission_shard g_shards[EXITOS_FRONTEND_ADMISSION_SHARDS];
static __thread unsigned
    __attribute__((tls_model("initial-exec"))) g_tls_shard_plus_one;

static struct admission_shard *thread_shard(void)
{
    unsigned shard = g_tls_shard_plus_one;

    if (shard == 0) {
        shard = atomic_fetch_add_explicit(&g_next_shard, 1,
                                          memory_order_relaxed) %
                    EXITOS_FRONTEND_ADMISSION_SHARDS + 1;
        g_tls_shard_plus_one = shard;
    }
    return &g_shards[shard - 1];
}

void exitos_frontend_admission_enable(void)
{
    atomic_store_explicit(&g_accepting, 1, memory_order_seq_cst);
}

int exitos_frontend_admission_enter(void)
{
    struct admission_shard *shard;

    /* The first load keeps an inert DSO at one ordinary atomic load per entry.
     * The increment plus second load close the check/use race with quiesce().
     * Sequential consistency is intentional: if quiesce observes zero before
     * our increment, our later accepting load must observe its close and abort;
     * if it observes our increment, it must wait for leave(). */
    if (!atomic_load_explicit(&g_accepting, memory_order_seq_cst))
        return 0;
    shard = thread_shard();
    atomic_fetch_add_explicit(&shard->refs, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&g_accepting, memory_order_seq_cst))
        return 1;
    atomic_fetch_sub_explicit(&shard->refs, 1, memory_order_seq_cst);
    return 0;
}

void exitos_frontend_admission_leave(void)
{
    struct admission_shard *shard = thread_shard();

    atomic_fetch_sub_explicit(&shard->refs, 1, memory_order_seq_cst);
}

void exitos_frontend_admission_quiesce(void)
{
    unsigned i;

    atomic_store_explicit(&g_accepting, 0, memory_order_seq_cst);
    for (;;) {
        int busy = 0;
        for (i = 0; i < EXITOS_FRONTEND_ADMISSION_SHARDS; ++i) {
            if (atomic_load_explicit(&g_shards[i].refs,
                                     memory_order_seq_cst) != 0) {
                busy = 1;
                break;
            }
        }
        if (!busy)
            return;
        sched_yield();
    }
}
