/* Pick the cheapest CORRECT way to make a write durable, from what the device
 * reports rather than from an assumption.
 *
 * Background: enterprise drives with power-loss protection report no volatile
 * write cache. The kernel sees that (/sys/block/X/queue/write_cache ==
 * "write through") and already skips flushing on fdatasync. Issuing an explicit
 * NVMe FLUSH anyway still costs a full controller barrier and buys nothing.
 * Setting FUA on such a drive is equally pointless: there is no cache to
 * bypass.
 *
 * Hence: ask the device, then do the least work that is still correct. */
#ifndef EXITOS_DURABILITY_H
#define EXITOS_DURABILITY_H

typedef enum {
    EXITOS_DUR_NONE = 0,  /* no volatile cache: a completed write is durable   */
    EXITOS_DUR_FUA,       /* volatile cache + FUA: durability rides on the write*/
    EXITOS_DUR_FLUSH,     /* volatile cache, no FUA: explicit FLUSH required   */
} exitos_durability;

/* has_volatile_cache / supports_fua are booleans. */
exitos_durability exitos_durability_policy(int has_volatile_cache, int supports_fua);

/* Read the two facts from sysfs for a block device path. 0 on success. */
int exitos_durability_probe(const char *dev_path, int *has_volatile_cache, int *supports_fua);

/* Probe then decide. FAILS CLOSED: anything unreadable yields EXITOS_DUR_FLUSH,
 * because wrongly skipping a flush loses data while a needless flush only costs
 * time. */
exitos_durability exitos_durability_policy_for(const char *dev_path);

const char *exitos_durability_str(exitos_durability d);
#endif
