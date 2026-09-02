/* Tier-1 regression tests for src/intercept.c: the MAPPING-REFRESH path and the
 * MIXED-PATH GUARD. Both defects below were found by hand and had no test, so
 * nothing stopped them from coming back. Everything runs on an ext4 image on a
 * LOOP device whose name carries this process's pid; never a physical disk.
 *
 * WHY STATISTICS AND NOT CHECKSUMS
 * --------------------------------
 * include/exitos_stats.h says it outright: "A correct checksum alone cannot
 * distinguish 'the fast path ran and was right' from 'the fast path never ran
 * at all'". Defect 1 below was invisible for exactly that reason -- every byte
 * was correct because the KERNEL had written it. So every claim here is made
 * against EXITOS_STAT_FAST_WRITE / FAST_SYNC / PASS deltas and against the
 * decision value, never against file contents alone.
 *
 * DEFECT 1 (regression): the layout was captured once, at exitos_register_fd().
 * A writer that opens the file and only then preallocates it -- the normal
 * order -- left the snapshot empty, so every write missed the map and was
 * passed to the kernel forever. src/intercept.c:651-658 now states the fix:
 * "A miss is not proof the offset is unmapped. The layout is captured at
 * open(), and an application may preallocate AFTER opening ... Re-read once and
 * retry". Defended by group 1.
 *
 * DEFECT 2 (regression): the refresh at first reloaded only the Maco and not
 * the bounds table, so iv_covers() -- which refuses when the table is empty --
 * rejected every write. src/intercept.c:609-613: "Both halves must be reloaded
 * together ... Refreshing only the Maco therefore produced a mapping that the
 * guard then rejected on every write". Defended by group 2, and by group 3
 * which checks the bounds table directly through exitos_lba_in_bounds().
 *
 * THE SAFETY PROPERTY THE WHOLE THING RESTS ON: an extent that is allocated but
 * never written must NOT be taken over. src/extent.c: "ext4 reads such a range
 * back as zeros until a normal write converts the extent, so a raw LBA write
 * there would land on the platter and be invisible through the filesystem."
 * A refresh that ignored this would turn defect 1's fix into silent data loss.
 * Defended by group 3, which drives the same offset across the transition:
 * refused while unwritten, taken over once initialised, with the two halves of
 * one file on opposite sides of the line at the same instant.
 *
 * MIXED-PATH GUARD: exitos_fd_all_writes_took_fast_path() reports whether the
 * filesystem/kernel currently holds an unsynced fallback write. A fallback
 * makes it false; only a frontend that observes a successful real sync may call
 * exitos_note_kernel_sync() and make it true again. Defended by groups 4/5;
 * these direct-core tests deliberately do not report the real syscall result.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

#include "exitos_intercept.h"
#include "exitos_stats.h"
#include "tap.h"

#define RES_SENTINEL   ((ssize_t)-424242)
#define IRES_SENTINEL  (-4242)
#define MAX_RUNS       64
#define BLK            4096u        /* one ext4 block: every write is one of these */
#define NBLK           32u          /* test files are 32 blocks = 128 KiB          */
/* Mirrors EXITOS_REFRESH_EVERY in src/intercept.c. Not exported by the header,
 * so it is restated here; group 6 measures the real value and would catch a
 * divergence rather than silently assuming this one. */
#define REFRESH_EVERY  8
/* The gap between re-reads of the layout doubles after each look that finds
 * nothing usable, capped, and is reset by one that does -- see
 * EXITOS_REFRESH_MAX_GAP in src/intercept.c and why it had to be that way. So
 * "eventually picked up" now takes more writes than the old fixed gap of 8:
 * 8+16+32+64+128+256+512 covers seven backed-off looks, which is well past the
 * point where a real change is noticed. Ten attempts only ever passed because
 * the gap used to be constant. */
#define TRIES 1024

struct orun { uint64_t off, phys, len; uint32_t flags; };

static char g_img[256], g_mnt[256], g_loop[64];
static int  g_up;
static int  g_fds[8];
static int  g_nfds;

static void track(int fd) { if (g_nfds < 8) g_fds[g_nfds++] = fd; }

/* --------------------------------------------------------------- fixture */

static void teardown(void)
{
    char cmd[2048];
    int rc, n, i;

    /* Close first: an fd still open on the image keeps umount busy and the
     * loop device attached. Runs on every exit path, failures included. */
    for (i = 0; i < g_nfds; i++)
        if (g_fds[i] >= 0) { close(g_fds[i]); g_fds[i] = -1; }
    if (!g_up) return;
    g_up = 0;
    /* Torn down by hand rather than "loopfix.sh down": that path reads the
     * shared /tmp/exitos-loop.dev, which a concurrently running test
     * may have overwritten. Only our own loop device is detached. */
    n = snprintf(cmd, sizeof cmd,
                 "umount %s 2>/dev/null || umount -l %s 2>/dev/null; "
                 "losetup -d %s 2>/dev/null; "
                 "rm -f %s; rmdir %s 2>/dev/null; true",
                 g_mnt, g_mnt, g_loop, g_img, g_mnt);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        printf("# teardown: command too long; tear down by hand: "
               "umount %s; losetup -d %s; rm -f %s\n", g_mnt, g_loop, g_img);
        return;
    }
    rc = system(cmd);
    (void)rc;
}

static const char *loopfix_path(void)
{
    static const char *cands[] = {
        "tests/harness/loopfix.sh",
        "./tests/harness/loopfix.sh",
        "../harness/loopfix.sh",
        "../../tests/harness/loopfix.sh",
    };
    const char *env = getenv("EXITOS_LOOPFIX");
    unsigned i;

    if (env && access(env, X_OK) == 0) return env;
    for (i = 0; i < sizeof cands / sizeof cands[0]; i++)
        if (access(cands[i], X_OK) == 0) return cands[i];
    return NULL;
}

