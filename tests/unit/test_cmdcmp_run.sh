#!/bin/bash
# Pure contract tests for tools/cmdcmp-run.sh and tools/cmdcmp.bt.  The runner's
# special test modes are gated by EXITOS_CMDCMP_TEST_MODE=1 and must return
# before resolver, mount, block-device, or real-bpftrace setup.
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
expect_fail() {
    local label=$1
    shift
    if "$@"; then nok "$label"; else ok "$label"; fi
}
finish() {
    echo "1..$n  ($fail failed)"
    test "$fail" -eq 0
}

# This source gate keeps the first RED run safe: the legacy runner evaluated
# the real device resolver before looking at argv.  Do not dynamically execute
# it until the explicitly gated pure-test entry points exist.
expect_ok "runner exposes a read-only plan mode" \
    rg -q -- '--plan' tools/cmdcmp-run.sh
if ! rg -q 'EXITOS_CMDCMP_TEST_MODE' tools/cmdcmp-run.sh; then
    nok "runner exposes gated pure-test modes before device setup"
    finish
    exit $?
fi
ok "runner exposes gated pure-test modes before device setup"

tmp=$(mktemp -d /tmp/exitos-cmdcmp-test.XXXXXX)
cleanup() { rm -rf -- "$tmp"; }
trap cleanup EXIT

mkdir -p "$tmp/bin" "$tmp/mnt" "$tmp/irq-sys/class/nvme/nvme-test/device/msi_irqs" \
         "$tmp/irq-proc/irq/41"
: >"$tmp/irq-sys/class/nvme/nvme-test/device/msi_irqs/41"
printf '%s\n' '0,4' >"$tmp/irq-proc/irq/41/smp_affinity_list"
printf '%s\n' '0' >"$tmp/irq-proc/irq/41/effective_affinity_list"

# Every command that could touch mount state is a poison pill.  A pure-test
# mode reaching one of these is a safety failure, even if the command's result
# would otherwise be ignored.
poison_log="$tmp/poison.log"
for cmd in mount umount mountpoint findmnt sync bpftrace; do
    printf '%s\n' '#!/bin/bash' \
        "printf '%s\\n' '$cmd' >>'$poison_log'" \
        'exit 97' >"$tmp/bin/$cmd"
    chmod 0700 "$tmp/bin/$cmd"
done

tripwire="$tmp/resolver-ran"
trip_resolver="$tmp/trip-resolver"
printf '%s\n' '#!/bin/bash' ": >'$tripwire'" 'exit 98' >"$trip_resolver"
chmod 0700 "$trip_resolver"

base_env=(
    CPU=0 ITERS=2 READY_TIMEOUT=2 TRACE_TIMEOUT=2 APP_TIMEOUT=3
    TRACE_STOP_TIMEOUT=2 EXITOS_CMDCMP_TEST_MODE=1
    EXITOS_TESTMNT="$tmp/mnt" EXITOS_TESTDISK="$trip_resolver"
    EXITOS_IRQ_SYSFS_ROOT="$tmp/irq-sys"
    EXITOS_IRQ_PROC_ROOT="$tmp/irq-proc"
    EXITOS_TEST_NVME_CTRL=nvme-test
    EXITOS_TEST_WHOLE_MAJMIN=259:0
)

plan=$(env "${base_env[@]}" \
    EXITOS_QDSPLIT_BIN=/definitely/missing/qdsplit \
    EXITOS_BPFTRACE_BIN=/definitely/missing/bpftrace \
    PATH="$tmp/bin:$PATH" bash tools/cmdcmp-run.sh --plan \
    2>"$tmp/plan.err")
plan_rc=$?
expect_ok "plan succeeds without benchmark, tracer, resolver, or device" \
    test "$plan_rc" -eq 0
expect_ok "plan exits before the test-disk resolver" test ! -e "$tripwire"
expect_ok "plan exits before mount-related commands" test ! -s "$poison_log"
expect_ok "plan records bounded run metadata" \
    grep -q '^PLAN cpu=0 iters=2 ready_timeout=2 trace_timeout=2 app_timeout=3 trace_stop_timeout=2$' \
        <<<"$plan"
expected_cells=$'CELL index=1 buffer=normal arm=0 name=ext4-pwrite contract=scattered\nCELL index=2 buffer=normal arm=14 name=block-pwrite contract=scattered\nCELL index=3 buffer=normal arm=1 name=uring-passthrough contract=scattered\nCELL index=4 buffer=contiguous arm=0 name=ext4-pwrite contract=contiguous\nCELL index=5 buffer=contiguous arm=14 name=block-pwrite contract=contiguous\nCELL index=6 buffer=contiguous arm=1 name=uring-passthrough contract=contiguous'
actual_cells=$(grep '^CELL ' <<<"$plan")
expect_ok "plan fixes the six-cell 8kpath order and one arm per cell" \
    test "$actual_cells" = "$expected_cells"

for assignment in \
    'CPU=-1' 'CPU=nope' 'CPU=1024' \
    'ITERS=0' 'ITERS=nope' 'ITERS=2001' 'ITERS=1000001' \
    'READY_TIMEOUT=0' 'READY_TIMEOUT=nope' 'READY_TIMEOUT=121' \
    'TRACE_TIMEOUT=0' 'TRACE_TIMEOUT=nope' 'TRACE_TIMEOUT=121' \
    'APP_TIMEOUT=0' 'APP_TIMEOUT=nope' 'APP_TIMEOUT=86401' \
    'TRACE_STOP_TIMEOUT=0' 'TRACE_STOP_TIMEOUT=nope' 'TRACE_STOP_TIMEOUT=121'; do
    expect_fail "invalid bounded input is rejected in plan mode: $assignment" \
        env "${base_env[@]}" "$assignment" PATH="$tmp/bin:$PATH" \
            bash tools/cmdcmp-run.sh --plan
done

# Resolver output is a six-record data protocol, not shell.  The two historical
# bare partition values are normalized, but no quoting or shell punctuation is
# interpreted.
valid_resolver="$tmp/valid-resolver"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" "export EXITOS_DEV=/dev/nvme0n1" "export EXITOS_EXPECT_SERIAL=SERIAL-1" "export EXITOS_FSPART=nvme0n1p1" "export EXITOS_WINDOW_IN_PART=nvme0n1p1" "export EXITOS_TESTPART=/dev/nvme0n1p1" "export EXITOS_CHARDEV=/dev/ng0n1"' \
    >"$valid_resolver"
chmod 0700 "$valid_resolver"
resolved=$(env "${base_env[@]}" EXITOS_TESTDISK="$valid_resolver" \
    bash tools/cmdcmp-run.sh --test-resolver 2>"$tmp/resolver.err")
resolved_rc=$?
expect_ok "strict resolver accepts exactly the six fixed fields" \
    test "$resolved_rc" -eq 0
expect_ok "partition aliases are normalized without evaluation" \
    test "$resolved" = $'/dev/nvme0n1\tSERIAL-1\t/dev/nvme0n1p1\t/dev/nvme0n1p1\t/dev/nvme0n1p1\t/dev/ng0n1'

injected="$tmp/resolver-injected"
bad_resolver="$tmp/bad-resolver"
printf '%s\n' '#!/bin/bash' \
    "printf '%s\\n' 'export EXITOS_DEV=/dev/nvme0n1;touch $injected'" \
    >"$bad_resolver"
chmod 0700 "$bad_resolver"
expect_fail "resolver shell syntax is rejected as data" \
    env "${base_env[@]}" EXITOS_TESTDISK="$bad_resolver" \
        bash tools/cmdcmp-run.sh --test-resolver
expect_ok "rejected resolver text is never executed" test ! -e "$injected"

duplicate_resolver="$tmp/duplicate-resolver"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" "export EXITOS_DEV=/dev/nvme0n1" "export EXITOS_DEV=/dev/nvme0n2" "export EXITOS_EXPECT_SERIAL=SERIAL-1" "export EXITOS_FSPART=nvme0n1p1" "export EXITOS_WINDOW_IN_PART=nvme0n1p1" "export EXITOS_TESTPART=/dev/nvme0n1p1" "export EXITOS_CHARDEV=/dev/ng0n1"' \
    >"$duplicate_resolver"
chmod 0700 "$duplicate_resolver"
expect_fail "duplicate resolver fields are refused" \
    env "${base_env[@]}" EXITOS_TESTDISK="$duplicate_resolver" \
        bash tools/cmdcmp-run.sh --test-resolver

omitted_resolver="$tmp/omitted-resolver"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" "export EXITOS_DEV=/dev/nvme0n1" "export EXITOS_EXPECT_SERIAL=SERIAL-1" "export EXITOS_FSPART=nvme0n1p1" "export EXITOS_WINDOW_IN_PART=nvme0n1p1" "export EXITOS_TESTPART=/dev/nvme0n1p1"' \
    >"$omitted_resolver"
chmod 0700 "$omitted_resolver"
expect_fail "omitted resolver fields are refused" \
    env "${base_env[@]}" EXITOS_TESTDISK="$omitted_resolver" \
        bash tools/cmdcmp-run.sh --test-resolver

