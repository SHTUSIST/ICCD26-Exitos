#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "exitos_benchguard.h"

static uint64_t bench_off_t_max(void)
{
    unsigned bits = (unsigned)(sizeof(off_t) * CHAR_BIT);
    unsigned value_bits = bits - (((off_t)-1 < (off_t)0) ? 1u : 0u);
    uint64_t max = 0;
    unsigned i;

    if (value_bits > 64u) value_bits = 64u;
    for (i = 0; i < value_bits; i++) max = (max << 1) | UINT64_C(1);
    return max;
}

int exitos_bench_io_position(uint64_t window_start, uint64_t window_count,
                             uint64_t group, uint64_t group_width,
                             uint64_t member, size_t io_bytes, uint32_t lbs,
                             struct exitos_bench_io_position *out)
{
    struct exitos_bench_io_position pos;
    uint64_t ordinal, delta, byte_offset, bytes, off_max;

    if (!out || window_count == 0 || group_width == 0 ||
        member >= group_width || io_bytes == 0 || lbs == 0 ||
        io_bytes % (size_t)lbs != 0)
        return -EINVAL;
    bytes = (uint64_t)io_bytes;
    if ((size_t)bytes != io_bytes)
        return -EOVERFLOW;
    pos.blocks = bytes / (uint64_t)lbs;
    if (pos.blocks == 0 || window_count > UINT64_MAX - window_start)
        return -EOVERFLOW;
    pos.window_end = window_start + window_count;
    if (group > UINT64_MAX / group_width)
        return -EOVERFLOW;
    ordinal = group * group_width;
    if (member > UINT64_MAX - ordinal)
        return -EOVERFLOW;
    ordinal += member;
    if (ordinal > UINT64_MAX / pos.blocks)
        return -EOVERFLOW;
    delta = ordinal * pos.blocks;
    if (delta >= window_count || pos.blocks > window_count - delta)
        return -ERANGE;
    pos.lba = window_start + delta;
    if (pos.lba > UINT64_MAX / (uint64_t)lbs)
        return -EOVERFLOW;
    byte_offset = pos.lba * (uint64_t)lbs;
    off_max = bench_off_t_max();
    if (byte_offset > off_max || bytes - 1 > off_max - byte_offset)
        return -EOVERFLOW;
    pos.byte_offset = (off_t)byte_offset;
    *out = pos;
    return 0;
}

int exitos_bench_byte_span(uint64_t index, uint64_t count, size_t item_bytes,
                           struct exitos_bench_byte_span *out)
{
    struct exitos_bench_byte_span span;
    uint64_t bytes = (uint64_t)item_bytes;
    uint64_t offset, length, off_max;

    if (!out || count == 0 || item_bytes == 0 || (size_t)bytes != item_bytes)
        return -EINVAL;
    if (index > UINT64_MAX / bytes || count > UINT64_MAX / bytes)
        return -EOVERFLOW;
    offset = index * bytes;
    length = count * bytes;
    off_max = bench_off_t_max();
    if (offset > off_max || length > off_max ||
        length - 1 > off_max - offset)
        return -EOVERFLOW;
    span.offset = (off_t)offset;
    span.length = (off_t)length;
    *out = span;
    return 0;
}

int exitos_bench_parse_u64(const char *text, uint64_t *out)
{
    unsigned long long value;
    char *end = NULL;

    if (!text || !*text || !out || text[0] == '+' || text[0] == '-' ||
        (unsigned char)text[0] <= (unsigned char)' ')
        return -EINVAL;
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || !end || *end != '\0')
        return -EINVAL;
    *out = (uint64_t)value;
    return 0;
}

int exitos_bench_arm_enabled(const char *only, int read_only,
                             const char *arm, int arm_writes)
{
    if (!arm || !*arm || (read_only && arm_writes)) return 0;
    return !only || !*only || strcmp(only, arm) == 0;
}

int exitos_bench_subwindow(uint64_t parent_start, uint64_t parent_count,
                           unsigned index, unsigned parts, uint64_t alignment,
                           uint64_t *start, uint64_t *count)
{
    uint64_t slice, offset;

    if (!start || !count || parts == 0 || index >= parts || alignment == 0)
        return -EINVAL;
    if (parent_count > UINT64_MAX - parent_start)
        return -EOVERFLOW;
    slice = parent_count / parts;
    slice = slice / alignment * alignment;
    if (slice == 0)
        return -ENOSPC;
    if ((uint64_t)index > UINT64_MAX / slice)
        return -EOVERFLOW;
    offset = (uint64_t)index * slice;
    if (offset > parent_count || slice > parent_count - offset)
        return -ERANGE;
    *start = parent_start + offset;
    *count = slice;
    return 0;
}

int exitos_bench_extent_lba(const struct exitos_bench_extent *ext, size_t next,
                            uint64_t off, uint64_t len, uint32_t lbs,
                            uint64_t *lba)
{
    uint64_t end;
    size_t i;

    if (!ext || !lba || next == 0 || len == 0 || lbs == 0)
        return -EINVAL;
    if (off % lbs != 0 || len % lbs != 0)
        return -EINVAL;
    if (len > UINT64_MAX - off)
        return -EOVERFLOW;
    end = off + len;
    for (i = 0; i < next; i++) {
        uint64_t ext_end, delta;
        if (ext[i].len > UINT64_MAX - ext[i].logical)
            return -EOVERFLOW;
        ext_end = ext[i].logical + ext[i].len;
        if (off < ext[i].logical || off >= ext_end)
            continue;
        if (end > ext_end)
            return -ERANGE;
        delta = (off - ext[i].logical) / lbs;
        if (delta > UINT64_MAX - ext[i].lba)
            return -EOVERFLOW;
        *lba = ext[i].lba + delta;
        return 0;
    }
    return -ENOENT;
}

int exitos_bench_sectors_to_lba(uint64_t sectors, uint32_t lbs, uint64_t *lba)
{
    uint64_t a = 512, b = lbs, gcd, denominator, multiplier, q;

    if (!lba || lbs == 0)
        return -EINVAL;
    while (b != 0) {
        uint64_t r = a % b;
        a = b;
        b = r;
    }
    gcd = a;
    denominator = (uint64_t)lbs / gcd;
    multiplier = 512 / gcd;
    if (sectors % denominator != 0)
        return -EINVAL;
    q = sectors / denominator;
    if (q > UINT64_MAX / multiplier)
        return -EOVERFLOW;
    *lba = q * multiplier;
    return 0;
}

int exitos_bench_nvme_status(int ioctl_result, int saved_errno)
{
    if (ioctl_result == 0)
        return 0;
    if (ioctl_result < 0)
        return -(saved_errno > 0 ? saved_errno : EIO);
    return -EIO;
}
