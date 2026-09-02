/* Staging-slot (EXITOS_STAGE) tests for the native NVMe backends.
 *
 * The scattered-buffer passthrough penalty comes from the kernel forcing SGL
 * for user commands and building an external descriptor list when the payload
 * spans more than one physical segment.  The optional staging slot copies a
 * multi-page payload into one physically contiguous 2 MiB-backed slot so the
 * command carries a single inline descriptor, and on the uring backend the
 * slot is registered once so per-command page pinning disappears.
 *
 * Like test_uring_state.c this file includes the production iopath.c behind
 * fakes; ring memory, locking and the staging policy are the real code.  No
 * device is opened. */
#include "tap.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static long fake_syscall(long number, ...);
static void *fake_mmap(void *addr, size_t length, int prot, int flags,
                       int fd, off_t offset);
static int fake_munmap(void *addr, size_t length);
static int fake_close(int fd);
static int fake_ioctl(int fd, unsigned long request, ...);
static int fake_madvise(void *addr, size_t length, int advice);
static int fake_open(const char *path, int flags, ...);
static ssize_t fake_pread(int fd, void *buf, size_t len, off_t off);

void exitos_internal_fd_enter(int fd) { (void)fd; }
void exitos_internal_fd_leave(int fd) { (void)fd; }
/* Delegates to the same fake as the plain open, the way the close hook below
 * already does: the staging pagemap probe uses the direct-call API so neither
 * frontend's interposer can see it, and the fake still has to answer it. */
int exitos_internal_open_call(const char *path, int flags, mode_t mode)
{ (void)mode; return fake_open(path, flags); }
int exitos_internal_openat_call(int dirfd, const char *path, int flags,
                                mode_t mode)
{ (void)dirfd; (void)path; (void)flags; (void)mode; errno = ENOSYS; return -1; }
int exitos_internal_close_call(int fd) { return fake_close(fd); }
ssize_t exitos_internal_pwrite_call(int fd, const void *buf, size_t count,
                                    off_t offset)
{ (void)fd; (void)buf; (void)count; (void)offset; errno = ENOSYS; return -1; }
int exitos_internal_fdatasync_call(int fd)
{ (void)fd; errno = ENOSYS; return -1; }

#define syscall fake_syscall
#define mmap fake_mmap
#define munmap fake_munmap
#define close fake_close
#define ioctl fake_ioctl
#define madvise fake_madvise
#define open fake_open
#define pread fake_pread
#define IOPATH_EXTERNAL_INTERNAL_FD_HOOKS 1
#include "../../src/iopath.c"
#undef IOPATH_EXTERNAL_INTERNAL_FD_HOOKS
#undef pread
#undef open
#undef madvise
#undef ioctl
#undef close
#undef munmap
#undef mmap
#undef syscall

struct fake_state {
    /* ring plumbing */
    unsigned setup_calls;
    unsigned enter_calls;
    unsigned char sq_map[4096];
    unsigned char cq_map[4096];
    unsigned char sqes_map[4096];
    struct iopath *path;

    /* io_uring_register capture */
    unsigned register_calls;
    int register_ring_fd;
    unsigned register_opcode;
    void *register_base;
    size_t register_len;
    int fail_register;

    /* submitted-command capture (uring and ioctl alike) */
    unsigned submits;
    uint64_t last_addr;
    uint32_t last_data_len;
    uint32_t last_rw_flags;
    uint16_t last_buf_index;
    uint8_t  last_opcode;
    unsigned char last_payload_head[16];

    /* staging environment */
    unsigned char stage_backing[(4u << 20) + 4096u];
    void *stage_mapped;
    unsigned anon_map_calls;
    unsigned anon_unmap_calls;
    int collapse_rc;
    unsigned collapse_calls;
    int pagemap_mode;       /* 0 = unreadable, 1 = contiguous, 2 = scattered */
    unsigned pagemap_reads;
};

static struct fake_state g;

static void fake_reset(void)
{
    struct iopath *keep = NULL;
    (void)keep;
    memset(&g, 0, sizeof g);
    g.register_ring_fd = -1;
    unsetenv("EXITOS_STAGE");
}