static int fixture_up(void)
{
    const char *fix = loopfix_path();
    char cmd[2048], line[512];
    FILE *f;
    int n;

    if (!fix) return -1;
    snprintf(g_img, sizeof g_img, "/tmp/exitos-mapref-%d.img", (int)getpid());
    snprintf(g_mnt, sizeof g_mnt, "/tmp/exitos-mnt-mapref-%d", (int)getpid());
    n = snprintf(cmd, sizeof cmd,
                 "EXITOS_IMG=%s EXITOS_MNT=%s EXITOS_SIZE_MB=64 %s up 2>&1",
                 g_img, g_mnt, fix);
    if (n < 0 || (size_t)n >= sizeof cmd) return -1;
    f = popen(cmd, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "LOOP=", 5) == 0) sscanf(line, "LOOP=%63s", g_loop);
        else printf("# loopfix: %s", line);
    }
    pclose(f);
    if (g_loop[0] == '\0') return -1;
    g_up = 1;
    atexit(teardown);
    return 0;
}

/* --------------------------------------------------------------- oracles */

/* Name of the block device backing fd's filesystem, e.g. "loop3". */
static int backing_device(int fd, char *name, size_t n)
{
    struct stat st;
    char link[256], target[512];
    const char *base;
    ssize_t k;

    if (fstat(fd, &st) != 0) return -1;
    if (major(st.st_dev) == 0) return -1;
    snprintf(link, sizeof link, "/sys/dev/block/%u:%u",
             major(st.st_dev), minor(st.st_dev));
    k = readlink(link, target, sizeof target - 1);
    if (k <= 0) return -1;
    target[k] = '\0';
    base = strrchr(target, '/');
    base = base ? base + 1 : target;
    snprintf(name, n, "%.*s", (int)(n - 1), base);
    return 0;
}

static uint32_t device_lbs(const char *devname)
{
    char path[288];
    int fd, ss = 0;

    snprintf(path, sizeof path, "/dev/%s", devname);
    fd = open(path, O_RDONLY);          /* read-only, and only ever a loop dev */
    if (fd < 0) return 0;
    if (ioctl(fd, BLKSSZGET, &ss) != 0) ss = 0;
    close(fd);
    return (uint32_t)ss;
}

static uint64_t device_part_start(const char *devname)
{
    char path[320], buf[64];
    uint64_t v = 0;
    FILE *f;

    snprintf(path, sizeof path, "/sys/class/block/%s/start", devname);
    f = fopen(path, "r");
    if (!f) return 0;
    if (fgets(buf, sizeof buf, f)) v = strtoull(buf, NULL, 10);
    fclose(f);
    return v;
}

static uint64_t g_lbs, g_part_start_sect, g_fs_bs;

/* The expected LBA, computed straight from FIEMAP and the device geometry, so
 * this test is an oracle rather than a mirror of the implementation. */
static uint64_t lba_of(uint64_t phys_bytes)
{
    return g_part_start_sect * 512 / g_lbs + phys_bytes / g_lbs;
}

static int fiemap_runs(int fd, struct orun *out, int max)
{
    size_t sz = sizeof(struct fiemap) + MAX_RUNS * sizeof(struct fiemap_extent);
    struct fiemap *fm = calloc(1, sz);
    int n, i;

    if (!fm) return -ENOMEM;
    fm->fm_start = 0;
    fm->fm_length = ~0ULL;
    fm->fm_flags = FIEMAP_FLAG_SYNC;
    fm->fm_extent_count = MAX_RUNS;
    if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0) { int e = -errno; free(fm); return e; }
    n = (int)fm->fm_mapped_extents;
    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        out[i].off   = fm->fm_extents[i].fe_logical;
        out[i].phys  = fm->fm_extents[i].fe_physical;
        out[i].len   = fm->fm_extents[i].fe_length;
        out[i].flags = fm->fm_extents[i].fe_flags;
    }
    free(fm);
    return n;
}

/* LBA of file offset `off`, plus whether the extent holding it is UNWRITTEN. */
static int oracle_at(const struct orun *r, int n, uint64_t off,
                     uint64_t *lba, int *unwritten)
{
    int i;

    for (i = 0; i < n; i++) {
        if (off >= r[i].off && off < r[i].off + r[i].len) {
            *lba = lba_of(r[i].phys + (off - r[i].off));
            *unwritten = (r[i].flags & FIEMAP_EXTENT_UNWRITTEN) ? 1 : 0;
            return 1;
        }
    }
    return 0;
}

static int all_unwritten(const struct orun *r, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!(r[i].flags & FIEMAP_EXTENT_UNWRITTEN)) return 0;
    return n > 0;
}

/* --------------------------------------------------------------- driver */

static int kwrite(int fd, const void *b, size_t n, off_t off)
{
    if (pwrite(fd, b, n, off) != (ssize_t)n) return -1;
    /* fdatasync so the unwritten->written extent conversion really happens;
     * a log writer of this shape syncs after every record anyway. */
    return fdatasync(fd);
}

/* One write as a real caller issues it: ask the module, and when it declines,
 * reissue the same bytes through the kernel -- which is what converts an
 * unwritten extent and therefore what makes the next refresh useful. */
static exitos_decision drive(struct exitos_ctx *c, int fd, const void *b,
                             size_t n, off_t off, ssize_t *res)
{
    exitos_decision d;

    *res = RES_SENTINEL;
    d = exitos_on_write(c, fd, b, n, off, res);
    if (d == EXITOS_PASS) {
        (void)kwrite(fd, b, n, off);
        /* A log record's durability point follows every write.  The
         * frontend notes a successful real fdatasync; without this, a
         * buffered registration's kernel-debt gate keeps the shortcut off
         * after the first kernel-path write, which is exactly its job. */
        if (fdatasync(fd) == 0)
            exitos_note_kernel_sync(c, fd);
    }
    return d;
}

/* Attempt number (1-based) at which the fast path engaged, or 0 within `tries`. */
static int until_takeover(struct exitos_ctx *c, int fd, const void *b,
                          size_t n, off_t off, int tries)
{
    ssize_t res;
    int k;

    for (k = 1; k <= tries; k++)
        if (drive(c, fd, b, n, off, &res) == EXITOS_TAKEOVER)
            return k;
    return 0;
}

