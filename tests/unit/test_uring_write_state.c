/* Device-free white-box tests for IOPATH_URING_WRITE_POLL.
 *
 * The production implementation is included behind syscall, fd, ioctl,
 * devguard, and mmap fakes.  The fake fd is deliberately reported as a block
 * object, but no device node is opened and no I/O reaches a kernel block
 * driver.  SQ/CQ memory and the QD1 state machine are the production code. */
#include "tap.h"
#include "exitos_devguard.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/io_uring.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static long fake_syscall(long number, ...);
static void *fake_mmap(void *addr, size_t length, int prot, int flags,
                       int fd, off_t offset);
static int fake_munmap(void *addr, size_t length);
static int fake_fstat(int fd, struct stat *st);
static int fake_fcntl(int fd, int op, ...);
static int fake_ioctl(int fd, unsigned long request, ...);
static int fake_devguard_lba_span_fd(int fd, uint64_t *base, uint64_t *count);
static int fake_devguard_poll_available_fd(int fd);
static int fake_devguard_whole_block_at(const char *root, unsigned maj,
                                        unsigned min);
static int fake_devguard_nvme_pair_at(const char *root,
                                      unsigned bmaj, unsigned bmin,
                                      unsigned cmaj, unsigned cmin,
                                      uint32_t nsid);

static int fake_internal_open(const char *path, int flags, mode_t mode);
static int fake_internal_close(int fd);
static int fake_fdatasync(int fd);

void exitos_internal_fd_enter(int fd) { (void)fd; }
void exitos_internal_fd_leave(int fd) { (void)fd; }
int exitos_internal_open_call(const char *path, int flags, mode_t mode)
{
    return fake_internal_open(path, flags, mode);
}
int exitos_internal_openat_call(int dirfd, const char *path, int flags,
                                mode_t mode)
{
    (void)dirfd;
    return fake_internal_open(path, flags, mode);
}
int exitos_internal_close_call(int fd) { return fake_internal_close(fd); }
ssize_t exitos_internal_pwrite_call(int fd, const void *buf, size_t len,
                                    off_t off)
{
    (void)fd; (void)buf; (void)len; (void)off;
    errno = ENOSYS;
    return -1;
}
int exitos_internal_fdatasync_call(int fd) { return fake_fdatasync(fd); }

#define syscall fake_syscall
#define mmap fake_mmap
#define munmap fake_munmap
#define fstat fake_fstat
#define fcntl fake_fcntl
#define ioctl fake_ioctl
#define exitos_devguard_lba_span_fd fake_devguard_lba_span_fd
#define exitos_devguard_poll_available_fd fake_devguard_poll_available_fd
#define exitos_devguard_whole_block_at fake_devguard_whole_block_at
#define exitos_devguard_nvme_pair_at fake_devguard_nvme_pair_at
#define IOPATH_EXTERNAL_INTERNAL_FD_HOOKS 1
#include "../../src/iopath.c"
#undef IOPATH_EXTERNAL_INTERNAL_FD_HOOKS
#undef exitos_devguard_nvme_pair_at
#undef exitos_devguard_whole_block_at
#undef exitos_devguard_lba_span_fd
#undef exitos_devguard_poll_available_fd
#undef ioctl
#undef fcntl
#undef fstat
#undef munmap
#undef mmap
#undef syscall

#define SOURCE_FD 40
#define HELD_FD   41
#define POLL_FD   42
#define RING_FD   901

struct enter_action {
    int ret;
    int err;
    int consume;
    int complete;
    int32_t res;
    uint64_t token;
};

#define MAX_ACTIONS 32
#define MAX_ENTERS  64

struct fake_state {
    pthread_mutex_t lock;
    struct iopath *path;

    mode_t mode;
    int open_flags;
    int dup_errno;
    int fstat_errno;
    int getfl_errno;
    int lbs;
    uint64_t size_bytes;
    int blkssz_errno;
    int blkgetsize_errno;
    int span_rc;
    uint64_t base_lba;
    uint64_t count_lba;
    int poll_available;
    int poll_fd_seen;
    unsigned poll_calls;

