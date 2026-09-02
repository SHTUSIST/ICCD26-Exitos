/* Tier-1 integration test for the donor module (include/exitos_donor.h).
 *
 * Everything here runs against a loop-backed ext4 image built by
 * tests/harness/loopfix.sh.  No physical block device is ever opened.
 *
 * The image path is made unique per process (EXITOS_IMG / EXITOS_MNT carry a
 * pid suffix) so several module tests can run in parallel without colliding.
 * Teardown is done inline instead of via "loopfix.sh down" on purpose:
 * loopfix.sh records the loop device in the shared file /tmp/exitos-loop.dev,
 * so a "down" run here could detach another test's loop device and delete
 * another test's image.  We detach exactly the loop device our own "up" call
 * reported and nothing else.
 *
 * Kernel facts this test was calibrated against (measured on the loop fixture,
 * Linux 5.15.0-187-generic, ext4, 4 KiB blocks):
 *   - struct move_extent is {reserved, donor_fd, orig_start, donor_start, len,
 *     moved_len}; the first u32 is "reserved, must be zero" in current kernels,
 *     NOT an orig_fd field.  The orig file is the fd the ioctl is issued on.
 *     Offsets and lengths are in filesystem blocks.
 *   - EXT4_IOC_MOVE_EXT = _IOWR('f', 15, struct move_extent).
 *   - The ioctl SWAPS extents; the orig file's data is preserved across the
 *     swap.  It requires BOTH files to have real blocks over the range:
 *     a pure hole in the orig (ftruncate only) fails with ENODATA.
 *   - Donor extents may stay unwritten (plain fallocate); zero-filling the
 *     donor is not required by the ioctl.
 *   - Refusals: donor on another filesystem -> EINVAL, orig opened O_RDONLY ->
 *     EBADF, orig of size 0 -> EINVAL.
 *   - Writing into an already-allocated (unwritten) extent adds 0 new blocks,
 *     which is what makes the "no per-append allocation" assertions below
 *     meaningful.
 */
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#include "tap.h"
#include "exitos_donor.h"

#define KIB (1024ULL)
#define MIB (1024ULL * 1024ULL)
#define BLK 4096ULL

/* ---- EXT4_IOC_MOVE_EXT, defined locally: no uapi header exports it ------- */
struct exitos_move_extent {
    uint32_t reserved;    /* must be zero */
    uint32_t donor_fd;
    uint64_t orig_start;  /* in filesystem blocks */
    uint64_t donor_start; /* in filesystem blocks */
    uint64_t len;         /* in filesystem blocks */
    uint64_t moved_len;   /* out: blocks actually moved */
};
#define EXITOS_IOC_MOVE_EXT _IOWR('f', 15, struct exitos_move_extent)

/* ---- fixture state ------------------------------------------------------ */
static char g_img[256], g_mnt[256], g_loop[128];
static int  g_up;

static int sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static int sh(const char *fmt, ...)
{
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return system(cmd);
}

static void fixture_down(void)
{
    if (!g_up) return;
    g_up = 0;
    sh("umount %s 2>/dev/null || umount -l %s 2>/dev/null", g_mnt, g_mnt);
    if (g_loop[0]) sh("losetup -d %s 2>/dev/null", g_loop);
    sh("rm -f %s", g_img);
    sh("rmdir %s 2>/dev/null", g_mnt);
}

static void crash_handler(int sig)
{
    printf("not ok - test process died on signal %d; tearing the loop fixture down\n", sig);
    fflush(stdout);
    fixture_down();
    _exit(1);
}

static const char *find_loopfix(const char *argv0, char *out, size_t n)
{
    const char *env = getenv("EXITOS_LOOPFIX");
    char tmp[512], *d;

    if (env && access(env, R_OK) == 0) { snprintf(out, n, "%s", env); return out; }
    if (access("tests/harness/loopfix.sh", R_OK) == 0) {
        snprintf(out, n, "tests/harness/loopfix.sh");
        return out;
    }
    snprintf(tmp, sizeof tmp, "%s", argv0);
    d = dirname(tmp);
    snprintf(out, n, "%s/../harness/loopfix.sh", d);
    if (access(out, R_OK) == 0) return out;
    return NULL;
}

/* Returns 0 on success. Fills g_mnt / g_loop / g_img. */
static int fixture_up(const char *argv0)
{
    char loopfix[512], cmd[2048], line[512];
    FILE *f;
    int pid = (int)getpid();

    if (!find_loopfix(argv0, loopfix, sizeof loopfix)) {
        printf("# loopfix.sh not found (run from the repo root or set EXITOS_LOOPFIX)\n");
        return -1;
    }
    snprintf(g_img, sizeof g_img, "/tmp/exitos-donor-%d.img", pid);
    snprintf(g_mnt, sizeof g_mnt, "/tmp/exitos-donor-%d.mnt", pid);
    snprintf(cmd, sizeof cmd,
             "EXITOS_IMG=%s EXITOS_MNT=%s EXITOS_SIZE_MB=512 bash %s up 2>&1",
             g_img, g_mnt, loopfix);
    f = popen(cmd, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char l[128], m[256];
        if (sscanf(line, "LOOP=%127s MNT=%255s", l, m) == 2) {
            snprintf(g_loop, sizeof g_loop, "%s", l);
            g_up = 1;
        } else {
            printf("# loopfix: %s", line);
        }
    }
    pclose(f);
    if (!g_up) return -1;
    atexit(fixture_down);
    signal(SIGSEGV, crash_handler);
    signal(SIGBUS,  crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGINT,  crash_handler);
    signal(SIGTERM, crash_handler);
    return 0;
}

