/* Sweeps write size across the submission paths, so that a change in relative
 * standing can be attributed to the right cause. A difference that follows the
 * size can be a property of NVMe passthrough in general or of the ioctl
 * interface in particular, and the two have different consequences: the first
 * is a limit of the design, the second is a reason to submit through
 * io_uring_cmd instead.
 *
 * Writes only into the window the caller names, and refuses without it. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "exitos_iopath.h"
#include "exitos_devguard.h"

static int cmp(const void *a, const void *b)
{ double x=*(const double*)a, y=*(const double*)b; return x<y?-1:x>y?1:0; }

static double run(struct iopath *p, uint64_t lba0, uint64_t lbacnt,
                  int iters, size_t iosz, uint32_t lbs, int fua)
{
    void *buf; double *s; struct timespec a, b; int i, n = 0;
    uint64_t blocks = iosz / lbs;
    if (posix_memalign(&buf, 4096, iosz) != 0) return -1;
    memset(buf, 0x5A, iosz);
    s = malloc(sizeof(double) * (size_t)iters);
    if (!s) { free(buf); return -1; }
    (void)iopath_set_fua(p, fua);
    for (i = 0; i < iters; i++) {
        uint64_t lba = lba0 + (uint64_t)i * blocks;
        if (lba + blocks > lba0 + lbacnt) break;
        clock_gettime(CLOCK_MONOTONIC, &a);
        if (iopath_write(p, lba, buf, iosz) != 0) break;
        clock_gettime(CLOCK_MONOTONIC, &b);
        s[n++] = (double)(b.tv_sec-a.tv_sec)*1e6 + (double)(b.tv_nsec-a.tv_nsec)/1e3;
    }
    if (n == 0) { free(s); free(buf); return -1; }
    qsort(s, (size_t)n, sizeof(double), cmp);
    { double v = s[n/2]; free(s); free(buf); return v; }
}

int main(int argc, char **argv)
{
    const char *dev = getenv("EXITOS_DEV");
    const char *s0 = getenv("EXITOS_LBA_START"), *sc = getenv("EXITOS_LBA_COUNT");
    const char *part = getenv("EXITOS_WINDOW_IN_PART");
    const char *sn = getenv("EXITOS_EXPECT_SERIAL");
    int iters = argc > 1 ? atoi(argv[1]) : 2000;
    uint32_t lbs;
    uint64_t lba0, cnt;
    char why[320];
    /* The passthru arms set FUA while the block-layer arm does not, so the two
     * were never comparing the same durability request. This turns FUA off so
     * the difference can be attributed. */
    int use_fua = getenv("EXITOS_NO_FUA") ? 0 : 1;
    size_t sizes[] = { 4096, 8192, 12288, 16384, 32768 };
    size_t i;
    /* Optional third and fourth arms, so the block-device question is answered
     * with the same program, the same window and the same iteration count as
     * the NVMe arms. EXITOS_PWRITE_DEV names a node to write through the block
     * layer; EXITOS_PWRITE_OFFSET is the LBA to use on it, which differs from
     * the namespace LBA when the node is a partition, because the kernel makes
     * a partition node's offsets partition-relative. */
    const char *pdev = getenv("EXITOS_PWRITE_DEV");
    const char *poff = getenv("EXITOS_PWRITE_LBA");

    if (!dev || !s0 || !sc || !part || !sn) {
        fprintf(stderr, "refusing: need EXITOS_DEV, EXITOS_LBA_START, EXITOS_LBA_COUNT,\n"
                        "EXITOS_WINDOW_IN_PART and EXITOS_EXPECT_SERIAL\n");
        return 2;
    }
    lba0 = strtoull(s0, NULL, 0); cnt = strtoull(sc, NULL, 0);
    lbs = exitos_devguard_lbs(dev);
    if (lbs == 0) { fprintf(stderr, "cannot read logical block size\n"); return 3; }
    if (exitos_devguard_check(dev, sn, NULL, 0) == DEVGUARD_SERIAL_MISMATCH) {
        fprintf(stderr, "refusing: %s is not the disk named by EXITOS_EXPECT_SERIAL\n", dev);
        return 3;
    }
    if (exitos_devguard_window_in_partition(dev, part, lba0, cnt, lbs, why, sizeof why) != 0) {
        fprintf(stderr, "refusing: %s\n", why);
        return 3;
    }
    printf("window checked: %s\n", why);
    printf("%8s %13s %13s %13s %13s\n", "IO", "ioctl+FUA", "uring+FUA",
           "uringPOLL+FUA", pdev ? "blocklayer" : "");
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        struct iopath *a = iopath_open(dev, IOPATH_NVME_IOCTL, lbs);
        struct iopath *b = iopath_open(dev, IOPATH_URING_CMD, lbs);
        struct iopath *d = iopath_open(dev, IOPATH_URING_CMD_POLL, lbs);
        double ta = a ? run(a, lba0, cnt, iters, sizes[i], lbs, use_fua) : -1;
        double tb = b ? run(b, lba0, cnt, iters, sizes[i], lbs, use_fua) : -1;
        double td = d ? run(d, lba0, cnt, iters, sizes[i], lbs, use_fua) : -1;
        double tc = -1;
        if (pdev) {
            struct iopath *c = iopath_open(pdev, IOPATH_PWRITE, lbs);
            uint64_t plba = poff ? strtoull(poff, NULL, 0) : lba0;
            if (c) { tc = run(c, plba, cnt, iters, sizes[i], lbs, 0); iopath_close(c); }
        }
        if (pdev) printf("%7zuB %11.2fus %11.2fus %11.2fus %11.2fus\n", sizes[i], ta, tb, td, tc);
        else      printf("%7zuB %11.2fus %11.2fus %11.2fus\n", sizes[i], ta, tb, td);
        if (a) iopath_close(a);
        if (b) iopath_close(b);
        if (d) iopath_close(d);
    }
    return 0;
}
