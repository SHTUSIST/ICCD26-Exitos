/* Tier 1 integration tests for the iopath module, IOPATH_PWRITE backend.
 *
 * SAFETY: this test never opens a physical disk. It brings up a loop device
 * through tests/harness/loopfix.sh (an ext4 image on a file), refuses to
 * continue unless the device the fixture handed back is literally under
 * /dev/loop, and tears the fixture down on every exit path including failure.
 * The image path and mount point carry this process's pid so two tests running
 * at the same time cannot collide.
 *
 * What it proves:
 *   - LBA arithmetic: a write to LBA n lands at byte offset n * lbs, verified
 *     both by iopath_read and by an independent buffered pread on the device.
 *   - Neighbouring LBAs are untouched (catches an off-by-one in n * lbs).
 *   - Data survives close and reopen (iopath_flush really persists).
 *   - O_DIRECT's two hard requirements are enforced by the module rather than
 *     handed to the kernel as EINVAL: length must be a multiple of lbs, and the
 *     buffer must be aligned. Both must be refused with -EINVAL.
 *
 * The fixture mounts the image; we unmount it immediately and keep only the
 * loop device, so ext4 writeback cannot land on top of the raw LBAs we write.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "exitos_iopath.h"
#include "tap.h"

#define LBS      4096u
#define IMG_MB   128            /* 128 MiB image -> 32768 LBAs of 4096 bytes */
#define DEV_LBAS (IMG_MB * 1024u * 1024u / LBS)

static char g_fix[512];
static char g_img[256];
static char g_mnt[256];
static char g_loop[128];
static int  g_torn;

