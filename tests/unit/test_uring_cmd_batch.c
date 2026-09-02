#include "tap.h"

#include <errno.h>
#include <linux/io_uring.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../tests/uring-passthrough-qd/uring_cmd_batch.h"

struct fake_ring {
    struct qdb_item staged[QDB_RING_DEPTH];
    uint64_t completion_token[QDB_RING_DEPTH];
    int32_t completion_result[QDB_RING_DEPTH];
    unsigned stage_calls;
    unsigned publish_calls;
    unsigned publish_count;
    unsigned enter_calls;
    unsigned enter_submit;
    unsigned enter_complete;
    unsigned enter_flags;
    unsigned reap_calls;
    unsigned reap_attempts;
    unsigned reap_eagain_once;
    int enter_result;
};

static int fake_stage(void *opaque, enum qdb_backend backend,
                      unsigned slot, const struct qdb_item *item)
{
    struct fake_ring *f = opaque;
    (void)backend;
    f->staged[slot] = *item;
    f->stage_calls++;
    return 0;
}

static int fake_publish(void *opaque, unsigned count)
{
    struct fake_ring *f = opaque;
    f->publish_calls++;
    f->publish_count = count;
    return 0;
}

static int fake_enter(void *opaque, unsigned to_submit,
                      unsigned min_complete, unsigned flags)
{
    struct fake_ring *f = opaque;
    f->enter_calls++;
    f->enter_submit = to_submit;
    f->enter_complete = min_complete;
    f->enter_flags = flags;
    return f->enter_result >= 0 ? f->enter_result : (int)to_submit;
}

static int fake_reap(void *opaque, uint64_t *token, int32_t *result)
{
    struct fake_ring *f = opaque;
    unsigned i;
    f->reap_attempts++;
    if (f->reap_eagain_once) {
        f->reap_eagain_once = 0;
        return -EAGAIN;
    }
    i = f->reap_calls++;
    *token = f->completion_token[i];
    *result = f->completion_result[i];
    return 0;
}

static const struct qdb_driver fake_driver = {
    .stage = fake_stage,
    .publish = fake_publish,
    .enter = fake_enter,
    .reap = fake_reap,
};

static void fill_items(struct qdb_item *items, unsigned qd,
                       unsigned char buffers[QDB_RING_DEPTH][QDB_BLOCK_SIZE])
{
    unsigned i;
    for (i = 0; i < qd; i++) {
        items[i].buffer = buffers[i];
        items[i].target = QDB_BLOCK_SIZE * (uint64_t)(i + 1);
        items[i].token = UINT64_C(0x100000000) + i + 1;
    }
}

static void prime_success(struct fake_ring *f, const struct qdb_item *items,
                          unsigned qd, enum qdb_backend backend)
{
    unsigned i;
    memset(f, 0, sizeof *f);
    f->enter_result = -1;
    for (i = 0; i < qd; i++) {
        f->completion_token[i] = items[i].token;
        f->completion_result[i] = backend == QDB_BACKEND_EXT4
                                ? QDB_BLOCK_SIZE : 0;
    }
}

