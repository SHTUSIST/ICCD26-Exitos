/* Byte-for-byte correctness oracle for filesystem-bypass writes.
 *
 * This is intentionally a multi-process protocol.  `prepare` creates an
 * O_EXCL file and fixes its allocation state before an interposer can see the
 * fd.  `write` only opens that existing file and issues the measured writes.
 * `verify` then runs without interception and checks every byte through both a
 * buffered fd and an O_DIRECT fd.  Record 0 and record N+1 are sentinels, so a
 * correct payload at the wrong LBA cannot pass unnoticed.
 *
 *   readback prepare      FILE N init|noinit|append
 *   readback write        FILE N init|noinit|append
 *   readback verify       FILE N init|noinit|append
 *   readback raw-verify   FILE N init RAW-FD IMAGE-FD [PHYSICAL-DELTA]
 *   readback raw-fd-check RAW-FD IMAGE-FD
 *   readback raw-fd-detach RAW-FD IMAGE-FD
 *   readback association-gone LOOP-PATH LOOP-ID IMAGE-DEV IMAGE-INO
 *   readback damage       FILE N swap|half|guard
 *   readback cache-hold   FILE N READY RELEASE RESULT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define REC 8192U
#define MAP_EXTENTS 16U
#define MAX_RECORDS 1048576U

_Static_assert(MAX_RECORDS + 1U < (1U << 21),
               "record field must cover payloads and right guard");
_Static_assert(REC / sizeof(uint64_t) <= (1U << 10),
               "word-offset field must cover a complete record");

enum pattern_tag {
    PAT_BASE    = 1, /* bytes present before the intercepted process */
    PAT_PAYLOAD = 2, /* bytes written by the intercepted process      */
    PAT_GUARD_L = 3,
    PAT_GUARD_R = 4,
};

static void usage(const char *p)
{
    fprintf(stderr,
            "usage: %s prepare <file> <records> <init|noinit|append>\n"
            "       %s write <file> <records> <init|noinit|append>\n"
            "       %s verify <file> <records> <init|noinit|append>\n"
            "       %s raw-verify <file> <records> init <raw-fd> <image-fd> "
            "[physical-delta]\n"
            "       %s raw-fd-check <raw-fd> <image-fd>\n"
            "       %s raw-fd-detach <raw-fd> <image-fd>\n"
            "       %s association-gone <loop-path> <loop-id> <image-dev> "
            "<image-ino>\n"
            "       %s damage <file> <records> <swap|half|guard>\n"
            "       %s cache-hold <file> <records> <ready> <release> <result>\n",
            p, p, p, p, p, p, p, p, p);
}

static int parse_records(const char *s, size_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !*s || !out)
        return -1;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno || !end || *end || v == 0 || v > MAX_RECORDS)
        return -1;
    *out = (size_t)v;
    return 0;
}

static int parse_fd(const char *s, int *out)
{
    char *end = NULL;
    long v;

    if (!s || !*s || !out)
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || !end || *end || v < 0 || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_delta(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !*s || !out)
        return -1;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno || !end || *end || (v % 4096U) != 0 ||
        v > (unsigned long long)INT64_MAX)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_uint64_dec(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !*s || !out)
        return -1;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno || !end || *end)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_dev_hex(const char *s, dev_t *out)
{
    char *colon = NULL, *end = NULL;
    unsigned long maj, min;
    dev_t value;

    if (!s || !*s || !out)
        return -1;
    errno = 0;
    maj = strtoul(s, &colon, 16);
    if (errno || !colon || *colon != ':' || colon == s)
        return -1;
    errno = 0;
    min = strtoul(colon + 1, &end, 16);
    if (errno || !end || *end || end == colon + 1 ||
        maj > UINT_MAX || min > UINT_MAX)
        return -1;
    value = makedev((unsigned int)maj, (unsigned int)min);
    if (major(value) != maj || minor(value) != min)
        return -1;
    *out = value;
    return 0;
}

static int valid_mode(const char *mode)
{
    return mode && (!strcmp(mode, "init") || !strcmp(mode, "noinit") ||
                    !strcmp(mode, "append"));
}

static void fill_pattern(void *vbuf, enum pattern_tag tag, size_t index)
{
    unsigned char *buf = vbuf;
    size_t word;

    /* Every 64-bit word is an injective encoding of pattern class, record
     * number and word offset over the complete supported range.  In
     * particular, both 4 KiB halves distinguish every record; correctness
     * does not depend on a small text header or a byte-periodic tail. */
    for (word = 0; word < REC / sizeof(uint64_t); word++) {
        uint64_t value = UINT64_C(0xe100000000000000) |
                         ((uint64_t)tag << 52U) |
                         ((uint64_t)index << 31U) |
                         (uint64_t)word;

        for (unsigned int byte = 0; byte < sizeof(value); byte++)
            buf[word * sizeof(value) + byte] =
                (unsigned char)(value >> (byte * 8U));
    }
}

static void fill_initial_record(void *buf, size_t record, size_t n)
{
    if (record == 0)
        fill_pattern(buf, PAT_GUARD_L, 0);
    else if (record == n + 1)
        fill_pattern(buf, PAT_GUARD_R, n + 1);
    else
        fill_pattern(buf, PAT_BASE, record - 1);
}

