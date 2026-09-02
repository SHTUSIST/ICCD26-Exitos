#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "exitos_devguard.h"
#include "exitos_iopath.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/sysmacros.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>

/* Device-guard control I/O can run while a frontend registration transaction
 * is guarded against signal reentry.  Use the explicit internal boundary;
 * unlike a TLS "next fd" token, a signal-handler open cannot consume it. */
static int devguard_internal_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT)
#ifdef O_TMPFILE
        || (flags & O_TMPFILE) == O_TMPFILE
#endif
        ) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return exitos_internal_open_call(path, flags, mode);
}

static int devguard_internal_openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT)
#ifdef O_TMPFILE
        || (flags & O_TMPFILE) == O_TMPFILE
#endif
        ) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return exitos_internal_openat_call(dirfd, path, flags, mode);
}

#define open   devguard_internal_open
#define openat devguard_internal_openat
#define close  exitos_internal_close_call

static void trim(char *s){ size_t n=strlen(s); while(n && (isspace((unsigned char)s[n-1]))) s[--n]=0; }

/* Read exactly one complete text line.  Returning a truncated prefix would let
 * two long serials/WWIDs collapse to the same pin identity, so overflow and
 * extra data are hard failures. */
static int read_line(const char *path, char *out, unsigned len){
    int fd;
    ssize_t got;
    size_t n;
    int rc = 0;

    if (!path || !out || len < 2 || len > (unsigned)INT_MAX)
        return -EINVAL;
    fd = exitos_internal_open_call(path, O_RDONLY | O_CLOEXEC, 0);
    if(fd < 0) return -errno;
    do { got = read(fd, out, len - 1); } while (got < 0 && errno == EINTR);
    if (got <= 0) { rc = got < 0 ? -errno : -EINVAL; goto done; }
    out[got] = '\0';
    n = strlen(out);
    if (n == 0) { rc = -EINVAL; goto done; }
    if (out[n-1] == '\n') {
        out[--n] = '\0';
        {
            char extra;
            do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
            if (got != 0) { rc = got < 0 ? -EIO : -EOVERFLOW; goto done; }
        }
    } else {
        char extra;
        do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
        if (got != 0) { rc = got < 0 ? -EIO : -EOVERFLOW; goto done; }
    }
    trim(out);
done:
    if (exitos_internal_close_call(fd) != 0 && rc == 0) rc = -EIO;
    if (rc != 0) out[0] = '\0';
    return rc;
}

static int read_line_at(int dirfd, const char *name, char *out, unsigned len)
{
    int fd;
    ssize_t got;
    size_t n;
    int rc = 0;

    if (dirfd < 0 || !name || !*name || strchr(name, '/') || !out || len < 2 ||
        len > (unsigned)INT_MAX)
        return -EINVAL;
    fd = exitos_internal_openat_call(dirfd, name,
                                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
    if (fd < 0) return -errno;
    do { got = read(fd, out, len - 1); } while (got < 0 && errno == EINTR);
    if (got <= 0) { rc = got < 0 ? -errno : -EINVAL; goto done; }
    out[got] = '\0';
    n = strlen(out);
    if (n == 0) { rc = -EINVAL; goto done; }
    if (out[n - 1] == '\n') {
        out[--n] = '\0';
        {
            char extra;
            do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
            if (got != 0) { rc = got < 0 ? -EIO : -EOVERFLOW; goto done; }
        }
    } else {
        char extra;
        do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
        if (got != 0) { rc = got < 0 ? -EIO : -EOVERFLOW; goto done; }
    }
    trim(out);
done:
    if (exitos_internal_close_call(fd) != 0 && rc == 0) rc = -EIO;
    if (rc != 0) out[0] = '\0';
    return rc;
}

static int parse_decimal_slice(const char *s, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (!s || n == 0 || !out) return 0;
    for (i = 0; i < n; i++) {
        uint64_t d;
        if (s[i] < '0' || s[i] > '9') return 0;
        d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / UINT64_C(10)) return 0;
        v = v * UINT64_C(10) + d;
    }
    *out = v;
    return 1;
}

static int parse_dev_text(const char *s, unsigned *maj_out, unsigned *min_out)
{
    const char *colon;
    uint64_t maj, min;
    if (!s || !maj_out || !min_out || !(colon = strchr(s, ':')) ||
        strchr(colon + 1, ':') ||
        !parse_decimal_slice(s, (size_t)(colon - s), &maj) ||
        !parse_decimal_slice(colon + 1, strlen(colon + 1), &min) ||
        maj > UINT_MAX || min > UINT_MAX)
        return 0;
    *maj_out = (unsigned)maj;
    *min_out = (unsigned)min;
    return 1;
}

static void devguard_why(char *why, unsigned wlen, const char *fmt, ...)
{
    va_list ap;
    if (!why || wlen == 0) return;
    va_start(ap, fmt);
    vsnprintf(why, wlen, fmt, ap);
    va_end(ap);
}

static int safe_device_path(const char *input, char *out, size_t out_len)
{
    size_t i, n;
    if (!input || !*input || !out || out_len == 0) return 0;
    if (strchr(input, '/')) {
        n = strlen(input);
        if (n >= out_len) return 0;
        memcpy(out, input, n + 1);
        return 1;
    }
    n = strlen(input);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)input[i];
        if (!(isalnum(c) || c == '-' || c == '_')) return 0;
    }
    return snprintf(out, out_len, "/dev/%s", input) >= 0 &&
           strlen(input) + sizeof "/dev/" <= out_len;
}

/* Compatibility by-name lookup is explicit (/dev/<name>) and immediately
 * resolves the opened object's rdev.  A caller-controlled basename never
 * selects sysfs metadata by itself. */
int exitos_devguard_identity(const char *base, char *out, unsigned len)
{
    struct devguard_device_info info;
    char path[PATH_MAX];
    int fd, rc;
    if (!out || len == 0 || !safe_device_path(base, path, sizeof path)) return -1;
    out[0] = '\0';
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    rc = exitos_devguard_resolve_fd(fd, &info);
    close(fd);
    if (rc != 0 || strlen(info.identity) >= len) return -1;
    memcpy(out, info.identity, strlen(info.identity) + 1);
    return 0;
}

static int devguard_sysfs_dev_path(char *out, size_t outsz,
                                   const char *root, const char *kind,
                                   unsigned maj, unsigned min)
{
    int n;
    if (!out || outsz == 0 || !root || !*root || !kind || !*kind)
        return 0;
    n = snprintf(out, outsz, "%s/dev/%s/%u:%u", root, kind, maj, min);
    return n >= 0 && (size_t)n < outsz;
}

static int sysfs_target_has_dev(const char *real, unsigned want_major,
                                unsigned want_minor)
{
    char path[PATH_MAX], line[64];
    unsigned got_major, got_minor;
    if (!real || snprintf(path, sizeof path, "%s/dev", real) >= (int)sizeof path ||
        read_line(path, line, sizeof line) != 0 ||
        !parse_dev_text(line, &got_major, &got_minor))
        return 0;
    return got_major == want_major && got_minor == want_minor;
}

static int devguard_sysfs_target(const char *root, unsigned maj, unsigned min,
                                 char *real, size_t real_len)
{
    char link[PATH_MAX];
    struct stat st;
    return devguard_sysfs_dev_path(link, sizeof link, root, "block", maj, min) &&
           real && real_len >= PATH_MAX && realpath(link, real) != NULL &&
           stat(real, &st) == 0 && S_ISDIR(st.st_mode) &&
           sysfs_target_has_dev(real, maj, min);
}

