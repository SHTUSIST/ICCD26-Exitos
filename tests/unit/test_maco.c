/* Tier-0 unit tests for Maco: the in-memory file-offset -> device-LBA map.
 *
 * Pure data structure. No syscalls, no device, no root, no filesystem.
 *
 * THE CENTRAL ARITHMETIC UNDER TEST
 * ---------------------------------
 * struct maco_entry stores file_off in BYTES, len in BYTES, and lba in DEVICE
 * LOGICAL BLOCKS (lbs bytes each). So for a run {file_off, lba, len} and any
 * byte k in [0, len):
 *
 *     maco_lookup(m, file_off + k) => *lba   = lba + k / lbs   (floor division)
 *                                    *contig = len - k
 *
 * A byte offset that lands in the middle of a device block still resolves to
 * that block's LBA (floor), and contig is counted in bytes, not blocks. Both
 * halves of that are asserted with concrete numbers below.
 *
 * CONTRACT DECISIONS THIS TEST FIXES (the header is silent; these tests pin
 * them down, and every one of them is required for the arithmetic above to
 * stay sound):
 *
 *   1. Return convention matches the rest of the codebase: 0 on success,
 *      negative errno on failure. maco_lookup returns -ENOENT on a miss (the
 *      header says so explicitly); maco_insert / maco_invalidate return
 *      -EINVAL when the caller hands them something they cannot represent.
 *   2. maco_create(0) returns NULL. lbs == 0 makes k / lbs a division by zero,
 *      so a map with that block size cannot answer a single lookup.
 *   3. maco_insert refuses a file_off that is not a multiple of lbs. The
 *      lba + k/lbs formula is only correct when a run starts on a device block
 *      boundary, so a misaligned run cannot be stored at all.
 *   4. maco_insert refuses len == 0 (an empty run carries no mapping) and
 *      refuses file_off + len that overflows 64 bits.
 *   5. A later insert wins over an earlier one on the bytes they share. The
 *      map is refreshed from the extent tree, and the fresher extent is the
 *      truth; an overwrite in the middle of a run splits it into three.
 *   6. Runs that are adjacent in the file AND contiguous on the device
 *      coalesce, so lookup reports the full contiguous byte count across them.
 *      This is the whole point of the module: the write path asks "how many
 *      bytes can I send in one command from here". FIEMAP routinely reports
 *      physically contiguous space as several extents, so without coalescing
 *      the answer would be needlessly short. Conversely, contig must NEVER run
 *      past an LBA discontinuity even when the file offsets are adjacent.
 *   7. maco_invalidate rounds OUTWARD to whole device blocks. Truncation is
 *      not always block aligned, and keeping a mapping for a block that was
 *      partly invalidated would let the write path aim at a block whose
 *      contents are no longer known. Rounding outward also keeps every stored
 *      file_off block-aligned, which decision 3 requires. Zero-length
 *      invalidate is a no-op success; an overflowing range is -EINVAL.
 *
 * NULL-argument robustness is checked in a forked child so that an
 * implementation which dereferences NULL is reported as one failed assertion
 * instead of taking the whole test binary down.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tap.h"
#include "exitos_maco.h"

typedef unsigned long long ull;

#define SENTINEL 0xA5A5A5A5A5A5A5A5ULL

/* ---------------------------------------------------------------- helpers */

static struct maco *mk(uint32_t lbs, const char *what)
{
    struct maco *m = maco_create(lbs);
    T_OK(m != NULL, "%s: maco_create(lbs=%u) returns a map", what, (unsigned)lbs);
    return m;
}

/* One TAP line carrying every number, so a failure is self-diagnosing. */
static void expect_hit(const struct maco *m, uint64_t off,
                       uint64_t want_lba, uint64_t want_contig, const char *what)
{
    uint64_t lba = SENTINEL, contig = SENTINEL;
    int rc = maco_lookup(m, off, &lba, &contig);
    T_OK(rc == 0 && lba == want_lba && contig == want_contig,
         "%s: lookup(off=%llu) -> rc=%d lba=%llu contig=%llu  (want rc=0 lba=%llu contig=%llu)",
         what, (ull)off, rc, (ull)lba, (ull)contig, (ull)want_lba, (ull)want_contig);
}

static void expect_miss(const struct maco *m, uint64_t off, const char *what)
{
    uint64_t lba = SENTINEL, contig = SENTINEL;
    int rc = maco_lookup(m, off, &lba, &contig);
    T_OK(rc == -ENOENT, "%s: lookup(off=%llu) -> rc=%d (want -ENOENT=%d)",
         what, (ull)off, rc, -ENOENT);
}

static void expect_counts(const struct maco *m, size_t want_count,
                          uint64_t want_bytes, const char *what)
{
    T_OK(maco_count(m) == want_count,
         "%s: count=%llu (want %llu)", what,
         (ull)maco_count(m), (ull)want_count);
    T_OK(maco_mapped_bytes(m) == want_bytes,
         "%s: mapped_bytes=%llu (want %llu)", what,
         (ull)maco_mapped_bytes(m), (ull)want_bytes);
}