static void fill_final_record(void *buf, size_t record, size_t n,
                              const char *mode)
{
    if (record == 0) {
        fill_pattern(buf, PAT_GUARD_L, 0);
    } else if (record == n + 1) {
        fill_pattern(buf, PAT_GUARD_R, n + 1);
    } else if (!strcmp(mode, "append") && record > n + 1) {
        fill_pattern(buf, PAT_PAYLOAD, record - (n + 2));
    } else if (!strcmp(mode, "append")) {
        fill_pattern(buf, PAT_BASE, record - 1);
    } else {
        fill_pattern(buf, PAT_PAYLOAD, record - 1);
    }
}

static int alloc_record(void **out)
{
    long page = sysconf(_SC_PAGESIZE);
    size_t align;

    if (!out || page <= 0)
        return -1;
    align = (size_t)page;
    if (align < 4096U)
        align = 4096U;
    if ((align & (align - 1U)) != 0) {
        fprintf(stderr, "unsupported page alignment %zu for %u-byte record\n",
                align, REC);
        return -1;
    }
    return posix_memalign(out, align, REC);
}

static int exact_pwrite_len(int fd, const void *buf, size_t len, off_t off,
                            const char *what)
{
    ssize_t rc = pwrite(fd, buf, len, off);

    if (rc == (ssize_t)len)
        return 0;
    if (rc < 0)
        fprintf(stderr, "%s pwrite at %jd failed: %s\n", what,
                (intmax_t)off, strerror(errno));
    else
        fprintf(stderr, "%s pwrite at %jd was short: %zd/%zu\n", what,
                (intmax_t)off, rc, len);
    return -1;
}

static int exact_pwrite(int fd, const void *buf, off_t off, const char *what)
{
    return exact_pwrite_len(fd, buf, REC, off, what);
}

static int exact_write(int fd, const void *buf, const char *what)
{
    ssize_t rc = write(fd, buf, REC);

    if (rc == (ssize_t)REC)
        return 0;
    if (rc < 0)
        fprintf(stderr, "%s write failed: %s\n", what, strerror(errno));
    else
        fprintf(stderr, "%s write was short: %zd/%u\n", what, rc, REC);
    return -1;
}

static int exact_pread(int fd, void *buf, size_t len, off_t off,
                       const char *what)
{
    ssize_t rc = pread(fd, buf, len, off);

    if (rc == (ssize_t)len)
        return 0;
    if (rc < 0)
        fprintf(stderr, "%s pread at %jd failed: %s\n", what,
                (intmax_t)off, strerror(errno));
    else
        fprintf(stderr, "%s pread at %jd was short: %zd/%zu\n", what,
                (intmax_t)off, rc, len);
    return -1;
}

static int close_checked(int fd, const char *what)
{
    if (close(fd) == 0)
        return 0;
    fprintf(stderr, "%s close failed: %s\n", what, strerror(errno));
    return -1;
}

static int check_size(int fd, off_t expected, const char *what)
{
    struct stat st;

    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "%s fstat failed: %s\n", what, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_size != expected) {
        fprintf(stderr, "%s size/type mismatch: size=%jd expected=%jd regular=%d\n",
                what, (intmax_t)st.st_size, (intmax_t)expected,
                S_ISREG(st.st_mode) ? 1 : 0);
        return -1;
    }
    return 0;
}

static int find_extent(int fd, uint64_t logical, uint64_t len,
                       struct fiemap_extent *out)
{
    size_t bytes = sizeof(struct fiemap) +
                   MAP_EXTENTS * sizeof(struct fiemap_extent);
    struct fiemap *fm = calloc(1, bytes);
    uint64_t end = logical + len;
    int found = -1;

    if (!fm)
        return -1;
    fm->fm_start = logical;
    fm->fm_length = len;
    fm->fm_flags = FIEMAP_FLAG_SYNC;
    fm->fm_extent_count = MAP_EXTENTS;
    if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0) {
        fprintf(stderr, "FIEMAP [0x%" PRIx64 ",+0x%" PRIx64 "] failed: %s\n",
                logical, len, strerror(errno));
        free(fm);
        return -1;
    }
    for (uint32_t i = 0; i < fm->fm_mapped_extents; i++) {
        struct fiemap_extent *fe = &fm->fm_extents[i];
        uint64_t fe_end = fe->fe_logical + fe->fe_length;
        if (fe->fe_logical <= logical && fe_end >= end) {
            *out = *fe;
            found = 0;
            break;
        }
    }
    free(fm);
    if (found != 0)
        fprintf(stderr, "FIEMAP has no single extent covering logical "
                        "[0x%" PRIx64 ",0x%" PRIx64 ")\n", logical, end);
    return found;
}

static uint32_t unsafe_extent_flags(void)
{
    uint32_t f = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
                 FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DATA_ENCRYPTED |
                 FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE |
                 FIEMAP_EXTENT_DATA_TAIL | FIEMAP_EXTENT_SHARED;
    return f;
}

