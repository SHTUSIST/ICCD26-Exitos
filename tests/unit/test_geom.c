/* Tier-0 unit tests for the geom module (no root, no device, pure logic).
 *
 * Contract under test: include/exitos_geom.h
 *   lba = part_start_sect * 512 / lbs + fs_block * (fs_bs / lbs)
 * and the refusal rule: exitos_geom_fsblock_to_lba() returns -EPERM whenever
 * geom->writable_raw == 0, and must not write through the out pointer then.
 *
 * exitos_geom_probe() is NOT exercised here: it reads the real filesystem and
 * sysfs, so it belongs to the tier-1 test (tests/integ/test_geom_integ.c).
 * Only the pure translation and the class->string mapping are pure logic.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "exitos_geom.h"
#include "tap.h"

/* A value no legal translation can produce here, so "out pointer untouched"
 * is an observable fact and not an accident. */
#define SENTINEL 0xDEADBEEFCAFEBABEULL

static struct exitos_geom mkgeom(exitos_dev_class c, int writable_raw,
                                 uint64_t start, uint32_t fs_bs, uint32_t lbs)
{
    struct exitos_geom g;
    memset(&g, 0, sizeof g);
    snprintf(g.part_name, sizeof g.part_name, "%s", "nvme0n1p1");
    snprintf(g.disk_path, sizeof g.disk_path, "%s", "/dev/nvme0n1");
    g.part_start_sect = start;
    g.fs_bs = fs_bs;
    g.lbs = lbs;
    g.devclass = c;
    g.writable_raw = writable_raw;
    return g;
}

/* Happy path: expect rc == 0 and exactly `want`. */
static void expect_lba(const char *what, const struct exitos_geom *g,
                       uint64_t fs_block, uint64_t want)
{
    uint64_t lba = SENTINEL;
    int rc = exitos_geom_fsblock_to_lba(g, fs_block, &lba);
    T_EQ(rc, 0, "%s: fsblock_to_lba(blk=%llu) returns 0",
         what, (unsigned long long)fs_block);
    T_OK(lba == want, "%s: blk=%llu -> lba=%llu (got %llu)", what,
         (unsigned long long)fs_block, (unsigned long long)want,
         (unsigned long long)lba);
}

/* Refusal path: expect exactly -EPERM and an untouched out pointer. */
static void expect_refusal(const char *what, const struct exitos_geom *g,
                           uint64_t fs_block)
{
    uint64_t lba = SENTINEL;
    int rc = exitos_geom_fsblock_to_lba(g, fs_block, &lba);
    T_EQ(rc, -EPERM, "%s: fsblock_to_lba(blk=%llu) refuses with -EPERM (got %d)",
         what, (unsigned long long)fs_block, rc);
    T_OK(lba == SENTINEL,
         "%s: blk=%llu leaves *lba untouched (want 0x%llx, got 0x%llx)", what,
         (unsigned long long)fs_block, (unsigned long long)SENTINEL,
         (unsigned long long)lba);
}

/* ---- exitos_devclass_str -------------------------------------------- */

static void test_devclass_str(void)
{
    static const exitos_dev_class all[] = {
        EXITOS_DEV_OK, EXITOS_DEV_DM, EXITOS_DEV_MD,
        EXITOS_DEV_LOOP, EXITOS_DEV_UNKNOWN,
    };
    static const char *names[] = {
        "EXITOS_DEV_OK", "EXITOS_DEV_DM", "EXITOS_DEV_MD",
        "EXITOS_DEV_LOOP", "EXITOS_DEV_UNKNOWN",
    };
    const char *s[5];
    size_t i, j;

    for (i = 0; i < 5; i++) {
        s[i] = exitos_devclass_str(all[i]);
        T_OK(s[i] != NULL, "devclass_str(%s) is non-NULL", names[i]);
        T_OK(s[i] != NULL && s[i][0] != '\0',
             "devclass_str(%s) is a non-empty string", names[i]);
    }

    /* Distinct: a shared label would make a refusal indistinguishable from an
     * allowed class in any log or error message. */
    for (i = 0; i < 5; i++)
        for (j = i + 1; j < 5; j++)
            T_OK(s[i] && s[j] && strcmp(s[i], s[j]) != 0,
                 "devclass_str(%s) differs from devclass_str(%s)",
                 names[i], names[j]);

    /* Stable: same value must map to the same text on every call. */
    for (i = 0; i < 5; i++) {
        const char *again = exitos_devclass_str(all[i]);
        T_OK(again != NULL && s[i] != NULL && strcmp(again, s[i]) == 0,
             "devclass_str(%s) is stable across calls", names[i]);
    }
}

/* ---- arithmetic: convert sysfs 512-byte sectors, then add fs blocks ---- */

