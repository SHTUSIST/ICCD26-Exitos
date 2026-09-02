#!/bin/bash
# Pure fake-sysfs contract tests for the fixed-BDF NVMe polling transaction.
# Nothing in this test may resolve, read, mount, bind, or open a host device.
set -u
set -o pipefail
set -f
cd "$(dirname "$0")/../../.."

SCRIPT=${EXITOS_NVME_REBIND_SCRIPT_UNDER_TEST:-tools/nvme-poll-rebind-transaction.sh}
n=0
failed=0
ok() { n=$((n + 1)); echo "ok $n - $*"; }
nok() { n=$((n + 1)); failed=1; echo "not ok $n - $*"; }
expect_ok() {
    local label=$1
    shift
    if "$@"; then ok "$label"; else nok "$label"; fi
}
expect_fail() {
    local label=$1
    shift
    if "$@"; then nok "$label"; else ok "$label"; fi
}

tmp=$(mktemp -d /tmp/exitos-nvme-rebind-test.XXXXXX) || exit 1
case "$tmp" in /tmp/exitos-nvme-rebind-test.*) ;; *) exit 1 ;; esac
[ -n "$tmp" ] && [ "$tmp" != / ] && [ "$tmp" != /sys ] &&
    [[ $tmp != /sys/* ]] || exit 1

owned_test_pids() {
    local proc pid command
    for proc in /proc/[0-9]*; do
        pid=${proc##*/}
        [ "$pid" != "$$" ] || continue
        command=$(tr '\0' ' ' 2>/dev/null <"$proc/cmdline") || continue
        case "$command" in *"$tmp/"*) printf '%s\n' "$pid" ;; esac
    done
}

cleanup() {
    local pids
    pids=$(owned_test_pids)
    [ -z "$pids" ] || kill -TERM $pids 2>/dev/null || true
    for _ in $(seq 1 100); do
        pids=$(owned_test_pids)
        [ -n "$pids" ] || break
        sleep 0.01
    done
    pids=$(owned_test_pids)
    [ -z "$pids" ] || kill -KILL $pids 2>/dev/null || true
    rm -rf -- "$tmp"
}
trap cleanup EXIT

sys="$tmp/sys"
proc="$tmp/proc"
dev="$tmp/dev"
run="$tmp/run"
mnt="$tmp/mnt/exitos_fs"
state="$tmp/state"
template="$tmp/template"
events="$tmp/events.log"
fail_arm="$state/fail-arm"
admin_serial="$state/admin-serial"
mount_state="$state/mount"
child_marker="$state/child-marker"
argv_log="$state/argv.log"
desc_pid="$state/desc.pid"
BDF=0000:01:00.0
SERIAL=EXAMPLESERIAL0001
WWID=eui.00000000000000000000000000000000
UUID=00000000-0000-0000-0000-000000000000
PCI_REAL="$sys/devices/pci0000:d8/$BDF"

