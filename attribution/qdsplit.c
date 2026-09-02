/* Does splitting one 8 KiB write into several 4 KiB commands at higher queue
 * depth beat the filesystem writing 8 KiB as one request in one queue?
 *
 * Three things have to be separated to answer that, so this probe runs them as
 * separate arms and interleaves them within each repetition:
 *   - parallelism:   two 4 KiB commands submitted together, versus the same two
 *                    commands submitted one after the other. If the split helps
 *                    only when they are submitted together, the gain is device
 *                    concurrency; if it helps either way, a 4 KiB command is
 *                    intrinsically cheaper per byte than an 8 KiB one.
 *   - FUA:           the passthru arms have been setting FUA while the ext4 arm
 *                    could not, because this controller reports no volatile
 *                    write cache and the block layer therefore strips FUA before
 *                    it reaches the device. Every passthru arm runs both ways.
 *   - the interface: ext4 O_DIRECT, retained whole-device O_DIRECT pwrite,
 *                    and io_uring NVMe passthru.  The middle arm bypasses the
 *                    filesystem mapping while retaining an ordinary block
 *                    request, so it separates filesystem work from the
 *                    USERCMD/SGL choice made for passthru.
 *
 * iopath opens its ring with one entry, which cannot hold two commands in
 * flight, so the ring here is its own.
 *
 * Destructive safety is deliberately narrower than the historical interface:
 * non-samefile/raw-window runs are frozen.  A runnable experiment must create
 * an anonymous O_TMPFILE inode on the retained partition, initialize and
 * synchronize its complete immutable plan, map every whole request to one
 * stable extent, and re-prove the exact raw target is zero immediately before
 * submission.  SIGINT/SIGTERM request a cooperative stop and are deferred
 * across submit-to-known-completion intervals.  SIGKILL and host failure cannot
 * preserve this userspace lifetime proof and must not be used during a run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/mman.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/io_uring.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include "exitos_benchguard.h"
#include "exitos_devguard.h"
#include "exitos_durability.h"
#include "exitos_extent.h"
#include "exitos_iopath.h"

#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif
#ifndef IORING_REGISTER_BUFFERS
#define IORING_REGISTER_BUFFERS 0
#endif
/* SQE uring_cmd_flags bit 0 = IORING_URING_CMD_FIXED (uapi name arrived in
 * 6.1; defined locally so the file builds against older userspace headers). */
#define QDSPLIT_URING_CMD_FIXED 1u
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define OP_URING_CMD 46u
#define SETUP_SQE128 (1u << 10)
#define SETUP_CQE32  (1u << 11)
#define RING_DEPTH   8u

struct nvme_uring_cmd_l {
    uint8_t opcode, flags; uint16_t rsvd1; uint32_t nsid, cdw2, cdw3;
    uint64_t metadata, addr; uint32_t metadata_len, data_len;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15, timeout_ms, rsvd2;
};
#define NVME_URING_CMD_IO _IOWR('N', 0x80, struct nvme_uring_cmd_l)

struct sqe128 {
    uint8_t opcode, flags; uint16_t ioprio; int32_t fd; uint32_t cmd_op, pad1;
    uint64_t addr; uint32_t len, rw_flags; uint64_t user_data;
    uint16_t buf_index, personality; int32_t splice_fd_in; uint8_t cmd[80];
};
struct cqe32 { uint64_t user_data; int32_t res; uint32_t flags; uint64_t big[2]; };
_Static_assert(sizeof(struct nvme_uring_cmd_l) == 72, "nvme_uring_cmd is 72 bytes");
_Static_assert(sizeof(struct sqe128) == 128, "SQE128 is 128 bytes");
_Static_assert(offsetof(struct sqe128, cmd) == 48, "uring_cmd payload at byte 48");
_Static_assert(sizeof(struct cqe32) == 32, "CQE32 is 32 bytes");

/* Two rings, because the completion path is one of the things under test: one
 * ordinary ring whose completions arrive by interrupt, and one set up with
 * IORING_SETUP_IOPOLL whose completions the kernel spins for. The polled ring
 * only works if the driver was given poll queues at probe time. */
struct ring { int fd, dev_fd; struct sqe128 *sqes; struct cqe32 *cqes;
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_array;
    unsigned *cq_head, *cq_tail, *cq_mask;
    void *sq_ring_map, *cq_ring_map, *sqes_map;
    size_t sq_ring_len, cq_ring_len, sqes_len;
    unsigned inflight;
    uint32_t nsid; int polled; };
static struct ring R, RP;
static int have_poll;
static volatile sig_atomic_t stop_signal;

static void request_stop(int signo)
{
    stop_signal = signo;
}

static int stop_set(sigset_t *set)
{
    if (sigemptyset(set) != 0 || sigaddset(set, SIGINT) != 0 ||
        sigaddset(set, SIGTERM) != 0)
        return -1;
    return 0;
}

static int install_stop_handlers(struct sigaction *old_int,
                                 struct sigaction *old_term,
                                 sigset_t *old_mask)
{
    struct sigaction sa;
    sigset_t set;

    memset(&sa, 0, sizeof sa);
    if (stop_set(&set) != 0 || sigprocmask(SIG_BLOCK, NULL, old_mask) != 0)
        return -1;
    sa.sa_handler = request_stop;
    sa.sa_mask = set;
    if (sigaction(SIGINT, &sa, old_int) != 0) return -1;
    if (sigaction(SIGTERM, &sa, old_term) != 0) {
        (void)sigaction(SIGINT, old_int, NULL);
        return -1;
    }
    /* A launcher-provided blocked mask must not silently discard campaign
     * cancellation; after both handlers exist, make these signals deliverable. */
    if (sigprocmask(SIG_UNBLOCK, &set, NULL) != 0) {
        (void)sigaction(SIGTERM, old_term, NULL);
        (void)sigaction(SIGINT, old_int, NULL);
        return -1;
    }
    return 0;
}

static void restore_stop_handlers(const struct sigaction *old_int,
                                  const struct sigaction *old_term,
                                  const sigset_t *old_mask)
{
    (void)sigaction(SIGTERM, old_term, NULL);
    (void)sigaction(SIGINT, old_int, NULL);
    (void)sigprocmask(SIG_SETMASK, old_mask, NULL);
}

static int block_stop_signals(sigset_t *old_mask)
{
    sigset_t set;
    return stop_set(&set) == 0
         ? sigprocmask(SIG_BLOCK, &set, old_mask) : -1;
}

static int stop_exit_status(void)
{
    sig_atomic_t signo = stop_signal;
    return signo == SIGINT || signo == SIGTERM ? 128 + (int)signo : 7;
}

enum { N_LEGACY_ARM = 18, N_ARM = 20 };
#define QDSPLIT_HAS_CHECKED_PLAN_COUNT 1
#define OWNED_GUARD_BYTES (8192u)
#define READBACK_CHUNK_BYTES (1u << 20)
/* Staging region: one 2 MiB-alignable anonymous mapping whose aligned head
 * doubles as the physically-contiguous bounce slot.  The deliberately odd
 * region length keeps it distinguishable from the 4 MiB EXITOS_HUGEBUF
 * mapping in the fake-based safety suite. */
#define STAGE_REGION_BYTES ((size_t)(4u << 20) + 4096u)
#define STAGE_SLOT_BYTES (32768u)
static const char *const arm_name[N_ARM] = {
    "fs-8k-qd1", "pt-8k-qd1", "pt-8k-qd1-fua",
    "pt-4kx2-seq", "pt-4kx2-qd2", "pt-4kx2-qd2-fua",
    "pt-4k-qd1", "fs-4k-qd1", "fs-8k-uring", "pt-8k-ioctl",
    "pt-fua-poll", "fs-pwrite-fdatasync-prealloc",
    "fs-pwrite-fdatasync-append",
    "fs-8k-AA-control", "blk-8k-pwrite",
    "pt-8k-fixedbuf", "pt-8k-bounce", "pt-8k-bounce-fixed",
    "prod-pt-8k-iopoll", "prod-blk-8k-iopoll"
};

static int qdsplit_checked_plan_count(size_t iterations, size_t stride,
                                      size_t *out)
{
    if (!out || !stride) return -EINVAL;
    if (iterations > SIZE_MAX / stride) return -EOVERFLOW;
    *out = iterations * stride;
    return 0;
}

struct qdsplit_durability_binding {
    struct devguard_device_info info;
    dev_t rdev;
    uint64_t diskseq;
    char sysfs_target[PATH_MAX];
    char probe_path[PATH_MAX];
};

/* exitos_durability_probe() consumes only the basename of its path argument.
 * Bind that basename to the already-open whole block object: devguard's fd
 * resolver verifies the real sysfs target's `dev` and absence of `partition`,
 * while BLKGETDISKSEQ detects major:minor reuse even across A->B->A. */
static int qdsplit_durability_binding_capture(
    int fd, uint32_t expected_lbs, struct qdsplit_durability_binding *out)
{
    struct qdsplit_durability_binding binding;
    struct stat st;
    char devlink[64], target[PATH_MAX];
    const char *name;
    ssize_t got;
    int n;

    if (fd < 0 || !expected_lbs || !out ||
        fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode))
        return -EINVAL;
    memset(&binding, 0, sizeof binding);
    binding.rdev = st.st_rdev;
    if (ioctl(fd, BLKGETDISKSEQ, &binding.diskseq) != 0)
        return -errno;
    /* This production fd-bound probe retains target/parent/queue dirfds while
     * it verifies the sysfs parent's diskseq against BLKGETDISKSEQ(fd). */
    if (!exitos_devguard_poll_available_fd(fd)) return -ESTALE;
    if (exitos_devguard_resolve_fd(fd, &binding.info) != 0 ||
        binding.info.dev_major != major(st.st_rdev) ||
        binding.info.dev_minor != minor(st.st_rdev) ||
        binding.info.parent_major != binding.info.dev_major ||
        binding.info.parent_minor != binding.info.dev_minor ||
        binding.info.is_partition ||
        binding.info.logical_block_size != expected_lbs ||
        !binding.info.identity[0])
        return -ESTALE;
    n = snprintf(devlink, sizeof devlink, "/sys/dev/block/%u:%u",
                 major(st.st_rdev), minor(st.st_rdev));
    if (n < 0 || (size_t)n >= sizeof devlink) return -ENAMETOOLONG;
    got = readlink(devlink, target, sizeof target - 1);
    if (got <= 0 || (size_t)got >= sizeof target - 1) return -EIO;
    target[got] = '\0';
    name = strrchr(target, '/');
    if (!name || !*++name || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return -EINVAL;
    memcpy(binding.sysfs_target, target, (size_t)got + 1);
    n = snprintf(binding.probe_path, sizeof binding.probe_path,
                 "/sys/class/block/%s", name);
    if (n < 0 || (size_t)n >= sizeof binding.probe_path)
        return -ENAMETOOLONG;
    *out = binding;
    return 0;
}

