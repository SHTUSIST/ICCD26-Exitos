/* Tier 1 integration tests for the intercept module.
 *
 * Fixture: an ext4 image on a LOOP device, built by tests/harness/loopfix.sh.
 * Never a physical disk. The image path and mount point carry this process's
 * pid so parallel test runs cannot collide, and teardown is registered with
 * atexit() so the loop device is released even on an early exit.
 *
 * HARD SAFETY GATE: before a single fd is registered, every test file is
 * checked to be on a filesystem whose backing block device is named "loop*"
 * (resolved through /sys/dev/block/<major>:<minor>). If that check does not
 * pass, the whole file skips instead of registering anything. On this machine
 * / and /tmp live on /dev/sda4, a real partition, and registering a file there
 * would arm a raw-LBA write path on physical media.
 *
 * What is tested here (the half the unit tier cannot reach, because the
 * header exposes no way to inject a hand-built Maco):
 *
 *   happy path   - a registered fd whose write is fully inside one mapped run
 *                  returns EXITOS_TAKEOVER and sets *result to the byte count,
 *                  and the bytes really land at that LBA (O_DIRECT readback);
 *   hole         - a write landing in a Maco hole returns EXITOS_PASS;
 *   straddle     - a write crossing the end of a mapped run returns
 *                  EXITOS_PASS rather than a half-right LBA;
 *   bounds       - exitos_lba_in_bounds rejects one block below the range,
 *                  one block above it, a run straddling the top, LBA 0,
 *                  and another file's LBA (the cross-file corruption guard);
 *   truncate     - exitos_on_ftruncate invalidates the affected range, so a
 *                  later write there PASSes and the stale LBA is out of
 *                  bounds;
 *   setup refresh- a native-fallocated UNWRITTEN range is rejected, then the
 *                  same initialized range is synchronously refreshed while a
 *                  transaction is held and its first 8 KiB write performs no
 *                  additional timed refresh;
 *   unregister   - after unregistering, the fd is back on the normal path.
 *
 * The expected LBAs are computed independently, straight from FIEMAP plus the
 * device's logical block size, so this test is an oracle rather than a mirror
 * of whatever the implementation happens to do.
 */
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
#include <sys/wait.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

#include "exitos_intercept.h"
#include "exitos_stats.h"
#include "tap.h"

#define RES_SENTINEL   ((ssize_t)-424242)
#define IRES_SENTINEL  (-4242)
#define MAX_RUNS       64
#define A_SIZE         (1u << 20)     /* file A: 1 MiB, fully written        */
#define B_HEAD         (64u << 10)    /* file B: 64 KiB at 0 ...             */
#define B_TAIL_OFF     (1u << 20)     /* ... then a hole, then 64 KiB at 1MiB */

struct run { uint64_t off, phys, len; uint32_t flags; };

static char g_img[256], g_mnt[256], g_loop[64];
static int  g_up;
static int  g_fdA = -1, g_fdB = -1, g_fdC = -1, g_fdD = -1;

/* --------------------------------------------------------------- fixture */

