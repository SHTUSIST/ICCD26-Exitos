#!/bin/bash
# Disk safety helpers. Every one of these exists because skipping it caused
# real damage during development; none is optional before a raw-LBA write.
#
#   map          which kernel device name currently holds which SERIAL.
#                Kernel NVMe names are assigned in probe order and CHANGE ACROSS
#                REBOOTS. A reboot once moved the spare disk from nvme3n1 to
#                nvme2n1 while the ROOT filesystem took over the name nvme3n1;
#                writing to the remembered name would have destroyed it.
#   scan  DEV MB COUNT   verify a window really is all zero, block by block.
#                A window certified earlier is not certified now: a benchmark,
#                another user, or a reboot may have written to it since.
#   holders DEV  filesystem signatures, partitions, mounts, holders, dm/md/LVM.
#   watch DEV    is anything writing to it right now (diskstats delta).
#
# Rules these enforce, learned the hard way:
#   1. Identify a disk by serial, never by device name.
#   2. Refuse anything partitioned or mounted.
#   3. Re-verify immediately before writing, not at planning time.
#   4. NVMe passthru LBAs are namespace-absolute and are NOT clamped to a
#      partition: passing a partition-relative LBA to a passthru command on a
#      partition node writes elsewhere on the disk. That is how a GPT was once
#      overwritten by a benchmark's own fill byte.
set -uo pipefail
usage(){ sed -n '2,25p' "$0"; exit 2; }
cmd="${1:-}"; shift 2>/dev/null || true

case "$cmd" in
map)
  printf "%-14s %-24s %-10s %s\n" DEVICE SERIAL SIZE MOUNTS
  for d in /dev/nvme[0-9]n1 /dev/sd[a-z]; do
    [ -b "$d" ] || continue
    n=$(basename "$d")
    sn=$(nvme id-ctrl "$d" 2>/dev/null | awk -F: '/^sn /{gsub(/ /,"",$2);print $2}')
    [ -z "$sn" ] && sn=$(cat "/sys/class/block/$n/device/serial" 2>/dev/null || echo "?")
    sz=$(lsblk -bdno SIZE "$d" 2>/dev/null | numfmt --to=iec 2>/dev/null || echo "?")
    mp=$(lsblk -no MOUNTPOINT "$d" 2>/dev/null | tr -s '\n' ' ')
    printf "%-14s %-24s %-10s %s\n" "$d" "$sn" "$sz" "${mp:- (none)}"
  done ;;
scan)
  dev="${1:?device}"; start="${2:?start MB}"; count="${3:?count MB}"
  Z=$(dd if=/dev/zero bs=1M count=8 status=none | md5sum | cut -d' ' -f1)
  bad=0
  for ((o=start; o<start+count; o+=8)); do
    m=$(dd if="$dev" bs=1M skip=$o count=8 status=none 2>/dev/null | md5sum | cut -d' ' -f1)
    [ "$m" != "$Z" ] && { echo "NON-ZERO at ${o}MB"; bad=1; break; }
  done
  [ $bad -eq 0 ] && echo "VERIFIED all zero: ${start}MB..$((start+count))MB on $dev" || exit 1 ;;
holders)
  dev="${1:?device}"; n=$(basename "$dev")
  echo "signatures:"; blkid "$dev" 2>&1 | sed 's/^/  /'
  echo "wipefs (read-only):"; wipefs -n "$dev" 2>&1 | sed 's/^/  /'
  echo "layout:"; lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT "$dev" | sed 's/^/  /'
  echo "holders: $(ls "/sys/block/$n/holders/" 2>/dev/null | tr '\n' ' ' || echo none)"
  echo "open by: $(lsof "$dev" 2>/dev/null | tail -n +2 | wc -l) process(es)"
  echo "device-mapper: $(dmsetup ls 2>/dev/null | head -3 || echo none)"
  echo "md raid: $(grep -c md /proc/mdstat 2>/dev/null || echo 0) line(s)" ;;
watch)
  dev="${1:?device}"; n=$(basename "$dev")
  a=$(grep -E " $n " /proc/diskstats | awk '{print $10}')
  sleep "${2:-8}"
  b=$(grep -E " $n " /proc/diskstats | awk '{print $10}')
  echo "sectors written over ${2:-8}s: $((b-a))"
  [ "$a" = "$b" ] && echo "=> no write activity" || echo "=> SOMETHING IS WRITING" ;;
*) usage ;;
esac
