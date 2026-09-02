#!/bin/bash
# Print the test disk's device paths, resolved from its serial number.
#
# Device names move, and they do not move together. The test host renumbered its
# NVMe controllers three times across reboots and PCI re-probes; after one
# recovery the block device came back as nvme1n2 while its generic character
# device came back as ng4n2, so deriving one name from the other gave a path
# that did not exist. Both are looked up here, each against the serial number,
# and neither is guessed.
#
# Usage:
#     eval "$(bash tools/testdisk.sh)"
#     echo "$EXITOS_DEV $EXITOS_CHARDEV"
#
# It also exports EXITOS_EXPECT_SERIAL, which the library's registration path
# and the benchmark guards both check, so a stale value fails closed instead of
# writing somewhere unintended.
set -u
SN="${EXITOS_TEST_SERIAL:-EXAMPLESERIAL0001}"
SYSFS_ROOT=/sys
DEV_ROOT=/dev
NVME_BIN=nvme
TEST_MODE=0
if [ -n "${EXITOS_TESTDISK_SYSFS_ROOT+x}${EXITOS_TESTDISK_DEV_ROOT+x}${EXITOS_TESTDISK_NVME_BIN+x}" ]; then
    [ "${EXITOS_TESTDISK_TEST_MODE:-}" = 1 ] || {
        echo "testdisk: resolver root overrides are pure-test-only" >&2
        exit 2
    }
    TEST_MODE=1
    SYSFS_ROOT=${EXITOS_TESTDISK_SYSFS_ROOT:?pure test requires sysfs root}
    DEV_ROOT=${EXITOS_TESTDISK_DEV_ROOT:?pure test requires dev root}
    NVME_BIN=${EXITOS_TESTDISK_NVME_BIN:?pure test requires nvme helper}
fi

is_char_node() {
    if [ "$TEST_MODE" = 1 ]; then [ -f "$1" ] && [ ! -L "$1" ];
    else [ -c "$1" ]; fi
}
is_block_node() {
    if [ "$TEST_MODE" = 1 ]; then [ -f "$1" ] && [ ! -L "$1" ];
    else [ -b "$1" ]; fi
}

BLK=""
NSID=""
BLK_COUNT=0
for d in "$SYSFS_ROOT"/class/block/nvme*n[0-9]*; do
    b=$(basename "$d")
    [ -e "$d/partition" ] && continue                # a partition, not a namespace
    case "$b" in *c*) continue;; esac                # nvmeXcYnZ alias node
    [ "$(tr -d ' ' < "$d/device/serial" 2>/dev/null)" = "$SN" ] || continue
    BLK="$b"
    NSID="$(cat "$d/nsid" 2>/dev/null)"
    BLK_COUNT=$((BLK_COUNT + 1))
done
[ "$BLK_COUNT" -eq 1 ] || {
    echo "testdisk: expected exactly one whole namespace for serial $SN; found $BLK_COUNT" >&2
    exit 1
}
WHOLE="$DEV_ROOT/$BLK"
PART="${WHOLE}p1"
is_block_node "$WHOLE" || {
    echo "testdisk: resolved whole node is absent or not a block device: $WHOLE" >&2
    exit 1
}
is_block_node "$PART" || {
    echo "testdisk: resolved partition node is absent or not a block device: $PART" >&2
    exit 1
}

# The character device is what the passthru backends open. Ask each candidate
# who it is rather than assuming its name matches the block device's.
CHR=""
CHR_COUNT=0
for c in "$DEV_ROOT"/ng*; do
    is_char_node "$c" || continue
    csn=$("$NVME_BIN" id-ctrl "$c" 2>/dev/null | awk -F: '/^sn /{gsub(/ /,"",$2); print $2}')
    [ "$csn" = "$SN" ] || continue
    cns=$("$NVME_BIN" id-ns "$c" -n "${NSID:-1}" >/dev/null 2>&1 && echo ok)
    [ -n "$cns" ] || continue
    CHR="$c"
    CHR_COUNT=$((CHR_COUNT + 1))
done
[ "$CHR_COUNT" -eq 1 ] || {
    echo "testdisk: expected exactly one generic character device for serial $SN and namespace $NSID; found $CHR_COUNT" >&2
    exit 1
}

echo "export EXITOS_DEV=$WHOLE"
echo "export EXITOS_EXPECT_SERIAL=$SN"
echo "export EXITOS_FSPART=$PART"
echo "export EXITOS_WINDOW_IN_PART=$PART"
echo "export EXITOS_TESTPART=$PART"
echo "export EXITOS_CHARDEV=$CHR"
