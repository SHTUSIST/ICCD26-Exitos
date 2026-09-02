#!/bin/bash
# Loop-device fixture: an ext4 image on a file. Everything the paper needs
# (FIEMAP, move-extent, raw LBA writes) is exercised here with zero risk to
# any real disk. Never touches a physical device.
set -euo pipefail
IMG="${EXITOS_IMG:-/tmp/exitos-test.img}"; MNT="${EXITOS_MNT:-/tmp/exitos-mnt}"
SIZE_MB="${EXITOS_SIZE_MB:-512}"
case "${1:-}" in
  up)
    [ -e "$IMG" ] && { echo "refusing: $IMG exists" >&2; exit 1; }
    truncate -s "${SIZE_MB}M" "$IMG"
    LOOP=$(losetup --find --show "$IMG")
    mkfs.ext4 -q -F -b 4096 "$LOOP"
    mkdir -p "$MNT"; mount -o data=ordered "$LOOP" "$MNT"
    echo "$LOOP" > /tmp/exitos-loop.dev
    echo "LOOP=$LOOP MNT=$MNT" ;;
  down)
    LOOP=$(cat /tmp/exitos-loop.dev 2>/dev/null || true)
    mountpoint -q "$MNT" && umount "$MNT" || true
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
    rm -f "$IMG" /tmp/exitos-loop.dev; echo "torn down" ;;
  *) echo "usage: $0 {up|down}" >&2; exit 2 ;;
esac