static void teardown(void)
{
    char cmd[2048];
    int rc, n;

    /* Any fd still open on the image keeps umount busy and the loop device
     * attached, so the fds are closed here and not only on the success path. */
    if (g_fdA >= 0) { close(g_fdA); g_fdA = -1; }
    if (g_fdB >= 0) { close(g_fdB); g_fdB = -1; }
    if (g_fdC >= 0) { close(g_fdC); g_fdC = -1; }
    if (g_fdD >= 0) { close(g_fdD); g_fdD = -1; }
    if (!g_up) return;
    g_up = 0;
    /* Torn down by hand rather than via "loopfix.sh down": that path reads the
     * shared /tmp/exitos-loop.dev, which a concurrently running test may have
     * overwritten with its own loop device. Only our own loop is detached. */
    n = snprintf(cmd, sizeof cmd,
                 "umount %s 2>/dev/null || umount -l %s 2>/dev/null; "
                 "losetup -d %s 2>/dev/null; "
                 "rm -f %s; rmdir %s 2>/dev/null; true",
                 g_mnt, g_mnt, g_loop, g_img, g_mnt);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        /* Never run a half-written shell command. */
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

/* Brings up the ext4-on-loop fixture. Returns 0 on success. */
static int fixture_up(void)
{
    const char *fix = loopfix_path();
    char cmd[2048], line[512];
    FILE *f;
    int n;

    if (!fix) return -1;
    snprintf(g_img, sizeof g_img, "/tmp/exitos-intercept-%d.img", (int)getpid());
    snprintf(g_mnt, sizeof g_mnt, "/tmp/exitos-mnt-intercept-%d", (int)getpid());
    n = snprintf(cmd, sizeof cmd,
                 "EXITOS_IMG=%s EXITOS_MNT=%s EXITOS_SIZE_MB=64 %s up 2>&1",
                 g_img, g_mnt, fix);
    if (n < 0 || (size_t)n >= sizeof cmd) return -1;
    f = popen(cmd, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "LOOP=", 5) == 0)
            sscanf(line, "LOOP=%63s", g_loop);
        else
            printf("# loopfix: %s", line);
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
    if (major(st.st_dev) == 0) return -1;      /* no block device at all */
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
    fd = open(path, O_RDONLY);        /* read-only, and only ever a loop dev */
    if (fd < 0) return 0;
    if (ioctl(fd, BLKSSZGET, &ss) != 0) ss = 0;
    close(fd);
    return (uint32_t)ss;
}

/* /sys/class/block/<dev>/start, in 512-byte sectors. Absent for whole
 * devices such as a loop device, in which case the answer is 0. */
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

static int fiemap_runs(int fd, struct run *out, int max)
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

static uint64_t g_lbs, g_part_start_sect, g_fs_bs;

static uint64_t lba_of(uint64_t phys_bytes)
{
    return g_part_start_sect * 512 / g_lbs + phys_bytes / g_lbs;
}

/* ---------------------------------------------------------------- files */

static int make_file(const char *path, const uint64_t *offs, const uint64_t *lens,
                     int n, int fill)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    char *buf;
    int i;

    if (fd < 0) return -1;
    buf = malloc(1u << 20);
    if (!buf) { close(fd); return -1; }
    memset(buf, fill, 1u << 20);
    for (i = 0; i < n; i++) {
        uint64_t done = 0;
        while (done < lens[i]) {
            size_t chunk = (size_t)((lens[i] - done > (1u << 20))
                                    ? (1u << 20) : (lens[i] - done));
            if (pwrite(fd, buf, chunk, (off_t)(offs[i] + done)) != (ssize_t)chunk) {
                free(buf); close(fd); return -1;
            }
            done += chunk;
        }
    }
    free(buf);
    if (fsync(fd) != 0) { close(fd); return -1; }
    return fd;
}

/* A registered fd must never report a successful takeover of a bogus write.
 * Run in a child so a crash costs one assertion, not the whole run. */
