/* intercept: the userspace decision hook.
 *
 * The paper describes redirecting a write from inside the kernel, at a syscall
 * tracepoint. A tracepoint cannot do that: it observes, it does not divert. So
 * the decision is made here, in user space, with the discipline namei_ext uses
 * for its own single hook:
 *
 *   ONE decision function per operation, DEFAULT PASS, act only on fds that
 *   were explicitly registered, and hand the call back to the normal kernel
 *   path on any doubt whatsoever.
 *
 * "Any doubt" is meant literally. Every one of these returns EXITOS_PASS and
 * leaves *result untouched:
 *
 *   - a NULL context, a negative fd, an fd this context never registered;
 *   - a NULL buffer, a zero-length write, a negative offset, a NULL result
 *     pointer (a takeover the caller cannot read back is worse than no
 *     takeover);
 *   - an offset or a length that is not a whole number of device logical
 *     blocks, or a buffer that is not aligned for O_DIRECT;
 *   - an offset the Maco does not map (a hole, or past the mapped end);
 *   - a write that starts inside a mapped run but reaches past its end -- half
 *     of a straddling write at the right LBA and half of it at a wrong one is
 *     exactly the corruption this module exists to prevent;
 *   - an LBA range that is not fully inside THIS file's own extents;
 *   - any failure from the raw write itself. The caller then reissues the same
 *     bytes through the kernel, so the data converges either way.
 *
 * WHY THE LBA BOUNDS CHECK IS A SEPARATE STRUCTURE
 * ------------------------------------------------
 * The Maco answers "which LBA holds file offset X". It cannot answer "does LBA
 * Y belong to this file", because it is indexed by file offset. A bug that
 * produced a plausible-but-wrong LBA -- a stale mapping after truncation, a
 * geometry with the wrong partition start -- would sail straight through a
 * lookup-only check and overwrite another file. So each registration also
 * carries the file's device footprint as a sorted, merged array of
 * [lba, lba+blocks) intervals, and no raw write is issued unless the whole
 * target range sits inside one of them. Both structures are searched with a
 * binary search: O(log n), never a list walk, since removing the extent-tree
 * walk from the write path is the entire point of the design being reproduced.
 *
 * SAFETY GATE ON REAL HARDWARE
 * ----------------------------
 * Registering a file arms a raw-LBA write path on the block device underneath
 * it. In this reproduction that is allowed without ceremony only for a loop
 * device (the test fixture). Any other device -- including a perfectly
 * ordinary NVMe partition that geom classifies as EXITOS_DEV_OK -- must be
 * named explicitly in the EXITOS_DEV environment variable, which is the same
 * opt-in the repository's tier-2 tests use. Without it registration is refused
 * and the fd simply keeps the normal path.
 *
 * KNOWN LIMITATION, STATED RATHER THAN HIDDEN
 * -------------------------------------------
 * A taken-over write goes to the device and not through the page cache, so a
 * later BUFFERED read of that range on the same file can still be served from
 * a stale cached page. Readers must use O_DIRECT, which is what a log writer
 * of this shape does anyway. Dropping the cached pages on every takeover would
 * mean one more syscall per write, which is precisely the cost the design sets
 * out to remove.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/fiemap.h>

#include "exitos_intercept.h"
#include "exitos_durability.h"
#include "exitos_extent.h"
#include "exitos_geom.h"
#include "exitos_iopath.h"
#include "exitos_devguard.h"
#include "exitos_maco.h"
#include "exitos_durability.h"
#include "exitos_stats.h"

/* ------------------------------------------------------------------ *
 * state
 * ------------------------------------------------------------------ */

/* One mapped run of the file, in the same units as struct maco_entry:
 * file_off and len are bytes, lba counts device logical blocks. */
struct run {
    uint64_t file_off;
    uint64_t lba;
    uint64_t len;
};

/* Half-open device interval [lo, hi) in logical blocks. */
struct lba_iv {
    uint64_t lo;
    uint64_t hi;
};

struct reg {
    pthread_mutex_t    mu;       /* decision -> I/O/syscall -> note transaction */
    int                active;   /* false after successful close/dup replacement */
    int                fd;
    struct exitos_geom g;
    struct maco       *m;       /* file offset -> LBA, binary searched      */
    struct iopath     *io;      /* open handle on g.disk_path               */
    struct run        *runs;    /* sorted by file_off, source for iv        */
    size_t             nruns;
    struct lba_iv     *iv;      /* sorted by lo, merged, non-overlapping    */
    size_t             niv;
    int                dur;     /* exitos_durability for g.disk_path        */
    uint64_t           refreshes;/* extent re-extractions (see on_write)    */
    uint64_t           passed;  /* writes on this fd handed to the kernel   */
    /* Set when a write on this fd is handed to the kernel, and cleared when the
     * caller reports that a real fdatasync on it returned success -- at that
     * point the kernel is holding nothing unpersisted and the shortcut may
     * engage again. `passed` cannot serve this purpose because it is also the
     * refresh throttle and is reset whenever the layout snapshot is rebuilt.
     *
     * This used to be sticky, on the grounds that the layer could not see
     * whether the kernel's fdatasync succeeded. It could not, so the interposer
     * now tells it (exitos_note_kernel_sync). Leaving it sticky meant one early
     * fallback disabled the shortcut for the life of the descriptor: measured on
     * a preallocated write-ahead-log workload, 0 of 801 fdatasync calls were
     * ever answered by this layer. */
    int      kernel_holds;
    uint32_t refresh_gap;            /* grows geometrically after a fruitless look */
    uint32_t refresh_fails;          /* consecutive looks that found nothing usable */
    int      unflushed;              /* we wrote something the device may still hold */
    uint64_t isize;                  /* file size as we last saw it */
    uint64_t partial_off;            /* start of a block a truncation split, +1 */
    int      append;                 /* O_APPEND: the kernel picks the offset */
    dev_t    id_dev;                 /* file identity captured at registration */
    ino_t    id_ino;
    /* The fd was opened without O_DIRECT: raw writes then
     * need page-cache coherence work -- takeover only with no kernel-side
     * debt, and an invalidation of every raw-written range. */
    int      buffered;
};

/* One pending page-cache invalidation: [off, off+len) of fd, already widened
 * to full page boundaries. */
struct exitos_coh_ent {
    int      fd;
    uint64_t off;
    uint64_t len;
};
#define EXITOS_COH_QCAP 256u

/* Steady-state transactions only need to freeze the registration belonging to
 * their own fd.  A single process-wide rwlock made every pwrite/fdatasync pair
 * dirty one shared cache line even though readers were logically concurrent.
 * Keep the simple sorted table, but distribute its lifetime pins over
 * cacheline-separated shards.  Rare table writers take every shard in numeric
 * order, preserving the old global-freeze semantics. */
#ifdef EXITOS_TABLE_GLOBAL_BASELINE
#define EXITOS_TABLE_SHARDS 1u
#else
#define EXITOS_TABLE_SHARDS 64u
#endif
struct table_lock_shard {
    pthread_rwlock_t rw;
} __attribute__((aligned(64)));

struct exitos_ctx {
    struct reg **r;             /* stable heap records, sorted by fd        */
    size_t      n;
    size_t      cap;
    struct table_lock_shard table_rw[EXITOS_TABLE_SHARDS];
    int         verify_identity;/* off by default: it costs one fstat/write */
    /* Strict mode (the paper's Exitos-S): before every taken-over write,
     * issue a zero-length pwrite on the intercepted fd through the internal
     * real-syscall channel.  The kernel runs exactly its per-write permission
     * gate on it -- the FMODE_WRITE check and security_file_permission, the
     * hook SELinux/AppArmor evaluate per I/O -- with no data-path effect.  A
     * refusal sends the write down the ordinary path, so the kernel itself
     * reports the error, byte-identical to a system without this library.
     * fdatasync is deliberately not probed: the vanilla fdatasync path runs
     * no LSM file hook, and probing it would be stricter than the baseline. */
    int         strict;
    iopath_backend backend;     /* immutable selection, snapped at create   */
    int         backend_explicit;/* explicit selection never falls back    */
    _Atomic int poisoned;       /* permanent fail-closed frontend state      */

    /* Asynchronous page-cache invalidation for buffered registrations: the
     * write path enqueues raw-written ranges; a lazily-started invalidator
     * thread drains them with POSIX_FADV_DONTNEED off the critical path.
     * When the thread is absent (never started, failed to start, or lost to
     * fork) the note falls back to an inline call, and a full queue does the
     * same -- an invalidation is never dropped. */
    struct exitos_coh_ent coh_q[EXITOS_COH_QCAP];
    unsigned    coh_head;
    unsigned    coh_tail;
    pthread_mutex_t coh_mu;
    pthread_cond_t  coh_cv;
    pthread_t   coh_thread;
    int         coh_thread_on;
    int         coh_stop;
};

static unsigned table_shard_for_fd(int fd)
{
    /* Descriptor numbers are commonly consecutive.  The multiplicative mix
     * prevents adjacent fds from repeatedly occupying adjacent cache sets,
     * while the power-of-two mask keeps this off the measurable hot path. */
    return ((unsigned)fd * 2654435761u) & (EXITOS_TABLE_SHARDS - 1u);
}

static void table_rdlock_fd(struct exitos_ctx *c, int fd)
{
    pthread_rwlock_rdlock(&c->table_rw[table_shard_for_fd(fd)].rw);
}

static void table_rdunlock_fd(struct exitos_ctx *c, int fd)
{
    pthread_rwlock_unlock(&c->table_rw[table_shard_for_fd(fd)].rw);
}

static void table_rdlock_pair(struct exitos_ctx *c, int first_fd, int second_fd)
{
    unsigned a = table_shard_for_fd(first_fd);
    unsigned b = second_fd >= 0 ? table_shard_for_fd(second_fd) : a;

    if (b < a) {
        unsigned t = a;
        a = b;
        b = t;
    }
    pthread_rwlock_rdlock(&c->table_rw[a].rw);
    if (b != a)
        pthread_rwlock_rdlock(&c->table_rw[b].rw);
}