static void ins_ok(struct maco *m, uint64_t off, uint64_t lba, uint64_t len,
                   const char *what)
{
    int rc = maco_insert(m, off, lba, len);
    T_OK(rc == 0, "%s: insert(off=%llu lba=%llu len=%llu) -> rc=%d (want 0)",
         what, (ull)off, (ull)lba, (ull)len, rc);
}

static void inv_ok(struct maco *m, uint64_t off, uint64_t len, const char *what)
{
    int rc = maco_invalidate(m, off, len);
    T_OK(rc == 0, "%s: invalidate(off=%llu len=%llu) -> rc=%d (want 0)",
         what, (ull)off, (ull)len, rc);
}

/* ------------------------------------------------- create / destroy / empty */

static void test_create_and_empty(void)
{
    struct maco *bad = maco_create(0);
    T_OK(bad == NULL,
         "maco_create(lbs=0) refuses: lba + k/lbs is undefined for lbs=0 (got %p)",
         (void *)bad);
    if (bad) maco_destroy(bad);

    struct maco *m = mk(512, "empty/512");
    if (!m) return;
    expect_counts(m, 0, 0, "fresh map");
    expect_miss(m, 0, "fresh map");
    expect_miss(m, 4096, "fresh map");
    expect_miss(m, UINT64_MAX, "fresh map, offset at the top of the range");

    inv_ok(m, 0, 1u << 20, "invalidate on an empty map is a no-op success");
    expect_counts(m, 0, 0, "empty map after no-op invalidate");
    maco_clear(m);
    expect_counts(m, 0, 0, "empty map after clear");
    maco_destroy(m);

    struct maco *m4 = mk(4096, "empty/4096");
    if (!m4) return;
    expect_counts(m4, 0, 0, "fresh 4096-lbs map");
    expect_miss(m4, 0, "fresh 4096-lbs map");
    maco_destroy(m4);
}

/* ------------------------------------------- exact hit + middle-of-run math */

static void test_single_run_lbs512(void)
{
    /* [0, 8192) at LBA 1000..1015 with 512-byte blocks: 16 device blocks. */
    struct maco *m = mk(512, "single/512");
    if (!m) return;
    ins_ok(m, 0, 1000, 8192, "single/512");
    expect_counts(m, 1, 8192, "single/512 after one insert");

    expect_hit(m, 0,    1000, 8192, "single/512 exact start");
    expect_hit(m, 512,  1001, 7680, "single/512 one block in");
    expect_hit(m, 1024, 1002, 7168, "single/512 two blocks in");
    expect_hit(m, 4096, 1008, 4096, "single/512 halfway");
    expect_hit(m, 7680, 1015, 512,  "single/512 last block");

    /* Byte offsets inside a block: floor division, contig counted in bytes. */
    expect_hit(m, 1,    1000, 8191, "single/512 byte 1 is still in block 0");
    expect_hit(m, 511,  1000, 7681, "single/512 last byte of block 0");
    expect_hit(m, 513,  1001, 7679, "single/512 second byte of block 1");
    expect_hit(m, 8191, 1015, 1,    "single/512 final byte of the run");

    expect_miss(m, 8192,       "single/512 one byte past the run");
    expect_miss(m, 8192 + 512, "single/512 well past the run");
    maco_destroy(m);
}

static void test_single_run_lbs4096_offset_base(void)
{
    /* A run that does not start at file offset 0: [1 MiB, 2 MiB) at LBA 2000. */
    const uint64_t base = 1u << 20, len = 1u << 20;
    struct maco *m = mk(4096, "single/4096");
    if (!m) return;
    ins_ok(m, base, 2000, len, "single/4096");
    expect_counts(m, 1, len, "single/4096 after one insert");

    expect_hit(m, base,               2000, 1048576, "single/4096 exact start");
    expect_hit(m, base + 4096,        2001, 1044480, "single/4096 one block in");
    expect_hit(m, base + 12295,       2003, 1036281,
               "single/4096 12295 bytes in: 12295/4096=3, contig 1048576-12295");
    expect_hit(m, base + 1048575,     2255, 1,       "single/4096 final byte");
    expect_hit(m, base + 4095,        2000, 1044481, "single/4096 last byte of block 0");

    expect_miss(m, base - 1,   "single/4096 one byte before the run");
    expect_miss(m, 0,          "single/4096 file offset 0 is unmapped");
    expect_miss(m, base + len, "single/4096 one byte past the run");
    maco_destroy(m);
}