static void run_quiet(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void run_quiet(const char *fmt, ...)
{
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    strncat(cmd, " >/dev/null 2>&1", sizeof(cmd) - strlen(cmd) - 1);
    if (system(cmd) != 0) { /* best-effort cleanup step; ignore */ }
}

/* Tear the fixture down. loopfix.sh records the loop device in a fixed path,
 * /tmp/exitos-loop.dev, which a concurrently running test may have overwritten.
 * So we only let the fixture's own "down" run when that file still names OUR
 * loop device; otherwise we undo exactly what we created and nothing else. */
static void teardown(void)
{
    if (g_torn) return;
    g_torn = 1;

    char shared[128] = "";
    FILE *f = fopen("/tmp/exitos-loop.dev", "r");
    if (f) {
        if (fgets(shared, sizeof(shared), f)) {
            size_t n = strlen(shared);
            while (n && (shared[n - 1] == '\n' || shared[n - 1] == '\r'))
                shared[--n] = '\0';
        }
        fclose(f);
    }

    if (g_loop[0] && strcmp(shared, g_loop) == 0 && g_fix[0]) {
        run_quiet("EXITOS_IMG='%s' EXITOS_MNT='%s' bash '%s' down",
                  g_img, g_mnt, g_fix);
    } else {
        /* Somebody else owns the shared state file - do not touch their loop. */
        if (g_mnt[0])  run_quiet("umount '%s'", g_mnt);
        if (g_loop[0]) run_quiet("losetup -d '%s'", g_loop);
        if (g_img[0])  run_quiet("rm -f '%s'", g_img);
    }
    if (g_mnt[0]) run_quiet("rmdir '%s'", g_mnt);
}

static const char *find_loopfix(void)
{
    const char *cands[] = {
        getenv("EXITOS_LOOPFIX"),
        "tests/harness/loopfix.sh",
        "../harness/loopfix.sh",
        "../../tests/harness/loopfix.sh",
        /* No absolute path here: one baked in from the machine a test was
         * written on is a pointer that silently stops resolving on every other
         * checkout, and the group it guards then skips while still reporting
         * "0 failed". */
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
        if (cands[i] && access(cands[i], R_OK) == 0) return cands[i];
    return NULL;
}

/* Deterministic, LBA-tagged content: the first 8 bytes are the LBA itself, so a
 * block read from the wrong offset is recognisable, not just unequal. */
static void fill_pattern(unsigned char *b, size_t len, uint64_t lba, uint8_t salt)
{
    for (size_t i = 0; i < len; i++)
        b[i] = (unsigned char)((i * 131u + 17u) ^ salt);
    for (int i = 0; i < 8 && (size_t)i < len; i++)
        b[i] = (unsigned char)(lba >> (8 * i));
}

int main(void)
{
    if (!t_need_root()) {
        T_SKIP("not root - the loop-device fixture needs root; skipping tier 1");
        T_DONE();
    }

    const char *fix = find_loopfix();
    if (!fix) {
        T_SKIP("tests/harness/loopfix.sh not found from cwd - skipping tier 1");
        T_DONE();
    }
    snprintf(g_fix, sizeof(g_fix), "%s", fix);
    snprintf(g_img, sizeof(g_img), "/tmp/exitos-iopath-%d.img", (int)getpid());
    snprintf(g_mnt, sizeof(g_mnt), "/tmp/exitos-iopath-mnt-%d", (int)getpid());
    atexit(teardown);

    /* Bring the fixture up and read the loop device out of its own stdout
     * rather than the shared /tmp/exitos-loop.dev file. */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "EXITOS_IMG='%s' EXITOS_MNT='%s' EXITOS_SIZE_MB=%d bash '%s' up 2>&1",
             g_img, g_mnt, IMG_MB, g_fix);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        T_SKIP("cannot run the loop fixture (popen failed) - skipping tier 1");
        T_DONE();
    }
    char line[512], last[512] = "";
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "LOOP=", 5) == 0) snprintf(last, sizeof(last), "%s", line);
    }
    int up_rc = pclose(fp);
    if (up_rc != 0 || sscanf(last, "LOOP=%127s", g_loop) != 1) {
        g_loop[0] = '\0';
        T_SKIP("loop fixture did not come up (losetup/mkfs.ext4 unavailable?) - skipping");
        T_DONE();
    }

    /* HARD SAFETY GATE: only ever operate on /dev/loopN. */
    if (strncmp(g_loop, "/dev/loop", 9) != 0) {
        T_OK(0, "fixture returned '%s' which is not a /dev/loop device - refusing", g_loop);
        g_loop[0] = '\0';   /* do not detach something we do not understand */
        T_DONE();
    }
    T_OK(1, "loop fixture up on %s (image %s, %d MiB)", g_loop, g_img, IMG_MB);

    /* Drop the filesystem; we want the raw block device to ourselves. */
    run_quiet("umount '%s'", g_mnt);

    /* ---- buffers: 4096-aligned as O_DIRECT demands ------------------- */
    void *wraw = NULL, *rraw = NULL;
    if (posix_memalign(&wraw, LBS, 8 * LBS) != 0 ||
        posix_memalign(&rraw, LBS, 8 * LBS) != 0) {
        T_OK(0, "posix_memalign for O_DIRECT buffers failed");
        T_DONE();
    }
    unsigned char *wbuf = wraw, *rbuf = rraw;

    struct iopath *p = iopath_open(g_loop, IOPATH_PWRITE, LBS);
    T_OK(p != NULL, "iopath_open(%s, IOPATH_PWRITE, lbs=%u) succeeds", g_loop, LBS);
    if (!p) { free(wraw); free(rraw); T_DONE(); }

    /* ---------------------------------------------------------------- *
     * 1. Round trip: two blocks at LBA 1024.
     * ---------------------------------------------------------------- */
    const uint64_t LBA_A = 1024, LBA_C = 1026, LBA_HI = DEV_LBAS - 8;
    int rc;

    fill_pattern(wbuf, 2 * LBS, LBA_A, 0x5A);
    rc = iopath_write(p, LBA_A, wbuf, 2 * LBS);
    T_EQ(rc, 0, "write 8192 bytes at LBA %llu returns 0", (unsigned long long)LBA_A);

    rc = iopath_flush(p);
    T_EQ(rc, 0, "iopath_flush returns 0");

    memset(rbuf, 0, 2 * LBS);
    rc = iopath_read(p, LBA_A, rbuf, 2 * LBS);
    T_EQ(rc, 0, "read 8192 bytes back from LBA %llu returns 0", (unsigned long long)LBA_A);
    T_EQ(memcmp(rbuf, wbuf, 2 * LBS), 0, "read-back bytes match the written pattern");

    /* ---------------------------------------------------------------- *
     * 2. The bytes really are at offset LBA * lbs. Independent check with a
     *    plain buffered pread on the same device, no iopath code involved.
     * ---------------------------------------------------------------- */
    int vfd = open(g_loop, O_RDONLY);
    T_OK(vfd >= 0, "opened %s read-only for independent verification", g_loop);
    unsigned char *vb = vfd >= 0 ? malloc(2 * LBS) : NULL;
    if (vfd >= 0 && !vb) { close(vfd); vfd = -1; T_OK(0, "malloc for verification buffer failed"); }
    if (vfd >= 0) {
        ssize_t got = pread(vfd, vb, 2 * LBS, (off_t)(LBA_A * LBS));
        T_EQ(got, (ssize_t)(2 * LBS), "pread of 8192 bytes at byte offset %llu",
             (unsigned long long)(LBA_A * LBS));
        T_EQ(memcmp(vb, wbuf, 2 * LBS), 0,
             "bytes at offset LBA*lbs == %llu are exactly what we wrote",
             (unsigned long long)(LBA_A * LBS));

        /* An off-by-one in the offset would have put them one block earlier or
         * later; both neighbours must NOT hold our pattern's first 8 bytes. */
        unsigned char nb[8];
        if (pread(vfd, nb, 8, (off_t)((LBA_A - 1) * LBS)) == 8)
            T_OK(memcmp(nb, wbuf, 8) != 0,
                 "LBA %llu (one block before) does not hold our tag",
                 (unsigned long long)(LBA_A - 1));
        if (pread(vfd, nb, 8, (off_t)((LBA_A + 2) * LBS)) == 8)
            T_OK(memcmp(nb, wbuf, 8) != 0,
                 "LBA %llu (one block after the 2-block write) does not hold our tag",
                 (unsigned long long)(LBA_A + 2));
        free(vb);
        close(vfd);
    }

    /* ---------------------------------------------------------------- *
     * 3. Writing a neighbour must not disturb LBA_A..LBA_A+1.
     * ---------------------------------------------------------------- */
    unsigned char *wb2 = wbuf + 4 * LBS;
    fill_pattern(wb2, LBS, LBA_C, 0xC3);
    rc = iopath_write(p, LBA_C, wb2, LBS);
    T_EQ(rc, 0, "write 4096 bytes at LBA %llu returns 0", (unsigned long long)LBA_C);
    T_EQ(iopath_flush(p), 0, "flush after neighbour write returns 0");

    memset(rbuf, 0, 2 * LBS);
    T_EQ(iopath_read(p, LBA_A, rbuf, 2 * LBS), 0, "re-read LBA %llu returns 0",
         (unsigned long long)LBA_A);
    T_EQ(memcmp(rbuf, wbuf, 2 * LBS), 0,
         "LBA %llu still holds its own pattern after the neighbour write",
         (unsigned long long)LBA_A);

    memset(rbuf, 0, LBS);
    T_EQ(iopath_read(p, LBA_C, rbuf, LBS), 0, "read LBA %llu returns 0",
         (unsigned long long)LBA_C);
    T_EQ(memcmp(rbuf, wb2, LBS), 0, "LBA %llu holds the neighbour pattern",
         (unsigned long long)LBA_C);

    /* ---------------------------------------------------------------- *
     * 4. A high LBA near the end of the device, to exercise the 64-bit
     *    multiply rather than only small offsets.
     * ---------------------------------------------------------------- */
    fill_pattern(wbuf, LBS, LBA_HI, 0x77);
    T_EQ(iopath_write(p, LBA_HI, wbuf, LBS), 0,
         "write at high LBA %llu (byte offset %llu) returns 0",
         (unsigned long long)LBA_HI, (unsigned long long)(LBA_HI * LBS));
    T_EQ(iopath_flush(p), 0, "flush after high-LBA write returns 0");
    memset(rbuf, 0, LBS);
    T_EQ(iopath_read(p, LBA_HI, rbuf, LBS), 0, "read at high LBA %llu returns 0",
         (unsigned long long)LBA_HI);
    T_EQ(memcmp(rbuf, wbuf, LBS), 0, "high-LBA read-back matches");

    /* ---------------------------------------------------------------- *
     * 5. Refusals. len must be a multiple of lbs and buf must be aligned;
     *    the module must return -EINVAL itself, not pass the mess to the
     *    kernel. Each of these must also leave the device untouched.
     * ---------------------------------------------------------------- */
    struct { size_t len; const char *why; } badlen[] = {
        { 1,            "1 byte"                },
        { 512,          "512 bytes (< lbs)"     },
        { 4095,         "4095 bytes"            },
        { LBS + 1,      "lbs + 1"               },
        { LBS + 512,    "lbs + 512"             },
        { 3 * LBS - 1,  "3*lbs - 1"             },
    };
    for (size_t i = 0; i < sizeof(badlen) / sizeof(badlen[0]); i++) {
        rc = iopath_write(p, LBA_A, wbuf, badlen[i].len);
        T_EQ(rc, -EINVAL, "write of %s is refused with -EINVAL (got %d)",
             badlen[i].why, rc);
        rc = iopath_read(p, LBA_A, rbuf, badlen[i].len);
        T_EQ(rc, -EINVAL, "read of %s is refused with -EINVAL (got %d)",
             badlen[i].why, rc);
    }

    /* Misaligned buffers: O_DIRECT needs the buffer aligned to the block size.
     * wbuf is 4096-aligned, so wbuf+1 and wbuf+512 are not. */
    struct { size_t off; const char *why; } badalign[] = {
        { 1,    "buffer + 1 (byte-misaligned)"   },
        { 3,    "buffer + 3"                     },
        { 512,  "buffer + 512 (512-aligned only)"},
        { 2048, "buffer + 2048 (half a block)"   },
    };
    for (size_t i = 0; i < sizeof(badalign) / sizeof(badalign[0]); i++) {
        rc = iopath_write(p, LBA_A, wbuf + badalign[i].off, LBS);
        T_EQ(rc, -EINVAL, "write from %s is refused with -EINVAL (got %d)",
             badalign[i].why, rc);
        rc = iopath_read(p, LBA_A, rbuf + badalign[i].off, LBS);
        T_EQ(rc, -EINVAL, "read into %s is refused with -EINVAL (got %d)",
             badalign[i].why, rc);
    }

    /* NULL buffer with an otherwise valid length. */
    T_OK(iopath_write(p, LBA_A, NULL, LBS) < 0, "write with buf == NULL is refused");
    T_OK(iopath_read(p, LBA_A, NULL, LBS) < 0, "read with buf == NULL is refused");

    /* After all those refusals the data must still be intact. */
    memset(rbuf, 0, 2 * LBS);
    fill_pattern(wbuf, 2 * LBS, LBA_A, 0x5A);
    T_EQ(iopath_read(p, LBA_A, rbuf, 2 * LBS), 0, "read after the refusal set returns 0");
    T_EQ(memcmp(rbuf, wbuf, 2 * LBS), 0,
         "refused calls left LBA %llu unmodified", (unsigned long long)LBA_A);

    /* Past the end of a 128 MiB device: the kernel refuses, and iopath must
     * report that as a negative return rather than a silent success. */
    rc = iopath_write(p, (uint64_t)1 << 40, wbuf, LBS);
    T_OK(rc < 0, "write past the end of the device fails (LBA 2^40, got %d)", rc);
    rc = iopath_read(p, (uint64_t)1 << 40, rbuf, LBS);
    T_OK(rc < 0, "read past the end of the device fails (LBA 2^40, got %d)", rc);

    /* ---------------------------------------------------------------- *
     * 6. Durability across close/reopen.
     * ---------------------------------------------------------------- */
    T_EQ(iopath_flush(p), 0, "final flush returns 0");
    iopath_close(p);
    T_OK(1, "iopath_close returned");

    p = iopath_open(g_loop, IOPATH_PWRITE, LBS);
    T_OK(p != NULL, "reopened %s after close", g_loop);
    if (p) {
        memset(rbuf, 0, 2 * LBS);
        T_EQ(iopath_read(p, LBA_A, rbuf, 2 * LBS), 0, "read after reopen returns 0");
        T_EQ(memcmp(rbuf, wbuf, 2 * LBS), 0,
             "data written before close is still there after reopen");

        memset(rbuf, 0, LBS);
        fill_pattern(wbuf, LBS, LBA_HI, 0x77);
        T_EQ(iopath_read(p, LBA_HI, rbuf, LBS), 0, "read high LBA after reopen returns 0");
        T_EQ(memcmp(rbuf, wbuf, LBS), 0, "high-LBA data survived close/reopen");
        iopath_close(p);
    }

    free(wraw);
    free(rraw);
    teardown();
    T_DONE();
}
