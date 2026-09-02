/* Maco (Metadata Collector): in-memory file-offset -> device-LBA map,
 * populated from the ext4 extent tree so the write path never walks it. */
#ifndef EXITOS_MACO_H
#define EXITOS_MACO_H
#include <stdint.h>
#include <stddef.h>

struct maco_entry {
    uint64_t file_off;  /* byte offset within the file      */
    uint64_t lba;       /* device LBA (logical-block units) */
    uint64_t len;       /* byte length of this run          */
};

struct maco;

struct maco *maco_create(uint32_t lbs);
void         maco_destroy(struct maco *m);
int          maco_insert(struct maco *m, uint64_t file_off, uint64_t lba, uint64_t len);

/* Look up file_off. On hit: *lba = target LBA, *contig = contiguous bytes
 * remaining in this run from file_off. Returns 0 on hit, -ENOENT on miss. */
int          maco_lookup(const struct maco *m, uint64_t file_off,
                         uint64_t *lba, uint64_t *contig);

/* Invalidate [off, off+len). Used on truncate / log rotation. */
int          maco_invalidate(struct maco *m, uint64_t off, uint64_t len);
void         maco_clear(struct maco *m);
size_t       maco_count(const struct maco *m);
uint64_t     maco_mapped_bytes(const struct maco *m);
#endif
