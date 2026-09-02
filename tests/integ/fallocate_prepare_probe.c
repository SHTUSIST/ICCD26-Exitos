/* Minimal application/oracle for test_fallocate_prepare_e2e.sh.
 *
 * `workload` deliberately has the same descriptor shape as fio 3.41's default
 * setup followed by one 8 KiB psync operation: create the fresh file through
 * an O_WRONLY setup descriptor, fallocate(mode=0), close it, then reopen the
 * pathname O_RDWR|O_DIRECT for pwrite+fdatasync.  It knows nothing about
 * donors or either frontend.
 *
 * `verify` is a separate, ordinary-path complete readback.  It compares each
 * byte exactly once after the workload; it never computes a checksum/hash and
 * it performs no validation inside the write path.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FS_IOC_FIEMAP
#define FS_IOC_FIEMAP _IOWR('f', 11, struct fiemap)
#endif

#define PROBE_BATCH 128u
#define TIMED_BYTES 8192u
#define PATTERN_BYTE 0xa5u

#define PROBE_UNSAFE_FLAGS (FIEMAP_EXTENT_UNKNOWN        | \
                            FIEMAP_EXTENT_DELALLOC       | \
                            FIEMAP_EXTENT_ENCODED        | \
                            FIEMAP_EXTENT_DATA_ENCRYPTED | \
                            FIEMAP_EXTENT_NOT_ALIGNED    | \
                            FIEMAP_EXTENT_DATA_INLINE    | \
                            FIEMAP_EXTENT_DATA_TAIL      | \
                            FIEMAP_EXTENT_SHARED         | \
                            FIEMAP_EXTENT_UNWRITTEN)

#define NATIVE_DISALLOWED_FLAGS (PROBE_UNSAFE_FLAGS & \
                                 ~FIEMAP_EXTENT_UNWRITTEN)

struct map_summary {
    uint64_t mapped;
    uint32_t flags_or;
    unsigned extents;
    int complete;
    int all_unwritten;
    int physical_nonzero;
};

static int parse_bytes(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (!text || !*text || text[0] == '-' || text[0] == '+')
        return -1;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0 ||
        value > (unsigned long long)INT64_MAX)
        return -1;
    *out = (uint64_t)value;
    return 0;
}

static int add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b)
        return -1;
    *out = a + b;
    return 0;
}

/* Independent, synchronized FIEMAP coverage oracle.  It accepts physical
 * discontinuities, but never a logical gap, zero physical address, overflow,
 * or an unsafe extent flag. */
static int summarize_mapping(int fd, uint64_t bytes, struct map_summary *out)
{
    const size_t alloc_bytes = sizeof(struct fiemap) +
        (size_t)PROBE_BATCH * sizeof(struct fiemap_extent);
    struct fiemap *fm = calloc(1, alloc_bytes);
    uint64_t cursor = 0;
    int done = 0;

    if (!fm || !out) {
        free(fm);
        return -ENOMEM;
    }
    memset(out, 0, sizeof(*out));
    out->all_unwritten = 1;
    out->physical_nonzero = 1;

    while (cursor < bytes && !done) {
        unsigned i;

        memset(fm, 0, alloc_bytes);
        fm->fm_start = cursor;
        fm->fm_length = bytes - cursor;
        fm->fm_flags = FIEMAP_FLAG_SYNC;
        fm->fm_extent_count = PROBE_BATCH;
        if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0) {
            int rc = -(errno ? errno : EIO);
            free(fm);
            return rc;
        }
        if (fm->fm_mapped_extents == 0)
            break;
        if (fm->fm_mapped_extents > PROBE_BATCH) {
            free(fm);
            return -EIO;
        }

        for (i = 0; i < fm->fm_mapped_extents && cursor < bytes; ++i) {
            const struct fiemap_extent *fe = &fm->fm_extents[i];
            uint64_t extent_end, covered_end;

            if (fe->fe_length == 0 ||
                add_u64(fe->fe_logical, fe->fe_length, &extent_end) != 0 ||
                extent_end <= cursor || fe->fe_logical > cursor) {
                free(fm);
                return -ENXIO;
            }
            if (fe->fe_physical == 0)
                out->physical_nonzero = 0;
            out->flags_or |= fe->fe_flags;
            if ((fe->fe_flags & FIEMAP_EXTENT_UNWRITTEN) == 0)
                out->all_unwritten = 0;
            out->extents++;
            covered_end = extent_end < bytes ? extent_end : bytes;
            out->mapped += covered_end - cursor;
            cursor = covered_end;
            if (fe->fe_flags & FIEMAP_EXTENT_LAST)
                done = 1;
        }
    }

    out->complete = cursor == bytes;
    if (out->extents == 0)
        out->all_unwritten = 0;
    free(fm);
    return 0;
}

static int open_fresh_direct(const char *path)
{
    return open(path, O_RDWR | O_CREAT | O_EXCL | O_DIRECT | O_CLOEXEC, 0600);
}

