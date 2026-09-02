/* Writes a recognisable pattern through plain POSIX calls. With the preload
 * library armed these go down the fast path; without it they are normal writes.
 * Either way the file content must end up identical -- that is the whole test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char **argv){
    if(argc<3){ fprintf(stderr,"usage: %s <file> <nblocks>\n",argv[0]); return 2; }
    int n=atoi(argv[2]); size_t BS=4096;
    int fd=open(argv[1],O_RDWR|O_CREAT|O_DIRECT,0644);
    if(fd<0){ perror("open"); return 1; }
    if(fallocate(fd,0,0,(off_t)BS*n)!=0){ perror("fallocate"); return 1; }
    void *buf; if(posix_memalign(&buf,4096,BS)) return 1;
    for(int i=0;i<n;i++){
        memset(buf,0,BS);
        snprintf((char*)buf,BS,"EXITOS-BLOCK-%06d-payload",i);
        if(pwrite(fd,buf,BS,(off_t)i*BS)!=(ssize_t)BS){ perror("pwrite"); return 1; }
    }
    if(fdatasync(fd)!=0){ perror("fdatasync"); return 1; }
    close(fd); free(buf);
    printf("wrote %d blocks\n",n); return 0;
}
