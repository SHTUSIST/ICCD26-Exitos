#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

typedef int (*claims_fn)(long);
typedef int (*available_fn)(const char *);
typedef int (*start_fn)(const char *);
typedef int (*started_fn)(void);

int main(int argc, char **argv)
{
    void *h;
    claims_fn claims;
    available_fn available;
    start_fn start;
    started_fn started;
    unsigned char *table;
    long nr;

    if (argc != 2)
        return 64;
    h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 65;
    }
    claims = (claims_fn)dlsym(h, "exitos_bpftime_claims");
    available = (available_fn)dlsym(h, "exitos_bpftime_available");
    start = (start_fn)dlsym(h, "exitos_bpftime_start");
    started = (started_fn)dlsym(h, "exitos_bpftime_started");
    table = (unsigned char *)dlsym(h, "exitos_claim_tbl");
    if (!claims || !available || !start || !started || !table)
        return 66;

    for (nr = 0; nr < 512; nr++)
        if (claims(nr) != 0 || table[nr] != 0)
            return 67;
    if (claims(-1) != 0 || claims(999999) != 0)
        return 68;
    if (available(argv[1]) != 0)
        return 69;
    if (start(argv[1]) != -EOPNOTSUPP)
        return 70;
    if (started() != 0)
        return 71;

    puts("claims=0 table=0 available=0 start=-EOPNOTSUPP started=0");
    dlclose(h);
    return 0;
}
