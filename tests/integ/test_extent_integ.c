/* Tier-1 integration tests for the `extent` module (include/exitos_extent.h).
 *
 * Everything here runs against an ext4 image on a LOOP device built by
 * tests/harness/loopfix.sh. No physical block device is opened, read or
 * written by this file, at any point. The only device node this test ever
 * names is the /dev/loopN that loopfix.sh just created for it, and even that
 * is only ever handed to `losetup -d` during teardown, guarded by a check
 * that the name really is a loop device.
 *
 * ---------------------------------------------------------------------------
 * CONTRACT DECISIONS THIS TEST PINS DOWN
 * ---------------------------------------------------------------------------
 * The header fixes the signatures but leaves three things open. The tests
 * below choose an answer for each, and here is the reasoning, so that whoever
 * writes src/extent.c implements the same thing rather than guessing.
 *
 * (1) UNITS.  FIEMAP reports fe_logical and fe_length in BYTES and fe_physical
 *     in BYTES. struct extent_run has file_off, fs_block, len. We take:
 *         file_off = fe_logical                   (bytes)
 *         fs_block = fe_physical / fs_bs          (filesystem block number)
 *         len      = fe_length                    (bytes)
 *     file_off is in bytes because struct maco_entry.file_off is documented as
 *     "byte offset within the file"; len is in bytes because maco_entry.len is
 *     documented as "byte length of this run", and load_maco has to hand maco
 *     those numbers. fs_block is a block number because exitos_geom_fsblock_to_lba
 *     is documented to take "an ext4 block number (fs_bs units)". Forgetting the
 *     division is the classic FIEMAP bug, so test_written() asserts an upper
 *     bound on fs_block that a byte value could not possibly satisfy.
 *
 * (2) WHICH FIEMAP FLAGS ARE SKIPPED.  Measured on this fixture (ext4, 4 KiB
 *     blocks, kernel 5.15) with filefrag -v:
 *
 *       file, written + fsync'd  -> flags: last,eof            physical 34816
 *       file, fallocate only     -> flags: last,unwritten,eof  physical 33792
 *       file, written NOT synced -> flags: last,unknown_loc,delalloc,eof
 *                                          physical 0, length 0
 *       file, sparse or empty    -> no extents at all
 *
 *     So the rule is:
 *
 *       FIEMAP_EXTENT_UNKNOWN, _DELALLOC, _ENCODED, _DATA_INLINE, _DATA_TAIL:
 *         exitos_extent_read must NOT return these as runs at all. Their
 *         fe_physical is meaningless. Note what the measurement above shows:
 *         a delayed-allocation extent reports physical offset 0. Passing block
 *         0 through geom yields the LBA of the very start of the filesystem,
 *         i.e. the superblock. Handing that to a raw writer destroys the
 *         filesystem. This is the single most dangerous thing this module can
 *         get wrong, so it is asserted in test_delalloc().
 *         An implementation may instead pass FIEMAP_FLAG_SYNC in fm_flags,
 *         which makes the kernel flush first so these extents never appear;
 *         that also satisfies the assertion.
 *
 *       FIEMAP_EXTENT_UNWRITTEN:
 *         exitos_extent_read MAY return the run (its fe_physical is a real,
 *         allocated block) but must set FIEMAP_EXTENT_UNWRITTEN in
 *         extent_run.flags so the caller can see it. Returning nothing is also
 *         acceptable. What is NOT acceptable is returning it with the flag
 *         cleared, i.e. presenting it as ordinary written data.
 *         exitos_extent_load_maco must NOT load unwritten runs. Blocks under an
 *         unwritten extent are allocated but the extent is still marked
 *         "contains no data": ext4 returns zeros for reads of that range until
 *         the extent is converted by a normal write. A raw LBA write into that
 *         range is therefore invisible through the filesystem - the data is on
 *         the platter and unreadable. Silently losing writes is worse than
 *         refusing, so those runs are dropped.
 *
 *     FIEMAP_EXTENT_LAST / _MERGED / _SHARED / _NOT_ALIGNED are not asserted on
 *     either way; they say nothing about whether the physical offset is usable.
 *
 * (3) RETURN VALUES.  exitos_extent_read returns the run count or a negative
 *     errno (that much is in the header). exitos_extent_load_maco is asserted
 *     to return >= 0 on success and a negative errno on refusal, specifically
 *     -EPERM when geom->writable_raw == 0, matching the documented return of
 *     exitos_geom_fsblock_to_lba for that same condition.
 *
 * ---------------------------------------------------------------------------
 * A NOTE ON TEARDOWN
 * ---------------------------------------------------------------------------
 * The fixture is brought up with `loopfix.sh up`, as required. It is torn down
 * here directly rather than with `loopfix.sh down`, because `down` reads the
 * loop device name from the fixed path /tmp/exitos-loop.dev, which every
 * concurrently running test in this repo also writes. Calling it could detach
 * another test's loop device. Teardown here detaches exactly the device that
 * this process's own `up` printed, and nothing else.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/fiemap.h>
#include <linux/fs.h>

#include "exitos_extent.h"
#include "exitos_geom.h"
#include "exitos_maco.h"
#include "tap.h"

/* ------------------------------------------------------------------ */
/* fixture                                                             */
/* ------------------------------------------------------------------ */

