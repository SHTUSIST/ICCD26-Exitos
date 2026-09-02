/* Pure safety arithmetic shared by raw-device benchmarks.  Keeping it outside
 * the benchmark mains makes every LBA range and NVMe completion rule testable
 * without opening a device. */
#ifndef EXITOS_BENCHGUARD_H
#define EXITOS_BENCHGUARD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct exitos_bench_io_position {
    uint64_t lba;
    uint64_t blocks;
    uint64_t window_end;
    off_t byte_offset;
};

struct exitos_bench_byte_span {
    off_t offset;
    off_t length;
};

/* Resolve one member of a grouped benchmark request without unchecked
 * multiplication or addition.  The returned byte offset is proven to fit this
 * build's off_t ABI, and the complete request lies inside the certified native
 * LBA window.  Outputs are unchanged on failure. */
int exitos_bench_io_position(uint64_t window_start, uint64_t window_count,
                             uint64_t group, uint64_t group_width,
                             uint64_t member, size_t io_bytes, uint32_t lbs,
                             struct exitos_bench_io_position *out);

/* Checked byte range for ordinary file setup/read arms. */
int exitos_bench_byte_span(uint64_t index, uint64_t count, size_t item_bytes,
                           struct exitos_bench_byte_span *out);

/* Strict base-0 unsigned parser for safety-sensitive environment values.
 * Whitespace, signs, trailing bytes and overflow are all rejected. */
int exitos_bench_parse_u64(const char *text, uint64_t *out);

/* Pure benchmark-arm planner.  Read-only mode categorically disables every
 * arm that can submit a write, and `only` selects exactly one named arm. */
int exitos_bench_arm_enabled(const char *only, int read_only,
                             const char *arm, int arm_writes);

struct exitos_bench_extent {
    uint64_t logical; /* file byte offset */
    uint64_t lba;     /* first native device LBA */
    uint64_t len;     /* bytes, physically contiguous within this extent */
};

int exitos_bench_subwindow(uint64_t parent_start, uint64_t parent_count,
                           unsigned index, unsigned parts, uint64_t alignment,
                           uint64_t *start, uint64_t *count);

/* Translate the whole byte range only if it fits within one physical extent.
 * A range crossing even two logically adjacent extents is refused. */
int exitos_bench_extent_lba(const struct exitos_bench_extent *ext, size_t next,
                            uint64_t off, uint64_t len, uint32_t lbs,
                            uint64_t *lba);

/* Convert Linux sysfs partition-start units (always 512-byte sectors) to the
 * namespace's native logical blocks without overflowing start * 512. */
int exitos_bench_sectors_to_lba(uint64_t sectors, uint32_t lbs, uint64_t *lba);

/* Normalize NVME_IOCTL_IO_CMD's unusual result convention: negative means
 * errno, zero success, and positive is an NVMe status (therefore failure). */
int exitos_bench_nvme_status(int ioctl_result, int saved_errno);

#endif
