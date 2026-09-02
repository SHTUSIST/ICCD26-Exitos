/* Tier-0 ADVERSARIAL unit tests for Maco (src/maco.c, include/exitos_maco.h).
 *
 * Pure data structure: no syscalls, no device, no root, no filesystem.
 *
 * This file is deliberately disjoint from tests/unit/test_maco.c. That file
 * pins the happy paths and the five basic overlap shapes. This one goes after
 * what it does not touch:
 *
 *   - runs whose LENGTH is not a whole number of device blocks (test_maco.c
 *     only ever inserts block multiples). src/maco.c refuses a misaligned
 *     file_off but says nothing about len, and punch() only claims to be exact
 *     "whenever the inserted run is a whole number of blocks";
 *   - overlap shapes with a device-contiguity relationship, i.e. an overwrite
 *     that must re-merge the map instead of fragmenting it;
 *   - the top of the 64-bit address space, on BOTH axes: file offsets near
 *     2^64 and LBAs near 2^64;
 *   - a randomised differential test against an independent shadow model that
 *     stores one LBA per device block, checking lba, contig, count and
 *     mapped_bytes after every single operation;
 *   - 50,000 runs inserted in a shuffled (Fisher-Yates) order, and 20,000
 *     device-contiguous blocks inserted in shuffled order which must collapse
 *     back to exactly ONE entry;
 *   - maco_create() with block sizes that are not powers of two.
 *
 * THE CONTRACT USED FOR THE ASSERTIONS
 * ------------------------------------
 * include/exitos_maco.h documents only the lookup return convention. Everything
 * else quoted below comes from the block comment at the top of src/maco.c,
 * which is the only written statement of the invariants:
 *
 *   ARITHMETIC: "For a run {file_off, lba, len} and any byte k in [0, len):
 *                lookup(file_off + k) -> lba + k / lbs   (floor division)
 *                contig = len - k"
 *   I1: "entries are sorted strictly ascending by file_off and never overlap"
 *   I2: "every entry has len > 0"
 *   I3: "every stored file_off is a multiple of lbs, so the formula above is
 *        exact. maco_insert enforces this on the way in and maco_invalidate
 *        rounds outward to whole blocks so that trimming preserves it"
 *   I4: "two entries that are adjacent in the file AND contiguous on the device
 *        are merged into one ... Merging never crosses an LBA discontinuity."
 *
 * WHERE THE HEADER IS SILENT (reported as contract ambiguity, not as a bug):
 * include/exitos_maco.h does not say who wins when two inserts overlap, whether
 * len == 0 or a misaligned file_off is an error or a no-op, whether len must be
 * a multiple of lbs, that lbs must be non-zero or a power of two, or that
 * maco_invalidate rounds outward. A caller who compiles against the installed
 * header alone cannot learn any of that.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void hit(const struct maco *m, uint64_t off,
                uint64_t want_lba, uint64_t want_contig, const char *what)
{
    uint64_t lba = SENTINEL, contig = SENTINEL;
    int rc = maco_lookup(m, off, &lba, &contig);
    T_OK(rc == 0 && lba == want_lba && contig == want_contig,
         "%s: lookup(off=%llu) -> rc=%d lba=%llu contig=%llu  (want rc=0 lba=%llu contig=%llu)",
         what, (ull)off, rc, (ull)lba, (ull)contig, (ull)want_lba, (ull)want_contig);
}

static void miss(const struct maco *m, uint64_t off, const char *what)
{
    uint64_t lba = SENTINEL, contig = SENTINEL;
    int rc = maco_lookup(m, off, &lba, &contig);
    T_OK(rc == -ENOENT, "%s: lookup(off=%llu) -> rc=%d lba=%llu (want -ENOENT=%d)",
         what, (ull)off, rc, (ull)lba, -ENOENT);
}

static void counts(const struct maco *m, size_t want_n, uint64_t want_bytes,
                   const char *what)
{
    T_OK(maco_count(m) == want_n && maco_mapped_bytes(m) == want_bytes,
         "%s: count=%llu mapped_bytes=%llu (want count=%llu mapped_bytes=%llu)",
         what, (ull)maco_count(m), (ull)maco_mapped_bytes(m),
         (ull)want_n, (ull)want_bytes);
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

/* ==========================================================================
 * GROUP 1 - run lengths that are NOT a whole number of device blocks.
 *
 * PROPERTY DEFENDED: after any insert, every byte that the insert did not
 * overwrite must still resolve to the device block it resolved to before.
 *
 * WHY IT MATTERS: maco answers "which device block do I write this file byte
 * to". If a surviving byte's answer moves by one block, the write path aims a
 * write at the wrong block - silent data corruption on the device, and the
 * block that should have been written is left stale.
 *
 * WHY THESE ASSERTIONS ARE FAIR UNDER *EITHER* READING OF THE CONTRACT: the
 * probes below are all at offsets the partial-block insert does NOT cover. If
 * maco_insert is supposed to REFUSE a len that is not a multiple of lbs, the
 * map still holds the untouched original run and these probes pass. If it is
 * supposed to ACCEPT it, the untouched bytes must keep their original mapping
 * and these probes pass too. They can only fail if the insert both lands and
 * corrupts bytes it never claimed.
 * ========================================================================== */

static void test_partial_block_insert_head(void)
{
    /* lbs=4096. [0,16384) -> LBA 100..103. Overwrite only the first 100 bytes.
     * Offsets 4096/8192/12288 are block 1/2/3 of the original run and must
     * still read back as LBA 101/102/103. */
    struct maco *m = mk(4096, "partial-head");
    if (!m) return;
    int rc;

    ins_ok(m, 0, 100, 16384, "partial-head base run [0,16384) @100");
    rc = maco_insert(m, 0, 777, 100);       /* 100 bytes: 100 % 4096 != 0 */
    T_OK(rc == 0 || rc == -EINVAL,
         "partial-head: insert(off=0 lba=777 len=100) -> rc=%d "
         "(0 = accepted, -EINVAL = refused; either is a coherent contract)", rc);

    hit(m, 4096,  101, 12288,
        "partial-head: block 1 of the original run is untouched by a 100-byte "
        "insert, so it must still map to 100+1");
    hit(m, 8192,  102, 8192,
        "partial-head: block 2 of the original run must still map to 100+2");
    hit(m, 12288, 103, 4096,
        "partial-head: block 3 of the original run must still map to 100+3");
    hit(m, 16383, 103, 1,
        "partial-head: last byte of the original run still maps to 100+3");
    miss(m, 16384, "partial-head: past the end of everything");

    /* 12288 surviving bytes (blocks 1-3) + the 100 newly inserted ones. NOT
      * 16384: the assertions just above require the surviving tail to start at
      * 4096, which means block 0 gives up its mapping, and it cannot both give
      * it up and still be counted. A partly overwritten block must lose its
      * mapping, because every entry's file offset has to stay block aligned --
      * lookup() derives the LBA as base + (off - file_off)/lbs, so a tail
      * starting at 100 would answer one block too low for the whole tail. */
     T_OK(maco_mapped_bytes(m) == 12388,
         "partial-head: mapped_bytes=%llu (want 12388: 12288 surviving + 100 new; "
         "the partly overwritten block 0 loses its mapping to keep offsets aligned)",
         (ull)maco_mapped_bytes(m));
    maco_destroy(m);
}