make_initial_tree() {
    rm -rf -- "$sys" "$proc" "$dev" "$run" "$state" "$template" "$tmp/mnt"
    mkdir -p "$sys/bus/pci/devices" "$sys/bus/pci/drivers/vfio-pci" \
        "$sys/bus/pci/drivers/nvme" "$sys/kernel/iommu_groups/21/devices" \
        "$sys/module/nvme/parameters" "$sys/class/nvme" "$sys/class/block" \
        "$sys/block" "$PCI_REAL" "$proc/sys/kernel/random" "$dev/vfio" \
        "$run" "$state" "$template/nvme7/nvme7n1/nvme7n1p1" \
        "$template/nvme7/nvme7n1/queue" "$template/nvme7/nvme7n1/mq" \
        "$mnt"
    chmod 0700 "$run" "$state"
    ln -s "$PCI_REAL" "$sys/bus/pci/devices/$BDF"
    ln -s "$sys/bus/pci/drivers/vfio-pci" "$PCI_REAL/driver"
    ln -s "$sys/kernel/iommu_groups/21" "$PCI_REAL/iommu_group"
    ln -s "$PCI_REAL" "$sys/kernel/iommu_groups/21/devices/$BDF"
    printf '%s\n' 0x1e0f >"$PCI_REAL/vendor"
    printf '%s\n' 0x002c >"$PCI_REAL/device"
    printf '%s\n' 0x1028 >"$PCI_REAL/subsystem_vendor"
    printf '%s\n' 0x22c2 >"$PCI_REAL/subsystem_device"
    printf '%s\n' '(null)' >"$PCI_REAL/driver_override"
    printf '%s\n' 0 >"$sys/module/nvme/parameters/poll_queues"
    : >"$sys/bus/pci/drivers/vfio-pci/unbind"
    : >"$sys/bus/pci/drivers/vfio-pci/bind"
    : >"$sys/bus/pci/drivers/nvme/unbind"
    : >"$sys/bus/pci/drivers/nvme/bind"
    : >"$sys/bus/pci/drivers_probe"
    printf '%s\n' live >"$template/nvme7/state"
    printf '%s\n' 241:0 >"$template/nvme7/dev"
    printf '%s\n' "$SERIAL" >"$template/nvme7/serial"
    printf '%s\n' 1 >"$template/nvme7/nvme7n1/nsid"
    printf '%s\n' "$WWID" >"$template/nvme7/nvme7n1/wwid"
    printf '%s\n' 259:8 >"$template/nvme7/nvme7n1/dev"
    printf '%s\n' 72 >"$template/nvme7/nvme7n1/diskseq"
    printf '%s\n' 1 >"$template/nvme7/nvme7n1/queue/io_poll"
    printf '%s\n' '0 0' >"$template/nvme7/nvme7n1/inflight"
    printf '%s\n' 2048 >"$template/nvme7/nvme7n1/nvme7n1p1/start"
    printf '%s\n' 419430400 >"$template/nvme7/nvme7n1/nvme7n1p1/size"
    printf '%s\n' 259:9 >"$template/nvme7/nvme7n1/nvme7n1p1/dev"
    printf '%s\n' 1 >"$template/nvme7/nvme7n1/nvme7n1p1/partition"
    local q
    for q in $(seq 0 7); do
        mkdir -p "$template/nvme7/nvme7n1/mq/$q"
        printf '%s\n' "$q,$((q + 16))" >"$template/nvme7/nvme7n1/mq/$q/cpu_list"
    done
    printf '%s\n' fake-boot-id >"$proc/sys/kernel/random/boot_id"
    printf '259 8 nvme7n1 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n' >"$proc/diskstats"
    : >"$events"
    printf '%s\n' "$SERIAL" >"$admin_serial"
    rm -f -- "$fail_arm" "$mount_state" "$child_marker" "$argv_log" "$desc_pid"
}

helper="$tmp/fake-nvme-control"
apply_patch_helper_body=''
cat >"$helper" <<'FAKE_HELPER'
#!/bin/bash
set -u
set -f
action=$1
shift
printf '%s' "$action" >>"$FAKE_EVENTS"
for arg in "$@"; do printf ' <%s>' "$arg" >>"$FAKE_EVENTS"; done
printf '\n' >>"$FAKE_EVENTS"

armed() {
    [ -f "$FAKE_FAIL_ARM" ] && [ "$(<"$FAKE_FAIL_ARM")" = "$1" ] || return 1
    rm -f -- "$FAKE_FAIL_ARM"
    return 0
}

remove_nvme_tree() {
    rm -rf -- "$FAKE_PCI_REAL/nvme"
    rm -f -- "$FAKE_SYS/class/nvme/nvme7" "$FAKE_SYS/class/block/nvme7n1" \
        "$FAKE_SYS/block/nvme7n1" "$FAKE_DEV/nvme7" \
        "$FAKE_DEV/nvme7n1" "$FAKE_DEV/nvme7n1p1"
}

create_nvme_tree() {
    mkdir -p "$FAKE_PCI_REAL/nvme"
    cp -a "$FAKE_TEMPLATE/nvme7" "$FAKE_PCI_REAL/nvme/"
    ln -s "$FAKE_PCI_REAL/nvme/nvme7" "$FAKE_SYS/class/nvme/nvme7"
    ln -s "$FAKE_PCI_REAL/nvme/nvme7/nvme7n1" "$FAKE_SYS/class/block/nvme7n1"
    ln -s "$FAKE_PCI_REAL/nvme/nvme7/nvme7n1" "$FAKE_SYS/block/nvme7n1"
    : >"$FAKE_DEV/nvme7"
    : >"$FAKE_DEV/nvme7n1"
    : >"$FAKE_DEV/nvme7n1p1"
    if [ "${FAKE_INJECT_RAW_AFTER_PROBE:-0}" = 1 ]; then
        mkdir -p "$FAKE_PROC/919/fd"
        printf '%s\n' foreign-raw-user >"$FAKE_PROC/919/comm"
        ln -s "$FAKE_DEV/nvme7n1" "$FAKE_PROC/919/fd/5"
    fi
    if [ "${FAKE_INJECT_INFLIGHT:-0}" = 1 ]; then
        printf '%s\n' '0 1' >"$FAKE_PCI_REAL/nvme/nvme7/nvme7n1/inflight"
    fi
}

