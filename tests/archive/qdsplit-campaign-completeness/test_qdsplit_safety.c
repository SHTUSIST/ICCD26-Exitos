/* Exercise qdsplit's real startup ordering with every storage-facing call
 * replaced by an in-process fake.  These tests must never open a device. */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/io_uring.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>

#include "tap.h"
#include "exitos_devguard.h"
#include "exitos_durability.h"
#include "exitos_iopath.h"

enum {
    MOCK_WHOLE_FD = 40,
    MOCK_CHAR_FD = 41,
    MOCK_PART_FD = 42,
    MOCK_FILE_FD = 43,
    MOCK_APPEND_FD = 44,
    MOCK_DIR_FD = 45,
    MOCK_PART_RW_FD = 47,
};

static unsigned g_write_submits;
static unsigned g_uring_submits;
static unsigned g_ioctl_submits;
static unsigned g_path_guard_calls;
static unsigned g_pwrite_calls;
static unsigned g_unlink_calls;
static unsigned g_append_unlink_calls;
static unsigned g_fiemap_calls;
static unsigned g_zero_calls;
static unsigned g_window_fd_calls;
static dev_t g_file_dev;
static dev_t g_part_rdev;
static dev_t g_part_rw_rdev;
static nlink_t g_file_nlink;
static unsigned g_open_file_flags;
static unsigned g_open_append_flags;
static unsigned g_fail_pwrite_call;
static unsigned g_short_pwrite_call;
static int g_file_preexists;
static int g_append_preexists;
static int g_poll_available;
static int g_ioctl_status;
static int g_cqe_result_override;
static int g_short_submit;
static int g_cross_extent;
static int g_extent_physical_zero;
static int g_zero_result;
static int g_after_short;
static int g_fail_fallocate;
static int g_fail_fsync;
static uint32_t g_fiemap_flags;
static unsigned g_fail_window_call;
static unsigned g_cross_boundary;
static unsigned g_setup_calls;
static unsigned g_fail_setup_call;
static unsigned g_open_whole_calls;
static unsigned g_open_char_calls;
static unsigned g_open_part_calls;
static unsigned g_open_part_rw_calls;
static unsigned g_open_file_calls;
static unsigned g_open_append_calls;
static unsigned g_open_parent_calls;
static unsigned g_open_whole_flags;
static unsigned g_open_char_flags;
static unsigned g_open_part_flags;
static unsigned g_open_part_rw_flags;
static unsigned g_open_parent_flags;
static unsigned g_tmpfile_calls;
static int g_fail_tmpfile;
static off_t g_fallocate_len;
static size_t g_first_pwrite_len;
static off_t g_first_pwrite_off;
static unsigned g_fiemap_sync_seen;
static unsigned g_sleep_calls;
static unsigned g_pagemap_reads;
static int g_pagemap_mode;
static void *g_mock_stage_region;
static uint64_t g_last_pt_addr;
static unsigned g_register_calls;
static int g_register_bad;
static int g_fail_register;
static void *g_register_base[2];
static size_t g_register_len[2];
static unsigned g_register_nr;
static unsigned g_fixed_submits;
static unsigned g_bad_fixed;
static unsigned g_last_fixed_buf_index;
static uint64_t g_last_window_lba;
static uint64_t g_last_window_count;
static unsigned g_saw_derived_8k_window;
static uint64_t g_last_zero_lba;
static uint64_t g_last_zero_count;
static unsigned g_bad_guard_args;
static unsigned g_bad_command_encoding;
static unsigned g_decoded_raw_commands;
static unsigned g_raw_block_pwrite_calls;
static unsigned g_bad_raw_block_write;
static int g_short_raw_block_write;
static off_t g_last_raw_block_off;
static size_t g_last_raw_block_len;
static unsigned g_fiemap_too_many;
static unsigned g_fiemap_length_overflow;
static unsigned g_short_submit_delayed;
static unsigned g_eintr_consumed_delayed;
static unsigned g_signal_submit_delayed;
static unsigned g_delayed_pending;
static unsigned g_delayed_wait_errors;
static unsigned g_delayed_completion_seq;
static unsigned g_file_close_seq;
static unsigned g_ring_close_seq;
static unsigned g_event_seq;
static uint32_t g_mock_lbs;
static uint32_t g_part_rw_lbs;
static unsigned g_file_readback_calls;
static unsigned g_raw_readback_calls;
static unsigned g_verify_success_lines;
static unsigned g_verify_selected_zero_lines;
static unsigned g_verify_selected_all_lines;
static size_t g_preflight_entries;
static unsigned g_durability_proof_lines;
static char g_timing_contract_line[512];
static unsigned g_arm12_frozen_lines;
static unsigned g_default_all_lines;
static unsigned g_table_header_lines;
static unsigned g_arm_result_lines;
static unsigned g_idle_report_lines;
static unsigned g_stdout_calls;
static int g_swap_equal_slots;
static int g_duplicate_equal_slots;
static unsigned g_fail_file_read_call;
static unsigned g_short_file_read_call;
static unsigned g_fail_raw_read_call;
static unsigned g_short_raw_read_call;
static uint64_t g_corrupt_file_logical;
static uint64_t g_corrupt_raw_logical;
static unsigned g_clock_calls;
static unsigned g_first_read_clock_calls;
static unsigned g_readback_while_inflight;
static unsigned g_iopath_open_calls;
static unsigned g_iopath_exclusive_calls;
static unsigned g_iopath_write_calls;
static unsigned g_iopath_close_calls;
static unsigned g_iopath_pt_close_calls;
static unsigned g_iopath_block_close_calls;
static unsigned g_fail_iopath_open_call;
static unsigned g_fail_iopath_exclusive_call;
static unsigned g_fail_iopath_write_call;
static unsigned g_short_iopath_write_call;
static unsigned g_signal_iopath_write_call;
static unsigned g_bad_iopath_args;
static int g_iopath_open_fd_arg[2];
static iopath_backend g_iopath_open_backend[2];
static uint32_t g_iopath_open_lbs[2];
static struct iopath *g_iopath_write_handle[64];
static uint64_t g_iopath_write_lba[64];
static const void *g_iopath_write_buf[64];
static size_t g_iopath_write_len[64];
static unsigned g_iopath_pt_writes;
static unsigned g_iopath_block_writes;
static unsigned g_close_whole_calls;
static unsigned g_close_part_calls;
static unsigned g_close_part_rw_calls;
static unsigned g_close_char_calls;
static unsigned g_close_file_calls;
static unsigned g_close_dir_calls;
static unsigned g_lifecycle_seq;
static unsigned g_first_init_write_seq;
static unsigned g_first_exclusive_seq;
static unsigned g_last_exclusive_seq;
static unsigned g_first_handle_close_seq;
static unsigned g_last_handle_close_seq;
static unsigned g_first_retained_close_seq;
static unsigned g_file_teardown_seq;
static unsigned g_buffer_teardown_seq;
static unsigned g_handle_close_with_caller_open;
static unsigned g_durability_probe_calls;
static unsigned g_durability_policy_calls;
static unsigned g_durability_resolve_calls;
static unsigned g_fail_durability_resolve_call;
static unsigned g_durability_resolve_major[2];
static unsigned g_durability_resolve_minor[2];
static int g_durability_resolve_is_partition[2];
static unsigned g_durability_diskseq_calls;
static unsigned g_fail_durability_diskseq_call;
static uint64_t g_durability_diskseq[2];
static unsigned g_durability_poll_bind_calls;
static uint64_t g_durability_sysfs_diskseq[2];
static unsigned g_durability_sysfs_readlink_calls;
static unsigned g_fail_durability_sysfs_readlink_call;
static char g_durability_sysfs_link[2][256];
static int g_durability_probe_rc;
static int g_durability_probe_vol;
static int g_durability_probe_fua;
static int g_durability_policy_vol;
static int g_durability_policy_fua;
static exitos_durability g_durability_policy_result;
static char g_durability_probe_path[PATH_MAX];

static int mock_any_ring_inflight(void);

/* A byte-exact model of the anonymous primary inode and its FIEMAP-backed
 * whole-device view.  It is deliberately independent of qdsplit's oracle: the
 * fake writes capture what each timed path submitted, while fake direct reads
 * expose those same physical bytes (or an injected equal-length slot swap). */
#define MOCK_BACKING_BYTES (1u << 20)
static unsigned char g_backing[MOCK_BACKING_BYTES];
static unsigned char g_file_read_covered[MOCK_BACKING_BYTES];
static unsigned char g_raw_read_covered[MOCK_BACKING_BYTES];
static off_t g_written_slot_off[2];
static size_t g_written_slot_len[2];
static unsigned g_written_slot_count;

enum mock_io_event_kind {
    MOCK_EVENT_FS_WRITE = 1,
    MOCK_EVENT_ZERO,
    MOCK_EVENT_BLOCK_WRITE,
    MOCK_EVENT_PT_SUBMIT,
    MOCK_EVENT_PROD_PT_WRITE,
    MOCK_EVENT_PROD_BLOCK_WRITE,
};

struct mock_io_event {
    unsigned kind;
    int fd;
    uint64_t lba;
    uint64_t count;
    off_t off;
    size_t len;
    unsigned fua;
};

#define MAX_IO_EVENTS 128u
static struct mock_io_event g_io_events[MAX_IO_EVENTS];
static unsigned g_io_event_count;
static unsigned g_io_event_overflow;

struct signal_report {
    int returned;
    int qdsplit_rc;
    unsigned masked_at_raise;
    unsigned raise_seq;
    unsigned completion_seq;
    unsigned ring_close_seq;
    unsigned file_close_seq;
};
static struct signal_report *g_signal_report;

/* Declared before qdsplit's mmap/munmap interposition so forked signal tests
 * can report ordering through genuinely shared anonymous memory. */
static void *real_shared_alloc(size_t len)
{
    return mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

static void real_shared_free(void *p, size_t len)
{
    (void)munmap(p, len);
}

struct mock_ring_map {
    int fd;
    unsigned char *sq;
    unsigned char *cq;
    unsigned char *sqes;
};
struct mock_sqe128 {
    uint8_t opcode, flags; uint16_t ioprio; int32_t fd; uint32_t cmd_op, pad1;
    uint64_t addr; uint32_t len, rw_flags; uint64_t user_data;
    uint16_t buf_index, personality; int32_t splice_fd_in; uint8_t cmd[80];
};
struct mock_cqe32 {
    uint64_t user_data; int32_t res; uint32_t flags; uint64_t big[2];
};
struct mock_nvme_uring_cmd {
    uint8_t opcode, flags; uint16_t rsvd1; uint32_t nsid, cdw2, cdw3;
    uint64_t metadata, addr; uint32_t metadata_len, data_len;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15, timeout_ms, rsvd2;
};
static struct mock_ring_map g_rings[2];
static unsigned g_nrings;
static int g_delayed_ringfd;
static uint64_t g_delayed_user_data;
static int32_t g_delayed_result;

static void record_io_event(unsigned kind, int fd, uint64_t lba,
                            uint64_t count, off_t off, size_t len,
                            unsigned fua)
{
    struct mock_io_event *e;
    if (g_io_event_count >= MAX_IO_EVENTS) {
        g_io_event_overflow++;
        return;
    }
    e = &g_io_events[g_io_event_count++];
    e->kind = kind;
    e->fd = fd;
    e->lba = lba;
    e->count = count;
    e->off = off;
    e->len = len;
    e->fua = fua;
}

static int mock_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/mock/whole")) {
        g_open_whole_calls++;
        g_open_whole_flags = (unsigned)flags;
        return MOCK_WHOLE_FD;
    }
    if (!strcmp(path, "/mock/ng")) {
        g_open_char_calls++;
        g_open_char_flags = (unsigned)flags;
        return MOCK_CHAR_FD;
    }
    if (!strcmp(path, "/mock/part")) {
        if ((flags & O_ACCMODE) == O_RDWR) {
            g_open_part_rw_calls++;
            g_open_part_rw_flags = (unsigned)flags;
            return MOCK_PART_RW_FD;
        }
        g_open_part_calls++;
        g_open_part_flags = (unsigned)flags;
        return MOCK_PART_FD;
    }
    if (!strcmp(path, "/mock")) {
        g_open_parent_calls++;
        g_open_parent_flags = (unsigned)flags;
        return MOCK_DIR_FD;
    }
    if (!strcmp(path, "/mock/file")) {
        g_open_file_calls++;
        g_open_file_flags = (unsigned)flags;
        if (g_file_preexists && (flags & O_EXCL)) { errno = EEXIST; return -1; }
        return MOCK_FILE_FD;
    }
    if (!strcmp(path, "/mock/file.append")) {
        g_open_append_calls++;
        g_open_append_flags = (unsigned)flags;
        if (g_append_preexists && (flags & O_EXCL)) { errno = EEXIST; return -1; }
        return MOCK_APPEND_FD;
    }
    if (!strcmp(path, "/proc/self/pagemap")) {
        if (g_pagemap_mode) return 46;
        errno = EACCES;
        return -1;
    }
    errno = ENOENT;
    return -1;
}

static __attribute__((unused)) int
mock_openat(int dirfd, const char *path, int flags, ...)
{
    if (dirfd != MOCK_DIR_FD || strcmp(path, ".") != 0 ||
        (flags & O_TMPFILE) != O_TMPFILE || !(flags & O_EXCL) ||
        !(flags & O_DIRECT) || !(flags & O_CLOEXEC) ||
        !(flags & O_NOFOLLOW) || (flags & O_ACCMODE) != O_RDWR)
        g_bad_guard_args++;
    g_tmpfile_calls++;
    if (g_fail_tmpfile) { errno = EOPNOTSUPP; return -1; }
    if (g_tmpfile_calls == 1) {
        g_open_file_flags = (unsigned)flags;
        return MOCK_FILE_FD;
    }
    g_open_append_flags = (unsigned)flags;
    return MOCK_APPEND_FD;
}

static int mock_close(int fd)
{
    g_event_seq++;
    g_lifecycle_seq++;
    if (fd == MOCK_WHOLE_FD) g_close_whole_calls++;
    if (fd == MOCK_PART_FD) g_close_part_calls++;
    if (fd == MOCK_PART_RW_FD) g_close_part_rw_calls++;
    if (fd == MOCK_CHAR_FD) g_close_char_calls++;
    if (fd == MOCK_FILE_FD) g_close_file_calls++;
    if (fd == MOCK_DIR_FD) g_close_dir_calls++;
    if ((fd == MOCK_WHOLE_FD || fd == MOCK_PART_FD ||
         fd == MOCK_PART_RW_FD || fd == MOCK_CHAR_FD) &&
        g_first_retained_close_seq == 0)
        g_first_retained_close_seq = g_lifecycle_seq;
    if (fd == MOCK_FILE_FD) {
        g_file_teardown_seq = g_lifecycle_seq;
        g_file_close_seq = g_event_seq;
        if (g_signal_report) g_signal_report->file_close_seq = g_event_seq;
    }
    if (fd >= 90) {
        g_ring_close_seq = g_event_seq;
        if (g_signal_report) g_signal_report->ring_close_seq = g_event_seq;
    }
    return 0;
}
static __attribute__((unused)) int mock_unlink(const char *path)
{
    g_unlink_calls++;
    if (!strcmp(path, "/mock/file.append")) g_append_unlink_calls++;
    return 0;
}
static int mock_fallocate(int fd, int mode, off_t off, off_t len)
{
    (void)fd; (void)mode; (void)off; (void)len;
    g_fallocate_len = len;
    if (g_fail_fallocate) { errno = ENOSPC; return -1; }
    if (off != 0 || len <= 0 || (uint64_t)len > MOCK_BACKING_BYTES) {
        errno = EFBIG;
        return -1;
    }
    memset(g_backing, 0, sizeof g_backing);
    return 0;
}
static int mock_fsync(int fd)
{
    (void)fd;
    if (g_fail_fsync) { errno = EIO; return -1; }
    return 0;
}
static int mock_fdatasync(int fd) { (void)fd; return 0; }
static unsigned mock_sleep(unsigned seconds)
{ (void)seconds; g_sleep_calls++; return 0; }

static int mock_clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id != CLOCK_MONOTONIC || !ts) {
        errno = EINVAL;
        return -1;
    }
    g_clock_calls++;
    ts->tv_sec = 0;
    ts->tv_nsec = (long)g_clock_calls * 1000;
    return 0;
}

static int mock_buffer_is_zero(const unsigned char *p, size_t len)
{
    size_t i;
    if (!p) return 0;
    for (i = 0; i < len; i++) if (p[i] != 0) return 0;
    return 1;
}

static void mock_record_written_slot(off_t off, size_t len)
{
    unsigned i;
    if (off < 0 || !len) return;
    for (i = 0; i < g_written_slot_count; i++)
        if (g_written_slot_off[i] == off && g_written_slot_len[i] == len)
            return;
    if (g_written_slot_count < 2) {
        g_written_slot_off[g_written_slot_count] = off;
        g_written_slot_len[g_written_slot_count] = len;
        g_written_slot_count++;
    }
}