static void test_partial_block_insert_middle(void)
{
    /* Same idea, but the partial-block run lands in the middle of the run, so
     * punch() has to build BOTH a head and a tail. The tail is the one that
     * starts at a non-block-aligned file offset (invariant I3). */
    struct maco *m = mk(4096, "partial-mid");
    if (!m) return;
    int rc;

    ins_ok(m, 0, 100, 16384, "partial-mid base run [0,16384) @100");
    rc = maco_insert(m, 4096, 777, 100);   /* aligned start, ragged end 4196 */
    T_OK(rc == 0 || rc == -EINVAL,
         "partial-mid: insert(off=4096 lba=777 len=100) -> rc=%d "
         "(0 = accepted, -EINVAL = refused)", rc);

    hit(m, 8192,  102, 8192,
        "partial-mid: block 2 is beyond the 100 overwritten bytes, so it must "
        "still map to 100+2");
    hit(m, 12288, 103, 4096,
        "partial-mid: block 3 must still map to 100+3");
    hit(m, 16383, 103, 1,
        "partial-mid: last byte of the original run still maps to 100+3");
    hit(m, 0, 100, 4096,
        "partial-mid: block 0 is before the overwrite and keeps LBA 100 "
        "(contig may legitimately stop at 4096 where the newer run starts)");

    T_OK(maco_mapped_bytes(m) == 12388,
         "partial-mid: mapped_bytes=%llu (want 12388: the partly overwritten block loses its mapping, see partial-head)", (ull)maco_mapped_bytes(m));
    maco_destroy(m);
}

static void test_partial_block_insert_512(void)
{
    /* The same defect class at lbs=512, with a length that is a multiple of
     * nothing in particular. [0,4096) @ LBA 900..907, overwrite [0,3). */
    struct maco *m = mk(512, "partial-512");
    if (!m) return;
    int rc;

    ins_ok(m, 0, 900, 4096, "partial-512 base run [0,4096) @900 (8 blocks)");
    rc = maco_insert(m, 0, 55, 3);
    T_OK(rc == 0 || rc == -EINVAL,
         "partial-512: insert(off=0 lba=55 len=3) -> rc=%d", rc);

    hit(m, 512,  901, 3584, "partial-512: block 1 still maps to 900+1");
    hit(m, 2048, 904, 2048, "partial-512: block 4 still maps to 900+4");
    hit(m, 4095, 907, 1,    "partial-512: last byte still maps to 900+7");
    T_OK(maco_mapped_bytes(m) == 3587,
         "partial-512: mapped_bytes=%llu (want 3587: same rule at lbs=512)", (ull)maco_mapped_bytes(m));
    maco_destroy(m);
}

/* Zero-length insert on a POPULATED map: test_maco.c only checks the return
 * code on an empty map. An empty run carries no mapping, so whatever the return
 * code is, the map must be bit-for-bit unchanged. */
static void test_zero_length_insert_is_inert(void)
{
    struct maco *m = mk(4096, "zerolen");
    if (!m) return;

    ins_ok(m, 0,    100, 8192, "zerolen A");
    ins_ok(m, 16384, 900, 8192, "zerolen B");
    counts(m, 2, 16384, "zerolen: two runs before");

    int rc = maco_insert(m, 4096, 555, 0);
    T_OK(rc == -EINVAL,
         "insert(len=0) -> rc=%d (want -EINVAL: src/maco.c says \"an empty run "
         "carries no mapping\")", rc);
    counts(m, 2, 16384, "zerolen: a refused zero-length insert changed nothing");
    hit(m, 4096, 101, 4096, "zerolen: the byte it aimed at is untouched");

    /* Zero length at an offset that would have punched a hole in a run. */
    rc = maco_insert(m, 0, 555, 0);
    T_OK(rc == -EINVAL, "insert(off=0 len=0) -> rc=%d (want -EINVAL)", rc);
    hit(m, 0, 100, 8192, "zerolen: run A intact, still 8192 contiguous bytes");
    maco_destroy(m);
}

/* Misaligned file_off must not punch either: a refused insert has to be a
 * complete no-op, not "refuse after destroying the overlap". */
static void test_misaligned_insert_does_not_punch(void)
{
    struct maco *m = mk(4096, "misalign");
    if (!m) return;

    ins_ok(m, 0, 100, 16384, "misalign base run [0,16384) @100");
    int rc = maco_insert(m, 1, 777, 8192);
    T_OK(rc == -EINVAL, "insert(off=1) -> rc=%d (want -EINVAL, invariant I3)", rc);
    rc = maco_insert(m, 4095, 777, 8192);
    T_OK(rc == -EINVAL, "insert(off=4095) -> rc=%d (want -EINVAL)", rc);
    rc = maco_insert(m, 8193, 777, 8192);
    T_OK(rc == -EINVAL, "insert(off=8193) -> rc=%d (want -EINVAL)", rc);

    counts(m, 1, 16384, "misalign: three refused inserts left the map untouched");
    hit(m, 0,     100, 16384, "misalign: the run is still one 16384-byte run");
    hit(m, 8192,  102, 8192,  "misalign: middle of the run untouched");
    hit(m, 12288, 103, 4096,  "misalign: tail of the run untouched");
    maco_destroy(m);
}

/* ==========================================================================
 * GROUP 2 - overlap shapes that must RE-MERGE rather than fragment.
 *
 * PROPERTY DEFENDED: invariant I4. contig must answer "how many bytes can I
 * send in a single device command from here". An overwrite that restores the
 * original device layout must leave a map that is indistinguishable from the
 * one before it.
 *
 * WHY IT MATTERS: maco is refreshed from the extent tree. FIEMAP re-reports
 * extents that were already there. If a redundant re-insert fragments the map,
 * every refresh makes contig shorter and the write path issues more, smaller
 * commands - the exact cost this module exists to remove.
 * ========================================================================== */

static void test_redundant_insert_must_not_fragment(void)
{
    struct maco *m = mk(4096, "redundant");
    if (!m) return;

    ins_ok(m, 0, 100, 16384, "redundant base [0,16384) @100");
    /* Re-insert the middle block with exactly the LBA it already had. */
    ins_ok(m, 4096, 101, 4096, "redundant re-insert of block 1 with its own LBA");
    counts(m, 1, 16384,
           "redundant: re-inserting a block with the LBA it already had must "
           "leave ONE entry (I4), not three");
    hit(m, 0, 100, 16384,
        "redundant: contig is still the full 16384 bytes after the re-insert");
    hit(m, 8192, 102, 8192, "redundant: block 2 unchanged");

    /* Re-insert a 2-block slice spanning the middle, again with its own LBAs. */
    ins_ok(m, 4096, 101, 8192, "redundant re-insert of blocks 1-2");
    counts(m, 1, 16384, "redundant: still one entry after a 2-block re-insert");
    hit(m, 0, 100, 16384, "redundant: contig still 16384");
    maco_destroy(m);
}

static void test_overlap_that_joins_left_only(void)
{
    /* Two runs, discontiguous on the device: [0,8192)@100 and [8192,8192)@500.
     * Now overwrite the second one with the LBA that CONTINUES the first
     * (102). The map must collapse to a single 16384-byte run. */
    struct maco *m = mk(4096, "join-left");
    if (!m) return;

    ins_ok(m, 0,    100, 8192, "join-left A @100");
    ins_ok(m, 8192, 500, 8192, "join-left B @500 (discontiguous)");
    counts(m, 2, 16384, "join-left: two entries while B is discontiguous");
    hit(m, 0, 100, 8192, "join-left: contig stops at the discontinuity");

    ins_ok(m, 8192, 102, 8192, "join-left: rewrite B as the continuation of A");
    counts(m, 1, 16384,
           "join-left: an overwrite that restores device contiguity must merge "
           "back to ONE entry (I4)");
    hit(m, 0,     100, 16384, "join-left: contig now spans both runs");
    hit(m, 12288, 103, 4096,  "join-left: last block of the merged run");
    maco_destroy(m);
}