unexpected_resolver="$tmp/unexpected-resolver"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" "export EXITOS_DEV=/dev/nvme0n1" "export EXITOS_EXPECT_SERIAL=SERIAL-1" "export EXITOS_FSPART=nvme0n1p1" "export EXITOS_WINDOW_IN_PART=nvme0n1p1" "export EXITOS_TESTPART=/dev/nvme0n1p1" "export EXITOS_CHARDEV=/dev/ng0n1" "export EXITOS_SURPRISE=1"' \
    >"$unexpected_resolver"
chmod 0700 "$unexpected_resolver"
expect_fail "unexpected resolver fields are refused" \
    env "${base_env[@]}" EXITOS_TESTDISK="$unexpected_resolver" \
        bash tools/cmdcmp-run.sh --test-resolver

# A regular file is sufficient for this pure retention test: production still
# requires a block fd/st_rdev, while the test proves that controller derivation
# and later checks use the retained object plus /sys/dev/block identity, not a
# resolver pathname that can be replaced after open(2).
fake_device_sys="$tmp/device-sys"
whole_sys="$fake_device_sys/devices/pci0000:00/0000:00:01.0/nvme/nvme7/nvme7n1"
mkdir -p "$whole_sys" "$fake_device_sys/dev/block"
printf '%s\n' '259:0' >"$whole_sys/dev"
ln -s -- "$whole_sys" "$fake_device_sys/dev/block/259:0"
whole_path="$tmp/misleading-resolver-name"
printf '%s\n' RETAINED_ORIGINAL >"$whole_path"
retained_log="$tmp/retained-whole.log"
env "${base_env[@]}" EXITOS_TEST_WHOLE_PATH="$whole_path" \
    EXITOS_DEVICE_SYSFS_ROOT="$fake_device_sys" \
    EXITOS_TEST_WHOLE_MAJMIN=259:0 \
    bash tools/cmdcmp-run.sh --test-retained-whole \
    >"$retained_log" 2>&1 & retained_pid=$!
for _ in $(seq 1 200); do
    grep -q '^RETAINED_WHOLE_READY$' "$retained_log" 2>/dev/null && break
    kill -0 "$retained_pid" 2>/dev/null || break
    sleep 0.01
done
if grep -q '^RETAINED_WHOLE_READY$' "$retained_log" 2>/dev/null; then
    retained_saved="$tmp/retained-whole-original"
    mv -- "$whole_path" "$retained_saved"
    printf '%s\n' HOSTILE_REPLACEMENT >"$whole_path"
    kill -CONT "$retained_pid" 2>/dev/null || true
    wait "$retained_pid"; retained_rc=$?
    expect_ok "retained whole-device test survives resolver-path replacement" \
        test "$retained_rc" -eq 0
    expect_ok "controller is derived from retained dev_t sysfs identity" \
        grep -q '^RETAINED_WHOLE controller=nvme7 dev_t=259:0 source_path_ignored=1$' "$retained_log"
    expect_ok "whole-device test preserves the hostile replacement" \
        grep -q '^HOSTILE_REPLACEMENT$' "$whole_path"
else
    kill -CONT "$retained_pid" 2>/dev/null || true
    wait "$retained_pid" 2>/dev/null || true
    nok "retained whole-device test survives resolver-path replacement"
    nok "controller is derived from retained dev_t sysfs identity"
    nok "whole-device test preserves the hostile replacement"
fi

taskset_log="$tmp/taskset.log"
fake_taskset="$tmp/fake-taskset"
printf '%s\n' '#!/bin/bash' \
    'test "$#" -ge 3 && test "$1" = -c && [[ $2 =~ ^[0-9]+$ ]] || exit 91' \
    'printf "%s\n" "$2" >>"$FAKE_TASKSET_LOG"' \
    'shift 2' 'exec "$@"' >"$fake_taskset"
chmod 0700 "$fake_taskset"

app_log="$tmp/app.log"
ready_dir="$tmp/ready"
mkdir "$ready_dir"
fake_qdsplit="$tmp/fake-qdsplit"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'trap "exit 143" TERM' \
    'test "$#" -eq 1 && [[ $1 =~ ^[1-9][0-9]*$ ]] || exit 81' \
    'case "${EXITOS_ONLY-}" in 0|1|14) arm=$EXITOS_ONLY ;; *) exit 82 ;; esac' \
    'if test "${EXITOS_REQUIRE_SCATTERED-}" = 1 && test -z "${EXITOS_REQUIRE_CONTIG+x}" && test -z "${EXITOS_HUGEBUF+x}"; then buffer=normal; elif test "${EXITOS_REQUIRE_CONTIG-}" = 1 && test "${EXITOS_HUGEBUF-}" = 1 && test -z "${EXITOS_REQUIRE_SCATTERED+x}"; then buffer=contiguous; else exit 83; fi' \
    'parent=${EXITOS_FSFILE%/*}' \
    'test -d "$parent" && test ! -L "$parent" && test "$(stat -Lc %a -- "$parent")" = 700 || exit 84' \
    'test ! -e "$EXITOS_FSFILE" || exit 85' \
    'printf "%s %s %s\n" "$arm" "$buffer" "$EXITOS_FSFILE" >>"$FAKE_APP_LOG"' \
    'if test "${EXITOS_FAKE_APP_MODE-good}" = no-ready; then sleep 0.2; exit 0; fi' \
    'printf "%s %s\n" "$arm" "$buffer" >"$FAKE_READY_DIR/$$"' \
    'printf "READY\n"' \
    'sleep 0.2' \
    'if test "${EXITOS_FAKE_APP_MODE-good}" = fail; then exit 9; fi' \
    'if test "${EXITOS_FAKE_APP_MODE-good}" = false-proof; then printf "buffer contract: unproven\n"; elif test "$buffer" = normal; then printf "buffer contract: scattered proven runs=2\n"; else printf "buffer contract: contiguous proven runs=1\n"; fi' \
    'printf "fake-arm-%s median 1.000 us\n" "$arm"' \
    'exit 0' >"$fake_qdsplit"
chmod 0700 "$fake_qdsplit"

trace_log="$tmp/trace.log"
fake_bpftrace="$tmp/fake-bpftrace"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'target=$2' \
    'case "$#" in 2) dev_major=$FAKE_EXPECT_MAJOR; dev_minor=$FAKE_EXPECT_MINOR ;; 4) dev_major=$3; dev_minor=$4 ;; *) exit 71 ;; esac' \
    '[[ $target =~ ^[1-9][0-9]*$ && $dev_major =~ ^[0-9]+$ && $dev_minor =~ ^[0-9]+$ ]] || exit 71' \
    'test -e "$FAKE_READY_DIR/$target" || { echo "tracer started before qdsplit READY" >&2; exit 72; }' \
    'printf "pid=%s major=%s minor=%s argc=%s\n" "$target" "$dev_major" "$dev_minor" "$#" >>"$FAKE_TRACE_LOG"' \
    'mode=${EXITOS_FAKE_TRACE_MODE-good}' \
    'if test "$mode" = no-ready; then sleep 0.1; exit 0; fi' \
    'if test "$mode" = wrong-ready; then printf "TRACE_BEGIN target_pid=1\n"; echo "Attached 3 probes" >&2; sleep 0.3; exit 0; fi' \
    'if test "$mode" = attached-first; then echo "Attached 3 probes" >&2; sleep 0.12; printf "TRACE_BEGIN target_pid=%s\n" "$target"; else printf "TRACE_BEGIN target_pid=%s\n" "$target"; fi' \
    'if test "$mode" = attached-first; then sleep 0.08; fi' \
    'if test "$mode" = no-attached; then sleep 0.3; exit 0; fi' \
    'if test "$mode" != attached-first; then echo "Attached 3 probes" >&2; fi' \
    'if test "$mode" = dead-after-markers; then exit 0; fi' \
    'if test "$mode" = early; then printf "TRACE_EVENT pid=%s dev_major=%s dev_first_minor=%s format=PRP\n" "$target" "$dev_major" "$dev_minor"; exit 0; fi' \
    'event_major=$dev_major; if test "$mode" = wrong-dev; then event_major=$((dev_major + 1)); fi' \
    'read -r arm buffer <"$FAKE_READY_DIR/$target"' \
    'if test "$mode" = all-prp; then shape="format=PRP psdt=0 usercmd=0 nr_phys_segments=2 iod_req_flags=0x0 iod_nr_descriptors=0"; else case "$arm:$buffer" in 0:normal|14:normal) shape="format=PRP psdt=0 usercmd=0 nr_phys_segments=2 iod_req_flags=0x0 iod_nr_descriptors=0" ;; 1:normal) shape="format=SGL sgl_class=external_last_segment psdt=1 usercmd=1 nr_phys_segments=2 iod_req_flags=0x2 iod_nr_descriptors=1 sgl_type=0x30 sgl_length=32" ;; 0:contiguous|14:contiguous) shape="format=PRP psdt=0 usercmd=0 nr_phys_segments=1 iod_req_flags=0x0 iod_nr_descriptors=0" ;; 1:contiguous) shape="format=SGL sgl_class=inline_data psdt=1 usercmd=1 nr_phys_segments=1 iod_req_flags=0x2 iod_nr_descriptors=0 sgl_type=0x0 sgl_length=8192" ;; *) exit 73 ;; esac; fi' \
    'case "$mode:$arm:$buffer" in bad-usercmd:0:normal) shape=${shape/usercmd=0/usercmd=1} ;; bad-nr-phys:0:normal) shape=${shape/nr_phys_segments=2/nr_phys_segments=1} ;; bad-psdt:0:normal) shape=${shape/psdt=0/psdt=1} ;; bad-sgl-type:1:normal) shape=${shape/sgl_type=0x30/sgl_type=0x20} ;; bad-sgl-length:1:normal) shape=${shape/sgl_length=32/sgl_length=16} ;; bad-descriptors:1:normal) shape=${shape/iod_nr_descriptors=1/iod_nr_descriptors=0} ;; esac' \
    'if test "$mode" != empty; then for _ in 1 2; do printf "TRACE_EVENT pid=%s dev_major=%s dev_first_minor=%s %s\n" "$target" "$event_major" "$dev_minor" "$shape"; done; fi' \
    'if test "$mode" = duplicate-late; then sleep 0.08; printf "TRACE_BEGIN target_pid=%s\n" "$target"; fi' \
    'while kill -0 "$target" 2>/dev/null; do sleep 0.02; done' \
    'if test "$mode" = stderr; then echo "synthetic tracer error" >&2; fi' \
    'if test "$mode" = empty; then events=0; else events=2; fi' \
    'printf "TRACE_END target_pid=%s events=%s\n" "$target" "$events"' \
    'test "$mode" != stderr' >"$fake_bpftrace"