/* ---- small helpers ------------------------------------------------------ */
static uint64_t alloc_bytes(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return 0;
    return (uint64_t)st.st_blocks * 512ULL;
}

static uint64_t file_size(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return 0;
    return (uint64_t)st.st_size;
}

static uint64_t fs_free(const char *path)
{
    struct statvfs v;
    if (statvfs(path, &v) < 0) return 0;
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
}

/* One physical extent as FIEMAP reports it. */
struct fx { uint64_t logical, physical, length; unsigned flags; };

#define MAXE 512

/* Full FIEMAP extent list for a path. *n is capped at max. */
static int fiemap_extents(const char *path, struct fx *out, unsigned max, unsigned *n)
{
    char *buf;
    struct fiemap *fm;
    int fd, rc = 0;
    unsigned i;

    *n = 0;
    fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    buf = calloc(1, sizeof(struct fiemap) + MAXE * sizeof(struct fiemap_extent));
    if (!buf) { close(fd); return -ENOMEM; }
    fm = (struct fiemap *)buf;
    fm->fm_start = 0;
    fm->fm_length = ~0ULL;
    fm->fm_flags = FIEMAP_FLAG_SYNC;
    fm->fm_extent_count = MAXE;
    if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
        rc = -errno;
    } else {
        for (i = 0; i < fm->fm_mapped_extents && i < max; i++) {
            out[i].logical  = fm->fm_extents[i].fe_logical;
            out[i].physical = fm->fm_extents[i].fe_physical;
            out[i].length   = fm->fm_extents[i].fe_length;
            out[i].flags    = fm->fm_extents[i].fe_flags;
        }
        *n = i;
    }
    free(buf);
    close(fd);
    return rc;
}

/* FIEMAP summary: extent count, bytes mapped, OR of all extent flags. */
static int fiemap_of(const char *path, unsigned *n_ext, uint64_t *mapped,
                     unsigned *flags_or)
{
    struct fx ex[MAXE];
    unsigned i, n;
    int rc;

    *n_ext = 0; *mapped = 0; *flags_or = 0;
    rc = fiemap_extents(path, ex, MAXE, &n);
    if (rc) return rc;
    *n_ext = n;
    for (i = 0; i < n; i++) { *mapped += ex[i].length; *flags_or |= ex[i].flags; }
    return 0;
}

/* ---- physical-block provenance ------------------------------------------
 * The whole point of the module is that the target's new blocks come out of
 * the donor pool rather than from a fresh allocation.  Snapshot every physical
 * range the pool's donor files hold before an extend, then check the target's
 * blocks afterwards against that snapshot. */
static struct fx g_pool_ranges[MAXE];
static unsigned  g_pool_nranges;

static int snapshot_pool_ranges(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;

    g_pool_nranges = 0;
    if (!d) return -errno;
    while ((de = readdir(d))) {
        char p[1024];
        struct stat st;
        struct fx ex[MAXE];
        unsigned i, n = 0;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (stat(p, &st) < 0 || !S_ISREG(st.st_mode)) continue;
        if (fiemap_extents(p, ex, MAXE, &n) != 0) continue;
        for (i = 0; i < n && g_pool_nranges < MAXE; i++)
            g_pool_ranges[g_pool_nranges++] = ex[i];
    }
    closedir(d);
    return 0;
}

/* Bytes of [phys, phys+len) that lie inside the snapshotted donor ranges. */
static uint64_t overlap_with_pool(uint64_t phys, uint64_t len)
{
    uint64_t total = 0, a0 = phys, a1 = phys + len;
    unsigned i;
    for (i = 0; i < g_pool_nranges; i++) {
        uint64_t b0 = g_pool_ranges[i].physical;
        uint64_t b1 = b0 + g_pool_ranges[i].length;
        uint64_t lo = a0 > b0 ? a0 : b0;
        uint64_t hi = a1 < b1 ? a1 : b1;
        if (hi > lo) total += hi - lo;
    }
    return total;
}

static int fiemap_fd(int fd, unsigned *n_ext, uint64_t *mapped, unsigned *flags_or)
{
    char p[64];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    return fiemap_of(p, n_ext, mapped, flags_or);
}

static int all_bytes_are(const unsigned char *b, size_t n, unsigned char v)
{
    size_t i;
    for (i = 0; i < n; i++) if (b[i] != v) return 0;
    return 1;
}