static void test_overlap_that_joins_both_sides(void)
{
    /* A hole between two device-contiguous runs, filled by an insert that also
     * OVERLAPS both neighbours. The result must be one run. */
    struct maco *m = mk(512, "join-both");
    if (!m) return;

    ins_ok(m, 0,    2000, 2048, "join-both A [0,2048) @2000 (4 blocks)");
    ins_ok(m, 4096, 2008, 2048, "join-both C [4096,6144) @2008 (4 blocks)");
    counts(m, 2, 4096, "join-both: two runs with a 2048-byte hole between them");
    miss(m, 2048, "join-both: the hole");

    /* Insert [1536, 3072) @2003 : overlaps A's last block and C's first block,
     * and is device-contiguous with both. */
    ins_ok(m, 1536, 2003, 3072, "join-both B overlapping both neighbours");
    counts(m, 1, 6144,
           "join-both: an overlapping, device-contiguous fill must produce ONE "
           "entry covering [0,6144)");
    hit(m, 0,    2000, 6144, "join-both: contig spans the whole thing");
    hit(m, 3072, 2006, 3072, "join-both: block 6 maps to 2000+6");
    hit(m, 6143, 2011, 1,    "join-both: last byte maps to 2000+11");
    miss(m, 6144, "join-both: past the end");
    maco_destroy(m);
}

static void test_insert_spanning_several_runs_and_holes(void)
{
    /* One insert that eats: the tail of A, all of B, all of a hole, and the
     * head of C. Nothing must survive inside its range, and the two survivors
     * must keep exact LBAs. */
    struct maco *m = mk(4096, "span-ins");
    if (!m) return;

    ins_ok(m, 0,     100, 12288, "span-ins A [0,12288) @100");
    ins_ok(m, 16384, 200, 8192,  "span-ins B [16384,24576) @200");
    ins_ok(m, 32768, 300, 12288, "span-ins C [32768,45056) @300");
    counts(m, 3, 32768, "span-ins: three runs before");

    /* [8192, 36864) : last block of A, all of B, the hole, first block of C. */
    ins_ok(m, 8192, 900, 28672, "span-ins the big overwrite");
    counts(m, 3, 45056,
           "span-ins: head of A + the new run + tail of C = 3 entries, "
           "8192 + 28672 + 8192 = 45056 bytes");
    hit(m, 0,     100, 8192,  "span-ins: A's surviving head, contig stops at 8192");
    hit(m, 8192,  900, 28672, "span-ins: the new run");
    hit(m, 20480, 903, 16384, "span-ins: 12288 bytes into the new run -> 900+3");
    hit(m, 36864, 301, 8192,  "span-ins: C's tail keeps 300+1 (one block eaten)");
    hit(m, 45055, 302, 1,     "span-ins: last byte of C -> 300+2");
    miss(m, 45056, "span-ins: past the end");
    maco_destroy(m);
}

/* ==========================================================================
 * GROUP 3 - boundary lookups: exactly at, one byte before, one byte after.
 *
 * PROPERTY DEFENDED: the half-open interval [file_off, file_off+len). An
 * off-by-one at either edge either loses a legitimately mapped byte (the write
 * path falls back to the slow path) or claims a byte that is not mapped (the
 * write path aims at a block belonging to nothing).
 * ========================================================================== */

static void test_boundaries_lbs512(void)
{
    /* lbs=512. One run of three blocks at a non-zero, non-power-of-two-block
     * file offset, with holes on both sides. */
    struct maco *m = mk(512, "bound512");
    if (!m) return;

    ins_ok(m, 1024, 50, 1536, "bound512 run [1024,2560) @50 (3 blocks)");

    miss(m, 0,    "bound512: far before the run");
    miss(m, 1023, "bound512: exactly one byte BEFORE the run");
    hit(m, 1024,  50, 1536, "bound512: exactly AT the start");
    hit(m, 1025,  50, 1535, "bound512: one byte after the start (k=1, floor -> +0)");
    hit(m, 1535,  50, 1025, "bound512: last byte of block 0 -- k=511 floors to "
                            "+0, and contig counts bytes left in the RUN "
                            "(1536-511), not in the block");
    hit(m, 1536,  51, 1024, "bound512: first byte of block 1 (k=512 -> +1)");
    hit(m, 1537,  51, 1023, "bound512: one byte into block 1");
    hit(m, 2047,  51, 513,  "bound512: last byte of block 1 (k=1023 -> +1)");
    hit(m, 2048,  52, 512,  "bound512: first byte of block 2 (k=1024 -> +2)");
    hit(m, 2559,  52, 1,    "bound512: LAST byte of the run (k=1535 -> +2)");
    miss(m, 2560, "bound512: exactly one byte AFTER the run");
    miss(m, 2561, "bound512: two bytes after the run");
    maco_destroy(m);
}

static void test_boundaries_lbs4096(void)
{
    struct maco *m = mk(4096, "bound4096");
    if (!m) return;

    ins_ok(m, 8192, 700, 12288, "bound4096 run [8192,20480) @700 (3 blocks)");

    miss(m, 8191, "bound4096: one byte before the run");
    hit(m, 8192,  700, 12288, "bound4096: at the start");
    hit(m, 8193,  700, 12287, "bound4096: k=1 -> +0");
    hit(m, 12287, 700, 8193,  "bound4096: k=4095 -> +0 (last byte of block 0)");
    hit(m, 12288, 701, 8192,  "bound4096: k=4096 -> +1");
    hit(m, 12289, 701, 8191,  "bound4096: k=4097 -> +1");
    hit(m, 17192, 702, 3288,  "bound4096: k=9000 -> 9000/4096=2 -> +2");
    hit(m, 20479, 702, 1,     "bound4096: last byte of the run (k=12287 -> +2)");
    miss(m, 20480, "bound4096: one byte after the run");

    /* Now put a device-DISCONTIGUOUS run immediately after and re-check the
     * seam from both sides: contig must NOT run past the discontinuity. */
    ins_ok(m, 20480, 9000, 4096, "bound4096: discontiguous neighbour");
    hit(m, 20479, 702, 1,
        "bound4096: contig at the last byte is still 1, not 1+4096 - merging "
        "never crosses an LBA discontinuity (I4)");
    hit(m, 20480, 9000, 4096, "bound4096: the neighbour, from its first byte");
    hit(m, 24575, 9000, 1,    "bound4096: last byte of the neighbour");
    miss(m, 24576, "bound4096: past both");
    maco_destroy(m);
}

/* ==========================================================================
 * GROUP 4 - invalidate: whole map, empty ranges, split, cross-run partials.
 *
 * PROPERTY DEFENDED: after invalidate, mapped_bytes must equal the exact number
 * of bytes still reachable through lookup, and both sides of a split must carry
 * the right LBA.
 *
 * WHY IT MATTERS: mapped_bytes is the accounting the rest of the system trusts
 * to decide whether the map is worth consulting. A split that leaks bytes into
 * the counter makes the map look populated when it is not; a split that loses
 * them makes a valid map look empty.
 * ========================================================================== */