chmod 0700 "$fake_bpftrace"

real_rmdir=$(command -v rmdir)
cleanup_log="$tmp/cleanup.log"
fake_rmdir="$tmp/bin/rmdir"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'target=${!#}' \
    'printf "%s\n" "$target" >>"$FAKE_CLEANUP_LOG"' \
    'case "${EXITOS_FAKE_RMDIR_MODE-good}:$target" in' \
    '  work:*/.exitos-cmdcmp.*) exit 96 ;;' \
    '  state:*/exitos-cmdcmp-state.*) exit 95 ;;' \
    '  signal-work:*/.exitos-cmdcmp.*) kill -TERM "$PPID"; sleep 0.05 ;;' \
    'esac' \
    'exec "$REAL_RMDIR_BIN" "$@"' >"$fake_rmdir"
chmod 0700 "$fake_rmdir"

real_chmod=$(command -v chmod)
chmod_signal_log="$tmp/chmod-signal.log"
fake_chmod="$tmp/bin/chmod"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'target=${!#}' \
    'if test "${EXITOS_FAKE_CHMOD_MODE-good}" = signal-work && [[ $target == */.exitos-cmdcmp.* ]]; then' \
    '  printf "%s\n" "$target" >>"$FAKE_CHMOD_SIGNAL_LOG"' \
    '  kill -TERM "$PPID"' \
    '  sleep 0.05' \
    'fi' \
    'exec "$REAL_CHMOD_BIN" "$@"' >"$fake_chmod"
chmod 0700 "$fake_chmod"

run_env=(
    "${base_env[@]}" PATH="$tmp/bin:$PATH"
    EXITOS_QDSPLIT_BIN="$fake_qdsplit"
    EXITOS_BPFTRACE_BIN="$fake_bpftrace"
    EXITOS_TASKSET_BIN="$fake_taskset"
    FAKE_TASKSET_LOG="$taskset_log" FAKE_APP_LOG="$app_log"
    FAKE_READY_DIR="$ready_dir" FAKE_TRACE_LOG="$trace_log"
    FAKE_CLEANUP_LOG="$cleanup_log" REAL_RMDIR_BIN="$real_rmdir"
    FAKE_EXPECT_MAJOR=259 FAKE_EXPECT_MINOR=0
    FAKE_CHMOD_SIGNAL_LOG="$chmod_signal_log" REAL_CHMOD_BIN="$real_chmod"
)

: >"$app_log"; : >"$trace_log"; : >"$taskset_log"; : >"$cleanup_log"
env "${run_env[@]}" EXITOS_FAKE_TRACE_MODE=all-prp \
    bash tools/cmdcmp-run.sh --test-run \
    >"$tmp/all-prp.out" 2>"$tmp/all-prp.err"
all_prp_rc=$?
expect_ok "six-cell all-PRP trace is rejected by semantic shape validation" \
    test "$all_prp_rc" -ne 0
expect_ok "all-PRP rejection identifies the missing USER_CMD bit" \
    grep -q 'usercmd=0 expected=1' "$tmp/all-prp.err"
expect_ok "all-PRP rejection publishes the expected external SGL contract" \
    grep -Eq '^SHAPE_CONTRACT index=[0-9]+ usercmd=1 nr_phys_segments=2 format=SGL psdt=1 sgl_class=external_last_segment sgl_type=0x30 sgl_length=32 iod_nr_descriptors=1$' \
        "$tmp/all-prp.out"

: >"$app_log"; : >"$trace_log"; : >"$taskset_log"; : >"$cleanup_log"
good=$(env "${run_env[@]}" bash tools/cmdcmp-run.sh --test-run \
    2>"$tmp/good.err")
good_rc=$?
if [ "$good_rc" -ne 0 ]; then
    sed -n '1,200p' "$tmp/good.err" >&2
fi
expect_ok "fake six-cell 8kpath run succeeds" test "$good_rc" -eq 0
expect_ok "pure run never invokes resolver or mount-related commands" \
    sh -c 'test ! -e "$1" && test ! -s "$2"' sh "$tripwire" "$poison_log"
expect_ok "run emits one immutable metadata record" \
    test "$(grep -c '^RUN_METADATA ' <<<"$good")" -eq 1
expect_ok "traced campaign labels command shape as its only valid measurement" \
    test "$(grep -c '^MEASUREMENT_CONTRACT shape-only latency-invalid-under-tracing$' <<<"$good")" -eq 1
expect_ok "every captured application output warns that traced latency is invalid" \
    test "$(grep -c '^APP_OUTPUT_BEGIN index=[1-6] interpretation=shape-only/latency-invalid-under-tracing$' <<<"$good")" -eq 6
expect_ok "run labels its non-device test identity" \
    grep -q '^DEVICE_METADATA mode=pure-test ' <<<"$good"
expect_ok "each cell records the IRQ affinity snapshot" \
    test "$(grep -c '^IRQ_AFFINITY cell=' <<<"$good")" -eq 6
expect_ok "each cell emits a PID-bound tracer handshake" \
    test "$(grep -c '^TRACE_READY target_pid=' <<<"$good")" -eq 6
expect_ok "each cell contains at least one command event" \
    test "$(grep -c '^TRACE_EVENT ' <<<"$good")" -eq 12
expect_ok "six cells complete in their declared order" \
    test "$(grep -c '^CELL_RESULT index=.* status=ok$' <<<"$good")" -eq 6
expect_ok "each successful cell carries a strict PFN-layout proof" \
    test "$(grep -Ec '^buffer contract: (scattered proven runs=2|contiguous proven runs=1)$' <<<"$good")" -eq 6
expected_shape_contracts=$'SHAPE_CONTRACT index=1 usercmd=0 nr_phys_segments=2 format=PRP psdt=0 sgl_class=none sgl_type=none sgl_length=none iod_nr_descriptors=0\nSHAPE_CONTRACT index=2 usercmd=0 nr_phys_segments=2 format=PRP psdt=0 sgl_class=none sgl_type=none sgl_length=none iod_nr_descriptors=0\nSHAPE_CONTRACT index=3 usercmd=1 nr_phys_segments=2 format=SGL psdt=1 sgl_class=external_last_segment sgl_type=0x30 sgl_length=32 iod_nr_descriptors=1\nSHAPE_CONTRACT index=4 usercmd=0 nr_phys_segments=1 format=PRP psdt=0 sgl_class=none sgl_type=none sgl_length=none iod_nr_descriptors=0\nSHAPE_CONTRACT index=5 usercmd=0 nr_phys_segments=1 format=PRP psdt=0 sgl_class=none sgl_type=none sgl_length=none iod_nr_descriptors=0\nSHAPE_CONTRACT index=6 usercmd=1 nr_phys_segments=1 format=SGL psdt=1 sgl_class=inline_data sgl_type=0x0 sgl_length=8192 iod_nr_descriptors=0'
actual_shape_contracts=$(grep '^SHAPE_CONTRACT ' <<<"$good")
expect_ok "runner publishes the exact six per-cell semantic shape contracts" \
    test "$actual_shape_contracts" = "$expected_shape_contracts"
expect_ok "runner proves every captured event matched its published shape contract" \
    test "$(grep -c '^SHAPE_RESULT index=[1-6] status=ok matched_events=2$' <<<"$good")" -eq 6
expected_env=$'0 normal\n14 normal\n1 normal\n0 contiguous\n14 contiguous\n1 contiguous'
actual_env=$(awk '{print $1, $2}' "$app_log")
expect_ok "normal/contiguous contracts and 8kpath arm order are exact" \
    test "$actual_env" = "$expected_env"
expect_ok "taskset pins every qdsplit child to the validated CPU" \
    sh -c 'test "$(wc -l <"$1")" -eq 6 && ! grep -vx 0 "$1" >/dev/null' sh "$taskset_log"
