/* iopath: deliver data to a device LBA behind three interchangeable backends.
 *
 *   IOPATH_PWRITE      pwrite()/pread() at lba * lbs. Path-based opens attempt
 *                      O_DIRECT; retained-fd opens preserve the caller's flags
 *                      and access mode. Works on a loop device, so the LBA
 *                      arithmetic is validated without a real disk.
 *   IOPATH_NVME_IOCTL  NVME_IOCTL_IO_CMD passthru (WRITE 0x01, READ 0x02,
 *                      FLUSH 0x00). Needs a real NVMe controller.
 *   IOPATH_URING_CMD   io_uring IORING_OP_URING_CMD carrying an NVMe passthru
 *                      command. Needs Linux >= 5.19 and a real NVMe controller.
 *   IOPATH_URING_WRITE_POLL
 *                      ordinary IORING_OP_WRITE to an exact O_DIRECT block fd
 *                      on an IOPOLL ring; the block layer remains in the path.
 *
 * iopath_encode_nvme_write() builds the NVMe WRITE command without submitting
 * it, so the command encoding is unit-testable on a machine with no NVMe.
 *
 * NLB convention: the NVMe NLB field (CDW12 bits 15:00) is zero based -- a
 * transfer of N logical blocks is encoded as N-1. The encoder is handed a
 * uint16_t and no logical block size, so it cannot derive the count itself: the
 * caller passes the already-zero-based value and the encoder stores it verbatim.
 * The internal backends do the blocks -> blocks-1 conversion before calling it.
 *
 * The three backends enforce the same two rules the O_DIRECT path needs, and
 * they enforce them in user space rather than handing a mess to the kernel:
 * len must be a multiple of lbs, and buf must be aligned to lbs. Both are
 * refused with -EINVAL on every backend.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <linux/io_uring.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>

#include "exitos_devguard.h"
#include "exitos_iopath.h"

#ifndef IOPATH_EXTERNAL_INTERNAL_FD_HOOKS
__attribute__((weak)) void exitos_internal_fd_enter(int fd) { (void)fd; }
__attribute__((weak)) void exitos_internal_fd_leave(int fd) { (void)fd; }
__attribute__((weak)) int exitos_internal_open_call(const char *path, int flags,
                                                     mode_t mode)
{
    return open(path, flags, mode);
}
__attribute__((weak)) int exitos_internal_openat_call(int dirfd,
                                                       const char *path,
                                                       int flags, mode_t mode)
{
    return openat(dirfd, path, flags, mode);
}
__attribute__((weak)) int exitos_internal_close_call(int fd)
{
    return close(fd);
}
__attribute__((weak)) int exitos_internal_ftruncate_call(int fd, off_t len)
{
    return ftruncate(fd, len);
}
__attribute__((weak)) int exitos_internal_fallocate_call(int fd, int mode,
                                                          off_t off, off_t len)
{
    return fallocate(fd, mode, off, len);
}
__attribute__((weak)) ssize_t exitos_internal_pwrite_call(int fd,
                                                           const void *buf,
                                                           size_t len,
                                                           off_t off)
{
    return pwrite(fd, buf, len, off);
}
__attribute__((weak)) int exitos_internal_fdatasync_call(int fd)
{
    return fdatasync(fd);
}
#endif

static int iopath_internal_close(int fd)
{
    return exitos_internal_close_call(fd);
}

static ssize_t iopath_internal_pwrite(int fd, const void *buf, size_t len,
                                      off_t off)
{
    return exitos_internal_pwrite_call(fd, buf, len, off);
}

static int iopath_internal_fdatasync(int fd)
{
    return exitos_internal_fdatasync_call(fd);
}

/* Page-cache invalidation issued by the intercept core for buffered
 * registrations.  Weak so tests can observe it; the default is the real
 * fadvise -- neither frontend interposes posix_fadvise, and the bpftime
 * reentrancy guard passes the underlying syscall through. */
__attribute__((weak)) int exitos_internal_fadvise_call(int fd, off_t off,
                                                       off_t len, int advice)
{
    return posix_fadvise(fd, off, len, advice);
}

/* ------------------------------------------------------------------ *
 * NVMe opcodes (NVM Command Set).
 * ------------------------------------------------------------------ */
#define NVME_OPC_FLUSH 0x00
#define NVME_OPC_WRITE 0x01
#define NVME_OPC_READ  0x02

/* Largest transfer a single NVMe command can describe: NLB is 16 bits and
 * zero based, so 65536 logical blocks. */
#define NVME_MAX_BLOCKS 65536u

/* ------------------------------------------------------------------ *
 * uring_cmd definitions. The kernel headers on a 5.15 build system have
 * neither IORING_OP_URING_CMD nor the 128-byte SQE, so the ABI pieces are
 * spelled out here under our own names. They are compile-time constants of the
 * kernel ABI, identical on every architecture that io_uring supports.
 * ------------------------------------------------------------------ */
#define EXITOS_IORING_OP_URING_CMD   46u
#define EXITOS_IORING_SETUP_SQE128   (1u << 10)
#define EXITOS_IORING_SETUP_CQE32    (1u << 11)
#define EXITOS_IORING_REGISTER_BUFFERS 0u
/* Micro-optimizations: a registered ring fd lets io_uring_enter skip the
 * per-call fd-table lookup, and a registered device fd lets each SQE skip
 * fget/fput.  Both are pure fast-path savings; when the kernel refuses either
 * registration the handle falls back to the plain path with identical
 * semantics. */
#define EXITOS_IORING_REGISTER_FILES        2u
#define EXITOS_IORING_REGISTER_RING_FDS     20u
#define EXITOS_IORING_ENTER_REGISTERED_RING (1u << 4)
#define EXITOS_IOSQE_FIXED_FILE             1u
struct exitos_rsrc_update {
    uint32_t offset;
    uint32_t resv;
    uint64_t data;
};
/* SQE uring_cmd_flags bit 0 = IORING_URING_CMD_FIXED (uapi name arrived in
 * 6.1; 5.15 build-system headers do not have it). */
#define EXITOS_URING_CMD_FIXED       1u
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

/* Optional bounce staging (EXITOS_STAGE=1, native NVMe backends only).
 *
 * Why it exists: the kernel forces SGL descriptors for user passthrough
 * commands; a payload spanning more than one physical segment then needs an
 * external descriptor list that the device must fetch separately.  Copying
 * such a payload into one physically contiguous slot keeps the whole command
 * inside a single inline descriptor.  The slot region is a private anonymous
 * mapping large enough to guarantee one fully-mapped 2 MiB-aligned range for THP
 * collapse; only the first EXITOS_STAGE_MAX bytes are ever submitted. */
#define EXITOS_STAGE_REGION_BYTES ((size_t)(4u << 20) + 4096u)
#define EXITOS_STAGE_MAX          ((size_t)32768u)
/* Payloads of one page or less are already a single DMA segment (inline SGL);
 * staging them would add memcpy for nothing. */
#define EXITOS_STAGE_MIN_EXCL     ((size_t)4096u)

/* struct nvme_uring_cmd, exactly 72 bytes; field-for-field the passthru command
 * with the trailing 64-bit result replaced by a reserved word. */
struct exitos_nvme_uring_cmd {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t rsvd1;
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t metadata;
    uint64_t addr;
    uint32_t metadata_len;
    uint32_t data_len;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
    uint32_t timeout_ms;
    uint32_t rsvd2;
};
#define EXITOS_NVME_URING_CMD_IO _IOWR('N', 0x80, struct exitos_nvme_uring_cmd)

/* The 128-byte submission queue entry: the ordinary 64-byte SQE followed by 80
 * bytes of command payload starting at offset 48 (the addr3 union). */
struct exitos_sqe128 {
    uint8_t  opcode;        /*  0 */
    uint8_t  flags;         /*  1 */
    uint16_t ioprio;        /*  2 */
    int32_t  fd;            /*  4 */
    uint32_t cmd_op;        /*  8 (union with off/addr2) */
    uint32_t pad1;          /* 12 */
    uint64_t addr;          /* 16 */
    uint32_t len;           /* 24 */
    uint32_t rw_flags;      /* 28 */
    uint64_t user_data;     /* 32 */
    uint16_t buf_index;     /* 40 */
    uint16_t personality;   /* 42 */
    int32_t  splice_fd_in;  /* 44 */
    uint8_t  cmd[80];       /* 48 .. 127 */
};

