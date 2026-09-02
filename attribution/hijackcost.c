/* What does the interception itself cost, per intercepted call?
 *
 * This is the number that decides whether the whole design can pay for itself:
 * the shortcut saves a fixed amount of software work per write, and if getting
 * into the shortcut costs more than that, the result is negative however fast
 * the device path is. Measured against the same program doing the same writes
 * with nothing loaded at all.
 *
 * Modes are chosen by the environment the program is launched with, so one
 * binary measures every configuration and no measurement can differ because a
 * different program was used. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/hijackcost.dat";
    int n = argc > 2 ? atoi(argv[2]) : 20000;
    size_t BS = argc > 3 ? (size_t)atoi(argv[3]) : 4096;
    void *buf;
    double *s;
    struct timespec a, b;
    int fd, i;

    if (posix_memalign(&buf, 4096, BS) != 0) return 1;
    memset(buf, 0x5A, BS);
    s = malloc(sizeof(double) * (size_t)n);
    if (!s) return 1;

    fd = open(path, O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (fallocate(fd, 0, 0, (off_t)BS * n) != 0) { /* not fatal */ }
    /* Convert every extent before timing: a first write into an unwritten
     * extent is a metadata operation and would be measured as if it were the
     * cost of a write. */
    for (i = 0; i < n; i++)
        if (pwrite(fd, buf, BS, (off_t)i * (off_t)BS) != (ssize_t)BS) break;
    if (fdatasync(fd) != 0) { /* not fatal */ }

    for (i = 0; i < n; i++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        if (pwrite(fd, buf, BS, (off_t)i * (off_t)BS) != (ssize_t)BS) break;
        clock_gettime(CLOCK_MONOTONIC, &b);
        s[i] = (double)(b.tv_sec - a.tv_sec) * 1e6 +
               (double)(b.tv_nsec - a.tv_nsec) / 1e3;
    }
    close(fd);
    qsort(s, (size_t)i, sizeof(double), cmp);
    printf("n=%d p50=%.3fus p99=%.3fus mean=%.3fus\n", i,
           s[i / 2], s[(int)((double)i * 0.99)],
           ({ double t = 0; int k; for (k = 0; k < i; k++) t += s[k]; t / i; }));
    free(s); free(buf);
    return 0;
}
