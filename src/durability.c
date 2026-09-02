#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "exitos_durability.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "exitos_iopath.h"

exitos_durability exitos_durability_policy(int has_volatile_cache, int supports_fua)
{
    if (!has_volatile_cache)  return EXITOS_DUR_NONE;   /* already durable */
    if (supports_fua)         return EXITOS_DUR_FUA;    /* cheapest correct */
    return EXITOS_DUR_FLUSH;                            /* last resort */
}

static int read_first_line(const char *path, char *out, size_t n)
{
    ssize_t got;
    int fd;

    if (!out || n < 2)
        return -1;
    fd = exitos_internal_open_call(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    do {
        got = read(fd, out, n - 1);
    } while (got < 0 && errno == EINTR);
    if (exitos_internal_close_call(fd) != 0 || got <= 0)
        return -1;
    out[got] = '\0';
    size_t l = strlen(out);
    while (l && isspace((unsigned char)out[l-1])) out[--l] = 0;
    return 0;
}

/* Strip a partition suffix so queue/ attributes resolve on the parent disk. */
static void disk_of(const char *dev_path, char *out, size_t n)
{
    const char *b = strrchr(dev_path, '/');
    b = b ? b + 1 : dev_path;
    snprintf(out, n, "%s", b);
    char probe[320];
    snprintf(probe, sizeof probe, "/sys/class/block/%s/partition", out);
    struct stat st;
    if (stat(probe, &st) != 0) return;
    size_t L = strlen(out);
    while (L && out[L-1] >= '0' && out[L-1] <= '9') L--;
    if (L > 1 && out[L-1] == 'p' && out[L-2] >= '0' && out[L-2] <= '9') L--;
    out[L] = 0;
}

int exitos_durability_probe(const char *dev_path, int *has_volatile_cache, int *supports_fua)
{
    if (!dev_path) return -1;
    char disk[128]; disk_of(dev_path, disk, sizeof disk);

    char p[320], v[128];
    snprintf(p, sizeof p, "/sys/class/block/%s/queue/write_cache", disk);
    if (read_first_line(p, v, sizeof v) != 0) return -1;
    /* "write back" = volatile cache present; "write through" = none. */
    int vol = (strstr(v, "back") != NULL);

    int fua = 0;
    snprintf(p, sizeof p, "/sys/class/block/%s/queue/fua", disk);
    if (read_first_line(p, v, sizeof v) == 0) fua = (atoi(v) != 0);

    if (has_volatile_cache) *has_volatile_cache = vol;
    if (supports_fua)       *supports_fua = fua;
    return 0;
}

exitos_durability exitos_durability_policy_for(const char *dev_path)
{
    int vol = 1, fua = 0;                 /* fail closed */
    if (exitos_durability_probe(dev_path, &vol, &fua) != 0)
        return EXITOS_DUR_FLUSH;
    return exitos_durability_policy(vol, fua);
}

const char *exitos_durability_str(exitos_durability d)
{
    switch (d) {
    case EXITOS_DUR_NONE:  return "none (device has no volatile write cache)";
    case EXITOS_DUR_FUA:   return "fua (durability rides on the write command)";
    case EXITOS_DUR_FLUSH: return "flush (explicit device cache flush)";
    }
    return "unknown";
}