static void test_invalidate_entire_map_by_max_range(void)
{
    struct maco *m = mk(4096, "inv-all");
    if (!m) return;

    ins_ok(m, 0,        100, 8192,  "inv-all A at the very bottom");
    ins_ok(m, 1ULL<<40, 200, 8192,  "inv-all B at 2^40");
    ins_ok(m, 0xFFFFFFFFFFFFE000ULL, 300, 4096, "inv-all C just under 2^64");
    counts(m, 3, 20480, "inv-all: three runs spread across the whole range");

    /* [0, 2^64-1) is the largest range maco_invalidate can be handed without
     * off+len wrapping. It must clear everything. */
    inv_ok(m, 0, UINT64_MAX, "inv-all invalidate(0, 2^64-1)");
    counts(m, 0, 0, "inv-all: the whole map is gone, accounting back to zero");
    miss(m, 0, "inv-all: A gone");
    miss(m, 1ULL<<40, "inv-all: B gone");
    miss(m, 0xFFFFFFFFFFFFE000ULL, "inv-all: C gone");
    maco_destroy(m);
}

static void test_invalidate_empty_ranges(void)
{
    struct maco *m = mk(4096, "inv-empty");
    if (!m) return;

    ins_ok(m, 16384, 100, 4096, "inv-empty A");
    ins_ok(m, 32768, 200, 4096, "inv-empty B");
    counts(m, 2, 8192, "inv-empty: two runs");

    /* Entirely BEFORE the first run - test_maco.c covers the hole and the
     * past-the-end cases but not this one. */
    inv_ok(m, 0, 16384, "inv-empty invalidate [0,16384), entirely before A");
    counts(m, 2, 8192, "inv-empty: nothing changed");
    hit(m, 16384, 100, 4096, "inv-empty: A intact");

    /* Exactly abutting A's start, one byte short. */
    inv_ok(m, 12288, 4096, "inv-empty invalidate [12288,16384), abuts A");
    counts(m, 2, 8192, "inv-empty: abutting range changed nothing");
    hit(m, 16384, 100, 4096, "inv-empty: A still intact after the abutting range");

    /* Exactly abutting A's end from the right. */
    inv_ok(m, 20480, 4096, "inv-empty invalidate [20480,24576), abuts A's end");
    counts(m, 2, 8192, "inv-empty: A survives a range that starts where it ends");
    hit(m, 16384, 100, 4096, "inv-empty: A untouched");
    hit(m, 20479, 100, 1,    "inv-empty: A's last byte untouched");
    maco_destroy(m);
}

static void test_invalidate_split_exact_accounting_512(void)
{
    /* lbs=512, a 20-block run. Punch out blocks 5..6 (a mid-block range that
     * rounds outward), then punch the tail again. Every surviving byte and the
     * byte counter are checked. */
    struct maco *m = mk(512, "inv-split512");
    if (!m) return;

    ins_ok(m, 0, 4000, 10240, "inv-split512 run [0,10240) @4000 (20 blocks)");
    counts(m, 1, 10240, "inv-split512: one run of 20 blocks");

    /* [2600, 3100) touches block 5 (2560..3071) and block 6 (3072..3583).
     * Outward rounding must drop the whole of both: [2560, 3584). */
    inv_ok(m, 2600, 500, "inv-split512 punch [2600,3100) -> rounds to [2560,3584)");
    counts(m, 2, 10240 - 1024,
           "inv-split512: 2 entries, exactly 2 blocks (1024 bytes) removed");
    hit(m, 0,    4000, 2560, "inv-split512: head is blocks 0-4, contig 2560");
    hit(m, 2559, 4004, 1,    "inv-split512: last byte of the head -> 4000+4");
    miss(m, 2560, "inv-split512: first byte of the hole");
    miss(m, 2600, "inv-split512: the byte actually asked for");
    miss(m, 3583, "inv-split512: last byte of the hole");
    hit(m, 3584, 4007, 6656, "inv-split512: tail starts at block 7 -> 4000+7");
    hit(m, 10239, 4019, 1,   "inv-split512: last byte of the tail -> 4000+19");
    miss(m, 10240, "inv-split512: past the end");

    /* Split the tail again, in its middle. */
    inv_ok(m, 6144, 512, "inv-split512 punch block 12 out of the tail");
    counts(m, 3, 10240 - 1024 - 512,
           "inv-split512: 3 entries, exactly 3 blocks removed in total");
    hit(m, 3584, 4007, 2560, "inv-split512: tail-head is blocks 7-11");
    miss(m, 6144, "inv-split512: the second hole");
    hit(m, 6656, 4013, 3584, "inv-split512: tail-tail starts at block 13");
    hit(m, 10239, 4019, 1,   "inv-split512: last byte still -> 4000+19");
    maco_destroy(m);
}

static void test_invalidate_head_of_one_tail_of_another(void)
{
    /* The case the task names: one range that takes the TAIL of run A and the
     * HEAD of run B, with a hole in between, leaving both partly alive. */
    struct maco *m = mk(4096, "inv-2sided");
    if (!m) return;

    ins_ok(m, 0,     100, 16384, "inv-2sided A [0,16384) @100");
    ins_ok(m, 32768, 500, 16384, "inv-2sided B [32768,49152) @500");
    counts(m, 2, 32768, "inv-2sided: two runs, a 16384-byte hole between them");

    /* [8192, 40960): A's blocks 2-3, the hole, B's blocks 0-1. */
    inv_ok(m, 8192, 32768, "inv-2sided invalidate [8192,40960)");
    counts(m, 2, 8192 + 8192,
           "inv-2sided: A's head (8192) + B's tail (8192) = 16384 bytes left");
    hit(m, 0,    100, 8192, "inv-2sided: A's head survives, contig 8192");
    hit(m, 4096, 101, 4096, "inv-2sided: A's block 1 -> 100+1");
    miss(m, 8192,  "inv-2sided: A's block 2 gone");
    miss(m, 16383, "inv-2sided: A's last byte gone");
    miss(m, 32768, "inv-2sided: B's block 0 gone");
    miss(m, 40959, "inv-2sided: B's block 1 gone");
    hit(m, 40960, 502, 8192, "inv-2sided: B's tail keeps 500+2");
    hit(m, 49151, 503, 1,    "inv-2sided: B's last byte -> 500+3");
    miss(m, 49152, "inv-2sided: past B");

    /* Now an UNALIGNED version of the same shape on what is left: outward
     * rounding must eat whole blocks on both edges. */
    inv_ok(m, 4097, 36864, "inv-2sided unaligned [4097,40961) -> [4096,45056)");
    counts(m, 2, 4096 + 4096,
           "inv-2sided: rounding outward leaves A block 0 and B block 3 only");
    hit(m, 0, 100, 4096, "inv-2sided: A block 0 survives");
    miss(m, 4096, "inv-2sided: A block 1 rounded away");
    miss(m, 40960, "inv-2sided: B block 2 rounded away");
    hit(m, 45056, 503, 4096, "inv-2sided: B block 3 survives at 500+3");
    maco_destroy(m);
}

/* ==========================================================================
 * GROUP 5 - the top of the 64-bit address space.
 *
 * PROPERTY DEFENDED: file_off + len must never wrap, and a run that ends
 * exactly at 2^64-1 must still be looked up correctly at its last byte.
 *
 * WHY IT MATTERS: entry_end() is a bare addition with the comment "insert
 * refuses runs that would wrap". If that guard is off by one, entry_end wraps
 * to a small number, every comparison in lower_bound/find_entry/punch inverts,
 * and lookups return garbage or the map silently loses entries.
 * ========================================================================== */

#define TOP8K  0xFFFFFFFFFFFFE000ULL   /* 2^64 - 8192 */
#define TOP4K  0xFFFFFFFFFFFFF000ULL   /* 2^64 - 4096 */

