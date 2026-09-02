/* Pure-memory tests for the QD1 io_uring_cmd state machine.
 *
 * This translation unit includes the production implementation so it can drive
 * the otherwise-private ring code.  Only the four kernel-facing operations are
 * replaced; SQ/CQ memory, tokens, retry decisions, locking, and teardown are
 * the real iopath.c logic.  No device is opened and no block I/O is issued. */
#include "tap.h"

#include <errno.h>
#include <linux/io_uring.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>

static long fake_syscall(long number, ...);
static void *fake_mmap(void *addr, size_t length, int prot, int flags,
                       int fd, off_t offset);
static int fake_munmap(void *addr, size_t length);
static int fake_close(int fd);
static int g_marker_enter_calls;
static int g_marker_leave_calls;
static int g_marker_last_fd;

void exitos_internal_fd_enter(int fd)
{
    g_marker_enter_calls++;
    g_marker_last_fd = fd;
}

void exitos_internal_fd_leave(int fd)
{
    g_marker_leave_calls++;
    if (g_marker_last_fd != fd)
        g_marker_last_fd = -2;
}

int exitos_internal_open_call(const char *path, int flags, mode_t mode)
{
    (void)path; (void)flags; (void)mode;
    errno = ENOSYS;
    return -1;
}

int exitos_internal_openat_call(int dirfd, const char *path, int flags,
                                mode_t mode)
{
    (void)dirfd; (void)path; (void)flags; (void)mode;
    errno = ENOSYS;
    return -1;
}

int exitos_internal_close_call(int fd)
{
    return fake_close(fd);
}

ssize_t exitos_internal_pwrite_call(int fd, const void *buf, size_t count,
                                    off_t offset)
{
    (void)fd; (void)buf; (void)count; (void)offset;
    errno = ENOSYS;
    return -1;
}

int exitos_internal_fdatasync_call(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}

#define syscall fake_syscall
#define mmap fake_mmap
#define munmap fake_munmap
#define close fake_close
#define IOPATH_EXTERNAL_INTERNAL_FD_HOOKS 1
#include "../../src/iopath.c"
#undef IOPATH_EXTERNAL_INTERNAL_FD_HOOKS
#undef close
#undef munmap
#undef mmap
#undef syscall

enum token_source {
    TOKEN_EXPLICIT,
    TOKEN_CURRENT,
};

struct completion_action {
    enum token_source source;
    uint64_t token;
    int32_t res;
};

struct enter_action {
    int ret;
    int err;
    int consume;
    unsigned ncqes;
    struct completion_action cqes[4];
};

#define MAX_ACTIONS 32
#define MAX_CALLS 128
#define MAX_TOKENS 64

struct fake_kernel {
    pthread_mutex_t lock;
    struct iopath *path;

    struct enter_action actions[MAX_ACTIONS];
    unsigned nactions;
    unsigned action_pos;
    unsigned enter_calls;
    unsigned enter_to_submit[MAX_CALLS];
    unsigned enter_min_complete[MAX_CALLS];
    unsigned enter_flags[MAX_CALLS];
    uint64_t last_token;
    uint64_t submitted_tokens[MAX_TOKENS];
    unsigned nsubmitted_tokens;

    int concurrent_mode;
    atomic_int active_enters;
    atomic_int max_active_enters;

    int setup_errno;
    int setup_single_mmap;
    unsigned setup_calls;
    unsigned map_calls;
    unsigned fail_map_call;
    unsigned unmap_calls;
    void *unmapped[4];
    unsigned close_calls;
    int closed_fd;

    unsigned char sq_map[4096];
    unsigned char cq_map[4096];
    unsigned char sqes_map[4096];
};

