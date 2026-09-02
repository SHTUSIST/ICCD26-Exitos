#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "uring_cmd_batch.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "exitos_devguard.h"
#include "exitos_extent.h"

#ifndef FS_IOC_FIEMAP
#define FS_IOC_FIEMAP _IOWR('f', 11, struct fiemap)
#endif

#define FIEMAP_BATCH 256u

struct options {
    enum qdb_backend backend;
    const char *file;
    const char *partition;
    const char *char_device;
    const char *expect_identity;
    const char *output;
    uint64_t operations;
    unsigned qd;
    unsigned seen;
};

enum {
    SEEN_ARM = 1u << 0,
    SEEN_FILE = 1u << 1,
    SEEN_PARTITION = 1u << 2,
    SEEN_CHAR = 1u << 3,
    SEEN_QD = 1u << 4,
    SEEN_OPERATIONS = 1u << 5,
    SEEN_OUTPUT = 1u << 6,
    SEEN_IDENTITY = 1u << 7,
    SEEN_ALL = (1u << 8) - 1,
};

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "usage: %s --arm ext4|passthrough --file ABS "
            "--partition ABS --char-device ABS --expect-identity ID "
            "--qd 1|2|4|8 "
            "--operations N --output ABS\n", program);
}

static int parse_u64(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;
    if (!text || !*text || text[0] == '-') return -EINVAL;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || end == text || *end != '\0') return -EINVAL;
    *out = (uint64_t)value;
    return 0;
}

static int take_once(struct options *options, unsigned bit)
{
    if (options->seen & bit) return -EINVAL;
    options->seen |= bit;
    return 0;
}

static int parse_options(int argc, char **argv, struct options *options)
{
    int i;
    memset(options, 0, sizeof *options);
    for (i = 1; i < argc; i += 2) {
        const char *key, *value;
        uint64_t number;
        unsigned bit;
        if (i + 1 >= argc) return -EINVAL;
        key = argv[i];
        value = argv[i + 1];
        if (strcmp(key, "--arm") == 0) {
            bit = SEEN_ARM;
            if (strcmp(value, "ext4") == 0)
                options->backend = QDB_BACKEND_EXT4;
            else if (strcmp(value, "passthrough") == 0)
                options->backend = QDB_BACKEND_PASSTHROUGH;
            else return -EINVAL;
        } else if (strcmp(key, "--file") == 0) {
            bit = SEEN_FILE;
            options->file = value;
        } else if (strcmp(key, "--partition") == 0) {
            bit = SEEN_PARTITION;
            options->partition = value;
        } else if (strcmp(key, "--char-device") == 0) {
            bit = SEEN_CHAR;
            options->char_device = value;
        } else if (strcmp(key, "--expect-identity") == 0) {
            bit = SEEN_IDENTITY;
            options->expect_identity = value;
        } else if (strcmp(key, "--qd") == 0) {
            bit = SEEN_QD;
            if (parse_u64(value, &number) != 0 || number > UINT_MAX)
                return -EINVAL;
            options->qd = (unsigned)number;
        } else if (strcmp(key, "--operations") == 0) {
            bit = SEEN_OPERATIONS;
            if (parse_u64(value, &options->operations) != 0)
                return -EINVAL;
        } else if (strcmp(key, "--output") == 0) {
            bit = SEEN_OUTPUT;
            options->output = value;
        } else {
            return -EINVAL;
        }
        if (take_once(options, bit) != 0) return -EINVAL;
    }
    if (options->seen != SEEN_ALL || !qdb_qd_supported(options->qd) ||
        options->operations == 0 ||
        options->operations % options->qd != 0 ||
        !options->file || options->file[0] != '/' ||
        !options->partition || options->partition[0] != '/' ||
        !options->char_device || options->char_device[0] != '/' ||
        !options->expect_identity || options->expect_identity[0] == '\0' ||
        strchr(options->expect_identity, '\n') ||
        strchr(options->expect_identity, '\r') ||
        !options->output || options->output[0] != '/')
        return -EINVAL;
    return 0;
}