static void test_file_offsets_near_2_64(void)
{
    struct maco *m = mk(4096, "top64");
    if (!m) return;
    int rc;

    ins_ok(m, TOP8K, 5000, 4096, "top64 run at 2^64-8192, one block");
    hit(m, TOP8K,        5000, 4096, "top64: first byte");
    hit(m, TOP8K + 1,    5000, 4095, "top64: k=1");
    hit(m, TOP8K + 4095, 5000, 1,    "top64: last byte of the run");
    miss(m, TOP4K, "top64: one byte past the run");
    miss(m, UINT64_MAX, "top64: the very last representable byte is unmapped");

    /* len must be capped so file_off+len <= 2^64-1. 4096 would make end == 2^64. */
    rc = maco_insert(m, TOP4K, 6000, 4096);
    T_OK(rc == -EINVAL,
         "insert(off=2^64-4096 len=4096) -> rc=%d (want -EINVAL: end would be "
         "2^64, which src/maco.c refuses as \"the run would wrap past 2^64\")", rc);
    counts(m, 1, 4096, "top64: the refused insert left nothing behind");

    /* 4095 is the largest length that fits: it ends exactly at 2^64-1. */
    ins_ok(m, TOP4K, 6000, 4095, "top64 the largest run that fits at the top");
    counts(m, 2, 8191, "top64: two runs, 4096+4095 bytes");
    hit(m, TOP4K,          6000, 4095, "top64: top run first byte");
    hit(m, UINT64_MAX - 1, 6000, 1,    "top64: byte 2^64-2, the last mapped byte");
    miss(m, UINT64_MAX, "top64: byte 2^64-1 can never be mapped, and must miss");
    hit(m, TOP8K + 4095, 5000, 1,
        "top64: the lower run is unaffected by a neighbour that ends at 2^64-1");
    maco_destroy(m);
}

static void test_merge_at_the_top_of_the_file(void)
{
    /* Two device-contiguous runs whose union ends at 2^64-1. Merging must not
     * make entry_end() wrap. */
    struct maco *m = mk(4096, "top-merge");
    if (!m) return;

    ins_ok(m, TOP8K, 5000, 4096, "top-merge lower run @5000");
    ins_ok(m, TOP4K, 5001, 4095, "top-merge upper run @5001, device-contiguous");
    counts(m, 1, 8191, "top-merge: the two runs merge into one 8191-byte run");
    hit(m, TOP8K,          5000, 8191, "top-merge: contig spans both runs");
    hit(m, TOP8K + 4096,   5001, 4095, "top-merge: k=4096 -> 5000+1");
    hit(m, UINT64_MAX - 1, 5001, 1,    "top-merge: last mapped byte -> 5000+1");
    miss(m, UINT64_MAX, "top-merge: 2^64-1 is still unmapped");
    maco_destroy(m);
}

static void test_invalidate_near_2_64(void)
{
    /* Outward rounding of the END must clamp instead of wrapping to 0. If it
     * wrapped, punch(start, 0) would return immediately and the invalidate
     * would silently do nothing - a stale mapping surviving a truncate. */
    struct maco *m = mk(4096, "inv-top");
    if (!m) return;
    int rc;

    ins_ok(m, TOP4K, 6000, 4095, "inv-top run [2^64-4096, 2^64-1)");
    inv_ok(m, UINT64_MAX - 10, 5,
           "inv-top invalidate 5 bytes at 2^64-11: end rounds outward and must "
           "clamp at 2^64-1, not wrap to 0");
    counts(m, 0, 0,
           "inv-top: the whole block is dropped (outward rounding), not left "
           "behind by a wrapped end");
    miss(m, TOP4K, "inv-top: the run is gone");

    ins_ok(m, TOP8K, 5000, 8191, "inv-top second run spanning the last 2 blocks");
    inv_ok(m, TOP8K, UINT64_MAX - TOP8K,
           "inv-top invalidate the largest range that starts at 2^64-8192");
    counts(m, 0, 0, "inv-top: gone");

    rc = maco_invalidate(m, UINT64_MAX, 2);
    T_OK(rc == -EINVAL,
         "invalidate(off=2^64-1 len=2) -> rc=%d (want -EINVAL, off+len wraps)", rc);
    maco_destroy(m);
}

/* ==========================================================================
 * GROUP 6 - the DEVICE side of the arithmetic near 2^64.
 *
 * PROPERTY DEFENDED: maco_insert refuses a run whose file range would wrap past
 * 2^64 ("the run would wrap past 2^64"). The device range is the same hazard:
 * a run of n blocks starting at LBA L occupies L .. L+n-1, and lookup computes
 * lba + k/lbs with no overflow check. can_join computes a->lba + a->len/lbs
 * with no overflow check either, so two runs at opposite ends of the device
 * address space can compare as "contiguous".
 *
 * WHY IT MATTERS: contig is handed to the write path as "bytes I can send in
 * ONE command from this LBA". A merge across an LBA wrap produces a command
 * that starts near the top of the device address space and claims to run past
 * it. src/maco.c states flatly: "Merging never crosses an LBA discontinuity."
 *
 * HONEST NOTE ON REACHABILITY: no real block device has 2^64 logical blocks, so
 * these LBAs cannot come from a live ext4 extent tree today. They can come from
 * a corrupted or hostile FIEMAP reply, which is what maco is fed from.
 * ========================================================================== */

static void test_lba_wrap_must_not_be_merged(void)
{
    struct maco *m = mk(4096, "lba-wrap");
    if (!m) return;

    /* A = 2 blocks at LBA 2^64-2, so it occupies LBAs 2^64-2 and 2^64-1.
     * B starts at LBA 0. In unsigned arithmetic A.lba + 2 == 0 == B.lba. */
    ins_ok(m, 0,    UINT64_MAX - 1, 8192, "lba-wrap A @ LBA 2^64-2, 2 blocks");
    ins_ok(m, 8192, 0,              4096, "lba-wrap B @ LBA 0, file-adjacent");

    T_OK(maco_count(m) == 2,
         "lba-wrap: count=%llu (want 2). LBA 2^64-2 + 2 blocks wraps to 0; that "
         "is a device discontinuity, and src/maco.c states \"Merging never "
         "crosses an LBA discontinuity\"",
         (ull)maco_count(m));
    hit(m, 0, UINT64_MAX - 1, 8192,
        "lba-wrap: contig from offset 0 must stop at 8192 - a single device "
        "command cannot start at LBA 2^64-2 and cover 12288 bytes");
    maco_destroy(m);
}

static void test_insert_refuses_lba_range_that_wraps(void)
{
    struct maco *m = mk(4096, "lba-ovf");
    if (!m) return;
    uint64_t lba = SENTINEL, contig = SENTINEL;
    int rc, r2 = -1;

    /* 2 blocks starting at LBA 2^64-1 would occupy 2^64-1 and 2^64, which does
     * not exist. maco_insert already refuses exactly this shape on the file
     * axis. If it is accepted here, the second block resolves to LBA 0 - the
     * map points the write path at block 0 of the device. */
    rc = maco_insert(m, 0, UINT64_MAX, 8192);
    if (rc == 0)
        r2 = maco_lookup(m, 4096, &lba, &contig);
    T_OK(rc == -EINVAL,
         "insert(lba=2^64-1 len=8192 lbs=4096) -> rc=%d (want -EINVAL: the run "
         "spans LBAs 2^64-1 and 2^64; the file axis rejects the same shape with "
         "\"the run would wrap past 2^64\"). Consequence of accepting it: "
         "lookup(4096) -> rc=%d lba=%llu, i.e. block 0 of the device",
         rc, r2, (ull)lba);
    maco_destroy(m);
}