static void table_rdunlock_pair(struct exitos_ctx *c, int first_fd,
                                int second_fd)
{
    unsigned a = table_shard_for_fd(first_fd);
    unsigned b = second_fd >= 0 ? table_shard_for_fd(second_fd) : a;

    if (b < a) {
        unsigned t = a;
        a = b;
        b = t;
    }
    if (b != a)
        pthread_rwlock_unlock(&c->table_rw[b].rw);
    pthread_rwlock_unlock(&c->table_rw[a].rw);
}

static void table_wrlock_all(struct exitos_ctx *c)
{
    unsigned i;

    for (i = 0; i < EXITOS_TABLE_SHARDS; i++)
        pthread_rwlock_wrlock(&c->table_rw[i].rw);
}

static void table_wrunlock_all(struct exitos_ctx *c)
{
    unsigned i = EXITOS_TABLE_SHARDS;

    while (i-- > 0)
        pthread_rwlock_unlock(&c->table_rw[i].rw);
}

_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "signal-path context poison must be lock-free");

#define CTX_CAP0      8u
#define RUNS_CAP_MAX  (1 << 20)   /* 1M extents: past this the file is not a log */

/* Extents read per attempt. Overridable at compile time so a test can force the
 * grow-and-retry path without needing a file with hundreds of extents. */
#ifndef RUNS_CAP0
#define RUNS_CAP0     64
#endif

/* ------------------------------------------------------------------ *
 * registration table: sorted by fd, binary searched
 * ------------------------------------------------------------------ */

/* Sets *pos to the index of fd, or to where it would be inserted.
 * Returns 1 when fd is present, 0 when it is not. */
static int reg_lookup(const struct exitos_ctx *c, int fd, size_t *pos)
{
    size_t lo = 0, hi = c->n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c->r[mid]->fd < fd)
            lo = mid + 1;
        else
            hi = mid;
    }
    *pos = lo;
    return lo < c->n && c->r[lo]->fd == fd;
}

static void reg_release(struct reg *r)
{
    if (!r)
        return;
    if (r->io)
        iopath_close(r->io);
    maco_destroy(r->m);
    free(r->runs);
    free(r->iv);
    pthread_mutex_destroy(&r->mu);
    free(r);
}

static int ctx_reserve(struct exitos_ctx *c, size_t want)
{
    struct reg **nr;
    size_t cap;

    if (want <= c->cap)
        return 0;
    cap = c->cap ? c->cap : CTX_CAP0;
    while (cap < want) {
        if (cap > SIZE_MAX / 2 / sizeof(*c->r))
            return -ENOMEM;
        cap *= 2;
    }
    nr = realloc(c->r, cap * sizeof(*c->r));
    if (!nr)
        return -ENOMEM;
    c->r = nr;
    c->cap = cap;
    return 0;
}

/* ------------------------------------------------------------------ *
 * the file's device footprint
 * ------------------------------------------------------------------ */

static int iv_cmp(const void *a, const void *b)
{
    const struct lba_iv *x = a, *y = b;

    if (x->lo < y->lo) return -1;
    if (x->lo > y->lo) return 1;
    if (x->hi < y->hi) return -1;
    if (x->hi > y->hi) return 1;
    return 0;
}

/* Rebuild the merged LBA intervals from r->runs. Returns 0 or -errno; on
 * failure the interval list is left EMPTY, which refuses every write rather
 * than permitting one on stale information. */
/* How many writes may go to the kernel before we re-read the file's layout.
 *
 * A blind refresh budget does not work here. A log writer opens the file, then
 * preallocates it, then initialises it. Between fallocate() and the first real
 * write every extent is UNWRITTEN, and src/extent.c deliberately refuses to map
 * those (ext4 reads them back as zeros until a normal write converts them, so a
 * raw LBA write would be invisible through the filesystem). So during that phase
 * a refresh CANNOT succeed, and a fixed budget is spent entirely on attempts
 * that were doomed -- leaving none for the moment the extents become usable.
 *
 * Each write handed back to the kernel may have converted one extent, so that
 * count is the signal that the layout has actually changed. Refreshing every
 * Nth passed write is self-limiting: once a refresh succeeds, writes stop being
 * passed, so the refreshes stop too. */
#define EXITOS_REFRESH_EVERY 8
/* How far the gap between re-reads of the layout is allowed to grow. A file
 * that is being appended to never becomes fully mapped -- the block a write
 * needs is created by that write -- so a fixed gap of 8 meant roughly one
 * re-read per 8 writes forever. On an appending write+fdatasync workload the
 * cost of those unbounded re-reads dominated everything the shortcut saved.
 *
 * The gap doubles after each look that finds nothing usable and is reset by one
 * that does. It is bounded rather than abandoned, because a file CAN become
 * mappable later -- space preallocated behind it, or another writer
 * initialising the range -- and a registration that gave up permanently would
 * never notice. At the cap the cost is one FIEMAP per 4096 writes. */
#define EXITOS_REFRESH_MAX_GAP   4096

static int iv_rebuild(struct reg *r)
{
    struct lba_iv *iv;
    uint64_t lbs = r->g.lbs;
    size_t i, k = 0, w;

    free(r->iv);
    r->iv = NULL;
    r->niv = 0;

    if (r->nruns == 0 || lbs == 0)
        return 0;

    iv = calloc(r->nruns, sizeof(*iv));
    if (!iv)
        return -ENOMEM;

    for (i = 0; i < r->nruns; i++) {
        uint64_t blocks;

        if (r->runs[i].len == 0)
            continue;
        if (r->runs[i].len > UINT64_MAX - (lbs - 1)) {
            free(iv);
            return -EOVERFLOW;
        }
        blocks = (r->runs[i].len + lbs - 1) / lbs;   /* a partial block counts */
        if (r->runs[i].lba > UINT64_MAX - blocks) {
            free(iv);
            return -EOVERFLOW;
        }
        iv[k].lo = r->runs[i].lba;
        iv[k].hi = r->runs[i].lba + blocks;
        k++;
    }
    if (k == 0) {
        free(iv);
        return 0;
    }

    qsort(iv, k, sizeof(*iv), iv_cmp);

    /* Merge overlapping and touching intervals, so a file laid out as several
     * device-contiguous extents answers as one range. */
    w = 0;
    for (i = 1; i < k; i++) {
        if (iv[i].lo <= iv[w].hi) {
            if (iv[i].hi > iv[w].hi)
                iv[w].hi = iv[i].hi;
        } else {
            w++;
            iv[w] = iv[i];
        }
    }

    r->iv = iv;
    r->niv = w + 1;
    return 0;
}

/* Is [lba, lba+blocks) entirely inside one of this file's intervals? */
static int iv_covers(const struct reg *r, uint64_t lba, uint64_t blocks)
{
    size_t lo = 0, hi = r->niv;
    const struct lba_iv *e;

    if (blocks == 0 || r->niv == 0)
        return 0;
    if (lba > UINT64_MAX - blocks)
        return 0;

    /* last interval whose lo <= lba */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (r->iv[mid].lo <= lba)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return 0;
    e = &r->iv[lo - 1];
    return lba >= e->lo && lba + blocks <= e->hi;
}

/* Read fd's extents and translate them to LBAs, keeping exactly the runs that
 * exitos_extent_load_maco also loads: FIEMAP_EXTENT_UNWRITTEN is dropped here
 * for the same reason it is dropped there -- ext4 reads such a range back as
 * zeros until a normal write converts the extent, so a raw write into it would
 * land on the platter and be invisible through the filesystem.
 * Returns 0 and fills in the two out parameters, or a negative errno and
 * leaves the run pointer NULL. */
static int runs_load(int fd, const struct exitos_geom *g,
                     struct run **out, size_t *nout, int want_sync)
{
    struct extent_run *er = NULL;
    struct run *rr;
    int cap = RUNS_CAP0, n = 0, i;
    size_t k = 0;

    *out = NULL;
    *nout = 0;

    for (;;) {
        struct extent_run *tmp = realloc(er, (size_t)cap * sizeof(*er));

        if (!tmp) {
            free(er);
            return -ENOMEM;
        }
        er = tmp;
        n = exitos_extent_read_sync(fd, er, cap, want_sync);
        if (n < 0) {
            free(er);
            return n;
        }
        if (n < cap)
            break;                      /* the whole file fitted */
        if (cap >= RUNS_CAP_MAX) {
            free(er);
            return -E2BIG;
        }
        cap *= 2;
    }

    if (n == 0) {
        free(er);
        return 0;                       /* empty file: nothing mapped, no error */
    }

    rr = calloc((size_t)n, sizeof(*rr));
    if (!rr) {
        free(er);
        return -ENOMEM;
    }

    for (i = 0; i < n; i++) {
        uint64_t lba;
        int rc;

        if (er[i].flags & FIEMAP_EXTENT_UNWRITTEN)
            continue;
        if (er[i].len == 0)
            continue;
        rc = exitos_geom_fsblock_to_lba(g, er[i].fs_block, &lba);
        if (rc != 0) {
            free(rr);
            free(er);
            return rc < 0 ? rc : -EINVAL;
        }
        rr[k].file_off = er[i].file_off;
        rr[k].lba      = lba;
        rr[k].len      = er[i].len;
        k++;
    }

    free(er);
    *out = rr;                          /* FIEMAP reports ascending offsets */
    *nout = k;
    return 0;
}

/* Populate Maco from the exact translated extent snapshot that also backs
 * runs/iv.  Maco and the ownership bounds are a correlated safety proof: if
 * they are built by two independent FIEMAP walks, a concurrent remap can make
 * a logical offset point at a different LBA that still happens to belong to
 * this same file.  A bounds-only check then passes and a raw write corrupts
 * another logical block.  One snapshot is therefore the unit of publication. */
static int maco_load_runs(struct maco *m, const struct run *runs, size_t nruns)
{
    size_t i;

    if (!m || (nruns != 0 && !runs))
        return -EINVAL;
    for (i = 0; i < nruns; i++) {
        int rc = maco_insert(m, runs[i].file_off, runs[i].lba, runs[i].len);

        if (rc != 0)
            return rc < 0 ? rc : -EINVAL;
    }
    return 0;
}