expect_ok "tracer receives PID plus retained whole-device major/minor per cell" \
    sh -c 'test "$(wc -l <"$1")" -eq 6 && ! grep -Ev "^pid=[1-9][0-9]* major=259 minor=0 argc=4$" "$1" >/dev/null' sh "$trace_log"
expect_ok "anonymous-file nominal paths leave no named cell files" \
    sh -c 'while read -r _ _ p; do test ! -e "$p" || exit 1; done <"$1"' sh "$app_log"
expect_ok "successful run reports cleanup before the final success result" \
    bash -c 'test "$(grep -c "^CLEANUP_RESULT status=ok owned_paths=absent owned_mount=absent$" <<<"$1")" -eq 1 && test "$(tail -n 1 <<<"$1")" = "RUN_RESULT status=ok cells=6"' bash "$good"
expect_ok "successful run leaves none of its retained private directories" \
    sh -c 'while IFS= read -r p; do test ! -e "$p" && test ! -L "$p" || exit 1; done <"$1"' \
        sh "$cleanup_log"

exercise_bad_shape() {
    local mode=$1 label=$2 diagnostic=$3 rc
    : >"$cleanup_log"
    env "${run_env[@]}" EXITOS_FAKE_TRACE_MODE="$mode" \
        bash tools/cmdcmp-run.sh --test-run \
        >"$tmp/$mode.out" 2>"$tmp/$mode.err"
    rc=$?
    expect_ok "$label is rejected" test "$rc" -ne 0
    expect_ok "$label has a field-specific diagnostic" \
        grep -Fq "$diagnostic" "$tmp/$mode.err"
}
exercise_bad_shape bad-usercmd "wrong USER_CMD classification" 'usercmd=1 expected=0'
exercise_bad_shape bad-nr-phys "DMA-coalesced scattered shape" \
    'nr_phys_segments=1 after scattered-PFN proof; required 2 (DMA-coalesced shape is out of contract)'
exercise_bad_shape bad-psdt "wrong PSDT discriminator" 'psdt=1 expected=0'
exercise_bad_shape bad-sgl-type "wrong external SGL type" 'sgl_type=0x20 expected=0x30'
exercise_bad_shape bad-sgl-length "wrong external SGL list length" 'sgl_length=16 expected=32'
exercise_bad_shape bad-descriptors "wrong external SGL descriptor allocation count" \
    'iod_nr_descriptors=0 expected=1'

: >"$cleanup_log"
if env "${run_env[@]}" EXITOS_FAKE_RMDIR_MODE=work \
    bash tools/cmdcmp-run.sh --test-run \
    >"$tmp/cleanup-fail.out" 2>"$tmp/cleanup-fail.err"; then
    nok "owned work-directory cleanup failure fails a successful campaign"
else
    ok "owned work-directory cleanup failure fails a successful campaign"
fi
expect_ok "cleanup failure suppresses the final success result" \
    sh -c '! grep -q "^RUN_RESULT status=ok" "$1"' sh "$tmp/cleanup-fail.out"
expect_ok "cleanup failure is diagnosed instead of swallowed" \
    grep -q 'cleanup.*work directory' "$tmp/cleanup-fail.err"
failed_workdir=$(grep '/\.exitos-cmdcmp\.' "$cleanup_log" | tail -1)
expect_ok "failed cleanup leaves evidence rather than removing an unknown path" \
    test -d "$failed_workdir"

# Exercise mount cleanup entirely through a state-file fake.  No mount or
# block-device syscall is made: this checks failure propagation and the rule
# that an unexpected mount ID is never taken over.
mount_cleanup_bin="$tmp/mount-cleanup-bin"
mount_state="$tmp/mount.state"
mount_cleanup_log="$tmp/mount-cleanup.log"
mkdir "$mount_cleanup_bin"
printf '%s\n' '#!/bin/bash' \
    'test "$#" -eq 3 && test "$1" = -q && test "$2" = -- && test "$3" = "$FAKE_MOUNT_TARGET" || exit 92' \
    'test "$(cat "$FAKE_MOUNT_STATE")" = mounted' \
    >"$mount_cleanup_bin/mountpoint"
printf '%s\n' '#!/bin/bash' \
    'test "$#" -eq 5 && test "$1" = -n && test "$2" = -o && test "$4" = --target && test "$5" = "$FAKE_MOUNT_TARGET" || exit 93' \
    'test "$(cat "$FAKE_MOUNT_STATE")" = mounted || exit 1' \
    'case "$3" in MAJ:MIN) printf "%s\n" "$FAKE_MOUNT_MAJMIN" ;; ID) printf "%s\n" "$FAKE_MOUNT_CURRENT_ID" ;; *) exit 94 ;; esac' \
    >"$mount_cleanup_bin/findmnt"
printf '%s\n' '#!/bin/bash' \
    'test "$#" -eq 2 && test "$1" = -- && test "$2" = "$FAKE_MOUNT_TARGET" || exit 91' \
    'printf "%s\n" "$2" >>"$FAKE_MOUNT_CLEANUP_LOG"' \
    'test "${EXITOS_FAKE_UMOUNT_MODE-good}" != fail || exit 90' \
    '# Model a completed umount(2) before the helper is interrupted.' \
    'printf "%s\n" unmounted >"$FAKE_MOUNT_STATE"' \
    'if test "${EXITOS_GROUP_SIGNAL_STAGE-}" = umount; then' \
    '  runner_pgid=$("$REAL_PS_BIN" -o pgid= -p "$PPID") || exit 89' \
    '  runner_pgid=${runner_pgid//[[:space:]]/}' \
    '  "$FAKE_GROUP_SIGNAL_BIN" umount "$2" "$runner_pgid"' \
    'fi' \
    >"$mount_cleanup_bin/umount"
printf '%s\n' '#!/bin/bash' \
    'test "$#" -eq 4 && test "$1" = --source && test "$2" = "$FAKE_MOUNT_SOURCE" && test "$3" = --target && test "$4" = "$FAKE_MOUNT_TARGET" || exit 88' \
    'printf "%s\n" "$4" >>"$FAKE_MOUNT_ACQUIRE_LOG"' \
    'test "$(cat "$FAKE_MOUNT_STATE")" = unmounted || exit 87' \
    '# This state write models a successful mount(2) before userspace returns.' \
    'printf "%s\n" mounted >"$FAKE_MOUNT_STATE"' \
    'runner_pgid=$("$REAL_PS_BIN" -o pgid= -p "$PPID") || exit 86' \
    'runner_pgid=${runner_pgid//[[:space:]]/}' \
    '"$FAKE_GROUP_SIGNAL_BIN" mount "$4" "$runner_pgid"' \
    >"$mount_cleanup_bin/mount"
chmod 0700 "$mount_cleanup_bin/mountpoint" "$mount_cleanup_bin/findmnt" \
    "$mount_cleanup_bin/umount" "$mount_cleanup_bin/mount"
mount_cleanup_env=(
    "${base_env[@]}" PATH="$mount_cleanup_bin:$PATH"
    EXITOS_TESTMNT="$tmp/mnt" FAKE_MOUNT_TARGET="$tmp/mnt"
    FAKE_MOUNT_STATE="$mount_state" FAKE_MOUNT_CLEANUP_LOG="$mount_cleanup_log"
    FAKE_MOUNT_ACQUIRE_LOG="$tmp/mount-acquire.log"
    FAKE_MOUNT_SOURCE="$tmp/fake-retained-part-fd"
    FAKE_MOUNT_MAJMIN=259:1 FAKE_MOUNT_CURRENT_ID=77
    EXITOS_TEST_PART_MAJMIN=259:1 EXITOS_TEST_MOUNT_ID=77
)
printf '%s\n' mounted >"$mount_state"; : >"$mount_cleanup_log"
mount_cleanup_out=$(env "${mount_cleanup_env[@]}" \
    bash tools/cmdcmp-run.sh --test-owned-mount-cleanup \
    2>"$tmp/mount-cleanup.err")
mount_cleanup_rc=$?
expect_ok "owned fake mount cleanup succeeds" test "$mount_cleanup_rc" -eq 0
expect_ok "owned fake mount cleanup proves the target is no longer mounted" \
    sh -c 'test "$(cat "$1")" = unmounted && test "$2" = "MOUNT_CLEANUP_RESULT status=ok no_mount=proven"' \
        sh "$mount_state" "$mount_cleanup_out"
expect_ok "owned fake mount is unmounted exactly once" \
    test "$(wc -l <"$mount_cleanup_log")" -eq 1

printf '%s\n' mounted >"$mount_state"; : >"$mount_cleanup_log"
expect_fail "umount failure propagates from cleanup" \
    env "${mount_cleanup_env[@]}" EXITOS_FAKE_UMOUNT_MODE=fail \
        bash tools/cmdcmp-run.sh --test-owned-mount-cleanup
expect_ok "failed umount leaves mounted-state evidence" \
    grep -q '^mounted$' "$mount_state"

printf '%s\n' mounted >"$mount_state"; : >"$mount_cleanup_log"
expect_fail "cleanup refuses a mount whose retained ID no longer matches" \
    env "${mount_cleanup_env[@]}" FAKE_MOUNT_CURRENT_ID=78 \
        bash tools/cmdcmp-run.sh --test-owned-mount-cleanup
expect_ok "unknown mount identity is never unmounted" test ! -s "$mount_cleanup_log"

