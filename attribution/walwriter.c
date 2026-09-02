/* An ordinary write-ahead log writer. It includes no Exitos header and knows
 * nothing about the interception layer, so the same binary measures both the
 * baseline and the accelerated case and no difference can come from having used
 * a different program.
 *
 * One record per iteration: write it, then make it durable. That is the shape a
 * log has, and it is the shape the durability shortcut has to beat.
 *
 * WAL_PREALLOC=1 allocates the whole file and writes it once before timing, the
 * way a log file is laid out in advance; without it every record extends the
 * file, which forces the filesystem to allocate a block and commit a journal
 * transaction on the critical path.
 *
 * Usage:  walwriter <file> <records> [bytes-per-record]
 *   WAL_DIRECT=1    open with O_DIRECT
 *   WAL_PREALLOC=1  lay the file out in advance
 *   WAL_NOSYNC=1    skip the fdatasync, to separate its cost from the write's
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static int cmp(const void *a, const void *b)
{ double x=*(const double*)a, y=*(const double*)b; return x<y?-1:x>y?1:0; }

static double now_us(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec*1e6 + (double)t.tv_nsec/1e3; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <file> <records> [bytes]\n", argv[0]); return 2; }
    const char *path = argv[1];
    int n = atoi(argv[2]);
    size_t rec = argc > 3 ? (size_t)atoi(argv[3]) : 8192;
    int direct = getenv("WAL_DIRECT") != NULL;
    int prealloc = getenv("WAL_PREALLOC") != NULL;
    int nosync = getenv("WAL_NOSYNC") != NULL;

    int flags = O_RDWR | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0);
    int fd = open(path, flags, 0644);
    if (fd < 0) { perror("open"); return 1; }

    void *buf;
    if (posix_memalign(&buf, 4096, rec) != 0) return 1;
    memset(buf, 0x5A, rec);

    if (prealloc) {
        if (fallocate(fd, 0, 0, (off_t)rec * n) != 0) { perror("fallocate"); return 1; }
        /* fallocate leaves extents unwritten, and the first write to one still
         * costs a conversion. A log file that has been in use is past that, so
         * the whole range is written once before anything is timed. */
        for (int i = 0; i < n; i++)
            if (pwrite(fd, buf, rec, (off_t)i * (off_t)rec) != (ssize_t)rec)
                { perror("prewrite"); return 1; }
        if (fdatasync(fd) != 0) { perror("fdatasync"); return 1; }
    }

    double *s = malloc(sizeof(double) * (size_t)n);
    if (!s) return 1;
    int got = 0;
    for (int i = 0; i < n; i++) {
        double t0 = now_us();
        if (pwrite(fd, buf, rec, (off_t)i * (off_t)rec) != (ssize_t)rec) { perror("pwrite"); break; }
        if (!nosync && fdatasync(fd) != 0) { perror("fdatasync"); break; }
        s[got++] = now_us() - t0;
    }
    if (got == 0) return 1;
    qsort(s, (size_t)got, sizeof(double), cmp);
    printf("%-28s n=%-6d median=%7.2f  p10=%7.2f  p99=%8.2f us\n",
           prealloc ? (direct ? "O_DIRECT+fdatasync prealloc" : "buffered+fdatasync prealloc")
                    : (direct ? "O_DIRECT+fdatasync append"   : "buffered+fdatasync append"),
           got, s[got/2], s[got/10], s[got*99/100]);
    close(fd); free(s); free(buf);
    return 0;
}