/* Drop everything at or after `cut`, trimming the run that spans it. `cut` is
 * already rounded down to a whole device block by the caller, so every
 * surviving run still ends on a block boundary. */
static void runs_trim_from(struct reg *r, uint64_t cut)
{
    size_t i, k = 0;

    for (i = 0; i < r->nruns; i++) {
        struct run s = r->runs[i];

        if (s.file_off >= cut)
            continue;
        if (s.file_off + s.len > cut)
            s.len = cut - s.file_off;
        r->runs[k++] = s;
    }
    r->nruns = k;
}

/* ------------------------------------------------------------------ *
 * registration
 * ------------------------------------------------------------------ */

/* The path of an open fd, verified to still name that exact inode. Returns 0
 * or a negative errno. Without the identity check a renamed or replaced file
 * could hand us the geometry of a different filesystem. */
static int path_of_fd(int fd, const struct stat *st, char *out, size_t n)
{
    char link[64];
    struct stat st2;
    ssize_t k;
    int len;

    len = snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    if (len < 0 || (size_t)len >= sizeof link)
        return -EINVAL;

    k = readlink(link, out, n - 1);
    if (k < 0)
        return -errno;
    if (k == 0 || (size_t)k >= n - 1)
        return -ENAMETOOLONG;
    out[k] = '\0';
    if (out[0] != '/')
        return -EINVAL;                 /* anonymous inode: pipe, socket, ... */
    if (stat(out, &st2) != 0)
        return -errno;                  /* deleted or unreachable */
    if (st2.st_dev != st->st_dev || st2.st_ino != st->st_ino)
        return -ESTALE;
    return 0;
}

/* Arming a raw-LBA write path on real hardware must be a deliberate act. */
static int raw_write_allowed(const struct exitos_geom *g)
{
    const char *dev = getenv("EXITOS_DEV");
    char part[80];
    int n;

    if (!g->writable_raw)
        return 0;
    if (g->devclass == EXITOS_DEV_LOOP)
        return 1;                       /* the test fixture, harmless by design */
    if (!dev || dev[0] == '\0')
        return 0;
    if (strcmp(dev, g->disk_path) == 0)
        return 1;
    n = snprintf(part, sizeof part, "/dev/%s", g->part_name);
    if (n < 0 || (size_t)n >= sizeof part)
        return 0;
    return strcmp(dev, part) == 0;
}

/* Which way the shortcut issues its writes. EXITOS_IOPATH names the backend;
 * the accepted names are pwrite, nvme, uring, uringpoll and uringwritepoll.
 * The name is resolved once, when the context is initialised, and the choice
 * then stays fixed for the life of that context; the fallbacks below are tried
 * in order when the named backend cannot be opened.
 *
 * Resolution was briefly changed to pick NVMe passthru on its own for an NVMe
 * namespace, on the strength of a measurement that showed the block-device
 * backend losing badly. That measurement did not reproduce: re-run on the same
 * disk with the same commands, the two backends came out the same, and a
 * careful sweep put NVMe passthru BEHIND the block-device path from 8 KiB
 * upward, with only a slight edge at 4 KiB. Picking passthru without being
 * asked would therefore trade a small gain on the smallest writes for a large
 * loss on everything bigger, so passthru is something the operator names.
 */
static int backend_from_name(const char *s, iopath_backend *backend,
                             int *explicit_mode)
{
    static const struct {
        const char *name;
        iopath_backend backend;
    } modes[] = {
        { "pwrite", IOPATH_PWRITE },
        { "nvme", IOPATH_NVME_IOCTL },
        { "uring", IOPATH_URING_CMD },
        { "uringpoll", IOPATH_URING_CMD_POLL },
        { "uringwritepoll", IOPATH_URING_WRITE_POLL },
    };
    size_t i;

    if (!backend || !explicit_mode)
        return -EINVAL;
    if (s) {
        for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
            if (strcmp(s, modes[i].name) == 0) {
                *backend = modes[i].backend;
                *explicit_mode = 1;
                return 0;
            }
        }
        return -EINVAL;
    }
    /* What is recorded here is a preference, not a requirement: explicit_mode
     * stays 0, so the ordered fallbacks below may replace this backend.
     * iopath_open refuses IOPATH_URING_CMD_POLL on a device whose driver was
     * given no poll queues (nvme.poll_queues defaults to 0), so the request is
     * made and allowed to fall back rather than making the whole registration
     * depend on a boot-time setting. */
    *backend = IOPATH_URING_CMD_POLL;
    *explicit_mode = 0;
    return 0;
}

/* The backend the operator asked for, then the ordered fallbacks.  Returning a
 * handle from a lower preference is better than refusing to register: the
 * shortcut still works, only without polled completion. */
static struct iopath *iopath_open_preferred(const struct exitos_ctx *ctx,
                                            const struct exitos_geom *g,
                                            dev_t file_dev)
{
    iopath_backend want = ctx->backend;
    struct iopath *io;

    /* The two polled paths intentionally cross different kernel boundaries.
     * uringpoll opens the paired /dev/ng node and submits URING_CMD, bypassing
     * both the filesystem and normal block layer.  uringwritepoll opens the
     * exact block object behind this file and submits ordinary IORING_OP_WRITE,
     * bypassing ext4 while retaining block-layer mapping.  Binding it from the
     * regular file's st_dev prevents a remembered whole-disk name from silently
     * replacing the mounted partition that actually owns the extents. */
    if (want == IOPATH_URING_WRITE_POLL) {
        char block_path[128];
        struct stat bst;
        int block_fd, n;

        n = snprintf(block_path, sizeof block_path, "/dev/block/%u:%u",
                     major(file_dev), minor(file_dev));
        if (n < 0 || (size_t)n >= sizeof block_path)
            return NULL;
        block_fd = exitos_internal_open_call(block_path,
                    O_RDWR | O_DIRECT | O_CLOEXEC, 0);
        if (block_fd < 0)
            return NULL;
        if (fstat(block_fd, &bst) != 0 || !S_ISBLK(bst.st_mode) ||
            bst.st_rdev != file_dev) {
            (void)exitos_internal_close_call(block_fd);
            return NULL;
        }
        io = iopath_open_fd(block_fd, want, g->lbs);
        (void)exitos_internal_close_call(block_fd);
        return io; /* explicit block selection is fail-closed, never fallback */
    }

    io = iopath_open(g->disk_path, want, g->lbs);

    if (io || ctx->backend_explicit)
        return io;                     /* an explicit request is not overridden */
    io = iopath_open(g->disk_path, IOPATH_NVME_IOCTL, g->lbs);
    if (io)
        return io;
    return iopath_open(g->disk_path, IOPATH_PWRITE, g->lbs);
}

static int reg_flush_raw_debt(struct reg *r);

