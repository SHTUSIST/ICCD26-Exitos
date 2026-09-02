/* Micro-optimization contract tests for the iopath hot path: fixed file,
 * modulo-to-mask arithmetic, and caller-serialized lock elision.  A registered
 * ring-fd index is task-private and therefore is deliberately not cached in a
 * handle that other threads may submit through.
 * None of these choices may change refusal or error semantics.  Includes the
 * production iopath.c behind fakes; no device. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <unistd.h>

#include "tap.h"

static long fake_syscall(long number, ...);
static void *fake_mmap(void *addr, size_t length, int prot, int flags,
                       int fd, off_t offset);
static int fake_munmap(void *addr, size_t length);
static int fake_close(int fd);
static int count_mutex_lock(pthread_mutex_t *m);
static int count_mutex_unlock(pthread_mutex_t *m);

void exitos_internal_fd_enter(int fd) { (void)fd; }
void exitos_internal_fd_leave(int fd) { (void)fd; }
int exitos_internal_open_call(const char *path, int flags, mode_t mode)
{ (void)path; (void)flags; (void)mode; errno = ENOSYS; return -1; }
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
#define pthread_mutex_lock count_mutex_lock
#define pthread_mutex_unlock count_mutex_unlock
#define IOPATH_EXTERNAL_INTERNAL_FD_HOOKS 1
#include "../../src/iopath.c"
#undef IOPATH_EXTERNAL_INTERNAL_FD_HOOKS
#undef pthread_mutex_unlock
#undef pthread_mutex_lock
#undef close
#undef munmap
#undef mmap
#undef syscall

struct fake_state {
    unsigned char sq_map[4096];
    unsigned char cq_map[4096];
    unsigned char sqes_map[4096];
    struct iopath *path;

    int fail_ring_reg;          /* IORING_REGISTER_RING_FDS fails */
    int fail_file_reg;          /* IORING_REGISTER_FILES fails */
    unsigned ring_reg_calls;
    unsigned file_reg_calls;
    int file_reg_fd;            /* the fd handed to REGISTER_FILES */

    unsigned enter_calls;
    int last_enter_fd;
    unsigned last_enter_flags;
    uint8_t last_sqe_flags;
    int32_t last_sqe_fd;

    unsigned locks;
    unsigned unlocks;
};

static struct fake_state g;