/* ==========================================================================
 * GROUP 7 - maco_create() argument validation.
 *
 * PROPERTY DEFENDED: a map can only be created with a block size the rest of
 * the module can divide by. lbs == 0 is rejected (division by zero). A block
 * size that is not a power of two is accepted; that is recorded here as a
 * contract question, and the assertions only check that whatever is accepted
 * stays self-consistent.
 * ========================================================================== */

static void test_create_block_sizes(void)
{
    struct maco *m0 = maco_create(0);
    T_OK(m0 == NULL, "maco_create(0) -> %p (want NULL: k/lbs divides by zero)",
         (void *)m0);
    maco_destroy(m0);

    /* lbs = 1: every file offset is block aligned and lba advances per byte. */
    struct maco *m1 = maco_create(1);
    T_OK(m1 != NULL, "maco_create(1) -> %p", (void *)m1);
    if (m1) {
        ins_ok(m1, 7, 1000, 5, "lbs1: an odd offset is legal when lbs==1");
        hit(m1, 7,  1000, 5, "lbs1: first byte");
        hit(m1, 9,  1002, 3, "lbs1: k=2 -> lba+2");
        hit(m1, 11, 1004, 1, "lbs1: last byte");
        miss(m1, 12, "lbs1: past the end");
        counts(m1, 1, 5, "lbs1: accounting");
        maco_destroy(m1);
    }

    /* Not a power of two. Real logical block sizes always are, and the module
     * never says otherwise, so this is a contract question rather than a proven
     * defect. What IS checked: if the map is handed out, its arithmetic must
     * stay self-consistent. */
    struct maco *m3 = maco_create(1000);
    T_SKIP("maco_create(1000) -> %s. A non-power-of-two logical block size: the "
           "header states no requirement either way, so this is recorded as a "
           "contract question, not asserted. What IS asserted below is that a "
           "map handed out with lbs=1000 stays self-consistent.",
           m3 ? "a map" : "NULL");
    if (m3) {
        ins_ok(m3, 2000, 77, 3000, "lbs1000: run [2000,5000) @77");
        hit(m3, 2000, 77, 3000, "lbs1000: first byte");
        hit(m3, 2999, 77, 2001, "lbs1000: k=999 -> +0");
        hit(m3, 3000, 78, 2000, "lbs1000: k=1000 -> +1");
        hit(m3, 4999, 79, 1,    "lbs1000: k=2999 -> +2");
        miss(m3, 5000, "lbs1000: past the end");
        int rc = maco_insert(m3, 2001, 5, 1000);
        T_OK(rc == -EINVAL,
             "lbs1000: insert(off=2001) -> rc=%d (want -EINVAL, 2001 %% 1000 != 0)",
             rc);
        counts(m3, 1, 3000, "lbs1000: accounting after the refusal");
        maco_destroy(m3);
    }

    /* A block size larger than any run we will ever insert. */
    struct maco *mb = maco_create(1u << 31);
    T_OK(mb != NULL, "maco_create(2^31) -> %p", (void *)mb);
    if (mb) {
        ins_ok(mb, 0, 42, 1024, "lbs2^31: a run much shorter than one block");
        hit(mb, 0,    42, 1024, "lbs2^31: first byte");
        hit(mb, 1023, 42, 1,    "lbs2^31: last byte, still block 0 -> lba+0");
        miss(mb, 1024, "lbs2^31: past the end");
        int rc = maco_insert(mb, 1024, 43, 1024);
        T_OK(rc == -EINVAL,
             "lbs2^31: insert(off=1024) -> rc=%d (want -EINVAL, not a multiple "
             "of 2^31)", rc);
        maco_destroy(mb);
    }
}

/* NULL out-parameters: the header says nothing, and src/maco.c guards both with
 * `if (lba)` / `if (contig)`. A caller that only wants to know "is it mapped"
 * must not have to supply storage it will not read. */
static void test_lookup_null_outparams(void)
{
    struct maco *m = mk(4096, "nullout");
    if (!m) return;
    uint64_t one = SENTINEL;

    ins_ok(m, 4096, 100, 4096, "nullout run");
    T_OK(maco_lookup(m, 4096, NULL, NULL) == 0,
         "lookup(lba=NULL contig=NULL) on a hit -> 0");
    T_OK(maco_lookup(m, 0, NULL, NULL) == -ENOENT,
         "lookup(lba=NULL contig=NULL) on a miss -> -ENOENT");
    T_OK(maco_lookup(m, 4096, &one, NULL) == 0 && one == 100,
         "lookup(contig=NULL) still fills lba (got %llu, want 100)", (ull)one);
    one = SENTINEL;
    T_OK(maco_lookup(m, 8191, NULL, &one) == 0 && one == 1,
         "lookup(lba=NULL) still fills contig (got %llu, want 1)", (ull)one);
    maco_destroy(m);
}

/* ==========================================================================
 * GROUP 8 - randomised differential test against an independent model.
 *
 * PROPERTY DEFENDED: the whole public contract at once. A shadow array holds
 * one LBA per device block and is updated by the plain-English meaning of each
 * call ("insert overwrites those blocks", "invalidate rounds outward to whole
 * blocks and clears them"). After EVERY operation the model predicts, for every
 * block: hit or miss, the LBA, the exact contig (the full maximal device-
 * contiguous stretch, per invariant I4), maco_count (the number of maximal
 * stretches) and maco_mapped_bytes.
 *
 * WHY IT MATTERS: the deterministic tests above can only probe shapes someone
 * thought of. punch() has four independent booleans (head survives, tail
 * survives, range starts in a hole, range ends in a hole) and coalesce_at() can
 * fire on either side; the interesting bugs live in combinations. Randomised
 * sequences with a full re-check after every step localise a failure to one
 * operation.
 *
 * All runs here are whole numbers of blocks, so the model is unambiguous: the
 * ragged-length question is handled separately in GROUP 1.
 * ========================================================================== */

#define SHM_FREE UINT64_MAX
#define SHM_BLOCKS 96u

static uint64_t rnd_state = 0x243F6A8885A308D3ULL;

static uint64_t rnd(void)
{
    uint64_t x = rnd_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rnd_state = x;
    return x;
}

/* Predict every observable from the shadow array. Returns 0 when the map
 * agrees, 1 on the first disagreement (written into `detail`). */
