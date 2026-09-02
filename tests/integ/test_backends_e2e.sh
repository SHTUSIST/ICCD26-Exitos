#!/bin/bash
# End-to-end: the active LD_PRELOAD frontend and the deliberately frozen
# bpftime DSO against unmodified libc and raw-syscall writers.
#
# Correctness is checked the same way for every arm: the file must come out
# byte-identical to the same writer running with no interception at all. And the
# counters must be consulted: backend 1 should accelerate only the libc writer,
# while frozen backend 2 must accelerate neither writer.
set -uo pipefail
cd "$(dirname "$0")/../.."; R=$PWD
IMG=/tmp/e2e-$$.img; MNT=/tmp/e2e-mnt-$$; S=/tmp/e2e-stats-$$
fail=0; ok(){ echo "ok - $1"; }; bad(){ echo "not ok - $1"; fail=1; }
if [ "${EXITOS_BPFTIME_CONTRACT_ONLY:-0}" = 1 ]; then
  probe=/tmp/bpf-freeze-$$
  trap 'rm -f "$probe"' EXIT HUP INT TERM
  gcc -O2 -Wall -Wextra -Werror -o "$probe" \
      tests/integ/bpftime_frozen_probe.c -ldl || exit 1
  if "$probe" "$R/libexitos_bpftime.so" >/dev/null; then
    echo "ok 1 - backend2 production DSO claims nothing and start is -EOPNOTSUPP"
    echo "1..1  (0 failed)"
    exit 0
  fi
  echo "not ok 1 - backend2 production DSO freeze contract"
  echo "1..1  (1 failed)"
  exit 1
fi
[ "$(id -u)" -ne 0 ] && { echo "# SKIP - needs root for its owned loop-device fixture"; echo "1..0  (0 failed)"; exit 0; }
cleanup(){ mountpoint -q "$MNT" 2>/dev/null && umount "$MNT"; [ -n "${LOOP:-}" ] && losetup -d "$LOOP" 2>/dev/null; rm -rf "$IMG" "$MNT" "$S".* /tmp/wl-$$ /tmp/wr-$$ /tmp/bpf-freeze-$$; }
trap cleanup EXIT
truncate -s 256M "$IMG"; LOOP=$(losetup --find --show "$IMG")
mkfs.ext4 -q -F -b 4096 "$LOOP"; mkdir -p "$MNT"; mount "$LOOP" "$MNT"
gcc -O2 -D_GNU_SOURCE -o /tmp/wl-$$ tests/integ/writer_libc.c       || { bad "build libc writer"; exit 1; }
gcc -O2 -D_GNU_SOURCE -o /tmp/wr-$$ tests/integ/writer_rawsyscall.c || { bad "build raw writer"; exit 1; }
gcc -O2 -Wall -Wextra -Werror -o /tmp/bpf-freeze-$$ \
    tests/integ/bpftime_frozen_probe.c -ldl || { bad "build bpftime freeze probe"; exit 1; }
WL=/tmp/wl-$$; WR=/tmp/wr-$$
P1=$R/libexitos_preload.so; P2=$R/libexitos_bpftime.so
fw(){ if [ -f "$1" ]; then sed -n 1p "$1"; else echo MISSING; fi; }

/tmp/bpf-freeze-$$ "$P2" >/dev/null \
  && ok "backend2 production DSO claims nothing and start is -EOPNOTSUPP" \
  || bad "backend2 production DSO freeze contract"

# Reference: no interception. Both writers must agree with each other too.
$WL "$MNT/ref_libc.log" 64 >/dev/null 2>&1 && ok "reference libc writer ran" || bad "reference libc writer"
$WR "$MNT/ref_raw.log"  64 >/dev/null 2>&1 && ok "reference raw writer ran"  || bad "reference raw writer"
REF=$(md5sum "$MNT/ref_libc.log" | cut -d' ' -f1)
[ "$(md5sum "$MNT/ref_raw.log"|cut -d' ' -f1)" = "$REF" ] && ok "both writers produce the same bytes uninterceped" || bad "writers disagree — the control is broken"

run(){ # $1=lib $2=writer $3=tag ; prepares the file first so extents are initialised
  local lib=$1
  local w=$2
  local tag=$3
  local f="$MNT/$tag.log"
  $w "$f" 64 >/dev/null 2>&1
  LD_PRELOAD=$lib EXITOS_FILES=".log" EXITOS_BPFTIME_SO=$P2 EXITOS_STATS=$S.$tag \
    $w "$f" 64 >/dev/null 2>&1
  sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
  echo "$(md5sum "$f"|cut -d' ' -f1) $(fw $S.$tag)"
}

# --- backend 1 (symbol interposition) ---
read -r m1 c1 <<<"$(run "$P1" "$WL" b1libc)"
[ "$m1" = "$REF" ] && ok "backend1 + libc writer: bytes identical" || bad "backend1 + libc writer: DATA DIFFERS"
[ "$c1" != MISSING ] && [ "$c1" -gt 0 ] 2>/dev/null && ok "backend1 caught the libc writer ($c1 fast writes)" || bad "backend1 missed the libc writer"

read -r m2 c2 <<<"$(run "$P1" "$WR" b1raw)"
[ "$m2" = "$REF" ] && ok "backend1 + raw writer: bytes identical" || bad "backend1 + raw writer: DATA DIFFERS"
case "$c2" in
  MISSING) bad "backend1 + raw writer: no stats file — the run did not complete" ;;
  0) ok "backend1 MISSED the raw-syscall writer, as predicted (H2)" ;;
  *) bad "backend1 unexpectedly caught a raw syscall ($c2) — H2 is wrong" ;;
esac

# --- backend 2 (hard-frozen; kernel path only) ---
read -r m3 c3 <<<"$(run "$P2" "$WL" b2libc)"
[ "$m3" = "$REF" ] && ok "backend2 + libc writer: bytes identical" || bad "backend2 + libc writer: DATA DIFFERS"
[ "$c3" = 0 ] 2>/dev/null && ok "frozen backend2 takes over zero libc writes" \
                         || bad "frozen backend2 reported libc takeovers ($c3)"

read -r m4 c4 <<<"$(run "$P2" "$WR" b2raw)"
[ "$m4" = "$REF" ] && ok "backend2 + raw writer: bytes identical" || bad "backend2 + raw writer: DATA DIFFERS"
[ "$c4" = 0 ] 2>/dev/null && ok "frozen backend2 takes over zero raw-syscall writes" \
                         || bad "frozen backend2 reported raw-syscall takeovers ($c4)"

[ $fail -eq 0 ] && echo "1..12  (0 failed)" || echo "1..12  (FAILURES)"
exit $fail
