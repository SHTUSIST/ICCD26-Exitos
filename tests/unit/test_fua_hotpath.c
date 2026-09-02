/* Verify the command that the real ioctl data path submits, rather than only
 * the standalone encoder.  A regular scratch file is enough: this executable
 * supplies ioctl(), makes NVME_IOCTL_ID identify it as namespace 7, and records
 * every passthrough command without sending anything to hardware. */
#include "tap.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "exitos_devguard.h"
#include "exitos_iopath.h"

static struct nvme_passthru_cmd submitted;
static unsigned submit_count;
static unsigned id_query_count;
static unsigned lbs_query_count;
static ino_t submitted_inode;
static off_t submitted_offset;
static int mock_whole = 1;
static int mock_lbs = 4096;
static int mock_lbs_failure;
static unsigned fdatasync_call_count;
static int fdatasync_last_fd = -1;

/* White-box prefix used only to retag an already-open scratch-file handle for
 * durability dispatch.  These are the documented leading fields of iopath.c's
 * private handle; no ring operation is attempted through the retagged handle. */
struct iopath_prefix {
    int fd;
    iopath_backend backend;
};

int exitos_internal_fdatasync_call(int fd)
{
    fdatasync_call_count++;
    fdatasync_last_fd = fd;
    return 0;
}

/* This test deliberately exercises the post-open NVMe submit path without a
 * device.  Make its owned scratch fd look like one fixed block dev_t and make
 * the isolated sysfs decision explicit.  Production iopath_open still uses
 * the real fstat and /sys/dev/block gate. */
int fstat(int fd, struct stat *st)
{
    int rc = (int)syscall(SYS_fstat, fd, st);
    if (rc == 0 && S_ISREG(st->st_mode)) {
        st->st_mode = (st->st_mode & ~S_IFMT) | S_IFBLK;
        st->st_rdev = makedev(259, 77);
    }
    return rc;
}

int exitos_devguard_whole_block_at(const char *root, unsigned maj, unsigned min)
{
    return mock_whole && root && strcmp(root, "/sys") == 0 &&
           maj == 259 && min == 77;
}

int exitos_devguard_nvme_pair_at(const char *root,
                                 unsigned bmaj, unsigned bmin,
                                 unsigned cmaj, unsigned cmin,
                                 uint32_t nsid)
{
    (void)root; (void)bmaj; (void)bmin; (void)cmaj; (void)cmin; (void)nsid;
    return 0;
}

int exitos_devguard_resolve_fd(int fd, struct devguard_device_info *out)
{
    (void)fd; (void)out;
    return -ENODEV;
}

int exitos_devguard_lba_span_fd(int fd, uint64_t *base, uint64_t *count)
{
    (void)fd; (void)base; (void)count;
    return -ENODEV;
}