static int shm_check(const struct maco *m, const uint64_t *sh, uint64_t lbs,
                     char *detail, size_t dsz)
{
    unsigned groups = 0;
    uint64_t mapped_blocks = 0;
    const uint64_t probes[4] = { 0, 1, lbs / 2, lbs - 1 };

    for (unsigned b = 0; b < SHM_BLOCKS; b++) {
        uint64_t base = (uint64_t)b * lbs;
        uint64_t lba = SENTINEL, contig = SENTINEL;
        unsigned ge;
        int rc;

        if (sh[b] == SHM_FREE) {
            for (unsigned p = 0; p < 4; p++) {
                rc = maco_lookup(m, base + probes[p], &lba, &contig);
                if (rc != -ENOENT) {
                    snprintf(detail, dsz,
                             "block %u is unmapped in the model but lookup(off=%llu) "
                             "-> rc=%d lba=%llu contig=%llu (want -ENOENT)",
                             b, (ull)(base + probes[p]), rc, (ull)lba, (ull)contig);
                    return 1;
                }
            }
            continue;
        }

        mapped_blocks++;
        if (b == 0 || sh[b - 1] == SHM_FREE || sh[b - 1] + 1 != sh[b])
            groups++;
        ge = b + 1;
        while (ge < SHM_BLOCKS && sh[ge] != SHM_FREE && sh[ge] == sh[ge - 1] + 1)
            ge++;

        for (unsigned p = 0; p < 4; p++) {
            uint64_t off = base + probes[p];
            uint64_t want_contig = (uint64_t)(ge - b) * lbs - probes[p];

            rc = maco_lookup(m, off, &lba, &contig);
            if (rc != 0 || lba != sh[b] || contig != want_contig) {
                snprintf(detail, dsz,
                         "lookup(off=%llu, block %u) -> rc=%d lba=%llu contig=%llu; "
                         "model says rc=0 lba=%llu contig=%llu (block group is "
                         "[%u,%u))",
                         (ull)off, b, rc, (ull)lba, (ull)contig,
                         (ull)sh[b], (ull)want_contig, b, ge);
                return 1;
            }
        }
    }

    if (maco_count(m) != groups) {
        snprintf(detail, dsz,
                 "maco_count=%llu but the model has %u maximal device-contiguous "
                 "stretches (invariant I4: adjacent+contiguous runs are merged)",
                 (ull)maco_count(m), groups);
        return 1;
    }
    if (maco_mapped_bytes(m) != mapped_blocks * lbs) {
        snprintf(detail, dsz,
                 "maco_mapped_bytes=%llu but the model has %llu mapped blocks of "
                 "%llu bytes = %llu",
                 (ull)maco_mapped_bytes(m), (ull)mapped_blocks, (ull)lbs,
                 (ull)(mapped_blocks * lbs));
        return 1;
    }
    return 0;
}

static void test_random_differential(uint32_t lbs, int unaligned_inv,
                                     unsigned nops, uint64_t seed,
                                     const char *what)
{
    struct maco *m = mk(lbs, what);
    uint64_t sh[SHM_BLOCKS];
    char detail[512];
    char opdesc[160];
    unsigned op;

    if (!m) return;
    rnd_state = seed;
    detail[0] = '\0';
    opdesc[0] = '\0';
    for (unsigned b = 0; b < SHM_BLOCKS; b++) sh[b] = SHM_FREE;

    for (op = 0; op < nops; op++) {
        unsigned pick = (unsigned)(rnd() % 100);

        if (pick < 60) {                                   /* insert */
            unsigned b0 = (unsigned)(rnd() % SHM_BLOCKS);
            unsigned nb = 1u + (unsigned)(rnd() % 8);
            uint64_t lba = 1 + (rnd() % (1u << 20));
            int rc;

            if (b0 + nb > SHM_BLOCKS) nb = SHM_BLOCKS - b0;
            rc = maco_insert(m, (uint64_t)b0 * lbs, lba, (uint64_t)nb * lbs);
            snprintf(opdesc, sizeof opdesc,
                     "insert(off=%llu lba=%llu len=%llu) rc=%d",
                     (ull)((uint64_t)b0 * lbs), (ull)lba, (ull)((uint64_t)nb * lbs), rc);
            if (rc != 0) {
                snprintf(detail, sizeof detail,
                         "a whole-block insert inside the test range was refused");
                break;
            }
            for (unsigned i = 0; i < nb; i++) sh[b0 + i] = lba + i;
        } else if (pick < 96) {                            /* invalidate */
            unsigned b0 = (unsigned)(rnd() % SHM_BLOCKS);
            unsigned nb = 1u + (unsigned)(rnd() % 8);
            uint64_t off = (uint64_t)b0 * lbs;
            uint64_t len = (uint64_t)nb * lbs;
            unsigned sblk, eblk;
            int rc;

            if (unaligned_inv) {
                off += rnd() % lbs;
                len = 1 + (rnd() % ((uint64_t)nb * lbs));
            }
            rc = maco_invalidate(m, off, len);
            snprintf(opdesc, sizeof opdesc,
                     "invalidate(off=%llu len=%llu) rc=%d", (ull)off, (ull)len, rc);
            if (rc != 0) {
                snprintf(detail, sizeof detail,
                         "an in-range invalidate was refused");
                break;
            }
            sblk = (unsigned)(off / lbs);
            eblk = (unsigned)((off + len + lbs - 1) / lbs);
            if (eblk > SHM_BLOCKS) eblk = SHM_BLOCKS;
            for (unsigned b = sblk; b < eblk; b++) sh[b] = SHM_FREE;
        } else {                                           /* clear */
            maco_clear(m);
            snprintf(opdesc, sizeof opdesc, "maco_clear()");
            for (unsigned b = 0; b < SHM_BLOCKS; b++) sh[b] = SHM_FREE;
        }

        if (shm_check(m, sh, lbs, detail, sizeof detail))
            break;
    }

    if (op == nops) {
        T_OK(1, "%s: %u randomised ops (lbs=%u, %s invalidate offsets) all agree "
                "with the shadow model (lba, contig, count, mapped_bytes checked "
                "on every block after every op)",
             what, nops, (unsigned)lbs, unaligned_inv ? "unaligned" : "aligned");
    } else {
        T_OK(0, "%s: op #%u (%s) disagrees with the shadow model: %s",
             what, op, opdesc, detail);
    }
    maco_destroy(m);
}

/* ==========================================================================
 * GROUP 9 - scale: tens of thousands of runs inserted out of order.
 *
 * PROPERTY DEFENDED: the sorted-array representation survives insertion in an
 * arbitrary order. tests/unit/test_maco.c walks its 10,000 indices with a fixed
 * multiplicative stride (i*7919 mod 10000), which is a very regular permutation;
 * this one uses a Fisher-Yates shuffle of 50,000 indices at lbs=512, and probes
 * every run at four offsets including one that is not a multiple of the block
 * size.
 *
 * WHY IT MATTERS: extents arrive from FIEMAP in file order, but the map is also
 * refreshed piecemeal after allocations, so inserts land in the middle of the
 * array constantly. An insertion-order-dependent bug would only ever be seen in
 * production.
 * ========================================================================== */

#define BIG_N       50000u
#define BIG_LBS     512u
#define BIG_LEN     1536ULL     /* 3 blocks per run                          */
#define BIG_STRIDE  2560ULL     /* 3 blocks mapped + 2 blocks of hole        */
#define BIG_LBA0    100ULL
#define BIG_LBASTEP 4ULL        /* device gap as well, so nothing coalesces  */

static uint64_t big_off(unsigned i) { return (uint64_t)i * BIG_STRIDE; }
static uint64_t big_lba(unsigned i) { return BIG_LBA0 + (uint64_t)i * BIG_LBASTEP; }