static int reg_build(const struct exitos_ctx *ctx, int fd, struct reg **out)
{
    struct reg *reg;
    struct stat st;
    char path[PATH_MAX];
    int rc;

    *out = NULL;
    reg = calloc(1, sizeof(*reg));
    if (!reg)
        return -ENOMEM;
    if (pthread_mutex_init(&reg->mu, NULL) != 0) {
        free(reg);
        return -ENOMEM;
    }
    reg->active = 1;
    reg->fd = fd;

    if (fd < 0) {
        rc = -EINVAL;
        goto fail;
    }

    if (fstat(fd, &st) != 0) {
        rc = -errno;                       /* closed fd, or not an fd at all */
        goto fail;
    }
    /* Only a regular file has data extents to map. This one test refuses
     * pipes, sockets, directories and device nodes before anything else. */
    if (!S_ISREG(st.st_mode)) {
        rc = -EINVAL;
        goto fail;
    }

    rc = path_of_fd(fd, &st, path, sizeof path);
    if (rc != 0)
        goto fail;

    rc = exitos_geom_probe(path, &reg->g);
    if (rc != 0)
        goto fail;
    /* geom reports writable_raw = 0 for every stack it cannot translate
     * (dm, md, unknown, and any filesystem with no block device behind it). */
    if (!raw_write_allowed(&reg->g)) {
        rc = -EPERM;
        goto fail;
    }
    /* When the operator has named the disk by serial, that name wins over every
     * device path, because device paths move. A PCI re-probe once renumbered
     * the controllers on the test host: /dev/nvme2n1 stopped being the test
     * disk and became an unrelated drive, and a measurement command with the
     * device name typed into it went on using it. Nothing in this library
     * objected, because the per-write identity check is opt-in and registration
     * never looked at the serial at all. It does now. */
    {
        const char *want = getenv("EXITOS_EXPECT_SERIAL");
        /* The loop device is this project's test fixture and raw_write_allowed
         * above lets it through unconditionally; a loop device has no serial to
         * match, and raw writes to one land in its backing file. Checking the
         * serial here as well refused every fixture-based test the moment an
         * operator had a serial exported in their shell. */
        if (want && *want && reg->g.devclass != EXITOS_DEV_LOOP) {
            char have[256];
            if (exitos_devguard_identity(reg->g.part_name, have, sizeof have) != 0 ||
                strcmp(have, want) != 0) {
                rc = -EPERM;
                goto fail;
            }
        }
    }
    if (reg->g.lbs == 0 || reg->g.fs_bs == 0 ||
        reg->g.fs_bs % reg->g.lbs != 0) {
        rc = -EINVAL;
        goto fail;
    }

    rc = runs_load(fd, &reg->g, &reg->runs, &reg->nruns, 1);
    if (rc != 0)
        goto fail;

    reg->m = maco_create(reg->g.lbs);
    if (!reg->m) {
        rc = -ENOMEM;
        goto fail;
    }
    rc = maco_load_runs(reg->m, reg->runs, reg->nruns);
    if (rc != 0)
        goto fail;
    rc = iv_rebuild(reg);
    if (rc != 0)
        goto fail;

    reg->io = iopath_open_preferred(ctx, &reg->g, st.st_dev);
    /* Every use of reg->io in this core runs under the per-fd transaction
     * lock, so the handle's internal mutexes are redundant here. */
    if (reg->io)
        (void)iopath_set_exclusive(reg->io, 1);
    reg->dur = (int)exitos_durability_policy_for(reg->g.disk_path);
    if (reg->io && reg->dur == (int)EXITOS_DUR_FUA &&
        iopath_set_fua(reg->io, 1) != 0) {
        /* The selected backend cannot carry native NVMe FUA (notably the
         * pwrite backend).  Preserve the durability guarantee by
         * retaining a real flush at fdatasync instead of silently treating
         * each write as persistent. */
        reg->dur = (int)EXITOS_DUR_FLUSH;
    }
    reg->refresh_gap = 0;
    reg->refresh_fails = 0;
    {   /* An O_APPEND write goes wherever the kernel says the end of the file
         * is at the moment it runs, which is not the position lseek reports. */
        int fl = fcntl(fd, F_GETFL);
        reg->append = (fl >= 0 && (fl & O_APPEND)) ? 1 : 0;
    }
    {
        struct stat idst;
        if (fstat(fd, &idst) == 0) {
            reg->id_dev = idst.st_dev;
            reg->id_ino = idst.st_ino;
            reg->isize = (uint64_t)idst.st_size;
        }
    }
    if (!reg->io) {
        rc = -EACCES;
        goto fail;
    }

    *out = reg;
    return 0;

fail:
    reg_release(reg);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Page-cache coherence for buffered registrations.
 *
 * mm/fadvise.c facts this code is built on (verified v6.18-rc5):
 * POSIX_FADV_DONTNEED first queues asynchronous writeback of any DIRTY page
 * in the byte range, then drops only CLEAN, unmapped, unlocked pages, and
 * only pages the range covers COMPLETELY.  Hence: (1) the write path must
 * guarantee no dirty pages exist when it runs (the kernel_holds gate), or
 * the call itself would push stale data to the device; (2) ranges must be
 * widened to page boundaries or stale edge bytes survive; (3) mmap'ed pages
 * are never dropped -- shared mappings stay outside the support contract.
 * ------------------------------------------------------------------ */
static uint64_t coherence_page(void)
{
    static uint64_t pg;
    if (pg == 0) {
        long v = sysconf(_SC_PAGESIZE);
        pg = v > 0 ? (uint64_t)v : 4096u;
    }
    return pg;
}

static void coherence_drain(struct exitos_ctx *c)
{
    for (;;) {
        struct exitos_coh_ent e;
        pthread_mutex_lock(&c->coh_mu);
        if (c->coh_head == c->coh_tail) {
            pthread_mutex_unlock(&c->coh_mu);
            return;
        }
        e = c->coh_q[c->coh_head % EXITOS_COH_QCAP];
        c->coh_head++;
        pthread_mutex_unlock(&c->coh_mu);
        (void)exitos_internal_fadvise_call(e.fd, (off_t)e.off, (off_t)e.len,
                                           POSIX_FADV_DONTNEED);
    }
}

static void *coherence_thread_main(void *opaque)
{
    struct exitos_ctx *c = opaque;

    pthread_mutex_lock(&c->coh_mu);
    while (!c->coh_stop) {
        if (c->coh_head == c->coh_tail) {
            pthread_cond_wait(&c->coh_cv, &c->coh_mu);
            continue;
        }
        pthread_mutex_unlock(&c->coh_mu);
        coherence_drain(c);
        pthread_mutex_lock(&c->coh_mu);
    }
    pthread_mutex_unlock(&c->coh_mu);
    return NULL;
}

static void coherence_thread_ensure(struct exitos_ctx *c)
{
    pthread_mutex_lock(&c->coh_mu);
    if (!c->coh_thread_on && !c->coh_stop &&
        pthread_create(&c->coh_thread, NULL, coherence_thread_main, c) == 0)
        c->coh_thread_on = 1;   /* creation failure leaves inline mode */
    pthread_mutex_unlock(&c->coh_mu);
}

/* Record that [off, off+len) of fd was just raw-written.  Hot path: one short
 * critical section to enqueue (merging into the newest entry when the ranges
 * touch), or an inline fadvise when no invalidator thread will drain. */
static void coherence_note(struct exitos_ctx *c, int fd, uint64_t off,
                           uint64_t len)
{
    uint64_t pg = coherence_page();
    uint64_t lo = off & ~(pg - 1);
    uint64_t hi = (off + len + pg - 1) & ~(pg - 1);

    if (c->coh_thread_on) {
        pthread_mutex_lock(&c->coh_mu);
        if (c->coh_thread_on && c->coh_tail - c->coh_head < EXITOS_COH_QCAP) {
            if (c->coh_tail != c->coh_head) {
                struct exitos_coh_ent *t =
                    &c->coh_q[(c->coh_tail - 1) % EXITOS_COH_QCAP];
                if (t->fd == fd && lo <= t->off + t->len && t->off <= hi) {
                    if (lo < t->off) {
                        t->len += t->off - lo;
                        t->off = lo;
                    }
                    if (hi > t->off + t->len)
                        t->len = hi - t->off;
                    pthread_cond_signal(&c->coh_cv);
                    pthread_mutex_unlock(&c->coh_mu);
                    return;
                }
            }
            c->coh_q[c->coh_tail % EXITOS_COH_QCAP] =
                (struct exitos_coh_ent){ fd, lo, hi - lo };
            c->coh_tail++;
            pthread_cond_signal(&c->coh_cv);
            pthread_mutex_unlock(&c->coh_mu);
            return;
        }
        pthread_mutex_unlock(&c->coh_mu);
    }
    (void)exitos_internal_fadvise_call(fd, (off_t)lo, (off_t)(hi - lo),
                                       POSIX_FADV_DONTNEED);
}

/* Settle a buffered fd at registration: pages dirtied before registration are
 * flushed by one real fdatasync, and the now-clean cache is dropped whole
 * (fadvise length 0 means to the end of file) so raw writes start coherent.
 * A failed sync records kernel debt instead -- the shortcut then stays off
 * until the application's own fdatasync succeeds. */
static void reg_buffered_init_sync(int fd, struct reg *r)
{
    if (exitos_internal_fdatasync_call(fd) != 0) {
        r->kernel_holds = 1;
        return;
    }
    (void)exitos_internal_fadvise_call(fd, 0, 0, POSIX_FADV_DONTNEED);
}

static struct exitos_ctx *ctx_alloc(void)
{
    struct exitos_ctx *c = NULL;
    unsigned i;

    /* table_lock_shard is intentionally cacheline-over-aligned.  malloc/calloc
     * only promise max_align_t, so using calloc here placed every rwlock at a
     * non-64B offset (and made the over-aligned object itself undefined C).
     * Preserve calloc's zeroed-state contract with an explicitly aligned
     * allocation followed by memset. */
    if (posix_memalign((void **)&c, _Alignof(struct exitos_ctx),
                       sizeof(*c)) != 0)
        return NULL;
    memset(c, 0, sizeof(*c));
    for (i = 0; i < EXITOS_TABLE_SHARDS; i++) {
        if (pthread_rwlock_init(&c->table_rw[i].rw, NULL) != 0) {
            while (i-- > 0)
                pthread_rwlock_destroy(&c->table_rw[i].rw);
            free(c);
            return NULL;
        }
    }
    if (pthread_mutex_init(&c->coh_mu, NULL) != 0) {
        for (i = 0; i < EXITOS_TABLE_SHARDS; i++)
            pthread_rwlock_destroy(&c->table_rw[i].rw);
        free(c);
        return NULL;
    }
    if (pthread_cond_init(&c->coh_cv, NULL) != 0) {
        pthread_mutex_destroy(&c->coh_mu);
        for (i = 0; i < EXITOS_TABLE_SHARDS; i++)
            pthread_rwlock_destroy(&c->table_rw[i].rw);
        free(c);
        return NULL;
    }
    return c;
}

int exitos_ctx_create_with_options(struct exitos_ctx **out,
                                   const char *iopath_mode)
{
    struct exitos_ctx *c;
    iopath_backend backend;
    int explicit_mode;
    int rc;

    if (!out)
        return -EINVAL;
    *out = NULL;
    rc = backend_from_name(iopath_mode, &backend, &explicit_mode);
    if (rc != 0)
        return rc;
    c = ctx_alloc();
    if (!c)
        return -ENOMEM;
    c->backend = backend;
    c->backend_explicit = explicit_mode;
    *out = c;
    return 0;
}

struct exitos_ctx *exitos_ctx_create(void)
{
    struct exitos_ctx *c = NULL;
    int rc = exitos_ctx_create_with_options(&c, getenv("EXITOS_IOPATH"));

    if (rc != 0) {
        errno = -rc;
        return NULL;
    }
    return c;
}

void exitos_ctx_destroy(struct exitos_ctx *c)
{
    struct reg **old;
    size_t n;
    size_t i;

    if (!c)
        return;
    /* Context lifetime has the normal owner-quiesces-users contract.  The
     * writer lock also waits out any transaction that was already admitted.
     * Pay completed raw debt before releasing the only iopath able to flush it.
     * This API is void, so failure is necessarily best-effort; a caller that
     * needs a reportable durability boundary must sync/unregister first.
     * Continue across a failure so one bad target cannot suppress another
     * target's attempt. Detach before closing iopath fds: an interposer close
     * can re-enter its frontend and must not find a table-WR lock held. */
    table_wrlock_all(c);
    old = c->r;
    n = c->n;
    for (i = 0; i < n; i++)
        (void)reg_flush_raw_debt(old[i]);
    c->r = NULL;
    c->n = c->cap = 0;
    for (i = 0; i < n; i++)
        old[i]->active = 0;
    table_wrunlock_all(c);
    for (i = 0; i < n; i++)
        reg_release(old[i]);
    free(old);
    /* Stop the invalidator before freeing its context, then pay any queued
     * invalidations inline: an enqueued range is a promise. */
    {
        int had_thread;
        pthread_mutex_lock(&c->coh_mu);
        c->coh_stop = 1;
        had_thread = c->coh_thread_on;
        c->coh_thread_on = 0;
        pthread_cond_broadcast(&c->coh_cv);
        pthread_mutex_unlock(&c->coh_mu);
        if (had_thread)
            (void)pthread_join(c->coh_thread, NULL);
        coherence_drain(c);
    }
    pthread_cond_destroy(&c->coh_cv);
    pthread_mutex_destroy(&c->coh_mu);
    for (i = 0; i < EXITOS_TABLE_SHARDS; i++)
        pthread_rwlock_destroy(&c->table_rw[i].rw);
    free(c);
}

int exitos_register_fd(struct exitos_ctx *c, int fd)
{
    struct reg *fresh = NULL, *old;
    size_t pos;
    int rc, existed;

    if (!c || fd < 0)
        return -EINVAL;
    if (atomic_load_explicit(&c->poisoned, memory_order_acquire))
        return -ECANCELED;

    /* A registration arms a separate writable block-device handle.  Therefore
     * the application fd's mode is an authority and coherency contract, not a
     * performance hint: accepting O_RDONLY would turn an otherwise-EBADF
     * pwrite into a successful raw write, and accepting append/sync modes
     * would bypass semantics owned by the filesystem/open description.  A
     * buffered fd (no O_DIRECT) is accepted -- the paper never conditions
     * interception on O_DIRECT and its MySQL default-configuration results
     * imply buffered redo writes -- but it opts the registration into the
     * page-cache coherence machinery: registration-time settlement,
     * the kernel-debt takeover gate, and post-write invalidation. */
    int buffered;
    {
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0)
            return -errno;
        if ((fl & O_ACCMODE) == O_RDONLY)
            return -EBADF;
        if ((fl & O_APPEND) || (fl & (O_SYNC | O_DSYNC)))
            return -EOPNOTSUPP;
        buffered = (fl & O_DIRECT) == 0;
    }

    /* New-fd registration does expensive discovery outside the writer lock.
     * Same-fd replacement deliberately does it under the writer lock: otherwise
     * a concurrent truncate could make the freshly-built snapshot stale before
     * it replaces the old one. This is a rare control-plane freeze, not a lock
     * in the steady-state write path. */
retry:
    table_rdlock_fd(c, fd);
    existed = reg_lookup(c, fd, &pos);
    table_rdunlock_fd(c, fd);

    if (!existed) {
        rc = reg_build(c, fd, &fresh);
        if (rc != 0)
            return rc;
        fresh->buffered = buffered;
        if (buffered)
            reg_buffered_init_sync(fd, fresh);
        table_wrlock_all(c);
        if (reg_lookup(c, fd, &pos)) {
            table_wrunlock_all(c);
            reg_release(fresh);
            fresh = NULL;
            goto retry;
        }
        rc = ctx_reserve(c, c->n + 1);
        if (rc == 0) {
            memmove(&c->r[pos + 1], &c->r[pos],
                    (c->n - pos) * sizeof(*c->r));
            c->r[pos] = fresh;
            c->n++;
            fresh = NULL;
        }
        table_wrunlock_all(c);
        if (fresh)
            reg_release(fresh);
        if (rc == 0 && buffered)
            coherence_thread_ensure(c);
        return rc;
    }

    table_wrlock_all(c);
    if (!reg_lookup(c, fd, &pos)) {
        table_wrunlock_all(c);
        goto retry;
    }
    old = c->r[pos];
    rc = reg_build(c, fd, &fresh);
    if (rc != 0) {
        table_wrunlock_all(c);
        return rc;
    }

    /* Debt belongs to completed operations, not to a mapping snapshot. Settle
     * the old raw domain using the OLD policy before replacement. A failed
     * FLUSH leaves the old record (and both debts) intact and rejects refresh. */
    rc = reg_flush_raw_debt(old);
    if (rc != 0) {
        table_wrunlock_all(c);
        reg_release(fresh);
        return rc;
    }
    fresh->buffered = buffered;
    if (buffered)
        reg_buffered_init_sync(fd, fresh);
    fresh->kernel_holds = old->kernel_holds || fresh->kernel_holds;
    old->active = 0;
    c->r[pos] = fresh;
    table_wrunlock_all(c);
    reg_release(old);
    if (buffered)
        coherence_thread_ensure(c);
    return 0;
}