case "$action" in
  write)
    path=$1 value=$2
    case "$path:$value" in
      */parameters/poll_queues:0) step=restore-param ;;
      */parameters/poll_queues:*) step=poll-write ;;
      */driver_override:nvme) step=override-write ;;
      */driver_override:*) step=restore-override ;;
      */vfio-pci/unbind:*) step=vfio-unbind ;;
      */drivers_probe:*) step=nvme-probe ;;
      */nvme/unbind:*) step=nvme-unbind ;;
      */vfio-pci/bind:*) step=restore-bind ;;
      *) step=unknown-write ;;
    esac
    armed "$step" && exit 41
    case "$step" in
      poll-write|restore-param|override-write)
        printf '%s\n' "$value" >"$path"
        ;;
      restore-override)
        if [ -n "$value" ]; then printf '%s\n' "$value" >"$path"
        else printf '%s\n' '(null)' >"$path"; fi
        ;;
      vfio-unbind)
        rm -f -- "$FAKE_PCI_REAL/driver"
        ;;
      nvme-probe)
        [ "$value" = 0000:01:00.0 ] || exit 42
        ln -s "$FAKE_SYS/bus/pci/drivers/nvme" "$FAKE_PCI_REAL/driver"
        create_nvme_tree
        ;;
      nvme-unbind)
        rm -f -- "$FAKE_PCI_REAL/driver"
        remove_nvme_tree
        ;;
      restore-bind)
        [ "$value" = 0000:01:00.0 ] || exit 43
        ln -s "$FAKE_SYS/bus/pci/drivers/vfio-pci" "$FAKE_PCI_REAL/driver"
        ;;
      *) exit 44 ;;
    esac
    ;;
  admin-id)
    armed admin-id && exit 45
    printf 'serial=%s\n' "$(<"$FAKE_ADMIN_SERIAL")"
    ;;
  block-identity)
    armed block-identity && exit 46
    printf 'whole_dev=259:8\npart_dev=259:9\nuuid=%s\n' "$FAKE_UUID"
    ;;
  mount)
    armed mount && exit 47
    printf '%s\t%s\n' "$1" "$FAKE_UUID" >"$FAKE_MOUNT_STATE"
    ;;
  mount-info)
    armed mount-info && exit 48
    [ -f "$FAKE_MOUNT_STATE" ] || exit 49
    IFS=$'\t' read -r source uuid <"$FAKE_MOUNT_STATE"
    printf 'source=%s\nuuid=%s\n' "$source" "$uuid"
    ;;
  unmount)
    armed unmount && exit 50
    rm -f -- "$FAKE_MOUNT_STATE"
    ;;
  *) exit 51 ;;
esac
FAKE_HELPER
chmod 0700 "$helper"

observer="$tmp/observer"
cat >"$observer" <<'OBSERVER'
#!/bin/bash
set -u
: >"$CHILD_MARKER"
printf '<%s>\n' "$@" >"$ARGV_LOG"
exit "${OBSERVER_RC:-0}"
OBSERVER
chmod 0700 "$observer"

sleeper="$tmp/sleeper"
cat >"$sleeper" <<'SLEEPER'
#!/bin/bash
set -u
: >"$CHILD_MARKER"
trap 'exit 0' HUP INT TERM
while :; do sleep 0.05; done
SLEEPER
chmod 0700 "$sleeper"

descender="$tmp/descender"
cat >"$descender" <<'DESCENDER'
#!/bin/bash
set -u
(trap '' HUP INT TERM; printf '%s\n' "$BASHPID" >"$DESC_PID"; while :; do sleep 1; done) &
for _ in $(seq 1 100); do [ -s "$DESC_PID" ] && exit 0; sleep 0.01; done
exit 61
DESCENDER
chmod 0700 "$descender"

