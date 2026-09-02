#include "tap.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>

#include "exitos_benchguard.h"

#define SENTINEL 0xfeedfacecafebeefULL

static uint64_t test_off_t_max(void)
{
    unsigned bits = (unsigned)(sizeof(off_t) * CHAR_BIT);
    unsigned value_bits = bits - (((off_t)-1 < (off_t)0) ? 1u : 0u);
    uint64_t max = 0;
    unsigned i;

    if (value_bits > 64u) value_bits = 64u;
    for (i = 0; i < value_bits; i++) max = (max << 1) | UINT64_C(1);
    return max;
}

int main(void)
{
    uint64_t start = SENTINEL, count = SENTINEL, lba = SENTINEL;

    T_EQ(exitos_bench_subwindow(1000, 600, 2, 6, 8, &start, &count), 0,
         "split a declared window into aligned benchmark subwindows");
    T_EQ(start, 1192, "subwindow 2 starts inside the declared parent");
    T_EQ(count, 96, "each subwindow is rounded down to the LBA alignment");
    T_OK(start >= 1000 && start + count <= 1600,
         "computed subwindow is wholly inside [1000,1600)");

    start = count = SENTINEL;
    T_EQ(exitos_bench_subwindow(1000, 7, 0, 6, 8, &start, &count), -ENOSPC,
         "a parent too small for one aligned slice is refused");
    T_OK(start == SENTINEL && count == SENTINEL,
         "failed subwindow calculation leaves outputs untouched");
    T_EQ(exitos_bench_subwindow(UINT64_MAX - 3, 8, 0, 1, 1,
                               &start, &count), -EOVERFLOW,
         "overflowing parent window is refused");

    struct exitos_bench_extent ext[] = {
        { .logical = 0,    .lba = 100, .len = 8192 },
        { .logical = 8192, .lba = 900, .len = 8192 },
    };
    T_EQ(exitos_bench_extent_lba(ext, 2, 0, 8192, 512, &lba), 0,
         "range wholly inside one extent translates");
    T_EQ(lba, 100, "extent translation returns its first LBA");
    lba = SENTINEL;
    T_EQ(exitos_bench_extent_lba(ext, 2, 4096, 8192, 512, &lba), -ERANGE,
         "8 KiB crossing two physically discontiguous extents is refused");
    T_EQ(lba, SENTINEL, "cross-extent refusal leaves LBA untouched");
    T_EQ(exitos_bench_extent_lba(ext, 2, 8192, 4096, 512, &lba), 0,
         "range beginning exactly at the second extent translates");
    T_EQ(lba, 900, "second extent uses its own physical LBA");
    T_EQ(exitos_bench_extent_lba(ext, 2, 1, 4096, 512, &lba), -EINVAL,
         "logical offset not aligned to the device LBA is refused");

    T_EQ(exitos_bench_sectors_to_lba(262144, 4096, &lba), 0,
         "sysfs 512-byte sectors convert on a 4Kn device");
    T_EQ(lba, 32768, "262144 sectors equal 32768 native 4 KiB LBAs");
    lba = SENTINEL;
    T_EQ(exitos_bench_sectors_to_lba(1, 4096, &lba), -EINVAL,
         "non-LBA-aligned partition start is refused");
    T_EQ(lba, SENTINEL, "failed sector conversion leaves output untouched");

    T_EQ(exitos_bench_nvme_status(0, 0), 0,
         "zero ioctl result is NVMe success");
    T_EQ(exitos_bench_nvme_status(-1, EACCES), -EACCES,
         "negative ioctl result preserves errno");
    T_EQ(exitos_bench_nvme_status(0x81, 0), -EIO,
         "positive NVMe status is rejection, never a fast success");

    {
        struct exitos_bench_io_position pos = {
            .lba = SENTINEL,
            .blocks = SENTINEL,
            .window_end = SENTINEL,
            .byte_offset = (off_t)17,
        };
        T_EQ(exitos_bench_io_position(10, 4, 1, 2, 1, 4096, 4096, &pos), 0,
             "checked grouped I/O arithmetic accepts an in-window 4Kn request");
        T_EQ(pos.lba, 13, "group/member arithmetic selects native LBA 13");
        T_EQ(pos.blocks, 1, "one 4Kn I/O occupies one native logical block");
        T_EQ(pos.window_end, 14, "checked helper returns the proven window end");
        T_EQ((uint64_t)pos.byte_offset, UINT64_C(13) * 4096,
             "checked helper returns the exact representable byte offset");

        pos.lba = pos.blocks = pos.window_end = SENTINEL;
        pos.byte_offset = (off_t)17;
        T_EQ(exitos_bench_io_position(UINT64_MAX - 1, 2, 0, 1, 0,
                                     4096, 4096, &pos), -EOVERFLOW,
             "window start plus count overflow is refused");
        T_OK(pos.lba == SENTINEL && pos.blocks == SENTINEL &&
             pos.window_end == SENTINEL && pos.byte_offset == (off_t)17,
             "failed checked arithmetic leaves every output untouched");

        T_EQ(exitos_bench_io_position(0, UINT64_MAX, UINT64_MAX, 2, 0,
                                     4096, 4096, &pos), -EOVERFLOW,
             "group times group-width overflow is refused");
        T_EQ(exitos_bench_io_position(0, UINT64_MAX, UINT64_MAX, 1, 0,
                                     8192, 4096, &pos), -EOVERFLOW,
             "I/O ordinal times blocks overflow is refused independently");
        T_EQ(exitos_bench_io_position(100, 2, 2, 1, 0,
                                     4096, 4096, &pos), -ERANGE,
             "request beyond the certified LBA window is refused");

        {
            uint64_t off_max = test_off_t_max();
            uint64_t first_bad_lba = off_max / 4096 + 1;
            T_EQ(exitos_bench_io_position(first_bad_lba, 1, 0, 1, 0,
                                         4096, 4096, &pos), -EOVERFLOW,
                 "byte offset outside this ABI's off_t range is refused");
            if (off_max >= 4096) {
                uint64_t crossing_lba = off_max / 4096;
                T_EQ(exitos_bench_io_position(crossing_lba, 2, 0, 1, 0,
                                             8192, 4096, &pos), -EOVERFLOW,
                     "byte window crossing this ABI's off_t maximum is refused");
            }
        }
    }

    {
        struct exitos_bench_byte_span span = {
            .offset = (off_t)17,
            .length = (off_t)19,
        };
        T_EQ(exitos_bench_byte_span(3, 2, 4096, &span), 0,
             "checked file-span arithmetic accepts two ordinary records");
        T_EQ((uint64_t)span.offset, 12288,
             "checked file span multiplies its starting record exactly");
        T_EQ((uint64_t)span.length, 8192,
             "checked file span multiplies its record count exactly");
        span.offset = (off_t)17;
        span.length = (off_t)19;
        T_EQ(exitos_bench_byte_span(UINT64_MAX, 1, 2, &span), -EOVERFLOW,
             "checked file span rejects index times size overflow");
        T_OK(span.offset == (off_t)17 && span.length == (off_t)19,
             "failed file-span arithmetic leaves outputs untouched");
        T_EQ(exitos_bench_byte_span(0, UINT64_MAX, 2, &span), -EOVERFLOW,
             "checked file span rejects count times size overflow");
        {
            uint64_t off_max = test_off_t_max();
            T_EQ(exitos_bench_byte_span(off_max / 2, 2, 2, &span), -EOVERFLOW,
                 "checked file span rejects a last byte beyond off_t");
        }
    }

    {
        uint64_t parsed = SENTINEL;
        T_EQ(exitos_bench_parse_u64("4096", &parsed), 0,
             "strict environment parser accepts decimal unsigned data");
        T_EQ(parsed, 4096, "strict environment parser preserves the value");
        T_EQ(exitos_bench_parse_u64("0x10", &parsed), 0,
             "strict environment parser retains explicit hexadecimal support");
        T_EQ(parsed, 16, "hexadecimal expectation is parsed exactly");
        T_OK(exitos_bench_parse_u64(" 1", &parsed) < 0,
             "strict environment parser rejects leading whitespace");
        T_OK(exitos_bench_parse_u64("1 ", &parsed) < 0,
             "strict environment parser rejects trailing whitespace");
        T_OK(exitos_bench_parse_u64("+1", &parsed) < 0,
             "strict environment parser rejects a plus sign");
        T_OK(exitos_bench_parse_u64("-1", &parsed) < 0,
             "strict environment parser rejects a minus sign");
        T_OK(exitos_bench_parse_u64("1junk", &parsed) < 0,
             "strict environment parser rejects trailing junk");
        T_OK(exitos_bench_parse_u64("18446744073709551616", &parsed) < 0,
             "strict environment parser rejects uint64 overflow");
    }

    {
        const char *write_arms[] = {
            "fsbuf", "fsdir", "fsnf", "raw", "rawnf",
            "flush", "nf", "fua", "batch"
        };
        unsigned enabled_writes = 0;
        size_t i;
        for (i = 0; i < sizeof write_arms / sizeof write_arms[0]; i++)
            enabled_writes += (unsigned)exitos_bench_arm_enabled(
                NULL, 1, write_arms[i], 1);
        T_EQ(enabled_writes, 0,
             "read-only planning enables zero write-submit arms");
        T_OK(exitos_bench_arm_enabled(NULL, 1, "rawread", 0),
             "read-only planning still enables a raw read arm");
        T_OK(!exitos_bench_arm_enabled("rawnf", 0, "raw", 1),
             "EXITOS_ONLY excludes every differently named write arm");
        T_OK(exitos_bench_arm_enabled("rawnf", 0, "rawnf", 1),
             "EXITOS_ONLY enables exactly its named arm");
        T_OK(!exitos_bench_arm_enabled("rawnf", 0, "syscall", 0),
             "EXITOS_ONLY also isolates non-I/O measurement arms");
    }

    T_DONE();
}