static int mock_raw_byte_to_logical(uint64_t raw_byte, uint64_t *logical)
{
    const uint64_t partition_byte = UINT64_C(800) * 512;
    const uint64_t first_physical = UINT64_C(100) * 4096;
    uint64_t first_len = g_cross_extent
                       ? (g_cross_boundary ? g_cross_boundary : 4096)
                       : (UINT64_C(32) << 20);
    uint64_t start = partition_byte + first_physical;

    if (raw_byte >= start && raw_byte - start < first_len) {
        *logical = raw_byte - start;
        return 0;
    }
    if (g_cross_extent) {
        const uint64_t second_physical = UINT64_C(900) * 4096;
        uint64_t second_len = (UINT64_C(32) << 20) - first_len;
        start = partition_byte + second_physical;
        if (raw_byte >= start && raw_byte - start < second_len) {
            *logical = first_len + raw_byte - start;
            return 0;
        }
    }
    return -1;
}

static uint64_t mock_read_logical_index(uint64_t logical)
{
    if (g_duplicate_equal_slots && g_written_slot_count == 2 &&
        g_written_slot_len[0] == g_written_slot_len[1]) {
        uint64_t a = (uint64_t)g_written_slot_off[0];
        uint64_t b = (uint64_t)g_written_slot_off[1];
        uint64_t n = g_written_slot_len[0];
        if (logical >= b && logical - b < n) return a + logical - b;
    }
    if (g_swap_equal_slots && g_written_slot_count == 2 &&
        g_written_slot_len[0] == g_written_slot_len[1]) {
        uint64_t a = (uint64_t)g_written_slot_off[0];
        uint64_t b = (uint64_t)g_written_slot_off[1];
        uint64_t n = g_written_slot_len[0];
        if (logical >= a && logical - a < n) return b + logical - a;
        if (logical >= b && logical - b < n) return a + logical - b;
    }
    return logical;
}

static int mock_store_logical(off_t off, const void *buf, size_t len,
                              int timed)
{
    if (off < 0 || (uint64_t)off > MOCK_BACKING_BYTES ||
        len > MOCK_BACKING_BYTES - (size_t)off)
        return -1;
    memcpy(g_backing + (size_t)off, buf, len);
    if (timed) mock_record_written_slot(off, len);
    return 0;
}

static int mock_store_raw(off_t off, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    size_t i;
    if (off < 0 || !p) return -1;
    for (i = 0; i < len; i++) {
        uint64_t logical;
        if (mock_raw_byte_to_logical((uint64_t)off + i, &logical) != 0 ||
            logical >= MOCK_BACKING_BYTES)
            return -1;
        g_backing[logical] = p[i];
    }
    return 0;
}

static ssize_t mock_pwrite(int fd, const void *buf, size_t len, off_t off)
{
    const unsigned char *payload = buf;
    g_lifecycle_seq++;
    if (g_first_init_write_seq == 0)
        g_first_init_write_seq = g_lifecycle_seq;
    g_write_submits++;
    g_pwrite_calls++;
    if (g_pwrite_calls == 1) {
        g_first_pwrite_len = len;
        g_first_pwrite_off = off;
    }
    if (fd == MOCK_FILE_FD && payload && len &&
        !mock_buffer_is_zero(payload, len))
        record_io_event(MOCK_EVENT_FS_WRITE, fd, 0, 0, off, len, UINT_MAX);
    if (fd == MOCK_WHOLE_FD) {
        uint64_t lba = 0, blocks = 0, end = 0, zero_end = 0;

        g_raw_block_pwrite_calls++;
        g_last_raw_block_off = off;
        g_last_raw_block_len = len;
        if (off < 0 || !g_mock_lbs || (uint64_t)off % g_mock_lbs ||
            !len || len % g_mock_lbs) {
            g_bad_raw_block_write++;
        } else {
            lba = (uint64_t)off / g_mock_lbs;
            blocks = (uint64_t)len / g_mock_lbs;
            record_io_event(MOCK_EVENT_BLOCK_WRITE, fd, lba, blocks,
                            off, len, UINT_MAX);
            end = lba + blocks;
            zero_end = g_last_zero_lba + g_last_zero_count;
            if (!g_last_zero_count || end < lba ||
                zero_end < g_last_zero_lba || lba < g_last_zero_lba ||
                end > zero_end || !payload)
                g_bad_raw_block_write++;
        }
        if (g_short_raw_block_write)
            return len ? (ssize_t)len - 1 : 0;
    }
    if (g_pwrite_calls == g_fail_pwrite_call) {
        errno = EIO;
        return -1;
    }
    if (g_pwrite_calls == g_short_pwrite_call)
        return len ? (ssize_t)len - 1 : 0;
    if (fd == MOCK_FILE_FD) {
        int timed = !mock_buffer_is_zero(payload, len);
        if (mock_store_logical(off, buf, len, timed) != 0) {
            errno = EFBIG;
            return -1;
        }
    } else if (fd == MOCK_WHOLE_FD && mock_store_raw(off, buf, len) != 0) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)len;
}

static ssize_t mock_pread(int fd, void *buf, size_t len, off_t off)
{
    uint64_t pfn;
    if (fd == 46 && len == sizeof(uint64_t) && g_pagemap_mode) {
        if (g_pagemap_mode == 1)
            pfn = UINT64_C(100) + g_pagemap_reads;
        else if (g_pagemap_mode == 2)
            pfn = UINT64_C(100) + 2 * g_pagemap_reads;
        else
            pfn = UINT64_C(100) + g_pagemap_reads +
                  (g_pagemap_reads == 7 ? UINT64_C(1000) : 0);
        *(uint64_t *)buf = (UINT64_C(1) << 63) | pfn;
        g_pagemap_reads++;
        return (ssize_t)len;
    }
    if (fd == MOCK_FILE_FD) {
        unsigned char *out = buf;
        size_t i;
        if (off < 0 || (uint64_t)off > MOCK_BACKING_BYTES ||
            len > MOCK_BACKING_BYTES - (size_t)off) {
            errno = EIO;
            return -1;
        }
        g_file_readback_calls++;
        if (g_first_read_clock_calls == UINT_MAX)
            g_first_read_clock_calls = g_clock_calls;
        if (mock_any_ring_inflight()) g_readback_while_inflight++;
        if (g_file_readback_calls == g_fail_file_read_call) {
            errno = EIO;
            return -1;
        }
        for (i = 0; i < len; i++) {
            uint64_t logical = (uint64_t)off + i;
            uint64_t source = mock_read_logical_index(logical);
            out[i] = g_backing[source];
            if (logical == g_corrupt_file_logical) out[i] ^= 1u;
            g_file_read_covered[logical] = 1;
        }
        if (g_file_readback_calls == g_short_file_read_call)
            return len ? (ssize_t)len - 1 : 0;
        return (ssize_t)len;
    }
    if (fd == MOCK_WHOLE_FD) {
        unsigned char *out = buf;
        size_t i;
        if (off < 0) {
            errno = EIO;
            return -1;
        }
        g_raw_readback_calls++;
        if (g_first_read_clock_calls == UINT_MAX)
            g_first_read_clock_calls = g_clock_calls;
        if (mock_any_ring_inflight()) g_readback_while_inflight++;
        if (g_raw_readback_calls == g_fail_raw_read_call) {
            errno = EIO;
            return -1;
        }
        for (i = 0; i < len; i++) {
            uint64_t logical, source;
            if (mock_raw_byte_to_logical((uint64_t)off + i, &logical) != 0 ||
                logical >= MOCK_BACKING_BYTES) {
                errno = EIO;
                return -1;
            }
            source = mock_read_logical_index(logical);
            out[i] = g_backing[source];
            if (logical == g_corrupt_raw_logical) out[i] ^= 1u;
            g_raw_read_covered[logical] = 1;
        }
        if (g_raw_readback_calls == g_short_raw_read_call)
            return len ? (ssize_t)len - 1 : 0;
        return (ssize_t)len;
    }
    errno = EIO;
    return -1;
}

static int mock_fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof *st);
    switch (fd) {
    case MOCK_WHOLE_FD:
        st->st_mode = S_IFBLK | 0600;
        st->st_rdev = makedev(259, 0);
        return 0;
    case MOCK_PART_FD:
        st->st_mode = S_IFBLK | 0600;
        st->st_rdev = g_part_rdev;
        return 0;
    case MOCK_PART_RW_FD:
        st->st_mode = S_IFBLK | 0600;
        st->st_rdev = g_part_rw_rdev;
        return 0;
    case MOCK_CHAR_FD:
        st->st_mode = S_IFCHR | 0600;
        st->st_rdev = makedev(240, 0);
        return 0;
    case MOCK_FILE_FD:
    case MOCK_APPEND_FD:
        st->st_mode = S_IFREG | 0600;
        st->st_dev = g_file_dev;
        st->st_nlink = g_file_nlink;
        return 0;
    default:
        errno = EBADF;
        return -1;
    }
}

static __attribute__((unused)) ssize_t
mock_readlink(const char *path, char *buf, size_t bufsiz)
{
    unsigned call = g_durability_sysfs_readlink_calls++;
    size_t len;

    if (!path || strcmp(path, "/sys/dev/block/259:0") != 0 ||
        g_fail_durability_sysfs_readlink_call == call + 1) {
        errno = ENOENT;
        return -1;
    }
    len = strlen(g_durability_sysfs_link[call == 0 ? 0 : 1]);
    if (len > bufsiz) len = bufsiz;
    memcpy(buf, g_durability_sysfs_link[call == 0 ? 0 : 1], len);
    return (ssize_t)len;
}

static void mock_validate_raw_sqe(const struct mock_sqe128 *sqe)
{
    const struct mock_nvme_uring_cmd *c;
    const unsigned char *payload;
    uint64_t lba, blocks, end, zero_end;

    if (sqe->opcode != 46u) return;
    c = (const struct mock_nvme_uring_cmd *)(const void *)sqe->cmd;
    lba = (uint64_t)c->cdw10 | ((uint64_t)c->cdw11 << 32);
    blocks = (uint64_t)(c->cdw12 & UINT32_C(0xffff)) + 1;
    end = lba + blocks;
    zero_end = g_last_zero_lba + g_last_zero_count;
    payload = (const unsigned char *)(uintptr_t)c->addr;
    g_decoded_raw_commands++;
    g_last_pt_addr = c->addr;
    if (sqe->rw_flags & 1u) {
        g_fixed_submits++;
        g_last_fixed_buf_index = sqe->buf_index;
        if (sqe->buf_index >= g_register_nr ||
            c->addr < (uint64_t)(uintptr_t)g_register_base[sqe->buf_index] ||
            c->addr + c->data_len >
                (uint64_t)(uintptr_t)g_register_base[sqe->buf_index] +
                g_register_len[sqe->buf_index])
            g_bad_fixed++;
    }
    record_io_event(MOCK_EVENT_PT_SUBMIT, sqe->fd, lba, blocks, 0,
                    c->data_len, (c->cdw12 >> 30) & 1u);
    if (sqe->fd != MOCK_CHAR_FD || c->opcode != 0x01 || c->nsid != 7 ||
        !c->data_len || c->data_len != blocks * g_mock_lbs ||
        !g_last_zero_count || end < lba || zero_end < g_last_zero_lba ||
        lba < g_last_zero_lba || end > zero_end || !payload)
        g_bad_command_encoding++;
}

static void mock_apply_sqe(const struct mock_sqe128 *sqe)
{
    if (sqe->opcode == 46u) {
        const struct mock_nvme_uring_cmd *c =
            (const struct mock_nvme_uring_cmd *)(const void *)sqe->cmd;
        uint64_t lba = (uint64_t)c->cdw10 | ((uint64_t)c->cdw11 << 32);
        uint64_t raw_byte = lba * g_mock_lbs;
        if (g_mock_lbs && raw_byte / g_mock_lbs == lba &&
            raw_byte <= (uint64_t)INT64_MAX)
            (void)mock_store_raw((off_t)raw_byte,
                                 (const void *)(uintptr_t)c->addr,
                                 c->data_len);
    } else if (sqe->opcode == 23u && sqe->fd == MOCK_FILE_FD) {
        uint64_t raw_off = (uint64_t)sqe->cmd_op |
                           ((uint64_t)sqe->pad1 << 32);
        if (raw_off <= (uint64_t)INT64_MAX) {
            const void *payload = (const void *)(uintptr_t)sqe->addr;
            (void)mock_store_logical((off_t)raw_off, payload, sqe->len, 1);
            record_io_event(MOCK_EVENT_FS_WRITE, sqe->fd, 0, 0,
                            (off_t)raw_off, sqe->len, UINT_MAX);
        }
    }
}

static void mock_enqueue_cqe(struct mock_ring_map *m, uint64_t user_data,
                             int32_t result)
{
    unsigned *tail = (unsigned *)(void *)(m->cq + 4);
    unsigned mask = *(unsigned *)(void *)(m->cq + 8);
    struct mock_cqe32 *cqes = (struct mock_cqe32 *)(void *)(m->cq + 64);
    unsigned q = *tail & mask;
    cqes[q].user_data = user_data;
    cqes[q].res = result;
    (*tail)++;
}

static long mock_syscall(long nr, ...)
{
    va_list ap;
    va_start(ap, nr);
    if (nr == __NR_io_uring_setup) {
        (void)va_arg(ap, unsigned);
        struct io_uring_params *p = va_arg(ap, struct io_uring_params *);
        memset(p, 0, sizeof *p);
        p->sq_entries = p->cq_entries = 8;
        p->sq_off.head = 0; p->sq_off.tail = 4; p->sq_off.ring_mask = 8;
        p->sq_off.array = 64;
        p->cq_off.head = 0; p->cq_off.tail = 4; p->cq_off.ring_mask = 8;
        p->cq_off.cqes = 64;
        g_setup_calls++;
        if (g_setup_calls == g_fail_setup_call) {
            va_end(ap);
            errno = EOPNOTSUPP;
            return -1;
        }
        int fd = 90 + (int)g_nrings;
        if (g_nrings < 2) g_rings[g_nrings++].fd = fd;
        va_end(ap);
        return fd;
    }
    if (nr == __NR_io_uring_enter) {
        int ringfd = va_arg(ap, int);
        unsigned to_submit = va_arg(ap, unsigned);
        struct mock_ring_map *m = NULL;
        unsigned i;
        g_uring_submits++;
        for (i = 0; i < g_nrings; i++) if (g_rings[i].fd == ringfd) m = &g_rings[i];
        if (g_delayed_pending && !to_submit && m && ringfd == g_delayed_ringfd) {
            if (g_delayed_wait_errors) {
                g_delayed_wait_errors--;
                va_end(ap);
                errno = EINTR;
                return -1;
            }
            mock_enqueue_cqe(m, g_delayed_user_data, g_delayed_result);
            g_delayed_pending = 0;
            g_delayed_completion_seq = ++g_event_seq;
            if (g_signal_report)
                g_signal_report->completion_seq = g_delayed_completion_seq;
            va_end(ap);
            return 0;
        }
        if (g_short_submit_delayed && to_submit >= 2 && m && m->sq && m->sqes) {
            unsigned sq_tail = *(unsigned *)(void *)(m->sq + 4);
            unsigned sq_mask = *(unsigned *)(void *)(m->sq + 8);
            unsigned slot = (sq_tail - to_submit) & sq_mask;
            struct mock_sqe128 *sqes = (struct mock_sqe128 *)(void *)m->sqes;
            mock_validate_raw_sqe(&sqes[slot]);
            mock_apply_sqe(&sqes[slot]);
            (*(unsigned *)(void *)m->sq)++;
            g_delayed_ringfd = ringfd;
            g_delayed_user_data = sqes[slot].user_data;
            g_delayed_result = g_cqe_result_override != INT32_MIN
                             ? g_cqe_result_override : 0;
            g_delayed_pending = 1;
            va_end(ap);
            return 1;
        }
        if (g_eintr_consumed_delayed && to_submit && m && m->sq && m->sqes) {
            unsigned sq_tail = *(unsigned *)(void *)(m->sq + 4);
            unsigned sq_mask = *(unsigned *)(void *)(m->sq + 8);
            unsigned slot = (sq_tail - to_submit) & sq_mask;
            struct mock_sqe128 *sqes = (struct mock_sqe128 *)(void *)m->sqes;
            mock_validate_raw_sqe(&sqes[slot]);
            mock_apply_sqe(&sqes[slot]);
            (*(unsigned *)(void *)m->sq)++;
            g_delayed_ringfd = ringfd;
            g_delayed_user_data = sqes[slot].user_data;
            g_delayed_result = g_cqe_result_override != INT32_MIN
                             ? g_cqe_result_override : 0;
            g_delayed_pending = 1;
            g_eintr_consumed_delayed = 0;
            va_end(ap);
            errno = EINTR;
            return -1;
        }
        if (g_signal_submit_delayed && to_submit && m && m->sq && m->sqes) {
            sigset_t current;
            unsigned sq_tail = *(unsigned *)(void *)(m->sq + 4);
            unsigned sq_mask = *(unsigned *)(void *)(m->sq + 8);
            unsigned slot = (sq_tail - to_submit) & sq_mask;
            struct mock_sqe128 *sqes = (struct mock_sqe128 *)(void *)m->sqes;
            mock_validate_raw_sqe(&sqes[slot]);
            mock_apply_sqe(&sqes[slot]);
            (*(unsigned *)(void *)m->sq)++;
            g_delayed_ringfd = ringfd;
            g_delayed_user_data = sqes[slot].user_data;
            g_delayed_result = 0;
            g_delayed_pending = 1;
            g_signal_submit_delayed = 0;
            if (sigprocmask(SIG_BLOCK, NULL, &current) == 0 &&
                sigismember(&current, SIGINT) == 1 &&
                sigismember(&current, SIGTERM) == 1)
                g_signal_report->masked_at_raise = 1;
            g_signal_report->raise_seq = ++g_event_seq;
            (void)raise(SIGTERM);
            va_end(ap);
            return 1;
        }
        if (g_short_submit && to_submit) {
            g_after_short = 1;
            va_end(ap);
            return (long)(to_submit - 1);
        }
        if (g_after_short && !to_submit) {
            va_end(ap);
            errno = EIO;
            return -1;
        }
        if (to_submit && m && m->cq && m->sqes) {
            unsigned *tail = (unsigned *)(void *)(m->cq + 4);
            unsigned mask = *(unsigned *)(void *)(m->cq + 8);
            unsigned sq_tail = *(unsigned *)(void *)(m->sq + 4);
            unsigned sq_mask = *(unsigned *)(void *)(m->sq + 8);
            struct mock_cqe32 *cqes = (struct mock_cqe32 *)(void *)(m->cq + 64);
            struct mock_sqe128 *sqes = (struct mock_sqe128 *)(void *)m->sqes;
            for (i = 0; i < to_submit; i++) {
                unsigned q = (*tail + i) & mask;
                unsigned slot = (sq_tail - to_submit + i) & sq_mask;
                mock_validate_raw_sqe(&sqes[slot]);
                mock_apply_sqe(&sqes[slot]);
                cqes[q].user_data = sqes[slot].user_data;
                cqes[q].res = g_cqe_result_override != INT32_MIN
                            ? g_cqe_result_override
                            : (sqes[slot].opcode == 23u
                               ? (int32_t)sqes[slot].len : 0);
            }
            *tail += to_submit;
            *(unsigned *)(void *)m->sq += to_submit;
        }
        va_end(ap);
        return (long)to_submit;
    }
    if (nr == __NR_io_uring_register) {
        int ringfd = va_arg(ap, int);
        unsigned op = va_arg(ap, unsigned);
        const struct iovec *iov = va_arg(ap, const struct iovec *);
        unsigned nr_iov = va_arg(ap, unsigned);
        unsigned i, known = 0;
        g_register_calls++;
        for (i = 0; i < g_nrings; i++) if (g_rings[i].fd == ringfd) known = 1;
        if (!known || op != 0u /* IORING_REGISTER_BUFFERS */ || !iov ||
            nr_iov == 0 || nr_iov > 2) {
            g_register_bad++;
            va_end(ap);
            errno = EINVAL;
            return -1;
        }
        for (i = 0; i < nr_iov; i++) {
            if (!iov[i].iov_base || !iov[i].iov_len) g_register_bad++;
            g_register_base[i] = iov[i].iov_base;
            g_register_len[i] = iov[i].iov_len;
        }
        g_register_nr = nr_iov;
        if (g_fail_register) {
            g_register_nr = 0;
            va_end(ap);
            errno = ENOMEM;
            return -1;
        }
        va_end(ap);
        return 0;
    }
    va_end(ap);
    errno = ENOSYS;
    return -1;
}

