/* extent: pull a file's offset->physical-block runs out of ext4 via FIEMAP,
 * translate them through geom, and load them into a Maco.
 *
 * Contract decisions (matching tests/integ/test_extent_integ.c):
 *
 *  UNITS.  FIEMAP reports fe_logical, fe_length and fe_physical in BYTES.
 *          struct extent_run carries
 *              file_off = fe_logical              (bytes)
 *              len      = fe_length               (bytes)
 *              fs_block = fe_physical / fs_bs     (ext4 block number)
 *          because struct maco_entry documents file_off/len as byte values and
 *          exitos_geom_fsblock_to_lba documents its argument as an ext4 block
 *          number in fs_bs units.
 *
 *  FLAGS.  FIEMAP_FLAG_SYNC is passed, so the kernel flushes before mapping and
 *          delayed-allocation extents normally do not appear at all. Should any
 *          extent still come back carrying UNKNOWN, DELALLOC, ENCODED,
 *          DATA_ENCRYPTED, NOT_ALIGNED, DATA_INLINE, DATA_TAIL, or SHARED it is
 *          dropped and never surfaced as a run:
 *          fe_physical is meaningless for those (a delalloc extent reports
 *          physical offset 0, which translates to the LBA of the very start of
 *          the filesystem - the superblock). A run whose computed block number
 *          is 0, or whose length is 0, is dropped for the same reason.
 *
 *          FIEMAP_EXTENT_UNWRITTEN extents point at real allocated blocks, so
 *          exitos_extent_read returns them with the flag preserved in
 *          extent_run.flags. exitos_extent_load_maco does NOT load them: ext4
 *          reads such a range back as zeros until a normal write converts the
 *          extent, so a raw LBA write there would land on the platter and be
 *          invisible through the filesystem. Refusing beats losing writes.
 *
 *  RETURNS. exitos_extent_read returns the run count or a negative errno.
 *          exitos_extent_load_maco returns the number of entries loaded, or a
 *          negative errno; specifically -EPERM when geom->writable_raw == 0,
 *          matching exitos_geom_fsblock_to_lba for that condition. On any
 *          refusal it loads nothing and leaves the maco untouched.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <linux/fiemap.h>
#include <linux/fs.h>

#include "exitos_extent.h"

#ifndef FS_IOC_FIEMAP
#define FS_IOC_FIEMAP _IOWR('f', 11, struct fiemap)
#endif
#ifndef FIGETBSZ
#define FIGETBSZ _IO(0x00, 2)
#endif
#ifndef FIEMAP_MAX_OFFSET
#define FIEMAP_MAX_OFFSET (~0ULL)
#endif

/* Extents whose fe_physical does not name a real location on the device. */
#define EXTENT_BAD_LOCATION (FIEMAP_EXTENT_UNKNOWN     | \
                             FIEMAP_EXTENT_DELALLOC    | \
                             FIEMAP_EXTENT_ENCODED     | \
                             FIEMAP_EXTENT_DATA_ENCRYPTED | \
                             FIEMAP_EXTENT_NOT_ALIGNED | \
                             FIEMAP_EXTENT_DATA_INLINE | \
                             FIEMAP_EXTENT_DATA_TAIL   | \
                             FIEMAP_EXTENT_SHARED)

int exitos_extent_flags_have_stable_location(uint32_t flags)
{
    return (flags & EXTENT_BAD_LOCATION) ? 0 : 1;
}

/* Extents fetched from the kernel per ioctl. */
#define FIEMAP_BATCH 256

/* ------------------------------------------------------------------ */
/* filesystem block size of the filesystem holding fd                   */
/* ------------------------------------------------------------------ */

static int query_fs_bs(int fd, uint64_t *out)
{
    struct statfs sfs;
    int bsz = 0;

    if (ioctl(fd, FIGETBSZ, &bsz) == 0 && bsz > 0) {
        *out = (uint64_t)bsz;
        return 0;
    }
    if (fstatfs(fd, &sfs) == 0 && sfs.f_bsize > 0) {
        *out = (uint64_t)sfs.f_bsize;
        return 0;
    }
    return errno ? -errno : -EINVAL;
}

/* ------------------------------------------------------------------ */
/* generic FIEMAP walk                                                  */
/* ------------------------------------------------------------------ */

/* Callback contract: 0 = keep going, 1 = stop walking (success),
 * negative = abort with that errno. */
typedef int (*run_cb)(void *ctx, const struct extent_run *run);

static int fiemap_walk(int fd, uint64_t fs_bs, run_cb cb, void *ctx, int want_sync)
{
    struct fiemap *fm;
    size_t bytes;
    uint64_t logical = 0;
    /* Sync once, on the first ioctl: that is what flushes delayed allocation
     * so those extents never reach us. Repeating it on every batch would make
     * a large, many-extent file wait for a full writeback pass per batch.
     *
     * The caller decides whether to ask for it at all. On a file that is being
     * appended to, re-reading the layout with this flag drags a full writeback
     * into every attempt. */
    uint32_t sync = want_sync ? FIEMAP_FLAG_SYNC : 0;
    int rc = 0;

    if (fs_bs == 0)
        return -EINVAL;

    bytes = sizeof(*fm) + (size_t)FIEMAP_BATCH * sizeof(struct fiemap_extent);
    fm = malloc(bytes);
    if (!fm)
        return -ENOMEM;

    for (;;) {
        const struct fiemap_extent *fe;
        uint64_t next;
        unsigned int i, got;
        int last = 0;

        memset(fm, 0, bytes);
        fm->fm_start        = logical;
        fm->fm_length       = FIEMAP_MAX_OFFSET - logical;
        fm->fm_flags        = sync;
        fm->fm_extent_count = FIEMAP_BATCH;

        if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0) {
            rc = errno ? -errno : -EIO;
            break;
        }
        sync = 0;

        got = fm->fm_mapped_extents;
        if (got == 0)
            break;
        if (got > FIEMAP_BATCH) {            /* kernel would never do this */
            rc = -EIO;
            break;
        }

        for (i = 0; i < got; i++) {
            struct extent_run run;

            fe = &fm->fm_extents[i];
            if (fe->fe_flags & FIEMAP_EXTENT_LAST)
                last = 1;
            if (!exitos_extent_flags_have_stable_location(fe->fe_flags))
                continue;                     /* physical offset is not real  */
            if (fe->fe_length == 0)
                continue;                     /* nothing to map               */

            run.file_off = fe->fe_logical;
            run.len      = fe->fe_length;
            run.fs_block = fe->fe_physical / fs_bs;
            run.flags    = fe->fe_flags;

            if (run.fs_block == 0)
                continue;                     /* block 0 is the superblock    */

            rc = cb(ctx, &run);
            if (rc != 0)
                goto done;                    /* stop (1) or error (negative) */
        }

        fe   = &fm->fm_extents[got - 1];
        next = fe->fe_logical + fe->fe_length;
        if (last || next <= logical)          /* no forward progress possible */
            break;
        logical = next;
    }

