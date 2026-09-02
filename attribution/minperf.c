/* Minimal performance probe answering three questions with measurements:
 *   Q1 how much slower is a filesystem write+fdatasync than writing raw LBAs?
 *   Q2 what does one syscall (kernel entry/exit) actually cost here?
 *   Q3 what does the userspace intercept decision itself cost per write?
 *
 * SAFETY: refuses to touch a device unless EXITOS_DEV is set AND the caller
 * gives an explicit LBA window via EXITOS_LBA_START / EXITOS_LBA_COUNT.
 * Every device write is bounds-checked against that window, so it cannot
 * stray outside the region the operator verified as free.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/nvme_ioctl.h>
#include "exitos_intercept.h"
#include "exitos_benchguard.h"
#include "exitos_maco.h"
#include "exitos_devguard.h"
#include "exitos_iopath.h"

#define NS 1000000000ULL
static uint64_t now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*NS + t.tv_nsec; }
static int cmp_u64(const void *a,const void *b){ uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;
    return x<y?-1:(x>y?1:0); }

static void report(const char *name, uint64_t *s, int n, size_t iosz){
    if(!n) { printf("%-34s  (no samples)\n", name); return; }
    qsort(s,n,sizeof(uint64_t),cmp_u64);
    double mean=0; for(int i=0;i<n;i++) mean+=(double)s[i]; mean/=n;
    double mb = (double)iosz*n / (1024.0*1024.0);
    double secs=0; for(int i=0;i<n;i++) secs+=(double)s[i]/1e9;
    printf("%-34s  n=%-6d p50=%8.2fus p99=%8.2fus mean=%8.2fus  %7.1f MB/s\n",
        name,n,s[n/2]/1000.0,s[(int)(n*0.99)]/1000.0,mean/1000.0, secs>0?mb/secs:0.0);
}

/* ---- Q2: bare syscall cost (kernel entry/exit, no I/O) ---- */
static int bench_syscall(int iters){
    uint64_t *s=iters>0?calloc((size_t)iters,sizeof(uint64_t)):NULL;
    if(iters>0 && !s) return -ENOMEM;
    for(int i=0;i<iters;i++){ uint64_t t0=now_ns(); (void)syscall(SYS_getpid); s[i]=now_ns()-t0; }
    report("syscall floor (getpid)", s, iters, 0); free(s);
    return 0;
}

/* ---- Q3: cost of the userspace intercept decision itself ---- */
static int bench_decision(int iters, size_t iosz){
    struct exitos_ctx *c = exitos_ctx_create();
    if(!c){ printf("intercept ctx unavailable\n"); return -ENODEV; }
    char *buf=NULL;
    if(posix_memalign((void **)&buf,4096,iosz)!=0 || !buf){
        exitos_ctx_destroy(c); return -ENOMEM;
    }
    memset(buf,0xA5,iosz);
    uint64_t *s=iters>0?calloc((size_t)iters,sizeof(uint64_t)):NULL;
    ssize_t r; int n=0, rc=0;
    if(iters>0 && !s){ free(buf); exitos_ctx_destroy(c); return -ENOMEM; }
    for(int i=0;i<iters;i++){
        struct exitos_bench_byte_span span;
        rc=exitos_bench_byte_span((uint64_t)i,1,iosz,&span);
        if(rc!=0) break;
        uint64_t t0=now_ns();
        (void)exitos_on_write(c,-1,buf,iosz,span.offset,&r);  /* unregistered -> PASS */
        s[n++]=now_ns()-t0;
    }
    if(rc==0 && n!=iters) rc=-EIO;
    if(rc==0) report("intercept decision (PASS path)",s,n,0);
    free(s); free(buf); exitos_ctx_destroy(c);
    return rc;
}

