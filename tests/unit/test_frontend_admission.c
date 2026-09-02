/* Shared frontend admission/quiescence contract.
 *
 * This is deliberately device-free.  One caller holds an admission while a
 * second thread begins shutdown; shutdown must close admission immediately and
 * must not return until the admitted caller leaves. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <sched.h>

#include "tap.h"

int  exitos_frontend_admission_enter(void);
void exitos_frontend_admission_leave(void);
void exitos_frontend_admission_enable(void);
void exitos_frontend_admission_quiesce(void);

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int worker_entered;
static int release_worker;
static int quiesce_done;

static void *admitted_worker(void *unused)
{
    (void)unused;
    if (!exitos_frontend_admission_enter())
        return (void *)1;

    pthread_mutex_lock(&mu);
    worker_entered = 1;
    pthread_cond_broadcast(&cv);
    while (!release_worker)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);

    exitos_frontend_admission_leave();
    return NULL;
}

static void *quiescer(void *unused)
{
    (void)unused;
    exitos_frontend_admission_quiesce();
    pthread_mutex_lock(&mu);
    quiesce_done = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void)
{
    pthread_t worker, stop;
    void *worker_result = (void *)1;
    int observed_closed = 0;

    T_EQ(exitos_frontend_admission_enter(), 0,
         "admission starts closed before a frontend publishes its context");
    exitos_frontend_admission_enable();
    T_EQ(pthread_create(&worker, NULL, admitted_worker, NULL), 0,
         "started an admitted frontend caller");

    pthread_mutex_lock(&mu);
    while (!worker_entered)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);

    T_EQ(pthread_create(&stop, NULL, quiescer, NULL), 0,
         "started frontend quiescence");
    for (int i = 0; i < 100000; ++i) {
        if (!exitos_frontend_admission_enter()) {
            observed_closed = 1;
            break;
        }
        exitos_frontend_admission_leave();
        sched_yield();
    }
    T_OK(observed_closed, "quiescence closes admission before it drains callers");

    pthread_mutex_lock(&mu);
    T_EQ(quiesce_done, 0,
         "quiescence cannot return while an admitted caller still holds ctx");
    release_worker = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);

    T_EQ(pthread_join(worker, &worker_result), 0, "joined admitted caller");
    T_OK(worker_result == NULL, "caller entered the enabled frontend");
    T_EQ(pthread_join(stop, NULL), 0, "joined frontend quiescence");
    T_EQ(quiesce_done, 1, "quiescence returns after the last caller leaves");
    T_EQ(exitos_frontend_admission_enter(), 0,
         "admission remains closed after quiescence");

    exitos_frontend_admission_enable();
    T_EQ(exitos_frontend_admission_enter(), 1,
         "a fully quiesced gate can be enabled for a new frontend lifetime");
    exitos_frontend_admission_leave();
    exitos_frontend_admission_quiesce();

    T_DONE();
}
