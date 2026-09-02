/* Compiled as its own optimized translation unit by test_preload_fortify.sh.
 * Keeping this separate from preload.c is essential: this is the way a normal
 * application gets glibc's __open*_2 references. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 2
#endif

#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fd_count(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            n++;
    closedir(d);
    return n;
}

int main(int argc, char **argv)
{
    int flags, fd = -1, before, after;

    if (argc != 4)
        return 64;
    if (strcmp(argv[3], "normal") == 0)
        flags = O_RDWR | O_DIRECT;
    else if (strcmp(argv[3], "create") == 0)
        flags = O_RDWR | O_CREAT;
#ifdef O_TMPFILE
    else if (strcmp(argv[3], "tmpfile") == 0)
        flags = O_RDWR | O_TMPFILE;
#endif
    else
        return 65;

    before = fd_count();
    if (strcmp(argv[1], "open") == 0)
        fd = open(argv[2], flags);
    else if (strcmp(argv[1], "open64") == 0)
        fd = open64(argv[2], flags);
    else if (strcmp(argv[1], "openat") == 0)
        fd = openat(AT_FDCWD, argv[2], flags);
    else if (strcmp(argv[1], "openat64") == 0)
        fd = openat64(AT_FDCWD, argv[2], flags);
    else
        return 66;

    if (fd < 0) {
        perror("fortified open");
        return 1;
    }
    if (close(fd) != 0)
        return 2;
    after = fd_count();
    if (before < 0 || after != before) {
        fprintf(stderr, "fd leak: before=%d after=%d returned=%d\n",
                before, after, fd);
        return 3;
    }
    return 0;
}