/* ---- Q1a: filesystem write + fdatasync ---- */
static int bench_fs_mode(const char *path,int iters,size_t iosz,int direct,int dosync,const char *label);
static int bench_fs(const char *path,int iters,size_t iosz,int direct){
    return bench_fs_mode(path,iters,iosz,direct,1,
        direct?"ext4 O_DIRECT write+fdatasync":"ext4 buffered write+fdatasync");
}
static int bench_fs_mode(const char *path,int iters,size_t iosz,int direct,int dosync,const char *label){
    int fl=O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW|(direct?O_DIRECT:0);
    int fd=open(path,fl,0600);
    int created=fd>=0;
    int rc=0, n=0;
    void *z=NULL;
    char *buf=NULL;
    uint64_t *s=NULL;
    if(fd<0){ rc=-errno; printf("fs open %s: %s\n",path,strerror(errno)); return rc; }
    struct exitos_bench_byte_span total;
    if (iters > 0 &&
        exitos_bench_byte_span(0, (uint64_t)iters, iosz, &total) != 0) {
        fprintf(stderr,"filesystem benchmark byte range overflows\n");
        rc=-EOVERFLOW; goto out;
    }
    if(iters > 0 && fallocate(fd,0,total.offset,total.length)!=0) {
        rc=-errno; goto out;
    }
    if(iters==0){ report(label,NULL,0,iosz); goto out; }
    /* fallocate leaves UNWRITTEN extents; the first write to each converts it,
     * which is a metadata change and forces a journal commit. Measuring that
     * would report ext4's worst case and flatter our own path. Initialise the
     * extents first (the paper does the same) so we measure steady-state
     * overwrite of already-allocated, already-initialised blocks. */
    if(iters>0) {
        if(posix_memalign(&z,4096,iosz)!=0 || !z){ rc=-ENOMEM; goto out; }
        memset(z,0,iosz);
        for(int i=0;i<iters;i++) {
            struct exitos_bench_byte_span span;
            rc=exitos_bench_byte_span((uint64_t)i,1,iosz,&span);
            if(rc!=0) goto out;
            if(pwrite(fd,z,iosz,span.offset)!=(ssize_t)iosz){ rc=-EIO; goto out; }
        }
        if(fdatasync(fd)!=0){ rc=-errno; goto out; }
        free(z); z=NULL;
    }
    if(posix_memalign((void **)&buf,4096,iosz)!=0 || !buf){
        rc=-ENOMEM; goto out;
    }
    memset(buf,0x5A,iosz);
    s=iters>0?calloc((size_t)iters,sizeof(uint64_t)):NULL;
    if(iters>0 && !s){ rc=-ENOMEM; goto out; }
    for(int i=0;i<iters;i++){
        struct exitos_bench_byte_span span;
        rc=exitos_bench_byte_span((uint64_t)i,1,iosz,&span);
        if(rc!=0) goto out;
        uint64_t t0=now_ns();
        if(pwrite(fd,buf,iosz,span.offset)!=(ssize_t)iosz){ rc=-EIO; goto out; }
        if(dosync && fdatasync(fd)!=0){ rc=-errno; goto out; }
        s[n++]=now_ns()-t0;
    }
    if(n!=iters){ rc=-EIO; goto out; }
    report(label,s,n,iosz);
out:
    free(s); free(buf); free(z); close(fd);
    if(created) unlink(path);
    return rc;
}

/* Page alignment makes an 8 KiB buffer cover a deterministic number of
 * virtual pages on a 4 KiB-page host.  It does not prove physical contiguity
 * or a DMA segment count; the real benchmark must establish those by tracing.
 * The maximum is usable only when it is also a legal POSIX alignment and a
 * multiple of both constraints. */
static int alloc_raw_buffer(void **buf, size_t iosz, uint32_t lbs)
{
    long page_size_value;
    size_t page_size, alignment;
    int err;

    if(!buf || lbs==0) return -EINVAL;
    *buf=NULL;
    page_size_value=sysconf(_SC_PAGESIZE);
    if(page_size_value<=0) return -EINVAL;
    page_size=(size_t)page_size_value;
    if((long)page_size!=page_size_value) return -EOVERFLOW;
    alignment=page_size>(size_t)lbs?page_size:(size_t)lbs;
    if(alignment<sizeof(void *) || alignment%sizeof(void *)!=0 ||
       (alignment & (alignment-1u))!=0 || alignment%page_size!=0 ||
       alignment%(size_t)lbs!=0)
        return -EINVAL;
    err=posix_memalign(buf,alignment,iosz);
    if(err!=0) return -err;
    return *buf?0:-ENOMEM;
}

/* Every raw arm submits through a handle opened from the one retained and
 * already-verified device fd.  Position arithmetic is centralized in
 * exitos_bench_io_position(), so no arm can wrap back into another LBA. */