static struct fake_kernel g = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void fake_reset(void)
{
    pthread_mutex_lock(&g.lock);
    g.path = NULL;
    memset(g.actions, 0, sizeof g.actions);
    g.nactions = 0;
    g.action_pos = 0;
    g.enter_calls = 0;
    memset(g.enter_to_submit, 0, sizeof g.enter_to_submit);
    memset(g.enter_min_complete, 0, sizeof g.enter_min_complete);
    memset(g.enter_flags, 0, sizeof g.enter_flags);
    g.last_token = 0;
    memset(g.submitted_tokens, 0, sizeof g.submitted_tokens);
    g.nsubmitted_tokens = 0;
    g.concurrent_mode = 0;
    atomic_store(&g.active_enters, 0);
    atomic_store(&g.max_active_enters, 0);
    g.setup_errno = 0;
    g.setup_single_mmap = 0;
    g.setup_calls = 0;
    g.map_calls = 0;
    g.fail_map_call = 0;
    g.unmap_calls = 0;
    memset(g.unmapped, 0, sizeof g.unmapped);
    g.close_calls = 0;
    g.closed_fd = -1;
    g_marker_enter_calls = 0;
    g_marker_leave_calls = 0;
    g_marker_last_fd = -1;
    memset(g.sq_map, 0, sizeof g.sq_map);
    memset(g.cq_map, 0, sizeof g.cq_map);
    memset(g.sqes_map, 0, sizeof g.sqes_map);
    pthread_mutex_unlock(&g.lock);
}

static void add_action(int ret, int err, int consume)
{
    struct enter_action *a;
    pthread_mutex_lock(&g.lock);
    a = &g.actions[g.nactions++];
    memset(a, 0, sizeof *a);
    a->ret = ret;
    a->err = err;
    a->consume = consume;
    pthread_mutex_unlock(&g.lock);
}

static void add_cqe(enum token_source source, uint64_t token, int32_t res)
{
    struct enter_action *a;
    pthread_mutex_lock(&g.lock);
    a = &g.actions[g.nactions - 1];
    a->cqes[a->ncqes].source = source;
    a->cqes[a->ncqes].token = token;
    a->cqes[a->ncqes].res = res;
    a->ncqes++;
    pthread_mutex_unlock(&g.lock);
}

static void update_max_active(int active)
{
    int old = atomic_load(&g.max_active_enters);
    while (active > old &&
           !atomic_compare_exchange_weak(&g.max_active_enters, &old, active))
        ;
}

static uint64_t consume_one_locked(unsigned to_submit)
{
    struct iopath *p = g.path;
    unsigned head;
    unsigned array_pos;
    unsigned sqe_index;
    uint64_t token;

    if (!p || to_submit == 0)
        return g.last_token;
    head = __atomic_load_n(p->sq_head, __ATOMIC_RELAXED);
    array_pos = head & *p->sq_mask;
    sqe_index = p->sq_array[array_pos];
    token = p->sqes[sqe_index].user_data;
    __atomic_store_n(p->sq_head, head + 1, __ATOMIC_RELEASE);
    g.last_token = token;
    if (g.nsubmitted_tokens < MAX_TOKENS)
        g.submitted_tokens[g.nsubmitted_tokens++] = token;
    return token;
}

static void queue_cqe_locked(const struct completion_action *ca)
{
    struct iopath *p = g.path;
    unsigned tail = __atomic_load_n(p->cq_tail, __ATOMIC_RELAXED);
    struct exitos_cqe *cqe = &p->cqes[tail & *p->cq_mask];

    memset(cqe, 0, sizeof *cqe);
    cqe->user_data = ca->source == TOKEN_CURRENT ? g.last_token : ca->token;
    cqe->res = ca->res;
    __atomic_store_n(p->cq_tail, tail + 1, __ATOMIC_RELEASE);
}

