/* Maco (Metadata Collector): in-memory file-offset -> device-LBA map.
 *
 * Pure data structure, no syscalls. It is filled from the ext4 extent tree at
 * open time so that the write path never has to walk that tree again.
 *
 * REPRESENTATION
 * --------------
 * A single array of struct maco_entry kept sorted by file_off and, as an
 * invariant, non-overlapping. Lookup is a binary search, so it is O(log n);
 * insert and invalidate are O(log n) plus one memmove of the tail of the
 * array. A linked list would make lookup O(n), which is exactly the cost this
 * module exists to remove from the write path.
 *
 * ARITHMETIC
 * ----------
 * file_off and len are BYTES; lba counts DEVICE LOGICAL BLOCKS of lbs bytes.
 * For a run {file_off, lba, len} and any byte k in [0, len):
 *     lookup(file_off + k) -> lba + k / lbs   (floor division)
 *                             contig = len - k
 * A byte that lands mid-block still resolves to that block's LBA, and contig
 * is a byte count, not a block count.
 *
 * INVARIANTS
 * ----------
 *   I1. entries are sorted strictly ascending by file_off and never overlap;
 *   I2. every entry has len > 0;
 *   I3. every stored file_off is a multiple of lbs, so the formula above is
 *       exact. maco_insert enforces this on the way in and maco_invalidate
 *       rounds outward to whole blocks so that trimming preserves it;
 *   I4. two entries that are adjacent in the file AND contiguous on the device
 *       are merged into one, so contig answers "how many bytes can I send in a
 *       single command from here" rather than "how far to the next extent
 *       boundary FIEMAP happened to report". Merging never crosses an LBA
 *       discontinuity.
 *
 * Every entry point tolerates a NULL map: it refuses rather than dereferencing.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "exitos_maco.h"

struct maco {
    struct maco_entry *e;   /* sorted, non-overlapping                      */
    size_t             n;   /* entries in use                               */
    size_t             cap; /* entries allocated                            */
    uint64_t           bytes; /* running sum of e[i].len                    */
    uint32_t           lbs; /* device logical block size, bytes; never 0    */
};

#define MACO_CAP0 16u

/* ------------------------------------------------------------- internals */

static uint64_t entry_end(const struct maco_entry *e)
{
    return e->file_off + e->len;   /* insert refuses runs that would wrap */
}

/* Grow to hold at least `want` entries. Returns 0 or -ENOMEM. */
static int ensure_cap(struct maco *m, size_t want)
{
    size_t cap;
    struct maco_entry *ne;

    if (want <= m->cap)
        return 0;
    cap = m->cap ? m->cap : MACO_CAP0;
    while (cap < want) {
        if (cap > SIZE_MAX / 2 / sizeof(*m->e))
            return -ENOMEM;
        cap *= 2;
    }
    ne = realloc(m->e, cap * sizeof(*m->e));
    if (!ne)
        return -ENOMEM;
    m->e = ne;
    m->cap = cap;
    return 0;
}