static int bench_iopath_mode(struct iopath *io, uint64_t lba0, uint64_t lbacnt,
                              int iters, size_t iosz, uint32_t lbs,
                              int do_flush, int fua, unsigned char fill,
                              const char *label)
{
    void *buf = NULL;
    uint64_t *samples = NULL;
    int n = 0, rc = 0;

    if (!io) return -ENODEV;
    rc=alloc_raw_buffer(&buf,iosz,lbs);
    if(rc!=0) return rc;
    memset(buf, fill, iosz);
    samples = iters>0?calloc((size_t)iters,sizeof *samples):NULL;
    if (iters>0 && !samples){ rc=-ENOMEM; goto out; }
    rc=iopath_set_fua(io,fua);
    if(rc!=0) goto out;
    for (int i = 0; i < iters; i++) {
        struct exitos_bench_io_position pos;
        rc=exitos_bench_io_position(lba0,lbacnt,(uint64_t)i,1,0,
                                    iosz,lbs,&pos);
        if(rc!=0){ fprintf(stderr,"checked I/O position refused: %s\n",strerror(-rc)); goto out; }
        uint64_t t0 = now_ns();
        rc = iopath_write(io, pos.lba, buf, iosz);
        if (rc == 0 && do_flush) rc = iopath_flush(io);
        if(rc!=0){ fprintf(stderr,"%s failed: %s\n",label,strerror(-rc)); goto out; }
        samples[n++] = now_ns() - t0;
    }
    if(n!=iters){ rc=-EIO; goto out; }
    report(label,samples,n,iosz);
out:
    free(samples);
    free(buf);
    return rc;
}

/* ---- The batched case: databases usually do N writes then ONE fdatasync.
 * Per-write FUA pays a durability cost on every write; a trailing FLUSH pays it
 * once for the whole group. Which wins depends on N, so measure N. ---- */
static int bench_batch_plan_fits(uint64_t lba0, uint64_t lbacnt,
                                 int groups, int N, size_t iosz, uint32_t lbs)
{
    struct exitos_bench_io_position last;

    if(groups<=0 || N<=0) return -EINVAL;
    return exitos_bench_io_position(lba0,lbacnt,(uint64_t)groups-1,
                                    (uint64_t)N,(uint64_t)N-1,
                                    iosz,lbs,&last);
}

static int bench_batch(struct iopath *io, uint64_t lba0, uint64_t lbacnt,
                        int groups, int N, size_t iosz, uint32_t lbs)
{
    void *buf = NULL;
    uint64_t *a = NULL, *b = NULL;
    int na = 0, nb = 0, rc = 0;

    if(!io) return -EINVAL;
    /* Both comparison phases address the same complete group/member plan.
     * Prove its final member before allocation, FUA changes, or the first
     * submission; a later group can no longer invalidate an already-written
     * prefix of the benchmark. */
    rc=bench_batch_plan_fits(lba0,lbacnt,groups,N,iosz,lbs);
    if(rc!=0) return rc;
    rc=alloc_raw_buffer(&buf,iosz,lbs);
    if(rc!=0) return rc;
    memset(buf, 0x5c, iosz);
    a = groups>0?calloc((size_t)groups,sizeof *a):NULL;
    b = groups>0?calloc((size_t)groups,sizeof *b):NULL;
    if(groups>0 && (!a || !b)){ rc=-ENOMEM; goto out; }
    rc=iopath_set_fua(io,0);
    if(rc!=0) goto out;

    for (int g = 0; g < groups; g++) {
        struct exitos_bench_io_position pos;
        rc=exitos_bench_io_position(lba0,lbacnt,(uint64_t)g,
                                    (uint64_t)N,(uint64_t)N-1,
                                    iosz,lbs,&pos);
        if(rc!=0) goto out;
        uint64_t t0 = now_ns();
        for (int i = 0; i < N; i++) {
            rc = exitos_bench_io_position(lba0, lbacnt, (uint64_t)g,
                                          (uint64_t)N, (uint64_t)i,
                                          iosz, lbs, &pos);
            if(rc!=0) goto out;
            rc=iopath_write(io,pos.lba,buf,iosz);
            if(rc!=0) goto out;
        }
        rc=iopath_flush(io);
        if(rc!=0) goto out;
        a[na++] = now_ns() - t0;
    }
    if(na!=groups){ rc=-EIO; goto out; }

    rc=iopath_set_fua(io,1);
    if(rc!=0) goto out;
    for (int g = 0; g < groups; g++) {
        struct exitos_bench_io_position pos;
        rc=exitos_bench_io_position(lba0,lbacnt,(uint64_t)g,
                                    (uint64_t)N,(uint64_t)N-1,
                                    iosz,lbs,&pos);
        if(rc!=0) goto out;
        uint64_t t0 = now_ns();
        for (int i = 0; i < N; i++) {
            rc = exitos_bench_io_position(lba0, lbacnt, (uint64_t)g,
                                          (uint64_t)N, (uint64_t)i,
                                          iosz, lbs, &pos);
            if(rc!=0) goto out;
            rc=iopath_write(io,pos.lba,buf,iosz);
            if(rc!=0) goto out;
        }
        b[nb++] = now_ns() - t0;
    }
    if(nb!=groups){ rc=-EIO; goto out; }

    {
        char l1[96], l2[96];
        struct exitos_bench_byte_span throughput;
        size_t group_bytes=0;
        rc=exitos_bench_byte_span(0,(uint64_t)N,iosz,&throughput);
        if(rc!=0 || (uint64_t)throughput.length>SIZE_MAX){
            rc=rc!=0?rc:-EOVERFLOW;
            goto out;
        }
        group_bytes=(size_t)throughput.length;
        snprintf(l1,sizeof l1,"batch N=%-3d : %d writes + 1 FLUSH",N,N);
        snprintf(l2,sizeof l2,"batch N=%-3d : %d FUA writes",N,N);
        report(l1,a,na,group_bytes);
        report(l2,b,nb,group_bytes);
    }
out:
    free(a); free(b); free(buf);
    return rc;
}


