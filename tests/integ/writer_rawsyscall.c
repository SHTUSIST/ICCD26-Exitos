/* Same output, but every write and sync goes straight to the kernel through
 * inline assembly, so libc is never involved. LD_PRELOAD replaces libc symbols
 * and therefore cannot see these calls; a syscall-instruction rewriter can.
 * This is the control that separates the two interception backends. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

static long raw_pwrite(int fd, const void *b, size_t n, long off){
    long r;
    register long r10 __asm__("r10") = off;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "a"((long)SYS_pwrite64), "D"((long)fd), "S"(b), "d"((long)n), "r"(r10)
        : "rcx", "r11", "memory");
    return r;
}
static long raw_fdatasync(int fd){
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
        : "a"((long)SYS_fdatasync), "D"((long)fd) : "rcx", "r11", "memory");
    return r;
}
int main(int argc, char **argv){
    if (argc < 3) { fprintf(stderr, "usage: %s <file> <nblocks>\n", argv[0]); return 2; }
    int n = atoi(argv[2]); size_t BS = 4096;
    int fd = open(argv[1], O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (fallocate(fd, 0, 0, (off_t)BS * n) != 0) { /* non-fatal */ }
    void *buf; if (posix_memalign(&buf, 4096, BS) != 0) return 1;
    memset(buf, 0, BS);
    for (int i = 0; i < n; i++)
        if (raw_pwrite(fd, buf, BS, (long)i * (long)BS) != (long)BS) { fprintf(stderr,"prewrite\n"); return 1; }
    for (int i = 0; i < n; i++) {
        memset(buf, 0, BS);
        snprintf((char *)buf, BS, "EXITOS-BLOCK-%06d", i);
        if (raw_pwrite(fd, buf, BS, (long)i * (long)BS) != (long)BS) { fprintf(stderr,"pwrite\n"); return 1; }
    }
    if (raw_fdatasync(fd) != 0) { fprintf(stderr,"fdatasync\n"); return 1; }
    close(fd); free(buf);
    printf("wrote %d blocks\n", n); return 0;
}
