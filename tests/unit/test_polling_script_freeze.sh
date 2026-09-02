#!/bin/bash
# Pure safety contract for the retired PCI reprobe helper.  Nothing in this
# test may inspect or change a real controller.
set -u
cd "$(dirname "$0")/../.."

n=0
fail=0
ok()  { n=$((n + 1)); echo "ok $n - $*"; }
nok() { n=$((n + 1)); fail=1; echo "not ok $n - $*"; }
expect_ok() {
    local label=$1
    shift
    if "$@"; then ok "$label"; else nok "$label"; fi
}
expect_status() {
    local want=$1 label=$2
    shift 2
    "$@" >/dev/null 2>&1
    local got=$?
    if [ "$got" -eq "$want" ]; then ok "$label"; else
        nok "$label (wanted $want, got $got)"
    fi
}

script=tools/enable-nvme-polling.sh
expect_ok "script parses as shell" bash -n "$script"

tmp=$(mktemp -d /tmp/exitos-polling-freeze-test.XXXXXX)
cleanup() { rm -rf -- "$tmp"; }
trap cleanup EXIT
mkdir -p "$tmp/bin"

# If the frozen tool reaches any historical discovery, mount, benchmark, or
# controller-management utility, record the escape and fail it.
log="$tmp/external.log"
for cmd in nvme findmnt readlink basename mount mountpoint umount udevadm \
           lsblk fio sleep awk join who grep sed cat; do
    printf '%s\n' '#!/bin/bash' \
        "printf '%s\\n' '$cmd' >>'$log'" \
        'exit 97' >"$tmp/bin/$cmd"
    chmod 0700 "$tmp/bin/$cmd"
done

run_script() {
    PATH="$tmp/bin" /bin/bash "$script" "$@"
}
source_lacks() {
    ! rg -q -- "$1" "$script"
}

explain=$(run_script --explain 2>"$tmp/explain.err")
explain_rc=$?
expect_ok "explain mode succeeds without host discovery" test "$explain_rc" -eq 0
expect_ok "explain mode names the destructive incident" \
    grep -qi 'device did not return\|reboot' <<<"$explain"
expect_ok "explain mode says executable reprobe is frozen" \
    grep -qi 'frozen\|disabled' <<<"$explain"
expect_ok "explain mode invokes no external utility" test ! -s "$log"

plan=$(run_script --plan 2>"$tmp/plan.err")
plan_rc=$?
expect_ok "plan mode succeeds without host discovery" test "$plan_rc" -eq 0
expect_ok "plan requires a maintenance-window reboot design" \
    grep -qi 'boot\|reboot' <<<"$plan"
expect_ok "plan requires stable controller and namespace identity" \
    grep -qi 'serial.*nsid\|nsid.*serial' <<<"$plan"
expect_ok "plan mode invokes no external utility" test ! -s "$log"

expect_status 64 "default invocation refuses before discovery" run_script
expect_status 64 "legacy run mode is hard-frozen" run_script run
expect_status 64 "legacy restore mode is hard-frozen" run_script restore
expect_status 64 "unknown modes fail closed" run_script --unknown
expect_ok "all refusing modes invoke no external utility" test ! -s "$log"

# Keep the dangerous implementation out of the executable entirely.  A later
# replacement must be reviewed as new code, not silently re-enable this one.
expect_ok "source contains no PCI unbind endpoint" \
    source_lacks '/sys/bus/pci/drivers/nvme/unbind'
expect_ok "source contains no PCI bind endpoint" \
    source_lacks '/sys/bus/pci/drivers/nvme/bind'
expect_ok "source contains no global poll_queues write endpoint" \
    source_lacks '/sys/module/nvme/parameters/poll_queues'
expect_ok "source contains no eval execution" \
    source_lacks '(^|[[:space:]])eval([[:space:]]|$)'

echo "1..$n  ($fail failed)"
test "$fail" -eq 0
