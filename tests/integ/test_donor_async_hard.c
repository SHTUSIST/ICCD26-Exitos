/* Adversarial tests for the background space preparer.
 *
 *   contract: include/exitos_donor_async.h
 *   behaviour: src/donor_async.c
 *
 * This is the only threaded module in the project and the write path calls into
 * it on every append, so the properties defended here are: (a) it never loses
 * data the application already wrote, (b) it never lies about how much space is
 * ready, (c) it always makes progress or stops, never spins, and (d) the two
 * write-path entry points stay cheap and safe under concurrency.
 *
 * Tier-1: needs root and a loop-mounted ext4 image (EXT4_IOC_MOVE_EXT only
 * exists there). Image and mountpoint carry getpid() so parallel runs cannot
 * collide, and both are torn down on every exit path including signals.
 * NEVER touches a physical block device.
 */
#include "tap.h"
#include "exitos_donor.h"
#include "exitos_donor_async.h"
#include "exitos_extent.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MIB (1024ull * 1024ull)
#define BLK 4096ull

/* ------------------------------------------------------------------ */
/* fixture                                                             */
/* ------------------------------------------------------------------ */
static char g_img[256], g_mnt[256];
static int  g_owns_fixture;
static char g_dir[256];

static void fixture_down(void)
{
    char cmd[1400];
    if (!g_owns_fixture) return;
    g_owns_fixture = 0;
    snprintf(cmd, sizeof cmd,
             "umount %s 2>/dev/null; L=$(cat %s.loop 2>/dev/null); "
             "[ -n \"$L\" ] && losetup -d \"$L\" 2>/dev/null; "
             "rm -rf %s %s %s.loop 2>/dev/null",
             g_mnt, g_img, g_img, g_mnt, g_img);
    if (system(cmd) != 0) { /* best effort */ }
}

static void on_signal(int sig)
{
    fixture_down();
    _exit(128 + sig);
}

/* A forked child must never tear the fixture down, and must die on its own
 * default disposition so the parent can see WIFSIGNALED and the signal number
 * instead of an exit code laundered through on_signal(). */