static int check_prepared_extents(int fd, size_t n, int want_unwritten)
{
    struct fiemap_extent fe;
    size_t total = n + 2;

    for (size_t r = 0; r < total; r++) {
        int inner = r > 0 && r < n + 1;
        uint64_t at = (uint64_t)r * REC;
        uint64_t end = at + REC;

        /* A record may legitimately straddle two extents.  Prove complete
         * coverage and the state of every segment instead of requiring one
         * FIEMAP entry to cover all 8 KiB. */
        while (at < end) {
            uint64_t fe_end;
            if (find_extent(fd, at, 1, &fe) != 0)
                return -1;
            fe_end = fe.fe_logical + fe.fe_length;
            if (fe_end <= at) {
                fprintf(stderr, "FIEMAP made no progress at record %zu\n", r);
                return -1;
            }
            if (fe.fe_flags & unsafe_extent_flags()) {
                fprintf(stderr, "unsafe FIEMAP flags 0x%x at record %zu\n",
                        fe.fe_flags, r);
                return -1;
            }
            if (inner && want_unwritten) {
                if (!(fe.fe_flags & FIEMAP_EXTENT_UNWRITTEN)) {
                    fprintf(stderr, "record %zu is not UNWRITTEN "
                                    "(flags=0x%x)\n", r, fe.fe_flags);
                    return -1;
                }
            } else if (fe.fe_flags & FIEMAP_EXTENT_UNWRITTEN) {
                fprintf(stderr, "initialized record %zu remains UNWRITTEN "
                                "(flags=0x%x)\n", r, fe.fe_flags);
                return -1;
            }
            at = fe_end < end ? fe_end : end;
        }
    }
    return 0;
}

static int prepare_file(const char *path, size_t n, const char *mode)
{
    size_t total = n + 2;
    off_t bytes = (off_t)(total * REC);
    void *buf = NULL;
    int fd = -1, rc = 1;

    if (!valid_mode(mode)) {
        fprintf(stderr, "invalid prepare mode: %s\n", mode ? mode : "(null)");
        return 2;
    }
    if (alloc_record(&buf) != 0) {
        fprintf(stderr, "aligned allocation failed\n");
        return 1;
    }
    fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_DIRECT | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "O_EXCL prepare open failed: %s\n", strerror(errno));
        goto out;
    }
    if (fallocate(fd, 0, 0, bytes) != 0) {
        fprintf(stderr, "fallocate(%jd) failed: %s\n", (intmax_t)bytes,
                strerror(errno));
        goto out;
    }
    if (check_size(fd, bytes, "prepare") != 0)
        goto out;

    if (!strcmp(mode, "noinit")) {
        fill_initial_record(buf, 0, n);
        if (exact_pwrite(fd, buf, 0, "left guard") != 0)
            goto out;
        fill_initial_record(buf, n + 1, n);
        if (exact_pwrite(fd, buf, (off_t)(n + 1) * REC,
                         "right guard") != 0)
            goto out;
    } else {
        /* This full O_DIRECT prewrite is the eligibility precondition.  It is
         * done before the interposed process exists; no short write is hidden
         * by a retry loop. */
        for (size_t r = 0; r < total; r++) {
            fill_initial_record(buf, r, n);
            if (exact_pwrite(fd, buf, (off_t)r * REC,
                             "full initialization") != 0)
                goto out;
        }
    }
    if (fdatasync(fd) != 0) {
        fprintf(stderr, "prepare fdatasync failed: %s\n", strerror(errno));
        goto out;
    }
    if (check_prepared_extents(fd, n, !strcmp(mode, "noinit")) != 0)
        goto out;
    if (close_checked(fd, "prepare") != 0) {
        fd = -1;
        goto out;
    }
    fd = -1;
    printf("prepared=%zu bytes=%jd state=%s O_EXCL=1 O_DIRECT=1\n",
           total, (intmax_t)bytes, mode);
    rc = 0;
out:
    if (fd >= 0)
        (void)close(fd);
    free(buf);
    return rc;
}

static int write_file(const char *path, size_t n, const char *mode)
{
    size_t initial = n + 2;
    off_t initial_bytes = (off_t)(initial * REC);
    off_t final_bytes = !strcmp(mode, "append")
                      ? (off_t)((2 * n + 2) * REC) : initial_bytes;
    int flags = O_WRONLY | O_DIRECT | O_CLOEXEC;
    void *buf = NULL;
    int fd = -1, rc = 1;

    if (!valid_mode(mode)) {
        fprintf(stderr, "invalid write mode: %s\n", mode ? mode : "(null)");
        return 2;
    }
    if (!strcmp(mode, "append"))
        flags |= O_APPEND;
    if (alloc_record(&buf) != 0) {
        fprintf(stderr, "aligned allocation failed\n");
        return 1;
    }
    /* No O_CREAT and no allocation here: registration happens on this open,
     * after the prepare process has completely closed the file. */
    /* Pass the otherwise-unused mode argument deliberately.  On fortified
     * glibc, the two-argument spelling may be lowered to __open_2 instead of
     * open; this oracle must exercise the interposer entry point it names.
     * This is not a substitute for the frontend covering __open_2/__openat_2;
     * those entry points need their own interception tests. */
    fd = open(path, flags, 0);
    if (fd < 0) {
        fprintf(stderr, "open existing prepared file failed: %s\n",
                strerror(errno));
        goto out;
    }
    if (check_size(fd, initial_bytes, "write precondition") != 0)
        goto out;

    for (size_t i = 0; i < n; i++) {
        fill_pattern(buf, PAT_PAYLOAD, i);
        if (!strcmp(mode, "append")) {
            if (exact_write(fd, buf, "append payload") != 0)
                goto out;
        } else if (exact_pwrite(fd, buf, (off_t)(i + 1) * REC,
                                "payload") != 0) {
            goto out;
        }
    }
    if (fdatasync(fd) != 0) {
        fprintf(stderr, "write fdatasync failed: %s\n", strerror(errno));
        goto out;
    }
    if (check_size(fd, final_bytes, "write result") != 0)
        goto out;
    if (close_checked(fd, "write") != 0) {
        fd = -1;
        goto out;
    }
    fd = -1;
    printf("wrote=%zu mode=%s existing_file=1\n", n, mode);
    rc = 0;
out:
    if (fd >= 0)
        (void)close(fd);
    free(buf);
    return rc;
}