run_must_fail() {
    local label=$1 app_mode=$2 trace_mode=$3 logbase=$4
    : >"$app_log"; : >"$trace_log"; : >"$taskset_log"
    if env "${run_env[@]}" EXITOS_FAKE_APP_MODE="$app_mode" \
        EXITOS_FAKE_TRACE_MODE="$trace_mode" \
        bash tools/cmdcmp-run.sh --test-run \
        >"$tmp/$logbase.out" 2>"$tmp/$logbase.err"; then
        nok "$label"
    else
        ok "$label"
    fi
}
run_must_succeed() {
    local label=$1 trace_mode=$2 logbase=$3
    : >"$app_log"; : >"$trace_log"; : >"$taskset_log"
    if env "${run_env[@]}" EXITOS_FAKE_APP_MODE=good \
        EXITOS_FAKE_TRACE_MODE="$trace_mode" \
        bash tools/cmdcmp-run.sh --test-run \
        >"$tmp/$logbase.out" 2>"$tmp/$logbase.err"; then
        ok "$label"
    else
        nok "$label"
    fi
}
run_must_succeed "attach marker may precede a delayed exact BEGIN marker" attached-first trace-attached-first
run_must_fail "qdsplit failure fails the entire run" fail good app-fail
run_must_fail "qdsplit missing READY fails the entire run" no-ready good app-no-ready
run_must_fail "missing PFN-layout proof fails the entire run" false-proof good app-false-proof
run_must_fail "tracer missing TRACE_READY fails the entire run" good no-ready trace-no-ready
run_must_fail "wrong-PID TRACE_READY fails the entire run" good wrong-ready trace-wrong-ready
run_must_fail "missing post-attach runtime marker fails the entire run" good no-attached trace-no-attached
run_must_fail "tracer early exit fails the entire run" good early trace-early
expect_ok "dead tracer is never published as TRACE_READY" \
    sh -c '! grep -q "^TRACE_READY " "$1"' sh "$tmp/trace-early.out"
run_must_fail "tracer dead immediately after both markers fails the run" good dead-after-markers trace-dead-markers
expect_ok "completed dead tracer is rejected before TRACE_READY publication" \
    sh -c '! grep -q "^TRACE_READY " "$1"' sh "$tmp/trace-dead-markers.out"
run_must_fail "empty command trace fails the entire run" good empty trace-empty
run_must_fail "tracer stderr/nonzero status fails the entire run" good stderr trace-stderr
run_must_fail "late duplicate BEGIN marker fails the entire run" good duplicate-late trace-duplicate-late
run_must_fail "trace event for the wrong retained device fails the entire run" good wrong-dev trace-wrong-dev

# A shell trap defers only the runner's own handler.  It does not mask the
# signal in an external helper which shares the runner's foreground process
# group.  These fakes broadcast to a runner launched as its own session leader,
# exactly as Ctrl-C/TERM would, then log whether the helper lived long enough to
# return its result.  All resources are synthetic directories/state files.
real_mktemp=$(command -v mktemp)
real_stat=$(command -v stat)
real_ps=$(command -v ps)
real_setsid=$(command -v setsid)
real_env=$(command -v env)
real_timeout=$(command -v timeout)
group_signal_bin="$tmp/group-signal"
group_signal_log="$tmp/group-signal.log"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'stage=$1; resource=$2; runner_pgid=$3' \
    'test "${EXITOS_GROUP_SIGNAL_STAGE-}" = "$stage" || exit 0' \
    'test ! -e "$FAKE_GROUP_SIGNAL_ONCE" || exit 0' \
    ': >"$FAKE_GROUP_SIGNAL_ONCE"' \
    'helper_pgid=$("$REAL_PS_BIN" -o pgid= -p "$$") || exit 79' \
    'helper_pgid=${helper_pgid//[[:space:]]/}' \
    'test -z "$(trap -p INT)" && int_disposition=default || int_disposition=nondefault' \
    'test -z "$(trap -p TERM)" && term_disposition=default || term_disposition=nondefault' \
    'printf "ENTER stage=%s helper_pgid=%s runner_pgid=%s int=%s term=%s resource=%s\n" "$stage" "$helper_pgid" "$runner_pgid" "$int_disposition" "$term_disposition" "$resource" >>"$FAKE_GROUP_SIGNAL_LOG"' \
    'kill -s "$EXITOS_GROUP_SIGNAL_NAME" -- "-$runner_pgid"' \
    'sleep 0.05' \
    'printf "COMMIT stage=%s resource=%s\n" "$stage" "$resource" >>"$FAKE_GROUP_SIGNAL_LOG"' \
    'printf "HELPER_COMMIT stage=%s resource=%s\n" "$stage" "$resource" >&2' \
    >"$group_signal_bin"
chmod 0700 "$group_signal_bin"

group_signal_helpers="$tmp/group-signal-bin"
mkdir "$group_signal_helpers"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'out=$("$REAL_MKTEMP_BIN" "$@") || exit $?' \
    'printf "%s\n" "$out" >>"$FAKE_CREATED_PATH_LOG"' \
    'runner_pgid=$("$REAL_PS_BIN" -o pgid= -p "$PPID") || exit 78' \
    'runner_pgid=${runner_pgid//[[:space:]]/}' \
    '"$FAKE_GROUP_SIGNAL_BIN" mktemp "$out" "$runner_pgid"' \
    'printf "%s\n" "$out"' \
    >"$group_signal_helpers/mktemp"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'target=${!#}' \
    '"$REAL_CHMOD_BIN" "$@" || exit $?' \
    'runner_pgid=$("$REAL_PS_BIN" -o pgid= -p "$PPID") || exit 77' \
    'runner_pgid=${runner_pgid//[[:space:]]/}' \
    '"$FAKE_GROUP_SIGNAL_BIN" chmod "$target" "$runner_pgid"' \
    >"$group_signal_helpers/chmod"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'target=${!#}' \
    'out=$("$REAL_STAT_BIN" "$@") || exit $?' \
    'stage=' \
    'case "${EXITOS_GROUP_SIGNAL_STAGE-}:$target" in' \
    '  stat-work:*/.exitos-cmdcmp.*) stage=stat-work ;;' \
    '  fd-stat:/proc/*/fd/*) stage=fd-stat ;;' \
    'esac' \
    'if test -n "$stage"; then' \
    '  runner_pgid=$("$REAL_PS_BIN" -o pgid= -p "$PPID") || exit 76' \
    '  runner_pgid=${runner_pgid//[[:space:]]/}' \
    '  "$FAKE_GROUP_SIGNAL_BIN" "$stage" "$target" "$runner_pgid"' \
    'fi' \
    'printf "%s\n" "$out"' \
    >"$group_signal_helpers/stat"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'target=${!#}' \
    'printf "%s\n" "$target" >>"$FAKE_CLEANUP_LOG"' \
    '"$REAL_RMDIR_BIN" "$@" || exit $?' \
    'runner_pgid=$("$REAL_PS_BIN" -o pgid= -p "$PPID") || exit 75' \
    'runner_pgid=${runner_pgid//[[:space:]]/}' \
    '"$FAKE_GROUP_SIGNAL_BIN" rmdir "$target" "$runner_pgid"' \
    >"$group_signal_helpers/rmdir"
chmod 0700 "$group_signal_helpers/mktemp" "$group_signal_helpers/chmod" \
    "$group_signal_helpers/stat" "$group_signal_helpers/rmdir"

group_common_env=(
    "${base_env[@]}" PATH="$group_signal_helpers:$tmp/bin:$PATH"
    FAKE_GROUP_SIGNAL_BIN="$group_signal_bin"
    FAKE_GROUP_SIGNAL_LOG="$group_signal_log" REAL_PS_BIN="$real_ps"
    REAL_MKTEMP_BIN="$real_mktemp" REAL_CHMOD_BIN="$real_chmod"
    REAL_STAT_BIN="$real_stat" REAL_RMDIR_BIN="$real_rmdir"
    FAKE_CLEANUP_LOG="$cleanup_log"
)
group_foreign_sentinel="$tmp/mnt/DO_NOT_TOUCH_GROUP_SIGNAL"
printf '%s\n' GROUP_FOREIGN_KEEP >"$group_foreign_sentinel"

