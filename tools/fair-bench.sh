#!/bin/bash
# A latency comparison built to survive the ways this kind of measurement lies.
#
# Every control here exists because leaving it out produced a wrong answer at
# least once in this project:
#
#  * SEPARATE REGION PER ARM. Arms used to share one LBA window, so whichever
#    arm ran second wrote blocks the first arm had just made hot in the drive's
#    mapping. Each arm now gets its own disjoint sub-window, and the filesystem
#    arm gets a freshly created file each repetition.
#  * SETTLE TIME. A drive that has just absorbed a burst of writes keeps working
#    after the writes stop, and a measurement started immediately afterwards
#    reads that recovery as latency. Every arm is followed by a sync and a
#    pause. Mount and unmount count as bursts too.
#  * NO PAGE CACHE. Every arm is O_DIRECT; the filesystem arm's file is created
#    and fully written before timing, so no timing sample is the first write to
#    an unwritten extent.
#  * WARM-UP DISCARDED. The first pass over each arm's window is not timed.
#  * ROTATED ORDER. Running the arms in a fixed order makes the ordering part of
#    the result, and repeating it makes the bias look like consistency.
#  * PINNED. One core, so the scheduler is not part of the difference.
#  * A/A CONTROL. One arm measured against itself, to show the noise floor the
#    real differences have to beat.
#
# Usage:
#   EXITOS_DEV=/dev/nvmeXnY EXITOS_EXPECT_SERIAL=... EXITOS_WINDOW_IN_PART=... \
#   EXITOS_LBA_START=... EXITOS_LBA_COUNT=... EXITOS_PIN=... \
#   tools/fair-bench.sh <ext4-partition> <iosize-bytes> [reps] [settle-seconds]
set -u
PART=${1:?usage: fair-bench.sh <ext4-partition> <iosize> [reps] [settle]}
IOSZ=${2:-4096}
REPS=${3:-4}
SETTLE=${4:-5}
CORE=${EXITOS_CORE:-$(( $(nproc) - 2 ))}
REPO=$(cd "$(dirname "$0")/.." && pwd)
MNT=${EXITOS_MNT:-/mnt/exitos-fair}
: "${EXITOS_DEV:?set EXITOS_DEV}"; : "${EXITOS_EXPECT_SERIAL:?set EXITOS_EXPECT_SERIAL}"
: "${EXITOS_WINDOW_IN_PART:?set EXITOS_WINDOW_IN_PART}"; : "${EXITOS_LBA_START:?set EXITOS_LBA_START}"
: "${EXITOS_LBA_COUNT:?set EXITOS_LBA_COUNT}"; : "${EXITOS_PIN:?set EXITOS_PIN}"

# Three disjoint sub-windows inside the certified window, one per raw arm, so no
# arm writes blocks another arm just wrote.
SUB=$(( EXITOS_LBA_COUNT / 4 ))
W_RAW=$EXITOS_LBA_START
W_NVME=$(( EXITOS_LBA_START + SUB ))
W_AA=$(( EXITOS_LBA_START + 2 * SUB ))

settle() { sync; sleep "$SETTLE"; }

p50() { grep -F "$2" <<< "$1" | grep -oP 'p50=\s*\K[0-9.]+'; }

common() {
  echo "EXITOS_DEV=$EXITOS_DEV EXITOS_EXPECT_SERIAL=$EXITOS_EXPECT_SERIAL"
}

arm_fs() {   # fresh file every repetition, fully written before timing
  mkdir -p "$MNT"; mount -o noatime "$PART" "$MNT" >/dev/null 2>&1 || { echo ERR; return; }
  local f="$MNT/fair-$RANDOM-$$.dat"
  dd if=/dev/zero of="$f" bs=1M count=64 oflag=direct conv=fsync status=none
  settle
  local o; o=$(taskset -c "$CORE" env EXITOS_ONLY=${1:-fsnf} EXITOS_SCRATCH="$MNT" \
                 "$REPO/attribution/minperf" 2000 "$IOSZ" 2>/dev/null)
  rm -f "$f"; umount "$MNT"; rmdir "$MNT" 2>/dev/null
  settle
  p50 "$o" "$2"
}

arm_raw() {  # its own sub-window
  local o; o=$(taskset -c "$CORE" env EXITOS_ONLY=rawnf EXITOS_LBA_START=$1 \
                 EXITOS_LBA_COUNT=$SUB EXITOS_REUSE_WINDOW=1 \
                 "$REPO/attribution/minperf" 2000 "$IOSZ" 2>/dev/null)
  settle
  p50 "$o" "raw blockdev pwrite (NO"
}

arm_nvme() {
  local o; o=$(taskset -c "$CORE" env EXITOS_ONLY=fua EXITOS_LBA_START=$1 \
                 EXITOS_LBA_COUNT=$SUB EXITOS_REUSE_WINDOW=1 \
                 "$REPO/attribution/minperf" 2000 "$IOSZ" 2>/dev/null)
  settle
  p50 "$o" "passthru write (FUA bit)"
}

echo "io=${IOSZ}B reps=$REPS settle=${SETTLE}s core=$CORE"
echo "sub-windows: raw=$W_RAW nvme=$W_NVME aa=$W_AA size=$SUB sectors each"
printf '%-5s %-12s %-12s %-12s %-12s\n' rep ext4+fdsync ext4 blockdev nvme-passthru

# untimed warm-up over every arm
arm_raw $W_RAW >/dev/null; arm_nvme $W_NVME >/dev/null; arm_fs fsnf "ext4 O_DIRECT write, NO" >/dev/null

for r in $(seq 1 "$REPS"); do
  case $(( r % 3 )) in
    0) a=$(arm_fs fs "ext4 O_DIRECT write+fdatasync"); b=$(arm_fs fsnf "ext4 O_DIRECT write, NO")
       c=$(arm_raw $W_RAW); d=$(arm_nvme $W_NVME) ;;
    1) c=$(arm_raw $W_RAW); d=$(arm_nvme $W_NVME)
       a=$(arm_fs fs "ext4 O_DIRECT write+fdatasync"); b=$(arm_fs fsnf "ext4 O_DIRECT write, NO") ;;
    2) d=$(arm_nvme $W_NVME); a=$(arm_fs fs "ext4 O_DIRECT write+fdatasync")
       c=$(arm_raw $W_RAW); b=$(arm_fs fsnf "ext4 O_DIRECT write, NO") ;;
  esac
  printf '%-5s %-12s %-12s %-12s %-12s\n' "$r" "$a" "$b" "$c" "$d"
done

x=$(arm_nvme $W_AA); y=$(arm_nvme $W_AA)
echo "A/A control (same arm, own window, twice): $x $y"
