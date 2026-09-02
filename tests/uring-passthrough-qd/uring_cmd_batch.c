#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "uring_cmd_batch.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif

#define QDB_NVME_WRITE 0x01u

struct qdb_cqe32 {
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
    uint64_t extra[2];
};

struct qdb_ring {
    enum qdb_backend backend;
    int target_fd;
    int ring_fd;
    uint32_t nsid;
    uint32_t logical_block_size;

    void *sq_map;
    void *cq_map;
    void *sqes_map;
    size_t sq_map_len;
    size_t cq_map_len;
    size_t sqes_map_len;
    size_t sqe_stride;
    size_t cqe_stride;

    unsigned *sq_head;
    unsigned *sq_tail;
    unsigned *sq_mask;
    unsigned *sq_array;
    unsigned *cq_head;
    unsigned *cq_tail;
    unsigned *cq_mask;
    void *cqes;

    unsigned batch_tail;
    unsigned staged;
};

_Static_assert(sizeof(struct qdb_nvme_uring_cmd) == 72,
               "NVMe uring command must be 72 bytes");
_Static_assert(sizeof(struct qdb_sqe128) == 128,
               "SQE128 must be 128 bytes");
_Static_assert(offsetof(struct qdb_sqe128, cmd) == 48,
               "uring command begins at SQE byte 48");
_Static_assert(sizeof(struct qdb_cqe32) == 32,
               "CQE32 must be 32 bytes");

int qdb_qd_supported(unsigned qd)
{
    return qd == 1 || qd == 2 || qd == 4 || qd == 8;
}

unsigned qdb_setup_flags(enum qdb_backend backend)
{
    if (backend == QDB_BACKEND_PASSTHROUGH)
        return IORING_SETUP_IOPOLL | IORING_SETUP_SQE128 |
               IORING_SETUP_CQE32;
    if (backend == QDB_BACKEND_EXT4)
        return IORING_SETUP_IOPOLL;
    return 0;
}

int qdb_encode_passthrough_sqe(struct qdb_sqe128 *sqe, int fd,
                               uint32_t nsid, uint32_t logical_block_size,
                               const struct qdb_item *item)
{
    struct qdb_nvme_uring_cmd command;
    uint32_t blocks;

    if (!sqe || !item || !item->buffer || fd < 0 || nsid == 0 ||
        logical_block_size == 0 ||
        QDB_BLOCK_SIZE % logical_block_size != 0 ||
        ((uintptr_t)item->buffer % QDB_BLOCK_SIZE) != 0 ||
        item->token == 0)
        return -EINVAL;
    blocks = QDB_BLOCK_SIZE / logical_block_size;
    if (blocks == 0 || blocks > UINT16_MAX + 1u)
        return -EINVAL;

    memset(sqe, 0, sizeof *sqe);
    memset(&command, 0, sizeof command);
    sqe->opcode = 46; /* IORING_OP_URING_CMD */
    sqe->fd = fd;
    sqe->cmd_op = QDB_NVME_URING_CMD_IO;
    sqe->user_data = item->token;

    command.opcode = QDB_NVME_WRITE;
    command.nsid = nsid;
    command.addr = (uint64_t)(uintptr_t)item->buffer;
    command.data_len = QDB_BLOCK_SIZE;
    command.cdw10 = (uint32_t)item->target;
    command.cdw11 = (uint32_t)(item->target >> 32);
    command.cdw12 = blocks - 1u;
    memcpy(sqe->cmd, &command, sizeof command);
    return 0;
}

int qdb_encode_ext4_sqe(struct io_uring_sqe *sqe, int fd,
                        const struct qdb_item *item)
{
    if (!sqe || !item || !item->buffer || fd < 0 || item->token == 0 ||
        ((uintptr_t)item->buffer % QDB_BLOCK_SIZE) != 0 ||
        item->target % QDB_BLOCK_SIZE != 0)
        return -EINVAL;
    memset(sqe, 0, sizeof *sqe);
    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->off = item->target;
    sqe->addr = (uint64_t)(uintptr_t)item->buffer;
    sqe->len = QDB_BLOCK_SIZE;
    sqe->user_data = item->token;
    return 0;
}

