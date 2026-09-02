/* TDD red phase: FUA support in the library, and fdatasync takeover.
 * Rationale: a separate NVMe FLUSH is an extra command submitted after the
 * write, while the FUA bit rides on the write command itself.
 * So durability should ride on the write command, and once writes are FUA the
 * following fdatasync has nothing left to do and can be answered immediately. */
#include "tap.h"
#include <linux/nvme_ioctl.h>
#include <string.h>
#include <stdint.h>
#include "exitos_iopath.h"

int main(void)
{
    struct nvme_passthru_cmd c;
    char buf[4096] = {0};

    /* Without FUA the bit must be clear. */
    memset(&c, 0xEE, sizeof c);
    T_EQ(iopath_encode_nvme_write(&c, 1, 0x1000, 7, buf, sizeof buf), 0,
         "encode plain write returns 0");
    T_EQ(c.opcode, 0x01, "opcode is NVMe WRITE");
    T_EQ((c.cdw12 >> 30) & 1, 0, "FUA bit clear on a plain write");
    T_EQ(c.cdw12 & 0xFFFF, 7, "NLB preserved in cdw12 low bits");

    /* With FUA the bit must be set and NLB must survive untouched. */
    memset(&c, 0xEE, sizeof c);
    T_EQ(iopath_encode_nvme_write_fua(&c, 1, 0x1000, 7, buf, sizeof buf, 1), 0,
         "encode FUA write returns 0");
    T_EQ((c.cdw12 >> 30) & 1, 1, "FUA bit SET when requested");
    T_EQ(c.cdw12 & 0xFFFF, 7, "NLB still correct with FUA set");
    T_EQ(c.opcode, 0x01, "still a WRITE opcode with FUA");

    /* 64-bit LBA split must be unaffected by the FUA flag. */
    memset(&c, 0, sizeof c);
    T_EQ(iopath_encode_nvme_write_fua(&c, 1, 0x1234567890ULL, 0, buf, sizeof buf, 1), 0,
         "encode FUA write with a >32-bit LBA");
    T_EQ(c.cdw10, 0x34567890u, "cdw10 holds low 32 bits of LBA");
    T_EQ(c.cdw11, 0x12u,       "cdw11 holds high 32 bits of LBA");
    T_EQ((c.cdw12 >> 30) & 1, 1, "FUA still set with a large LBA");

    T_DONE();
}