static void test_lbs_scales_the_lba_advance(void)
{
    /* Identical file layout, different device block size: the LBA advance for
     * the same byte offset must differ by exactly the ratio of block sizes. */
    struct maco *a = mk(512, "scale/512");
    struct maco *b = mk(4096, "scale/4096");
    if (!a || !b) { if (a) maco_destroy(a); if (b) maco_destroy(b); return; }

    ins_ok(a, 0, 1000, 65536, "scale/512");
    ins_ok(b, 0, 1000, 65536, "scale/4096");

    expect_hit(a, 32768, 1064, 32768, "scale/512: 32768/512 = 64 blocks in");
    expect_hit(b, 32768, 1008, 32768, "scale/4096: 32768/4096 = 8 blocks in");
    expect_hit(a, 65024, 1127, 512,   "scale/512: last 512-byte block");
    expect_hit(b, 61440, 1015, 4096,  "scale/4096: last 4096-byte block");
    expect_hit(a, 65535, 1127, 1,     "scale/512: final byte");
    expect_hit(b, 65535, 1015, 1,     "scale/4096: final byte");
    expect_miss(a, 65536, "scale/512 past the end");
    expect_miss(b, 65536, "scale/4096 past the end");

    maco_destroy(a);
    maco_destroy(b);
}

/* ------------------------------------------------------ holes and boundaries */

static void test_holes(void)
{
    struct maco *m = mk(4096, "holes");
    if (!m) return;
    ins_ok(m, 0,     100, 8192, "holes A");
    ins_ok(m, 16384, 500, 8192, "holes B");
    ins_ok(m, 40960, 900, 8192, "holes C");
    expect_counts(m, 3, 24576, "holes: three disjoint runs");

    expect_hit(m, 0,     100, 8192, "holes A start");
    expect_hit(m, 8191,  101, 1,    "holes A final byte");
    expect_hit(m, 16384, 500, 8192, "holes B start");
    expect_hit(m, 20480, 501, 4096, "holes B second block");
    expect_hit(m, 24575, 501, 1,    "holes B final byte");
    expect_hit(m, 40960, 900, 8192, "holes C start");

    expect_miss(m, 8192,      "hole between A and B, first byte");
    expect_miss(m, 12288,     "hole between A and B, middle");
    expect_miss(m, 16383,     "hole between A and B, last byte");
    expect_miss(m, 24576,     "hole between B and C, first byte");
    expect_miss(m, 40959,     "hole between B and C, last byte");
    expect_miss(m, 49152,     "past the last run");
    expect_miss(m, 1u << 30,  "far past the last run");
    maco_destroy(m);
}

/* ------------------------------------------- adjacency: coalesce vs. do not */

static void test_adjacent_contiguous_coalesces(void)
{
    /* 512-byte blocks: 4096 bytes = 8 blocks, so 100 -> 108 -> 116 is one
     * unbroken device range and lookup must report the full length. */
    struct maco *m = mk(512, "coalesce");
    if (!m) return;
    ins_ok(m, 0,    100, 4096, "coalesce first");
    ins_ok(m, 4096, 108, 4096, "coalesce second");
    T_OK(maco_count(m) == 1,
         "coalesce: file-adjacent and device-contiguous runs become 1 entry (count=%llu)",
         (ull)maco_count(m));
    T_OK(maco_mapped_bytes(m) == 8192,
         "coalesce: mapped_bytes=%llu (want 8192)", (ull)maco_mapped_bytes(m));
    expect_hit(m, 0,    100, 8192, "coalesce: contig spans both runs");
    expect_hit(m, 2048, 104, 6144, "coalesce: middle of the first run");
    expect_hit(m, 4096, 108, 4096, "coalesce: start of the second run");
    expect_hit(m, 8191, 115, 1,    "coalesce: final byte");
    expect_miss(m, 8192, "coalesce: past the joined run");

    ins_ok(m, 8192, 116, 4096, "coalesce third");
    T_OK(maco_count(m) == 1, "coalesce: third run joins too (count=%llu)",
         (ull)maco_count(m));
    expect_hit(m, 0, 100, 12288, "coalesce: contig spans all three runs");

    /* The same thing inserted out of order must coalesce identically. */
    maco_clear(m);
    ins_ok(m, 4096, 108, 4096, "coalesce-reverse second");
    ins_ok(m, 0,    100, 4096, "coalesce-reverse first");
    T_OK(maco_count(m) == 1, "coalesce-reverse: still 1 entry (count=%llu)",
         (ull)maco_count(m));
    expect_hit(m, 0, 100, 8192, "coalesce-reverse: contig spans both runs");
    maco_destroy(m);
}