static int open_fresh_fio_setup(const char *path)
{
    return open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
}

static int reopen_fio_io(const char *path)
{
    return open(path, O_RDWR | O_DIRECT | O_CLOEXEC);
}

static int native_probe(const char *path, uint64_t bytes)
{
    struct map_summary map;
    struct stat st;
    int fd, rc;

    fd = open_fresh_direct(path);
    if (fd < 0) {
        perror("native open");
        return 10;
    }
    if (fallocate(fd, 0, 0, (off_t)bytes) != 0) {
        perror("native fallocate");
        close(fd);
        return 11;
    }
    if (fstat(fd, &st) != 0) {
        perror("native fstat");
        close(fd);
        return 12;
    }
    dprintf(STDERR_FILENO,
            "NATIVE_ALLOCATED size=%" PRIu64 " allocated=%" PRIu64 "\n",
            (uint64_t)st.st_size, (uint64_t)st.st_blocks * UINT64_C(512));

    rc = summarize_mapping(fd, bytes, &map);
    if (rc != 0) {
        dprintf(STDERR_FILENO, "native FIEMAP failed: %s\n", strerror(-rc));
        close(fd);
        return 13;
    }
    dprintf(STDERR_FILENO,
            "NATIVE_FIEMAP mapped=%" PRIu64 " extents=%u flags=0x%x "
            "all_unwritten=%d complete=%d physical_nonzero=%d\n",
            map.mapped, map.extents, map.flags_or, map.all_unwritten,
            map.complete, map.physical_nonzero);
    close(fd);
    if ((uint64_t)st.st_size != bytes ||
        (uint64_t)st.st_blocks * UINT64_C(512) < bytes ||
        map.mapped != bytes || !map.complete || !map.physical_nonzero ||
        !map.all_unwritten)
        return 14;
    return 0;
}

/* Inspect a file that the real application (fio) already created.  This mode
 * deliberately opens read-only and performs no allocation or data write: it
 * is an independent oracle for the exact state left by fio's implicit
 * native-fallocate setup. */
static int inspect_existing_native_probe(const char *path, uint64_t bytes)
{
    struct map_summary map;
    struct stat st;
    uint32_t non_unwritten_unsafe;
    int fd, rc;

    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        perror("inspect-existing-native open");
        return 40;
    }
    if (fstat(fd, &st) != 0) {
        perror("inspect-existing-native fstat");
        close(fd);
        return 41;
    }
    if (!S_ISREG(st.st_mode)) {
        dprintf(STDERR_FILENO,
                "inspect-existing-native target is not a regular file\n");
        close(fd);
        return 42;
    }
    dprintf(STDOUT_FILENO,
            "NATIVE_EXISTING_ALLOCATED size=%" PRIu64
            " allocated=%" PRIu64 "\n",
            (uint64_t)st.st_size, (uint64_t)st.st_blocks * UINT64_C(512));

    rc = summarize_mapping(fd, bytes, &map);
    if (rc != 0) {
        dprintf(STDERR_FILENO,
                "inspect-existing-native FIEMAP failed: %s\n",
                strerror(-rc));
        close(fd);
        return 43;
    }
    non_unwritten_unsafe = map.flags_or & NATIVE_DISALLOWED_FLAGS;
    dprintf(STDOUT_FILENO,
            "NATIVE_EXISTING_FIEMAP mapped=%" PRIu64
            " extents=%u flags=0x%x all_unwritten=%d complete=%d "
            "physical_nonzero=%d non_unwritten_unsafe=0x%x\n",
            map.mapped, map.extents, map.flags_or, map.all_unwritten,
            map.complete, map.physical_nonzero, non_unwritten_unsafe);
    close(fd);

    if ((uint64_t)st.st_size != bytes ||
        (uint64_t)st.st_blocks * UINT64_C(512) < bytes ||
        map.mapped != bytes || !map.complete || !map.physical_nonzero ||
        !map.all_unwritten || non_unwritten_unsafe != 0)
        return 44;
    return 0;
}