/* ---- READ path: no durability step at all, so any kernel cost is exposed
 * rather than buried under a cache flush. This is the regime most fast-storage
 * papers actually measure. ---- */
static int bench_fs_read(const char *path,int iters,size_t iosz){
    int fd=open(path,O_RDONLY|O_DIRECT);
    int rc=0, n=0;
    uint64_t *s=NULL;
    if(fd<0){ rc=-errno; printf("fs read open: %s\n",strerror(errno)); return rc; }
    void *buf = NULL;
    if(posix_memalign(&buf,4096,iosz)!=0 || !buf){ rc=-ENOMEM; goto out; }
    s=iters>0?calloc((size_t)iters,sizeof(uint64_t)):NULL;
    if(iters>0 && !s){ rc=-ENOMEM; goto out; }
    for(int i=0;i<iters;i++){
        struct exitos_bench_byte_span span;
        rc=exitos_bench_byte_span((uint64_t)i,1,iosz,&span);
        if(rc!=0) goto out;
        uint64_t t0=now_ns();
        if(pread(fd,buf,iosz,span.offset)!=(ssize_t)iosz){ rc=-EIO; goto out; }
        s[n++]=now_ns()-t0; }
    if(n!=iters){ rc=-EIO; goto out; }
    report("ext4 O_DIRECT read",s,n,iosz);
out:
    free(s); free(buf); close(fd); return rc;
}
static int bench_iopath_read(struct iopath *io,uint64_t lba0,uint64_t lbacnt,
                              int iters,size_t iosz,uint32_t lbs,
                              const char *label){
    void *buf = NULL;
    uint64_t *s=NULL;
    int n=0, rc=0;
    if(!io) return -ENODEV;
    rc=alloc_raw_buffer(&buf,iosz,lbs);
    if(rc!=0) return rc;
    s=iters>0?calloc((size_t)iters,sizeof(uint64_t)):NULL;
    if(iters>0 && !s){ rc=-ENOMEM; goto out; }
    for(int i=0;i<iters;i++){
        struct exitos_bench_io_position pos;
        rc=exitos_bench_io_position(lba0,lbacnt,(uint64_t)i,1,0,
                                    iosz,lbs,&pos);
        if(rc!=0) goto out;
        uint64_t t0=now_ns();
        rc=iopath_read(io,pos.lba,buf,iosz);
        if(rc!=0) goto out;
        s[n++]=now_ns()-t0; }
    if(n!=iters){ rc=-EIO; goto out; }
    report(label,s,n,iosz);
out:
    free(s); free(buf); return rc;
}

/* Run one arm only. Every arm used to run in one process in one fixed order:
 * the ext4 rows first on a comparatively cold device, then the raw rows, then a
 * FLUSH loop that commits thousands of times, and only then the passthru rows
 * whose numbers the comparison rests on. A small difference cannot be
 * separated from that ordering while the order never changes. With this knob
 * each arm runs in its own process and the caller shuffles them. */
static int arm_wanted(const char *name, int writes)
{
    return exitos_bench_arm_enabled(getenv("EXITOS_ONLY"),
                                    getenv("EXITOS_READ") != NULL,
                                    name, writes);
}