static int fake_enter(unsigned to_submit, unsigned min_complete, unsigned flags)
{
    struct enter_action a;
    unsigned i;
    int active = atomic_fetch_add(&g.active_enters, 1) + 1;
    update_max_active(active);

    pthread_mutex_lock(&g.lock);
    if (g.enter_calls < MAX_CALLS) {
        g.enter_to_submit[g.enter_calls] = to_submit;
        g.enter_min_complete[g.enter_calls] = min_complete;
        g.enter_flags[g.enter_calls] = flags;
    }
    g.enter_calls++;

    if (g.concurrent_mode) {
        memset(&a, 0, sizeof a);
        a.ret = to_submit ? (int)to_submit : 0;
        if (to_submit) {
            struct completion_action ca = { TOKEN_CURRENT, 0, 0 };
            consume_one_locked(to_submit);
            queue_cqe_locked(&ca);
        }
    } else if (g.action_pos < g.nactions) {
        a = g.actions[g.action_pos++];
        if (a.consume)
            consume_one_locked(to_submit ? to_submit : 1);
        for (i = 0; i < a.ncqes; i++)
            queue_cqe_locked(&a.cqes[i]);
    } else {
        /* Keep a broken implementation finite: once its explicit script is
         * exhausted, complete one pending request so assertions can report the
         * wrong enter arguments instead of hanging the test runner. */
        struct completion_action ca = { TOKEN_CURRENT, 0, 0 };
        memset(&a, 0, sizeof a);
        if (to_submit)
            consume_one_locked(to_submit);
        if (g.last_token)
            queue_cqe_locked(&ca);
        a.ret = to_submit ? (int)to_submit : 0;
    }
    pthread_mutex_unlock(&g.lock);

    if (g.concurrent_mode) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 15000000 };
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
    long ret;

    va_start(ap, number);
    if (number == __NR_io_uring_setup) {
        unsigned entries = va_arg(ap, unsigned);
        struct io_uring_params *pp = va_arg(ap, struct io_uring_params *);
        (void)entries;
        pthread_mutex_lock(&g.lock);
        g.setup_calls++;
        if (g.setup_errno) {
            errno = g.setup_errno;
            ret = -1;
        } else {
            memset(pp, 0, sizeof *pp);
            pp->sq_entries = 1;
            pp->cq_entries = 16;
            pp->features = g.setup_single_mmap ? IORING_FEAT_SINGLE_MMAP : 0;
            pp->sq_off.head = 0;
            pp->sq_off.tail = 4;
            pp->sq_off.ring_mask = 8;
            pp->sq_off.array = 16;
            pp->cq_off.head = 0;
            pp->cq_off.tail = 4;
            pp->cq_off.ring_mask = 8;
            pp->cq_off.cqes = 64;
            ret = 901;
        }
        pthread_mutex_unlock(&g.lock);
    } else if (number == __NR_io_uring_enter) {
        int ring_fd = va_arg(ap, int);
        unsigned to_submit = va_arg(ap, unsigned);
        unsigned min_complete = va_arg(ap, unsigned);
        unsigned flags = va_arg(ap, unsigned);
        (void)ring_fd;
        (void)va_arg(ap, void *);
        (void)va_arg(ap, size_t);
        ret = fake_enter(to_submit, min_complete, flags);
    } else {
        errno = ENOSYS;
        ret = -1;
    }
    va_end(ap);
    return ret;
}

static void *fake_mmap(void *addr, size_t length, int prot, int flags,
                       int fd, off_t offset)
{
    void *result;
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd;
    pthread_mutex_lock(&g.lock);
    g.map_calls++;
    if (g.fail_map_call == g.map_calls) {
        errno = ENOMEM;
        result = MAP_FAILED;
    } else if (offset == (off_t)IORING_OFF_SQ_RING) {
        result = g.sq_map;
    } else if (offset == (off_t)IORING_OFF_CQ_RING) {
        result = g.cq_map;
    } else if (offset == (off_t)IORING_OFF_SQES) {
        result = g.sqes_map;
    } else {
        errno = EINVAL;
        result = MAP_FAILED;
    }
    pthread_mutex_unlock(&g.lock);
    return result;
}

static int fake_munmap(void *addr, size_t length)
{
    (void)length;
    pthread_mutex_lock(&g.lock);
    if (g.unmap_calls < 4)
        g.unmapped[g.unmap_calls] = addr;
    g.unmap_calls++;
    pthread_mutex_unlock(&g.lock);
    return 0;
}

static int fake_close(int fd)
{
    pthread_mutex_lock(&g.lock);
    g.close_calls++;
    g.closed_fd = fd;
    pthread_mutex_unlock(&g.lock);
    return 0;
}