    int setup_errno;
    unsigned setup_flags;
    size_t sqes_map_len;
    unsigned register_files_calls;
    unsigned register_buffers_calls;
    unsigned register_ring_calls;
    int register_files_ok;
    int register_ring_ok;

    struct enter_action actions[MAX_ACTIONS];
    unsigned nactions;
    unsigned action_pos;
    unsigned enter_calls;
    unsigned enter_to_submit[MAX_ENTERS];
    unsigned enter_min_complete[MAX_ENTERS];
    unsigned enter_flags[MAX_ENTERS];
    unsigned consumed;
    struct io_uring_sqe last_sqe;

    int auto_complete;
    atomic_int active_enters;
    atomic_int max_active_enters;

    unsigned close_calls;
    int fdatasync_calls;
    int fdatasync_fd;
    int fdatasync_errno;

    unsigned char sq_map[4096];
    unsigned char cq_map[4096];
    unsigned char sqes_map[4096];
};

static struct fake_state g = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void update_max_active(int active)
{
    int old = atomic_load(&g.max_active_enters);
    while (active > old &&
           !atomic_compare_exchange_weak(&g.max_active_enters, &old, active))
        ;
}

static void fake_reset(void)
{
    pthread_mutex_lock(&g.lock);
    g.path = NULL;
    g.mode = S_IFBLK | 0600;
    g.open_flags = O_RDWR | O_DIRECT;
    g.dup_errno = 0;
    g.fstat_errno = 0;
    g.getfl_errno = 0;
    g.lbs = 4096;
    g.size_bytes = UINT64_C(16) * 4096;
    g.blkssz_errno = 0;
    g.blkgetsize_errno = 0;
    g.span_rc = 0;
    g.base_lba = 100;
    g.count_lba = 16;
    g.poll_available = 1;
    g.poll_fd_seen = -1;
    g.poll_calls = 0;
    g.setup_errno = 0;
    g.setup_flags = 0;
    g.sqes_map_len = 0;
    g.register_files_calls = 0;
    g.register_buffers_calls = 0;
    g.register_ring_calls = 0;
    g.register_files_ok = 1;
    /* Deliberately offer the optimization: production must still refuse to
     * cache this setup-thread-local index in a cross-thread-capable handle. */
    g.register_ring_ok = 1;
    memset(g.actions, 0, sizeof g.actions);
    g.nactions = 0;
    g.action_pos = 0;
    g.enter_calls = 0;
    memset(g.enter_to_submit, 0, sizeof g.enter_to_submit);
    memset(g.enter_min_complete, 0, sizeof g.enter_min_complete);
    memset(g.enter_flags, 0, sizeof g.enter_flags);
    g.consumed = 0;
    memset(&g.last_sqe, 0, sizeof g.last_sqe);
    g.auto_complete = 0;
    atomic_store(&g.active_enters, 0);
    atomic_store(&g.max_active_enters, 0);
    g.close_calls = 0;
    g.fdatasync_calls = 0;
    g.fdatasync_fd = -1;
    g.fdatasync_errno = 0;
    memset(g.sq_map, 0, sizeof g.sq_map);
    memset(g.cq_map, 0, sizeof g.cq_map);
    memset(g.sqes_map, 0, sizeof g.sqes_map);
    pthread_mutex_unlock(&g.lock);
    unsetenv("EXITOS_STAGE");
}

static void add_action(int ret, int err, int consume, int complete, int32_t res)
{
    struct enter_action *a = &g.actions[g.nactions++];
    memset(a, 0, sizeof *a);
    a->ret = ret;
    a->err = err;
    a->consume = consume;
    a->complete = complete;
    a->res = res;
}

static struct io_uring_sqe *standard_sqe(struct iopath *p, unsigned index)
{
    return (struct io_uring_sqe *)(void *)
        ((unsigned char *)(void *)p->sqes + (size_t)index * p->sqe_stride);
}

