/* donor: preallocated donor files whose extents are handed to a growing log
 * file with EXT4_IOC_MOVE_EXT, giving preallocation's effect on the fly.
 *
 * Mechanism (and why it is not just fallocate):
 *   EXT4_IOC_MOVE_EXT SWAPS extents between two files on the same ext4
 *   filesystem.  It refuses to donate into a hole: both the original and the
 *   donor must own real blocks over the range (a pure hole in the original
 *   fails with ENODATA on 5.15).  Donors are therefore zero-written and synced
 *   up front, while donor_extend first fallocates the target's exchange range.
 *   MOVE_EXT swaps physical ownership, but does not transfer the donor's
 *   WRITTEN state to that target range: the freshly fallocated target remains
 *   UNWRITTEN after the swap.  donor_extend consequently zero-writes the
 *   target range and donor_prepare_exact syncs and FIEMAP-verifies it before
 *   exposing success.  MOVE_EXT alone is never treated as completed setup.
 *
 * Pool accounting is logical: every donor file owns bytes_each of donatable
 * space, tracked as a small sorted array of free runs.  A donation consumes a
 * run's head; donor_reclaim gives those bytes back so the pool is not
 * permanently consumed.  A donation is always served from one donor and is
 * therefore contiguous in the donor's file offsets; a request larger than the
 * largest free run is clamped to that run rather than refused.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <linux/falloc.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

#include "exitos_donor.h"
#include "exitos_iopath.h"

#ifndef EXT4_SUPER_MAGIC
#define EXT4_SUPER_MAGIC 0xEF53
#endif

/* ---- EXT4_IOC_MOVE_EXT ---------------------------------------------------
 * No uapi header exports this: it lives in fs/ext4/ext4.h.  The first u32 is
 * "reserved, must be zero" on current kernels; the original file is the fd the
 * ioctl is issued on.  All offsets and lengths are in filesystem blocks. */
struct donor_move_extent {
    uint32_t reserved;
    uint32_t donor_fd;
    uint64_t orig_start;
    uint64_t donor_start;
    uint64_t len;
    uint64_t moved_len;
};
#ifndef EXT4_IOC_MOVE_EXT
#define EXT4_IOC_MOVE_EXT _IOWR('f', 15, struct donor_move_extent)
#endif
#ifndef FS_IOC_FIEMAP
#define FS_IOC_FIEMAP _IOWR('f', 11, struct fiemap)
#endif

#define DONOR_HIST_WIN 16u   /* sliding window for the write-size average */

struct free_run {
    uint64_t off;
    uint64_t len;
};

struct donor_file {
    int              fd;
    int              created; /* this pool owns name below pool->dirfd */
    char            *name;
    struct free_run *runs;   /* sorted by off, never overlapping */
    unsigned         nruns;
    unsigned         cruns;
};

/* One handout: bytes of donor space currently living in a target file. */
struct donor_chunk {
    dev_t    dev;
    ino_t    ino;
    uint64_t file_off;   /* offset inside the target        */
    uint64_t len;
    uint64_t donor_off;  /* offset inside the donor file    */
    unsigned donor_idx;
};

struct donor_pool {
    int                 dirfd;     /* owned identity, never a mutable path */
    unsigned            nfiles;
    uint64_t            bytes_each;
    uint64_t            blk;       /* filesystem block size */
    dev_t               dev;
    struct donor_file  *files;
    struct donor_chunk *chunks;
    unsigned            nchunks;
    unsigned            cchunks;
    uint64_t            hist[DONOR_HIST_WIN];
    uint64_t            hsum;
    unsigned            nhist;
    unsigned            hpos;
    /* EXT4_IOC_MOVE_EXT can report an errno after having exchanged a prefix.
     * Once that happens, keep the accounting record for the known prefix but
     * permanently refuse another handout from this pool.  An I/O/journal
     * error is not a sound boundary at which to assume the remaining donor
     * runs are reusable. */
    int                 poisoned;
    int                 mu_initialized;
    /* Every field above is mutable and is reached through three entry points
     * that a caller may drive from more than one thread: one async preparer per
     * target file, all sharing one pool. The free-run arrays and the chunk table
     * are realloc'd in place, so an unsynchronised second caller reads a pointer
     * that is being freed -- observed as "double free or corruption (top)". The
     * quieter half is worse: two callers could both take the head of the same
     * free run and hand identical physical blocks to two different files. The
     * lock lives here rather than in the async wrapper so that a caller using
     * the synchronous API directly is covered by the same serialisation. */
    pthread_mutex_t     mu;
};