static int damage_file(const char *path, size_t n, const char *kind)
{
    void *buf = NULL;
    int fd = -1, rc = 1;

    if (strcmp(kind, "swap") != 0 && strcmp(kind, "half") != 0 &&
        strcmp(kind, "guard") != 0) {
        fprintf(stderr, "invalid damage kind: %s\n", kind);
        return 2;
    }
    if (!strcmp(kind, "swap") && n < 2) {
        fprintf(stderr, "swap damage requires at least two records\n");
        return 2;
    }
    if (alloc_record(&buf) != 0) {
        fprintf(stderr, "damage aligned allocation failed\n");
        return 1;
    }
    fd = open(path, O_RDWR | O_DIRECT | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "damage open failed: %s\n", strerror(errno));
        goto out;
    }
    if (check_size(fd, (off_t)(n + 2) * REC, "damage") != 0)
        goto out;

    if (!strcmp(kind, "swap")) {
        fill_pattern(buf, PAT_PAYLOAD, 1);
        if (exact_pwrite(fd, buf, REC, "swapped record 0") != 0)
            goto out;
        fill_pattern(buf, PAT_PAYLOAD, 0);
        if (exact_pwrite(fd, buf, 2U * REC, "swapped record 1") != 0)
            goto out;
    } else if (!strcmp(kind, "half")) {
        fill_pattern(buf, PAT_PAYLOAD, 0);
        if (exact_pwrite_len(fd, buf, REC / 2U, REC,
                             "first-half-only record") != 0)
            goto out;
    } else {
        fill_pattern(buf, PAT_PAYLOAD, 0);
        if (exact_pwrite(fd, buf, 0, "overwritten left guard") != 0)
            goto out;
    }
    if (fdatasync(fd) != 0) {
        fprintf(stderr, "damage fdatasync failed: %s\n", strerror(errno));
        goto out;
    }
    if (close_checked(fd, "damage") != 0) {
        fd = -1;
        goto out;
    }
    fd = -1;
    printf("damaged=%s records=%zu O_DIRECT=1\n", kind, n);
    rc = 0;
out:
    if (fd >= 0)
        (void)close(fd);
    free(buf);
    return rc;
}

static size_t report_mismatch(const char *channel, size_t record,
                              const unsigned char *expected,
                              const unsigned char *actual)
{
    size_t k, first = REC, mismatches = 0;

    for (k = 0; k < REC; k++) {
        if (expected[k] != actual[k]) {
            if (first == REC)
                first = k;
            mismatches++;
        }
    }
    if (mismatches)
        fprintf(stderr, "%s mismatch: record=%zu mismatched_bytes=%zu "
                        "first_record_byte=%zu first_file_byte=%zu "
                        "expected=%02x actual=%02x\n",
                channel, record, mismatches, first,
                record * (size_t)REC + first, expected[first], actual[first]);
    return mismatches;
}

static int verify_file(const char *path, size_t n, const char *mode)
{
    size_t total;
    off_t bytes;
    void *expected = NULL, *got = NULL;
    uint64_t bad_b_bytes = 0, bad_d_bytes = 0;
    size_t bad_b = 0, bad_d = 0;
    int fb = -1, fd = -1, rc = 1;

    if (!valid_mode(mode)) {
        fprintf(stderr, "invalid verify mode: %s\n", mode ? mode : "(null)");
        return 2;
    }
    total = !strcmp(mode, "append") ? 2 * n + 2 : n + 2;
    bytes = (off_t)(total * REC);
    if (alloc_record(&expected) != 0 || alloc_record(&got) != 0) {
        fprintf(stderr, "aligned allocation failed\n");
        goto out;
    }
    fb = open(path, O_RDONLY | O_CLOEXEC);
    fd = open(path, O_RDONLY | O_DIRECT | O_CLOEXEC);
    if (fb < 0 || fd < 0) {
        fprintf(stderr, "verify open failed: %s\n", strerror(errno));
        goto out;
    }
    if (check_size(fb, bytes, "verify") != 0)
        goto out;

    for (size_t r = 0; r < total; r++) {
        fill_final_record(expected, r, n, mode);
        if (exact_pread(fb, got, REC, (off_t)r * REC, "buffered") != 0)
            goto out;
        if (memcmp(expected, got, REC) != 0) {
            bad_b++;
            bad_b_bytes += report_mismatch("buffered", r, expected, got);
        }
        if (exact_pread(fd, got, REC, (off_t)r * REC, "O_DIRECT") != 0)
            goto out;
        if (memcmp(expected, got, REC) != 0) {
            bad_d++;
            bad_d_bytes += report_mismatch("O_DIRECT", r, expected, got);
        }
    }
    printf("records=%zu buffered_mismatch_records=%zu "
           "buffered_mismatch_bytes=%" PRIu64 " "
           "direct_mismatch_records=%zu direct_mismatch_bytes=%" PRIu64
           " guards=2\n", total, bad_b, bad_b_bytes, bad_d, bad_d_bytes);
    rc = (bad_b || bad_d) ? 1 : 0;
out:
    if (fb >= 0)
        (void)close(fb);
    if (fd >= 0)
        (void)close(fd);
    free(expected);
    free(got);
    return rc;
}

