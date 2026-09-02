/* Tier 0 unit tests for the iopath module.
 *
 * NOTHING here touches a device: no root, no loop device, no NVMe. The star of
 * this file is iopath_encode_nvme_write(), which fills a struct
 * nvme_passthru_cmd for an NVMe WRITE without submitting it. That lets us prove
 * the command encoding -- opcode, nsid, data pointer, length, and above all the
 * 64-bit starting-LBA split across cdw10/cdw11 -- on a machine that has no NVMe
 * controller at all.
 *
 * NLB convention (NVMe base spec, Write command, CDW12 bits 15:00): the NLB
 * field is ZERO-BASED, i.e. a transfer of N logical blocks is encoded as N-1.
 * The header passes nlb as a uint16_t, and the encoder has no logical block
 * size to derive it from, so the caller hands in the already-zero-based value
 * and the encoder stores it verbatim in the low 16 bits of cdw12. Every case
 * below spells out both numbers: "8 blocks -> nlb 7 -> cdw12 low16 == 7".
 *
 * The encoder must not constrain the high bits of cdw12 (PRINFO/FUA/LR live
 * there), so we only ever mask off the low 16 bits.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <linux/nvme_ioctl.h>

#include "exitos_iopath.h"
#include "tap.h"

#define NVME_OPC_WRITE 0x01

/* Fill cmd with a recognisable poison so we can tell "the encoder wrote 0 here"
 * apart from "the encoder never touched this field". */
static void poison(struct nvme_passthru_cmd *c)
{
    memset(c, 0xAA, sizeof(*c));
}

/* One encode + the four field checks every case shares. */
static void check_common(struct nvme_passthru_cmd *c, uint32_t nsid,
                         const void *buf, size_t len, const char *what)
{
    T_EQ(c->opcode, NVME_OPC_WRITE, "%s: opcode == 0x01 (NVMe WRITE)", what);
    T_EQ(c->nsid, nsid, "%s: nsid == %u", what, nsid);
    T_OK(c->addr == (uint64_t)(uintptr_t)buf,
         "%s: addr == %p (points at the caller's buffer)", what, buf);
    T_EQ(c->data_len, (uint32_t)len, "%s: data_len == %zu", what, len);
}

