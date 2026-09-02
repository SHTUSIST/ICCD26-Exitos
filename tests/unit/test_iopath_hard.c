/* Tier 0 ADVERSARIAL unit tests for src/iopath.c -- command encoding and
 * backend selection.
 *
 * NO DEVICE, NO NVMe, NO LOOP DEVICE, NO BLOCK DEVICE IS EVER OPENED OR
 * WRITTEN. Everything here is either pure arithmetic on a struct
 * nvme_passthru_cmd that is never submitted, or file-system objects this test
 * creates itself under /tmp/exitos_ioph_<pid>/ and removes again. The only
 * device node touched is /dev/null, a character device that by definition
 * stores nothing -- it is used as a "node that is openable but is not an NVMe
 * controller", which is exactly the case the open path must reject.
 *
 * The point of this file is to BREAK the module, not to be green. Where the
 * observed behaviour disagrees with the contract in include/exitos_iopath.h or
 * with a comment in src/iopath.c, the assertion states what the contract says
 * and is left to FAIL. Each such group quotes the sentence it is defending.
 *
 * Vocabulary used below, spelled out once because the abbreviations are not
 * self-explanatory:
 *   SLBA  starting logical block address: the first block the command touches.
 *         64 bits wide, split low half into cdw10 and high half into cdw11.
 *   NLB   number of logical blocks. Lives in cdw12 bits 15:00 and is ZERO
 *         BASED: a transfer of N blocks is encoded as N-1, so the value 0 means
 *         one block. An off-by-one here writes the wrong number of blocks.
 *   FUA   Force Unit Access, cdw12 bit 30. Makes this one write reach
 *         persistent media without draining the whole device write cache.
 *   cdwNN command dword NN: the 32-bit fields of an NVMe command.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <linux/nvme_ioctl.h>
#include <linux/io_uring.h>

#include "exitos_iopath.h"
#include "tap.h"

#define NVME_OPC_WRITE 0x01
#define FUA_BIT        (1u << 30)

static int g_internal_enter_calls;
static int g_internal_leave_calls;
static int g_internal_last_fd = -1;

/* Strong test hooks override iopath.c's weak no-ops.  No device access is
 * involved; they prove the marker encloses the actual libc boundary. */
void exitos_internal_fd_enter(int fd)
{
    g_internal_enter_calls++;
    g_internal_last_fd = fd;
}

void exitos_internal_fd_leave(int fd)
{
    g_internal_leave_calls++;
    if (g_internal_last_fd != fd)
        g_internal_last_fd = -2;
}

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif

/* ---------------------------------------------------------------- *
 * Scratch objects. Every name carries getpid() so that several concurrent
 * runs of this suite cannot collide. cleanup() is registered with
 * atexit(), so the tree is removed on every normal exit including the failing
 * ones; only a hard crash (which would itself be a reported finding) can leave
 * anything behind.
 * ---------------------------------------------------------------- */
static char g_dir[256];
static char g_file[320];        /* an ordinary regular file                  */
static char g_fifo[320];        /* a FIFO: openable, but has no byte offsets */
static char g_d1[1024], g_d2[1024];
static char g_longA[1024];       /* path is exactly 511 bytes long            */
static char g_longB[1024];       /* the same path plus 6 more bytes           */

static uint64_t test_off_t_max(void)
{
    unsigned bits = (unsigned)(sizeof(off_t) * CHAR_BIT);
    unsigned value_bits = bits - (((off_t)-1 < (off_t)0) ? 1u : 0u);
    uint64_t max = 0;
    unsigned i;
    if (value_bits > 64u) value_bits = 64u;
    for (i = 0; i < value_bits; i++) max = (max << 1) | UINT64_C(1);
    return max;
}

static void cleanup(void)
{
    unlink(g_longB); unlink(g_longA);
    rmdir(g_d2);     rmdir(g_d1);
    unlink(g_fifo);  unlink(g_file);
    rmdir(g_dir);
}

static int write_file(const char *path, unsigned char byte, size_t len)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    unsigned char *b = malloc(len);
    if (!b) { close(fd); return -1; }
    memset(b, byte, len);
    ssize_t n = write(fd, b, len);
    free(b);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int first_byte_of(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char c = 0;
    ssize_t n = read(fd, &c, 1);
    close(fd);
    return (n == 1) ? (int)c : -1;
}

/* Does THIS kernel accept a 128-byte submission queue entry? io_uring's
 * IORING_SETUP_SQE128 arrived in Linux 5.19. iopath_open() feeds
 * iopath_setup_flags() straight into io_uring_setup(), so on an older kernel
 * every uring backend fails at ring setup, before the target is ever looked at.
 * Knowing which of the two happened is the difference between "the uring
 * backend rejected a regular file" and "the uring backend rejects everything
 * here". */
static int uring_sqe128_supported(void)
{
    struct io_uring_params pp;
    memset(&pp, 0, sizeof pp);
    pp.flags = IOPATH_SETUP_SQE128;
    long r = syscall(__NR_io_uring_setup, 1, &pp);
    if (r < 0) return 0;
    close((int)r);
    return 1;
}

/* Name of some block device on this host whose queue/io_poll reads 1, or NULL.
 * Used to decide whether the basename-confusion test below can discriminate. */
static const char *find_poll_capable_disk(void)
{
    static char name[300];
    DIR *d = opendir("/sys/class/block");
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[320];
        snprintf(p, sizeof p, "/sys/class/block/%s/queue/io_poll", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        int v = 0;
        if (fscanf(f, "%d", &v) != 1) v = 0;
        fclose(f);
        if (v) { snprintf(name, sizeof name, "%.255s", e->d_name); closedir(d); return name; }
    }
    closedir(d);
    return NULL;
}

static void poison(struct nvme_passthru_cmd *c) { memset(c, 0xAA, sizeof(*c)); }

/* Every field the WRITE command does not use must reach the kernel as zero.
 * src/iopath.c:212 -- "every field this command does not use must reach the
 * kernel as 0. A stale metadata pointer is a real hazard, not untidiness." */
static void check_unused_zero(const struct nvme_passthru_cmd *c, const char *what)
{
    T_EQ(c->flags, 0u,        "%s: flags cleared", what);
    T_EQ(c->rsvd1, 0u,        "%s: rsvd1 cleared", what);
    T_EQ(c->cdw2, 0u,         "%s: cdw2 cleared", what);
    T_EQ(c->cdw3, 0u,         "%s: cdw3 cleared", what);
    T_EQ(c->metadata, 0ull,   "%s: metadata cleared (stale pointer hazard)", what);
    T_EQ(c->metadata_len, 0u, "%s: metadata_len cleared", what);
    T_EQ(c->cdw13, 0u,        "%s: cdw13 cleared", what);
    T_EQ(c->cdw14, 0u,        "%s: cdw14 cleared", what);
    T_EQ(c->cdw15, 0u,        "%s: cdw15 cleared", what);
    T_EQ(c->timeout_ms, 0u,   "%s: timeout_ms cleared (asks kernel default)", what);
    T_EQ(c->result, 0u,       "%s: result cleared", what);
}