static int workload_probe(const char *path, uint64_t bytes)
{
    struct map_summary map;
    unsigned char *payload = NULL;
    ssize_t wrote;
    int fd, flags, rc = 0;

    if (bytes < TIMED_BYTES || bytes % 4096u != 0)
        return 20;
    fd = open_fresh_fio_setup(path);
    if (fd < 0) {
        perror("workload setup open");
        return 21;
    }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_WRONLY) {
        dprintf(STDERR_FILENO,
                "workload setup descriptor is not O_WRONLY flags=0x%x\n",
                flags);
        close(fd);
        return 29;
    }
    dprintf(STDERR_FILENO,
            "APP_SETUP_OPEN_RETURN fd=%d access=wronly\n", fd);
    dprintf(STDERR_FILENO, "APP_FALLOCATE_CALL mode=0 off=0 len=%" PRIu64 "\n",
            bytes);
    if (fallocate(fd, 0, 0, (off_t)bytes) != 0) {
        perror("workload fallocate");
        close(fd);
        return 22;
    }
    dprintf(STDERR_FILENO,
            "APP_FALLOCATE_RETURN mode=0 off=0 len=%" PRIu64 "\n", bytes);
    if (close(fd) != 0) {
        perror("workload setup close");
        return 30;
    }
    dprintf(STDERR_FILENO, "APP_SETUP_CLOSE_RETURN rc=0\n");

    fd = reopen_fio_io(path);
    if (fd < 0) {
        perror("workload I/O reopen");
        return 31;
    }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDWR ||
        (flags & O_DIRECT) == 0) {
        dprintf(STDERR_FILENO,
                "workload I/O descriptor is not O_RDWR|O_DIRECT flags=0x%x\n",
                flags);
        close(fd);
        return 32;
    }
    dprintf(STDERR_FILENO,
            "APP_IO_OPEN_RETURN fd=%d access=rdwr direct=1\n", fd);

    rc = summarize_mapping(fd, bytes, &map);
    if (rc != 0) {
        dprintf(STDERR_FILENO, "workload FIEMAP failed: %s\n", strerror(-rc));
        close(fd);
        return 23;
    }
    dprintf(STDERR_FILENO,
            "APP_FIEMAP_SAFE mapped=%" PRIu64 " extents=%u complete=%d "
            "unsafe_flags=0x%x physical_nonzero=%d\n",
            map.mapped, map.extents, map.complete,
            map.flags_or & PROBE_UNSAFE_FLAGS, map.physical_nonzero);
    if (map.mapped != bytes || !map.complete || !map.physical_nonzero ||
        (map.flags_or & PROBE_UNSAFE_FLAGS) != 0) {
        close(fd);
        return 24;
    }

    if (posix_memalign((void **)&payload, 4096u, TIMED_BYTES) != 0 ||
        !payload) {
        close(fd);
        return 25;
    }
    memset(payload, PATTERN_BYTE, TIMED_BYTES);
    dprintf(STDERR_FILENO, "APP_TIMED_BEGIN bytes=%u\n", TIMED_BYTES);
    wrote = pwrite(fd, payload, TIMED_BYTES, 0);
    if (wrote != (ssize_t)TIMED_BYTES) {
        if (wrote < 0)
            perror("workload pwrite");
        else
            dprintf(STDERR_FILENO, "workload short pwrite: %zd\n", wrote);
        rc = 26;
        goto out;
    }
    dprintf(STDERR_FILENO, "APP_PWRITE_RETURN bytes=%zd\n", wrote);
    if (fdatasync(fd) != 0) {
        perror("workload fdatasync");
        rc = 27;
        goto out;
    }
    dprintf(STDERR_FILENO, "APP_FDATASYNC_RETURN rc=0\n");
out:
    free(payload);
    if (close(fd) != 0 && rc == 0) {
        perror("workload close");
        rc = 28;
    }
    return rc;
}

static int verify_probe(const char *path, uint64_t bytes)
{
    unsigned char *buffer;
    struct stat st;
    uint64_t done = 0, mismatch = 0;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("verify open");
        return 30;
    }
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size != bytes) {
        dprintf(STDERR_FILENO, "verify size mismatch\n");
        close(fd);
        return 31;
    }
    buffer = malloc(64u * 1024u);
    if (!buffer) {
        close(fd);
        return 32;
    }
    while (done < bytes) {
        size_t want = bytes - done > 64u * 1024u
                    ? 64u * 1024u : (size_t)(bytes - done);
        ssize_t got = pread(fd, buffer, want, (off_t)done);
        size_t i;

        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            dprintf(STDERR_FILENO, "verify short read at offset=%" PRIu64 "\n",
                    done);
            free(buffer);
            close(fd);
            return 33;
        }
        for (i = 0; i < (size_t)got; ++i) {
            unsigned char expected = done + i < TIMED_BYTES
                                   ? PATTERN_BYTE : 0;
            if (buffer[i] != expected)
                mismatch++;
        }
        done += (uint64_t)got;
    }
    free(buffer);
    close(fd);
    dprintf(STDERR_FILENO,
            "READBACK_OK bytes=%" PRIu64 " compared=%" PRIu64
            " mismatch=%" PRIu64 "\n",
            bytes, done, mismatch);
    return mismatch == 0 ? 0 : 34;
}

int main(int argc, char **argv)
{
    uint64_t bytes;

    if (argc != 4 || parse_bytes(argv[3], &bytes) != 0) {
        fprintf(stderr,
                "usage: %s {native|inspect-existing-native|workload|verify} "
                "PATH BYTES\n",
                argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "native") == 0)
        return native_probe(argv[2], bytes);
    if (strcmp(argv[1], "inspect-existing-native") == 0)
        return inspect_existing_native_probe(argv[2], bytes);
    if (strcmp(argv[1], "workload") == 0)
        return workload_probe(argv[2], bytes);
    if (strcmp(argv[1], "verify") == 0)
        return verify_probe(argv[2], bytes);
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