static int qdsplit_durability_binding_same(
    const struct qdsplit_durability_binding *a,
    const struct qdsplit_durability_binding *b)
{
    return a && b && a->rdev == b->rdev && a->diskseq == b->diskseq &&
           a->info.logical_block_size == b->info.logical_block_size &&
           a->info.dev_major == b->info.dev_major &&
           a->info.dev_minor == b->info.dev_minor &&
           a->info.parent_major == b->info.parent_major &&
           a->info.parent_minor == b->info.parent_minor &&
           a->info.is_partition == b->info.is_partition &&
           strcmp(a->info.identity, b->info.identity) == 0 &&
           strcmp(a->info.controller_serial,
                  b->info.controller_serial) == 0 &&
           strcmp(a->sysfs_target, b->sysfs_target) == 0 &&
           strcmp(a->probe_path, b->probe_path) == 0;
}

struct qdsplit_plan_io {
    off_t foff;
    off_t block_off;
    uint64_t raw_off;
    uint64_t lba;
    size_t len;
    unsigned enabled;
    unsigned raw;
};

static int arm_is_raw(int arm)
{
    return (arm >= 1 && arm <= 6) || arm == 9 || arm == 10 || arm == 14 ||
           (arm >= 15 && arm <= 19);
}

static size_t arm_bytes(int arm, size_t big)
{
    return (arm == 6 || arm == 7) ? 4096u : big;
}

/* Largest contiguous buffer span passed to one kernel/device command.  Split
 * arms touch more bytes per sample, but each command describes one 4 KiB page;
 * claiming a scattered command from pages outside that span is not causal. */
static size_t arm_command_span(int arm, size_t big)
{
    return (arm >= 3 && arm <= 7) ? 4096u : big;
}

static int select_arms(const char *only_text, const char *paired,
                       int *only, int pset[6], int *pn, unsigned enabled[N_ARM])
{
    uint64_t parsed;
    int i;

    if (!only || !pset || !pn || !enabled || (only_text && paired))
        return -EINVAL;
    *only = -1;
    *pn = 0;
    memset(enabled, 0, N_ARM * sizeof enabled[0]);
    if (only_text) {
        if (exitos_bench_parse_u64(only_text, &parsed) != 0 ||
            parsed >= N_LEGACY_ARM)
            return -EINVAL;
        *only = (int)parsed;
        enabled[*only] = 1;
        return 0;
    }
    if (!paired) {
        for (i = 0; i < N_LEGACY_ARM; i++) enabled[i] = 1;
        return 0;
    }
    if (strcmp(paired, "8k") == 0) { pset[0]=0; pset[1]=1; *pn=2; }
    else if (strcmp(paired, "4k") == 0) { pset[0]=7; pset[1]=6; *pn=2; }
    else if (strcmp(paired, "8kall") == 0) {
        pset[0]=0; pset[1]=1; pset[2]=8; pset[3]=9; *pn=4;
    } else if (strcmp(paired, "aa") == 0) {
        pset[0]=0; pset[1]=13; pset[2]=1; pset[3]=9; *pn=4;
    } else if (strcmp(paired, "split") == 0) {
        pset[0]=0; pset[1]=1; pset[2]=4; pset[3]=5; *pn=4;
    } else if (strcmp(paired, "wal") == 0) {
        pset[0]=11; pset[1]=12; pset[2]=10; pset[3]=2; *pn=4;
    } else if (strcmp(paired, "poll") == 0) {
        pset[0]=0; pset[1]=2; pset[2]=10; pset[3]=11; *pn=4;
    } else if (strcmp(paired, "8kpath") == 0) {
        pset[0]=0; pset[1]=14; pset[2]=1; *pn=3;
    } else if (strcmp(paired, "opt") == 0) {
        pset[0]=0; pset[1]=1; pset[2]=15; pset[3]=16; pset[4]=17; *pn=5;
    } else if (strcmp(paired, "blockpoll") == 0) {
        pset[0]=0; pset[1]=18; pset[2]=19; *pn=3;
    } else {
        return -EINVAL;
    }
    for (i = 0; i < *pn; i++) enabled[pset[i]] = 1;
    return 0;
}

/* EXITOS_FSFILE now names only the target directory plus a human-readable
 * nominal leaf.  The benchmark inode itself must never become reachable by a
 * pathname: otherwise another process can truncate, punch or move its extents
 * after FIEMAP and turn an old, still-zero LBA into somebody else's block. */
static int open_file_parent(const char *nominal_path)
{
    char parent[PATH_MAX];
    char *slash;
    size_t len;

    if (!nominal_path || !*nominal_path) {
        errno = EINVAL;
        return -1;
    }
    len = strlen(nominal_path);
    if (len >= sizeof parent || nominal_path[len - 1] == '/') {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(parent, nominal_path, len + 1);
    slash = strrchr(parent, '/');
    if (!slash) {
        memcpy(parent, ".", 2);
    } else if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }
    return open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

static int open_owned_tmpfile(int parent_fd)
{
    return openat(parent_fd, ".", O_TMPFILE | O_EXCL | O_RDWR | O_DIRECT |
                                   O_CLOEXEC | O_NOFOLLOW, 0600);
}

static int poll_available_fd(int fd)
{
    struct stat st;
    char path[192];
    FILE *f;
    int value = 0;

    if (fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode) ||
        snprintf(path, sizeof path, "/sys/dev/block/%u:%u/queue/io_poll",
                 major(st.st_rdev), minor(st.st_rdev)) >= (int)sizeof path)
        return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    if (fscanf(f, "%d", &value) != 1) value = 0;
    fclose(f);
    return value ? 1 : 0;
}

static int partition_start_lba_fd(int fd, uint32_t lbs, uint64_t *out)
{
    struct stat st;
    char path[192];
    unsigned long long sectors;
    FILE *f;

    if (!out || fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode) ||
        snprintf(path, sizeof path, "/sys/dev/block/%u:%u/start",
                 major(st.st_rdev), minor(st.st_rdev)) >= (int)sizeof path)
        return -EINVAL;
    f = fopen(path, "r");
    if (!f) return -errno;
    if (fscanf(f, "%llu", &sectors) != 1) {
        fclose(f);
        return -EIO;
    }
    fclose(f);
    return exitos_bench_sectors_to_lba((uint64_t)sectors, lbs, out);
}

/* Produce every logical file position before the file is allocated.  Each
 * non-paired arm owns distinct slots; paired arms deliberately interleave
 * distinct slots in one compact region.  Nothing later is allowed to invent
 * an offset inside the timed loop. */
static int build_logical_plan(unsigned iters, size_t big, const char *paired,
                              const int pset[6], int pn,
                              const unsigned enabled[N_ARM],
                              struct qdsplit_plan_io *plan, size_t plan_stride,
                              uint64_t *primary_bytes)
{
    unsigned rank[N_ARM];
    uint64_t nr_primary = 0, max_end = 0;
    int a;

    if (!iters || !big || !enabled || !plan || !primary_bytes ||
        (plan_stride != N_LEGACY_ARM && plan_stride != N_ARM))
        return -EINVAL;
    for (a = 0; a < N_ARM; a++) rank[a] = UINT_MAX;
    if (paired) {
        if (pn <= 0 || pn > 6) return -EINVAL;
        for (a = 0; a < pn; a++) {
            if (pset[a] < 0 || (size_t)pset[a] >= plan_stride)
                return -EINVAL;
            rank[pset[a]] = (unsigned)a;
        }
    } else {
        for (a = 0; (size_t)a < plan_stride; a++)
            if (enabled[a] && a != 12) rank[a] = (unsigned)nr_primary++;
    }

    for (unsigned i = 0; i < iters; i++) {
        for (a = 0; (size_t)a < plan_stride; a++) {
            struct qdsplit_plan_io *io =
                &plan[(size_t)i * plan_stride + (size_t)a];
            struct exitos_bench_byte_span span;
            uint64_t ordinal, end;
            int rc;

            if (!enabled[a]) continue;
            io->enabled = 1;
            io->raw = (unsigned)arm_is_raw(a);
            io->len = arm_bytes(a, big);
            if (a == 12) {
                ordinal = i;
            } else if (paired) {
                if (rank[a] == UINT_MAX || i > UINT64_MAX / (uint64_t)pn)
                    return -EOVERFLOW;
                ordinal = (uint64_t)i * (uint64_t)pn + rank[a];
            } else {
                if (rank[a] == UINT_MAX ||
                    (uint64_t)rank[a] > UINT64_MAX / (uint64_t)iters)
                    return -EOVERFLOW;
                ordinal = (uint64_t)rank[a] * (uint64_t)iters + i;
            }
            rc = exitos_bench_byte_span(ordinal, 1, big, &span);
            if (rc != 0) return rc;
            io->foff = span.offset;
            io->raw_off = (uint64_t)span.offset;
            if (a == 12) continue; /* separate append file */
            if ((uint64_t)span.offset > UINT64_MAX - io->len)
                return -EOVERFLOW;
            end = (uint64_t)span.offset + io->len;
            if (end > max_end) max_end = end;
        }
    }
    /* Keep the retained primary file real even for an append-only run. */
    if (max_end == 0) max_end = big;
    *primary_bytes = max_end;
    return 0;
}

/* Keep every timed byte strictly between two owned, initialized guard ranges.
 * The append arm targets its own anonymous inode and therefore retains its
 * separate-file offset. */
static int add_owned_guards(struct qdsplit_plan_io *plan, size_t nplan,
                            size_t plan_stride,
                            uint64_t primary_bytes, uint64_t *owned_bytes)
{
    size_t i;

    if (!plan || !owned_bytes || !primary_bytes ||
        (plan_stride != N_LEGACY_ARM && plan_stride != N_ARM) ||
        primary_bytes > UINT64_MAX - 2u * (uint64_t)OWNED_GUARD_BYTES)
        return -EOVERFLOW;
    for (i = 0; i < nplan; i++) {
        struct qdsplit_plan_io *io = &plan[i];
        uint64_t logical, end;

        if (!io->enabled || i % plan_stride == 12) continue;
        if (io->foff < 0 || (uint64_t)io->foff != io->raw_off ||
            io->raw_off > primary_bytes ||
            io->len > primary_bytes - io->raw_off)
            return -ERANGE;
        logical = io->raw_off + OWNED_GUARD_BYTES;
        if (logical > UINT64_MAX - io->len) return -EOVERFLOW;
        end = logical + io->len;
        if (logical < OWNED_GUARD_BYTES ||
            end > OWNED_GUARD_BYTES + primary_bytes ||
            (uint64_t)(off_t)logical != logical)
            return -ERANGE;
        io->foff = (off_t)logical;
        io->raw_off = logical;
    }
    *owned_bytes = primary_bytes + 2u * (uint64_t)OWNED_GUARD_BYTES;
    return 0;
}