/* 32 bytes, because the ring is set up with IORING_SETUP_CQE32: the two extra
 * words carry what an NVMe passthru command completed with. The stride matters
 * as much as the contents -- reading a CQE32 ring with a 16-byte stride lands
 * on the middle of an entry from the second completion onwards. */
struct exitos_cqe {
    uint64_t user_data;
    int32_t  res;
    uint32_t flags;
    uint64_t big_cqe[2];
};

/* These three sizes are the kernel ABI, not a local choice. sizeof() is also
 * baked into the ioctl number below, so a mistake here would silently build a
 * command the driver rejects. Fail the build instead. */
_Static_assert(sizeof(struct exitos_nvme_uring_cmd) == 72,
               "struct nvme_uring_cmd must be 72 bytes");
_Static_assert(sizeof(struct exitos_sqe128) == 128,
               "an SQE128 submission queue entry must be 128 bytes");
_Static_assert(offsetof(struct exitos_sqe128, cmd) == 48,
               "the uring_cmd payload starts at byte 48 of the SQE");
_Static_assert(sizeof(struct exitos_cqe) == 32,
               "a CQE32 completion queue entry must be 32 bytes");
_Static_assert(sizeof(struct io_uring_sqe) == 64,
               "an ordinary io_uring SQE must be 64 bytes");
_Static_assert(sizeof(struct io_uring_cqe) == 16,
               "an ordinary io_uring CQE must be 16 bytes");

/* ------------------------------------------------------------------ *
 * Handle.
 * ------------------------------------------------------------------ */
struct iopath {
    int             fd;         /* block device, or NVMe char device */
    iopath_backend  backend;
    uint32_t        lbs;
    uint32_t        nsid;       /* NVMe namespace id, backends 1 and 2 */
    int             direct;     /* O_DIRECT is in effect (pwrite backend) */
    int             fua;        /* durability rides on the write command */

    /* io_uring state for command passthrough and ordinary block WRITE. */
    int             ring_fd;
    void           *sq_ptr;
    size_t          sq_sz;
    void           *cq_ptr;
    size_t          cq_sz;
    struct exitos_sqe128 *sqes;
    size_t          sqes_sz;
    size_t          sqe_stride;
    unsigned       *sq_head;
    unsigned       *sq_tail;
    unsigned       *sq_mask;
    unsigned       *sq_array;
    unsigned       *cq_head;
    unsigned       *cq_tail;
    unsigned       *cq_mask;
    struct exitos_cqe *cqes;
    size_t          cqe_stride;

    /* A one-entry ring is shared by every operation on this handle.  Keep the
     * complete publish -> submit -> reap transaction under one mutex: locking
     * only the SQE fill would still let another caller consume the first
     * caller's CQE.  iopath_close() deliberately does not race this lock; the
     * public lifetime contract requires callers to quiesce the handle first. */
    pthread_mutex_t ring_lock;
    int             ring_lock_init;
    int             ring_poisoned;
    uint64_t        next_token;

    /* Optional bounce staging (EXITOS_STAGE=1).  stage_slot is the 2 MiB-
     * aligned head of stage_region, proven physically contiguous at init.
     * The uring backend copies into it under ring_lock; the ioctl backend
     * serializes copy+submit under stage_lock.  stage_fixed means the slot
     * was registered with the ring and submits may set the FIXED flag. */
    void           *stage_region;
    void           *stage_slot;
    size_t          stage_max;
    int             stage_fixed;
    pthread_mutex_t stage_lock;
    int             stage_lock_init;

    /* Enter through the real ring fd.  IORING_REGISTER_RING_FDS returns an
     * index in current->io_uring, so caching an index in this cross-thread
     * handle would make another submitter address an unrelated slot.  The
     * ring-local fixed-file table is safe to share and remains enabled. */
    int             enter_fd;
    unsigned        enter_flags;
    int             fixed_file;
    /* Caller-declared exclusive ownership: every operation on this handle is
     * externally serialized, so the ring and staging mutexes are skipped.  The
     * intercept core qualifies (all handle use sits under its per-fd lock);
     * standalone callers keep the locked contract unless they opt in. */
    int             exclusive;
    /* Highest LBA whose byte offset still fits off_t, precomputed so the
     * write path divides by nothing. */
    uint64_t        max_lba;
    /* Absolute native-LBA span of an exact retained block object.  Only the
     * ordinary block-WRITE backend consumes these fields. */
    uint64_t        base_lba;
    uint64_t        count_lba;
};

/* ------------------------------------------------------------------ *
 * Small helpers.
 * ------------------------------------------------------------------ */