static struct io_uring_cqe *standard_cqe(struct iopath *p, unsigned index)
{
    return (struct io_uring_cqe *)(void *)
        ((unsigned char *)(void *)p->cqes + (size_t)index * p->cqe_stride);
}

static uint64_t consume_one_locked(void)
{
    struct iopath *p = g.path;
    unsigned head = __atomic_load_n(p->sq_head, __ATOMIC_RELAXED);
    unsigned pos = head & *p->sq_mask;
    unsigned index = p->sq_array[pos];
    struct io_uring_sqe *sqe = standard_sqe(p, index);
    memcpy(&g.last_sqe, sqe, sizeof g.last_sqe);
    __atomic_store_n(p->sq_head, head + 1, __ATOMIC_RELEASE);
    g.consumed++;
    return sqe->user_data;
}

static void complete_locked(uint64_t token, int32_t res)
{
    struct iopath *p = g.path;
    unsigned tail = __atomic_load_n(p->cq_tail, __ATOMIC_RELAXED);
    struct io_uring_cqe *cqe = standard_cqe(p, tail & *p->cq_mask);
    memset(cqe, 0, sizeof *cqe);
    cqe->user_data = token;
    cqe->res = res;
    __atomic_store_n(p->cq_tail, tail + 1, __ATOMIC_RELEASE);
}

static int fake_enter(unsigned to_submit, unsigned min_complete, unsigned flags)
{
    struct enter_action a;
    uint64_t token = 0;
    int active = atomic_fetch_add(&g.active_enters, 1) + 1;
    update_max_active(active);

    pthread_mutex_lock(&g.lock);
    if (g.enter_calls < MAX_ENTERS) {
        g.enter_to_submit[g.enter_calls] = to_submit;
        g.enter_min_complete[g.enter_calls] = min_complete;
        g.enter_flags[g.enter_calls] = flags;
    }
    g.enter_calls++;

    if (g.auto_complete) {
        memset(&a, 0, sizeof a);
        a.ret = to_submit ? (int)to_submit : 0;
        if (to_submit) {
            token = consume_one_locked();
            complete_locked(token, (int32_t)g.last_sqe.len);
        }
    } else if (g.action_pos < g.nactions) {
        a = g.actions[g.action_pos++];
        if (a.consume)
            token = consume_one_locked();
        if (a.complete) {
            if (a.token != 0)
                token = a.token;
            complete_locked(token, a.res);
        }
    } else {
        memset(&a, 0, sizeof a);
        a.ret = to_submit ? (int)to_submit : 0;
        if (to_submit) {
            token = consume_one_locked();
            complete_locked(token, (int32_t)g.last_sqe.len);
        }
    }
    pthread_mutex_unlock(&g.lock);

    if (g.auto_complete) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 12000000 };
        nanosleep(&ts, NULL);
    }
    atomic_fetch_sub(&g.active_enters, 1);
    if (a.ret < 0)
        errno = a.err;
    return a.ret;
}