/* A pair's key is injective for every supported iteration and arm.  XOR with
 * a position-dependent word keeps that key injective at every eight-byte
 * lane, while explicit byte extraction makes the on-media pattern independent
 * of host endianness. */
static uint64_t sample_pattern_word(unsigned iteration, unsigned arm,
                                    uint64_t word_index)
{
    uint64_t key = ((uint64_t)iteration << 8) | (uint64_t)(arm + 1u);
    return key ^ (UINT64_C(0x9e3779b97f4a7c15) * (word_index + 1u));
}

static unsigned char sample_pattern_byte(unsigned iteration, unsigned arm,
                                         uint64_t byte_offset)
{
    uint64_t word = sample_pattern_word(iteration, arm, byte_offset / 8u);
    return (unsigned char)(word >> (8u * (unsigned)(byte_offset % 8u)));
}

static void fill_sample_pattern(void *buf, size_t len, unsigned iteration,
                                unsigned arm)
{
    unsigned char *p = buf;
    size_t i;

    for (i = 0; i < len; i++)
        p[i] = sample_pattern_byte(iteration, arm, (uint64_t)i);
}

static int ring_open(struct ring *r, int chardev_fd, uint32_t nsid,
                     unsigned extra_flags)
{
    struct io_uring_params pp;
    void *sq = MAP_FAILED, *cq = MAP_FAILED, *sqes = MAP_FAILED;
    size_t sq_len, cq_len, sqes_len;

    memset(&pp, 0, sizeof pp);
    pp.flags = SETUP_SQE128 | SETUP_CQE32 | extra_flags;
    r->polled = (extra_flags & IORING_SETUP_IOPOLL) ? 1 : 0;
    r->fd = (int)syscall(__NR_io_uring_setup, RING_DEPTH, &pp);
    if (r->fd < 0) { if (!r->polled) perror("io_uring_setup"); return -1; }
    if (pp.sq_entries < RING_DEPTH || pp.cq_entries < RING_DEPTH ||
        pp.sq_entries > (SIZE_MAX - pp.sq_off.array) / sizeof(unsigned) ||
        pp.cq_entries > (SIZE_MAX - pp.cq_off.cqes) / sizeof(struct cqe32)) {
        errno = EOVERFLOW;
        return -1;
    }
    sq_len = pp.sq_off.array + (size_t)pp.sq_entries * sizeof(unsigned);
    cq_len = pp.cq_off.cqes + (size_t)pp.cq_entries * sizeof(struct cqe32);
    sqes_len = (size_t)pp.sq_entries * sizeof(struct sqe128);
    if (pp.features & IORING_FEAT_SINGLE_MMAP) {
        size_t shared_len = sq_len > cq_len ? sq_len : cq_len;
        sq = mmap(NULL, shared_len, PROT_READ|PROT_WRITE,
                  MAP_SHARED|MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
        cq = sq;
        sq_len = cq_len = shared_len;
    } else {
        sq = mmap(NULL, sq_len, PROT_READ|PROT_WRITE,
                  MAP_SHARED|MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
        cq = mmap(NULL, cq_len, PROT_READ|PROT_WRITE,
                  MAP_SHARED|MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
    }
    sqes = mmap(NULL, sqes_len, PROT_READ|PROT_WRITE,
                MAP_SHARED|MAP_POPULATE, r->fd, IORING_OFF_SQES);
    if (sq == MAP_FAILED || cq == MAP_FAILED || sqes == MAP_FAILED) {
        perror("mmap");
        if (sqes != MAP_FAILED) munmap(sqes, sqes_len);
        if (cq != MAP_FAILED && cq != sq) munmap(cq, cq_len);
        if (sq != MAP_FAILED) munmap(sq, sq_len);
        return -1;
    }
    r->sq_ring_map = sq;
    r->cq_ring_map = cq;
    r->sqes_map = sqes;
    r->sq_ring_len = sq_len;
    r->cq_ring_len = cq_len;
    r->sqes_len = sqes_len;
    r->sqes = sqes;
    r->sq_head = (unsigned *)((char *)sq + pp.sq_off.head);
    r->sq_tail = (unsigned *)((char *)sq + pp.sq_off.tail);
    r->sq_mask = (unsigned *)((char *)sq + pp.sq_off.ring_mask);
    r->sq_array = (unsigned *)((char *)sq + pp.sq_off.array);
    r->cq_head = (unsigned *)((char *)cq + pp.cq_off.head);
    r->cq_tail = (unsigned *)((char *)cq + pp.cq_off.tail);
    r->cq_mask = (unsigned *)((char *)cq + pp.cq_off.ring_mask);
    r->cqes = (struct cqe32 *)((char *)cq + pp.cq_off.cqes);
    /* Borrow the already-open, identity-checked generic-character fd.  Opening
     * its pathname again here would reintroduce a check/use race. */
    r->dev_fd = chardev_fd;
    r->nsid = nsid;
    for (unsigned i = 0; i < pp.sq_entries; i++) r->sq_array[i] = i;
    return 0;
}

/* Reap everything the kernel has already consumed.  A command error poisons
 * the sample but is still a completion: returning on the first bad CQE would
 * allow cleanup to free the anonymous inode while a sibling raw write remains
 * in flight. */
static int ring_reap(struct ring *r, unsigned target, int expect, int validate)
{
    unsigned done = 0, seen = 0;
    int bad = 0;

    while (done < target) {
        unsigned h = __atomic_load_n(r->cq_head, __ATOMIC_RELAXED);
        if (h == __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE)) {
            if (syscall(__NR_io_uring_enter, r->fd, 0, target - done,
                        IORING_ENTER_GETEVENTS, NULL, 0) < 0) {
                bad = 1;
                /* close(uring_fd) queues asynchronous exit work; it is not a
                 * synchronous lifetime barrier.  Once the kernel has consumed
                 * a raw SQE there is therefore no safe timeout/fallback: keep
                 * the anonymous inode and user buffer alive and retry until a
                 * CQE proves that command can no longer touch its old LBA. */
                sched_yield();
                continue;
            }
            continue;
        }
        struct cqe32 *cqe = &r->cqes[h & *r->cq_mask];
        if (validate &&
            (cqe->user_data >= target ||
             (seen & (1u << cqe->user_data)) || cqe->res != expect))
            bad = 1;
        if (cqe->user_data < target)
            seen |= 1u << cqe->user_data;
        __atomic_store_n(r->cq_head, h + 1, __ATOMIC_RELEASE);
        done++;
        if (r->inflight) r->inflight--;
        else bad = 1;
    }
    return bad || done != target ? -1 : 0;
}

/* Drain accepted work before dropping mappings.  Linux ring release schedules
 * exit work asynchronously, so close is deliberately used only after inflight
 * reached zero; callers keep all file descriptors and buffers alive here. */
static void ring_destroy(struct ring *r)
{
    if (!r) return;
    if (r->fd >= 0 && r->inflight && r->cq_head)
        (void)ring_reap(r, r->inflight, 0, 0);
    if (r->sqes_map) munmap(r->sqes_map, r->sqes_len);
    if (r->cq_ring_map && r->cq_ring_map != r->sq_ring_map)
        munmap(r->cq_ring_map, r->cq_ring_len);
    if (r->sq_ring_map) munmap(r->sq_ring_map, r->sq_ring_len);
    if (r->fd >= 0) close(r->fd);
    memset(r, 0, sizeof *r);
    r->fd = r->dev_fd = -1;
}

/* Queue the i-th command of the batch that is about to be submitted. The
 * submission tail advances monotonically across batches, so the slot a command
 * lands in is not its index within the batch: staging always into slot 0 would
 * make every batch after the first submit a stale entry. */
static void stage(struct ring *r, unsigned i, uint64_t lba, uint16_t nlb,
                  void *buf, size_t len, int fua)
{
    unsigned slot = (__atomic_load_n(r->sq_tail, __ATOMIC_RELAXED) + i) & *r->sq_mask;
    struct sqe128 *s = &r->sqes[slot];
    struct nvme_uring_cmd_l *c = (struct nvme_uring_cmd_l *)s->cmd;
    memset(s, 0, sizeof *s);
    s->opcode = (uint8_t)OP_URING_CMD;
    s->fd = r->dev_fd;
    s->cmd_op = (uint32_t)NVME_URING_CMD_IO;
    s->user_data = i;
    memset(c, 0, sizeof *c);
    c->opcode = 0x01;                    /* NVM Write */
    c->nsid = r->nsid;
    c->addr = (uint64_t)(uintptr_t)buf;
    c->data_len = (uint32_t)len;
    c->cdw10 = (uint32_t)(lba & 0xFFFFFFFFu);
    c->cdw11 = (uint32_t)(lba >> 32);
    c->cdw12 = (uint32_t)nlb | (fua ? (1u << 30) : 0u);
}

/* Same slot arithmetic as stage(), then mark the command as using a
 * registered buffer: uring_cmd_flags bit 0 plus the iovec index.  The data
 * pointer stays the real user address inside that iovec; the kernel resolves
 * it against the pages pinned at registration instead of pinning per command. */
static void stage_fixed(struct ring *r, unsigned i, uint64_t lba, uint16_t nlb,
                        void *buf, size_t len, int fua, uint16_t buf_index)
{
    unsigned slot = (__atomic_load_n(r->sq_tail, __ATOMIC_RELAXED) + i) & *r->sq_mask;
    stage(r, i, lba, nlb, buf, len, fua);
    r->sqes[slot].rw_flags |= QDSPLIT_URING_CMD_FIXED;
    r->sqes[slot].buf_index = buf_index;
}

/* Register the fixed-buffer iovec table once, before the timed loop.  The
 * kernel pins these pages here; per-command submissions with the FIXED flag
 * then skip get_user_pages entirely. */
static int ring_register_buffers(struct ring *r, const struct iovec *iov,
                                 unsigned nr)
{
    return syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_BUFFERS,
                   iov, nr) == 0 ? 0 : -1;
}

/* Count physically contiguous runs behind a span, or -1 when pagemap is
 * unreadable (missing privilege reads PFN 0).  Same decoding as the main
 * buffer-contract check; kept separate so that check's printed output stays
 * byte-identical. */
static int span_physical_runs(const void *p, size_t bytes)
{
    int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    int runs = 0;
    uint64_t prev = 0;
    size_t npg = (bytes + 4095u) / 4096u;

    if (pm < 0) return -1;
    for (size_t k = 0; k < npg; k++) {
        uint64_t e = 0, pfn;
        if (pread(pm, &e, 8,
                  (off_t)(((uintptr_t)p + k * 4096) / 4096) * 8) != 8) {
            runs = -1;
            break;
        }
        pfn = e & ((1ULL << 55) - 1);
        if (!(e & (UINT64_C(1) << 63)) || (e & (UINT64_C(1) << 62)) ||
            pfn == 0) {
            runs = -1;
            break;
        }
        if (k == 0 || pfn != prev + 1) runs++;
        prev = pfn;
    }
    close(pm);
    return runs;
}

/* Submit n staged commands and wait for all n. Returns 0 if every one of them
 * completed with status 0; a rejected command that is counted as a success
 * would report the latency of a command the device never performed. */
static int submit_wait_n(struct ring *r, unsigned n, int expect)
{
    unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);
    unsigned head_before = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    unsigned accepted, consumed;
    int bad;
    long rc;

    __atomic_store_n(r->sq_tail, tail + n, __ATOMIC_RELEASE);
    rc = syscall(__NR_io_uring_enter, r->fd, n, n,
                 IORING_ENTER_GETEVENTS, NULL, 0);
    consumed = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE) - head_before;
    bad = rc < 0 || (unsigned long)rc != n || consumed > n;
    if (consumed > n) consumed = n;
    accepted = rc > 0 && (unsigned long)rc <= n ? (unsigned)rc : 0;
    if (consumed > accepted) accepted = consumed;
    if (accepted > UINT_MAX - r->inflight) return -1;
    r->inflight += accepted;
    /* Never submit the unpublished remainder after a short enter.  Only drain
     * the prefix already consumed, then abort the run. */
    if (accepted && ring_reap(r, accepted, expect, 1) != 0) bad = 1;
    return bad ? -1 : 0;
}

