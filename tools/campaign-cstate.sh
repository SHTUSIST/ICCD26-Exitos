#!/bin/bash
# One pass over every question left open about why an 8 KiB passthru write was
# slower than the same write through the filesystem.
#
# Runs the arms four ways -- two buffer layouts crossed with two idle-state
# settings -- because the two candidate causes predict different patterns:
#
#   If the cause is the kernel's work describing a non-contiguous buffer, the
#   extra time sits before the doorbell and holding the core awake changes
#   nothing.
#
#   If the cause is the core sleeping deeper while the device works, the extra
#   time sits after the doorbell and disappears when the deep states are off.
#
# The completion interrupt for the queue in use is pinned to the same core that
# submits, so that core's wake-up is on the critical path either way.
#
# Everything here writes only inside one file on the test partition, identified
# by serial. Idle-state changes are confined to one core and restored on exit.
set -u
CPU=170
SN=EXAMPLESERIAL0001
ITERS=${ITERS:-1000}
cd "$(dirname "$0")/.."

DISK=""
for d in /sys/class/block/nvme*n[0-9]; do
    b=$(basename "$d"); [ -e "$d/partition" ] && continue
    [ "$(tr -d ' ' < "$d/device/serial" 2>/dev/null)" = "$SN" ] && DISK=$b && break
done
[ -n "$DISK" ] || { echo "test disk with serial $SN not found" >&2; exit 1; }
echo "test disk: /dev/$DISK (serial $SN)"

mountpoint -q /mnt/exitos_fs || mount "/dev/${DISK}p1" /mnt/exitos_fs
export EXITOS_DEV=/dev/$DISK EXITOS_CHARDEV=/dev/ng${DISK#nvme}
export EXITOS_EXPECT_SERIAL=$SN EXITOS_SAMEFILE=1
export EXITOS_FSPART=${DISK}p1 EXITOS_WINDOW_IN_PART=${DISK}p1
export EXITOS_LBA_START=0 EXITOS_LBA_COUNT=0

run() {   # run <label> <extra-env-assignments...>
    local label="$1"; shift
    export EXITOS_FSFILE=/mnt/exitos_fs/c_$(echo "$label" | tr -c 'a-zA-Z0-9' _).dat
    rm -f "$EXITOS_FSFILE"* ; sync ; sleep 5
    echo "--- $label ---"
    env "$@" taskset -c $CPU ./attribution/qdsplit "$ITERS" 2>&1 \
        | grep -E "^(fs-|pt-)|^  C[0-9]|^idle states|^buffer pages|^polled" | grep -v FAILED
    rm -f "$EXITOS_FSFILE"*
}

echo "===== 0. Noise floor: two identical arms ====="
for rep in 1 2; do run "noise-floor-normal-$rep" EXITOS_PAIRED=aa; run "noise-floor-huge-$rep" EXITOS_PAIRED=aa EXITOS_HUGEBUF=1; done

echo "===== 1. Buffer contiguity: re-measure that finding, two repetitions of each buffer ====="
for rep in 1 2; do
  run "normal-buffer-$rep" EXITOS_PAIRED=8kall
  run "huge-buffer-$rep" EXITOS_PAIRED=8kall EXITOS_HUGEBUF=1
done

echo "===== 2. The same group, but with that core's deep idle states turned off ====="
for rep in 1 2; do
  bash tools/cstate-pin.sh $CPU 2 -- env EXITOS_PAIRED=8kall \
      EXITOS_FSFILE=/mnt/exitos_fs/cs_n_$rep.dat taskset -c $CPU ./attribution/qdsplit "$ITERS" 2>&1 \
      | grep -E "^(fs-|pt-)|^  C[0-9]|^idle states|keeping the" | grep -v FAILED
  echo "  ^^ normal buffer, deep states off, repetition $rep"
  rm -f /mnt/exitos_fs/cs_n_$rep.dat*; sync; sleep 5
  bash tools/cstate-pin.sh $CPU 2 -- env EXITOS_PAIRED=8kall EXITOS_HUGEBUF=1 \
      EXITOS_FSFILE=/mnt/exitos_fs/cs_h_$rep.dat taskset -c $CPU ./attribution/qdsplit "$ITERS" 2>&1 \
      | grep -E "^(fs-|pt-)|^  C[0-9]|^idle states" | grep -v FAILED
  echo "  ^^ huge buffer, deep states off, repetition $rep"
  rm -f /mnt/exitos_fs/cs_h_$rep.dat*; sync; sleep 5
done

echo "===== 3. Five-segment timing: separate the kernel work before the doorbell from the time after it ====="
for buf in normal huge; do
  for arm in 0 1; do
    case $arm in 0) nm="pwrite to the file";; 1) nm="io_uring passthru";; esac
    export EXITOS_FSFILE=/mnt/exitos_fs/f5_${buf}_$arm.dat EXITOS_ONLY=$arm EXITOS_PAUSE=8
    [ $buf = huge ] && export EXITOS_HUGEBUF=1 || unset EXITOS_HUGEBUF
    rm -f "$EXITOS_FSFILE"*; sync; sleep 4
    ( taskset -c $CPU ./attribution/qdsplit "$ITERS" > /tmp/f5_${buf}_$arm.txt 2>&1 ) & APP=$!
    while ! grep -q READY /tmp/f5_${buf}_$arm.txt 2>/dev/null; do sleep 0.3; done
    bpftrace tools/fiveway.bt $CPU > /tmp/bt5_${buf}_$arm.txt 2>/dev/null & BT=$!
    wait $APP; sleep 1; kill -INT $BT 2>/dev/null; wait $BT 2>/dev/null
    echo "--- $nm, $buf buffer ---"
    grep -E "^(fs-|pt-)|^  C[0-9]|^idle states" /tmp/f5_${buf}_$arm.txt | grep -v FAILED
    for sec in a_enter_to_setup b1a_prepare_doorbell b1b_device_and_wake b2_deliver c_return; do
      printf "  %-24s " "$sec:"
      sed -n "/@${sec}/,/^$/p" /tmp/bt5_${buf}_$arm.txt | grep -E "^\[|^\(" \
        | awk '$3>60 {printf "%s%s=%s  ", $1, $2, $3}'
      echo
    done
    rm -f "$EXITOS_FSFILE"*
  done
