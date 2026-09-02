/* Observable interception counters.
 *
 * Without these a green checksum cannot tell "interception worked" from
 * "interception silently did nothing" — the cross-process test's failure
 * criteria F2 (armed run never intercepted) and F3 (intercepted when it should
 * have declined) are unprovable until the counts can be read from outside the
 * process that did the work. */
#include "tap.h"
#include "exitos_stats.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/* Test-only probes are hidden from the shared-library ABI.  Weak references
 * let the pre-sharding implementation run and report an honest behavioral
 * failure instead of stopping at link time. */
extern size_t exitos_stats_test_shard_capacity(void) __attribute__((weak));
extern const void *exitos_stats_test_current_shard(void) __attribute__((weak));
extern int exitos_stats_test_current_uses_overflow(void) __attribute__((weak));
extern void exitos_stats_test_set_cold_hook(void (*)(int))
    __attribute__((weak));
extern size_t exitos_stats_test_claimed_shards(void)
    __attribute__((weak));

enum { DUMP_WRITERS = 4, DUMP_ITERS = 50000 };

static int (*real_pthread_once_fn)(pthread_once_t *, void (*)(void));
static int (*real_pthread_setspecific_fn)(pthread_key_t, const void *);
extern int __pthread_once(pthread_once_t *, void (*)(void))
    __attribute__((weak));
extern int __pthread_setspecific(pthread_key_t, const void *)
    __attribute__((weak));
static _Thread_local volatile sig_atomic_t inside_stats_signal;
static _Atomic int unsafe_pthread_calls;

/* Forwarding wrappers make a first-use signal-path violation deterministic:
 * calling either API from exitos_stat_inc() is itself the failure, even when
 * this libc happens not to deadlock in that particular run. */
int pthread_once(pthread_once_t *once, void (*init)(void))
{
    if (inside_stats_signal)
        atomic_fetch_add_explicit(&unsafe_pthread_calls, 1,
                                  memory_order_relaxed);
    if (real_pthread_once_fn)
        return real_pthread_once_fn(once, init);
    return __pthread_once ? __pthread_once(once, init) : EINVAL;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (inside_stats_signal)
        atomic_fetch_add_explicit(&unsafe_pthread_calls, 1,
                                  memory_order_relaxed);
    if (real_pthread_setspecific_fn)
        return real_pthread_setspecific_fn(key, value);
    return __pthread_setspecific
               ? __pthread_setspecific(key, value)
               : EINVAL;
}

struct first_signal_probe {
    _Atomic int ready;
    _Atomic int done;
};

static _Atomic(struct first_signal_probe *) first_signal_probe;

static void first_increment_from_signal(int signo)
{
    struct first_signal_probe *probe =
        atomic_load_explicit(&first_signal_probe, memory_order_acquire);

    (void)signo;
    if (!probe)
        return;
    inside_stats_signal = 1;
    exitos_stat_inc(EXITOS_STAT_REFRESH);
    inside_stats_signal = 0;
    atomic_store_explicit(&probe->done, 1, memory_order_release);
}

static void *wait_for_first_increment_signal(void *opaque)
{
    struct first_signal_probe *probe = opaque;
    sigset_t one;

    sigemptyset(&one);
    sigaddset(&one, SIGUSR2);
    (void)pthread_sigmask(SIG_UNBLOCK, &one, NULL);
    atomic_store_explicit(&probe->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&probe->done, memory_order_acquire))
        sched_yield();
    (void)pthread_sigmask(SIG_BLOCK, &one, NULL);
    return NULL;
}

#ifndef EXITOS_STATS_GLOBAL_BASELINE
struct cold_reentry_probe {
    size_t claimed_shards;
    int uses_overflow;
};

static _Atomic int cold_hook_phase;
static _Atomic int cold_signal_increments;
static _Thread_local volatile sig_atomic_t inside_cold_hook;

static void increment_during_cold_acquire(int signo)
{
    (void)signo;
    exitos_stat_inc(EXITOS_STAT_REFRESH);
    atomic_fetch_add_explicit(&cold_signal_increments, 1,
                              memory_order_relaxed);
}