static void *mock_mmap(void *addr, size_t len, int prot, int flags, int fd,
                       off_t off)
{
    (void)addr; (void)prot; (void)flags; (void)fd; (void)off;
    unsigned char *p = calloc(1, len ? len : 4096);
    if (p && fd == -1 && len == (4u << 20) + 4096u && !g_mock_stage_region)
        g_mock_stage_region = p;
    if (p && len > 12) *(unsigned *)(void *)(p + 8) = 7;
    for (unsigned i = 0; i < g_nrings; i++) {
        if (g_rings[i].fd != fd) continue;
        if (off == IORING_OFF_SQ_RING) g_rings[i].sq = p;
        else if (off == IORING_OFF_CQ_RING) g_rings[i].cq = p;
        else if (off == IORING_OFF_SQES) g_rings[i].sqes = p;
    }
    return p ? p : MAP_FAILED;
}

static int mock_munmap(void *addr, size_t len)
{
    g_lifecycle_seq++;
    if (len == (4u << 20)) g_buffer_teardown_seq = g_lifecycle_seq;
    (void)len;
    free(addr);
    return 0;
}

static int mock_madvise(void *addr, size_t len, int advice)
{ (void)addr; (void)len; (void)advice; return 0; }

static int mock_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    if (request == BLKGETDISKSEQ) {
        uint64_t *diskseq = va_arg(ap, uint64_t *);
        unsigned call = g_durability_diskseq_calls++;

        if (fd != MOCK_WHOLE_FD || !diskseq) g_bad_guard_args++;
        if (g_fail_durability_diskseq_call == call + 1) {
            va_end(ap);
            errno = EIO;
            return -1;
        }
        *diskseq = g_durability_diskseq[call == 0 ? 0 : 1];
        va_end(ap);
        return 0;
    }
    if (request == NVME_IOCTL_ID) {
        if (fd != MOCK_CHAR_FD) g_bad_guard_args++;
        va_end(ap);
        return 7;
    }
    if (request == NVME_IOCTL_IO_CMD) {
        struct nvme_passthru_cmd *c = va_arg(ap, struct nvme_passthru_cmd *);
        const unsigned char *payload = (const unsigned char *)(uintptr_t)c->addr;
        uint64_t lba = (uint64_t)c->cdw10 | ((uint64_t)c->cdw11 << 32);
        uint64_t blocks = (uint64_t)(c->cdw12 & UINT32_C(0xffff)) + 1;
        uint64_t end = lba + blocks;
        uint64_t zero_end = g_last_zero_lba + g_last_zero_count;
        g_ioctl_submits++;
        g_decoded_raw_commands++;
        if (fd != MOCK_WHOLE_FD || c->opcode != 0x01 || c->nsid != 7 ||
            !c->data_len || c->data_len != blocks * g_mock_lbs ||
            !g_last_zero_count || end < lba || zero_end < g_last_zero_lba ||
            lba < g_last_zero_lba || end > zero_end || !payload)
            g_bad_command_encoding++;
        if (g_ioctl_status == 0 && payload && g_mock_lbs &&
            lba <= (uint64_t)INT64_MAX / g_mock_lbs)
            (void)mock_store_raw((off_t)(lba * g_mock_lbs), payload,
                                 c->data_len);
        va_end(ap);
        return g_ioctl_status;
    }
    if (request == FS_IOC_FIEMAP) {
        struct fiemap *fm = va_arg(ap, struct fiemap *);
        struct fiemap_extent *fe = fm->fm_extents;
        g_fiemap_calls++;
        if (fm->fm_flags & FIEMAP_FLAG_SYNC) g_fiemap_sync_seen++;
        if (fd != MOCK_FILE_FD) g_bad_guard_args++;
        if (g_fiemap_too_many) {
            fm->fm_mapped_extents = 65;
            for (unsigned i = 0; i < 64; i++) {
                fe[i].fe_logical = i == 0 ? 0 : 8192 + (uint64_t)(i - 1) * 4096;
                fe[i].fe_physical = (UINT64_C(100) + i * 2) * 4096;
                fe[i].fe_length = i == 0 ? 8192 : 4096;
                fe[i].fe_flags = i == 63 ? FIEMAP_EXTENT_LAST : 0;
            }
        } else if (g_fiemap_length_overflow) {
            fm->fm_mapped_extents = 2;
            fe[0].fe_logical = 0;
            fe[0].fe_physical = UINT64_C(100) * 4096;
            fe[0].fe_length = UINT64_MAX - UINT64_C(4095);
            fe[0].fe_flags = 0;
            fe[1].fe_logical = fe[0].fe_length;
            fe[1].fe_physical = UINT64_C(900) * 4096;
            fe[1].fe_length = 12288;
            fe[1].fe_flags = FIEMAP_EXTENT_LAST;
        } else if (!g_cross_extent) {
            fm->fm_mapped_extents = 1;
            fe[0].fe_logical = 0;
            fe[0].fe_physical = g_extent_physical_zero
                              ? 0 : UINT64_C(100) * 4096;
            fe[0].fe_length = UINT64_C(32) << 20;
            fe[0].fe_flags = FIEMAP_EXTENT_LAST | g_fiemap_flags;
        } else {
            fm->fm_mapped_extents = 2;
            fe[0].fe_logical = 0;
            fe[0].fe_physical = UINT64_C(100) * 4096;
            fe[0].fe_length = g_cross_boundary ? g_cross_boundary : 4096;
            fe[0].fe_flags = 0;
            fe[1].fe_logical = fe[0].fe_length;
            fe[1].fe_physical = UINT64_C(900) * 4096;
            fe[1].fe_length = (UINT64_C(32) << 20) - fe[0].fe_length;
            fe[1].fe_flags = FIEMAP_EXTENT_LAST;
        }
        va_end(ap);
        return 0;
    }
    va_end(ap);
    errno = EINVAL;
    return -1;
}

static FILE *mock_fopen(const char *path, const char *mode)
{
    static char poll_yes[] = "1\n";
    static char poll_no[] = "0\n";
    static char part_start[] = "800\n";
    (void)mode;
    if (strstr(path, "/queue/io_poll"))
        return fmemopen(g_poll_available ? poll_yes : poll_no, 2, "r");
    if (strstr(path, "/start"))
        return fmemopen(part_start, 4, "r");
    errno = ENOENT;
    return NULL;
}

static __attribute__((unused)) uint32_t mock_lbs(const char *path)
{ (void)path; g_path_guard_calls++; return 4096; }
static uint32_t mock_lbs_fd(int fd)
{
    if (fd != MOCK_WHOLE_FD && fd != MOCK_PART_FD &&
        fd != MOCK_PART_RW_FD) g_bad_guard_args++;
    return fd == MOCK_PART_RW_FD ? g_part_rw_lbs : g_mock_lbs;
}

static __attribute__((unused)) devguard_verdict
mock_check(const char *path, const char *expect,
                                   char *out, unsigned len)
{
    (void)path; (void)expect; (void)out; (void)len;
    g_path_guard_calls++;
    return DEVGUARD_OK;
}

static devguard_verdict mock_check_fd(int fd, const char *expect,
                                      char *out, unsigned len)
{
    if (fd != MOCK_WHOLE_FD) g_bad_guard_args++;
    (void)expect; (void)out; (void)len;
    return DEVGUARD_OK;
}

static __attribute__((unused)) int
mock_resolve_fd(int fd, struct devguard_device_info *out)
{
    unsigned call = g_durability_resolve_calls++;
    unsigned slot = call == 0 ? 0 : 1;

    if (fd != MOCK_WHOLE_FD || !out) g_bad_guard_args++;
    if (!out || g_fail_durability_resolve_call == call + 1) return -ESTALE;
    memset(out, 0, sizeof *out);
    snprintf(out->identity, sizeof out->identity, "mock-ns-identity");
    snprintf(out->controller_serial, sizeof out->controller_serial,
             "mock-controller");
    out->logical_block_size = g_mock_lbs;
    out->dev_major = g_durability_resolve_major[slot];
    out->dev_minor = g_durability_resolve_minor[slot];
    out->parent_major = out->dev_major;
    out->parent_minor = out->dev_minor;
    out->is_partition = g_durability_resolve_is_partition[slot];
    return 0;
}

static __attribute__((unused)) int mock_poll_available_fd(int fd)
{
    unsigned call = g_durability_poll_bind_calls++;
    unsigned slot = call == 0 ? 0 : 1;

    if (fd != MOCK_WHOLE_FD) g_bad_guard_args++;
    return g_durability_sysfs_diskseq[slot] ==
           g_durability_diskseq[slot];
}

static int mock_passthru_ok_fd(int fd)
{ if (fd != MOCK_WHOLE_FD) g_bad_guard_args++; return 1; }

static __attribute__((unused)) int
mock_window(const char *dev, const char *part, uint64_t lba,
                       uint64_t count, uint32_t lbs, char *why, unsigned wlen)
{
    (void)dev; (void)part; (void)lba; (void)count; (void)lbs;
    g_path_guard_calls++;
    if (why && wlen) snprintf(why, wlen, "mock window");
    return 0;
}

static int mock_window_fds(int disk_fd, int part_fd, uint64_t lba,
                           uint64_t count, uint32_t lbs,
                           char *why, unsigned wlen)
{
    if (disk_fd != MOCK_WHOLE_FD || part_fd != MOCK_PART_FD ||
        lbs != g_mock_lbs ||
        !count)
        g_bad_guard_args++;
    g_window_fd_calls++;
    g_last_window_lba = lba;
    g_last_window_count = count;
    if (lba == 202 && count == 2) g_saw_derived_8k_window = 1;
    if (g_window_fd_calls == g_fail_window_call) return -ERANGE;
    if (why && wlen) snprintf(why, wlen, "mock retained window");
    return 0;
}

static __attribute__((unused)) int mock_pair(const char *blk, const char *chr)
{ (void)blk; (void)chr; g_path_guard_calls++; return 1; }

static int mock_pair_at(const char *root, unsigned bmaj, unsigned bmin,
                        unsigned cmaj, unsigned cmin, uint32_t nsid)
{
    if (strcmp(root, "/sys") != 0 || bmaj != 259 || bmin != 0 ||
        cmaj != 240 || cmin != 0 || nsid != 7)
        g_bad_guard_args++;
    return 1;
}

static int mock_zero_fd(int fd, uint64_t lba, uint64_t count, uint64_t *bad)
{
    (void)bad;
    if (fd != MOCK_WHOLE_FD || !count) g_bad_guard_args++;
    record_io_event(MOCK_EVENT_ZERO, fd, lba, count, 0, 0, UINT_MAX);
    g_zero_calls++;
    g_last_zero_lba = lba;
    g_last_zero_count = count;
    return g_zero_result;
}

#define MOCK_PROD_PT_HANDLE \
    ((struct iopath *)(uintptr_t)UINT64_C(0x1800))
#define MOCK_PROD_BLOCK_HANDLE \
    ((struct iopath *)(uintptr_t)UINT64_C(0x1900))

static __attribute__((unused)) struct iopath *
mock_iopath_open_fd(int fd, iopath_backend backend, uint32_t lbs)
{
    unsigned call = ++g_iopath_open_calls;
    struct iopath *handle = backend == IOPATH_URING_CMD_POLL
                          ? MOCK_PROD_PT_HANDLE : MOCK_PROD_BLOCK_HANDLE;

    g_lifecycle_seq++;
    if (call <= 2) {
        g_iopath_open_fd_arg[call - 1] = fd;
        g_iopath_open_backend[call - 1] = backend;
        g_iopath_open_lbs[call - 1] = lbs;
    }
    if ((backend == IOPATH_URING_CMD_POLL && fd != MOCK_WHOLE_FD) ||
        (backend == IOPATH_URING_WRITE_POLL && fd != MOCK_PART_RW_FD) ||
        (backend != IOPATH_URING_CMD_POLL &&
         backend != IOPATH_URING_WRITE_POLL) || lbs != g_mock_lbs)
        g_bad_iopath_args++;
    if (call == g_fail_iopath_open_call) return NULL;
    return handle;
}

static __attribute__((unused)) int
mock_iopath_set_exclusive(struct iopath *handle, int on)
{
    unsigned call = ++g_iopath_exclusive_calls;

    g_lifecycle_seq++;
    if (g_first_exclusive_seq == 0) g_first_exclusive_seq = g_lifecycle_seq;
    g_last_exclusive_seq = g_lifecycle_seq;
    if ((handle != MOCK_PROD_PT_HANDLE &&
         handle != MOCK_PROD_BLOCK_HANDLE) || on != 1)
        g_bad_iopath_args++;
    return call == g_fail_iopath_exclusive_call ? -EIO : 0;
}

static __attribute__((unused)) int
mock_iopath_write(struct iopath *handle, uint64_t lba,
                  const void *buf, size_t len)
{
    unsigned call = ++g_iopath_write_calls;
    unsigned slot = call - 1;
    unsigned kind;
    uint64_t byte_off;

    g_lifecycle_seq++;
    if (slot < sizeof g_iopath_write_handle / sizeof g_iopath_write_handle[0]) {
        g_iopath_write_handle[slot] = handle;
        g_iopath_write_lba[slot] = lba;
        g_iopath_write_buf[slot] = buf;
        g_iopath_write_len[slot] = len;
    }
    if (handle == MOCK_PROD_PT_HANDLE) {
        g_iopath_pt_writes++;
        kind = MOCK_EVENT_PROD_PT_WRITE;
    } else if (handle == MOCK_PROD_BLOCK_HANDLE) {
        g_iopath_block_writes++;
        kind = MOCK_EVENT_PROD_BLOCK_WRITE;
    } else {
        g_bad_iopath_args++;
        return -EINVAL;
    }
    if (!buf || !len || !g_mock_lbs || len % g_mock_lbs ||
        g_last_zero_lba != lba || g_last_zero_count != len / g_mock_lbs)
        g_bad_iopath_args++;
    if (call == g_fail_iopath_write_call) return -ENOSPC;
    if (call == g_short_iopath_write_call) return -EIO;
    if (lba > UINT64_MAX / g_mock_lbs) return -EOVERFLOW;
    byte_off = lba * g_mock_lbs;
    if (byte_off > (uint64_t)INT64_MAX ||
        mock_store_raw((off_t)byte_off, buf, len) != 0)
        return -EIO;
    record_io_event(kind,
                    handle == MOCK_PROD_PT_HANDLE
                        ? MOCK_WHOLE_FD : MOCK_PART_RW_FD,
                    lba, len / g_mock_lbs, 0, len, 0);
    if (call == g_signal_iopath_write_call) {
        sigset_t current;
        if (g_signal_report &&
            sigprocmask(SIG_BLOCK, NULL, &current) == 0 &&
            sigismember(&current, SIGINT) == 1 &&
            sigismember(&current, SIGTERM) == 1)
            g_signal_report->masked_at_raise = 1;
        if (g_signal_report) g_signal_report->raise_seq = ++g_event_seq;
        (void)raise(SIGTERM);
        if (g_signal_report)
            g_signal_report->completion_seq = ++g_event_seq;
    }
    return 0;
}