static void test_supported_depths(void)
{
    static const unsigned qds[] = { 1, 2, 4, 8 };
    unsigned char (*buffers)[QDB_BLOCK_SIZE] = NULL;
    struct qdb_item items[QDB_RING_DEPTH];
    unsigned q;

    T_EQ(posix_memalign((void **)&buffers, QDB_BLOCK_SIZE,
                        sizeof(*buffers) * QDB_RING_DEPTH), 0,
         "allocate aligned test buffers");
    if (!buffers) return;
    fill_items(items, QDB_RING_DEPTH, buffers);

    for (q = 0; q < sizeof(qds) / sizeof(qds[0]); q++) {
        struct qdb_batch_stats stats = {0};
        struct fake_ring f;
        unsigned i, qd = qds[q];
        prime_success(&f, items, qd, QDB_BACKEND_PASSTHROUGH);

        T_EQ(qdb_execute_batch(&fake_driver, &f,
                               QDB_BACKEND_PASSTHROUGH,
                               items, qd, &stats), 0,
             "QD%u batch succeeds", qd);
        T_EQ(f.stage_calls, qd, "QD%u stages every command", qd);
        T_EQ(f.publish_calls, 1, "QD%u publishes SQ tail once", qd);
        T_EQ(f.publish_count, qd, "QD%u publishes all commands together", qd);
        T_EQ(f.enter_calls, 1, "QD%u makes one submit enter", qd);
        T_EQ(f.enter_submit, qd, "QD%u enter to_submit=N", qd);
        T_EQ(f.enter_complete, qd, "QD%u enter min_complete=N", qd);
        T_OK((f.enter_flags & IORING_ENTER_GETEVENTS) != 0,
             "QD%u enter requests completions", qd);
        T_EQ(f.reap_calls, qd, "QD%u reaps N completions", qd);
        T_EQ(stats.submit_calls, 1, "QD%u accounts one submit", qd);
        T_EQ(stats.submitted_commands, qd,
             "QD%u accounts N submissions", qd);
        T_EQ(stats.completed_commands, qd,
             "QD%u accounts N completions", qd);
        T_EQ(stats.max_submitted_batch, qd,
             "QD%u records real maximum batch", qd);
        for (i = 0; i < qd; i++) {
            T_OK(f.staged[i].buffer == buffers[i],
                 "QD%u slot %u retains its distinct buffer", qd, i);
            T_EQ(f.staged[i].target, items[i].target,
                 "QD%u slot %u retains its distinct target", qd, i);
            T_EQ(f.staged[i].token, items[i].token,
                 "QD%u slot %u retains its distinct token", qd, i);
        }
    }
    free(buffers);
}

static void test_encoders(void)
{
    unsigned char buffer[QDB_BLOCK_SIZE] __attribute__((aligned(QDB_BLOCK_SIZE)));
    struct qdb_item item = { buffer, UINT64_C(0x123456789), UINT64_C(0xabc) };
    struct qdb_sqe128 psqe;
    struct io_uring_sqe wsqe;
    const struct qdb_nvme_uring_cmd *cmd;

    T_EQ(qdb_setup_flags(QDB_BACKEND_PASSTHROUGH),
         IORING_SETUP_IOPOLL | IORING_SETUP_SQE128 | IORING_SETUP_CQE32,
         "passthrough ring requires IOPOLL+SQE128+CQE32");
    T_EQ(qdb_setup_flags(QDB_BACKEND_EXT4), IORING_SETUP_IOPOLL,
         "ext4 ring requires IOPOLL");
    T_EQ(qdb_encode_passthrough_sqe(&psqe, 17, 9, 512, &item), 0,
         "encode one 4KiB NVMe command");
    cmd = (const void *)psqe.cmd;
    T_EQ(psqe.opcode, 46, "passthrough SQE uses IORING_OP_URING_CMD");
    T_EQ(psqe.cmd_op, QDB_NVME_URING_CMD_IO,
         "passthrough SQE uses NVME_URING_CMD_IO");
    T_EQ(psqe.user_data, item.token, "passthrough SQE carries token");
    T_EQ(cmd->opcode, 0x01, "NVMe payload is WRITE");
    T_EQ(cmd->nsid, 9, "NVMe payload carries namespace ID");
    T_EQ(cmd->addr, (uintptr_t)buffer, "NVMe payload carries buffer");
    T_EQ(cmd->data_len, QDB_BLOCK_SIZE, "NVMe payload is exactly 4KiB");
    T_EQ(cmd->cdw10, 0x23456789u, "NVMe payload carries low SLBA bits");
    T_EQ(cmd->cdw11, 1, "NVMe payload carries high SLBA bits");
    T_EQ(cmd->cdw12 & 0xffffu, 7, "512B namespace encodes eight blocks");

    item.target = QDB_BLOCK_SIZE;
    T_EQ(qdb_encode_ext4_sqe(&wsqe, 23, &item), 0,
         "encode one ext4 direct write");
    T_EQ(wsqe.opcode, IORING_OP_WRITE, "ext4 SQE uses IORING_OP_WRITE");
    T_EQ(wsqe.fd, 23, "ext4 SQE carries file fd");
    T_EQ(wsqe.off, item.target, "ext4 SQE carries file offset");
    T_EQ(wsqe.addr, (uintptr_t)buffer, "ext4 SQE carries buffer");
    T_EQ(wsqe.len, QDB_BLOCK_SIZE, "ext4 SQE writes exactly 4KiB");
    T_EQ(wsqe.user_data, item.token, "ext4 SQE carries token");
}

