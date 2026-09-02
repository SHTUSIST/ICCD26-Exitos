/* Keep one logical CPU out of hardware idle states without competing with the
 * benchmark whenever that benchmark is runnable.  SCHED_IDLE is weaker than
 * every ordinary SCHED_OTHER task; the `pause` loop therefore runs only in the
 * benchmark's I/O wait gaps on the same logical CPU.
 *
 * This is an experimental control, not a performance technique: it also
 * changes frequency and cache/front-end state, so campaigns must include an
 * SMT-sibling keeper as a separate control and interpret deltas accordingly. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stopped;

static void stop_now(int signo)
{
    (void)signo;
    stopped = 1;
}

int main(int argc, char **argv)
{
    char *end = NULL;
    long cpu;
    cpu_set_t set, effective;
    struct sched_param param;
    struct sigaction sa;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <logical-cpu>\n", argv[0]);
        return 2;
    }
    errno = 0;
    cpu = strtol(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' ||
        cpu < 0 || cpu >= CPU_SETSIZE || cpu > INT_MAX) {
        fprintf(stderr, "invalid logical CPU: %s\n", argv[1]);
        return 2;
    }

    CPU_ZERO(&set);
    CPU_SET((int)cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) {
        fprintf(stderr, "cannot pin to CPU %ld: %s\n", cpu, strerror(errno));
        return 1;
    }
    memset(&param, 0, sizeof param);
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        fprintf(stderr, "cannot enter SCHED_IDLE: %s\n", strerror(errno));
        return 1;
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = stop_now;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGTERM, &sa, NULL) != 0 ||
        sigaction(SIGHUP, &sa, NULL) != 0) {
        fprintf(stderr, "cannot install signal handlers: %s\n", strerror(errno));
        return 1;
    }

    CPU_ZERO(&effective);
    if (sched_getaffinity(0, sizeof effective, &effective) != 0 ||
        CPU_COUNT(&effective) != 1 || !CPU_ISSET((int)cpu, &effective) ||
        sched_getscheduler(0) != SCHED_IDLE) {
        fprintf(stderr, "effective affinity or scheduler policy differs from request\n");
        return 1;
    }

    printf("READY cpu=%ld policy=SCHED_IDLE\n", cpu);
    fflush(stdout);
    while (!stopped) {
#if defined(__i386__) || defined(__x86_64__)
        __asm__ __volatile__("pause" ::: "memory");
#else
        __asm__ __volatile__("" ::: "memory");
#endif
    }
    return 0;
}
