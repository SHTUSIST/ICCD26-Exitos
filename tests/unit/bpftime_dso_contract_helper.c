#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef int64_t (*orig_fn)(int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t);
typedef int (*internal_open_fn)(const char *, int, mode_t);
typedef int (*internal_openat_fn)(int, const char *, int, mode_t);
typedef int (*internal_close_fn)(int);
typedef ssize_t (*internal_pwrite_fn)(int, const void *, size_t, off_t);
typedef int (*internal_fdatasync_fn)(int);
typedef int (*claims_fn)(long);
typedef int (*start_fn)(const char *);

static int64_t scripted_result;
static int calls;
static int64_t last_nr, last_a1, last_a3, last_a4;
static const char *last_path;

static int maps_contains(const char *needle)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char *line = NULL;
    size_t cap = 0;
    int found = 0;

    if (!f)
        return -1;
    while (getline(&line, &cap, f) >= 0) {
        if (strstr(line, needle)) {
            found = 1;
            break;
        }
    }
    free(line);
    fclose(f);
    return found;
}

static int64_t fake_orig(int64_t nr, int64_t a1, int64_t a2, int64_t a3,
                         int64_t a4, int64_t a5, int64_t a6)
{
    (void)a5; (void)a6;
    calls++;
    last_nr = nr;
    last_a1 = a1;
    last_path = (const char *)(uintptr_t)a2;
    last_a3 = a3;
    last_a4 = a4;
    return scripted_result;
}

int main(int argc, char **argv)
{
    void *h;
    orig_fn *orig_slot;
    internal_open_fn iopen;
    internal_openat_fn iopenat;
    internal_close_fn iclose;
    internal_pwrite_fn ipwrite;
    internal_fdatasync_fn ifdatasync;
    claims_fn claims;
    start_fn start;
    unsigned char *claim_tbl;
    int rc;

    if (argc != 4)
        return 64;
    h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 65;
    }
    orig_slot = (orig_fn *)dlsym(h, "exitos_bpftime_orig");
    iopen = (internal_open_fn)dlsym(h, "exitos_internal_open_call");
    iopenat = (internal_openat_fn)dlsym(h, "exitos_internal_openat_call");
    iclose = (internal_close_fn)dlsym(h, "exitos_internal_close_call");
    ipwrite = (internal_pwrite_fn)dlsym(h, "exitos_internal_pwrite_call");
    ifdatasync = (internal_fdatasync_fn)dlsym(h, "exitos_internal_fdatasync_call");
    claims = (claims_fn)dlsym(h, "exitos_bpftime_claims");
    start = (start_fn)dlsym(h, "exitos_bpftime_start");
    claim_tbl = (unsigned char *)dlsym(h, "exitos_claim_tbl");
    if (!orig_slot || !iopen || !iopenat || !iclose || !ipwrite ||
        !ifdatasync || !claims || !start || !claim_tbl)
        return 66;
    *orig_slot = fake_orig;

    calls = 0;
    scripted_result = 77;
    errno = 0;
    rc = iopen("/control/open", O_RDONLY | O_CLOEXEC, 0);
    if (rc != 77 || calls != 1 || last_nr != SYS_openat ||
        last_a1 != AT_FDCWD || strcmp(last_path, "/control/open") != 0 ||
        last_a3 != (O_RDONLY | O_CLOEXEC) || last_a4 != 0)
        return 67;

    calls = 0;
    scripted_result = 78;
    rc = iopenat(13, "relative", O_RDWR | O_CLOEXEC, 0600);
    if (rc != 78 || calls != 1 || last_nr != SYS_openat || last_a1 != 13 ||
        strcmp(last_path, "relative") != 0 ||
        last_a3 != (O_RDWR | O_CLOEXEC) || last_a4 != 0600)
        return 68;

    calls = 0;
    scripted_result = -EREMOTEIO;
    errno = 0;
    rc = iclose(78);
    if (rc != -1 || errno != EREMOTEIO || calls != 1 ||
        last_nr != SYS_close || last_a1 != 78)
        return 69;

    calls = 0;
    scripted_result = 0;
    if (iclose(77) != 0 || calls != 1 || last_nr != SYS_close || last_a1 != 77)
        return 70;

    calls = 0;
    scripted_result = 3;
    if (ipwrite(79, "abc", 3, 4096) != 3 || calls != 1 ||
        last_nr != SYS_pwrite64 || last_a1 != 79 || last_a3 != 3 ||
        last_a4 != 4096)
        return 71;

    calls = 0;
    scripted_result = -EIO;
    errno = 0;
    if (ifdatasync(79) != -1 || errno != EIO || calls != 1 ||
        last_nr != SYS_fdatasync || last_a1 != 79)
        return 72;

    if (access(argv[2], R_OK) != 0 || maps_contains(argv[2]) != 0)
        return 73;
    if (setenv("EXITOS_BPFTIME_MARKER_SENTINEL", argv[3], 1) != 0)
        return 74;
    if (claim_tbl[SYS_pwrite64] != 0 || claims(SYS_pwrite64) != 0 ||
        start(argv[2]) != -EOPNOTSUPP)
        return 75;
    if (access(argv[3], F_OK) == 0)
        return 76;
    if (maps_contains(argv[2]) != 0)
        return 77;

    dlclose(h);
    return 0;
}