struct raw_identity {
    dev_t raw_rdev;
    dev_t image_dev;
    ino_t image_ino;
    uint64_t loop_offset;
    uint64_t loop_number;
    uint32_t loop_flags;
};

static int require_direct_fd(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags < 0) {
        fprintf(stderr, "raw fd F_GETFL failed: %s\n", strerror(errno));
        return -1;
    }
    if (!(flags & O_DIRECT) && fcntl(fd, F_SETFL, flags | O_DIRECT) != 0) {
        fprintf(stderr, "raw fd cannot enable O_DIRECT: %s\n",
                strerror(errno));
        return -1;
    }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || !(flags & O_DIRECT)) {
        fprintf(stderr, "raw fd did not retain O_DIRECT\n");
        return -1;
    }
    return 0;
}

static int capture_raw_identity(int raw_fd, int image_fd,
                                struct raw_identity *out,
                                const char *phase)
{
    struct loop_info64 li;
    struct stat raw_st, image_st;

    memset(&li, 0, sizeof(li));
    if (fstat(raw_fd, &raw_st) != 0 || fstat(image_fd, &image_st) != 0) {
        fprintf(stderr, "%s retained-fd fstat failed: %s\n", phase,
                strerror(errno));
        return -1;
    }
    if (!S_ISBLK(raw_st.st_mode) || !S_ISREG(image_st.st_mode)) {
        fprintf(stderr, "%s retained-fd type mismatch: raw_block=%d "
                        "image_regular=%d\n", phase,
                S_ISBLK(raw_st.st_mode) ? 1 : 0,
                S_ISREG(image_st.st_mode) ? 1 : 0);
        return -1;
    }
    if (ioctl(raw_fd, LOOP_GET_STATUS64, &li) != 0) {
        fprintf(stderr, "%s LOOP_GET_STATUS64 failed: %s\n", phase,
                strerror(errno));
        return -1;
    }
    if ((dev_t)li.lo_device != image_st.st_dev ||
        (ino_t)li.lo_inode != image_st.st_ino || li.lo_offset != 0) {
        fprintf(stderr, "%s loop backing mismatch: loop_dev=%ju "
                        "image_dev=%ju loop_ino=%ju image_ino=%ju "
                        "offset=%" PRIu64 "\n", phase,
                (uintmax_t)li.lo_device, (uintmax_t)image_st.st_dev,
                (uintmax_t)li.lo_inode, (uintmax_t)image_st.st_ino,
                (uint64_t)li.lo_offset);
        return -1;
    }
    out->raw_rdev = raw_st.st_rdev;
    out->image_dev = image_st.st_dev;
    out->image_ino = image_st.st_ino;
    out->loop_offset = li.lo_offset;
    out->loop_number = li.lo_number;
    out->loop_flags = li.lo_flags;
    return 0;
}

static int same_raw_identity(const struct raw_identity *a,
                             const struct raw_identity *b)
{
    return a->raw_rdev == b->raw_rdev &&
           a->image_dev == b->image_dev &&
           a->image_ino == b->image_ino &&
           a->loop_offset == b->loop_offset &&
           a->loop_number == b->loop_number &&
           a->loop_flags == b->loop_flags;
}

static int raw_fd_detach(int raw_fd, int image_fd)
{
    struct raw_identity before;
    struct loop_info64 li;
    struct stat raw_st, image_st;
    const char *state;

    if (require_direct_fd(raw_fd) != 0 ||
        capture_raw_identity(raw_fd, image_fd, &before,
                             "raw-fd-detach-before") != 0)
        return 1;
    if (ioctl(raw_fd, LOOP_CLR_FD, 0) != 0) {
        fprintf(stderr, "retained-fd LOOP_CLR_FD failed: %s\n",
                strerror(errno));
        return 1;
    }

    /* With another inherited reference open, modern kernels mark the loop
     * autoclear and complete teardown when the final fd is closed.  If it was
     * cleared immediately, LOOP_GET_STATUS64 returns ENXIO instead. */
    memset(&li, 0, sizeof(li));
    if (ioctl(raw_fd, LOOP_GET_STATUS64, &li) == 0) {
        if (fstat(raw_fd, &raw_st) != 0 || fstat(image_fd, &image_st) != 0) {
            fprintf(stderr, "retained-fd post-detach fstat failed: %s\n",
                    strerror(errno));
            return 1;
        }
        if (before.raw_rdev != raw_st.st_rdev ||
            before.image_dev != image_st.st_dev ||
            before.image_ino != image_st.st_ino ||
            (dev_t)li.lo_device != image_st.st_dev ||
            (ino_t)li.lo_inode != image_st.st_ino ||
            li.lo_offset != before.loop_offset ||
            li.lo_number != before.loop_number ||
            !(li.lo_flags & LO_FLAGS_AUTOCLEAR)) {
            fprintf(stderr, "LOOP_CLR_FD neither cleared nor armed autoclear "
                            "on the retained association\n");
            return 1;
        }
        state = "autoclear";
    } else if (errno == ENXIO) {
        state = "cleared";
    } else {
        fprintf(stderr, "cannot prove retained-fd detach state: %s\n",
                strerror(errno));
        return 1;
    }
    printf("detach_ioctl=LOOP_CLR_FD retained_raw_fd=1 backing_match=1 "
           "offset=0 state=%s\n", state);
    return 0;
}

