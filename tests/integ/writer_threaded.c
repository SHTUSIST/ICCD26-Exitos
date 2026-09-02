/* Minimal reproducer for the crash seen in fio: a worker thread issuing
 * syscalls while the instruction-rewriting backend is loaded. fio was run with
 * --thread and died on a worker thread with rip = 0xffffffffffffffff, so the
 * first thing to separate is whether threads alone are enough. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

static const char *g_path;
static int g_n = 200;

static void *worker(void *arg)
{
    long id = (long)arg;
    int fd = open("/dev/null", O_WRONLY);
    int i;
    if (fd < 0) { fprintf(stderr, "t%ld open failed\n", id); return (void *)1; }
    for (i = 0; i < g_n; i++) {
        (void)getpid();
        if (write(fd, "x", 1) != 1) { fprintf(stderr, "t%ld write failed\n", id); close(fd); return (void *)1; }
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    int nthreads = argc > 1 ? atoi(argv[1]) : 2;
    pthread_t t[64];
    long i;
    void *rv;
    int bad = 0;

    /* 0 means: do the same work on the main thread only, creating no threads at
     * all. That is the case the rewriting survives. */
    if (nthreads < 0 || nthreads > 64) nthreads = 2;
    if (nthreads == 0) {
        void *rv0 = worker((void *)0L);
        printf("main thread only x %d syscalls: %s\n", g_n, rv0 ? "FAILED" : "ok");
        return rv0 ? 1 : 0;
    }
    g_path = argc > 2 ? argv[2] : NULL;

    for (i = 0; i < nthreads; i++)
        if (pthread_create(&t[i], NULL, worker, (void *)i) != 0) { perror("pthread_create"); return 2; }
    for (i = 0; i < nthreads; i++) { pthread_join(t[i], &rv); if (rv) bad = 1; }

    printf("%d threads x %d syscalls each: %s\n", nthreads, g_n, bad ? "FAILED" : "ok");
    return bad;
}