static int open_named_device(const char *name, int flags)
{
    char path[512];
    int n;
    if (!name || !*name) return -1;
    if (strchr(name,'/')) return open(name,flags | O_CLOEXEC);
    n=snprintf(path,sizeof path,"/dev/%s",name);
    if(n<0 || (size_t)n>=sizeof path) return -1;
    return open(path,flags | O_CLOEXEC);
}

static int native_only_name(const char *only)
{
    return only && (!strcmp(only,"flush") || !strcmp(only,"nf") ||
                    !strcmp(only,"fua") || !strcmp(only,"batch") ||
                    !strcmp(only,"nvmeread"));
}

static int arm_name_known(const char *name)
{
    static const char *const names[] = {
        "syscall", "decision", "fsread", "fsbuf", "fsdir", "fsnf",
        "raw", "rawnf", "rawread", "flush", "nf", "fua", "batch",
        "nvmeread"
    };
    size_t i;

    if (!name || !*name) return 0;
    for (i=0;i<sizeof names/sizeof names[0];i++)
        if(strcmp(name,names[i])==0) return 1;
    return 0;
}

static int arm_name_writes(const char *name)
{
    return name && (!strcmp(name,"fsbuf") || !strcmp(name,"fsdir") ||
                    !strcmp(name,"fsnf") || !strcmp(name,"raw") ||
                    !strcmp(name,"rawnf") || !strcmp(name,"flush") ||
                    !strcmp(name,"nf") || !strcmp(name,"fua") ||
                    !strcmp(name,"batch"));
}

