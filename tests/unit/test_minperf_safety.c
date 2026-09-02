/* Execute minperf's real control flow with storage syscalls and safety gates
 * interposed.  No device is opened: every fd/guard/iopath operation below is a
 * deterministic in-process fake, while the arm-selection logic is production
 * attribution/minperf.c. */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/nvme_ioctl.h>

#include "tap.h"
#include "exitos_devguard.h"
#include "exitos_iopath.h"

static unsigned g_pwrite_submits;
static unsigned g_iopath_write_submits;
static unsigned g_read_submits;
static unsigned g_flush_submits;
static unsigned g_device_open_calls;
static unsigned g_filesystem_open_calls;
static unsigned g_path_api_calls;
static unsigned g_bad_retained_fd_calls;
static unsigned g_alloc_calls;
static size_t g_alloc_alignments[8];
static unsigned g_calloc_calls;
static unsigned g_fallocate_calls;
static unsigned g_device_open_generation;
static int g_device_open_flags;
static int g_filesystem_open_flags;
static unsigned g_filesystem_unlink_calls;
static int g_filesystem_path_preexists;
static int g_fail_device_direct_open;
static int g_mock_nsid;
static int g_mock_whole;
static long g_mock_page_size;
static unsigned g_fail_alloc_call;
static unsigned g_fail_calloc_call;
static unsigned g_fail_pwrite_call;
static unsigned g_short_pwrite_call;
static unsigned g_fail_pread_call;
static unsigned g_short_pread_call;
static unsigned g_fail_flush_call;
static unsigned g_fail_write_call;
static unsigned g_fail_read_call;
static unsigned g_fail_fdatasync_call;
static int g_fail_fallocate;

enum { RETAINED_FD = 77, FILESYSTEM_FD = 88 };

static void require_retained_fd(int fd)
{
    if (fd != RETAINED_FD) g_bad_retained_fd_calls++;
}

static int mock_open(const char *path, int flags, ...)
{
    if (strcmp(path, "/mock/retained-device") == 0) {
        g_device_open_calls++;
        g_device_open_flags = flags;
        g_device_open_generation++;
        if (g_fail_device_direct_open && (flags & O_DIRECT)) {
            errno = EINVAL;
            return -1;
        }
        return RETAINED_FD + (int)(g_device_open_generation - 1u) * 100;
    }
    g_filesystem_open_calls++;
    g_filesystem_open_flags = flags;
    if (g_filesystem_path_preexists && (flags & O_CREAT) && (flags & O_EXCL)) {
        errno = EEXIST;
        return -1;
    }
    return FILESYSTEM_FD;
}

static int mock_close(int fd) { (void)fd; return 0; }

static int mock_unlink(const char *path)
{
    (void)path;
    g_filesystem_unlink_calls++;
    return 0;
}

static ssize_t mock_pwrite(int fd, const void *buf, size_t len, off_t off)
{
    (void)fd; (void)buf; (void)off;
    g_pwrite_submits++;
    if (g_pwrite_submits == g_fail_pwrite_call) {
        errno = EIO;
        return -1;
    }
    if (g_pwrite_submits == g_short_pwrite_call)
        return len > 0 ? (ssize_t)len - 1 : 0;
    return (ssize_t)len;
}

static ssize_t mock_pread(int fd, void *buf, size_t len, off_t off)
{
    (void)fd; (void)off;
    g_read_submits++;
    if (g_read_submits == g_fail_pread_call) {
        errno = EIO;
        return -1;
    }
    if (g_read_submits == g_short_pread_call) {
        if (len > 1) memset(buf, 0, len - 1);
        return len > 0 ? (ssize_t)len - 1 : 0;
    }
    memset(buf, 0, len);
    return (ssize_t)len;
}

