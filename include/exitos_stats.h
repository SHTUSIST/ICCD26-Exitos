/* Counters that make interception observable from outside the process.
 * A correct checksum alone cannot distinguish "the fast path ran and was right"
 * from "the fast path never ran at all"; these separate the two. */
#ifndef EXITOS_STATS_H
#define EXITOS_STATS_H
#include <stdint.h>
typedef enum {
    EXITOS_STAT_FAST_WRITE = 0,  /* a write serviced by the shortcut          */
    EXITOS_STAT_FAST_SYNC,       /* an fdatasync answered without the kernel  */
    EXITOS_STAT_PASS,            /* handed back to the ordinary kernel path   */
    EXITOS_STAT_DECLINED,        /* registration refused (unsupported device) */
    EXITOS_STAT_REFRESH,         /* the file layout was re-read from the kernel */
    EXITOS_STAT_MAX
} exitos_stat_id;
void     exitos_stat_inc(exitos_stat_id id);
uint64_t exitos_stat_get(exitos_stat_id id);
int      exitos_stats_dump(const char *path);
int      exitos_stats_load(const char *path, uint64_t *out, int n);
#endif