exercise_group_acquire_signal() {
    local stage=$1 signal=$2 expected=$3 marker created log
    local rc path enter
    marker="$tmp/group-$stage.once"
    created="$tmp/group-$stage-created.log"
    log="$tmp/group-$stage.out"
    : >"$group_signal_log"; : >"$created"; : >"$cleanup_log"
    "$real_env" --default-signal=INT --default-signal=TERM -- \
        "$real_timeout" -k 1 5 "$real_setsid" env "${group_common_env[@]}" \
        EXITOS_TESTMNT="$tmp/mnt" EXITOS_GROUP_SIGNAL_STAGE="$stage" \
        EXITOS_GROUP_SIGNAL_NAME="$signal" FAKE_GROUP_SIGNAL_ONCE="$marker" \
        FAKE_CREATED_PATH_LOG="$created" \
        bash tools/cmdcmp-run.sh --test-owned-dir-retained >"$log" 2>&1
    rc=$?
    path=$(head -1 "$created")
    enter=$(grep "^ENTER stage=$stage " "$group_signal_log" || true)
    expect_ok "group $signal during $stage returns $expected after cleanup" \
        test "$rc" -eq "$expected"
    expect_ok "group $signal during $stage reached the external helper window" \
        test -n "$enter"
    expect_ok "group $signal isolates the $stage helper from the runner process group" \
        bash -c '[[ $1 =~ helper_pgid=([0-9]+)[[:space:]]runner_pgid=([0-9]+) ]] && test "${BASH_REMATCH[1]}" != "${BASH_REMATCH[2]}"' bash "$enter"
    expect_ok "isolated $stage helper retains default INT/TERM dispositions" \
        bash -c '[[ $1 == *" int=default term=default "* ]]' bash "$enter"
    expect_ok "group $signal lets the $stage helper commit its return value" \
        grep -q "^COMMIT stage=$stage " "$group_signal_log"
    expect_ok "group $signal during $stage leaves no acquired directory" \
        bash -c 'test -n "$1" && test ! -e "$1" && test ! -L "$1"' bash "$path"
    expect_ok "group $signal during $stage reports cleanup only after completion" \
        bash -c 'commit=$(grep -n "^HELPER_COMMIT stage=$2 " "$1" | head -1 | cut -d: -f1); result=$(grep -n "^SIGNAL_RESULT signal=$3 exit=$4 cleanup=complete$" "$1" | head -1 | cut -d: -f1); test -n "$commit" && test -n "$result" && test "$commit" -lt "$result"' \
            bash "$log" "$stage" "$signal" "$expected"
}
exercise_group_acquire_signal mktemp TERM 143
exercise_group_acquire_signal chmod INT 130
exercise_group_acquire_signal stat-work TERM 143

# The retained fd is opened before the identity-stat result is committed.  A
# group signal must not kill that stat helper; its post-syscall COMMIT plus the
# final cleanup proof show the descriptor acquisition reached a stable point.
fd_group_source="$tmp/group-fd-source"
printf '%s\n' FD_SENTINEL >"$fd_group_source"
: >"$group_signal_log"
fd_group_marker="$tmp/group-fd.once"
"$real_env" --default-signal=INT --default-signal=TERM -- \
    "$real_timeout" -k 1 5 "$real_setsid" env "${group_common_env[@]}" \
    EXITOS_TEST_WHOLE_PATH="$fd_group_source" \
    EXITOS_DEVICE_SYSFS_ROOT="$fake_device_sys" EXITOS_TEST_WHOLE_MAJMIN=259:0 \
    EXITOS_GROUP_SIGNAL_STAGE=fd-stat EXITOS_GROUP_SIGNAL_NAME=INT \
    FAKE_GROUP_SIGNAL_ONCE="$fd_group_marker" FAKE_CREATED_PATH_LOG="$tmp/fd-created.log" \
    bash tools/cmdcmp-run.sh --test-retained-whole \
    >"$tmp/group-fd.out" 2>&1
fd_group_rc=$?
fd_group_enter=$(grep '^ENTER stage=fd-stat ' "$group_signal_log" || true)
expect_ok "group INT after retained-fd open returns 130 after cleanup" \
    test "$fd_group_rc" -eq 130
expect_ok "retained-fd post-open identity helper runs outside the runner group" \
    bash -c '[[ $1 =~ helper_pgid=([0-9]+)[[:space:]]runner_pgid=([0-9]+) ]] && test "${BASH_REMATCH[1]}" != "${BASH_REMATCH[2]}"' bash "$fd_group_enter"
expect_ok "retained-fd stat helper retains default INT/TERM dispositions" \
    bash -c '[[ $1 == *" int=default term=default "* ]]' bash "$fd_group_enter"
expect_ok "retained-fd identity proof commits after the group signal" \
    bash -c 'grep -q "^COMMIT stage=fd-stat " "$1" && proof=$(grep -n "^RETAINED_WHOLE_IDENTITY_COMMITTED$" "$2" | cut -d: -f1); result=$(grep -n "^SIGNAL_RESULT signal=INT exit=130 cleanup=complete$" "$2" | cut -d: -f1); helper=$(grep -n "^HELPER_COMMIT stage=fd-stat " "$2" | cut -d: -f1); test -n "$proof" && test -n "$result" && test -n "$helper" && test "$helper" -lt "$proof" && test "$proof" -lt "$result"' \
        bash "$group_signal_log" "$tmp/group-fd.out"
expect_ok "retained-fd signal cleanup preserves the unowned source" \
    grep -q '^FD_SENTINEL$' "$fd_group_source"
expect_ok "retained-fd signal is reported after descriptor cleanup" \
    grep -q '^SIGNAL_RESULT signal=INT exit=130 cleanup=complete$' "$tmp/group-fd.out"

# Fake mount writes mounted before broadcasting, modeling the syscall-to-ID
# retention window.  Fake umount broadcasts before clearing that state.  Both
# helpers must finish in an isolated session and cleanup must prove unmounted.
printf '%s\n' RETAINED_FD >"$tmp/fake-retained-part-fd"
: >"$tmp/mount-acquire.log"; : >"$mount_cleanup_log"; : >"$group_signal_log"
printf '%s\n' unmounted >"$mount_state"
mount_group_marker="$tmp/group-mount.once"
"$real_env" --default-signal=INT --default-signal=TERM -- \
    "$real_timeout" -k 1 5 "$real_setsid" env "${mount_cleanup_env[@]}" \
    PATH="$mount_cleanup_bin:$group_signal_helpers:$PATH" \
    FAKE_GROUP_SIGNAL_BIN="$group_signal_bin" FAKE_GROUP_SIGNAL_LOG="$group_signal_log" \
    REAL_PS_BIN="$real_ps" REAL_STAT_BIN="$real_stat" REAL_CHMOD_BIN="$real_chmod" \
    REAL_MKTEMP_BIN="$real_mktemp" REAL_RMDIR_BIN="$real_rmdir" \
    EXITOS_GROUP_SIGNAL_STAGE=mount EXITOS_GROUP_SIGNAL_NAME=TERM \
    FAKE_GROUP_SIGNAL_ONCE="$mount_group_marker" FAKE_CREATED_PATH_LOG="$tmp/mount-created.log" \
    EXITOS_TEST_PART_FD_PATH="$tmp/fake-retained-part-fd" \
    bash tools/cmdcmp-run.sh --test-owned-mount-acquire \
    >"$tmp/group-mount.out" 2>&1
mount_group_rc=$?
mount_group_enter=$(grep '^ENTER stage=mount ' "$group_signal_log" || true)
expect_ok "group TERM after fake mount returns 143 after cleanup" test "$mount_group_rc" -eq 143
expect_ok "fake mount syscall completed before its group signal" \
    test "$(wc -l <"$tmp/mount-acquire.log")" -eq 1
expect_ok "mount helper runs outside the runner process group" \
    bash -c '[[ $1 =~ helper_pgid=([0-9]+)[[:space:]]runner_pgid=([0-9]+) ]] && test "${BASH_REMATCH[1]}" != "${BASH_REMATCH[2]}"' bash "$mount_group_enter"
expect_ok "isolated mount helper retains default INT/TERM dispositions" \
    bash -c '[[ $1 == *" int=default term=default "* ]]' bash "$mount_group_enter"
expect_ok "mount helper commits and owned cleanup proves unmounted" \
    bash -c 'grep -q "^COMMIT stage=mount " "$1" && test "$(cat "$2")" = unmounted && test "$(wc -l <"$3")" -eq 1' \
        bash "$group_signal_log" "$mount_state" "$mount_cleanup_log"
expect_ok "mount-window signal is reported only after cleanup" \
    bash -c 'helper=$(grep -n "^HELPER_COMMIT stage=mount " "$1" | cut -d: -f1); result=$(grep -n "^SIGNAL_RESULT signal=TERM exit=143 cleanup=complete$" "$1" | cut -d: -f1); test -n "$helper" && test -n "$result" && test "$helper" -lt "$result"' \
        bash "$tmp/group-mount.out"

: >"$group_signal_log"; : >"$mount_cleanup_log"
printf '%s\n' mounted >"$mount_state"
umount_group_marker="$tmp/group-umount.once"
"$real_env" --default-signal=INT --default-signal=TERM -- \
    "$real_timeout" -k 1 5 "$real_setsid" env "${mount_cleanup_env[@]}" \
    FAKE_GROUP_SIGNAL_BIN="$group_signal_bin" FAKE_GROUP_SIGNAL_LOG="$group_signal_log" \
    REAL_PS_BIN="$real_ps" EXITOS_GROUP_SIGNAL_STAGE=umount EXITOS_GROUP_SIGNAL_NAME=INT \
    FAKE_GROUP_SIGNAL_ONCE="$umount_group_marker" \
    bash tools/cmdcmp-run.sh --test-owned-mount-cleanup \
    >"$tmp/group-umount.out" 2>&1
umount_group_rc=$?
umount_group_enter=$(grep '^ENTER stage=umount ' "$group_signal_log" || true)
expect_ok "group INT during fake umount returns 130 after cleanup" test "$umount_group_rc" -eq 130
expect_ok "umount helper runs outside the runner process group" \
    bash -c '[[ $1 =~ helper_pgid=([0-9]+)[[:space:]]runner_pgid=([0-9]+) ]] && test "${BASH_REMATCH[1]}" != "${BASH_REMATCH[2]}"' bash "$umount_group_enter"