static int checked_file_size(uint64_t operations, uint64_t *out)
{
    if (operations > UINT64_MAX / QDB_BLOCK_SIZE - 2u)
        return -EOVERFLOW;
    *out = (operations + 2u) * QDB_BLOCK_SIZE;
    if (*out > (uint64_t)INT64_MAX) return -EOVERFLOW;
    return 0;
}

static int guard_is_zero(int fd, uint64_t offset, void *block)
{
    unsigned i;
    ssize_t got = pread(fd, block, QDB_BLOCK_SIZE, (off_t)offset);
    if (got != (ssize_t)QDB_BLOCK_SIZE) return -EIO;
    for (i = 0; i < QDB_BLOCK_SIZE; i++)
        if (((const unsigned char *)block)[i] != 0) return -EBADMSG;
    return 0;
}

static int map_passthrough_lbas(int fd, uint64_t operations,
                                uint64_t file_size, uint64_t partition_start,
                                uint64_t partition_count, uint32_t lbs,
                                uint64_t *lbas)
{
    struct fiemap *map = NULL;
    size_t map_bytes = sizeof(*map) +
                       FIEMAP_BATCH * sizeof(struct fiemap_extent);
    uint64_t logical = 0, data_begin = QDB_BLOCK_SIZE;
    uint64_t data_end = data_begin + operations * QDB_BLOCK_SIZE;
    uint64_t filled = 0;
    uint32_t flags = FIEMAP_FLAG_SYNC;
    int rc = -EIO;

    map = malloc(map_bytes);
    if (!map) return -ENOMEM;
    while (logical < file_size) {
        unsigned i;
        uint64_t previous = logical;
        int saw_last = 0;
        memset(map, 0, map_bytes);
        map->fm_start = logical;
        map->fm_length = file_size - logical;
        map->fm_flags = flags;
        map->fm_extent_count = FIEMAP_BATCH;
        if (ioctl(fd, FS_IOC_FIEMAP, map) != 0) {
            rc = -errno;
            goto out;
        }
        flags = 0;
        if (map->fm_mapped_extents == 0 ||
            map->fm_mapped_extents > FIEMAP_BATCH) {
            rc = -ENODATA;
            goto out;
        }
        for (i = 0; i < map->fm_mapped_extents; i++) {
            const struct fiemap_extent *extent = &map->fm_extents[i];
            uint64_t extent_end, overlap_begin, overlap_end, file_offset;
            if (extent->fe_flags & FIEMAP_EXTENT_LAST) saw_last = 1;
            if (!exitos_extent_flags_have_stable_location(extent->fe_flags) ||
                (extent->fe_flags & FIEMAP_EXTENT_UNWRITTEN) ||
                extent->fe_logical != logical || extent->fe_length == 0 ||
                extent->fe_length > UINT64_MAX - extent->fe_logical ||
                extent->fe_physical == 0 || extent->fe_physical % lbs != 0 ||
                extent->fe_logical % QDB_BLOCK_SIZE != 0 ||
                extent->fe_length % QDB_BLOCK_SIZE != 0) {
                rc = -ENOTSUP;
                goto out;
            }
            extent_end = extent->fe_logical + extent->fe_length;
            overlap_begin = extent->fe_logical > data_begin
                          ? extent->fe_logical : data_begin;
            overlap_end = extent_end < data_end ? extent_end : data_end;
            for (file_offset = overlap_begin; file_offset < overlap_end;
                 file_offset += QDB_BLOCK_SIZE) {
                uint64_t physical = extent->fe_physical +
                                    file_offset - extent->fe_logical;
                uint64_t lba, blocks = QDB_BLOCK_SIZE / lbs;
                uint64_t index = (file_offset - data_begin) / QDB_BLOCK_SIZE;
                if (physical < extent->fe_physical || physical % lbs != 0 ||
                    physical / lbs > UINT64_MAX - partition_start) {
                    rc = -EOVERFLOW;
                    goto out;
                }
                lba = partition_start + physical / lbs;
                if (lba < partition_start || lba > UINT64_MAX - blocks ||
                    partition_start > UINT64_MAX - partition_count ||
                    lba + blocks > partition_start + partition_count ||
                    index != filled || index >= operations) {
                    rc = -ERANGE;
                    goto out;
                }
                lbas[index] = lba;
                filled++;
            }
            logical = extent_end;
            if (logical >= file_size) break;
        }
        if (logical <= previous || (saw_last && logical < file_size)) {
            rc = -ENODATA;
            goto out;
        }
    }
    if (logical < file_size || filled != operations) {
        rc = -ENODATA;
        goto out;
    }
    rc = 0;
out:
    free(map);
    return rc;
}