static int batch_is_valid(enum qdb_backend backend,
                          const struct qdb_item *items, unsigned qd)
{
    unsigned i, j;

    if ((backend != QDB_BACKEND_EXT4 &&
         backend != QDB_BACKEND_PASSTHROUGH) ||
        !items || !qdb_qd_supported(qd))
        return 0;
    for (i = 0; i < qd; i++) {
        if (!items[i].buffer || items[i].token == 0 ||
            (uintptr_t)items[i].buffer % QDB_BLOCK_SIZE != 0 ||
            (backend == QDB_BACKEND_EXT4 &&
             items[i].target % QDB_BLOCK_SIZE != 0))
            return 0;
        for (j = 0; j < i; j++) {
            if (items[i].buffer == items[j].buffer ||
                items[i].target == items[j].target ||
                items[i].token == items[j].token)
                return 0;
        }
    }
    return 1;
}

int qdb_execute_batch(const struct qdb_driver *driver, void *opaque,
                      enum qdb_backend backend, const struct qdb_item *items,
                      unsigned qd, struct qdb_batch_stats *stats)
{
    uint64_t seen[QDB_RING_DEPTH];
    unsigned accepted, nseen = 0, i;
    int rc, bad = 0, failure_rc = 0, incomplete = 0;

    if (!driver || !driver->stage || !driver->publish || !driver->enter ||
        !driver->reap || !stats || !batch_is_valid(backend, items, qd))
        return -EINVAL;
    for (i = 0; i < qd; i++) {
        rc = driver->stage(opaque, backend, i, &items[i]);
        if (rc != 0) return rc < 0 ? rc : -EIO;
    }
    rc = driver->publish(opaque, qd);
    if (rc != 0) return rc < 0 ? rc : -EIO;

    stats->submit_calls++;
    rc = driver->enter(opaque, qd, qd, IORING_ENTER_GETEVENTS);
    if (rc < 0) return rc;
    if ((unsigned)rc > qd) return -EIO;
    accepted = (unsigned)rc;
    stats->submitted_commands += accepted;
    if (accepted != qd)
        incomplete = 1;
    else if (stats->max_submitted_batch < qd)
        stats->max_submitted_batch = qd;

    /* A short enter is still a failed batch, but its accepted prefix owns the
     * caller's buffers until every corresponding CQE is reaped.  Drain that
     * prefix without retrying or publishing the remainder, then fail closed. */
    for (i = 0; i < accepted; i++) {
        uint64_t token = 0;
        int32_t result = 0;
        unsigned j;
        int expected = backend == QDB_BACKEND_EXT4 ? QDB_BLOCK_SIZE : 0;
        int found = 0;

        for (;;) {
            rc = driver->reap(opaque, &token, &result);
            if (rc != -EAGAIN) break;
            rc = driver->enter(opaque, 0, 1, IORING_ENTER_GETEVENTS);
            if (rc < 0) break;
        }
        if (rc != 0) {
            stats->completion_errors++;
            return rc < 0 ? rc : -EIO;
        }
        stats->completed_commands++;
        stats->last_completion_token = token;
        stats->last_completion_result = result;
        for (j = 0; j < accepted; j++)
            if (items[j].token == token) found = 1;
        for (j = 0; j < nseen; j++)
            if (seen[j] == token) found = 0;
        if (!found || result != expected) {
            stats->completion_errors++;
            if (failure_rc == 0 && result < 0)
                failure_rc = result;
            bad = 1;
        } else {
            seen[nseen++] = token;
        }
    }
    if (failure_rc != 0) return failure_rc;
    return bad || incomplete ? -EIO : 0;
}