run_tx() {
    EXITOS_NVME_REBIND_TEST_MODE=1 \
    EXITOS_NVME_REBIND_SYS_ROOT="$sys" \
    EXITOS_NVME_REBIND_PROC_ROOT="$proc" \
    EXITOS_NVME_REBIND_DEV_ROOT="$dev" \
    EXITOS_NVME_REBIND_RUN_ROOT="$run" \
    EXITOS_NVME_REBIND_MOUNTPOINT="$mnt" \
    EXITOS_NVME_REBIND_TEST_HELPER="$helper" \
    FAKE_EVENTS="$events" FAKE_FAIL_ARM="$fail_arm" \
    FAKE_PCI_REAL="$PCI_REAL" FAKE_SYS="$sys" FAKE_DEV="$dev" \
    FAKE_PROC="$proc" \
    FAKE_TEMPLATE="$template" FAKE_ADMIN_SERIAL="$admin_serial" \
    FAKE_UUID="$UUID" FAKE_MOUNT_STATE="$mount_state" \
    CHILD_MARKER="$child_marker" ARGV_LOG="$argv_log" DESC_PID="$desc_pid" \
        bash "$SCRIPT" "$@"
}

is_initial() {
    [ "$(basename "$(readlink "$PCI_REAL/driver")")" = vfio-pci ] &&
    [ "$(<"$PCI_REAL/driver_override")" = '(null)' ] &&
    [ "$(<"$sys/module/nvme/parameters/poll_queues")" = 0 ] &&
    [ ! -e "$mount_state" ] && [ ! -e "$PCI_REAL/nvme" ]
}

make_initial_tree
run_tx --with-polling 8 -- "$observer" 'two words' '*' \
    >"$tmp/success.out" 2>"$tmp/success.err"
success_rc=$?
[ "$success_rc" -eq 0 ] || sed -n '1,120p' "$tmp/success.err" >&2
expect_ok "fixed target polling transaction succeeds" test "$success_rc" -eq 0
expect_ok "command argv is forwarded literally" \
    bash -c 'grep -q "^<two words>$" "$1" && grep -q "^<\*>$" "$1"' _ "$argv_log"
expect_ok "successful transaction restores exact initial state" is_initial
expect_ok "poll parameter is written and read before the only exact BDF probe" \
    bash -c 'p=$(grep -n "^write <.*/poll_queues> <8>$" "$1" | cut -d: -f1); q=$(grep -n "^write <.*/drivers_probe> <0000:01:00.0>$" "$1" | cut -d: -f1); test -n "$p" && test -n "$q" && test "$p" -lt "$q"' _ "$events"
expect_ok "the tool never uses rescan, new_id, wildcard bind, or module unload" \
    bash -c '! grep -E "rescan|new_id|modprobe|rmmod|nvme.*unload|<\*>" "$1"' _ "$events"
expect_ok "admin identity precedes every block-data identity operation" \
    bash -c 'a=$(grep -n "^admin-id " "$1" | head -1 | cut -d: -f1); b=$(grep -n "^block-identity " "$1" | head -1 | cut -d: -f1); test -n "$a" && test -n "$b" && test "$a" -lt "$b"' _ "$events"
expect_ok "success reports exact identity and polling generation" \
    grep -Eq "^NVME_POLL_READY bdf=$BDF serial=$SERIAL wwid=$WWID nsid=1 poll_queues=8 io_poll=1 mq_sha256=[0-9a-f]{64}$" "$tmp/success.out"
expect_ok "success reports verified rollback" \
    grep -q '^NVME_POLL_RESTORE driver=vfio-pci override=(null) poll_queues=0 verified=yes$' "$tmp/success.out"

for queues in 2 4 8; do
    make_initial_tree
    run_tx --with-polling "$queues" -- "$observer" >"$tmp/q$queues.out" 2>"$tmp/q$queues.err"
    rc=$?
    expect_ok "explicit poll queue count $queues is accepted and restored" \
        bash -c 'test "$1" -eq 0 && grep -q "poll_queues=$2 " "$3" && "$4"' \
            _ "$rc" "$queues" "$tmp/q$queues.out" is_initial
done

make_initial_tree
run_tx --with-polling -- "$observer" >"$tmp/default.out" 2>"$tmp/default.err"
default_rc=$?
expect_ok "omitted poll count defaults to eight" \
    bash -c 'test "$1" -eq 0 && grep -q "poll_queues=8 " "$2"' _ "$default_rc" "$tmp/default.out"

for bad in 0 1 3 5 9 08 x -1; do
    make_initial_tree
    expect_fail "poll queue value '$bad' is rejected before mutation" \
        run_tx --with-polling "$bad" -- "$observer"
    expect_ok "rejected queue value '$bad' leaves state untouched" is_initial
done

make_initial_tree
expect_fail "relative command path is rejected" run_tx --with-polling 8 -- true
expect_ok "relative command rejection is pre-mutation" is_initial

