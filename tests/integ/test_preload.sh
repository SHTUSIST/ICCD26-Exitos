#!/bin/bash
# End-to-end proof: data written through the interception layer must be readable
# and identical when read back through the normal filesystem path.
set -uo pipefail
cd "$(dirname "$0")/../.."
R=$PWD; IMG=/tmp/exitos-preload-$$.img; MNT=/tmp/exitos-preload-mnt-$$
fail=0; ok(){ echo "ok - $1"; }; bad(){ echo "not ok - $1"; fail=1; }
[ "$(id -u)" -ne 0 ] && { echo "# SKIP - needs root for loop devices"; exit 0; }
cleanup(){ mountpoint -q "$MNT" && umount "$MNT"; [ -n "${LOOP:-}" ] && losetup -d "$LOOP" 2>/dev/null; rm -rf "$IMG" "$MNT"; }
trap cleanup EXIT
truncate -s 256M "$IMG"; LOOP=$(losetup --find --show "$IMG")
mkfs.ext4 -q -F -b 4096 "$LOOP"; mkdir -p "$MNT"; mount "$LOOP" "$MNT"

gcc -O2 -D_GNU_SOURCE -o /tmp/pw-$$ tests/integ/preload_writer.c || { bad "build writer"; exit 1; }

# 1) BASELINE: no preload at all.
/tmp/pw-$$ "$MNT/base.log" 64 >/dev/null 2>&1 && ok "baseline write succeeded" || bad "baseline write"
md5base=$(md5sum "$MNT/base.log" | cut -d' ' -f1)

# 2) INERT: library loaded but EXITOS_FILES unset -> must behave identically.
LD_PRELOAD=$R/libexitos_preload.so /tmp/pw-$$ "$MNT/inert.log" 64 >/dev/null 2>&1 \
  && ok "inert-mode write succeeded" || bad "inert-mode write"
md5inert=$(md5sum "$MNT/inert.log" | cut -d' ' -f1)
[ "$md5base" = "$md5inert" ] && ok "inert mode is byte-identical to baseline" \
                             || bad "inert mode changed data ($md5base vs $md5inert)"

# 3) ARMED: fast path enabled for *.log on this loop-backed ext4.
LD_PRELOAD=$R/libexitos_preload.so EXITOS_FILES=".log" EXITOS_VERBOSE=1 \
  /tmp/pw-$$ "$MNT/fast.log" 64 > /tmp/fastout-$$ 2>&1 \
  && ok "armed-mode write succeeded" || bad "armed-mode write"
grep -q "ARMED" /tmp/fastout-$$ && ok "preload reported ARMED" || bad "preload did not arm"

# THE decisive check: drop caches, then read back through the filesystem.
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
md5fast=$(md5sum "$MNT/fast.log" | cut -d' ' -f1)
[ "$md5base" = "$md5fast" ] \
  && ok "ARMED path produced byte-identical data (LBA math correct)" \
  || bad "ARMED path CORRUPTED data ($md5base vs $md5fast)"

# content spot check
blk=$(dd if="$MNT/fast.log" bs=4096 skip=17 count=1 status=none | head -c 27)
[ "$blk" = "EXITOS-BLOCK-000017-payload" ] && ok "block 17 content exact" || bad "block 17 wrong: '$blk'"
echo "# registration decisions:"; grep -c "register fd" /tmp/fastout-$$ >/dev/null && sed -n 's/^/#   /p' /tmp/fastout-$$ | head -4

# 4) URINGPOLL on a device without poll queues: the backend must refuse at
#    registration (iopath_poll_available reads queue/io_poll), say why under
#    EXITOS_VERBOSE, and leave the workload on the ordinary path, unharmed.
LD_PRELOAD=$R/libexitos_preload.so EXITOS_FILES=".log" EXITOS_IOPATH=uringpoll \
  EXITOS_VERBOSE=1 EXITOS_STATS=/tmp/pollstats-$$ \
  /tmp/pw-$$ "$MNT/poll.log" 64 > /tmp/pollout-$$ 2>&1 \
  && ok "uringpoll-mode write still succeeded" || bad "uringpoll-mode write failed"
grep -q "declined" /tmp/pollout-$$ \
  && ok "uringpoll registration declined visibly (no poll queues on loop)" \
  || bad "uringpoll refusal left no visible reason"
[ "$(sed -n 1p /tmp/pollstats-$$ 2>/dev/null || echo 0)" -eq 0 ] \
  && ok "uringpoll refusal means zero fast-path writes" \
  || bad "uringpoll took over writes on a device without poll queues"
md5poll=$(md5sum "$MNT/poll.log" | cut -d' ' -f1)
[ "$md5base" = "$md5poll" ] && ok "uringpoll fallback is byte-identical" \
                            || bad "uringpoll fallback corrupted data"
rm -f /tmp/pw-$$ /tmp/fastout-$$ /tmp/pollout-$$ /tmp/pollstats-$$
[ $fail -eq 0 ] && echo "1..11  (0 failed)" || echo "1..11  (FAILURES)"
exit $fail