done
unset EXITOS_ONLY EXITOS_PAUSE

echo "===== 4. O_DIRECT+fdatasync compared: preallocated versus not preallocated ====="
for rep in 1 2; do
  run "WAL-compare-$rep" EXITOS_PAIRED=wal EXITOS_HUGEBUF=1
done

umount /mnt/exitos_fs 2>/dev/null
echo "===== Done, partition unmounted ====="

echo "===== 5. End to end: the unmodified program, baseline versus hijack ====="
# The same binary both ways: the difference cannot come from a different program.
mountpoint -q /mnt/exitos_fs || mount "/dev/${DISK}p1" /mnt/exitos_fs
for mode in prealloc append; do
  [ $mode = prealloc ] && PRE=1 || PRE=
  for arm in baseline hijack-nvme hijack-poll; do
    f=/mnt/exitos_fs/wal_${mode}_${arm}.dat
    rm -f "$f"; sync; sleep 5
    case $arm in
      baseline)     PRELOAD= ; IOP= ;;
      hijack-nvme)  PRELOAD=./libexitos_preload.so ; IOP=nvme ;;
      hijack-poll)  PRELOAD=./libexitos_preload.so ; IOP=uringpoll ;;
    esac
    printf "  %-10s %-12s " "$mode" "$arm"
    env ${PRE:+WAL_PREALLOC=1} WAL_DIRECT=1 \
        ${PRELOAD:+LD_PRELOAD=$PRELOAD} ${PRELOAD:+EXITOS_FILES=wal_} \
        ${IOP:+EXITOS_IOPATH=$IOP} ${PRELOAD:+EXITOS_STATS=1} \
        taskset -c $CPU ./attribution/walwriter "$f" "$ITERS" 8192 2>&1 \
        | grep -E "median|takeover|ARMED|inert" | tr '\n' ' '
    echo
    rm -f "$f"
  done
done
umount /mnt/exitos_fs 2>/dev/null
echo "===== End to end done ====="
