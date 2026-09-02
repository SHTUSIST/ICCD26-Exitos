#!/bin/bash
# Cross-process proof: an UNMODIFIED writer binary, intercepted from outside.
# Controls C1-C4 and failure criteria F1-F4 are labelled inline below.
set -uo pipefail
cd "$(dirname "$0")/../.."; R=$PWD
IMG=/tmp/exitos-cp-$$.img; MNT=/tmp/exitos-cp-mnt-$$; S=/tmp/cp-stats-$$
fail=0; ok(){ echo "ok - $1"; }; bad(){ echo "not ok - $1"; fail=1; }
[ "$(id -u)" -ne 0 ] && { echo "# SKIP - needs root for loop devices"; echo "1..0  (0 failed)"; exit 0; }
cleanup(){ mountpoint -q "$MNT" 2>/dev/null && umount "$MNT"; [ -n "${LOOP:-}" ] && losetup -d "$LOOP" 2>/dev/null; rm -rf "$IMG" "$MNT" "$S".* /tmp/wl-$$ /tmp/wr-$$ /tmp/c4-$$.log; }
trap cleanup EXIT
truncate -s 256M "$IMG"; LOOP=$(losetup --find --show "$IMG")
mkfs.ext4 -q -F -b 4096 "$LOOP"; mkdir -p "$MNT"; mount "$LOOP" "$MNT"
gcc -O2 -D_GNU_SOURCE -o /tmp/wl-$$ tests/integ/writer_libc.c || { bad "build libc writer"; exit 1; }
gcc -O2 -D_GNU_SOURCE -o /tmp/wr-$$ tests/integ/writer_rawsyscall.c || { bad "build raw writer"; exit 1; }
W=/tmp/wl-$$; W2=/tmp/wr-$$; LIB=$R/libexitos_preload.so
fw(){ sed -n 1p "$1" 2>/dev/null || echo 0; }   # EXITOS_STAT_FAST_WRITE
fs(){ sed -n 2p "$1" 2>/dev/null || echo 0; }   # EXITOS_STAT_FAST_SYNC

# C1 reference: no interception at all.
$W "$MNT/c1.log" 64 >/dev/null 2>&1 && ok "C1 writer succeeded" || bad "C1 writer"
REF=$(md5sum "$MNT/c1.log" | cut -d' ' -f1)

# C2 library loaded but disarmed.
LD_PRELOAD=$LIB EXITOS_STATS=$S.c2 $W "$MNT/c2.log" 64 >/dev/null 2>&1 && ok "C2 writer succeeded" || bad "C2 writer"
[ "$(md5sum "$MNT/c2.log"|cut -d' ' -f1)" = "$REF" ] && ok "C2 byte-identical to C1" || bad "C2 differs (F1)"
[ "$(fw $S.c2)" -eq 0 ] 2>/dev/null && ok "C2 fast-path count is 0 (inert)" || bad "C2 intercepted while disarmed (F3)"

# C3 armed, pointed at a filename the writer never uses.
LD_PRELOAD=$LIB EXITOS_FILES=".nomatch" EXITOS_STATS=$S.c3 $W "$MNT/c3.log" 64 >/dev/null 2>&1 \
  && ok "C3 writer succeeded" || bad "C3 writer"
[ "$(md5sum "$MNT/c3.log"|cut -d' ' -f1)" = "$REF" ] && ok "C3 byte-identical to C1" || bad "C3 differs (F1)"
[ "$(fw $S.c3)" -eq 0 ] 2>/dev/null && ok "C3 declined a non-matching filename" || bad "C3 intercepted the wrong file (F3)"

# ARMED: the case under test. The paper's scenario is a log file that is ALREADY
# preallocated when it is opened, so create and initialise it first, without
# interception. Skipping this would test a different thing: a file created and
# then preallocated has no extents at open() and, until a normal write converts
# them, only UNWRITTEN ones — which src/extent.c refuses to map on purpose,
# because ext4 reads such a range back as zeros and a raw write there would be
# invisible. The fast path is then correctly declined and the run proves nothing.
$W "$MNT/armed.log" 64 >/dev/null 2>&1 || bad "armed-file preparation"
LD_PRELOAD=$LIB EXITOS_FILES=".log" EXITOS_STATS=$S.on $W "$MNT/armed.log" 64 >/dev/null 2>&1 \
  && ok "ARMED writer succeeded" || bad "ARMED writer"
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
[ "$(md5sum "$MNT/armed.log"|cut -d' ' -f1)" = "$REF" ] && ok "ARMED byte-identical to C1 (LBA math correct)" || bad "ARMED corrupted data (F1)"
[ "$(fw $S.on)" -gt 0 ] 2>/dev/null && ok "ARMED took the fast path $(fw $S.on) times" || bad "ARMED never intercepted (F2)"
[ "$(fs $S.on)" -gt 0 ] 2>/dev/null && ok "ARMED took over fdatasync $(fs $S.on) times" || bad "fdatasync still went to the kernel (F4)"

# C4 unsupported device: tmpfs has no block device behind it.
LD_PRELOAD=$LIB EXITOS_FILES=".log" EXITOS_STATS=$S.c4 $W /tmp/c4-$$.log 8 >/dev/null 2>&1 \
  && ok "C4 write on tmpfs still succeeded" || bad "C4 write failed on unsupported device"
[ "$(fw $S.c4)" -eq 0 ] 2>/dev/null && ok "C4 declined an unsupported device" || bad "C4 used the fast path on tmpfs (F3)"

# H2: LD_PRELOAD must NOT see a writer that bypasses libc.
$W2 "$MNT/raw.log" 64 >/dev/null 2>&1 || bad "raw-file preparation"
LD_PRELOAD=$LIB EXITOS_FILES=".log" EXITOS_STATS=$S.raw $W2 "$MNT/raw.log" 64 >/dev/null 2>&1 \
  && ok "raw-syscall writer succeeded" || bad "raw-syscall writer"
[ "$(fw $S.raw)" -eq 0 ] 2>/dev/null \
  && ok "H2 confirmed: LD_PRELOAD does NOT catch raw-syscall writes" \
  || bad "unexpected: LD_PRELOAD caught a raw syscall (H2 wrong)"
[ "$(md5sum "$MNT/raw.log"|cut -d' ' -f1)" = "$REF" ] && ok "raw writer output still correct via kernel path" || bad "raw writer output wrong"
[ $fail -eq 0 ] && echo "1..16  (0 failed)" || echo "1..16  (FAILURES)"
exit $fail