static long fake_syscall(long number, ...)
{
    va_list ap;
    long ret = -1;
    va_start(ap, number);
    if (number == __NR_io_uring_setup) {
        unsigned entries = va_arg(ap, unsigned);
        struct io_uring_params *pp = va_arg(ap, struct io_uring_params *);
        (void)entries;
        g.setup_flags = pp->flags;
        if (g.setup_errno) {
            errno = g.setup_errno;
            ret = -1;
        } else {
            memset(pp, 0, sizeof *pp);
            pp->sq_entries = 1;
            pp->cq_entries = 8;
            pp->sq_off.head = 0;
            pp->sq_off.tail = 4;
            pp->sq_off.ring_mask = 8;
            pp->sq_off.array = 16;
            pp->cq_off.head = 0;
            pp->cq_off.tail = 4;
            pp->cq_off.ring_mask = 8;
            pp->cq_off.cqes = 64;
            ret = RING_FD;
        }
    } else if (number == __NR_io_uring_register) {
        int ringfd = va_arg(ap, int);
        unsigned op = va_arg(ap, unsigned);
        void *arg = va_arg(ap, void *);
        unsigned nr = va_arg(ap, unsigned);
        (void)ringfd; (void)nr;
        if (op == EXITOS_IORING_REGISTER_FILES) {
            g.register_files_calls++;
            ret = g.register_files_ok ? 0 : -1;
            if (ret < 0) errno = EOPNOTSUPP;
        } else if (op == EXITOS_IORING_REGISTER_RING_FDS) {
            g.register_ring_calls++;
            if (g.register_ring_ok) {
                ((struct exitos_rsrc_update *)arg)->offset = 7;
                ret = 1;
            } else {
                errno = EOPNOTSUPP;
                ret = -1;
            }
        } else if (op == EXITOS_IORING_REGISTER_BUFFERS) {
            g.register_buffers_calls++;
            errno = EOPNOTSUPP;
            ret = -1;
        } else {
            errno = EINVAL;
            ret = -1;
        }
    } else if (number == __NR_io_uring_enter) {
        int ringfd = va_arg(ap, int);
        unsigned to_submit = va_arg(ap, unsigned);
        unsigned min_complete = va_arg(ap, unsigned);
        unsigned flags = va_arg(ap, unsigned);
        (void)ringfd;
        (void)va_arg(ap, void *);
        (void)va_arg(ap, size_t);
        ret = fake_enter(to_submit, min_complete, flags);
    } else {
        errno = ENOSYS;
    }
    va_end(ap);
    return ret;
}

static void *fake_mmap(void *addr, size_t length, int prot, int flags,
                       int fd, off_t offset)
{
    (void)addr; (void)prot; (void)flags; (void)fd;
    if (offset == (off_t)IORING_OFF_SQ_RING)
        return g.sq_map;
    if (offset == (off_t)IORING_OFF_CQ_RING)
        return g.cq_map;
    if (offset == (off_t)IORING_OFF_SQES) {
        g.sqes_map_len = length;
        return g.sqes_map;
    }
    errno = EINVAL;
    return MAP_FAILED;
}

static int fake_munmap(void *addr, size_t length)
{
    (void)addr; (void)length;
    return 0;
}

static int fake_fstat(int fd, struct stat *st)
{
    if (g.fstat_errno) {
        errno = g.fstat_errno;
        return -1;
    }
    if (fd != SOURCE_FD && fd != HELD_FD) {
        errno = EBADF;
        return -1;
    }
    memset(st, 0, sizeof *st);
    st->st_mode = g.mode;
    st->st_rdev = makedev(259, 5);
    return 0;
}

static int fake_fcntl(int fd, int op, ...)
{
    (void)fd;
    if (op == F_DUPFD_CLOEXEC) {
        if (g.dup_errno) {
            errno = g.dup_errno;
            return -1;
        }
        return HELD_FD;
    }
    if (op == F_GETFL) {
        if (g.getfl_errno) {
            errno = g.getfl_errno;
            return -1;
        }
        return g.open_flags;
    }
    errno = EINVAL;
    return -1;
}

