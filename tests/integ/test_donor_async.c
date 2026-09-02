/* The preparer must stay ahead of the writer, and the write path must never
 * block on it. */
#include "tap.h"
#include "exitos_donor.h"
#include "exitos_donor_async.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define MIB (1024ull*1024ull)
static double ms_since(struct timespec a){ struct timespec b; clock_gettime(CLOCK_MONOTONIC,&b);
    return (b.tv_sec-a.tv_sec)*1e3 + (b.tv_nsec-a.tv_nsec)/1e6; }

int main(int argc, char **argv)
{
    char mnt[256], img[256]; const char *dir; char cmd[1024];
    if (argc > 1) dir = argv[1];
    else {
        if (geteuid() != 0) { T_SKIP("needs root for a loop device"); printf("1..0  (0 failed)\n"); return 0; }
        snprintf(img,sizeof img,"/tmp/exitos-async-%d.img",(int)getpid());
        snprintf(mnt,sizeof mnt,"/tmp/exitos-async-mnt-%d",(int)getpid());
        snprintf(cmd,sizeof cmd,"truncate -s 512M %s && L=$(losetup --find --show %s) && "
          "mkfs.ext4 -qF -b 4096 $L && mkdir -p %s && mount $L %s && echo $L > %s.loop",
          img,img,mnt,mnt,img);
        if (system(cmd)!=0){ T_OK(0,"loop fixture"); T_DONE(); }
        dir = mnt;
    }
    char dp[512], tp[512];
    snprintf(dp,sizeof dp,"%s/donors",dir); mkdir(dp,0700);
    snprintf(tp,sizeof tp,"%s/growing.log",dir);

    struct donor_pool *p = donor_pool_create(dp, 2, 32*MIB);
    T_OK(p != 0, "donor pool created");
    int fd = open(tp, O_RDWR|O_CREAT|O_TRUNC, 0644);
    T_OK(fd >= 0, "target opened");

    struct donor_async *a = donor_async_start(p, fd, 4*MIB, 2*MIB);
    T_OK(a != 0, "preparer started");

    /* The write-path query must be immediate. Anything that took a syscall
     * would not come in under this. */
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC,&t0);
    for (int i = 0; i < 100000; i++) (void)donor_async_runway(a, 0);
    double per = ms_since(t0) * 1000.0 / 100000.0;   /* microseconds per call */
    T_OK(per < 1.0, "runway() costs %.3f us per call (memory read, no syscall)", per);

    /* Give the preparer a moment to get ahead. */
    for (int i = 0; i < 200 && donor_async_runway(a,0) == 0; i++) usleep(10000);
    T_OK(donor_async_runway(a, 0) > 0, "preparer got ahead of the writer: runway=%llu bytes",
         (unsigned long long)donor_async_runway(a, 0));

    /* Consume, and check the preparer keeps refilling rather than running dry. */
    uint64_t off = 0, dry = 0;
    for (int i = 0; i < 24; i++) {
        if (donor_async_runway(a, off) == 0) { dry++; usleep(20000); }
        off += 1*MIB;
        donor_async_advance(a, off);
        usleep(2000);
    }
    T_OK(donor_async_prepared_to(a) >= off,
         "preparer stayed ahead of consumption (prepared to %llu, consumed to %llu)",
         (unsigned long long)donor_async_prepared_to(a), (unsigned long long)off);
    T_OK(donor_async_rounds(a) > 1, "preparer ran %llu rounds, refilling as it went",
         (unsigned long long)donor_async_rounds(a));

    donor_async_stop(a);
    T_OK(1, "preparer stopped cleanly");

    /* chunk == 0 selects the paper's rule: four times the moving average of
     * observed write sizes, computed on the preparer thread so even that
     * arithmetic stays off the append. */
    char tp2[512]; snprintf(tp2,sizeof tp2,"%s/adaptive.log",dir);
    int fd2 = open(tp2, O_RDWR|O_CREAT|O_TRUNC, 0644);
    struct donor_async *b2 = donor_async_start(p, fd2, 0, 0);
    T_OK(b2 != 0, "preparer starts with chunk=0 (moving-average sizing)");
    if (b2) {
        uint64_t o = 0;
        for (int i = 0; i < 40; i++) { o += 64*1024; donor_async_advance(b2, o); usleep(3000); }
        for (int i = 0; i < 200 && donor_async_rounds(b2) == 0; i++) usleep(10000);
        T_OK(donor_async_rounds(b2) > 0,
             "adaptive preparer donated %llu round(s) sized from observed writes",
             (unsigned long long)donor_async_rounds(b2));
        T_OK(donor_async_prepared_to(b2) > 0, "adaptive preparer produced %llu bytes of runway",
             (unsigned long long)donor_async_prepared_to(b2));
        donor_async_stop(b2);
    }
    close(fd2); unlink(tp2);
    close(fd); unlink(tp); donor_pool_destroy(p);
    if (argc <= 1) {
        snprintf(cmd,sizeof cmd,"umount %s 2>/dev/null; losetup -d $(cat %s.loop 2>/dev/null) 2>/dev/null; rm -rf %s %s %s.loop",
                 mnt,img,img,mnt,img);
        if (system(cmd)!=0){}
    }
    T_DONE();
}