/* Worker for the thread-safety probe: hammers the FUA encoder on its own
 * thread while the main thread encodes plain writes. */
static void *fua_hammer(void *arg)
{
    (void)arg;
    struct nvme_passthru_cmd c;
    for (int i = 0; i < 20000; i++)
        (void)iopath_encode_nvme_write_fua(&c, 1, 0x99, 3, &c, 512, 1);
    return NULL;
}

int main(void)
{
    void *raw = NULL;
    if (posix_memalign(&raw, 4096, 65536) != 0 || raw == NULL) {
        T_SKIP("posix_memalign failed - cannot run encoder tests");
        T_DONE();
    }
    memset(raw, 0x11, 65536);
    unsigned char *buf = raw;

    /* A command struct with a canary immediately behind it: the encoder must
     * not write one byte past sizeof(struct nvme_passthru_cmd). */
    struct { struct nvme_passthru_cmd c; unsigned char canary[64]; } box;
    struct nvme_passthru_cmd *c = &box.c;
    int rc;
    struct iopath *p;

    /* ================================================================ *
     * GROUP 1 -- the 64-bit SLBA split, at and above the 2^32 boundary.
     *
     * PROPERTY: slba low 32 bits go to cdw10, high 32 bits go to cdw11, and
     * nothing else moves. WHY IT MATTERS: a device with more than 2^32 blocks
     * (any drive above 2 TB at 512-byte blocks) is addressed entirely through
     * cdw11. If the high half were dropped, every write above that boundary
     * would silently land at the wrong address near the start of the drive --
     * data loss with no error returned anywhere.
     * ================================================================ */
    struct { uint64_t slba; uint32_t lo, hi; const char *name; } sp[] = {
        { 0xFFFFFFFFULL,         0xFFFFFFFFu, 0x00000000u, "slba = 2^32-1 (last 32-bit LBA)" },
        { 0x100000000ULL,        0x00000000u, 0x00000001u, "slba = 2^32 exactly (boundary)" },
        { 0x100000001ULL,        0x00000001u, 0x00000001u, "slba = 2^32+1" },
        { 0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFu, 0x7FFFFFFFu, "slba = 2^63-1" },
        { 0x8000000000000000ULL, 0x00000000u, 0x80000000u, "slba = 2^63 (sign bit set)" },
        { 0xDEADBEEFCAFEBABEULL, 0xCAFEBABEu, 0xDEADBEEFu, "slba = 0xDEADBEEFCAFEBABE" },
        { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFu, 0xFFFFFFFFu, "slba = 2^64-1" },
    };
    for (size_t i = 0; i < sizeof sp / sizeof sp[0]; i++) {
        poison(c);
        memset(box.canary, 0x5C, sizeof box.canary);
        rc = iopath_encode_nvme_write(c, 4, sp[i].slba, 0, buf, 4096);
        T_EQ(rc, 0, "%s: encode returns 0", sp[i].name);
        T_EQ(c->cdw10, sp[i].lo, "%s: cdw10 == 0x%08X (low 32 of SLBA)", sp[i].name, sp[i].lo);
        T_EQ(c->cdw11, sp[i].hi, "%s: cdw11 == 0x%08X (high 32 of SLBA)", sp[i].name, sp[i].hi);
        T_EQ(c->cdw12 & 0xFFFFu, 0u, "%s: SLBA did not leak into the block count", sp[i].name);
        T_EQ(c->opcode, NVME_OPC_WRITE, "%s: opcode is still 0x01 WRITE", sp[i].name);
        T_EQ(c->nsid, 4u, "%s: nsid carried verbatim", sp[i].name);
        T_EQ(c->data_len, 4096u, "%s: data_len == 4096", sp[i].name);
        T_OK(c->addr == (uint64_t)(uintptr_t)buf, "%s: addr is the caller's buffer", sp[i].name);
        check_unused_zero(c, sp[i].name);
        int canary_ok = 1;
        for (size_t k = 0; k < sizeof box.canary; k++)
            if (box.canary[k] != 0x5C) canary_ok = 0;
        T_OK(canary_ok, "%s: encoder wrote nothing past the 72-byte command", sp[i].name);
    }

    /* Reconstructing the 64-bit value from the two halves must give back
     * exactly what went in -- the strongest single statement of the split. */
    for (size_t i = 0; i < sizeof sp / sizeof sp[0]; i++) {
        poison(c);
        (void)iopath_encode_nvme_write(c, 1, sp[i].slba, 0, buf, 512);
        uint64_t back = ((uint64_t)c->cdw11 << 32) | (uint64_t)c->cdw10;
        T_OK(back == sp[i].slba,
             "%s: (cdw11<<32)|cdw10 reconstructs the SLBA exactly", sp[i].name);
    }

    /* ================================================================ *
     * GROUP 2 -- NLB is ZERO BASED, pinned down from both directions.
     *
     * PROPERTY: the uint16_t the caller hands in is stored verbatim in cdw12
     * bits 15:00; the encoder neither adds nor subtracts one. WHY IT MATTERS:
     * src/iopath.c:14-18 states the convention and says the internal backends
     * do the blocks -> blocks-1 conversion BEFORE calling the encoder. If the
     * encoder also subtracted one, every transfer would be one block short
     * (silent truncation); if it added one, every transfer would overwrite one
     * block past the intended range (silent corruption of the next record).
     * ================================================================ */
    struct { uint16_t nlb; uint32_t blocks; } nl[] = {
        { 0,      1     },   /* the trap: 0 means ONE block, not zero blocks */
        { 1,      2     },
        { 7,      8     },
        { 0x00FF, 256   },
        { 0x0100, 257   },
        { 0xFFFE, 65535 },
        { 0xFFFF, 65536 },   /* the largest transfer one command can describe */
    };
    for (size_t i = 0; i < sizeof nl / sizeof nl[0]; i++) {
        poison(c);
        rc = iopath_encode_nvme_write(c, 1, 0x2A, nl[i].nlb, buf, 512);
        T_EQ(rc, 0, "nlb=%u: encode returns 0", (unsigned)nl[i].nlb);
        T_EQ(c->cdw12 & 0xFFFFu, (uint32_t)nl[i].nlb,
             "nlb=%u (%u blocks): cdw12[15:0] == %u verbatim, no +1 and no -1",
             (unsigned)nl[i].nlb, nl[i].blocks, (unsigned)nl[i].nlb);
        T_OK((c->cdw12 & 0xFFFFu) != (uint32_t)nl[i].blocks || nl[i].nlb == nl[i].blocks,
             "nlb=%u: the encoder did NOT store the one-based block count %u",
             (unsigned)nl[i].nlb, nl[i].blocks);
    }
    /* The single most valuable assertion in this file: nlb 0 must encode as 0,
     * because 0 is how NVMe spells "one block". An encoder that stored 1 here
     * would write two blocks for every one-block request. */
    poison(c);
    (void)iopath_encode_nvme_write(c, 1, 0, 0, buf, 512);
    T_EQ(c->cdw12 & 0xFFFFu, 0u,
         "one 512-byte block -> cdw12[15:0] == 0 (zero-based: 0 means 1 block)");
    T_EQ(c->cdw12, 0u, "one block, no FUA: cdw12 is entirely zero");

    /* The block count must not spill into cdw13 or above. */
    poison(c);
    (void)iopath_encode_nvme_write(c, 1, 0, 0xFFFF, buf, 512);
    T_EQ(c->cdw12 >> 16, 0u, "nlb=0xFFFF: cdw12[31:16] stays clear (no FUA asked)");
    T_EQ(c->cdw13, 0u, "nlb=0xFFFF: nothing spilled into cdw13");

    /* ================================================================ *
     * GROUP 3 -- FUA is cdw12 bit 30, set ONLY when asked, and nothing else.
     *
     * PROPERTY: with fua != 0, cdw12 == nlb | 0x40000000 exactly. WHY EXACTLY:
     * cdw12 bit 31 is Limited Retry and bits 29:26 are PRINFO (protection
     * information). Setting any of those by accident changes what the drive
     * does with the data. include/exitos_iopath.h says only "sets the Force
     * Unit Access bit (cdw12 bit 30) when fua != 0".
     * ================================================================ */
    struct { int fua; int want; const char *name; } fv[] = {
        { 0,       0, "fua = 0"          },
        { 1,       1, "fua = 1"          },
        { 2,       1, "fua = 2"          },
        { -1,      1, "fua = -1"         },
        { INT_MIN, 1, "fua = INT_MIN"    },
        { INT_MAX, 1, "fua = INT_MAX"    },
    };
    for (size_t i = 0; i < sizeof fv / sizeof fv[0]; i++) {
        poison(c);
        rc = iopath_encode_nvme_write_fua(c, 9, 0x100000000ULL, 7, buf, 4096, fv[i].fua);
        T_EQ(rc, 0, "%s: encode returns 0", fv[i].name);
        T_EQ((c->cdw12 >> 30) & 1u, (unsigned)fv[i].want,
             "%s: cdw12 bit 30 (FUA) is %d", fv[i].name, fv[i].want);
        T_EQ(c->cdw12, 7u | (fv[i].want ? FUA_BIT : 0u),
             "%s: cdw12 == 0x%08X exactly (NLB intact, bit 31 and PRINFO clear)",
             fv[i].name, 7u | (fv[i].want ? FUA_BIT : 0u));
        T_EQ(c->cdw10, 0u, "%s: SLBA low half unaffected by FUA", fv[i].name);
        T_EQ(c->cdw11, 1u, "%s: SLBA high half unaffected by FUA", fv[i].name);
        T_EQ(c->opcode, NVME_OPC_WRITE, "%s: still a WRITE", fv[i].name);
        check_unused_zero(c, fv[i].name);
    }

    /* FUA must not survive the call. The flag lives in a thread-local that the
     * FUA wrapper sets and clears; if it ever leaked, a later plain write would
     * silently become a synchronous one -- a large, invisible slowdown. */
    (void)iopath_encode_nvme_write_fua(c, 1, 0, 0, buf, 512, 1);
    poison(c);
    (void)iopath_encode_nvme_write(c, 1, 0, 0, buf, 512);
    T_EQ((c->cdw12 >> 30) & 1u, 0u,
         "a plain encode right after a FUA encode has FUA clear (no state leak)");

    /* Same, when the FUA call fails: the error path must still clear it. */
    rc = iopath_encode_nvme_write_fua(NULL, 1, 0, 0, buf, 512, 1);
    T_OK(rc < 0, "FUA encode with cmd == NULL is refused (got %d)", rc);
    poison(c);
    (void)iopath_encode_nvme_write(c, 1, 0, 0, buf, 512);
    T_EQ((c->cdw12 >> 30) & 1u, 0u,
         "FUA state cleared even when the FUA encode failed (cmd == NULL)");
    rc = iopath_encode_nvme_write_fua(c, 1, 0, 0, NULL, 512, 1);
    T_OK(rc < 0, "FUA encode with buf == NULL is refused (got %d)", rc);
    poison(c);
    (void)iopath_encode_nvme_write(c, 1, 0, 0, buf, 512);
    T_EQ((c->cdw12 >> 30) & 1u, 0u,
         "FUA state cleared even when the FUA encode failed (buf == NULL)");

    /* Concurrency: one thread asking for FUA must not turn another thread's
     * plain writes into FUA writes. */
    {
        pthread_t th;
        int leaked = 0, spawned = (pthread_create(&th, NULL, fua_hammer, NULL) == 0);
        if (spawned) {
            for (int i = 0; i < 20000; i++) {
                struct nvme_passthru_cmd t;
                (void)iopath_encode_nvme_write(&t, 1, 0, 5, buf, 512);
                if (t.cdw12 != 5u) { leaked = 1; break; }
            }
            pthread_join(th, NULL);
            T_OK(!leaked,
                 "20000 concurrent plain encodes never picked up another thread's FUA bit");
        } else {
            T_SKIP("pthread_create failed - concurrency probe not run");
        }
    }

    /* ================================================================ *
     * GROUP 4 -- encoder refusals and boundaries.
     *
     * PROPERTY: data_len is 32 bits wide, so any len above UINT32_MAX must be
     * refused rather than truncated. WHY IT MATTERS: a truncating encoder would
     * turn a 4 GiB + 512 byte request into a 512-byte one and report success --
     * the caller would believe 4 GiB reached the media.
     * ================================================================ */
    rc = iopath_encode_nvme_write(NULL, 1, 0, 0, buf, 512);
    T_OK(rc < 0, "encode(cmd == NULL) refused (got %d, want < 0)", rc);
    rc = iopath_encode_nvme_write(c, 1, 0, 0, NULL, 512);
    T_OK(rc < 0, "encode(buf == NULL) refused (got %d, want < 0)", rc);
    rc = iopath_encode_nvme_write(NULL, 1, 0, 0, NULL, 512);
    T_OK(rc < 0, "encode(cmd == NULL && buf == NULL) refused (got %d)", rc);

    poison(c);
    rc = iopath_encode_nvme_write(c, 1, 0, 0, buf, (size_t)UINT32_MAX);
    T_EQ(rc, 0, "len == UINT32_MAX is the largest data_len that fits: accepted");
    T_EQ(c->data_len, UINT32_MAX, "len == UINT32_MAX stored without truncation");

    rc = iopath_encode_nvme_write(c, 1, 0, 0, buf, (size_t)UINT32_MAX + 1);
    T_OK(rc < 0, "len == 2^32 does not fit data_len: refused (got %d, want < 0)", rc);
    rc = iopath_encode_nvme_write(c, 1, 0, 0, buf, SIZE_MAX);
    T_OK(rc < 0, "len == SIZE_MAX refused (got %d, want < 0)", rc);

    /* A WRITE that transfers zero bytes is not a command any device can honour,
     * and the module refuses len == 0 everywhere else it looks at a length:
     * check_io_args() (src/iopath.c:179) returns -EINVAL for len == 0, and
     * nvme_rw() (src/iopath.c:553) returns -EINVAL when blocks == 0. The
     * exported encoder should hold the same line; it currently accepts it and
     * produces a WRITE with data_len 0 while NLB still claims nlb+1 blocks. */
    poison(c);
    rc = iopath_encode_nvme_write(c, 1, 0, 0, buf, 0);
    T_OK(rc < 0,
         "encode(len == 0) refused like every other length check in the module "
         "(got %d, want < 0; cdw12[15:0]=%u claims %u block(s) with data_len=%u)",
         rc, c->cdw12 & 0xFFFFu, (c->cdw12 & 0xFFFFu) + 1u, c->data_len);

    /* ================================================================ *
     * GROUP 5 -- iopath_setup_flags for every backend, exactly.
     *
     * PROPERTY: the value returned is handed unchanged to io_uring_setup()
     * (src/iopath.c:288, "params.flags = iopath_setup_flags(p->backend)"), so
     * these constants are kernel ABI, not local bookkeeping, and the mapping
     * must be exact -- not merely "has the right bit set". WHY IT MATTERS: a
     * stray IORING_SETUP_IOPOLL on a device without poll queues gives a ring
     * whose commands never complete (the header says so: "a backend that polls
     * a device without poll queues never completes"). A missing SQE128 gives a
     * 64-byte submission entry, into which the 72-byte NVMe passthru command
     * does not fit.
     * ================================================================ */
    T_EQ(iopath_setup_flags(IOPATH_PWRITE), 0u,
         "pwrite backend requests no ring flags at all");
    T_EQ(iopath_setup_flags(IOPATH_NVME_IOCTL), 0u,
         "ioctl backend requests no ring flags at all (it uses no ring)");
    /* Both halves, not just the submission half. The 128-byte SQE carries the
     * NVMe command down; the 32-byte CQE carries its completion result back,
     * and a controller refuses the command with EOPNOTSUPP when the ring cannot
     * carry one. Asserting SQE128 alone is what let a backend ship that failed
     * every single command on real hardware while this test passed. */
    T_EQ(iopath_setup_flags(IOPATH_URING_CMD),
         IOPATH_SETUP_SQE128 | IOPATH_SETUP_CQE32,
         "uring backend requests SQE128 and CQE32, and NOT IOPOLL");
    T_EQ(iopath_setup_flags(IOPATH_URING_CMD_POLL),
         IOPATH_SETUP_SQE128 | IOPATH_SETUP_CQE32 | IOPATH_SETUP_IOPOLL,
         "polled uring backend requests SQE128|CQE32|IOPOLL");
    T_EQ(iopath_setup_flags(IOPATH_URING_WRITE_POLL),
         IOPATH_SETUP_IOPOLL,
         "polled block WRITE backend requests only IOPOLL (SQE64/CQE16)");
    T_EQ(iopath_setup_flags(IOPATH_URING_CMD) & IOPATH_SETUP_IOPOLL, 0u,
         "the non-polled uring backend must never ask for a polled ring");
    T_EQ(iopath_setup_flags(IOPATH_URING_CMD_POLL) & IOPATH_SETUP_SQE128,
         IOPATH_SETUP_SQE128, "the polled backend still needs 128-byte SQEs");
    /* Values outside the enum: must be the safe default, never IOPOLL. */
    T_EQ(iopath_setup_flags((iopath_backend)5), 0u, "backend 5 (out of range) -> 0");
    T_EQ(iopath_setup_flags((iopath_backend)99), 0u, "backend 99 (out of range) -> 0");
    T_EQ(iopath_setup_flags((iopath_backend)-1), 0u, "backend -1 (out of range) -> 0");
    /* Pure function: no hidden state between calls. */
    T_EQ(iopath_setup_flags(IOPATH_PWRITE), 0u,
         "setup_flags(PWRITE) still 0 after all the calls above (no hidden state)");
    /* The constants themselves must equal the kernel's. */
    T_EQ(IOPATH_SETUP_IOPOLL, IORING_SETUP_IOPOLL,
         "IOPATH_SETUP_IOPOLL == IORING_SETUP_IOPOLL from linux/io_uring.h");
    T_EQ(IOPATH_SETUP_SQE128, 1u << 10,
         "IOPATH_SETUP_SQE128 == 1<<10, the kernel's IORING_SETUP_SQE128");

    /* ================================================================ *
     * GROUP 6 -- iopath_poll_available.
     *
     * PROPERTY: "Never guesses" (include/exitos_iopath.h). 0 for anything it
     * cannot positively confirm, and never a crash. WHY IT MATTERS: a false 1
     * selects the polled backend on a device without poll queues, and those
     * commands never complete -- the process hangs forever.
     * ================================================================ */
    T_EQ(iopath_poll_available(NULL), 0, "poll_available(NULL) == 0, no crash");
    T_EQ(iopath_poll_available(""), 0, "poll_available(\"\") == 0");
    T_EQ(iopath_poll_available("/"), 0, "poll_available(\"/\") == 0");
    T_EQ(iopath_poll_available("/dev/"), 0, "poll_available(trailing slash) == 0");
    T_EQ(iopath_poll_available("/dev/exitos-no-such-device-12345"), 0,
         "poll_available(missing path) == 0");
    T_EQ(iopath_poll_available("/etc/hostname"), 0,
         "poll_available(a regular file, not a block device) == 0");
    T_EQ(iopath_poll_available("/tmp"), 0, "poll_available(a directory) == 0");
    T_EQ(iopath_poll_available("/dev/null"), 0,
         "poll_available(/dev/null, a character device) == 0");
    T_EQ(iopath_poll_available("../../../etc/passwd"), 0,
         "poll_available(a relative traversal path) == 0");
    {   /* Overlong input: the internal buffers are 128 and 320 bytes. Must
         * truncate safely and answer 0, not smash the stack. */
        char big[4096];
        memset(big, 'q', sizeof big - 1);
        big[0] = '/';
        big[sizeof big - 1] = '\0';
        T_EQ(iopath_poll_available(big), 0, "poll_available(4095-byte path) == 0, no crash");
    }
    {   /* The lookup uses only the BASENAME of what it is given, so a path that
         * is not a device at all still probes /sys/class/block/<basename>. That
         * can only be caught on a host that has a poll-capable disk to borrow a
         * name from; otherwise every answer is 0 and the two cases are
         * indistinguishable. Report which case this run is. */
        const char *disk = find_poll_capable_disk();
        if (disk) {
            char bogus[320];
            snprintf(bogus, sizeof bogus, "/tmp/definitely-not-a-device-%d/%.100s",
                     (int)getpid(), disk);
            T_EQ(iopath_poll_available(bogus), 0,
                 "a nonexistent path whose basename happens to be '%s' must NOT "
                 "inherit that disk's poll capability", disk);
        } else {
            T_SKIP("no block device on this host reports queue/io_poll == 1, so the "
                   "basename-only lookup cannot be distinguished from a correct one here");
        }
    }

    /* ================================================================ *
     * GROUP 7 -- NULL handles on every exported entry point.
     *
     * PROPERTY: an error return, never a dereference. WHY IT MATTERS: these are
     * the paths a caller reaches after a failed open, which is exactly when a
     * crash is least acceptable.
     * ================================================================ */
    fflush(stdout);
    T_OK(iopath_write(NULL, 0, buf, 4096) < 0, "iopath_write(NULL handle) refused");
    fflush(stdout);
    T_OK(iopath_read(NULL, 0, buf, 4096) < 0, "iopath_read(NULL handle) refused");
    fflush(stdout);
    T_OK(iopath_flush(NULL) < 0, "iopath_flush(NULL handle) refused");
    fflush(stdout);
    T_EQ(iopath_set_fua(NULL, 1), -EINVAL, "iopath_set_fua(NULL, 1) == -EINVAL");
    T_EQ(iopath_set_fua(NULL, 0), -EINVAL, "iopath_set_fua(NULL, 0) == -EINVAL");
    T_EQ(iopath_get_fua(NULL), -EINVAL, "iopath_get_fua(NULL) == -EINVAL");
    T_OK(iopath_get_fua(NULL) < 0,
         "iopath_get_fua(NULL) is negative, so it cannot be read as 'FUA is on'");
    iopath_close(NULL);
    T_OK(1, "iopath_close(NULL) is a no-op");
    T_OK(iopath_open(NULL, IOPATH_PWRITE, 4096) == NULL, "iopath_open(NULL path) == NULL");

    /* ================================================================ *
     * GROUP 8 -- scratch objects for the open/backend matrix.
     * ================================================================ */
    snprintf(g_dir, sizeof g_dir, "/tmp/exitos_ioph_%d", (int)getpid());
    mkdir(g_dir, 0700);
    snprintf(g_file, sizeof g_file, "%s/plain.img", g_dir);
    snprintf(g_fifo, sizeof g_fifo, "%s/fifo", g_dir);
    atexit(cleanup);
    int have_file = (write_file(g_file, 0x11, 65536) == 0);
    int have_fifo = (mkfifo(g_fifo, 0600) == 0);

    /* ---- 8a. Argument validation, independent of backend ---- */
    const struct { iopath_backend b; const char *name; } be[] = {
        { IOPATH_PWRITE,         "PWRITE"         },
        { IOPATH_NVME_IOCTL,     "NVME_IOCTL"     },
        { IOPATH_URING_CMD,      "URING_CMD"      },
        { IOPATH_URING_CMD_POLL, "URING_CMD_POLL" },
        { IOPATH_URING_WRITE_POLL, "URING_WRITE_POLL" },
    };
    for (size_t i = 0; i < sizeof be / sizeof be[0]; i++) {
        p = iopath_open(NULL, be[i].b, 4096);
        T_OK(p == NULL, "%s: open(NULL path) == NULL", be[i].name);
        if (p) iopath_close(p);
        p = iopath_open("", be[i].b, 4096);
        T_OK(p == NULL, "%s: open(empty path) == NULL", be[i].name);
        if (p) iopath_close(p);
        p = iopath_open("/dev/exitos-no-such-device-12345", be[i].b, 4096);
        T_OK(p == NULL, "%s: open(nonexistent path) == NULL", be[i].name);
        if (p) iopath_close(p);
        p = iopath_open(g_dir, be[i].b, 4096);
        T_OK(p == NULL, "%s: open(a directory) == NULL", be[i].name);
        if (p) iopath_close(p);
        /* A logical block size that is not a power of two makes every offset
         * computation meaningless, so it must be refused before any fd exists. */
        if (have_file) {
            uint32_t bad[] = { 0u, 3u, 511u, 4097u, 0xFFFFFFFFu };
            for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
                p = iopath_open(g_file, be[i].b, bad[k]);
                T_OK(p == NULL, "%s: open(lbs = %u, not a power of two) == NULL",
                     be[i].name, bad[k]);
                if (p) iopath_close(p);
            }
        }
    }
    /* Backend values that are not in the enum at all. */
    if (have_file) {
        int bogus_bk[] = { 5, 6, 99, -1, INT_MAX };
        for (size_t k = 0; k < sizeof bogus_bk / sizeof bogus_bk[0]; k++) {
            p = iopath_open(g_file, (iopath_backend)bogus_bk[k], 4096);
            T_OK(p == NULL, "open(backend = %d, outside the enum) == NULL", bogus_bk[k]);
            if (p) iopath_close(p);
        }
    }

    /* ---- 8b. Node types that cannot be addressed by an offset ----
     *
     * PROPERTY: iopath_open must refuse a node that has no byte offsets.
     * src/iopath.c:470-472 states this intent in its own words: "A directory is
     * not a device; open(O_RDWR) already refused it with EISDIR, but say so
     * explicitly for anything else that is not a file we can address by
     * offset." A FIFO and /dev/null are precisely "anything else that is not a
     * file we can address by offset", and the header says the two NVMe backends
     * need "a real NVMe". WHY IT MATTERS: a mistyped path that happens to name
     * /dev/null yields a handle whose writes RETURN SUCCESS and store nothing.
     */
    int uring128 = uring_sqe128_supported();
    T_OK(1, "this kernel %s IORING_SETUP_SQE128 (128-byte SQEs)",
         uring128 ? "supports" : "does NOT support");

    struct iopath *h_null_pw = iopath_open("/dev/null", IOPATH_PWRITE, 4096);
    T_OK(h_null_pw == NULL,
         "PWRITE: open(/dev/null, a character device) == NULL "
         "(it is not a block device and stores nothing)");
    if (h_null_pw) {
        /* Demonstrate the cost of accepting it: the write reports success and
         * the data cannot be read back. */
        int wr = iopath_write(h_null_pw, 0, buf, 4096);
        int rd = iopath_read(h_null_pw, 0, buf, 4096);
        T_OK(!(wr == 0 && rd < 0),
             "PWRITE on /dev/null: write returned %d (success) while read back "
             "returned %d -- 4096 bytes reported as written are unrecoverable", wr, rd);
        iopath_close(h_null_pw);
    }

    struct iopath *h_fifo = have_fifo ? iopath_open(g_fifo, IOPATH_PWRITE, 4096) : NULL;
    if (!have_fifo) {
        T_SKIP("mkfifo failed - FIFO open test not run");
    } else {
        T_OK(h_fifo == NULL,
             "PWRITE: open(a FIFO) == NULL (a FIFO has no byte offsets at all)");
    }

    struct iopath *h_null_io = iopath_open("/dev/null", IOPATH_NVME_IOCTL, 512);
    T_OK(h_null_io == NULL,
         "NVME_IOCTL: open(/dev/null) == NULL -- the header says this backend "
         "\"Needs a real NVMe\", and /dev/null is not one");

    struct iopath *h_file_io = have_file ? iopath_open(g_file, IOPATH_NVME_IOCTL, 512) : NULL;
    if (!have_file) {
        T_SKIP("could not create the scratch file - regular-file open tests not run");
    } else {
        T_OK(h_file_io == NULL,
             "NVME_IOCTL: open(a regular file) == NULL -- a regular file is not "
             "an NVMe controller");
        struct iopath *h_file_ur = iopath_open(g_file, IOPATH_URING_CMD, 512);
        if (uring128) {
            /* Ring setup would succeed here, and uring_setup() never looks at
             * the target fd, so a handle would mean the uring backend accepts a
             * regular file just as the ioctl backend does. */
            T_OK(h_file_ur == NULL,
                 "URING_CMD: open(a regular file) == NULL on a kernel that does "
                 "support SQE128");
        } else {
            T_OK(h_file_ur == NULL,
                 "URING_CMD: open(a regular file) == NULL, but on THIS kernel "
                 "that proves nothing about the target: io_uring_setup(SQE128) "
                 "fails here, so every uring open fails regardless of the path");
        }
        if (h_file_ur) iopath_close(h_file_ur);
        /* The asymmetry, stated as one assertion: two backends, same target,
         * same answer expected. */
        T_OK((h_file_io == NULL) == (h_file_ur == NULL),
             "NVME_IOCTL and URING_CMD agree on whether a regular file is a "
             "valid NVMe target (ioctl handle=%s, uring handle=%s)",
             h_file_io ? "yes" : "no", h_file_ur ? "yes" : "no");
    }

    /* ---- 8c. What a wrongly-accepted NVMe handle then does ----
     * If the module hands out a handle for a non-NVMe node, the least it must
     * do is fail every command rather than report success. */
    if (h_null_io) {
        int w = iopath_write(h_null_io, 0, buf, 512);
        T_OK(w < 0, "NVMe handle over /dev/null: write fails (%d) instead of "
                    "reporting a success that stored nothing", w);
        int f = iopath_flush(h_null_io);
        T_OK(f < 0, "NVMe handle over /dev/null: flush fails (%d)", f);

        /* The 65536-block ceiling. NLB is 16 bits and zero based, so one
         * command can describe at most 65536 blocks (src/iopath.c:59-61).
         * 65536 must be ACCEPTED (and fail later, at the ioctl); 65537 must be
         * refused up front with -EINVAL. Distinguishing the two return values
         * is what proves the limit is enforced in the right place. */
        void *big = NULL;
        if (posix_memalign(&big, 4096, (size_t)65537 * 512) == 0 && big) {
            int r1 = iopath_write(h_null_io, 0, big, (size_t)65536 * 512);
            T_OK(r1 != -EINVAL,
                 "65536 blocks (the maximum one NVMe command can describe) is "
                 "accepted by the encoder and only fails at submission (got %d)", r1);
            int r2 = iopath_write(h_null_io, 0, big, (size_t)65537 * 512);
            T_EQ(r2, -EINVAL,
                 "65537 blocks exceeds the 16-bit zero-based NLB field: refused "
                 "with -EINVAL before any submission");
            free(big);
        } else {
            T_SKIP("could not allocate 32 MiB - NLB ceiling test not run");
        }
    } else {
        T_SKIP("no NVMe-backend handle over a non-NVMe node - submission-side "
               "tests for it not run (this is the GOOD outcome)");
    }

    /* ================================================================ *
     * GROUP 9 -- iopath_set_fua / iopath_get_fua on a real handle.
     *
     * PROPERTY (include/exitos_iopath.h): native NVMe handles can put FUA on
     * subsequent writes.  PWRITE has no NVMe command dword and must reject the
     * promise, retaining flush-based durability.  Accepting it used to make
     * iopath_flush() a no-op while the actual writes carried no FUA.
     *
     * The two witnesses below are chosen so that "returns immediately" is
     * OBSERVABLE: on a target whose flush cannot succeed, an implementation that
     * honours the sentence returns 0 without touching the target, while one that
     * ignores the flag returns the target's error.
     * ================================================================ */
    struct iopath *h_file_pw = have_file ? iopath_open(g_file, IOPATH_PWRITE, 4096) : NULL;
    if (h_file_pw) {
        T_EQ(iopath_get_fua(h_file_pw), 0, "a fresh handle has FUA off");
        T_EQ(iopath_set_fua(h_file_pw, 1), -EOPNOTSUPP,
             "pwrite set_fua(1) is refused: this backend cannot encode it");
        T_EQ(iopath_get_fua(h_file_pw), 0,
             "a refused pwrite FUA request leaves flush mode enabled");
        T_EQ(iopath_set_fua(h_file_pw, 0), 0, "set_fua(0) returns 0");
        T_EQ(iopath_get_fua(h_file_pw), 0, "get_fua reads back 0");
        T_EQ(iopath_set_fua(h_file_pw, 42), -EOPNOTSUPP,
             "every nonzero pwrite FUA request is refused");
        T_EQ(iopath_get_fua(h_file_pw), 0,
             "non-boolean input cannot put pwrite into fake FUA mode");
        T_EQ(iopath_set_fua(h_file_pw, INT_MIN), -EOPNOTSUPP,
             "negative nonzero pwrite FUA request is also refused");
        T_EQ(iopath_get_fua(h_file_pw), 0,
             "pwrite remains in flush mode after every refused request");
        /* A handle setting must not change what a stateless plain encoder
         * produces. */
        (void)iopath_set_fua(h_file_pw, 1);
        poison(c);
        (void)iopath_encode_nvme_write(c, 1, 0, 0, buf, 512);
        T_EQ((c->cdw12 >> 30) & 1u, 0u,
             "the handle FUA flag does not bleed into the stateless encoder");
    } else {
        T_SKIP("no pwrite handle over the scratch file - set/get FUA tests not run");
    }

    if (h_fifo) {
        (void)iopath_set_fua(h_fifo, 0);
        int f_off = iopath_flush(h_fifo);
        (void)iopath_set_fua(h_fifo, 1);
        int f_on  = iopath_flush(h_fifo);
        T_EQ(f_on, 0,
             "PWRITE handle with FUA on: flush \"has nothing left to do and "
             "returns immediately\" -> 0 (with FUA off it returns %d)", f_off);
        T_OK(f_on != f_off,
             "turning FUA on changes what iopath_flush does (off=%d, on=%d)",
             f_off, f_on);
        iopath_close(h_fifo);
    }
    if (h_null_io) {
        (void)iopath_set_fua(h_null_io, 0);
        int f_off = iopath_flush(h_null_io);
        (void)iopath_set_fua(h_null_io, 1);
        int f_on  = iopath_flush(h_null_io);
        T_EQ(f_on, 0,
             "NVMe handle with FUA on: flush returns immediately -> 0, no FLUSH "
             "command is built (with FUA off it returns %d)", f_off);
        iopath_close(h_null_io);
    }
    if (h_file_io) iopath_close(h_file_io);

    /* ================================================================ *
     * GROUP 10 -- the pwrite backend's LBA arithmetic and argument rules,
     * exercised against an ordinary file in /tmp. No device is involved: the
     * property under test is "byte offset == lba * lbs", which is arithmetic.
     *
     * WHY IT MATTERS: this is the one backend that can be validated without
     * hardware, so it is the reference the NVMe encoders are checked against.
     * ================================================================ */
    if (h_file_pw) {
        unsigned char *pat = buf;
        memset(pat, 0x5A, 4096);
        g_internal_enter_calls = g_internal_leave_calls = 0;
        g_internal_last_fd = -1;
        rc = iopath_write(h_file_pw, 7, pat, 4096);
        T_EQ(rc, 0, "pwrite backend: write of 1 block at lba 7 returns 0");
        T_EQ(g_internal_enter_calls, 0,
             "actual raw pwrite uses the explicit direct-call API, not a stealable marker");
        T_EQ(g_internal_leave_calls, 0,
             "actual raw pwrite leaves no exact-fd marker window");
        /* Read the raw file and check the data landed at 7 * 4096 = 28672. */
        int fd = open(g_file, O_RDONLY);
        unsigned char probe[4096];
        int at_target = 0, at_zero = 0;
        if (fd >= 0) {
            if (pread(fd, probe, 4096, 28672) == 4096) at_target = (probe[0] == 0x5A);
            if (pread(fd, probe, 4096, 0) == 4096)      at_zero   = (probe[0] == 0x5A);
            close(fd);
        }
        T_OK(at_target, "lba 7 with lbs 4096 landed at byte offset 28672 exactly");
        T_OK(!at_zero, "nothing was written at offset 0 (the LBA was not ignored)");

        memset(pat, 0, 4096);
        rc = iopath_read(h_file_pw, 7, pat, 4096);
        T_EQ(rc, 0, "pwrite backend: read back at lba 7 returns 0");
        T_EQ(pat[0], 0x5A, "read at lba 7 returns what write at lba 7 stored");
        T_EQ(pat[4095], 0x5A, "the whole block came back, not just the first byte");

        {
            int before_enter = g_internal_enter_calls;
            int before_leave = g_internal_leave_calls;
            T_EQ(iopath_flush(h_file_pw), 0,
                 "pwrite backend flush succeeds on scratch file");
            T_EQ(g_internal_enter_calls, before_enter,
                 "actual raw fdatasync uses the explicit direct-call API, not a marker");
            T_EQ(g_internal_leave_calls, before_leave,
                 "actual raw fdatasync leaves no exact-fd marker window");
        }

        /* Argument rules. src/iopath.c:20-23: "len must be a multiple of lbs,
         * and buf must be aligned to lbs. Both are refused with -EINVAL on
         * every backend." */
        T_EQ(iopath_write(h_file_pw, 0, NULL, 4096), -EINVAL, "write(buf == NULL) == -EINVAL");
        T_EQ(iopath_read(h_file_pw, 0, NULL, 4096), -EINVAL,  "read(buf == NULL) == -EINVAL");
        T_EQ(iopath_write(h_file_pw, 0, buf, 0), -EINVAL,     "write(len == 0) == -EINVAL");
        T_EQ(iopath_write(h_file_pw, 0, buf, 4095), -EINVAL,  "write(len not a multiple of lbs) == -EINVAL");
        T_EQ(iopath_write(h_file_pw, 0, buf, 1), -EINVAL,     "write(len == 1) == -EINVAL");
        T_EQ(iopath_write(h_file_pw, 0, buf + 1, 4096), -EINVAL, "write(buf misaligned by 1) == -EINVAL");
        T_EQ(iopath_write(h_file_pw, 0, buf + 512, 4096), -EINVAL,
             "write(buf aligned to 512 but not to lbs 4096) == -EINVAL");
        T_EQ(iopath_read(h_file_pw, 0, buf + 1, 4096), -EINVAL, "read(buf misaligned) == -EINVAL");

        /* Overflow. lba * lbs must never wrap into a small offset: that would
         * write over the start of the device instead of failing. */
        T_EQ(iopath_write(h_file_pw, UINT64_MAX, buf, 4096), -EOVERFLOW,
             "write(lba = 2^64-1) == -EOVERFLOW, not a wrapped-around offset");
        {
            uint64_t off_max = test_off_t_max();
            uint64_t first_unrepresentable_lba = off_max / 4096 + 1;
            uint64_t end_crossing_lba = off_max / 4096;
            T_EQ(iopath_write(h_file_pw, first_unrepresentable_lba, buf, 4096),
                 -EOVERFLOW,
                 "write offset beyond this ABI's off_t maximum is refused");
            T_EQ(iopath_write(h_file_pw, end_crossing_lba, buf, 4096),
                 -EOVERFLOW,
                 "write whose byte window crosses this ABI's off_t maximum is refused");
        }
        T_EQ(iopath_read(h_file_pw, UINT64_MAX, buf, 4096), -EOVERFLOW,
             "read(lba = 2^64-1) == -EOVERFLOW");
        /* A short read at the end of the file is an error, not a success:
         * src/iopath.c:541 "end of device: short read, not a success". */
        T_OK(iopath_read(h_file_pw, 1000000, buf, 4096) < 0,
             "read past the end of the target fails instead of returning stale data");
        {
            int before_enter = g_internal_enter_calls;
            int before_leave = g_internal_leave_calls;
            iopath_close(h_file_pw);
            T_EQ(g_internal_enter_calls, before_enter,
                 "data-fd close uses explicit internal call, not marker token");
            T_EQ(g_internal_leave_calls, before_leave,
                 "explicit internal data-fd close needs no frontend marker token");
        }
    }

    /* ================================================================ *
     * GROUP 11 -- a device path longer than the internal 512-byte buffer.
     *
     * PROPERTY: if iopath_open cannot represent the path it was given, it must
     * refuse it. src/iopath.c:445-446 copies dev_path into "char path[512]"
     * with snprintf, which TRUNCATES silently at 511 bytes and reports nothing.
     * WHY IT MATTERS: if the truncated prefix happens to name a different
     * existing file, every write goes to the wrong target while every call
     * returns success. That is silent data loss, and the caller has no way to
     * detect it. The two files below are built so that the 511-byte prefix of
     * the second one is exactly the first one.
     * ================================================================ */
    {
        char comp[256];
        memset(comp, 'd', 200); comp[200] = '\0';
        snprintf(g_d1, sizeof g_d1, "%.200s/%.250s", g_dir, comp);
        memset(comp, 'e', 200); comp[200] = '\0';
        snprintf(g_d2, sizeof g_d2, "%.500s/%.250s", g_d1, comp);
        int made = (mkdir(g_d1, 0700) == 0) && (mkdir(g_d2, 0700) == 0);
        size_t need = 0;
        if (made) {
            size_t base = strlen(g_d2) + 1;      /* "<g_d2>/" */
            if (base < 511 && 511 - base < 250) {
                need = 511 - base;
                memset(comp, 'f', need); comp[need] = '\0';
                snprintf(g_longA, sizeof g_longA, "%.700s/%.250s", g_d2, comp);
                snprintf(g_longB, sizeof g_longB, "%.900sZZZZZZ", g_longA);
            } else {
                made = 0;
            }
        }
        if (!made || strlen(g_longA) != 511) {
            T_SKIP("could not build a 511-byte path (built %zu bytes) - "
                   "path-truncation test not run", strlen(g_longA));
        } else if (write_file(g_longA, 0x11, 4096) != 0 ||
                   write_file(g_longB, 0x22, 4096) != 0) {
            T_SKIP("could not create the two long-path files - truncation test not run");
        } else {
            T_EQ(strlen(g_longA), 511u, "the short file's path is exactly 511 bytes");
            T_EQ(strlen(g_longB), 517u, "the long file's path is exactly 517 bytes "
                                        "and starts with the short one");
            struct iopath *hp = iopath_open(g_longB, IOPATH_PWRITE, 4096);
            if (hp == NULL) {
                T_OK(1, "open(a 517-byte path) is refused rather than silently "
                        "truncated to 511 bytes");
            } else {
                memset(buf, 0x5A, 4096);
                int w = iopath_write(hp, 0, buf, 4096);
                iopath_close(hp);
                int a = first_byte_of(g_longA), b = first_byte_of(g_longB);
                T_EQ(b, 0x5A,
                     "write through a 517-byte path landed in the file that was "
                     "NAMED (write returned %d; that file still starts with 0x%02X)",
                     w, b);
                T_EQ(a, 0x11,
                     "the unrelated file whose path is the 511-byte prefix was "
                     "NOT overwritten (it now starts with 0x%02X)", a);
            }
        }
    }

    /* ================================================================ *
     * GROUP 12 -- IOPATH_URING_CMD_POLL cannot be opened at all.
     *
     * iopath_setup_flags() maps it (checked in group 5), and the header
     * documents it as a distinct completion backend. But
     * src/iopath.c:433 reads
     *     if (b != IOPATH_PWRITE && b != IOPATH_NVME_IOCTL && b != IOPATH_URING_CMD)
     *         return NULL;
     * so the enum value is rejected before the path is even opened, and
     * iopath_close() at src/iopath.c:502 only tears the ring down for
     * IOPATH_URING_CMD, so a polled handle would also leak its ring fd and
     * three mappings if it ever existed. On a kernel without SQE128 the uring
     * backends fail anyway, so this cannot be told apart from outside here.
     * ================================================================ */
    T_OK(IOPATH_URING_CMD != IOPATH_URING_CMD_POLL,
         "the polled backend is a distinct enum value (%d vs %d)",
         (int)IOPATH_URING_CMD, (int)IOPATH_URING_CMD_POLL);
    if (uring128 && have_file) {
        struct iopath *a = iopath_open(g_file, IOPATH_URING_CMD, 512);
        struct iopath *b = iopath_open(g_file, IOPATH_URING_CMD_POLL, 512);
        T_OK((a == NULL) == (b == NULL),
             "URING_CMD and URING_CMD_POLL, which differ only by one ring setup "
             "flag, agree on the same target (plain=%s, polled=%s)",
             a ? "handle" : "NULL", b ? "handle" : "NULL");
        if (a) iopath_close(a);
        if (b) iopath_close(b);
    } else {
        T_SKIP("io_uring_setup(SQE128) is unavailable on this kernel, so every "
               "uring open fails for that reason alone and the extra rejection of "
               "IOPATH_URING_CMD_POLL at src/iopath.c:433 cannot be observed from "
               "outside the module");
    }

    /* ================================================================ *
     * GROUP 8 -- asking for polled completion on a device that does not
     * offer it must be refused, not quietly downgraded.
     *
     * REQUIREMENT: a caller that selects the polled backend and gets a working
     * handle is entitled to assume its completions were polled. Observed on
     * an NVMe test device with poll_queues=0: setting up the ring with
     * IORING_SETUP_IOPOLL succeeded, the kernel mapped the request onto an
     * ordinary interrupt queue, and every command completed -- so the caller
     * timed an interrupt-driven path and reported it as polled. The two
     * completion paths are not interchangeable.
     *
     * iopath_poll_available() already exists to answer exactly this question
     * and nothing in the codebase calls it.
     * ================================================================ */
    {
        char nvme[320] = "";
        DIR *d = opendir("/sys/class/block");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                char probe[320];
                struct stat st;
                if (strncmp(e->d_name, "nvme", 4) != 0) continue;
                if (strchr(e->d_name, 'c') != NULL) continue;   /* nvmeXcYnZ alias */
                snprintf(probe, sizeof probe, "/sys/class/block/%s/partition", e->d_name);
                if (stat(probe, &st) == 0) continue;            /* a partition, not a namespace */
                snprintf(probe, sizeof probe, "/sys/class/block/%s/queue/io_poll", e->d_name);
                if (stat(probe, &st) != 0) continue;
                snprintf(nvme, sizeof nvme, "/dev/%s", e->d_name);
                break;
            }
            closedir(d);
        }
        if (nvme[0] == '\0') {
            T_SKIP("no NVMe namespace on this host, so the polled backend cannot "
                   "be opened against a device that lacks poll queues");
        } else if (iopath_poll_available(nvme) != 0) {
            /* The other half of the same requirement. A fix that turned the
             * silent downgrade into a blanket refusal would satisfy the
             * refusal assertion below and break the feature, so the accepting
             * case is asserted wherever a device offers polled completion. */
            struct iopath *p = iopath_open(nvme, IOPATH_URING_CMD_POLL, 512);
            T_OK(p != NULL,
                 "iopath_open(%s, IOPATH_URING_CMD_POLL) still succeeds when the "
                 "device does report poll queues", nvme);
            if (p) iopath_close(p);
        } else {
            struct iopath *p = iopath_open(nvme, IOPATH_URING_CMD_POLL, 512);
            T_OK(p == NULL,
                 "iopath_open(%s, IOPATH_URING_CMD_POLL) refuses when the device "
                 "reports no poll queues, instead of returning a handle whose "
                 "completions arrive by interrupt", nvme);
            if (p) iopath_close(p);
        }
    }

    free(raw);
    T_DONE();
}
