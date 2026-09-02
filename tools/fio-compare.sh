#!/bin/bash
# Run an UNMODIFIED fio under interception and against nothing, on the same file
# and the same filesystem, and print both results side by side.
#
# The point of this script is that fio is not modified, not recompiled, and not
# even aware of the interception: whatever difference appears is what a real
# application would get. It also prints the interception counters, because a run
# that intercepted nothing and a run that helped nothing look the same from the
# IOPS alone -- that mistake was made once, and the comparison it produced was
# meaningless.
#
# Three write patterns matter and they behave completely differently:
#   overwrite  a preallocated, already-written file (no metadata change)
#   unwritten  space that was fallocated and never written (extent conversion)
#   append     a file that grows (allocation + i_size + journal commit)
#
# Usage:
#   EXITOS_DEV=/dev/nvmeXnY tools/fio-compare.sh <mountpoint> [pattern] [size] [bs]
#
# EXITOS_DEV is required by the module itself: raw writes to a real device are
# refused unless the operator names the device.
set -u
MNT=${1:?usage: fio-compare.sh <mountpoint> [overwrite|unwritten|append] [size] [bs]}
PAT=${2:-overwrite}
SZ=${3:-128M}
BS=${4:-4k}
REPO=$(cd "$(dirname "$0")/.." && pwd)
SO=$REPO/libexitos_preload.so
: "${EXITOS_DEV:?set EXITOS_DEV=/dev/... : the module refuses raw writes to an unnamed device}"
command -v fio >/dev/null || { echo "fio is not installed"; exit 2; }
[ -r "$SO" ] || { echo "build the library first: make libexitos_preload.so"; exit 2; }

F=$MNT/fio-compare.dat
JOB="--name=w --bs=$BS --rw=write --ioengine=psync --direct=1 --fdatasync=1 \
     --numjobs=1 --thread --loops=1 --group_reporting --output-format=json \
     --size=$SZ --fallocate=none"

prepare() {
  rm -f "$F"
  case $PAT in
    overwrite) dd if=/dev/zero of="$F" bs=1M count=$(( ${SZ%M} )) oflag=direct conv=fsync status=none ;;
    unwritten) fallocate -l "$SZ" "$F" ;;
    append)    : ;;   # created by fio itself, grows as it writes
    *) echo "unknown pattern: $PAT"; exit 2 ;;
  esac
}

report() {
  python3 - "$1" <<'PY'
import json,sys
d=json.load(open(sys.argv[1])); j=d["jobs"][0]["write"]; s=d["jobs"][0].get("sync",{})
p=j["clat_ns"]["percentile"]
line="IOPS=%-8.0f BW=%-8.1fMB/s write p50=%7.2fus p99=%8.2fus" % (
    j["iops"], j["bw"]/1024.0, p["50.000000"]/1000.0, p["99.000000"]/1000.0)
if s.get("lat_ns",{}).get("percentile"):
    line += "  fdatasync p50=%8.2fus" % (s["lat_ns"]["percentile"]["50.000000"]/1000.0)
print(line)
PY
}

echo "pattern=$PAT size=$SZ bs=$BS mount=$MNT device=$EXITOS_DEV order=${EXITOS_ORDER:-plain-first}"

EXTRA=""; [ "$PAT" = append ] && EXTRA="--create_on_open=1"

run_plain() {
  prepare
  fio $JOB --filename="$F" $EXTRA >/tmp/fio-plain.$$ 2>/dev/null
}
run_exitos() {
  prepare
  LD_PRELOAD=$SO EXITOS_FILES=fio-compare.dat EXITOS_DEV=$EXITOS_DEV \
    EXITOS_IOPATH=${EXITOS_IOPATH:-nvme} EXITOS_STATS=/tmp/fio-stats.$$ \
    fio $JOB --filename="$F" $EXTRA >/tmp/fio-exitos.$$ 2>/dev/null
}

# Which arm runs first is aliased with the arm unless it is varied. Running the
# plain arm first in every repetition produces the same run-after-run
# consistency that a real effect does, so a sign test over such repetitions
# proves nothing about the effect. Set EXITOS_ORDER=exitos-first on half the
# repetitions.
if [ "${EXITOS_ORDER:-plain-first}" = exitos-first ]; then
  run_exitos; run_plain
else
  run_plain; run_exitos
fi

printf "  plain fio       "; report /tmp/fio-plain.$$
printf "  fio + exitos    "; report /tmp/fio-exitos.$$
printf "  counters (fast_write fast_sync pass declined refresh): %s\n" \
       "$(tr '\n' ' ' < /tmp/fio-stats.$$ 2>/dev/null)"

rm -f "$F" /tmp/fio-plain.$$ /tmp/fio-exitos.$$ /tmp/fio-stats.$$