/* Where does this file actually live on the device? Without this the
 * filesystem arm writes p1 and the passthru arms write p2, and any difference
 * between them could be a difference between two regions of the drive rather
 * than between the two paths. Returns the device LBA backing file offset 0 and
 * the number of contiguous bytes from there. */
/* The whole extent map, not just the first extent. ext4 handed out a 13 MB
 * first extent for a 33 MB file, and a run that assumed one contiguous extent
 * aborted with no results; the size of the first extent is not something the
 * measurement gets to choose. */
#define MAX_EXT 64
static struct exitos_bench_extent g_ext[MAX_EXT];
static int g_next;
static uint64_t g_mapped;

static int file_extents(int fd, uint64_t part_start_lba, uint32_t lbs,
                        uint64_t required_bytes)
{
    struct { struct fiemap f; struct fiemap_extent e[MAX_EXT]; } q;
    unsigned i;
    memset(&q, 0, sizeof q);
    q.f.fm_start = 0;
    q.f.fm_length = required_bytes;
    q.f.fm_flags = FIEMAP_FLAG_SYNC;
    q.f.fm_extent_count = MAX_EXT;
    if (ioctl(fd, FS_IOC_FIEMAP, &q.f) != 0) { perror("FIEMAP"); return -1; }
    if (q.f.fm_mapped_extents == 0) { fprintf(stderr, "file has no extents\n"); return -1; }
    if (q.f.fm_mapped_extents > MAX_EXT) {
        fprintf(stderr, "FIEMAP returned %u extents into a %u-entry request\n",
                q.f.fm_mapped_extents, MAX_EXT);
        return -1;
    }
    g_next = 0; g_mapped = 0;
    for (i = 0; i < q.f.fm_mapped_extents; i++) {
        if (!exitos_extent_flags_have_stable_location(q.e[i].fe_flags)) {
            fprintf(stderr, "extent %u has unsafe FIEMAP flags 0x%x\n",
                    i, q.e[i].fe_flags);
            return -1;
        }
        if (q.e[i].fe_flags & FIEMAP_EXTENT_UNWRITTEN) {
            fprintf(stderr, "extent %u remains unwritten after initialization\n", i);
            return -1;
        }
        if (q.e[i].fe_logical != g_mapped) {
            fprintf(stderr, "hole/non-contiguous logical map before extent %u\n", i);
            return -1;
        }
        if (q.e[i].fe_length > UINT64_MAX - g_mapped ||
            q.e[i].fe_logical % lbs || q.e[i].fe_physical == 0 ||
            q.e[i].fe_physical % lbs ||
            q.e[i].fe_length == 0 || q.e[i].fe_length % lbs ||
            q.e[i].fe_physical / lbs > UINT64_MAX - part_start_lba) {
            fprintf(stderr, "extent %u is not representable in native LBAs\n", i);
            return -1;
        }
        g_ext[g_next].logical = q.e[i].fe_logical;
        g_ext[g_next].lba = part_start_lba + q.e[i].fe_physical / lbs;
        g_ext[g_next].len = q.e[i].fe_length;
        g_mapped += q.e[i].fe_length;
        g_next++;
    }
    if (!g_next || g_mapped < required_bytes) {
        fprintf(stderr, "synchronized FIEMAP covers %llu of %llu required bytes\n",
                (unsigned long long)g_mapped,
                (unsigned long long)required_bytes);
        return -1;
    }
    return 0;
}

static int map_primary_plan(struct qdsplit_plan_io *plan, size_t nplan,
                            size_t plan_stride,
                            const struct exitos_bench_extent *ext, size_t next,
                            int blkfd, int partfd, uint64_t window_start,
                            uint64_t window_count, uint32_t lbs,
                            uint64_t *effective_start,
                            uint64_t *effective_count,
                            char *why, size_t why_len)
{
    uint64_t window_end = 0, union_start = UINT64_MAX, union_end = 0;
    int derive_window;

    if (!plan || !ext || !next || !lbs || !effective_start ||
        (plan_stride != N_LEGACY_ARM && plan_stride != N_ARM) ||
        !effective_count || ((window_start == 0) != (window_count == 0)))
        return -EINVAL;
    derive_window = window_start == 0;
    if (!derive_window) {
        if (window_count > UINT64_MAX - window_start) return -EOVERFLOW;
        window_end = window_start + window_count;
    }
    for (size_t i = 0; i < nplan; i++) {
        struct qdsplit_plan_io *io = &plan[i];
        uint64_t blocks, end;
        int rc;
        if (!io->enabled || i % plan_stride == 12) continue;
        rc = exitos_bench_extent_lba(ext, next, io->raw_off, io->len,
                                     lbs, &io->lba);
        if (rc != 0) return rc;
        blocks = (uint64_t)io->len / lbs;
        if (!blocks || blocks > UINT64_MAX - io->lba)
            return -ERANGE;
        end = io->lba + blocks;
        /* The user-supplied destructive window constrains raw writes.  A
         * filesystem arm may map elsewhere on the same retained partition,
         * but it still receives the identical per-range partition proof and
         * pre-submit whole-device zero gate. */
        if (io->raw && !derive_window &&
            (io->lba < window_start || end > window_end))
            return -ERANGE;
        rc = exitos_devguard_window_in_partition_fds(
            blkfd, partfd, io->lba, blocks, lbs, why, (unsigned)why_len);
        if (rc != 0) return rc;
        {
            struct exitos_bench_io_position pos;
            rc = exitos_bench_io_position(io->lba, blocks, 0, 1, 0,
                                           io->len, lbs, &pos);
            if (rc != 0 || pos.lba != io->lba || pos.blocks != blocks)
                return rc != 0 ? rc : -ERANGE;
            io->block_off = pos.byte_offset;
        }
        if (io->lba < union_start) union_start = io->lba;
        if (end > union_end) union_end = end;
    }
    if (union_start == UINT64_MAX || union_end <= union_start)
        return -ENOENT;
    if (derive_window) {
        uint64_t union_count = union_end - union_start;
        int rc = exitos_devguard_window_in_partition_fds(
            blkfd, partfd, union_start, union_count, lbs,
            why, (unsigned)why_len);
        if (rc != 0) return rc;
        *effective_start = union_start;
        *effective_count = union_count;
    } else {
        *effective_start = window_start;
        *effective_count = window_count;
    }
    return 0;
}

static const struct exitos_bench_extent *extent_for_logical(uint64_t logical)
{
    int i;

    for (i = 0; i < g_next; i++) {
        uint64_t end;
        if (g_ext[i].len > UINT64_MAX - g_ext[i].logical) return NULL;
        end = g_ext[i].logical + g_ext[i].len;
        if (logical >= g_ext[i].logical && logical < end) return &g_ext[i];
    }
    return NULL;
}

/* Verify one logical file range through both retained direct-I/O views.  A
 * range may cross FIEMAP extents, so every raw read is independently derived
 * from (and bounded by) the synchronized map and revalidated against the still
 * open partition fd. */
static int verify_readback_range(int ffd, int blkfd, int partfd, uint32_t lbs,
                                 uint64_t logical, size_t len,
                                 int expect_zero, unsigned iteration,
                                 unsigned arm, void *file_buf, void *raw_buf,
                                 size_t buffer_len, char *why,
                                 size_t why_len)
{
    uint64_t done = 0;

    if (!lbs || !len || !file_buf || !raw_buf || !buffer_len ||
        logical % lbs || len % lbs || buffer_len % lbs)
        return -EINVAL;
    while (done < len) {
        const struct exitos_bench_extent *ext;
        struct exitos_bench_io_position pos;
        uint64_t at = logical + done, ext_end, extent_left, blocks, lba;
        size_t chunk = len - (size_t)done;
        ssize_t file_got, raw_got;
        unsigned char *f = file_buf, *r = raw_buf;
        int guard_rc;

        if (at < logical) return -EOVERFLOW;
        ext = extent_for_logical(at);
        if (!ext || ext->len > UINT64_MAX - ext->logical)
            return -ERANGE;
        ext_end = ext->logical + ext->len;
        extent_left = ext_end - at;
        if (chunk > buffer_len) chunk = buffer_len;
        if ((uint64_t)chunk > extent_left) chunk = (size_t)extent_left;
        if (!chunk || chunk % lbs || (at - ext->logical) % lbs)
            return -ERANGE;
        blocks = (uint64_t)chunk / lbs;
        if (ext->lba > UINT64_MAX - (at - ext->logical) / lbs)
            return -EOVERFLOW;
        lba = ext->lba + (at - ext->logical) / lbs;
        guard_rc = exitos_devguard_window_in_partition_fds(
            blkfd, partfd, lba, blocks, lbs, why, (unsigned)why_len);
        if (guard_rc != 0) {
            fprintf(stderr,
                    "post-run raw read outside retained partition at logical %llu: %s\n",
                    (unsigned long long)at, why ? why : "");
            return guard_rc;
        }
        guard_rc = exitos_bench_io_position(lba, blocks, 0, 1, 0,
                                             chunk, lbs, &pos);
        if (guard_rc != 0 || pos.lba != lba || pos.blocks != blocks)
            return guard_rc != 0 ? guard_rc : -ERANGE;
        if ((uint64_t)(off_t)at != at) return -EOVERFLOW;
        file_got = pread(ffd, file_buf, chunk, (off_t)at);
        if (file_got != (ssize_t)chunk) {
            fprintf(stderr,
                    "post-run file read failed/short at logical %llu: got %lld expected %zu\n",
                    (unsigned long long)at, (long long)file_got, chunk);
            return -EIO;
        }
        raw_got = pread(blkfd, raw_buf, chunk, pos.byte_offset);
        if (raw_got != (ssize_t)chunk) {
            fprintf(stderr,
                    "post-run raw read failed/short at logical %llu: got %lld expected %zu\n",
                    (unsigned long long)at, (long long)raw_got, chunk);
            return -EIO;
        }
        for (size_t j = 0; j < chunk; j++) {
            uint64_t within = done + j;
            unsigned char expected = expect_zero
                                   ? 0
                                   : sample_pattern_byte(iteration, arm,
                                                         within);
            if (f[j] != expected || r[j] != expected || f[j] != r[j]) {
                fprintf(stderr,
                        "post-run mismatch logical=%llu expected=0x%02x file=0x%02x raw=0x%02x\n",
                        (unsigned long long)(at + j), expected, f[j], r[j]);
                return -EIO;
            }
        }
        done += chunk;
    }
    return 0;
}