make_initial_tree
printf '%s\n' BAD-SERIAL >"$admin_serial"
expect_fail "controller admin serial mismatch fails closed" \
    run_tx --with-polling 8 -- "$observer"
expect_ok "serial mismatch occurs before block data is opened" \
    bash -c '! grep -q "^block-identity " "$1" && ! grep -q "^mount " "$1"' _ "$events"
expect_ok "serial mismatch rolls back exactly" is_initial

make_initial_tree
mkdir -p "$proc/313/fd"
printf '%s\n' qemu-system-x86_64 >"$proc/313/comm"
expect_fail "a VM process is refused before mutation" run_tx --with-polling 8 -- "$observer"
expect_ok "VM refusal preserves vfio initial state" is_initial

make_initial_tree
mkdir -p "$proc/314/fd"
printf '%s\n' holder >"$proc/314/comm"
ln -s "$dev/vfio/21" "$proc/314/fd/7"
expect_fail "a VFIO group opener is refused before mutation" run_tx --with-polling 8 -- "$observer"
expect_ok "VFIO opener refusal preserves initial state" is_initial

make_initial_tree
mkdir -p "$sys/kernel/iommu_groups/21/devices/0000:01:00.1"
expect_fail "a non-singleton IOMMU group is refused" run_tx --with-polling 8 -- "$observer"
expect_ok "non-singleton refusal is pre-mutation" is_initial

make_initial_tree
FAKE_INJECT_RAW_AFTER_PROBE=1 run_tx --with-polling 8 -- "$observer" \
    >"$tmp/raw-race.out" 2>"$tmp/raw-race.err"
raw_race_rc=$?
expect_ok "a raw opener appearing after probe is refused before CMD" test "$raw_race_rc" -ne 0
expect_ok "post-probe raw opener refusal rolls back" is_initial

make_initial_tree
FAKE_INJECT_INFLIGHT=1 run_tx --with-polling 8 -- "$observer" \
    >"$tmp/inflight.out" 2>"$tmp/inflight.err"
inflight_rc=$?
expect_ok "nonzero target inflight I/O is refused before CMD" test "$inflight_rc" -ne 0
expect_ok "inflight refusal rolls back" is_initial

make_initial_tree
printf '%s\n' 0 >"$template/nvme7/nvme7n1/queue/io_poll"
expect_fail "namespace without io_poll capability is refused" \
    run_tx --with-polling 8 -- "$observer"
expect_ok "io_poll refusal rolls back" is_initial

make_initial_tree
rm -rf -- "$template/nvme7/nvme7n1/mq/7"
expect_fail "fewer mq contexts than requested poll queues is refused" \
    run_tx --with-polling 8 -- "$observer"
expect_ok "mq count refusal rolls back" is_initial

for step in poll-write override-write vfio-unbind nvme-probe admin-id \
            block-identity mount mount-info; do
    make_initial_tree
    printf '%s\n' "$step" >"$fail_arm"
    run_tx --with-polling 8 -- "$observer" >"$tmp/fail-$step.out" 2>"$tmp/fail-$step.err"
    rc=$?
    expect_ok "partial failure at $step returns failure" test "$rc" -ne 0
    expect_ok "partial failure at $step rolls back exact state" is_initial
done

make_initial_tree
OBSERVER_RC=37 run_tx --with-polling 8 -- "$observer" \
    >"$tmp/cmd-fail.out" 2>"$tmp/cmd-fail.err"
cmd_fail_rc=$?
expect_ok "command failure is propagated" test "$cmd_fail_rc" -eq 37
expect_ok "command failure still rolls back" is_initial

for sig in HUP INT TERM; do
    make_initial_tree
    run_tx --with-polling 8 -- "$sleeper" >"$tmp/sig-$sig.out" 2>"$tmp/sig-$sig.err" &
    tx_pid=$!
    for _ in $(seq 1 200); do [ -e "$child_marker" ] && break; sleep 0.01; done
    kill -s "$sig" "$tx_pid"
    wait "$tx_pid"
    sig_rc=$?
    case "$sig" in HUP) expected_rc=129 ;; INT) expected_rc=130 ;; TERM) expected_rc=143 ;; esac
    expect_ok "$sig returns conventional signal status" test "$sig_rc" -eq "$expected_rc"
    expect_ok "$sig terminates workload and restores state" is_initial
done

make_initial_tree
run_tx --with-polling 8 -- "$sleeper" >"$tmp/lock-owner.out" 2>"$tmp/lock-owner.err" &
owner_pid=$!
for _ in $(seq 1 200); do [ -e "$child_marker" ] && break; sleep 0.01; done
before=$(wc -l <"$events")
expect_fail "a concurrent transaction loses the nonblocking global lock" \
    run_tx --with-polling 8 -- "$observer"