static void test_arithmetic(void)
{
    /* 4 KiB ext4 block on a 512 B logical-block device: factor 8.
     * part_start_sect = 2048 is the usual first-partition start. */
    struct exitos_geom p = mkgeom(EXITOS_DEV_OK, 1, 2048, 4096, 512);
    expect_lba("part/4096/512", &p, 0, 2048);
    expect_lba("part/4096/512", &p, 1, 2056);
    expect_lba("part/4096/512", &p, 2, 2064);
    expect_lba("part/4096/512", &p, 100, 2848);
    expect_lba("part/4096/512", &p, 999, 10040);

    /* Whole device (loop0 has no /sys/class/block/loop0/start): start = 0. */
    struct exitos_geom l = mkgeom(EXITOS_DEV_LOOP, 1, 0, 4096, 512);
    expect_lba("loop/4096/512", &l, 0, 0);
    expect_lba("loop/4096/512", &l, 1, 8);
    expect_lba("loop/4096/512", &l, 12345, 98760);

    /* fs_bs == lbs == 4096: factor 1, whole device. */
    struct exitos_geom f1 = mkgeom(EXITOS_DEV_OK, 1, 0, 4096, 4096);
    expect_lba("whole/4096/4096", &f1, 0, 0);
    expect_lba("whole/4096/4096", &f1, 1, 1);
    expect_lba("whole/4096/4096", &f1, 4096, 4096);
    expect_lba("whole/4096/4096", &f1, 1048576, 1048576);

    /* factor 1 with a non-zero start, in unambiguous 512 B units. */
    struct exitos_geom f512 = mkgeom(EXITOS_DEV_OK, 1, 2048, 512, 512);
    expect_lba("part/512/512", &f512, 0, 2048);
    expect_lba("part/512/512", &f512, 7, 2055);

    /* 1 KiB ext4 block: factor 2. */
    struct exitos_geom k1 = mkgeom(EXITOS_DEV_OK, 1, 206848, 1024, 512);
    expect_lba("part/1024/512", &k1, 0, 206848);
    expect_lba("part/1024/512", &k1, 5, 206858);

    /* factor 16. */
    struct exitos_geom k8 = mkgeom(EXITOS_DEV_OK, 1, 1, 8192, 512);
    expect_lba("part/8192/512", &k8, 10, 161);

    /* 4 Kn device (lbs = 4096) with a non-zero start.  sysfs reports the
     * partition start in 512-byte sectors, so 262144 sectors are 32768 native
     * 4 KiB LBAs, not 262144 LBAs. */
    struct exitos_geom kn = mkgeom(EXITOS_DEV_OK, 1, 262144, 4096, 4096);
    expect_lba("part/4096/4096", &kn, 0, 32768);
    expect_lba("part/4096/4096", &kn, 3, 32771);

    /* A start that is not aligned to one native logical block cannot be
     * represented as an LBA and must be refused without changing the output. */
    struct exitos_geom odd = mkgeom(EXITOS_DEV_OK, 1, 1, 4096, 4096);
    uint64_t out = SENTINEL;
    int rc = exitos_geom_fsblock_to_lba(&odd, 0, &out);
    T_EQ(rc, -EINVAL, "unaligned 512-byte partition start on 4Kn is refused");
    T_OK(out == SENTINEL, "unaligned partition start leaves output untouched");
}

/* ---- 64-bit width: nothing may be computed in 32 bits ---------------- */

static void test_large_blocks(void)
{
    struct exitos_geom p = mkgeom(EXITOS_DEV_OK, 1, 2048, 4096, 512);

    /* 2^32 blocks * 8 = 34359738368; a 32-bit intermediate would yield 2048. */
    expect_lba("wide/2^32", &p, 4294967296ULL, 34359740416ULL);
    /* 2^40 blocks * 8 = 8796093022208. */
    expect_lba("wide/2^40", &p, 1099511627776ULL, 8796093024256ULL);

    /* Largest product that still fits: (2^61 - 1) * 8 = 2^64 - 8. */
    struct exitos_geom z = mkgeom(EXITOS_DEV_OK, 1, 0, 4096, 512);
    expect_lba("wide/max-product", &z, 2305843009213693951ULL,
               18446744073709551608ULL);

    /* factor 1 identity at the very top of the range. */
    struct exitos_geom one = mkgeom(EXITOS_DEV_OK, 1, 0, 4096, 4096);
    expect_lba("wide/identity", &one, 18446744073709551615ULL,
               18446744073709551615ULL);

    /* A start larger than 2^32-1 must survive too (huge partition offset). */
    struct exitos_geom bigstart = mkgeom(EXITOS_DEV_OK, 1, 4294967295ULL,
                                         4096, 512);
    expect_lba("wide/start=2^32-1", &bigstart, 1, 4294967303ULL);
    struct exitos_geom hugestart = mkgeom(EXITOS_DEV_OK, 1, 8796093022208ULL,
                                          4096, 512);
    expect_lba("wide/start=2^43", &hugestart, 2, 8796093022224ULL);
}

/* ---- refusal: the safety-critical half ------------------------------ */