static void child_detach(void)
{
    g_owns_fixture = 0;
    signal(SIGSEGV, SIG_DFL); signal(SIGABRT, SIG_DFL);
    signal(SIGINT, SIG_DFL);  signal(SIGTERM, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

static void describe_status(const char *tag, int i, int st)
{
    if (WIFSIGNALED(st))
        printf("# %s child %d: killed by signal %d (%s)\n", tag, i, WTERMSIG(st),
               strsignal(WTERMSIG(st)));
    else if (WIFEXITED(st))
        printf("# %s child %d: exit %d\n", tag, i, WEXITSTATUS(st));
    else
        printf("# %s child %d: raw status 0x%x\n", tag, i, (unsigned)st);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */
static double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
static double cpu_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
static uint64_t wait_prepared(struct donor_async *a, uint64_t want, double max_ms)
{
    double t0 = now_ms();
    uint64_t p;
    while ((p = donor_async_prepared_to(a)) < want && now_ms() - t0 < max_ms)
        usleep(2000);
    return p;
}
/* Every byte of the pattern is non-zero, so a zero byte read back is proof the
 * application's data was destroyed rather than merely reordered. */
static unsigned char pat_byte(uint64_t off)
{
    return (unsigned char)(((off / BLK) * 7u + (off % BLK)) % 251u + 1u);
}
static int write_pattern(int fd, uint64_t off, uint64_t len)
{
    unsigned char *b = malloc(MIB);
    uint64_t done = 0;
    if (!b) return -1;
    while (done < len) {
        uint64_t n = (len - done) < MIB ? (len - done) : MIB, i;
        for (i = 0; i < n; i++) b[i] = pat_byte(off + done + i);
        if (pwrite(fd, b, (size_t)n, (off_t)(off + done)) != (ssize_t)n) { free(b); return -1; }
        done += n;
    }
    free(b);
    return 0;
}
/* Returns bytes that no longer match; *first_bad = first such offset,
 * *zeroed = how many of them read back as 0. */
static uint64_t check_pattern(int fd, uint64_t off, uint64_t len,
                              uint64_t *first_bad, uint64_t *zeroed)
{
    unsigned char *b = malloc(MIB);
    uint64_t done = 0, bad = 0, z = 0;
    *first_bad = UINT64_MAX; *zeroed = 0;
    if (!b) return UINT64_MAX;
    while (done < len) {
        uint64_t n = (len - done) < MIB ? (len - done) : MIB, i;
        ssize_t r = pread(fd, b, (size_t)n, (off_t)(off + done));
        if (r != (ssize_t)n) { free(b); return UINT64_MAX; }
        for (i = 0; i < n; i++) {
            if (b[i] != pat_byte(off + done + i)) {
                if (*first_bad == UINT64_MAX) *first_bad = off + done + i;
                bad++;
                if (b[i] == 0) z++;
            }
        }
        done += n;
    }
    free(b);
    *zeroed = z;
    return bad;
}
static int open_target(const char *name, char *path, size_t plen)
{
    snprintf(path, plen, "%s/%s", g_dir, name);
    return open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
}

/* ------------------------------------------------------------------ */
/* shared state for the concurrency groups                             */
/* ------------------------------------------------------------------ */
struct hammer {
    struct donor_async *a;
    _Atomic int         go;
    _Atomic uint64_t    v_monotonic;   /* prepared_to went backwards       */
    _Atomic uint64_t    v_align;       /* prepared_to not block aligned    */
    _Atomic uint64_t    v_overrun;     /* runway claimed more than exists  */
    _Atomic uint64_t    advances;
    _Atomic uint64_t    reads;
    _Atomic uint64_t    off;
};

static void *th_advance(void *arg)
{
    struct hammer *h = arg;
    while (!atomic_load(&h->go)) sched_yield();
    while (atomic_load(&h->go) == 1) {
        uint64_t o = atomic_fetch_add(&h->off, 64ull * 1024ull) + 64ull * 1024ull;
        donor_async_advance(h->a, o);
        atomic_fetch_add(&h->advances, 1);
    }
    return NULL;
}

static void *th_sample(void *arg)
{
    struct hammer *h = arg;
    uint64_t last = 0;
    while (!atomic_load(&h->go)) sched_yield();
    while (atomic_load(&h->go) == 1) {
        uint64_t p1 = donor_async_prepared_to(h->a);
        uint64_t off = 4ull * MIB;
        uint64_t r   = donor_async_runway(h->a, off);
        uint64_t p2  = donor_async_prepared_to(h->a);

        if (p1 < last)            atomic_fetch_add(&h->v_monotonic, 1);
        if (p1 % BLK)             atomic_fetch_add(&h->v_align, 1);
        /* prepared_to only ever grows, so a runway sampled between p1 and p2
         * can never exceed what p2 says is prepared beyond off. */
        if (r > (p2 > off ? p2 - off : 0)) atomic_fetch_add(&h->v_overrun, 1);
        last = p1;
        atomic_fetch_add(&h->reads, 1);
    }
    return NULL;
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* forked-child payloads (things that may legitimately crash)          */
/* ------------------------------------------------------------------ */
struct shm_result {
    uint64_t prepared_a, prepared_b, rounds_a, rounds_b, pool_bytes;
    uint64_t advanced_a, advanced_b;   /* how far each writer said it reached */
    int      overlap_runs;
    int      nruns_a, nruns_b;
    int      reached_end;
};

int main(int argc, char **argv)
{
    char cmd[1600];
    setvbuf(stdout, NULL, _IOLBF, 0);   /* keep child stderr from splitting a line */
    char tp[512];
    struct donor_pool *p = NULL;
    int fd = -1;

    if (argc > 1) {
        snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    } else {
        if (!t_need_root()) {
            T_SKIP("needs root for a loop device (tier-1)");
            printf("1..0  (0 failed)\n");
            return 0;
        }
        snprintf(g_img, sizeof g_img, "/tmp/exitos-asynchard-%d.img", (int)getpid());
        snprintf(g_mnt, sizeof g_mnt, "/tmp/exitos-asynchard-mnt-%d", (int)getpid());
        snprintf(cmd, sizeof cmd,
                 "truncate -s 640M %s && L=$(losetup --find --show %s) && "
                 "mkfs.ext4 -qF -b 4096 $L && mkdir -p %s && mount $L %s && echo $L > %s.loop",
                 g_img, g_img, g_mnt, g_mnt, g_img);
        g_owns_fixture = 1;
        atexit(fixture_down);
        signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
        signal(SIGSEGV, on_signal); signal(SIGABRT, on_signal);
        if (system(cmd) != 0) {
            T_OK(0, "loop fixture came up");
            fixture_down();
            T_DONE();
        }
        snprintf(g_dir, sizeof g_dir, "%s", g_mnt);
    }

    char dp[600];
    snprintf(dp, sizeof dp, "%s/donors", g_dir);
    if (mkdir(dp, 0700) != 0 && errno != EEXIST) { T_OK(0, "donor dir"); T_DONE(); }

    /* ============================================================== *
     * GROUP 1 - NULL handles.
     * The header exposes runway()/advance() as write-path calls and
     * prepared_to()/rounds() as diagnostics. A NULL handle is what a caller
     * holds after donor_async_start() fails (it returns NULL and the header
     * gives no other failure signal), so every one of these must degrade to
     * "no runway" rather than dereference NULL on the write path.
     * ============================================================== */
    T_EQ(donor_async_runway(NULL, 0), 0, "runway(NULL,0) returns 0, no crash");
    T_EQ(donor_async_runway(NULL, UINT64_MAX), 0, "runway(NULL,UINT64_MAX) returns 0");
    donor_async_advance(NULL, 0);
    donor_async_advance(NULL, UINT64_MAX);
    T_OK(1, "advance(NULL,...) is a no-op, no crash");
    T_EQ(donor_async_prepared_to(NULL), 0, "prepared_to(NULL) returns 0");
    T_EQ(donor_async_rounds(NULL), 0, "rounds(NULL) returns 0");
    donor_async_stop(NULL);
    donor_async_stop(NULL);
    T_OK(1, "stop(NULL) twice is a no-op, no crash");

    /* Bad construction arguments must be refused up front, not deferred to a
     * thread that dies silently. */
    T_OK(donor_async_start(NULL, 0, 4 * MIB, MIB) == NULL, "start(pool=NULL) refused");

    p = donor_pool_create(dp, 3, 48 * MIB);
    T_OK(p != NULL, "donor pool created (3 x 48 MiB)");
    if (!p) { T_DONE(); }

    T_OK(donor_async_start(p, -1, 4 * MIB, MIB) == NULL, "start(fd=-1) refused");

    /* ============================================================== *
     * GROUP 2 - lifecycle with no consumption at all.
     * The write path is allowed to never call advance() (a file that is opened
     * and closed without appending). Start/stop must then still terminate, and
     * must not leak a thread: this module is the only place the project spawns
     * one, so a leaked thread here leaks per opened file.
     * ============================================================== */
    {
        int nfd = open_target("g2.log", tp, sizeof tp);
        T_OK(nfd >= 0, "target for lifecycle group opened");

        struct donor_async *a1 = donor_async_start(p, nfd, 4 * MIB, 2 * MIB);
        T_OK(a1 != NULL, "start() with no consumption");
        double t0 = now_ms();
        donor_async_stop(a1);                     /* stop immediately after start */
        double dt = now_ms() - t0;
        T_OK(dt < 30000.0, "stop() immediately after start() returned in %.1f ms", dt);

        /* thread accounting: repeated start/stop must return to baseline */
        int base = 0, after = 0;
        {
            FILE *f = popen("ls /proc/self/task | wc -l", "r");
            if (f) { if (fscanf(f, "%d", &base) != 1) base = 0; pclose(f); }
        }
        for (int i = 0; i < 40; i++) {
            struct donor_async *ai = donor_async_start(p, nfd, 4 * MIB, 2 * MIB);
            if (!ai) { T_OK(0, "start() failed on cycle %d", i); break; }
            donor_async_stop(ai);
        }
        {
            FILE *f = popen("ls /proc/self/task | wc -l", "r");
            if (f) { if (fscanf(f, "%d", &after) != 1) after = 0; pclose(f); }
        }
        T_OK(base > 0 && after <= base,
             "40 start/stop cycles leaked no threads (%d before, %d after)", base, after);

        /* Everything donated above sits in the target; give it back so later
         * groups get a full pool. */
        donor_reclaim(p, nfd, 0);
        close(nfd); unlink(tp);
    }

    /* ============================================================== *
     * GROUP 3 - "Preparation for the first chunk starts immediately."
     * (exitos_donor_async.h, struct donor_async_start comment.)
     * Both sizing modes must honour it: a fixed chunk, and chunk == 0 which
     * the header defines as moving-average sizing. A preparer that silently
     * never prepares is the worst failure mode this module has, because
     * runway() keeps returning 0 and the write path keeps falling back to the
     * kernel forever with no error anywhere.
     * ============================================================== */
    {
        struct { uint64_t chunk, low; const char *what; } cases[] = {
            { 4 * MIB, 2 * MIB, "chunk=4MiB low_water=2MiB" },
            { 4 * MIB, 0,       "chunk=4MiB low_water=0 (defaulted)" },
            { 0,       0,       "chunk=0 (moving average) low_water=0" },
            { 0,       MIB,     "chunk=0 low_water=1MiB" },
            { BLK,     0,       "chunk=4096 low_water=0" },
            { 2,       0,       "chunk=2 low_water=0 (low_water becomes 1)" },
            { 1,       1,       "chunk=1 low_water=1 (explicit)" },
            { 1,       0,       "chunk=1 (sub-block) low_water=0" },
        };
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            char nm[64]; snprintf(nm, sizeof nm, "g3-%u.log", i);
            int cfd = open_target(nm, tp, sizeof tp);
            struct donor_async *a = donor_async_start(p, cfd, cases[i].chunk, cases[i].low);
            uint64_t got = 0;
            if (a) got = wait_prepared(a, BLK, 4000.0);
            T_OK(a && got > 0,
                 "first chunk prepared without any advance(): %s -> prepared_to=%llu rounds=%llu",
                 cases[i].what, (unsigned long long)got,
                 (unsigned long long)donor_async_rounds(a));
            if (a) { donor_async_stop(a); }
            donor_reclaim(p, cfd, 0);
            close(cfd); unlink(tp);
        }
    }

    /* ============================================================== *
     * GROUP 4 - the preparer must never prepare space the writer has already
     * written into.
     *
     * The header states the fallback rule that creates this situation:
     *   "Zero means the writer has caught up with the preparer and must fall
     *    back to the ordinary kernel path for this write."
     * and it states that the module is told about it:
     *   "donor_async_advance(): Tell the preparer the writer has reached off."
     *
     * So a conforming caller WILL leave live file data above prepared_to, and
     * it announces exactly where it got to. Preparing at prepared_to after
     * that means donating over live data: donor_extend() swaps the target's
     * extents out for donor extents and then writes zeros over the whole
     * donated range (src/donor.c, the pwrite loop after runs_take_head), so
     * the application's bytes are gone. This group writes a known non-zero
     * pattern through the documented fallback and checks it survives.
    * ============================================================== */
    {
        char p4dir[600];
        snprintf(p4dir, sizeof p4dir, "%s/donors-g4", g_dir);
        (void)mkdir(p4dir, 0700);
        /* Pools now own collision-safe O_EXCL names. A second live pool must
         * therefore have its own directory instead of truncating pool p's
         * donor-0000.dat, which was the old test's unsafe hidden dependency. */
        struct donor_pool *p4 = donor_pool_create(p4dir, 1, 64 * MIB);
        T_OK(p4 != NULL, "second pool for the overwrite group");
        if (p4) {
            int f4 = open_target("g4-fallback.log", tp, sizeof tp);
            struct donor_async *a = donor_async_start(p4, f4, MIB, MIB);
            T_OK(a != NULL, "preparer started on an empty target");

            uint64_t p1 = wait_prepared(a, MIB, 5000.0);
            T_OK(p1 >= MIB, "preparer produced initial runway (prepared_to=%llu)",
                 (unsigned long long)p1);

            /* Consume the first MiB the way a writer would, then run past the
             * end of the runway on the ordinary kernel path, exactly as the
             * header instructs when runway() reads 0. */
            donor_async_advance(a, MIB);
            uint64_t p2 = wait_prepared(a, 2 * MIB, 5000.0);
            (void)p2;
            T_OK(write_pattern(f4, MIB, 8 * MIB) == 0, "writer wrote 8 MiB at 1 MiB via the kernel path");
            fsync(f4);
            donor_async_advance(a, 9 * MIB);       /* "the writer has reached 9 MiB" */

            /* Let the preparer act on that announcement. */
            uint64_t p3 = wait_prepared(a, 10 * MIB, 15000.0);
            donor_async_stop(a);
            fsync(f4);
            close(f4);
            f4 = open(tp, O_RDONLY);

            uint64_t first_bad = 0, zeroed = 0;
            uint64_t bad = check_pattern(f4, MIB, 8 * MIB, &first_bad, &zeroed);
            T_EQ(bad, 0,
                 "data written through the documented fallback survived preparation "
                 "(prepared_to=%llu, %llu bytes corrupted, %llu of them zeroed, first bad off=%llu)",
                 (unsigned long long)p3, (unsigned long long)bad,
                 (unsigned long long)zeroed,
                 bad ? (unsigned long long)first_bad : 0ull);
            close(f4); unlink(tp);

            /* CONTROL. Same module, same pool, same donation machinery - but
             * the writer never goes above prepared_to. If this survives while
             * the case above does not, the destruction is caused specifically
             * by preparing at prepared_to after being told the writer is past
             * it, not by donation per se. */
            int fc = open_target("g4-control.log", tp, sizeof tp);
            struct donor_async *c = donor_async_start(p4, fc, MIB, MIB);
            uint64_t pc0 = c ? wait_prepared(c, MIB, 5000.0) : 0;
            T_OK(pc0 >= MIB, "control: runway ready before the writer starts (%llu)",
                 (unsigned long long)pc0);
            T_OK(write_pattern(fc, 0, 512ull * 1024ull) == 0,
                 "control: writer wrote 512 KiB entirely inside the runway");
            donor_async_advance(c, 512ull * 1024ull);
            (void)wait_prepared(c, 2 * MIB, 8000.0);
            if (c) donor_async_stop(c);
            fsync(fc); close(fc); fc = open(tp, O_RDONLY);
            first_bad = 0; zeroed = 0;
            uint64_t badc = check_pattern(fc, 0, 512ull * 1024ull, &first_bad, &zeroed);
            T_EQ(badc, 0, "control: data written inside the runway survived (%llu bytes corrupted)",
                 (unsigned long long)badc);
            close(fc); unlink(tp);

            /* Same defect reached the other way: the header's start() takes a
             * target fd and no starting offset, and says preparation begins
             * immediately, so handing it a file that already holds data
             * donates over the head of that file. */
            int f5 = open_target("g4-nonempty.log", tp, sizeof tp);
            T_OK(write_pattern(f5, 0, 4 * MIB) == 0, "pre-existing 4 MiB of data written");
            fsync(f5);
            struct donor_async *b = donor_async_start(p4, f5, MIB, MIB);
            uint64_t pb = b ? wait_prepared(b, 2 * MIB, 10000.0) : 0;
            if (b) donor_async_stop(b);
            fsync(f5); close(f5); f5 = open(tp, O_RDONLY);
            first_bad = 0; zeroed = 0;
            uint64_t bad2 = check_pattern(f5, 0, 4 * MIB, &first_bad, &zeroed);
            T_EQ(bad2, 0,
                 "pre-existing file contents survived donor_async_start "
                 "(prepared_to=%llu, %llu bytes corrupted, %llu zeroed, first bad off=%llu)",
                 (unsigned long long)pb, (unsigned long long)bad2,
                 (unsigned long long)zeroed,
                 bad2 ? (unsigned long long)first_bad : 0ull);
            close(f5); unlink(tp);
            donor_pool_destroy(p4);
        }
        (void)rmdir(p4dir);
    }

    /* ============================================================== *
     * GROUP 5 - advance() abuse: backwards, repeated, and far past prepared.
     * advance() is on the write path and takes an unvalidated uint64. The
     * moving-average sizing is derived from the DELTA between successive
     * advances (src/donor_async.c: last_write = off - prev), so a bogus delta
     * feeds straight into donor_next_chunk. None of it may crash, hang, or
     * make prepared_to move backwards or exceed the pool.
     * ============================================================== */
    {
        int f6 = open_target("g5.log", tp, sizeof tp);
        struct donor_async *a = donor_async_start(p, f6, 0, MIB);   /* moving average */
        T_OK(a != NULL, "adaptive preparer started for advance() abuse");
        uint64_t seen = wait_prepared(a, BLK, 5000.0);
        T_OK(seen > 0, "adaptive preparer made initial progress (%llu bytes)",
             (unsigned long long)seen);

        uint64_t before = donor_async_prepared_to(a);
        for (int i = 0; i < 200; i++) donor_async_advance(a, 8 * MIB);   /* same offset */
        usleep(200000);
        uint64_t mid = donor_async_prepared_to(a);
        T_OK(mid >= before, "200 identical advance(8MiB) calls: prepared_to did not regress "
             "(%llu -> %llu)", (unsigned long long)before, (unsigned long long)mid);

        for (int i = 0; i < 200; i++) donor_async_advance(a, MIB);      /* backwards */
        donor_async_advance(a, 0);
        usleep(200000);
        uint64_t back = donor_async_prepared_to(a);
        T_OK(back >= mid, "advance() going backwards did not regress prepared_to (%llu -> %llu)",
             (unsigned long long)mid, (unsigned long long)back);
        T_EQ(back % BLK, 0, "prepared_to stayed block aligned after backwards advance (%llu)",
             (unsigned long long)back);

        /* Far past anything prepared, and then the extreme value. The sizing
         * rule must not turn that into an absurd request: the pool holds
         * 3 x 48 MiB, so nothing may be prepared beyond that. */
        donor_async_advance(a, 1024ull * MIB);
        usleep(300000);
        donor_async_advance(a, UINT64_MAX);
        double t0 = now_ms();
        while (now_ms() - t0 < 2000.0) {
            (void)donor_async_runway(a, UINT64_MAX);
            (void)donor_async_runway(a, 0);
            usleep(20000);
        }
        uint64_t far = donor_async_prepared_to(a);
        T_EQ(donor_async_runway(a, UINT64_MAX), 0,
             "runway() past the end of the file is 0, not a wrapped huge number");
        /* Two separate things to prove here. First, the extreme value must not
         * wrap the prepared offset back to a small number: rounding it up to a
         * block boundary once did exactly that, and the preparer then donated
         * over the head of the file. Second, no matter what the writer claims,
         * the pool cannot hand out more than it holds -- measured as the space
         * prepared beyond the last believable advance, not as the raw offset,
         * which follows the writer by design. */
        T_OK(far >= 1024ull * MIB,
             "advance(UINT64_MAX) did not wrap the prepared offset backwards "
             "(prepared_to=%llu, writer had reached 1024 MiB)",
             (unsigned long long)far);
        T_OK(far - 1024ull * MIB <= 3ull * 48ull * MIB,
             "no more space was prepared than the pool owns (%llu beyond the "
             "writer, pool %llu)",
             (unsigned long long)(far - 1024ull * MIB),
             (unsigned long long)(3ull * 48ull * MIB));
        donor_async_stop(a);
        T_OK(1, "stop() after advance(UINT64_MAX) joined without hanging");
        donor_reclaim(p, f6, 0);
        close(f6); unlink(tp);
    }

    /* ============================================================== *
     * GROUP 6 - concurrency.
     * The header calls runway() a "MEMORY READ ONLY" write-path call and
     * advance() "Non-blocking; safe on the write path", and src/donor_async.c
     * claims both are lock-free apart from a conditional signal. Many writer
     * threads therefore have to be able to call them while the preparer is
     * donating. Defended: no crash, prepared_to never goes backwards or loses
     * its block alignment (a torn 64-bit value would show up as either), and
     * runway() never claims more space than prepared_to says exists.
     * ============================================================== */
    {
        const int NADV = 6, NSAMP = 2, ITERS = 8;
        uint64_t tot_adv = 0, tot_rd = 0, v_mono = 0, v_align = 0, v_over = 0;
        int crashed = 0;

        for (int it = 0; it < ITERS && !crashed; it++) {
            char nm[64]; snprintf(nm, sizeof nm, "g6-%d.log", it);
            int f = open_target(nm, tp, sizeof tp);
            struct hammer h;
            memset(&h, 0, sizeof h);
            h.a = donor_async_start(p, f, 2 * MIB, MIB);
            if (!h.a) { T_OK(0, "concurrency iteration %d could not start", it); close(f); break; }
            atomic_store(&h.go, 0);

            pthread_t adv[8], smp[4];
            for (int i = 0; i < NADV; i++) pthread_create(&adv[i], NULL, th_advance, &h);
            for (int i = 0; i < NSAMP; i++) pthread_create(&smp[i], NULL, th_sample, &h);
            atomic_store(&h.go, 1);
            usleep(250000);
            atomic_store(&h.go, 2);
            for (int i = 0; i < NADV; i++) pthread_join(adv[i], NULL);
            for (int i = 0; i < NSAMP; i++) pthread_join(smp[i], NULL);

            /* stop() lands here while the preparer is very likely mid-donation:
             * the hammering kept the runway short for 250 ms. */
            donor_async_stop(h.a);

            tot_adv += atomic_load(&h.advances);
            tot_rd  += atomic_load(&h.reads);
            v_mono  += atomic_load(&h.v_monotonic);
            v_align += atomic_load(&h.v_align);
            v_over  += atomic_load(&h.v_overrun);

            donor_reclaim(p, f, 0);
            close(f); unlink(tp);
        }
        T_OK(1, "survived %d concurrent rounds: %llu advance() and %llu runway() calls "
                "across %d writer threads", ITERS,
             (unsigned long long)tot_adv, (unsigned long long)tot_rd, NADV);
        T_EQ(v_mono, 0, "prepared_to never went backwards under %d writer threads",  NADV);
        T_EQ(v_align, 0, "prepared_to never lost block alignment (no torn 64-bit read)");
        T_EQ(v_over, 0, "runway() never reported more space than prepared_to holds");
    }

    /* ============================================================== *
     * GROUP 7 - the write path must stay cheap under contention.
     * The header: runway() is "a plain memory read: no ioctl, no io_uring, no
     * syscall of any kind", advance() is "Non-blocking". Those are the load
     * bearing claims for putting them in an append. Measured with six other
     * threads hammering advance() and the preparer donating.
     * ============================================================== */
    {
        int f = open_target("g7.log", tp, sizeof tp);
        struct hammer h;
        memset(&h, 0, sizeof h);
        h.a = donor_async_start(p, f, 2 * MIB, MIB);
        T_OK(h.a != NULL, "preparer started for the cost measurement");
        pthread_t adv[6];
        atomic_store(&h.go, 0);
        for (int i = 0; i < 6; i++) pthread_create(&adv[i], NULL, th_advance, &h);
        atomic_store(&h.go, 1);
        usleep(20000);

        const int N = 200000;
        double t0 = now_ms();
        uint64_t sink = 0;
        for (int i = 0; i < N; i++) sink += donor_async_runway(h.a, (uint64_t)i * BLK);
        double us_per = (now_ms() - t0) * 1000.0 / N;
        T_OK(us_per < 1.0, "runway() costs %.4f us/call under 6-thread contention (sink=%llu)",
             us_per, (unsigned long long)(sink & 0xffff));

        /* advance() latency distribution: it takes the preparer's mutex
         * whenever the runway is short, which under contention is most of the
         * time. "Non-blocking" should still mean sub-microsecond typical. */
        const int M = 20000;
        double *lat = malloc((size_t)M * sizeof *lat);
        uint64_t base = 512ull * MIB;
        for (int i = 0; i < M; i++) {
            double s = now_ms();
            donor_async_advance(h.a, base + (uint64_t)i * BLK);
            lat[i] = (now_ms() - s) * 1000.0;
        }
        qsort(lat, M, sizeof *lat, cmp_dbl);
        double med = lat[M / 2], p99 = lat[(int)(M * 0.99)], mx = lat[M - 1];
        T_OK(med < 5.0, "advance() median %.3f us, p99 %.3f us, max %.3f us under contention",
             med, p99, mx);
        free(lat);

        atomic_store(&h.go, 2);
        for (int i = 0; i < 6; i++) pthread_join(adv[i], NULL);
        donor_async_stop(h.a);
        donor_reclaim(p, f, 0);
        close(f); unlink(tp);
    }

    /* ============================================================== *
     * GROUP 8 - exhaustion must stop, not spin.
     * src/donor_async.c: "Pool exhausted or the filesystem refused. Stop
     * trying ... Spinning here would burn a core for nothing." A spin here
     * would steal a core from the very write path this module exists to keep
     * fast, so it is measured as CPU time, not just as a round count.
    * ============================================================== */
    {
        char tinydir[600];
        snprintf(tinydir, sizeof tinydir, "%s/donors-g8", g_dir);
        (void)mkdir(tinydir, 0700);
        struct donor_pool *tiny = donor_pool_create(tinydir, 1, 2 * MIB);
        T_OK(tiny != NULL, "tiny 2 MiB pool created");
        if (tiny) {
            int f = open_target("g8-tiny.log", tp, sizeof tp);
            /* chunk far larger than the whole pool, and a low watermark it can
             * never reach: the preparer can never be satisfied. */
            struct donor_async *a = donor_async_start(tiny, f, 64 * MIB, 64 * MIB);
            T_OK(a != NULL, "preparer started against an unsatisfiable pool");
            usleep(400000);
            uint64_t r1 = donor_async_rounds(a);
            double c0 = cpu_ms();
            usleep(500000);
            double burn = cpu_ms() - c0;
            uint64_t r2 = donor_async_rounds(a);
            T_EQ(r2, r1, "preparer stopped issuing rounds once the pool was dry (%llu rounds)",
                 (unsigned long long)r2);
            T_OK(burn < 50.0,
                 "exhausted preparer burned %.1f ms of CPU while idle for 500 ms", burn);
            T_OK(donor_async_runway(a, 64 * MIB) == 0,
                 "runway() past the exhausted pool reads 0 without hanging");
            double t0 = now_ms();
            donor_async_stop(a);
            T_OK(now_ms() - t0 < 5000.0, "stop() joined an already-exited preparer promptly");
            close(f); unlink(tp);

            /* A target the filesystem will always refuse: read-only fd. The
             * preparer must give up rather than retry an ioctl forever. */
            int rfd = open_target("g8-ro.log", tp, sizeof tp);
            close(rfd);
            rfd = open(tp, O_RDONLY);
            T_OK(rfd >= 0, "read-only target opened");
            struct donor_async *b = donor_async_start(tiny, rfd, MIB, MIB);
            if (b) {
                double c1 = cpu_ms();
                usleep(500000);
                double burn2 = cpu_ms() - c1;
                T_EQ(donor_async_rounds(b), 0, "no rounds completed against a read-only target");
                T_EQ(donor_async_prepared_to(b), 0, "prepared_to stayed 0 against a read-only target");
                T_OK(burn2 < 50.0, "preparer on a refusing target burned %.1f ms CPU in 500 ms", burn2);
                donor_async_stop(b);
                T_OK(1, "stop() after the preparer gave up joined cleanly");
            } else {
                T_OK(1, "start() refused a read-only target outright");
            }
            close(rfd); unlink(tp);
            donor_pool_destroy(tiny);
        }
        (void)rmdir(tinydir);
    }

    /* ============================================================== *
     * GROUP 9 - stop() while the preparer is mid-donation.
     * A donation is the expensive part: zero-writing the donated range plus
     * the ioctl. stop() must join it cleanly with no use-after-free: the
     * preparer touches a->prepared_to and a->mu, all of which stop() frees.
     * Run in a child so a use-after-free abort is reported instead of killing
     * the whole run.
     * ============================================================== */
    {
        int rounds_ok = 0, i;
        for (i = 0; i < 12; i++) {
            pid_t pid = fork();
            if (pid == 0) {
                char cp[512];
                child_detach();
                char nm[64]; snprintf(nm, sizeof nm, "g9-%d.log", i);
                int f = open_target(nm, cp, sizeof cp);
                struct donor_async *a = donor_async_start(p, f, 16 * MIB, 16 * MIB);
                if (!a) _exit(3);
                /* land inside donor_extend: it is doing a MOVE_EXT plus a
                 * 16 MiB zeroing pass, which takes milliseconds. */
                usleep((useconds_t)(300 + i * 900));
                donor_async_stop(a);
                close(f); unlink(cp);
                _exit(0);
            } else if (pid > 0) {
                int st = 0;
                double t0 = now_ms();
                pid_t w;
                do {
                    w = waitpid(pid, &st, WNOHANG);
                    if (w == 0) usleep(20000);
                } while (w == 0 && now_ms() - t0 < 30000.0);
                if (w == 0) { kill(pid, SIGKILL); waitpid(pid, &st, 0); st = -1; }
                if (w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0) rounds_ok++;
                else describe_status("stop-mid-donation", i, st);
            }
        }
        T_EQ(rounds_ok, 12, "stop() mid-donation joined cleanly in all 12 attempts");
    }

    /* ============================================================== *
     * GROUP 10 - two preparers over one pool.
     * Nothing in exitos_donor_async.h or exitos_donor.h restricts a pool to
     * one preparer, and the natural deployment is one preparer per open log
     * file over a shared pool: donor_async_start() takes the pool and one
     * target fd, so a second file needs a second handle. Each preparer thread
     * calls donor_extend() and donor_next_chunk() directly, and both mutate
     * unguarded pool state (the per-donor free-run arrays, which are
     * realloc'd; the chunk table, also realloc'd; the moving-average history).
     * Defended: no crash, and the two targets must never be handed the same
     * physical blocks - that is silent cross-file corruption.
     * Each attempt runs in a child so a crash is reported, not fatal.
     * ============================================================== */
    {
        const int TRIES = 5;
        int clean = 0, ended = 0, overlap_total = 0, oversub = 0;
        struct shm_result *sr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        T_OK(sr != MAP_FAILED, "shared result page mapped");
        for (int t = 0; sr != MAP_FAILED && t < TRIES; t++) {
            memset(sr, 0, sizeof *sr);
            pid_t pid = fork();
            if (pid == 0) {
                char d2[600], t1[512], t2[512];
                child_detach();
                snprintf(d2, sizeof d2, "%s/donors2-%d", g_dir, t);
                mkdir(d2, 0700);
                struct donor_pool *sp = donor_pool_create(d2, 2, 16 * MIB);
                if (!sp) _exit(4);
                sr->pool_bytes = 2ull * 16ull * MIB;
                snprintf(t1, sizeof t1, "%s/g10-a-%d.log", g_dir, t);
                snprintf(t2, sizeof t2, "%s/g10-b-%d.log", g_dir, t);
                int fa = open(t1, O_RDWR | O_CREAT | O_TRUNC, 0644);
                int fb = open(t2, O_RDWR | O_CREAT | O_TRUNC, 0644);
                if (fa < 0 || fb < 0) _exit(5);
                struct donor_async *aa = donor_async_start(sp, fa, 0, 4 * MIB);
                struct donor_async *ab = donor_async_start(sp, fb, 0, 4 * MIB);
                if (!aa || !ab) _exit(6);
                uint64_t oa = 0, ob = 0;
                for (int k = 0; k < 4000; k++) {
                    oa += 64 * 1024; ob += 96 * 1024;
                    donor_async_advance(aa, oa);
                    donor_async_advance(ab, ob);
                    if ((k & 63) == 0) usleep(500);
                }
                usleep(300000);
                sr->prepared_a = donor_async_prepared_to(aa);
                sr->prepared_b = donor_async_prepared_to(ab);
                sr->advanced_a = oa;
                sr->advanced_b = ob;
                sr->rounds_a   = donor_async_rounds(aa);
                sr->rounds_b   = donor_async_rounds(ab);
                donor_async_stop(aa);
                donor_async_stop(ab);

                /* Did the shared pool hand the same physical blocks to both? */
                struct extent_run *ra = calloc(4096, sizeof *ra);
                struct extent_run *rb = calloc(4096, sizeof *rb);
                int na = 0, nb = 0, ov = 0;
                if (ra && rb) {
                    na = exitos_extent_read(fa, ra, 4096);
                    nb = exitos_extent_read(fb, rb, 4096);
                    for (int x = 0; x < na; x++)
                        for (int y = 0; y < nb; y++) {
                            uint64_t a0 = ra[x].fs_block, a1 = a0 + ra[x].len / BLK;
                            uint64_t b0 = rb[y].fs_block, b1 = b0 + rb[y].len / BLK;
                            if (a0 < b1 && b0 < a1) ov++;
                        }
                }
                sr->nruns_a = na; sr->nruns_b = nb; sr->overlap_runs = ov;
                sr->reached_end = 1;
                free(ra); free(rb);
                close(fa); close(fb); unlink(t1); unlink(t2);
                donor_pool_destroy(sp);
                _exit(0);
            } else if (pid > 0) {
                int st = 0; pid_t w; double t0 = now_ms();
                do { w = waitpid(pid, &st, WNOHANG); if (w == 0) usleep(20000); }
                while (w == 0 && now_ms() - t0 < 180000.0);
                if (w == 0) { kill(pid, SIGKILL); waitpid(pid, &st, 0); st = 0x7f; }
                if (w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0) clean++;
                else describe_status("two-preparers-one-pool", t, st);
                {
                    char rm[1200];
                    snprintf(rm, sizeof rm,
                             "rm -rf %s/donors2-%d %s/g10-a-%d.log %s/g10-b-%d.log 2>/dev/null",
                             g_dir, t, g_dir, t, g_dir, t);
                    if (system(rm) != 0) { /* best effort */ }
                }
                if (sr->reached_end) {
                    ended++;
                    overlap_total += sr->overlap_runs;
                    /* prepared_to is a FILE OFFSET, not a count of bytes taken
                     * from the pool: preparation starts where the writer has
                     * reached, so it carries the advance offsets with it.
                     * Comparing it against the pool's capacity therefore
                     * measures nothing. What the pool must never exceed is the
                     * space actually handed out, which is prepared_to minus the
                     * point the writer had reached. */
                    {   /* Saturating: an attempt whose preparer never got as
                         * far as the writer records prepared_to BELOW the
                         * advance point, and an unsigned subtraction turns that
                         * into a huge number that looks like massive
                         * oversubscription. It means the opposite -- nothing
                         * was prepared there at all. */
                        uint64_t da = sr->prepared_a > sr->advanced_a
                                    ? sr->prepared_a - sr->advanced_a : 0;
                        uint64_t db = sr->prepared_b > sr->advanced_b
                                    ? sr->prepared_b - sr->advanced_b : 0;
                        if (da + db > sr->pool_bytes) oversub++;
                    }
                    printf("# two-preparers attempt %d: prepared %llu / %llu bytes, "
                           "rounds %llu / %llu, extents %d vs %d, overlapping %d\n", t,
                           (unsigned long long)sr->prepared_a,
                           (unsigned long long)sr->prepared_b,
                           (unsigned long long)sr->rounds_a,
                           (unsigned long long)sr->rounds_b,
                           sr->nruns_a, sr->nruns_b, sr->overlap_runs);
                }
            }
        }
        T_EQ(clean, TRIES,
             "two preparers sharing one pool ran to completion in all %d attempts (%d clean)",
             TRIES, clean);
        if (ended > 0) {
            T_EQ(overlap_total, 0,
                 "the shared pool never handed the same physical blocks to both targets "
                 "(%d of %d attempts got far enough to check)", ended, TRIES);
            T_EQ(oversub, 0,
                 "two preparers never handed out more space than the pool owns");
        } else {
            T_SKIP("no attempt survived long enough to compare physical blocks or pool accounting");
        }
        if (sr != MAP_FAILED) munmap(sr, 4096);
    }

    /* ============================================================== *
     * GROUP 11 - stop() called twice on a live handle.
     * The header documents stop(a) once per start(a) and the code frees the
     * handle, so a second stop is caller error; what is defended here is only
     * that it terminates rather than hangs (a hung stop in a shutdown path
     * wedges the whole application). Run in a child so the expected abort does
     * not take the suite down.
     * ============================================================== */
    {
        pid_t pid = fork();
        if (pid == 0) {
            char cp[512];
            int f = open_target("g11.log", cp, sizeof cp);
            struct donor_async *a = donor_async_start(p, f, 2 * MIB, MIB);
            if (!a) _exit(3);
            usleep(50000);
            donor_async_stop(a);
            donor_async_stop(a);          /* caller error, on purpose */
            close(f); unlink(cp);
            _exit(0);
        } else if (pid > 0) {
            int st = 0; pid_t w; double t0 = now_ms();
            do { w = waitpid(pid, &st, WNOHANG); if (w == 0) usleep(20000); }
            while (w == 0 && now_ms() - t0 < 20000.0);
            int hung = (w == 0);
            if (hung) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
            if (!hung) describe_status("double-stop", 0, st);
            T_OK(!hung, "double stop() terminated rather than hanging "
                        "(how it ended is caller error and is not asserted)");
        }
    }

    if (p) donor_pool_destroy(p);
    (void)fd;
    fixture_down();
    T_DONE();
}