static __attribute__((unused)) void mock_iopath_close(struct iopath *handle)
{
    int caller_open = 0;

    if (!handle) return;
    g_lifecycle_seq++;
    if (g_first_handle_close_seq == 0)
        g_first_handle_close_seq = g_lifecycle_seq;
    g_last_handle_close_seq = g_lifecycle_seq;
    g_iopath_close_calls++;
    if (handle == MOCK_PROD_PT_HANDLE) {
        g_iopath_pt_close_calls++;
        caller_open = g_close_whole_calls == 0;
    } else if (handle == MOCK_PROD_BLOCK_HANDLE) {
        g_iopath_block_close_calls++;
        caller_open = g_close_part_rw_calls == 0;
    } else {
        g_bad_iopath_args++;
    }
    if (caller_open) g_handle_close_with_caller_open++;
    if (g_signal_report)
        g_signal_report->ring_close_seq = ++g_event_seq;
}

static __attribute__((unused)) int
mock_durability_probe(const char *dev_path, int *has_volatile_cache,
                      int *supports_fua)
{
    g_durability_probe_calls++;
    snprintf(g_durability_probe_path, sizeof g_durability_probe_path,
             "%s", dev_path ? dev_path : "(null)");
    if (g_durability_probe_rc != 0) return g_durability_probe_rc;
    if (has_volatile_cache) *has_volatile_cache = g_durability_probe_vol;
    if (supports_fua) *supports_fua = g_durability_probe_fua;
    return 0;
}

static __attribute__((unused)) exitos_durability
mock_durability_policy(int has_volatile_cache, int supports_fua)
{
    g_durability_policy_calls++;
    g_durability_policy_vol = has_volatile_cache;
    g_durability_policy_fua = supports_fua;
    g_durability_policy_result =
        exitos_durability_policy(has_volatile_cache, supports_fua);
    return g_durability_policy_result;
}

static int mock_printf(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    int n;
    size_t entries;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    g_stdout_calls++;
    if (strstr(line, "POST_RUN_VERIFY")) g_verify_success_lines++;
    if (strstr(line, "POST_RUN_VERIFY") && strstr(line, "selected=0"))
        g_verify_selected_zero_lines++;
    if (strstr(line, "POST_RUN_VERIFY") && strstr(line, "selected=17"))
        g_verify_selected_all_lines++;
    if (sscanf(line,
               "immutable guarded primary map: %*d stable extents, %zu entries preflighted",
               &entries) == 1)
        g_preflight_entries = entries;
    if (strcmp(line,
               "DURABILITY_PROOF probe=success volatile_write_cache=0 fua=0 policy=DUR_NONE\n") == 0)
        g_durability_proof_lines++;
    if (strncmp(line, "timing contract:", strlen("timing contract:")) == 0) {
        size_t copy = strnlen(line, sizeof g_timing_contract_line - 1);
        memcpy(g_timing_contract_line, line, copy);
        g_timing_contract_line[copy] = '\0';
    }
    if (strstr(line, "ARM12_FROZEN")) g_arm12_frozen_lines++;
    if (strstr(line, "DEFAULT_ARMS selected=17 append=excluded"))
        g_default_all_lines++;
    if (strncmp(line, "arm ", 4) == 0) g_table_header_lines++;
    if (strncmp(line, "fs-", 3) == 0 || strncmp(line, "pt-", 3) == 0 ||
        strncmp(line, "blk-", 4) == 0 || strncmp(line, "prod-", 5) == 0)
        g_arm_result_lines++;
    if (strncmp(line, "idle states on cpu", 18) == 0)
        g_idle_report_lines++;
    if (fputs(line, stdout) == EOF) return -1;
    return n;
}

#define main qdsplit_program_main
#define printf mock_printf
#define open mock_open
#define openat mock_openat
#define close mock_close
#define unlink mock_unlink
#define fallocate mock_fallocate
#define fsync mock_fsync
#define fdatasync mock_fdatasync
#define sleep mock_sleep
#define clock_gettime mock_clock_gettime
#define pwrite mock_pwrite
#define pread mock_pread
#define fstat mock_fstat
#define readlink mock_readlink
#define syscall mock_syscall
#define mmap mock_mmap
#define munmap mock_munmap
#define madvise mock_madvise
#define ioctl mock_ioctl
#define fopen mock_fopen
#define exitos_devguard_lbs mock_lbs
#define exitos_devguard_lbs_fd mock_lbs_fd
#define exitos_devguard_check mock_check
#define exitos_devguard_check_fd mock_check_fd
#define exitos_devguard_resolve_fd mock_resolve_fd
#define exitos_devguard_poll_available_fd mock_poll_available_fd
#define exitos_devguard_passthru_ok_fd mock_passthru_ok_fd
#define exitos_devguard_window_in_partition mock_window
#define exitos_devguard_window_in_partition_fds mock_window_fds
#define exitos_devguard_nvme_pair mock_pair
#define exitos_devguard_nvme_pair_at mock_pair_at
#define exitos_devguard_window_is_zero_fd mock_zero_fd
#define iopath_open_fd mock_iopath_open_fd
#define iopath_set_exclusive mock_iopath_set_exclusive
#define iopath_write mock_iopath_write
#define iopath_close mock_iopath_close
#define exitos_durability_probe mock_durability_probe
#define exitos_durability_policy mock_durability_policy
#include "../../../attribution/qdsplit.c"
#undef main
#undef printf
#undef clock_gettime
#undef readlink
#undef iopath_open_fd
#undef iopath_set_exclusive
#undef iopath_write
#undef iopath_close
#undef exitos_durability_probe
#undef exitos_durability_policy

static int mock_any_ring_inflight(void)
{
    return R.inflight != 0 || RP.inflight != 0;
}

static void reset_case(void)
{
    const char *vars[] = {
        "EXITOS_SAMEFILE", "EXITOS_PAIRED", "EXITOS_ONLY", "EXITOS_BIGSZ",
        "EXITOS_HUGEBUF", "EXITOS_REQUIRE_CONTIG",
        "EXITOS_REQUIRE_SCATTERED", "EXITOS_PAUSE", "EXITOS_ORDER_OFFSET",
        "LD_PRELOAD", "LD_AUDIT"
    };
    size_t i;
    for (i = 0; i < sizeof vars / sizeof vars[0]; i++) unsetenv(vars[i]);
    setenv("EXITOS_DEV", "/mock/whole", 1);
    setenv("EXITOS_CHARDEV", "/mock/ng", 1);
    setenv("EXITOS_FSFILE", "/mock/file", 1);
    setenv("EXITOS_WINDOW_IN_PART", "/mock/part", 1);
    setenv("EXITOS_FSPART", "/mock/part", 1);
    setenv("EXITOS_EXPECT_SERIAL", "mock-serial", 1);
    setenv("EXITOS_LBA_START", "100", 1);
    setenv("EXITOS_LBA_COUNT", "100000", 1);
    g_write_submits = g_uring_submits = g_ioctl_submits = 0;
    g_pwrite_calls = g_unlink_calls = g_append_unlink_calls = g_fiemap_calls = 0;
    g_zero_calls = g_window_fd_calls = 0;
    g_path_guard_calls = 0;
    g_part_rdev = makedev(259, 1);
    g_part_rw_rdev = g_part_rdev;
    g_file_dev = g_part_rdev;
    g_file_nlink = 0;
    g_open_file_flags = g_open_append_flags = 0;
    g_fail_pwrite_call = g_short_pwrite_call = 0;
    g_file_preexists = g_append_preexists = 0;
    g_poll_available = 0;
    g_ioctl_status = 0;
    g_cqe_result_override = INT32_MIN;
    g_short_submit = 0;
    g_cross_extent = 0;
    g_extent_physical_zero = 0;
    g_zero_result = 0;
    g_after_short = 0;
    g_fail_fallocate = g_fail_fsync = 0;
    g_fiemap_flags = 0;
    g_fail_window_call = 0;
    g_cross_boundary = 0;
    g_setup_calls = g_fail_setup_call = 0;
    g_open_whole_calls = g_open_char_calls = g_open_part_calls = 0;
    g_open_part_rw_calls = 0;
    g_open_file_calls = g_open_append_calls = g_open_parent_calls = 0;
    g_open_whole_flags = g_open_char_flags = g_open_part_flags = 0;
    g_open_part_rw_flags = 0;
    g_open_parent_flags = 0;
    g_tmpfile_calls = 0;
    g_fail_tmpfile = 0;
    g_fallocate_len = 0;
    g_first_pwrite_len = 0;
    g_first_pwrite_off = (off_t)-1;
    g_fiemap_sync_seen = 0;
    g_sleep_calls = 0;
    g_pagemap_reads = 0;
    g_pagemap_mode = 0;
    g_mock_stage_region = NULL;
    g_last_pt_addr = 0;
    g_register_calls = 0;
    g_register_bad = 0;
    g_fail_register = 0;
    g_register_base[0] = g_register_base[1] = NULL;
    g_register_len[0] = g_register_len[1] = 0;
    g_register_nr = 0;
    g_fixed_submits = 0;
    g_bad_fixed = 0;
    g_last_fixed_buf_index = 0;
    g_last_window_lba = g_last_window_count = 0;
    g_saw_derived_8k_window = 0;
    g_last_zero_lba = g_last_zero_count = 0;
    g_bad_guard_args = g_bad_command_encoding = g_decoded_raw_commands = 0;
    g_raw_block_pwrite_calls = g_bad_raw_block_write = 0;
    g_short_raw_block_write = 0;
    g_last_raw_block_off = (off_t)-1;
    g_last_raw_block_len = 0;
    g_fiemap_too_many = g_fiemap_length_overflow = 0;
    g_short_submit_delayed = g_eintr_consumed_delayed = 0;
    g_signal_submit_delayed = 0;
    g_delayed_pending = g_delayed_wait_errors = 0;
    g_delayed_completion_seq = g_file_close_seq = g_ring_close_seq = 0;
    g_event_seq = 0;
    g_delayed_ringfd = -1;
    g_delayed_user_data = 0;
    g_delayed_result = 0;
    g_mock_lbs = 4096;
    g_part_rw_lbs = g_mock_lbs;
    g_file_readback_calls = g_raw_readback_calls = 0;
    g_verify_success_lines = 0;
    g_verify_selected_zero_lines = g_verify_selected_all_lines = 0;
    g_preflight_entries = 0;
    g_durability_proof_lines = 0;
    memset(g_timing_contract_line, 0, sizeof g_timing_contract_line);
    g_arm12_frozen_lines = g_default_all_lines = 0;
    g_table_header_lines = g_arm_result_lines = g_idle_report_lines = 0;
    g_stdout_calls = 0;
    g_swap_equal_slots = 0;
    g_duplicate_equal_slots = 0;
    g_fail_file_read_call = g_short_file_read_call = 0;
    g_fail_raw_read_call = g_short_raw_read_call = 0;
    g_corrupt_file_logical = UINT64_MAX;
    g_corrupt_raw_logical = UINT64_MAX;
    g_clock_calls = 0;
    g_first_read_clock_calls = UINT_MAX;
    g_readback_while_inflight = 0;
    g_iopath_open_calls = g_iopath_exclusive_calls = 0;
    g_iopath_write_calls = g_iopath_close_calls = 0;
    g_iopath_pt_close_calls = g_iopath_block_close_calls = 0;
    g_fail_iopath_open_call = g_fail_iopath_exclusive_call = 0;
    g_fail_iopath_write_call = g_short_iopath_write_call = 0;
    g_signal_iopath_write_call = g_bad_iopath_args = 0;
    memset(g_iopath_open_fd_arg, 0, sizeof g_iopath_open_fd_arg);
    memset(g_iopath_open_backend, 0, sizeof g_iopath_open_backend);
    memset(g_iopath_open_lbs, 0, sizeof g_iopath_open_lbs);
    memset(g_iopath_write_handle, 0, sizeof g_iopath_write_handle);
    memset(g_iopath_write_lba, 0, sizeof g_iopath_write_lba);
    memset(g_iopath_write_buf, 0, sizeof g_iopath_write_buf);
    memset(g_iopath_write_len, 0, sizeof g_iopath_write_len);
    g_iopath_pt_writes = g_iopath_block_writes = 0;
    g_close_whole_calls = g_close_part_calls = g_close_part_rw_calls = 0;
    g_close_char_calls = g_close_file_calls = g_close_dir_calls = 0;
    g_lifecycle_seq = g_first_init_write_seq = 0;
    g_first_exclusive_seq = g_last_exclusive_seq = 0;
    g_first_handle_close_seq = g_last_handle_close_seq = 0;
    g_first_retained_close_seq = g_file_teardown_seq = 0;
    g_buffer_teardown_seq = g_handle_close_with_caller_open = 0;
    g_durability_probe_calls = g_durability_policy_calls = 0;
    g_durability_resolve_calls = 0;
    g_fail_durability_resolve_call = 0;
    g_durability_resolve_major[0] = g_durability_resolve_major[1] = 259;
    g_durability_resolve_minor[0] = g_durability_resolve_minor[1] = 0;
    g_durability_resolve_is_partition[0] = 0;
    g_durability_resolve_is_partition[1] = 0;
    g_durability_diskseq_calls = 0;
    g_fail_durability_diskseq_call = 0;
    g_durability_diskseq[0] = g_durability_diskseq[1] = UINT64_C(71);
    g_durability_poll_bind_calls = 0;
    g_durability_sysfs_diskseq[0] = UINT64_C(71);
    g_durability_sysfs_diskseq[1] = UINT64_C(71);
    g_durability_sysfs_readlink_calls = 0;
    g_fail_durability_sysfs_readlink_call = 0;
    snprintf(g_durability_sysfs_link[0], sizeof g_durability_sysfs_link[0],
             "../../devices/mock/block/nvme-mock");
    snprintf(g_durability_sysfs_link[1], sizeof g_durability_sysfs_link[1],
             "../../devices/mock/block/nvme-mock");
    g_durability_probe_rc = 0;
    g_durability_probe_vol = 0;
    g_durability_probe_fua = 0;
    g_durability_policy_vol = g_durability_policy_fua = -1;
    g_durability_policy_result = EXITOS_DUR_FLUSH;
    memset(g_durability_probe_path, 0, sizeof g_durability_probe_path);
    memset(g_backing, 0, sizeof g_backing);
    memset(g_file_read_covered, 0, sizeof g_file_read_covered);
    memset(g_raw_read_covered, 0, sizeof g_raw_read_covered);
    memset(g_written_slot_off, 0, sizeof g_written_slot_off);
    memset(g_written_slot_len, 0, sizeof g_written_slot_len);
    g_written_slot_count = 0;
    memset(g_io_events, 0, sizeof g_io_events);
    g_io_event_count = g_io_event_overflow = 0;
    g_signal_report = NULL;
    memset(g_rings, 0, sizeof g_rings);
    g_nrings = 0;
}

static unsigned all_write_submits(void)
{
    return g_write_submits + g_uring_submits + g_ioctl_submits +
           g_iopath_write_calls;
}

static unsigned all_storage_opens(void)
{
    return g_open_whole_calls + g_open_char_calls + g_open_part_calls +
           g_open_part_rw_calls + g_open_file_calls + g_open_append_calls +
           g_open_parent_calls;
}

static int timed_arm_at(unsigned position)
{
    unsigned event = position * 2 + 1;

    if (event >= g_io_event_count) return -1;
    if (g_io_events[event].kind == MOCK_EVENT_FS_WRITE) return 0;
    if (g_io_events[event].kind == MOCK_EVENT_PROD_PT_WRITE) return 18;
    if (g_io_events[event].kind == MOCK_EVENT_PROD_BLOCK_WRITE) return 19;
    return -1;
}

static int event_is(unsigned i, unsigned kind, int fd, uint64_t lba,
                    uint64_t count, off_t off, size_t len)
{
    const struct mock_io_event *e;
    if (i >= g_io_event_count) return 0;
    e = &g_io_events[i];
    return e->kind == kind && e->fd == fd && e->lba == lba &&
           e->count == count && e->off == off && e->len == len;
}

