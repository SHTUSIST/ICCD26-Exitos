#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* Deliberately observable constructor used only as a dlopen tripwire. */
__attribute__((constructor)) static void marker_ctor(void)
{
    const char *path = getenv("EXITOS_BPFTIME_MARKER_SENTINEL");
    static const char mark[] = "bpftime marker constructor ran\n";
    ssize_t written;
    int fd;

    if (!path || !*path)
        return;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    written = write(fd, mark, sizeof(mark) - 1);
    (void)written;
    (void)close(fd);
}