static int association_gone(const char *loop_path, dev_t expected_rdev,
                            dev_t image_dev, ino_t image_ino)
{
    struct loop_info64 li;
    struct stat lst, st;
    int fd, saved_errno;

    if (lstat(loop_path, &lst) != 0 || !S_ISBLK(lst.st_mode) ||
        lst.st_rdev != expected_rdev) {
        fprintf(stderr, "association proof loop node identity mismatch\n");
        return 1;
    }
    fd = open(loop_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        saved_errno = errno;
        if (saved_errno == ENXIO || saved_errno == ENODEV) {
            printf("association_gone=1 loop_rdev=%ju state=unconfigured\n",
                   (uintmax_t)expected_rdev);
            return 0;
        }
        fprintf(stderr, "association proof open failed: %s\n",
                strerror(saved_errno));
        return 1;
    }
    if (fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode) ||
        st.st_rdev != expected_rdev) {
        fprintf(stderr, "association proof opened loop identity mismatch\n");
        (void)close(fd);
        return 1;
    }
    memset(&li, 0, sizeof(li));
    if (ioctl(fd, LOOP_GET_STATUS64, &li) != 0) {
        saved_errno = errno;
        (void)close(fd);
        if (saved_errno == ENXIO || saved_errno == ENODEV) {
            printf("association_gone=1 loop_rdev=%ju state=unconfigured\n",
                   (uintmax_t)expected_rdev);
            return 0;
        }
        fprintf(stderr, "association proof LOOP_GET_STATUS64 failed: %s\n",
                strerror(saved_errno));
        return 1;
    }
    if (close_checked(fd, "association proof") != 0)
        return 1;
    if ((dev_t)li.lo_device == image_dev && (ino_t)li.lo_inode == image_ino) {
        fprintf(stderr, "association remains: loop_rdev=%ju image_dev=%ju "
                        "image_ino=%ju offset=%" PRIu64 "\n",
                (uintmax_t)expected_rdev, (uintmax_t)image_dev,
                (uintmax_t)image_ino, (uint64_t)li.lo_offset);
        return 1;
    }
    printf("association_gone=1 loop_rdev=%ju state=reused-other-backing\n",
           (uintmax_t)expected_rdev);
    return 0;
}

static int raw_fd_check(int raw_fd, int image_fd)
{
    struct raw_identity before, after;

    if (require_direct_fd(raw_fd) != 0 ||
        capture_raw_identity(raw_fd, image_fd, &before, "raw-fd-check") != 0 ||
        capture_raw_identity(raw_fd, image_fd, &after,
                             "raw-fd-check-repeat") != 0)
        return 1;
    if (!same_raw_identity(&before, &after)) {
        fprintf(stderr, "raw-fd-check identity changed between checks\n");
        return 1;
    }
    printf("retained_raw_fd=1 O_DIRECT=1 backing_match=1 offset=%" PRIu64
           " loop_number=%" PRIu64 " raw_rdev=%ju image_dev=%ju "
           "image_ino=%ju\n",
           before.loop_offset, before.loop_number,
           (uintmax_t)before.raw_rdev, (uintmax_t)before.image_dev,
           (uintmax_t)before.image_ino);
    return 0;
}