#define IMG_SIZE_MB   512ULL
#define FS_BS         4096ULL
#define LOOP_LBS      512ULL
/* No physical block of this filesystem can sit at or past this block number.
 * A raw FIEMAP byte offset would be ~34 million here, so this bound catches a
 * missing "divide fe_physical by fs_bs". */
#define MAX_FS_BLOCK  ((IMG_SIZE_MB * 1024ULL * 1024ULL) / FS_BS)   /* 131072 */

static char g_img[256];
static char g_mnt[256];
static char g_loop[64];
static int  g_up;

/* Flags whose extents must never be surfaced as runs: their physical offset
 * is not a real location on the device. */
#define BAD_LOCATION_FLAGS (FIEMAP_EXTENT_UNKNOWN     | \
                            FIEMAP_EXTENT_DELALLOC    | \
                            FIEMAP_EXTENT_ENCODED     | \
                            FIEMAP_EXTENT_DATA_INLINE | \
                            FIEMAP_EXTENT_DATA_TAIL)

static int is_loop_dev(const char *s)
{
    size_t i;
    if (strncmp(s, "/dev/loop", 9) != 0) return 0;
    if (s[9] == '\0') return 0;
    for (i = 9; s[i]; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

static const char *find_loopfix(void)
{
    static const char *cands[] = {
        "tests/harness/loopfix.sh",
        "../harness/loopfix.sh",
        "./loopfix.sh",
    };
    const char *env = getenv("EXITOS_LOOPFIX");
    size_t i;
    if (env && access(env, X_OK) == 0) return env;
    for (i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
        if (access(cands[i], R_OK) == 0) return cands[i];
    return NULL;
}

static void fixture_down(void)
{
    char cmd[768];
    if (!g_up) return;
    g_up = 0;
    sync();
    if (g_mnt[0]) {
        snprintf(cmd, sizeof(cmd),
                 "mountpoint -q '%s' && umount '%s' >/dev/null 2>&1; true", g_mnt, g_mnt);
        if (system(cmd) == -1) { /* best effort */ }
    }
    if (is_loop_dev(g_loop)) {
        snprintf(cmd, sizeof(cmd), "losetup -d '%s' >/dev/null 2>&1; true", g_loop);
        if (system(cmd) == -1) { /* best effort */ }
    }
    if (g_img[0]) unlink(g_img);
    if (g_mnt[0]) rmdir(g_mnt);
}

static int fixture_up(void)
{
    const char *sh = find_loopfix();
    char cmd[1024], line[512];
    FILE *p;
    int got = 0;

    if (!sh) return -1;
    /* Unique per process so parallel module tests never collide. */
    snprintf(g_img, sizeof(g_img), "/tmp/exitos-extent-%d.img", (int)getpid());
    snprintf(g_mnt, sizeof(g_mnt), "/tmp/exitos-extent-mnt-%d", (int)getpid());
    unlink(g_img);          /* loopfix refuses if the image already exists */

    snprintf(cmd, sizeof(cmd),
             "EXITOS_IMG='%s' EXITOS_MNT='%s' EXITOS_SIZE_MB=%llu bash '%s' up 2>&1",
             g_img, g_mnt, IMG_SIZE_MB, sh);
    p = popen(cmd, "r");
    if (!p) return -1;
    while (fgets(line, sizeof(line), p)) {
        char *l = strstr(line, "LOOP=");
        if (l) {
            if (sscanf(l, "LOOP=%63s", g_loop) == 1) got = 1;
        }
    }
    pclose(p);
    if (!got || !is_loop_dev(g_loop)) { g_loop[0] = '\0'; return -1; }
    g_up = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

#define MAX_RUNS 256
#define CANARY   0xA5

static void canary_fill(struct extent_run *r, int n)
{
    memset(r, CANARY, (size_t)n * sizeof(*r));
}

static int canary_intact(const struct extent_run *r)
{
    const unsigned char *p = (const unsigned char *)r;
    size_t i;
    for (i = 0; i < sizeof(*r); i++)
        if (p[i] != CANARY) return 0;
    return 1;
}

static int open_fixture_file(const char *name, char *path, size_t psz)
{
    snprintf(path, psz, "%s/%s", g_mnt, name);
    return open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
}

static int fill_range(int fd, uint64_t off, uint64_t len)
{
    static char buf[256 * 1024];
    uint64_t done = 0;
    memset(buf, 'X', sizeof(buf));
    while (done < len) {
        uint64_t chunk = len - done;
        ssize_t w;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        w = pwrite(fd, buf, (size_t)chunk, (off_t)(off + done));
        if (w <= 0) return -1;
        done += (uint64_t)w;
    }
    return 0;
}

/* Geometry probed once from the mount point; several tests need it. */
static struct exitos_geom g_geom;
static int g_geom_ok;

/* A hand-built geometry that a correct implementation must refuse. Not
 * produced by exitos_geom_probe, so this test stands even if geom is broken. */
static void make_refusing_geom(struct exitos_geom *g, exitos_dev_class cls,
                               const char *part, const char *disk)
{
    memset(g, 0, sizeof(*g));
    snprintf(g->part_name, sizeof(g->part_name), "%s", part);
    snprintf(g->disk_path, sizeof(g->disk_path), "%s", disk);
    g->part_start_sect = 2048;
    g->lbs             = 512;
    g->fs_bs           = 4096;
    g->devclass        = cls;
    g->writable_raw    = 0;      /* the gate under test */
}

/* ------------------------------------------------------------------ */
/* 1. geometry of the fixture                                          */
/* ------------------------------------------------------------------ */

static void test_geom_probe(void)
{
    int rc = exitos_geom_probe(g_mnt, &g_geom);
    T_EQ(rc, 0, "geom probe of the loop fixture mount succeeds");
    if (rc != 0) { g_geom_ok = 0; return; }
    g_geom_ok = 1;
    T_EQ(g_geom.devclass, EXITOS_DEV_LOOP, "fixture is classed as a loop device");
    T_EQ(g_geom.writable_raw, 1, "loop device is raw-writable");
    T_EQ(g_geom.fs_bs, (uint32_t)FS_BS, "fixture fs block size is 4096 (mkfs.ext4 -b 4096)");
    T_EQ(g_geom.lbs, (uint32_t)LOOP_LBS, "loop device logical block size is 512");
}

/* ------------------------------------------------------------------ */
/* 2. fully written, preallocated file                                 */
/* ------------------------------------------------------------------ */

#define WRITTEN_SZ (8ULL * 1024 * 1024)

static void test_written(void)
{
    struct extent_run runs[MAX_RUNS];
    char path[320];
    int fd, n, i;
    int contiguous = 1, increasing = 1, aligned = 1, in_range = 1;
    int nonzero_len = 1, clean_flags = 1, not_unwritten = 1;
    uint64_t total = 0, prev_end = 0;

    fd = open_fixture_file("written.log", path, sizeof(path));
    T_OK(fd >= 0, "created %s", path);
    if (fd < 0) return;

    T_EQ(fallocate(fd, 0, 0, (off_t)WRITTEN_SZ), 0, "fallocate 8 MiB");
    T_EQ(fill_range(fd, 0, WRITTEN_SZ), 0, "wrote all 8 MiB");
    T_EQ(fsync(fd), 0, "fsync converted the preallocation to written extents");

    canary_fill(runs, MAX_RUNS);
    n = exitos_extent_read(fd, runs, MAX_RUNS);
    T_OK(n > 0, "extent_read returns at least one run for an 8 MiB written file (got %d)", n);
    if (n <= 0) { close(fd); return; }
    T_OK(n < MAX_RUNS, "run count %d fits in the caller's array", n);

    for (i = 0; i < n; i++) {
        if (runs[i].len == 0)                            nonzero_len = 0;
        if (runs[i].file_off % FS_BS)                    aligned = 0;
        if (runs[i].len % FS_BS)                         aligned = 0;
        if (i > 0 && runs[i].file_off <= runs[i - 1].file_off) increasing = 0;
        if (runs[i].file_off != prev_end)                contiguous = 0;
        /* fs_block must be a BLOCK number, not the raw FIEMAP byte offset. */
        if (runs[i].fs_block == 0 || runs[i].fs_block >= MAX_FS_BLOCK) in_range = 0;
        if (runs[i].flags & BAD_LOCATION_FLAGS)          clean_flags = 0;
        if (runs[i].flags & FIEMAP_EXTENT_UNWRITTEN)     not_unwritten = 0;
        prev_end = runs[i].file_off + runs[i].len;
        total   += runs[i].len;
    }

    T_EQ((long long)total, (long long)WRITTEN_SZ,
         "runs cover the whole file: sum of len == 8388608 bytes");
    T_EQ((long long)runs[0].file_off, 0, "first run starts at file offset 0");
    T_OK(increasing,  "file_off values are strictly increasing");
    T_OK(contiguous,  "file_off values are contiguous: each run starts where the previous ended");
    T_OK(nonzero_len, "no run has zero length");
    T_OK(aligned,     "every file_off and len is a multiple of the 4096-byte fs block");
    T_OK(in_range,    "fs_block is a block number in (0, %llu), not a FIEMAP byte offset",
                      (unsigned long long)MAX_FS_BLOCK);
    T_OK(clean_flags, "no run carries UNKNOWN/DELALLOC/ENCODED/INLINE/TAIL");
    T_OK(not_unwritten, "an fsync'd fully written file yields no UNWRITTEN runs");
    T_OK(canary_intact(&runs[n]), "extent_read did not write past the run count it returned");

    /* ---- load into maco ---- */
    if (!g_geom_ok) {
        T_SKIP("geom probe failed, skipping maco load for written.log");
        close(fd);
        return;
    }
    {
        struct maco *m = maco_create(g_geom.lbs);
        int rc, lba_ok = 1, contig_ok = 1, ground_ok = 1;
        size_t cnt;

        T_OK(m != NULL, "maco_create for the load test");
        if (!m) { close(fd); return; }

        rc = exitos_extent_load_maco(fd, &g_geom, m);
        T_OK(rc >= 0, "load_maco succeeds on a loop-backed writable geom (rc=%d)", rc);
        T_EQ((long long)maco_mapped_bytes(m), (long long)WRITTEN_SZ,
             "maco_mapped_bytes equals the mapped length, 8388608 bytes");
        cnt = maco_count(m);
        T_OK(cnt >= 1 && cnt <= (size_t)n,
             "maco holds between 1 and %d entries (got %zu)", n, cnt);

        for (i = 0; i < n; i++) {
            uint64_t want = 0, lba = 0, contig = 0;
            if (exitos_geom_fsblock_to_lba(&g_geom, runs[i].fs_block, &want) != 0) {
                ground_ok = 0;
                continue;
            }
            /* Independent ground truth for the loop fixture: whole-device loop,
             * so partition start is 0 and LBA = fs_block * (4096/512). */
            if (g_geom.part_start_sect == 0 && g_geom.fs_bs == FS_BS && g_geom.lbs == LOOP_LBS)
                if (want != runs[i].fs_block * (FS_BS / LOOP_LBS)) ground_ok = 0;
            if (maco_lookup(m, runs[i].file_off, &lba, &contig) != 0) { lba_ok = 0; continue; }
            if (lba != want)             lba_ok = 0;
            if (contig != runs[i].len)   contig_ok = 0;
        }
        T_OK(ground_ok, "geom translation of each run matches fs_block * 8 on the loop fixture");
        T_OK(lba_ok,    "every run's file_off looks up to the geom-translated LBA");
        T_OK(contig_ok, "every run's file_off reports the full run length as contiguous");

        {
            uint64_t lba = 0, contig = 0;
            T_EQ(maco_lookup(m, WRITTEN_SZ, &lba, &contig), -ENOENT,
                 "lookup one byte past EOF misses");
        }
        maco_destroy(m);
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 3. preallocated but never written: UNWRITTEN extents                */
/* ------------------------------------------------------------------ */

#define PREALLOC_SZ (4ULL * 1024 * 1024)

static void test_prealloc_unwritten(void)
{
    struct extent_run runs[MAX_RUNS];
    char path[320];
    int fd, n, i, all_flagged = 1, clean = 1;

    fd = open_fixture_file("prealloc.log", path, sizeof(path));
    T_OK(fd >= 0, "created %s", path);
    if (fd < 0) return;
    T_EQ(fallocate(fd, 0, 0, (off_t)PREALLOC_SZ), 0, "fallocate 4 MiB, write nothing");
    T_EQ(fsync(fd), 0, "fsync the preallocation");

    canary_fill(runs, MAX_RUNS);
    n = exitos_extent_read(fd, runs, MAX_RUNS);
    T_OK(n >= 0, "extent_read on a purely preallocated file does not error (got %d)", n);

    for (i = 0; i < n; i++) {
        if (!(runs[i].flags & FIEMAP_EXTENT_UNWRITTEN)) all_flagged = 0;
        if (runs[i].flags & BAD_LOCATION_FLAGS)         clean = 0;
    }
    T_OK(all_flagged,
         "every run of an unwritten preallocation carries FIEMAP_EXTENT_UNWRITTEN "
         "(or none is returned); it is never presented as plain data");
    T_OK(clean, "no unknown-location flags on a preallocated file");

    if (!g_geom_ok) {
        T_SKIP("geom probe failed, skipping maco load for prealloc.log");
        close(fd);
        return;
    }
    {
        struct maco *m = maco_create(g_geom.lbs);
        int rc;
        T_OK(m != NULL, "maco_create for the unwritten load test");
        if (!m) { close(fd); return; }
        rc = exitos_extent_load_maco(fd, &g_geom, m);
        T_OK(rc >= 0, "load_maco on an all-unwritten file is not an error (rc=%d)", rc);
        T_EQ((long long)maco_count(m), 0,
             "unwritten extents are skipped: maco stays empty, so no raw write can "
             "land in a range ext4 still reads back as zeros");
        T_EQ((long long)maco_mapped_bytes(m), 0, "maco_mapped_bytes is 0 for an all-unwritten file");
        maco_destroy(m);
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 4. sparse and empty files yield zero runs                           */
/* ------------------------------------------------------------------ */

static void test_sparse_and_empty(void)
{
    struct extent_run runs[MAX_RUNS];
    char path[320];
    int fd, n;

    /* empty */
    fd = open_fixture_file("empty.log", path, sizeof(path));
    T_OK(fd >= 0, "created %s", path);
    if (fd >= 0) {
        T_EQ(fsync(fd), 0, "fsync the empty file");
        canary_fill(runs, MAX_RUNS);
        n = exitos_extent_read(fd, runs, MAX_RUNS);
        T_EQ(n, 0, "a zero-length file yields exactly 0 runs");
        T_OK(canary_intact(&runs[0]), "extent_read wrote nothing into the array for an empty file");
        if (g_geom_ok) {
            struct maco *m = maco_create(g_geom.lbs);
            if (m) {
                int rc = exitos_extent_load_maco(fd, &g_geom, m);
                T_OK(rc >= 0, "load_maco on an empty file is not an error (rc=%d)", rc);
                T_EQ((long long)maco_count(m), 0, "empty file loads 0 maco entries");
                T_EQ((long long)maco_mapped_bytes(m), 0, "empty file maps 0 bytes");
                maco_destroy(m);
            }
        }
        close(fd);
    }

    /* sparse: 16 MiB of pure hole, no data blocks anywhere */
    fd = open_fixture_file("sparse.log", path, sizeof(path));
    T_OK(fd >= 0, "created %s", path);
    if (fd >= 0) {
        T_EQ(ftruncate(fd, (off_t)(16ULL * 1024 * 1024)), 0, "ftruncate to 16 MiB without writing");
        T_EQ(fsync(fd), 0, "fsync the sparse file");
        canary_fill(runs, MAX_RUNS);
        n = exitos_extent_read(fd, runs, MAX_RUNS);
        T_EQ(n, 0, "a 16 MiB all-hole sparse file yields exactly 0 runs");
        T_OK(canary_intact(&runs[0]), "extent_read wrote nothing into the array for a sparse file");
        if (g_geom_ok) {
            struct maco *m = maco_create(g_geom.lbs);
            if (m) {
                int rc = exitos_extent_load_maco(fd, &g_geom, m);
                T_OK(rc >= 0, "load_maco on a sparse file is not an error (rc=%d)", rc);
                T_EQ((long long)maco_count(m), 0,
                     "holes are never mapped: 0 maco entries for a 16 MiB sparse file");
                T_EQ((long long)maco_mapped_bytes(m), 0, "sparse file maps 0 bytes");
                maco_destroy(m);
            }
        }
        close(fd);
    }
}

/* ------------------------------------------------------------------ */
/* 5. fragmented file: many runs, holes, and max_runs truncation       */
/* ------------------------------------------------------------------ */

#define FRAG_N      16
#define FRAG_CHUNK  (64ULL * 1024)
#define FRAG_STRIDE (1024ULL * 1024)
#define FRAG_MAPPED (FRAG_N * FRAG_CHUNK)                       /* 1 MiB      */
#define FRAG_SIZE   ((FRAG_N - 1) * FRAG_STRIDE + FRAG_CHUNK)   /* 15794176 B */

static void test_fragmented(void)
{
    struct extent_run runs[MAX_RUNS], few[8];
    char path[320];
    int fd, n, i, offs_ok = 1, lens_ok = 1, increasing = 1, has_gap = 0;
    uint64_t total = 0;

    fd = open_fixture_file("frag.log", path, sizeof(path));
    T_OK(fd >= 0, "created %s", path);
    if (fd < 0) return;

    for (i = 0; i < FRAG_N; i++)
        if (fill_range(fd, (uint64_t)i * FRAG_STRIDE, FRAG_CHUNK) != 0) break;
    T_EQ(i, FRAG_N, "wrote %d separate 64 KiB chunks at 1 MiB stride", FRAG_N);
    T_EQ(fsync(fd), 0, "fsync the fragmented file");

    canary_fill(runs, MAX_RUNS);
    n = exitos_extent_read(fd, runs, MAX_RUNS);
    T_EQ(n, FRAG_N, "16 chunks separated by holes yield exactly 16 runs");
    if (n <= 0) { close(fd); return; }

    for (i = 0; i < n && i < FRAG_N; i++) {
        if (runs[i].file_off != (uint64_t)i * FRAG_STRIDE) offs_ok = 0;
        if (runs[i].len != FRAG_CHUNK)                     lens_ok = 0;
        if (i > 0 && runs[i].file_off <= runs[i - 1].file_off) increasing = 0;
        if (i > 0 && runs[i].file_off > runs[i - 1].file_off + runs[i - 1].len) has_gap = 1;
        total += runs[i].len;
    }
    T_OK(offs_ok, "run i starts at file offset i * 1 MiB");
    T_OK(lens_ok, "every run is exactly 65536 bytes long");
    T_OK(increasing, "file_off values are strictly increasing across a fragmented file");
    T_OK(has_gap, "runs are NOT contiguous here: the holes between chunks are not mapped");
    T_EQ((long long)total, (long long)FRAG_MAPPED,
         "mapped length is 1048576 bytes, well under the 15794176-byte file size");

    /* truncation: caller's array is smaller than the extent count */
    canary_fill(few, 8);
    n = exitos_extent_read(fd, few, 4);
    T_EQ(n, 4, "max_runs=4 returns exactly 4 runs, not the full 16");
    T_OK(canary_intact(&few[4]), "extent_read did not write past max_runs");
    T_EQ((long long)few[0].file_off, 0, "the truncated read still starts at run 0");
    T_EQ((long long)few[3].file_off, (long long)(3 * FRAG_STRIDE),
         "the truncated read returns the first 4 runs in order");

    if (!g_geom_ok) {
        T_SKIP("geom probe failed, skipping maco load for frag.log");
        close(fd);
        return;
    }
    {
        struct maco *m = maco_create(g_geom.lbs);
        uint64_t lba = 0, contig = 0;
        int rc;
        T_OK(m != NULL, "maco_create for the fragmented load test");
        if (!m) { close(fd); return; }
        rc = exitos_extent_load_maco(fd, &g_geom, m);
        T_OK(rc >= 0, "load_maco of the fragmented file succeeds (rc=%d)", rc);
        T_EQ((long long)maco_count(m), FRAG_N,
             "16 disjoint runs cannot be merged: maco holds 16 entries");
        T_EQ((long long)maco_mapped_bytes(m), (long long)FRAG_MAPPED,
             "maco_mapped_bytes equals the mapped length, 1048576 bytes, not the file size");
        T_EQ(maco_lookup(m, 0, &lba, &contig), 0, "offset 0 is mapped");
        T_EQ((long long)contig, (long long)FRAG_CHUNK, "offset 0 has 65536 contiguous bytes");
        T_EQ(maco_lookup(m, FRAG_CHUNK + 4096, &lba, &contig), -ENOENT,
             "an offset inside the first hole is a miss, not a stale LBA");
        T_EQ(maco_lookup(m, FRAG_SIZE + 4096, &lba, &contig), -ENOENT,
             "an offset past the end of the file is a miss");
        maco_destroy(m);
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 6. delayed allocation: physical offset 0 must never escape          */
/* ------------------------------------------------------------------ */

static void test_delalloc(void)
{
    struct extent_run runs[MAX_RUNS];
    char path[320];
    int fd, n, i, clean = 1, phys_ok = 1;

    fd = open_fixture_file("delalloc.log", path, sizeof(path));
    T_OK(fd >= 0, "created %s", path);
    if (fd < 0) return;

    /* Deliberately NO fsync. On this fixture filefrag shows the extent as
     * unknown_loc,delalloc with physical offset 0 and length 0. */
    T_EQ(fill_range(fd, 0, 1024ULL * 1024), 0, "wrote 1 MiB and did not fsync");

    canary_fill(runs, MAX_RUNS);
    n = exitos_extent_read(fd, runs, MAX_RUNS);
    T_OK(n >= 0, "extent_read on an unsynced file does not error (got %d)", n);

    for (i = 0; i < n; i++) {
        if (runs[i].flags & BAD_LOCATION_FLAGS) clean = 0;
        if (runs[i].fs_block == 0 || runs[i].fs_block >= MAX_FS_BLOCK || runs[i].len == 0)
            phys_ok = 0;
    }
    T_OK(clean,
         "delayed-allocation extents are dropped (or synced first): no run carries "
         "UNKNOWN or DELALLOC");
    T_OK(phys_ok,
         "no returned run points at fs_block 0 - that block is the start of the "
         "filesystem, and a raw write there destroys the superblock");

    if (!g_geom_ok) {
        T_SKIP("geom probe failed, skipping maco load for delalloc.log");
        close(fd);
        return;
    }
    {
        struct maco *m = maco_create(g_geom.lbs);
        uint64_t lba = 0, contig = 0;
        int rc, looked;
        T_OK(m != NULL, "maco_create for the delalloc load test");
        if (!m) { close(fd); return; }
        rc = exitos_extent_load_maco(fd, &g_geom, m);
        T_OK(rc >= 0, "load_maco on an unsynced file is not an error (rc=%d)", rc);
        looked = maco_lookup(m, 0, &lba, &contig);
        if (looked == 0) {
            uint64_t base = 0;
            /* If anything was loaded at all, it must not point at the very
             * start of the filesystem. */
            T_OK(exitos_geom_fsblock_to_lba(&g_geom, 0, &base) != 0 || lba != base,
                 "whatever load_maco mapped at offset 0, it is not the filesystem's "
                 "first LBA (fs block 0)");
        } else {
            T_EQ(looked, -ENOENT, "nothing was mapped for the unsynced file");
        }
        maco_destroy(m);
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 7. CRITICAL SAFETY: writable_raw == 0 must load nothing             */
/* ------------------------------------------------------------------ */

static void refusal_case(int fd, exitos_dev_class cls, const char *part,
                         const char *disk, const char *why)
{
    struct exitos_geom bad;
    struct maco *m;
    uint64_t lba = 0, contig = 0;
    int rc;

    make_refusing_geom(&bad, cls, part, disk);
    m = maco_create(bad.lbs);
    T_OK(m != NULL, "maco_create for refusal case (%s)", why);
    if (!m) return;

    rc = exitos_extent_load_maco(fd, &bad, m);
    T_OK(rc < 0, "load_maco REFUSES when writable_raw==0 (%s): rc=%d is negative", why, rc);
    T_EQ(rc, -EPERM, "refusal returns -EPERM (%s), matching geom_fsblock_to_lba", why);
    T_EQ((long long)maco_count(m), 0,
         "load_maco loaded NOTHING (%s): maco_count()==0", why);
    T_EQ((long long)maco_mapped_bytes(m), 0,
         "load_maco mapped 0 bytes (%s)", why);
    T_EQ(maco_lookup(m, 0, &lba, &contig), -ENOENT,
         "no LBA is retrievable at offset 0 after refusal (%s)", why);
    maco_destroy(m);
}

static void test_load_maco_refusal(void)
{
    struct extent_run runs[MAX_RUNS];
    char path[320];
    int fd, n;

    fd = open_fixture_file("refuse.log", path, sizeof(path));
    T_OK(fd >= 0, "created %s", path);
    if (fd < 0) return;
    T_EQ(fallocate(fd, 0, 0, (off_t)(2ULL * 1024 * 1024)), 0, "fallocate 2 MiB for the refusal test");
    T_EQ(fill_range(fd, 0, 2ULL * 1024 * 1024), 0, "wrote 2 MiB");
    T_EQ(fsync(fd), 0, "fsync");

    /* Precondition: this file really does have loadable extents, so a later
     * maco_count()==0 proves refusal and not merely an empty file. */
    n = exitos_extent_read(fd, runs, MAX_RUNS);
    T_OK(n > 0, "precondition: refuse.log has %d loadable runs, so an empty maco "
                "afterwards can only mean the load was refused", n);

    refusal_case(fd, EXITOS_DEV_DM, "dm-0", "/dev/dm-0",
                 "device-mapper: fs blocks do not map linearly to the underlying disk");
    refusal_case(fd, EXITOS_DEV_MD, "md0", "/dev/md0",
                 "md RAID: fs blocks are striped across members");
    refusal_case(fd, EXITOS_DEV_UNKNOWN, "??", "/dev/unknown",
                 "unknown device class: translation cannot be validated");
    /* The gate is the writable_raw field itself, not the class. A struct that
     * claims a benign class but has writable_raw==0 must still be refused. */
    refusal_case(fd, EXITOS_DEV_OK, "nvme0n1p1", "/dev/nvme0n1",
                 "writable_raw==0 wins over a benign devclass");

    /* "Loads nothing" also means it does not disturb what is already there. */
    {
        struct exitos_geom bad;
        struct maco *m;
        uint64_t lba = 0, contig = 0;
        int rc;

        make_refusing_geom(&bad, EXITOS_DEV_DM, "dm-0", "/dev/dm-0");
        m = maco_create(bad.lbs);
        T_OK(m != NULL, "maco_create for the pre-populated refusal test");
        if (m) {
            T_EQ(maco_insert(m, 0, 0xdead000ULL, 4096), 0, "pre-seeded one entry into maco");
            rc = exitos_extent_load_maco(fd, &bad, m);
            T_OK(rc < 0, "load_maco still refuses on a non-empty maco (rc=%d)", rc);
            T_EQ((long long)maco_count(m), 1, "the pre-existing entry is neither added to nor cleared");
            T_EQ((long long)maco_mapped_bytes(m), 4096, "mapped bytes unchanged at 4096");
            T_EQ(maco_lookup(m, 0, &lba, &contig), 0, "the pre-existing entry still resolves");
            T_EQ((long long)lba, (long long)0xdead000ULL, "and it still holds the caller's LBA");
            maco_destroy(m);
        }
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 8. argument and error paths                                         */
/* ------------------------------------------------------------------ */

static void test_read_errors(void)
{
    struct extent_run runs[MAX_RUNS];
    char path[320];
    int fd, rc, pfd[2];

    fd = open_fixture_file("errs.log", path, sizeof(path));
    T_OK(fd >= 0, "created %s", path);
    if (fd < 0) return;
    T_EQ(fill_range(fd, 0, 1024ULL * 1024), 0, "wrote 1 MiB");
    T_EQ(fsync(fd), 0, "fsync");

    canary_fill(runs, MAX_RUNS);
    rc = exitos_extent_read(-1, runs, MAX_RUNS);
    T_OK(rc < 0, "extent_read on fd -1 returns a negative errno (got %d)", rc);
    T_OK(canary_intact(&runs[0]), "extent_read wrote nothing on the fd -1 path");

    rc = exitos_extent_read(fd, NULL, MAX_RUNS);
    T_OK(rc < 0, "extent_read with a NULL run array returns a negative errno (got %d)", rc);
    T_EQ(rc, -EINVAL, "NULL run array is -EINVAL");

    canary_fill(runs, MAX_RUNS);
    rc = exitos_extent_read(fd, runs, 0);
    T_OK(rc == 0 || rc == -EINVAL, "max_runs=0 returns 0 or -EINVAL, never a positive count (got %d)", rc);
    T_OK(canary_intact(&runs[0]), "max_runs=0 writes nothing into the array");

    canary_fill(runs, MAX_RUNS);
    rc = exitos_extent_read(fd, runs, -1);
    T_OK(rc < 0, "negative max_runs returns a negative errno (got %d)", rc);
    T_EQ(rc, -EINVAL, "negative max_runs is -EINVAL");
    T_OK(canary_intact(&runs[0]), "negative max_runs writes nothing into the array");

    /* A file descriptor that is not on a filesystem at all: FIEMAP is not a
     * valid ioctl there, and the error must be reported, not swallowed. */
    T_EQ(pipe(pfd), 0, "created a pipe");
    canary_fill(runs, MAX_RUNS);
    rc = exitos_extent_read(pfd[0], runs, MAX_RUNS);
    T_OK(rc < 0, "extent_read on a pipe fd returns a negative errno (got %d)", rc);
    T_OK(canary_intact(&runs[0]), "extent_read wrote nothing for the pipe fd");
    close(pfd[0]);
    close(pfd[1]);

    /* A closed descriptor. */
    {
        int dead = open(path, O_RDONLY);
        T_OK(dead >= 0, "reopened %s to close it", path);
        if (dead >= 0) {
            close(dead);
            rc = exitos_extent_read(dead, runs, MAX_RUNS);
            T_OK(rc < 0, "extent_read on a closed fd returns a negative errno (got %d)", rc);
        }
    }
    close(fd);
}

static void test_load_maco_bad_args(void)
{
    char path[320];
    struct exitos_geom good;
    struct maco *m;
    int fd, rc;

    fd = open_fixture_file("badargs.log", path, sizeof(path));
    T_OK(fd >= 0, "created %s", path);
    if (fd < 0) return;
    T_EQ(fill_range(fd, 0, 512ULL * 1024), 0, "wrote 512 KiB");
    T_EQ(fsync(fd), 0, "fsync");

    m = maco_create(512);
    T_OK(m != NULL, "maco_create for the bad-argument tests");
    if (!m) { close(fd); return; }

    rc = exitos_extent_load_maco(fd, NULL, m);
    T_OK(rc < 0, "load_maco with a NULL geom returns a negative errno (got %d)", rc);
    T_EQ((long long)maco_count(m), 0, "NULL geom is not treated as permission to write: nothing loaded");

    if (g_geom_ok) {
        rc = exitos_extent_load_maco(fd, &g_geom, NULL);
        T_OK(rc < 0, "load_maco with a NULL maco returns a negative errno (got %d)", rc);

        rc = exitos_extent_load_maco(-1, &g_geom, m);
        T_OK(rc < 0, "load_maco with fd -1 returns a negative errno (got %d)", rc);
        T_EQ((long long)maco_count(m), 0, "a bad fd loads nothing");
    } else {
        T_SKIP("geom probe failed, skipping the NULL-maco and bad-fd load cases");
    }

    /* Degenerate geometry with the write gate open: a zero block size would
     * divide by zero, a zero logical block size would produce nonsense LBAs.
     * Either reject it or load nothing; never load a guessed address. */
    memset(&good, 0, sizeof(good));
    snprintf(good.part_name, sizeof(good.part_name), "loop0");
    snprintf(good.disk_path, sizeof(good.disk_path), "/dev/loop0");
    good.part_start_sect = 0;
    good.lbs             = 512;
    good.fs_bs           = 0;              /* degenerate */
    good.devclass        = EXITOS_DEV_LOOP;
    good.writable_raw    = 1;
    maco_clear(m);
    rc = exitos_extent_load_maco(fd, &good, m);
    T_OK(rc < 0 || maco_count(m) == 0,
         "fs_bs==0 geometry: load_maco rejects it or loads nothing, never garbage LBAs (rc=%d, count=%zu)",
         rc, maco_count(m));

    good.fs_bs = 4096;
    good.lbs   = 0;                        /* degenerate */
    maco_clear(m);
    rc = exitos_extent_load_maco(fd, &good, m);
    T_OK(rc < 0 || maco_count(m) == 0,
         "lbs==0 geometry: load_maco rejects it or loads nothing (rc=%d, count=%zu)",
         rc, maco_count(m));

    maco_destroy(m);
    close(fd);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    if (!t_need_root()) {
        T_SKIP("extent tier-1 tests need root to create a loop device and mount ext4");
        T_DONE();
    }
    atexit(fixture_down);
    if (fixture_up() != 0) {
        T_SKIP("could not bring up the loop fixture (loopfix.sh up failed); "
               "no loop device or ext4 tools available");
        T_DONE();
    }
    printf("# fixture: img=%s mnt=%s loop=%s\n", g_img, g_mnt, g_loop);

    test_geom_probe();
    test_written();
    test_prealloc_unwritten();
    test_sparse_and_empty();
    test_fragmented();
    test_delalloc();
    test_load_maco_refusal();
    test_read_errors();
    test_load_maco_bad_args();

    T_DONE();
}