static void interrupt_cold_acquire(int point)
{
    if (inside_cold_hook ||
        point != atomic_load_explicit(&cold_hook_phase,
                                      memory_order_relaxed))
        return;
    inside_cold_hook = 1;
    (void)raise(SIGUSR2);
    inside_cold_hook = 0;
}

static void *run_interrupted_cold_acquire(void *opaque)
{
    struct cold_reentry_probe *probe = opaque;
    sigset_t one;

    sigemptyset(&one);
    sigaddset(&one, SIGUSR2);
    (void)pthread_sigmask(SIG_UNBLOCK, &one, NULL);
    exitos_stat_inc(EXITOS_STAT_REFRESH);
    probe->claimed_shards = exitos_stats_test_claimed_shards
                                ? exitos_stats_test_claimed_shards()
                                : 0;
    probe->uses_overflow = exitos_stats_test_current_uses_overflow
                               ? exitos_stats_test_current_uses_overflow()
                               : -1;
    (void)pthread_sigmask(SIG_BLOCK, &one, NULL);
    return NULL;
}

static int cold_reentry_phase_is_exact(int phase)
{
    pid_t child = fork();
    int status = 0;

    if (child == 0) {
        struct cold_reentry_probe probe = {0};
        pthread_t thread;
        uint64_t before = exitos_stat_get(EXITOS_STAT_REFRESH);
        size_t claimed_before = exitos_stats_test_claimed_shards();
        int ok;

        atomic_store_explicit(&cold_hook_phase, phase,
                              memory_order_relaxed);
        atomic_store_explicit(&cold_signal_increments, 0,
                              memory_order_relaxed);
        exitos_stats_test_set_cold_hook(interrupt_cold_acquire);
        ok = pthread_create(&thread, NULL, run_interrupted_cold_acquire,
                            &probe) == 0 &&
             pthread_join(thread, NULL) == 0;
        exitos_stats_test_set_cold_hook(NULL);
        ok = ok &&
             atomic_load_explicit(&cold_signal_increments,
                                  memory_order_relaxed) == 1 &&
             probe.claimed_shards == claimed_before + 1 &&
             probe.uses_overflow == 0 &&
             exitos_stat_get(EXITOS_STAT_REFRESH) == before + 2;
        _exit(ok ? 0 : 90 + phase);
    }
    return child > 0 && waitpid(child, &status, 0) == child &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#endif

static volatile sig_atomic_t signal_increments;

struct signal_race {
    pthread_t target;
    _Atomic int start;
    _Atomic int stop;
};

static void increment_from_signal(int signo)
{
    (void)signo;
    exitos_stat_inc(EXITOS_STAT_DECLINED);
    signal_increments++;
}

static void *send_increment_signals(void *opaque)
{
    struct signal_race *race = opaque;

    while (!atomic_load_explicit(&race->start, memory_order_acquire))
        sched_yield();
    while (!atomic_load_explicit(&race->stop, memory_order_acquire))
        (void)pthread_kill(race->target, SIGUSR1);
    return NULL;
}

struct dump_race {
    _Atomic int start;
    _Atomic int dump_started;
    _Atomic int done;
    uint64_t floor;
    uint64_t ceiling;
    char path[160];
    int ok;
    int saw_live_writer;
};

static void *increment_while_dumping(void *opaque)
{
    struct dump_race *race = opaque;

    while (!atomic_load_explicit(&race->start, memory_order_acquire))
        sched_yield();
    while (!atomic_load_explicit(&race->dump_started,
                                 memory_order_acquire))
        sched_yield();
    for (int i = 0; i < DUMP_ITERS; i++) {
        exitos_stat_inc(EXITOS_STAT_FAST_WRITE);
        if ((i & 255) == 0)
            sched_yield();
    }
    atomic_fetch_add_explicit(&race->done, 1, memory_order_release);
    return NULL;
}

static void *dump_while_incrementing(void *opaque)
{
    struct dump_race *race = opaque;
    uint64_t previous = race->floor;

    while (!atomic_load_explicit(&race->start, memory_order_acquire))
        sched_yield();
    atomic_store_explicit(&race->dump_started, 1, memory_order_release);
    do {
        uint64_t values[EXITOS_STAT_MAX] = {0};
        int live = atomic_load_explicit(&race->done,
                                        memory_order_acquire) < DUMP_WRITERS;

        if (live)
            race->saw_live_writer = 1;
        if (exitos_stats_dump(race->path) != 0 ||
            exitos_stats_load(race->path, values, EXITOS_STAT_MAX) != 0 ||
            values[EXITOS_STAT_FAST_WRITE] < previous ||
            values[EXITOS_STAT_FAST_WRITE] > race->ceiling) {
            race->ok = 0;
            break;
        }
        previous = values[EXITOS_STAT_FAST_WRITE];
    } while (atomic_load_explicit(&race->done,
                                  memory_order_acquire) < DUMP_WRITERS);
    return NULL;
}

#ifndef EXITOS_STATS_GLOBAL_BASELINE
struct shard_probe {
    _Atomic int *started;
    _Atomic int *release;
    const void *shard;
};

static void *probe_shard(void *opaque)
{
    struct shard_probe *probe = opaque;

    exitos_stat_inc(EXITOS_STAT_REFRESH);
    probe->shard = exitos_stats_test_current_shard
                       ? exitos_stats_test_current_shard()
                       : NULL;
    atomic_fetch_add_explicit(probe->started, 1, memory_order_release);
    while (!atomic_load_explicit(probe->release, memory_order_acquire))
        sched_yield();
    return NULL;
}

struct lifetime_probe {
    const void *shard;
    int overflow_after_first;
    int overflow_after_second;
};

static void *probe_lifetime_slot_then_exit(void *opaque)
{
    struct lifetime_probe *probe = opaque;

    exitos_stat_inc(EXITOS_STAT_REFRESH);
    probe->shard = exitos_stats_test_current_shard();
    probe->overflow_after_first =
        exitos_stats_test_current_uses_overflow();
    exitos_stat_inc(EXITOS_STAT_REFRESH);
    probe->overflow_after_second =
        exitos_stats_test_current_uses_overflow();
    return NULL;
}

/* Run in a child so prior unit-test threads cannot consume this test's finite
 * process-lifetime slot budget.  The atfork handler retains the caller's
 * inherited shard, frees orphan ownership, and preserves every counter. */
static int lifetime_slot_contract_is_exact(void)
{
    pid_t child = fork();
    int status = 0;

    if (child == 0) {
        enum { EXTRA_OVERFLOW_THREADS = 4 };
        size_t capacity = exitos_stats_test_shard_capacity();
        size_t claimed_before = exitos_stats_test_claimed_shards();
        size_t available = 0;
        uint64_t before = exitos_stat_get(EXITOS_STAT_REFRESH);
        int ok = capacity == 64 && claimed_before <= capacity;

        if (ok)
            available = capacity - claimed_before;

        for (size_t i = 0; ok &&
                           i < available + EXTRA_OVERFLOW_THREADS; i++) {
            struct lifetime_probe probe = {0};
            pthread_t thread;

            if (pthread_create(&thread, NULL,
                               probe_lifetime_slot_then_exit,
                               &probe) != 0 ||
                pthread_join(thread, NULL) != 0) {
                ok = 0;
                break;
            }
            if (i < available) {
                ok = probe.shard != NULL &&
                     !probe.overflow_after_first &&
                     !probe.overflow_after_second &&
                     exitos_stats_test_claimed_shards() ==
                         claimed_before + i + 1;
            } else {
                ok = probe.shard == NULL &&
                     probe.overflow_after_first &&
                     probe.overflow_after_second &&
                     exitos_stats_test_claimed_shards() == capacity;
            }
        }
        ok = ok &&
             exitos_stat_get(EXITOS_STAT_REFRESH) - before ==
                 2 * (available + EXTRA_OVERFLOW_THREADS);
        _exit(ok ? 0 : 94);
    }
    return child > 0 && waitpid(child, &status, 0) == child &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

struct overflow_probe {
    _Atomic int *started;
    _Atomic int *release;
    const void *shard;
    int overflow;
};

static void *probe_overflow(void *opaque)
{
    struct overflow_probe *probe = opaque;

    exitos_stat_inc(EXITOS_STAT_FAST_SYNC);
    probe->shard = exitos_stats_test_current_shard
                       ? exitos_stats_test_current_shard()
                       : NULL;
    probe->overflow = exitos_stats_test_current_uses_overflow
                          ? exitos_stats_test_current_uses_overflow()
                          : -1;
    atomic_fetch_add_explicit(probe->started, 1, memory_order_release);
    while (!atomic_load_explicit(probe->release, memory_order_acquire))
        sched_yield();
    return NULL;
}

struct fork_probe {
    int child_ok;
};

static void *fork_from_overflow(void *opaque)
{
    struct fork_probe *probe = opaque;
    uint64_t inherited;
    pid_t child;
    int status = 0;

    exitos_stat_inc(EXITOS_STAT_FAST_SYNC);
    if (!exitos_stats_test_current_uses_overflow ||
        !exitos_stats_test_current_uses_overflow())
        return NULL;
    inherited = exitos_stat_get(EXITOS_STAT_FAST_SYNC);
    child = fork();
    if (child == 0) {
        int ok = exitos_stat_get(EXITOS_STAT_FAST_SYNC) == inherited;

        exitos_stat_inc(EXITOS_STAT_FAST_SYNC);
        ok = ok && exitos_stats_test_current_shard &&
             exitos_stats_test_current_shard() != NULL &&
             !exitos_stats_test_current_uses_overflow() &&
             exitos_stat_get(EXITOS_STAT_FAST_SYNC) == inherited + 1;
        _exit(ok ? 0 : 91);
    }
    if (child > 0 && waitpid(child, &status, 0) == child &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0)
        probe->child_ok = 1;
    return NULL;
}
#endif

int main(void){
    real_pthread_once_fn = (int (*)(pthread_once_t *, void (*)(void)))
        dlsym(RTLD_NEXT, "pthread_once");
    real_pthread_setspecific_fn = (int (*)(pthread_key_t, const void *))
        dlsym(RTLD_NEXT, "pthread_setspecific");
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    T_EQ(exitos_stat_get(EXITOS_STAT_FAST_WRITE), 0, "counters start at zero");
    exitos_stat_inc(EXITOS_STAT_FAST_WRITE);
    exitos_stat_inc(EXITOS_STAT_FAST_WRITE);
    exitos_stat_inc(EXITOS_STAT_PASS);
    T_EQ(exitos_stat_get(EXITOS_STAT_FAST_WRITE), 2, "increments accumulate");
    T_EQ(exitos_stat_get(EXITOS_STAT_PASS), 1, "counters are independent");
    T_EQ(exitos_stat_get(EXITOS_STAT_MAX), 0, "out-of-range id reads zero, no crash");
    char p[128];
    snprintf(p, sizeof p, "/tmp/exitos_stats_test-%ld.txt", (long)getpid());
    T_EQ(exitos_stats_dump(p), 0, "dump writes the file");
    uint64_t v[EXITOS_STAT_MAX];
    T_EQ(exitos_stats_load(p, v, EXITOS_STAT_MAX), 0, "load reads it back");
    T_EQ(v[EXITOS_STAT_FAST_WRITE], 2, "fast-write survived the round trip");
    T_EQ(v[EXITOS_STAT_PASS], 1, "pass survived the round trip");
    unlink(p);
    T_EQ(exitos_stats_dump(0), -1, "NULL path refused");
    if (access("/dev/full", W_OK) == 0)
        T_EQ(exitos_stats_dump("/dev/full"), -1,
             "dump reports buffered write and close failures");
    else
        T_OK(1, "dump reports buffered write and close failures");
    {
        FILE *truncated;

        snprintf(p, sizeof p, "/tmp/exitos_stats_truncated-%ld.txt",
                 (long)getpid());
        truncated = fopen(p, "w");
        if (truncated) {
            (void)fprintf(truncated, "1\n");
            (void)fclose(truncated);
        }
        T_EQ(truncated ? exitos_stats_load(p, v, EXITOS_STAT_MAX) : -1,
             -1, "load rejects truncated counter evidence");
        unlink(p);
    }

#ifndef EXITOS_STATS_GLOBAL_BASELINE
    {
        struct sigaction action = {0}, old_action = {0};
        sigset_t one, old_mask;
        int action_installed;
        int mask_saved = 0;
        int hooks_present =
            exitos_stats_test_set_cold_hook != NULL &&
            exitos_stats_test_claimed_shards != NULL;

        sigemptyset(&one);
        sigaddset(&one, SIGUSR2);
        action.sa_handler = increment_during_cold_acquire;
        sigemptyset(&action.sa_mask);
        action_installed = sigaction(SIGUSR2, &action, &old_action) == 0;
        if (action_installed)
            mask_saved = pthread_sigmask(SIG_BLOCK, &one, &old_mask) == 0;

        T_OK(hooks_present && action_installed && mask_saved &&
                 cold_reentry_phase_is_exact(1),
             "signal after cold TLS read consumes exactly one shard");
        T_OK(hooks_present && action_installed && mask_saved &&
                 cold_reentry_phase_is_exact(2),
             "signal before cold TLS publication consumes exactly one shard");

        if (mask_saved)
            (void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        if (action_installed)
            (void)sigaction(SIGUSR2, &old_action, NULL);
    }

#endif

    {
        struct sigaction action = {0}, old_action = {0};
        struct first_signal_probe probe = { .ready = 0, .done = 0 };
        sigset_t one, old_mask;
        pthread_t thread;
        uint64_t before = exitos_stat_get(EXITOS_STAT_REFRESH);
        int action_installed = 0;
        int mask_saved = 0;
        int thread_started = 0;
        int ok = (real_pthread_once_fn != NULL || __pthread_once != NULL) &&
                 (real_pthread_setspecific_fn != NULL ||
                  __pthread_setspecific != NULL);

        sigemptyset(&one);
        sigaddset(&one, SIGUSR2);
        action.sa_handler = first_increment_from_signal;
        sigemptyset(&action.sa_mask);
        atomic_store_explicit(&unsafe_pthread_calls, 0, memory_order_relaxed);
        atomic_store_explicit(&first_signal_probe, &probe,
                              memory_order_release);
        if (ok) {
            action_installed =
                sigaction(SIGUSR2, &action, &old_action) == 0;
            ok = action_installed;
        }
        if (ok) {
            mask_saved =
                pthread_sigmask(SIG_BLOCK, &one, &old_mask) == 0;
            ok = mask_saved;
        }
        if (ok) {
            thread_started =
                pthread_create(&thread, NULL,
                               wait_for_first_increment_signal,
                               &probe) == 0;
            ok = thread_started;
        }
        while (ok && !atomic_load_explicit(&probe.ready,
                                            memory_order_acquire))
            sched_yield();
        if (thread_started) {
            int kill_ok = pthread_kill(thread, SIGUSR2) == 0;

            if (!kill_ok)
                atomic_store_explicit(&probe.done, 1,
                                      memory_order_release);
            ok = ok && kill_ok;
            ok = pthread_join(thread, NULL) == 0 && ok;
        }
        if (mask_saved)
            (void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        if (action_installed)
            (void)sigaction(SIGUSR2, &old_action, NULL);
        atomic_store_explicit(&first_signal_probe, NULL,
                              memory_order_release);

        T_OK(ok && exitos_stat_get(EXITOS_STAT_REFRESH) == before + 1,
             "a new thread's first stat increment can run in a signal handler");
        T_EQ(atomic_load_explicit(&unsafe_pthread_calls,
                                  memory_order_relaxed), 0,
             "first signal-path increment calls no pthread API");
    }

    {
        enum { OUTER_INCREMENTS = 5000000 };
        struct sigaction action = {0}, old_action = {0};
        sigset_t one, old_mask, pending;
        struct signal_race race = {
            .target = pthread_self(), .start = 0, .stop = 0,
        };
        pthread_t sender;
        uint64_t before, after;
        int action_installed;
        int mask_saved = 0;
        int sender_started = 0;
        int ok;

        sigemptyset(&one);
        sigaddset(&one, SIGUSR1);
        action.sa_handler = increment_from_signal;
        sigemptyset(&action.sa_mask);
        signal_increments = 0;
        action_installed = sigaction(SIGUSR1, &action, &old_action) == 0;
        ok = action_installed;
        if (ok) {
            mask_saved =
                pthread_sigmask(SIG_UNBLOCK, &one, &old_mask) == 0;
            ok = mask_saved;
        }
        if (ok) {
            sender_started =
                pthread_create(&sender, NULL, send_increment_signals,
                               &race) == 0;
            ok = sender_started;
        }
        before = exitos_stat_get(EXITOS_STAT_DECLINED);
        if (sender_started) {
            atomic_store_explicit(&race.start, 1, memory_order_release);
            for (int i = 0; i < OUTER_INCREMENTS; i++)
                exitos_stat_inc(EXITOS_STAT_DECLINED);
            atomic_store_explicit(&race.stop, 1, memory_order_release);
            ok = pthread_join(sender, NULL) == 0 && ok;
        }
        if (mask_saved)
            (void)pthread_sigmask(SIG_BLOCK, &one, NULL);
        if (mask_saved && sigpending(&pending) == 0 &&
            sigismember(&pending, SIGUSR1)) {
            int signo;
            (void)sigwait(&one, &signo);
        }
        after = exitos_stat_get(EXITOS_STAT_DECLINED);
        if (mask_saved)
            (void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        if (action_installed)
            (void)sigaction(SIGUSR1, &old_action, NULL);

        T_OK(ok && signal_increments > 0,
             "signal handler reenters the warmed stats shard");
        T_EQ(after - before,
             (uint64_t)OUTER_INCREMENTS + (uint64_t)signal_increments,
             "same-thread signal reentry loses no increments");
    }

    {
        pthread_t writers[DUMP_WRITERS], dumper;
        struct dump_race race = {
            .start = 0,
            .dump_started = 0,
            .done = 0,
            .floor = exitos_stat_get(EXITOS_STAT_FAST_WRITE),
            .ok = 1,
            .saw_live_writer = 0,
        };
        uint64_t values[EXITOS_STAT_MAX] = {0};
        int writers_started = 0;
        int dumper_started = 0;
        int ok = 1;

        race.ceiling = race.floor +
                       (uint64_t)DUMP_WRITERS * DUMP_ITERS;
        snprintf(race.path, sizeof race.path,
                 "/tmp/exitos_stats_concurrent-%ld.txt", (long)getpid());
        unlink(race.path);
        if (pthread_create(&dumper, NULL, dump_while_incrementing, &race) != 0)
            ok = 0;
        else
            dumper_started = 1;
        for (int i = 0; ok && i < DUMP_WRITERS; i++) {
            if (pthread_create(&writers[i], NULL, increment_while_dumping,
                               &race) != 0) {
                ok = 0;
                break;
            }
            writers_started++;
        }
        if (!ok) {
            atomic_store_explicit(&race.done, DUMP_WRITERS,
                                  memory_order_release);
            atomic_store_explicit(&race.dump_started, 1,
                                  memory_order_release);
        }
        atomic_store_explicit(&race.start, 1, memory_order_release);
        for (int i = 0; i < writers_started; i++)
            (void)pthread_join(writers[i], NULL);
        if (dumper_started)
            (void)pthread_join(dumper, NULL);

        T_OK(ok && race.ok && race.saw_live_writer,
             "dump is race-free and bounded while increments run");
        T_EQ(exitos_stat_get(EXITOS_STAT_FAST_WRITE), race.ceiling,
             "get exactly aggregates live and exited writer shards");
        T_EQ(exitos_stats_dump(race.path), 0,
             "final concurrent-counter dump succeeds");
        T_EQ(exitos_stats_load(race.path, values, EXITOS_STAT_MAX), 0,
             "final concurrent-counter dump reloads");
        T_EQ(values[EXITOS_STAT_FAST_WRITE], race.ceiling,
             "final dump exactly aggregates every writer increment");
        unlink(race.path);
    }

#ifdef EXITOS_STATS_GLOBAL_BASELINE
    T_OK(exitos_stats_test_shard_capacity &&
             exitos_stats_test_shard_capacity() == 0 &&
             exitos_stats_test_current_shard &&
             exitos_stats_test_current_shard() == NULL,
         "measurement baseline selects the global atomic implementation");
#else
    {
        enum { NTHREAD = 4 };
        pthread_t threads[NTHREAD];
        struct shard_probe probes[NTHREAD];
        _Atomic int started = 0;
        _Atomic int release = 0;
        int created = 0;
        int ok = exitos_stats_test_shard_capacity != NULL &&
                 exitos_stats_test_current_shard != NULL;

        if (ok)
            ok = exitos_stats_test_shard_capacity() > NTHREAD;
        if (ok) {
            for (int i = 0; i < NTHREAD; i++) {
                probes[i].started = &started;
                probes[i].release = &release;
                probes[i].shard = NULL;
                if (pthread_create(&threads[i], NULL, probe_shard,
                                   &probes[i]) != 0) {
                    ok = 0;
                    break;
                }
                created++;
            }
            while (atomic_load_explicit(&started, memory_order_acquire) <
                   created)
                sched_yield();
            for (int i = 0; ok && i < created; i++) {
                if (ok && (probes[i].shard == NULL ||
                           (uintptr_t)probes[i].shard % 64 != 0))
                    ok = 0;
                for (int j = 0; ok && j < i; j++)
                    if (probes[i].shard == probes[j].shard)
                        ok = 0;
            }
            atomic_store_explicit(&release, 1, memory_order_release);
            for (int i = 0; i < created; i++)
                (void)pthread_join(threads[i], NULL);
        }
        T_OK(ok, "hot-path threads use distinct cacheline-aligned shards");
    }

    T_OK(exitos_stats_test_shard_capacity &&
             exitos_stats_test_current_shard &&
             exitos_stats_test_current_uses_overflow &&
             exitos_stats_test_claimed_shards &&
             lifetime_slot_contract_is_exact(),
         "64 lifetime slots stay claimed after exit; overflow is sticky and exact");

    if (exitos_stats_test_shard_capacity &&
        exitos_stats_test_current_shard) {
        size_t capacity = exitos_stats_test_shard_capacity();
        size_t requested = capacity + 8;
        pthread_t *threads = calloc(requested, sizeof *threads);
        struct overflow_probe *probes = calloc(requested, sizeof *probes);
        _Atomic int started = 0;
        _Atomic int release = 0;
        uint64_t before = exitos_stat_get(EXITOS_STAT_FAST_SYNC);
        size_t created = 0;
        size_t overflow = 0;
        int ok = threads != NULL && probes != NULL;

        while (ok && created < requested) {
            probes[created].started = &started;
            probes[created].release = &release;
            probes[created].shard = NULL;
            probes[created].overflow = -1;
            if (pthread_create(&threads[created], NULL, probe_overflow,
                               &probes[created]) != 0) {
                ok = 0;
                break;
            }
            created++;
        }
        while (atomic_load_explicit(&started, memory_order_acquire) <
               (int)created)
            sched_yield();
        for (size_t i = 0; i < created; i++) {
            if (probes[i].overflow == 1 && probes[i].shard == NULL)
                overflow++;
        }
        atomic_store_explicit(&release, 1, memory_order_release);
        for (size_t i = 0; i < created; i++)
            (void)pthread_join(threads[i], NULL);

        T_OK(ok && created == requested && overflow >= 9,
             "slot overflow uses an explicit sticky fallback shard");
        T_EQ(exitos_stat_get(EXITOS_STAT_FAST_SYNC) - before, created,
             "slot overflow preserves every increment exactly");
        free(probes);
        free(threads);
    } else {
        T_OK(0, "slot overflow uses an explicit sticky fallback shard");
        T_OK(0, "slot overflow preserves every increment exactly");
    }


    if (exitos_stats_test_shard_capacity &&
        exitos_stats_test_current_shard &&
        exitos_stats_test_current_uses_overflow) {
        size_t blockers = exitos_stats_test_shard_capacity() - 1;
        pthread_t *threads = calloc(blockers, sizeof *threads);
        struct overflow_probe *probes = calloc(blockers, sizeof *probes);
        _Atomic int started = 0;
        _Atomic int release = 0;
        uint64_t before = exitos_stat_get(EXITOS_STAT_FAST_SYNC);
        size_t created = 0;
        pthread_t forker;
        struct fork_probe fork_probe = { .child_ok = 0 };
        int forker_started = 0;
        int ok = threads != NULL && probes != NULL;

        while (ok && created < blockers) {
            probes[created].started = &started;
            probes[created].release = &release;
            probes[created].shard = NULL;
            probes[created].overflow = -1;
            if (pthread_create(&threads[created], NULL, probe_overflow,
                               &probes[created]) != 0) {
                ok = 0;
                break;
            }
            created++;
        }
        while (atomic_load_explicit(&started, memory_order_acquire) <
               (int)created)
            sched_yield();
        if (ok && created == blockers &&
            pthread_create(&forker, NULL, fork_from_overflow,
                           &fork_probe) == 0) {
            forker_started = 1;
            (void)pthread_join(forker, NULL);
        } else {
            ok = 0;
        }
        atomic_store_explicit(&release, 1, memory_order_release);
        for (size_t i = 0; i < created; i++)
            (void)pthread_join(threads[i], NULL);

        T_OK(ok && forker_started && fork_probe.child_ok,
             "fork child reclaims orphaned slots and leaves sticky overflow");
        T_EQ(exitos_stat_get(EXITOS_STAT_FAST_SYNC) - before,
             blockers + (forker_started ? 1 : 0),
             "fork keeps the parent's exact pre-fork counter state");
        free(probes);
        free(threads);
    } else {
        T_OK(0, "fork child reclaims orphaned slots and leaves sticky overflow");
        T_OK(0, "fork keeps the parent's exact pre-fork counter state");
    }
#endif

    /* fio process-mode jobs inherit one EXITOS_STATS string. A literal path
     * lets the last exiting process truncate every earlier process's counts,
     * so a zero-valued parent can make a fully intercepted run look inert (or
     * vice versa).  %p must expand independently in each dumping process. */
    {
        char templ[160], child_path[2][160], literal_path[160];
        pid_t child[2];
        uint64_t inherited = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
        int child_ok = 1;

        snprintf(templ, sizeof templ, "/tmp/exitos_stats_fork-%%p-%ld.txt",
                 (long)getpid());
        snprintf(literal_path, sizeof literal_path,
                 "/tmp/exitos_stats_fork-%%p-%ld.txt", (long)getpid());
        unlink(literal_path);
        for (int i = 0; i < 2; i++) {
            child[i] = fork();
            if (child[i] == 0) {
                for (int k = 0; k <= i; k++)
                    exitos_stat_inc(EXITOS_STAT_FAST_WRITE);
                _exit(exitos_stats_dump(templ) == 0 ? 0 : 90);
            }
            if (child[i] < 0)
                child_ok = 0;
        }
        for (int i = 0; i < 2; i++) {
            int st = 0;
            if (child[i] <= 0 || waitpid(child[i], &st, 0) != child[i] ||
                !WIFEXITED(st) || WEXITSTATUS(st) != 0)
                child_ok = 0;
        }
        T_OK(child_ok, "two forked dumpers complete successfully");
        for (int i = 0; i < 2; i++) {
            uint64_t cv[EXITOS_STAT_MAX] = {0};
            snprintf(child_path[i], sizeof child_path[i],
                     "/tmp/exitos_stats_fork-%ld-%ld.txt",
                     (long)child[i], (long)getpid());
            T_EQ(exitos_stats_load(child_path[i], cv, EXITOS_STAT_MAX), 0,
                 "each child gets a distinct PID-expanded stats file");
            T_EQ(cv[EXITOS_STAT_FAST_WRITE], inherited + 1 + i,
                 "each PID-expanded file preserves that child's exact count");
            unlink(child_path[i]);
        }
        T_OK(access(literal_path, F_OK) != 0,
             "the literal percent-p template is never created");
        unlink(literal_path);
    }
    T_DONE();
}