static void test_adjacent_discontiguous_never_merges(void)
{
    /* File-adjacent but the device jumps: contig MUST stop at the seam,
     * otherwise the write path would run off the end of the real extent. */
    struct maco *m = mk(512, "seam");
    if (!m) return;
    ins_ok(m, 0,    100, 4096, "seam first");
    ins_ok(m, 4096, 900, 4096, "seam second (LBA jumps 108 -> 900)");
    expect_counts(m, 2, 8192, "seam: two entries survive");
    expect_hit(m, 0,    100, 4096, "seam: contig stops at the LBA discontinuity");
    expect_hit(m, 2048, 104, 2048, "seam: middle of the first run");
    expect_hit(m, 4095, 107, 1,    "seam: last byte before the seam");
    expect_hit(m, 4096, 900, 4096, "seam: first byte after the seam");
    expect_hit(m, 8191, 907, 1,    "seam: final byte");
    expect_miss(m, 8192, "seam: past both runs");
    maco_clear(m);

    /* Backwards on the device is also a discontinuity. */
    ins_ok(m, 0,    200, 4096, "backwards first");
    ins_ok(m, 4096, 100, 4096, "backwards second (LBA goes down)");
    expect_counts(m, 2, 8192, "backwards: two entries");
    expect_hit(m, 0,    200, 4096, "backwards: contig stops at the seam");
    expect_hit(m, 4096, 100, 4096, "backwards: second run");
    maco_destroy(m);
}

/* --------------------------------------------------- overlapping inserts */

static void test_overlapping_inserts(void)
{
    struct maco *m = mk(4096, "overlap");
    if (!m) return;

    /* (a) identical insert twice: idempotent, no double counting. */
    ins_ok(m, 0, 100, 8192, "overlap dup first");
    ins_ok(m, 0, 100, 8192, "overlap dup second");
    expect_counts(m, 1, 8192, "overlap: duplicate insert is idempotent");
    expect_hit(m, 0, 100, 8192, "overlap dup: mapping intact");

    /* (b) exact replace: the newer LBA wins. */
    ins_ok(m, 0, 700, 8192, "overlap replace");
    expect_counts(m, 1, 8192, "overlap: exact replace keeps one entry");
    expect_hit(m, 0,    700, 8192, "overlap replace: new LBA wins");
    expect_hit(m, 4096, 701, 4096, "overlap replace: middle of the new run");
    maco_clear(m);

    /* (c) tail overlap: [0,8192)@100 then [4096,8192)@500. */
    ins_ok(m, 0,    100, 8192, "overlap tail base");
    ins_ok(m, 4096, 500, 4096, "overlap tail newer");
    expect_counts(m, 2, 8192, "overlap tail: 8192 bytes still mapped, in 2 runs");
    expect_hit(m, 0,    100, 4096, "overlap tail: old run truncated at the seam");
    expect_hit(m, 4096, 500, 4096, "overlap tail: newer run wins its bytes");
    maco_clear(m);

    /* (d) head overlap: [4096,12288)@200 then [0,8192)@900. */
    ins_ok(m, 4096, 200, 8192, "overlap head base");
    ins_ok(m, 0,    900, 8192, "overlap head newer");
    expect_counts(m, 2, 12288, "overlap head: 12288 bytes mapped, in 2 runs");
    expect_hit(m, 0,    900, 8192, "overlap head: newer run owns [0,8192)");
    expect_hit(m, 4096, 901, 4096, "overlap head: middle of the newer run");
    expect_hit(m, 8192, 201, 4096,
               "overlap head: surviving tail keeps its advanced LBA 200+1");
    expect_miss(m, 12288, "overlap head: past the end");
    maco_clear(m);

    /* (e) a wide insert swallows a narrow one whole. */
    ins_ok(m, 4096, 200, 4096,  "overlap swallow base");
    ins_ok(m, 0,    900, 16384, "overlap swallow newer");
    expect_counts(m, 1, 16384, "overlap swallow: only the wide run remains");
    expect_hit(m, 0,     900, 16384, "overlap swallow: start");
    expect_hit(m, 4096,  901, 12288, "overlap swallow: the old run's bytes now map to 901");
    expect_hit(m, 12288, 903, 4096,  "overlap swallow: last block");
    maco_clear(m);

    /* (f) an insert strictly inside a run splits it into three. */
    ins_ok(m, 0,    100, 16384, "overlap split base");
    ins_ok(m, 4096, 777, 4096,  "overlap split newer (lands in the middle)");
    expect_counts(m, 3, 16384, "overlap split: head + new + tail");
    expect_hit(m, 0,     100, 4096, "overlap split: head stops at the newer run");
    expect_hit(m, 4096,  777, 4096, "overlap split: newer run");
    expect_hit(m, 8192,  102, 8192, "overlap split: tail keeps LBA 100+2");
    expect_hit(m, 12288, 103, 4096, "overlap split: middle of the tail");
    expect_miss(m, 16384, "overlap split: past the end");
    maco_destroy(m);
}

/* --------------------------------------------------------- invalidate */