static long fake_syscall(long number, ...)
{
    va_list ap;
    long ret = -1;

    va_start(ap, number);
    if (number == __NR_io_uring_setup) {
        (void)va_arg(ap, unsigned);
        struct io_uring_params *pp = va_arg(ap, struct io_uring_params *);
        memset(pp, 0, sizeof *pp);
        pp->sq_entries = 1;
        pp->cq_entries = 16;
        pp->sq_off.head = 0;  pp->sq_off.tail = 4;
        pp->sq_off.ring_mask = 8;  pp->sq_off.array = 16;
        pp->cq_off.head = 0;  pp->cq_off.tail = 4;
        pp->cq_off.ring_mask = 8;  pp->cq_off.cqes = 64;
        g.setup_calls++;
        ret = 901;
    } else if (number == __NR_io_uring_register) {
        int ring_fd = va_arg(ap, int);
        unsigned opcode = va_arg(ap, unsigned);
        const struct iovec *iov = va_arg(ap, const struct iovec *);
        unsigned nr = va_arg(ap, unsigned);
        if (opcode == 20u || opcode == 2u) {
            /* Optional fast-enter registrations (ring fd / fixed file) from
             * uring_setup: refuse them so the handle exercises its fallback,
             * and keep the buffer-registration counters unpolluted. */
            va_end(ap);
            errno = EINVAL;
            return -1;
        }
        g.register_calls++;
        g.register_ring_fd = ring_fd;
        g.register_opcode = opcode;
        if (iov && nr >= 1) {
            g.register_base = iov[0].iov_base;
            g.register_len = iov[0].iov_len;
        }
        if (g.fail_register) {
            errno = EINVAL;
            ret = -1;
        } else {
            ret = 0;
        }
    } else if (number == __NR_io_uring_enter) {
        int ring_fd = va_arg(ap, int);
        unsigned to_submit = va_arg(ap, unsigned);
        struct iopath *p = g.path;
        (void)ring_fd;
        g.enter_calls++;
        if (p && to_submit) {
            unsigned head = __atomic_load_n(p->sq_head, __ATOMIC_RELAXED);
            unsigned idx = p->sq_array[head & *p->sq_mask];
            struct exitos_sqe128 *sqe = &p->sqes[idx];
            const struct exitos_nvme_uring_cmd *nc =
                (const struct exitos_nvme_uring_cmd *)(const void *)sqe->cmd;
            unsigned tail;
            struct exitos_cqe *cqe;
            g.submits++;
            g.last_addr = nc->addr;
            g.last_data_len = nc->data_len;
            g.last_rw_flags = sqe->rw_flags;
            g.last_buf_index = sqe->buf_index;
            g.last_opcode = nc->opcode;
            memcpy(g.last_payload_head, (const void *)(uintptr_t)nc->addr,
                   sizeof g.last_payload_head);
            __atomic_store_n(p->sq_head, head + 1, __ATOMIC_RELEASE);
            tail = __atomic_load_n(p->cq_tail, __ATOMIC_RELAXED);
            cqe = &p->cqes[tail & *p->cq_mask];
            memset(cqe, 0, sizeof *cqe);
            cqe->user_data = sqe->user_data;
            cqe->res = 0;
            __atomic_store_n(p->cq_tail, tail + 1, __ATOMIC_RELEASE);
        }
        ret = (long)to_submit;
    } else {
        errno = ENOSYS;
    }
    va_end(ap);
    return ret;
}

static void *fake_mmap(void *addr, size_t length, int prot, int flags,
                       int fd, off_t offset)
{
    (void)addr; (void)prot;
    if (fd == -1 && (flags & MAP_ANONYMOUS)) {
        if (length > sizeof g.stage_backing)
            return MAP_FAILED;
        g.anon_map_calls++;
        g.stage_mapped = g.stage_backing;
        return g.stage_backing;
    }
    if (offset == (off_t)IORING_OFF_SQ_RING) return g.sq_map;
    if (offset == (off_t)IORING_OFF_CQ_RING) return g.cq_map;
    if (offset == (off_t)IORING_OFF_SQES) return g.sqes_map;
    errno = EINVAL;
    return MAP_FAILED;
}

static int fake_munmap(void *addr, size_t length)
{
    (void)length;
    if (addr == g.stage_backing) g.anon_unmap_calls++;
    return 0;
}

static int fake_close(int fd) { (void)fd; return 0; }