static int ring_stage(void *opaque, enum qdb_backend backend, unsigned slot,
                      const struct qdb_item *item)
{
    struct qdb_ring *ring = opaque;
    unsigned index, head;
    void *sqe;
    int rc;

    if (!ring || backend != ring->backend || slot != ring->staged ||
        slot >= QDB_RING_DEPTH)
        return -EINVAL;
    if (slot == 0) {
        ring->batch_tail = __atomic_load_n(ring->sq_tail, __ATOMIC_RELAXED);
        head = __atomic_load_n(ring->sq_head, __ATOMIC_ACQUIRE);
        if (ring->batch_tail - head > QDB_RING_DEPTH - 1u)
            return -EBUSY;
    }
    index = (ring->batch_tail + slot) & *ring->sq_mask;
    sqe = (unsigned char *)ring->sqes_map + index * ring->sqe_stride;
    if (backend == QDB_BACKEND_PASSTHROUGH)
        rc = qdb_encode_passthrough_sqe(sqe, ring->target_fd, ring->nsid,
                                        ring->logical_block_size, item);
    else
        rc = qdb_encode_ext4_sqe(sqe, ring->target_fd, item);
    if (rc != 0) return rc;
    ring->sq_array[(ring->batch_tail + slot) & *ring->sq_mask] = index;
    ring->staged++;
    return 0;
}

static int ring_publish(void *opaque, unsigned count)
{
    struct qdb_ring *ring = opaque;
    if (!ring || count == 0 || count != ring->staged) return -EINVAL;
    __atomic_store_n(ring->sq_tail, ring->batch_tail + count,
                     __ATOMIC_RELEASE);
    ring->staged = 0;
    return 0;
}

static int ring_enter(void *opaque, unsigned to_submit,
                      unsigned min_complete, unsigned flags)
{
    struct qdb_ring *ring = opaque;
    long rc;
    if (!ring) return -EINVAL;
    rc = syscall(__NR_io_uring_enter, ring->ring_fd, to_submit, min_complete,
                 flags, NULL, (size_t)0);
    return rc < 0 ? -errno : (int)rc;
}

static int ring_reap(void *opaque, uint64_t *token, int32_t *result)
{
    struct qdb_ring *ring = opaque;
    unsigned head, tail, index;
    const struct io_uring_cqe *cqe;

    if (!ring || !token || !result) return -EINVAL;
    head = __atomic_load_n(ring->cq_head, __ATOMIC_RELAXED);
    tail = __atomic_load_n(ring->cq_tail, __ATOMIC_ACQUIRE);
    if (head == tail) return -EAGAIN;
    index = head & *ring->cq_mask;
    cqe = (const struct io_uring_cqe *)
          ((const unsigned char *)ring->cqes +
           (size_t)index * ring->cqe_stride);
    *token = cqe->user_data;
    *result = cqe->res;
    __atomic_store_n(ring->cq_head, head + 1, __ATOMIC_RELEASE);
    return 0;
}

static const struct qdb_driver real_driver = {
    .stage = ring_stage,
    .publish = ring_publish,
    .enter = ring_enter,
    .reap = ring_reap,
};

static void ring_unmap(struct qdb_ring *ring)
{
    if (!ring) return;
    if (ring->sqes_map && ring->sqes_map != MAP_FAILED)
        munmap(ring->sqes_map, ring->sqes_map_len);
    if (ring->cq_map && ring->cq_map != MAP_FAILED &&
        ring->cq_map != ring->sq_map)
        munmap(ring->cq_map, ring->cq_map_len);
    if (ring->sq_map && ring->sq_map != MAP_FAILED)
        munmap(ring->sq_map, ring->sq_map_len);
    if (ring->ring_fd >= 0) close(ring->ring_fd);
}