static void test_refusal_by_class(void)
{
    /* As exitos_geom_probe() would fill them in: stacked devices get
     * writable_raw = 0, so every translation must be refused. */
    struct exitos_geom dm  = mkgeom(EXITOS_DEV_DM, 0, 2048, 4096, 512);
    struct exitos_geom md  = mkgeom(EXITOS_DEV_MD, 0, 2048, 4096, 512);
    struct exitos_geom unk = mkgeom(EXITOS_DEV_UNKNOWN, 0, 2048, 4096, 512);

    expect_refusal("dm", &dm, 0);
    expect_refusal("dm", &dm, 1);
    expect_refusal("dm", &dm, 1000);
    expect_refusal("dm", &dm, 18446744073709551615ULL);

    expect_refusal("md", &md, 0);
    expect_refusal("md", &md, 1000);

    expect_refusal("unknown", &unk, 0);
    expect_refusal("unknown", &unk, 1000);

    /* fs_block = 0 is the tempting "harmless" case; it must be refused too,
     * because LBA part_start_sect is still someone else's data. */
    expect_refusal("dm/blk0-is-not-harmless", &dm, 0);
}

static void test_refusal_when_not_writable(void)
{
    /* writable_raw is the gate named in the header, independent of the class
     * label: a probe that could not confirm writability must be honoured. */
    struct exitos_geom ok_but_ro   = mkgeom(EXITOS_DEV_OK, 0, 2048, 4096, 512);
    struct exitos_geom loop_but_ro = mkgeom(EXITOS_DEV_LOOP, 0, 0, 4096, 512);

    expect_refusal("class=OK,writable_raw=0", &ok_but_ro, 42);
    expect_refusal("class=LOOP,writable_raw=0", &loop_but_ro, 42);

    /* Degenerate geometry on a refused device: the permission check must run
     * before any arithmetic, so lbs = 0 must not divide by zero. */
    struct exitos_geom degen = mkgeom(EXITOS_DEV_UNKNOWN, 0, 0, 0, 0);
    expect_refusal("unknown/lbs=0", &degen, 7);
}

static void test_writable_geoms_are_not_refused(void)
{
    /* The mirror image of the refusal tests: an allowed class must translate,
     * otherwise a test suite that only refuses would look green. */
    struct exitos_geom ok   = mkgeom(EXITOS_DEV_OK, 1, 2048, 4096, 512);
    struct exitos_geom loop = mkgeom(EXITOS_DEV_LOOP, 1, 0, 4096, 512);
    uint64_t lba;
    int rc;

    lba = SENTINEL;
    rc = exitos_geom_fsblock_to_lba(&ok, 3, &lba);
    T_EQ(rc, 0, "class=OK,writable_raw=1 translates (rc=%d)", rc);
    T_OK(lba == 2072, "class=OK,writable_raw=1: blk=3 -> 2072 (got %llu)",
         (unsigned long long)lba);

    lba = SENTINEL;
    rc = exitos_geom_fsblock_to_lba(&loop, 3, &lba);
    T_EQ(rc, 0, "class=LOOP,writable_raw=1 translates (rc=%d)", rc);
    T_OK(lba == 24, "class=LOOP,writable_raw=1: blk=3 -> 24 (got %llu)",
         (unsigned long long)lba);
}

/* ---- the translator must not mutate the geometry it is handed ------- */

static void test_geom_is_not_modified(void)
{
    struct exitos_geom g = mkgeom(EXITOS_DEV_OK, 1, 2048, 4096, 512);
    struct exitos_geom copy = g;
    uint64_t lba = SENTINEL;

    (void)exitos_geom_fsblock_to_lba(&g, 12345, &lba);
    T_OK(memcmp(&g, &copy, sizeof g) == 0,
         "successful translation leaves the geom struct unchanged");

    struct exitos_geom r = mkgeom(EXITOS_DEV_DM, 0, 2048, 4096, 512);
    struct exitos_geom rcopy = r;
    lba = SENTINEL;
    (void)exitos_geom_fsblock_to_lba(&r, 12345, &lba);
    T_OK(memcmp(&r, &rcopy, sizeof r) == 0,
         "refused translation leaves the geom struct unchanged");
}

/* ---- repeated calls are deterministic -------------------------------- */

static void test_repeatable(void)
{
    struct exitos_geom g = mkgeom(EXITOS_DEV_OK, 1, 2048, 4096, 512);
    uint64_t a = SENTINEL, b = SENTINEL;
    int rca = exitos_geom_fsblock_to_lba(&g, 777, &a);
    int rcb = exitos_geom_fsblock_to_lba(&g, 777, &b);
    T_OK(rca == 0 && rcb == 0 && a == b && a == 2048 + 777 * 8,
         "same input translates to the same lba twice (%llu, %llu)",
         (unsigned long long)a, (unsigned long long)b);
}

int main(void)
{
    test_devclass_str();
    test_arithmetic();
    test_large_blocks();
    test_refusal_by_class();
    test_refusal_when_not_writable();
    test_writable_geoms_are_not_refused();
    test_geom_is_not_modified();
    test_repeatable();
    T_DONE();
}