static void test_invalidate_split_middle(void)
{
    /* The log-truncation case: punch a block out of the middle of a run. */
    struct maco *m = mk(4096, "inv-split");
    if (!m) return;
    ins_ok(m, 0, 1000, 16384, "inv-split base");
    inv_ok(m, 4096, 4096, "inv-split punch one block out of the middle");
    expect_counts(m, 2, 12288, "inv-split: the run became two runs");
    expect_hit(m, 0,     1000, 4096, "inv-split: head");
    expect_miss(m, 4096, "inv-split: the punched block, first byte");
    expect_miss(m, 8191, "inv-split: the punched block, last byte");
    expect_hit(m, 8192,  1002, 8192, "inv-split: tail keeps LBA 1000+2");
    expect_hit(m, 12288, 1003, 4096, "inv-split: middle of the tail");
    expect_miss(m, 16384, "inv-split: past the end");

    /* Punching two separate holes leaves three runs. */
    inv_ok(m, 12288, 4096, "inv-split second punch");
    expect_counts(m, 2, 8192, "inv-split: after the second punch");
    expect_hit(m, 8192, 1002, 4096, "inv-split: tail shortened to one block");
    expect_miss(m, 12288, "inv-split: second hole");
    maco_destroy(m);
}

static void test_invalidate_whole_and_over(void)
{
    struct maco *m = mk(4096, "inv-whole");
    if (!m) return;
    ins_ok(m, 0, 1000, 16384, "inv-whole base");
    inv_ok(m, 0, 16384, "inv-whole exact cover");
    expect_counts(m, 0, 0, "inv-whole: an exactly covered run is dropped");
    expect_miss(m, 0,     "inv-whole: start is gone");
    expect_miss(m, 16383, "inv-whole: end is gone");

    /* A range far larger than the run also drops it, and does not underflow. */
    ins_ok(m, 8192, 500, 4096, "inv-over base");
    expect_counts(m, 1, 4096, "inv-over: one run before");
    inv_ok(m, 0, 1u << 20, "inv-over covering far more than the run");
    expect_counts(m, 0, 0, "inv-over: run dropped, accounting back to zero");
    expect_miss(m, 8192, "inv-over: gone");
    maco_destroy(m);
}

static void test_invalidate_prefix_and_suffix(void)
{
    struct maco *m = mk(4096, "inv-prefix");
    if (!m) return;
    ins_ok(m, 0, 1000, 16384, "inv-prefix base");
    inv_ok(m, 0, 4096, "inv-prefix drop the first block");
    expect_counts(m, 1, 12288, "inv-prefix: one shorter run");
    expect_miss(m, 0,    "inv-prefix: first block gone");
    expect_miss(m, 4095, "inv-prefix: last byte of the dropped block");
    expect_hit(m, 4096, 1001, 12288, "inv-prefix: survivor starts at LBA 1000+1");
    expect_hit(m, 8192, 1002, 8192,  "inv-prefix: middle of the survivor");
    maco_clear(m);

    ins_ok(m, 0, 1000, 16384, "inv-suffix base");
    inv_ok(m, 12288, 4096, "inv-suffix drop the last block");
    expect_counts(m, 1, 12288, "inv-suffix: one shorter run");
    expect_hit(m, 0, 1000, 12288, "inv-suffix: contig shrinks to 12288");
    expect_miss(m, 12288, "inv-suffix: dropped block");
    expect_miss(m, 16383, "inv-suffix: end of the dropped block");
    maco_clear(m);

    /* A suffix range that runs off the end of the run is fine. */
    ins_ok(m, 0, 1000, 16384, "inv-suffix-over base");
    inv_ok(m, 12288, 1u << 20, "inv-suffix-over past the end of the run");
    expect_counts(m, 1, 12288, "inv-suffix-over: same result as an exact suffix");
    expect_hit(m, 0, 1000, 12288, "inv-suffix-over: contig 12288");
    expect_miss(m, 12288, "inv-suffix-over: dropped tail");
    maco_destroy(m);
}

static void test_invalidate_spanning_runs_and_holes(void)
{
    /* 512-byte blocks so 2048 is a legal boundary. Three runs with holes;
     * one invalidate crosses all of it. */
    struct maco *m = mk(512, "inv-span");
    if (!m) return;
    ins_ok(m, 0,     100, 4096, "inv-span A");
    ins_ok(m, 8192,  200, 4096, "inv-span B");
    ins_ok(m, 16384, 300, 4096, "inv-span C");
    expect_counts(m, 3, 12288, "inv-span: three runs before");

    inv_ok(m, 2048, 12288, "inv-span invalidate [2048, 14336)");
    expect_counts(m, 2, 6144, "inv-span: A trimmed, B gone, C untouched");
    expect_hit(m, 0, 100, 2048, "inv-span: A survives as [0,2048)");
    expect_miss(m, 2048,  "inv-span: trimmed part of A");
    expect_miss(m, 4095,  "inv-span: end of the trimmed part of A");
    expect_miss(m, 8192,  "inv-span: B was removed entirely");
    expect_miss(m, 12287, "inv-span: end of removed B");
    expect_hit(m, 16384, 300, 4096, "inv-span: C untouched");
    expect_hit(m, 18432, 304, 2048, "inv-span: middle of untouched C");
    maco_destroy(m);
}