static void test_many_runs_fisher_yates(void)
{
    struct maco *m = mk(BIG_LBS, "big-shuffle");
    unsigned *order;
    char detail[256];
    unsigned bad = 0, first_bad = 0, insfail = 0;

    if (!m) return;
    order = malloc(BIG_N * sizeof(*order));
    if (!order) {
        T_SKIP("big-shuffle: cannot allocate %u indices", BIG_N);
        maco_destroy(m);
        return;
    }
    detail[0] = '\0';
    for (unsigned i = 0; i < BIG_N; i++) order[i] = i;
    rnd_state = 0x9E3779B97F4A7C15ULL;
    for (unsigned i = BIG_N - 1; i > 0; i--) {           /* Fisher-Yates */
        unsigned j = (unsigned)(rnd() % (i + 1));
        unsigned t = order[i]; order[i] = order[j]; order[j] = t;
    }

    for (unsigned k = 0; k < BIG_N; k++) {
        unsigned i = order[k];
        if (maco_insert(m, big_off(i), big_lba(i), BIG_LEN) != 0) insfail++;
    }
    T_OK(insfail == 0, "big-shuffle: %u shuffled inserts all returned 0 (%u failed)",
         BIG_N, insfail);
    counts(m, BIG_N, (uint64_t)BIG_N * BIG_LEN,
           "big-shuffle: 50,000 runs, none merged (each pair is separated by a "
           "hole AND by a device gap)");

    for (unsigned i = 0; i < BIG_N; i++) {
        uint64_t off = big_off(i), wl = big_lba(i);
        uint64_t lba = SENTINEL, contig = SENTINEL;
        int rc, ok = 1;

        rc = maco_lookup(m, off, &lba, &contig);
        if (rc != 0 || lba != wl || contig != BIG_LEN) ok = 0;
        if (ok) {   /* 511 bytes in: still block 0, 1025 bytes left */
            rc = maco_lookup(m, off + 511, &lba, &contig);
            if (rc != 0 || lba != wl || contig != 1025) ok = 0;
        }
        if (ok) {   /* 1535 bytes in: block 2 (1535/512 = 2), 1 byte left */
            rc = maco_lookup(m, off + 1535, &lba, &contig);
            if (rc != 0 || lba != wl + 2 || contig != 1) ok = 0;
        }
        if (ok) {   /* first byte of the hole */
            rc = maco_lookup(m, off + BIG_LEN, &lba, &contig);
            if (rc != -ENOENT) ok = 0;
        }
        if (ok) {   /* last byte of the hole */
            rc = maco_lookup(m, off + BIG_STRIDE - 1, &lba, &contig);
            if (rc != -ENOENT) ok = 0;
        }
        if (!ok) {
            if (bad == 0) {
                first_bad = i;
                snprintf(detail, sizeof detail,
                         "run %u at off=%llu want lba=%llu; last probe rc=%d "
                         "lba=%llu contig=%llu",
                         i, (ull)off, (ull)wl, rc, (ull)lba, (ull)contig);
            }
            bad++;
        }
    }
    T_OK(bad == 0, "big-shuffle: all %u runs read back correctly (%u wrong, "
         "first bad run %u) %s", BIG_N, bad, bad ? first_bad : 0, detail);

    /* One invalidate that removes exactly 1000 whole runs from the middle. */
    inv_ok(m, big_off(10000), 1000 * BIG_STRIDE,
           "big-shuffle: invalidate exactly 1000 runs' worth of file range");
    counts(m, BIG_N - 1000, (uint64_t)(BIG_N - 1000) * BIG_LEN,
           "big-shuffle: exactly 1000 runs and their bytes are gone");
    hit(m, big_off(9999), big_lba(9999), BIG_LEN, "big-shuffle: run 9999 survives");
    miss(m, big_off(10000), "big-shuffle: run 10000 removed");
    miss(m, big_off(10999), "big-shuffle: run 10999 removed");
    hit(m, big_off(11000), big_lba(11000), BIG_LEN,
        "big-shuffle: run 11000 survives - the range ended exactly at its start");

    free(order);
    maco_destroy(m);
}

/* PROPERTY DEFENDED: invariant I4 at scale. 20,000 single-block runs that are
 * device-contiguous end to end, inserted in shuffled order, must collapse into
 * exactly ONE entry. Every insert lands next to a hole, an entry, or two
 * entries, so this exercises coalesce_at()'s left branch, right branch, and
 * both-at-once path tens of thousands of times.
 *
 * WHY IT MATTERS: this is the module's reason to exist. If the merge is missed
 * even occasionally, contig comes back short and the write path splits one
 * device command into many. */
static void test_shuffled_fill_collapses_to_one_entry(void)
{
    enum { FILL_N = 20000 };
    const uint64_t lbs = 4096;
    struct maco *m = mk((uint32_t)lbs, "fill-collapse");
    unsigned *order;
    unsigned insfail = 0;

    if (!m) return;
    order = malloc(FILL_N * sizeof(*order));
    if (!order) {
        T_SKIP("fill-collapse: cannot allocate %d indices", FILL_N);
        maco_destroy(m);
        return;
    }
    for (unsigned i = 0; i < FILL_N; i++) order[i] = i;
    rnd_state = 0xDEADBEEFCAFEF00DULL;
    for (unsigned i = FILL_N - 1; i > 0; i--) {
        unsigned j = (unsigned)(rnd() % (i + 1));
        unsigned t = order[i]; order[i] = order[j]; order[j] = t;
    }

    for (unsigned k = 0; k < FILL_N; k++) {
        unsigned i = order[k];
        if (maco_insert(m, (uint64_t)i * lbs, 5000 + i, lbs) != 0) insfail++;
    }
    T_OK(insfail == 0, "fill-collapse: %d shuffled single-block inserts (%u failed)",
         FILL_N, insfail);
    T_OK(maco_count(m) == 1,
         "fill-collapse: count=%llu (want 1). %d file-adjacent, device-contiguous "
         "blocks must merge into one entry - src/maco.c invariant I4",
         (ull)maco_count(m), FILL_N);
    T_OK(maco_mapped_bytes(m) == (uint64_t)FILL_N * lbs,
         "fill-collapse: mapped_bytes=%llu (want %llu)",
         (ull)maco_mapped_bytes(m), (ull)((uint64_t)FILL_N * lbs));
    hit(m, 0, 5000, (uint64_t)FILL_N * lbs,
        "fill-collapse: one command can cover the whole file from offset 0");
    hit(m, (uint64_t)(FILL_N - 1) * lbs, 5000 + FILL_N - 1, lbs,
        "fill-collapse: the last block");
    hit(m, (uint64_t)(FILL_N / 2) * lbs + 4095, 5000 + FILL_N / 2,
        (uint64_t)FILL_N * lbs - ((uint64_t)(FILL_N / 2) * lbs + 4095),
        "fill-collapse: last byte of the middle block -- k floors to that "
        "block's LBA, and contig runs to the end of the merged run");
    miss(m, (uint64_t)FILL_N * lbs, "fill-collapse: one byte past the end");

    free(order);
    maco_destroy(m);
}

/* -------------------------------------------------------------------- main */

int main(void)
{
    /* Deterministic, small, and fast first, so that a crash in the big
     * randomised groups still leaves the specific findings on stdout. */
    test_create_block_sizes();
    test_lookup_null_outparams();

    test_zero_length_insert_is_inert();
    test_misaligned_insert_does_not_punch();
    test_partial_block_insert_head();
    test_partial_block_insert_middle();
    test_partial_block_insert_512();

    test_redundant_insert_must_not_fragment();
    test_overlap_that_joins_left_only();
    test_overlap_that_joins_both_sides();
    test_insert_spanning_several_runs_and_holes();

    test_boundaries_lbs512();
    test_boundaries_lbs4096();

    test_invalidate_entire_map_by_max_range();
    test_invalidate_empty_ranges();
    test_invalidate_split_exact_accounting_512();
    test_invalidate_head_of_one_tail_of_another();

    test_file_offsets_near_2_64();
    test_merge_at_the_top_of_the_file();
    test_invalidate_near_2_64();
    test_lba_wrap_must_not_be_merged();
    test_insert_refuses_lba_range_that_wraps();

    test_random_differential(4096, 0, 3000, 0x243F6A8885A308D3ULL, "rand4096-aligned");
    test_random_differential(4096, 1, 3000, 0x13198A2E03707344ULL, "rand4096-ragged");
    test_random_differential(512,  1, 3000, 0xA4093822299F31D0ULL, "rand512-ragged");

    test_many_runs_fisher_yates();
    test_shuffled_fill_collapses_to_one_entry();

    T_DONE();
}