done:
    free(fm);
    return rc < 0 ? rc : 0;
}

/* ------------------------------------------------------------------ */
/* exitos_extent_read                                                   */
/* ------------------------------------------------------------------ */

struct read_ctx {
    struct extent_run *runs;
    int                max;
    int                n;
};

static int read_sink(void *vctx, const struct extent_run *run)
{
    struct read_ctx *c = vctx;

    c->runs[c->n++] = *run;
    return c->n >= c->max ? 1 : 0;
}

int exitos_extent_read(int fd, struct extent_run *runs, int max_runs)
{
    return exitos_extent_read_sync(fd, runs, max_runs, 1);
}

int exitos_extent_read_sync(int fd, struct extent_run *runs, int max_runs, int want_sync)
{
    struct read_ctx c;
    uint64_t fs_bs = 0;
    int rc;

    if (!runs || max_runs < 0)
        return -EINVAL;
    if (max_runs == 0)
        return 0;

    rc = query_fs_bs(fd, &fs_bs);
    if (rc < 0)
        return rc;

    c.runs = runs;
    c.max  = max_runs;
    c.n    = 0;

    rc = fiemap_walk(fd, fs_bs, read_sink, &c, want_sync);
    if (rc < 0)
        return rc;
    return c.n;
}

/* ------------------------------------------------------------------ */
/* exitos_extent_load_maco                                              */
/* ------------------------------------------------------------------ */

struct collect_ctx {
    struct extent_run *runs;
    size_t             n;
    size_t             cap;
};

static int collect_sink(void *vctx, const struct extent_run *run)
{
    struct collect_ctx *c = vctx;

    /* Allocated but never written: ext4 reads the range back as zeros, so a
     * raw write through this mapping would be invisible. Do not map it. */
    if (run->flags & FIEMAP_EXTENT_UNWRITTEN)
        return 0;

    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 64;
        struct extent_run *p;

        if (cap > (size_t)-1 / sizeof(*p))
            return -ENOMEM;
        p = realloc(c->runs, cap * sizeof(*p));
        if (!p)
            return -ENOMEM;
        c->runs = p;
        c->cap  = cap;
    }
    c->runs[c->n++] = *run;
    return 0;
}

int exitos_extent_load_maco(int fd, const struct exitos_geom *g, struct maco *m)
{
    return exitos_extent_load_maco_sync(fd, g, m, 1);
}

int exitos_extent_load_maco_sync(int fd, const struct exitos_geom *g, struct maco *m,
                                 int want_sync)
{
    struct collect_ctx c = { NULL, 0, 0 };
    uint64_t *lbas = NULL;
    uint64_t fs_bs = 0;
    size_t i;
    int rc;

    if (!g || !m)
        return -EINVAL;
    /* The gate is the field, not the device class: a struct that claims a
     * benign class but has writable_raw == 0 is still refused, before any
     * extent is read, so that nothing at all is loaded. */
    if (!g->writable_raw)
        return -EPERM;
    if (g->fs_bs == 0 || g->lbs == 0 || g->fs_bs % g->lbs)
        return -EINVAL;

    rc = query_fs_bs(fd, &fs_bs);
    if (rc < 0)
        return rc;
    /* The geometry must describe the filesystem this file actually lives on,
     * or every block-number-to-LBA translation below is wrong. */
    if (fs_bs != (uint64_t)g->fs_bs)
        return -EINVAL;

    rc = fiemap_walk(fd, fs_bs, collect_sink, &c, want_sync);
    if (rc < 0)
        goto out;

    if (c.n == 0) {
        rc = 0;
        goto out;
    }

    /* Translate everything first: a single failure must leave the maco
     * completely untouched rather than half loaded. */
    lbas = calloc(c.n, sizeof(*lbas));
    if (!lbas) {
        rc = -ENOMEM;
        goto out;
    }
    for (i = 0; i < c.n; i++) {
        rc = exitos_geom_fsblock_to_lba(g, c.runs[i].fs_block, &lbas[i]);
        if (rc != 0) {
            if (rc > 0)
                rc = -EINVAL;
            goto out;
        }
    }

    for (i = 0; i < c.n; i++) {
        rc = maco_insert(m, c.runs[i].file_off, lbas[i], c.runs[i].len);
        if (rc != 0) {
            if (rc > 0)
                rc = -EINVAL;
            goto out;
        }
    }
    rc = (int)c.n;

out:
    free(lbas);
    free(c.runs);
    return rc;
}