static void test_invalidate_rounds_outward(void)
{
    /* Truncation is not always block aligned. Keeping a mapping for a block
     * that was partly invalidated would let the write path aim at a block
     * whose contents are no longer known, so the whole block must go. */
    struct maco *m = mk(4096, "inv-round");
    if (!m) return;
    ins_ok(m, 0, 1000, 16384, "inv-round base");
    inv_ok(m, 4196, 1000, "inv-round invalidate [4196,5196), inside block 1");
    expect_counts(m, 2, 12288, "inv-round: the whole of block 1 is dropped");
    expect_hit(m, 0, 1000, 4096, "inv-round: head ends on the block boundary");
    expect_miss(m, 4096, "inv-round: start of the partly-hit block");
    expect_miss(m, 4196, "inv-round: the byte actually asked for");
    expect_miss(m, 8191, "inv-round: end of the partly-hit block");
    expect_hit(m, 8192, 1002, 8192, "inv-round: tail starts on a block boundary");
    maco_clear(m);

    /* One byte at offset 1 costs the whole first block. */
    ins_ok(m, 0, 1000, 8192, "inv-round-one base");
    inv_ok(m, 1, 1, "inv-round-one invalidate a single byte");
    expect_counts(m, 1, 4096, "inv-round-one: first block dropped");
    expect_miss(m, 0,    "inv-round-one: block 0 gone");
    expect_miss(m, 4095, "inv-round-one: end of block 0");
    expect_hit(m, 4096, 1001, 4096, "inv-round-one: block 1 survives at LBA 1001");
    maco_destroy(m);
}

static void test_invalidate_noops_and_errors(void)
{
    struct maco *m = mk(4096, "inv-noop");
    if (!m) return;
    ins_ok(m, 0,    1000, 4096, "inv-noop A");
    ins_ok(m, 8192, 2000, 4096, "inv-noop B");
    expect_counts(m, 2, 8192, "inv-noop: two runs before");

    /* Zero length: nothing to invalidate, success, nothing changes. */
    int rc = maco_invalidate(m, 4096, 0);
    T_OK(rc == 0, "invalidate(len=0) is a no-op success (rc=%d, want 0)", rc);
    expect_counts(m, 2, 8192, "inv-noop: zero-length invalidate changed nothing");
    expect_hit(m, 0, 1000, 4096, "inv-noop: A intact after zero-length invalidate");

    /* Entirely inside a hole: success, nothing changes. */
    inv_ok(m, 4096, 4096, "inv-noop invalidate a hole");
    expect_counts(m, 2, 8192, "inv-noop: invalidating a hole changed nothing");
    expect_hit(m, 0,    1000, 4096, "inv-noop: A still intact");
    expect_hit(m, 8192, 2000, 4096, "inv-noop: B still intact");

    /* Past everything: success, nothing changes. */
    inv_ok(m, 1u << 20, 4096, "inv-noop invalidate past the last run");
    expect_counts(m, 2, 8192, "inv-noop: invalidating past the end changed nothing");

    /* off + len overflows 64 bits: refuse, change nothing. */
    rc = maco_invalidate(m, UINT64_MAX - 100, 1000);
    T_OK(rc == -EINVAL,
         "invalidate(off=2^64-101 len=1000) overflows -> rc=%d (want -EINVAL=%d)",
         rc, -EINVAL);
    expect_counts(m, 2, 8192, "inv-noop: refused overflow changed nothing");
    maco_destroy(m);
}

/* ----------------------------------------------------- insert refusals */

static void test_insert_refusals(void)
{
    struct maco *m = mk(512, "refuse/512");
    if (!m) return;
    int rc;

    rc = maco_insert(m, 0, 100, 0);
    T_OK(rc == -EINVAL, "insert(len=0) refuses -> rc=%d (want -EINVAL=%d)", rc, -EINVAL);

    rc = maco_insert(m, 511, 100, 4096);
    T_OK(rc == -EINVAL,
         "insert(file_off=511) refuses: a run must start on a 512-byte device block -> rc=%d (want %d)",
         rc, -EINVAL);

    rc = maco_insert(m, 4097, 100, 4096);
    T_OK(rc == -EINVAL,
         "insert(file_off=4097) refuses: 4097 %% 512 != 0 -> rc=%d (want %d)",
         rc, -EINVAL);

    rc = maco_insert(m, 0xFFFFFFFFFFFFFE00ULL, 100, 4096);
    T_OK(rc == -EINVAL,
         "insert(off=2^64-512 len=4096) overflows the file range -> rc=%d (want %d)",
         rc, -EINVAL);

    expect_counts(m, 0, 0, "refuse/512: no refused insert left any state behind");
    expect_miss(m, 0,    "refuse/512: nothing mapped at 0");
    expect_miss(m, 4096, "refuse/512: nothing mapped at 4096");

    /* An offset that is legal for 512 is illegal for 4096. */
    ins_ok(m, 512, 100, 4096, "refuse/512: 512 is aligned for lbs=512");
    expect_counts(m, 1, 4096, "refuse/512: the legal insert did land");
    expect_hit(m, 512, 100, 4096, "refuse/512: the legal insert reads back");
    maco_destroy(m);

    struct maco *m4 = mk(4096, "refuse/4096");
    if (!m4) return;
    rc = maco_insert(m4, 512, 100, 4096);
    T_OK(rc == -EINVAL,
         "insert(file_off=512) refuses on a 4096-byte-lbs map -> rc=%d (want %d)",
         rc, -EINVAL);
    expect_counts(m4, 0, 0, "refuse/4096: nothing stored");
    ins_ok(m4, 4096, 100, 4096, "refuse/4096: 4096 is aligned for lbs=4096");
    expect_hit(m4, 4096, 100, 4096, "refuse/4096: the legal insert reads back");
    maco_destroy(m4);
}