static int zero_is_fresh_for(unsigned zi, unsigned wi)
{
    const struct mock_io_event *z, *w;
    if (wi != zi + 1 || wi >= g_io_event_count) return 0;
    z = &g_io_events[zi];
    w = &g_io_events[wi];
    return z->kind == MOCK_EVENT_ZERO && z->fd == MOCK_WHOLE_FD &&
           (w->kind == MOCK_EVENT_FS_WRITE ||
            ((w->kind == MOCK_EVENT_BLOCK_WRITE ||
              w->kind == MOCK_EVENT_PT_SUBMIT ||
              w->kind == MOCK_EVENT_PROD_PT_WRITE ||
              w->kind == MOCK_EVENT_PROD_BLOCK_WRITE) &&
             z->lba == w->lba && z->count == w->count));
}

static int range_is_covered(const unsigned char *coverage, uint64_t off,
                            uint64_t len)
{
    uint64_t i;
    if (off > MOCK_BACKING_BYTES || len > MOCK_BACKING_BYTES - off)
        return 0;
    for (i = 0; i < len; i++) if (!coverage[off + i]) return 0;
    return 1;
}

int main(void)
{
    char *argv[] = { (char *)"qdsplit", (char *)"1", NULL };
    char *argv2[] = { (char *)"qdsplit", (char *)"2", NULL };
    char *argv20[] = { (char *)"qdsplit", (char *)"20", NULL };
    int rc;

    reset_case();
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "non-samefile qdsplit is frozen");
    T_EQ(all_write_submits(), 0,
         "non-samefile refusal happens before initialization or raw submission");

    reset_case();
    setenv("EXITOS_PAIRED", "8k", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "paired mode without samefile is frozen");
    T_EQ(all_write_submits(), 0,
         "paired non-samefile mode cannot bypass the raw-window freeze");

    reset_case();
    setenv("EXITOS_SAMEFILE", "", 1);
    setenv("EXITOS_ONLY", "0", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "empty SAMEFILE marker does not arm owned-file mode");
    T_EQ(all_write_submits(), 0,
         "empty SAMEFILE marker is refused before initialization");

    reset_case();
    setenv("EXITOS_SAMEFILE", "0", 1);
    setenv("EXITOS_ONLY", "0", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "false SAMEFILE marker does not arm owned-file mode");
    T_EQ(all_write_submits(), 0,
         "false SAMEFILE marker is refused before initialization");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    rc = qdsplit_program_main(2, argv2);
    T_EQ(rc, 0, "two-slot filesystem run passes its post-run oracle");
    T_OK(g_fallocate_len >= 4 * 8192,
         "owned primary file adds an 8 KiB-or-larger guard at both ends");
    T_EQ(g_written_slot_count, 2,
         "fixture captured both equal-length timed slots");
    T_OK(g_written_slot_count == 2 &&
         g_written_slot_off[0] >= (off_t)OWNED_GUARD_BYTES &&
         g_written_slot_off[1] >= (off_t)OWNED_GUARD_BYTES &&
         (uint64_t)g_written_slot_off[0] + g_written_slot_len[0] <=
             (uint64_t)g_fallocate_len - OWNED_GUARD_BYTES &&
         (uint64_t)g_written_slot_off[1] + g_written_slot_len[1] <=
             (uint64_t)g_fallocate_len - OWNED_GUARD_BYTES,
         "every planned slot stays strictly between both owned guards");
    T_OK(g_written_slot_count == 2 &&
         g_written_slot_len[0] == g_written_slot_len[1] &&
         memcmp(g_backing + g_written_slot_off[0],
                g_backing + g_written_slot_off[1],
                g_written_slot_len[0]) != 0,
         "iteration identity changes the complete deterministic slot pattern");
    T_OK(g_file_readback_calls > 0 && g_raw_readback_calls > 0,
         "oracle reads through both retained O_DIRECT file and whole-device fds");
    T_EQ(g_first_read_clock_calls, 4,
         "two-slot readback starts only after both samples recorded t1");
    T_EQ(g_readback_while_inflight, 0,
         "readback begins only with every accepted ring command quiescent");
    T_EQ(g_fiemap_sync_seen, 1,
         "even a file-only run derives raw readback solely from synchronized FIEMAP");
    T_EQ(g_zero_calls, 2,
         "each filesystem sample receives its own whole-device zero precheck");
    T_OK(g_io_event_count == 4 &&
         event_is(0, MOCK_EVENT_ZERO, MOCK_WHOLE_FD, 202, 2, 0, 0) &&
         event_is(1, MOCK_EVENT_FS_WRITE, MOCK_FILE_FD, 0, 0, 8192, 8192) &&
         event_is(2, MOCK_EVENT_ZERO, MOCK_WHOLE_FD, 204, 2, 0, 0) &&
         event_is(3, MOCK_EVENT_FS_WRITE, MOCK_FILE_FD, 0, 0, 16384, 8192) &&
         zero_is_fresh_for(0, 1) && zero_is_fresh_for(2, 3),
         "each filesystem zero proof is immediately adjacent to its exact mapped write");
    T_OK(g_fallocate_len > 0 &&
         range_is_covered(g_file_read_covered, 0,
                          (uint64_t)g_fallocate_len) &&
         range_is_covered(g_raw_read_covered, 0,
                          (uint64_t)g_fallocate_len),
         "file and raw oracles cover every byte of the guarded primary file");
    T_EQ(g_verify_success_lines, 1,
         "successful byte-exact readback emits one explicit success line");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_swap_equal_slots = 1;
    rc = qdsplit_program_main(2, argv2);
    T_OK(rc != 0,
         "post-run oracle rejects an equal-length whole-slot interchange");
    T_EQ(g_verify_success_lines, 0,
         "failed whole-slot oracle never emits the success line");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_duplicate_equal_slots = 1;
    rc = qdsplit_program_main(2, argv2);
    T_OK(rc != 0,
         "post-run oracle rejects duplication of one equal-length whole slot");
    T_EQ(g_verify_success_lines, 0,
         "failed whole-slot duplication never emits the success line");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_short_file_read_call = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0 && g_verify_success_lines == 0,
         "short O_DIRECT file read fails closed without a success line");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_fail_file_read_call = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0 && g_verify_success_lines == 0,
         "file-read EIO fails closed without a success line");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_short_raw_read_call = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0 && g_verify_success_lines == 0,
         "short whole-block read fails closed without a success line");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_fail_raw_read_call = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0 && g_verify_success_lines == 0,
         "whole-block read EIO fails closed without a success line");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_corrupt_file_logical = OWNED_GUARD_BYTES;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0 && g_verify_success_lines == 0,
         "one wrong selected byte in the file view fails the oracle");
    T_EQ(g_table_header_lines + g_arm_result_lines + g_idle_report_lines, 0,
         "oracle failure returns before table, arm result, or idle reporting");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_corrupt_raw_logical = 0;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0 && g_verify_success_lines == 0,
         "one wrong guard byte in the raw view fails the oracle");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_fail_window_call = 2; /* explicit window passes; primary-map proof fails */
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0 && g_pwrite_calls == 1 && g_zero_calls == 0 &&
         g_raw_readback_calls == 0 && g_verify_success_lines == 0,
         "every filesystem range is partition-validated before timed I/O");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_cross_extent = 1;
    g_cross_boundary = 4096; /* leading guard spans both stable extents */
    rc = qdsplit_program_main(2, argv);
    T_OK(rc == 0 && g_verify_success_lines == 1 &&
         range_is_covered(g_file_read_covered, 0,
                          (uint64_t)g_fallocate_len) &&
         range_is_covered(g_raw_read_covered, 0,
                          (uint64_t)g_fallocate_len),
         "oracle walks synchronized FIEMAP extent boundaries across guards and selected bytes");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_cross_extent = 1;
    g_cross_boundary = 12288; /* selected FS slot [8192,16384) crosses */
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0 && g_pwrite_calls == 1 && g_zero_calls == 0,
         "filesystem range without one exact FIEMAP LBA span fails before timing");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("EXITOS_LBA_START", "0", 1);
    setenv("EXITOS_LBA_COUNT", "0", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc == 0 && g_saw_derived_8k_window == 1 && g_zero_calls == 1 &&
         g_last_zero_lba == 202 && g_last_zero_count == 2,
         "derived window covers and prechecks a filesystem-only primary plan");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("EXITOS_LBA_START", "999", 1);
    setenv("EXITOS_LBA_COUNT", "1", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc == 0 && g_zero_calls == 1 && g_last_zero_lba == 202,
         "explicit raw window does not constrain a partition-valid filesystem precheck");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    setenv("EXITOS_LBA_START", "100", 1);
    setenv("EXITOS_LBA_COUNT", "1", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0 && g_ioctl_submits == 0,
         "explicit raw window still bounds every selected raw write");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("LD_PRELOAD", "./libexitos_preload.so", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "qdsplit refuses an inherited preload interposer");
    T_EQ(all_write_submits(), 0,
         "preload contamination is rejected before initialization or timed I/O");
    T_EQ(g_open_whole_calls, 0,
         "preload contamination is rejected before retained device opens");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("LD_AUDIT", "/tmp/measurement-auditor.so", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "qdsplit refuses an inherited dynamic-loader audit module");
    T_EQ(all_write_submits(), 0,
         "audit-module contamination is rejected before storage writes");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    setenv("EXITOS_LBA_START", "0", 1);
    setenv("EXITOS_LBA_COUNT", "0", 1);
    setenv("EXITOS_PAUSE", "0", 1);
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "samefile 0/0 derives a safe immutable raw window");
    T_EQ(g_sleep_calls, 1, "samefile 0/0 completes preflight and reaches READY");
    T_EQ(g_ioctl_submits, 1, "derived-window run submits its selected raw sample");
    T_EQ(g_saw_derived_8k_window, 1,
         "derived window begins at the retained file extent LBA");
    T_EQ(g_last_window_count, 2,
         "derived window spans the complete selected 8 KiB request");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    setenv("EXITOS_LBA_START", "0", 1);
    setenv("EXITOS_LBA_COUNT", "100", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "zero start with nonzero count is not the 0/0 sentinel");
    T_EQ(all_write_submits(), 0,
         "half-zero explicit window is rejected before initialization");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    setenv("EXITOS_LBA_START", "100", 1);
    setenv("EXITOS_LBA_COUNT", "0", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "nonzero start with zero count is not the 0/0 sentinel");
    T_EQ(all_write_submits(), 0,
         "reverse half-zero window is rejected before initialization");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    setenv("EXITOS_LBA_START", "0", 1);
    setenv("EXITOS_LBA_COUNT", "0", 1);
    g_fail_window_call = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "derived request outside retained partition fails closed");
    T_EQ(g_ioctl_submits, 0,
         "out-of-partition derived plan reaches no raw submission");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    setenv("EXITOS_LBA_START", "0", 1);
    setenv("EXITOS_LBA_COUNT", "0", 1);
    g_extent_physical_zero = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "0/0 sentinel cannot bless an unsafe physical extent");
    T_EQ(g_ioctl_submits, 0,
         "unsafe sentinel extent reaches no raw submission");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    setenv("EXITOS_LBA_START", "0", 1);
    setenv("EXITOS_LBA_COUNT", "0", 1);
    g_file_dev = makedev(259, 9);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "0/0 sentinel still requires exact file/partition identity");
    T_EQ(all_write_submits(), 0,
         "sentinel identity mismatch is refused before initialization");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_file_dev = makedev(259, 9);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "samefile refuses a file from a different filesystem");
    T_EQ(all_write_submits(), 0,
         "file/partition identity mismatch is found before initialization");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "10", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "selected polling arm fails when native polling is unavailable");
    T_EQ(all_write_submits(), 0,
         "poll availability is preflighted before initialization writes");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "not-an-arm", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "invalid EXITOS_ONLY is rejected instead of selecting arm zero");
    T_EQ(all_write_submits(), 0,
         "invalid arm selection is rejected before initialization writes");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "1", 1);
    setenv("EXITOS_PAIRED", "8k", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "EXITOS_ONLY and EXITOS_PAIRED cannot form an ambiguous plan");
    T_EQ(all_write_submits(), 0,
         "ambiguous selection is rejected before initialization writes");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_cross_extent = 1;
    g_cross_boundary = 12288; /* guarded selected slot [8192,16384) crosses */
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "a selected 8 KiB raw request crossing extents fails the run");
    T_EQ(g_ioctl_submits, 0,
         "every raw request is mapped as a whole before the first ioctl submit");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_ioctl_status = 0x81;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "positive NVMe ioctl status fails the selected arm and process");
    T_EQ(g_ioctl_submits, 1, "positive-status test issued exactly one ioctl sample");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "1", 1);
    g_cqe_result_override = 0x81;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "positive NVMe uring CQE status fails the process");
    T_EQ(g_uring_submits, 1, "positive CQE test submitted exactly one sample");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "1", 1);
    g_short_submit = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "short io_uring submission fails the selected run");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "4", 1);
    g_short_submit_delayed = 1;
    g_delayed_wait_errors = 3;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "one-of-two accepted uring submit fails the selected run");
    T_EQ(g_decoded_raw_commands, 1,
         "short-submit fixture accepted exactly one raw command");
    T_EQ(g_bad_command_encoding, 0,
         "accepted uring command matches its checked SLBA/NLB/namespace/payload");
    T_OK(g_delayed_completion_seq > 0,
         "accepted raw command is drained to completion on the failure path");
    T_OK(g_ring_close_seq > g_delayed_completion_seq,
         "ring teardown follows completion of every accepted command");
    T_OK(g_file_close_seq > g_ring_close_seq,
         "anonymous inode remains allocated until the ring is quiescent");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "1", 1);
    g_eintr_consumed_delayed = 1;
    g_delayed_wait_errors = 2;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0,
         "EINTR submit with an advanced SQ head fails the selected run");
    T_EQ(g_decoded_raw_commands, 1,
         "SQ-head oracle exposes the raw command consumed despite EINTR");
    T_EQ(g_bad_command_encoding, 0,
         "EINTR-consumed command retains checked SLBA/NLB/namespace/payload");
    T_OK(g_delayed_completion_seq > 0,
         "EINTR-consumed raw command is drained to delayed completion");
    T_OK(g_ring_close_seq > g_delayed_completion_seq,
         "EINTR-consumed completion precedes ring teardown");
    T_OK(g_file_close_seq > g_ring_close_seq,
         "owned inode outlives an EINTR-consumed raw command");

    {
        struct signal_report *report = real_shared_alloc(sizeof *report);
        pid_t child = -1;
        int status = 0;

        T_OK(report != MAP_FAILED,
             "signal-lifecycle fixture allocates shared ordering report");
        if (report != MAP_FAILED) {
            memset(report, 0, sizeof *report);
            (void)fflush(NULL);
            child = fork();
            T_OK(child >= 0, "signal-lifecycle fixture forks safely");
            if (child == 0) {
                reset_case();
                setenv("EXITOS_SAMEFILE", "1", 1);
                setenv("EXITOS_ONLY", "1", 1);
                g_signal_report = report;
                g_signal_submit_delayed = 1;
                report->qdsplit_rc = qdsplit_program_main(2, argv);
                report->returned = 1;
                _exit(0);
            }
            if (child > 0) (void)waitpid(child, &status, 0);
            T_OK(child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                 "SIGTERM during accepted raw I/O reaches graceful process return");
            T_EQ(report->returned, 1,
                 "SIGTERM path returns through qdsplit cleanup");
            T_EQ(report->qdsplit_rc, 128 + SIGTERM,
                 "graceful SIGTERM uses the documented nonzero exit status");
            T_EQ(report->masked_at_raise, 1,
                 "SIGINT and SIGTERM are blocked across submit-to-completion");
            T_OK(report->completion_seq > report->raise_seq,
                 "pending SIGTERM does not interrupt accepted command completion");
            T_OK(report->ring_close_seq > report->completion_seq,
                 "SIGTERM cleanup closes ring only after delayed completion");
            T_OK(report->file_close_seq > report->ring_close_seq,
                 "SIGTERM cleanup retains anonymous inode until ring teardown");
            real_shared_free(report, sizeof *report);
        }
    }

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_short_pwrite_call = 2;       /* one full-range initialization, then sample */
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "short timed filesystem write fails the selected run");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_zero_result = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "nonzero owned-file raw target fails immediately");
    T_EQ(g_ioctl_submits, 0,
         "retained-fd zero recheck happens before the raw ioctl submission");
    T_OK(g_zero_calls > 0, "raw safety check reads zero state through retained whole fd");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "safe retained-fd ioctl-only plan completes");
    T_EQ(g_path_guard_calls, 0,
         "successful samefile plan uses no pathname safety wrapper after open");
    T_OK(g_window_fd_calls > 1,
         "declared window and physical request are both checked via retained fds");
    T_EQ(g_bad_guard_args, 0,
         "retained guards receive the exact whole/partition/character fds and LBS");
    T_EQ(g_open_whole_flags,
         (unsigned)(O_RDWR | O_DIRECT | O_CLOEXEC | O_NOFOLLOW),
         "whole-device fd uses the exact retained raw-write open contract");
    T_EQ(g_open_char_flags, (unsigned)(O_RDWR | O_CLOEXEC | O_NOFOLLOW),
         "NVMe character fd uses the exact retained passthrough open contract");
    T_EQ(g_open_part_flags,
         (unsigned)(O_RDONLY | O_DIRECT | O_CLOEXEC | O_NOFOLLOW),
         "partition fd uses the exact retained read-only direct-I/O contract");
    T_EQ(g_open_parent_flags,
         (unsigned)(O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
         "owned-file parent uses the exact retained directory open contract");
    T_EQ(g_decoded_raw_commands, 1,
         "ioctl raw command is decoded by the independent test oracle");
    T_EQ(g_bad_command_encoding, 0,
         "ioctl SLBA/NLB/payload exactly match the immediately checked interval");
    T_EQ(g_open_file_calls, 0,
         "owned inode is never opened through the visible FSFILE pathname");
    T_EQ(g_tmpfile_calls, 1,
         "owned inode is created as one anonymous O_TMPFILE");
    T_EQ(g_open_file_flags,
         (unsigned)(O_TMPFILE | O_EXCL | O_RDWR | O_DIRECT | O_CLOEXEC |
                    O_NOFOLLOW),
         "owned inode open retains every anonymous/direct/exclusive flag");
    T_EQ(g_unlink_calls, 0,
         "anonymous owned inode needs no pathname unlink cleanup");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_mock_lbs = 512;
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "512-byte retained target completes one ioctl sample");
    T_EQ(g_decoded_raw_commands, 1,
         "512-byte target command is decoded by the independent oracle");
    T_EQ(g_bad_command_encoding, 0,
         "512-byte target command uses the actual retained-fd LBS");
    T_EQ(g_last_zero_lba, 1616,
         "512-byte target maps partition start plus FIEMAP physical offset");
    T_EQ(g_last_zero_count, 16,
         "8 KiB payload is encoded as sixteen 512-byte native blocks");
    T_EQ(g_bad_guard_args, 0,
         "512-byte LBS is propagated consistently to every retained-fd guard");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "12", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "explicit append-only arm is safety-frozen");
    T_EQ(all_write_submits(), 0,
         "append-only refusal occurs before initialization or timed writes");
    T_EQ(g_open_whole_calls + g_open_part_calls + g_open_parent_calls, 0,
         "append-only refusal occurs before every storage open");
    T_EQ(g_tmpfile_calls, 0,
         "append-only refusal cannot create either anonymous inode");
    T_EQ(g_arm12_frozen_lines, 1,
         "append-only refusal emits the explicit ARM12_FROZEN contract");
    T_EQ(g_verify_selected_zero_lines, 0,
         "append-only refusal cannot print a selected=0 oracle success");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_PAIRED", "wal", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "paired set containing append arm is safety-frozen");
    T_EQ(all_write_submits(), 0,
         "append-containing paired refusal occurs before every write");
    T_EQ(g_open_whole_calls + g_open_part_calls + g_open_parent_calls, 0,
         "append-containing paired refusal occurs before storage opens");
    T_EQ(g_arm12_frozen_lines, 1,
         "append-containing paired refusal emits ARM12_FROZEN");
    T_EQ(g_verify_success_lines, 0,
         "append-containing paired refusal reaches no oracle success");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_extent_physical_zero = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "FIEMAP physical offset zero is rejected as an unsafe location");
    T_EQ(g_ioctl_submits, 0,
         "physical-block-zero map cannot reach a raw ioctl submission");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_fail_fallocate = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "preallocation failure is propagated to main");
    T_EQ(all_write_submits(), 0,
         "failed preallocation cannot start initialization or raw I/O");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_short_pwrite_call = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "short full-range initialization is fatal");
    T_EQ(g_ioctl_submits, 0,
         "short initialization prevents FIEMAP-based raw submission");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_fail_fsync = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "initialization fsync failure is fatal");
    T_EQ(g_fiemap_calls, 0,
         "failed initialization sync cannot publish a physical plan");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_fiemap_flags = FIEMAP_EXTENT_SHARED;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "shared FIEMAP extent is rejected");
    T_EQ(g_ioctl_submits, 0,
         "unstable/shared extent cannot reach raw submission");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_fail_window_call = 2;       /* declared window passes; raw request fails */
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "raw request outside retained partition/window proof is rejected");
    T_EQ(g_ioctl_submits, 0,
         "per-request retained-fd window failure is pre-submit");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    g_poll_available = 1;
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "default plan executes seventeen verifiable primary-file arms");
    T_EQ(g_default_all_lines, 1,
         "default selector clearly reports append exclusion and seventeen arms");
    T_EQ(g_tmpfile_calls, 1,
         "default selector creates no unverified append inode");
    T_EQ(g_verify_selected_all_lines, 1,
         "default oracle success reports all seventeen selected primary arms");
    T_EQ(g_verify_selected_zero_lines, 0,
         "default run cannot emit a selected=0 oracle success");
    T_EQ(g_zero_calls, 17,
         "all seventeen primary FS/block/PT arms receive zero prechecks");
    T_EQ(g_ioctl_submits, 1,
         "default plan executes its ioctl arm exactly once");
    T_EQ(g_decoded_raw_commands, 14,
         "default plan decodes every raw ioctl/uring command and split member");
    T_EQ(g_bad_command_encoding, 0,
         "all default raw commands stay inside their immediately checked intervals");
    T_EQ(g_readback_while_inflight, 0,
         "default mixed-ring oracle observes no accepted command in flight");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "-1", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "legacy EXITOS_ONLY=-1 is rejected, not treated as all arms");
    T_EQ(all_write_submits(), 0,
         "legacy campaign selection fails before every storage write");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_file_preexists = 1;
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "pre-existing nominal FSFILE cannot collide with anonymous inode");
    T_EQ(g_open_file_calls, 0,
         "pre-existing nominal FSFILE is never opened or overwritten");
    T_OK((g_open_file_flags & O_TMPFILE) == O_TMPFILE,
         "primary benchmark inode is created with O_TMPFILE");
    T_EQ(g_unlink_calls, 0,
         "pre-existing nominal FSFILE is never unlinked");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_file_nlink = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "linked inode cannot satisfy anonymous ownership proof");
    T_EQ(all_write_submits(), 0,
         "linked-inode anomaly is refused before initialization");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    g_fail_tmpfile = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "filesystem without O_TMPFILE support stays safety-frozen");
    T_EQ(all_write_submits(), 0,
         "O_TMPFILE failure occurs before the first initialization write");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "single raw sample initializes and maps its exact owned range");
    T_EQ((uint64_t)g_fallocate_len, 24576,
         "preallocation includes the selected slot and both 8 KiB guards");
    T_EQ(g_first_pwrite_len, 24576,
         "initialization writes both guards and every planned byte");
    T_EQ(g_first_pwrite_off, 0,
         "full-range initialization begins at file offset zero");
    T_EQ(g_fiemap_sync_seen, 1,
         "raw plan obtains its mapping with FIEMAP_FLAG_SYNC");
    T_EQ(g_open_whole_calls, 1, "whole namespace pathname is opened once");
    T_EQ(g_open_char_calls, 1, "generic-character pathname is opened once");
    T_EQ(g_open_part_calls, 1, "partition pathname is opened once");
    T_EQ(g_open_parent_calls, 1, "target directory pathname is opened once");
    T_EQ(g_tmpfile_calls, 1, "owned anonymous file is opened once");
    T_EQ(g_open_file_calls, 0, "owned file pathname is never opened");
    T_EQ(g_bad_command_encoding, 0,
         "raw command encodes the checked LBA, length, namespace and payload");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "14", 1);
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0,
         "retained whole-device pwrite arm completes on its owned extent");
    T_EQ(g_raw_block_pwrite_calls, 1,
         "raw-block arm issues exactly one timed pwrite to the retained whole fd");
    T_EQ(g_last_raw_block_len, 8192,
         "raw-block arm writes the selected 8 KiB command span");
    T_EQ(g_last_raw_block_off, (off_t)(UINT64_C(202) * 4096),
         "raw-block arm converts the FIEMAP-derived native LBA to an exact byte offset");
    T_EQ(g_zero_calls, 1,
         "raw-block pwrite is preceded by the same retained-fd zero recheck");
    T_EQ(g_bad_raw_block_write, 0,
         "raw-block write lies wholly inside the immediately checked zero interval");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "14", 1);
    g_mock_lbs = 512;
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "raw-block pwrite uses the retained device's 512-byte LBS");
    T_EQ(g_last_raw_block_off, (off_t)(UINT64_C(1616) * 512),
         "512-byte native LBA converts to the same exact physical byte offset");
    T_EQ(g_last_zero_count, 16,
         "512-byte raw-block request rechecks all sixteen native blocks");
    T_EQ(g_bad_raw_block_write, 0,
         "512-byte raw-block request matches its retained-fd zero proof");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "14", 1);
    g_short_raw_block_write = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "short raw-block pwrite fails the selected arm and process");
    T_EQ(g_raw_block_pwrite_calls, 1,
         "short raw-block pwrite is not retried or mislabeled successful");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "14", 1);
    g_fail_pwrite_call = 2; /* full initialization succeeds; first raw sample EIO */
    rc = qdsplit_program_main(2, argv2);
    T_OK(rc != 0, "raw-block pwrite -1/EIO fails the selected run and main");
    T_EQ(g_raw_block_pwrite_calls, 1,
         "raw-block EIO stops before the second planned raw write");
    T_EQ(g_zero_calls, 1,
         "raw-block EIO stops before a second zero-check/write pair");

    {
        int only = -2, pn = 0, pset[6] = {-1, -1, -1, -1, -1, -1};
        unsigned selected[N_ARM];
        unsigned selected_count = 0;

        rc = select_arms(NULL, "8kpath", &only, pset, &pn, selected);
        for (int a = 0; a < N_ARM; a++) selected_count += selected[a] != 0;
        T_EQ(rc, 0, "8kpath selector accepts its exact named contract");
        T_EQ(pn, 3, "8kpath selector returns exactly three arms");
        T_OK(pset[0] == 0 && pset[1] == 14 && pset[2] == 1,
             "8kpath ordered arms are exactly FS, raw-block, non-FUA passthrough");
        T_OK(only == -1 && selected_count == 3 && selected[0] &&
             selected[14] && selected[1] && !selected[2],
             "8kpath enables arm 1 and explicitly excludes its FUA twin arm 2");
    }

    {
        unsigned char base[64], other_iteration[64], other_arm[64], replay[64];
        fill_sample_pattern(base, sizeof base, 0, 0);
        fill_sample_pattern(other_iteration, sizeof other_iteration, 1, 0);
        fill_sample_pattern(other_arm, sizeof other_arm, 0, 1);
        fill_sample_pattern(replay, sizeof replay, 0, 0);
        T_OK(memcmp(base, other_iteration, sizeof base) != 0 &&
             memcmp(base, other_arm, sizeof base) != 0,
             "deterministic pattern identity includes both iteration and arm");
        T_OK(memcmp(base, replay, sizeof base) == 0,
             "recomputing one iteration/arm pattern is byte-for-byte stable");
    }

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_PAIRED", "8kpath", 1);
    rc = qdsplit_program_main(2, argv2);
    T_EQ(rc, 0,
         "two-iteration 8kpath interleaves filesystem, raw-block, and passthrough requests");
    T_EQ(g_raw_block_pwrite_calls, 2,
         "8kpath includes one ordinary raw-block request in each iteration");
    T_EQ(g_decoded_raw_commands, 2,
         "8kpath includes one passthrough NVMe command in each iteration");
    T_EQ(g_zero_calls, 6,
         "all six FS/block/PT samples perform retained-fd zero prechecks");
    T_EQ(g_bad_raw_block_write + g_bad_command_encoding, 0,
         "all three 8kpath cells stay inside their independent planned extents");
    T_EQ(g_io_event_overflow, 0,
         "8kpath ordered I/O oracle retains its complete event history");
    T_EQ(g_io_event_count, 12,
         "two 8kpath iterations emit six adjacent zero/write pairs");
    T_EQ(g_io_events[3].fua, 0,
         "iteration 0 passthrough command decodes with FUA clear");
    T_EQ(g_io_events[7].fua, 0,
         "iteration 1 passthrough command decodes with FUA clear");
    T_OK(event_is(0, MOCK_EVENT_ZERO, MOCK_WHOLE_FD, 202, 2, 0, 0) &&
         event_is(1, MOCK_EVENT_FS_WRITE, MOCK_FILE_FD, 0, 0, 8192, 8192) &&
         zero_is_fresh_for(0, 1),
         "iteration 0 filesystem cell has an adjacent mapped zero proof");
    T_OK(event_is(2, MOCK_EVENT_ZERO, MOCK_WHOLE_FD, 206, 2, 0, 0) &&
         event_is(3, MOCK_EVENT_PT_SUBMIT, MOCK_CHAR_FD, 206, 2, 0, 8192) &&
         zero_is_fresh_for(2, 3),
         "iteration 0 passthrough uses slot 2 with an immediately adjacent fresh zero proof");
    T_OK(event_is(4, MOCK_EVENT_ZERO, MOCK_WHOLE_FD, 204, 2, 0, 0) &&
         event_is(5, MOCK_EVENT_BLOCK_WRITE, MOCK_WHOLE_FD, 204, 2,
                  (off_t)(UINT64_C(204) * 4096), 8192) &&
         zero_is_fresh_for(4, 5),
         "iteration 0 block pwrite uses slot 1 with an immediately adjacent fresh zero proof");
    T_OK(event_is(6, MOCK_EVENT_ZERO, MOCK_WHOLE_FD, 212, 2, 0, 0) &&
         event_is(7, MOCK_EVENT_PT_SUBMIT, MOCK_CHAR_FD, 212, 2, 0, 8192) &&
         zero_is_fresh_for(6, 7),
         "iteration 1 passthrough uses slot 5 with an immediately adjacent fresh zero proof");
    T_OK(event_is(8, MOCK_EVENT_ZERO, MOCK_WHOLE_FD, 210, 2, 0, 0) &&
         event_is(9, MOCK_EVENT_BLOCK_WRITE, MOCK_WHOLE_FD, 210, 2,
                  (off_t)(UINT64_C(210) * 4096), 8192) &&
         zero_is_fresh_for(8, 9),
         "iteration 1 block pwrite uses slot 4 with an immediately adjacent fresh zero proof");
    T_OK(event_is(10, MOCK_EVENT_ZERO, MOCK_WHOLE_FD, 208, 2, 0, 0) &&
         event_is(11, MOCK_EVENT_FS_WRITE, MOCK_FILE_FD, 0, 0,
                  (off_t)(OWNED_GUARD_BYTES + UINT64_C(3) * 8192), 8192) &&
         zero_is_fresh_for(10, 11),
         "iteration 1 filesystem slot has an adjacent mapped zero proof");
    T_OK(g_io_event_count >= 12 &&
         g_io_events[1].off == (off_t)OWNED_GUARD_BYTES &&
         (g_io_events[5].lba - 200) * 4096 ==
             OWNED_GUARD_BYTES + UINT64_C(1) * 8192 &&
         (g_io_events[3].lba - 200) * 4096 ==
             OWNED_GUARD_BYTES + UINT64_C(2) * 8192 &&
         g_io_events[11].off ==
             (off_t)(OWNED_GUARD_BYTES + UINT64_C(3) * 8192) &&
         (g_io_events[9].lba - 200) * 4096 ==
             OWNED_GUARD_BYTES + UINT64_C(4) * 8192 &&
         (g_io_events[7].lba - 200) * 4096 ==
             OWNED_GUARD_BYTES + UINT64_C(5) * 8192,
         "FS/block/PT requests occupy six distinct exact planned slots");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_fiemap_too_many = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "FIEMAP count beyond supplied capacity fails closed");
    T_EQ(g_ioctl_submits, 0,
         "oversized FIEMAP result reaches no raw command");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "9", 1);
    g_fiemap_length_overflow = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "overflowing cumulative FIEMAP length fails closed");
    T_EQ(g_ioctl_submits, 0,
         "overflowed extent coverage reaches no raw command");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_PAIRED", "8k", 1);
    g_cross_extent = 1;
    g_cross_boundary = 20480; /* guarded raw slot [16384,24576) crosses */
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "later unsafe raw arm invalidates the whole paired plan");
    T_EQ(g_pwrite_calls, 1,
         "paired preflight stops before the earlier filesystem timed sample");
    T_EQ(g_uring_submits, 0,
         "paired cross-extent refusal performs no raw timed submission");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "1", 1);
    g_fail_setup_call = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "selected native uring arm propagates ring setup failure");
    T_EQ(all_write_submits(), 0,
         "native ring setup is preflighted before file initialization");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "10", 1);
    g_poll_available = 1;
    g_fail_setup_call = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "selected poll arm propagates poll-ring setup failure");
    T_EQ(all_write_submits(), 0,
         "poll ring setup failure occurs before file initialization");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "10", 1);
    g_poll_available = 1;
    g_cqe_result_override = 0x81;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "positive polled NVMe completion status fails the process");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("EXITOS_REQUIRE_SCATTERED", "1", 1);
    g_pagemap_mode = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "scattered contract rejects a physically contiguous base-page buffer");
    T_EQ(all_write_submits(), 0,
         "failed scattered proof occurs before initialization or timed writes");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("EXITOS_REQUIRE_SCATTERED", "1", 1);
    g_pagemap_mode = 2;
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "scattered contract accepts a proven multi-run base-page buffer");
    T_EQ(g_pagemap_reads, 2,
         "8 KiB scattered proof inspects exactly the two submitted pages");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("EXITOS_REQUIRE_SCATTERED", "1", 1);
    g_pagemap_mode = 3;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0,
         "a gap outside the submitted 8 KiB cannot prove it is scattered");
    T_EQ(all_write_submits(), 0,
         "out-of-span PFN gap is rejected before storage writes");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "6", 1);
    setenv("EXITOS_REQUIRE_SCATTERED", "1", 1);
    g_pagemap_mode = 2;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "one-page command cannot claim a scattered DMA layout");
    T_EQ(all_write_submits(), 0,
         "impossible 4 KiB scattered contract fails before storage writes");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("EXITOS_REQUIRE_CONTIG", "1", 1);
    g_pagemap_mode = 3;
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0,
         "8 KiB contiguous contract ignores a gap outside the submitted span");
    T_EQ(g_pagemap_reads, 2,
         "8 KiB contiguous proof inspects exactly the submitted pages");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("EXITOS_REQUIRE_SCATTERED", "0", 1);
    g_pagemap_mode = 2;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "false/noncanonical scattered marker is rejected");
    T_EQ(g_open_whole_calls, 0,
         "malformed scattered contract is rejected before device opens");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("EXITOS_REQUIRE_CONTIG", "yes", 1);
    g_pagemap_mode = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "noncanonical contiguous marker is rejected");
    T_EQ(g_open_whole_calls, 0,
         "malformed contiguous contract is rejected before device opens");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("EXITOS_REQUIRE_CONTIG", "1", 1);
    setenv("EXITOS_REQUIRE_SCATTERED", "1", 1);
    g_pagemap_mode = 2;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "contiguous and scattered contracts are mutually exclusive");
    T_EQ(g_open_whole_calls, 0,
         "ambiguous buffer contract is rejected before device opens");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    setenv("EXITOS_REQUIRE_SCATTERED", "1", 1);
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "unobservable PFNs cannot prove the scattered contract");
    T_EQ(all_write_submits(), 0,
         "unprovable scattered layout is refused before storage writes");

    /* --- SGL-avoidance arms --- */

    T_EQ(N_ARM, 20,
         "arm table retains eighteen historical arms and adds two production IOPOLL arms");
    if (N_ARM >= 18) {
        int only = -1, pset[6] = {0}, pn = 0;
        unsigned en[N_ARM];
        T_EQ(select_arms(NULL, "opt", &only, pset, &pn, en), 0,
             "EXITOS_PAIRED=opt is a recognized pairing");
        T_OK(pn == 5 && en[0] && en[1] && en[15] && en[16] && en[17],
             "opt pairing interleaves fs, pt baseline and the three new arms");
        T_OK(strcmp(arm_name[15], "pt-8k-fixedbuf") == 0 &&
             strcmp(arm_name[16], "pt-8k-bounce") == 0 &&
             strcmp(arm_name[17], "pt-8k-bounce-fixed") == 0,
             "new arm names are pt-8k-fixedbuf / pt-8k-bounce / pt-8k-bounce-fixed");
        T_OK(arm_is_raw(15) && arm_is_raw(16) && arm_is_raw(17),
             "new arms are raw arms and stay under the zero gate and guards");
        T_OK(arm_bytes(15, 8192) == 8192 && arm_command_span(16, 8192) == 8192 &&
             arm_command_span(17, 16384) == 16384,
             "new arms submit one big-sized command span");
    }

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "16", 1);
    g_pagemap_mode = 2;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "bounce arm refuses when the staging slot is not one physical run");
    T_EQ(all_write_submits(), 0,
         "scattered staging slot is refused before any storage write");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "16", 1);
    g_pagemap_mode = 1;
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "bounce arm passes the full run and its post-run oracle");
    T_EQ(g_decoded_raw_commands, 1, "bounce arm submits exactly one passthrough command");
    T_EQ(g_bad_command_encoding, 0, "bounce command encoding is valid");
    T_OK(g_mock_stage_region &&
         g_last_pt_addr >= (uint64_t)(uintptr_t)g_mock_stage_region &&
         g_last_pt_addr < (uint64_t)(uintptr_t)g_mock_stage_region + (4u << 20),
         "bounce arm submits from the staging slot, not the sample buffer");
    T_EQ(g_register_calls, 0, "bounce-only run registers no buffers");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "15", 1);
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "fixedbuf arm passes the full run and its post-run oracle");
    T_EQ(g_register_calls, 1, "fixedbuf arm registers buffers exactly once");
    T_EQ(g_register_bad, 0, "buffer registration targets the data ring with valid iovecs");
    T_EQ(g_fixed_submits, 1, "fixedbuf submit carries IORING_URING_CMD_FIXED");
    T_EQ(g_bad_fixed, 0, "fixed submit stays inside its registered iovec");
    T_EQ(g_last_fixed_buf_index, 0, "fixedbuf arm uses registered buffer index 0");
    T_OK(g_mock_stage_region == NULL,
         "fixedbuf-only run allocates no staging region");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "17", 1);
    g_pagemap_mode = 1;
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "bounce-fixed arm passes the full run and its post-run oracle");
    T_EQ(g_register_calls, 1, "bounce-fixed arm registers buffers exactly once");
    T_EQ(g_fixed_submits, 1, "bounce-fixed submit carries IORING_URING_CMD_FIXED");
    T_EQ(g_last_fixed_buf_index, 1, "bounce-fixed arm uses the slot iovec, index 1");
    T_EQ(g_bad_fixed, 0, "bounce-fixed submit stays inside the slot iovec");
    T_OK(g_mock_stage_region &&
         g_last_pt_addr >= (uint64_t)(uintptr_t)g_mock_stage_region &&
         g_last_pt_addr < (uint64_t)(uintptr_t)g_mock_stage_region + (4u << 20),
         "bounce-fixed arm submits from the staging slot");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "1", 1);
    rc = qdsplit_program_main(2, argv);
    T_EQ(rc, 0, "baseline passthrough arm still passes unchanged");
    T_EQ(g_register_calls, 0, "unselected fixed arms trigger no buffer registration");
    T_OK(g_mock_stage_region == NULL,
         "unselected bounce arms allocate no staging region");
    T_EQ(g_fixed_submits, 0, "baseline submit carries no fixed-buffer flag");

    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "17", 1);
    g_pagemap_mode = 1;
    g_fail_register = 1;
    rc = qdsplit_program_main(2, argv);
    T_OK(rc != 0, "failed buffer registration refuses the run");
    T_EQ(all_write_submits(), 0,
         "failed registration is refused before any storage write");

    /* --- Production polling arms --- */
    reset_case();
    setenv("EXITOS_SAMEFILE", "1", 1);
    setenv("EXITOS_ONLY", "0", 1);
    rc = qdsplit_program_main(2, argv2);
    T_EQ(rc, 0,
         "historical selector still completes with the legacy plan shape");
    T_EQ(g_preflight_entries, 36,
         "historical selector preflights exactly eighteen entries per iteration");
    T_OK(strcmp(g_timing_contract_line,
                "timing contract: fs/block/ioctl arms include call/command construction; "
                "uring SQE staging is outside t0 except pt-4kx2-seq; "
                "bounce arms charge their staging memcpy inside t0\n") == 0,
         "historical selector retains its timing-contract line byte-for-byte");
    T_OK(g_durability_probe_calls == 0 && g_durability_proof_lines == 0,
         "historical selector emits no production-only durability evidence");