static int is_pow2_u32(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

static int backend_uses_ring(iopath_backend b)
{
    return b == IOPATH_URING_CMD || b == IOPATH_URING_CMD_POLL ||
           b == IOPATH_URING_WRITE_POLL;
}

static int backend_uses_uring_cmd(iopath_backend b)
{
    return b == IOPATH_URING_CMD || b == IOPATH_URING_CMD_POLL;
}

static int backend_uses_nvme_cmd(iopath_backend b)
{
    return b == IOPATH_NVME_IOCTL || backend_uses_uring_cmd(b);
}

static int backend_uses_block_rw(iopath_backend b)
{
    return b == IOPATH_PWRITE || b == IOPATH_URING_WRITE_POLL;
}

static uint64_t iopath_off_t_max(void)
{
    /* Memoized: the value is an ABI constant, and this used to be recomputed
     * with a 63-iteration loop on every pwrite-backend write.  A concurrent
     * first call recomputes the same value, which is benign. */
    static uint64_t cached;
    if (cached == 0) {
        unsigned bits = (unsigned)(sizeof(off_t) * CHAR_BIT);
        unsigned value_bits = bits - (((off_t)-1 < (off_t)0) ? 1u : 0u);
        uint64_t max = 0;
        unsigned i;
        if (value_bits > 64u) value_bits = 64u;
        for (i = 0; i < value_bits; i++) max = (max << 1) | UINT64_C(1);
        cached = max;
    }
    return cached;
}

/* Precompute the division-free limits that depend on the (power-of-two)
 * logical block size.  Called once whenever a handle's lbs becomes final. */
static void iopath_recompute_limits(struct iopath *p)
{
    p->max_lba = p->lbs ? iopath_off_t_max() / p->lbs : 0;
}

/* The two rules every backend shares. Returns 0, or -EINVAL naming exactly what
 * the caller got wrong. Keep the NULL checks ahead of the alignment check: a
 * NULL pointer is congruent to 0 and would otherwise pass as "aligned". */
static int check_io_args(const struct iopath *p, const void *buf, size_t len)
{
    if (p == NULL || p->fd < 0)
        return -EINVAL;
    if (buf == NULL)
        return -EINVAL;
    /* lbs is enforced power-of-two at every open, so alignment is a mask,
     * not a 64-bit division. */
    if (len == 0 || (len & ((size_t)p->lbs - 1)) != 0)
        return -EINVAL;
    if (((uintptr_t)buf & ((uintptr_t)p->lbs - 1)) != 0)
        return -EINVAL;
    return 0;
}

/* Byte offset of an LBA, refusing anything that would overflow off_t.  The
 * only division moved into iopath_recompute_limits(); max_lba * lbs is by
 * construction within off_t, so len is checked against the remaining room. */
static int lba_to_offset(const struct iopath *p, uint64_t lba, size_t len,
                         off_t *out)
{
    if (lba > p->max_lba)
        return -EOVERFLOW;
    uint64_t off = lba * p->lbs;
    if ((uint64_t)len > iopath_off_t_max() - off)
        return -EOVERFLOW;
    *out = (off_t)off;
    return 0;
}

/* ------------------------------------------------------------------ *
 * NVMe command encoding.
 * ------------------------------------------------------------------ */
static int encode_nvme_rw(struct nvme_passthru_cmd *c, uint8_t opcode,
                          uint32_t nsid, uint64_t slba, uint16_t nlb,
                          const void *buf, size_t len, int fua)
{
    if (c == NULL || buf == NULL)
        return -EINVAL;
    if (len == 0)
        /* NLB is zero based, so the encoded command would claim one logical
         * block while pointing at a zero-length buffer. The device would read
         * a block's worth of bytes from memory the caller never offered. */
        return -EINVAL;
    if (len > (size_t)UINT32_MAX)   /* data_len is 32 bits wide */
        return -EINVAL;

    /* Zero first: every field this command does not use must reach the kernel
     * as 0. A stale metadata pointer is a real hazard, not untidiness. */
    memset(c, 0, sizeof(*c));

    c->opcode   = opcode;
    c->nsid     = nsid;
    c->addr     = (uint64_t)(uintptr_t)buf;
    c->data_len = (uint32_t)len;
    c->cdw10    = (uint32_t)(slba & 0xFFFFFFFFULL);   /* SLBA low 32  */
    c->cdw11    = (uint32_t)(slba >> 32);             /* SLBA high 32 */
    c->cdw12    = (uint32_t)nlb;   /* NLB verbatim: already zero based */
    /* FUA is a WRITE control bit.  Keeping it as an explicit input prevents a
     * handle-level durability promise from getting lost between the public
     * setter and the command that is actually submitted; reads defensively
     * ignore it even if a caller passes a nonzero value by mistake. */
    if (opcode == NVME_OPC_WRITE && fua)
        c->cdw12 |= (1u << 30);
    /* timeout_ms stays 0, which asks the kernel for its own default. */
    return 0;
}

int iopath_encode_nvme_write(void *cmd, uint32_t nsid, uint64_t slba,
                             uint16_t nlb, const void *buf, size_t len)
{
    return encode_nvme_rw((struct nvme_passthru_cmd *)cmd, NVME_OPC_WRITE,
                          nsid, slba, nlb, buf, len, 0);
}

/* ------------------------------------------------------------------ *
 * Backend 2: NVME_IOCTL_IO_CMD passthru.
 * ------------------------------------------------------------------ */
static int nvme_ioctl_submit(int fd, struct nvme_passthru_cmd *c)
{
    int r = ioctl(fd, NVME_IOCTL_IO_CMD, c);
    if (r < 0)
        return -errno;
    if (r > 0)
        return -EIO;    /* nonzero NVMe status field */
    return 0;
}

/* ------------------------------------------------------------------ *
 * Backend 3: IORING_OP_URING_CMD.
 * ------------------------------------------------------------------ */
static int io_uring_setup_raw(unsigned entries, struct io_uring_params *pp)
{
    long r = syscall(__NR_io_uring_setup, entries, pp);
    return (int)r;
}

static int io_uring_enter_raw(const struct iopath *p, unsigned to_submit,
                              unsigned min_complete, unsigned flags)
{
    long r = syscall(__NR_io_uring_enter, p->enter_fd, to_submit, min_complete,
                     flags | p->enter_flags, NULL, (size_t)0);
    return (int)r;
}

static void uring_teardown(struct iopath *p)
{
    if (p->sqes && p->sqes != MAP_FAILED)
        munmap(p->sqes, p->sqes_sz);
    if (p->cq_ptr && p->cq_ptr != MAP_FAILED && p->cq_ptr != p->sq_ptr)
        munmap(p->cq_ptr, p->cq_sz);
    if (p->sq_ptr && p->sq_ptr != MAP_FAILED)
        munmap(p->sq_ptr, p->sq_sz);
    if (p->ring_fd >= 0)
        iopath_internal_close(p->ring_fd);
    p->sqes = NULL;
    p->cq_ptr = NULL;
    p->sq_ptr = NULL;
    p->sq_head = NULL;
    p->sq_tail = NULL;
    p->sq_mask = NULL;
    p->sq_array = NULL;
    p->cq_head = NULL;
    p->cq_tail = NULL;
    p->cq_mask = NULL;
    p->cqes = NULL;
    p->sq_sz = 0;
    p->cq_sz = 0;
    p->sqes_sz = 0;
    p->sqe_stride = 0;
    p->cqe_stride = 0;
    p->ring_fd = -1;
    if (p->ring_lock_init) {
        /* Closing a live handle is outside the API contract; after caller-side
         * quiescence no thread can own or wait on this mutex. */
        (void)pthread_mutex_destroy(&p->ring_lock);
        p->ring_lock_init = 0;
    }
}

/* Bring up a one-entry ring.  NVMe uring_cmd needs SQE128/CQE32; ordinary
 * block I/O uses the standard SQE64/CQE16 ABI. */
static int uring_setup(struct iopath *p)
{
    struct io_uring_params params;
    int lock_rc;

    if (!p || !backend_uses_ring(p->backend))
        return -EINVAL;
    if (backend_uses_uring_cmd(p->backend)) {
        p->sqe_stride = sizeof(struct exitos_sqe128);
        p->cqe_stride = sizeof(struct exitos_cqe);
    } else {
        p->sqe_stride = sizeof(struct io_uring_sqe);
        p->cqe_stride = sizeof(struct io_uring_cqe);
    }

    lock_rc = pthread_mutex_init(&p->ring_lock, NULL);
    if (lock_rc != 0)
        return -lock_rc;
    p->ring_lock_init = 1;
    p->ring_poisoned = 0;
    p->next_token = 0;

    memset(&params, 0, sizeof(params));
    params.flags = iopath_setup_flags(p->backend);

    p->ring_fd = io_uring_setup_raw(1, &params);
    if (p->ring_fd < 0) {
        int e = -errno;
        p->ring_fd = -1;
        uring_teardown(p);
        return e;
    }

    p->sq_sz = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    p->cq_sz = params.cq_off.cqes + params.cq_entries * p->cqe_stride;

    /* Single mmap for both rings when the kernel reports that feature. */
    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        if (p->cq_sz > p->sq_sz)
            p->sq_sz = p->cq_sz;
        p->cq_sz = p->sq_sz;
    }

    p->sq_ptr = mmap(NULL, p->sq_sz, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, p->ring_fd, IORING_OFF_SQ_RING);
    if (p->sq_ptr == MAP_FAILED) {
        int e = -errno;
        p->sq_ptr = NULL;
        uring_teardown(p);
        return e;
    }
    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        p->cq_ptr = p->sq_ptr;
    } else {
        p->cq_ptr = mmap(NULL, p->cq_sz, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, p->ring_fd,
                         IORING_OFF_CQ_RING);
        if (p->cq_ptr == MAP_FAILED) {
            int e = -errno;
            p->cq_ptr = NULL;
            uring_teardown(p);
            return e;
        }
    }

    p->sqes_sz = params.sq_entries * p->sqe_stride;
    p->sqes = mmap(NULL, p->sqes_sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, p->ring_fd, IORING_OFF_SQES);
    if (p->sqes == MAP_FAILED) {
        int e = -errno;
        p->sqes = NULL;
        uring_teardown(p);
        return e;
    }

    unsigned char *sq = p->sq_ptr;
    unsigned char *cq = p->cq_ptr;
    p->sq_head  = (unsigned *)(void *)(sq + params.sq_off.head);
    p->sq_tail  = (unsigned *)(void *)(sq + params.sq_off.tail);
    p->sq_mask  = (unsigned *)(void *)(sq + params.sq_off.ring_mask);
    p->sq_array = (unsigned *)(void *)(sq + params.sq_off.array);
    p->cq_head  = (unsigned *)(void *)(cq + params.cq_off.head);
    p->cq_tail  = (unsigned *)(void *)(cq + params.cq_off.tail);
    p->cq_mask  = (unsigned *)(void *)(cq + params.cq_off.ring_mask);
    p->cqes     = (struct exitos_cqe *)(void *)(cq + params.cq_off.cqes);

    /* The fixed-file table is ring-local and safe for callers in any thread.
     * Do not register the ring fd itself: that index is task-local, cannot be
     * stored in this shared handle, and is not automatically unregistered when
     * a live setup task merely closes the ring. */
    p->enter_fd = p->ring_fd;
    p->enter_flags = 0;
    p->fixed_file = 0;
    {
        int32_t files[1];
        files[0] = p->fd;
        if (syscall(__NR_io_uring_register, p->ring_fd,
                    EXITOS_IORING_REGISTER_FILES, files, 1u) == 0)
            p->fixed_file = 1;
    }
    return 0;
}

