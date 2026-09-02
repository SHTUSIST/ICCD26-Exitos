/* Separate optimized application TU: _FILE_OFFSET_BITS=64 may redirect the
 * source spelling ftruncate, while explicit users call ftruncate64 directly. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE 1
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int fd;
    if (argc != 3)
        return 64;
    fd = open(argv[2], O_RDWR | O_DIRECT);
    if (fd < 0)
        return 65;
    if (argv[1][0] == '6') {
        if (ftruncate64(fd, 0) != 0)
            return 66;
    } else if (ftruncate(fd, 0) != 0) {
        return 67;
    }
    return close(fd) == 0 ? 0 : 68;
}