static int bogus_write_child(struct exitos_ctx *c, int fd, const void *buf,
                             size_t count, off_t off)
{
    pid_t p = fork();
    int st = 0;

    if (p < 0) return -1;
    if (p == 0) {
        ssize_t res = RES_SENTINEL;
        exitos_decision d = exitos_on_write(c, fd, buf, count, off, &res);
        /* Legal: PASS. Also legal: TAKEOVER of 0 bytes. Never legal: claiming
         * `count` bytes were written from a buffer that cannot supply them. */
        if (d == EXITOS_TAKEOVER && res != 0) _exit(1);
        _exit(0);
    }
    if (waitpid(p, &st, 0) < 0) return -1;
    if (!WIFEXITED(st)) return -2;
    return WEXITSTATUS(st);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    struct exitos_ctx *c = NULL;
    char pathA[320], pathB[320], pathC[320], pathD[320], devname[64];
    int fdA = -1, fdB = -1, fdC = -1, fdD = -1;
    struct run ra[MAX_RUNS], rb[MAX_RUNS];
    int na, nb, i;
    struct statfs sfs;
    uint64_t a_first_lba = 0, a_end_lba = 0, b_first_lba = 0, stale_lba = 0;
    uint64_t fsblk_lbas, hole_off = 0, straddle_off = 0, ctl_off = 0, ctl_len = 0;
    char *wbuf = NULL;
    ssize_t res;
    int ires, rc;
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

    /* file A: 1 MiB, contiguous, every block written (no unwritten extents) */
    snprintf(pathA, sizeof pathA, "%s/logA", g_mnt);
    /* file B: 64 KiB at 0, a hole, 64 KiB at 1 MiB -> a real Maco hole */
    snprintf(pathB, sizeof pathB, "%s/logB", g_mnt);
    /* file C: exists, never registered */
    snprintf(pathC, sizeof pathC, "%s/logC", g_mnt);
    /* file D: registered empty, then prepared after registration exactly as a
     * setup-time intercepted native fallocate would be. */
    snprintf(pathD, sizeof pathD, "%s/logD", g_mnt);
    {
        uint64_t offA[1] = { 0 }, lenA[1] = { A_SIZE };
        uint64_t offB[2] = { 0, B_TAIL_OFF }, lenB[2] = { B_HEAD, B_HEAD };
        uint64_t offC[1] = { 0 }, lenC[1] = { B_HEAD };
        fdA = make_file(pathA, offA, lenA, 1, 0x11);
        fdB = make_file(pathB, offB, lenB, 2, 0x22);
        fdC = make_file(pathC, offC, lenC, 1, 0x33);
        fdD = open(pathD, O_RDWR | O_CREAT | O_TRUNC, 0600);
        /* published so teardown() can close them on every exit path */
        g_fdA = fdA; g_fdB = fdB; g_fdC = fdC; g_fdD = fdD;
    }
    T_OK(fdA >= 0 && fdB >= 0 && fdC >= 0 && fdD >= 0,
         "fixture: created logA (1 MiB), logB (sparse), logC and empty logD on the loop fs");
    if (fdA < 0 || fdB < 0 || fdC < 0 || fdD < 0) { teardown(); T_DONE(); }

    /* ---- HARD SAFETY GATE: everything below must be on a loop device ---- */
    if (backing_device(fdA, devname, sizeof devname) != 0 ||
        strncmp(devname, "loop", 4) != 0) {
        T_SKIP("logA is backed by '%s', not a loop device - refusing to "
               "register anything (a raw write path on physical media)",
               devname[0] ? devname : "?");
        teardown();
        T_DONE();
    }
    T_OK(1, "safety gate: test files are backed by loop device %s", devname);

    g_lbs = device_lbs(devname);
    g_part_start_sect = device_part_start(devname);
    if (statfs(pathA, &sfs) == 0 && sfs.f_bsize > 0) g_fs_bs = (uint64_t)sfs.f_bsize;
    T_OK(g_lbs == 512 || g_lbs == 4096,
         "oracle: device logical block size is %llu bytes",
         (unsigned long long)g_lbs);
    T_EQ(g_fs_bs, 4096, "oracle: ext4 block size is 4096 bytes");
    if (!g_lbs || !g_fs_bs) { teardown(); T_DONE(); }
    fsblk_lbas = g_fs_bs / g_lbs;

    na = fiemap_runs(fdA, ra, MAX_RUNS);
    nb = fiemap_runs(fdB, rb, MAX_RUNS);
    T_OK(na >= 1, "oracle: FIEMAP found %d extent(s) for logA", na);
    T_OK(nb >= 2, "oracle: FIEMAP found %d extent(s) for logB "
                  "(need >= 2 so a hole exists between them)", nb);
    if (na < 1 || nb < 2) { teardown(); T_DONE(); }
    for (i = 0; i < na; i++)
        printf("# logA run %d: off=%llu phys=%llu len=%llu\n", i,
               (unsigned long long)ra[i].off, (unsigned long long)ra[i].phys,
               (unsigned long long)ra[i].len);
    for (i = 0; i < nb; i++)
        printf("# logB run %d: off=%llu phys=%llu len=%llu\n", i,
               (unsigned long long)rb[i].off, (unsigned long long)rb[i].phys,
               (unsigned long long)rb[i].len);

    a_first_lba = lba_of(ra[0].phys);
    a_end_lba   = lba_of(ra[na - 1].phys + ra[na - 1].len);   /* one past end */
    b_first_lba = lba_of(rb[0].phys);

    /* A run of at least 64 KiB inside logA, used as the positive control. */
    for (i = 0; i < na; i++)
        if (ra[i].len >= (64u << 10)) { ctl_off = ra[i].off; ctl_len = 64u << 10; break; }
    T_OK(ctl_len == (64u << 10),
         "oracle: found a >= 64 KiB contiguous run in logA at file offset %llu",
         (unsigned long long)ctl_off);

    /* The hole in logB, and an offset that straddles the end of run 0. */
    T_OK(rb[1].off > rb[0].off + rb[0].len,
         "oracle: logB really has a hole between %llu and %llu",
         (unsigned long long)(rb[0].off + rb[0].len),
         (unsigned long long)rb[1].off);
    hole_off     = rb[0].off + rb[0].len + 4096;
    straddle_off = rb[0].off + rb[0].len - 4096;

    /* The LBA that maps logA's 512 KiB offset, needed by the truncate test. */
    for (i = 0; i < na; i++)
        if (ra[i].off <= (512u << 10) && (512u << 10) < ra[i].off + ra[i].len) {
            stale_lba = lba_of(ra[i].phys + ((512u << 10) - ra[i].off));
            break;
        }
    T_OK(stale_lba != 0, "oracle: logA offset 512 KiB maps to LBA %llu",
         (unsigned long long)stale_lba);

    if (posix_memalign((void **)&wbuf, 4096, 1u << 20) != 0 || !wbuf) {
        T_SKIP("posix_memalign failed");
        teardown();
        T_DONE();
    }
    memset(wbuf, 0x77, 1u << 20);

    c = exitos_ctx_create();
    T_OK(c != NULL, "exitos_ctx_create returns a context");
    if (!c) { teardown(); T_DONE(); }

    /* ---- before registration, logA must be on the normal path ---------- */
    res = RES_SENTINEL;
    T_EQ(exitos_on_write(c, fdA, wbuf, 4096, (off_t)ctl_off, &res), EXITOS_PASS,
         "unregistered logA: write inside a mapped run still PASSes");
    T_EQ(res, RES_SENTINEL, "unregistered logA: *result untouched");
    T_EQ(exitos_lba_in_bounds(c, fdA, a_first_lba, (size_t)g_fs_bs), 0,
         "unregistered logA: its own first LBA is not in bounds");

    /* ---- registration on the loop device must succeed ------------------ */
    rc = exitos_register_fd(c, fdA);
    T_EQ(rc, 0, "exitos_register_fd(logA on loop) returns 0");
    if (rc != 0) {
        T_SKIP("logA not registered; the fast-path tests below cannot run");
        exitos_ctx_destroy(c);
        teardown();
        T_DONE();
    }
    rc = exitos_register_fd(c, fdB);
    T_EQ(rc, 0, "exitos_register_fd(logB on loop) returns 0");
    rc = exitos_register_fd(c, fdD);
    T_EQ(rc, 0, "exitos_register_fd(empty logD on loop) returns 0");

    /* ---- setup-time refresh after native fallocate/initialization ------ */
    if (rc == 0) {
        struct exitos_fd_txn tx;
        uint64_t refresh_after_setup, refresh_after_first_write;

        T_EQ(fallocate(fdD, 0, 0, 8192), 0,
             "native fallocate allocates the setup range on loop/ext4");
        T_EQ(exitos_fd_txn_begin(c, fdD, &tx), 0,
             "began held transaction for the UNWRITTEN setup range");
        T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, 8192), -ENXIO,
             "native-fallocated but UNWRITTEN bytes are not declared raw-write safe");
        exitos_fd_txn_end(&tx);

        T_EQ(pwrite(fdD, wbuf, 8192, 0), 8192,
             "initialized the full 8 KiB setup range through ext4");
        T_EQ(fdatasync(fdD), 0,
             "persisted setup initialization before publishing its mapping");
        T_EQ(exitos_fd_txn_begin(c, fdD, &tx), 0,
             "began held setup transaction after initialization");
        T_EQ(exitos_txn_refresh_mapping_range(&tx, 0, 8192), 0,
             "initialized 8 KiB range refreshes and validates in the held transaction");
        exitos_fd_txn_end(&tx);

        refresh_after_setup = exitos_stat_get(EXITOS_STAT_REFRESH);
        res = RES_SENTINEL;
        d = exitos_on_write(c, fdD, wbuf, 8192, 0, &res);
        T_EQ(d, EXITOS_TAKEOVER,
             "first 8 KiB write after setup refresh takes the raw fast path");
        T_EQ(res, 8192, "first post-setup write reports all 8192 bytes");
        refresh_after_first_write = exitos_stat_get(EXITOS_STAT_REFRESH);
        T_EQ(refresh_after_first_write, refresh_after_setup,
             "first timed 8 KiB write does not issue another FIEMAP refresh");
    }

    /* ---- happy path: fully covered write ------------------------------- */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdA, wbuf, 4096, (off_t)ctl_off, &res);
    T_EQ(d, EXITOS_TAKEOVER, "covered 4096-byte write -> EXITOS_TAKEOVER");
    T_EQ(res, 4096, "covered 4096-byte write sets *result to 4096");

    memcpy(wbuf, "EXITOS-A", 8);
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdA, wbuf, (size_t)ctl_len, (off_t)ctl_off, &res);
    T_EQ(d, EXITOS_TAKEOVER, "covered 65536-byte write -> EXITOS_TAKEOVER");
    T_EQ(res, (ssize_t)ctl_len, "covered 65536-byte write sets *result to 65536");

    /* ---- and the bytes must be at that LBA, not somewhere else --------- */
    ires = IRES_SENTINEL;
    d = exitos_on_fdatasync(c, fdA, &ires);
    T_OK(d == EXITOS_PASS || (d == EXITOS_TAKEOVER && ires == 0),
         "registered fdatasync: PASS, or TAKEOVER with *result == 0 (d=%d res=%d)",
         (int)d, ires);
    if (d == EXITOS_PASS) fdatasync(fdA);
    {
        int rfd = open(pathA, O_RDONLY | O_DIRECT);
        if (rfd < 0) {
            T_SKIP("O_DIRECT open failed (%s): cannot verify the bytes landed",
                   strerror(errno));
        } else {
            char *rbuf = NULL;
            if (posix_memalign((void **)&rbuf, 4096, 4096) != 0 || !rbuf) {
                T_SKIP("posix_memalign for the readback buffer failed");
            } else {
                ssize_t got = pread(rfd, rbuf, 4096, (off_t)ctl_off);
                T_EQ(got, 4096, "readback: pread of 4096 bytes at offset %llu",
                     (unsigned long long)ctl_off);
                T_EQ(memcmp(rbuf, wbuf, 4096), 0,
                     "readback: the taken-over write landed at the right LBA");
                free(rbuf);
            }
            close(rfd);
        }
    }

    /* ---- refusal: a write that falls in a Maco hole -------------------- */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdB, wbuf, 4096, (off_t)hole_off, &res);
    T_EQ(d, EXITOS_PASS, "write at %llu (inside logB's hole) -> EXITOS_PASS",
         (unsigned long long)hole_off);
    T_EQ(res, RES_SENTINEL, "hole write left *result untouched");

    /* The passed write left kernel-side debt on this buffered
     * registration; model the application's durability point (real fdatasync
     * + the frontend's note) before expecting the shortcut again. */
    T_EQ(fdatasync(fdB), 0, "durability point after the passed hole write");
    exitos_note_kernel_sync(c, fdB);

    /* positive control on the same file, so "always PASS" cannot pass ---- */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdB, wbuf, 4096, (off_t)rb[0].off, &res);
    T_EQ(d, EXITOS_TAKEOVER, "logB control: write inside run 0 -> TAKEOVER");
    T_EQ(res, 4096, "logB control: *result == 4096");

    /* ---- refusal: a write straddling the end of a mapped run ----------- */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdB, wbuf, 8192, (off_t)straddle_off, &res);
    T_EQ(d, EXITOS_PASS,
         "8192-byte write at %llu straddles the end of run 0 -> EXITOS_PASS",
         (unsigned long long)straddle_off);
    T_EQ(res, RES_SENTINEL, "straddling write left *result untouched");

    /* straddling EOF of logA (last mapped block, then past the end) */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdA, wbuf, 8192, (off_t)(A_SIZE - 4096), &res);
    T_EQ(d, EXITOS_PASS, "write straddling logA's EOF -> EXITOS_PASS");
    T_EQ(res, RES_SENTINEL, "EOF-straddling write left *result untouched");

    /* entirely past the mapped range */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdA, wbuf, 4096, (off_t)(8u << 20), &res);
    T_EQ(d, EXITOS_PASS, "write at 8 MiB (logA is 1 MiB) -> EXITOS_PASS");
    T_EQ(res, RES_SENTINEL, "past-the-end write left *result untouched");

    /* ---- refusal: writes that cannot be expressed as whole LBAs -------- */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdA, wbuf, 100, (off_t)ctl_off, &res);
    T_EQ(d, EXITOS_PASS, "100-byte write (not a multiple of the block size) "
                         "-> EXITOS_PASS");
    T_EQ(res, RES_SENTINEL, "short unaligned write left *result untouched");

    res = RES_SENTINEL;
    d = exitos_on_write(c, fdA, wbuf, 4095, (off_t)(ctl_off + 1), &res);
    T_EQ(d, EXITOS_PASS, "write at a byte-misaligned offset -> EXITOS_PASS");
    T_EQ(res, RES_SENTINEL, "misaligned-offset write left *result untouched");

    /* ---- refusal: an fd this context does not know --------------------- */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdC, wbuf, 4096, 0, &res);
    T_EQ(d, EXITOS_PASS, "unregistered logC on the same filesystem -> EXITOS_PASS");
    T_EQ(res, RES_SENTINEL, "unregistered logC left *result untouched");
    T_EQ(exitos_on_fdatasync(c, fdC, &ires), EXITOS_PASS,
         "unregistered logC: fdatasync -> EXITOS_PASS");

    /* ---- exitos_lba_in_bounds: the anti-corruption guard ---------------- */
    T_OK(exitos_lba_in_bounds(c, fdA, a_first_lba, (size_t)g_fs_bs) != 0,
         "in_bounds: logA's own first LBA %llu accepted",
         (unsigned long long)a_first_lba);
    T_OK(exitos_lba_in_bounds(c, fdA, a_end_lba - fsblk_lbas,
                              (size_t)g_fs_bs) != 0,
         "in_bounds: logA's own last block accepted");
    if (a_first_lba > fsblk_lbas)
        T_EQ(exitos_lba_in_bounds(c, fdA, a_first_lba - fsblk_lbas,
                                  (size_t)g_fs_bs), 0,
             "in_bounds: one block BELOW logA's range (%llu) rejected",
             (unsigned long long)(a_first_lba - fsblk_lbas));
    else
        T_SKIP("logA starts at LBA %llu, no block below it to test",
               (unsigned long long)a_first_lba);
    T_EQ(exitos_lba_in_bounds(c, fdA, a_end_lba, (size_t)g_fs_bs), 0,
         "in_bounds: one block ABOVE logA's range (%llu) rejected",
         (unsigned long long)a_end_lba);
    T_EQ(exitos_lba_in_bounds(c, fdA, a_end_lba - fsblk_lbas,
                              (size_t)(g_fs_bs * 2)), 0,
         "in_bounds: a run that starts inside logA but ends past its top "
         "is rejected");
    T_EQ(exitos_lba_in_bounds(c, fdA, 0, (size_t)g_fs_bs), 0,
         "in_bounds: LBA 0 (filesystem superblock area) rejected for logA");
    T_EQ(exitos_lba_in_bounds(c, fdA, (uint64_t)1 << 40, (size_t)g_fs_bs), 0,
         "in_bounds: an LBA far past the device rejected for logA");
    T_EQ(exitos_lba_in_bounds(c, fdA, b_first_lba, (size_t)g_fs_bs), 0,
         "in_bounds: logB's first LBA %llu rejected for logA "
         "(cross-file corruption guard)", (unsigned long long)b_first_lba);
    T_EQ(exitos_lba_in_bounds(c, fdC, a_first_lba, (size_t)g_fs_bs), 0,
         "in_bounds: unregistered logC never has any LBA in bounds");

    /* ---- a registered fd must not claim a bogus write succeeded -------- */
    T_EQ(bogus_write_child(c, fdA, NULL, 4096, (off_t)ctl_off), 0,
         "NULL buffer on a registered fd: no TAKEOVER claiming bytes written");
    T_EQ(bogus_write_child(c, fdA, wbuf, 0, (off_t)ctl_off), 0,
         "zero-length write on a registered fd: no TAKEOVER of nonzero bytes");

    /* ---- ftruncate must invalidate the affected Maco range -------------- */
    T_OK(exitos_lba_in_bounds(c, fdA, stale_lba, (size_t)g_fs_bs) != 0,
         "pre-truncate: the LBA behind logA's 512 KiB offset is in bounds");

    ires = IRES_SENTINEL;
    d = exitos_on_ftruncate(c, fdA, 65536, &ires);
    if (d == EXITOS_TAKEOVER)
        T_EQ(ires, 0, "on_ftruncate took over and reported success");
    else
        T_EQ(ftruncate(fdA, 65536), 0,
             "on_ftruncate PASSed; the caller's real ftruncate succeeded");
    {
        struct stat st;
        T_EQ(fstat(fdA, &st), 0, "fstat after truncate");
        T_EQ(st.st_size, 65536, "logA is now 65536 bytes");
    }

    res = RES_SENTINEL;
    d = exitos_on_write(c, fdA, wbuf, 4096, (off_t)(512u << 10), &res);
    T_EQ(d, EXITOS_PASS,
         "post-truncate: a write at 512 KiB (truncated away) -> EXITOS_PASS");
    T_EQ(res, RES_SENTINEL, "post-truncate write left *result untouched");
    T_EQ(exitos_lba_in_bounds(c, fdA, stale_lba, (size_t)g_fs_bs), 0,
         "post-truncate: the stale LBA %llu is out of bounds - it may now "
         "belong to another file", (unsigned long long)stale_lba);
    T_EQ(exitos_lba_in_bounds(c, fdA, a_end_lba - fsblk_lbas,
                              (size_t)g_fs_bs), 0,
         "post-truncate: logA's old last block is out of bounds");

    /* The surviving range may stay mapped or be dropped wholesale; both are
     * safe. What is never allowed is a takeover reporting the wrong count. */
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdA, wbuf, 4096, 0, &res);
    T_OK(d == EXITOS_PASS || (d == EXITOS_TAKEOVER && res == 4096),
         "post-truncate: a write inside the surviving range is either PASSed "
         "or taken over with *result == 4096 (d=%d res=%lld)",
         (int)d, (long long)res);

    /* ---- unregister puts the fd back on the normal path ---------------- */
    T_EQ(exitos_unregister_fd(c, fdB), 0, "exitos_unregister_fd(logB) returns 0");
    T_EQ(exitos_unregister_fd(c, fdD), 0, "exitos_unregister_fd(logD) returns 0");
    res = RES_SENTINEL;
    d = exitos_on_write(c, fdB, wbuf, 4096, (off_t)rb[0].off, &res);
    T_EQ(d, EXITOS_PASS, "after unregister, a covered write on logB -> EXITOS_PASS");
    T_EQ(res, RES_SENTINEL, "after unregister, *result untouched");
    T_EQ(exitos_lba_in_bounds(c, fdB, b_first_lba, (size_t)g_fs_bs), 0,
         "after unregister, logB's own LBA is no longer in bounds");
    ires = IRES_SENTINEL;
    T_EQ(exitos_on_fdatasync(c, fdB, &ires), EXITOS_PASS,
         "after unregister, fdatasync on logB -> EXITOS_PASS");
    T_EQ(ires, IRES_SENTINEL, "after unregister, fdatasync *result untouched");

    /* ---- destroying the context must not leave a live fast path -------- */
    exitos_unregister_fd(c, fdA);
    exitos_ctx_destroy(c);
    c = NULL;

    free(wbuf);
    teardown();          /* closes logA/logB/logC, then umount + losetup -d */
    T_DONE();
}