/* Declare that every operation on this handle is serialized by the caller,
 * so the internal ring/staging mutexes may be skipped.  Returns 0, or
 * -EINVAL for a null handle.  There is deliberately no way back to the locked
 * contract: flipping it off while another thread is inside an unlocked
 * submit would be undetectable. */
int iopath_set_exclusive(struct iopath *p, int on)
{
    if (p == NULL)
        return -EINVAL;
    if (on)
        p->exclusive = 1;
    return 0;
}

/* Consume every CQE visible at the acquire-load of cq_tail.  CQE32 is encoded
 * in the pointee type, so indexing advances by the kernel's 32-byte stride.
 * A QD1 transaction expects exactly one occurrence of target_token; any other
 * token (or a duplicate target token) is a protocol violation. */
/* Count physically contiguous runs behind the slot span via pagemap, or -1
 * when PFNs are unreadable (no privilege).  Direct proof beats inference; the
 * MADV_COLLAPSE return code is only the fallback for unprivileged processes. */
static int stage_span_runs(const void *ptr, size_t bytes)
{
    /* The direct-call API, like the close below: this runs from iopath_open,
     * which registration calls, and a plain open() there is interposed by the
     * LD_PRELOAD frontend and rewritten by the bpftime one. */
    int pm = exitos_internal_open_call("/proc/self/pagemap", O_RDONLY | O_CLOEXEC, 0);
    int runs = 0;
    uint64_t prev = 0;
    size_t npg = (bytes + 4095u) / 4096u;

    if (pm < 0)
        return -1;
    for (size_t k = 0; k < npg; k++) {
        uint64_t e = 0, pfn;
        if (pread(pm, &e, 8,
                  (off_t)(((uintptr_t)ptr + k * 4096) / 4096) * 8) != 8) {
            runs = -1;
            break;
        }
        pfn = e & ((1ULL << 55) - 1);
        if (!(e & (UINT64_C(1) << 63)) || (e & (UINT64_C(1) << 62)) ||
            pfn == 0) {
            runs = -1;
            break;
        }
        if (k == 0 || pfn != prev + 1)
            runs++;
        prev = pfn;
    }
    iopath_internal_close(pm);
    return runs;
}

/* Arm the optional staging slot on an otherwise fully-built handle.  Never
 * fails the open: an unprovable slot simply leaves staging off and every
 * write goes out exactly as before.  Acceptance requires positive proof of
 * one physical run -- pagemap when readable, otherwise a successful
 * MADV_COLLAPSE, which the kernel only returns for a PMD-mapped range. */