int exitos_devguard_poll_available_fd(int fd)
{
    (void)fd;
    return 0;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    (void)fd;

    if (request == BLKSSZGET) {
        int *out;
        lbs_query_count++;
        if (mock_lbs_failure) {
            errno = EIO;
            return -1;
        }
        va_start(ap, request);
        out = va_arg(ap, int *);
        va_end(ap);
        *out = mock_lbs;
        return 0;
    }
    if (request == NVME_IOCTL_ID) {
        id_query_count++;
        return 7;
    }
    if (request == NVME_IOCTL_IO_CMD) {
        struct nvme_passthru_cmd *cmd;
        struct stat real;
        va_start(ap, request);
        cmd = va_arg(ap, struct nvme_passthru_cmd *);
        va_end(ap);
        submitted = *cmd;
        submitted_inode = syscall(SYS_fstat, fd, &real) == 0 ? real.st_ino : 0;
        submitted_offset = lseek(fd, 321, SEEK_SET);
        submit_count++;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

int main(void)
{
    char path[] = "/tmp/exitos-fua-hotpath-XXXXXX";
    struct iopath *nvme = NULL, *pwrite = NULL;
    void *buf = NULL;
    int fd = mkstemp(path);

    T_OK(fd >= 0, "created an owned fake namespace file");
    if (fd >= 0) {
        T_EQ(ftruncate(fd, 16384), 0, "sized the fake namespace file");
        close(fd);
    }
    T_EQ(posix_memalign(&buf, 4096, 4096), 0,
         "allocated one logical-block-aligned buffer");
    if (buf)
        memset(buf, 0x5a, 4096);

    id_query_count = 0;
    lbs_query_count = 0;
    T_OK(iopath_open(path, IOPATH_NVME_IOCTL, 512) == NULL,
         "native open refuses caller LBS that differs from retained namespace LBS");
    T_EQ(lbs_query_count, 1, "native mismatch probes LBS on the opened namespace fd");
    T_EQ(id_query_count, 0, "LBS mismatch is refused before namespace-ID query");

    fd = open(path, O_RDWR | O_CLOEXEC);
    T_OK(fd >= 0, "retained the fake namespace fd for open_fd mismatch tests");
    if (fd >= 0) {
        iopath_backend native_backends[] = {
            IOPATH_NVME_IOCTL, IOPATH_URING_CMD, IOPATH_URING_CMD_POLL
        };
        const char *backend_names[] = { "ioctl", "uring", "poll" };
        for (size_t i = 0; i < sizeof native_backends / sizeof native_backends[0]; i++) {
            struct iopath *bad;
            lbs_query_count = 0;
            id_query_count = 0;
            bad = iopath_open_fd(fd, native_backends[i], 512);
            T_OK(bad == NULL,
                 "open_fd %s refuses caller LBS that differs from retained namespace LBS",
                 backend_names[i]);
            T_EQ(lbs_query_count, 1,
                 "open_fd %s queries LBS on the retained block fd before discovery/setup",
                 backend_names[i]);
            T_EQ(id_query_count, 0,
                 "open_fd %s LBS mismatch cannot reach an NSID query",
                 backend_names[i]);
            iopath_close(bad);
        }
        close(fd);
    }

    mock_lbs_failure = 1;
    id_query_count = 0;
    T_OK(iopath_open(path, IOPATH_NVME_IOCTL, 4096) == NULL,
         "native open fails closed when namespace LBS cannot be queried");
    T_EQ(id_query_count, 0, "failed LBS query cannot reach namespace-ID query");
    mock_lbs_failure = 0;

    lbs_query_count = 0;
    nvme = iopath_open(path, IOPATH_NVME_IOCTL, 4096);
    T_OK(nvme != NULL, "opened the fake namespace through the ioctl backend");
    T_EQ(lbs_query_count, 1,
         "matching native open verifies caller LBS against the namespace fd");
    T_EQ(iopath_lbs(nvme), 4096,
         "native handle exposes only its verified namespace LBS");
    T_EQ(iopath_nsid(nvme), 7,
         "native handle exposes the NSID discovered from that namespace fd");
    if (nvme && buf) {
        memset(&submitted, 0, sizeof submitted);
        submit_count = 0;
        T_EQ(iopath_set_fua(nvme, 1), 0,
             "native NVMe backend accepts FUA mode");
        T_EQ(iopath_write(nvme, 3, buf, 4096), 0,
             "FUA write reached the mocked submit path");
        T_EQ(submit_count, 1, "exactly one command was submitted");
        T_EQ(submitted.opcode, 0x01, "submitted command is NVMe WRITE");
        T_EQ((submitted.cdw12 >> 30) & 1u, 1u,
             "the actual submitted WRITE carries CDW12.FUA");
        T_EQ(submitted.cdw12 & 0xffffu, 0u,
             "one block still encodes zero-based NLB 0");

        memset(&submitted, 0xff, sizeof submitted);
        T_EQ(iopath_read(nvme, 3, buf, 4096), 0,
             "read reached the mocked submit path while FUA mode is enabled");
        T_EQ(submitted.opcode, 0x02, "submitted command is NVMe READ");
        T_EQ((submitted.cdw12 >> 30) & 1u, 0u,
             "READ never inherits the write-only FUA bit");

        T_EQ(iopath_set_fua(nvme, 0), 0,
             "native NVMe backend can disable FUA mode");
        T_EQ(iopath_write(nvme, 4, buf, 4096), 0,
             "plain write reached the mocked submit path");
        T_EQ((submitted.cdw12 >> 30) & 1u, 0u,
             "submitted WRITE has FUA clear after disabling it");
    }

    iopath_close(nvme);
    nvme = NULL;
    mock_whole = 0;
    id_query_count = 0;
    T_OK(iopath_open(path, IOPATH_NVME_IOCTL, 4096) == NULL,
         "native open refuses when actual sysfs identity says partition");
    T_EQ(id_query_count, 0,
         "partition is rejected before even querying an NVMe namespace ID");

    lbs_query_count = 0;
    pwrite = iopath_open(path, IOPATH_PWRITE, 4096);
    T_OK(pwrite != NULL, "opened the same scratch storage via pwrite");
    T_EQ(lbs_query_count, 0,
         "pwrite backend keeps regular-file semantics and does not issue BLKSSZGET");
    if (pwrite) {
        struct iopath_prefix *view = (struct iopath_prefix *)(void *)pwrite;
        int retained_fd = view->fd;
        T_EQ(iopath_lbs(pwrite), 4096,
             "pwrite handle reports the caller byte-storage LBS");
        T_EQ(iopath_nsid(pwrite), 0,
             "pwrite handle never invents an NVMe namespace ID");
        T_EQ(iopath_set_fua(pwrite, 1), -EOPNOTSUPP,
             "pwrite refuses a native-FUA promise it cannot encode");
        T_EQ(iopath_get_fua(pwrite), 0,
             "failed FUA request leaves pwrite in flush-based mode");

        view->backend = IOPATH_URING_WRITE_POLL;
        fdatasync_call_count = 0;
        fdatasync_last_fd = -1;
        T_EQ(iopath_set_fua(pwrite, 1), -EOPNOTSUPP,
             "ordinary block WRITE backend refuses native-command FUA");
        T_EQ(iopath_get_fua(pwrite), 0,
             "block WRITE FUA refusal leaves flush-based durability active");
        T_EQ(iopath_flush(pwrite), 0,
             "ordinary block WRITE durability dispatch succeeds through fdatasync");
        T_EQ(fdatasync_call_count, 1,
             "ordinary block WRITE flush calls fdatasync exactly once");
        T_EQ(fdatasync_last_fd, retained_fd,
             "ordinary block WRITE flush targets the retained exact fd");
        view->backend = IOPATH_PWRITE;
    }

    iopath_close(pwrite);
    pwrite = NULL;

    {
        char original[160];
        struct stat real;
        ino_t retained_inode = 0;
        int retained = open(path, O_RDWR | O_CLOEXEC);
        snprintf(original, sizeof original, "%s-retained", path);
        if (retained >= 0 && syscall(SYS_fstat, retained, &real) == 0)
            retained_inode = real.st_ino;
        T_OK(retained >= 0 && retained_inode != 0,
             "opened the namespace object before rebinding its pathname");
        T_EQ(rename(path, original), 0,
             "moved the retained namespace away from its original pathname");
        fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        T_OK(fd >= 0, "rebound the original pathname to a replacement object");
        if (fd >= 0) {
            T_EQ(ftruncate(fd, 16384), 0, "sized the pathname replacement");
            close(fd);
        }
        mock_whole = 1;
        nvme = retained >= 0 ? iopath_open_fd(retained, IOPATH_NVME_IOCTL, 4096) : NULL;
        T_OK(nvme != NULL,
             "native open_fd retains the already-verified object after pathname rebinding");
        submitted_inode = 0;
        submitted_offset = -1;
        if (nvme && buf)
            T_EQ(iopath_write(nvme, 0, buf, 4096), 0,
                 "write submits through the retained native handle");
        T_EQ(submitted_inode, retained_inode,
             "native submission fd still names the retained inode, not the replacement path");
        T_EQ(submitted_offset, 321,
             "mocked native submit moved its handle's open-file-description offset");
        T_EQ(retained >= 0 ? lseek(retained, 0, SEEK_CUR) : -1, 321,
             "NVMe ioctl handle is a duplicate of the retained open file description");
        iopath_close(nvme);
        nvme = NULL;
        if (retained >= 0) close(retained);
        unlink(path);
        rename(original, path);
    }

    free(buf);
    unlink(path);
    T_DONE();
}