static int prepare_handle(struct iopath *p)
{
    int rc;
    fake_reset();
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

static struct nvme_passthru_cmd fake_command(uint64_t tag)
{
    struct nvme_passthru_cmd c;
    memset(&c, 0, sizeof c);
    c.opcode = NVME_OPC_WRITE;
    c.nsid = 9;
    c.addr = tag;
    c.data_len = 4096;
    return c;
}

static void test_initial_enter_retry_matrix(void)
{
    struct iopath p;
    struct nvme_passthru_cmd c = fake_command(1);
    int rc;

    T_EQ(prepare_handle(&p), 0, "fake QD1 ring setup succeeds");
    add_action(-1, EINTR, 0);
    add_action(1, 0, 1);
    add_cqe(TOKEN_CURRENT, 0, 0);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, 0, "EINTR before SQ-head consumption retries the pending SQE");
    T_EQ(g.enter_calls, 2, "unconsumed EINTR needs exactly one retry");
    T_EQ(g.enter_to_submit[0], 1, "initial enter asks to submit one SQE");
    T_EQ(g.enter_to_submit[1], 1, "unconsumed EINTR retries submission, not only waiting");
    T_EQ(*p.sq_tail, 1, "EINTR retry publishes the SQE exactly once");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fresh fake ring for consumed EINTR case");
    add_action(-1, EINTR, 1);
    add_action(0, 0, 0);
    add_cqe(TOKEN_CURRENT, 0, 0);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, 0, "EINTR after SQ-head consumption waits for the in-flight command");
    T_EQ(g.enter_calls, 2, "consumed EINTR waits once for completion");
    T_EQ(g.enter_to_submit[1], 0, "consumed EINTR never resubmits the SQE");
    T_EQ(*p.sq_tail, 1, "consumed EINTR still has one publication");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fresh fake ring for unconsumed short-submit case");
    add_action(0, 0, 0);
    add_action(1, 0, 1);
    add_cqe(TOKEN_CURRENT, 0, 0);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, 0, "zero/short submit with unchanged SQ head is retried");
    T_EQ(g.enter_calls, 2, "unconsumed short submit needs one retry");
    T_EQ(g.enter_to_submit[1], 1, "unconsumed short submit keeps to_submit=1");
    T_EQ(*p.sq_tail, 1, "short-submit retry does not republish the SQE");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fresh fake ring for consumed zero-return case");
    add_action(0, 0, 1);
    add_action(0, 0, 0);
    add_cqe(TOKEN_CURRENT, 0, 0);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, 0, "zero return with advanced SQ head is treated as in flight");
    T_EQ(g.enter_to_submit[1], 0, "advanced SQ head switches to wait-only enter");
    T_EQ(*p.sq_tail, 1, "consumed zero-return path publishes once");
    uring_teardown(&p);
}