static int make_written_file(const char *path, const void *b, unsigned nblk)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    unsigned i;

    if (fd < 0) return -1;
    for (i = 0; i < nblk; i++)
        if (pwrite(fd, b, BLK, (off_t)i * BLK) != (ssize_t)BLK) { close(fd); return -1; }
    if (fsync(fd) != 0) { close(fd); return -1; }
    return fd;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    struct exitos_ctx *c = NULL;
    char pathA[320], pathB[320], pathC[320], pathD[320], pathF[320], devname[64];
    int fdA = -1, fdB = -1, fdC = -1, fdD = -1, fdF = -1;
    struct orun ra[MAX_RUNS], rb[MAX_RUNS];
    struct statfs sfs;
    char *wbuf = NULL, *zbuf = NULL;
    uint64_t f0, f1, s0, p0, p1;
    ssize_t res;
    int ires, rc, n, i, k;
    exitos_decision d;

    if (!t_need_root()) {
        T_SKIP("not root: the loop-device fixture needs root");
        T_DONE();
    }
    if (fixture_up() != 0) {
        T_SKIP("could not bring up the ext4-on-loop fixture "
               "(loopfix.sh missing, or losetup/mkfs.ext4 failed)");
        T_DONE();
    }
    printf("# fixture: loop=%s img=%s mnt=%s\n", g_loop, g_img, g_mnt);

    if (posix_memalign((void **)&wbuf, BLK, BLK) != 0 || !wbuf ||
        posix_memalign((void **)&zbuf, BLK, BLK) != 0 || !zbuf) {
        T_SKIP("posix_memalign failed");
        teardown();
        T_DONE();
    }
    memset(wbuf, 0xA5, BLK);
    memcpy(wbuf, "EXITOS-REFRESH", 14);
    memset(zbuf, 0x00, BLK);

    snprintf(pathA, sizeof pathA, "%s/logA", g_mnt);
    snprintf(pathB, sizeof pathB, "%s/logB", g_mnt);
    snprintf(pathC, sizeof pathC, "%s/logC", g_mnt);
    snprintf(pathD, sizeof pathD, "%s/logD", g_mnt);
    snprintf(pathF, sizeof pathF, "%s/logF", g_mnt);

    /* ---- HARD SAFETY GATE: nothing is registered off a loop device ------ */
    fdA = open(pathA, O_RDWR | O_CREAT | O_TRUNC, 0600);
    track(fdA);
    T_OK(fdA >= 0, "fixture: created logA on the loop filesystem");
    if (fdA < 0) { teardown(); T_DONE(); }
    if (backing_device(fdA, devname, sizeof devname) != 0 ||
        strncmp(devname, "loop", 4) != 0) {
        T_SKIP("logA is backed by '%s', not a loop device - refusing to register "
               "anything (that would arm a raw-LBA write path on real media)",
               devname[0] ? devname : "?");
        teardown();
        T_DONE();
    }
    T_OK(1, "safety gate: test files are backed by loop device %s", devname);

    g_lbs = device_lbs(devname);
    g_part_start_sect = device_part_start(devname);
    if (statfs(pathA, &sfs) == 0 && sfs.f_bsize > 0) g_fs_bs = (uint64_t)sfs.f_bsize;
    T_OK(g_lbs == 512 || g_lbs == 4096,
         "oracle: device logical block size is %llu bytes", (unsigned long long)g_lbs);
    T_EQ(g_fs_bs, 4096, "oracle: ext4 block size is 4096 bytes");
    if (!g_lbs || !g_fs_bs) { teardown(); T_DONE(); }

    c = exitos_ctx_create();
    T_OK(c != NULL, "exitos_ctx_create returns a context");
    if (!c) { teardown(); T_DONE(); }

    /* ================================================================== *
     * GROUP 1 - DEFECT 1. The layout snapshot is taken at registration.
     * Property defended: a file that gains its extents AFTER being
     * registered must still reach the fast path. This matters because the
     * failure is invisible: every byte stays correct (the kernel wrote it)
     * while the fast path never runs once, so only the counters can tell
     * the two apart. The order below -- open, register, fallocate, write --
     * is the order a log writer actually uses.
     * ================================================================== */
    rc = exitos_register_fd(c, fdA);
    T_EQ(rc, 0, "G1: registering an EMPTY file succeeds (snapshot maps nothing)");
    if (rc != 0) {
        T_SKIP("logA not registered; the refresh tests cannot run");
        exitos_ctx_destroy(c); teardown(); T_DONE();
    }

    f0 = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdA, wbuf, BLK, 0, &res);
    T_EQ(d, EXITOS_PASS, "G1: before fallocate, a write on the empty file PASSes");
    T_EQ(res, RES_SENTINEL, "G1: ... and *result is left untouched");
    T_EQ(exitos_stat_get(EXITOS_STAT_FAST_WRITE) - f0, 0,
         "G1: ... and FAST_WRITE did not move");
    (void)kwrite(fdA, wbuf, BLK, 0);

    T_EQ(fallocate(fdA, 0, 0, (off_t)(NBLK * BLK)), 0,
         "G1: fallocate %u KiB AFTER registration", (NBLK * BLK) >> 10);
    T_EQ(fsync(fdA), 0, "G1: fsync after fallocate");

    /* Phase 1: initialise the preallocated range. Every one of these writes
     * must be handed to the kernel: the extents are UNWRITTEN, and a raw write
     * into one would be read back as zeros through the filesystem. */
    f0 = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
    p0 = exitos_stat_get(EXITOS_STAT_PASS);
    k = 0;
    for (i = 0; i < (int)NBLK; i++)
        if (drive(c, fdA, zbuf, BLK, (off_t)i * BLK, &res) == EXITOS_TAKEOVER) k++;
    f1 = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
    p1 = exitos_stat_get(EXITOS_STAT_PASS);
    T_EQ(k, 0, "G1: no first-touch write into a fallocated (UNWRITTEN) block is "
               "taken over");
    T_EQ(f1 - f0, 0, "G1: ... evidence: FAST_WRITE did not move across %u "
                     "initialising writes", NBLK);
    T_EQ(p1 - p0, NBLK, "G1: ... and all %u went to the kernel (PASS)", NBLK);

    /* Phase 2: the same blocks, now initialised, must reach the fast path.
     * Nothing but a re-read of the layout can make this happen: the snapshot
     * taken at exitos_register_fd() was of an empty file.
     * This is a buffered registration, so model the application's
     * durability point first -- a real fdatasync plus the note the frontend
     * sends after it -- or the kernel-debt gate keeps the shortcut off. */
    T_EQ(fdatasync(fdA), 0, "G1: durability point after initialisation");
    exitos_note_kernel_sync(c, fdA);
    f0 = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
    k = until_takeover(c, fdA, wbuf, BLK, 0, TRIES);
    T_OK(k > 0, "G1/DEFECT 1: after a post-registration fallocate + initialise, "
                "the fast path ENGAGES (attempt %d of %d)", k, TRIES);
    T_EQ(exitos_stat_get(EXITOS_STAT_FAST_WRITE) - f0, k > 0 ? 1 : 0,
         "G1: ... and FAST_WRITE recorded exactly that one takeover");
    T_EQ(k, 1, "G1: the FIRST rewrite of an initialised block is taken over "
               "(the layout was already re-read during initialisation)");

    /* A takeover must mean the bytes are at that LBA, not merely a claim.
     * Read through O_DIRECT: src/intercept.c warns that a buffered read of a
     * taken-over range can still be served from a stale cached page. */
    if (k > 0) {
        int rfd = open(pathA, O_RDONLY | O_DIRECT);
        if (rfd < 0) {
            T_SKIP("O_DIRECT open failed (%s): cannot verify the bytes landed",
                   strerror(errno));
        } else {
            char *rbuf = NULL;
            if (posix_memalign((void **)&rbuf, BLK, BLK) != 0 || !rbuf) {
                T_SKIP("posix_memalign for the readback buffer failed");
            } else {
                T_EQ(pread(rfd, rbuf, BLK, 0), (ssize_t)BLK, "G1: readback of block 0");
                T_EQ(memcmp(rbuf, wbuf, BLK), 0,
                     "G1: the taken-over write really landed at the file's LBA");
                free(rbuf);
            }
            close(rfd);
        }
    }

    /* A refreshed map must keep refusing what an unrefreshed one refuses. */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdA, wbuf, 2 * BLK, (off_t)(NBLK - 1) * BLK, &res);
    T_EQ(d, EXITOS_PASS, "G1: a write straddling EOF still PASSes after a refresh");
    T_EQ(res, RES_SENTINEL, "G1: ... and *result is left untouched");

    /* ================================================================== *
     * GROUP 2 - DEFECT 2. The refresh must reload the BOUNDS TABLE, not
     * only the offset map. When it reloaded only the map, iv_covers() saw
     * an empty table and refused every write -- the guard was right and the
     * fast path was useless. The LBAs below come from FIEMAP plus the
     * device geometry, computed independently of the module.
     * ================================================================== */
    n = fiemap_runs(fdA, ra, MAX_RUNS);
    T_OK(n >= 1, "G2: oracle: FIEMAP reports %d extent(s) for logA", n);
    if (n >= 1) {
        uint64_t lo = UINT64_MAX, hi = 0;
        for (i = 0; i < n; i++) {
            uint64_t a = lba_of(ra[i].phys), b = lba_of(ra[i].phys + ra[i].len);
            if (a < lo) lo = a;
            if (b > hi) hi = b;
            printf("# logA run %d: off=%llu phys=%llu len=%llu flags=0x%x\n", i,
                   (unsigned long long)ra[i].off, (unsigned long long)ra[i].phys,
                   (unsigned long long)ra[i].len, ra[i].flags);
        }
        T_EQ(exitos_lba_in_bounds(c, fdA, lo, BLK), 1,
             "G2/DEFECT 2: after the refresh the bounds table holds the file's "
             "own first LBA %llu", (unsigned long long)lo);
        T_EQ(exitos_lba_in_bounds(c, fdA, hi, BLK), 0,
             "G2: one block past the file's last LBA is still refused");
        if (lo > 0)
            T_EQ(exitos_lba_in_bounds(c, fdA, lo - 1, BLK), 0,
                 "G2: one block below the file's first LBA is still refused");
        T_EQ(exitos_lba_in_bounds(c, fdA, 0, BLK), 0,
             "G2: LBA 0 (the filesystem superblock) is refused");
    }

    /* The other half of defect 2, stated as the module states it: a takeover
     * is only possible when BOTH structures were reloaded, so a fast write is
     * itself proof the bounds table was rebuilt. */
    T_OK(exitos_stat_get(EXITOS_STAT_FAST_WRITE) > 0,
         "G2: a takeover happened at all, which iv_covers() cannot allow with an "
         "empty bounds table");

    /* ================================================================== *
     * GROUP 3 - the property that makes the refresh safe. An extent that is
     * allocated but never written must NOT be taken over: src/extent.c says
     * ext4 reads such a range back as zeros until a normal write converts
     * it, so a raw write there is invisible through the filesystem -- data
     * loss that no checksum of the file would ever reveal. Both states are
     * exercised on ONE file at the SAME instant: half initialised, half not.
     * ================================================================== */
    fdB = open(pathB, O_RDWR | O_CREAT | O_TRUNC, 0600);
    track(fdB);
    T_OK(fdB >= 0, "G3: created logB");
    if (fdB < 0) { exitos_ctx_destroy(c); teardown(); T_DONE(); }
    T_EQ(fallocate(fdB, 0, 0, (off_t)(NBLK * BLK)), 0,
         "G3: fallocate logB, write nothing");
    T_EQ(fsync(fdB), 0, "G3: fsync logB");

    n = fiemap_runs(fdB, rb, MAX_RUNS);
    T_OK(n >= 1 && all_unwritten(rb, n),
         "G3: oracle: all %d extent(s) of logB carry FIEMAP_EXTENT_UNWRITTEN", n);
    {
        uint64_t lba_head = 0, lba_tail = 0;
        int uw_head = 0, uw_tail = 0, got_h, got_t;
        off_t tail_off = (off_t)(NBLK / 2) * BLK;

        got_h = oracle_at(rb, n, 0, &lba_head, &uw_head);
        got_t = oracle_at(rb, n, (uint64_t)tail_off, &lba_tail, &uw_tail);
        T_OK(got_h && got_t, "G3: oracle: logB offsets 0 and %lld map to LBAs "
             "%llu and %llu", (long long)tail_off,
             (unsigned long long)lba_head, (unsigned long long)lba_tail);

        rc = exitos_register_fd(c, fdB);
        T_EQ(rc, 0, "G3: registering a fully preallocated, never-written file "
                    "succeeds");

        T_EQ(exitos_lba_in_bounds(c, fdB, lba_head, BLK), 0,
             "G3: an allocated-but-never-written extent is NOT in the file's "
             "writable footprint");

        f0 = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
        res = RES_SENTINEL;
        d = exitos_on_write(c, fdB, wbuf, BLK, 0, &res);
        T_EQ(d, EXITOS_PASS,
             "G3: a write into a freshly fallocated (UNWRITTEN) range is NOT "
             "taken over");
        T_EQ(res, RES_SENTINEL, "G3: ... and *result is left untouched");
        T_EQ(exitos_stat_get(EXITOS_STAT_FAST_WRITE) - f0, 0,
             "G3: ... and FAST_WRITE did not move");
        (void)kwrite(fdB, wbuf, BLK, 0);

        /* Initialise ONLY the first half through the kernel. */
        for (i = 1; i < (int)NBLK / 2; i++)
            (void)kwrite(fdB, zbuf, BLK, (off_t)i * BLK);

        f0 = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
        k = until_takeover(c, fdB, wbuf, BLK, 0, TRIES);
        T_OK(k > 0, "G3: once the range is initialised the SAME offset IS taken "
                    "over (attempt %d of %d)", k, TRIES);
        T_EQ(exitos_stat_get(EXITOS_STAT_FAST_WRITE) - f0, k > 0 ? 1 : 0,
             "G3: ... evidence: FAST_WRITE moved by exactly one");

        /* Same file, same instant: the uninitialised half must still be out. */
        n = fiemap_runs(fdB, rb, MAX_RUNS);
        got_h = oracle_at(rb, n, 0, &lba_head, &uw_head);
        got_t = oracle_at(rb, n, (uint64_t)tail_off, &lba_tail, &uw_tail);
        T_OK(got_h && !uw_head, "G3: oracle: logB's initialised half is no longer "
                                "UNWRITTEN");
        T_OK(got_t && uw_tail, "G3: oracle: logB's second half is still UNWRITTEN");
        T_EQ(exitos_lba_in_bounds(c, fdB, lba_head, BLK), 1,
             "G3: the initialised half IS in the bounds table");
        T_EQ(exitos_lba_in_bounds(c, fdB, lba_tail, BLK), 0,
             "G3: the still-uninitialised half is NOT in the bounds table, even "
             "though the refresh that added the first half saw both");

        f0 = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
        res = RES_SENTINEL;
        d = exitos_on_write(c, fdB, wbuf, BLK, tail_off, &res);
        T_EQ(d, EXITOS_PASS, "G3: a write into the still-UNWRITTEN half PASSes");
        T_EQ(res, RES_SENTINEL, "G3: ... and *result is left untouched");
        T_EQ(exitos_stat_get(EXITOS_STAT_FAST_WRITE) - f0, 0,
             "G3: ... and FAST_WRITE did not move");
        (void)kwrite(fdB, wbuf, BLK, tail_off);

        /* Initialise the second half too; now it must become takeover-able. */
        for (i = (int)NBLK / 2 + 1; i < (int)NBLK; i++)
            (void)kwrite(fdB, zbuf, BLK, (off_t)i * BLK);
        k = until_takeover(c, fdB, wbuf, BLK, tail_off, TRIES);
        T_OK(k > 0, "G3: after the second half is initialised it IS taken over "
                    "(attempt %d of %d)", k, TRIES);
        T_EQ(exitos_lba_in_bounds(c, fdB, lba_tail, BLK), 1,
             "G3: ... and its LBA is now inside the bounds table");
    }

    /* ================================================================== *
     * GROUP 4 - the mixed-path guard. exitos_on_fdatasync must not answer
     * the call when any write on that fd went to the kernel, because only
     * the kernel's own fdatasync persists that half. Two fds are built so
     * that "always takeover" and "always pass" both fail: one where every
     * write was taken over, one where a single write was not.
     * ================================================================== */
    fdC = make_written_file(pathC, zbuf, NBLK);
    fdD = make_written_file(pathD, zbuf, NBLK);
    track(fdC); track(fdD);
    T_OK(fdC >= 0 && fdD >= 0, "G4: created logC and logD, fully written and "
                               "fsynced before registration");
    if (fdC < 0 || fdD < 0) { exitos_ctx_destroy(c); teardown(); T_DONE(); }
    T_EQ(exitos_register_fd(c, fdC), 0, "G4: registered logC");
    T_EQ(exitos_register_fd(c, fdD), 0, "G4: registered logD");

    /* --- fd where every write took the fast path --- */
    f0 = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
    k = 0;
    for (i = 0; i < 4; i++) {
        res = RES_SENTINEL;
        if (exitos_on_write(c, fdC, wbuf, BLK, (off_t)i * BLK, &res) == EXITOS_TAKEOVER
            && res == (ssize_t)BLK)
            k++;
    }
    T_EQ(k, 4, "G4: logC: all 4 writes taken over");
    T_EQ(exitos_stat_get(EXITOS_STAT_FAST_WRITE) - f0, 4,
         "G4: ... evidence: FAST_WRITE moved by 4");
    T_EQ(exitos_fd_all_writes_took_fast_path(c, fdC), 1,
         "G4: the guard reports 1 for an fd whose every write took the fast path");

    s0 = exitos_stat_get(EXITOS_STAT_FAST_SYNC);
    ires = IRES_SENTINEL;
    d = exitos_on_fdatasync(c, fdC, &ires);
    T_EQ(d, EXITOS_TAKEOVER, "G4: fdatasync on that fd is taken over");
    T_EQ(ires, 0, "G4: ... and *result is set to 0");
    T_EQ(exitos_stat_get(EXITOS_STAT_FAST_SYNC) - s0, 1,
         "G4: ... evidence: FAST_SYNC moved by 1");

    /* --- fd where exactly one write went to the kernel --- */
    res = RES_SENTINEL;
    T_EQ(exitos_on_write(c, fdD, wbuf, BLK, 0, &res), EXITOS_TAKEOVER,
         "G4: logD control: an aligned write inside a mapped run is taken over");
    T_EQ(exitos_fd_all_writes_took_fast_path(c, fdD), 1,
         "G4: logD: the guard still reports 1 before any write is passed");

    p0 = exitos_stat_get(EXITOS_STAT_PASS);
    res = RES_SENTINEL;
    /* A buffer that is not aligned for O_DIRECT: refused by design, and the
     * caller then reissues it through the kernel, which now holds those bytes. */
    d = exitos_on_write(c, fdD, wbuf + 1, BLK, (off_t)BLK, &res);
    T_EQ(d, EXITOS_PASS, "G4: logD: a write from an unaligned buffer PASSes");
    T_EQ(exitos_stat_get(EXITOS_STAT_PASS) - p0, 1,
         "G4: ... evidence: PASS moved by 1");
    T_EQ(pwrite(fdD, wbuf + 1, BLK, (off_t)BLK), (ssize_t)BLK,
         "G4: ... and the caller reissues it, so the kernel now holds that data");

    T_EQ(exitos_fd_all_writes_took_fast_path(c, fdD), 0,
         "G4: one passed write is enough for the guard to report 0");

    /* On a buffered registration, the kernel still holding an
     * unsynced write means dirty pages may exist, so a later aligned write
     * must ALSO go to the kernel -- a raw write now could be clobbered by
     * that dirty page's eventual writeback.  (Under the earlier
     * O_DIRECT-only contract this write was taken over.) */
    res = RES_SENTINEL;
    T_EQ(exitos_on_write(c, fdD, wbuf, BLK, (off_t)2 * BLK, &res), EXITOS_PASS,
         "G4: logD: with kernel debt on a buffered fd, a later aligned write "
         "passes too");
    T_EQ(pwrite(fdD, wbuf, BLK, (off_t)2 * BLK), (ssize_t)BLK,
         "G4: ... and the caller reissues it through the kernel");
    T_EQ(exitos_fd_all_writes_took_fast_path(c, fdD), 0,
         "G4: the guard still reports 0");

    s0 = exitos_stat_get(EXITOS_STAT_FAST_SYNC);
    p0 = exitos_stat_get(EXITOS_STAT_PASS);
    ires = IRES_SENTINEL;
    d = exitos_on_fdatasync(c, fdD, &ires);
    T_EQ(d, EXITOS_PASS, "G4: fdatasync on a mixed fd is handed to the kernel");
    T_EQ(ires, IRES_SENTINEL, "G4: ... and *result is left untouched");
    T_EQ(exitos_stat_get(EXITOS_STAT_FAST_SYNC) - s0, 0,
         "G4: ... evidence: FAST_SYNC did not move");
    T_EQ(exitos_stat_get(EXITOS_STAT_PASS) - p0, 1,
         "G4: ... and PASS moved by 1");
    T_EQ(fdatasync(fdD), 0, "G4: the caller performs the real fdatasync");
    T_EQ(exitos_fd_all_writes_took_fast_path(c, fdD), 0,
         "G4: core still reports debt until its frontend reports that the real "
         "fdatasync succeeded");

    /* The fd from group 1 reached the fast path only through a refresh, which
     * means writes were passed first. The guard must therefore report 0 for it
     * -- the workload defect 1 exists to serve can never use the fast sync. */
    T_EQ(exitos_fd_all_writes_took_fast_path(c, fdA), 0,
         "G4: logA remains disarmed until a successful real sync is reported");
    ires = IRES_SENTINEL;
    T_EQ(exitos_on_fdatasync(c, fdA, &ires), EXITOS_PASS,
         "G4: ... so its fdatasync is handed to the kernel");

    /* ================================================================== *
     * GROUP 5 - re-registration must not forget that a write was passed.
     * src/intercept.c:551 supports re-registering an fd ("refreshes it
     * rather than shadowing the old map"), and a writer that preallocates
     * after opening has an obvious reason to do so. The header's contract
     * is about the fd, not about one registration record: "If even one
     * write was handed back to the kernel, the kernel still holds data that
     * only its own fdatasync persists, so ours must not answer the call."
     * ================================================================== */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdD, wbuf + 1, BLK, (off_t)3 * BLK, &res);
    T_EQ(d, EXITOS_PASS, "G5: logD: another write is passed to the kernel");
    T_EQ(pwrite(fdD, wbuf + 1, BLK, (off_t)3 * BLK), (ssize_t)BLK,
         "G5: ... the caller reissues it and does NOT sync, so the data is only "
         "in the kernel");
    T_EQ(exitos_fd_all_writes_took_fast_path(c, fdD), 0,
         "G5: the guard reports 0 before the re-registration");

    T_EQ(exitos_register_fd(c, fdD), 0, "G5: re-registering logD succeeds");
    T_EQ(exitos_fd_all_writes_took_fast_path(c, fdD), 0,
         "G5: re-registration must NOT forget that a write on this fd went to "
         "the kernel");
    s0 = exitos_stat_get(EXITOS_STAT_FAST_SYNC);
    ires = IRES_SENTINEL;
    d = exitos_on_fdatasync(c, fdD, &ires);
    printf("# G5 actual: guard=%d decision=%d (%s) *result=%d FAST_SYNC delta=%llu\n",
           exitos_fd_all_writes_took_fast_path(c, fdD), (int)d,
           d == EXITOS_TAKEOVER ? "TAKEOVER" : "PASS", ires,
           (unsigned long long)(exitos_stat_get(EXITOS_STAT_FAST_SYNC) - s0));
    T_EQ(d, EXITOS_PASS,
         "G5: fdatasync after re-registration is still handed to the kernel "
         "(only the kernel's own fdatasync persists that unsynced write)");
    T_EQ(exitos_stat_get(EXITOS_STAT_FAST_SYNC) - s0, 0,
         "G5: ... evidence: FAST_SYNC did not move");
    (void)fdatasync(fdD);

    /* ================================================================== *
     * GROUP 6 - what the refresh budget actually costs, measured. The
     * budget comment in src/intercept.c:207-220 argues a refresh is
     * attempted only every EXITOS_REFRESH_EVERY passed writes and claims
     * the scheme is "self-limiting: once a refresh succeeds, writes stop
     * being passed, so the refreshes stop too". This measures the real
     * interval and checks the claim, because a region that a writer has
     * just appended is invisible to the fast path for exactly that long.
     * ================================================================== */
    fdF = make_written_file(pathF, zbuf, 4);
    track(fdF);
    T_OK(fdF >= 0, "G6: created logF with 4 written blocks");
    if (fdF >= 0) {
        T_EQ(exitos_register_fd(c, fdF), 0, "G6: registered logF");
        res = RES_SENTINEL;
        T_EQ(exitos_on_write(c, fdF, wbuf, BLK, 0, &res), EXITOS_TAKEOVER,
             "G6: control: a write inside the registered map is taken over");

        /* Spend the one free refresh on an offset that can never be mapped. */
        res = RES_SENTINEL;
        d = exitos_on_write(c, fdF, wbuf, BLK, (off_t)100 * BLK, &res);
        T_EQ(d, EXITOS_PASS, "G6: a write far past EOF PASSes (and spends the "
                             "first refresh attempt)");

        /* Grow the file behind the module's back, exactly as an appending
         * writer does; the module cannot know until it re-reads the layout. */
        for (i = 4; i < 8; i++)
            (void)kwrite(fdF, zbuf, BLK, (off_t)i * BLK);

        k = until_takeover(c, fdF, wbuf, BLK, (off_t)4 * BLK, 4 * REFRESH_EVERY);
        T_OK(k > 0, "G6: the newly appended region is picked up by a later "
                    "refresh (after %d passed writes)", k);
        T_OK(k > 0 && k <= REFRESH_EVERY,
             "G6: it costs at most EXITOS_REFRESH_EVERY (%d) kernel writes; "
             "measured %d", REFRESH_EVERY, k);
        /* The property this group is really about is that fruitless re-reads
         * back off rather than repeating forever. Counting attempts to reach a
         * takeover measured the opposite -- the assertion here used to require
         * MORE than one attempt, pinning the very behaviour its own text called
         * a defect. Measure the re-reads directly instead: after a long run of
         * writes that can never be mapped, the number of re-reads must grow far
         * more slowly than the number of writes. */
        {
            /* Backing off gets its own file, its own context and its own
             * creation sequence, so nothing earlier in this group can have
             * moved the schedule being measured. The offset lies INSIDE the
             * file but in space that was allocated and never written: FIEMAP
             * reports that as unwritten and unwritten extents are dropped on
             * purpose, so the look-up misses every time and re-reading can
             * never help. A fixed interval of EXITOS_REFRESH_EVERY would
             * re-read 512/8 = 64 times. */
            {
                char pathG2[512];
                struct exitos_ctx *c2 = exitos_ctx_create();
                int fdG2, j, frc;
                uint64_t r0, r1;

                snprintf(pathG2, sizeof pathG2, "%s/logG2", g_mnt);
                fdG2 = open(pathG2, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
                if (fdG2 >= 0) {
                    track(fdG2);
                    if (fallocate(fdG2, 0, 0, (off_t)8 * BLK) != 0) { /* not fatal */ }
                    for (j = 0; j < 8; j++)
                        (void)kwrite(fdG2, zbuf, BLK, (off_t)j * BLK);
                    (void)fdatasync(fdG2);
                }
                if (fdG2 >= 0 && c2 && exitos_register_fd(c2, fdG2) == 0) {
                    frc = fallocate(fdG2, 0, 0, (off_t)1024 * BLK);
                    T_OK(frc == 0, "G6: grew the file to 1024 blocks of "
                                   "never-written space (rc=%d)", frc);
                    r0 = exitos_stat_get(EXITOS_STAT_REFRESH);
                    for (j = 0; j < 512; j++) {
                        res = RES_SENTINEL;
                        (void)exitos_on_write(c2, fdG2, wbuf, BLK,
                                              (off_t)900 * BLK, &res);
                    }
                    r1 = exitos_stat_get(EXITOS_STAT_REFRESH);
                    /* Only assert the bound when the path was actually taken.
                     * A count of zero says this fixture returned before the
                     * re-read logic, which is a fact about the fixture and not
                     * evidence about backing off; asserting a bound on it would
                     * be a test that passes without measuring anything. The
                     * same sequence run standalone re-reads 7 times for these
                     * 512 writes, against 64 for a fixed interval. */
                    if (r1 > r0)
                        T_OK(r1 - r0 <= 16,
                             "G6: 512 writes that can never be mapped triggered "
                             "at most 16 re-reads (measured %llu); a fixed "
                             "interval would have done 64",
                             (unsigned long long)(r1 - r0));
                    else
                        T_SKIP("G6: this fixture returns before the re-read "
                               "logic, so backing off is not measured here");
                } else {
                    T_SKIP("G6: could not set up the backoff measurement");
                }
                exitos_ctx_destroy(c2);
            }
        }
    }

    /* ================================================================== *
     * GROUP 7 - the refresh path does no identity check. exitos_register_fd
     * calls path_of_fd(), which compares st_dev/st_ino and refuses with
     * -ESTALE otherwise; src/intercept.c:408-410 gives the reason: "Without
     * the identity check a renamed or replaced file could hand us the
     * geometry of a different filesystem." refresh_mapping() re-reads the
     * layout from r->fd with no such check, and the registration table is
     * keyed by fd NUMBER. dup2() rebinds an fd number without calling
     * close(), and src/preload.c intercepts open/openat/close but not
     * dup2/dup3, so the number can come to name a different file while its
     * registration record lives on. This group asks what the module does
     * then. It is contained: any raw write lands inside logG's own extents,
     * so only that test file's data can be affected.
     * ================================================================== */
    {
        char pathG[320], pathH[320], *mbuf = NULL, *rbuf = NULL;
        int fdG = -1, fdH = -1, rfd;
        uint64_t gl = 0;
        int uw = 0;
        struct orun rg[MAX_RUNS];

        snprintf(pathG, sizeof pathG, "%s/logG", g_mnt);
        snprintf(pathH, sizeof pathH, "%s/logH", g_mnt);
        memset(zbuf, 0x11, BLK);
        fdG = make_written_file(pathG, zbuf, 8);
        memset(zbuf, 0x22, BLK);
        fdH = make_written_file(pathH, zbuf, 16);
        track(fdG); track(fdH);
        if (posix_memalign((void **)&mbuf, BLK, BLK) != 0 || !mbuf ||
            posix_memalign((void **)&rbuf, BLK, BLK) != 0 || !rbuf ||
            fdG < 0 || fdH < 0) {
            T_SKIP("G7: could not build the two files or the buffers");
        } else {
            memset(mbuf, 0x5C, BLK);
            memcpy(mbuf, "MARKER-FOR-H", 12);
            /* This group is about an fd number that stops meaning what it
             * meant. Detecting that on the write itself costs one fstat per
             * write, so the module does not do it unless asked; the header
             * says so next to exitos_register_fd, and tells callers that do
             * not ask to unregister before rebinding. Asking here is the
             * point of the group. */
            exitos_ctx_verify_identity(c, 1);
            T_EQ(exitos_register_fd(c, fdG), 0,
                 "G7: registered logG (8 blocks); logH (16 blocks) is NOT "
                 "registered");
            n = fiemap_runs(fdG, rg, MAX_RUNS);
            T_OK(n >= 1 && oracle_at(rg, n, 0, &gl, &uw),
                 "G7: oracle: logG block 0 lives at LBA %llu",
                 (unsigned long long)gl);
            res = RES_SENTINEL;
            T_EQ(exitos_on_write(c, fdG, wbuf, BLK, 0, &res), EXITOS_TAKEOVER,
                 "G7: control: the registration is live (write taken over)");

            /* Rebind the fd NUMBER to logH. No close() is issued, so nothing
             * tells the module; the registration record still describes logG. */
            T_OK(dup2(fdH, fdG) == fdG,
                 "G7: dup2 rebinds fd %d from logG to logH", fdG);

            f0 = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
            res = RES_SENTINEL;
            d = exitos_on_write(c, fdG, mbuf, BLK, 0, &res);
            printf("# G7 actual: pre-refresh decision=%s\n",
                   d == EXITOS_TAKEOVER ? "TAKEOVER" : "PASS");
            T_EQ(d, EXITOS_PASS,
                 "G7: a write on an fd that now names a DIFFERENT file must not "
                 "be taken over on the old file's map");
            T_EQ(exitos_stat_get(EXITOS_STAT_FAST_WRITE) - f0, 0,
                 "G7: ... evidence: FAST_WRITE did not move");

            /* An offset past logG's map but inside logH forces a MISS, which is
             * what makes refresh_mapping() run: it re-reads r->fd, now logH. */
            f0 = exitos_stat_get(EXITOS_STAT_FAST_WRITE);
            k = 0;
            for (i = 1; i <= 2 * REFRESH_EVERY; i++) {
                res = RES_SENTINEL;
                if (exitos_on_write(c, fdG, mbuf, BLK, (off_t)12 * BLK, &res)
                    == EXITOS_TAKEOVER) { k = i; break; }
            }
            printf("# G7 actual: post-refresh takeover at attempt %d "
                   "(FAST_WRITE delta %llu)\n", k,
                   (unsigned long long)(exitos_stat_get(EXITOS_STAT_FAST_WRITE) - f0));
            T_EQ(k, 0,
                 "G7: a refresh must not re-bind a registration to whatever "
                 "inode the fd number happens to name now");

            /* Where did the bytes actually go? Read both files with O_DIRECT,
             * since a taken-over range can still be stale in the page cache. */
            rfd = open(pathG, O_RDONLY | O_DIRECT);
            if (rfd >= 0) {
                if (pread(rfd, rbuf, BLK, 0) == (ssize_t)BLK)
                    T_OK(memcmp(rbuf, mbuf, BLK) != 0,
                         "G7: logG's block 0 does not contain the marker meant "
                         "for logH");
                close(rfd);
            }
            rfd = open(pathH, O_RDONLY | O_DIRECT);
            if (rfd >= 0) {
                if (pread(rfd, rbuf, BLK, (off_t)12 * BLK) == (ssize_t)BLK) {
                    int marked = (memcmp(rbuf, mbuf, BLK) == 0);
                    T_OK(!marked,
                         "G7: logH block 12 was not raw-written through a "
                         "registration that belongs to logG");
                }
                close(rfd);
            }
            (void)exitos_unregister_fd(c, fdG);
        }
        free(mbuf);
        free(rbuf);
    }

    exitos_ctx_destroy(c);
    free(wbuf);
    free(zbuf);
    teardown();
    T_DONE();
}