static void iopath_stage_init(struct iopath *p)
{
    const char *v = getenv("EXITOS_STAGE");
    void *region, *slot;
    int collapse_rc, runs, accepted;

    if (!p || !v || strcmp(v, "1") != 0)
        return;
    if (!backend_uses_nvme_cmd(p->backend))
        return; /* the pwrite backend keeps PRP for scattered pages already */
    region = mmap(NULL, EXITOS_STAGE_REGION_BYTES, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        return;
    slot = (void *)(((uintptr_t)region + (2u << 20) - 1) &
                    ~(uintptr_t)((2u << 20) - 1));
    (void)madvise(slot, 2u << 20, MADV_HUGEPAGE);
    memset(slot, 0, 2u << 20);
    collapse_rc = madvise(slot, 2u << 20, MADV_COLLAPSE);
    runs = stage_span_runs(slot, EXITOS_STAGE_MAX);
    accepted = runs == 1 || (runs < 0 && collapse_rc == 0);
    if (!accepted || pthread_mutex_init(&p->stage_lock, NULL) != 0) {
        munmap(region, EXITOS_STAGE_REGION_BYTES);
        return;
    }
    p->stage_lock_init = 1;
    p->stage_region = region;
    p->stage_slot = slot;
    p->stage_max = EXITOS_STAGE_MAX;
    if (backend_uses_uring_cmd(p->backend) && p->ring_fd >= 0) {
        struct iovec iov;
        iov.iov_base = slot;
        iov.iov_len = EXITOS_STAGE_MAX;
        if (syscall(__NR_io_uring_register, p->ring_fd,
                    EXITOS_IORING_REGISTER_BUFFERS, &iov, 1u) == 0)
            p->stage_fixed = 1;
    }
}

static void iopath_stage_teardown(struct iopath *p)
{
    if (!p)
        return;
    if (p->stage_region)
        munmap(p->stage_region, EXITOS_STAGE_REGION_BYTES);
    if (p->stage_lock_init)
        pthread_mutex_destroy(&p->stage_lock);
    p->stage_region = NULL;
    p->stage_slot = NULL;
    p->stage_max = 0;
    p->stage_fixed = 0;
    p->stage_lock_init = 0;
}

static int uring_drain_visible(struct iopath *p, uint64_t target_token,
                               int *target_seen, int *target_res)
{
    unsigned head = __atomic_load_n(p->cq_head, __ATOMIC_RELAXED);
    unsigned initial_head = head;
    int protocol_error = 0;

    for (;;) {
        unsigned tail = __atomic_load_n(p->cq_tail, __ATOMIC_ACQUIRE);
        if (head == tail)
            break;

        struct io_uring_cqe cqe;
        unsigned index = head & *p->cq_mask;
        const unsigned char *slot = (const unsigned char *)(const void *)p->cqes +
                                    (size_t)index * p->cqe_stride;
        memcpy(&cqe, slot, sizeof cqe);
        head++;
        if (target_token != 0 && cqe.user_data == target_token &&
            !*target_seen) {
            *target_seen = 1;
            *target_res = cqe.res;
        } else {
            protocol_error = 1;
        }
    }
    if (head != initial_head)
        __atomic_store_n(p->cq_head, head, __ATOMIC_RELEASE);
    return protocol_error;
}

static int uring_result_from_cqe(const struct iopath *p, int res,
                                 size_t expected_len)
{
    if (res < 0)
        return res;             /* already a negative errno */
    if (p->backend == IOPATH_URING_WRITE_POLL)
        return (size_t)res == expected_len ? 0 : -EIO;
    if (res > 0)
        return -EIO;            /* nonzero NVMe status */
    return 0;
}

/* Submit one NVMe passthru command or ordinary block WRITE through a QD1 ring
 * and wait for the CQE carrying this request's token.
 *
 * The SQ tail is published once.  io_uring_enter() returning EINTR, zero, or a
 * short count is not by itself evidence that the request was not accepted: the
 * shared SQ head and CQ are checked after every return.  Once either the return
 * count or SQ head proves consumption, this function never asks to submit that
 * SQE again and never returns before its target completion is known. */
/* stage_src, when non-NULL, is the caller's original payload: the command in
 * *c already points at the staging slot, and the copy into the slot happens
 * here, under ring_lock, so a concurrent submit on a shared handle can never
 * overwrite a slot whose command has not completed. */
static int uring_submit_qd1(struct iopath *p,
                            const struct nvme_passthru_cmd *c,
                            const void *stage_src,
                            const void *write_buf, size_t write_len,
                            off_t write_off)
{
    struct exitos_nvme_uring_cmd nc;
    struct exitos_sqe128 *sqe;
    struct io_uring_sqe *write_sqe;
    uint64_t token;
    unsigned sq_head_start, tail, index;
    int submitted = 0;
    int target_seen = 0;
    int target_res = 0;
    int lock_rc;
    int rc = -EUCLEAN;

    if (!p->ring_lock_init)
        return -EUCLEAN;
    if (!p->exclusive) {
        lock_rc = pthread_mutex_lock(&p->ring_lock);
        if (lock_rc != 0)
            return -lock_rc;
    }

    if (p->ring_poisoned || !p->sq_head || !p->sq_tail || !p->sq_mask ||
        !p->sq_array || !p->cq_head || !p->cq_tail || !p->cq_mask ||
        !p->sqes || !p->cqes) {
        p->ring_poisoned = 1;
        goto out_unlock;
    }

    /* A synchronous QD1 transaction leaves no completion behind.  Drain a
     * stale batch before publishing anything, poison the handle, and let the
     * caller safely use another path because THIS command never entered the
     * ring. */
    if (uring_drain_visible(p, 0, &target_seen, &target_res)) {
        p->ring_poisoned = 1;
        goto out_unlock;
    }

    sq_head_start = __atomic_load_n(p->sq_head, __ATOMIC_ACQUIRE);
    tail = __atomic_load_n(p->sq_tail, __ATOMIC_RELAXED);
    if (sq_head_start != tail) {
        p->ring_poisoned = 1;
        goto out_unlock;
    }

    token = ++p->next_token;
    if (token == 0)
        token = ++p->next_token;

    index = tail & *p->sq_mask;
    if (backend_uses_uring_cmd(p->backend)) {
        if (!c || write_buf || write_len != 0) {
            rc = -EINVAL;
            goto out_unlock;
        }
        memset(&nc, 0, sizeof(nc));
        nc.opcode       = c->opcode;
        nc.flags        = c->flags;
        nc.rsvd1        = c->rsvd1;
        nc.nsid         = c->nsid;
        nc.cdw2         = c->cdw2;
        nc.cdw3         = c->cdw3;
        nc.metadata     = c->metadata;
        nc.addr         = c->addr;
        nc.metadata_len = c->metadata_len;
        nc.data_len     = c->data_len;
        nc.cdw10        = c->cdw10;
        nc.cdw11        = c->cdw11;
        nc.cdw12        = c->cdw12;
        nc.cdw13        = c->cdw13;
        nc.cdw14        = c->cdw14;
        nc.cdw15        = c->cdw15;
        nc.timeout_ms   = c->timeout_ms;

        if (stage_src) {
            if (!p->stage_slot || c->data_len > p->stage_max ||
                nc.addr != (uint64_t)(uintptr_t)p->stage_slot) {
                rc = -EINVAL;
                goto out_unlock;
            }
            memcpy(p->stage_slot, stage_src, c->data_len);
        }

        sqe = (struct exitos_sqe128 *)(void *)
              ((unsigned char *)(void *)p->sqes +
               (size_t)index * p->sqe_stride);
        memset(sqe, 0, p->sqe_stride);
        sqe->opcode = (uint8_t)EXITOS_IORING_OP_URING_CMD;
        if (p->fixed_file) {
            sqe->fd = 0;                /* registered-file table index */
            sqe->flags |= (uint8_t)EXITOS_IOSQE_FIXED_FILE;
        } else {
            sqe->fd = p->fd;
        }
        sqe->cmd_op = (uint32_t)EXITOS_NVME_URING_CMD_IO;
        sqe->user_data = token;
        if (stage_src && p->stage_fixed) {
            sqe->rw_flags |= EXITOS_URING_CMD_FIXED;
            sqe->buf_index = 0;
        }
        memcpy(sqe->cmd, &nc, sizeof(nc));
    } else if (p->backend == IOPATH_URING_WRITE_POLL) {
        if (c || stage_src || !write_buf || write_len == 0 ||
            write_len > (size_t)INT32_MAX) {
            rc = -EINVAL;
            goto out_unlock;
        }
        write_sqe = (struct io_uring_sqe *)(void *)
                    ((unsigned char *)(void *)p->sqes +
                     (size_t)index * p->sqe_stride);
        memset(write_sqe, 0, p->sqe_stride);
        write_sqe->opcode = IORING_OP_WRITE;
        if (p->fixed_file) {
            write_sqe->fd = 0;
            write_sqe->flags |= (uint8_t)EXITOS_IOSQE_FIXED_FILE;
        } else {
            write_sqe->fd = p->fd;
        }
        write_sqe->off = (uint64_t)write_off;
        write_sqe->addr = (uint64_t)(uintptr_t)write_buf;
        write_sqe->len = (uint32_t)write_len;
        write_sqe->user_data = token;
    } else {
        p->ring_poisoned = 1;
        goto out_unlock;
    }

    p->sq_array[tail & *p->sq_mask] = index;
    __atomic_store_n(p->sq_tail, tail + 1, __ATOMIC_RELEASE);

    for (;;) {
        unsigned now;
        unsigned consumed;
        int r;
        int saved_errno = 0;

        if (uring_drain_visible(p, token, &target_seen, &target_res))
            p->ring_poisoned = 1;
        if (target_seen) {
            rc = uring_result_from_cqe(p, target_res, write_len);
            break;
        }

        now = __atomic_load_n(p->sq_head, __ATOMIC_ACQUIRE);
        consumed = now - sq_head_start;
        if (consumed != 0) {
            submitted = 1;
            if (consumed != 1)
                p->ring_poisoned = 1;
        }

        r = io_uring_enter_raw(p, submitted ? 0u : 1u, 1,
                               IORING_ENTER_GETEVENTS);
        if (r < 0)
            saved_errno = errno;

        /* A return count is another proof of consumption even if the shared
         * head's cache line is not observed until the following acquire. */
        if (!submitted && r > 0) {
            submitted = 1;
            if (r != 1)
                p->ring_poisoned = 1;
        } else if (submitted && r > 0) {
            /* A wait-only enter cannot legally submit anything. */
            p->ring_poisoned = 1;
        }

        if (r < 0) {
            if (uring_drain_visible(p, token, &target_seen, &target_res))
                p->ring_poisoned = 1;
            now = __atomic_load_n(p->sq_head, __ATOMIC_ACQUIRE);
            consumed = now - sq_head_start;
            if (consumed != 0) {
                submitted = 1;
                if (consumed != 1)
                    p->ring_poisoned = 1;
            }
            if (target_seen) {
                rc = uring_result_from_cqe(p, target_res, write_len);
                break;
            }

            if (!submitted) {
                if (p->ring_poisoned) {
                    /* The new SQE was not consumed, but it has already been
                     * published.  Leave the monotonic ring tail alone and make
                     * poison ensure no later enter can ever submit it. */
                    rc = -EUCLEAN;
                    break;
                }
                if (saved_errno == EINTR || saved_errno == EAGAIN ||
                    saved_errno == EBUSY)
                    continue;

                /* A fatal ring-level error makes this ring untrustworthy.  The
                 * SQ head proves that this request did not enter the kernel, so
                 * an upper layer may safely submit it elsewhere; poison keeps
                 * the still-published SQE from being entered in the future. */
                p->ring_poisoned = 1;
                rc = -saved_errno;
                break;
            }

            /* An accepted command has unknown status.  Returning an error here
             * would make intercept.c issue the same write through the filesystem.
             * Poison future submissions, but keep reaping until this token is
             * observed.  EINTR/EAGAIN/EBUSY are ordinary retryable wait errors;
             * other errors additionally make the ring permanently unusable. */
            if (saved_errno != EINTR && saved_errno != EAGAIN &&
                saved_errno != EBUSY)
                p->ring_poisoned = 1;
        }
    }

out_unlock:
    if (!p->exclusive)
        (void)pthread_mutex_unlock(&p->ring_lock);
    return rc;
}

static int uring_cmd_submit(struct iopath *p,
                            const struct nvme_passthru_cmd *c,
                            const void *stage_src)
{
    return uring_submit_qd1(p, c, stage_src, NULL, 0, 0);
}

static int uring_write_submit(struct iopath *p, off_t off,
                              const void *buf, size_t len)
{
    return uring_submit_qd1(p, NULL, NULL, buf, len, off);
}

struct block_namespace_info {
    dev_t rdev;
    uint32_t lbs;
};

static int block_namespace_info_from_fd(int fd, struct block_namespace_info *out)
{
    struct block_namespace_info info;
    struct stat st;
    int lbs = 0;
    if (fd < 0 || !out || fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode) ||
        !exitos_devguard_whole_block_at("/sys", major(st.st_rdev),
                                        minor(st.st_rdev)) ||
        ioctl(fd, BLKSSZGET, &lbs) != 0 || lbs <= 0 ||
        !is_pow2_u32((uint32_t)lbs))
        return 0;
    info.rdev = st.st_rdev;
    info.lbs = (uint32_t)lbs;
    *out = info;
    return 1;
}

