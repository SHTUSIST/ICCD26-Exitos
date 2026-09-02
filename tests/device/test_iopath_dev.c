/* Tier 2 device tests for the iopath module: IOPATH_NVME_IOCTL and
 * IOPATH_URING_CMD, which cannot be exercised without a real NVMe controller.
 *
 * THIS TEST REFUSES TO RUN BY DEFAULT. It does nothing at all unless the
 * operator sets EXITOS_DEV explicitly, and it applies two further gates:
 *
 *   EXITOS_DEV        must be set, and must name a /dev/nvme* node. Anything
 *                     else (a loop device, an SATA disk, a regular file) is
 *                     refused outright -- a passthru write to the wrong node
 *                     destroys data.
 *   EXITOS_DEV_LBA    the starting LBA to use. No default: the operator must
 *                     name a range they are willing to lose.
 *   EXITOS_DEV_WRITE_OK=1  required before ANY write is submitted. Without it
 *                     the test still checks open, flush, and the refusal paths
 *                     (which submit no I/O at all) and skips the round trip.
 *   EXITOS_DEV_LBS    logical block size, default 512.
 *
 * On the machine this repository was developed on there is no NVMe device and
 * the kernel is 5.15, which has no IORING_OP_URING_CMD (that needs 5.19), so
 * every case below skips. That is the intended outcome here.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <linux/nvme_ioctl.h>

#include "exitos_iopath.h"
#include "exitos_devguard.h"
#include "tap.h"

/* Return 1 if the running kernel is at least maj.min. */
static int kernel_at_least(int maj, int min)
{
    struct utsname u;
    int a = 0, b = 0;
    if (uname(&u) != 0) return 0;
    if (sscanf(u.release, "%d.%d", &a, &b) != 2) return 0;
    return (a > maj) || (a == maj && b >= min);
}

static void fill_pattern(unsigned char *p, size_t len, uint64_t lba)
{
    for (size_t i = 0; i < len; i++)
        p[i] = (unsigned char)((i * 197u + 41u) ^ 0x3C);
    for (int i = 0; i < 8 && (size_t)i < len; i++)
        p[i] = (unsigned char)(lba >> (8 * i));
}