static int fake_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;
    (void)fd;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (request == BLKSSZGET) {
        if (g.blkssz_errno) {
            errno = g.blkssz_errno;
            return -1;
        }
        *(int *)arg = g.lbs;
        return 0;
    }
    if (request == BLKGETSIZE64) {
        if (g.blkgetsize_errno) {
            errno = g.blkgetsize_errno;
            return -1;
        }
        *(uint64_t *)arg = g.size_bytes;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

static int fake_devguard_lba_span_fd(int fd, uint64_t *base, uint64_t *count)
{
    (void)fd;
    if (g.span_rc)
        return g.span_rc;
    *base = g.base_lba;
    *count = g.count_lba;
    return 0;
}

static int fake_devguard_poll_available_fd(int fd)
{
    g.poll_calls++;
    g.poll_fd_seen = fd;
    return g.poll_available;
}

static int fake_devguard_whole_block_at(const char *root, unsigned maj,
                                        unsigned min)
{
    (void)root; (void)maj; (void)min;
    return 1;
}

static int fake_devguard_nvme_pair_at(const char *root,
                                      unsigned bmaj, unsigned bmin,
                                      unsigned cmaj, unsigned cmin,
                                      uint32_t nsid)
{
    (void)root; (void)bmaj; (void)bmin; (void)cmaj; (void)cmin; (void)nsid;
    return 0;
}

static int fake_internal_open(const char *path, int flags, mode_t mode)
{
    (void)path;
    (void)flags; (void)mode;
    return POLL_FD;
}

static int fake_internal_close(int fd)
{
    (void)fd;
    g.close_calls++;
    return 0;
}

static int fake_fdatasync(int fd)
{
    g.fdatasync_calls++;
    g.fdatasync_fd = fd;
    if (g.fdatasync_errno) {
        errno = g.fdatasync_errno;
        return -1;
    }
    return 0;
}

static int prepare_handle(struct iopath *p)
{
    int rc;
    fake_reset();
    memset(p, 0, sizeof *p);
    p->fd = HELD_FD;
    p->ring_fd = -1;
    p->backend = IOPATH_URING_WRITE_POLL;
    p->lbs = 4096;
    p->base_lba = 100;
    p->count_lba = 16;
    iopath_recompute_limits(p);
    rc = uring_setup(p);
    if (rc == 0) {
        *p->sq_mask = 0;
        *p->cq_mask = 7;
        g.path = p;
    }
    return rc;
}

static void test_setup_and_poll_probe(void)
{
    struct iopath p;

    T_EQ(prepare_handle(&p), 0, "standard block-poll ring setup succeeds in memory");
    T_EQ(g.setup_flags, IORING_SETUP_IOPOLL,
         "block WRITE ring passes exactly IORING_SETUP_IOPOLL to the kernel");
    T_EQ(p.sqe_stride, sizeof(struct io_uring_sqe), "block ring uses SQE64 stride");
    T_EQ(p.cqe_stride, sizeof(struct io_uring_cqe), "block ring uses CQE16 stride");
    T_EQ(g.sqes_map_len, sizeof(struct io_uring_sqe),
         "one-entry block ring maps exactly one standard SQE");
    uring_teardown(&p);

    fake_reset();
    T_EQ(iopath_poll_available_fd(SOURCE_FD), 1,
         "fd-bound probe accepts exact parent queue io_poll=1");
    T_EQ(g.poll_calls, 1,
         "iopath delegates polling proof to the generation-bound devguard helper");
    T_EQ(g.poll_fd_seen, SOURCE_FD,
         "polling proof receives the exact retained block descriptor");
    g.poll_available = 0;
    T_EQ(iopath_poll_available_fd(SOURCE_FD), 0,
         "generation-bound helper refusal fails polling closed");
}

static void test_sqe_and_completion_contract(void)
{
    struct iopath p;
    void *buf = NULL;
    int rc;
    T_EQ(posix_memalign(&buf, 4096, 8192), 0, "allocated aligned fake payload");
    if (!buf) return;

    T_EQ(prepare_handle(&p), 0, "prepared block-poll handle for SQE inspection");
    add_action(1, 0, 1, 1, 8192);
    rc = iopath_write(&p, 102, buf, 8192);
    T_EQ(rc, 0, "exact-byte WRITE completion succeeds");
    T_EQ(g.last_sqe.opcode, IORING_OP_WRITE, "SQE opcode is ordinary IORING_OP_WRITE");
    T_EQ(g.last_sqe.fd, 0, "registered exact block fd is addressed by fixed-file index zero");
    T_OK((g.last_sqe.flags & IOSQE_FIXED_FILE) != 0, "fixed-file acceleration remains enabled");
    T_EQ(g.last_sqe.off, UINT64_C(2) * 4096,
         "absolute LBA is translated to partition-relative byte offset");
    T_EQ(g.last_sqe.addr, (uint64_t)(uintptr_t)buf, "SQE carries the caller buffer directly");
    T_EQ(g.last_sqe.len, 8192, "SQE carries the exact transfer length");
    T_EQ(g.last_sqe.rw_flags, 0, "ordinary WRITE has no implicit durability or fixed-buffer flags");
    T_EQ(g.register_buffers_calls, 0,
         "ordinary block WRITE never registers the arbitrary caller buffer");
    T_EQ(*p.sq_tail, 1, "one logical write publishes one SQE");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fresh handle for short completion");
    add_action(1, 0, 1, 1, 4096);
    T_EQ(iopath_write(&p, 100, buf, 8192), -EIO,
         "short positive WRITE completion is an error, never success");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fresh handle for negative completion");
    add_action(1, 0, 1, 1, -ENOSPC);
    T_EQ(iopath_write(&p, 100, buf, 8192), -ENOSPC,
         "negative WRITE CQE errno is preserved");
    uring_teardown(&p);

    fake_reset();
    g.register_files_ok = 0;
    memset(&p, 0, sizeof p);
    p.fd = HELD_FD;
    p.ring_fd = -1;
    p.backend = IOPATH_URING_WRITE_POLL;
    p.lbs = 4096;
    p.base_lba = 100;
    p.count_lba = 16;
    iopath_recompute_limits(&p);
    T_EQ(uring_setup(&p), 0, "ring survives unavailable fixed-file registration");
    *p.sq_mask = 0; *p.cq_mask = 7; g.path = &p;
    add_action(1, 0, 1, 1, 4096);
    T_EQ(iopath_write(&p, 100, buf, 4096), 0, "plain-fd SQE fallback preserves semantics");
    T_EQ(g.last_sqe.fd, HELD_FD, "plain-fd fallback carries the retained block fd");
    T_EQ(g.last_sqe.flags & IOSQE_FIXED_FILE, 0, "plain-fd fallback clears IOSQE_FIXED_FILE");
    uring_teardown(&p);

    free(buf);
}

static void test_retry_poison_and_bounds(void)
{
    struct iopath p;
    void *buf = NULL;
    unsigned calls;
    T_EQ(posix_memalign(&buf, 4096, 8192), 0, "allocated retry/bounds payload");
    if (!buf) return;

    T_EQ(prepare_handle(&p), 0, "prepared retry handle");
    add_action(-1, EINTR, 0, 0, 0);
    add_action(1, 0, 1, 1, 4096);
    T_EQ(iopath_write(&p, 100, buf, 4096), 0,
         "unconsumed EINTR retries the one published block WRITE");
    T_EQ(g.enter_calls, 2, "retry needs two enters");
    T_EQ(g.enter_to_submit[0], 1, "initial enter submits one request");
    T_EQ(g.enter_to_submit[1], 1, "unconsumed EINTR retries submission");
    T_EQ(g.consumed, 1, "retry never duplicates the block request");
    T_EQ(*p.sq_tail, 1, "retry publishes the SQE exactly once");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "prepared accepted-EINTR handle");
    add_action(-1, EINTR, 1, 0, 0);
    add_action(0, 0, 0, 1, 4096);
    g.actions[1].token = 1;
    T_EQ(iopath_write(&p, 100, buf, 4096), 0,
         "accepted request is drained after an interrupted enter");
    T_EQ(g.enter_to_submit[1], 0,
         "accepted request is only waited for and can never be resubmitted");
    T_EQ(g.consumed, 1, "accepted EINTR path consumes exactly one request");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "prepared stale-token handle");
    complete_locked(UINT64_C(0xdeadbeef), -EIO);
    calls = g.enter_calls;
    T_EQ(iopath_write(&p, 100, buf, 4096), -EUCLEAN,
         "stale completion poisons before publishing a new WRITE");
    T_EQ(g.enter_calls, calls, "poisoned pre-submit state performs no enter");
    T_EQ(*p.sq_tail, 0, "stale completion leaves SQ tail unpublished");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "prepared bounds handle");
    add_action(1, 0, 1, 1, 4096);
    T_EQ(iopath_write(&p, 100, buf, 4096), 0,
         "first LBA of exact block object is accepted");
    T_EQ(g.last_sqe.off, 0, "first absolute LBA maps to fd offset zero");
    add_action(1, 0, 1, 1, 8192);
    T_EQ(iopath_write(&p, 114, buf, 8192), 0,
         "transfer ending exactly at the object boundary is accepted");
    T_EQ(g.last_sqe.off, UINT64_C(14) * 4096,
         "last in-range transfer uses its exact relative offset");
    calls = g.enter_calls;
    T_EQ(iopath_write(&p, 99, buf, 4096), -ERANGE,
         "absolute LBA below the partition base is refused");
    T_EQ(iopath_write(&p, 115, buf, 8192), -ERANGE,
         "transfer crossing the partition end is refused");
    T_EQ(iopath_write(&p, UINT64_MAX, buf, 4096), -ERANGE,
         "absolute-LBA arithmetic overflow fails closed");
    T_EQ(g.enter_calls, calls, "all range failures occur before SQE publication");
    p.base_lba = 0;
    p.count_lba = UINT64_MAX;
    T_EQ(iopath_write(&p, p.max_lba + 1, buf, 4096), -EOVERFLOW,
         "partition-relative byte offset beyond off_t is refused");
    T_EQ(iopath_read(&p, 100, buf, 4096), -EOPNOTSUPP,
         "write-only backend never turns a read into an NVMe command on a block fd");
    uring_teardown(&p);
    free(buf);
}