static int raw_read_logical(int file_fd, int raw_fd, void *vbuf,
                            uint64_t logical, size_t len,
                            uint64_t physical_delta)
{
    unsigned char *buf = vbuf;
    size_t done = 0;

    while (done < len) {
        struct fiemap_extent fe;
        uint64_t at = logical + done;
        uint64_t fe_end, avail, phys;
        size_t chunk;

        if (find_extent(file_fd, at, 1, &fe) != 0)
            return -1;
        if (fe.fe_flags & (unsafe_extent_flags() | FIEMAP_EXTENT_UNWRITTEN)) {
            fprintf(stderr, "raw oracle unsafe extent flags 0x%x at 0x%" PRIx64
                            "\n", fe.fe_flags, at);
            return -1;
        }
        fe_end = fe.fe_logical + fe.fe_length;
        avail = fe_end - at;
        chunk = avail < len - done ? (size_t)avail : len - done;
        phys = fe.fe_physical + (at - fe.fe_logical);
        if (phys > UINT64_MAX - physical_delta ||
            phys + physical_delta > (uint64_t)INT64_MAX) {
            fprintf(stderr, "raw oracle physical offset overflow\n");
            return -1;
        }
        phys += physical_delta;
        if ((at % 4096U) || (phys % 4096U) || (chunk % 4096U)) {
            fprintf(stderr, "raw oracle unaligned mapping logical=0x%" PRIx64
                            " physical=0x%" PRIx64 " len=%zu\n", at, phys,
                    chunk);
            return -1;
        }
        if (exact_pread(raw_fd, buf + done, chunk, (off_t)phys,
                        "raw-device O_DIRECT") != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

static int raw_verify(const char *path, size_t n, const char *mode,
                      int raw_fd, int image_fd, uint64_t physical_delta)
{
    struct raw_identity before, after;
    struct stat fst, fst_after;
    size_t total = n + 2;
    void *expected = NULL, *got = NULL;
    uint64_t bad_bytes = 0;
    size_t bad = 0;
    int ff = -1, rc = 1;

    if (strcmp(mode, "init") != 0) {
        fprintf(stderr, "raw-verify currently requires init mode\n");
        return 2;
    }
    if (require_direct_fd(raw_fd) != 0 ||
        capture_raw_identity(raw_fd, image_fd, &before,
                             "raw-oracle-before") != 0)
        goto out;
    ff = open(path, O_RDONLY | O_CLOEXEC);
    if (ff < 0) {
        fprintf(stderr, "raw oracle file open failed: %s\n", strerror(errno));
        goto out;
    }
    if (fstat(ff, &fst) != 0) {
        fprintf(stderr, "raw oracle file fstat failed: %s\n", strerror(errno));
        goto out;
    }
    if (!S_ISREG(fst.st_mode) || fst.st_dev != before.raw_rdev) {
        fprintf(stderr, "raw oracle refuses device mismatch: file_dev=%ju "
                        "raw_rdev=%ju file_regular=%d\n",
                (uintmax_t)fst.st_dev, (uintmax_t)before.raw_rdev,
                S_ISREG(fst.st_mode) ? 1 : 0);
        goto out;
    }
    if (check_size(ff, (off_t)total * REC, "raw oracle") != 0)
        goto out;
    if (alloc_record(&expected) != 0 || alloc_record(&got) != 0) {
        fprintf(stderr, "raw oracle aligned allocation failed\n");
        goto out;
    }
    for (size_t r = 0; r < total; r++) {
        fill_final_record(expected, r, n, mode);
        if (raw_read_logical(ff, raw_fd, got, (uint64_t)r * REC, REC,
                             physical_delta) != 0)
            goto out;
        if (memcmp(expected, got, REC) != 0) {
            bad++;
            bad_bytes += report_mismatch("raw-device O_DIRECT", r,
                                         expected, got);
        }
    }
    if (capture_raw_identity(raw_fd, image_fd, &after,
                             "raw-oracle-after") != 0)
        goto out;
    if (fstat(ff, &fst_after) != 0) {
        fprintf(stderr, "raw oracle final file fstat failed: %s\n",
                strerror(errno));
        goto out;
    }
    if (!same_raw_identity(&before, &after)) {
        fprintf(stderr, "raw oracle retained identity changed while reading\n");
        goto out;
    }
    if (fst.st_dev != fst_after.st_dev || fst.st_ino != fst_after.st_ino ||
        fst_after.st_dev != after.raw_rdev) {
        fprintf(stderr, "raw oracle file/device identity changed while reading\n");
        goto out;
    }
    printf("records=%zu raw_direct_mismatch_records=%zu "
           "raw_direct_mismatch_bytes=%" PRIu64
           " guards=2 same_dev_t=1 retained_raw_fd=1 O_DIRECT=1 "
           "backing_match=1 offset=%" PRIu64 " physical_delta=%" PRIu64
           "\n", total, bad, bad_bytes, before.loop_offset, physical_delta);
    rc = bad ? 1 : 0;
out:
    if (ff >= 0)
        (void)close(ff);
    free(expected);
    free(got);
    return rc;
}

static int create_marker(const char *path, const char *text)
{
    size_t len = strlen(text);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    ssize_t wr;

    if (fd < 0) {
        fprintf(stderr, "create marker %s failed: %s\n", path,
                strerror(errno));
        return -1;
    }
    wr = write(fd, text, len);
    if (wr != (ssize_t)len) {
        if (wr < 0)
            fprintf(stderr, "write marker %s failed: %s\n", path,
                    strerror(errno));
        else
            fprintf(stderr, "write marker %s was short: %zd/%zu\n", path,
                    wr, len);
        (void)close(fd);
        return -1;
    }
    return close_checked(fd, "marker");
}

static int wait_for_regular(const char *path)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };

    for (unsigned int i = 0; i < 3000; i++) {
        struct stat st;
        if (lstat(path, &st) == 0)
            return S_ISREG(st.st_mode) ? 0 : -1;
        if (errno != ENOENT)
            return -1;
        (void)nanosleep(&delay, NULL);
    }
    errno = ETIMEDOUT;
    return -1;
}

static void classify(const unsigned char *got, const unsigned char *old,
                     const unsigned char *newv, unsigned int *old_n,
                     unsigned int *new_n, unsigned int *unexpected,
                     const char *channel, size_t record)
{
    if (memcmp(got, old, REC) == 0) {
        (*old_n)++;
    } else if (memcmp(got, newv, REC) == 0) {
        (*new_n)++;
    } else {
        (*unexpected)++;
        (void)report_mismatch(channel, record, old, got);
    }
}

static int cache_hold(const char *path, size_t n, const char *ready,
                      const char *release, const char *result)
{
    size_t total = n + 2;
    size_t bytes = total * (size_t)REC;
    unsigned char *map = MAP_FAILED;
    void *old = NULL, *newv = NULL, *got = NULL;
    unsigned int bo = 0, bn = 0, mo = 0, mn = 0, unexpected = 0;
    char line[256];
    int fd = -1, rc = 1;

    if (alloc_record(&old) != 0 || alloc_record(&newv) != 0 ||
        alloc_record(&got) != 0) {
        fprintf(stderr, "cache holder aligned allocation failed\n");
        goto out;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || check_size(fd, (off_t)bytes, "cache holder") != 0) {
        fprintf(stderr, "cache holder open/precondition failed: %s\n",
                strerror(errno));
        goto out;
    }
    map = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "cache holder mmap failed: %s\n", strerror(errno));
        goto out;
    }

    /* Fault and validate both access paths before signalling readiness. */
    for (size_t r = 0; r < total; r++) {
        fill_initial_record(old, r, n);
        if (exact_pread(fd, got, REC, (off_t)r * REC,
                        "cache-prime buffered") != 0 ||
            memcmp(got, old, REC) != 0 ||
            memcmp(map + r * (size_t)REC, old, REC) != 0) {
            fprintf(stderr, "cache holder baseline mismatch at record %zu\n", r);
            goto out;
        }
    }
    if (create_marker(ready, "ready\n") != 0)
        goto out;
    if (wait_for_regular(release) != 0) {
        fprintf(stderr, "cache holder release wait failed: %s\n",
                strerror(errno));
        goto out;
    }

    for (size_t r = 0; r < total; r++) {
        fill_initial_record(old, r, n);
        if (r == 0 || r == n + 1) {
            memcpy(newv, old, REC);
            if (exact_pread(fd, got, REC, (off_t)r * REC,
                            "cache-holder buffered guard") != 0)
                goto out;
            if (memcmp(got, old, REC) != 0 ||
                memcmp(map + r * (size_t)REC, old, REC) != 0) {
                unexpected++;
                fprintf(stderr, "cache holder guard changed at record %zu\n", r);
            }
            continue;
        } else {
            fill_pattern(newv, PAT_PAYLOAD, r - 1);
        }
        if (exact_pread(fd, got, REC, (off_t)r * REC,
                        "cache-holder buffered") != 0)
            goto out;
        classify(got, old, newv, &bo, &bn, &unexpected, "buffered-cache", r);
        classify(map + r * (size_t)REC, old, newv, &mo, &mn, &unexpected,
                 "mmap-cache", r);
    }
    (void)snprintf(line, sizeof line,
                   "buffered_old=%u buffered_new=%u mmap_old=%u mmap_new=%u "
                   "unexpected=%u\n", bo, bn, mo, mn, unexpected);
    if (create_marker(result, line) != 0)
        goto out;
    rc = unexpected ? 1 : 0;