static int mock_fdatasync(int fd)
{
    (void)fd;
    g_flush_submits++;
    if (g_flush_submits == g_fail_fdatasync_call) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int mock_fallocate(int fd, int mode, off_t offset, off_t len)
{
    (void)fd; (void)mode; (void)offset; (void)len;
    g_fallocate_calls++;
    if (g_fail_fallocate) { errno = ENOSPC; return -1; }
    return 0;
}

static int mock_posix_memalign(void **out, size_t alignment, size_t size)
{
    g_alloc_calls++;
    if (g_alloc_calls <= sizeof g_alloc_alignments / sizeof g_alloc_alignments[0])
        g_alloc_alignments[g_alloc_calls - 1u] = alignment;
    if (g_alloc_calls == g_fail_alloc_call) return ENOMEM;
    return posix_memalign(out, alignment, size);
}

static long mock_sysconf(int name)
{
    if (name == _SC_PAGESIZE) return g_mock_page_size;
    errno = EINVAL;
    return -1;
}

static void *mock_calloc(size_t count, size_t size)
{
    g_calloc_calls++;
    if (g_calloc_calls == g_fail_calloc_call) return NULL;
    return calloc(count, size);
}

static int mock_ioctl(int fd, unsigned long request, ...)
{
    require_retained_fd(fd);
    (void)request;
    return g_mock_nsid;
}

static __attribute__((unused)) devguard_verdict
mock_devguard_check(const char *path, const char *expect,
                                             char *serial, unsigned serial_len)
{
    (void)path; (void)expect; (void)serial; (void)serial_len;
    g_path_api_calls++;
    return DEVGUARD_NOT_A_DEVICE;
}

static devguard_verdict mock_devguard_check_fd(int fd, const char *expect,
                                                char *serial,
                                                unsigned serial_len)
{
    require_retained_fd(fd);
    if (serial && serial_len) snprintf(serial, serial_len, "mock-ns");
    if (expect && *expect && strcmp(expect, "mock-ns") != 0)
        return DEVGUARD_SERIAL_MISMATCH;
    return DEVGUARD_OK;
}

static int mock_passthru_ok_fd(int fd) { require_retained_fd(fd); return g_mock_whole; }
static uint32_t mock_lbs_fd(int fd) { require_retained_fd(fd); return 4096; }

static __attribute__((unused)) int
mock_window_partition(const char *disk, const char *part,
                                 uint64_t lba, uint64_t count, uint32_t lbs,
                                 char *why, unsigned wlen)
{
    (void)disk; (void)part; (void)lba; (void)count; (void)lbs;
    g_path_api_calls++;
    if (why && wlen) snprintf(why, wlen, "mock-contained");
    return 0;
}

static int mock_window_partition_fds(int disk_fd, int part_fd,
                                     uint64_t lba, uint64_t count,
                                     uint32_t lbs, char *why, unsigned wlen)
{
    require_retained_fd(disk_fd);
    if (part_fd < 0) g_bad_retained_fd_calls++;
    (void)lba; (void)count; (void)lbs;
    if (why && wlen) snprintf(why, wlen, "mock-contained");
    return 0;
}

static __attribute__((unused)) int
mock_pin_capture(const char *path, uint64_t lba, uint64_t count,
                            struct devguard_pin *pin)
{
    (void)path; (void)lba; (void)count;
    (void)pin;
    g_path_api_calls++;
    return -1;
}

static int mock_pin_capture_fd(int fd, uint64_t lba, uint64_t count,
                               struct devguard_pin *pin)
{
    require_retained_fd(fd);
    (void)lba; (void)count;
    memset(pin, 0, sizeof *pin);
    snprintf(pin->serial, sizeof pin->serial, "mock-ns");
    pin->logical_block_size = 4096;
    return 0;
}

static __attribute__((unused)) int
mock_pin_verify(const char *path, const char *pin, uint64_t lba,
                           uint64_t count, char *why, unsigned wlen)
{
    (void)path; (void)pin; (void)lba; (void)count;
    (void)why; (void)wlen;
    g_path_api_calls++;
    return -1;
}

static int mock_pin_verify_fd(int fd, const char *pin, uint64_t lba,
                              uint64_t count, char *why, unsigned wlen)
{
    require_retained_fd(fd);
    (void)pin; (void)lba; (void)count;
    if (why && wlen) snprintf(why, wlen, "mock-pin-ok");
    return 0;
}

static __attribute__((unused)) int
mock_zero(const char *path, uint64_t lba, uint64_t count,
                     uint32_t lbs, uint64_t *bad)
{
    (void)path; (void)lba; (void)count; (void)lbs; (void)bad;
    g_path_api_calls++;
    return -1;
}

static int mock_zero_fd(int fd, uint64_t lba, uint64_t count, uint64_t *bad)
{
    require_retained_fd(fd);
    (void)lba; (void)count; (void)bad;
    return 0;
}

static struct iopath *mock_iopath_open_fd(int fd, iopath_backend backend,
                                          uint32_t lbs)
{
    require_retained_fd(fd);
    (void)lbs;
    return (struct iopath *)(uintptr_t)(backend + 1u);
}

static void mock_iopath_close(struct iopath *path) { (void)path; }

static int mock_iopath_write(struct iopath *path, uint64_t lba,
                             const void *buf, size_t len)
{
    (void)path; (void)lba; (void)buf; (void)len;
    g_iopath_write_submits++;
    if (g_iopath_write_submits == g_fail_write_call) return -EIO;
    return 0;
}

static int mock_iopath_read(struct iopath *path, uint64_t lba,
                            void *buf, size_t len)
{
    (void)path; (void)lba;
    g_read_submits++;
    if (g_read_submits == g_fail_read_call) return -EIO;
    memset(buf, 0, len);
    return 0;
}

static int mock_iopath_flush(struct iopath *path)
{
    (void)path;
    g_flush_submits++;
    if (g_flush_submits == g_fail_flush_call) return -EIO;
    return 0;
}

static int mock_iopath_set_fua(struct iopath *path, int on)
{
    (void)path; (void)on;
    return 0;
}

static uint32_t mock_iopath_lbs(const struct iopath *path)
{
    return path ? 4096 : 0;
}

static uint32_t mock_iopath_nsid(const struct iopath *path)
{
    return (uintptr_t)path == (uintptr_t)(IOPATH_NVME_IOCTL + 1u) ? 7 : 0;
}

#define main minperf_program_main
#define open mock_open
#define close mock_close
#define unlink mock_unlink
#define pwrite mock_pwrite
#define pread mock_pread
#define fdatasync mock_fdatasync
#define fallocate mock_fallocate
#define posix_memalign mock_posix_memalign
#define sysconf mock_sysconf
#define calloc mock_calloc
#define ioctl mock_ioctl
#define exitos_devguard_check mock_devguard_check
#define exitos_devguard_check_fd mock_devguard_check_fd
#define exitos_devguard_passthru_ok_fd mock_passthru_ok_fd
#define exitos_devguard_lbs_fd mock_lbs_fd
#define exitos_devguard_window_in_partition mock_window_partition
#define exitos_devguard_window_in_partition_fds mock_window_partition_fds
#define exitos_devguard_pin_capture mock_pin_capture
#define exitos_devguard_pin_capture_fd mock_pin_capture_fd
#define exitos_devguard_pin_verify_window mock_pin_verify
#define exitos_devguard_pin_verify_window_fd mock_pin_verify_fd
#define exitos_devguard_window_is_zero_checked mock_zero
#define exitos_devguard_window_is_zero_fd mock_zero_fd
#define iopath_open_fd mock_iopath_open_fd
#define iopath_close mock_iopath_close
#define iopath_write mock_iopath_write
#define iopath_read mock_iopath_read
#define iopath_flush mock_iopath_flush
#define iopath_set_fua mock_iopath_set_fua
#define iopath_lbs mock_iopath_lbs
#define iopath_nsid mock_iopath_nsid
#include "../../attribution/minperf.c"
#undef main
#undef calloc
#undef sysconf
#undef posix_memalign
#undef fallocate
#undef unlink

static void reset_env(void)
{
    const char *names[] = {
        "EXITOS_SCRATCH", "EXITOS_CERTIFY", "EXITOS_REUSE_WINDOW",
        "EXITOS_WINDOW_IN_PART", "EXITOS_ALLOW_PARTITION", "EXITOS_READ",
        "EXITOS_ONLY", "EXITOS_FSFILE"
    };
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) unsetenv(names[i]);
    setenv("EXITOS_DEV", "/mock/retained-device", 1);
    setenv("EXITOS_LBA_START", "0", 1);
    setenv("EXITOS_LBA_COUNT", "8", 1);
    setenv("EXITOS_LBS", "4096", 1);
    setenv("EXITOS_NSID", "7", 1);
    setenv("EXITOS_EXPECT_SERIAL", "mock-ns", 1);
    setenv("EXITOS_PIN", "/mock/pin", 1);
    setenv("EXITOS_ITERS", "1", 1);
    g_pwrite_submits = g_iopath_write_submits = 0;
    g_read_submits = g_flush_submits = 0;
    g_device_open_calls = g_filesystem_open_calls = 0;
    g_path_api_calls = g_bad_retained_fd_calls = 0;
    g_alloc_calls = g_calloc_calls = g_fallocate_calls = 0;
    memset(g_alloc_alignments, 0, sizeof g_alloc_alignments);
    g_device_open_generation = 0;
    g_device_open_flags = 0;
    g_filesystem_open_flags = 0;
    g_filesystem_unlink_calls = 0;
    g_filesystem_path_preexists = 0;
    g_fail_device_direct_open = 0;
    g_mock_nsid = 7;
    g_mock_whole = 1;
    g_mock_page_size = getpagesize();
    g_fail_alloc_call = g_fail_calloc_call = 0;
    g_fail_pwrite_call = g_short_pwrite_call = 0;
    g_fail_pread_call = g_short_pread_call = 0;
    g_fail_flush_call = g_fail_write_call = g_fail_read_call = 0;
    g_fail_fdatasync_call = 0;
    g_fail_fallocate = 0;
}