static uint64_t elapsed_ns(const struct timespec *begin,
                           const struct timespec *end)
{
    return (uint64_t)(end->tv_sec - begin->tv_sec) * UINT64_C(1000000000) +
           (uint64_t)(end->tv_nsec - begin->tv_nsec);
}

static void json_string(FILE *stream, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    fputc('"', stream);
    while (*p) {
        if (*p == '"' || *p == '\\') fprintf(stream, "\\%c", *p);
        else if (*p < 0x20) fprintf(stream, "\\u%04x", *p);
        else fputc(*p, stream);
        p++;
    }
    fputc('"', stream);
}

static int write_json(const struct options *options,
                      const struct qdb_batch_stats *stats,
                      const struct stat *file_stat,
                      const struct stat *partition_stat,
                      const struct stat *char_stat,
                      uint32_t nsid, uint64_t partition_start,
                      uint64_t bytes, uint64_t nanos)
{
    int fd = open(options->output,
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    FILE *stream;
    const char *backend = options->backend == QDB_BACKEND_EXT4
                        ? "ext4-iopoll" : "nvme-uring-cmd-iopoll";
    const char *opcode = options->backend == QDB_BACKEND_EXT4
                       ? "IORING_OP_WRITE" : "IORING_OP_URING_CMD";
    double iops = nanos ? (double)options->operations * 1e9 / (double)nanos : 0;
    int rc = 0;

    if (fd < 0) return -errno;
    stream = fdopen(fd, "w");
    if (!stream) {
        rc = -errno;
        close(fd);
        return rc;
    }
    fprintf(stream, "{\n  \"schema_version\": 1,\n  \"threads\": 1,\n"
                    "  \"bs\": %u,\n  \"backend\": ", QDB_BLOCK_SIZE);
    json_string(stream, backend);
    fprintf(stream, ",\n  \"opcode\": ");
    json_string(stream, opcode);
    fprintf(stream,
            ",\n  \"iopoll\": true,\n  \"pattern_byte\": 90,\n"
            "  \"requested_qd\": %u,\n"
            "  \"max_submitted_batch\": %u,\n"
            "  \"submit_calls\": %" PRIu64 ",\n"
            "  \"submitted_commands\": %" PRIu64 ",\n"
            "  \"completed_commands\": %" PRIu64 ",\n"
            "  \"completion_errors\": %" PRIu64 ",\n"
            "  \"bytes\": %" PRIu64 ",\n"
            "  \"elapsed_ns\": %" PRIu64 ",\n"
            "  \"iops\": %.6f,\n  \"file_path\": ",
            options->qd, stats->max_submitted_batch, stats->submit_calls,
            stats->submitted_commands, stats->completed_commands,
            stats->completion_errors, bytes, nanos, iops);
    json_string(stream, options->file);
    fprintf(stream,
            ",\n  \"file_dev_major\": %u,\n"
            "  \"file_dev_minor\": %u,\n"
            "  \"file_inode\": %" PRIu64 ",\n  \"partition_path\": ",
            major(file_stat->st_dev), minor(file_stat->st_dev),
            (uint64_t)file_stat->st_ino);
    json_string(stream, options->partition);
    fprintf(stream,
            ",\n  \"partition_dev_major\": %u,\n"
            "  \"partition_dev_minor\": %u,\n"
            "  \"char_device_path\": ",
            major(partition_stat->st_rdev), minor(partition_stat->st_rdev));
    json_string(stream, options->char_device);
    fprintf(stream,
            ",\n  \"char_device_major\": %u,\n"
            "  \"char_device_minor\": %u,\n"
            "  \"namespace_id\": %u,\n"
            "  \"namespace_identity\": ",
            major(char_stat->st_rdev), minor(char_stat->st_rdev), nsid);
    json_string(stream, options->expect_identity);
    fprintf(stream,
            ",\n"
            "  \"partition_start_lba\": %" PRIu64 "\n}\n",
            partition_start);
    if (fflush(stream) != 0 || fsync(fd) != 0) rc = -EIO;
    if (fclose(stream) != 0 && rc == 0) rc = -EIO;
    return rc;
}

int main(int argc, char **argv)
{
    struct options options;
    struct stat file_stat, partition_stat, char_stat;
    struct devguard_device_info partition_info;
    struct qdb_batch_stats stats = {0};
    struct qdb_ring *ring = NULL;
    struct qdb_item items[QDB_RING_DEPTH];
    struct timespec begin, end;
    void *buffers = NULL, *guard = NULL;
    uint64_t *lbas = NULL;
    uint64_t file_size, partition_start, partition_count;
    uint64_t operation, bytes, nanos;
    uint32_t nsid, lbs;
    int file_fd = -1, partition_fd = -1, char_fd = -1;
    int ring_target, id, rc = 1, submit_rc;
    unsigned i;

    if (parse_options(argc, argv, &options) != 0) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (checked_file_size(options.operations, &file_size) != 0 ||
        options.operations > SIZE_MAX / sizeof(*lbas)) {
        fprintf(stderr, "operations overflow benchmark plan\n");
        return 2;
    }

    file_fd = open(options.file, O_RDWR | O_DIRECT | O_CLOEXEC);
    partition_fd = open(options.partition, O_RDONLY | O_CLOEXEC);
    char_fd = open(options.char_device, O_RDWR | O_CLOEXEC);
    if (file_fd < 0 || partition_fd < 0 || char_fd < 0) {
        perror("open benchmark targets");
        goto out;
    }
    if (fstat(file_fd, &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
        (uint64_t)file_stat.st_size != file_size ||
        fstat(partition_fd, &partition_stat) != 0 ||
        !S_ISBLK(partition_stat.st_mode) ||
        fstat(char_fd, &char_stat) != 0 || !S_ISCHR(char_stat.st_mode) ||
        file_stat.st_dev != partition_stat.st_rdev) {
        fprintf(stderr, "target type, size, or filesystem/partition binding mismatch\n");
        goto out;
    }
    id = ioctl(char_fd, NVME_IOCTL_ID);
    if (id <= 0 ||
        exitos_devguard_resolve_fd(partition_fd, &partition_info) != 0 ||
        !partition_info.is_partition ||
        strcmp(partition_info.identity, options.expect_identity) != 0 ||
        !exitos_devguard_nvme_pair_at("/sys", partition_info.parent_major,
                                     partition_info.parent_minor,
                                     major(char_stat.st_rdev),
                                     minor(char_stat.st_rdev), (uint32_t)id) ||
        !exitos_devguard_poll_available_fd(partition_fd) ||
        exitos_devguard_lba_span_fd(partition_fd, &partition_start,
                                    &partition_count) != 0) {
        fprintf(stderr, "namespace identity, pairing, NSID, partition span, or poll proof failed\n");
        goto out;
    }
    nsid = (uint32_t)id;
    lbs = partition_info.logical_block_size;
    if (lbs == 0 || QDB_BLOCK_SIZE % lbs != 0) {
        fprintf(stderr, "4KiB is not representable in native logical blocks\n");
        goto out;
    }
    if (posix_memalign(&guard, QDB_BLOCK_SIZE, QDB_BLOCK_SIZE) != 0 ||
        guard_is_zero(file_fd, 0, guard) != 0 ||
        guard_is_zero(file_fd, file_size - QDB_BLOCK_SIZE, guard) != 0) {
        fprintf(stderr, "front/back 4KiB guard is not zero\n");
        goto out;
    }

    if (options.backend == QDB_BACKEND_PASSTHROUGH) {
        lbas = calloc((size_t)options.operations, sizeof *lbas);
        if (!lbas || map_passthrough_lbas(file_fd, options.operations,
                                          file_size, partition_start,
                                          partition_count, lbs, lbas) != 0) {
            fprintf(stderr, "FIEMAP_SYNC did not prove full stable initialized coverage\n");
            goto out;
        }
    }
    if (posix_memalign(&buffers, QDB_BLOCK_SIZE,
                       QDB_RING_DEPTH * QDB_BLOCK_SIZE) != 0) {
        fprintf(stderr, "cannot allocate aligned QD buffers\n");
        goto out;
    }
    memset(buffers, 0x5a, QDB_RING_DEPTH * QDB_BLOCK_SIZE);

    ring_target = options.backend == QDB_BACKEND_PASSTHROUGH
                ? char_fd : file_fd;
    if (qdb_ring_open(&ring, options.backend, ring_target, nsid, lbs) != 0) {
        fprintf(stderr, "strict IOPOLL ring setup failed (no fallback)\n");
        goto out;
    }
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &begin) != 0) goto out;
    for (operation = 0; operation < options.operations;
         operation += options.qd) {
        for (i = 0; i < options.qd; i++) {
            uint64_t index = operation + i;
            items[i].buffer = (unsigned char *)buffers +
                              (size_t)i * QDB_BLOCK_SIZE;
            items[i].target = options.backend == QDB_BACKEND_PASSTHROUGH
                            ? lbas[index]
                            : QDB_BLOCK_SIZE + index * QDB_BLOCK_SIZE;
            items[i].token = index + 1;
        }
        submit_rc = qdb_ring_submit(ring, items, options.qd, &stats);
        if (submit_rc != 0) {
            fprintf(stderr,
                    "batch submit/completion failed (no fallback): rc=%d "
                    "error=%s submitted=%" PRIu64 " completed=%" PRIu64
                    " completion_errors=%" PRIu64 " last_token=%" PRIu64
                    " last_result=%d\n",
                    submit_rc,
                    submit_rc < 0 ? strerror(-submit_rc) : "invalid-status",
                    stats.submitted_commands, stats.completed_commands,
                    stats.completion_errors, stats.last_completion_token,
                    stats.last_completion_result);
            goto out;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &end) != 0) goto out;
    nanos = elapsed_ns(&begin, &end);
    bytes = options.operations * QDB_BLOCK_SIZE;
    if (nanos == 0 || stats.submit_calls != options.operations / options.qd ||
        stats.submitted_commands != options.operations ||
        stats.completed_commands != options.operations ||
        stats.completion_errors != 0 ||
        stats.max_submitted_batch != options.qd) {
        fprintf(stderr, "batch accounting invariant failed\n");
        goto out;
    }
    if (write_json(&options, &stats, &file_stat, &partition_stat, &char_stat,
                   nsid, partition_start, bytes, nanos) != 0) {
        perror("write result JSON");
        goto out;
    }
    rc = 0;
out:
    qdb_ring_close(ring);
    free(lbas);
    free(buffers);
    free(guard);
    if (char_fd >= 0) close(char_fd);
    if (partition_fd >= 0) close(partition_fd);
    if (file_fd >= 0) close(file_fd);
    return rc;
}
