#ifndef EXITOS_EXAMPLE_URING_CMD_BATCH_H
#define EXITOS_EXAMPLE_URING_CMD_BATCH_H

#include <linux/io_uring.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define QDB_BLOCK_SIZE 4096u
#define QDB_RING_DEPTH 8u
#ifndef IORING_SETUP_SQE128
#define IORING_SETUP_SQE128 (1u << 10)
#endif
#ifndef IORING_SETUP_CQE32
#define IORING_SETUP_CQE32 (1u << 11)
#endif

enum qdb_backend {
    QDB_BACKEND_EXT4 = 0,
    QDB_BACKEND_PASSTHROUGH = 1,
};

struct qdb_nvme_uring_cmd {
    uint8_t opcode, flags;
    uint16_t rsvd1;
    uint32_t nsid, cdw2, cdw3;
    uint64_t metadata, addr;
    uint32_t metadata_len, data_len;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
    uint32_t timeout_ms, rsvd2;
};

#define QDB_NVME_URING_CMD_IO \
    _IOWR('N', 0x80, struct qdb_nvme_uring_cmd)

struct qdb_sqe128 {
    uint8_t opcode, flags;
    uint16_t ioprio;
    int32_t fd;
    uint32_t cmd_op, pad1;
    uint64_t addr;
    uint32_t len, rw_flags;
    uint64_t user_data;
    uint16_t buf_index, personality;
    int32_t splice_fd_in;
    uint8_t cmd[80];
};

struct qdb_item {
    void *buffer;
    uint64_t target; /* ext4 byte offset or absolute namespace LBA */
    uint64_t token;
};

struct qdb_batch_stats {
    uint64_t submit_calls;
    uint64_t submitted_commands;
    uint64_t completed_commands;
    uint64_t completion_errors;
    uint64_t last_completion_token;
    int32_t last_completion_result;
    unsigned max_submitted_batch;
};

struct qdb_driver {
    int (*stage)(void *opaque, enum qdb_backend backend, unsigned slot,
                 const struct qdb_item *item);
    int (*publish)(void *opaque, unsigned count);
    int (*enter)(void *opaque, unsigned to_submit, unsigned min_complete,
                 unsigned flags);
    int (*reap)(void *opaque, uint64_t *token, int32_t *result);
};

int qdb_qd_supported(unsigned qd);
unsigned qdb_setup_flags(enum qdb_backend backend);
int qdb_encode_passthrough_sqe(struct qdb_sqe128 *sqe, int fd,
                               uint32_t nsid, uint32_t logical_block_size,
                               const struct qdb_item *item);
int qdb_encode_ext4_sqe(struct io_uring_sqe *sqe, int fd,
                        const struct qdb_item *item);
int qdb_execute_batch(const struct qdb_driver *driver, void *opaque,
                      enum qdb_backend backend, const struct qdb_item *items,
                      unsigned qd, struct qdb_batch_stats *stats);

struct qdb_ring;
int qdb_ring_open(struct qdb_ring **out, enum qdb_backend backend, int target_fd,
                  uint32_t nsid, uint32_t logical_block_size);
int qdb_ring_submit(struct qdb_ring *ring, const struct qdb_item *items,
                    unsigned qd, struct qdb_batch_stats *stats);
void qdb_ring_close(struct qdb_ring *ring);

#endif