int qdb_ring_open(struct qdb_ring **out, enum qdb_backend backend, int target_fd,
                  uint32_t nsid, uint32_t logical_block_size)
{
    struct io_uring_params params;
    struct qdb_ring *ring;
    unsigned char *sq, *cq;
    size_t sq_len, cq_len;
    int fd, saved;

    if (!out || target_fd < 0 ||
        (backend != QDB_BACKEND_EXT4 &&
         backend != QDB_BACKEND_PASSTHROUGH) ||
        logical_block_size == 0 ||
        QDB_BLOCK_SIZE % logical_block_size != 0 ||
        (backend == QDB_BACKEND_PASSTHROUGH && nsid == 0))
        return -EINVAL;
    *out = NULL;
    memset(&params, 0, sizeof params);
    params.flags = qdb_setup_flags(backend);
    fd = (int)syscall(__NR_io_uring_setup, QDB_RING_DEPTH, &params);
    if (fd < 0) return -errno;
    if (params.sq_entries < QDB_RING_DEPTH ||
        params.cq_entries < QDB_RING_DEPTH) {
        close(fd);
        return -EIO;
    }

    ring = calloc(1, sizeof *ring);
    if (!ring) {
        saved = errno;
        close(fd);
        return -saved;
    }
    ring->backend = backend;
    ring->target_fd = target_fd;
    ring->ring_fd = fd;
    ring->nsid = nsid;
    ring->logical_block_size = logical_block_size;
    ring->sqe_stride = backend == QDB_BACKEND_PASSTHROUGH
                     ? sizeof(struct qdb_sqe128)
                     : sizeof(struct io_uring_sqe);
    ring->cqe_stride = backend == QDB_BACKEND_PASSTHROUGH
                     ? sizeof(struct qdb_cqe32)
                     : sizeof(struct io_uring_cqe);

    sq_len = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    cq_len = params.cq_off.cqes + params.cq_entries * ring->cqe_stride;
    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        if (cq_len > sq_len) sq_len = cq_len;
        cq_len = sq_len;
    }
    ring->sq_map_len = sq_len;
    ring->cq_map_len = cq_len;
    ring->sqes_map_len = params.sq_entries * ring->sqe_stride;
    ring->sq_map = mmap(NULL, sq_len, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (ring->sq_map == MAP_FAILED) goto map_fail;
    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        ring->cq_map = ring->sq_map;
    } else {
        ring->cq_map = mmap(NULL, cq_len, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, fd,
                            IORING_OFF_CQ_RING);
        if (ring->cq_map == MAP_FAILED) goto map_fail;
    }
    ring->sqes_map = mmap(NULL, ring->sqes_map_len,
                          PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                          fd, IORING_OFF_SQES);
    if (ring->sqes_map == MAP_FAILED) goto map_fail;

    sq = ring->sq_map;
    cq = ring->cq_map;
    ring->sq_head = (unsigned *)(void *)(sq + params.sq_off.head);
    ring->sq_tail = (unsigned *)(void *)(sq + params.sq_off.tail);
    ring->sq_mask = (unsigned *)(void *)(sq + params.sq_off.ring_mask);
    ring->sq_array = (unsigned *)(void *)(sq + params.sq_off.array);
    ring->cq_head = (unsigned *)(void *)(cq + params.cq_off.head);
    ring->cq_tail = (unsigned *)(void *)(cq + params.cq_off.tail);
    ring->cq_mask = (unsigned *)(void *)(cq + params.cq_off.ring_mask);
    ring->cqes = cq + params.cq_off.cqes;
    *out = ring;
    return 0;

map_fail:
    saved = errno;
    ring_unmap(ring);
    free(ring);
    return -saved;
}

int qdb_ring_submit(struct qdb_ring *ring, const struct qdb_item *items,
                    unsigned qd, struct qdb_batch_stats *stats)
{
    if (!ring) return -EINVAL;
    return qdb_execute_batch(&real_driver, ring, ring->backend, items, qd,
                             stats);
}

void qdb_ring_close(struct qdb_ring *ring)
{
    if (!ring) return;
    ring_unmap(ring);
    free(ring);
}