static int devguard_controller_identity(const char *owner,
                                        unsigned parent_major,
                                        unsigned parent_minor,
                                        char *controller,
                                        size_t controller_len)
{
    char path[PATH_MAX];
    const char *base;
    int n, rc;

    if (!owner || !controller || controller_len == 0) return -EINVAL;
    controller[0] = '\0';
    if (snprintf(path, sizeof path, "%s/device/serial", owner) >=
            (int)sizeof path)
        return -ENAMETOOLONG;
    rc = read_line(path, controller, (unsigned)controller_len);
    if (rc == -ENOENT) {
        if (snprintf(path, sizeof path, "%s/device/wwid", owner) >=
                (int)sizeof path)
            return -ENAMETOOLONG;
        rc = read_line(path, controller, (unsigned)controller_len);
    }
    if (rc == 0 && controller[0] != '\0') return 0;
    base = strrchr(owner, '/');
    base = base ? base + 1 : owner;
    if (rc == -ENOENT && strncmp(base, "loop", 4) == 0) {
        n = snprintf(controller, controller_len, "(loop:%u:%u)",
                     parent_major, parent_minor);
        return n >= 0 && (size_t)n < controller_len ? 0 : -EOVERFLOW;
    }
    return rc ? rc : -EINVAL;
}

static int devguard_block_identity(const char *owner,
                                   unsigned parent_major,
                                   unsigned parent_minor,
                                   char *identity, size_t identity_len,
                                   char *controller, size_t controller_len)
{
    char path[PATH_MAX], nsid_line[64];
    uint64_t nsid;
    int n, rc;

    if (!owner || !identity || identity_len == 0 || !controller ||
        controller_len == 0) return -EINVAL;
    identity[0] = '\0';
    rc = devguard_controller_identity(owner, parent_major, parent_minor,
                                      controller, controller_len);
    if (rc != 0) return rc;

    /* A namespace WWID is already controller- and namespace-specific. */
    if (snprintf(path, sizeof path, "%s/wwid", owner) >= (int)sizeof path)
        return -ENAMETOOLONG;
    rc = read_line(path, identity, (unsigned)identity_len);
    if (rc == 0) return identity[0] ? 0 : -EINVAL;
    if (rc != -ENOENT) return rc;

    /* NVMe controller serials are shared by all namespaces.  When sysfs
     * exposes an NSID, it is part of the identity rather than optional
     * metadata, so sibling namespaces cannot collapse to one pin. */
    if (snprintf(path, sizeof path, "%s/nsid", owner) >= (int)sizeof path)
        return -ENAMETOOLONG;
    rc = read_line(path, nsid_line, sizeof nsid_line);
    if (rc == 0) {
        if (!parse_decimal_slice(nsid_line, strlen(nsid_line), &nsid) ||
            nsid == 0 || nsid > UINT32_MAX)
            return -EINVAL;
        n = snprintf(identity, identity_len, "%s#nsid=%llu", controller,
                     (unsigned long long)nsid);
        return n >= 0 && (size_t)n < identity_len ? 0 : -EOVERFLOW;
    }
    if (rc != -ENOENT) return rc;

    if (strlen(controller) >= identity_len) return -EOVERFLOW;
    memcpy(identity, controller, strlen(controller) + 1);
    return 0;
}

static int devguard_resolve_real(const char *real, unsigned maj, unsigned min,
                                 struct devguard_device_info *out)
{
    struct devguard_device_info info;
    char owner[PATH_MAX], path[PATH_MAX], line[64];
    char identity[EXITOS_DEVGUARD_IDENTITY_MAX];
    char controller[EXITOS_DEVGUARD_IDENTITY_MAX];
    struct stat st;
    unsigned pmaj = maj, pmin = min;
    uint64_t lbs;
    int is_partition, rc;

    if (!real || !out || stat(real, &st) != 0 || !S_ISDIR(st.st_mode) ||
        !sysfs_target_has_dev(real, maj, min))
        return -ENODEV;
    if (snprintf(path, sizeof path, "%s/partition", real) >= (int)sizeof path)
        return -ENAMETOOLONG;
    if (lstat(path, &st) == 0) {
        is_partition = 1;
    } else if (errno == ENOENT) {
        is_partition = 0;
    } else {
        return -errno;
    }

    if (strlen(real) >= sizeof owner) return -ENAMETOOLONG;
    memcpy(owner, real, strlen(real) + 1);
    if (is_partition) {
        char *slash = strrchr(owner, '/');
        if (!slash || slash == owner) return -ENODEV;
        *slash = '\0';
        if (snprintf(path, sizeof path, "%s/dev", owner) >= (int)sizeof path ||
            read_line(path, line, sizeof line) != 0 ||
            !parse_dev_text(line, &pmaj, &pmin))
            return -ENODEV;
    }

    if (snprintf(path, sizeof path, "%s/queue/logical_block_size", owner) >=
            (int)sizeof path || read_line(path, line, sizeof line) != 0 ||
        !parse_decimal_slice(line, strlen(line), &lbs) || lbs == 0 ||
        lbs > UINT32_MAX || (lbs & (lbs - 1)) != 0)
        return -EINVAL;

    rc = devguard_block_identity(owner, pmaj, pmin, identity, sizeof identity,
                                 controller, sizeof controller);
    if (rc != 0) return rc;

    memset(&info, 0, sizeof info);
    memcpy(info.identity, identity, strlen(identity) + 1);
    memcpy(info.controller_serial, controller, strlen(controller) + 1);
    info.logical_block_size = (uint32_t)lbs;
    info.dev_major = maj;
    info.dev_minor = min;
    info.parent_major = pmaj;
    info.parent_minor = pmin;
    info.is_partition = is_partition;
    *out = info;
    return 0;
}

static int devguard_resolve_dev_at(const char *root, unsigned maj, unsigned min,
                                   struct devguard_device_info *out)
{
    char real[PATH_MAX];
    if (!out || !devguard_sysfs_target(root, maj, min, real, sizeof real))
        return -ENODEV;
    return devguard_resolve_real(real, maj, min, out);
}

int exitos_devguard_resolve_fd_at(int fd, const char *sysfs_root,
                                  struct devguard_device_info *out)
{
    struct stat st;
    if (fd < 0 || !sysfs_root || !*sysfs_root || !out) return -EINVAL;
    if (fstat(fd, &st) != 0) return -errno;
    if (!S_ISBLK(st.st_mode)) return -ENOTBLK;
    return devguard_resolve_dev_at(sysfs_root, major(st.st_rdev),
                                   minor(st.st_rdev), out);
}

int exitos_devguard_resolve_fd(int fd, struct devguard_device_info *out)
{
    return exitos_devguard_resolve_fd_at(fd, "/sys", out);
}

struct devguard_fd_binding {
    int target_fd;
    int parent_fd;
    int queue_fd;
    struct stat target_st;
    struct stat parent_st;
    struct stat queue_st;
    dev_t block_rdev;
    uint64_t diskseq;
    uint32_t logical_block_size;
    unsigned target_major;
    unsigned target_minor;
    unsigned parent_major;
    unsigned parent_minor;
    int is_partition;
    char target_link[PATH_MAX];
    char parent_link[PATH_MAX];
};

static void devguard_binding_init(struct devguard_fd_binding *b)
{
    memset(b, 0, sizeof *b);
    b->target_fd = -1;
    b->parent_fd = -1;
    b->queue_fd = -1;
}

static int devguard_binding_close(struct devguard_fd_binding *b)
{
    int rc = 0;
    if (b->queue_fd >= 0 && close(b->queue_fd) != 0) rc = -EIO;
    if (b->parent_fd >= 0 && close(b->parent_fd) != 0) rc = -EIO;
    if (b->target_fd >= 0 && close(b->target_fd) != 0) rc = -EIO;
    b->queue_fd = b->parent_fd = b->target_fd = -1;
    return rc;
}

static int devguard_same_dir(const char *path, const struct stat *bound)
{
    struct stat current;
    int current_fd, rc = 0;

    current_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current_fd < 0 || fstat(current_fd, &current) != 0 ||
        current.st_dev != bound->st_dev || current.st_ino != bound->st_ino)
        rc = -ESTALE;
    if (current_fd >= 0 && close(current_fd) != 0 && rc == 0) rc = -EIO;
    return rc;
}

static int devguard_read_partition(int target_fd, int *is_partition)
{
    char line[64];
    uint64_t number;
    int rc;

    rc = read_line_at(target_fd, "partition", line, sizeof line);
    if (rc == -ENOENT) {
        *is_partition = 0;
        return 0;
    }
    if (rc != 0 || !parse_decimal_slice(line, strlen(line), &number) ||
        number == 0 || number > UINT_MAX)
        return rc != 0 ? rc : -EINVAL;
    *is_partition = 1;
    return 0;
}