static int verify_post_run(int ffd, int blkfd, int partfd, uint32_t lbs,
                           const struct qdsplit_plan_io *plan,
                           size_t nplan, size_t plan_stride,
                           uint64_t primary_bytes,
                           uint64_t owned_bytes, char *why, size_t why_len)
{
    void *file_buf = NULL, *raw_buf = NULL;
    size_t selected = 0;
    int rc = -1;

    if (!plan || !primary_bytes ||
        (plan_stride != N_LEGACY_ARM && plan_stride != N_ARM) ||
        owned_bytes != primary_bytes + 2u * (uint64_t)OWNED_GUARD_BYTES ||
        R.inflight || RP.inflight) {
        fprintf(stderr, "post-run oracle refused: invalid plan or in-flight I/O remains\n");
        return -EINVAL;
    }
    if (posix_memalign(&file_buf, 4096, READBACK_CHUNK_BYTES) != 0 ||
        posix_memalign(&raw_buf, 4096, READBACK_CHUNK_BYTES) != 0) {
        fprintf(stderr, "post-run oracle cannot allocate direct-I/O buffers\n");
        goto out;
    }
    if (verify_readback_range(ffd, blkfd, partfd, lbs, 0,
                              OWNED_GUARD_BYTES, 1, 0, 0,
                              file_buf, raw_buf, READBACK_CHUNK_BYTES,
                              why, why_len) != 0)
        goto out;
    for (size_t i = 0; i < nplan; i++) {
        const struct qdsplit_plan_io *io = &plan[i];
        unsigned arm = (unsigned)(i % plan_stride);
        unsigned iteration = (unsigned)(i / plan_stride);

        if (!io->enabled || arm == 12) continue;
        if (io->foff < (off_t)OWNED_GUARD_BYTES ||
            (uint64_t)io->foff != io->raw_off ||
            io->raw_off > OWNED_GUARD_BYTES + primary_bytes ||
            io->len > OWNED_GUARD_BYTES + primary_bytes - io->raw_off) {
            fprintf(stderr, "post-run oracle found a plan entry in a guard\n");
            goto out;
        }
        if (verify_readback_range(ffd, blkfd, partfd, lbs, io->raw_off,
                                  io->len, 0, iteration, arm,
                                  file_buf, raw_buf, READBACK_CHUNK_BYTES,
                                  why, why_len) != 0)
            goto out;
        selected++;
    }
    if (verify_readback_range(ffd, blkfd, partfd, lbs,
                              OWNED_GUARD_BYTES + primary_bytes,
                              OWNED_GUARD_BYTES, 1, 0, 0,
                              file_buf, raw_buf, READBACK_CHUNK_BYTES,
                              why, why_len) != 0)
        goto out;
    printf("POST_RUN_VERIFY file=exact raw=exact selected=%zu guards=zero\n",
           selected);
    rc = 0;
out:
    free(raw_buf);
    free(file_buf);
    return rc;
}

/* A plain io_uring write to the file. Submitted through the same ring as the
 * passthru commands, so the arm that uses it differs from the passthru arms in
 * what the command is and not in how it is submitted. */
#define OP_WRITE 23u
static void stage_file_write(struct ring *r, unsigned i, int fd, void *buf,
                             size_t len, off_t off)
{
    unsigned slot = (__atomic_load_n(r->sq_tail, __ATOMIC_RELAXED) + i) & *r->sq_mask;
    struct sqe128 *s = &r->sqes[slot];
    memset(s, 0, sizeof *s);
    s->opcode = (uint8_t)OP_WRITE;
    s->fd = fd;
    s->cmd_op = (uint32_t)(off & 0xFFFFFFFFu);     /* union with off, low half  */
    s->pad1   = (uint32_t)((uint64_t)off >> 32);   /* union with off, high half */
    s->addr = (uint64_t)(uintptr_t)buf;
    s->len = (uint32_t)len;
    s->user_data = i;
}

/* The same passthru write, submitted with the older ioctl instead of io_uring. */
static int ioctl_write(int blk_fd, uint32_t nsid, uint64_t lba, uint16_t nlb,
                       void *buf, size_t len, int fua)
{
    struct nvme_passthru_cmd c;
    memset(&c, 0, sizeof c);
    c.opcode = 0x01;
    c.nsid = nsid;
    c.addr = (uint64_t)(uintptr_t)buf;
    c.data_len = (uint32_t)len;
    c.cdw10 = (uint32_t)(lba & 0xFFFFFFFFu);
    c.cdw11 = (uint32_t)(lba >> 32);
    c.cdw12 = (uint32_t)nlb | (fua ? (1u << 30) : 0u);
    errno = 0;
    {
        int raw = ioctl(blk_fd, NVME_IOCTL_IO_CMD, &c);
        return exitos_bench_nvme_status(raw, errno);
    }
}

/* Passthru commands complete with 0; a file write completes with the byte
 * count. Accepting any non-negative result would count a rejected command as a
 * successful one and report the latency of a write the device never did. */
static int submit_wait(struct ring *r, unsigned n) { return submit_wait_n(r, n, 0); }

/* Which idle states did the measuring core enter, and how many times per write?
 * These counters are only a host observation: qdsplit does not prove completion
 * IRQ affinity.  If the campaign independently verifies that the active queue's
 * completion interrupt targets this CPU, its idle-state exit latency can lie on
 * the critical path and should be considered when interpreting small deltas. */
#define IDLE_STATES 8
struct idlesnap { unsigned long usage[IDLE_STATES], time[IDLE_STATES];
                  char name[IDLE_STATES][16]; int n; };

static void idle_snapshot(int cpu, struct idlesnap *o)
{
    char path[192]; FILE *f; int i;
    memset(o, 0, sizeof *o);
    for (i = 0; i < IDLE_STATES; i++) {
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/name", cpu, i);
        f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%15s", o->name[i]) != 1) o->name[i][0] = 0;
        fclose(f);
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/usage", cpu, i);
        f = fopen(path, "r");
        if (f) { if (fscanf(f, "%lu", &o->usage[i]) != 1) o->usage[i] = 0; fclose(f); }
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/time", cpu, i);
        f = fopen(path, "r");
        if (f) { if (fscanf(f, "%lu", &o->time[i]) != 1) o->time[i] = 0; fclose(f); }
        o->n = i + 1;
    }
}

static void idle_report(int cpu, const struct idlesnap *a, const struct idlesnap *b,
                        int writes)
{
    int i;
    printf("idle states on cpu %d, per write over %d writes:\n", cpu, writes);
    for (i = 0; i < b->n; i++) {
        unsigned long du = b->usage[i] - a->usage[i];
        unsigned long dt = b->time[i] - a->time[i];
        if (du == 0 && dt == 0) continue;
        printf("  %-6s entered %7.3f times/write, %8.3f us/write\n",
               b->name[i], (double)du / writes, (double)dt / writes);
    }
}

static int cmp(const void *a, const void *b)
{ double x=*(const double*)a, y=*(const double*)b; return x<y?-1:x>y?1:0; }

