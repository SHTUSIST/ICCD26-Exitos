#!/bin/bash
# THE decisive experiment: filesystem write vs raw LBA write, on the SAME physical
# NVMe, through the SAME loop indirection -- so the loop overhead cancels and the
# difference is purely what VFS + ext4 cost.
# Non-destructive: both loop devices map onto regions verified all-zero first.
set -uo pipefail
DEV=${DEV:-/dev/nvme2n1}
OFF_FS=${OFF_FS:-1677721600000}     # 1600000 MB, sampled zero
OFF_RAW=${OFF_RAW:-1750000000000}   # separate zero region
SZ=$((8*1024*1024*1024))            # 8 GiB each
ITERS=${ITERS:-3000}; IOSZ=${IOSZ:-4096}
cleanup(){ mountpoint -q /mnt/exitos_fs 2>/dev/null && umount /mnt/exitos_fs
  [ -n "${LA:-}" ] && losetup -d "$LA" 2>/dev/null
  [ -n "${LB:-}" ] && losetup -d "$LB" 2>/dev/null; }
trap cleanup EXIT

echo "== verifying both target regions are all-zero before use =="
for o in $OFF_FS $OFF_RAW; do
  mb=$((o/1048576))
  z=$(dd if=/dev/zero bs=1M count=4 status=none|md5sum|cut -d' ' -f1)
  m=$(dd if=$DEV bs=1M skip=$mb count=4 status=none 2>/dev/null|md5sum|cut -d' ' -f1)
  [ "$z" = "$m" ] && echo "  offset ${mb}MB: ZERO ok" || { echo "  offset ${mb}MB: NOT ZERO -> ABORT"; exit 1; }
done

LA=$(losetup --find --show --offset $OFF_FS  --sizelimit $SZ "$DEV")
LB=$(losetup --find --show --offset $OFF_RAW --sizelimit $SZ "$DEV")
echo "== loopA(ext4)=$LA  loopB(raw)=$LB  both backed by $DEV =="
mkfs.ext4 -q -F -b 4096 "$LA"
mkdir -p /mnt/exitos_fs && mount -o data=ordered "$LA" /mnt/exitos_fs

cd "$(dirname "$0")/.."
echo; echo "== A) ext4 on the NVMe (through loopA) =="
EXITOS_SCRATCH=/mnt/exitos_fs ./attribution/minperf $ITERS $IOSZ 2>&1 | grep -E "ext4|syscall|intercept"
echo; echo "== B) raw LBA writes to the same NVMe (through loopB, no filesystem) =="
EXITOS_DEV=$LB EXITOS_PIN=/tmp/loopb.pin EXITOS_CERTIFY=1 \
  EXITOS_LBA_START=0 EXITOS_LBA_COUNT=$((SZ/512)) ./attribution/minperf 0 $IOSZ >/dev/null 2>&1
EXITOS_DEV=$LB EXITOS_PIN=/tmp/loopb.pin EXITOS_LBA_START=0 EXITOS_LBA_COUNT=$((SZ/512)) \
  EXITOS_LBS=512 ./attribution/minperf $ITERS $IOSZ 2>&1 | grep -E "raw blockdev"