static int fake_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    (void)fd;
    if (request == (unsigned long)NVME_IOCTL_IO_CMD) {
        struct nvme_passthru_cmd *c;
        va_start(ap, request);
        c = va_arg(ap, struct nvme_passthru_cmd *);
        va_end(ap);
        g.submits++;
        g.last_addr = c->addr;
        g.last_data_len = c->data_len;
        g.last_opcode = c->opcode;
        g.last_rw_flags = 0;
        g.last_buf_index = 0;
        memcpy(g.last_payload_head, (const void *)(uintptr_t)c->addr,
               sizeof g.last_payload_head);
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

static int fake_madvise(void *addr, size_t length, int advice)
{
    (void)addr; (void)length;
    if (advice == MADV_COLLAPSE) {
        g.collapse_calls++;
        if (g.collapse_rc != 0) {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

static int fake_open(const char *path, int flags, ...)
{
    (void)flags;
    if (path && strcmp(path, "/proc/self/pagemap") == 0) {
        if (g.pagemap_mode) return 46;
        errno = EACCES;
        return -1;
    }
    errno = ENOENT;
    return -1;
}

static ssize_t fake_pread(int fd, void *buf, size_t len, off_t off)
{
    (void)off;
    if (fd == 46 && len == sizeof(uint64_t) && g.pagemap_mode) {
        uint64_t pfn = g.pagemap_mode == 1
                     ? UINT64_C(5000) + g.pagemap_reads
                     : UINT64_C(5000) + 2u * g.pagemap_reads;
        uint64_t e = pfn | (UINT64_C(1) << 63);
        g.pagemap_reads++;
        memcpy(buf, &e, sizeof e);
        return (ssize_t)sizeof e;
    }
    errno = EBADF;
    return -1;
}

static int prepare_uring_handle(struct iopath *p)
{
    int rc;
    memset(p, 0, sizeof *p);
    p->fd = 77;
    p->ring_fd = -1;
    p->backend = IOPATH_URING_CMD;
    p->lbs = 4096;
    p->nsid = 9;
    rc = uring_setup(p);
    if (rc == 0) {
        *p->sq_mask = 0;
        *p->cq_mask = 15;
        g.path = p;
    }
    return rc;
}

static void prepare_ioctl_handle(struct iopath *p)
{
    memset(p, 0, sizeof *p);
    p->fd = 78;
    p->ring_fd = -1;
    p->backend = IOPATH_NVME_IOCTL;
    p->lbs = 4096;
    p->nsid = 9;
}

static void release_stage(struct iopath *p)
{
    iopath_stage_teardown(p);
}

int main(void)
{
    static unsigned char src[32768] __attribute__((aligned(4096)));
    struct iopath p;
    int rc;

    for (size_t i = 0; i < sizeof src; i++)
        src[i] = (unsigned char)(i * 131u + 7u);

    /* 1: default off. */
    fake_reset();
    T_EQ(prepare_uring_handle(&p), 0, "fake QD1 ring setup succeeds");
    iopath_stage_init(&p);
    T_OK(p.stage_slot == NULL, "staging stays off without EXITOS_STAGE=1");
    rc = iopath_write(&p, 4096, src, 8192);
    T_EQ(rc, 0, "8 KiB write succeeds without staging");
    T_OK(g.last_addr == (uint64_t)(uintptr_t)src,
         "unstaged command carries the caller's buffer address");
    T_EQ(g.anon_map_calls, 0, "no staging region is allocated when off");
    uring_teardown(&p);

    /* 2: gate on, collapse ok, pagemap unreadable -> staged + registered. */
    fake_reset();
    setenv("EXITOS_STAGE", "1", 1);
    T_EQ(prepare_uring_handle(&p), 0, "ring setup succeeds (staged case)");
    iopath_stage_init(&p);
    T_OK(p.stage_slot != NULL,
         "collapse success accepts the slot when pagemap is unreadable");
    T_EQ(g.register_calls, 1, "uring backend registers the slot exactly once");
    T_EQ(g.register_ring_fd, 901, "registration targets the data ring fd");
    T_EQ(g.register_opcode, 0, "registration opcode is IORING_REGISTER_BUFFERS");
    T_OK(g.register_base == p.stage_slot, "registered iovec is the slot");
    T_EQ(p.stage_fixed, 1, "successful registration arms fixed-buffer submits");
    rc = iopath_write(&p, 4096, src, 8192);
    T_EQ(rc, 0, "staged 8 KiB write succeeds");
    T_OK(g.last_addr == (uint64_t)(uintptr_t)p.stage_slot,
         "staged command carries the slot address, not the caller's buffer");
    T_OK(g.last_rw_flags & 1u, "staged submit sets IORING_URING_CMD_FIXED");
    T_EQ(g.last_buf_index, 0, "staged submit uses registered buffer index 0");
    T_OK(memcmp(p.stage_slot, src, 8192) == 0,
         "slot holds the caller's bytes at submit time");
    T_OK(memcmp(g.last_payload_head, src, sizeof g.last_payload_head) == 0,
         "payload visible to the kernel equals the caller's bytes");

    /* 3: size policy on the same staged handle. */
    rc = iopath_write(&p, 4096, src, 4096);
    T_EQ(rc, 0, "single-page write succeeds");
    T_OK(g.last_addr == (uint64_t)(uintptr_t)src,
         "single-page write is never staged");
    T_OK((g.last_rw_flags & 1u) == 0,
         "single-page write carries no fixed-buffer flag");
    rc = iopath_write(&p, 4096, src, 32768);
    T_EQ(rc, 0, "32 KiB write succeeds");
    T_OK(g.last_addr == (uint64_t)(uintptr_t)p.stage_slot,
         "32 KiB write is staged (upper policy bound inclusive)");

    /* 4: reads are never staged. */
    rc = iopath_read(&p, 4096, src, 8192);
    T_EQ(rc, 0, "8 KiB read succeeds");
    T_OK(g.last_addr == (uint64_t)(uintptr_t)src,
         "read commands carry the caller's buffer, never the slot");
    T_OK((g.last_rw_flags & 1u) == 0, "read commands are not fixed-buffer");
    release_stage(&p);
    T_EQ(g.anon_unmap_calls, 1, "stage teardown unmaps the staging region");
    uring_teardown(&p);

    /* 5: collapse failure with unreadable pagemap refuses staging. */
    fake_reset();
    setenv("EXITOS_STAGE", "1", 1);
    g.collapse_rc = -1;
    T_EQ(prepare_uring_handle(&p), 0, "ring setup succeeds (refusal case)");
    iopath_stage_init(&p);
    T_OK(p.stage_slot == NULL,
         "unprovable slot contiguity disables staging");
    T_EQ(g.register_calls, 0, "no registration without an accepted slot");
    rc = iopath_write(&p, 4096, src, 8192);
    T_EQ(rc, 0, "write still succeeds unstaged");
    T_OK(g.last_addr == (uint64_t)(uintptr_t)src,
         "refused staging falls back to the caller's buffer");
    uring_teardown(&p);

    /* 6: readable pagemap overrides a lying collapse. */
    fake_reset();
    setenv("EXITOS_STAGE", "1", 1);
    g.pagemap_mode = 2;
    T_EQ(prepare_uring_handle(&p), 0, "ring setup succeeds (pagemap case)");
    iopath_stage_init(&p);
    T_OK(p.stage_slot == NULL,
         "scattered pagemap runs refuse the slot even when collapse returned 0");
    uring_teardown(&p);

    /* 7: readable contiguous pagemap accepts even when collapse fails. */
    fake_reset();
    setenv("EXITOS_STAGE", "1", 1);
    g.pagemap_mode = 1;
    g.collapse_rc = -1;
    T_EQ(prepare_uring_handle(&p), 0, "ring setup succeeds (pagemap-ok case)");
    iopath_stage_init(&p);
    T_OK(p.stage_slot != NULL,
         "pagemap proof of one run accepts the slot without collapse");
    release_stage(&p);
    uring_teardown(&p);

    /* 8: registration failure keeps staging but without the fixed flag. */
    fake_reset();
    setenv("EXITOS_STAGE", "1", 1);
    g.fail_register = 1;
    T_EQ(prepare_uring_handle(&p), 0, "ring setup succeeds (no-register case)");
    iopath_stage_init(&p);
    T_OK(p.stage_slot != NULL, "slot survives a failed registration");
    T_EQ(p.stage_fixed, 0, "failed registration disarms fixed-buffer submits");
    rc = iopath_write(&p, 4096, src, 8192);
    T_EQ(rc, 0, "staged write without registration succeeds");
    T_OK(g.last_addr == (uint64_t)(uintptr_t)p.stage_slot,
         "memcpy staging still engages without registration");
    T_OK((g.last_rw_flags & 1u) == 0,
         "no fixed flag is set when registration failed");
    release_stage(&p);
    uring_teardown(&p);

    /* 9: ioctl backend stages by memcpy, never registers. */
    fake_reset();
    setenv("EXITOS_STAGE", "1", 1);
    prepare_ioctl_handle(&p);
    iopath_stage_init(&p);
    T_OK(p.stage_slot != NULL, "ioctl backend accepts the slot");
    T_EQ(g.register_calls, 0, "ioctl backend never touches io_uring_register");
    rc = iopath_write(&p, 4096, src, 8192);
    T_EQ(rc, 0, "staged ioctl write succeeds");
    T_OK(g.last_addr == (uint64_t)(uintptr_t)p.stage_slot,
         "staged ioctl command carries the slot address");
    T_OK(memcmp(g.last_payload_head, src, sizeof g.last_payload_head) == 0,
         "ioctl payload equals the caller's bytes");
    rc = iopath_write(&p, 4096, src, 4096);
    T_OK(g.last_addr == (uint64_t)(uintptr_t)src,
         "single-page ioctl write is never staged");
    release_stage(&p);

    /* 10: pwrite backend ignores the gate entirely. */
    fake_reset();
    setenv("EXITOS_STAGE", "1", 1);
    memset(&p, 0, sizeof p);
    p.fd = 79;
    p.ring_fd = -1;
    p.backend = IOPATH_PWRITE;
    p.lbs = 4096;
    iopath_stage_init(&p);
    T_OK(p.stage_slot == NULL,
         "pwrite backend keeps PRP and never allocates a slot");
    T_EQ(g.anon_map_calls, 0, "pwrite backend maps no staging region");

    T_DONE();
}