expect_ok "isolated umount helper retains default INT/TERM dispositions" \
    bash -c '[[ $1 == *" int=default term=default "* ]]' bash "$umount_group_enter"
expect_ok "umount helper commits and cleanup proves unmounted" \
    bash -c 'grep -q "^COMMIT stage=umount " "$1" && test "$(cat "$2")" = unmounted' \
        bash "$group_signal_log" "$mount_state"
expect_ok "umount-window signal is reported only after cleanup" \
    bash -c 'helper=$(grep -n "^HELPER_COMMIT stage=umount " "$1" | cut -d: -f1); result=$(grep -n "^SIGNAL_RESULT signal=INT exit=130 cleanup=complete$" "$1" | cut -d: -f1); test -n "$helper" && test -n "$result" && test "$helper" -lt "$result"' \
        bash "$tmp/group-umount.out"

# rmdir itself is a cleanup helper.  It broadcasts before removal; cleanup must
# survive that process-group signal, remove both private directories, and only
# then return the signal status.
group_rmdir_state="$tmp/group-rmdir-state"
mkdir "$group_rmdir_state"
: >"$group_signal_log"; : >"$cleanup_log"
group_rmdir_marker="$tmp/group-rmdir.once"
"$real_env" --default-signal=INT --default-signal=TERM -- \
    "$real_timeout" -k 1 5 "$real_setsid" env "${run_env[@]}" \
    PATH="$group_signal_helpers:$tmp/bin:$PATH" TMPDIR="$group_rmdir_state" \
    FAKE_GROUP_SIGNAL_BIN="$group_signal_bin" FAKE_GROUP_SIGNAL_LOG="$group_signal_log" \
    REAL_PS_BIN="$real_ps" REAL_MKTEMP_BIN="$real_mktemp" REAL_STAT_BIN="$real_stat" \
    REAL_CHMOD_BIN="$real_chmod" REAL_RMDIR_BIN="$real_rmdir" \
    EXITOS_GROUP_SIGNAL_STAGE=rmdir EXITOS_GROUP_SIGNAL_NAME=TERM \
    FAKE_GROUP_SIGNAL_ONCE="$group_rmdir_marker" FAKE_CREATED_PATH_LOG="$tmp/rmdir-created.log" \
    bash tools/cmdcmp-run.sh --test-run >"$tmp/group-rmdir.out" 2>&1
group_rmdir_rc=$?
group_rmdir_enter=$(grep '^ENTER stage=rmdir ' "$group_signal_log" || true)
expect_ok "group TERM during rmdir returns 143 after cleanup" test "$group_rmdir_rc" -eq 143
expect_ok "rmdir helper runs outside the runner process group" \
    bash -c '[[ $1 =~ helper_pgid=([0-9]+)[[:space:]]runner_pgid=([0-9]+) ]] && test "${BASH_REMATCH[1]}" != "${BASH_REMATCH[2]}"' bash "$group_rmdir_enter"
expect_ok "isolated rmdir helper retains default INT/TERM dispositions" \
    bash -c '[[ $1 == *" int=default term=default "* ]]' bash "$group_rmdir_enter"
expect_ok "rmdir helper commits after broadcasting to the runner group" \
    grep -q '^COMMIT stage=rmdir ' "$group_signal_log"
expect_ok "group TERM during rmdir leaves all private directories absent" \
    bash -c '! find "$1" -mindepth 1 -print -quit | grep -q . && while IFS= read -r p; do test ! -e "$p" && test ! -L "$p" || exit 1; done <"$2"' \
        bash "$group_rmdir_state" "$cleanup_log"
expect_ok "rmdir-window signal is reported only after cleanup" \
    bash -c 'helper=$(grep -n "^HELPER_COMMIT stage=rmdir " "$1" | cut -d: -f1); result=$(grep -n "^SIGNAL_RESULT signal=TERM exit=143 cleanup=complete$" "$1" | cut -d: -f1); test -n "$helper" && test -n "$result" && test "$helper" -lt "$result"' \
        bash "$tmp/group-rmdir.out"
expect_ok "process-group signal cleanup preserves neighbouring unowned content" \
    grep -q '^GROUP_FOREIGN_KEEP$' "$group_foreign_sentinel"

# A signal delivered by rmdir itself lands while cleanup is active.  Cleanup
# must defer it until every owned path is proven absent, then return 143.
signal_state_parent="$tmp/signal-cleanup-state"
mkdir "$signal_state_parent"
: >"$cleanup_log"
env "${run_env[@]}" TMPDIR="$signal_state_parent" \
    EXITOS_FAKE_RMDIR_MODE=signal-work \
    bash tools/cmdcmp-run.sh --test-run \
    >"$tmp/signal-during-cleanup.out" 2>"$tmp/signal-during-cleanup.err"
signal_cleanup_rc=$?
signal_cleanup_workdir=$(grep '/\.exitos-cmdcmp\.' "$cleanup_log" | head -1)
expect_ok "TERM during rmdir is returned only after cleanup" \
    test "$signal_cleanup_rc" -eq 143
expect_ok "TERM during rmdir reports a completed deferred cleanup" \
    grep -q '^SIGNAL_RESULT signal=TERM exit=143 cleanup=complete$' \
        "$tmp/signal-during-cleanup.err"
expect_ok "TERM during rmdir leaves neither owned work nor state directory" \
    bash -c 'test -n "$1" && test ! -e "$1" && test ! -L "$1" && ! find "$2" -mindepth 1 -print -quit | grep -q .' \
        bash "$signal_cleanup_workdir" "$signal_state_parent"

# The chmod wrapper signals after mktemp created the work directory but before
# its dev:ino:uid commit.  Acquisition must defer TERM until identity is
# committed, then cleanup that exact object without touching its neighbour.
acquire_sentinel="$tmp/mnt/DO_NOT_TOUCH_DURING_ACQUIRE"
printf '%s\n' KEEP >"$acquire_sentinel"
: >"$cleanup_log"; : >"$chmod_signal_log"
timeout -k 1 5 env "${base_env[@]}" EXITOS_TESTMNT="$tmp/mnt" \
    PATH="$tmp/bin:$PATH" FAKE_CLEANUP_LOG="$cleanup_log" \
    REAL_RMDIR_BIN="$real_rmdir" REAL_CHMOD_BIN="$real_chmod" \
    FAKE_CHMOD_SIGNAL_LOG="$chmod_signal_log" EXITOS_FAKE_CHMOD_MODE=signal-work \
    bash tools/cmdcmp-run.sh --test-owned-dir-retained \
    >"$tmp/signal-during-acquire.out" 2>"$tmp/signal-during-acquire.err"
signal_acquire_rc=$?
signal_acquire_workdir=$(head -1 "$chmod_signal_log")
expect_ok "TERM in mktemp-to-identity window returns conventional status" \
    test "$signal_acquire_rc" -eq 143
expect_ok "TERM in mktemp-to-identity window cleans the committed owned directory" \
    bash -c 'test -n "$1" && test ! -e "$1" && test ! -L "$1"' \
        bash "$signal_acquire_workdir"
expect_ok "window cleanup preserves neighbouring unowned content" \
    grep -q '^KEEP$' "$acquire_sentinel"
expect_ok "window signal is reported only after cleanup completion" \
    grep -q '^SIGNAL_RESULT signal=TERM exit=143 cleanup=complete$' \
        "$tmp/signal-during-acquire.err"

# Replace the work-directory pathname while the runner is stopped.  Cleanup
# may rmdir only the retained dev:inode:uid object, never the replacement.
path_log="$tmp/path-replacement.log"
env "${base_env[@]}" EXITOS_TESTMNT="$tmp/mnt" \
    bash tools/cmdcmp-run.sh --test-owned-dir-retained \
    >"$path_log" 2>&1 & path_pid=$!
for _ in $(seq 1 200); do
    grep -q '^WORKDIR_READY$' "$path_log" 2>/dev/null && break
    kill -0 "$path_pid" 2>/dev/null || break
    sleep 0.01
done
if grep -q '^WORKDIR_READY$' "$path_log" 2>/dev/null; then
    workdir=$(sed -n 's/^WORKDIR=//p' "$path_log" | head -1)
    saved="$tmp/original-workdir"
    victim="$tmp/replacement-victim"
    mv -- "$workdir" "$saved"
    mkdir "$victim"
    printf '%s\n' DO_NOT_REMOVE >"$victim/sentinel"
    ln -s -- "$victim" "$workdir"
    kill -CONT "$path_pid" 2>/dev/null || true
    wait "$path_pid"; path_rc=$?
    expect_ok "path replacement makes cleanup fail closed" test "$path_rc" -ne 0
    expect_ok "cleanup preserves a replacement symlink" test -L "$workdir"
    expect_ok "cleanup preserves replacement contents" \
        grep -q '^DO_NOT_REMOVE$' "$victim/sentinel"
    expect_ok "path replacement is reported as an unverified cleanup" \
        grep -q 'cleanup.*work directory.*identity' "$path_log"
else
    kill -CONT "$path_pid" 2>/dev/null || true
    wait "$path_pid" 2>/dev/null || true
    nok "owned-directory test reaches its synchronization point"
    nok "cleanup preserves a replacement symlink"
    nok "cleanup preserves replacement contents"
    nok "path replacement is reported as an unverified cleanup"