static void pool_lock(struct donor_pool *p)   { pthread_mutex_lock(&p->mu); }
static void pool_unlock(struct donor_pool *p) { pthread_mutex_unlock(&p->mu); }

/* ---- small helpers ------------------------------------------------------ */
static uint64_t align_down(uint64_t v, uint64_t a) { return v - (v % a); }
static uint64_t align_up(uint64_t v, uint64_t a)
{
    uint64_t r = v % a;
    return r ? v + (a - r) : v;
}

static int runs_reserve(struct donor_file *df, unsigned need)
{
    struct free_run *r;
    unsigned c;

    if (df->cruns >= need) return 0;
    c = df->cruns ? df->cruns * 2 : 8;
    while (c < need) c *= 2;
    r = realloc(df->runs, (size_t)c * sizeof *r);
    if (!r) return -ENOMEM;
    df->runs = r;
    df->cruns = c;
    return 0;
}

static int chunks_reserve(struct donor_pool *p, unsigned need)
{
    struct donor_chunk *c;
    unsigned nc;
    size_t alloc_bytes;

    if (p->cchunks >= need)
        return 0;
    nc = p->cchunks ? p->cchunks * 2 : 16;
    if (nc < p->cchunks)
        return -EOVERFLOW;
    while (nc < need) {
        if (nc > UINT32_MAX / 2u)
            return -EOVERFLOW;
        nc *= 2;
    }
    alloc_bytes = (size_t)nc * sizeof *c;
    if (nc != 0 && alloc_bytes / sizeof *c != (size_t)nc)
        return -EOVERFLOW;
    c = realloc(p->chunks, alloc_bytes);
    if (!c)
        return -ENOMEM;
    p->chunks = c;
    p->cchunks = nc;
    return 0;
}

/* Put [off, off+len) back into the donor's free set, merging neighbours. */
static int runs_free(struct donor_file *df, uint64_t off, uint64_t len)
{
    unsigned i, j;

    if (len == 0) return 0;
    if (runs_reserve(df, df->nruns + 1) != 0) return -ENOMEM;
    for (i = 0; i < df->nruns && df->runs[i].off < off; i++)
        ;
    memmove(&df->runs[i + 1], &df->runs[i],
            (size_t)(df->nruns - i) * sizeof df->runs[0]);
    df->runs[i].off = off;
    df->runs[i].len = len;
    df->nruns++;
    for (j = 0; j + 1 < df->nruns; ) {
        if (df->runs[j].off + df->runs[j].len == df->runs[j + 1].off) {
            df->runs[j].len += df->runs[j + 1].len;
            memmove(&df->runs[j + 1], &df->runs[j + 2],
                    (size_t)(df->nruns - j - 2) * sizeof df->runs[0]);
            df->nruns--;
        } else {
            j++;
        }
    }
    return 0;
}

/* Consume `len` bytes off the head of run `ri`. */
static void runs_take_head(struct donor_file *df, unsigned ri, uint64_t len)
{
    struct free_run *r = &df->runs[ri];

    if (len > r->len) len = r->len;
    r->off += len;
    r->len -= len;
    if (r->len == 0) {
        memmove(&df->runs[ri], &df->runs[ri + 1],
                (size_t)(df->nruns - ri - 1) * sizeof df->runs[0]);
        df->nruns--;
    }
}

/* Best fit: the smallest free run that covers `want`; if none does, the
 * largest run there is, so the caller can clamp the request to it. */
static int pool_pick(struct donor_pool *p, uint64_t want,
                     unsigned *out_fi, unsigned *out_ri, uint64_t *out_len)
{
    unsigned fi, ri;
    unsigned best_fi = 0, best_ri = 0;
    uint64_t best_len = 0;
    int found = 0, exact = 0;

    for (fi = 0; fi < p->nfiles; fi++) {
        struct donor_file *df = &p->files[fi];
        for (ri = 0; ri < df->nruns; ri++) {
            uint64_t l = df->runs[ri].len;
            if (l == 0) continue;
            if (l >= want) {
                if (!exact || l < best_len) {
                    exact = 1; found = 1;
                    best_len = l; best_fi = fi; best_ri = ri;
                }
            } else if (!exact && l > best_len) {
                found = 1;
                best_len = l; best_fi = fi; best_ri = ri;
            }
        }
    }
    if (!found) return -ENOSPC;
    *out_fi = best_fi;
    *out_ri = best_ri;
    *out_len = best_len;
    return 0;
}