after=$(wc -l <"$events")
expect_ok "lock loser performs no mutation" test "$before" -eq "$after"
expect_ok "live journal has mode 0600" \
    test "$(stat -Lc %a "$run/exitos-nvme-poll-rebind/journal")" = 600
expect_ok "journal pins immutable target and original identity" \
    bash -c 'grep -q "^bdf=0000:01:00.0$" "$1" && grep -q "^serial=EXAMPLESERIAL0001$" "$1" && grep -q "^original_driver=vfio-pci$" "$1" && grep -q "^original_poll_queues=0$" "$1"' \
        _ "$run/exitos-nvme-poll-rebind/journal"
kill -TERM "$owner_pid"
wait "$owner_pid" || true
expect_ok "lock owner cleanup restores initial state" is_initial

make_initial_tree
run_tx --with-polling 8 -- "$descender" >"$tmp/desc.out" 2>"$tmp/desc.err"
desc_rc=$?
expect_ok "a command that leaves a process-group descendant is failed" test "$desc_rc" -ne 0
if [ -s "$desc_pid" ]; then descendant=$(<"$desc_pid"); else descendant=0; fi
expect_ok "left-behind descendant is terminated" \
    bash -c 'test "$1" = 0 || ! kill -0 "$1" 2>/dev/null' _ "$descendant"
expect_ok "descendant cleanup restores state" is_initial

make_initial_tree
printf '%s\n' restore-bind >"$fail_arm"
run_tx --with-polling 8 -- "$observer" >"$tmp/restore-fail.out" 2>"$tmp/restore-fail.err"
restore_fail_rc=$?
expect_ok "rollback verification failure has dedicated rc70" test "$restore_fail_rc" -eq 70
expect_ok "failed rollback retains recovery journal" \
    test -f "$run/exitos-nvme-poll-rebind/journal"

# Clear the one-shot arm and recover the retained journal without running I/O.
rm -f -- "$fail_arm"
run_tx --recover >"$tmp/recover.out" 2>"$tmp/recover.err"
recover_rc=$?
expect_ok "explicit recovery completes a retained transaction" test "$recover_rc" -eq 0
expect_ok "explicit recovery verifies exact initial state" is_initial
expect_ok "successful recovery removes journal" \
    test ! -e "$run/exitos-nvme-poll-rebind/journal"

make_initial_tree
run_tx --with-polling 8 -- "$sleeper" >"$tmp/kill-owner.out" 2>"$tmp/kill-owner.err" &
kill_owner=$!
for _ in $(seq 1 200); do [ -e "$child_marker" ] && break; sleep 0.01; done
kill -KILL "$kill_owner"
wait "$kill_owner" 2>/dev/null || true
expect_ok "SIGKILL leaves the durable journal for recovery" \
    test -f "$run/exitos-nvme-poll-rebind/journal"
# The killed wrapper cannot reap its workload; terminate the owned fake command,
# then exercise journal recovery. A real operator must likewise quiesce owners.
pids=$(owned_test_pids)
[ -z "$pids" ] || kill -KILL $pids 2>/dev/null || true
run_tx --recover >"$tmp/kill-recover.out" 2>"$tmp/kill-recover.err"
kill_recover_rc=$?
expect_ok "SIGKILL journal is recoverable after owners are quiesced" \
    test "$kill_recover_rc" -eq 0
expect_ok "SIGKILL recovery restores exact state" is_initial

make_initial_tree
mkdir -p "$run/exitos-nvme-poll-rebind"
sentinel="$tmp/sentinel"
printf '%s\n' keep >"$sentinel"
ln -s "$sentinel" "$run/exitos-nvme-poll-rebind/journal"
expect_fail "a symlink journal is refused" run_tx --with-polling 8 -- "$observer"
expect_ok "symlink journal target is not overwritten" \
    bash -c 'test "$(<"$1")" = keep' _ "$sentinel"
expect_ok "symlink journal refusal is pre-mutation" is_initial

expect_ok "test artifacts are confined to the private fake root" \
    bash -c '! grep -R -E "(^|[[:space:]<])/(sys|dev|proc|run|root)(/|[[:space:]>])" "$1"' _ "$events"
expect_ok "no test-owned descendants remain" test -z "$(owned_test_pids)"

echo "1..$n"
exit "$failed"