struct worker_arg {
    struct iopath *p;
    const void *buf;
    uint64_t lba;
    int rc;
};

static void *write_worker(void *opaque)
{
    struct worker_arg *a = opaque;
    a->rc = iopath_write(a->p, a->lba, a->buf, 4096);
    return NULL;
}

static void test_serialization(void)
{
    struct iopath p;
    void *buf = NULL;
    pthread_t a, b;
    struct worker_arg wa, wb;
    T_EQ(posix_memalign(&buf, 4096, 4096), 0, "allocated concurrent payload");
    if (!buf) return;
    T_EQ(prepare_handle(&p), 0, "prepared shared block-poll handle");
    T_EQ(g.register_ring_calls, 0,
         "cross-thread-capable handle never registers a task-private ring index");
    T_EQ(p.enter_fd, RING_FD,
         "cross-thread-capable handle retains the real ring descriptor");
    T_EQ(p.enter_flags, 0,
         "cross-thread-capable handle never arms registered-ring enter");
    g.auto_complete = 1;
    wa = (struct worker_arg){ .p = &p, .buf = buf, .lba = 100, .rc = -999 };
    wb = (struct worker_arg){ .p = &p, .buf = buf, .lba = 101, .rc = -999 };
    T_EQ(pthread_create(&a, NULL, write_worker, &wa), 0, "started first submitter");
    T_EQ(pthread_create(&b, NULL, write_worker, &wb), 0, "started second submitter");
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    T_EQ(wa.rc, 0, "first concurrent write completes");
    T_EQ(wb.rc, 0, "second concurrent write completes");
    T_EQ(atomic_load(&g.max_active_enters), 1,
         "per-handle mutex serializes the complete publish/submit/reap transaction");
    T_EQ(g.consumed, 2, "both serialized requests are submitted once");
    for (unsigned i = 0; i < g.enter_calls && i < MAX_ENTERS; i++)
        T_EQ(g.enter_flags[i] & EXITOS_IORING_ENTER_REGISTERED_RING, 0,
             "every worker enters through the real ring fd");
    uring_teardown(&p);
    free(buf);
}