/* ------------------------------------------------ clear / accounting */

static void test_clear_and_accounting(void)
{
    struct maco *m = mk(4096, "acct");
    if (!m) return;

    ins_ok(m, 0, 100, 16384, "acct step 1");
    expect_counts(m, 1, 16384, "acct after step 1");

    ins_ok(m, 32768, 200, 8192, "acct step 2");
    expect_counts(m, 2, 24576, "acct after step 2");

    /* Overwrite the tail of run 1 with a discontiguous LBA: byte count is
     * unchanged, entry count goes up by one. */
    ins_ok(m, 8192, 900, 8192, "acct step 3 (overwrites the tail of run 1)");
    expect_counts(m, 3, 24576, "acct after step 3: same bytes, one more entry");
    expect_hit(m, 0,     100, 8192, "acct: run 1 head");
    expect_hit(m, 8192,  900, 8192, "acct: the newer middle run");
    expect_hit(m, 32768, 200, 8192, "acct: run 2 untouched");

    inv_ok(m, 0, 8192, "acct step 4");
    expect_counts(m, 2, 16384, "acct after step 4");
    expect_miss(m, 0, "acct: invalidated head is gone");

    maco_clear(m);
    expect_counts(m, 0, 0, "acct: clear empties the map");
    expect_miss(m, 8192,  "acct: cleared map misses at a formerly mapped offset");
    expect_miss(m, 32768, "acct: cleared map misses at the other former run");

    /* The map is reusable after clear. */
    ins_ok(m, 0, 4242, 4096, "acct: reuse after clear");
    expect_counts(m, 1, 4096, "acct: reused map holds one run");
    expect_hit(m, 0, 4242, 4096, "acct: reused map reads back");
    maco_clear(m);
    maco_clear(m);
    expect_counts(m, 0, 0, "acct: clear twice is harmless");
    maco_destroy(m);
}

/* ------------------------------------------------------------ many runs */

#define MANY_N       10000u
#define MANY_STRIDE  16384ULL   /* file pitch: 8192 mapped + 8192 hole      */
#define MANY_LEN     8192ULL    /* two 4096-byte device blocks per run      */
#define MANY_LBA0    1000000ULL
#define MANY_LBASTEP 64ULL      /* device gap too, so nothing can coalesce  */

static uint64_t many_off(unsigned i) { return (uint64_t)i * MANY_STRIDE; }
static uint64_t many_lba(unsigned i) { return MANY_LBA0 + (uint64_t)i * MANY_LBASTEP; }

/* Verify every run without emitting 50k TAP lines: count failures, report the
 * first one with its numbers. */
static void verify_many(const struct maco *m, const char *what)
{
    unsigned bad = 0, first_bad = 0;
    char detail[256];
    detail[0] = '\0';

    for (unsigned i = 0; i < MANY_N; i++) {
        uint64_t off = many_off(i), want_lba = many_lba(i);
        uint64_t lba = SENTINEL, contig = SENTINEL;
        int rc;
        int ok = 1;

        rc = maco_lookup(m, off, &lba, &contig);
        if (rc != 0 || lba != want_lba || contig != MANY_LEN) ok = 0;
        if (ok) {
            rc = maco_lookup(m, off + 4096, &lba, &contig);
            if (rc != 0 || lba != want_lba + 1 || contig != 4096) ok = 0;
        }
        if (ok) {
            /* 4196 bytes in: 4196/4096 = 1 block, 8192-4196 = 3996 bytes left. */
            rc = maco_lookup(m, off + 4196, &lba, &contig);
            if (rc != 0 || lba != want_lba + 1 || contig != 3996) ok = 0;
        }
        if (ok) {
            rc = maco_lookup(m, off + MANY_LEN, &lba, &contig);
            if (rc != -ENOENT) ok = 0;                 /* first byte of the hole */
        }
        if (ok) {
            rc = maco_lookup(m, off + MANY_STRIDE - 1, &lba, &contig);
            if (rc != -ENOENT) ok = 0;                 /* last byte of the hole  */
        }
        if (!ok) {
            if (bad == 0) {
                first_bad = i;
                snprintf(detail, sizeof detail,
                         "run %u at off=%llu want lba=%llu; last probe gave rc=%d lba=%llu contig=%llu",
                         i, (ull)off, (ull)want_lba, rc, (ull)lba, (ull)contig);
            }
            bad++;
        }
    }
    T_OK(bad == 0, "%s: all %u runs look up correctly (%u wrong, first bad run %u) %s",
         what, MANY_N, bad, bad ? first_bad : 0, detail);
}