static void fake_reset(void)
{
    memset(&g, 0, sizeof g);
    g.last_enter_fd = -1;
    g.file_reg_fd = -1;
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
        ret = 901;
    } else if (number == __NR_io_uring_register) {
        int ring_fd = va_arg(ap, int);
        unsigned op = va_arg(ap, unsigned);
        void *arg = va_arg(ap, void *);
        unsigned nr = va_arg(ap, unsigned);
        (void)ring_fd; (void)nr;
        if (op == 20u /* IORING_REGISTER_RING_FDS */) {
            g.ring_reg_calls++;
            if (g.fail_ring_reg) {
                errno = EINVAL;
                ret = -1;
            } else {
                struct { uint32_t offset, resv; uint64_t data; } *upd = arg;
                upd->offset = 7;
                ret = 1;
            }
        } else if (op == 2u /* IORING_REGISTER_FILES */) {
            g.file_reg_calls++;
            if (g.fail_file_reg) {
                errno = EINVAL;
                ret = -1;
            } else {
                g.file_reg_fd = ((const int32_t *)arg)[0];
                ret = 0;
            }
        } else {
            errno = EINVAL;
            ret = -1;
        }
    } else if (number == __NR_io_uring_enter) {
        int enter_fd = va_arg(ap, int);
        unsigned to_submit = va_arg(ap, unsigned);
        struct iopath *p = g.path;
        g.enter_calls++;
        g.last_enter_fd = enter_fd;
        (void)va_arg(ap, unsigned);           /* min_complete */
        g.last_enter_flags = va_arg(ap, unsigned);
        if (p && to_submit) {
            unsigned head = __atomic_load_n(p->sq_head, __ATOMIC_RELAXED);
            unsigned idx = p->sq_array[head & *p->sq_mask];
            struct exitos_sqe128 *sqe = &p->sqes[idx];
            unsigned tail;
            struct exitos_cqe *cqe;
            g.last_sqe_flags = sqe->flags;
            g.last_sqe_fd = sqe->fd;
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
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd;
    if (offset == (off_t)IORING_OFF_SQ_RING) return g.sq_map;
    if (offset == (off_t)IORING_OFF_CQ_RING) return g.cq_map;
    if (offset == (off_t)IORING_OFF_SQES) return g.sqes_map;
    errno = EINVAL;
    return MAP_FAILED;
}

static int fake_munmap(void *addr, size_t length)
{ (void)addr; (void)length; return 0; }

static int fake_close(int fd) { (void)fd; return 0; }

static int count_mutex_lock(pthread_mutex_t *m)
{
    g.locks++;
    return (pthread_mutex_lock)(m);
}

static int count_mutex_unlock(pthread_mutex_t *m)
{
    g.unlocks++;
    return (pthread_mutex_unlock)(m);
}

static int prepare(struct iopath *p)
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

static struct nvme_passthru_cmd wcmd(void)
{
    struct nvme_passthru_cmd c;
    memset(&c, 0, sizeof c);
    c.opcode = NVME_OPC_WRITE;
    c.nsid = 9;
    c.addr = 0x1000;
    c.data_len = 4096;
    return c;
}

int main(void)
{
    static unsigned char abuf[8192] __attribute__((aligned(4096)));
    struct iopath p;
    struct nvme_passthru_cmd c;
    unsigned locks_before;
    off_t off = -1;

    /* 1: a shared handle always enters through the real ring fd.  Registered
     * ring-fd slots live in current->io_uring, so an index allocated by the
     * setup thread is not valid in an arbitrary submitter thread.  Fixed-file
     * registration is ring-local and remains safe. */
    fake_reset();
    T_EQ(prepare(&p), 0, "ring setup succeeds with registration available");
    T_EQ(g.ring_reg_calls, 0,
         "shared handle never allocates a task-private registered ring-fd slot");
    T_EQ(g.file_reg_calls, 1, "setup registers the device fd exactly once");
    T_EQ(g.file_reg_fd, 77, "the registered file is the device fd");
    T_EQ(p.enter_fd, 901, "enter retains the process fd valid in every thread");
    T_EQ(p.enter_flags, 0,
         "shared handle never carries IORING_ENTER_REGISTERED_RING");
    T_EQ(p.fixed_file, 1, "fixed-file submits are armed");
    c = wcmd();
    T_EQ(uring_cmd_submit(&p, &c, NULL), 0, "submit succeeds");
    T_EQ(g.last_enter_fd, 901, "io_uring_enter targeted the real ring fd");
    T_EQ(g.last_enter_flags & (1u << 4), 0,
         "io_uring_enter omitted the task-private registered-ring flag");
    T_OK(g.last_sqe_flags & 1u /* IOSQE_FIXED_FILE */,
         "the SQE marks its fd as a fixed-file index");
    T_EQ(g.last_sqe_fd, 0, "the SQE fd is the fixed-file table index");
    uring_teardown(&p);

    /* 2: fixed-file registration failure still uses raw descriptors. */
    fake_reset();
    g.fail_file_reg = 1;
    T_EQ(prepare(&p), 0, "ring setup succeeds without registration support");
    T_EQ(p.enter_fd, 901, "fallback enter uses the raw ring fd");
    T_EQ(p.enter_flags, 0, "fallback enter carries no registered-ring flag");
    T_EQ(p.fixed_file, 0, "fallback submits use the real device fd");
    c = wcmd();
    T_EQ(uring_cmd_submit(&p, &c, NULL), 0, "fallback submit succeeds");
    T_EQ(g.last_enter_fd, 901, "fallback enter targeted the raw ring fd");
    T_EQ(g.last_sqe_fd, 77, "fallback SQE carries the real device fd");
    T_OK((g.last_sqe_flags & 1u) == 0, "fallback SQE has no fixed-file flag");
    uring_teardown(&p);

    /* Closing a ring cannot clean registered-ring slots owned by other live
     * tasks.  Repeated shared-handle setup therefore must never allocate one. */
    for (unsigned cycle = 0; cycle < 8; cycle++) {
        fake_reset();
        T_EQ(prepare(&p), 0, "repeated shared-ring setup succeeds");
        T_EQ(g.ring_reg_calls, 0,
             "repeated setup cannot consume a task-private ring slot");
        uring_teardown(&p);
    }

    /* 3: lock elision under caller-declared exclusive ownership. */
    fake_reset();
    T_EQ(prepare(&p), 0, "ring setup succeeds (lock cases)");
    c = wcmd();
    locks_before = g.locks;
    T_EQ(uring_cmd_submit(&p, &c, NULL), 0, "shared-mode submit succeeds");
    T_OK(g.locks > locks_before, "shared mode takes the ring mutex");
    T_EQ(iopath_set_exclusive(&p, 1), 0, "exclusive ownership can be declared");
    locks_before = g.locks;
    c = wcmd();
    T_EQ(uring_cmd_submit(&p, &c, NULL), 0, "exclusive-mode submit succeeds");
    T_EQ(g.locks, locks_before, "exclusive mode takes no mutex at all");
    uring_teardown(&p);
    T_OK(iopath_set_exclusive(NULL, 1) != 0, "exclusive refuses a null handle");

    /* 4: mask arithmetic preserves every refusal check_io_args made. */
    fake_reset();
    memset(&p, 0, sizeof p);
    p.fd = 77;
    p.lbs = 4096;
    T_EQ(check_io_args(&p, abuf, 4096), 0, "aligned single-block I/O accepted");
    T_EQ(check_io_args(&p, abuf, 8192), 0, "aligned two-block I/O accepted");
    T_EQ(check_io_args(&p, abuf, 4097), -EINVAL, "ragged length still refused");
    T_EQ(check_io_args(&p, abuf + 512, 4096), -EINVAL,
         "misaligned buffer still refused");
    T_EQ(check_io_args(&p, abuf, 0), -EINVAL, "zero length still refused");
    T_EQ(check_io_args(&p, NULL, 4096), -EINVAL, "null buffer still refused");

    /* 5: off_t ceiling identical after memoization; overflow still refused. */
    T_OK(iopath_off_t_max() == (uint64_t)INT64_MAX,
         "off_t ceiling is INT64_MAX on this ABI");
    T_OK(iopath_off_t_max() == (uint64_t)INT64_MAX,
         "memoized ceiling is stable across calls");
    p.lbs = 4096;
    iopath_recompute_limits(&p);
    T_EQ(lba_to_offset(&p, 4, 4096, &off), 0, "in-range LBA converts");
    T_OK(off == (off_t)4 * 4096, "byte offset is lba times block size");
    T_EQ(lba_to_offset(&p, UINT64_MAX / 2, 4096, &off), -EOVERFLOW,
         "overflowing LBA still refused");

    T_DONE();
}