static void test_tokens_and_completion_drain(void)
{
    struct iopath p;
    struct nvme_passthru_cmd c = fake_command(2);
    unsigned calls;
    int rc;

    T_EQ(prepare_handle(&p), 0, "fake ring setup for token sequence");
    add_action(1, 0, 1);
    add_cqe(TOKEN_CURRENT, 0, 0);
    add_action(1, 0, 1);
    add_cqe(TOKEN_CURRENT, 0, 0);
    T_EQ(uring_cmd_submit(&p, &c, NULL), 0, "first tokenized request completes");
    T_EQ(uring_cmd_submit(&p, &c, NULL), 0, "second tokenized request completes");
    T_EQ(g.nsubmitted_tokens, 2, "fake kernel consumed two requests");
    T_OK(g.submitted_tokens[0] != 0 && g.submitted_tokens[1] != 0,
         "every submitted user_data token is nonzero");
    T_OK(g.submitted_tokens[0] < g.submitted_tokens[1],
         "successive requests use monotonically increasing tokens");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fake ring setup for token wraparound");
    p.next_token = UINT64_MAX;
    add_action(1, 0, 1);
    add_cqe(TOKEN_CURRENT, 0, 0);
    T_EQ(uring_cmd_submit(&p, &c, NULL), 0, "request at token wraparound completes");
    T_EQ(g.submitted_tokens[0], 1,
         "token wraparound skips reserved user_data value zero");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fake ring setup for mismatched CQE ordering");
    add_action(1, 0, 1);
    add_cqe(TOKEN_EXPLICIT, UINT64_C(0xfeedface), -EIO);
    add_action(0, 0, 0);
    add_cqe(TOKEN_CURRENT, 0, 0);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, 0,
         "unexpected CQE cannot trigger fallback before the target completion arrives");
    T_EQ(g.enter_calls, 2, "token mismatch keeps waiting for the target token");
    T_EQ(g.enter_to_submit[1], 0, "token mismatch never resubmits an in-flight command");
    T_EQ(*p.cq_head, 2, "mismatched and target CQE32 entries are both consumed");
    calls = g.enter_calls;
    T_EQ(uring_cmd_submit(&p, &c, NULL), -EUCLEAN,
         "a token protocol mismatch poisons the ring before another publication");
    T_EQ(g.enter_calls, calls, "poisoned handle performs no further enter syscall");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fake ring setup for a visible CQE batch");
    add_action(1, 0, 1);
    add_cqe(TOKEN_CURRENT, 0, 0);
    add_cqe(TOKEN_EXPLICIT, UINT64_C(0x12345678), -EINVAL);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, 0, "target success is preserved after draining a later bad CQE");
    T_EQ(*p.cq_head, 2,
         "all visible CQE32-stride entries are consumed before returning");
    calls = g.enter_calls;
    T_EQ(uring_cmd_submit(&p, &c, NULL), -EUCLEAN,
         "later unexpected CQE leaves the handle poisoned");
    T_EQ(g.enter_calls, calls, "poison prevents a new SQE from reaching the kernel");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fake ring setup for target error drain");
    add_action(1, 0, 1);
    add_cqe(TOKEN_CURRENT, 0, -EIO);
    add_cqe(TOKEN_EXPLICIT, UINT64_C(0xbadc0de), -EINVAL);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, -EIO, "target CQE error is returned after completion is identified");
    T_EQ(*p.cq_head, 2, "error and unexpected CQEs are both consumed");
    uring_teardown(&p);
}

static void test_submit_errors_preserve_known_inflight(void)
{
    struct iopath p;
    struct nvme_passthru_cmd c = fake_command(4);
    unsigned calls;
    int rc;

    T_EQ(prepare_handle(&p), 0, "fake ring setup for positive-count/head-lag case");
    add_action(1, 0, 0);
    add_action(0, 0, 1);
    add_cqe(TOKEN_CURRENT, 0, 0);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, 0, "positive submit count proves in-flight state while SQ head lags");
    T_EQ(g.enter_to_submit[1], 0,
         "positive submit count prevents resubmission despite stale SQ head");
    T_EQ(*p.sq_tail, 1, "head-lag path still publishes only once");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fake ring setup for fatal consumed initial enter");
    add_action(-1, EIO, 1);
    add_action(0, 0, 0);
    add_cqe(TOKEN_CURRENT, 0, 0);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, 0,
         "fatal initial-enter error cannot escape after SQ head proves consumption");
    T_EQ(g.enter_to_submit[1], 0,
         "fatal consumed submit changes only to completion waiting");
    calls = g.enter_calls;
    T_EQ(uring_cmd_submit(&p, &c, NULL), -EUCLEAN,
         "fatal enter error poisons subsequent submissions");
    T_EQ(g.enter_calls, calls, "fatal-error poison blocks further kernel entry");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fake ring setup for fatal completion wait");
    add_action(1, 0, 1);
    add_action(-1, EIO, 0);
    add_action(0, 0, 0);
    add_cqe(TOKEN_CURRENT, 0, 0);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, 0,
         "fatal wait error cannot escape while target completion is unknown");
    T_EQ(g.enter_calls, 3, "fatal wait error keeps reaping until target CQE");
    calls = g.enter_calls;
    T_EQ(uring_cmd_submit(&p, &c, NULL), -EUCLEAN,
         "fatal wait error leaves the ring poisoned after target completion");
    T_EQ(g.enter_calls, calls, "wait-error poison blocks later kernel entry");
    uring_teardown(&p);

    T_EQ(prepare_handle(&p), 0, "fake ring setup for fatal pre-submit failure");
    add_action(-1, EBADF, 0);
    rc = uring_cmd_submit(&p, &c, NULL);
    T_EQ(rc, -EBADF, "fatal error before SQ consumption is returned to the caller");
    T_EQ(*p.sq_tail, 1,
         "fatal unconsumed error leaves the published SQE pending, not republished");
    calls = g.enter_calls;
    T_EQ(uring_cmd_submit(&p, &c, NULL), -EUCLEAN,
         "fatal ring syscall poisons the handle instead of reusing a pending slot");
    T_EQ(g.enter_calls, calls,
         "poison guarantees the pending SQE can never be submitted later");
    uring_teardown(&p);
}