static int fill_and_read(int fd, uint64_t off, size_t len, unsigned char pat,
                         unsigned char *rb)
{
    unsigned char *wb = malloc(len);
    ssize_t w, r;
    if (!wb) return -ENOMEM;
    memset(wb, pat, len);
    w = pwrite(fd, wb, len, (off_t)off);
    free(wb);
    if (w != (ssize_t)len) return -errno;
    if (fsync(fd) < 0) return -errno;
    r = pread(fd, rb, len, (off_t)off);
    if (r != (ssize_t)len) return -errno;
    return 0;
}

static void mk_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", g_mnt, name);
}

static int open_target(const char *name, int flags)
{
    char p[512];
    mk_path(p, sizeof p, name);
    return open(p, flags, 0644);
}

/* Does this kernel/config actually implement EXT4_IOC_MOVE_EXT here?
 * Uses two fallocated files (both need real blocks) so a failure really means
 * "unsupported", not "wrong arguments". Returns 1 if supported, 0 otherwise
 * with *err set to the errno. */
static int move_ext_supported(int *err)
{
    char op[512], dp[512];
    struct exitos_move_extent me;
    int o, d, r;
    const uint64_t sz = 64 * KIB;

    *err = 0;
    mk_path(op, sizeof op, ".probe-orig");
    mk_path(dp, sizeof dp, ".probe-donor");
    o = open(op, O_RDWR | O_CREAT | O_TRUNC, 0644);
    d = open(dp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (o < 0 || d < 0) { *err = errno; if (o >= 0) close(o); if (d >= 0) close(d); return 0; }
    if (fallocate(o, 0, 0, (off_t)sz) < 0 || fallocate(d, 0, 0, (off_t)sz) < 0) {
        *err = errno; close(o); close(d); unlink(op); unlink(dp); return 0;
    }
    memset(&me, 0, sizeof me);
    me.donor_fd = (uint32_t)d;
    me.len = sz / BLK;
    errno = 0;
    r = ioctl(o, EXITOS_IOC_MOVE_EXT, &me);
    *err = (r < 0) ? errno : 0;
    close(o); close(d); unlink(op); unlink(dp);
    return r == 0 && me.moved_len == sz / BLK;
}

/* ---- the tests ---------------------------------------------------------- */
int main(int argc, char **argv)
{
    char dirA[512], dirB[512], dirC[512], dirD[512], dirE[512], nodir[512];
    char xdev_path[256];
    struct donor_pool *pA = NULL, *pB = NULL, *pC = NULL, *pD = NULL, *pE = NULL;
    int fdA = -1, fdC = -1, fdC2 = -1, fdD = -1, fdRO = -1, fdX = -1, fdDir = -1;
    unsigned char *rb = NULL;
    uint64_t free0, free1, free2;
    unsigned nx; uint64_t mapped; unsigned fl;
    int64_t r64;
    int rc, err = 0;

    (void)argc;

    if (!t_need_root()) {
        T_SKIP("donor integ needs root (loop device + mount); running as uid %d", (int)geteuid());
        T_DONE();
    }
    if (fixture_up(argv[0]) != 0) {
        T_SKIP("could not bring up the ext4 loop fixture; donor integ not run");
        T_DONE();
    }
    printf("# fixture: img=%s mnt=%s loop=%s\n", g_img, g_mnt, g_loop);

    if (!move_ext_supported(&err)) {
        T_SKIP("EXT4_IOC_MOVE_EXT unsupported on this kernel/config: errno=%d (%s). "
               "THE WHOLE donor INTEGRATION TEST WAS SKIPPED, NOT PASSED.",
               err, strerror(err));
        printf("# SKIPPED: donor extent-donation path could not be exercised here.\n");
        T_DONE();
    }
    T_OK(1, "EXT4_IOC_MOVE_EXT is supported on the loop ext4 fixture");

    mk_path(dirA, sizeof dirA, "pool-a");
    mk_path(dirB, sizeof dirB, "pool-b");
    mk_path(dirC, sizeof dirC, "pool-c");
    mk_path(dirD, sizeof dirD, "pool-d");
    mk_path(dirE, sizeof dirE, "pool-e");
    mk_path(nodir, sizeof nodir, "no-such-dir/deeper");
    sh("mkdir -m 0700 -p %s %s %s %s %s", dirA, dirB, dirC, dirD, dirE);

    /* =====================================================================
     * A. donor_pool_create refusals
     * ===================================================================== */
    T_OK(donor_pool_create(NULL, 4, 8 * MIB) == NULL,
         "pool_create(dir=NULL) refuses and returns NULL");
    T_OK(donor_pool_create(nodir, 4, 8 * MIB) == NULL,
         "pool_create on a nonexistent directory returns NULL");
    T_OK(donor_pool_create(dirA, 0, 8 * MIB) == NULL,
         "pool_create(nfiles=0) returns NULL");
    T_OK(donor_pool_create(dirA, -1, 8 * MIB) == NULL,
         "pool_create(nfiles=-1) returns NULL");
    T_OK(donor_pool_create(dirA, 4, 0) == NULL,
         "pool_create(bytes_each=0) returns NULL");

    /* Ask for 4 GiB of donors on a 512 MiB filesystem. */
    free0 = fs_free(g_mnt);
    T_OK(donor_pool_create(dirA, 8, 512 * MIB) == NULL,
         "pool_create refuses when the pool cannot fit (8 x 512 MiB on a 512 MiB fs)");
    free1 = fs_free(g_mnt);
    T_OK(free1 + 8 * MIB >= free0,
         "a failed pool_create leaves no partial donors behind (free %llu MiB -> %llu MiB)",
         (unsigned long long)(free0 / MIB), (unsigned long long)(free1 / MIB));

    /* =====================================================================
     * B. donor_pool_create happy path: 4 donors x 8 MiB, really allocated
     * ===================================================================== */
    free0 = fs_free(g_mnt);
    pA = donor_pool_create(dirA, 4, 8 * MIB);
    T_OK(pA != NULL, "pool_create(%s, 4, 8 MiB) succeeds", dirA);
    if (!pA) {
        printf("# cannot continue without a pool\n");
        T_DONE();
    }
    free1 = fs_free(g_mnt);
    T_OK(free0 - free1 >= 32 * MIB - 256 * KIB,
         "the pool reserved >= 32 MiB of filesystem space (took %llu MiB)",
         (unsigned long long)((free0 - free1) / MIB));

    {
        DIR *d = opendir(dirA);
        struct dirent *de;
        int nfiles = 0, n_big_alloc = 0, n_real_extents = 0, n_delalloc = 0;
        T_OK(d != NULL, "pool directory %s is readable", dirA);
        while (d && (de = readdir(d))) {
            char p[1024];
            struct stat st;
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            snprintf(p, sizeof p, "%s/%s", dirA, de->d_name);
            if (stat(p, &st) < 0 || !S_ISREG(st.st_mode)) continue;
            nfiles++;
            if ((uint64_t)st.st_blocks * 512ULL >= 8 * MIB) n_big_alloc++;
            if (fiemap_of(p, &nx, &mapped, &fl) == 0 && nx >= 1 && mapped >= 8 * MIB)
                n_real_extents++;
            if (fl & (FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_UNKNOWN)) n_delalloc++;
        }
        if (d) closedir(d);
        T_EQ(nfiles, 4, "pool directory holds exactly 4 donor files");
        T_EQ(n_big_alloc, 4, "every donor has >= 8 MiB of blocks charged to it (st_blocks), not a hole");
        T_EQ(n_real_extents, 4, "FIEMAP shows every donor covered by >= 8 MiB of real extents");
        T_EQ(n_delalloc, 0, "no donor extent is delayed-allocation or unknown; blocks are on disk");
    }

    /* =====================================================================
     * C. donor_extend happy path
     * ===================================================================== */
    fdA = open_target("log-a", O_RDWR | O_CREAT | O_TRUNC);
    T_OK(fdA >= 0, "created target log file log-a");
    T_EQ(alloc_bytes(fdA), 0, "fresh target starts with 0 allocated bytes");

    /* remember which physical blocks the pool holds, to check provenance below */
    T_EQ(snapshot_pool_ranges(dirA), 0, "snapshotted the donor pool's physical extents");
    T_OK(g_pool_nranges >= 4, "the snapshot covers all 4 donors (%u physical ranges)",
         g_pool_nranges);

    r64 = donor_extend(pA, fdA, 0, 1 * MIB);
    T_EQ(r64, (int64_t)(1 * MIB), "donor_extend(off=0, 1 MiB) moves the full 1 MiB");
    T_OK(alloc_bytes(fdA) >= 1 * MIB,
         "target's allocated size grew to >= 1 MiB (is %llu KiB)",
         (unsigned long long)(alloc_bytes(fdA) / KIB));
    T_OK(file_size(fdA) >= 1 * MIB,
         "target's size covers the donated region (is %llu KiB)",
         (unsigned long long)(file_size(fdA) / KIB));

    rb = malloc(2 * MIB);
    T_OK(rb != NULL, "allocated a 2 MiB read buffer");
    memset(rb, 0xAA, 1 * MIB);
    T_EQ(pread(fdA, rb, 1 * MIB, 0), (int64_t)(1 * MIB),
         "the whole donated region is readable (pread returns 1 MiB)");
    T_OK(all_bytes_are(rb, 1 * MIB, 0x00),
         "donated region reads as zeros: no stale donor data is exposed");

    T_OK(fiemap_fd(fdA, &nx, &mapped, &fl) == 0 && mapped >= 1 * MIB,
         "FIEMAP on the target maps >= 1 MiB of real extents (mapped %llu KiB in %u extents)",
         (unsigned long long)(mapped / KIB), nx);
    T_OK(nx >= 1 && nx <= 2,
         "the donated 1 MiB arrived as contiguous space (%u extents, expected 1-2)", nx);

    {   /* provenance: the target's blocks must be blocks the pool used to own.
         * A plain fallocate on the target would pass every check above and
         * fail this one, which is the whole difference the module exists for. */
        struct fx tex[MAXE];
        unsigned i, n = 0;
        uint64_t from_pool = 0, tot = 0;
        char pa[512];

        snprintf(pa, sizeof pa, "/proc/self/fd/%d", fdA);
        T_EQ(fiemap_extents(pa, tex, MAXE, &n), 0, "read the target's physical extents");
        for (i = 0; i < n; i++) {
            tot += tex[i].length;
            from_pool += overlap_with_pool(tex[i].physical, tex[i].length);
        }
        T_OK(tot >= 1 * MIB, "target maps %llu KiB of physical space",
             (unsigned long long)(tot / KIB));
        T_OK(from_pool >= 1 * MIB - 64 * KIB,
             "the target's blocks came out of the donor pool: %llu KiB of %llu KiB "
             "sit inside physical ranges the donors held before the extend",
             (unsigned long long)(from_pool / KIB), (unsigned long long)(tot / KIB));

        /* and the pool must no longer be sitting on those same blocks */
        {
            struct fx now[MAXE];
            unsigned j, m = 0, still = 0;
            DIR *d = opendir(dirA);
            struct dirent *de;
            uint64_t donor_overlap = 0;
            while (d && (de = readdir(d))) {
                char p[1024];
                struct stat st;
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                snprintf(p, sizeof p, "%s/%s", dirA, de->d_name);
                if (stat(p, &st) < 0 || !S_ISREG(st.st_mode)) continue;
                if (fiemap_extents(p, now, MAXE, &m) != 0) continue;
                for (j = 0; j < m; j++) {
                    unsigned k;
                    for (k = 0; k < n; k++) {
                        uint64_t a0 = now[j].physical, a1 = a0 + now[j].length;
                        uint64_t b0 = tex[k].physical, b1 = b0 + tex[k].length;
                        uint64_t lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
                        if (hi > lo) donor_overlap += hi - lo;
                    }
                }
                still++;
            }
            if (d) closedir(d);
            T_EQ(donor_overlap, 0,
                 "no donor file still claims the physical blocks now owned by the target");
            T_EQ(still, 4, "donating 1 MiB out of 8 MiB donors leaves all 4 donor files in place");
        }
    }

    {
        uint64_t before = alloc_bytes(fdA);
        rc = fill_and_read(fdA, 0, 1 * MIB, 0x5A, rb);
        T_EQ(rc, 0, "wrote 1 MiB of 0x5A into the donated region and read it back");
        T_OK(all_bytes_are(rb, 1 * MIB, 0x5A), "the pattern round-trips byte for byte");
        T_EQ(alloc_bytes(fdA), (int64_t)before,
             "appending into donated space allocated no new blocks (%llu KiB before and after)",
             (unsigned long long)(before / KIB));
    }

    r64 = donor_extend(pA, fdA, 1 * MIB, 1 * MIB);
    T_EQ(r64, (int64_t)(1 * MIB), "a second donor_extend at offset 1 MiB moves another 1 MiB");
    T_OK(alloc_bytes(fdA) >= 2 * MIB, "target now holds >= 2 MiB of blocks (is %llu KiB)",
         (unsigned long long)(alloc_bytes(fdA) / KIB));
    T_OK(fiemap_fd(fdA, &nx, &mapped, &fl) == 0 && mapped >= 2 * MIB,
         "FIEMAP maps >= 2 MiB after the second extend (mapped %llu KiB)",
         (unsigned long long)(mapped / KIB));
    memset(rb, 0xAA, 1 * MIB);
    T_EQ(pread(fdA, rb, 1 * MIB, (off_t)(1 * MIB)), (int64_t)(1 * MIB),
         "the second donated region is readable too");
    T_OK(all_bytes_are(rb, 1 * MIB, 0x00), "the second donated region also reads as zeros");
    memset(rb, 0xAA, 1 * MIB);
    T_EQ(pread(fdA, rb, 1 * MIB, 0), (int64_t)(1 * MIB), "first region still readable");
    T_OK(all_bytes_are(rb, 1 * MIB, 0x5A),
         "the second extend did not disturb data already written in the first region");

    /* Sub-block request: blocks are the unit of donation. */
    fdD = open_target("log-d", O_RDWR | O_CREAT | O_TRUNC);
    T_OK(fdD >= 0, "created target log-d");
    r64 = donor_extend(pA, fdD, 0, 5000);
    T_OK(r64 == -EINVAL || (r64 > 0 && r64 % (int64_t)BLK == 0),
         "a non-block-multiple request is refused with -EINVAL or rounded to whole blocks (got %lld)",
         (long long)r64);

    /* =====================================================================
     * D. donor_extend refusals
     * ===================================================================== */
    r64 = donor_extend(NULL, fdA, 0, 4096);
    T_EQ(r64, (int64_t)(-EINVAL), "donor_extend(pool=NULL) returns -EINVAL (got %lld)", (long long)r64);

    r64 = donor_extend(pA, -1, 0, 4096);
    T_OK(r64 < 0, "donor_extend on fd -1 returns a negative errno (got %lld)", (long long)r64);
    T_EQ(r64, (int64_t)(-EBADF), "donor_extend on fd -1 reports -EBADF");

    r64 = donor_extend(pA, fdA, 0, 0);
    T_OK(r64 == 0 || r64 == -EINVAL,
         "donor_extend(bytes=0) moves nothing: returns 0 or -EINVAL (got %lld)", (long long)r64);

    fdRO = open_target("log-d", O_RDONLY);
    T_OK(fdRO >= 0, "reopened log-d read-only");
    r64 = donor_extend(pA, fdRO, 0, 1 * MIB);
    T_OK(r64 < 0, "donor_extend into a read-only fd is refused (got %lld)", (long long)r64);

    fdDir = open(g_mnt, O_RDONLY | O_DIRECTORY);
    T_OK(fdDir >= 0, "opened the mount point as a directory fd");
    r64 = donor_extend(pA, fdDir, 0, 1 * MIB);
    T_OK(r64 < 0, "donor_extend into a directory fd is refused (got %lld)", (long long)r64);

    /* Target on a different filesystem: donor extents cannot cross a mount. */
    snprintf(xdev_path, sizeof xdev_path, "/tmp/exitos-donor-xdev-%d", (int)getpid());
    fdX = open(xdev_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    T_OK(fdX >= 0, "created a target on another filesystem (%s)", xdev_path);
    r64 = donor_extend(pA, fdX, 0, 1 * MIB);
    T_OK(r64 < 0,
         "donor_extend into a file on a different filesystem is refused (got %lld)",
         (long long)r64);
    T_EQ(alloc_bytes(fdX), 0,
         "the cross-filesystem target was left untouched (0 blocks allocated)");

    /* Capacity. A pool of one 4 MiB donor cannot hand out more than 4 MiB of
     * contiguous space in one call, and cannot hand out more than it holds in
     * total - the header calls the pool something that must "not be
     * permanently consumed", i.e. its supply is finite. */
    pB = donor_pool_create(dirB, 1, 4 * MIB);
    T_OK(pB != NULL, "pool_create(%s, 1, 4 MiB) succeeds", dirB);
    fdC = open_target("log-c", O_RDWR | O_CREAT | O_TRUNC);
    T_OK(fdC >= 0, "created target log-c");
    if (pB) {
        int64_t served = 0;
        r64 = donor_extend(pB, fdC, 0, 8 * MIB);
        T_OK(r64 <= (int64_t)(4 * MIB),
             "one 4 MiB donor cannot supply an 8 MiB contiguous request (got %lld)",
             (long long)r64);
        if (r64 > 0) served += r64;
        /* Drain whatever is left, then ask once more. */
        while (served < (int64_t)(8 * MIB)) {
            int64_t g = donor_extend(pB, fdC, (uint64_t)served, 1 * MIB);
            if (g <= 0) break;
            served += g;
        }
        T_OK(served <= (int64_t)(4 * MIB),
             "a 1 x 4 MiB pool never hands out more than 4 MiB in total (handed out %lld bytes)",
             (long long)served);
        r64 = donor_extend(pB, fdC, (uint64_t)served, 1 * MIB);
        T_OK(r64 <= 0,
             "an exhausted pool refuses further extends: 0 or a negative errno (got %lld)",
             (long long)r64);
        T_OK(r64 == 0 || r64 == -ENOSPC,
             "the exhausted-pool return value is 0 or -ENOSPC (got %lld)", (long long)r64);
        T_EQ(served, (int64_t)(4 * MIB),
             "the pool did hand out its whole 4 MiB before refusing");
    }

    /* =====================================================================
     * E. donor_reclaim
     * ===================================================================== */
    r64 = donor_reclaim(NULL, fdA, 0);
    T_EQ(r64, (int64_t)(-EINVAL), "donor_reclaim(pool=NULL) returns -EINVAL (got %lld)",
         (long long)r64);
    r64 = donor_reclaim(pA, -1, 0);
    T_OK(r64 < 0, "donor_reclaim on fd -1 returns a negative errno (got %lld)", (long long)r64);

    {   /* reclaiming past EOF must be a no-op, not a truncation */
        uint64_t a_before = alloc_bytes(fdA), s_before = file_size(fdA);
        rc = donor_reclaim(pA, fdA, 64 * MIB);
        T_EQ(rc, 0, "reclaim from an offset past EOF succeeds as a no-op (got %d)", rc);
        T_EQ(alloc_bytes(fdA), (int64_t)a_before,
             "reclaim from an offset past EOF frees nothing (%llu KiB before and after)",
             (unsigned long long)(a_before / KIB));
        T_EQ(file_size(fdA), (int64_t)s_before, "reclaim past EOF does not change the file size");
    }

    {   /* reclaim the tail, keep the head */
        uint64_t a_before = alloc_bytes(fdA), freed;
        rc = donor_reclaim(pA, fdA, 1 * MIB);
        T_EQ(rc, 0, "donor_reclaim(from_off=1 MiB) returns 0 (got %d)", rc);
        freed = a_before - alloc_bytes(fdA);
        T_OK(alloc_bytes(fdA) <= a_before,
             "reclaim did not grow the target's allocation");
        T_OK(freed >= 1 * MIB - 32 * KIB,
             "reclaim gave back >= 1 MiB of the target's blocks (freed %llu KiB of %llu KiB)",
             (unsigned long long)(freed / KIB), (unsigned long long)(a_before / KIB));
        memset(rb, 0xAA, 1 * MIB);
        T_EQ(pread(fdA, rb, 1 * MIB, 0), (int64_t)(1 * MIB),
             "the retained head region is still readable after reclaim");
        T_OK(all_bytes_are(rb, 1 * MIB, 0x5A),
             "reclaim did not damage data below from_off (0x5A pattern intact)");
    }

    if (pB) {   /* reclaim returns space to the pool, not just to the file */
        uint64_t held = alloc_bytes(fdC);
        /* dirty the donated space first, so the reuse test also proves that
         * recycled space is not handed out with the old bytes still in it */
        rc = fill_and_read(fdC, 0, 1 * MIB, 0x5A, rb);
        T_EQ(rc, 0, "wrote a 0x5A pattern into log-c's donated space");
        T_OK(held >= 4 * MIB, "log-c holds the pool's whole 4 MiB (%llu KiB)",
             (unsigned long long)(held / KIB));
        rc = donor_reclaim(pB, fdC, 0);
        T_EQ(rc, 0, "donor_reclaim(from_off=0) on log-c returns 0 (got %d)", rc);
        T_OK(alloc_bytes(fdC) <= 32 * KIB,
             "log-c gave all of its blocks back (%llu KiB left)",
             (unsigned long long)(alloc_bytes(fdC) / KIB));

        fdC2 = open_target("log-c2", O_RDWR | O_CREAT | O_TRUNC);
        T_OK(fdC2 >= 0, "created target log-c2");
        r64 = donor_extend(pB, fdC2, 0, 4 * MIB);
        T_EQ(r64, (int64_t)(4 * MIB),
             "after reclaim the exhausted pool can serve a full 4 MiB again (got %lld)",
             (long long)r64);
        memset(rb, 0xAA, 1 * MIB);
        T_EQ(pread(fdC2, rb, 1 * MIB, 0), (int64_t)(1 * MIB),
             "the recycled region is readable");
        T_OK(all_bytes_are(rb, 1 * MIB, 0x00),
             "recycled donor space reads as zeros: the old 0x5A bytes are not leaked");
    }

    /* =====================================================================
     * F. donor_next_chunk: 4x the moving average of observed write sizes.
     * Fresh pools with no extends, so nothing else perturbs the average.
     * Donors are 32 MiB so no plausible clamp to donor size can fire for the
     * values used here (largest expectation is 4 MiB).
     * ===================================================================== */
    pC = donor_pool_create(dirC, 1, 32 * MIB);
    T_OK(pC != NULL, "pool_create(%s, 1, 32 MiB) succeeds for the chunk tests", dirC);
    if (pC) {
        uint64_t v, i, bad = 0;
        v = donor_next_chunk(pC, 64 * KIB);
        T_EQ(v, (int64_t)(4 * 64 * KIB),
             "first observed write of 64 KiB gives a chunk of 256 KiB (4x) (got %llu)",
             (unsigned long long)v);
        for (i = 0; i < 10; i++) {
            v = donor_next_chunk(pC, 64 * KIB);
            if (v != 4 * 64 * KIB) bad++;
        }
        T_EQ(bad, 0, "a constant 64 KiB series keeps the chunk at exactly 256 KiB "
                     "for 10 more observations (%llu deviations)", (unsigned long long)bad);
        /* a single zero-sized observation must not zero or explode the chunk */
        v = donor_next_chunk(pC, 0);
        T_OK(v > 0 && v <= 4 * 64 * KIB,
             "one zero-sized observation keeps the chunk in (0, 256 KiB] (got %llu)",
             (unsigned long long)v);
    }

    pD = donor_pool_create(dirD, 1, 32 * MIB);
    T_OK(pD != NULL, "pool_create(%s, 1, 32 MiB) succeeds for the averaging series", dirD);
    if (pD) {
        const uint64_t series[4] = { 8 * KIB, 16 * KIB, 24 * KIB, 32 * KIB };
        uint64_t v = 0, lo, hi, i, small, large, out_of_range = 0;

        for (i = 0; i < 4; i++) {
            uint64_t j, mn = series[0], mx = series[0];
            v = donor_next_chunk(pD, series[i]);
            for (j = 0; j <= i; j++) {
                if (series[j] < mn) mn = series[j];
                if (series[j] > mx) mx = series[j];
            }
            if (v < 4 * mn || v > 4 * mx) out_of_range++;
        }
        T_EQ(out_of_range, 0,
             "every chunk in the 8/16/24/32 KiB series stays within 4x[min,max] observed");
        lo = 4 * 8 * KIB; hi = 4 * 32 * KIB;
        T_OK(v >= lo && v <= hi,
             "after 8,16,24,32 KiB the chunk is between 32 KiB and 128 KiB (got %llu KiB)",
             (unsigned long long)(v / KIB));
        small = v;

        for (i = 0; i < 32; i++) v = donor_next_chunk(pD, 1 * MIB);
        large = v;
        T_OK(large > small,
             "the average tracks a shift to 1 MiB writes: chunk rose from %llu KiB to %llu KiB",
             (unsigned long long)(small / KIB), (unsigned long long)(large / KIB));
        T_OK(large <= 4 * MIB,
             "the chunk never exceeds 4x the largest observed write, 4 MiB (got %llu KiB)",
             (unsigned long long)(large / KIB));
        T_OK(large >= 4 * 8 * KIB,
             "the chunk never drops below 4x the smallest observed write, 32 KiB (got %llu KiB)",
             (unsigned long long)(large / KIB));
        /* 32 identical 1 MiB writes: any bounded-window or exponential average
         * has converged; a plain cumulative average has not, so only the
         * bounds above are asserted as equality-free. */
        T_OK(donor_next_chunk(pD, 1 * MIB) > 0,
             "donor_next_chunk keeps returning a nonzero chunk");
    }

    T_EQ(donor_next_chunk(NULL, 64 * KIB), 0,
         "donor_next_chunk(pool=NULL) returns 0 rather than crashing");

    /* =====================================================================
     * G. donor_pool_destroy releases the pool's space
     * ===================================================================== */
    free0 = fs_free(g_mnt);
    pE = donor_pool_create(dirE, 2, 8 * MIB);
    T_OK(pE != NULL, "pool_create(%s, 2, 8 MiB) succeeds", dirE);
    free1 = fs_free(g_mnt);
    T_OK(free0 - free1 >= 16 * MIB - 256 * KIB,
         "that pool reserved >= 16 MiB (took %llu MiB)",
         (unsigned long long)((free0 - free1) / MIB));
    donor_pool_destroy(pE);
    pE = NULL;
    free2 = fs_free(g_mnt);
    T_OK(free2 + 4 * MIB >= free0,
         "donor_pool_destroy returned the pool's space to the filesystem "
         "(%llu MiB free before create, %llu MiB after destroy)",
         (unsigned long long)(free0 / MIB), (unsigned long long)(free2 / MIB));

    donor_pool_destroy(NULL);
    T_OK(1, "donor_pool_destroy(NULL) is a safe no-op");

    /* =====================================================================
     * Z. Pool accounting: the pool hands out exactly what it owns, no more.
     *
     * A donation consumes a free run and nothing puts blocks back, so the total
     * handed out over the life of a pool must equal its nominal size and the
     * next request must be refused. Worth pinning down because the number that
     * looks like over-donation elsewhere -- donor_async's prepared_to -- is a
     * FILE OFFSET that follows the writer, not a count of pool bytes, and the
     * two were compared against each other once already.
     * ===================================================================== */
    {
        char zdir[600], ztgt[600];
        struct donor_pool *pz;
        int fdz;
        unsigned long long donated = 0;
        int rounds = 0;
        int64_t got = 0;

        snprintf(zdir, sizeof zdir, "%s/donorsZ", g_mnt);
        snprintf(ztgt, sizeof ztgt, "%s/targetZ.bin", g_mnt);
        (void)mkdir(zdir, 0700);
        pz = donor_pool_create(zdir, 2, 16 * MIB);
        T_OK(pz != NULL, "Z: pool of 2 x 16 MiB created");
        fdz = open(ztgt, O_RDWR | O_CREAT | O_TRUNC, 0644);
        T_OK(fdz >= 0, "Z: donation target created");
        if (pz && fdz >= 0) {
            while (rounds < 4096) {
                got = donor_extend(pz, fdz, donated, 4 * MIB);
                if (got <= 0)
                    break;
                donated += (unsigned long long)got;
                rounds++;
            }
            T_OK(got < 0,
                 "Z: the pool refuses once it is empty (rc=%lld after %d donations)",
                 (long long)got, rounds);
            T_OK(donated == 2ull * 16ull * MIB,
                 "Z: total handed out is exactly the pool's size (%llu bytes, nominal %llu)",
                 donated, (unsigned long long)(2ull * 16ull * MIB));
        }
        if (fdz >= 0) { close(fdz); unlink(ztgt); }
        donor_pool_destroy(pz);
    }

    /* =====================================================================
     * cleanup
     * ===================================================================== */
    donor_pool_destroy(pA);
    donor_pool_destroy(pB);
    donor_pool_destroy(pC);
    donor_pool_destroy(pD);
    if (fdA >= 0) close(fdA);
    if (fdC >= 0) close(fdC);
    if (fdC2 >= 0) close(fdC2);
    if (fdD >= 0) close(fdD);
    if (fdRO >= 0) close(fdRO);
    if (fdDir >= 0) close(fdDir);
    if (fdX >= 0) { close(fdX); unlink(xdev_path); }
    free(rb);

    fixture_down();
    T_DONE();
}