/* Which character device belongs to this namespace, according to sysfs?
 *
 * Both /sys/class/block/<blk>/device and /sys/class/nvme-generic/<ng>/device
 * resolve to the controller (or, for a multipath namespace, the subsystem) that
 * owns them, so the one that matches is the right character device. Asking is
 * necessary because the two numbering schemes can disagree: a PCI rescan can
 * bring a namespace back as /dev/nvme1n2 while its character device comes back
 * as /dev/ng4n2. Returns 1 and fills out on success. */
static int nvme_char_device_from_sysfs(const char *dev_path, char *out, size_t outsz,
                                       struct block_namespace_info *block_out)
{
    struct block_namespace_info block;
    int bfd;
    DIR *d;
    struct dirent *e;
    int found = 0;

    bfd = exitos_internal_open_call(dev_path, O_RDONLY | O_CLOEXEC, 0);
    if (bfd < 0)
        return 0;
    if (!block_namespace_info_from_fd(bfd, &block)) {
        iopath_internal_close(bfd);
        return 0;
    }

    d = opendir("/sys/class/nvme-generic");
    if (d) {
        while ((e = readdir(d)) != NULL) {
            char cand[320];
            struct stat cs;
            int cfd, id;
            if (e->d_name[0] == '.')
                continue;
            if (snprintf(cand, sizeof cand, "/dev/%s", e->d_name) >= (int)sizeof cand ||
                access(cand, R_OK | W_OK) != 0)
                continue;
            cfd = exitos_internal_open_call(cand, O_RDONLY | O_CLOEXEC, 0);
            if (cfd < 0)
                continue;
            id = ioctl(cfd, NVME_IOCTL_ID);
            if (fstat(cfd, &cs) == 0 && S_ISCHR(cs.st_mode) && id > 0 &&
                exitos_devguard_nvme_pair_at("/sys",
                    major(block.rdev), minor(block.rdev),
                    major(cs.st_rdev), minor(cs.st_rdev), (uint32_t)id) &&
                strlen(cand) < outsz) {
                memcpy(out, cand, strlen(cand) + 1);
                found = 1;
            }
            iopath_internal_close(cfd);
            if (found)
                break;
        }
        closedir(d);
    }
    if (found && block_out)
        *block_out = block;
    iopath_internal_close(bfd);
    return found;
}

/* NVMe passthru over io_uring is served by the character device (/dev/ngXnY),
 * not the block device (/dev/nvmeXnY). Ask sysfs which one belongs to this
 * namespace.  Kernel block and generic-character numbering can diverge after a
 * reprobe, so failure to prove the pair is a refusal, never a name-derived
 * guess. */
static int nvme_char_device_path(const char *dev_path, char *out, size_t outsz,
                                 struct block_namespace_info *block_out)
{
    return nvme_char_device_from_sysfs(dev_path, out, outsz, block_out);
}

/* ------------------------------------------------------------------ *
 * Open / close.
 * ------------------------------------------------------------------ */
struct iopath *iopath_open(const char *dev_path, iopath_backend b, uint32_t lbs)
{
    struct iopath *fd_result;
    int exact_fd;

    if (dev_path == NULL || *dev_path == '\0')
        return NULL;
    if (!is_pow2_u32(lbs))
        return NULL;
    /* IOPATH_URING_CMD_POLL was missing from this list, so every attempt to
     * open the polled backend was refused at the door and the polled path had
     * never run once -- while iopath_setup_flags() happily returned the right
     * flags for it and the unit tests checked those flags. A backend that
     * cannot be opened cannot be measured. */
    if (b != IOPATH_PWRITE && b != IOPATH_NVME_IOCTL &&
        b != IOPATH_URING_CMD && b != IOPATH_URING_CMD_POLL &&
        b != IOPATH_URING_WRITE_POLL)
        return NULL;

    /* The ordinary block path must retain this exact block object with direct
     * read/write access.  Unlike PWRITE, failure to obtain O_DIRECT is final;
     * silently switching to buffered I/O would no longer be the requested
     * backend.  iopath_open_fd() performs all identity/geometry/poll checks. */
    if (b == IOPATH_URING_WRITE_POLL) {
        exact_fd = exitos_internal_open_call(dev_path,
                                             O_RDWR | O_DIRECT | O_CLOEXEC, 0);
        if (exact_fd < 0)
            return NULL;
        fd_result = iopath_open_fd(exact_fd, b, lbs);
        iopath_internal_close(exact_fd);
        return fd_result;
    }
    /* Asking for polled completion on a device that has no poll queues used to
     * succeed: io_uring accepts IORING_SETUP_IOPOLL regardless, the kernel maps
     * the command onto an ordinary interrupt queue, and every command completes.
     * The caller then times an interrupt-driven path and reports it as polled.
     * The two queue types are not interchangeable, so the open is refused
     * instead. iopath_poll_available() was written to answer exactly this and
     * nothing called it. */
    if (b == IOPATH_URING_CMD_POLL && !iopath_poll_available(dev_path))
        return NULL;

    struct iopath *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NULL;
    p->fd      = -1;
    p->ring_fd = -1;
    p->backend = b;
    p->lbs     = b == IOPATH_PWRITE ? lbs : 0;
    p->nsid    = 0;

    char path[512];
    struct block_namespace_info passthru_block = {0};
    /* Refuse rather than truncate. snprintf silently cut a 517-byte path down
     * to 511 bytes and opened whatever file that shorter name happened to
     * refer to, so a write aimed at the device the caller named landed in an
     * unrelated file -- with no error anywhere. */
    if (strlen(dev_path) >= sizeof(path)) {
        free(p);
        return NULL;
    }
    snprintf(path, sizeof(path), "%s", dev_path);
    if (backend_uses_uring_cmd(b) &&
        !nvme_char_device_path(dev_path, path, sizeof(path),
                               &passthru_block)) {
        free(p);
        return NULL;
    }

    if (b == IOPATH_PWRITE) {
        /* O_DIRECT keeps the page cache out of the way, so what the test reads
         * back really came off the device. Some backing stores refuse it; fall
         * back to buffered I/O rather than failing the open, since the module
         * enforces the alignment rules itself either way. */
        p->fd = exitos_internal_open_call(path, O_RDWR | O_DIRECT | O_CLOEXEC, 0);
        if (p->fd >= 0) {
            p->direct = 1;
        } else if (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP) {
            p->fd = exitos_internal_open_call(path, O_RDWR | O_CLOEXEC, 0);
            p->direct = 0;
        }
    } else {
        p->fd = exitos_internal_open_call(path, O_RDWR | O_CLOEXEC, 0);
    }
    if (p->fd < 0) {
        free(p);
        return NULL;
    }