int main(void)
{
    const char *dev = getenv("EXITOS_DEV");

    if (!dev || !*dev) {
        T_SKIP("EXITOS_DEV is not set - tier 2 needs a real NVMe device and is "
               "opt-in only; refusing to run");
        T_DONE();
    }
    if (strncmp(dev, "/dev/nvme", 9) != 0) {
        T_SKIP("EXITOS_DEV='%s' is not a /dev/nvme* node - refusing to touch it", dev);
        T_DONE();
    }
    if (!t_need_root()) {
        T_SKIP("NVMe passthru ioctls need root - skipping tier 2");
        T_DONE();
    }

    uint32_t lbs = 512;
    const char *e = getenv("EXITOS_DEV_LBS");
    if (e && *e) lbs = (uint32_t)strtoul(e, NULL, 10);
    if (lbs == 0 || (lbs & (lbs - 1)) != 0) {
        T_SKIP("EXITOS_DEV_LBS='%s' is not a power of two - refusing", e ? e : "");
        T_DONE();
    }

    int write_ok = 0;
    e = getenv("EXITOS_DEV_WRITE_OK");
    if (e && strcmp(e, "1") == 0) write_ok = 1;

    uint64_t lba = 0;
    int have_lba = 0;
    e = getenv("EXITOS_DEV_LBA");
    if (e && *e) { lba = strtoull(e, NULL, 0); have_lba = 1; }

    /* Two gates this test used to leave entirely to the operator's care, both of
     * which have already gone wrong once on this project.
     *
     * The first is identity: an NVMe kernel name is assigned in probe order and
     * moves across reboots, so EXITOS_DEV alone names whichever disk currently
     * answers to it. The serial is what identifies a disk.
     *
     * The second is aim: the write below is a raw LBA write of 8 blocks and
     * nothing here checked where those blocks are. A partition-relative LBA
     * sent to a namespace overwrote a GPT. The operator names the
     * partition the LBA is meant to be inside and the partition table decides
     * whether it is. */
    if (write_ok && have_lba) {
        const char *want_sn = getenv("EXITOS_EXPECT_SERIAL");
        const char *part = getenv("EXITOS_WINDOW_IN_PART");
        char why[320], sn[128] = "";
        devguard_verdict dv;

        if (!want_sn || !*want_sn) {
            T_OK(0, "EXITOS_EXPECT_SERIAL must be set before any raw write: the "
                    "kernel name of a disk is not stable across reboots");
            T_DONE();
        }
        dv = exitos_devguard_check(dev, want_sn, sn, sizeof sn);
        if (dv == DEVGUARD_SERIAL_MISMATCH || dv == DEVGUARD_NO_SERIAL) {
            T_OK(0, "%s is not the disk named by EXITOS_EXPECT_SERIAL (%s): got '%s'",
                 dev, want_sn, exitos_devguard_str(dv));
            T_DONE();
        }
        T_OK(1, "identity proven before writing: %s carries serial %s", dev, sn);

        if (!part || !*part) {
            T_OK(0, "EXITOS_WINDOW_IN_PART must name the partition the target LBA "
                    "lies inside, so a mistyped LBA is refused rather than written");
            T_DONE();
        }
        if (exitos_devguard_window_in_partition(dev, part, lba, 8, lbs,
                                                why, sizeof why) != 0) {
            T_OK(0, "target LBA refused: %s", why);
            T_DONE();
        }
        T_OK(1, "target checked against the partition table: %s", why);
    }

    void *raw = NULL;
    if (posix_memalign(&raw, 4096, 16 * (size_t)lbs) != 0 || !raw) {
        T_OK(0, "posix_memalign failed");
        T_DONE();
    }
    unsigned char *buf = raw, *rbuf = buf + 8 * (size_t)lbs;

    /* ================= IOPATH_NVME_IOCTL ============================== */
    struct iopath *p = iopath_open(dev, IOPATH_NVME_IOCTL, lbs);
    T_OK(p != NULL, "iopath_open(%s, IOPATH_NVME_IOCTL, lbs=%u) succeeds", dev, lbs);

    if (p) {
        /* Refusals first: these must be rejected before any command reaches the
         * controller, so they are safe even without EXITOS_DEV_WRITE_OK. */
        T_EQ(iopath_write(p, 0, buf, lbs + 1), -EINVAL,
             "nvme-ioctl: write of lbs+1 bytes refused with -EINVAL");
        T_EQ(iopath_write(p, 0, buf, 1), -EINVAL,
             "nvme-ioctl: write of 1 byte refused with -EINVAL");
        T_EQ(iopath_read(p, 0, rbuf, lbs - 1), -EINVAL,
             "nvme-ioctl: read of lbs-1 bytes refused with -EINVAL");
        T_OK(iopath_write(p, 0, NULL, lbs) < 0,
             "nvme-ioctl: write with buf == NULL refused");
        if (lbs >= 2)
            T_EQ(iopath_write(p, 0, buf + 1, lbs), -EINVAL,
                 "nvme-ioctl: write from a misaligned buffer refused with -EINVAL");

        T_EQ(iopath_flush(p), 0, "nvme-ioctl: iopath_flush (NVMe FLUSH) returns 0");

        if (!write_ok) {
            T_SKIP("EXITOS_DEV_WRITE_OK=1 not set - skipping the nvme-ioctl "
                   "write/read round trip (it would overwrite device data)");
        } else if (!have_lba) {
            T_SKIP("EXITOS_DEV_LBA is not set - refusing to pick an LBA to "
                   "overwrite; skipping the nvme-ioctl round trip");
        } else {
            size_t len = 8 * (size_t)lbs;    /* 8 blocks */
            fill_pattern(buf, len, lba);
            T_EQ(iopath_write(p, lba, buf, len), 0,
                 "nvme-ioctl: write %zu bytes at LBA %llu returns 0",
                 len, (unsigned long long)lba);
            T_EQ(iopath_flush(p), 0, "nvme-ioctl: flush after write returns 0");
            memset(rbuf, 0, len);
            T_EQ(iopath_read(p, lba, rbuf, len), 0,
                 "nvme-ioctl: read %zu bytes back from LBA %llu returns 0",
                 len, (unsigned long long)lba);
            T_EQ(memcmp(rbuf, buf, len), 0,
                 "nvme-ioctl: read-back matches the written pattern");
        }
        iopath_close(p);
        T_OK(1, "nvme-ioctl: iopath_close returned");
    }

    /* ================= IOPATH_URING_CMD =============================== */
    if (!kernel_at_least(5, 19)) {
        struct utsname u;
        uname(&u);
        T_SKIP("kernel %s has no IORING_OP_URING_CMD (needs 5.19+) - skipping "
               "the uring_cmd backend", u.release);
    } else {
        p = iopath_open(dev, IOPATH_URING_CMD, lbs);
        T_OK(p != NULL, "iopath_open(%s, IOPATH_URING_CMD, lbs=%u) succeeds", dev, lbs);
        if (p) {
            T_EQ(iopath_write(p, 0, buf, lbs + 1), -EINVAL,
                 "uring-cmd: write of lbs+1 bytes refused with -EINVAL");
            if (lbs >= 2)
                T_EQ(iopath_write(p, 0, buf + 1, lbs), -EINVAL,
                     "uring-cmd: write from a misaligned buffer refused with -EINVAL");
            T_EQ(iopath_flush(p), 0, "uring-cmd: iopath_flush returns 0");

            if (!write_ok || !have_lba) {
                T_SKIP("EXITOS_DEV_WRITE_OK=1 and EXITOS_DEV_LBA required - "
                       "skipping the uring_cmd round trip");
            } else {
                size_t len = 8 * (size_t)lbs;
                fill_pattern(buf, len, lba + 64);
                T_EQ(iopath_write(p, lba, buf, len), 0,
                     "uring-cmd: write %zu bytes at LBA %llu returns 0",
                     len, (unsigned long long)lba);
                T_EQ(iopath_flush(p), 0, "uring-cmd: flush after write returns 0");
                memset(rbuf, 0, len);
                T_EQ(iopath_read(p, lba, rbuf, len), 0,
                     "uring-cmd: read %zu bytes back returns 0", len);
                T_EQ(memcmp(rbuf, buf, len), 0,
                     "uring-cmd: read-back matches the written pattern");

                /* Both backends address the same media: what uring_cmd wrote
                 * must be visible through the ioctl backend. */
                struct iopath *q = iopath_open(dev, IOPATH_NVME_IOCTL, lbs);
                if (q) {
                    memset(rbuf, 0, len);
                    T_EQ(iopath_read(q, lba, rbuf, len), 0,
                         "cross-backend: nvme-ioctl read of the uring_cmd write returns 0");
                    T_EQ(memcmp(rbuf, buf, len), 0,
                         "cross-backend: both backends address the same LBA");
                    iopath_close(q);
                }
            }
            iopath_close(p);
        }
    }

    free(raw);
    T_DONE();
}