static double now_us(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec*1e6 + (double)t.tv_nsec/1e3; }
int main(int argc, char **argv)
{
    const char *dev = getenv("EXITOS_DEV");
    const char *chr = getenv("EXITOS_CHARDEV");
    const char *fsp = getenv("EXITOS_FSFILE");
    const char *part = getenv("EXITOS_WINDOW_IN_PART");
    const char *fspart = getenv("EXITOS_FSPART");
    const char *sn = getenv("EXITOS_EXPECT_SERIAL");
    const char *s0 = getenv("EXITOS_LBA_START");
    const char *sc = getenv("EXITOS_LBA_COUNT");
    const char *paired = getenv("EXITOS_PAIRED");
    const char *only_text = getenv("EXITOS_ONLY");
    const char *order_offset_text = getenv("EXITOS_ORDER_OFFSET");
    const char *samefile = getenv("EXITOS_SAMEFILE");
    const char *big_text = getenv("EXITOS_BIGSZ");
    const char *contig_text = getenv("EXITOS_REQUIRE_CONTIG");
    const char *scatter_text = getenv("EXITOS_REQUIRE_SCATTERED");
    struct qdsplit_plan_io *plan = NULL;
    double *samples[N_ARM] = {0};
    int nsample[N_ARM] = {0};
    unsigned enabled[N_ARM];
    int pset[6], pn = 0, only = -1;
    uint64_t parsed = 0, lba0 = 0, cnt = 0, primary_bytes = 0;
    uint64_t owned_bytes = 0;
    size_t plan_count = 0, plan_stride = N_LEGACY_ARM, big = 8192;
    size_t buffer_contract_bytes = 0;
    unsigned iters = 2000, nsplit, b8, b4;
    unsigned order_offset = 0;
    int blkfd = -1, chrfd = -1, partfd = -1, part_rwfd = -1, dirfd = -1;
    int ffd = -1, afd = -1;
    int need_ring = 0, blockpoll_mode = 0;
    int derive_window = 0;
    int require_contig = 0, require_scattered = 0;
    void *buf = NULL, *huge_region = NULL;
    void *stage_region = NULL, *stage_slot = NULL;
    char why[320] = {0};
    struct stat bst, cst, pst, prwst, fst;
    struct idlesnap idle_before, idle_after;
    struct sigaction old_int, old_term;
    sigset_t old_signal_mask;
    int meas_cpu = -1, run_failed = 0, stop_seen = 0, rc = 2;
    int handlers_installed = 0;
    uint32_t lbs = 0, nsid = 0;
    struct iopath *prod_pt_io = NULL, *prod_block_io = NULL;

    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&R, 0, sizeof R);
    memset(&RP, 0, sizeof RP);
    R.fd = RP.fd = -1;
    R.dev_fd = RP.dev_fd = -1;
    have_poll = 0;
    stop_signal = 0;
    {
        const char *ld_preload = getenv("LD_PRELOAD");
        const char *ld_audit = getenv("LD_AUDIT");
        if ((ld_preload && *ld_preload) || (ld_audit && *ld_audit)) {
            fprintf(stderr, "refusing: qdsplit measurements require an un-interposed process\n");
            rc = 3;
            goto out;
        }
    }
    if (install_stop_handlers(&old_int, &old_term, &old_signal_mask) != 0) {
        perror("install SIGINT/SIGTERM handlers");
        rc = 3;
        goto out;
    }
    handlers_installed = 1;
    if (stop_signal) {
        rc = stop_exit_status();
        goto out;
    }

    if (argc > 2 || (argc == 2 &&
        (exitos_bench_parse_u64(argv[1], &parsed) != 0 || parsed == 0 ||
         parsed > INT_MAX))) {
        fprintf(stderr, "iterations must be a positive integer <= INT_MAX\n");
        goto out;
    }
    if (argc == 2) iters = (unsigned)parsed;
    if (big_text) {
        if (exitos_bench_parse_u64(big_text, &parsed) != 0 ||
            parsed < 4096 || parsed > 32768 || parsed % 4096) {
            fprintf(stderr, "EXITOS_BIGSZ must be 4096..32768 and a multiple of 4096\n");
            goto out;
        }
        big = (size_t)parsed;
    }
    if (select_arms(only_text, paired, &only, pset, &pn,
                    enabled) != 0) {
        fprintf(stderr, "invalid/ambiguous EXITOS_ONLY or EXITOS_PAIRED selection\n");
        goto out;
    }
    blockpoll_mode = paired && strcmp(paired, "blockpoll") == 0;
    if (order_offset_text) {
        if (!blockpoll_mode) {
            fprintf(stderr,
                    "EXITOS_ORDER_OFFSET is valid only with EXITOS_PAIRED=blockpoll\n");
            rc = 3;
            goto out;
        }
        if (strcmp(order_offset_text, "0") == 0)
            order_offset = 0;
        else if (strcmp(order_offset_text, "1") == 0)
            order_offset = 1;
        else if (strcmp(order_offset_text, "2") == 0)
            order_offset = 2;
        else {
            fprintf(stderr,
                    "EXITOS_ORDER_OFFSET must be exactly 0, 1, or 2\n");
            rc = 3;
            goto out;
        }
    }
    if (blockpoll_mode && big != 8192u) {
        fprintf(stderr,
                "EXITOS_PAIRED=blockpoll requires exact 8192-byte I/O\n");
        rc = 3;
        goto out;
    }
    plan_stride = blockpoll_mode ? N_ARM : N_LEGACY_ARM;
    if (only == 12 || (paired && enabled[12])) {
        printf("ARM12_FROZEN append inode has no post-run correctness oracle\n");
        rc = 3;
        goto out;
    }
    if (!only_text && !paired) {
        enabled[12] = 0;
        printf("DEFAULT_ARMS selected=17 append=excluded\n");
    }
    if (!dev || !chr || !fsp || !part || !fspart || !sn || !*sn || !s0 || !sc) {
        fprintf(stderr, "need EXITOS_DEV CHARDEV FSFILE WINDOW_IN_PART FSPART "
                        "EXPECT_SERIAL LBA_START LBA_COUNT\n");
        goto out;
    }
    if (!samefile || strcmp(samefile, "1") != 0) {
        fprintf(stderr, "refusing: qdsplit raw-window modes are safety-frozen; "
                        "set EXITOS_SAMEFILE=1 for an owned-file run\n");
        rc = 3;
        goto out;
    }
    if ((contig_text && strcmp(contig_text, "1") != 0) ||
        (scatter_text && strcmp(scatter_text, "1") != 0) ||
        (contig_text && scatter_text)) {
        fprintf(stderr, "refusing: buffer contract must be exactly one of "
                        "EXITOS_REQUIRE_CONTIG=1 or EXITOS_REQUIRE_SCATTERED=1\n");
        rc = 3;
        goto out;
    }
    require_contig = contig_text != NULL;
    require_scattered = scatter_text != NULL;
    if (strcmp(part, fspart) != 0) {
        fprintf(stderr, "refusing: WINDOW_IN_PART and FSPART must name one retained partition path\n");
        rc = 3;
        goto out;
    }
    if (getenv("EXITOS_SWAP45")) {
        fprintf(stderr, "refusing: legacy slice swapping is not part of the immutable plan\n");
        rc = 3;
        goto out;
    }
    if (qdsplit_checked_plan_count((size_t)iters, plan_stride,
                                   &plan_count) != 0 ||
        plan_count > SIZE_MAX / sizeof *plan) {
        fprintf(stderr, "plan is too large\n");
        rc = 3;
        goto out;
    }
    plan = calloc(plan_count, sizeof *plan);
    if (!plan || build_logical_plan(iters, big, paired, pset, pn, enabled,
                                    plan, plan_stride, &primary_bytes) != 0 ||
        add_owned_guards(plan, plan_count, plan_stride,
                         primary_bytes, &owned_bytes) != 0 ||
        (uint64_t)(off_t)owned_bytes != owned_bytes ||
        (off_t)owned_bytes <= 0) {
        fprintf(stderr, "cannot build a complete representable logical plan\n");
        rc = 3;
        goto out;
    }
    for (int a = 0; a < N_ARM; a++) {
        if (!enabled[a]) continue;
        samples[a] = malloc(sizeof **samples * (size_t)iters);
        if (!samples[a]) {
            fprintf(stderr, "cannot allocate all selected sample arrays\n");
            rc = 5;
            goto out;
        }
        if ((a >= 1 && a <= 8) || (a >= 15 && a <= 17)) need_ring = 1;
        if (arm_command_span(a, big) > buffer_contract_bytes)
            buffer_contract_bytes = arm_command_span(a, big);
    }

    /* Retain every object, and preflight completion mechanisms before the
     * first filesystem initialization write. */
    blkfd = open(dev, O_RDWR | O_DIRECT | O_CLOEXEC | O_NOFOLLOW);
    chrfd = open(chr, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    partfd = open(fspart, O_RDONLY | O_DIRECT | O_CLOEXEC | O_NOFOLLOW);
    if (enabled[19])
        part_rwfd = open(fspart, O_RDWR | O_DIRECT | O_CLOEXEC | O_NOFOLLOW);
    if (blkfd < 0 || chrfd < 0 || partfd < 0 ||
        (enabled[19] && part_rwfd < 0)) {
        perror("open retained qdsplit device");
        rc = 3;
        goto out;
    }
    devguard_verdict verdict = exitos_devguard_check_fd(blkfd, sn, NULL, 0);
    if (fstat(blkfd, &bst) != 0 || !S_ISBLK(bst.st_mode) ||
        fstat(chrfd, &cst) != 0 || !S_ISCHR(cst.st_mode) ||
        fstat(partfd, &pst) != 0 || !S_ISBLK(pst.st_mode) ||
        (enabled[19] &&
         (fstat(part_rwfd, &prwst) != 0 || !S_ISBLK(prwst.st_mode) ||
          prwst.st_rdev != pst.st_rdev)) ||
        !exitos_devguard_passthru_ok_fd(blkfd) ||
        (verdict != DEVGUARD_OK && verdict != DEVGUARD_HAS_PARTITIONS &&
         verdict != DEVGUARD_MOUNTED)) {
        fprintf(stderr, "refusing: retained device identity/type check failed\n");
        rc = 3;
        goto out;
    }
    lbs = exitos_devguard_lbs_fd(blkfd);
    if (!lbs || lbs > 4096 || 4096u % lbs || big % lbs ||
        exitos_devguard_lbs_fd(partfd) != lbs ||
        (enabled[19] && exitos_devguard_lbs_fd(part_rwfd) != lbs) ||
        exitos_bench_parse_u64(s0, &lba0) != 0 ||
        exitos_bench_parse_u64(sc, &cnt) != 0 ||
        ((lba0 == 0) != (cnt == 0))) {
        fprintf(stderr, "refusing: retained window/LBS check failed: %s\n", why);
        rc = 3;
        goto out;
    }
    derive_window = lba0 == 0;
    if (!derive_window &&
        exitos_devguard_window_in_partition_fds(blkfd, partfd, lba0, cnt,
                                                lbs, why, sizeof why) != 0) {
        fprintf(stderr, "refusing: retained explicit window check failed: %s\n", why);
        rc = 3;
        goto out;
    }
    if (derive_window)
        printf("window: pending retained-file FIEMAP derivation\n");
    else
        printf("window checked: %s\n", why);
    printf("lbs=%u  %zu bytes = %zu blocks\n", lbs, big, big / lbs);
    {
        int id = ioctl(chrfd, NVME_IOCTL_ID);
        nsid = id > 0 ? (uint32_t)id : 0;
    }
    if (!nsid || !exitos_devguard_nvme_pair_at("/sys",
            major(bst.st_rdev), minor(bst.st_rdev),
            major(cst.st_rdev), minor(cst.st_rdev), nsid)) {
        fprintf(stderr, "refusing: retained block and character fds are not one namespace\n");
        rc = 3;
        goto out;
    }
    if (blockpoll_mode) {
        struct qdsplit_durability_binding before, after;
        int probe_rc, volatile_cache = -1, supports_fua = -1;
        exitos_durability policy = EXITOS_DUR_FLUSH;
        const char *policy_name = "UNPROBED";
        int binding_rc;

        binding_rc = qdsplit_durability_binding_capture(blkfd, lbs, &before);
        if (binding_rc != 0) {
            fprintf(stderr,
                    "refusing: cannot derive durability probe identity from retained block fd\n");
            rc = 4;
            goto out;
        }
        probe_rc = exitos_durability_probe(before.probe_path, &volatile_cache,
                                           &supports_fua);
        binding_rc = qdsplit_durability_binding_capture(blkfd, lbs, &after);
        if (binding_rc == 0 &&
            !qdsplit_durability_binding_same(&before, &after))
            binding_rc = -ESTALE;
        if (probe_rc == 0 && binding_rc == 0) {
            policy = exitos_durability_policy(volatile_cache, supports_fua);
            policy_name = policy == EXITOS_DUR_NONE ? "DUR_NONE" :
                          policy == EXITOS_DUR_FUA ? "DUR_FUA" :
                          policy == EXITOS_DUR_FLUSH ? "DUR_FLUSH" :
                          "UNKNOWN";
        }
        if (probe_rc != 0 || binding_rc != 0 || volatile_cache != 0 ||
            policy != EXITOS_DUR_NONE) {
            fprintf(stderr,
                    "refusing: blockpoll durability gate probe_rc=%d "
                    "binding_rc=%d volatile_write_cache=%d fua=%d policy=%s; requires "
                    "successful probe, write-through, and DUR_NONE\n",
                    probe_rc, binding_rc, volatile_cache, supports_fua,
                    policy_name);
            rc = 4;
            goto out;
        }
        printf("DURABILITY_PROOF probe=success volatile_write_cache=0 "
               "fua=%d policy=DUR_NONE\n", supports_fua);
    }
    if (need_ring && ring_open(&R, chrfd, nsid, 0) != 0) {
        rc = 4;
        goto out;
    }
    if (enabled[10]) {
        if (!poll_available_fd(blkfd)) {
            fprintf(stderr, "refusing: selected poll arm has no native poll queue\n");
            rc = 4;
            goto out;
        }
        if (ring_open(&RP, chrfd, nsid, IORING_SETUP_IOPOLL) != 0) {
            fprintf(stderr, "refusing: selected poll ring setup failed\n");
            rc = 4;
            goto out;
        }
        have_poll = 1;
    }
    if (enabled[18]) {
        prod_pt_io = iopath_open_fd(blkfd, IOPATH_URING_CMD_POLL, lbs);
        if (!prod_pt_io || iopath_set_exclusive(prod_pt_io, 1) != 0) {
            fprintf(stderr,
                    "refusing: production PT IOPOLL handle/exclusive setup failed\n");
            rc = 4;
            goto out;
        }
        have_poll = 1;
    }
    if (enabled[19]) {
        prod_block_io = iopath_open_fd(part_rwfd,
                                       IOPATH_URING_WRITE_POLL, lbs);
        if (!prod_block_io ||
            iopath_set_exclusive(prod_block_io, 1) != 0) {
            fprintf(stderr,
                    "refusing: production block IOPOLL handle/exclusive setup failed\n");
            rc = 4;
            goto out;
        }
        have_poll = 1;
    }
    printf("polled completion: %s\n", have_poll ? "ENABLED" : "not selected");

    /* Allocate all measurement memory before creating either storage object. */
    if (getenv("EXITOS_HUGEBUF")) {
        huge_region = mmap(NULL, 4u << 20, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (huge_region == MAP_FAILED) {
            huge_region = NULL;
            perror("mmap huge buffer");
            rc = 6;
            goto out;
        }
        buf = (void *)(((uintptr_t)huge_region + (2u << 20) - 1) &
                       ~(uintptr_t)((2u << 20) - 1));
        if (madvise(buf, 2u << 20, MADV_HUGEPAGE) != 0) perror("madvise");
        memset(buf, 0x5A, 2u << 20);
        if (madvise(buf, 2u << 20, MADV_COLLAPSE) != 0 && errno != EINVAL)
            perror("madvise(MADV_COLLAPSE)");
    } else {
        if (posix_memalign(&buf, 4096, 32768) != 0) {
            buf = NULL;
            rc = 6;
            goto out;
        }
        memset(buf, 0x5A, 32768);
    }
    {
        int buffer_runs = -1;
        int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (pm >= 0) {
            size_t npg = buffer_contract_bytes / 4096;
            uint64_t prev = 0;
            int runs = 0;
            printf("buffer pages:");
            for (size_t k = 0; k < npg; k++) {
                uint64_t e = 0, pfn;
                if (pread(pm, &e, 8,
                          (off_t)(((uintptr_t)buf + k * 4096) / 4096) * 8) != 8) {
                    runs = -1;
                    break;
                }
                pfn = e & ((1ULL << 55) - 1);
                if (!(e & (UINT64_C(1) << 63)) ||
                    (e & (UINT64_C(1) << 62)) || pfn == 0) {
                    runs = -1;
                    break;
                }
                if (k == 0 || pfn != prev + 1) runs++;
                if (k < 4) printf(" %llu", (unsigned long long)pfn);
                prev = pfn;
            }
            printf(" ... submitted %zu-byte command span has %d physical run%s\n",
                   buffer_contract_bytes, runs, runs == 1 ? "" : "s");
            buffer_runs = runs;
            close(pm);
        }
        if (require_contig && buffer_runs != 1) {
            fprintf(stderr, "contiguous-buffer cell refused: observed %d physical runs\n",
                    buffer_runs);
            rc = 6;
            goto out;
        }
        if (require_scattered && buffer_runs <= 1) {
            fprintf(stderr, "scattered-buffer cell refused: observed %d physical runs\n",
                    buffer_runs);
            rc = 6;
            goto out;
        }
        if (require_contig)
            printf("buffer contract: contiguous proven runs=1\n");
        if (require_scattered)
            printf("buffer contract: scattered proven runs=%d\n", buffer_runs);
    }

    /* Bounce arms need a physically contiguous staging slot.  Prove the slot
     * before the first timed sample: a pagemap run count of 1 is direct proof;
     * when PFNs are unreadable, a successful MADV_COLLAPSE return is the
     * kernel's own statement that the range is PMD-mapped.  Anything else
     * refuses the whole run rather than measuring a slot that would carry the
     * very external-SGL shape the arm exists to remove. */
    if (enabled[16] || enabled[17]) {
        int collapse_rc, slot_runs, accepted;
        stage_region = mmap(NULL, STAGE_REGION_BYTES, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stage_region == MAP_FAILED) {
            stage_region = NULL;
            perror("mmap staging region");
            rc = 6;
            goto out;
        }
        stage_slot = (void *)(((uintptr_t)stage_region + (2u << 20) - 1) &
                              ~(uintptr_t)((2u << 20) - 1));
        if (madvise(stage_slot, 2u << 20, MADV_HUGEPAGE) != 0)
            perror("madvise(stage MADV_HUGEPAGE)");
        memset(stage_slot, 0x5A, 2u << 20);
        collapse_rc = madvise(stage_slot, 2u << 20, MADV_COLLAPSE);
        slot_runs = span_physical_runs(stage_slot, big);
        accepted = slot_runs == 1 || (slot_runs < 0 && collapse_rc == 0);
        printf("stage slot: runs=%d collapse=%s -> %s\n", slot_runs,
               collapse_rc == 0 ? "ok" : "unavailable",
               accepted ? "accepted" : "refused");
        if (!accepted) {
            fprintf(stderr, "refusing: staging slot is not one physical run "
                            "(runs=%d, collapse=%d)\n", slot_runs, collapse_rc);
            rc = 6;
            goto out;
        }
    }
    if (enabled[15] || enabled[17]) {
        struct iovec reg_iov[2];
        unsigned reg_nr = enabled[17] ? 2u : 1u;
        reg_iov[0].iov_base = buf;
        reg_iov[0].iov_len = STAGE_SLOT_BYTES;
        reg_iov[1].iov_base = stage_slot;
        reg_iov[1].iov_len = STAGE_SLOT_BYTES;
        if (ring_register_buffers(&R, reg_iov, reg_nr) != 0) {
            perror("io_uring_register(IORING_REGISTER_BUFFERS)");
            rc = 4;
            goto out;
        }
        printf("registered fixed buffers: %u\n", reg_nr);
    }

    dirfd = open_file_parent(fsp);
    if (dirfd < 0) {
        perror("open EXITOS_FSFILE parent");
        rc = 5;
        goto out;
    }
    ffd = open_owned_tmpfile(dirfd);
    if (ffd < 0) {
        perror("O_TMPFILE primary benchmark inode");
        rc = 5;
        goto out;
    }
    if (fstat(ffd, &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_nlink != 0 ||
        fst.st_dev != pst.st_rdev) {
        fprintf(stderr, "refusing: primary inode is linked or not on retained EXITOS_FSPART\n");
        rc = 3;
        goto out;
    }
    if (enabled[12]) {
        struct stat ast;
        afd = open_owned_tmpfile(dirfd);
        if (afd < 0) {
            perror("O_TMPFILE append benchmark inode");
            rc = 5;
            goto out;
        }
        if (fstat(afd, &ast) != 0 || !S_ISREG(ast.st_mode) || ast.st_nlink != 0 ||
            ast.st_dev != pst.st_rdev) {
            fprintf(stderr, "refusing: append inode is linked or not on retained partition\n");
            rc = 3;
            goto out;
        }
    }

    /* fallocate alone leaves unwritten extents. Initialize exactly the full
     * planned range, reject every short write, synchronize, and only then ask
     * FIEMAP for stable locations. */
    if (fallocate(ffd, 0, 0, (off_t)owned_bytes) != 0) {
        perror("fallocate");
        rc = 5;
        goto out;
    }
    {
        const size_t chunk = 1u << 20;
        void *zero = NULL;
        uint64_t off = 0;
        if (posix_memalign(&zero, 4096, chunk) != 0) {
            rc = 5;
            goto out;
        }
        memset(zero, 0, chunk);
        while (off < owned_bytes) {
            size_t len = owned_bytes - off > chunk
                       ? chunk : (size_t)(owned_bytes - off);
            if (pwrite(ffd, zero, len, (off_t)off) != (ssize_t)len) {
                perror("initialize full qdsplit file");
                free(zero);
                rc = 5;
                goto out;
            }
            off += len;
        }
        free(zero);
    }
    if (fsync(ffd) != 0) {
        perror("fsync initialized benchmark file");
        rc = 5;
        goto out;
    }

    {
        uint64_t part_start_lba;
        if (partition_start_lba_fd(partfd, lbs, &part_start_lba) != 0 ||
            file_extents(ffd, part_start_lba, lbs, owned_bytes) != 0) {
            fprintf(stderr, "refusing: guarded synchronized file map is not safe\n");
            rc = 3;
            goto out;
        }
        if (map_primary_plan(plan, plan_count, plan_stride,
                             g_ext, (size_t)g_next,
                             blkfd, partfd, lba0, cnt, lbs,
                             &lba0, &cnt,
                             why, sizeof why) != 0) {
            fprintf(stderr, "refusing: complete synchronized primary plan is not safe\n");
            rc = 3;
            goto out;
        }
        if (derive_window)
            printf("derived retained-file window: start=%llu count=%llu native LBAs\n",
                   (unsigned long long)lba0, (unsigned long long)cnt);
        printf("immutable guarded primary map: %d stable extents, %zu entries preflighted\n",
               g_next, plan_count);
    }

    if (stop_signal) {
        rc = stop_exit_status();
        goto out;
    }
    if (getenv("EXITOS_PAUSE")) {
        uint64_t pause_seconds;
        if (exitos_bench_parse_u64(getenv("EXITOS_PAUSE"), &pause_seconds) != 0 ||
            pause_seconds > UINT_MAX) {
            fprintf(stderr, "EXITOS_PAUSE is invalid\n");
            rc = 2;
            goto out;
        }
        printf("READY\n");
        fflush(stdout);
        sleep((unsigned)pause_seconds);
        if (stop_signal) {
            rc = stop_exit_status();
            goto out;
        }
    }

    nsplit = (unsigned)(big / 4096);
    b8 = (unsigned)(big / lbs);
    b4 = 4096u / lbs;
    meas_cpu = sched_getcpu();
    if (blockpoll_mode)
        printf("timing contract: fs/block/ioctl arms include call/command construction; "
               "uring SQE staging is outside t0 except pt-4kx2-seq; "
               "bounce arms charge their staging memcpy inside t0; "
               "production arms time one iopath_write completion\n");
    else
        printf("timing contract: fs/block/ioctl arms include call/command construction; "
               "uring SQE staging is outside t0 except pt-4kx2-seq; "
               "bounce arms charge their staging memcpy inside t0\n");
    idle_snapshot(meas_cpu, &idle_before);
    for (unsigned i = 0; i < iters && !run_failed && !stop_seen; i++) {
        int timed_count = blockpoll_mode ? pn : N_LEGACY_ARM;
        for (int k = 0; k < timed_count; k++) {
            int a = blockpoll_mode
                  ? pset[(k + (int)(i % (unsigned)pn) +
                           (int)order_offset) % pn]
                  : (k + (int)(i % N_LEGACY_ARM)) % N_LEGACY_ARM;
            struct qdsplit_plan_io *io =
                &plan[(size_t)i * plan_stride + (size_t)a];
            double t0 = 0.0, t1 = 0.0;
            int ok = 0;
            sigset_t io_old_mask;
            if (!io->enabled) continue;
            if (stop_signal) {
                stop_seen = 1;
                break;
            }
            /* Refill the same already-proven physical pages before both the
             * safety gate and t0.  The whole-device zero read is then the last
             * storage operation before every FS/block/PT submission. */
            fill_sample_pattern(buf, io->len, i, (unsigned)a);
            {
                uint64_t bad = 0;
                int zero = exitos_devguard_window_is_zero_fd(
                    blkfd, io->lba, (uint64_t)io->len / lbs, &bad);
                if (zero != 0) {
                    fprintf(stderr,
                            "primary target for arm %s is not still zero (rc=%d, lba=%llu)\n",
                            arm_name[a], zero, (unsigned long long)bad);
                    run_failed = 1;
                    break;
                }
            }
            /* Closing an io_uring fd is not a synchronous cancellation barrier.
             * Atomically block cooperative-stop signals before submission, then
             * deliver any pending stop only after this sample has a known
             * completion.  The mask transitions stay outside t0/t1. */
            if (block_stop_signals(&io_old_mask) != 0) {
                perror("block SIGINT/SIGTERM around I/O");
                run_failed = 1;
                break;
            }
            if (stop_signal) {
                (void)sigprocmask(SIG_SETMASK, &io_old_mask, NULL);
                stop_seen = 1;
                break;
            }
            switch (a) {
            case 0:
            case 13:
                t0 = now_us();
                ok = pwrite(ffd, buf, big, io->foff) == (ssize_t)big;
                t1 = now_us();
                break;
            case 14:
                t0 = now_us();
                ok = pwrite(blkfd, buf, io->len, io->block_off) ==
                     (ssize_t)io->len;
                t1 = now_us();
                break;
            case 1:
            case 2:
                stage(&R, 0, io->lba, (uint16_t)(b8 - 1), buf, big, a == 2);
                t0 = now_us();
                ok = submit_wait(&R, 1) == 0;
                t1 = now_us();
                break;
            case 3:
                t0 = now_us();
                ok = 1;
                for (unsigned z = 0; z < nsplit && ok; z++) {
                    stage(&R, 0, io->lba + (uint64_t)z * b4,
                          (uint16_t)(b4 - 1), (char *)buf + (size_t)z * 4096,
                          4096, 0);
                    ok = submit_wait(&R, 1) == 0;
                }
                t1 = now_us();
                break;
            case 4:
            case 5:
                for (unsigned z = 0; z < nsplit; z++)
                    stage(&R, z, io->lba + (uint64_t)z * b4,
                          (uint16_t)(b4 - 1), (char *)buf + (size_t)z * 4096,
                          4096, a == 5);
                t0 = now_us();
                ok = submit_wait(&R, nsplit) == 0;
                t1 = now_us();
                break;
            case 6:
                stage(&R, 0, io->lba, (uint16_t)(b4 - 1), buf, 4096, 0);
                t0 = now_us();
                ok = submit_wait(&R, 1) == 0;
                t1 = now_us();
                break;
            case 7:
                t0 = now_us();
                ok = pwrite(ffd, buf, 4096, io->foff) == 4096;
                t1 = now_us();
                break;
            case 8:
                stage_file_write(&R, 0, ffd, buf, big, io->foff);
                t0 = now_us();
                ok = submit_wait_n(&R, 1, (int)big) == 0;
                t1 = now_us();
                break;
            case 9:
                t0 = now_us();
                ok = ioctl_write(blkfd, nsid, io->lba, (uint16_t)(b8 - 1),
                                 buf, big, 0) == 0;
                t1 = now_us();
                break;
            case 10:
                stage(&RP, 0, io->lba, (uint16_t)(b8 - 1), buf, big, 1);
                t0 = now_us();
                ok = submit_wait(&RP, 1) == 0;
                t1 = now_us();
                break;
            case 11:
                t0 = now_us();
                ok = pwrite(ffd, buf, big, io->foff) == (ssize_t)big &&
                     fdatasync(ffd) == 0;
                t1 = now_us();
                break;
            case 12:
                t0 = now_us();
                ok = afd >= 0 &&
                     pwrite(afd, buf, big, io->foff) == (ssize_t)big &&
                     fdatasync(afd) == 0;
                t1 = now_us();
                break;
            case 15:
                stage_fixed(&R, 0, io->lba, (uint16_t)(b8 - 1), buf, big, 0, 0);
                t0 = now_us();
                ok = submit_wait(&R, 1) == 0;
                t1 = now_us();
                break;
            case 16:
            case 17:
                if (a == 16)
                    stage(&R, 0, io->lba, (uint16_t)(b8 - 1), stage_slot,
                          big, 0);
                else
                    stage_fixed(&R, 0, io->lba, (uint16_t)(b8 - 1), stage_slot,
                                big, 0, 1);
                t0 = now_us();
                memcpy(stage_slot, buf, big);
                ok = submit_wait(&R, 1) == 0;
                t1 = now_us();
                break;
            case 18:
                t0 = now_us();
                ok = iopath_write(prod_pt_io, io->lba, buf, io->len) == 0;
                t1 = now_us();
                break;
            case 19:
                t0 = now_us();
                ok = iopath_write(prod_block_io, io->lba, buf, io->len) == 0;
                t1 = now_us();
                break;
            default:
                ok = 0;
                break;
            }
            if (sigprocmask(SIG_SETMASK, &io_old_mask, NULL) != 0) {
                perror("restore SIGINT/SIGTERM mask after I/O");
                ok = 0;
            }
            if (!ok) {
                fprintf(stderr, "selected arm %s failed at sample %u\n",
                        arm_name[a], i);
                run_failed = 1;
                break;
            }
            samples[a][nsample[a]++] = t1 - t0;
            if (stop_signal) {
                stop_seen = 1;
                break;
            }
        }
    }
    if (stop_seen || stop_signal) {
        rc = stop_exit_status();
        goto out;
    }
    idle_snapshot(meas_cpu, &idle_after);

    if (!run_failed &&
        verify_post_run(ffd, blkfd, partfd, lbs, plan, plan_count, plan_stride,
                        primary_bytes, owned_bytes, why, sizeof why) != 0) {
        rc = 7;
        goto out;
    }

    printf("%-30s %8s %8s %8s %8s\n", "arm", "n", "median", "p10", "p99");
    for (int a = 0; a < N_ARM; a++) {
        if (!enabled[a]) continue;
        if (nsample[a] != (int)iters) {
            printf("%-30s %8s\n", arm_name[a], "FAILED");
            run_failed = 1;
            continue;
        }
        qsort(samples[a], (size_t)nsample[a], sizeof **samples, cmp);
        printf("%-30s %8d %8.2f %8.2f %8.2f\n", arm_name[a], nsample[a],
               samples[a][nsample[a] / 2], samples[a][nsample[a] / 10],
               samples[a][nsample[a] * 99 / 100]);
    }
    {
        int total = 0;
        for (int a = 0; a < N_ARM; a++) total += nsample[a];
        if (total) idle_report(meas_cpu, &idle_before, &idle_after, total);
    }
    rc = run_failed ? 7 : 0;

out:
    /* Production handles own their duplicate/paired-character/ring fds.  Close
     * them while every caller-owned retained object and buffer is still live. */
    iopath_close(prod_block_io);
    iopath_close(prod_pt_io);
    /* Rings must be fully quiescent while their command buffers and the
     * anonymous file extents they target are still allocated. */
    ring_destroy(&RP);
    ring_destroy(&R);
    if (afd >= 0) close(afd);
    if (ffd >= 0) close(ffd);
    if (dirfd >= 0) close(dirfd);
    if (chrfd >= 0) close(chrfd);
    if (part_rwfd >= 0) close(part_rwfd);
    if (partfd >= 0) close(partfd);
    if (blkfd >= 0) close(blkfd);
    if (stage_region) munmap(stage_region, STAGE_REGION_BYTES);
    if (huge_region) munmap(huge_region, 4u << 20);
    else free(buf);
    for (int a = 0; a < N_ARM; a++) free(samples[a]);
    free(plan);
    if (stop_signal) {
        rc = stop_exit_status();
        fprintf(stderr, "signal %d requested safe stop after quiescing I/O\n",
                (int)stop_signal);
    }
    if (handlers_installed)
        restore_stop_handlers(&old_int, &old_term, &old_signal_mask);
    return rc;
}