/* First index whose file_off >= off (m->n if there is none). */
static size_t lower_bound(const struct maco *m, uint64_t off)
{
    size_t lo = 0, hi = m->n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->e[mid].file_off < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Index of the entry containing off, or m->n if off falls in a hole. */
static size_t find_entry(const struct maco *m, uint64_t off)
{
    size_t i = lower_bound(m, off);

    if (i < m->n && m->e[i].file_off == off)
        return i;
    if (i > 0 && entry_end(&m->e[i - 1]) > off)
        return i - 1;
    return m->n;
}

/* True when b starts exactly where a ends, in the file and on the device.
 * a->len must be a whole number of blocks, otherwise a and b would share a
 * device block and "contiguous" would not be well defined. */
static int can_join(const struct maco *m, const struct maco_entry *a,
                    const struct maco_entry *b)
{
    uint64_t blocks;

    if (entry_end(a) != b->file_off || a->len % m->lbs != 0)
        return 0;
    blocks = a->len / m->lbs;
    /* a->lba + blocks would wrap past 2^64. The sum then equals b->lba for a
     * b->lba near 0, and the two runs get merged across the end of the device
     * address space -- a discontinuity, not a contiguity. A single device
     * command built from the merged run would start near 2^64 and be expected
     * to cover blocks at 0. */
    if (blocks > UINT64_MAX - a->lba)
        return 0;
    return a->lba + blocks == b->lba;
}

/* Drop entry i without touching the byte accounting (its bytes moved into a
 * neighbour). */
static void raw_erase(struct maco *m, size_t i)
{
    memmove(&m->e[i], &m->e[i + 1], (m->n - i - 1) * sizeof(*m->e));
    m->n--;
}

/* Merge entry i with whichever neighbours it is file-adjacent and
 * device-contiguous with (invariant I4). */
static void coalesce_at(struct maco *m, size_t i)
{
    if (i + 1 < m->n && can_join(m, &m->e[i], &m->e[i + 1])) {
        m->e[i].len += m->e[i + 1].len;
        raw_erase(m, i + 1);
    }
    if (i > 0 && can_join(m, &m->e[i - 1], &m->e[i])) {
        m->e[i - 1].len += m->e[i].len;
        raw_erase(m, i);
    }
}

/* Remove every mapped byte in [start, end) from the map, splitting or trimming
 * the entries at the edges. The caller must have reserved room for one extra
 * entry: punching a hole in the middle of a single run turns it into two.
 *
 * The trimmed tail's LBA is advanced by (end - file_off) / lbs. When `end` is
 * block aligned -- which it always is on the maco_invalidate path, and on the
 * maco_insert path whenever the inserted run is a whole number of blocks --
 * that is exact. */
static void punch(struct maco *m, uint64_t start, uint64_t end)
{
    struct maco_entry head, tail;
    int have_head = 0, have_tail = 0;
    size_t i, j, k, nrep;
    uint64_t removed = 0;

    if (start >= end || m->n == 0)
        return;

    i = lower_bound(m, start);
    if (i > 0 && entry_end(&m->e[i - 1]) > start)
        i--;
    j = i;
    while (j < m->n && m->e[j].file_off < end)
        j++;
    if (i == j)
        return;                     /* the range lies entirely in a hole */

    if (m->e[i].file_off < start) { /* the first entry survives on its left */
        head = m->e[i];
        head.len = start - head.file_off;
        have_head = 1;
    }
    if (entry_end(&m->e[j - 1]) > end) { /* the last one survives on its right */
        const struct maco_entry *last = &m->e[j - 1];
        uint64_t delta = end - last->file_off;

        tail.file_off = end;
        tail.lba = last->lba + delta / m->lbs;
        tail.len = entry_end(last) - end;
        have_tail = 1;
    }

    for (k = i; k < j; k++)
        removed += m->e[k].len;

    nrep = (size_t)have_head + (size_t)have_tail;
    memmove(&m->e[i + nrep], &m->e[j], (m->n - j) * sizeof(*m->e));
    m->n = m->n - (j - i) + nrep;

    k = i;
    if (have_head)
        m->e[k++] = head;
    if (have_tail)
        m->e[k++] = tail;

    m->bytes -= removed;
    if (have_head)
        m->bytes += head.len;
    if (have_tail)
        m->bytes += tail.len;
}

/* ---------------------------------------------------------------- public */

struct maco *maco_create(uint32_t lbs)
{
    struct maco *m;

    if (lbs == 0)          /* k / lbs would divide by zero: no map is possible */
        return NULL;
    m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->lbs = lbs;
    return m;
}

void maco_destroy(struct maco *m)
{
    if (!m)
        return;
    free(m->e);
    free(m);
}

int maco_insert(struct maco *m, uint64_t file_off, uint64_t lba, uint64_t len)
{
    uint64_t end;
    size_t i;
    int rc;

    if (!m)
        return -EINVAL;
    if (len == 0)                       /* an empty run carries no mapping   */
        return -EINVAL;
    if (file_off % m->lbs != 0)         /* invariant I3                      */
        return -EINVAL;
    if (len > UINT64_MAX - file_off)    /* the run would wrap past 2^64      */
        return -EINVAL;
    /* The same rejection on the device axis. Accepting a run whose last block
     * is past 2^64 makes lookup() hand back a wrapped LBA: a lookup one block
     * into a run starting at 2^64-1 returned 0, which is block 0 of the device
     * -- where the partition table lives. */
    if (len > m->lbs) {
        uint64_t blocks = (len + m->lbs - 1) / m->lbs;
        if (blocks - 1 > UINT64_MAX - lba)
            return -EINVAL;
    }
    end = file_off + len;

    /* punch may add one entry, the insert below adds one more. */
    rc = ensure_cap(m, m->n + 2);
    if (rc != 0)
        return rc;

    /* Round the punched end OUT to a whole block, the way maco_invalidate
     * already does. Every entry's file_off must stay block aligned (I3),
     * because lookup() computes the LBA as base_lba + (off - file_off)/lbs. A
     * partial insert used to leave a tail starting at, say, 100, and every
     * lookup inside that tail then divided from a misaligned base and returned
     * an LBA one block too low -- a raw write aimed one block before the block
     * that actually holds the data. A block only partly covered by the new run
     * loses its mapping instead; the write path refuses sub-block work anyway. */
    {
        uint64_t prem = end % m->lbs;
        uint64_t pend = prem ? end + (m->lbs - prem) : end;
        if (pend < end) pend = end;     /* rounding cannot wrap us backwards */
        punch(m, file_off, pend);       /* the newer extent wins its bytes */
    }

    i = lower_bound(m, file_off);
    memmove(&m->e[i + 1], &m->e[i], (m->n - i) * sizeof(*m->e));
    m->e[i].file_off = file_off;
    m->e[i].lba = lba;
    m->e[i].len = len;
    m->n++;
    m->bytes += len;

    coalesce_at(m, i);
    return 0;
}

int maco_lookup(const struct maco *m, uint64_t file_off,
                uint64_t *lba, uint64_t *contig)
{
    const struct maco_entry *e;
    uint64_t k;
    size_t i;

    if (!m)
        return -EINVAL;
    i = find_entry(m, file_off);
    if (i == m->n)
        return -ENOENT;

    e = &m->e[i];
    k = file_off - e->file_off;
    if (lba)
        *lba = e->lba + k / m->lbs;
    if (contig)
        *contig = e->len - k;
    return 0;
}

int maco_invalidate(struct maco *m, uint64_t off, uint64_t len)
{
    uint64_t start, end, rem;
    int rc;

    if (!m)
        return -EINVAL;
    if (len == 0)                       /* invalidating nothing is a no-op   */
        return 0;
    if (len > UINT64_MAX - off)         /* the range would wrap past 2^64    */
        return -EINVAL;

    /* Round OUTWARD to whole device blocks: a block that is only partly
     * invalidated must lose its mapping too, or the write path could aim at a
     * block whose contents are no longer known. It is also what keeps every
     * surviving file_off block aligned (invariant I3). */
    start = off - off % m->lbs;
    end = off + len;
    rem = end % m->lbs;
    if (rem != 0) {
        uint64_t pad = m->lbs - rem;
        end = (end > UINT64_MAX - pad) ? UINT64_MAX : end + pad;
    }

    rc = ensure_cap(m, m->n + 1);       /* a mid-run punch splits one in two */
    if (rc != 0)
        return rc;

    punch(m, start, end);
    return 0;
}

void maco_clear(struct maco *m)
{
    if (!m)
        return;
    m->n = 0;
    m->bytes = 0;
}

size_t maco_count(const struct maco *m)
{
    return m ? m->n : 0;
}

uint64_t maco_mapped_bytes(const struct maco *m)
{
    return m ? m->bytes : 0;
}