static void test_many_runs_ascending(void)
{
    struct maco *m = mk(4096, "many-asc");
    if (!m) return;
    int insfail = 0;
    for (unsigned i = 0; i < MANY_N; i++)
        if (maco_insert(m, many_off(i), many_lba(i), MANY_LEN) != 0) insfail++;
    T_OK(insfail == 0, "many-asc: %u inserts all returned 0 (%d failed)", MANY_N, insfail);
    expect_counts(m, MANY_N, (uint64_t)MANY_N * MANY_LEN, "many-asc: after 10k inserts");
    verify_many(m, "many-asc");

    expect_miss(m, many_off(MANY_N), "many-asc: past the last run");
    expect_hit(m, many_off(MANY_N - 1), many_lba(MANY_N - 1), MANY_LEN,
               "many-asc: the last run");

    /* One invalidate that spans half the map. */
    inv_ok(m, 0, many_off(MANY_N / 2), "many-asc: invalidate the first half");
    expect_counts(m, MANY_N / 2, (uint64_t)(MANY_N / 2) * MANY_LEN,
                  "many-asc: after invalidating the first half");
    expect_miss(m, many_off(0), "many-asc: first run gone");
    expect_miss(m, many_off(MANY_N / 2 - 1), "many-asc: last run of the first half gone");
    expect_hit(m, many_off(MANY_N / 2), many_lba(MANY_N / 2), MANY_LEN,
               "many-asc: first surviving run");
    maco_destroy(m);
}

static void test_many_runs_shuffled(void)
{
    /* i*7919 mod 10000 walks every index exactly once (7919 is coprime with
     * 10000), so this is the same set of runs in a scattered order. */
    struct maco *m = mk(4096, "many-shuf");
    if (!m) return;
    int insfail = 0;
    for (unsigned i = 0; i < MANY_N; i++) {
        unsigned idx = (unsigned)(((uint64_t)i * 7919ULL) % MANY_N);
        if (maco_insert(m, many_off(idx), many_lba(idx), MANY_LEN) != 0) insfail++;
    }
    T_OK(insfail == 0, "many-shuf: %u out-of-order inserts all returned 0 (%d failed)",
         MANY_N, insfail);
    expect_counts(m, MANY_N, (uint64_t)MANY_N * MANY_LEN,
                  "many-shuf: out-of-order inserts give the same map");
    verify_many(m, "many-shuf");
    maco_destroy(m);
}

/* ----------------------------------------------- NULL-argument robustness */

static void null_arg_child(void)
{
    uint64_t lba = 12345, contig = 6789;
    int bad = 0;

    /* A NULL map must never report a hit. -EINVAL is the intended code; any
     * nonzero refusal passes here, a crash does not. */
    if (maco_lookup(NULL, 0, &lba, &contig) == 0) bad |= 1;
    if (maco_insert(NULL, 0, 100, 4096) != -EINVAL) bad |= 2;
    if (maco_invalidate(NULL, 0, 4096) != -EINVAL) bad |= 4;
    if (maco_count(NULL) != 0) bad |= 8;
    if (maco_mapped_bytes(NULL) != 0) bad |= 16;
    maco_clear(NULL);
    maco_destroy(NULL);
    _exit(bad);
}

static void test_null_args_refuse(void)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        T_SKIP("fork() failed (%s): cannot run the NULL-argument checks", strerror(errno));
        return;
    }
    if (pid == 0) null_arg_child();

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        T_SKIP("waitpid failed (%s): NULL-argument checks inconclusive", strerror(errno));
        return;
    }
    if (WIFSIGNALED(status)) {
        T_OK(0, "NULL-argument calls must refuse, not crash: child died on signal %d",
             WTERMSIG(status));
        return;
    }
    T_OK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "NULL map/args refused by every entry point (child status bitmask=%d: "
         "1=lookup 2=insert 4=invalidate 8=count 16=mapped_bytes)",
         WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/* -------------------------------------------------------------------- main */

int main(void)
{
    test_create_and_empty();
    test_single_run_lbs512();
    test_single_run_lbs4096_offset_base();
    test_lbs_scales_the_lba_advance();
    test_holes();
    test_adjacent_contiguous_coalesces();
    test_adjacent_discontiguous_never_merges();
    test_overlapping_inserts();
    test_invalidate_split_middle();
    test_invalidate_whole_and_over();
    test_invalidate_prefix_and_suffix();
    test_invalidate_spanning_runs_and_holes();
    test_invalidate_rounds_outward();
    test_invalidate_noops_and_errors();
    test_insert_refusals();
    test_clear_and_accounting();
    test_many_runs_ascending();
    test_many_runs_shuffled();
    test_null_args_refuse();   /* last: it forks, so a NULL deref costs one line */
    T_DONE();
}