static int devguard_binding_open(int fd, const char *sysfs_root,
                                 struct devguard_fd_binding *b)
{
    struct stat block_st;
    char line[64];
    uint64_t lbs, parent_diskseq;
    unsigned got_major, got_minor;
    int rc;

    devguard_binding_init(b);
    if (fd < 0 || !sysfs_root || !*sysfs_root || fstat(fd, &block_st) != 0)
        return -EINVAL;
    if (!S_ISBLK(block_st.st_mode)) return -ENOTBLK;
    b->block_rdev = block_st.st_rdev;
    b->target_major = major(block_st.st_rdev);
    b->target_minor = minor(block_st.st_rdev);
    if (ioctl(fd, BLKGETDISKSEQ, &b->diskseq) != 0)
        return -errno;
    if (!devguard_sysfs_dev_path(b->target_link, sizeof b->target_link,
                                 sysfs_root, "block", b->target_major,
                                 b->target_minor))
        return -ENAMETOOLONG;
    b->target_fd = open(b->target_link,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (b->target_fd < 0 || fstat(b->target_fd, &b->target_st) != 0 ||
        !S_ISDIR(b->target_st.st_mode)) {
        rc = -ENODEV;
        goto fail;
    }
    rc = read_line_at(b->target_fd, "dev", line, sizeof line);
    if (rc != 0 || !parse_dev_text(line, &got_major, &got_minor) ||
        got_major != b->target_major || got_minor != b->target_minor) {
        rc = -ESTALE;
        goto fail;
    }
    rc = devguard_read_partition(b->target_fd, &b->is_partition);
    if (rc != 0) goto fail;
    b->parent_fd = openat(b->target_fd, b->is_partition ? ".." : ".",
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (b->parent_fd < 0 || fstat(b->parent_fd, &b->parent_st) != 0 ||
        !S_ISDIR(b->parent_st.st_mode)) {
        rc = -ENODEV;
        goto fail;
    }
    rc = read_line_at(b->parent_fd, "dev", line, sizeof line);
    if (rc != 0 || !parse_dev_text(line, &b->parent_major,
                                   &b->parent_minor) ||
        (!b->is_partition &&
         (b->parent_major != b->target_major ||
          b->parent_minor != b->target_minor))) {
        rc = -ESTALE;
        goto fail;
    }
    rc = read_line_at(b->parent_fd, "diskseq", line, sizeof line);
    if (rc != 0 ||
        !parse_decimal_slice(line, strlen(line), &parent_diskseq) ||
        parent_diskseq != b->diskseq) {
        rc = -ESTALE;
        goto fail;
    }
    if (!devguard_sysfs_dev_path(b->parent_link, sizeof b->parent_link,
                                 sysfs_root, "block", b->parent_major,
                                 b->parent_minor)) {
        rc = -ENAMETOOLONG;
        goto fail;
    }
    b->queue_fd = openat(b->parent_fd, "queue",
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (b->queue_fd < 0 || fstat(b->queue_fd, &b->queue_st) != 0 ||
        !S_ISDIR(b->queue_st.st_mode)) {
        rc = -ENODEV;
        goto fail;
    }
    rc = read_line_at(b->queue_fd, "logical_block_size", line, sizeof line);
    if (rc != 0 || !parse_decimal_slice(line, strlen(line), &lbs) || lbs == 0 ||
        lbs > UINT32_MAX || (lbs & (lbs - 1)) != 0) {
        rc = -EINVAL;
        goto fail;
    }
    b->logical_block_size = (uint32_t)lbs;
    rc = devguard_same_dir(b->target_link, &b->target_st);
    if (rc == 0) rc = devguard_same_dir(b->parent_link, &b->parent_st);
    if (rc != 0) goto fail;
    return 0;

fail:
    (void)devguard_binding_close(b);
    return rc;
}

static int devguard_binding_verify(int fd, struct devguard_fd_binding *b)
{
    struct stat block_st, current_queue;
    char line[64];
    uint64_t diskseq, lbs;
    unsigned got_major, got_minor;
    int current_queue_fd = -1, is_partition, rc;

    if (fstat(fd, &block_st) != 0 || !S_ISBLK(block_st.st_mode) ||
        block_st.st_rdev != b->block_rdev ||
        ioctl(fd, BLKGETDISKSEQ, &diskseq) != 0 || diskseq != b->diskseq)
        return -ESTALE;
    rc = read_line_at(b->target_fd, "dev", line, sizeof line);
    if (rc != 0 || !parse_dev_text(line, &got_major, &got_minor) ||
        got_major != b->target_major || got_minor != b->target_minor)
        return -ESTALE;
    rc = devguard_read_partition(b->target_fd, &is_partition);
    if (rc != 0 || is_partition != b->is_partition) return -ESTALE;
    rc = read_line_at(b->parent_fd, "dev", line, sizeof line);
    if (rc != 0 || !parse_dev_text(line, &got_major, &got_minor) ||
        got_major != b->parent_major || got_minor != b->parent_minor)
        return -ESTALE;
    rc = read_line_at(b->parent_fd, "diskseq", line, sizeof line);
    if (rc != 0 || !parse_decimal_slice(line, strlen(line), &diskseq) ||
        diskseq != b->diskseq)
        return -ESTALE;
    rc = read_line_at(b->queue_fd, "logical_block_size", line, sizeof line);
    if (rc != 0 || !parse_decimal_slice(line, strlen(line), &lbs) ||
        lbs != b->logical_block_size)
        return -ESTALE;
    current_queue_fd = openat(b->parent_fd, "queue",
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (current_queue_fd < 0 || fstat(current_queue_fd, &current_queue) != 0 ||
        current_queue.st_dev != b->queue_st.st_dev ||
        current_queue.st_ino != b->queue_st.st_ino) {
        rc = -ESTALE;
        goto out;
    }
    rc = devguard_same_dir(b->target_link, &b->target_st);
    if (rc == 0) rc = devguard_same_dir(b->parent_link, &b->parent_st);
out:
    if (current_queue_fd >= 0 && close(current_queue_fd) != 0 && rc == 0)
        rc = -EIO;
    return rc;
}

int exitos_devguard_lba_span_fd_at(int fd, const char *sysfs_root,
                                   uint64_t *base_lba, uint64_t *count_lba)
{
    struct devguard_fd_binding binding;
    char line[64];
    uint64_t start_sectors = 0, size_sectors, start_bytes, size_bytes;
    uint64_t base = 0, count = 0;
    int rc, close_rc;

    if (!sysfs_root || !*sysfs_root || !base_lba || !count_lba)
        return -EINVAL;
    rc = devguard_binding_open(fd, sysfs_root, &binding);
    if (rc != 0) return rc;
    if (binding.is_partition) {
        rc = read_line_at(binding.target_fd, "start", line, sizeof line);
        if (rc != 0 || !parse_decimal_slice(line, strlen(line), &start_sectors)) {
            rc = rc != 0 ? rc : -EINVAL;
            goto out;
        }
    }
    rc = read_line_at(binding.target_fd, "size", line, sizeof line);
    if (rc != 0 || !parse_decimal_slice(line, strlen(line), &size_sectors) ||
        size_sectors == 0) {
        rc = rc != 0 ? rc : -EINVAL;
        goto out;
    }
    if (start_sectors > UINT64_MAX / UINT64_C(512) ||
        size_sectors > UINT64_MAX / UINT64_C(512)) {
        rc = -EOVERFLOW;
        goto out;
    }
    start_bytes = start_sectors * UINT64_C(512);
    size_bytes = size_sectors * UINT64_C(512);
    if (start_bytes % binding.logical_block_size != 0 ||
        size_bytes % binding.logical_block_size != 0 ||
        start_bytes > UINT64_MAX - size_bytes) {
        rc = -EINVAL;
        goto out;
    }
    base = binding.is_partition ?
           start_bytes / binding.logical_block_size : 0;
    count = size_bytes / binding.logical_block_size;
    if (count == 0 || base > UINT64_MAX - count) {
        rc = -EOVERFLOW;
        goto out;
    }
    rc = devguard_binding_verify(fd, &binding);
out:
    close_rc = devguard_binding_close(&binding);
    if (rc == 0 && close_rc != 0) rc = close_rc;
    if (rc != 0) return rc;
    *base_lba = base;
    *count_lba = count;
    return 0;
}

int exitos_devguard_lba_span_fd(int fd, uint64_t *base_lba,
                                uint64_t *count_lba)
{
    return exitos_devguard_lba_span_fd_at(fd, "/sys", base_lba, count_lba);
}

int exitos_devguard_poll_available_fd_at(int fd, const char *sysfs_root)
{
    struct devguard_fd_binding binding;
    char line[64];
    int rc, close_rc, available = 0;

    rc = devguard_binding_open(fd, sysfs_root, &binding);
    if (rc != 0) return 0;
    if (read_line_at(binding.queue_fd, "io_poll", line, sizeof line) == 0 &&
        strcmp(line, "1") == 0 && devguard_binding_verify(fd, &binding) == 0)
        available = 1;
    close_rc = devguard_binding_close(&binding);
    if (close_rc != 0) available = 0;
    return available;
}

int exitos_devguard_poll_available_fd(int fd)
{
    return exitos_devguard_poll_available_fd_at(fd, "/sys");
}

static int sysfs_target_contains_dev(const char *root,
                                     unsigned target_major, unsigned target_minor,
                                     unsigned mounted_major, unsigned mounted_minor)
{
    char real[PATH_MAX], child[PATH_MAX], partition[PATH_MAX];
    char devpath[PATH_MAX], line[64];
    struct stat st;
    DIR *d;
    struct dirent *e;
    int result = 0;

    if (target_major == mounted_major && target_minor == mounted_minor) return 1;
    if (!devguard_sysfs_target(root, target_major, target_minor, real, sizeof real))
        return -1;
    if (snprintf(partition, sizeof partition, "%s/partition", real) >=
            (int)sizeof partition)
        return -1;
    if (lstat(partition, &st) == 0) return 0;
    if (errno != ENOENT) return -1;

    d = opendir(real);
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        unsigned cmaj, cmin;
        if (e->d_name[0] == '.') continue;
        if (snprintf(child, sizeof child, "%s/%s", real, e->d_name) >=
                (int)sizeof child) {
            result = -1;
            break;
        }
        if (stat(child, &st) != 0) {
            result = -1;
            break;
        }
        if (!S_ISDIR(st.st_mode))
            continue;
        if (snprintf(partition, sizeof partition, "%s/%s/partition", real,
                     e->d_name) >= (int)sizeof partition ||
            snprintf(devpath, sizeof devpath, "%s/%s/dev", real,
                     e->d_name) >= (int)sizeof devpath) {
            result = -1;
            break;
        }
        if (lstat(partition, &st) != 0) {
            if (errno == ENOENT) continue;
            result = -1;
            break;
        }
        if (read_line(devpath, line, sizeof line) != 0 ||
            !parse_dev_text(line, &cmaj, &cmin)) {
            result = -1;
            break;
        }
        if (cmaj == mounted_major && cmin == mounted_minor) {
            result = 1;
            break;
        }
    }
    if (closedir(d) != 0 && result == 0) result = -1;
    return result;
}

int exitos_devguard_mounted_at(const char *sysfs_root, const char *mountinfo_path,
                               unsigned dev_major, unsigned dev_minor)
{
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int result = 0, saw_record = 0;

    if (!sysfs_root || !*sysfs_root || !mountinfo_path || !*mountinfo_path)
        return -EINVAL;
    f = fopen(mountinfo_path, "r");
    if (!f) return -errno;
    while ((n = getline(&line, &cap, f)) >= 0) {
        char *save = NULL, *id, *parent, *dev;
        unsigned mmaj, mmin;
        int hit;
        if (n == 0 || line[n-1] != '\n' || memchr(line, '\0', (size_t)n) ||
            strstr(line, " - ") == NULL) { result = -EINVAL; break; }
        line[n-1] = '\0';
        id = strtok_r(line, " ", &save);
        parent = strtok_r(NULL, " ", &save);
        dev = strtok_r(NULL, " ", &save);
        if (!id || !parent || !dev ||
            !parse_decimal_slice(id, strlen(id), &(uint64_t){0}) ||
            !parse_decimal_slice(parent, strlen(parent), &(uint64_t){0}) ||
            !parse_dev_text(dev, &mmaj, &mmin)) {
            result = -EINVAL;
            break;
        }
        saw_record = 1;
        hit = sysfs_target_contains_dev(sysfs_root, dev_major, dev_minor,
                                        mmaj, mmin);
        if (hit < 0) { result = hit; break; }
        if (hit > 0) { result = 1; break; }
    }
    if (ferror(f) && result == 0) result = -EIO;
    if (result == 0 && !saw_record) result = -EINVAL;
    free(line);
    if (fclose(f) != 0 && result == 0) result = -EIO;
    return result;
}

int exitos_devguard_whole_block_at(const char *sysfs_root,
                                   unsigned dev_major, unsigned dev_minor)
{
    char link[PATH_MAX], real[PATH_MAX], partition[PATH_MAX];
    struct stat st;

    if (!devguard_sysfs_dev_path(link, sizeof link, sysfs_root, "block",
                                 dev_major, dev_minor) ||
        !realpath(link, real) || stat(real, &st) != 0 || !S_ISDIR(st.st_mode) ||
        !sysfs_target_has_dev(real, dev_major, dev_minor))
        return 0;
    if (snprintf(partition, sizeof partition, "%s/partition", real) >=
        (int)sizeof partition)
        return 0;
    if (lstat(partition, &st) == 0)
        return 0;
    /* Only ENOENT means the partition attribute is absent.  Permission and
     * traversal errors leave the identity unprovable and therefore refuse. */
    return errno == ENOENT ? 1 : 0;
}

static int parse_u32_line(const char *path, uint32_t *out)
{
    char line[64], *end;
    unsigned long v;
    if (!out || read_line(path, line, sizeof line) != 0 || line[0] == '\0')
        return 0;
    errno = 0;
    end = NULL;
    v = strtoul(line, &end, 0);
    if (errno != 0 || end == line || *end != '\0' || v == 0 || v > UINT32_MAX)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

int exitos_devguard_nvme_pair_at(const char *sysfs_root,
                                 unsigned block_major, unsigned block_minor,
                                 unsigned char_major, unsigned char_minor,
                                 uint32_t char_nsid)
{
    char block[PATH_MAX], chr[PATH_MAX];
    char char_real[PATH_MAX];
    char block_device[PATH_MAX], char_device[PATH_MAX];
    char block_ctrl[PATH_MAX], char_ctrl[PATH_MAX], nsid_path[PATH_MAX];
    uint32_t block_nsid;

    if (char_nsid == 0 ||
        !exitos_devguard_whole_block_at(sysfs_root, block_major, block_minor) ||
        !devguard_sysfs_dev_path(block, sizeof block, sysfs_root, "block",
                                 block_major, block_minor) ||
        !devguard_sysfs_dev_path(chr, sizeof chr, sysfs_root, "char",
                                 char_major, char_minor) ||
        !realpath(chr, char_real) ||
        !sysfs_target_has_dev(char_real, char_major, char_minor))
        return 0;

    /* Linux creates the block namespace below the controller and creates the
     * nvme-generic character device as another child of that controller.  The
     * two realpaths are therefore siblings, not parent/child.  Their `device`
     * links canonically identify the controller; the namespace identity comes
     * from the block nsid plus NVME_IOCTL_ID below. */
    if (snprintf(block_device, sizeof block_device, "%s/device", block) >=
            (int)sizeof block_device ||
        snprintf(char_device, sizeof char_device, "%s/device", chr) >=
            (int)sizeof char_device ||
        !realpath(block_device, block_ctrl) || !realpath(char_device, char_ctrl) ||
        strcmp(block_ctrl, char_ctrl) != 0)
        return 0;

    if (snprintf(nsid_path, sizeof nsid_path, "%s/nsid", block) >=
            (int)sizeof nsid_path ||
        !parse_u32_line(nsid_path, &block_nsid))
        return 0;
    return block_nsid == char_nsid;
}

int exitos_devguard_nvme_pair(const char *block_path, const char *char_path)
{
    struct stat bs, cs;
    int bfd, cfd, char_nsid, paired = 0;

    if (!block_path || !*block_path || !char_path || !*char_path)
        return 0;
    bfd = open(block_path, O_RDONLY | O_CLOEXEC);
    if (bfd < 0)
        return 0;
    if (fstat(bfd, &bs) != 0 || !S_ISBLK(bs.st_mode)) {
        close(bfd);
        return 0;
    }
    cfd = open(char_path, O_RDONLY | O_CLOEXEC);
    if (cfd < 0) {
        close(bfd);
        return 0;
    }
    if (fstat(cfd, &cs) != 0 || !S_ISCHR(cs.st_mode)) {
        close(cfd);
        close(bfd);
        return 0;
    }
    char_nsid = ioctl(cfd, NVME_IOCTL_ID);
    if (char_nsid > 0)
        paired = exitos_devguard_nvme_pair_at("/sys",
                                              major(bs.st_rdev), minor(bs.st_rdev),
                                              major(cs.st_rdev), minor(cs.st_rdev),
                                              (uint32_t)char_nsid);
    close(cfd);
    close(bfd);
    return paired;
}

static int devguard_has_partitions_at(const char *root,
                                      unsigned dev_major, unsigned dev_minor)
{
    char real[PATH_MAX], child[PATH_MAX], partition[PATH_MAX];
    DIR *d;
    struct dirent *e;
    struct stat st;
    int result = 0;

    if (!devguard_sysfs_target(root, dev_major, dev_minor, real, sizeof real))
        return -1;
    if (snprintf(partition, sizeof partition, "%s/partition", real) >=
            (int)sizeof partition)
        return -1;
    if (lstat(partition, &st) == 0) return 0;
    if (errno != ENOENT) return -1;
    d = opendir(real);
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (snprintf(child, sizeof child, "%s/%s", real, e->d_name) >=
                (int)sizeof child) {
            result = -1;
            break;
        }
        if (stat(child, &st) != 0) {
            result = -1;
            break;
        }
        if (!S_ISDIR(st.st_mode))
            continue;
        if (snprintf(partition, sizeof partition, "%s/partition", child) >=
                (int)sizeof partition) {
            result = -1;
            break;
        }
        if (lstat(partition, &st) == 0) { result = 1; break; }
        if (errno != ENOENT) { result = -1; break; }
    }
    if (closedir(d) != 0 && result == 0) result = -1;
    return result;
}

devguard_verdict exitos_devguard_check_fd_at(int fd, const char *sysfs_root,
                                             const char *mountinfo_path,
                                             const char *expect_serial,
                                             char *serial_out,
                                             unsigned serial_len)
{
    struct devguard_device_info info;
    int parts, mounted;

    if (serial_out && serial_len) serial_out[0] = '\0';
    if (exitos_devguard_resolve_fd_at(fd, sysfs_root, &info) != 0)
        return DEVGUARD_NOT_A_DEVICE;
    if (serial_out && serial_len)
        snprintf(serial_out, serial_len, "%s", info.controller_serial);
    if (expect_serial && *expect_serial &&
        strcmp(info.controller_serial, expect_serial) != 0)
        return DEVGUARD_SERIAL_MISMATCH;
    if (info.controller_serial[0] == '\0' || info.identity[0] == '\0')
        return DEVGUARD_NO_SERIAL;

    /* Mount state is an unconditional safety gate.  Callers may explicitly
     * accept the structural partition verdicts below, so neither a mounted
     * partition nor a whole disk with a mounted child may return early as an
     * otherwise-acceptable partition verdict. */
    mounted = exitos_devguard_mounted_at(sysfs_root, mountinfo_path,
                                         info.dev_major, info.dev_minor);
    if (mounted != 0) return DEVGUARD_MOUNTED; /* errors fail closed */
    if (info.is_partition) return DEVGUARD_IS_PARTITION;

    parts = devguard_has_partitions_at(sysfs_root, info.dev_major, info.dev_minor);
    if (parts < 0) return DEVGUARD_NOT_FOUND;
    if (parts > 0) return DEVGUARD_HAS_PARTITIONS;
    return DEVGUARD_OK;
}

devguard_verdict exitos_devguard_check_fd(int fd, const char *expect_serial,
                                          char *serial_out, unsigned serial_len)
{
    return exitos_devguard_check_fd_at(fd, "/sys", "/proc/self/mountinfo",
                                       expect_serial, serial_out, serial_len);
}

devguard_verdict exitos_devguard_check(const char *dev_path, const char *expect_serial,
                                       char *serial_out, unsigned serial_len)
{
    char path[PATH_MAX];
    struct stat st;
    devguard_verdict result;
    int fd;

    if (serial_out && serial_len) serial_out[0] = '\0';
    if (!safe_device_path(dev_path, path, sizeof path)) return DEVGUARD_NOT_FOUND;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return DEVGUARD_NOT_FOUND;
    if (fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode)) {
        close(fd);
        return DEVGUARD_NOT_FOUND;
    }
    result = exitos_devguard_check_fd(fd, expect_serial, serial_out, serial_len);
    close(fd);
    return result;
}

const char *exitos_devguard_str(devguard_verdict v){
    switch(v){
    case DEVGUARD_OK: return "OK";
    case DEVGUARD_NOT_FOUND: return "no such block device";
    case DEVGUARD_HAS_PARTITIONS: return "device is PARTITIONED (refusing: likely holds data)";
    case DEVGUARD_MOUNTED: return "device or a partition of it is MOUNTED (refusing)";
    case DEVGUARD_SERIAL_MISMATCH: return "serial does NOT match the expected disk (refusing)";
    case DEVGUARD_NO_SERIAL: return "cannot read serial, identity unprovable (refusing)";
    case DEVGUARD_IS_PARTITION: return "device is a PARTITION (refusing: never send passthru here)";
    case DEVGUARD_NOT_A_DEVICE: return "path is not a block device node (refusing)";
    }
    return "unknown";
}


uint32_t exitos_devguard_lbs(const char *dev_path)
{
    char path[PATH_MAX];
    uint32_t result;
    int fd;
    if (!safe_device_path(dev_path, path, sizeof path)) return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    result = exitos_devguard_lbs_fd(fd);
    close(fd);
    return result;
}

uint32_t exitos_devguard_lbs_fd(int fd)
{
    struct devguard_device_info info;
    return exitos_devguard_resolve_fd(fd, &info) == 0 ?
           info.logical_block_size : 0;
}

int exitos_devguard_window_in_partition_fds_at(int disk_fd, int partition_fd,
                                               const char *sysfs_root,
                                               uint64_t lba, uint64_t count,
                                               uint32_t lbs,
                                               char *why, unsigned wlen)
{
    struct devguard_device_info disk, part;
    char real[PATH_MAX], path[PATH_MAX], line[64];
    uint64_t pstart, psize, pend, mul, first, nsect, last;

    if (why && wlen) why[0] = '\0';
    if (!sysfs_root || !*sysfs_root || count == 0 || lbs == 0 || lbs % 512 != 0) {
        devguard_why(why,wlen,"invalid disk, partition, window, or logical block size");
        return -EINVAL;
    }
    if (exitos_devguard_resolve_fd_at(disk_fd,sysfs_root,&disk) != 0 ||
        disk.is_partition ||
        exitos_devguard_resolve_fd_at(partition_fd,sysfs_root,&part) != 0 ||
        !part.is_partition) {
        devguard_why(why,wlen,"opened objects are not a whole disk and partition");
        return -ENODEV;
    }
    if (part.parent_major != disk.dev_major || part.parent_minor != disk.dev_minor) {
        devguard_why(why,wlen,"opened partition belongs to a different disk");
        return -EXDEV;
    }
    if (lbs != disk.logical_block_size || part.logical_block_size != disk.logical_block_size) {
        devguard_why(why,wlen,"caller logical block size %u differs from actual %u",
                     lbs,disk.logical_block_size);
        return -EINVAL;
    }
    if (!devguard_sysfs_target(sysfs_root,part.dev_major,part.dev_minor,
                               real,sizeof real)) return -ENODEV;
    if (snprintf(path,sizeof path,"%s/start",real) >= (int)sizeof path ||
        read_line(path,line,sizeof line) != 0 ||
        !parse_decimal_slice(line,strlen(line),&pstart)) {
        devguard_why(why,wlen,"cannot read actual partition start");
        return -EIO;
    }
    if (snprintf(path,sizeof path,"%s/size",real) >= (int)sizeof path ||
        read_line(path,line,sizeof line) != 0 ||
        !parse_decimal_slice(line,strlen(line),&psize) || psize == 0 ||
        psize > UINT64_MAX-pstart) {
        devguard_why(why,wlen,"cannot read a valid actual partition size");
        return -EIO;
    }
    pend=pstart+psize;
    mul=lbs/512;
    if (lba>UINT64_MAX/mul || count>UINT64_MAX/mul) {
        devguard_why(why,wlen,"window conversion overflows");
        return -ERANGE;
    }
    first=lba*mul;
    nsect=count*mul;
    if (nsect>UINT64_MAX-first) {
        devguard_why(why,wlen,"window end overflows");
        return -ERANGE;
    }
    last=first+nsect;
    if (first<pstart || last>pend) {
        devguard_why(why,wlen,"window sectors [%llu,%llu) is outside actual partition "
                     "[%llu,%llu)",(unsigned long long)first,
                     (unsigned long long)last,(unsigned long long)pstart,
                     (unsigned long long)pend);
        return -ERANGE;
    }
    devguard_why(why,wlen,"window sectors [%llu,%llu) lies inside actual partition "
                 "[%llu,%llu)",(unsigned long long)first,
                 (unsigned long long)last,(unsigned long long)pstart,
                 (unsigned long long)pend);
    return 0;
}

int exitos_devguard_window_in_partition_fds(int disk_fd, int partition_fd,
                                            uint64_t lba, uint64_t count,
                                            uint32_t lbs,
                                            char *why, unsigned wlen)
{
    return exitos_devguard_window_in_partition_fds_at(disk_fd,partition_fd,"/sys",
                                                       lba,count,lbs,why,wlen);
}

int exitos_devguard_window_in_partition(const char *dev_path, const char *part_name,
                                        uint64_t lba, uint64_t count, uint32_t lbs,
                                        char *why, unsigned wlen)
{
    char disk_path[PATH_MAX], part_path[PATH_MAX];
    int disk_fd=-1, part_fd=-1, rc;
    if (!part_name || strchr(part_name,'/') ||
        !safe_device_path(dev_path,disk_path,sizeof disk_path) ||
        !safe_device_path(part_name,part_path,sizeof part_path)) {
        devguard_why(why,wlen,"invalid disk or partition path");
        return -EINVAL;
    }
    disk_fd=open(disk_path,O_RDONLY | O_CLOEXEC);
    if (disk_fd<0) { devguard_why(why,wlen,"cannot open disk"); return -errno; }
    part_fd=open(part_path,O_RDONLY | O_CLOEXEC);
    if (part_fd<0) { rc=-errno; close(disk_fd); devguard_why(why,wlen,"cannot open partition"); return rc; }
    rc=exitos_devguard_window_in_partition_fds(disk_fd,part_fd,lba,count,lbs,why,wlen);
    close(part_fd);
    close(disk_fd);
    return rc;
}

/* ---- disk pinning: detect that the disk changed under us ---- */
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#define FNV_OFF 1469598103934665603ULL
#define FNV_PRM 1099511628211ULL
static uint64_t fnv(uint64_t h, const void *p, size_t n){
    const unsigned char *b=p; for(size_t i=0;i<n;i++){ h^=b[i]; h*=FNV_PRM; } return h;
}
#define PIN_SAMPLES 64
#define PIN_CHUNK   4096

static uint64_t devguard_off_t_max(void);

static uint64_t dev_size_bytes(int fd){
    uint64_t bytes = 0;
    off_t current, end;
    if (ioctl(fd, BLKGETSIZE64, &bytes) == 0 && bytes > 0) return bytes;
    current = lseek(fd, 0, SEEK_CUR);
    end = lseek(fd, 0, SEEK_END);
    if (current >= 0) (void)lseek(fd, current, SEEK_SET);
    return end > 0 ? (uint64_t)end : 0;
}

int exitos_devguard_pin_capture_fd_at(int fd, const char *sysfs_root,
                                      uint64_t zlba, uint64_t zcnt,
                                      struct devguard_pin *out){
    struct devguard_device_info info;
    uint64_t off_max;
    unsigned char *buf = NULL;
    size_t alignment, chunk;
    uint64_t h=FNV_OFF;

    if(fd<0||!sysfs_root||!*sysfs_root||!out||zcnt==0||
       zlba>UINT64_MAX-zcnt) return -1;
    memset(out,0,sizeof *out);
    if (exitos_devguard_resolve_fd_at(fd, sysfs_root, &info) != 0 ||
        info.identity[0] == '\0')
        return -1;
    memcpy(out->serial, info.identity, strlen(info.identity) + 1);
    out->size_bytes = dev_size_bytes(fd);
    if (out->size_bytes < PIN_CHUNK) { memset(out,0,sizeof *out); return -1; }
    out->zero_lba_start=zlba; out->zero_lba_count=zcnt;
    out->logical_block_size=info.logical_block_size; out->nsamples=PIN_SAMPLES;
    alignment=info.logical_block_size;
    if(alignment<sizeof(void *)) alignment=sizeof(void *);
    chunk=info.logical_block_size>PIN_CHUNK ? info.logical_block_size : PIN_CHUNK;
    if(posix_memalign((void **)&buf,alignment,chunk)!=0 || !buf){
        memset(out,0,sizeof *out); return -1;
    }
    off_max=devguard_off_t_max();
    for(unsigned i=0;i<PIN_SAMPLES;i++){
        /* deterministic spread so the same disk always hashes the same */
        uint64_t off = out->size_bytes ? (out->size_bytes/PIN_SAMPLES)*i : 0;
        off -= off % chunk;
        if (off > off_max || off > out->size_bytes ||
            chunk > out->size_bytes - off ||
            pread(fd,buf,chunk,(off_t)off) != (ssize_t)chunk) {
            free(buf); memset(out,0,sizeof *out); return -1;
        }
        h=fnv(h,buf,chunk);
    }
    out->sample_hash=h; free(buf); return 0;
}

int exitos_devguard_pin_capture_fd(int fd, uint64_t zlba, uint64_t zcnt,
                                   struct devguard_pin *out)
{
    return exitos_devguard_pin_capture_fd_at(fd, "/sys", zlba, zcnt, out);
}

int exitos_devguard_pin_capture(const char *dev_path, uint64_t zlba, uint64_t zcnt,
                                struct devguard_pin *out){
    char path[PATH_MAX];
    int fd, rc;
    if (!safe_device_path(dev_path, path, sizeof path)) return -1;
    fd=open(path,O_RDONLY | O_CLOEXEC); if(fd<0) return -1;
    rc=exitos_devguard_pin_capture_fd(fd,zlba,zcnt,out);
    close(fd);
    return rc;
}

static int pin_open_parent(const char *path, char *leaf, size_t leaf_len)
{
    char dir[PATH_MAX], walk[PATH_MAX], *save = NULL, *part;
    const char *leaf_src, *slash;
    size_t path_len, dir_len;
    int dfd, next;

    if (!path || !*path || !leaf || leaf_len == 0) return -1;
    path_len = strlen(path);
    if (path_len >= PATH_MAX || path[path_len - 1] == '/') return -1;
    slash = strrchr(path, '/');
    leaf_src = slash ? slash + 1 : path;
    if (!*leaf_src || strcmp(leaf_src, ".") == 0 ||
        strcmp(leaf_src, "..") == 0 || strlen(leaf_src) >= leaf_len)
        return -1;
    memcpy(leaf, leaf_src, strlen(leaf_src) + 1);

    if (!slash) {
        memcpy(dir, ".", 2);
    } else if (slash == path) {
        memcpy(dir, "/", 2);
    } else {
        dir_len = (size_t)(slash - path);
        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';
    }
    memcpy(walk, dir, strlen(dir) + 1);
    dfd = open(dir[0] == '/' ? "/" : ".",
               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dfd < 0) return -1;
    part = strtok_r(dir[0] == '/' ? walk + 1 : walk, "/", &save);
    while (part) {
        if (strcmp(part, ".") == 0) {
            part = strtok_r(NULL, "/", &save);
            continue;
        }
        if (strcmp(part, "..") == 0) {
            close(dfd);
            return -1;
        }
        next = openat(dfd, part,
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            close(dfd);
            return -1;
        }
        close(dfd);
        dfd = next;
        part = strtok_r(NULL, "/", &save);
    }
    return dfd;
}

static int pin_random_bytes(unsigned char *out, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = getrandom(out + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int pin_temp_openat(int dfd, char *name, size_t name_len)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char random[8];
    unsigned attempt, i;
    int fd;

    if (!name || name_len < sizeof ".exitos-pin-0000000000000000") return -1;
    for (attempt = 0; attempt < 128; attempt++) {
        if (pin_random_bytes(random, sizeof random) != 0) return -1;
        memcpy(name, ".exitos-pin-", 12);
        for (i = 0; i < sizeof random; i++) {
            name[12 + i * 2] = hex[random[i] >> 4];
            name[13 + i * 2] = hex[random[i] & 0x0f];
        }
        name[28] = '\0';
        fd = openat(dfd, name,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    return -1;
}

int exitos_devguard_pin_save(const struct devguard_pin *p, const char *path){
    char leaf[NAME_MAX + 1], tmp[32] = {0};
    struct stat st;
    FILE *f = NULL;
    int fd = -1, dfd = -1, rc = -1, renamed = 0;
    if(!p || !path || !*path || p->serial[0]=='\0' ||
       strchr(p->serial,'\n') || strchr(p->serial,'\r') ||
       p->logical_block_size == 0 ||
       (p->logical_block_size & (p->logical_block_size - 1)) != 0) return -1;
    dfd = pin_open_parent(path, leaf, sizeof leaf);
    if (dfd < 0) return -1;
    if (fstatat(dfd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) goto out;
    } else if (errno != ENOENT) {
        goto out;
    }
    fd = pin_temp_openat(dfd, tmp, sizeof tmp);
    if (fd < 0) goto out;
    if (fchmod(fd, 0600) != 0) goto out;
    f = fdopen(fd, "w");
    if (!f) goto out;
    fd = -1; /* owned by f */
    if(fprintf(f,"serial=%s\nsize=%llu\nhash=%llu\nzlba=%llu\nzcnt=%llu\nlbs=%u\nn=%u\n",
        p->serial,(unsigned long long)p->size_bytes,(unsigned long long)p->sample_hash,
        (unsigned long long)p->zero_lba_start,(unsigned long long)p->zero_lba_count,
        p->logical_block_size,p->nsamples) < 0 ||
        fflush(f) != 0 || fsync(fileno(f)) != 0)
        goto out;
    if (fclose(f) != 0) { f = NULL; goto out; }
    f = NULL;
    if (renameat(dfd, tmp, dfd, leaf) != 0) goto out;
    renamed = 1;
    if (fsync(dfd) != 0) goto out;
    rc = 0;
out:
    if (f && fclose(f) != 0) rc = -1;
    if (fd >= 0) close(fd);
    if (!renamed && dfd >= 0 && tmp[0]) unlinkat(dfd, tmp, 0);
    if (dfd >= 0) close(dfd);
    return rc;
}

static int pin_line(FILE *f, const char *key, char *value, size_t value_len)
{
    char line[512];
    size_t key_len = strlen(key), n;
    if (!fgets(line, sizeof line, f)) return -1;
    n = strlen(line);
    if (n <= key_len || line[n-1] != '\n' || strncmp(line, key, key_len) != 0 ||
        line[key_len] != '=') return -1;
    line[--n] = '\0';
    if (n > 0 && line[n-1] == '\r') return -1;
    if (strlen(line + key_len + 1) + 1 > value_len) return -1;
    memcpy(value, line + key_len + 1, strlen(line + key_len + 1) + 1);
    return 0;
}

static int pin_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    const unsigned char *p = (const unsigned char *)s;
    if (!s || !*s || !out) return -1;
    for (; *p; p++) {
        uint64_t digit;
        if (*p < '0' || *p > '9') return -1;
        digit = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - digit) / UINT64_C(10)) return -1;
        v = v * UINT64_C(10) + digit;
    }
    *out = v;
    return 0;
}

int exitos_devguard_pin_load(struct devguard_pin *p, const char *path){
    char leaf[NAME_MAX + 1];
    struct stat st;
    FILE *f = NULL;
    char v[512], extra;
    uint64_t lbs, n;
    int fd = -1, dfd = -1, rc = -1;
    if(!p || !path || !*path) return -1;
    memset(p,0,sizeof *p);
    dfd=pin_open_parent(path,leaf,sizeof leaf);
    if(dfd<0) goto out;
    fd=openat(dfd,leaf,O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if(fd<0 || fstat(fd,&st)!=0 || !S_ISREG(st.st_mode)) goto out;
    f=fdopen(fd,"r");
    if(!f) goto out;
    fd=-1; /* owned by f */
    if(pin_line(f,"serial",v,sizeof v)!=0 || v[0]=='\0' ||
       strlen(v)>=sizeof p->serial) goto out;
    memcpy(p->serial,v,strlen(v)+1);
    if(pin_line(f,"size",v,sizeof v)!=0 || pin_u64(v,&p->size_bytes)!=0) goto out;
    if(pin_line(f,"hash",v,sizeof v)!=0 || pin_u64(v,&p->sample_hash)!=0) goto out;
    if(pin_line(f,"zlba",v,sizeof v)!=0 || pin_u64(v,&p->zero_lba_start)!=0) goto out;
    if(pin_line(f,"zcnt",v,sizeof v)!=0 || pin_u64(v,&p->zero_lba_count)!=0 ||
       p->zero_lba_count==0 ||
       p->zero_lba_start>UINT64_MAX-p->zero_lba_count) goto out;
    if(pin_line(f,"lbs",v,sizeof v)!=0 || pin_u64(v,&lbs)!=0 || lbs==0 ||
       lbs>UINT32_MAX || (lbs & (lbs-1))!=0) goto out;
    p->logical_block_size=(uint32_t)lbs;
    if(pin_line(f,"n",v,sizeof v)!=0 || pin_u64(v,&n)!=0 || n!=PIN_SAMPLES) goto out;
    p->nsamples=(uint32_t)n;
    if(fread(&extra,1,1,f)!=0 || ferror(f)) goto out;
    rc=0;
out:
    if(f && fclose(f)!=0) rc=-1;
    if(fd>=0) close(fd);
    if(dfd>=0) close(dfd);
    if(rc!=0) memset(p,0,sizeof *p);
    return rc;
}

static int pin_window_matches(const struct devguard_pin *old,
                              uint64_t lba, uint64_t count,
                              char *why, unsigned wlen)
{
    if(old->zero_lba_start!=lba || old->zero_lba_count!=count){
        devguard_why(why,wlen,"request window start=%llu count=%llu does not exactly "
                     "match pinned start=%llu count=%llu",
                     (unsigned long long)lba, (unsigned long long)count,
                     (unsigned long long)old->zero_lba_start,
                     (unsigned long long)old->zero_lba_count);
        return -ERANGE;
    }
    return 0;
}

static int pin_verify_loaded_fd_at(int fd, const char *sysfs_root,
                                   const struct devguard_pin *old,
                                   char *why, unsigned wlen){
    struct devguard_pin now;
    if(exitos_devguard_pin_capture_fd_at(fd,sysfs_root,old->zero_lba_start,
                                        old->zero_lba_count,&now)!=0){
        devguard_why(why,wlen,"cannot read retained device fd to verify pin"); return -1; }
    if(old->serial[0]=='\0' || now.serial[0]=='\0'){
        devguard_why(why,wlen,"pin carries no serial (pinned '%s', device now reports '%s'): "
                     "an empty identity matches every disk, which is the opposite of a pin",
                     old->serial, now.serial); return -1; }
    if(strcmp(old->serial,now.serial)!=0){
        devguard_why(why,wlen,"SERIAL CHANGED: pinned '%s' but device now reports '%s' "
                     "(kernel nvme names rotate across reboots - this is the dangerous case)",
                     old->serial,now.serial); return -1; }
    if(old->size_bytes!=now.size_bytes){
        devguard_why(why,wlen,"SIZE CHANGED: pinned %llu bytes, now %llu",
                     (unsigned long long)old->size_bytes,
                     (unsigned long long)now.size_bytes); return -1; }
    if(old->logical_block_size!=now.logical_block_size){
        devguard_why(why,wlen,"LOGICAL BLOCK SIZE CHANGED: pinned %u bytes, now %u",
                     old->logical_block_size,now.logical_block_size); return -1; }
    if(old->sample_hash!=now.sample_hash){
        devguard_why(why,wlen,"CONTENT CHANGED since certification (sampled hash differs). "
                     "Someone or something wrote to this disk. Re-certify before writing.");
        return -1; }
    devguard_why(why,wlen,"pin matches: serial=%s size=%llu hash ok",
                 now.serial,(unsigned long long)now.size_bytes);
    return 0;
}

int exitos_devguard_pin_verify_window_fd_at(int fd, const char *sysfs_root,
                                            const char *pin_path,
                                            uint64_t lba, uint64_t count,
                                            char *why, unsigned wlen){
    struct devguard_pin old;
    if(exitos_devguard_pin_load(&old,pin_path)!=0){
        devguard_why(why,wlen,"no valid pin file at %s - disk was never certified",
                     pin_path ? pin_path : "(null)"); return -1; }
    if (pin_window_matches(&old,lba,count,why,wlen) != 0) return -ERANGE;
    return pin_verify_loaded_fd_at(fd,sysfs_root,&old,why,wlen);
}

int exitos_devguard_pin_verify_window_fd(int fd, const char *pin_path,
                                         uint64_t lba, uint64_t count,
                                         char *why, unsigned wlen)
{
    return exitos_devguard_pin_verify_window_fd_at(fd,"/sys",pin_path,lba,count,
                                                    why,wlen);
}

int exitos_devguard_pin_verify_window(const char *dev_path, const char *pin_path,
                                      uint64_t lba, uint64_t count,
                                      char *why, unsigned wlen){
    struct devguard_pin old;
    char path[PATH_MAX];
    int fd, rc;
    if(exitos_devguard_pin_load(&old,pin_path)!=0){
        devguard_why(why,wlen,"no valid pin file at %s - disk was never certified",
                     pin_path ? pin_path : "(null)"); return -1; }
    rc=pin_window_matches(&old,lba,count,why,wlen);
    if(rc!=0) return rc;
    if (!safe_device_path(dev_path,path,sizeof path)) {
        devguard_why(why,wlen,"no valid device path to verify pin"); return -1;
    }
    fd=open(path,O_RDONLY | O_CLOEXEC);
    if(fd<0){ devguard_why(why,wlen,"cannot open device to verify pin"); return -1; }
    rc=pin_verify_loaded_fd_at(fd,"/sys",&old,why,wlen);
    close(fd);
    return rc;
}

static uint64_t devguard_off_t_max(void)
{
    unsigned bits = (unsigned)(sizeof(off_t) * CHAR_BIT);
    unsigned value_bits = bits - (((off_t)-1 < (off_t)0) ? 1u : 0u);
    uint64_t max = 0;
    unsigned i;

    if (value_bits > 64u) value_bits = 64u;
    for (i = 0; i < value_bits; i++)
        max = (max << 1) | UINT64_C(1);
    return max;
}

static int devguard_window_is_zero_checked_fd(int fd, uint64_t lba,
                                              uint64_t cnt, uint32_t lbs,
                                              uint64_t *first_bad_lba){
    uint64_t start, total, end, off_max;
    size_t alignment, chunk;
    unsigned char *b = NULL;
    if(fd<0 || lbs==0 || cnt==0 || (lbs & (lbs-1)) != 0 ||
       (uint32_t)(size_t)lbs != lbs) return -EINVAL;
    if(lba>UINT64_MAX/(uint64_t)lbs || cnt>UINT64_MAX/(uint64_t)lbs)
        return -EOVERFLOW;
    start=lba*(uint64_t)lbs;
    total=cnt*(uint64_t)lbs;
    if(total>UINT64_MAX-start)
        return -EOVERFLOW;
    end=start+total;
    off_max=devguard_off_t_max();
    if(start>off_max || end-1>off_max) return -EOVERFLOW;
    chunk=1u<<20;
    if((size_t)lbs>chunk) chunk=(size_t)lbs;
    alignment=lbs<sizeof(void *)?sizeof(void *):(size_t)lbs;
    if(posix_memalign((void **)&b,alignment,chunk)!=0 || !b) return -ENOMEM;
    int rc=0; uint64_t done=0;
    while(done<total){
        size_t n = (total-done) < chunk ? (size_t)(total-done) : chunk;
        uint64_t current=start+done;
        if(current>off_max){ rc=-EOVERFLOW; break; }
        ssize_t r=pread(fd,b,n,(off_t)current);
        if(r<0){ rc=-errno; break; }
        if((size_t)r!=n){ rc=-EIO; break; }
        for(ssize_t i=0;i<r;i++) if(b[i]){
            if(first_bad_lba) *first_bad_lba = lba + (done+(uint64_t)i)/(uint64_t)lbs;
            rc=1; goto out; }
        done += (uint64_t)r;
    }
out: free(b); return rc;
}

int exitos_devguard_window_is_zero_checked(const char *dev_path, uint64_t lba,
                                           uint64_t cnt, uint32_t lbs,
                                           uint64_t *first_bad_lba){
    int fd, rc;
    if(!dev_path || !*dev_path) return -EINVAL;
    fd=open(dev_path,O_RDONLY | O_CLOEXEC); if(fd<0) return -errno;
    rc=devguard_window_is_zero_checked_fd(fd,lba,cnt,lbs,first_bad_lba);
    close(fd);
    return rc;
}

int exitos_devguard_window_is_zero_fd_at(int fd, const char *sysfs_root,
                                         uint64_t lba, uint64_t cnt,
                                         uint64_t *first_bad_lba)
{
    struct devguard_device_info info;
    if (exitos_devguard_resolve_fd_at(fd,sysfs_root,&info) != 0) return -ENODEV;
    return devguard_window_is_zero_checked_fd(fd,lba,cnt,
                                              info.logical_block_size,
                                              first_bad_lba);
}

int exitos_devguard_window_is_zero_fd(int fd, uint64_t lba, uint64_t cnt,
                                      uint64_t *first_bad_lba)
{
    return exitos_devguard_window_is_zero_fd_at(fd,"/sys",lba,cnt,first_bad_lba);
}


int exitos_devguard_passthru_ok_fd_at(int fd, const char *sysfs_root)
{
    struct stat st;
    if (fd < 0 || !sysfs_root || !*sysfs_root || fstat(fd,&st) != 0 ||
        !S_ISBLK(st.st_mode)) return 0;
    return exitos_devguard_whole_block_at(sysfs_root,major(st.st_rdev),
                                          minor(st.st_rdev));
}

int exitos_devguard_passthru_ok_fd(int fd)
{
    return exitos_devguard_passthru_ok_fd_at(fd,"/sys");
}

int exitos_devguard_passthru_ok(const char *dev_path)
{
    char path[PATH_MAX];
    int fd, result;
    if (!safe_device_path(dev_path,path,sizeof path)) return 0;
    fd=open(path,O_RDONLY | O_CLOEXEC);
    if (fd<0) return 0;
    result=exitos_devguard_passthru_ok_fd(fd);
    close(fd);
    return result;
}