/* Caller reserved p->nchunks + 1 before MOVE_EXT. Appending after the swap is
 * therefore infallible: an allocator failure can never make us lose the only
 * bookkeeping record for blocks that have already changed owners. */
static void chunk_add_reserved(struct donor_pool *p, dev_t dev, ino_t ino,
                               uint64_t file_off, uint64_t len,
                               unsigned donor_idx, uint64_t donor_off)
{
    struct donor_chunk *c;

    c = &p->chunks[p->nchunks++];
    c->dev = dev;
    c->ino = ino;
    c->file_off = file_off;
    c->len = len;
    c->donor_idx = donor_idx;
    c->donor_off = donor_off;
}

static void chunk_drop(struct donor_pool *p, unsigned i)
{
    memmove(&p->chunks[i], &p->chunks[i + 1],
            (size_t)(p->nchunks - i - 1) * sizeof p->chunks[0]);
    p->nchunks--;
}

/* Write an exact zero-filled range through the frontend's direct-call seam.
 * The buffer alignment is a power of two at least as large as the filesystem
 * block, so the same routine is valid when target_fd carries O_DIRECT. */
static int zero_fill_exact(int fd, uint64_t file_off, uint64_t bytes,
                           uint64_t fs_block)
{
    const size_t default_chunk = 1u << 20;
    size_t alignment = sizeof(void *);
    size_t chunk = default_chunk;
    uint64_t done = 0;
    void *zero = NULL;
    int mrc;

    if (bytes == 0)
        return 0;
    if (file_off > UINT64_MAX - bytes)
        return -EOVERFLOW;
    if (fs_block == 0 || fs_block > SIZE_MAX)
        return -EINVAL;
    while ((uint64_t)alignment < fs_block) {
        if (alignment > SIZE_MAX / 2)
            return -EOVERFLOW;
        alignment *= 2;
    }
    if ((uint64_t)chunk < fs_block)
        chunk = alignment;

    mrc = posix_memalign(&zero, alignment, chunk);
    if (mrc != 0)
        return -mrc;
    memset(zero, 0, chunk);

    while (done < bytes) {
        uint64_t remain = bytes - done;
        size_t want = remain < (uint64_t)chunk ? (size_t)remain : chunk;
        uint64_t at_u64 = file_off + done;
        off_t at = (off_t)at_u64;
        ssize_t wrote;

        if (at < 0 || (uint64_t)at != at_u64) {
            free(zero);
            return -EOVERFLOW;
        }
        do {
            wrote = exitos_internal_pwrite_call(fd, zero, want, at);
        } while (wrote < 0 && errno == EINTR);
        if (wrote < 0) {
            int err = errno ? errno : EIO;
            free(zero);
            return -err;
        }
        if (wrote == 0 || (size_t)wrote > want) {
            free(zero);
            return -EIO;
        }
        done += (uint64_t)wrote;
    }
    free(zero);
    return 0;
}

/* ---- pool lifetime ------------------------------------------------------ */
static void pool_free(struct donor_pool *p, int unlink_files)
{
    unsigned i;
    int saved_errno = errno;

    if (!p) return;
    for (i = 0; p->files && i < p->nfiles; i++) {
        if (p->files[i].fd >= 0)
            (void)exitos_internal_close_call(p->files[i].fd);
        if (p->files[i].name) {
            if (unlink_files && p->files[i].created && p->dirfd >= 0)
                (void)unlinkat(p->dirfd, p->files[i].name, 0);
            free(p->files[i].name);
        }
        free(p->files[i].runs);
    }
    free(p->files);
    free(p->chunks);
    if (p->mu_initialized) {
        (void)pthread_mutex_destroy(&p->mu);
        p->mu_initialized = 0;
    }
    if (p->dirfd >= 0)
        (void)exitos_internal_close_call(p->dirfd);
    free(p);
    errno = saved_errno;
}

static struct donor_pool *pool_create_fail(struct donor_pool *p,
                                           int unlink_files, int err)
{
    if (err <= 0)
        err = EIO;
    errno = err;
    pool_free(p, unlink_files);
    errno = err;
    return NULL;
}