int exitos_unregister_fd(struct exitos_ctx *c, int fd)
{
    size_t pos;

    if (!c || fd < 0)
        return -EINVAL;
    /* Flush pending invalidations while every queued fd is still valid: after
     * removal the number may be closed and reused by an unrelated file. */
    coherence_drain(c);
    table_wrlock_all(c);
    if (!reg_lookup(c, fd, &pos)) {
        table_wrunlock_all(c);
        return -ENOENT;                 /* never registered: nothing to undo */
    }
    {
        struct reg *old = c->r[pos];
        int rc = reg_flush_raw_debt(old);

        /* Removing the only owner of an iopath must not silently discard a
         * completed write's volatile-cache debt.  On failure leave the exact
         * record installed so a later sync/unregister can retry. */
        if (rc != 0) {
            table_wrunlock_all(c);
            return rc;
        }
        old->active = 0;
        memmove(&c->r[pos], &c->r[pos + 1],
                (c->n - pos - 1) * sizeof(*c->r));
        c->n--;
        table_wrunlock_all(c);
        reg_release(old);
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * the decision functions
 * ------------------------------------------------------------------ */

static struct reg *txn_reg(const struct exitos_fd_txn *tx)
{
    struct reg *r;

    if (!tx || !tx->held || !tx->registered || !tx->registration)
        return NULL;
    r = tx->registration;
    if (!r->active || r->fd != tx->fd)
        return NULL;
    return r;
}

int exitos_fd_txn_begin(struct exitos_ctx *c, int fd, struct exitos_fd_txn *tx)
{
    struct reg *r = NULL;
    size_t pos;

    if (!tx)
        return -EINVAL;
    memset(tx, 0, sizeof(*tx));
    if (!c || fd < 0)
        return -EINVAL;

    /* Keep this fd's table shard locked through the whole frontend transaction. It
     * both pins a found record and freezes an absent fd slot, so registration
     * cannot appear between an "unregistered" decision and the real syscall. */
    table_rdlock_fd(c, fd);
    if (reg_lookup(c, fd, &pos)) {
        r = c->r[pos];
        pthread_mutex_lock(&r->mu);
    }
    tx->ctx = c;
    tx->registration = r;
    tx->fd = fd;
    tx->held = 1;
    tx->registered = (r && r->active) ? 1u : 0u;
    return 0;
}

int exitos_fd_txn_registered(const struct exitos_fd_txn *tx)
{
    return txn_reg(tx) ? 1 : 0;
}

int exitos_fd_txn_prepare_forget(struct exitos_fd_txn *tx)
{
    struct reg *r = txn_reg(tx);
    int rc;

    if (!r)
        return 0;
    tx->restore_raw_debt = r->unflushed ? 1u : 0u;
    rc = reg_flush_raw_debt(r);
    if (rc != 0)
        tx->restore_raw_debt = 0;
    return rc;
}

void exitos_fd_txn_abort_forget(struct exitos_fd_txn *tx)
{
    struct reg *r = txn_reg(tx);

    if (r && tx->restore_raw_debt)
        r->unflushed = 1;
    if (tx)
        tx->restore_raw_debt = 0;
}

int exitos_fd_txn_forget(struct exitos_fd_txn *tx)
{
    struct reg *r = txn_reg(tx);
    int rc;

    if (!r)
        return 0;
    rc = reg_flush_raw_debt(r);
    if (rc != 0)
        return rc;
    r->active = 0;
    tx->registered = 0;
    tx->restore_raw_debt = 0;
    return 0;
}

static void remove_inactive_exact(struct exitos_ctx *c, int fd,
                                  struct reg *expected)
{
    struct reg *old = NULL;
    size_t pos;

    table_wrlock_all(c);
    if (reg_lookup(c, fd, &pos) && c->r[pos] == expected &&
        !expected->active) {
        old = expected;
        memmove(&c->r[pos], &c->r[pos + 1],
                (c->n - pos - 1) * sizeof(*c->r));
        c->n--;
    }
    table_wrunlock_all(c);
    if (old)
        reg_release(old);
}

void exitos_fd_txn_end(struct exitos_fd_txn *tx)
{
    struct exitos_ctx *c;
    struct reg *r;
    int fd, cleanup;

    if (!tx || !tx->held)
        return;
    c = tx->ctx;
    r = tx->registration;
    fd = tx->fd;
    cleanup = r && !r->active;
    if (r)
        pthread_mutex_unlock(&r->mu);
    table_rdunlock_fd(c, fd);
    memset(tx, 0, sizeof(*tx));
    if (cleanup)
        remove_inactive_exact(c, fd, r);
}

int exitos_alias_txn_begin(struct exitos_ctx *c, int source_fd, int target_fd,
                           struct exitos_alias_txn *tx)
{
    struct reg *source = NULL, *target = NULL;
    size_t pos;

    if (!tx)
        return -EINVAL;
    memset(tx, 0, sizeof(*tx));
    if (!c || target_fd < -1)
        return -EINVAL;

    table_rdlock_pair(c, source_fd, target_fd);
    if (source_fd >= 0 && reg_lookup(c, source_fd, &pos))
        source = c->r[pos];
    if (target_fd >= 0 && target_fd != source_fd &&
        reg_lookup(c, target_fd, &pos))
        target = c->r[pos];

    /* Every alias operation follows this numeric-fd lock order. */
    if (source && target) {
        if (source_fd < target_fd) {
            tx->lock_first = source;
            tx->lock_second = target;
        } else {
            tx->lock_first = target;
            tx->lock_second = source;
        }
    } else {
        tx->lock_first = source ? (void *)source : (void *)target;
    }
    if (tx->lock_first)
        pthread_mutex_lock(&((struct reg *)tx->lock_first)->mu);
    if (tx->lock_second)
        pthread_mutex_lock(&((struct reg *)tx->lock_second)->mu);

    tx->ctx = c;
    tx->source = source;
    tx->target = target;
    tx->source_fd = source_fd;
    tx->target_fd = target_fd;
    tx->held = 1;
    tx->source_registered = (source && source->active) ? 1u : 0u;
    tx->target_registered = (target && target->active) ? 1u : 0u;
    return 0;
}

int exitos_alias_txn_prepare(struct exitos_alias_txn *tx)
{
    struct reg *source, *target;
    int rc;

    if (!tx || !tx->held)
        return -EINVAL;
    source = tx->source_registered ? tx->source : NULL;
    target = tx->target_registered ? tx->target : NULL;
    tx->restore_source_raw = (source && source->unflushed) ? 1u : 0u;
    tx->restore_target_raw = (target && target->unflushed) ? 1u : 0u;

    if (source) {
        rc = reg_flush_raw_debt(source);
        if (rc != 0)
            goto fail;
    }
    if (target && target != source) {
        rc = reg_flush_raw_debt(target);
        if (rc != 0)
            goto fail;
    }
    return 0;

fail:
    /* A paid debt is harmless to pay twice. Restoring the flags makes a failed
     * dup operation observationally preserve both registrations/debt states. */
    if (source && tx->restore_source_raw)
        source->unflushed = 1;
    if (target && tx->restore_target_raw)
        target->unflushed = 1;
    tx->restore_source_raw = tx->restore_target_raw = 0;
    return rc;
}

void exitos_alias_txn_abort(struct exitos_alias_txn *tx)
{
    struct reg *source, *target;

    if (!tx || !tx->held)
        return;
    source = tx->source_registered ? tx->source : NULL;
    target = tx->target_registered ? tx->target : NULL;
    if (source && tx->restore_source_raw)
        source->unflushed = 1;
    if (target && tx->restore_target_raw)
        target->unflushed = 1;
    tx->restore_source_raw = tx->restore_target_raw = 0;
}

void exitos_alias_txn_commit(struct exitos_alias_txn *tx)
{
    struct reg *source, *target;

    if (!tx || !tx->held)
        return;
    source = tx->source_registered ? tx->source : NULL;
    target = tx->target_registered ? tx->target : NULL;
    if (source)
        source->active = 0;
    if (target && target != source)
        target->active = 0;
    tx->source_registered = tx->target_registered = 0;
    tx->restore_source_raw = tx->restore_target_raw = 0;
}

void exitos_alias_txn_end(struct exitos_alias_txn *tx)
{
    struct exitos_ctx *c;
    struct reg *source, *target;
    int source_fd, target_fd;
    int clean_source, clean_target;

    if (!tx || !tx->held)
        return;
    c = tx->ctx;
    source = tx->source;
    target = tx->target;
    source_fd = tx->source_fd;
    target_fd = tx->target_fd;
    clean_source = source && !source->active;
    clean_target = target && target != source && !target->active;
    if (tx->lock_second)
        pthread_mutex_unlock(&((struct reg *)tx->lock_second)->mu);
    if (tx->lock_first)
        pthread_mutex_unlock(&((struct reg *)tx->lock_first)->mu);
    table_rdunlock_pair(c, source_fd, target_fd);
    memset(tx, 0, sizeof(*tx));
    if (clean_source)
        remove_inactive_exact(c, source_fd, source);
    if (clean_target)
        remove_inactive_exact(c, target_fd, target);
}

static void mapping_candidate_discard(struct reg *candidate)
{
    if (!candidate)
        return;
    maco_destroy(candidate->m);
    free(candidate->runs);
    free(candidate->iv);
    candidate->m = NULL;
    candidate->runs = NULL;
    candidate->nruns = 0;
    candidate->iv = NULL;
    candidate->niv = 0;
}

/* Build all correlated mapping views off to the side.  The caller either
 * proves any additional invariant it needs and publishes the whole candidate,
 * or discards it without changing the live registration. */
static int mapping_candidate_build(struct reg *r, struct reg *candidate)
{
    int rc;

    if (!r || !r->m || !candidate)
        return -EINVAL;
    memset(candidate, 0, sizeof(*candidate));
    candidate->g = r->g;

    /* Counted here, at the attempt, not at the end. What costs time is asking
     * the kernel for the layout at all; a look that fails on the way costs the
     * same as one that succeeds, and counting only the successes made the
     * backoff look like it was never exercised. */
    exitos_stat_inc(EXITOS_STAT_REFRESH);

    /* The fd may have been rebound since registration -- dup2 over it, or the
     * caller reopening something else onto the same number. Registration itself
     * refuses a changed inode with -ESTALE; a refresh that skipped the same
     * check would rebind this registration, geometry included, to whatever the
     * fd now names, and on a different device the LBAs would be computed with
     * the wrong block size and written to the wrong disk. */
    if (r->id_dev || r->id_ino) {
        struct stat now;
        if (fstat(r->fd, &now) != 0)
            return errno ? -errno : -EIO;
        if (now.st_dev != r->id_dev || now.st_ino != r->id_ino)
            return -ESTALE;
    }

    /* Both halves must be reloaded together. The Maco answers "where does this
     * offset live"; runs/iv answer "is that LBA inside this file's own extents",
     * and iv_covers() refuses when the bounds table is empty. Refreshing only
     * the Maco therefore produced a mapping that the guard then rejected on
     * every write -- correct of the guard, and useless as a fast path. */
    rc = runs_load(r->fd, &r->g, &candidate->runs,
                   &candidate->nruns, 0);
    if (rc != 0)
        return rc;

    /* Build all three correlated views off to the side.  Clearing the live
     * Maco before extent discovery completed used to leave a failed refresh
     * with an empty map but the old runs/iv.  Besides being internally
     * inconsistent, that erased a still-safe snapshot merely because a new
     * FIEMAP attempt failed. */
    candidate->m = maco_create(r->g.lbs);
    if (!candidate->m) {
        mapping_candidate_discard(candidate);
        return -ENOMEM;
    }
    rc = maco_load_runs(candidate->m, candidate->runs,
                        candidate->nruns);
    if (rc != 0) {
        mapping_candidate_discard(candidate);
        return rc;
    }
    rc = iv_rebuild(candidate);
    if (rc != 0) {
        mapping_candidate_discard(candidate);
        return rc;
    }

    return 0;
}

static void mapping_candidate_publish(struct reg *r, struct reg *candidate)
{
    maco_destroy(r->m);
    free(r->runs);
    free(r->iv);
    r->m = candidate->m;
    r->runs = candidate->runs;
    r->nruns = candidate->nruns;
    r->iv = candidate->iv;
    r->niv = candidate->niv;
    candidate->m = NULL;
    candidate->runs = NULL;
    candidate->nruns = 0;
    candidate->iv = NULL;
    candidate->niv = 0;
}

/* Re-read the file's layout into Maco. Used when a lookup misses: the snapshot
 * was taken at open(), and the file may have gained extents since. */
static int refresh_mapping(struct reg *r)
{
    struct reg candidate;
    int rc;

    rc = mapping_candidate_build(r, &candidate);
    if (rc != 0)
        return rc;

    mapping_candidate_publish(r, &candidate);
    return 0;
}

int exitos_txn_refresh_mapping_range(struct exitos_fd_txn *tx,
                                     uint64_t off, uint64_t len)
{
    struct reg *r = txn_reg(tx);
    struct reg candidate;
    struct stat st;
    uint64_t end, cursor, lbs;
    int rc;

    if (!r)
        return -ENOENT;
    if (len == 0)
        return -EINVAL;
    if (len > UINT64_MAX - off)
        return -EOVERFLOW;
    end = off + len;

    /* refresh_mapping works on the already-pinned registration.  Calling
     * exitos_register_fd here would attempt a table writer lock while this
     * transaction holds a table reader shard, i.e. a lock upgrade/deadlock. */
    rc = mapping_candidate_build(r, &candidate);
    if (rc != 0)
        return rc;

    if (fstat(tx->fd, &st) != 0) {
        rc = errno ? -errno : -EIO;
        goto fail;
    }
    if (st.st_size < 0) {
        rc = -EIO;
        goto fail;
    }
    if (end > (uint64_t)st.st_size) {
        rc = -ENXIO;
        goto fail;
    }

    lbs = candidate.g.lbs;
    if (lbs == 0) {
        rc = -EINVAL;
        goto fail;
    }

    cursor = off;
    while (cursor < end) {
        uint64_t lba = 0, contig = 0, chunk;
        uint64_t in_block, first, tail, blocks;

        if (maco_lookup(candidate.m, cursor, &lba, &contig) != 0 ||
            contig == 0) {
            rc = -ENXIO;
            goto fail;
        }
        chunk = contig < end - cursor ? contig : end - cursor;

        /* maco_lookup returns the containing block's LBA.  When the requested
         * byte starts in the middle of that block, include the leading byte
         * displacement when calculating how many LBA intervals the chunk
         * touches; ceil(chunk/lbs) alone would under-check a cross-boundary
         * partial range. */
        in_block = cursor % lbs;
        first = lbs - in_block;
        if (chunk <= first) {
            blocks = 1;
        } else {
            tail = chunk - first;
            blocks = 1 + tail / lbs + (tail % lbs != 0);
        }
        if (!iv_covers(&candidate, lba, blocks)) {
            rc = -ENXIO;
            goto fail;
        }
        cursor += chunk;
    }

    mapping_candidate_publish(r, &candidate);
    r->isize = (uint64_t)st.st_size;

    /* This snapshot was rebuilt explicitly after setup and the requested
     * range was proven complete.  A physical discontinuity can still make a
     * single large raw command impossible; that is not evidence the snapshot
     * is stale, so do not repeat FIEMAP on the very first such write.  One
     * normal passed-write batch is the earliest useful retry signal. */
    r->refresh_gap = EXITOS_REFRESH_EVERY;
    r->refresh_fails = 0;
    r->refreshes = r->passed > UINT64_MAX - EXITOS_REFRESH_EVERY
                 ? UINT64_MAX : r->passed + EXITOS_REFRESH_EVERY;
    return 0;

fail:
    mapping_candidate_discard(&candidate);
    return rc;
}

exitos_decision exitos_txn_on_write(struct exitos_fd_txn *tx,
                                    const void *buf, size_t count, off_t off,
                                    ssize_t *result)
{
    struct reg *r = txn_reg(tx);
    uint64_t lba = 0, contig = 0, lbs;

    if (!r || !r->m || !r->io)          return EXITOS_PASS;   /* unregistered */
    if (atomic_load_explicit(&tx->ctx->poisoned, memory_order_acquire)) {
        r->passed++;
        r->kernel_holds = 1;
        exitos_stat_inc(EXITOS_STAT_PASS);
        return EXITOS_PASS;
    }
    /* Read before any use. It used to be assigned further down, after the
     * checks below had already read it: the partial-block test falls back to
     * `lbs` when the filesystem block size is unknown, so on that path it was
     * computing a range from an uninitialised value and deciding whether to
     * take the write over on the result. */
    lbs = r->g.lbs;
    /* Optional: prove the fd still names the file it was registered for. An
     * fd number can be rebound by dup2 or by fcntl(F_DUPFD) at any moment, and
     * the write path has no way to notice without asking the kernel -- which
     * is the syscall this design exists to remove, so it is off unless the
     * caller asks for it. With it on, a rebound fd drops its registration and
     * the write goes the ordinary way instead of onto the previous file's
     * blocks. */
    if (tx->ctx->verify_identity && (r->id_dev || r->id_ino)) {
        struct stat now;
        if (fstat(tx->fd, &now) != 0 ||
            now.st_dev != r->id_dev || now.st_ino != r->id_ino) {
            /* Logical removal happens while this transaction still owns the
             * per-fd lock; end() physically unlinks the exact old pointer only
             * after the shared table lock is released. */
            r->active = 0;
            tx->registered = 0;
            exitos_stat_inc(EXITOS_STAT_PASS);
            return EXITOS_PASS;
        }
    }
    /* The shortcut only services bytes the file already has. A write that ends
     * past the current size is an append: the filesystem has to allocate or
     * convert something for it, and the block at the end may be only partly the
     * file's -- after a truncation to a non-block-aligned size, FIEMAP still
     * reports that block as allocated, and taking a full-block write over sent
     * it raw to a block the file no longer wholly owned. Preparing space ahead
     * of the writer, which is what the donor mechanism is for, grows the file
     * size as well as the extents, so a prepared append lands inside this test
     * rather than outside it. */
    /* A buffered registration may only bypass the kernel while the kernel
     * holds no unsynced write on this fd: a still-dirty page written back
     * later would clobber raw data on the device, and the invalidation after
     * the raw write must never meet a dirty page (POSIX_FADV_DONTNEED starts
     * writeback on dirty ranges).  O_DIRECT registrations keep the old
     * behavior -- their writes never dirty the cache. */
    if (r->buffered && r->kernel_holds) {
        r->passed++; r->kernel_holds = 1;
        exitos_stat_inc(EXITOS_STAT_PASS);
        return EXITOS_PASS;
    }
    if (r->partial_off) {
        uint64_t pstart = r->partial_off - 1;
        uint64_t pend = pstart + (r->g.fs_bs ? r->g.fs_bs : lbs);
        if ((uint64_t)off < pend && (uint64_t)off + (uint64_t)count > pstart) {
            r->passed++; r->kernel_holds = 1;
            exitos_stat_inc(EXITOS_STAT_PASS);
            return EXITOS_PASS;
        }
    }
    if (r->isize && (uint64_t)off + (uint64_t)count > r->isize) {
        struct stat now;
        if (fstat(tx->fd, &now) == 0)
            r->isize = (uint64_t)now.st_size;
        if ((uint64_t)off + (uint64_t)count > r->isize) {
            r->passed++; r->kernel_holds = 1;
            exitos_stat_inc(EXITOS_STAT_PASS);
            return EXITOS_PASS;
        }
    }
    if (r->append) {                    /* the kernel owns the offset */
        r->passed++; r->kernel_holds = 1;
        exitos_stat_inc(EXITOS_STAT_PASS);
        return EXITOS_PASS;
    }
    if (!buf || !result || count == 0 || off < 0) return EXITOS_PASS;
    if (count > (size_t)SSIZE_MAX)      return EXITOS_PASS;

    if (lbs == 0)                       { r->passed++; r->kernel_holds = 1; exitos_stat_inc(EXITOS_STAT_PASS); return EXITOS_PASS; }

    /* Whole device blocks only, from a buffer O_DIRECT can send as it stands. */
    if ((uint64_t)off % lbs != 0 || (uint64_t)count % lbs != 0 ||
        (uintptr_t)buf % (uintptr_t)lbs != 0) {
        r->passed++; r->kernel_holds = 1; exitos_stat_inc(EXITOS_STAT_PASS); return EXITOS_PASS;
    }

    if (maco_lookup(r->m, (uint64_t)off, &lba, &contig) != 0 ||
        contig < (uint64_t)count) {
        /* A miss is not proof the offset is unmapped. The layout is captured at
         * open(), and an application may preallocate AFTER opening: fallocate()
         * then gives the file extents our snapshot predates. The paper's
         * "extract the mappings at initialization, i.e. when opening the log
         * file" only holds when the file was already preallocated before it was
         * opened; a file created and then preallocated -- which is what a log
         * writer normally does -- has no extents at open() at all. Re-read once
         * and retry; a bounded counter stops this becoming a per-write FIEMAP. */
        if (r->passed >= r->refreshes) {
            /* Next attempt allowed only after another batch has gone to the
             * kernel, i.e. after the layout has had a chance to change. */
            /* Back off geometrically, and stop entirely once enough attempts
             * in a row have produced nothing usable. A miss that persists is
             * evidence about the file, not about the timing of the last look. */
            if (r->refresh_gap == 0)
                r->refresh_gap = EXITOS_REFRESH_EVERY;
            r->refreshes = r->passed + r->refresh_gap;
            if (r->refresh_gap < EXITOS_REFRESH_MAX_GAP)
                r->refresh_gap *= 2;
            if (refresh_mapping(r) == 0 &&
                maco_lookup(r->m, (uint64_t)off, &lba, &contig) == 0 &&
                contig >= (uint64_t)count) {
                r->refresh_fails = 0;       /* it paid off; start over */
                r->refresh_gap = 0;
                goto mapped;
            }
            r->refresh_fails++;
        }
        r->passed++; r->kernel_holds = 1; exitos_stat_inc(EXITOS_STAT_PASS);
        return EXITOS_PASS;             /* genuinely a hole, or straddles a run */
    }

mapped:
    /* The guard that keeps a mapping bug from reaching another file. */
    if (!iv_covers(r, lba, (uint64_t)count / lbs)) {
        r->passed++; r->kernel_holds = 1; exitos_stat_inc(EXITOS_STAT_PASS); return EXITOS_PASS;
    }

    /* Strict mode: before bypassing the kernel, ask the kernel whether this
     * write would still be allowed.  The zero-length probe runs the per-write
     * permission gate (FMODE_WRITE plus the LSM file_permission hook) and
     * nothing else; on refusal the write goes the ordinary way and the kernel
     * reports the error itself. */
    if (tx->ctx->strict &&
        exitos_internal_pwrite_call(tx->fd, buf, 0, off) != 0) {
        r->passed++; r->kernel_holds = 1; exitos_stat_inc(EXITOS_STAT_PASS);
        return EXITOS_PASS;
    }

    /* Submission/completion error does not prove that zero bytes reached the
     * device.  pwrite-style backends may make partial progress and native
     * commands can fail after submission.  Conservatively create raw debt
     * before attempting the write; a later sync settles it even if the caller's
     * filesystem fallback also fails. */
    r->unflushed = 1;
    if (iopath_write(r->io, lba, buf, count) != 0) {
        r->passed++; r->kernel_holds = 1; exitos_stat_inc(EXITOS_STAT_PASS);
        return EXITOS_PASS;             /* the caller reissues it normally */
    }

    /* Raw bytes reached the device behind the page cache's back; drop the
     * (clean, by the gate above) cached copies of this range so buffered
     * readers refetch instead of trusting stale pages. */
    if (r->buffered)
        coherence_note(tx->ctx, tx->fd, (uint64_t)off, (uint64_t)count);

    *result = (ssize_t)count;
    exitos_stat_inc(EXITOS_STAT_FAST_WRITE);
    return EXITOS_TAKEOVER;
}

exitos_decision exitos_on_write(struct exitos_ctx *c, int fd, const void *buf,
                                size_t count, off_t off, ssize_t *result)
{
    struct exitos_fd_txn tx;
    exitos_decision d;

    if (exitos_fd_txn_begin(c, fd, &tx) != 0)
        return EXITOS_PASS;
    d = exitos_txn_on_write(&tx, buf, count, off, result);
    exitos_fd_txn_end(&tx);
    return d;
}

static int reg_flush_raw_debt(struct reg *r)
{
    int rc;

    if (!r->unflushed)
        return 0;

    /* Completion already meant persistence for write-through media and for
     * writes that carried FUA. Keep the state exact without issuing a command. */
    if (!exitos_fdatasync_needed(r->dur)) {
        r->unflushed = 0;
        return 0;
    }

    if (!r->io)
        return -EIO;
    rc = iopath_flush(r->io);
    if (rc != 0)
        return rc < 0 ? rc : -EIO;

    r->unflushed = 0;
    return 0;
}

int exitos_txn_flush_raw_debt(struct exitos_fd_txn *tx)
{
    struct reg *r = txn_reg(tx);

    /* fsync/fdatasync frontends call this for every descriptor. An fd outside
     * the registration table has no raw writes from us, so there is no debt. */
    return r ? reg_flush_raw_debt(r) : 0;
}

int exitos_flush_raw_debt(struct exitos_ctx *c, int fd)
{
    struct exitos_fd_txn tx;
    int rc;

    if (exitos_fd_txn_begin(c, fd, &tx) != 0)
        return 0;
    rc = exitos_txn_flush_raw_debt(&tx);
    exitos_fd_txn_end(&tx);
    return rc;
}

exitos_decision exitos_txn_on_fdatasync(struct exitos_fd_txn *tx, int *result)
{
    struct reg *r = txn_reg(tx);
    int rc;

    if (!r || !result) {
        exitos_stat_inc(EXITOS_STAT_PASS);
        return EXITOS_PASS;
    }

    /* Pay our durability domain first. Falling through to a filesystem
     * fdatasync after this fails is unsafe: that syscall is not an unconditional
     * promise to flush unrelated raw writes, and its success must not mask the
     * device error. Keep both debts and return the internal negative errno. */
    rc = reg_flush_raw_debt(r);
    if (rc < 0) {
        *result = rc;
        return EXITOS_TAKEOVER;
    }

    /* Poison is permanent fail-closed state.  Raw debt above still belongs to
     * us and must be paid; after that, the real filesystem sync owns every
     * current and future kernel write. */
    if (atomic_load_explicit(&tx->ctx->poisoned, memory_order_acquire)) {
        exitos_stat_inc(EXITOS_STAT_PASS);
        return EXITOS_PASS;
    }

    /* If any write on this fd went through the kernel, only the kernel's
     * fdatasync persists that half. Taking the call over would drop it. */
    if (r->kernel_holds) {
        exitos_stat_inc(EXITOS_STAT_PASS);
        return EXITOS_PASS;
    }

    /* Every write was ours, and the helper above has either paid the raw debt
     * or proved that FUA/write-through left none. */
    *result = 0;
    exitos_stat_inc(EXITOS_STAT_FAST_SYNC);
    return EXITOS_TAKEOVER;
}

exitos_decision exitos_on_fdatasync(struct exitos_ctx *c, int fd, int *result)
{
    struct exitos_fd_txn tx;
    exitos_decision d;

    if (exitos_fd_txn_begin(c, fd, &tx) != 0) {
        exitos_stat_inc(EXITOS_STAT_PASS);
        return EXITOS_PASS;
    }
    d = exitos_txn_on_fdatasync(&tx, result);
    exitos_fd_txn_end(&tx);
    return d;
}

int exitos_fd_all_writes_took_fast_path(struct exitos_ctx *c, int fd)
{
    struct exitos_fd_txn tx;
    struct reg *r;
    int answer;

    if (exitos_fd_txn_begin(c, fd, &tx) != 0)
        return 0;
    r = txn_reg(&tx);
    answer = (r && !r->kernel_holds) ? 1 : 0;
    exitos_fd_txn_end(&tx);
    return answer;
}

exitos_decision exitos_txn_on_ftruncate(struct exitos_fd_txn *tx, off_t len,
                                        int *result)
{
    struct reg *r = txn_reg(tx);
    uint64_t cut, lbs;

    (void)result;
    if (!r || !r->m)
        return EXITOS_PASS;
    if (len < 0)
        return EXITOS_PASS;

    lbs = r->g.lbs;
    if (lbs == 0)
        return EXITOS_PASS;

    /* Invalidate BEFORE the caller truncates, never after: between the two
     * there must be no instant in which the map still names blocks the
     * filesystem has already handed to someone else.
     *
     * A block that is only partly truncated away loses its mapping too, which
     * is why the cut is rounded DOWN to a whole device block -- the same
     * rounding maco_invalidate performs on the map itself. */
    /* Round down to a whole FILESYSTEM block, not a device block. A truncation
     * that lands inside a filesystem block leaves the file owning only part of
     * it, and the filesystem is free to move or re-zero that block when the
     * file grows again -- so the whole block must lose its mapping, not just
     * the bytes past the new end. Rounding by the device block instead left the
     * head of that block mapped: a later 4 KiB write at its start was taken
     * over and sent raw to a block the file no longer wholly owned, and the
     * block read back with its pre-truncate contents. */
    {
        uint64_t fsb = r->g.fs_bs ? r->g.fs_bs : lbs;
        cut = (uint64_t)len - ((uint64_t)len % fsb);
    }
    r->isize = (uint64_t)len;
    /* A truncation that lands inside a block leaves the filesystem holding a
     * cached copy of that block with its tail zeroed. That page is written back
     * later, on the filesystem's own schedule, and it overwrites whatever we
     * put on the device directly -- the block read back with its pre-truncate
     * contents even though the shortcut had written new ones. The block stays
     * off the fast path from here on; a block-aligned truncation later clears
     * the exclusion because no partial block is left. */
    {
        uint64_t fsb2 = r->g.fs_bs ? r->g.fs_bs : lbs;
        r->partial_off = ((uint64_t)len % fsb2) ? cut + 1 : 0;
    }
    (void)maco_invalidate(r->m, cut, UINT64_MAX - cut);
    runs_trim_from(r, cut);
    if (iv_rebuild(r) != 0) {
        /* Out of memory while shrinking the footprint: keep nothing, so every
         * later write on this fd falls back to the kernel. */
        maco_clear(r->m);
        r->nruns = 0;
    }
    return EXITOS_PASS;                 /* the caller performs the truncation */
}

void exitos_txn_invalidate_mapping(struct exitos_fd_txn *tx)
{
    struct reg *r = txn_reg(tx);

    if (!r)
        return;
    if (r->m)
        maco_clear(r->m);
    free(r->runs);
    r->runs = NULL;
    r->nruns = 0;
    free(r->iv);
    r->iv = NULL;
    r->niv = 0;
    r->partial_off = 0;
    r->refresh_gap = 0;
    r->refresh_fails = 0;
    r->refreshes = r->passed;
}

exitos_decision exitos_on_ftruncate(struct exitos_ctx *c, int fd, off_t len,
                                    int *result)
{
    struct exitos_fd_txn tx;
    exitos_decision d;

    if (exitos_fd_txn_begin(c, fd, &tx) != 0)
        return EXITOS_PASS;
    d = exitos_txn_on_ftruncate(&tx, len, result);
    exitos_fd_txn_end(&tx);
    return d;
}

int exitos_lba_in_bounds(struct exitos_ctx *c, int fd, uint64_t lba, size_t len)
{
    struct exitos_fd_txn tx;
    struct reg *r;
    uint64_t lbs, blocks;
    int answer = 0;

    if (exitos_fd_txn_begin(c, fd, &tx) != 0)
        return 0;
    r = txn_reg(&tx);
    if (!r || len == 0)
        goto out;
    lbs = r->g.lbs;
    if (lbs == 0)
        goto out;
    if ((uint64_t)len > UINT64_MAX - (lbs - 1))
        goto out;
    blocks = ((uint64_t)len + lbs - 1) / lbs;
    answer = iv_covers(r, lba, blocks) ? 1 : 0;
out:
    exitos_fd_txn_end(&tx);
    return answer;
}


/* See the header. EXITOS_DUR_FLUSH is the only policy under which the device
 * still holds data we have not forced to persistent media. */
int exitos_fdatasync_needed(int durability_policy)
{
    return (durability_policy == (int)EXITOS_DUR_FLUSH) ? 1 : 0;
}

void exitos_txn_note_kernel_write(struct exitos_fd_txn *tx)
{
    struct reg *r = txn_reg(tx);
    if (r) {
        r->passed++;
        r->kernel_holds = 1;
        exitos_stat_inc(EXITOS_STAT_PASS);
    }
}

void exitos_txn_note_kernel_metadata(struct exitos_fd_txn *tx)
{
    struct reg *r = txn_reg(tx);

    if (r)
        r->kernel_holds = 1;
}

void exitos_note_kernel_write(struct exitos_ctx *c, int fd)
{
    struct exitos_fd_txn tx;

    if (exitos_fd_txn_begin(c, fd, &tx) != 0)
        return;
    exitos_txn_note_kernel_write(&tx);
    exitos_fd_txn_end(&tx);
}

void exitos_txn_note_kernel_sync(struct exitos_fd_txn *tx)
{
    struct reg *r = txn_reg(tx);

    if (!r)
        return;
    /* The kernel's fdatasync/fsync returned success, so everything it was
     * holding for this file is on stable storage. The debt to the kernel is
     * paid: the flag exists to stop us answering a
     * sync locally while the kernel still holds unpersisted data, and it does
     * not. Leaving it set was the whole defect -- one early fallback disabled
     * the shortcut for the rest of the descriptor's life, and a write-ahead-log
     * workload always has such a fallback while the file is being laid out.
     *
     * Do NOT clear unflushed here. A successful filesystem sync is not an API
     * guarantee that an unrelated raw command's volatile-cache debt was paid;
     * exitos_flush_raw_debt() owns that independent state. */
    r->kernel_holds = 0;
}

void exitos_note_kernel_sync(struct exitos_ctx *c, int fd)
{
    struct exitos_fd_txn tx;

    if (exitos_fd_txn_begin(c, fd, &tx) != 0)
        return;
    exitos_txn_note_kernel_sync(&tx);
    exitos_fd_txn_end(&tx);
}

void exitos_ctx_verify_identity(struct exitos_ctx *c, int on)
{
    if (c) {
        table_wrlock_all(c);
        c->verify_identity = on ? 1 : 0;
        table_wrunlock_all(c);
    }
}

/* Strict mode subsumes the fd identity check: the design doc's contract is
 * that strict mode also observes metadata/identity changes, and identity is
 * the half the permission probe cannot see. */
void exitos_ctx_strict(struct exitos_ctx *c, int on)
{
    if (c) {
        table_wrlock_all(c);
        c->strict = on ? 1 : 0;
        if (on)
            c->verify_identity = 1;
        table_wrunlock_all(c);
    }
}

void exitos_ctx_poison(struct exitos_ctx *c)
{
    if (c)
        atomic_store_explicit(&c->poisoned, 1, memory_order_release);
}

int exitos_ctx_is_poisoned(const struct exitos_ctx *c)
{
    return c ? atomic_load_explicit(&c->poisoned, memory_order_acquire) : 1;
}

int exitos_ctx_flush_all_and_poison(struct exitos_ctx *c)
{
    size_t i;
    int rc = 0;

    if (!c)
        return -EINVAL;
    table_wrlock_all(c);
    for (i = 0; i < c->n; i++) {
        rc = reg_flush_raw_debt(c->r[i]);
        if (rc != 0)
            break;
    }
    if (rc == 0)
        atomic_store_explicit(&c->poisoned, 1, memory_order_release);
    table_wrunlock_all(c);
    return rc;
}