    /* The target has to be something that stores bytes at offsets we choose.
     * Only directories were refused before, so /dev/null, a FIFO and a socket
     * all opened successfully: a write to /dev/null reported 4096 bytes
     * written and the read back failed, which is data reported as durable and
     * then gone. A FIFO has no byte offsets at all. */
    struct stat st;
    if (fstat(p->fd, &st) != 0) {
        iopath_internal_close(p->fd);
        free(p);
        return NULL;
    }
    if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode)) {
        iopath_internal_close(p->fd); /* FIFO, socket, directory */
        free(p);
        return NULL;
    }
    if (b == IOPATH_PWRITE && S_ISCHR(st.st_mode)) {
        iopath_internal_close(p->fd); /* character device is not addressable storage */
        free(p);
        return NULL;
    }

    if (backend_uses_nvme_cmd(b)) {
        struct block_namespace_info block;
        /* Ask the driver which namespace this node addresses. A node that
         * cannot answer is not an NVMe namespace, and these backends have
         * nothing to send commands to -- /dev/null answers no, and so does a
         * regular file. Both used to open successfully and then fail, or
         * silently succeed, at the first command. */
        if (b == IOPATH_NVME_IOCTL) {
            if (!block_namespace_info_from_fd(p->fd, &block) || block.lbs != lbs) {
                iopath_internal_close(p->fd);
                free(p);
                return NULL;
            }
        } else {
            block = passthru_block;
            if (block.rdev == 0 || block.lbs != lbs) {
                iopath_internal_close(p->fd);
                free(p);
                return NULL;
            }
        }
        p->lbs = block.lbs;
        if (b == IOPATH_NVME_IOCTL && !S_ISBLK(st.st_mode)) {
            iopath_internal_close(p->fd);
            free(p);
            return NULL;
        }
        int id = ioctl(p->fd, NVME_IOCTL_ID);
        if (id <= 0) {
            iopath_internal_close(p->fd);
            free(p);
            return NULL;
        }
        if (b != IOPATH_NVME_IOCTL) {
            if (!S_ISCHR(st.st_mode) || block.rdev == 0 ||
                !exitos_devguard_nvme_pair_at("/sys",
                    major(block.rdev), minor(block.rdev),
                    major(st.st_rdev), minor(st.st_rdev), (uint32_t)id)) {
                iopath_internal_close(p->fd);
                free(p);
                return NULL;
            }
        }
        p->nsid = (uint32_t)id;
    }

    if (backend_uses_ring(b)) {
        int rc = uring_setup(p);
        if (rc < 0) {           /* no IORING_OP_URING_CMD before Linux 5.19 */
            iopath_internal_close(p->fd);
            free(p);
            return NULL;
        }
    }
    iopath_recompute_limits(p);
    iopath_stage_init(p);
    return p;
}

static struct iopath *iopath_open_same_description(int fd, iopath_backend b,
                                                   uint32_t lbs)
{
    struct block_namespace_info block = {0};
    struct iopath *p;
    struct stat st;
    int held, flags, id = 0;

    if (fd < 0 || !is_pow2_u32(lbs) ||
        (b != IOPATH_PWRITE && b != IOPATH_NVME_IOCTL))
        return NULL;
    held = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (held < 0)
        return NULL;
    if (fstat(held, &st) != 0 ||
        (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode)) ||
        (b == IOPATH_NVME_IOCTL &&
         (!block_namespace_info_from_fd(held, &block) || block.lbs != lbs))) {
        iopath_internal_close(held);
        return NULL;
    }
    if (b == IOPATH_NVME_IOCTL) {
        id = ioctl(held, NVME_IOCTL_ID);
        if (id <= 0) {
            iopath_internal_close(held);
            return NULL;
        }
    }
    p = calloc(1, sizeof *p);
    if (!p) {
        iopath_internal_close(held);
        return NULL;
    }
    p->fd = held;
    p->ring_fd = -1;
    p->backend = b;
    p->lbs = b == IOPATH_NVME_IOCTL ? block.lbs : lbs;
    p->nsid = b == IOPATH_NVME_IOCTL ? (uint32_t)id : 0;
    flags = fcntl(held, F_GETFL);
    p->direct = flags >= 0 && (flags & O_DIRECT) != 0;
    iopath_recompute_limits(p);
    iopath_stage_init(p);
    return p;
}

static struct iopath *iopath_open_uring_write_description(int fd, uint32_t lbs)
{
    struct iopath *p = NULL;
    struct stat st;
    uint64_t size_bytes = 0, base_lba = 0, count_lba = 0;
    int held = -1, flags, actual_lbs = 0;

    if (fd < 0 || !is_pow2_u32(lbs))
        return NULL;
    held = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (held < 0)
        return NULL;
    flags = fcntl(held, F_GETFL);
    if (fstat(held, &st) != 0 || !S_ISBLK(st.st_mode) || flags < 0 ||
        (flags & O_ACCMODE) != O_RDWR || (flags & O_DIRECT) == 0 ||
        ioctl(held, BLKSSZGET, &actual_lbs) != 0 || actual_lbs <= 0 ||
        !is_pow2_u32((uint32_t)actual_lbs) || (uint32_t)actual_lbs != lbs ||
        ioctl(held, BLKGETSIZE64, &size_bytes) != 0 || size_bytes == 0 ||
        size_bytes % lbs != 0 ||
        exitos_devguard_lba_span_fd(held, &base_lba, &count_lba) != 0 ||
        count_lba == 0 || base_lba > UINT64_MAX - count_lba ||
        size_bytes / lbs != count_lba || !iopath_poll_available_fd(held)) {
        iopath_internal_close(held);
        return NULL;
    }

    p = calloc(1, sizeof *p);
    if (!p) {
        iopath_internal_close(held);
        return NULL;
    }
    p->fd = held;
    p->ring_fd = -1;
    p->backend = IOPATH_URING_WRITE_POLL;
    p->lbs = lbs;
    p->direct = 1;
    p->base_lba = base_lba;
    p->count_lba = count_lba;
    iopath_recompute_limits(p);
    if (uring_setup(p) != 0) {
        iopath_internal_close(held);
        free(p);
        return NULL;
    }
    /* EXITOS_STAGE belongs only to NVMe-command passthrough.  In particular,
     * do not copy or register an arbitrary caller buffer for this backend. */
    return p;
}

struct iopath *iopath_open_fd(int fd, iopath_backend b, uint32_t lbs)
{
    char procfd[64];
    struct block_namespace_info block;
    struct stat st;
    struct iopath *result;
    int held, n;
    if (fd < 0) return NULL;
    /* PWRITE and block-node ioctl operate on a duplicate of the retained open
     * file description.  This preserves both object identity and access mode:
     * in particular an O_RDONLY selector remains readable but cannot be
     * silently upgraded to O_RDWR through /proc/self/fd. */
    if (b == IOPATH_PWRITE || b == IOPATH_NVME_IOCTL)
        return iopath_open_same_description(fd, b, lbs);
    if (b == IOPATH_URING_WRITE_POLL)
        return iopath_open_uring_write_description(fd, lbs);
    if (!backend_uses_uring_cmd(b))
        return NULL;
    /* All native backends derive LBS from the already-selected block object
     * before character-device discovery, poll probing, or ring setup.  This is
     * also what makes a caller mismatch deterministic across ioctl and both
     * uring variants. */
    if (backend_uses_nvme_cmd(b) &&
        (!block_namespace_info_from_fd(fd, &block) || block.lbs != lbs))
        return NULL;
    held = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (held < 0) return NULL;
    if (fstat(held, &st) != 0) { iopath_internal_close(held); return NULL; }
    n = snprintf(procfd, sizeof procfd, "/proc/self/fd/%d", held);
    if (n < 0 || (size_t)n >= sizeof procfd) { iopath_internal_close(held); return NULL; }
    /* uring_cmd must use the namespace's separately opened generic character
     * device.  /proc/self/fd/N is used only as a stable retained block-identity
     * anchor for sysfs discovery and pair verification; the PWRITE/ioctl cases
     * above never reopen it. */
    result = iopath_open(procfd, b, lbs);
    iopath_internal_close(held);
    return result;
}

void iopath_close(struct iopath *p)
{
    if (p == NULL)
        return;
    iopath_stage_teardown(p);
    if (backend_uses_ring(p->backend))
        uring_teardown(p);
    if (p->fd >= 0)
        iopath_internal_close(p->fd);
    free(p);
}

uint32_t iopath_lbs(const struct iopath *p)
{
    return p ? p->lbs : 0;
}

uint32_t iopath_nsid(const struct iopath *p)
{
    return p ? p->nsid : 0;
}

/* ------------------------------------------------------------------ *
 * Data path.
 * ------------------------------------------------------------------ */
static int pwrite_all(int fd, const void *buf, size_t len, off_t off)
{
    const unsigned char *b = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = iopath_internal_pwrite(fd, b + done, len - done,
                                           off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)             /* past the end of the device */
            return -ENOSPC;
        done += (size_t)n;
    }
    return 0;
}

static int pread_all(int fd, void *buf, size_t len, off_t off)
{
    unsigned char *b = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, b + done, len - done, off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)             /* end of device: short read, not a success */
            return -EIO;
        done += (size_t)n;
    }
    return 0;
}