struct donor_pool *donor_pool_create_at(int borrowed_dirfd, int nfiles,
                                        uint64_t bytes_each)
{
    struct donor_pool *p = NULL;
    struct statvfs vfs;
    struct statfs fs;
    struct stat st;
    uint64_t blk, need, avail;
    unsigned i, n;
    int owned_dirfd = -1;
    int err;

    if (nfiles <= 0 || bytes_each == 0) {
        errno = EINVAL;
        return NULL;
    }

    /* Opening "." duplicates the directory identity through the existing
     * frontend direct-call seam.  It cannot be redirected by a later rename
     * or replacement of the pathname the caller originally opened. */
    owned_dirfd = exitos_internal_openat_call(
        borrowed_dirfd, ".",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    if (owned_dirfd < 0) {
        if (errno == 0)
            errno = EIO;
        return NULL;
    }
    if (fstat(owned_dirfd, &st) != 0) {
        err = errno ? errno : EIO;
        goto fail_early;
    }
    if (!S_ISDIR(st.st_mode)) {
        err = ENOTDIR;
        goto fail_early;
    }
    if (st.st_uid != geteuid() ||
        (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        err = EPERM;
        goto fail_early;
    }
    if (fstatfs(owned_dirfd, &fs) != 0) {
        err = errno ? errno : EIO;
        goto fail_early;
    }
    if ((unsigned long)fs.f_type != (unsigned long)EXT4_SUPER_MAGIC) {
        err = EOPNOTSUPP;
        goto fail_early;
    }
    if (fstatvfs(owned_dirfd, &vfs) != 0) {
        err = errno ? errno : EIO;
        goto fail_early;
    }

    blk = vfs.f_frsize ? (uint64_t)vfs.f_frsize : (uint64_t)vfs.f_bsize;
    if (blk == 0) blk = 4096;
    if (bytes_each > UINT64_MAX - (blk - 1)) {
        err = EOVERFLOW;
        goto fail_early;
    }
    bytes_each = align_up(bytes_each, blk);
    if ((off_t)bytes_each < 0 ||
        (uint64_t)(off_t)bytes_each != bytes_each) {
        err = EOVERFLOW;
        goto fail_early;
    }
    n = (unsigned)nfiles;

    /* MOVE_EXT is a swap, not a one-sided transfer. During setup the donor
     * files own n*bytes_each and the targets must temporarily allocate the
     * same amount before those blocks can be exchanged. Reserve both sides up
     * front so the Nth target does not discover ENOSPC halfway through setup. */
    if (bytes_each > UINT64_MAX / (uint64_t)n) {
        err = EOVERFLOW;
        goto fail_early;
    }
    need = bytes_each * (uint64_t)n;
    if (need > UINT64_MAX / 2u) {
        err = EOVERFLOW;
        goto fail_early;
    }
    need *= 2u;
    if ((uint64_t)vfs.f_bavail > UINT64_MAX / blk)
        avail = UINT64_MAX;
    else
        avail = (uint64_t)vfs.f_bavail * blk;
    if (need > avail) {
        err = ENOSPC;
        goto fail_early;
    }

    p = calloc(1, sizeof *p);
    if (!p) {
        err = ENOMEM;
        goto fail_early;
    }
    p->dirfd = owned_dirfd;
    owned_dirfd = -1;
    /* Initialised before anything else can fail: every later error path goes
     * through pool_free(), and destroying a mutex that was never initialised is
     * undefined. */
    err = pthread_mutex_init(&p->mu, NULL);
    if (err != 0)
        return pool_create_fail(p, 0, err);
    p->mu_initialized = 1;
    p->nfiles = n;
    p->bytes_each = bytes_each;
    p->blk = blk;
    p->dev = st.st_dev;
    p->files = calloc(n, sizeof *p->files);
    if (!p->files)
        return pool_create_fail(p, 0, ENOMEM);
    for (i = 0; i < n; i++) p->files[i].fd = -1;

    for (i = 0; i < n; i++) {
        struct donor_file *df = &p->files[i];
        char name[64];
        int name_len;
        int rc;

        name_len = snprintf(name, sizeof name, "donor-%04u.dat", i);
        if (name_len < 0 || (size_t)name_len >= sizeof name)
            return pool_create_fail(p, 1, ENAMETOOLONG);
        df->name = strdup(name);
        if (!df->name)
            return pool_create_fail(p, 1, ENOMEM);
        /* A fixed donor name is an ownership boundary, not permission to
         * destroy whatever happens to be there. O_EXCL makes collisions fail
         * closed, O_NOFOLLOW refuses a final-component symlink, and created
         * records whether cleanup is allowed to unlink this pathname. */
        df->fd = exitos_internal_openat_call(
            p->dirfd, df->name,
            O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (df->fd < 0)
            return pool_create_fail(p, 1, errno ? errno : EIO);
        df->created = 1;
        /* Allocate, then WRITE ZEROS over the whole donor.
         *
         * fallocate alone leaves unwritten extents, and MOVE_EXT is happy to
         * swap those -- but the donated blocks would then be unusable by the
         * very fast path this pool exists to feed: src/extent.c refuses to map
         * FIEMAP_EXTENT_UNWRITTEN ranges, because ext4 reads them back as zeros
         * until a normal write converts them, so a raw LBA write there would be
         * invisible through the filesystem. A donor of unwritten extents hands
         * the log file blocks it can never write directly, and the file falls
         * back to the kernel for its entire life while appearing to work.
         *
         * A donor is therefore a large contiguous range of blocks that has
         * been prefilled with zeros, not merely allocated.
         *
         * The cost is one sequential pass per donor, paid once when the pool is
         * built -- off the critical path, which is the whole point of building
         * the pool ahead of time rather than allocating during a write. */
        if (exitos_internal_fallocate_call(
                df->fd, 0, 0, (off_t)bytes_each) != 0) {
            return pool_create_fail(p, 1, errno ? errno : EIO);
        }
        rc = zero_fill_exact(df->fd, 0, bytes_each, blk);
        if (rc != 0)
            return pool_create_fail(p, 1, -rc);
        if (exitos_internal_fdatasync_call(df->fd) != 0) {
            return pool_create_fail(p, 1, errno ? errno : EIO);
        }
        rc = runs_free(df, 0, bytes_each);
        if (rc != 0)
            return pool_create_fail(p, 1, -rc);
    }
    return p;

fail_early:
    if (owned_dirfd >= 0) {
        errno = err;
        (void)exitos_internal_close_call(owned_dirfd);
    }
    errno = err;
    return NULL;
}

struct donor_pool *donor_pool_create(const char *dir, int nfiles,
                                     uint64_t bytes_each)
{
    struct donor_pool *p;
    int dirfd;
    int saved_errno;

    if (!dir || !*dir || nfiles <= 0 || bytes_each == 0) {
        errno = EINVAL;
        return NULL;
    }
    dirfd = exitos_internal_open_call(
        dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    if (dirfd < 0) {
        if (errno == 0)
            errno = EIO;
        return NULL;
    }
    p = donor_pool_create_at(dirfd, nfiles, bytes_each);
    saved_errno = errno;
    (void)exitos_internal_close_call(dirfd);
    errno = saved_errno;
    return p;
}

void donor_pool_destroy(struct donor_pool *p)
{
    pool_free(p, 1);
}

/* ---- donation ----------------------------------------------------------- */
static int target_check(const struct donor_pool *p, int fd, struct stat *st)
{
    int flags;

    if (fd < 0) return -EBADF;
    if (fstat(fd, st) != 0) return -errno;
    if (S_ISDIR(st->st_mode)) return -EISDIR;
    if (!S_ISREG(st->st_mode)) return -EINVAL;
    if (st->st_dev != p->dev) return -EXDEV;   /* extents cannot cross a mount */
    flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -errno;
    /* EXT4_IOC_MOVE_EXT requires the original file description to carry both
     * FMODE_READ and FMODE_WRITE.  O_WRONLY is not sufficient: accepting it
     * would let the target fallocate above the ioctl boundary mutate the file
     * before ext4 rejects MOVE_EXT with EBADF.  Keep one uniform O_RDWR
     * contract for every public target-fd operation. */
    if ((flags & O_ACCMODE) != O_RDWR) return -EBADF;
    return 0;
}

uint64_t donor_pool_blocksize(const struct donor_pool *p)
{
    return p ? p->blk : 0;
}

struct donor_extend_effect {
    uint64_t moved_bytes;
    int target_mutated;
};

static int64_t donor_extend_locked(struct donor_pool *p, int target_fd,
                     uint64_t file_off, uint64_t bytes,
                     struct donor_extend_effect *effect)
{
    struct donor_file *df;
    struct stat st;
    uint64_t want, avail = 0, moved = 0, doff;
    unsigned fi = 0, ri = 0;
    off_t old_size;
    int rc;

    if (effect)
        memset(effect, 0, sizeof *effect);
    if (!p) return -EINVAL;
    if (p->poisoned) return -EUCLEAN;
    rc = target_check(p, target_fd, &st);
    if (rc != 0) return rc;
    if (bytes == 0) return 0;
    if (file_off % p->blk) return -EINVAL;

    want = align_down(bytes, p->blk);
    if (want == 0) want = p->blk;          /* sub-block ask: one whole block */

    if (pool_pick(p, want, &fi, &ri, &avail) != 0) return -ENOSPC;
    if (avail < want) want = align_down(avail, p->blk);
    if (want == 0) return -ENOSPC;

    df = &p->files[fi];
    doff = df->runs[ri].off;
    old_size = st.st_size;

    /* Reserve before the first irreversible operation. Once MOVE_EXT swaps
     * ownership, failing to record the chunk would let the same donor run be
     * handed out twice or make it impossible to reclaim safely. */
    if (p->nchunks == UINT32_MAX)
        return -EOVERFLOW;
    rc = chunks_reserve(p, p->nchunks + 1);
    if (rc != 0)
        return rc;

    /* MOVE_EXT swaps: the target needs real blocks over the range first, or
     * the ioctl fails with ENODATA. */
    if (exitos_internal_fallocate_call(
            target_fd, 0, (off_t)file_off, (off_t)want) != 0)
        return -errno;
    if (effect)
        effect->target_mutated = 1;

    rc = 0;
    while (moved < want) {
        struct donor_move_extent me;
        uint64_t got;
        int ioctl_rc;
        int ioctl_errno = 0;

        memset(&me, 0, sizeof me);
        me.donor_fd    = (uint32_t)df->fd;
        me.orig_start  = (file_off + moved) / p->blk;
        me.donor_start = (doff + moved) / p->blk;
        me.len         = (want - moved) / p->blk;
        ioctl_rc = ioctl(target_fd, EXT4_IOC_MOVE_EXT, &me);
        if (ioctl_rc != 0)
            ioctl_errno = errno ? errno : EIO;
        /* The kernel copies moved_len back even when the ioctl itself returns
         * an error.  Read and account it before looking at ioctl_rc; otherwise
         * a partially exchanged prefix could be handed out a second time. */
        if (me.moved_len > (want - moved) / p->blk) {
            p->poisoned = 1;
            rc = -EUCLEAN;
            break;
        }
        got = me.moved_len * p->blk;
        /* MOVE_EXT carries the blocks but NOT the "already written" state: the
         * flag stays with the file's range, so a range that was fallocate'd
         * comes out of the swap still unwritten no matter how the donor looked.
         * Measured directly: a donor whose extents were initialised still
         * produced an UNWRITTEN target, while a target initialised beforehand
         * kept its state through the swap.
         *
         * That matters because src/extent.c refuses to map unwritten ranges --
         * ext4 reads them back as zeros until a normal write converts them, so
         * a raw LBA write there would be invisible. Left unconverted, every
         * donated block is unusable by the fast path and the file quietly runs
         * on the kernel path forever.
         *
         * fallocate(FALLOC_FL_ZERO_RANGE) does not clear the flag here: it
         * was tried and the range stayed unwritten. One sequential zero write
         * does clear it. That cost is paid once per donated chunk and
         * amortised over every append into it, which is the same trade
         * batched preallocation makes.
         * Do it here, not on the first append, so it stays off the write path.
         *
         * OFF THE CRITICAL PATH -- and this must be respected when measuring.
         * In the paper's design the space for future appends is prepared ahead
         * of demand, asynchronously, precisely so the append itself never waits
         * for it. Timing a donation together with the append that follows would
         * charge preparation to the write and understate the mechanism. Any
         * benchmark must either warm the region first and time only steady-state
         * appends, or report preparation as a separate line. */
        if (got > 0)
            moved += got;
        if (ioctl_rc != 0) {
            rc = -ioctl_errno;
            if (got > 0)
                p->poisoned = 1;
            break;
        }
        if (got == 0) break;               /* no progress: stop, do not spin */
    }

    if (moved < want) {
        /* Hand back the part we preallocated but could not fill from the
         * pool, so a failed or partial extend does not silently turn into a
         * plain fallocate. */
        if (exitos_internal_fallocate_call(
                target_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                (off_t)(file_off + moved), (off_t)(want - moved)) != 0) {
            /* best effort */
        }
        if ((uint64_t)old_size < file_off + want) {
            off_t back = (off_t)(file_off + moved);
            if (old_size > back) back = old_size;
            if (exitos_internal_ftruncate_call(target_fd, back) != 0) {
                /* best effort */
            }
        }
    }
    if (moved == 0) return rc ? rc : -ENOSPC;

    runs_take_head(df, ri, moved);
    chunk_add_reserved(p, st.st_dev, st.st_ino, file_off, moved, fi, doff);
    if (effect)
        effect->moved_bytes = moved;
    {
        int fill_rc = zero_fill_exact(target_fd, file_off, moved, p->blk);
        if (fill_rc != 0)
            return fill_rc;
    }
    if (rc != 0)
        return rc;
    return (int64_t)moved;
}

int64_t donor_extend(struct donor_pool *p, int target_fd,
                     uint64_t file_off, uint64_t bytes)
{
    int64_t rc;
    if (!p) return -EINVAL;
    pool_lock(p);
    rc = donor_extend_locked(p, target_fd, file_off, bytes, NULL);
    pool_unlock(p);
    return rc;
}

/* Extent flags for which FIEMAP's physical location cannot safely back raw
 * writes. Keep this in sync with extent.c; UNWRITTEN is added separately
 * because it has a physical address but filesystem reads hide raw writes. */
#define DONOR_FIEMAP_UNSAFE (FIEMAP_EXTENT_UNKNOWN        | \
                             FIEMAP_EXTENT_DELALLOC       | \
                             FIEMAP_EXTENT_ENCODED        | \
                             FIEMAP_EXTENT_DATA_ENCRYPTED | \
                             FIEMAP_EXTENT_NOT_ALIGNED    | \
                             FIEMAP_EXTENT_DATA_INLINE    | \
                             FIEMAP_EXTENT_DATA_TAIL      | \
                             FIEMAP_EXTENT_SHARED         | \
                             FIEMAP_EXTENT_UNWRITTEN)

#define DONOR_FIEMAP_BATCH 128u

static int donor_verify_exact_fiemap(int fd, uint64_t file_off,
                                     uint64_t bytes)
{
    struct fiemap *fm;
    size_t alloc_bytes;
    uint64_t cursor = file_off;
    uint64_t end = file_off + bytes;
    int rc = 0;

    alloc_bytes = sizeof *fm +
                  (size_t)DONOR_FIEMAP_BATCH * sizeof(struct fiemap_extent);
    fm = calloc(1, alloc_bytes);
    if (!fm)
        return -ENOMEM;

    while (cursor < end) {
        uint64_t before = cursor;
        unsigned i;
        int saw_last = 0;

        memset(fm, 0, alloc_bytes);
        fm->fm_start = cursor;
        fm->fm_length = end - cursor;
        fm->fm_flags = FIEMAP_FLAG_SYNC;
        fm->fm_extent_count = DONOR_FIEMAP_BATCH;
        do {
            rc = ioctl(fd, FS_IOC_FIEMAP, fm);
        } while (rc != 0 && errno == EINTR);
        if (rc != 0) {
            rc = -(errno ? errno : EIO);
            break;
        }
        if (fm->fm_mapped_extents == 0) {
            rc = -ENXIO;
            break;
        }
        if (fm->fm_mapped_extents > DONOR_FIEMAP_BATCH) {
            rc = -EIO;
            break;
        }

        for (i = 0; i < fm->fm_mapped_extents && cursor < end; i++) {
            const struct fiemap_extent *fe = &fm->fm_extents[i];
            uint64_t fe_end;

            if (fe->fe_flags & FIEMAP_EXTENT_LAST)
                saw_last = 1;
            if (fe->fe_length == 0 || fe->fe_physical == 0 ||
                (fe->fe_flags & DONOR_FIEMAP_UNSAFE)) {
                rc = -EUCLEAN;
                goto out;
            }
            if (fe->fe_logical > UINT64_MAX - fe->fe_length) {
                rc = -EUCLEAN;
                goto out;
            }
            fe_end = fe->fe_logical + fe->fe_length;
            if (fe_end <= cursor)
                continue;
            if (fe->fe_logical > cursor) {
                rc = -ENXIO;
                goto out;
            }
            cursor = fe_end < end ? fe_end : end;
        }
        if (cursor >= end)
            break;
        if (saw_last) {
            rc = -ENXIO;
            break;
        }
        if (cursor <= before) {
            rc = -EIO;
            break;
        }
    }

out:
    free(fm);
    return rc;
}

int donor_prepare_exact(struct donor_pool *p, int target_fd,
                        uint64_t file_off, uint64_t bytes,
                        struct donor_prepare_report *report)
{
    struct donor_prepare_report local;
    struct stat st;
    uint64_t done = 0;
    int rc = 0;

    if (!report)
        report = &local;
    memset(report, 0, sizeof *report);
    report->requested_bytes = bytes;

    if (!p)
        return -EINVAL;
    pool_lock(p);
    rc = target_check(p, target_fd, &st);
    if (rc != 0)
        goto out;
    if (file_off % p->blk || bytes % p->blk) {
        rc = -EINVAL;
        goto out;
    }
    if (file_off > UINT64_MAX - bytes) {
        rc = -EOVERFLOW;
        goto out;
    }
    if (bytes == 0) {
        report->flags |= DONOR_PREPARE_VERIFIED;
        rc = 0;
        goto out;
    }

    while (done < bytes) {
        struct donor_extend_effect effect;
        int64_t got = donor_extend_locked(p, target_fd, file_off + done,
                                          bytes - done, &effect);

        if (effect.target_mutated)
            report->flags |= DONOR_PREPARE_MUTATED;
        if (effect.moved_bytes > 0) {
            if (effect.moved_bytes > bytes - done) {
                rc = -EIO;
                goto out;
            }
            done += effect.moved_bytes;
            report->prepared_bytes = done;
            report->chunks++;
        }
        if (got < 0) {
            rc = (int)got;
            goto out;
        }
        if (got == 0 || (uint64_t)got != effect.moved_bytes) {
            rc = -EIO;
            goto out;
        }
    }

    do {
        rc = exitos_internal_fdatasync_call(target_fd);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        rc = -(errno ? errno : EIO);
        goto out;
    }
    report->flags |= DONOR_PREPARE_SYNCED;

    rc = donor_verify_exact_fiemap(target_fd, file_off, bytes);
    if (rc != 0)
        goto out;
    report->flags |= DONOR_PREPARE_VERIFIED;
    rc = 0;

out:
    pool_unlock(p);
    return rc;
}

/* ---- reclaim ------------------------------------------------------------ */
static int donor_reclaim_locked(struct donor_pool *p, int target_fd, uint64_t from_off)
{
    struct stat st;
    unsigned i;
    int rc;

    if (!p) return -EINVAL;
    rc = target_check(p, target_fd, &st);
    if (rc != 0) return rc;
    if ((uint64_t)st.st_size <= from_off) return 0;   /* past EOF: no-op */

    /* Give the pool back every chunk that lives at or after from_off.  The
     * blocks the target is holding are the donor's originals; the donor is
     * holding the blocks that were freshly allocated for the target and never
     * written, so recycled space still reads as zeros. */
    for (i = 0; i < p->nchunks; ) {
        struct donor_chunk *c = &p->chunks[i];

        if (c->dev != st.st_dev || c->ino != st.st_ino ||
            c->file_off + c->len <= from_off) {
            i++;
            continue;
        }
        if (c->file_off >= from_off) {
            runs_free(&p->files[c->donor_idx], c->donor_off, c->len);
            chunk_drop(p, i);
        } else {
            uint64_t keep = from_off - c->file_off;
            runs_free(&p->files[c->donor_idx], c->donor_off + keep,
                      c->len - keep);
            c->len = keep;
            i++;
        }
    }

    if (exitos_internal_ftruncate_call(target_fd, (off_t)from_off) != 0)
        return -errno;
    return 0;
}

int donor_reclaim(struct donor_pool *p, int target_fd, uint64_t from_off)
{
    int rc;
    if (!p) return -EINVAL;
    pool_lock(p);
    rc = donor_reclaim_locked(p, target_fd, from_off);
    pool_unlock(p);
    return rc;
}

/* ---- write-size feedback ------------------------------------------------ */
static uint64_t donor_next_chunk_locked(struct donor_pool *p, uint64_t observed_write)
{
    uint64_t avg, chunk;

    if (!p) return 0;

    if (p->nhist == DONOR_HIST_WIN) p->hsum -= p->hist[p->hpos];
    else                            p->nhist++;
    p->hist[p->hpos] = observed_write;
    p->hsum += observed_write;
    p->hpos = (p->hpos + 1) % DONOR_HIST_WIN;

    avg = p->hsum / p->nhist;
    chunk = align_down(4 * avg, p->blk);
    if (chunk < p->blk) chunk = p->blk;          /* never ask for nothing */
    if (chunk > p->bytes_each) chunk = align_down(p->bytes_each, p->blk);
    return chunk;
}

uint64_t donor_next_chunk(struct donor_pool *p, uint64_t observed_write)
{
    uint64_t rc;
    if (!p) return 0;
    pool_lock(p);
    rc = donor_next_chunk_locked(p, observed_write);
    pool_unlock(p);
    return rc;
}