fi

exercise_signal_cleanup() {
    local signal=$1 expected=$2 label=$3 log="$tmp/signal-$1.log"
    local workdir rc
    : >"$cleanup_log"
    timeout -k 1 5 env "${base_env[@]}" EXITOS_TESTMNT="$tmp/mnt" \
        PATH="$tmp/bin:$PATH" FAKE_CLEANUP_LOG="$cleanup_log" \
        REAL_RMDIR_BIN="$real_rmdir" REAL_CHMOD_BIN="$real_chmod" \
        FAKE_CHMOD_SIGNAL_LOG="$chmod_signal_log" EXITOS_TEST_SELF_SIGNAL="$signal" \
        bash tools/cmdcmp-run.sh --test-owned-dir-retained \
        >"$log" 2>&1
    rc=$?
    workdir=$(sed -n 's/^WORKDIR=//p' "$log" | head -1)
    expect_ok "$label reaches its synchronization point" \
        sh -c 'test -n "$1" && grep -q "^WORKDIR_READY$" "$2"' sh "$workdir" "$log"
    expect_ok "$label returns signal status $expected" test "$rc" -eq "$expected"
    expect_ok "$label removes its owned work directory" \
        sh -c 'test ! -e "$1" && test ! -L "$1"' sh "$workdir"
}
exercise_signal_cleanup INT 130 "INT cleanup"
exercise_signal_cleanup TERM 143 "TERM cleanup"

expect_ok "runner never evaluates resolver output" \
    sh -c '! rg -n "^[[:space:]]*eval([[:space:]]|$)" tools/cmdcmp-run.sh'
expect_ok "runner contains no global sync command" \
    sh -c '! rg -n "^[[:space:]]*sync([[:space:]]|$)" tools/cmdcmp-run.sh'
expect_ok "runner never removes a nominal current file" \
    sh -c '! rg -n "rm[[:space:]].*(CURRENT_FILE|EXITOS_FSFILE)" tools/cmdcmp-run.sh'
expect_ok "runner creates a randomized private directory inside the mount" \
    rg -q 'capture_created_dir RUN_DIR .*\.exitos-cmdcmp\.XXXXXX' tools/cmdcmp-run.sh
expect_ok "runner checks an existing mount by MAJ:MIN" \
    rg -q 'findmnt.*MAJ:MIN' tools/cmdcmp-run.sh
expect_ok "cleanup identifies its own mount by retained mount ID" \
    sh -c 'rg -q "findmnt.*-o ID" tools/cmdcmp-run.sh && rg -q "MOUNT_ID" tools/cmdcmp-run.sh'
expect_ok "mount uses a retained partition descriptor" \
    rg -q '/proc/\$\$/fd/\$PART_FD' tools/cmdcmp-run.sh
expect_ok "runner retains the strict-resolver whole device in an open fd" \
    rg -q 'exec \{WHOLE_FD\}<"\$EXITOS_DEV"' tools/cmdcmp-run.sh
expect_ok "whole-device identity is captured from retained-fd st_rdev" \
    rg -q 'stat -Lc '\''%t:%T'\'' -- "\$WHOLE_FD_PATH"' tools/cmdcmp-run.sh
expect_ok "controller derivation starts at retained dev_t under sysfs" \
    rg -q 'DEVICE_SYSFS_ROOT/dev/block/\$WHOLE_MAJMIN' tools/cmdcmp-run.sh
expect_ok "controller derivation no longer reads the resolver pathname" \
    bash -c '! sed -n '\''/^derive_nvme_controller()/,/^}/p'\'' tools/cmdcmp-run.sh | rg -q EXITOS_DEV'
expect_ok "retained device identity is checked before and after each traced cell" \
    bash -c 'count=$(rg -c "verify_retained_devices .* (pre|post)" tools/cmdcmp-run.sh || true); test "${count:-0}" -eq 2'
expect_ok "EXITOS_ONLY is exported exactly once" \
    test "$(rg -c 'export EXITOS_ONLY=' tools/cmdcmp-run.sh)" -eq 1
expect_ok "INT is captured for deferred cleanup with conventional exit status" \
    rg -q "^trap 'capture_signal 130 INT' INT$" tools/cmdcmp-run.sh
expect_ok "TERM is captured for deferred cleanup with conventional exit status" \
    rg -q "^trap 'capture_signal 143 TERM' TERM$" tools/cmdcmp-run.sh
expect_ok "cleanup DONE is committed only after destructive cleanup operations" \
    bash -c 'body=$(sed -n '\''/^cleanup_resources()/,/^}/p'\'' tools/cmdcmp-run.sh); done_line=$(grep -n "CLEANUP_DONE=1" <<<"$body" | tail -1 | cut -d: -f1); rmdir_line=$(grep -n "rmdir --" <<<"$body" | tail -1 | cut -d: -f1); test -n "$done_line" && test -n "$rmdir_line" && test "$done_line" -gt "$rmdir_line"'
expect_ok "successful campaign uses explicit cleanup rather than its EXIT fallback" \
    rg -q '^finish_successful_run$' tools/cmdcmp-run.sh
expect_ok "unmount failure is not swallowed" \
    sh -c '! rg -n "umount.*\\|\\|[[:space:]]*true" tools/cmdcmp-run.sh'
expect_ok "resource helpers use setsid --wait and never detached -f mode" \
    sh -c 'rg -Fq '\''"$SETSID_BIN" --wait --'\'' tools/cmdcmp-run.sh && ! rg -n '\''setsid.*[[:space:]]-f([[:space:]]|$)'\'' tools/cmdcmp-run.sh'
expect_ok "captured helpers reset inherited INT/TERM ignore dispositions" \
    sh -c 'rg -q -- "--default-signal=INT" tools/cmdcmp-run.sh && rg -q -- "--default-signal=TERM" tools/cmdcmp-run.sh'

expect_ok "BEGIN emits an explicit PID-specific pre-attach marker" \
    rg -q 'TRACE_BEGIN target_pid=%d' tools/cmdcmp.bt
expect_ok "runner waits for bpftrace's post-attach marker" \
    rg -q 'Attached 3 probes' tools/cmdcmp-run.sh
expect_ok "runner publishes TRACE_READY only after the attach gate" \
    rg -q "printf 'TRACE_READY target_pid=%s" tools/cmdcmp-run.sh
expect_ok "trace filter uses the numeric positional PID" \
    rg -q 'pid == \$1' tools/cmdcmp.bt
expect_ok "trace filter also binds retained whole-device major and first_minor" \
    sh -c 'rg -q '\''disk->major == \$2'\'' tools/cmdcmp.bt && rg -q '\''disk->first_minor == \$3'\'' tools/cmdcmp.bt'
expect_ok "trace output records its matched block-device identity" \
    sh -c 'rg -q "dev_major=%" tools/cmdcmp.bt && rg -q "dev_first_minor=%" tools/cmdcmp.bt'
expect_ok "runner rejects command events outside the retained whole device" \
    rg -q 'dev_major=.*WHOLE_MAJOR.*dev_first_minor=.*WHOLE_MINOR' tools/cmdcmp-run.sh
expect_ok "v0.25 fexit arguments use current args.field syntax" \
    rg -q '\$req = args\.req;' tools/cmdcmp.bt
expect_ok "trace never relies on the comm name" \
    sh -c '! rg -n "comm[[:space:]]*==" tools/cmdcmp.bt'
expect_ok "trace records request physical segments" \
    rg -q 'nr_phys_segments' tools/cmdcmp.bt
expect_ok "trace derives USER_CMD from the nvme_request flag bit" \
    rg -q '\$usercmd = \(\$iod->req.flags & 0x2\) != 0;' tools/cmdcmp.bt
expect_ok "every trace-event format emits the explicit USER_CMD classification" \
    test "$(rg -c 'TRACE_EVENT.*usercmd=%u' tools/cmdcmp.bt)" -eq 4
expect_ok "trace records iod descriptor counts" \
    rg -q 'nr_descriptors' tools/cmdcmp.bt
expect_ok "trace does not read stale cross-tag iod nr_dma_vecs" \
    sh -c '! rg -n "nr_dma_vecs" tools/cmdcmp.bt'
expect_ok "trace labels PRP events separately" \
    rg -q 'format=PRP' tools/cmdcmp.bt
expect_ok "trace labels inline SGL data descriptors" \
    rg -q 'sgl_class=inline_data' tools/cmdcmp.bt
expect_ok "trace labels external last-segment SGL lists" \
    rg -q 'sgl_class=external_last_segment' tools/cmdcmp.bt
expect_ok "trace branches on PSDT before decoding the dptr union" \
    rg -q 'if \(\$psdt == 0\)' tools/cmdcmp.bt
expect_ok "trace counts only successfully prepared commands" \
    rg -q 'retval == 0' tools/cmdcmp.bt
expect_ok "runner requires exactly ITERS traced commands" \
    rg -q 'trace_events.*ITERS' tools/cmdcmp-run.sh
expect_ok "generic Makefile unit target discovers this shell test" \
    rg -q '^UNIT_SH[[:space:]]*:=[[:space:]]*\$\(wildcard tests/unit/test_\*\.sh\)' Makefile

finish