#ifdef QDSPLIT_HAS_CHECKED_PLAN_COUNT
    {
        size_t checked = 0;
        T_OK(qdsplit_checked_plan_count(2, 18, &checked) == 0 && checked == 36,
             "checked plan count preserves ordinary legacy multiplication");
        checked = 73;
        T_OK(qdsplit_checked_plan_count(SIZE_MAX / 2 + 1, 2, &checked) ==
                 -EOVERFLOW && checked == 73,
             "checked plan count rejects size_t multiplication overflow before computing it");
        T_OK(qdsplit_checked_plan_count(SIZE_MAX, 1, &checked) == 0 &&
             checked == SIZE_MAX,
             "checked plan count accepts the largest representable exact product");
    }
#else
    T_OK(0, "checked plan count preserves ordinary legacy multiplication");
    T_OK(0,
         "checked plan count rejects size_t multiplication overflow before computing it");
    T_OK(0, "checked plan count accepts the largest representable exact product");
#endif

    {
        static const char *const legacy_names[18] = {
            "fs-8k-qd1", "pt-8k-qd1", "pt-8k-qd1-fua",
            "pt-4kx2-seq", "pt-4kx2-qd2", "pt-4kx2-qd2-fua",
            "pt-4k-qd1", "fs-4k-qd1", "fs-8k-uring", "pt-8k-ioctl",
            "pt-fua-poll", "fs-pwrite-fdatasync-prealloc",
            "fs-pwrite-fdatasync-append", "fs-8k-AA-control",
            "blk-8k-pwrite", "pt-8k-fixedbuf", "pt-8k-bounce",
            "pt-8k-bounce-fixed"
        };
        int only = -2, pn = 0, pset[6] = {-1, -1, -1, -1, -1, -1};
        /* Fixed upper bound keeps the pre-implementation RED build warning-free
         * while the old source still declares N_ARM == 18. */
        unsigned selected[20];
        unsigned legacy_ok = 1, selected_count = 0;
        int select_rc;

        for (int a = 0; a < 18 && a < N_ARM; a++)
            legacy_ok &= strcmp(arm_name[a], legacy_names[a]) == 0;
        T_OK(legacy_ok,
             "historical arm 0-17 names remain byte-for-byte unchanged");
        select_rc = select_arms(NULL, "blockpoll", &only, pset, &pn,
                                selected);
        T_EQ(select_rc, 0,
             "EXITOS_PAIRED=blockpoll is the explicit production-arm selector");
        if (select_rc == 0) {
            for (int a = 0; a < N_ARM; a++)
                selected_count += selected[a] != 0;
            T_OK(N_ARM >= 20 &&
                 strcmp(arm_name[18], "prod-pt-8k-iopoll") == 0 &&
                 strcmp(arm_name[19], "prod-blk-8k-iopoll") == 0,
                 "production arm names explicitly distinguish PT and block IOPOLL");
            T_OK(pn == 3 && pset[0] == 0 && pset[1] == 18 && pset[2] == 19 &&
                 only == -1 && selected_count == 3 && selected[0] &&
                 selected[18] && selected[19],
                 "blockpoll selects exactly FS, production PT, and production block");
            T_OK(!selected[1] && !selected[15] && !selected[16] && !selected[17],
                 "blockpoll excludes interrupt and SGL-diagnostic passthrough arms");
            T_OK(arm_is_raw(18) && arm_is_raw(19) &&
                 arm_bytes(18, 8192) == 8192 && arm_bytes(19, 8192) == 8192 &&
                 arm_command_span(18, 8192) == 8192 &&
                 arm_command_span(19, 8192) == 8192,
                 "both production arms retain the raw 8 KiB safety/span contract");
        }

        only = -2;
        pn = 0;
        memset(pset, -1, sizeof pset);
        T_EQ(select_arms(NULL, NULL, &only, pset, &pn, selected), 0,
             "no selector retains a valid historical default set");
        selected_count = 0;
        for (int a = 0; a < N_ARM; a++) selected_count += selected[a] != 0;
        T_OK(selected_count == 18 &&
             (N_ARM < 20 || (!selected[18] && !selected[19])),
             "no selector enables only historical arms 0-17");
    }

    if (N_ARM >= 20) {
        unsigned ordinal_count[3][3] = {{0}};
        int order_ok = 1, balance_ok = 1, distinct_ok = 1;

        {
            static const char *const offsets[] = {NULL, "0", "1", "2"};
            static const int expected[][3] = {
                {0, 18, 19}, {0, 18, 19}, {18, 19, 0}, {19, 0, 18}
            };
            static const char *const labels[] = {
                "unset", "zero", "one", "two"
            };

            for (size_t oi = 0; oi < sizeof offsets / sizeof offsets[0]; oi++) {
                reset_case();
                setenv("EXITOS_SAMEFILE", "1", 1);
                setenv("EXITOS_PAIRED", "blockpoll", 1);
                setenv("EXITOS_HUGEBUF", "1", 1);
                if (offsets[oi])
                    setenv("EXITOS_ORDER_OFFSET", offsets[oi], 1);
                rc = qdsplit_program_main(2, argv);
                T_OK(rc == 0 && timed_arm_at(0) == expected[oi][0],
                     "blockpoll %s order offset selects the exact first arm",
                     labels[oi]);
                T_OK(rc == 0 && g_io_event_count == 6 &&
                     timed_arm_at(0) == expected[oi][0] &&
                     timed_arm_at(1) == expected[oi][1] &&
                     timed_arm_at(2) == expected[oi][2],
                     "blockpoll %s order offset selects the complete rotated order",
                     labels[oi]);
            }
        }

        {
            static const char *const invalid_offsets[] = {
                "-1", "+1", "00", "3", " ", ""
            };

            for (size_t oi = 0;
                 oi < sizeof invalid_offsets / sizeof invalid_offsets[0]; oi++) {
                reset_case();
                setenv("EXITOS_SAMEFILE", "1", 1);
                setenv("EXITOS_PAIRED", "blockpoll", 1);
                setenv("EXITOS_ORDER_OFFSET", invalid_offsets[oi], 1);
                rc = qdsplit_program_main(2, argv);
                T_OK(rc != 0,
                     "blockpoll rejects noncanonical order offset case %zu", oi);
                T_OK(all_storage_opens() == 0 && all_write_submits() == 0 &&
                     g_stdout_calls == 0,
                     "invalid blockpoll order offset case %zu fails before opens, writes, or stdout",
                     oi);
            }
        }

        {
            struct inherited_case {
                const char *paired;
                const char *only;
                const char *offset;
                const char *label;
            };
            static const struct inherited_case inherited[] = {
                {NULL, NULL, "", "historical default with empty inherited offset"},
                {NULL, "0", "1", "historical EXITOS_ONLY with inherited offset"},
                {"8k", NULL, "2", "historical paired selector with inherited offset"}
            };

            for (size_t ci = 0; ci < sizeof inherited / sizeof inherited[0]; ci++) {
                reset_case();
                setenv("EXITOS_SAMEFILE", "1", 1);
                setenv("EXITOS_HUGEBUF", "1", 1);
                if (inherited[ci].paired)
                    setenv("EXITOS_PAIRED", inherited[ci].paired, 1);
                if (inherited[ci].only)
                    setenv("EXITOS_ONLY", inherited[ci].only, 1);
                setenv("EXITOS_ORDER_OFFSET", inherited[ci].offset, 1);
                rc = qdsplit_program_main(2, argv);
                T_OK(rc != 0,
                     "%s is rejected instead of silently ignoring the label",
                     inherited[ci].label);
                T_OK(all_storage_opens() == 0 && all_write_submits() == 0 &&
                     g_stdout_calls == 0,
                     "%s fails before any open, write, or historical stdout",
                     inherited[ci].label);
            }
        }

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        setenv("EXITOS_BIGSZ", "4096", 1);
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0,
             "production blockpoll selector rejects a non-8 KiB command size");
        T_OK(g_open_whole_calls == 0 && g_open_part_calls == 0 &&
             g_open_part_rw_calls == 0 && all_write_submits() == 0,
             "non-8 KiB blockpoll is rejected before device opens or writes");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_fail_durability_sysfs_readlink_call = 1;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_sysfs_readlink_calls == 1 &&
             g_durability_probe_calls == 0,
             "failure resolving retained dev_t to sysfs refuses before the durability probe");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_verify_success_lines == 0 && g_durability_proof_lines == 0,
             "unresolved retained durability identity reaches no initialization, write, or evidence");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        snprintf(g_durability_sysfs_link[0], sizeof g_durability_sysfs_link[0],
                 "../../devices/mock/block/");
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_sysfs_readlink_calls == 1 &&
             g_durability_probe_calls == 0,
             "malformed retained-devt sysfs target is refused before the durability probe");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_iopath_open_calls == 0 && g_verify_success_lines == 0 &&
             g_durability_proof_lines == 0,
             "malformed retained durability identity reaches no handle, write, or evidence");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_fail_durability_diskseq_call = 1;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_diskseq_calls == 1 &&
             g_durability_resolve_calls == 0 &&
             g_durability_sysfs_readlink_calls == 0 &&
             g_durability_probe_calls == 0,
             "initial retained-fd BLKGETDISKSEQ failure refuses before sysfs or probe");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_iopath_open_calls == 0 && g_verify_success_lines == 0 &&
             g_durability_proof_lines == 0,
             "initial fd-generation failure reaches no handle, write, or evidence");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_durability_sysfs_diskseq[0] = UINT64_C(72);
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_diskseq_calls == 1 &&
             g_durability_poll_bind_calls == 1 &&
             g_durability_resolve_calls == 0 &&
             g_durability_sysfs_readlink_calls == 0 &&
             g_durability_probe_calls == 0,
             "initial sysfs target diskseq must equal BLKGETDISKSEQ on the retained fd");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_iopath_open_calls == 0 && g_verify_success_lines == 0 &&
             g_durability_proof_lines == 0,
             "initial target-generation mismatch reaches no handle, write, or evidence");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_durability_resolve_major[0] = 8;
        g_durability_resolve_minor[0] = 1;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_diskseq_calls == 1 &&
             g_durability_resolve_calls == 1 &&
             g_durability_sysfs_readlink_calls == 0 &&
             g_durability_probe_calls == 0,
             "resolver-reported sysfs dev must equal the retained block fd rdev");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_iopath_open_calls == 0 && g_verify_success_lines == 0,
             "sysfs dev mismatch fails before initialization or timed writes");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_durability_resolve_is_partition[0] = 1;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_diskseq_calls == 1 &&
             g_durability_resolve_calls == 1 &&
             g_durability_sysfs_readlink_calls == 0 &&
             g_durability_probe_calls == 0,
             "durability sysfs binding refuses a resolver-reported partition object");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_iopath_open_calls == 0 && g_verify_success_lines == 0,
             "partition metadata cannot reach initialization or timed writes");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_fail_durability_diskseq_call = 2;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_diskseq_calls == 2 &&
             g_durability_resolve_calls == 1 &&
             g_durability_sysfs_readlink_calls == 1 &&
             g_durability_probe_calls == 1 &&
             g_durability_policy_calls == 0,
             "post-probe retained-fd BLKGETDISKSEQ failure invalidates the snapshot");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_iopath_open_calls == 0 && g_verify_success_lines == 0 &&
             g_durability_proof_lines == 0,
             "post-probe fd-generation failure emits no writes or durability evidence");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_durability_sysfs_diskseq[1] = UINT64_C(72);
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_diskseq_calls == 2 &&
             g_durability_poll_bind_calls == 2 &&
             g_durability_resolve_calls == 1 &&
             g_durability_sysfs_readlink_calls == 1 &&
             g_durability_probe_calls == 1 &&
             g_durability_policy_calls == 0,
             "post-probe sysfs diskseq reuse cannot borrow metadata for the old fd");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_iopath_open_calls == 0 && g_verify_success_lines == 0 &&
             g_durability_proof_lines == 0,
             "post-probe target-generation mismatch emits no writes or evidence");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_durability_diskseq[1] = UINT64_C(72);
        g_durability_sysfs_diskseq[1] = UINT64_C(72);
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_diskseq_calls == 2 &&
             g_durability_diskseq[0] != g_durability_diskseq[1] &&
             g_durability_poll_bind_calls == 2 &&
             g_durability_resolve_calls == 2 &&
             g_durability_sysfs_readlink_calls == 2 &&
             strcmp(g_durability_sysfs_link[0],
                    g_durability_sysfs_link[1]) == 0 &&
             g_durability_probe_calls == 1 &&
             g_durability_policy_calls == 0,
             "direct BLKGETDISKSEQ drift rejects a changed retained-fd generation");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_iopath_open_calls == 0 && g_verify_success_lines == 0 &&
             g_durability_proof_lines == 0,
             "fd-generation drift reaches no initialization, timed write, or evidence");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        snprintf(g_durability_sysfs_link[1],
                 sizeof g_durability_sysfs_link[1],
                 "../../devices/rebound/block/nvme-mock");
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_diskseq_calls == 2 &&
             g_durability_resolve_calls == 2 &&
             g_durability_sysfs_readlink_calls == 2 &&
             strcmp(g_durability_sysfs_link[0],
                    g_durability_sysfs_link[1]) != 0 &&
             g_durability_probe_calls == 1 &&
             g_durability_policy_calls == 0,
             "full retained-devt sysfs target drift is rejected even with one basename");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_iopath_open_calls == 0 && g_verify_success_lines == 0 &&
             g_durability_proof_lines == 0,
             "sysfs target drift reaches no initialization, timed write, or evidence");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_durability_probe_rc = -EIO;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_probe_calls == 1 &&
             g_durability_policy_calls == 0,
             "failed blockpoll durability probe refuses without inventing a policy");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_verify_success_lines == 0 && g_durability_proof_lines == 0,
             "durability probe failure precedes initialization, timed writes, and success evidence");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_durability_probe_vol = 1;
        g_durability_probe_fua = 1;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_probe_calls == 1 &&
             g_durability_policy_calls == 1 &&
             g_durability_policy_result == EXITOS_DUR_FUA,
             "volatile-cache FUA policy is refused by the write-completion-only matrix");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_verify_success_lines == 0 && g_durability_proof_lines == 0,
             "FUA durability mismatch stops before initialization or timed writes");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_durability_probe_vol = 1;
        g_durability_probe_fua = 0;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_durability_probe_calls == 1 &&
             g_durability_policy_calls == 1 &&
             g_durability_policy_result == EXITOS_DUR_FLUSH,
             "volatile-cache FLUSH policy is refused by the write-completion-only matrix");
        T_OK(g_pwrite_calls == 0 && all_write_submits() == 0 &&
             g_verify_success_lines == 0 && g_durability_proof_lines == 0,
             "FLUSH durability mismatch stops before initialization or timed writes");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_part_rw_rdev = makedev(259, 2);
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_iopath_open_calls == 0 && g_pwrite_calls == 0 &&
             all_write_submits() == 0 && g_verify_success_lines == 0,
             "partition RW rdev mismatch is rejected before handles, initialization, or writes");
        T_OK(g_close_part_calls == 1 && g_close_part_rw_calls == 1,
             "rdev mismatch closes both benchmark-owned partition fds once");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_part_rw_lbs = 512;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_iopath_open_calls == 0 && g_pwrite_calls == 0 &&
             all_write_submits() == 0 && g_verify_success_lines == 0,
             "partition RW LBS mismatch is rejected before handles, initialization, or writes");
        T_OK(g_close_part_calls == 1 && g_close_part_rw_calls == 1,
             "LBS mismatch closes both benchmark-owned partition fds once");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        setenv("EXITOS_HUGEBUF", "1", 1);
        rc = qdsplit_program_main(2, argv20);
        T_EQ(rc, 0,
             "twenty-iteration production blockpoll run passes the exact oracle");
        T_OK(g_durability_diskseq_calls == 2 &&
             g_durability_poll_bind_calls == 2 &&
             g_durability_resolve_calls == 2 &&
             g_durability_sysfs_readlink_calls == 2 &&
             g_durability_probe_calls == 1 &&
             strcmp(g_durability_probe_path,
                    "/sys/class/block/nvme-mock") == 0 &&
             g_durability_policy_calls == 1 &&
             g_durability_policy_vol == 0 && g_durability_policy_fua == 0 &&
             g_durability_policy_result == EXITOS_DUR_NONE &&
             g_durability_proof_lines == 1,
             "successful blockpoll binds one probe snapshot to one stable retained fd generation");
        T_OK(strcmp(g_timing_contract_line,
                    "timing contract: fs/block/ioctl arms include call/command construction; "
                    "uring SQE staging is outside t0 except pt-4kx2-seq; "
                    "bounce arms charge their staging memcpy inside t0; "
                    "production arms time one iopath_write completion\n") == 0,
             "blockpoll timing contract explicitly contains one production write completion");
        T_OK(g_iopath_open_calls == 2 &&
             g_iopath_open_fd_arg[0] == MOCK_WHOLE_FD &&
             g_iopath_open_backend[0] == IOPATH_URING_CMD_POLL &&
             g_iopath_open_lbs[0] == g_mock_lbs &&
             g_iopath_open_fd_arg[1] == MOCK_PART_RW_FD &&
             g_iopath_open_backend[1] == IOPATH_URING_WRITE_POLL &&
             g_iopath_open_lbs[1] == g_mock_lbs,
             "production handles open exact retained fds/backends/LBS without fallback");
        T_OK(g_iopath_exclusive_calls == 2 && g_bad_iopath_args == 0 &&
             g_first_exclusive_seq > 0 &&
             g_last_exclusive_seq < g_first_init_write_seq,
             "both QD1 exclusive setters succeed before file initialization");
        T_OK(g_open_part_rw_calls == 1 &&
             g_open_part_rw_flags ==
                 (unsigned)(O_RDWR | O_DIRECT | O_CLOEXEC | O_NOFOLLOW) &&
             g_open_part_calls == 1 &&
             g_open_part_flags ==
                 (unsigned)(O_RDONLY | O_DIRECT | O_CLOEXEC | O_NOFOLLOW),
             "block handle receives a distinct exact-partition RW direct fd");
        T_OK(g_iopath_write_calls == 40 && g_iopath_pt_writes == 20 &&
             g_iopath_block_writes == 20 &&
             g_iopath_write_handle[0] == MOCK_PROD_PT_HANDLE &&
             g_iopath_write_lba[0] == 204 &&
             g_iopath_write_handle[1] == MOCK_PROD_BLOCK_HANDLE &&
             g_iopath_write_lba[1] == 206 &&
             g_iopath_write_len[0] == 8192 &&
             g_iopath_write_len[1] == 8192 &&
             g_iopath_write_buf[0] != NULL &&
             g_iopath_write_buf[0] == g_iopath_write_buf[1],
             "production writes use exact handles, absolute FIEMAP LBAs, length, and caller buffer");
        T_OK(g_zero_calls == 60 && g_io_event_count == 120 &&
             g_io_event_overflow == 0,
             "all sixty blockpoll samples retain adjacent zero-window checks");
        if (g_io_event_count == 120) {
            for (unsigned i = 0; i < 20; i++) {
                uint64_t seen_lba[3] = {0, 0, 0};
                for (unsigned pos = 0; pos < 3; pos++) {
                    unsigned zi = i * 6 + pos * 2;
                    unsigned wi = zi + 1;
                    unsigned arm;
                    if (g_io_events[wi].kind == MOCK_EVENT_FS_WRITE) arm = 0;
                    else if (g_io_events[wi].kind == MOCK_EVENT_PROD_PT_WRITE) arm = 1;
                    else if (g_io_events[wi].kind == MOCK_EVENT_PROD_BLOCK_WRITE) arm = 2;
                    else { order_ok = 0; continue; }
                    ordinal_count[arm][pos]++;
                    seen_lba[pos] = g_io_events[zi].lba;
                    if (!zero_is_fresh_for(zi, wi)) order_ok = 0;
                }
                if (seen_lba[0] == seen_lba[1] ||
                    seen_lba[0] == seen_lba[2] ||
                    seen_lba[1] == seen_lba[2])
                    distinct_ok = 0;
            }
            order_ok &= g_io_events[1].kind == MOCK_EVENT_FS_WRITE &&
                        g_io_events[3].kind == MOCK_EVENT_PROD_PT_WRITE &&
                        g_io_events[5].kind == MOCK_EVENT_PROD_BLOCK_WRITE &&
                        g_io_events[7].kind == MOCK_EVENT_PROD_PT_WRITE &&
                        g_io_events[9].kind == MOCK_EVENT_PROD_BLOCK_WRITE &&
                        g_io_events[11].kind == MOCK_EVENT_FS_WRITE &&
                        g_io_events[13].kind == MOCK_EVENT_PROD_BLOCK_WRITE &&
                        g_io_events[15].kind == MOCK_EVENT_FS_WRITE &&
                        g_io_events[17].kind == MOCK_EVENT_PROD_PT_WRITE;
        } else {
            order_ok = distinct_ok = balance_ok = 0;
        }
        for (unsigned pos = 0; pos < 3; pos++) {
            unsigned min = ordinal_count[0][pos], max = min;
            for (unsigned arm = 1; arm < 3; arm++) {
                if (ordinal_count[arm][pos] < min) min = ordinal_count[arm][pos];
                if (ordinal_count[arm][pos] > max) max = ordinal_count[arm][pos];
            }
            if (min < 6 || max > 7 || max - min > 1) balance_ok = 0;
        }
        T_OK(order_ok,
             "blockpoll timed order rotates only inside its selected pset");
        T_OK(balance_ok,
             "twenty iterations balance every arm at each ordinal within one");
        T_OK(distinct_ok && g_fallocate_len == (off_t)507904,
             "production arms use distinct guarded 8 KiB slots");
        T_OK(g_verify_success_lines == 1 &&
             range_is_covered(g_file_read_covered, 0,
                              (uint64_t)g_fallocate_len) &&
             range_is_covered(g_raw_read_covered, 0,
                              (uint64_t)g_fallocate_len),
             "blockpoll success seal follows full file/raw/left/right-guard oracle");
        T_OK(g_iopath_close_calls == 2 && g_iopath_pt_close_calls == 1 &&
             g_iopath_block_close_calls == 1 &&
             g_close_whole_calls == 1 && g_close_part_calls == 1 &&
             g_close_part_rw_calls == 1 && g_close_char_calls == 1 &&
             g_close_file_calls == 1 && g_close_dir_calls == 1,
             "handles and every caller-owned retained fd close exactly once");
        T_OK(g_handle_close_with_caller_open == 2 &&
             g_last_handle_close_seq < g_first_retained_close_seq &&
             g_last_handle_close_seq < g_file_teardown_seq &&
             g_last_handle_close_seq < g_buffer_teardown_seq,
             "handles close while caller fds remain valid and before retained resources");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_fail_iopath_open_call = 1;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_iopath_open_calls == 1 &&
             g_iopath_exclusive_calls == 0,
             "failure opening the first production handle stops second-handle setup");
        T_OK(g_pwrite_calls == 0 && g_iopath_write_calls == 0 &&
             g_verify_success_lines == 0,
             "first-handle open failure precedes initialization and timed writes");
        T_OK(g_iopath_close_calls == 0 && g_close_whole_calls == 1 &&
             g_close_part_calls == 1 && g_close_part_rw_calls == 1,
             "first-handle open failure leaves no benchmark handle and closes retained fds once");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_fail_iopath_exclusive_call = 1;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_iopath_open_calls == 1 &&
             g_iopath_exclusive_calls == 1 && g_pwrite_calls == 0,
             "first exclusive declaration failure stops before block-handle setup or initialization");
        T_OK(g_iopath_pt_close_calls == 1 && g_iopath_block_close_calls == 0 &&
             g_iopath_write_calls == 0 && g_verify_success_lines == 0,
             "first exclusive failure closes the one successful benchmark handle without a seal");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_fail_iopath_open_call = 2;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_iopath_open_calls == 2 &&
             g_iopath_exclusive_calls == 1,
             "failure opening the second production handle fails closed");
        T_OK(g_pwrite_calls == 0 && g_iopath_write_calls == 0 &&
             g_verify_success_lines == 0,
             "second-handle open failure precedes initialization and timed writes");
        T_OK(g_iopath_pt_close_calls == 1 && g_iopath_block_close_calls == 0 &&
             g_close_whole_calls == 1 && g_close_part_rw_calls == 1,
             "second-handle open failure releases the first handle and retained fds once");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_fail_iopath_exclusive_call = 2;
        rc = qdsplit_program_main(2, argv);
        T_OK(rc != 0 && g_iopath_open_calls == 2 &&
             g_iopath_exclusive_calls == 2 && g_pwrite_calls == 0,
             "second exclusive declaration failure precedes file initialization");
        T_OK(g_iopath_pt_close_calls == 1 && g_iopath_block_close_calls == 1 &&
             g_iopath_write_calls == 0 && g_verify_success_lines == 0,
             "exclusive failure closes both partial-init handles without success seal");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_fail_iopath_write_call = 1;
        rc = qdsplit_program_main(2, argv2);
        T_OK(rc != 0 && g_iopath_write_calls == 1 &&
             g_iopath_pt_writes == 1 && g_iopath_block_writes == 0,
             "negative production PT write stops all subsequent timed writes");
        T_OK(g_iopath_close_calls == 2 && g_verify_success_lines == 0,
             "negative production write releases both handles without success seal");

        reset_case();
        setenv("EXITOS_SAMEFILE", "1", 1);
        setenv("EXITOS_PAIRED", "blockpoll", 1);
        g_short_iopath_write_call = 2;
        rc = qdsplit_program_main(2, argv2);
        T_OK(rc != 0 && g_iopath_write_calls == 2 &&
             g_iopath_pt_writes == 1 && g_iopath_block_writes == 1,
             "production -EIO short completion fails the block arm and stops iteration two");
        T_OK(g_iopath_close_calls == 2 && g_verify_success_lines == 0,
             "short completion frees both handles and cannot emit success seal");

        {
            struct signal_report report;
            memset(&report, 0, sizeof report);
            reset_case();
            setenv("EXITOS_SAMEFILE", "1", 1);
            setenv("EXITOS_PAIRED", "blockpoll", 1);
            g_signal_report = &report;
            g_signal_iopath_write_call = 1;
            rc = qdsplit_program_main(2, argv2);
            T_EQ(rc, 128 + SIGTERM,
                 "signal during production partial sample returns safe-stop status");
            T_OK(report.masked_at_raise == 1 &&
                 report.completion_seq > report.raise_seq &&
                 report.ring_close_seq > report.completion_seq,
                 "production completion becomes known before signal cleanup closes handles");
            T_OK(g_iopath_write_calls == 1 && g_iopath_pt_writes == 1 &&
                 g_iopath_block_writes == 0 && g_iopath_close_calls == 2 &&
                 g_verify_success_lines == 0,
                 "signal/partial sample stops later writes, frees handles, and forbids seal");
        }
    }

    T_DONE();
}