static void test_stale_completion_refuses_publication(void)
{
    struct iopath p;
    struct nvme_passthru_cmd c = fake_command(3);
    struct completion_action stale = {
        TOKEN_EXPLICIT, UINT64_C(0x11111111), 0
    };

    T_EQ(prepare_handle(&p), 0, "fake ring setup for stale completion");
    pthread_mutex_lock(&g.lock);
    queue_cqe_locked(&stale);
    pthread_mutex_unlock(&g.lock);
    T_EQ(uring_cmd_submit(&p, &c, NULL), -EUCLEAN,
         "a stale CQE poisons a supposedly quiescent QD1 ring");
    T_EQ(*p.cq_head, 1, "stale CQE is consumed before refusing the request");
    T_EQ(*p.sq_tail, 0, "stale CQE is detected before publishing a new SQE");
    T_EQ(g.enter_calls, 0, "stale CQE refusal makes no enter syscall");
    uring_teardown(&p);
}

static void test_ring_counters_wrap_as_unsigned(void)
{
    struct iopath p;
    struct nvme_passthru_cmd c = fake_command(5);

    T_EQ(prepare_handle(&p), 0, "fake ring setup for 32-bit counter wrap");
    *p.sq_head = UINT_MAX;
    *p.sq_tail = UINT_MAX;
    *p.cq_head = UINT_MAX;
    *p.cq_tail = UINT_MAX;
    add_action(0, 0, 1);
    add_action(0, 0, 0);
    add_cqe(TOKEN_CURRENT, 0, 0);
    T_EQ(uring_cmd_submit(&p, &c, NULL), 0,
         "SQ/CQ head and tail wrap does not change QD1 request identity");
    T_EQ(*p.sq_head, 0, "SQ head wraps from UINT_MAX to zero");
    T_EQ(*p.sq_tail, 0, "SQ tail wraps from UINT_MAX to zero");
    T_EQ(*p.cq_head, 0, "CQ head wraps from UINT_MAX to zero");
    T_EQ(*p.cq_tail, 0, "CQ tail wraps from UINT_MAX to zero");
    uring_teardown(&p);
}

struct thread_arg {
    struct iopath *path;
    struct nvme_passthru_cmd command;
    int rc;
};

static void *submit_thread(void *opaque)
{
    struct thread_arg *a = opaque;
    a->rc = uring_cmd_submit(a->path, &a->command, NULL);
    return NULL;
}

static void test_multithreaded_handle_is_serialized(void)
{
    enum { NTHREADS = 4 };
    struct iopath p;
    struct thread_arg args[NTHREADS];
    pthread_t threads[NTHREADS];
    unsigned i, unique = 0;

    T_EQ(prepare_handle(&p), 0, "fake ring setup for concurrent submitters");
    g.concurrent_mode = 1;
    for (i = 0; i < NTHREADS; i++) {
        args[i].path = &p;
        args[i].command = fake_command(100 + i);
        args[i].rc = -999;
        T_EQ(pthread_create(&threads[i], NULL, submit_thread, &args[i]), 0,
             "started submit thread %u", i);
    }
    for (i = 0; i < NTHREADS; i++)
        T_EQ(pthread_join(threads[i], NULL), 0, "joined submit thread %u", i);
    for (i = 0; i < NTHREADS; i++)
        T_EQ(args[i].rc, 0, "submit thread %u received its completion", i);
    T_EQ(atomic_load(&g.max_active_enters), 1,
         "one handle never has overlapping publish/enter/harvest sections");
    T_EQ(g.nsubmitted_tokens, NTHREADS, "fake kernel consumed one SQE per thread");
    for (i = 0; i < g.nsubmitted_tokens; i++) {
        unsigned j;
        int seen = 0;
        for (j = 0; j < i; j++)
            if (g.submitted_tokens[j] == g.submitted_tokens[i]) seen = 1;
        if (!seen && g.submitted_tokens[i] != 0) unique++;
    }
    T_EQ(unique, NTHREADS, "concurrent callers retain distinct nonzero CQE tokens");
    T_EQ(*p.sq_head, *p.sq_tail, "serialized QD1 ring is quiescent after all joins");
    T_EQ(*p.cq_head, *p.cq_tail, "every concurrent completion was consumed");
    uring_teardown(&p);
}

