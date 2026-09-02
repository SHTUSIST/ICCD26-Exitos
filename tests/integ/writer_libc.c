/* An ordinary writer. It includes no Exitos header and knows nothing about the
 * interception layer; that is the point of the cross-process test. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char **argv){
    if (argc < 3) { fprintf(stderr, "usage: %s <file> <nblocks> [fsync]\n", argv[0]); return 2; }
    int n = atoi(argv[2]); size_t BS = 4096;
    int use_fsync = argc > 3 && strcmp(argv[3], "fsync") == 0;
    int fd = open(argv[1], O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (fallocate(fd, 0, 0, (off_t)BS * n) != 0) { /* non-fatal */ }
    void *buf; if (posix_memalign(&buf, 4096, BS) != 0) return 1;
    memset(buf, 0, BS);                       /* initialise extents first */
    for (int i = 0; i < n; i++)
        if (pwrite(fd, buf, BS, (off_t)i * BS) != (ssize_t)BS) { perror("prewrite"); return 1; }
    /* Durability point after preparation, as any log writer has: fallocate
     * above changed metadata the journal must commit, and only a real
     * fdatasync settles that.  The pattern writes below then form a clean
     * epoch of their own. */
    if (fdatasync(fd) != 0) { perror("prepare fdatasync"); return 1; }
    for (int i = 0; i < n; i++) {
        memset(buf, 0, BS);
        snprintf((char *)buf, BS, "EXITOS-BLOCK-%06d", i);
        if (pwrite(fd, buf, BS, (off_t)i * BS) != (ssize_t)BS) { perror("pwrite"); return 1; }
    }
    if ((use_fsync ? fsync(fd) : fdatasync(fd)) != 0) {
        perror(use_fsync ? "fsync" : "fdatasync");
        return 1;
    }
    close(fd); free(buf);
    printf("wrote %d blocks\n", n); return 0;
}
