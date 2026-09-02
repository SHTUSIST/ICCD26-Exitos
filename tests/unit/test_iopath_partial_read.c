/* Pure regular-file regression for pread_all(): force every pread to complete
 * only 1024 bytes so pointer/offset advancement is observable. */
#include "tap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "exitos_iopath.h"

static unsigned partial_pread_calls;

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    /* Keep this ordinary-file test independent of host O_DIRECT granularity. */
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags & ~O_DIRECT, mode);
}

ssize_t pread(int fd, void *buf, size_t len, off_t off)
{
    size_t partial = len > 1024 ? 1024 : len;
    partial_pread_calls++;
    return (ssize_t)syscall(SYS_pread64, fd, buf, partial, off);
}

int main(void)
{
    char path[] = "/tmp/exitos-iopath-partial-XXXXXX";
    unsigned char expected[4096];
    void *raw = NULL;
    struct iopath *io;
    struct iopath *retained_io = NULL;
    int fd = mkstemp(path);

    for (size_t i = 0; i < sizeof expected; i++)
        expected[i] = (unsigned char)(i % 251u);
    T_OK(fd >= 0, "created ordinary-file partial-read fixture");
    if (fd >= 0) {
        T_EQ(write(fd, expected, sizeof expected), (ssize_t)sizeof expected,
             "wrote a complete patterned native block");
        close(fd);
    }
    T_EQ(posix_memalign(&raw, 4096, sizeof expected), 0,
         "allocated an aligned destination block");
    if (raw) memset(raw, 0xcc, sizeof expected);

    io = iopath_open(path, IOPATH_PWRITE, 4096);
    T_OK(io != NULL, "opened ordinary storage through the real pwrite backend");
    partial_pread_calls = 0;
    if (io && raw) {
        T_EQ(iopath_read(io, 0, raw, sizeof expected), 0,
             "pread_all completes a block across multiple partial reads");
        T_EQ(partial_pread_calls, 4,
             "each partial result advances exactly once, requiring four reads");
        T_OK(memcmp(raw, expected, sizeof expected) == 0,
             "partial reads fill every byte without skipped pointer regions");
    }

    iopath_close(io);

    fd = open(path, O_RDONLY | O_CLOEXEC);
    T_OK(fd >= 0, "opened an explicitly read-only retained storage fd");
    retained_io = fd >= 0 ? iopath_open_fd(fd, IOPATH_PWRITE, 4096) : NULL;
    T_OK(retained_io != NULL,
         "pwrite-family open_fd can retain a read-only fd for reads");
    if (fd >= 0) close(fd);
    if (retained_io && raw) {
        T_EQ(iopath_read(retained_io, 0, raw, sizeof expected), 0,
             "retained read-only handle remains readable after caller closes its fd");
        T_OK(iopath_write(retained_io, 0, raw, sizeof expected) != 0,
             "open_fd never upgrades a retained O_RDONLY description to O_RDWR");
    }
    iopath_close(retained_io);
    free(raw);
    unlink(path);
    T_DONE();
}