int main(int argc,char**argv){
    const char *ie=getenv("EXITOS_ITERS"), *only=getenv("EXITOS_ONLY");
    const char *dev=getenv("EXITOS_DEV"), *scratch=getenv("EXITOS_SCRATCH");
    const char *s0, *sc, *pin, *expect_lbs, *expect_nsid;
    uint64_t parsed, lba0, cnt, expected, window_end;
    uint32_t lbs, actual_nsid=0;
    int iters, read_only=getenv("EXITOS_READ")!=NULL;
    size_t iosz;
    int devfd=-1, partfd=-1, whole=0, is_nvme=0, result=0, arm_rc;
    struct iopath *raw_io=NULL, *nvme_io=NULL;
    struct exitos_bench_io_position first;
    char sn[EXITOS_DEVGUARD_IDENTITY_MAX]="";
    devguard_verdict v;

    if(exitos_bench_parse_u64(argc>1?argv[1]:(ie?ie:"2000"),&parsed)!=0 ||
       parsed>10000000){ fprintf(stderr,"bad iters\n"); return 2; }
    iters=(int)parsed;
    if(iters==0 && !getenv("EXITOS_CERTIFY")){
        fprintf(stderr,"bad iters: a measurement arm requires at least one sample\n");
        return 2;
    }
    if(exitos_bench_parse_u64(argc>2?argv[2]:"4096",&parsed)!=0 ||
       parsed==0 || parsed>SIZE_MAX){ fprintf(stderr,"bad io size\n"); return 2; }
    iosz=(size_t)parsed;
    if(only && !arm_name_known(only)){
        fprintf(stderr,"REFUSING: unknown EXITOS_ONLY arm\n");
        return 2;
    }
    if(only && read_only && arm_name_writes(only)){
        fprintf(stderr,"REFUSING: EXITOS_READ cannot select a write arm\n");
        return 2;
    }

    printf("== exitos minimal performance probe ==\n");
    printf("iters=%d io=%zuB\n\n",iters,iosz);
    if(arm_wanted("syscall",0)){
        arm_rc=bench_syscall(iters>20000?20000:iters);
        if(arm_rc!=0){ fprintf(stderr,"syscall arm failed: %s\n",strerror(-arm_rc)); return 4; }
    }
    if(arm_wanted("decision",0)){
        arm_rc=bench_decision(iters,iosz);
        if(arm_rc!=0){ fprintf(stderr,"decision arm failed: %s\n",strerror(-arm_rc)); return 4; }
    }
    printf("\n");

    if(only && !strcmp(only,"fsread") &&
       (!read_only || !getenv("EXITOS_FSFILE"))){
        fprintf(stderr,"filesystem read arm requires EXITOS_READ and EXITOS_FSFILE\n");
        return 4;
    }
    if(only && (!strcmp(only,"fsbuf") || !strcmp(only,"fsdir") ||
                !strcmp(only,"fsnf")) && (!scratch || read_only)){
        fprintf(stderr,"filesystem write arm requires EXITOS_SCRATCH and write mode\n");
        return 4;
    }

    if(read_only){
        const char *fsfile=getenv("EXITOS_FSFILE");
        if(fsfile && arm_wanted("fsread",0)){
            arm_rc=bench_fs_read(fsfile,iters,iosz);
            if(arm_rc!=0){ fprintf(stderr,"filesystem read arm failed: %s\n",strerror(-arm_rc)); return 4; }
        }
    } else if(scratch){
        char p[512];
        if(arm_wanted("fsbuf",1)){
            int n=snprintf(p,sizeof p,"%s/exitos_bench_buf.dat",scratch);
            if(n<0 || (size_t)n>=sizeof p) return 4;
            arm_rc=bench_fs(p,iters,iosz,0);
            if(arm_rc!=0){ fprintf(stderr,"filesystem buffered arm failed: %s\n",strerror(-arm_rc)); return 4; }
        }
        if(arm_wanted("fsdir",1)){
            int n=snprintf(p,sizeof p,"%s/exitos_bench_dir.dat",scratch);
            if(n<0 || (size_t)n>=sizeof p) return 4;
            arm_rc=bench_fs(p,iters,iosz,1);
            if(arm_rc!=0){ fprintf(stderr,"filesystem direct arm failed: %s\n",strerror(-arm_rc)); return 4; }
        }
        if(arm_wanted("fsnf",1)){
            int n=snprintf(p,sizeof p,"%s/exitos_bench_ns.dat",scratch);
            if(n<0 || (size_t)n>=sizeof p) return 4;
            arm_rc=bench_fs_mode(p,iters,iosz,1,0,
                                 "ext4 O_DIRECT write, NO fdatasync");
            if(arm_rc!=0){ fprintf(stderr,"filesystem no-sync arm failed: %s\n",strerror(-arm_rc)); return 4; }
        }
    }

    /* An isolated filesystem/CPU arm must not even open the raw-device path. */
    if(!arm_wanted("raw",1) && !arm_wanted("rawnf",1) &&
       !arm_wanted("rawread",0) && !arm_wanted("flush",1) &&
       !arm_wanted("nf",1) && !arm_wanted("fua",1) &&
       !arm_wanted("batch",1) && !arm_wanted("nvmeread",0) &&
       !getenv("EXITOS_CERTIFY")) return 0;

    printf("\n");
    if(!dev){
        printf("(set EXITOS_DEV + EXITOS_LBA_START + EXITOS_LBA_COUNT for raw-device arms)\n");
        return only || getenv("EXITOS_CERTIFY") ? 4 : 0;
    }
    s0=getenv("EXITOS_LBA_START"); sc=getenv("EXITOS_LBA_COUNT");
    if(!s0||!sc || exitos_bench_parse_u64(s0,&lba0)!=0 ||
       exitos_bench_parse_u64(sc,&cnt)!=0 || cnt==0){
        fprintf(stderr,"REFUSING: strict EXITOS_LBA_START/COUNT are required.\n");
        return 2;
    }

    {
        int dev_flags=(read_only||iters==0?O_RDONLY:O_RDWR)|O_CLOEXEC;
        if(arm_wanted("raw",1) || arm_wanted("rawnf",1) ||
           arm_wanted("rawread",0))
            dev_flags|=O_DIRECT;
        devfd=open(dev,dev_flags);
    }
    if(devfd<0){ fprintf(stderr,"REFUSING: cannot open %s: %s\n",dev,strerror(errno)); return 3; }
    v=exitos_devguard_check_fd(devfd,getenv("EXITOS_EXPECT_SERIAL"),sn,sizeof sn);
    lbs=exitos_devguard_lbs_fd(devfd);
    if(lbs==0){ fprintf(stderr,"REFUSING: cannot prove actual namespace LBS\n"); result=3; goto out; }
    expect_lbs=getenv("EXITOS_LBS");
    if(expect_lbs && (exitos_bench_parse_u64(expect_lbs,&expected)!=0 ||
                      expected>UINT32_MAX || (uint32_t)expected!=lbs)){
        fprintf(stderr,"REFUSING: EXITOS_LBS must exactly equal actual LBS %u\n",lbs);
        result=3; goto out;
    }
    {
        int id=ioctl(devfd,NVME_IOCTL_ID);
        if(id>0){ actual_nsid=(uint32_t)id; is_nvme=1; }
    }
    expect_nsid=getenv("EXITOS_NSID");
    if(expect_nsid && (exitos_bench_parse_u64(expect_nsid,&expected)!=0 ||
                       expected==0 || expected>UINT32_MAX || !is_nvme ||
                       (uint32_t)expected!=actual_nsid)){
        fprintf(stderr,"REFUSING: EXITOS_NSID does not exactly match this retained namespace\n");
        result=3; goto out;
    }
    whole=exitos_devguard_passthru_ok_fd(devfd);
    printf("devguard: %s serial=%s -> %s\n",dev,sn[0]?sn:"(unreadable)",
           exitos_devguard_str(v));

    if(v==DEVGUARD_IS_PARTITION && getenv("EXITOS_ALLOW_PARTITION")){
        fprintf(stderr,"NOTE: retained target is a partition; native passthru remains disabled.\n");
    } else if(v==DEVGUARD_HAS_PARTITIONS && getenv("EXITOS_WINDOW_IN_PART")){
        char why[320];
        if(!getenv("EXITOS_EXPECT_SERIAL") || !*getenv("EXITOS_EXPECT_SERIAL")){
            fprintf(stderr,"REFUSING: a partitioned namespace requires EXITOS_EXPECT_SERIAL\n");
            result=3; goto out;
        }
        partfd=open_named_device(getenv("EXITOS_WINDOW_IN_PART"),O_RDONLY);
        if(partfd<0 || exitos_devguard_window_in_partition_fds(devfd,partfd,
                    lba0,cnt,lbs,why,sizeof why)!=0){
            fprintf(stderr,"REFUSING: %s\n",partfd<0?"cannot retain named partition":why);
            result=3; goto out;
        }
        fprintf(stderr,"NOTE: retained partition containment proved: %s\n",why);
    } else if(v!=DEVGUARD_OK){
        fprintf(stderr,"REFUSING: %s\n",exitos_devguard_str(v));
        result=3; goto out;
    }

    if(exitos_bench_io_position(lba0,cnt,0,1,0,iosz,lbs,&first)!=0){
        fprintf(stderr,"REFUSING: initial I/O does not fit the checked native-LBA/off_t window\n");
        result=3; goto out;
    }
    window_end=first.window_end;

    pin=getenv("EXITOS_PIN");
    if(pin && getenv("EXITOS_CERTIFY")){
        struct devguard_pin np;
        if(exitos_devguard_pin_capture_fd(devfd,lba0,cnt,&np)==0 &&
           exitos_devguard_pin_save(&np,pin)==0)
            printf("CERTIFIED: pin written to %s (identity=%s lbs=%u)\n",
                   pin,np.serial,np.logical_block_size);
        else { fprintf(stderr,"could not capture pin\n"); result=3; }
        goto out;
    }
    if(pin){
        char why[512];
        if(exitos_devguard_pin_verify_window_fd(devfd,pin,lba0,cnt,why,sizeof why)!=0){
            fprintf(stderr,"REFUSING (pin): %s\n",why); result=3; goto out;
        }
        printf("pin: %s\n",why);
    } else {
        fprintf(stderr,"REFUSING: EXITOS_PIN not set; certify this retained namespace first.\n");
        result=3; goto out;
    }

    if(read_only){
        printf("read-only mode: write arms are disabled before I/O planning\n");
    } else if(iters>0 && getenv("EXITOS_REUSE_WINDOW")){
        printf("window zero-check WAIVED by EXITOS_REUSE_WINDOW\n");
    } else if(iters>0){
        uint64_t bad=0;
        int z=exitos_devguard_window_is_zero_fd(devfd,lba0,cnt,&bad);
        if(z!=0){
            if(z>0) fprintf(stderr,"REFUSING: target is nonzero at native LBA %llu\n",
                           (unsigned long long)bad);
            else fprintf(stderr,"REFUSING: cannot verify the retained window is zero\n");
            result=3; goto out;
        }
    }
    printf("device=%s window=[%llu,%llu) actual_lbs=%u actual_nsid=%u\n",dev,
           (unsigned long long)lba0,(unsigned long long)window_end,lbs,actual_nsid);

    if(arm_wanted("raw",1) || arm_wanted("rawnf",1) || arm_wanted("rawread",0)){
        raw_io=iopath_open_fd(devfd,IOPATH_PWRITE,lbs);
        if(!raw_io || iopath_lbs(raw_io)!=lbs){
            fprintf(stderr,"REFUSING: cannot retain pwrite handle with actual LBS\n");
            result=3; goto out;
        }
    }
    if(arm_wanted("flush",1) || arm_wanted("nf",1) || arm_wanted("fua",1) ||
       arm_wanted("batch",1) || arm_wanted("nvmeread",0)){
        if(!is_nvme || !whole){
            if(!only || native_only_name(only)){
                fprintf(stderr,"REFUSING: selected native arm needs an actual whole NVMe namespace\n");
                result=3; goto out;
            }
        } else {
            nvme_io=iopath_open_fd(devfd,IOPATH_NVME_IOCTL,lbs);
            if(!nvme_io || iopath_lbs(nvme_io)!=lbs ||
               iopath_nsid(nvme_io)!=actual_nsid ||
               (expect_nsid && iopath_nsid(nvme_io)!=(uint32_t)expected)){
                fprintf(stderr,"REFUSING: retained native handle identity/LBS/NSID changed\n");
                result=3; goto out;
            }
        }
    }

    if(arm_wanted("raw",1)){
        arm_rc=bench_iopath_mode(raw_io,lba0,cnt,iters,iosz,lbs,1,0,0x33,
                                 "raw blockdev pwrite+fdatasync");
        if(arm_rc!=0){ result=4; goto out; }
    }
    if(arm_wanted("rawread",0)){
        arm_rc=bench_iopath_read(raw_io,lba0,cnt,iters,iosz,lbs,"raw blockdev read");
        if(arm_rc!=0){ result=4; goto out; }
    }
    if(arm_wanted("nvmeread",0) && nvme_io){
        arm_rc=bench_iopath_read(nvme_io,lba0,cnt,iters,iosz,lbs,"nvme passthru read");
        if(arm_rc!=0){ result=4; goto out; }
    }
    if(arm_wanted("rawnf",1)){
        arm_rc=bench_iopath_mode(raw_io,lba0,cnt,iters,iosz,lbs,0,0,0x33,
                                 "raw blockdev pwrite (NO fdatasync)");
        if(arm_rc!=0){ result=4; goto out; }
    }
    if(arm_wanted("flush",1) && nvme_io){
        arm_rc=bench_iopath_mode(nvme_io,lba0,cnt,iters,iosz,lbs,1,0,0x77,
                                 "nvme passthru ioctl write+FLUSH");
        if(arm_rc!=0){ result=4; goto out; }
    }
    if(arm_wanted("nf",1) && nvme_io){
        arm_rc=bench_iopath_mode(nvme_io,lba0,cnt,iters,iosz,lbs,0,0,0x77,
                                 "nvme passthru write (NO flush)");
        if(arm_rc!=0){ result=4; goto out; }
    }
    if(arm_wanted("fua",1) && nvme_io){
        arm_rc=bench_iopath_mode(nvme_io,lba0,cnt,iters,iosz,lbs,0,1,0x77,
                                 "nvme passthru write (FUA bit)");
        if(arm_rc!=0){ result=4; goto out; }
    }
    if(arm_wanted("batch",1) && nvme_io){
        int Ns[]={1,2,4,8,16,32};
        uint64_t align=4096u/lbs;
        uint64_t starts[sizeof Ns/sizeof Ns[0]];
        uint64_t counts[sizeof Ns/sizeof Ns[0]];
        if(align==0) align=1;
        /* Preflight the complete six-N experiment before N=1 can write.  A
         * later subwindow/width failure must never leave earlier comparison
         * rows on the device. */
        for(unsigned k=0;k<sizeof Ns/sizeof Ns[0];k++){
            int rc=exitos_bench_subwindow(lba0,cnt,k,sizeof Ns/sizeof Ns[0],
                                          align,&starts[k],&counts[k]);
            if(rc==0)
                rc=bench_batch_plan_fits(starts[k],counts[k],300,Ns[k],
                                         iosz,lbs);
            if(rc!=0){ result=4; goto out; }
        }
        for(unsigned k=0;k<sizeof Ns/sizeof Ns[0];k++){
            int rc=bench_batch(nvme_io,starts[k],counts[k],300,Ns[k],iosz,lbs);
            if(rc!=0){ result=4; goto out; }
        }
    }
out:
    iopath_close(nvme_io);
    iopath_close(raw_io);
    if(partfd>=0) close(partfd);
    if(devfd>=0) close(devfd);
    return result;
}