out:
    if (map != MAP_FAILED)
        (void)munmap(map, bytes);
    if (fd >= 0)
        (void)close(fd);
    free(old);
    free(newv);
    free(got);
    return rc;
}

int main(int argc, char **argv)
{
    const char *op;
    const char *path;
    size_t n;

    if (argc == 4 && (!strcmp(argv[1], "raw-fd-check") ||
                      !strcmp(argv[1], "raw-fd-detach"))) {
        int raw_fd, image_fd;

        if (parse_fd(argv[2], &raw_fd) != 0 ||
            parse_fd(argv[3], &image_fd) != 0)
            goto bad_usage;
        if (!strcmp(argv[1], "raw-fd-check"))
            return raw_fd_check(raw_fd, image_fd);
        return raw_fd_detach(raw_fd, image_fd);
    }
    if (argc == 6 && !strcmp(argv[1], "association-gone")) {
        dev_t loop_rdev, image_dev;
        uint64_t image_dev_u64, image_ino_u64;

        if (parse_dev_hex(argv[3], &loop_rdev) != 0 ||
            parse_uint64_dec(argv[4], &image_dev_u64) != 0 ||
            parse_uint64_dec(argv[5], &image_ino_u64) != 0) {
            goto bad_usage;
        }
        image_dev = (dev_t)image_dev_u64;
        if ((uint64_t)image_dev != image_dev_u64 ||
            (uint64_t)(ino_t)image_ino_u64 != image_ino_u64)
            goto bad_usage;
        return association_gone(argv[2], loop_rdev, image_dev,
                                (ino_t)image_ino_u64);
    }

    if (argc < 4 || parse_records(argv[3], &n) != 0) {
        usage(argv[0]);
        return 2;
    }
    op = argv[1];
    path = argv[2];

    if (!strcmp(op, "prepare")) {
        if (argc != 5)
            goto bad_usage;
        return prepare_file(path, n, argv[4]);
    }
    if (!strcmp(op, "write")) {
        if (argc != 5)
            goto bad_usage;
        return write_file(path, n, argv[4]);
    }
    if (!strcmp(op, "verify")) {
        if (argc != 5)
            goto bad_usage;
        return verify_file(path, n, argv[4]);
    }
    if (!strcmp(op, "damage")) {
        if (argc != 5)
            goto bad_usage;
        return damage_file(path, n, argv[4]);
    }
    if (!strcmp(op, "raw-verify")) {
        int raw_fd, image_fd;
        uint64_t physical_delta = 0;

        if ((argc != 7 && argc != 8) ||
            parse_fd(argv[5], &raw_fd) != 0 ||
            parse_fd(argv[6], &image_fd) != 0 ||
            (argc == 8 && parse_delta(argv[7], &physical_delta) != 0))
            goto bad_usage;
        return raw_verify(path, n, argv[4], raw_fd, image_fd,
                          physical_delta);
    }
    if (!strcmp(op, "cache-hold")) {
        if (argc != 7)
            goto bad_usage;
        return cache_hold(path, n, argv[4], argv[5], argv[6]);
    }

bad_usage:
    usage(argv[0]);
    return 2;
}
