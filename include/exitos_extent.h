/* extent: pull a file's offset->physical-block runs out of ext4 via FIEMAP. */
#ifndef EXITOS_EXTENT_H
#define EXITOS_EXTENT_H
#include <stdint.h>
#include "exitos_maco.h"
#include "exitos_geom.h"

struct extent_run { uint64_t file_off, fs_block, len; uint32_t flags; };

/* True only when FIEMAP's physical address can safely be treated as a stable,
 * exclusive raw-block location.  UNWRITTEN is intentionally handled by the
 * Maco loader because it has a real location but different visibility rules. */
int exitos_extent_flags_have_stable_location(uint32_t flags);

/* Read up to max_runs extents of fd. Returns count, or negative errno. */
int exitos_extent_read(int fd, struct extent_run *runs, int max_runs);

/* Same, but the caller says whether the kernel must flush before reporting.
 * FIEMAP_FLAG_SYNC forces a writeback pass; that is right once, when a file is
 * first registered and delayed allocation may not have been resolved yet, and
 * wrong on every later re-read of a file that is actively being written -- each
 * one then drags a full writeback into the write path. */
int exitos_extent_read_sync(int fd, struct extent_run *runs, int max_runs, int sync);

/* Read fd's extents, translate through geom, load into maco.
 * Refuses (and loads nothing) when geom->writable_raw == 0. */
int exitos_extent_load_maco(int fd, const struct exitos_geom *g, struct maco *m);
int exitos_extent_load_maco_sync(int fd, const struct exitos_geom *g, struct maco *m,
                                 int sync);
#endif