/* Translate the absolute native LBA produced by Maco to the exact block fd's
 * relative byte offset.  Every range check precedes SQE publication. */
static int block_write_offset(const struct iopath *p, uint64_t absolute_lba,
                              size_t len, off_t *out)
{
    uint64_t relative_lba, blocks, off;

    if (!p || !out || p->lbs == 0 || p->count_lba == 0)
        return -EINVAL;
    if (len == 0 || len > (size_t)INT32_MAX)
        return -EOVERFLOW;
    blocks = (uint64_t)(len / p->lbs);
    if (absolute_lba < p->base_lba)
        return -ERANGE;
    relative_lba = absolute_lba - p->base_lba;
    if (relative_lba >= p->count_lba || blocks > p->count_lba - relative_lba)
        return -ERANGE;
    if (relative_lba > p->max_lba)
        return -EOVERFLOW;
    off = relative_lba * p->lbs;
    if ((uint64_t)len > iopath_off_t_max() - off)
        return -EOVERFLOW;
    *out = (off_t)off;
    return 0;
}

/* Shared body for the two NVMe backends. */
static int nvme_rw(struct iopath *p, uint8_t opcode, uint64_t lba,
                   const void *buf, size_t len)
{
    const void *stage_src = NULL;
    size_t blocks = len / p->lbs;
    if (blocks == 0 || blocks > NVME_MAX_BLOCKS)
        return -EINVAL;

    /* Stage only multi-page WRITE payloads that fit the slot.  Reads are
     * never staged: data flows device-to-host and must land in the caller's
     * buffer directly. */
    if (opcode == NVME_OPC_WRITE && p->stage_slot &&
        len > EXITOS_STAGE_MIN_EXCL && len <= p->stage_max) {
        stage_src = buf;
        buf = p->stage_slot;
    }

    struct nvme_passthru_cmd c;
    int rc = encode_nvme_rw(&c, opcode, p->nsid, lba,
                            (uint16_t)(blocks - 1), buf, len,
                            opcode == NVME_OPC_WRITE ? p->fua : 0);
    if (rc < 0)
        return rc;

    if (p->backend == IOPATH_URING_CMD || p->backend == IOPATH_URING_CMD_POLL)
        return uring_cmd_submit(p, &c, stage_src);
    if (stage_src) {
        /* The ioctl backend has no ring lock; serialize copy+submit so a
         * concurrent write on a shared handle cannot overwrite the slot
         * while the controller may still read it.  A caller that declared
         * exclusive ownership already serializes and skips the mutex. */
        if (!p->exclusive) {
            int lock_rc = pthread_mutex_lock(&p->stage_lock);
            if (lock_rc != 0)
                return -lock_rc;
        }
        memcpy(p->stage_slot, stage_src, len);
        rc = nvme_ioctl_submit(p->fd, &c);
        if (!p->exclusive)
            pthread_mutex_unlock(&p->stage_lock);
        return rc;
    }
    return nvme_ioctl_submit(p->fd, &c);
}

int iopath_write(struct iopath *p, uint64_t lba, const void *buf, size_t len)
{
    int rc = check_io_args(p, buf, len);
    if (rc < 0)
        return rc;

    if (p->backend == IOPATH_PWRITE) {
        off_t off;
        rc = lba_to_offset(p, lba, len, &off);
        if (rc < 0)
            return rc;
        return pwrite_all(p->fd, buf, len, off);
    }
    if (p->backend == IOPATH_URING_WRITE_POLL) {
        off_t off;
        rc = block_write_offset(p, lba, len, &off);
        if (rc < 0)
            return rc;
        return uring_write_submit(p, off, buf, len);
    }
    return nvme_rw(p, NVME_OPC_WRITE, lba, buf, len);
}

int iopath_read(struct iopath *p, uint64_t lba, void *buf, size_t len)
{
    int rc = check_io_args(p, buf, len);
    if (rc < 0)
        return rc;

    if (p->backend == IOPATH_PWRITE) {
        off_t off;
        rc = lba_to_offset(p, lba, len, &off);
        if (rc < 0)
            return rc;
        return pread_all(p->fd, buf, len, off);
    }
    if (p->backend == IOPATH_URING_WRITE_POLL)
        return -EOPNOTSUPP;
    return nvme_rw(p, NVME_OPC_READ, lba, buf, len);
}

int iopath_flush(struct iopath *p)
{
    if (p == NULL || p->fd < 0)
        return -EINVAL;

    /* With FUA set, every write this handle issued already reached persistent
     * media, so there is nothing left to flush. The header states this as the
     * contract; the code used to ignore the FUA state and issue a real FLUSH
     * (or an fdatasync) anyway, paying for a round trip that cannot change the
     * outcome. */
    if (p->fua)
        return 0;

    if (backend_uses_block_rw(p->backend)) {
        if (iopath_internal_fdatasync(p->fd) != 0)
            return -errno;
        return 0;
    }

    /* NVMe FLUSH: no data buffer, no LBA range, namespace only. */
    struct nvme_passthru_cmd c;
    memset(&c, 0, sizeof(c));
    c.opcode = NVME_OPC_FLUSH;
    c.nsid   = p->nsid;

    if (p->backend == IOPATH_URING_CMD || p->backend == IOPATH_URING_CMD_POLL)
        return uring_cmd_submit(p, &c, NULL);
    return nvme_ioctl_submit(p->fd, &c);
}


/* ---- Force Unit Access ------------------------------------------------------
 * FUA is not a flush: it forces just this write to persistent media instead of
 * draining the whole device cache, so it does not get more expensive when
 * several writes are in flight, and it removes the need to follow the write
 * with a separate NVMe FLUSH command.
 * -------------------------------------------------------------------------- */
int iopath_encode_nvme_write_fua(void *cmd, uint32_t nsid, uint64_t slba,
                                 uint16_t nlb, const void *buf, size_t len, int fua)
{
    return encode_nvme_rw((struct nvme_passthru_cmd *)cmd, NVME_OPC_WRITE,
                          nsid, slba, nlb, buf, len, fua ? 1 : 0);
}

int iopath_set_fua(struct iopath *p, int on)
{
    if (p == NULL)
        return -EINVAL;
    /* pwrite/pread carry no NVMe command dword in which to encode FUA.  Saying
     * yes here used to make iopath_flush() return success without issuing the
     * fdatasync that this backend still needs.  Refuse the promise and leave
     * the handle in flush-based mode. */
    if (on && backend_uses_block_rw(p->backend)) {
        p->fua = 0;
        return -EOPNOTSUPP;
    }
    p->fua = on ? 1 : 0;
    return 0;
}

int iopath_get_fua(const struct iopath *p)
{
    return (p == NULL) ? -EINVAL : p->fua;
}


/* ---- polling backend ------------------------------------------------------
 * IORING_SETUP_IOPOLL makes the kernel poll the device completion queue instead
 * of taking an interrupt. It is only legal on a ring whose target supports it,
 * which for NVMe means the driver allocated poll queues at probe time. */
unsigned iopath_setup_flags(iopath_backend b)
{
    switch (b) {
    case IOPATH_URING_CMD:      return IOPATH_SETUP_SQE128 | IOPATH_SETUP_CQE32;
    case IOPATH_URING_CMD_POLL: return IOPATH_SETUP_SQE128 | IOPATH_SETUP_CQE32 | IOPATH_SETUP_IOPOLL;
    case IOPATH_URING_WRITE_POLL:return IOPATH_SETUP_IOPOLL;
    case IOPATH_PWRITE:
    case IOPATH_NVME_IOCTL:
    default:                    return 0;   /* neither uses a ring */
    }
}

int iopath_poll_available_fd(int fd)
{
    return exitos_devguard_poll_available_fd(fd);
}

int iopath_poll_available(const char *dev_path)
{
    int fd, available;
    if (!dev_path || !*dev_path) return 0;
    fd = exitos_internal_open_call(dev_path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return 0;
    available = exitos_devguard_poll_available_fd(fd);
    if (exitos_internal_close_call(fd) != 0) available = 0;
    return available;
}