static struct iopath *open_block_handle(void)
{
    struct iopath *p = iopath_open_fd(SOURCE_FD, IOPATH_URING_WRITE_POLL, 4096);
    if (p) {
        *p->sq_mask = 0;
        *p->cq_mask = 7;
        g.path = p;
    }
    return p;
}

static void expect_open_refused(const char *what)
{
    struct iopath *p = open_block_handle();
    T_OK(p == NULL, "%s", what);
    if (p) iopath_close(p);
}

static void test_open_durability_and_staging(void)
{
    struct iopath *p;

    fake_reset();
    p = open_block_handle();
    T_OK(p != NULL, "exact O_RDWR|O_DIRECT block fd opens the block-poll backend");
    if (p) {
        T_EQ(p->fd, HELD_FD, "handle retains a duplicate of the exact open description");
        T_EQ(p->base_lba, 100, "handle caches the absolute partition base");
        T_EQ(p->count_lba, 16, "handle caches the exact partition length");
        T_EQ(iopath_set_fua(p, 1), -EOPNOTSUPP,
             "ordinary block WRITE cannot promise native NVMe-command FUA");
        T_EQ(iopath_get_fua(p), 0, "failed FUA enable leaves flush-based durability active");
        T_EQ(iopath_set_fua(p, 0), 0, "disabling FUA remains valid");
        T_EQ(iopath_flush(p), 0, "block WRITE backend flushes through fdatasync");
        T_EQ(g.fdatasync_calls, 1, "flush dispatches one fdatasync");
        T_EQ(g.fdatasync_fd, HELD_FD, "fdatasync targets the retained exact block fd");
        iopath_close(p);
    }

    fake_reset();
    setenv("EXITOS_STAGE", "1", 1);
    p = open_block_handle();
    T_OK(p != NULL, "block-poll open remains valid when passthrough staging is requested");
    if (p) {
        T_OK(p->stage_region == NULL && p->stage_slot == NULL,
             "block WRITE backend does not automatically stage or bounce payloads");
        T_EQ(g.register_buffers_calls, 0,
             "block WRITE backend does not register a staging buffer");
        iopath_close(p);
    }
    unsetenv("EXITOS_STAGE");

    fake_reset(); g.mode = S_IFREG | 0600;
    expect_open_refused("ordinary files cannot masquerade as the exact block object");
    fake_reset(); g.open_flags = O_RDONLY | O_DIRECT;
    expect_open_refused("O_RDONLY block descriptor is refused");
    fake_reset(); g.open_flags = O_WRONLY | O_DIRECT;
    expect_open_refused("O_WRONLY block descriptor is refused because flush/read semantics need O_RDWR");
    fake_reset(); g.open_flags = O_RDWR;
    expect_open_refused("descriptor without O_DIRECT is refused with no buffered fallback");
    fake_reset(); g.getfl_errno = EBADF;
    expect_open_refused("unverifiable descriptor flags fail closed");
    fake_reset(); g.blkssz_errno = ENOTTY;
    expect_open_refused("missing BLKSSZGET fails closed");
    fake_reset(); g.lbs = 512;
    expect_open_refused("caller LBS mismatch fails closed");
    fake_reset(); g.blkgetsize_errno = ENOTTY;
    expect_open_refused("missing BLKGETSIZE64 fails closed");
    fake_reset(); g.size_bytes = 0;
    expect_open_refused("zero block-object size fails closed");
    fake_reset(); g.size_bytes = UINT64_C(16) * 4096 + 1;
    expect_open_refused("block-object byte size not divisible by LBS fails closed");
    fake_reset(); g.size_bytes = UINT64_C(15) * 4096;
    expect_open_refused("BLKGETSIZE64 and fd-bound sysfs span disagreement fails closed");
    fake_reset(); g.span_rc = -ENODEV;
    expect_open_refused("unverifiable fd-bound absolute LBA span fails closed");
    fake_reset(); g.base_lba = UINT64_MAX - 7; g.count_lba = 16;
    expect_open_refused("overflowing absolute object end fails closed");
    fake_reset(); g.poll_available = 0;
    expect_open_refused("object whose parent queue has io_poll=0 is refused");
    fake_reset(); g.poll_available = 0;
    expect_open_refused("malformed io_poll state is refused");
    fake_reset(); g.setup_errno = EPERM;
    expect_open_refused("io_uring setup failure has no silent backend fallback");
}

int main(void)
{
    test_setup_and_poll_probe();
    test_sqe_and_completion_contract();
    test_retry_poison_and_bounds();
    test_serialization();
    test_open_durability_and_staging();
    T_DONE();
}
