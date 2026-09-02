/* What does space preparation cost the append, synchronous versus prepared?
 *
 * Methodology, per tools/README.md: preparation is NOT part of the write in the
 * paper's design, so it is reported on its own line and never folded into the
 * append figure. The synchronous arm exists only to show what the append would
 * pay if preparation were left on the critical path.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "exitos_donor.h"
#include "exitos_donor_async.h"

#define MIB (1024ull*1024ull)
static uint64_t ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull + t.tv_nsec; }
static int cmp(const void*a,const void*b){ uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;
    return x<y?-1:(x>y?1:0); }
static void rep(const char*n,uint64_t*s,int c){
    if(!c){printf("  %-42s (no samples)\n",n);return;}
    qsort(s,c,sizeof *s,cmp); double m=0; for(int i=0;i<c;i++) m+=(double)s[i];
    printf("  %-42s n=%-5d p50=%8.2fus p99=%9.2fus mean=%8.2fus\n",
        n,c,s[c/2]/1000.0,s[(int)(c*0.99)]/1000.0,m/c/1000.0);
}
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <ext4 mountpoint>\n",argv[0]);return 2;}
    char dp[512],tp[512]; int N=64; size_t IO=4096;
    snprintf(dp,sizeof dp,"%s/donors",argv[1]); mkdir(dp,0700);
    struct donor_pool *p=donor_pool_create(dp,4,64*MIB);
    if(!p){printf("pool create failed\n");return 1;}
    void *b; if(posix_memalign(&b,4096,IO)) return 1; memset(b,0x5A,IO);
    uint64_t *s=calloc(N,sizeof *s);

    printf("== preparation cost, reported separately (NOT part of the append) ==\n");
    snprintf(tp,sizeof tp,"%s/sync.log",argv[1]);
    int f1=open(tp,O_RDWR|O_CREAT|O_TRUNC,0644);
    int c1=0;
    for(int i=0;i<8;i++){ uint64_t t=ns();
        if(donor_extend(p,f1,(uint64_t)i*4*MIB,4*MIB)<=0) break; s[c1++]=ns()-t; }
    rep("donate+initialise 4 MiB (preparation)",s,c1);
    close(f1); unlink(tp);

    printf("\n== append latency ==\n");
    /* A: preparation left ON the critical path — every Nth append pays for it. */
    snprintf(tp,sizeof tp,"%s/a.log",argv[1]);
    int fa=open(tp,O_RDWR|O_CREAT|O_TRUNC|O_DIRECT,0644);
    int ca=0; uint64_t prepared=0;
    for(int i=0;i<N;i++){
        uint64_t off=(uint64_t)i*IO, t=ns();
        if(off+IO>prepared){ if(donor_extend(p,fa,prepared,4*MIB)>0) prepared+=4*MIB; }
        if(pwrite(fa,b,IO,(off_t)off)!=(ssize_t)IO) break;
        s[ca++]=ns()-t;
    }
    rep("append, preparation ON the critical path",s,ca);
    close(fa); unlink(tp);

    /* B: preparation moved to the background thread — the append never waits. */
    snprintf(tp,sizeof tp,"%s/b.log",argv[1]);
    int fb=open(tp,O_RDWR|O_CREAT|O_TRUNC|O_DIRECT,0644);
    struct donor_async *as=donor_async_start(p,fb,4*MIB,2*MIB);
    for(int i=0;i<300 && donor_async_runway(as,0)==0;i++) usleep(10000);  /* warm up */
    int cb=0; uint64_t caught_up=0;
    for(int i=0;i<N;i++){
        uint64_t off=(uint64_t)i*IO, t=ns();
        /* If the preparer is behind, take the ordinary kernel route for this
         * write and move on. Waiting for it would put the very latency we are
         * trying to remove straight back onto the append. Count how often the
         * writer outran the preparer instead — that is the number that says
         * whether the watermark is set right. */
        if(donor_async_runway(as,off) < IO) caught_up++;
        if(pwrite(fb,b,IO,(off_t)off)!=(ssize_t)IO) break;
        donor_async_advance(as,off+IO);
        s[cb++]=ns()-t;
    }
    rep("append, preparation on a background thread",s,cb);
    printf("  (preparer completed %llu rounds; the writer outran it %llu of %d times)\n",
           (unsigned long long)donor_async_rounds(as),(unsigned long long)caught_up,cb);
    donor_async_stop(as); close(fb); unlink(tp);
    free(s); free(b); donor_pool_destroy(p);
    return 0;
}