int main(void)
{
    char *argv[] = { (char *)"minperf", (char *)"1", (char *)"4096", NULL };
    char *argv2[] = { (char *)"minperf", (char *)"2", (char *)"4096", NULL };
    char *argv0[] = { (char *)"minperf", (char *)"0", (char *)"4096", NULL };
    size_t expected_small_lbs_alignment =
        (size_t)getpagesize() > 512u ? (size_t)getpagesize() : 512u;
    int rc;

    /* Alignment changes the virtual-page layout of an 8 KiB raw buffer.  It
     * does not prove physical contiguity or a particular DMA segment count;
     * those remain trace-observed properties in the real benchmark. */
    reset_env();
    rc = bench_iopath_mode((struct iopath *)(uintptr_t)1, 0, 64, 1, 8192,
                           512, 0, 0, 0x33, "alignment-write");
    T_EQ(rc, 0, "512-byte-LBS raw write path completes with an 8 KiB buffer");
    T_EQ(g_alloc_calls, 1, "raw write path performs one aligned buffer allocation");
    T_EQ(g_alloc_alignments[0], expected_small_lbs_alignment,
         "raw write buffer alignment is max(actual LBS, system page size)");
    T_EQ(g_alloc_alignments[0] % 512u, 0,
         "raw write page alignment continues to satisfy the actual LBS");

    reset_env();
    rc = bench_batch((struct iopath *)(uintptr_t)1, 0, 64, 1, 1, 8192, 512);
    T_EQ(rc, 0, "512-byte-LBS raw batch path completes with an 8 KiB buffer");
    T_EQ(g_alloc_calls, 1, "raw batch path performs one aligned buffer allocation");
    T_EQ(g_alloc_alignments[0], expected_small_lbs_alignment,
         "raw batch buffer uses the same system-page alignment policy");
    T_EQ(g_alloc_alignments[0] % 512u, 0,
         "raw batch page alignment continues to satisfy the actual LBS");

    reset_env();
    rc = bench_iopath_read((struct iopath *)(uintptr_t)1, 0, 64, 1, 8192,
                           512, "alignment-read");
    T_EQ(rc, 0, "512-byte-LBS raw read path completes with an 8 KiB buffer");
    T_EQ(g_alloc_calls, 1, "raw read path performs one aligned buffer allocation");
    T_EQ(g_alloc_alignments[0], expected_small_lbs_alignment,
         "raw read buffer uses the same system-page alignment policy");
    T_EQ(g_alloc_alignments[0] % 512u, 0,
         "raw read page alignment continues to satisfy the actual LBS");

    reset_env();
    g_mock_page_size = 6144;
    rc = bench_iopath_mode((struct iopath *)(uintptr_t)1, 0, 64, 1, 8192,
                           512, 0, 0, 0x33, "incompatible-alignment");
    T_EQ(rc, -EINVAL,
         "raw path rejects a page/LBS combination with no legal max alignment");
    T_EQ(g_alloc_calls, 0,
         "incompatible raw alignment is rejected before posix_memalign");
    T_EQ(g_iopath_write_submits, 0,
         "incompatible raw alignment is rejected before any submission");

    reset_env();
    g_mock_page_size = -1;
    rc = bench_iopath_mode((struct iopath *)(uintptr_t)1, 0, 64, 1, 8192,
                           512, 0, 0, 0x33, "invalid-page-size");
    T_EQ(rc, -EINVAL,
         "failed page-size discovery cannot wrap into a size_t alignment");
    T_EQ(g_alloc_calls, 0,
         "invalid page size is rejected before aligned allocation");
    T_EQ(g_iopath_write_submits, 0,
         "invalid page size is rejected before any submission");

    reset_env();
    g_mock_page_size = 4096;
    rc = bench_iopath_mode((struct iopath *)(uintptr_t)1, 0, 64, 1, 8192,
                           8192, 0, 0, 0x33, "large-lbs-alignment");
    T_EQ(rc, 0, "raw path accepts a legal LBS larger than the system page size");
    T_EQ(g_alloc_calls, 1, "large-LBS raw path performs one allocation");
    T_EQ(g_alloc_alignments[0], 8192,
         "raw buffer alignment uses actual LBS when it exceeds page size");
    T_EQ(g_alloc_alignments[0] % 4096u, 0,
         "larger legal LBS alignment also remains page aligned");

    reset_env();
    setenv("EXITOS_READ", "1", 1);
    unsetenv("EXITOS_ONLY");
    rc = minperf_program_main(3, argv);
    T_EQ(rc, 0, "mocked read-only minperf run completes");
    T_EQ(g_pwrite_submits + g_iopath_write_submits, 0,
         "EXITOS_READ submits zero writes before or during every read arm");
    T_EQ(g_read_submits, 2,
         "default read-only mode submits one retained raw read and one retained NVMe read");
    T_EQ(g_device_open_calls, 1,
         "minperf opens the device pathname once and all later operations use retained fd handles");
    T_EQ(g_device_open_flags & O_ACCMODE, O_RDONLY,
         "read mode selects the retained device initially through an O_RDONLY fd");
    T_OK((g_device_open_flags & O_DIRECT) != 0,
         "default read plan retains its raw block baseline with O_DIRECT");
    T_EQ(g_path_api_calls, 0, "read-only flow never calls a pathname safety API");
    T_EQ(g_bad_retained_fd_calls, 0,
         "every read-only safety and I/O helper receives the retained fd generation");

    reset_env();
    unsetenv("EXITOS_READ");
    setenv("EXITOS_ONLY", "rawnf", 1);
    rc = minperf_program_main(3, argv);
    T_EQ(rc, 0, "mocked EXITOS_ONLY=rawnf run completes");
    T_EQ(g_pwrite_submits + g_iopath_write_submits, 1,
         "EXITOS_ONLY=rawnf submits exactly its one requested write arm");
    T_EQ(g_flush_submits, 0,
         "EXITOS_ONLY=rawnf cannot run the flushing raw arm as a side effect");
    T_EQ(g_device_open_calls, 1,
         "write-mode safety and final submit share one retained pathname selection");
    T_EQ(g_device_open_flags & O_ACCMODE, O_RDWR,
         "write mode opens the retained device read-write exactly once");
    T_OK((g_device_open_flags & O_DIRECT) != 0,
         "raw write plan retains its block baseline with O_DIRECT");
    T_EQ(g_path_api_calls, 0, "write flow never calls a pathname safety API");
    T_EQ(g_bad_retained_fd_calls, 0,
         "every write safety and I/O helper receives the retained fd generation");

    reset_env();
    setenv("EXITOS_ONLY", "rawnf", 1);
    g_fail_device_direct_open = 1;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "raw baseline fails closed when retained O_DIRECT open is unavailable");
    T_EQ(g_device_open_calls, 1,
         "O_DIRECT refusal does not retry the device pathname with weaker flags");
    T_EQ(g_iopath_write_submits, 0,
         "failed O_DIRECT selection cannot fall back to a buffered raw sample");

    reset_env();
    setenv("EXITOS_ONLY", "rawnf", 1);
    rc = minperf_program_main(3, argv2);
    T_EQ(rc, 0, "two-sample retained raw plan completes exactly");
    T_EQ(g_iopath_write_submits, 2,
         "successful retained raw arm submits every planned sample exactly once");

    reset_env();
    unsetenv("EXITOS_READ");
    setenv("EXITOS_ONLY", "rawnf", 1);
    setenv("EXITOS_LBS", "512", 1);
    rc = minperf_program_main(3, argv);
    T_EQ(rc, 3, "caller LBS mismatch is a hard refusal");
    T_EQ(g_pwrite_submits + g_iopath_write_submits, 0,
         "caller LBS mismatch is rejected before any write submission");

    reset_env();
    unsetenv("EXITOS_READ");
    setenv("EXITOS_ONLY", "fua", 1);
    setenv("EXITOS_NSID", "8", 1);
    rc = minperf_program_main(3, argv);
    T_EQ(rc, 3, "caller NSID mismatch is a hard refusal");
    T_EQ(g_pwrite_submits + g_iopath_write_submits, 0,
         "caller NSID mismatch is rejected before any native write submission");

    reset_env();
    setenv("EXITOS_ONLY", "rawnf", 1);
    setenv("EXITOS_EXPECT_SERIAL", "wrong-controller", 1);
    rc = minperf_program_main(3, argv);
    T_EQ(rc, 3, "controller serial mismatch is a hard refusal");
    T_EQ(g_pwrite_submits + g_iopath_write_submits + g_read_submits, 0,
         "controller serial mismatch permits zero I/O submissions");
    T_EQ(g_path_api_calls, 0,
         "serial refusal is decided by the retained-fd guard, never a path wrapper");

    reset_env();
    setenv("EXITOS_ONLY", "rawnf", 1);
    g_fail_alloc_call = 1;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "selected raw arm propagates aligned-buffer allocation failure");
    T_EQ(g_iopath_write_submits, 0,
         "raw allocation failure submits no partial sample");

    reset_env();
    setenv("EXITOS_ONLY", "rawnf", 1);
    g_fail_calloc_call = 1;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "selected raw arm propagates sample-array allocation failure");
    T_EQ(g_iopath_write_submits, 0,
         "sample-array allocation failure submits no raw write");

    reset_env();
    setenv("EXITOS_ONLY", "rawnf", 1);
    g_fail_write_call = 2;
    rc = minperf_program_main(3, argv2);
    T_OK(rc != 0, "selected raw arm rejects a partial two-sample run");
    T_EQ(g_iopath_write_submits, 2,
         "raw arm stops immediately at the failed second planned sample");

    reset_env();
    setenv("EXITOS_ONLY", "raw", 1);
    g_fail_flush_call = 1;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "selected raw durability arm propagates flush failure");
    T_EQ(g_iopath_write_submits, 1,
         "flush failure occurs only after the arm's one planned write");

    reset_env();
    setenv("EXITOS_READ", "1", 1);
    setenv("EXITOS_ONLY", "rawread", 1);
    g_fail_read_call = 1;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "selected retained raw-read arm propagates read failure");
    T_EQ(g_read_submits, 1, "failed raw read is not reported as a complete sample");

    reset_env();
    setenv("EXITOS_READ", "1", 1);
    setenv("EXITOS_ONLY", "nvmeread", 1);
    g_fail_read_call = 1;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "selected retained NVMe-read arm propagates read failure");
    T_EQ(g_read_submits, 1, "failed native read stops its selected arm immediately");

    reset_env();
    setenv("EXITOS_ONLY", "batch", 1);
    setenv("EXITOS_LBA_COUNT", "100000", 1);
    g_fail_write_call = 2;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "selected batch arm propagates a member write failure");
    T_EQ(g_iopath_write_submits, 2,
         "batch failure stops all later members and comparison phases");

    reset_env();
    setenv("EXITOS_ONLY", "fsbuf", 1);
    setenv("EXITOS_SCRATCH", "/mock", 1);
    g_fail_fallocate = 1;
    rc = minperf_program_main(3, argv2);
    T_OK(rc != 0, "selected filesystem arm treats preallocation failure as fatal");
    T_EQ(g_pwrite_submits, 0,
         "failed preallocation prevents initialization and timed writes");
    T_EQ(g_device_open_calls, 0,
         "isolated filesystem failure never opens the raw device");

    reset_env();
    setenv("EXITOS_ONLY", "fsbuf", 1);
    setenv("EXITOS_SCRATCH", "/mock", 1);
    rc = minperf_program_main(3, argv2);
    T_EQ(rc, 0, "complete two-sample filesystem plan succeeds");
    T_EQ(g_fallocate_calls, 1, "filesystem plan preallocates its full byte range once");
    T_EQ(g_pwrite_submits, 4,
         "filesystem plan performs two complete prewrites and two timed writes");
    T_EQ(g_flush_submits, 3,
         "filesystem plan syncs initialization and every timed durable sample");
    T_EQ(g_device_open_calls, 0,
         "successful isolated filesystem arm never opens the raw device");
    T_EQ(g_filesystem_unlink_calls, 1,
         "successful filesystem arm removes exactly the scratch file it created");

    reset_env();
    setenv("EXITOS_ONLY", "fsbuf", 1);
    setenv("EXITOS_SCRATCH", "/mock", 1);
    g_filesystem_path_preexists = 1;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "filesystem arm refuses to reuse an existing scratch pathname");
    T_OK((g_filesystem_open_flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL),
         "filesystem scratch creation is exclusive");
    T_EQ(g_pwrite_submits, 0,
         "an existing scratch pathname is never overwritten");
    T_EQ(g_filesystem_unlink_calls, 0,
         "an existing scratch pathname is never removed by cleanup");

    reset_env();
    setenv("EXITOS_ONLY", "fsbuf", 1);
    setenv("EXITOS_SCRATCH", "/mock", 1);
    g_short_pwrite_call = 2;
    rc = minperf_program_main(3, argv2);
    T_OK(rc != 0, "selected filesystem arm rejects a short initialization write");
    T_EQ(g_pwrite_submits, 2,
         "short initialization stops before fdatasync and timed samples");

    reset_env();
    setenv("EXITOS_ONLY", "fsbuf", 1);
    setenv("EXITOS_SCRATCH", "/mock", 1);
    g_fail_fdatasync_call = 1;
    rc = minperf_program_main(3, argv2);
    T_OK(rc != 0, "selected filesystem arm propagates initialization fdatasync failure");
    T_EQ(g_pwrite_submits, 2,
         "initialization sync failure prevents every timed write");

    reset_env();
    setenv("EXITOS_ONLY", "fsbuf", 1);
    setenv("EXITOS_SCRATCH", "/mock", 1);
    g_short_pwrite_call = 3;
    rc = minperf_program_main(3, argv2);
    T_OK(rc != 0, "selected filesystem arm rejects a short timed sample");
    T_EQ(g_pwrite_submits, 3,
         "short first timed sample cannot fall through to a later sample");

    reset_env();
    setenv("EXITOS_ONLY", "fsbuf", 1);
    setenv("EXITOS_SCRATCH", "/mock", 1);
    g_fail_fdatasync_call = 2;
    rc = minperf_program_main(3, argv2);
    T_OK(rc != 0, "selected filesystem arm propagates timed fdatasync failure");
    T_EQ(g_pwrite_submits, 3,
         "timed sync failure stops before the second planned sample");

    reset_env();
    setenv("EXITOS_ONLY", "fsbuf", 1);
    setenv("EXITOS_SCRATCH", "/mock", 1);
    g_fail_alloc_call = 1;
    rc = minperf_program_main(3, argv2);
    T_OK(rc != 0, "selected filesystem arm propagates initialization allocation failure");
    T_EQ(g_pwrite_submits, 0,
         "filesystem allocation failure submits no initialization write");

    reset_env();
    setenv("EXITOS_READ", "1", 1);
    setenv("EXITOS_ONLY", "fsread", 1);
    setenv("EXITOS_FSFILE", "/mock/existing-file", 1);
    g_short_pread_call = 1;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "selected filesystem read rejects a short timed sample");
    T_EQ(g_read_submits, 1,
         "short filesystem read cannot be reported as a complete sample");
    T_EQ(g_device_open_calls, 0,
         "isolated filesystem read never opens the raw device");

    reset_env();
    setenv("EXITOS_ONLY", "decision", 1);
    g_fail_alloc_call = 1;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "selected decision arm propagates aligned-buffer allocation failure");

    reset_env();
    setenv("EXITOS_ONLY", "fsbuf", 1);
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "explicit filesystem write arm refuses a missing scratch directory");
    T_EQ(g_device_open_calls, 0,
         "missing filesystem prerequisites cannot fall through into device work");

    reset_env();
    setenv("EXITOS_READ", "1", 1);
    setenv("EXITOS_ONLY", "fsread", 1);
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "explicit filesystem read arm refuses a missing input file");
    T_EQ(g_read_submits, 0, "missing filesystem read input produces no sample");

    reset_env();
    setenv("EXITOS_ONLY", "raw", 1);
    unsetenv("EXITOS_DEV");
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "explicit raw arm refuses a missing retained-device path");
    T_EQ(g_iopath_write_submits, 0, "missing raw target produces no sample");

    reset_env();
    setenv("EXITOS_ONLY", "batch", 1);
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "batch plan whose later group exceeds the window is refused");
    T_EQ(g_iopath_write_submits, 0,
         "batch validates its entire two-phase plan before its first write");
    T_EQ(g_flush_submits, 0,
         "globally invalid batch plan cannot submit an early flush");

    reset_env();
    setenv("EXITOS_ONLY", "batch", 1);
    setenv("EXITOS_LBA_COUNT", "3600", 1);
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0,
         "batch arm refuses when an early N fits but a later N does not");
    T_EQ(g_iopath_write_submits, 0,
         "all six batch N plans are preflighted before the first N=1 write");
    T_EQ(g_flush_submits, 0,
         "later-N overflow is found before any earlier-N flush phase");

    reset_env();
    setenv("EXITOS_ONLY", "definitely-not-an-arm", 1);
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "unknown EXITOS_ONLY value fails closed");
    T_EQ(g_device_open_calls + g_iopath_write_submits + g_read_submits, 0,
         "unknown arm selection performs no device work");

    reset_env();
    setenv("EXITOS_READ", "1", 1);
    setenv("EXITOS_ONLY", "rawnf", 1);
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0, "read-only mode rejects an explicitly selected write arm");
    T_EQ(g_device_open_calls + g_iopath_write_submits, 0,
         "read-only/write-arm conflict is rejected before device selection");

    reset_env();
    unsetenv("EXITOS_ONLY");
    unsetenv("EXITOS_NSID");
    g_mock_nsid = 0;
    g_mock_whole = 0;
    rc = minperf_program_main(3, argv);
    T_OK(rc != 0,
         "default device plan refuses a target that cannot run its native arms");
    T_EQ(g_iopath_write_submits + g_read_submits, 0,
         "incomplete default plan cannot publish partial raw-device samples");

    reset_env();
    setenv("EXITOS_ONLY", "rawnf", 1);
    rc = minperf_program_main(3, argv0);
    T_OK(rc != 0, "a selected measurement arm refuses a zero-sample plan");
    T_EQ(g_iopath_write_submits, 0,
         "zero-sample refusal happens before any raw submission");

    T_EQ(g_path_api_calls, 0,
         "all exercised production flows avoided every pathname safety API");

    T_DONE();
}