static void test_setup_teardown_failure_paths(void)
{
    struct iopath p;
    unsigned fail;
    int rc;

    fake_reset();
    memset(&p, 0, sizeof p);
    p.backend = IOPATH_URING_CMD;
    p.ring_fd = -1;
    g.setup_errno = EMFILE;
    rc = uring_setup(&p);
    T_EQ(rc, -EMFILE, "setup syscall error is preserved");
    T_EQ(g.map_calls, 0, "setup failure maps nothing");
    T_EQ(g.close_calls, 0, "failed setup has no ring fd to close");
    T_EQ(p.ring_lock_init, 0, "failed setup destroys its initialized submit mutex");

    for (fail = 1; fail <= 3; fail++) {
        fake_reset();
        memset(&p, 0, sizeof p);
        p.backend = IOPATH_URING_CMD;
        p.ring_fd = -1;
        g.fail_map_call = fail;
        rc = uring_setup(&p);
        T_EQ(rc, -ENOMEM, "mmap failure %u is preserved", fail);
        T_EQ(g.map_calls, fail, "mmap failure %u stops immediately", fail);
        T_EQ(g.unmap_calls, fail - 1,
             "mmap failure %u releases every earlier mapping", fail);
        T_EQ(g.close_calls, 1, "mmap failure %u closes the ring fd", fail);
        T_EQ(p.ring_fd, -1, "mmap failure %u resets ring fd", fail);
        T_EQ(p.ring_lock_init, 0,
             "mmap failure %u destroys the submit mutex", fail);
    }

    fake_reset();
    memset(&p, 0, sizeof p);
    p.backend = IOPATH_URING_CMD;
    p.ring_fd = -1;
    g.setup_single_mmap = 1;
    g.fail_map_call = 2;
    rc = uring_setup(&p);
    T_EQ(rc, -ENOMEM, "SQE mmap failure is preserved for a single-mmap ring");
    T_EQ(g.unmap_calls, 1, "shared SQ/CQ mapping is unmapped exactly once");
    T_EQ(g.close_calls, 1, "single-mmap setup failure closes the ring fd");
    T_EQ(p.ring_lock_init, 0,
         "single-mmap setup failure destroys the submit mutex");

    T_EQ(prepare_handle(&p), 0, "fully mapped ring setup succeeds");
    uring_teardown(&p);
    T_EQ(g.unmap_calls, 3, "teardown releases SQEs, CQ, and SQ mappings");
    T_EQ(g.close_calls, 1, "teardown closes the ring fd exactly once");
    T_EQ(g_marker_enter_calls, 0,
         "ring close uses explicit internal boundary, not a stealable marker");
    T_EQ(g_marker_leave_calls, 0,
         "explicit internal ring close needs no frontend marker token");
    T_EQ(p.ring_lock_init, 0, "teardown destroys the submit mutex");
    uring_teardown(&p);
    T_EQ(g.unmap_calls, 3, "second teardown cannot unmap resources twice");
    T_EQ(g.close_calls, 1, "second teardown cannot close the ring fd twice");
}

int main(void)
{
    test_initial_enter_retry_matrix();
    test_tokens_and_completion_drain();
    test_submit_errors_preserve_known_inflight();
    test_stale_completion_refuses_publication();
    test_ring_counters_wrap_as_unsigned();
    test_multithreaded_handle_is_serialized();
    test_setup_teardown_failure_paths();
    T_DONE();
}