int main(void)
{
    /* 64 KiB, 4096-aligned: big enough for every data_len used below. */
    void *raw = NULL;
    if (posix_memalign(&raw, 4096, 65536) != 0 || raw == NULL) {
        T_SKIP("posix_memalign failed - cannot run encoder tests");
        T_DONE();
    }
    memset(raw, 0x11, 65536);
    unsigned char *buf = (unsigned char *)raw;

    struct nvme_passthru_cmd c;
    int rc;

    /* ---------------------------------------------------------------- *
     * 1. The headline case: a start LBA above 2^32 must split into
     *    cdw10 = low 32 bits, cdw11 = high 32 bits.
     *    slba = 0x1234567890ABCDEF -> cdw10 = 0x90ABCDEF, cdw11 = 0x12345678
     *    16 blocks -> nlb = 15 -> cdw12 low 16 bits = 15
     * ---------------------------------------------------------------- */
    poison(&c);
    rc = iopath_encode_nvme_write(&c, 1, 0x1234567890ABCDEFULL, 15, buf, 65536);
    T_EQ(rc, 0, "encode(slba=0x1234567890ABCDEF) returns 0");
    check_common(&c, 1, buf, 65536, "hi-slba");
    T_EQ(c.cdw10, 0x90ABCDEFu, "hi-slba: cdw10 == 0x90ABCDEF (slba low 32)");
    T_EQ(c.cdw11, 0x12345678u, "hi-slba: cdw11 == 0x12345678 (slba high 32)");
    T_EQ(c.cdw12 & 0xFFFFu, 15u,
         "hi-slba: cdw12[15:0] == 15 (16 blocks, zero-based)");

    /* The encoder must not leave poison in the fields it does not use. A
     * garbage metadata pointer handed to the kernel is a real hazard, not a
     * tidiness complaint. */
    T_EQ(c.metadata, 0ull, "hi-slba: metadata cleared to 0");
    T_EQ(c.metadata_len, 0u, "hi-slba: metadata_len cleared to 0");
    T_EQ(c.flags, 0u, "hi-slba: flags cleared to 0");
    T_EQ(c.rsvd1, 0u, "hi-slba: rsvd1 cleared to 0");
    T_EQ(c.cdw2, 0u, "hi-slba: cdw2 cleared to 0");
    T_EQ(c.cdw3, 0u, "hi-slba: cdw3 cleared to 0");

    /* ---------------------------------------------------------------- *
     * 2. The whole 64-bit split table, boundary by boundary.
     * ---------------------------------------------------------------- */
    struct { uint64_t slba; uint32_t cdw10; uint32_t cdw11; const char *name; } sp[] = {
        { 0ULL,                  0x00000000u, 0x00000000u, "slba=0" },
        { 1ULL,                  0x00000001u, 0x00000000u, "slba=1" },
        { 0xFFFFFFFFULL,         0xFFFFFFFFu, 0x00000000u, "slba=2^32-1" },
        { 0x100000000ULL,        0x00000000u, 0x00000001u, "slba=2^32" },
        { 0x100000001ULL,        0x00000001u, 0x00000001u, "slba=2^32+1" },
        { 0x00000001DEADBEEFULL, 0xDEADBEEFu, 0x00000001u, "slba=0x1DEADBEEF" },
        { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFu, 0xFFFFFFFFu, "slba=2^64-1" },
    };
    for (size_t i = 0; i < sizeof(sp) / sizeof(sp[0]); i++) {
        poison(&c);
        rc = iopath_encode_nvme_write(&c, 7, sp[i].slba, 0, buf, 4096);
        T_EQ(rc, 0, "%s: encode returns 0", sp[i].name);
        T_EQ(c.cdw10, sp[i].cdw10, "%s: cdw10 == 0x%08X", sp[i].name, sp[i].cdw10);
        T_EQ(c.cdw11, sp[i].cdw11, "%s: cdw11 == 0x%08X", sp[i].name, sp[i].cdw11);
        /* The slba must land in cdw10/cdw11 only -- never in the block count. */
        T_EQ(c.cdw12 & 0xFFFFu, 0u,
             "%s: cdw12[15:0] still 0 (1 block; slba did not leak here)", sp[i].name);
        check_common(&c, 7, buf, 4096, sp[i].name);
    }

    /* ---------------------------------------------------------------- *
     * 3. The zero-based block count, spelled out.
     *    blocks -> nlb handed in -> expected cdw12[15:0]
     *    len is kept consistent with a 512-byte logical block throughout, so
     *    data_len == blocks * 512. (The encoder only records the pointer and
     *    the length; it never dereferences buf, so the large cases are safe.)
     * ---------------------------------------------------------------- */
    struct { uint32_t blocks; uint16_t nlb; size_t len; } nl[] = {
        { 1,     0,      512u        },   /* 1 block   -> nlb 0      */
        { 2,     1,      1024u       },   /* 2 blocks  -> nlb 1      */
        { 8,     7,      4096u       },   /* 8 blocks  -> nlb 7      */
        { 128,   127,    65536u      },   /* 128       -> nlb 127    */
        { 4096,  4095,   2097152u    },   /* 4096      -> nlb 4095   */
        { 65536, 0xFFFF, 33554432u   },   /* 65536     -> nlb 0xFFFF, max legal */
    };
    for (size_t i = 0; i < sizeof(nl) / sizeof(nl[0]); i++) {
        poison(&c);
        rc = iopath_encode_nvme_write(&c, 3, 0x2A, nl[i].nlb, buf, nl[i].len);
        T_EQ(rc, 0, "nlb %u: encode returns 0", (unsigned)nl[i].nlb);
        T_EQ(c.cdw12 & 0xFFFFu, (uint32_t)nl[i].nlb,
             "%u blocks -> cdw12[15:0] == %u (zero-based, i.e. blocks-1)",
             nl[i].blocks, (unsigned)nl[i].nlb);
        /* The block count belongs in cdw12 alone. */
        T_EQ(c.cdw10, 0x2Au, "nlb %u: cdw10 still holds slba 0x2A", (unsigned)nl[i].nlb);
        T_EQ(c.cdw11, 0u,    "nlb %u: cdw11 still 0", (unsigned)nl[i].nlb);
        T_EQ(c.data_len, (uint32_t)nl[i].len,
             "%u blocks of 512 bytes -> data_len == %zu", nl[i].blocks, nl[i].len);
    }

    /* Guard against an encoder that "helpfully" subtracts one itself: hand it
     * 7 and it must store 7, not 6. */
    poison(&c);
    rc = iopath_encode_nvme_write(&c, 1, 0, 7, buf, 8 * 4096);
    T_EQ(rc, 0, "nlb passthrough: encode returns 0");
    T_EQ(c.cdw12 & 0xFFFFu, 7u, "nlb passthrough: 7 in -> 7 out (no extra -1)");

    /* ---------------------------------------------------------------- *
     * 4. addr must be the caller's pointer, not a copy or the start of some
     *    internal bounce buffer. Encode from an interior offset and check.
     * ---------------------------------------------------------------- */
    poison(&c);
    rc = iopath_encode_nvme_write(&c, 2, 0x1000, 1, buf + 8192, 8192);
    T_EQ(rc, 0, "interior buffer: encode returns 0");
    check_common(&c, 2, buf + 8192, 8192, "interior buffer");

    /* Two encodes with different buffers must yield different addr values. */
    struct nvme_passthru_cmd c2;
    poison(&c);
    poison(&c2);
    (void)iopath_encode_nvme_write(&c,  1, 0, 0, buf,        4096);
    (void)iopath_encode_nvme_write(&c2, 1, 0, 0, buf + 4096, 4096);
    T_OK(c.addr != c2.addr, "distinct buffers produce distinct addr fields");
    T_OK(c2.addr - c.addr == 4096,
         "addr difference == 4096, the actual byte distance between buffers");

    /* Distinct namespaces are carried through, not hardcoded to 1. */
    poison(&c);
    rc = iopath_encode_nvme_write(&c, 0xFFFFFFFEu, 0, 0, buf, 512);
    T_EQ(rc, 0, "nsid 0xFFFFFFFE: encode returns 0");
    T_EQ(c.nsid, 0xFFFFFFFEu, "nsid 0xFFFFFFFE carried through verbatim");
    T_EQ(c.data_len, 512u, "data_len == 512 for a 512-byte transfer");

    /* ---------------------------------------------------------------- *
     * 5. Refusal paths for the encoder.
     * ---------------------------------------------------------------- */
    rc = iopath_encode_nvme_write(NULL, 1, 0, 0, buf, 4096);
    T_OK(rc < 0, "encode with cmd == NULL is refused (got %d, want < 0)", rc);

    rc = iopath_encode_nvme_write(&c, 1, 0, 0, NULL, 4096);
    T_OK(rc < 0, "encode with buf == NULL is refused (got %d, want < 0)", rc);

    /* ---------------------------------------------------------------- *
     * 6. iopath_open refusals that need neither root nor a device.
     * ---------------------------------------------------------------- */
    struct iopath *p;

    p = iopath_open("/dev/exitos-no-such-device-12345", IOPATH_PWRITE, 4096);
    T_OK(p == NULL, "iopath_open on a nonexistent path returns NULL");
    if (p) iopath_close(p);

    p = iopath_open(NULL, IOPATH_PWRITE, 4096);
    T_OK(p == NULL, "iopath_open with dev_path == NULL returns NULL");
    if (p) iopath_close(p);

    /* A directory is not a device: open(O_RDWR) on it fails with EISDIR. */
    p = iopath_open("/tmp", IOPATH_PWRITE, 4096);
    T_OK(p == NULL, "iopath_open on a directory returns NULL");
    if (p) iopath_close(p);

    /* Closing a NULL handle must be a no-op, not a crash. */
    iopath_close(NULL);
    T_OK(1, "iopath_close(NULL) is a harmless no-op");

    /* ---------------------------------------------------------------- *
     * 7. NULL-handle refusals on the data path. These run last: if the module
     *    dereferences a NULL handle the process dies here, after every result
     *    above has already been printed.
     * ---------------------------------------------------------------- */
    fflush(stdout);
    rc = iopath_write(NULL, 0, buf, 4096);
    T_OK(rc < 0, "iopath_write on a NULL handle is refused (got %d, want < 0)", rc);
    fflush(stdout);
    rc = iopath_read(NULL, 0, buf, 4096);
    T_OK(rc < 0, "iopath_read on a NULL handle is refused (got %d, want < 0)", rc);
    fflush(stdout);
    rc = iopath_flush(NULL);
    T_OK(rc < 0, "iopath_flush on a NULL handle is refused (got %d, want < 0)", rc);

    free(raw);
    T_DONE();
}