static void test_hard_failures(void)
{
    unsigned char buffers[QDB_RING_DEPTH][QDB_BLOCK_SIZE]
        __attribute__((aligned(QDB_BLOCK_SIZE)));
    struct qdb_item items[QDB_RING_DEPTH];
    struct qdb_batch_stats stats;
    struct fake_ring f;
    int batch_rc;

    fill_items(items, QDB_RING_DEPTH, buffers);
    prime_success(&f, items, 2, QDB_BACKEND_PASSTHROUGH);
    f.enter_result = 1;
    memset(&stats, 0, sizeof stats);
    T_OK(qdb_execute_batch(&fake_driver, &f, QDB_BACKEND_PASSTHROUGH,
                           items, 2, &stats) < 0,
         "short submit fails without fallback");
    T_EQ(f.enter_calls, 1, "short submit is not retried or redirected");
    T_EQ(f.reap_calls, 1, "short submit drains the accepted prefix");
    T_EQ(stats.completed_commands, 1,
         "short submit accounts the drained accepted completion");
    T_EQ(stats.max_submitted_batch, 0,
         "short submit does not claim a complete batch");

    prime_success(&f, items, 2, QDB_BACKEND_EXT4);
    f.completion_result[1] = -EOPNOTSUPP;
    memset(&stats, 0, sizeof stats);
    batch_rc = qdb_execute_batch(&fake_driver, &f, QDB_BACKEND_EXT4,
                                 items, 2, &stats);
    T_EQ(batch_rc, -EOPNOTSUPP,
         "bad CQE preserves its kernel error without fallback");
    T_EQ(stats.completion_errors, 1, "bad CQE is accounted");
    T_EQ(f.enter_calls, 1, "bad CQE is not resubmitted elsewhere");

    prime_success(&f, items, 2, QDB_BACKEND_PASSTHROUGH);
    f.completion_token[1] = f.completion_token[0];
    memset(&stats, 0, sizeof stats);
    T_OK(qdb_execute_batch(&fake_driver, &f, QDB_BACKEND_PASSTHROUGH,
                           items, 2, &stats) < 0,
         "duplicate completion token fails");
    T_EQ(stats.completion_errors, 1,
         "duplicate completion token is accounted");

    prime_success(&f, items, 2, QDB_BACKEND_PASSTHROUGH);
    items[1].buffer = items[0].buffer;
    memset(&stats, 0, sizeof stats);
    T_EQ(qdb_execute_batch(&fake_driver, &f, QDB_BACKEND_PASSTHROUGH,
                           items, 2, &stats), -EINVAL,
         "duplicate buffer is rejected before publication");
    T_EQ(f.stage_calls, 0, "invalid batch stages nothing");
    T_EQ(f.publish_calls, 0, "invalid batch publishes nothing");
}

static void test_completion_visibility_wait(void)
{
    unsigned char buffers[QDB_RING_DEPTH][QDB_BLOCK_SIZE]
        __attribute__((aligned(QDB_BLOCK_SIZE)));
    struct qdb_item items[QDB_RING_DEPTH];
    struct qdb_batch_stats stats = {0};
    struct fake_ring f;

    fill_items(items, 1, buffers);
    prime_success(&f, items, 1, QDB_BACKEND_EXT4);
    f.reap_eagain_once = 1;
    T_EQ(qdb_execute_batch(&fake_driver, &f, QDB_BACKEND_EXT4,
                           items, 1, &stats), 0,
         "temporarily invisible CQE is waited for, not treated as failure");
    T_EQ(f.enter_calls, 2,
         "empty CQ triggers one completion-only io_uring_enter");
    T_EQ(f.reap_attempts, 2, "CQE is rechecked after the completion wait");
    T_EQ(f.reap_calls, 1, "visible completion is consumed exactly once");
}

int main(void)
{
    T_OK(qdb_qd_supported(1), "QD1 supported");
    T_OK(qdb_qd_supported(2), "QD2 supported");
    T_OK(qdb_qd_supported(4), "QD4 supported");
    T_OK(qdb_qd_supported(8), "QD8 supported");
    T_OK(!qdb_qd_supported(0), "QD0 rejected");
    T_OK(!qdb_qd_supported(3), "QD3 rejected");
    T_OK(!qdb_qd_supported(16), "QD16 rejected");
    test_supported_depths();
    test_encoders();
    test_hard_failures();
    test_completion_visibility_wait();
    T_DONE();
}
