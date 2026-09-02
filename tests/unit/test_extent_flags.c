#include "tap.h"

#include <linux/fiemap.h>

#include "exitos_extent.h"

int main(void)
{
    T_EQ(exitos_extent_flags_have_stable_location(0), 1,
         "a plain FIEMAP extent has a stable raw location");
    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_LAST), 1,
         "LAST is a walk marker, not a location hazard");
    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_MERGED), 1,
         "MERGED alone does not make the reported location unusable");
    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_UNWRITTEN), 1,
         "UNWRITTEN has a location; Maco rejects it for visibility separately");

    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_UNKNOWN), 0,
         "UNKNOWN location is refused");
    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_DELALLOC), 0,
         "delayed allocation has no stable physical location");
    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_ENCODED), 0,
         "encoded data cannot be bypassed as plain blocks");
    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_DATA_ENCRYPTED), 0,
         "encrypted data cannot be bypassed as plaintext blocks");
    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_NOT_ALIGNED), 0,
         "an unaligned physical extent cannot be translated blockwise");
    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_DATA_INLINE), 0,
         "inline data has no raw data-block location");
    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_DATA_TAIL), 0,
         "tail-packed data has no exclusive raw data-block location");
    T_EQ(exitos_extent_flags_have_stable_location(FIEMAP_EXTENT_SHARED), 0,
         "a shared/reflink extent must never be overwritten behind the filesystem");

    T_DONE();
}
