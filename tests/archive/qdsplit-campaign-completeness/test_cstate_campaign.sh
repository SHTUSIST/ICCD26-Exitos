#!/bin/bash
# Pure topology/plan tests for campaign-cstate-keeper.sh.  This test never
# opens, mounts, or writes a block device.
set -u
cd "$(dirname "$0")/../../.."

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
wait_for_stopped_state() {
    local pid=$1 attempt state
    [[ $pid =~ ^[1-9][0-9]*$ ]] || return 1
    for ((attempt = 0; attempt < 200; attempt++)); do
        [ -r "/proc/$pid/status" ] || return 1
        state=$(awk '$1 == "State:" { print $2 }' "/proc/$pid/status") || return 1
        [ "$state" = T ] && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.01
    done
    return 1
}
process_exited_or_zombie() {
    local pid=$1 state
    [ ! -e "/proc/$pid/status" ] && return 0
    state=$(awk '$1 == "State:" { print $2 }' "/proc/$pid/status" 2>/dev/null) ||
        return 1
    case "$state" in Z|X|x) return 0 ;; *) return 1 ;; esac
}
wait_for_exit_or_zombie() {
    local pid=$1 attempts=$2 attempt
    for ((attempt = 0; attempt < attempts; attempt++)); do
        process_exited_or_zombie "$pid" && return 0
        sleep 0.01
    done
    process_exited_or_zombie "$pid"
}
bounded_terminate_and_wait() {
    local pid=$1 child_rc=0
    kill -TERM "$pid" 2>/dev/null || true
    kill -CONT "$pid" 2>/dev/null || true
    if ! wait_for_exit_or_zombie "$pid" 200; then
        kill -KILL "$pid" 2>/dev/null || true
        wait_for_exit_or_zombie "$pid" 200 || return 124
    fi
    wait "$pid" 2>/dev/null || child_rc=$?
    return "$child_rc"
}
bounded_resume_and_wait() {
    local pid=$1 child_rc=0
    kill -CONT "$pid" 2>/dev/null || true
    if ! wait_for_exit_or_zombie "$pid" 200; then
        kill -TERM "$pid" 2>/dev/null || true
        kill -CONT "$pid" 2>/dev/null || true
        if ! wait_for_exit_or_zombie "$pid" 200; then
            kill -KILL "$pid" 2>/dev/null || true
            wait_for_exit_or_zombie "$pid" 200 || return 124
        fi
    fi
    wait "$pid" 2>/dev/null || child_rc=$?
    return "$child_rc"
}
proc_starttime() {
    local pid=$1 line rest
    local -a fields=()
    [[ $pid =~ ^[1-9][0-9]*$ ]] || return 1
    IFS= read -r line <"/proc/$pid/stat" || return 1
    [[ $line == *") "* ]] || return 1
    rest=${line##*) }
    read -r -a fields <<<"$rest" || return 1
    [ "${#fields[@]}" -ge 20 ] || return 1
    [[ ${fields[19]} =~ ^[1-9][0-9]*$ ]] || return 1
    printf '%s\n' "${fields[19]}"
}

tmp=$(mktemp -d /tmp/exitos-cstate-plan-test.XXXXXX)
cleanup() {
    [ "${EXITOS_KEEP_TEST_TMP:-0}" = 1 ] || rm -rf -- "$tmp"
}
trap cleanup EXIT

stuck_child_ready="$tmp/stuck-child.ready"
bash -c '
    trap "" TERM
    : >"$1"
    while :; do kill -STOP "$BASHPID"; done
' bash "$stuck_child_ready" & stuck_child_pid=$!
stuck_child_started=0
for _ in $(seq 1 200); do
    if [ -e "$stuck_child_ready" ] && wait_for_stopped_state "$stuck_child_pid"; then
        stuck_child_started=1
        break
    fi
    kill -0 "$stuck_child_pid" 2>/dev/null || break
    sleep 0.01
done
stuck_child_rc=0
bounded_terminate_and_wait "$stuck_child_pid" 2>/dev/null || stuck_child_rc=$?
expect_ok "bounded paused-child cleanup kills a TERM-resistant child" \
    sh -c 'test "$1" -eq 1 && test "$2" -eq 137 && test ! -e "/proc/$3/status"' \
        sh "$stuck_child_started" "$stuck_child_rc" "$stuck_child_pid"

# Exercise the real test-disk resolver against a complete fake sysfs/dev tree.
# Regular files stand in for device nodes only when the resolver's explicit
# pure-test mode is enabled; production mode must continue to require block and
# character special files.
testdisk_sys="$tmp/testdisk-sys"
testdisk_dev="$tmp/testdisk-dev"
testdisk_nvme="$tmp/fake-nvme"
mkdir -p "$testdisk_sys/class/block/nvme0n1/device" "$testdisk_dev"
printf '%s\n' 'SERIAL-1' >"$testdisk_sys/class/block/nvme0n1/device/serial"
printf '%s\n' '7' >"$testdisk_sys/class/block/nvme0n1/nsid"
: >"$testdisk_dev/nvme0n1"
: >"$testdisk_dev/nvme0n1p1"
: >"$testdisk_dev/ng0n1"
printf '%s\n' \
    '#!/bin/bash' \
    'case "$1" in' \
    '  id-ctrl) printf "%s\n" "sn : SERIAL-1" ;;' \
    '  id-ns) exit 0 ;;' \
    '  *) exit 2 ;;' \
    'esac' >"$testdisk_nvme"
chmod 0700 "$testdisk_nvme"

testdisk_out=$(env EXITOS_TEST_SERIAL=SERIAL-1 EXITOS_TESTDISK_TEST_MODE=1 \
    EXITOS_TESTDISK_SYSFS_ROOT="$testdisk_sys" \
    EXITOS_TESTDISK_DEV_ROOT="$testdisk_dev" \
    EXITOS_TESTDISK_NVME_BIN="$testdisk_nvme" \
    bash tools/testdisk.sh 2>"$tmp/testdisk-complete.err")
expect_ok "testdisk resolves a complete fake namespace in pure mode" \
    test "$testdisk_out" = "$(printf '%s\n' \
        "export EXITOS_DEV=$testdisk_dev/nvme0n1" \
        'export EXITOS_EXPECT_SERIAL=SERIAL-1' \
        "export EXITOS_FSPART=$testdisk_dev/nvme0n1p1" \
        "export EXITOS_WINDOW_IN_PART=$testdisk_dev/nvme0n1p1" \
        "export EXITOS_TESTPART=$testdisk_dev/nvme0n1p1" \
        "export EXITOS_CHARDEV=$testdisk_dev/ng0n1")"

rm -f -- "$testdisk_dev/nvme0n1p1"
expect_fail "testdisk refuses a resolved namespace whose p1 node is absent" \
    env EXITOS_TEST_SERIAL=SERIAL-1 EXITOS_TESTDISK_TEST_MODE=1 \
        EXITOS_TESTDISK_SYSFS_ROOT="$testdisk_sys" \
        EXITOS_TESTDISK_DEV_ROOT="$testdisk_dev" \
        EXITOS_TESTDISK_NVME_BIN="$testdisk_nvme" \
        bash tools/testdisk.sh
: >"$testdisk_dev/nvme0n1p1"

rm -f -- "$testdisk_dev/nvme0n1"
expect_fail "testdisk refuses a sysfs match whose whole node is absent" \
    env EXITOS_TEST_SERIAL=SERIAL-1 EXITOS_TESTDISK_TEST_MODE=1 \
        EXITOS_TESTDISK_SYSFS_ROOT="$testdisk_sys" \
        EXITOS_TESTDISK_DEV_ROOT="$testdisk_dev" \
        EXITOS_TESTDISK_NVME_BIN="$testdisk_nvme" \
        bash tools/testdisk.sh
: >"$testdisk_dev/nvme0n1"

rm -f -- "$testdisk_dev/ng0n1"
expect_fail "testdisk refuses a namespace whose generic char node is absent" \
    env EXITOS_TEST_SERIAL=SERIAL-1 EXITOS_TESTDISK_TEST_MODE=1 \
        EXITOS_TESTDISK_SYSFS_ROOT="$testdisk_sys" \
        EXITOS_TESTDISK_DEV_ROOT="$testdisk_dev" \
        EXITOS_TESTDISK_NVME_BIN="$testdisk_nvme" \
        bash tools/testdisk.sh
: >"$testdisk_dev/ng0n1"

mkdir -p "$testdisk_sys/class/block/nvme1n1/device"
printf '%s\n' 'SERIAL-1' >"$testdisk_sys/class/block/nvme1n1/device/serial"
printf '%s\n' '9' >"$testdisk_sys/class/block/nvme1n1/nsid"
: >"$testdisk_dev/nvme1n1"
: >"$testdisk_dev/nvme1n1p1"
expect_fail "testdisk refuses duplicate whole namespaces for one serial" \
    env EXITOS_TEST_SERIAL=SERIAL-1 EXITOS_TESTDISK_TEST_MODE=1 \
        EXITOS_TESTDISK_SYSFS_ROOT="$testdisk_sys" \
        EXITOS_TESTDISK_DEV_ROOT="$testdisk_dev" \
        EXITOS_TESTDISK_NVME_BIN="$testdisk_nvme" \
        bash tools/testdisk.sh
rm -rf -- "$testdisk_sys/class/block/nvme1n1"
rm -f -- "$testdisk_dev/nvme1n1" "$testdisk_dev/nvme1n1p1"

: >"$testdisk_dev/ng1n1"
expect_fail "testdisk refuses duplicate generic char candidates" \
    env EXITOS_TEST_SERIAL=SERIAL-1 EXITOS_TESTDISK_TEST_MODE=1 \
        EXITOS_TESTDISK_SYSFS_ROOT="$testdisk_sys" \
        EXITOS_TESTDISK_DEV_ROOT="$testdisk_dev" \
        EXITOS_TESTDISK_NVME_BIN="$testdisk_nvme" \
        bash tools/testdisk.sh
rm -f -- "$testdisk_dev/ng1n1"

mkdir -p "$tmp/sys/cpu0/topology" "$tmp/sys/cpu1/topology" \
    "$tmp/sys/cpu2/topology" "$tmp/sys/cpu4/topology"
printf '%s\n' '0,4' >"$tmp/sys/cpu0/topology/thread_siblings_list"
printf '%s\n' '0,4' >"$tmp/sys/cpu4/topology/thread_siblings_list"
printf '%s\n' '1'   >"$tmp/sys/cpu1/topology/thread_siblings_list"
printf '%s\n' '2'   >"$tmp/sys/cpu2/topology/thread_siblings_list"
printf '%s\n' 0 >"$tmp/sys/cpu0/topology/physical_package_id"
printf '%s\n' 0 >"$tmp/sys/cpu4/topology/physical_package_id"
printf '%s\n' 0 >"$tmp/sys/cpu1/topology/physical_package_id"
printf '%s\n' 1 >"$tmp/sys/cpu2/topology/physical_package_id"
printf '%s\n' 0 >"$tmp/sys/cpu0/topology/core_id"
printf '%s\n' 0 >"$tmp/sys/cpu4/topology/core_id"
printf '%s\n' 1 >"$tmp/sys/cpu1/topology/core_id"
printf '%s\n' 0 >"$tmp/sys/cpu2/topology/core_id"
printf '%s\n' '0-2,4' >"$tmp/sys/online"

# The primary campaign runs only inside the fixed-max core transaction.  Its
# own readback gate independently retains and rechecks every unique cpufreq
# policy represented by the target CPU and its SMT sibling.
policy0="$tmp/sys/cpufreq/policy0"
policy4="$tmp/sys/cpufreq/policy4"
mkdir -p "$policy0" "$policy4"
ln -s -- "$policy0" "$tmp/sys/cpu0/cpufreq"
ln -s -- "$policy4" "$tmp/sys/cpu4/cpufreq"
for policy_spec in "$policy0:0" "$policy4:4"; do
    policy_path=${policy_spec%:*}
    policy_cpu=${policy_spec##*:}
    printf '%s\n' "$policy_cpu" >"$policy_path/related_cpus"
    printf '%s\n' performance >"$policy_path/scaling_governor"
    printf '%s\n' performance >"$policy_path/energy_performance_preference"
    printf '%s\n' 3800000 >"$policy_path/scaling_min_freq"
    printf '%s\n' 3800000 >"$policy_path/scaling_max_freq"
done
policy0_id=$(stat -Lc '%d:%i' -- "$policy0")
policy4_id=$(stat -Lc '%d:%i' -- "$policy4")
expected_cpufreq_profile="fixed-max-3800000;policy4@$policy4_id@related=4@governor=performance@epp=performance@min=3800000@max=3800000;policy0@$policy0_id@related=0@governor=performance@epp=performance@min=3800000@max=3800000"
expected_execution_contract="selected_cpu=4@policy=SCHED_OTHER@nice=-20"
expected_core_util_contract="selected_cpu=4@selected_max_bp=100@sibling_cpu=0@sibling_max_bp=100@windows=2@threshold_lt_pct=15@threshold_exceeded=no"

# Pure profile probes exercise identity/scope handling without touching the
# device/mount campaign.  A shared policy must be counted exactly once.
ln -sfn -- "$policy0" "$tmp/sys/cpu4/cpufreq"
printf '%s\n' '0,4' >"$policy0/related_cpus"
shared_profile=$(env CPU=4 REPS=1 ITERS=1 SETTLE=0 \
    EXITOS_CAMPAIGN_TEST_MODE=1 EXITOS_CPU_SYSFS_ROOT="$tmp/sys" \
    bash tools/campaign-cstate-keeper.sh --test-cpufreq-profile 0 \
    2>"$tmp/shared-profile.err")
shared_profile_rc=$?
expected_shared_profile="fixed-max-3800000;policy0@$policy0_id@related=0,4@governor=performance@epp=performance@min=3800000@max=3800000"
expect_ok "shared sibling cpufreq policy is retained exactly once" \
    test "$shared_profile_rc:$shared_profile" = \
        "0:CPUFREQ_PROFILE policies=1 token=$expected_shared_profile"
ln -sfn -- "$policy4" "$tmp/sys/cpu4/cpufreq"
printf '%s\n' 0 >"$policy0/related_cpus"

outside_policy="$tmp/outside-policy"
mkdir "$outside_policy"
ln -sfn -- "$outside_policy" "$tmp/sys/cpu4/cpufreq"
env CPU=4 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" \
    bash tools/campaign-cstate-keeper.sh --test-cpufreq-profile 0 \
    >"$tmp/noncanonical-profile.out" 2>"$tmp/noncanonical-profile.err"
noncanonical_profile_rc=$?
expect_ok "noncanonical cpufreq policy mapping is rejected" \
    sh -c 'test "$1" -ne 0 && grep -q "noncanonical" "$2"' \
        sh "$noncanonical_profile_rc" "$tmp/noncanonical-profile.err"
ln -sfn -- "$policy4" "$tmp/sys/cpu4/cpufreq"

profile_scope_log="$tmp/profile-scope.log"
env CPU=4 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_TEST_CPUFREQ_PAUSE=1 \
    bash tools/campaign-cstate-keeper.sh --test-cpufreq-profile 0 \
    >"$profile_scope_log" 2>&1 & profile_scope_pid=$!
for _ in $(seq 1 100); do
    grep -q '^CPUFREQ_PROFILE_READY$' "$profile_scope_log" 2>/dev/null && break
    kill -0 "$profile_scope_pid" 2>/dev/null || break
    sleep 0.01
done
profile_scope_reached=0
profile_scope_rc=0
if grep -q '^CPUFREQ_PROFILE_READY$' "$profile_scope_log" 2>/dev/null; then
    profile_scope_reached=1
    printf '%s\n' '0,4' >"$policy4/related_cpus"
    kill -CONT "$profile_scope_pid"
fi
if wait "$profile_scope_pid" 2>/dev/null; then profile_scope_rc=0; else profile_scope_rc=$?; fi
printf '%s\n' 4 >"$policy4/related_cpus"
expect_ok "retained cpufreq related_cpus drift is rejected" \
    sh -c 'test "$1" -eq 1 && test "$2" -ne 0 && grep -q "scope changed" "$3"' \
        sh "$profile_scope_reached" "$profile_scope_rc" "$profile_scope_log"

profile_identity_log="$tmp/profile-identity.log"
env CPU=4 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_TEST_CPUFREQ_PAUSE=1 \
    bash tools/campaign-cstate-keeper.sh --test-cpufreq-profile 0 \
    >"$profile_identity_log" 2>&1 & profile_identity_pid=$!
for _ in $(seq 1 100); do
    grep -q '^CPUFREQ_PROFILE_READY$' "$profile_identity_log" 2>/dev/null && break
    kill -0 "$profile_identity_pid" 2>/dev/null || break
    sleep 0.01
done
profile_identity_reached=0
profile_identity_rc=0
if grep -q '^CPUFREQ_PROFILE_READY$' "$profile_identity_log" 2>/dev/null; then
    profile_identity_reached=1
    mv -- "$policy4" "$policy4.retained"
    mkdir "$policy4"
    kill -CONT "$profile_identity_pid"
fi
if wait "$profile_identity_pid" 2>/dev/null; then profile_identity_rc=0; else profile_identity_rc=$?; fi
if [ -d "$policy4.retained" ]; then
    rmdir -- "$policy4"
    mv -- "$policy4.retained" "$policy4"
fi
expect_ok "retained cpufreq policy inode replacement is rejected" \
    sh -c 'test "$1" -eq 1 && test "$2" -ne 0 && grep -q "identity changed" "$3"' \
        sh "$profile_identity_reached" "$profile_identity_rc" "$profile_identity_log"

policy9="$tmp/sys/cpufreq/policy9"
mkdir "$policy9"
printf '%s\n' 4 >"$policy9/related_cpus"
printf '%s\n' performance >"$policy9/scaling_governor"
printf '%s\n' performance >"$policy9/energy_performance_preference"
printf '%s\n' 3800000 >"$policy9/scaling_min_freq"
printf '%s\n' 3800000 >"$policy9/scaling_max_freq"
profile_mapping_log="$tmp/profile-mapping.log"
env CPU=4 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_TEST_CPUFREQ_PAUSE=1 \
    bash tools/campaign-cstate-keeper.sh --test-cpufreq-profile 0 \
    >"$profile_mapping_log" 2>&1 & profile_mapping_pid=$!
for _ in $(seq 1 100); do
    grep -q '^CPUFREQ_PROFILE_READY$' "$profile_mapping_log" 2>/dev/null && break
    kill -0 "$profile_mapping_pid" 2>/dev/null || break
    sleep 0.01
done
profile_mapping_reached=0
profile_mapping_rc=0
if grep -q '^CPUFREQ_PROFILE_READY$' "$profile_mapping_log" 2>/dev/null; then
    profile_mapping_reached=1
    ln -sfn -- "$policy9" "$tmp/sys/cpu4/cpufreq"
    kill -CONT "$profile_mapping_pid"
fi
if wait "$profile_mapping_pid" 2>/dev/null; then profile_mapping_rc=0; else profile_mapping_rc=$?; fi
ln -sfn -- "$policy4" "$tmp/sys/cpu4/cpufreq"
expect_ok "retained cpufreq unique policy mapping drift is rejected" \
    sh -c 'test "$1" -eq 1 && test "$2" -ne 0 && grep -q "mapping changed" "$3"' \
        sh "$profile_mapping_reached" "$profile_mapping_rc" "$profile_mapping_log"

topology() {
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS="$1" \
        bash tools/cstate-topology.sh "$2"
}

out=$(topology '0-2,4' 0 2>"$tmp/err")
expect_ok "range-form allowed list is accepted" test "$out" = $'0\t4\t1\tsame-package'

out=$(topology '0,2,4' 0 2>"$tmp/err")
expect_ok "placebo falls back to a different package" \
    test "$out" = $'0\t4\t2\tother-package'

expect_fail "target CPU outside allowed list is refused" topology '1-4' 0
expect_fail "missing allowed SMT sibling is refused" topology '0-2' 0
expect_fail "missing different physical core placebo is refused" topology '0,4' 0
expect_fail "malformed CPU ranges are refused" topology '0-2,8-,4' 0

printf '%s\n' '0-2' >"$tmp/sys/online"
expect_fail "offline SMT sibling is refused" topology '0-2,4' 0
printf '%s\n' '0,4' >"$tmp/sys/online"
expect_fail "offline different-core placebo is refused" topology '0-2,4' 0
printf '%s\n' '1,2,4' >"$tmp/sys/online"
expect_fail "offline measurement CPU is refused" topology '0-2,4' 0
printf '%s\n' '0-2,4' >"$tmp/sys/online"

plan=$(CPU=0 REPS=5 EXITOS_KEEPER_MODES=off,same \
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS='0-2,4' \
    EXITOS_QDSPLIT_BIN=/definitely/missing/qdsplit \
    EXITOS_KEEPER_BIN=/definitely/missing/keeper \
    EXITOS_TESTDISK=/definitely/missing/testdisk \
    EXITOS_ONLY=-1 \
    EXITOS_TEST_POLL_PROC_ROOT=/poison/poll-proc \
    EXITOS_TEST_BLKGETDISKSEQ_FILE=/poison/poll-diskseq \
    EXITOS_TEST_POLL_PAUSE_BEFORE=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER=1 \
    EXITOS_TEST_POLL_HELPER_PAUSE=1 \
    EXITOS_TEST_POLL_PAUSE_BEFORE_ALIAS_OPEN=1 \
    EXITOS_TEST_POLL_PAUSE_BEFORE_DEVICE_LINK_OPEN=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER_MQ_LIST=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1=1 \
    EXITOS_TEST_POLL_PARTIAL_OUTPUT=1 \
    bash tools/campaign-cstate-keeper.sh --plan 2>"$tmp/plan.err")
expect_ok "plan mode exits before checking benchmark/device helpers" test $? -eq 0
expect_ok "plan identifies same-package placebo" \
    grep -q '^PLAN cpu=0 sibling=4 placebo=1 placebo_scope=same-package$' <<<"$plan"
expect_ok "plan explicitly clears inherited EXITOS_ONLY" \
    grep -q '^PLAN_ENV EXITOS_ONLY=unset$' <<<"$plan"
expect_ok "plan records the local-core load and highest normal-priority contract" \
    grep -q '^PLAN_EXECUTION selected_cpu=0 sibling_cpu=4 prelaunch_windows=30 threshold_lt_pct=15 policy=SCHED_OTHER nice=-20$' <<<"$plan"
expect_ok "plan declares the fixed-max profile as a required precondition" \
    grep -q '^PLAN_SCREEN required_profile=fixed-max-3800000 keeper_modes=off,same cells=20$' <<<"$plan"
expect_ok "fixed-max primary plan has exactly twenty cells" \
    test "$(grep -c '^CELL ' <<<"$plan")" -eq 20

for mode in off same; do
    count=$(grep -c "keeper=$mode " <<<"$plan")
    expect_ok "plan has both buffers for five repetitions: $mode" test "$count" -eq 10
done
expect_ok "primary plan excludes sibling and placebo keeper cells" \
    bash -c '! grep -Eq "keeper=(sibling|placebo) " <<<"$1"' bash "$plan"
expect_ok "off arm has no keeper CPU" \
    grep -q '^CELL rep=1 buffer=normal keeper=off keeper_cpu=-$' <<<"$plan"
expect_ok "same arm targets the measurement CPU" \
    grep -q '^CELL rep=1 buffer=normal keeper=same keeper_cpu=0$' <<<"$plan"

opt_plan=$(CPU=0 REPS=5 EXITOS_KEEPER_MODES=off,same \
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS='0-2,4' \
    EXITOS_QDSPLIT_BIN=/definitely/missing/qdsplit \
    EXITOS_KEEPER_BIN=/definitely/missing/keeper \
    EXITOS_TESTDISK=/definitely/missing/testdisk \
    EXITOS_ONLY=-1 \
    EXITOS_TEST_POLL_PROC_ROOT=/poison/poll-proc \
    EXITOS_TEST_BLKGETDISKSEQ_FILE=/poison/poll-diskseq \
    EXITOS_TEST_POLL_PAUSE_BEFORE=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER=1 \
    EXITOS_TEST_POLL_HELPER_PAUSE=1 \
    EXITOS_TEST_POLL_PAUSE_BEFORE_ALIAS_OPEN=1 \
    EXITOS_TEST_POLL_PAUSE_BEFORE_DEVICE_LINK_OPEN=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER_MQ_LIST=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1=1 \
    EXITOS_TEST_POLL_PARTIAL_OUTPUT=1 \
    bash tools/campaign-cstate-keeper.sh --opt-plan 2>"$tmp/opt-plan.err")
opt_plan_rc=$?
expect_ok "opt plan is an explicit production CLI and needs no device helpers" \
    test "$opt_plan_rc" -eq 0
expect_ok "opt plan declares its screen and exact twenty-cell design" \
    grep -q '^PLAN_SCREEN screen=opt required_profile=fixed-max-3800000 keeper_modes=off,same cells=20$' \
        <<<"$opt_plan"
expect_ok "opt plan declares qdsplit opt and all five selected arms" \
    grep -q '^PLAN_ARMS EXITOS_PAIRED=opt arms=0:fs-8k-qd1,1:pt-8k-qd1,15:pt-8k-fixedbuf,16:pt-8k-bounce,17:pt-8k-bounce-fixed$' \
        <<<"$opt_plan"
expect_ok "opt plan has exactly the same five-by-two-by-two cells" \
    test "$(grep -c '^CELL ' <<<"$opt_plan")" -eq 20
expect_ok "opt plan cell schedule matches the primary screen" \
    test "$(grep '^CELL ' <<<"$opt_plan")" = "$(grep '^CELL ' <<<"$plan")"
expect_ok "default primary plan remains byte-for-byte unchanged by opt selection" \
    test "$(grep '^PLAN_SCREEN ' <<<"$plan")" = \
        'PLAN_SCREEN required_profile=fixed-max-3800000 keeper_modes=off,same cells=20'
for bad_reps in 1 4 6; do
    expect_fail "production plan rejects non-primary REPS=$bad_reps" \
        env CPU=0 REPS="$bad_reps" EXITOS_KEEPER_MODES=off,same \
            EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS='0-2,4' \
            bash tools/campaign-cstate-keeper.sh --plan
done
for bad_reps in 1 4 6; do
    expect_fail "production opt plan rejects non-primary REPS=$bad_reps" \
        env CPU=0 REPS="$bad_reps" EXITOS_KEEPER_MODES=off,same \
            EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS='0-2,4' \
            bash tools/campaign-cstate-keeper.sh --opt-plan
done
for incomplete_modes in off same same,off 'off,same,'; do
    expect_fail "production plan requires exact off,same modes: $incomplete_modes" \
        env CPU=0 REPS=5 EXITOS_KEEPER_MODES="$incomplete_modes" \
            EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS='0-2,4' \
            bash tools/campaign-cstate-keeper.sh --plan
    expect_fail "production opt plan requires exact off,same modes: $incomplete_modes" \
        env CPU=0 REPS=5 EXITOS_KEEPER_MODES="$incomplete_modes" \
            EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS='0-2,4' \
            bash tools/campaign-cstate-keeper.sh --opt-plan
done
for bad_modes in sibling placebo off,sibling same,placebo off,off same,same garbage; do
    expect_fail "keeper filter rejects non-primary or duplicate modes: $bad_modes" \
        env CPU=0 REPS=5 EXITOS_KEEPER_MODES="$bad_modes" \
            EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS='0-2,4' \
            bash tools/campaign-cstate-keeper.sh --plan
done

# Production execution must enter its own recursively-private mount namespace
# before it can inspect devices or create any mount/output state.  unshare and
# stat are fakes here; no namespace syscall is issued by this unit test.
namespace_bin="$tmp/namespace-bin"
namespace_log="$tmp/unshare.log"
namespace_out="$tmp/namespace-result.tsv"
namespace_mnt="$tmp/namespace-mnt"
mkdir "$namespace_bin"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" "$*" >"$FAKE_UNSHARE_LOG"' \
    'exit 73' >"$namespace_bin/unshare"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" 1:1' >"$namespace_bin/stat"
chmod 0700 "$namespace_bin/unshare" "$namespace_bin/stat"
env CPU=0 REPS=5 ITERS=1 SETTLE=0 PATH="$namespace_bin:$PATH" \
    FAKE_UNSHARE_LOG="$namespace_log" OUT="$namespace_out" \
    EXITOS_TESTMNT="$namespace_mnt" \
    bash tools/campaign-cstate-keeper.sh \
    >"$tmp/namespace.out" 2>"$tmp/namespace.err"
namespace_rc=$?
expect_ok "production run enters private namespace before device or output side effects" \
    sh -c 'test "$1" -eq 73 && test "$2" = "--mount --propagation private -- bash $3/tools/campaign-cstate-keeper.sh --private-mount-namespace-run" && test ! -e "$4" && test ! -e "$5"' \
        sh "$namespace_rc" "$(cat "$namespace_log")" "$PWD" \
        "$namespace_out" "$namespace_mnt"

rm -f -- "$namespace_log" "$namespace_out"
env CPU=0 REPS=5 ITERS=1 SETTLE=0 PATH="$namespace_bin:$PATH" \
    FAKE_UNSHARE_LOG="$namespace_log" OUT="$namespace_out" \
    EXITOS_TESTMNT="$namespace_mnt" \
    bash tools/campaign-cstate-keeper.sh --opt-run \
    >"$tmp/opt-namespace.out" 2>"$tmp/opt-namespace.err"
opt_namespace_rc=$?
expect_ok "production opt run retains screen identity across private namespace entry" \
    sh -c 'test "$1" -eq 73 && test "$2" = "--mount --propagation private -- bash $3/tools/campaign-cstate-keeper.sh --private-mount-namespace-opt-run" && test ! -e "$4" && test ! -e "$5"' \
        sh "$opt_namespace_rc" "$(cat "$namespace_log")" "$PWD" \
        "$namespace_out" "$namespace_mnt"

env CPU=0 REPS=5 ITERS=1 SETTLE=0 PATH="$namespace_bin:$PATH" \
    OUT="$namespace_out" EXITOS_TESTMNT="$namespace_mnt" \
    bash tools/campaign-cstate-keeper.sh --private-mount-namespace-run \
    >"$tmp/direct-namespace.out" 2>"$tmp/direct-namespace.err"
direct_namespace_rc=$?
expect_ok "private namespace internal entry cannot be invoked in the caller namespace" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && test ! -e "$3" && grep -q "did not create a new namespace" "$4"' \
        sh "$direct_namespace_rc" "$namespace_out" "$namespace_mnt" \
        "$tmp/direct-namespace.err"

for bad in 0 -1 nope 1001; do
    expect_fail "invalid REPS is rejected before plan: $bad" \
        env CPU=0 REPS="$bad" EXITOS_CPU_SYSFS_ROOT="$tmp/sys" \
            EXITOS_ALLOWED_CPUS='0-2,4' \
            bash tools/campaign-cstate-keeper.sh --plan
done
for assignment in 'CPU=-1' 'CPU=nope' 'CPU=1024' 'ITERS=0' \
                  'ITERS=nope' 'ITERS=1000001' 'SETTLE=-1' \
                  'SETTLE=nope' 'SETTLE=3601'; do
    expect_fail "invalid campaign integer is rejected: $assignment" \
        env CPU=0 REPS=1 ITERS=1 SETTLE=0 \
            EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS='0-2,4' \
            "$assignment" bash tools/campaign-cstate-keeper.sh --plan
done

# Controller fixture overrides are an all-or-nothing pure-test interface.  A
# production run must reject each name before invoking even the resolver (and
# therefore before any device open or qdsplit launch).
controller_boundary_resolver="$tmp/controller-boundary-resolver"
controller_boundary_qdsplit="$tmp/controller-boundary-qdsplit"
controller_boundary_resolver_ran="$tmp/controller-boundary-resolver.ran"
controller_boundary_qdsplit_ran="$tmp/controller-boundary-qdsplit.ran"
printf '%s\n' '#!/bin/bash' \
    ': >"$CONTROLLER_BOUNDARY_RESOLVER_RAN"' \
    'exit 99' >"$controller_boundary_resolver"
printf '%s\n' '#!/bin/bash' \
    ': >"$CONTROLLER_BOUNDARY_QDSPLIT_RAN"' \
    'exit 99' >"$controller_boundary_qdsplit"
chmod 0700 "$controller_boundary_resolver" "$controller_boundary_qdsplit"
for controller_override in EXITOS_TEST_CTRLDEV EXITOS_TEST_CTRL_MAJMIN \
                           EXITOS_TEST_CTRL_SYSFS_PATH; do
    rm -f -- "$controller_boundary_resolver_ran" \
        "$controller_boundary_qdsplit_ran"
    controller_boundary_err="$tmp/controller-boundary-$controller_override.err"
    controller_boundary_rc=0
    env -u EXITOS_CAMPAIGN_TEST_MODE \
        -u EXITOS_TEST_CTRLDEV -u EXITOS_TEST_CTRL_MAJMIN \
        -u EXITOS_TEST_CTRL_SYSFS_PATH \
        "$controller_override=$tmp/controller-boundary-value" \
        CPU=0 REPS=5 ITERS=1 SETTLE=0 \
        EXITOS_TESTDISK="$controller_boundary_resolver" \
        EXITOS_QDSPLIT_BIN="$controller_boundary_qdsplit" \
        CONTROLLER_BOUNDARY_RESOLVER_RAN="$controller_boundary_resolver_ran" \
        CONTROLLER_BOUNDARY_QDSPLIT_RAN="$controller_boundary_qdsplit_ran" \
        bash tools/campaign-cstate-keeper.sh --private-mount-namespace-run \
        >/dev/null 2>"$controller_boundary_err" || controller_boundary_rc=$?
    expect_ok "$controller_override is rejected before resolver, device open, or qdsplit" \
        sh -c 'test "$1" -eq 2 && test ! -e "$2" && test ! -e "$3" && grep -q "controller identity overrides are pure-test-only" "$4"' \
            sh "$controller_boundary_rc" "$controller_boundary_resolver_ran" \
            "$controller_boundary_qdsplit_ran" "$controller_boundary_err"
done
for controller_override in EXITOS_TEST_CTRLDEV EXITOS_TEST_CTRL_MAJMIN \
                           EXITOS_TEST_CTRL_SYSFS_PATH; do
    rm -f -- "$controller_boundary_resolver_ran" \
        "$controller_boundary_qdsplit_ran"
    controller_boundary_err="$tmp/controller-boundary-test-env-$controller_override.err"
    controller_boundary_rc=0
    env -u EXITOS_TEST_CTRLDEV -u EXITOS_TEST_CTRL_MAJMIN \
        -u EXITOS_TEST_CTRL_SYSFS_PATH \
        EXITOS_CAMPAIGN_TEST_MODE=1 \
        "$controller_override=$tmp/controller-boundary-value" \
        CPU=0 REPS=5 ITERS=1 SETTLE=0 \
        EXITOS_TESTDISK="$controller_boundary_resolver" \
        EXITOS_QDSPLIT_BIN="$controller_boundary_qdsplit" \
        CONTROLLER_BOUNDARY_RESOLVER_RAN="$controller_boundary_resolver_ran" \
        CONTROLLER_BOUNDARY_QDSPLIT_RAN="$controller_boundary_qdsplit_ran" \
        bash tools/campaign-cstate-keeper.sh --private-mount-namespace-run \
        >/dev/null 2>"$controller_boundary_err" || controller_boundary_rc=$?
    expect_ok "$controller_override cannot use test-mode env to enter a production run" \
        sh -c 'test "$1" -eq 2 && test ! -e "$2" && test ! -e "$3" && grep -q "controller identity overrides are pure-test-only" "$4"' \
            sh "$controller_boundary_rc" "$controller_boundary_resolver_ran" \
            "$controller_boundary_qdsplit_ran" "$controller_boundary_err"
done

controller_override_plan_err="$tmp/controller-runtime-overrides-plan.err"
controller_override_plan_rc=0
controller_override_plan=$(env CPU=0 REPS=5 EXITOS_KEEPER_MODES=off,same \
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS='0-2,4' \
    EXITOS_TEST_CTRLDEV=/ignored/controller \
    EXITOS_TEST_CTRL_MAJMIN=999:999 \
    EXITOS_TEST_CTRL_SYSFS_PATH=/ignored/sysfs/controller \
    bash tools/campaign-cstate-keeper.sh --plan \
    2>"$controller_override_plan_err") || controller_override_plan_rc=$?
expect_ok "abstract plan ignores controller runtime overrides byte-for-byte" \
    bash -c 'test "$1" -eq 0 && test ! -s "$2" && test "$3" = "$4"' \
        bash "$controller_override_plan_rc" "$controller_override_plan_err" \
        "$plan" "$controller_override_plan"

# The fake execution path exercises the real child-process environment builder
# but exits before resolver, mount, device, or benchmark setup.
fake_qdsplit="$tmp/fake-qdsplit"
printf '%s\n' \
    '#!/bin/bash' \
    'printf "only=%s huge=%s contig=%s scattered=%s\\n" \
        "${EXITOS_ONLY+x}" "${EXITOS_HUGEBUF+x}" \
        "${EXITOS_REQUIRE_CONTIG+x}" "${EXITOS_REQUIRE_SCATTERED-}"' \
    >"$fake_qdsplit"
chmod 0700 "$fake_qdsplit"
tripwire="$tmp/testdisk-was-run"
fake_testdisk="$tmp/fake-testdisk-tripwire"
printf '%s\n' '#!/bin/bash' ": >'$tripwire'" >"$fake_testdisk"
chmod 0700 "$fake_testdisk"

normal=$(CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_QDSPLIT_BIN="$fake_qdsplit" EXITOS_TESTDISK="$fake_testdisk" \
    EXITOS_ONLY=-1 EXITOS_HUGEBUF=1 EXITOS_REQUIRE_CONTIG=1 \
    bash tools/campaign-cstate-keeper.sh --test-cell-env normal)
expect_ok "normal child sees ONLY absent and only scattered contract" \
    test "$normal" = 'only= huge= contig= scattered=1'
expect_ok "normal fake cell exits before testdisk/device resolver" test ! -e "$tripwire"

contiguous=$(CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_QDSPLIT_BIN="$fake_qdsplit" EXITOS_TESTDISK="$fake_testdisk" \
    EXITOS_ONLY=-1 EXITOS_REQUIRE_SCATTERED=1 \
    bash tools/campaign-cstate-keeper.sh --test-cell-env contiguous)
expect_ok "contiguous child sees ONLY/scattered absent and huge contract" \
    test "$contiguous" = 'only= huge=x contig=x scattered='
expect_ok "contiguous fake cell exits before testdisk/device resolver" test ! -e "$tripwire"

# The production benchmark reports the PFN proof for the largest actual
# single-command span.  The campaign used to scrape an older "first 32 KiB"
# sentence, silently leaving the result's buffer_layout column empty after the
# benchmark wording/contract was corrected.  Exercise the real parser and
# require one, and only one, exact proof line.
layout_out="$tmp/layout.out"
printf '%s\n' \
    'buffer pages: 100 102 ... submitted 8192-byte command span has 2 physical runs' \
    >"$layout_out"
layout=$(CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    bash tools/campaign-cstate-keeper.sh --test-layout "$layout_out" 2>"$tmp/layout.err")
expect_ok "campaign records the actual submitted command span and PFN runs" \
    test "$layout" = 'bytes=8192,runs=2'

printf '%s\n' \
    'buffer pages: 100 101 ... submitted 8192-byte command span has 1 physical run' \
    >"$layout_out"
layout=$(CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    bash tools/campaign-cstate-keeper.sh --test-layout "$layout_out" 2>"$tmp/layout.err")
expect_ok "campaign records the singular contiguous PFN proof" \
    test "$layout" = 'bytes=8192,runs=1'

expect_ok "contiguous cell accepts only the matching 8 KiB PFN proof" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        bash tools/campaign-cstate-keeper.sh \
        --test-layout-contract "$layout_out" contiguous
expect_fail "contiguous PFN proof cannot satisfy a normal scattered cell" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        bash tools/campaign-cstate-keeper.sh \
        --test-layout-contract "$layout_out" normal

printf '%s\n' \
    'buffer pages: 100 102 ... submitted 8192-byte command span has 2 physical runs' \
    >"$layout_out"
expect_ok "normal cell accepts only the matching scattered 8 KiB PFN proof" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        bash tools/campaign-cstate-keeper.sh \
        --test-layout-contract "$layout_out" normal
expect_fail "scattered PFN proof cannot satisfy a contiguous cell" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        bash tools/campaign-cstate-keeper.sh \
        --test-layout-contract "$layout_out" contiguous

printf '%s\n' \
    'buffer pages: 100 ... submitted 4096-byte command span has 1 physical run' \
    >"$layout_out"
expect_fail "a non-8 KiB command span cannot enter the 8 KiB campaign" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        bash tools/campaign-cstate-keeper.sh \
        --test-layout-contract "$layout_out" contiguous

printf '%s\n' 'buffer contract: scattered proven runs=2' >"$layout_out"
expect_fail "missing PFN proof cannot produce an empty layout field" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        bash tools/campaign-cstate-keeper.sh --test-layout "$layout_out"

printf '%s\n' \
    'buffer pages: 100 102 ... submitted 8192-byte command span has 2 physical runs' \
    'buffer pages: 200 202 ... submitted 8192-byte command span has 2 physical runs' \
    >"$layout_out"
expect_fail "ambiguous duplicate PFN proofs are refused" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        bash tools/campaign-cstate-keeper.sh --test-layout "$layout_out"

printf '%s\n' \
    'buffer pages: 100 102 ... submitted 8192-byte command span has 2 physical runs' \
    'buffer pages: malformed second proof' \
    >"$layout_out"
expect_fail "one valid and one malformed PFN proof are still ambiguous" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        bash tools/campaign-cstate-keeper.sh --test-layout "$layout_out"

# Resolver output is data, never shell.  A fixed valid record is accepted and
# a line containing shell syntax is rejected without executing it.
valid_resolver="$tmp/valid-resolver"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\\n" "export EXITOS_DEV=/dev/nvme0n1" "export EXITOS_EXPECT_SERIAL=SERIAL-1" "export EXITOS_FSPART=nvme0n1p1" "export EXITOS_WINDOW_IN_PART=nvme0n1p1" "export EXITOS_TESTPART=/dev/nvme0n1p1" "export EXITOS_CHARDEV=/dev/ng0n1"' \
    >"$valid_resolver"
chmod 0700 "$valid_resolver"
resolved=$(CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_TESTDISK="$valid_resolver" \
    bash tools/campaign-cstate-keeper.sh --test-resolver)
expect_ok "strict resolver parser accepts the six fixed fields" \
    test "$resolved" = $'/dev/nvme0n1\tSERIAL-1\t/dev/nvme0n1p1\t/dev/nvme0n1p1\t/dev/nvme0n1p1\t/dev/ng0n1'

injected="$tmp/injected"
bad_resolver="$tmp/bad-resolver"
printf '%s\n' '#!/bin/bash' \
    "printf '%s\\n' 'export EXITOS_DEV=/dev/nvme0n1;touch $injected'" \
    >"$bad_resolver"
chmod 0700 "$bad_resolver"
expect_fail "resolver shell syntax is rejected as data" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        EXITOS_TESTDISK="$bad_resolver" \
        bash tools/campaign-cstate-keeper.sh --test-resolver
expect_ok "rejected resolver text is never executed" test ! -e "$injected"

# Retained-device pure fixture.  The launcher still executes its real
# fd/path/sysfs relationship checks; regular files replace special nodes only
# behind the explicit unit-test gate.
identity_dev="$tmp/identity-dev"
identity_sys="$tmp/identity-sys"
identity_whole="$identity_dev/nvme0n1"
identity_part="$identity_dev/nvme0n1p1"
identity_char="$identity_dev/ng0n1"
identity_ctrl_node="$identity_dev/nvme0"
identity_ctrl="$identity_sys/devices/pci/nvme/nvme0"
identity_ns="$identity_ctrl/nvme0n1"
identity_part_sys="$identity_ns/nvme0n1p1"
identity_char_sys="$identity_sys/devices/virtual/nvme-generic/ng0n1"
identity_pci="$identity_sys/devices/pci/0000:00:00.0"
identity_irq_proc="$tmp/identity-proc"
identity_interrupts="$identity_irq_proc/interrupts"
identity_proc_stat="$identity_irq_proc/stat"
identity_nvme="$tmp/identity-nvme"
identity_nvme_log="$tmp/identity-nvme.log"
mkdir -p "$identity_dev" "$identity_ns" "$identity_part_sys" \
         "$identity_char_sys" "$identity_sys/dev/block" "$identity_sys/dev/char" \
         "$identity_ns/mq/7" "$identity_pci/msi_irqs" \
         "$identity_irq_proc/irq/141" "$identity_irq_proc/irq/142" \
         "$identity_irq_proc/irq/177" "$identity_irq_proc/irq/178"
: >"$identity_whole"; : >"$identity_part"; : >"$identity_char"
: >"$identity_ctrl_node"
: >"$identity_nvme_log"
printf '%s\n' '259:0' >"$identity_ns/dev"
printf '%s\n' '7' >"$identity_ns/nsid"
printf '%s\n' '72' >"$identity_ns/diskseq"
printf '%s\n' '4' >"$identity_ns/mq/7/cpu_list"
printf '%s\n' 'SERIAL-1' >"$identity_ctrl/serial"
printf '%s\n' '241:0' >"$identity_ctrl/dev"
printf '%s\n' '259:1' >"$identity_part_sys/dev"
printf '%s\n' '1' >"$identity_part_sys/partition"
printf '%s\n' '240:0' >"$identity_char_sys/dev"
ln -s -- "$identity_ctrl" "$identity_ns/device"
ln -s -- "$identity_ctrl" "$identity_char_sys/device"
ln -s -- "$identity_pci" "$identity_ctrl/device"
ln -s -- "$identity_ns" "$identity_sys/dev/block/259:0"
ln -s -- "$identity_part_sys" "$identity_sys/dev/block/259:1"
ln -s -- "$identity_char_sys" "$identity_sys/dev/char/240:0"
ln -s -- "$identity_ctrl" "$identity_sys/dev/char/241:0"
: >"$identity_pci/msi_irqs/141"
: >"$identity_pci/msi_irqs/142"
: >"$identity_pci/msi_irqs/177"
: >"$identity_pci/msi_irqs/178"
for fake_irq in 141 142 177 178; do
    printf '%s\n' 4 >"$identity_irq_proc/irq/$fake_irq/smp_affinity_list"
    printf '%s\n' 4 >"$identity_irq_proc/irq/$fake_irq/effective_affinity_list"
done
printf '%s\n' \
    '           CPU0       CPU1       CPU2       CPU3       CPU4' \
    '141:         57          0          0          0        100 IR-PCI-MSIX nvme0q8' \
    '142:          0          0          0          0          0 IR-PCI-MSIX nvme0q9' \
    '150:          9          0          0          0          7 local-other' \
    'LOC:         10          0          0          0         20 Local timer interrupts' \
    'RES:          3          0          0          0          4 Rescheduling interrupts' \
    'CAL:          5          0          0          0          6 Function call interrupts' \
    'TLB:          7          0          0          0          8 TLB shootdowns' \
    'ERR:        999' \
    >"$identity_interrupts"
printf '%s\n' 'cpu 1 2 3 4' 'procs_running 1' 'procs_blocked 0' \
    >"$identity_proc_stat"

irq_snapshot=$(env CPU=4 REPS=1 ITERS=1 SETTLE=0 \
    EXITOS_CAMPAIGN_TEST_MODE=1 EXITOS_IRQ_PROC_ROOT="$identity_irq_proc" \
    bash tools/campaign-cstate-keeper.sh --test-irq-counters 141 0 \
    2>"$tmp/irq-snapshot.err")
irq_snapshot_rc=$?
expect_ok "counter snapshot subtracts target IRQ from selected and sibling totals" \
    test "$irq_snapshot_rc:$irq_snapshot" = \
        '0:IRQ_COUNTERS target_selected=100 target_total=157 selected_non_target=45 sibling_non_target=34'
printf '%s\n' '#!/bin/bash' \
    'test "$#" -eq 2 && test "$1" = get-ns-id || exit 90' \
    '[[ $2 == /proc/[0-9]*/fd/[0-9]* ]] && test -f "$2" || exit 91' \
    'test "$(readlink -f -- "$2")" = "$IDENTITY_CHAR_PATH" || exit 92' \
    'printf "%s\\n" "$2" >>"$IDENTITY_NVME_LOG"' \
    'printf "%s: namespace-id:%s\\n" "$2" "${IDENTITY_CHAR_NSID:-7}"' \
    >"$identity_nvme"
chmod 0700 "$identity_nvme"

identity_rc=0
identity_out=$(env CPU=0 REPS=1 ITERS=1 SETTLE=0 \
    EXITOS_CAMPAIGN_TEST_MODE=1 EXITOS_DEV="$identity_whole" \
    EXITOS_TESTPART="$identity_part" EXITOS_CHARDEV="$identity_char" \
    EXITOS_TEST_CTRLDEV="$identity_ctrl_node" \
    EXITOS_EXPECT_SERIAL=SERIAL-1 EXITOS_DEVICE_SYSFS_ROOT="$identity_sys" \
    EXITOS_TEST_WHOLE_MAJMIN=259:0 EXITOS_TEST_PART_MAJMIN=259:1 \
    EXITOS_TEST_CHAR_MAJMIN=240:0 EXITOS_TEST_CTRL_MAJMIN=241:0 \
    EXITOS_TEST_CTRL_SYSFS_PATH="$identity_ctrl" \
    EXITOS_TEST_NVME_BIN="$identity_nvme" \
    IDENTITY_NVME_LOG="$identity_nvme_log" IDENTITY_CHAR_PATH="$identity_char" \
    bash tools/campaign-cstate-keeper.sh --test-retained-devices \
    2>"$tmp/identity.err") || identity_rc=$?
expect_ok "campaign retains whole, p1, generic, and controller identities" \
    bash -c 'test "$1" -eq 0 && test ! -s "$2" && grep -q "^DEVICE_IDENTITY phase=setup whole_dev_t=259:0 part_dev_t=259:1 char_dev_t=240:0 ctrl_dev_t=241:0 nsid=7 controller=nvme0 serial=SERIAL-1 status=verified$" <<<"$3"' \
        bash "$identity_rc" "$tmp/identity.err" "$identity_out"
expect_ok "character NSID query uses only the retained character descriptor" \
    sh -c 'test "$(wc -l <"$1")" -ge 1 && ! grep -Ev "^/proc/[0-9]+/fd/[0-9]+$" "$1"' \
        sh "$identity_nvme_log"

expect_fail "campaign rejects a retained character fd for another namespace" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        EXITOS_DEV="$identity_whole" EXITOS_TESTPART="$identity_part" \
        EXITOS_CHARDEV="$identity_char" EXITOS_EXPECT_SERIAL=SERIAL-1 \
        EXITOS_TEST_CTRLDEV="$identity_ctrl_node" \
        EXITOS_DEVICE_SYSFS_ROOT="$identity_sys" \
        EXITOS_TEST_WHOLE_MAJMIN=259:0 EXITOS_TEST_PART_MAJMIN=259:1 \
        EXITOS_TEST_CHAR_MAJMIN=240:0 EXITOS_TEST_CTRL_MAJMIN=241:0 \
        EXITOS_TEST_CTRL_SYSFS_PATH="$identity_ctrl" \
        EXITOS_TEST_NVME_BIN="$identity_nvme" \
        IDENTITY_NVME_LOG="$identity_nvme_log" IDENTITY_CHAR_PATH="$identity_char" \
        IDENTITY_CHAR_NSID=8 \
        bash tools/campaign-cstate-keeper.sh --test-retained-devices

identity_replace_log="$tmp/identity-replace.log"
env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_TEST_RETAIN_PAUSE=1 EXITOS_DEV="$identity_whole" \
    EXITOS_TESTPART="$identity_part" EXITOS_CHARDEV="$identity_char" \
    EXITOS_TEST_CTRLDEV="$identity_ctrl_node" \
    EXITOS_EXPECT_SERIAL=SERIAL-1 EXITOS_DEVICE_SYSFS_ROOT="$identity_sys" \
    EXITOS_TEST_WHOLE_MAJMIN=259:0 EXITOS_TEST_PART_MAJMIN=259:1 \
    EXITOS_TEST_CHAR_MAJMIN=240:0 EXITOS_TEST_CTRL_MAJMIN=241:0 \
    EXITOS_TEST_CTRL_SYSFS_PATH="$identity_ctrl" \
    EXITOS_TEST_NVME_BIN="$identity_nvme" \
    IDENTITY_NVME_LOG="$identity_nvme_log" IDENTITY_CHAR_PATH="$identity_char" \
    bash tools/campaign-cstate-keeper.sh --test-retained-devices \
    >"$identity_replace_log" 2>&1 & identity_pid=$!
for _ in $(seq 1 100); do
    grep -q '^RETAINED_DEVICES_READY$' "$identity_replace_log" 2>/dev/null && break
    kill -0 "$identity_pid" 2>/dev/null || break
    sleep 0.01
done
identity_reached=0
identity_replace_rc=0
if grep -q '^RETAINED_DEVICES_READY$' "$identity_replace_log" 2>/dev/null &&
   wait_for_stopped_state "$identity_pid"; then
    identity_reached=1
    mv -- "$identity_part" "$identity_part.retained"
    : >"$identity_part"
    bounded_resume_and_wait "$identity_pid" || identity_replace_rc=$?
    rm -f -- "$identity_part"
    mv -- "$identity_part.retained" "$identity_part"
else
    bounded_terminate_and_wait "$identity_pid" || identity_replace_rc=$?
fi
expect_ok "retained-device verification rejects a replaced p1 pathname" \
    sh -c 'test "$1" -eq 1 && test "$2" -ne 0 && grep -q "retained device identity verification failed" "$3"' \
        sh "$identity_reached" "$identity_replace_rc" "$identity_replace_log"

identity_wrong_ctrl="$identity_sys/devices/pci/nvme/nvme9"
mkdir "$identity_wrong_ctrl"
printf '%s\n' '241:0' >"$identity_wrong_ctrl/dev"
rm -- "$identity_sys/dev/char/241:0"
ln -s -- "$identity_wrong_ctrl" "$identity_sys/dev/char/241:0"
identity_wrong_ctrl_err="$tmp/identity-wrong-controller.err"
identity_wrong_ctrl_rc=0
env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        EXITOS_DEV="$identity_whole" EXITOS_TESTPART="$identity_part" \
        EXITOS_CHARDEV="$identity_char" EXITOS_TEST_CTRLDEV="$identity_ctrl_node" \
        EXITOS_EXPECT_SERIAL=SERIAL-1 EXITOS_DEVICE_SYSFS_ROOT="$identity_sys" \
        EXITOS_TEST_WHOLE_MAJMIN=259:0 EXITOS_TEST_PART_MAJMIN=259:1 \
        EXITOS_TEST_CHAR_MAJMIN=240:0 EXITOS_TEST_CTRL_MAJMIN=241:0 \
        EXITOS_TEST_CTRL_SYSFS_PATH="$identity_wrong_ctrl" \
        EXITOS_TEST_NVME_BIN="$identity_nvme" \
        IDENTITY_NVME_LOG="$identity_nvme_log" IDENTITY_CHAR_PATH="$identity_char" \
        bash tools/campaign-cstate-keeper.sh --test-retained-devices \
        >/dev/null 2>"$identity_wrong_ctrl_err" || identity_wrong_ctrl_rc=$?
expect_ok "controller char sysfs must name the retained whole-device controller" \
    sh -c 'test "$1" -eq 2 && grep -q "controller char device belongs to a different controller" "$2"' \
        sh "$identity_wrong_ctrl_rc" "$identity_wrong_ctrl_err"
rm -- "$identity_sys/dev/char/241:0"
ln -s -- "$identity_ctrl" "$identity_sys/dev/char/241:0"

identity_ctrl_link="$identity_dev/controller-link"
ln -s -- "$identity_ctrl_node" "$identity_ctrl_link"
expect_fail "controller device pathname symlinks are refused before retention" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
        EXITOS_DEV="$identity_whole" EXITOS_TESTPART="$identity_part" \
        EXITOS_CHARDEV="$identity_char" EXITOS_TEST_CTRLDEV="$identity_ctrl_link" \
        EXITOS_EXPECT_SERIAL=SERIAL-1 EXITOS_DEVICE_SYSFS_ROOT="$identity_sys" \
        EXITOS_TEST_WHOLE_MAJMIN=259:0 EXITOS_TEST_PART_MAJMIN=259:1 \
        EXITOS_TEST_CHAR_MAJMIN=240:0 EXITOS_TEST_CTRL_MAJMIN=241:0 \
        EXITOS_TEST_CTRL_SYSFS_PATH="$identity_ctrl" \
        EXITOS_TEST_NVME_BIN="$identity_nvme" \
        IDENTITY_NVME_LOG="$identity_nvme_log" IDENTITY_CHAR_PATH="$identity_char" \
        bash tools/campaign-cstate-keeper.sh --test-retained-devices

identity_ctrl_replace_log="$tmp/identity-controller-replace.log"
env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_TEST_RETAIN_PAUSE=1 EXITOS_DEV="$identity_whole" \
    EXITOS_TESTPART="$identity_part" EXITOS_CHARDEV="$identity_char" \
    EXITOS_TEST_CTRLDEV="$identity_ctrl_node" \
    EXITOS_EXPECT_SERIAL=SERIAL-1 EXITOS_DEVICE_SYSFS_ROOT="$identity_sys" \
    EXITOS_TEST_WHOLE_MAJMIN=259:0 EXITOS_TEST_PART_MAJMIN=259:1 \
    EXITOS_TEST_CHAR_MAJMIN=240:0 EXITOS_TEST_CTRL_MAJMIN=241:0 \
    EXITOS_TEST_CTRL_SYSFS_PATH="$identity_ctrl" \
    EXITOS_TEST_NVME_BIN="$identity_nvme" \
    IDENTITY_NVME_LOG="$identity_nvme_log" IDENTITY_CHAR_PATH="$identity_char" \
    bash tools/campaign-cstate-keeper.sh --test-retained-devices \
    >"$identity_ctrl_replace_log" 2>&1 & identity_ctrl_pid=$!
for _ in $(seq 1 100); do
    grep -q '^RETAINED_DEVICES_READY$' "$identity_ctrl_replace_log" 2>/dev/null && break
    kill -0 "$identity_ctrl_pid" 2>/dev/null || break
    sleep 0.01
done
identity_ctrl_reached=0
identity_ctrl_replace_rc=0
if grep -q '^RETAINED_DEVICES_READY$' "$identity_ctrl_replace_log" 2>/dev/null &&
   wait_for_stopped_state "$identity_ctrl_pid"; then
    identity_ctrl_reached=1
    mv -- "$identity_ctrl_node" "$identity_ctrl_node.retained"
    : >"$identity_ctrl_node"
    bounded_resume_and_wait "$identity_ctrl_pid" || identity_ctrl_replace_rc=$?
    rm -f -- "$identity_ctrl_node"
    mv -- "$identity_ctrl_node.retained" "$identity_ctrl_node"
else
    bounded_terminate_and_wait "$identity_ctrl_pid" || identity_ctrl_replace_rc=$?
fi
expect_ok "retained controller verification rejects a paused pathname replacement" \
    sh -c 'test "$1" -eq 1 && test "$2" -ne 0 && grep -q "retained device identity verification failed" "$3"' \
        sh "$identity_ctrl_reached" "$identity_ctrl_replace_rc" \
        "$identity_ctrl_replace_log"

identity_ctrl_symlink_log="$tmp/identity-controller-symlink-swap.log"
env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_TEST_RETAIN_PAUSE=1 EXITOS_DEV="$identity_whole" \
    EXITOS_TESTPART="$identity_part" EXITOS_CHARDEV="$identity_char" \
    EXITOS_TEST_CTRLDEV="$identity_ctrl_node" \
    EXITOS_EXPECT_SERIAL=SERIAL-1 EXITOS_DEVICE_SYSFS_ROOT="$identity_sys" \
    EXITOS_TEST_WHOLE_MAJMIN=259:0 EXITOS_TEST_PART_MAJMIN=259:1 \
    EXITOS_TEST_CHAR_MAJMIN=240:0 EXITOS_TEST_CTRL_MAJMIN=241:0 \
    EXITOS_TEST_CTRL_SYSFS_PATH="$identity_ctrl" \
    EXITOS_TEST_NVME_BIN="$identity_nvme" \
    IDENTITY_NVME_LOG="$identity_nvme_log" IDENTITY_CHAR_PATH="$identity_char" \
    bash tools/campaign-cstate-keeper.sh --test-retained-devices \
    >"$identity_ctrl_symlink_log" 2>&1 & identity_ctrl_symlink_pid=$!
for _ in $(seq 1 100); do
    grep -q '^RETAINED_DEVICES_READY$' "$identity_ctrl_symlink_log" 2>/dev/null && break
    kill -0 "$identity_ctrl_symlink_pid" 2>/dev/null || break
    sleep 0.01
done
identity_ctrl_symlink_reached=0
identity_ctrl_symlink_rc=0
if grep -q '^RETAINED_DEVICES_READY$' "$identity_ctrl_symlink_log" 2>/dev/null &&
   wait_for_stopped_state "$identity_ctrl_symlink_pid"; then
    identity_ctrl_symlink_reached=1
    mv -- "$identity_ctrl_node" "$identity_ctrl_node.retained-symlink"
    ln -s -- "$identity_ctrl_node.retained-symlink" "$identity_ctrl_node"
    bounded_resume_and_wait "$identity_ctrl_symlink_pid" ||
        identity_ctrl_symlink_rc=$?
    rm -- "$identity_ctrl_node"
    mv -- "$identity_ctrl_node.retained-symlink" "$identity_ctrl_node"
else
    bounded_terminate_and_wait "$identity_ctrl_symlink_pid" ||
        identity_ctrl_symlink_rc=$?
fi
expect_ok "retained controller rejects a paused symlink swap to the original inode" \
    sh -c 'test "$1" -eq 1 && test "$2" -ne 0 && grep -q "retained device identity verification failed" "$3"' \
        sh "$identity_ctrl_symlink_reached" "$identity_ctrl_symlink_rc" \
        "$identity_ctrl_symlink_log"

# Task 2C batch 2: a canonical polling-generation snapshot is a pure hidden
# test surface only.  Its fixture mirrors the real NVMe sysfs relationships,
# while regular files replace all four special device nodes behind the
# explicit test CLI.  The expected frame below is assembled independently of
# the campaign implementation, including inode values and the SHA boundary.
poll_dev="$tmp/poll-dev"
poll_sys="$tmp/poll-sys"
poll_proc="$tmp/poll-proc"
poll_state_parent="$tmp/poll-state-parent"
poll_whole="$poll_dev/nvme0n1"
poll_part="$poll_dev/nvme0n1p1"
poll_char="$poll_dev/ng0n1"
poll_ctrl_node="$poll_dev/nvme0"
poll_pci="$poll_sys/devices/pci0000:00/0000:00:00.0"
poll_ctrl="$poll_pci/nvme/nvme0"
poll_ns="$poll_ctrl/nvme0n1"
poll_part_sys="$poll_ns/nvme0n1p1"
poll_char_sys="$poll_ctrl/ng0n1"
poll_queue="$poll_ns/queue"
poll_mq="$poll_ns/mq"
poll_diskseq_fd="$tmp/poll-diskseq-fd"
poll_high_fd_sentinel="$tmp/poll-high-fd-sentinel"
poll_nvme="$tmp/poll-nvme"

mkdir -p "$poll_dev" "$poll_state_parent" \
    "$poll_ns" "$poll_part_sys" "$poll_char_sys" "$poll_pci/msi_irqs" \
    "$poll_queue" "$poll_mq/10" "$poll_mq/2" "$poll_mq/11" \
    "$poll_sys/dev/block" "$poll_sys/dev/char" \
    "$poll_sys/module/nvme/parameters" \
    "$poll_proc/sys/kernel/random"
printf '%s\n' 1001 >"$poll_whole"
printf '%s\n' 1002 >"$poll_part"
printf '%s\n' 1003 >"$poll_char"
printf '%s\n' 1004 >"$poll_ctrl_node"
printf '%s\n' 259:0 >"$poll_ns/dev"
printf '%s\n' 259:1 >"$poll_part_sys/dev"
printf '%s\n' 1 >"$poll_part_sys/partition"
printf '%s\n' 240:0 >"$poll_char_sys/dev"
printf '%s\n' 241:0 >"$poll_ctrl/dev"
printf '%s\n' SERIAL-POLL >"$poll_ctrl/serial"
printf '%s\n' 7 >"$poll_ns/nsid"
printf '%s\n' 72623859790382856 >"$poll_ns/diskseq"
printf '%s\n' 72623859790382856 >"$poll_diskseq_fd"
printf '%s\n' high-fd-sentinel >"$poll_high_fd_sentinel"
printf '%s\n' 4 >"$poll_sys/module/nvme/parameters/poll_queues"
printf '%s\n' 1 >"$poll_queue/io_poll"
# Deliberately noncanonical but semantically well-formed CPU sets.  The
# snapshot must sort, deduplicate, and merge them without expanding ranges.
printf '%s\n' '12, 9-11,10, 8' >"$poll_mq/10/cpu_list"
printf '%s\n' '4, 0-2,1-3' >"$poll_mq/2/cpu_list"
printf '\n' >"$poll_mq/11/cpu_list"
printf '%s\n' 11111111-2222-3333-4444-555555555555 \
    >"$poll_proc/sys/kernel/random/boot_id"
ln -s -- "$poll_ctrl" "$poll_ns/device"
ln -s -- "$poll_ctrl" "$poll_char_sys/device"
ln -s -- "$poll_pci" "$poll_ctrl/device"
ln -s -- "$poll_ns" "$poll_sys/dev/block/259:0"
ln -s -- "$poll_part_sys" "$poll_sys/dev/block/259:1"
ln -s -- "$poll_char_sys" "$poll_sys/dev/char/240:0"
ln -s -- "$poll_ctrl" "$poll_sys/dev/char/241:0"
printf '%s\n' \
    '#!/bin/bash' \
    'test "$#" -eq 2 && test "$1" = get-ns-id || exit 90' \
    '[[ $2 == /proc/[0-9]*/fd/[0-9]* ]] && test -f "$2" || exit 91' \
    'printf "%s: namespace-id:7\n" "$2"' \
    >"$poll_nvme"
chmod 0700 "$poll_nvme"

poll_fixture_env=(
    CPU=0 REPS=1 ITERS=1 SETTLE=0 TMPDIR="$poll_state_parent"
    EXITOS_DEV="$poll_whole" EXITOS_TESTPART="$poll_part"
    EXITOS_CHARDEV="$poll_char" EXITOS_TEST_CTRLDEV="$poll_ctrl_node"
    EXITOS_EXPECT_SERIAL=SERIAL-POLL EXITOS_DEVICE_SYSFS_ROOT="$poll_sys"
    EXITOS_TEST_WHOLE_MAJMIN=259:0 EXITOS_TEST_PART_MAJMIN=259:1
    EXITOS_TEST_CHAR_MAJMIN=240:0 EXITOS_TEST_CTRL_MAJMIN=241:0
    EXITOS_TEST_CTRL_SYSFS_PATH="$poll_ctrl"
    EXITOS_TEST_NVME_BIN="$poll_nvme"
    EXITOS_TEST_POLL_PROC_ROOT="$poll_proc"
    EXITOS_TEST_BLKGETDISKSEQ_FILE="$poll_diskseq_fd"
)

poll_body="$tmp/poll-snapshot.body"
poll_golden="$tmp/poll-snapshot.golden"
poll_whole_inode=$(stat -Lc '%d:%i' -- "$poll_ns")
poll_part_inode=$(stat -Lc '%d:%i' -- "$poll_part_sys")
poll_char_inode=$(stat -Lc '%d:%i' -- "$poll_char_sys")
poll_ctrl_inode=$(stat -Lc '%d:%i' -- "$poll_ctrl")
poll_queue_inode=$(stat -Lc '%d:%i' -- "$poll_queue")
poll_pci_inode=$(stat -Lc '%d:%i' -- "$poll_pci")
poll_hctx2_inode=$(stat -Lc '%d:%i' -- "$poll_mq/2")
poll_hctx10_inode=$(stat -Lc '%d:%i' -- "$poll_mq/10")
poll_hctx11_inode=$(stat -Lc '%d:%i' -- "$poll_mq/11")
printf '%s\n' \
    'format=exitos-poll-generation-v1' \
    'boot_id=11111111-2222-3333-4444-555555555555' \
    'poll_queues=4' \
    'io_poll=1' \
    'whole_dev_t=259:0' \
    'part_dev_t=259:1' \
    'generic_dev_t=240:0' \
    'controller_dev_t=241:0' \
    'diskseq_fd=72623859790382856' \
    'diskseq_sysfs=72623859790382856' \
    'nsid=7' \
    "whole_target=$poll_ns" \
    "whole_inode=$poll_whole_inode" \
    "part_target=$poll_part_sys" \
    "part_inode=$poll_part_inode" \
    "generic_target=$poll_char_sys" \
    "generic_inode=$poll_char_inode" \
    "controller_char_target=$poll_ctrl" \
    "controller_char_inode=$poll_ctrl_inode" \
    "queue_target=$poll_queue" \
    "queue_inode=$poll_queue_inode" \
    "controller_target=$poll_ctrl" \
    "controller_inode=$poll_ctrl_inode" \
    "pci_target=$poll_pci" \
    "pci_inode=$poll_pci_inode" \
    'mq_count=3' \
    "mq_hctx=2:$poll_hctx2_inode:0-4" \
    "mq_hctx=10:$poll_hctx10_inode:8-12" \
    "mq_hctx=11:$poll_hctx11_inode:-" \
    >"$poll_body"
poll_body_sha=$(sha256sum -- "$poll_body" | awk '{ print $1 }')
{
    printf 'snapshot_sha256=%s\n' "$poll_body_sha"
    while IFS= read -r poll_golden_line; do
        printf '%s\n' "$poll_golden_line"
    done <"$poll_body"
} >"$poll_golden"

poll_snapshot_out="$tmp/poll-snapshot.out"
poll_snapshot_err="$tmp/poll-snapshot.err"
poll_snapshot_rc=0
env EXITOS_CAMPAIGN_TEST_MODE=1 "${poll_fixture_env[@]}" \
    bash tools/campaign-cstate-keeper.sh --test-poll-snapshot \
    >"$poll_snapshot_out" 2>"$poll_snapshot_err" || poll_snapshot_rc=$?
expect_ok "poll snapshot stdout matches the independent byte-exact canonical golden" \
    sh -c 'test "$1" -eq 0 && cmp -s -- "$2" "$3"' \
        sh "$poll_snapshot_rc" "$poll_golden" "$poll_snapshot_out"
expect_ok "poll snapshot has ASCII-only framing, empty stderr, and exactly one final LF" \
    python3 - "$poll_snapshot_rc" "$poll_snapshot_out" "$poll_snapshot_err" <<'PY'
import pathlib
import hashlib
import sys

rc = int(sys.argv[1])
data = pathlib.Path(sys.argv[2]).read_bytes()
err = pathlib.Path(sys.argv[3]).read_bytes()
first, separator, body = data.partition(b"\n")
expected_hash = b"snapshot_sha256=" + hashlib.sha256(body).hexdigest().encode("ascii")
raise SystemExit(not (rc == 0 and not err and separator == b"\n" and
                      first == expected_hash and data.isascii() and
                      b"\x00" not in data and b"\r" not in data and
                      body.startswith(b"format=exitos-poll-generation-v1\n") and
                      data.endswith(b"\n") and not data.endswith(b"\n\n")))
PY
expect_ok "poll snapshot numerically sorts hctx and canonicalizes noncanonical and empty CPU sets" \
    awk '
        /^mq_hctx=/ { rows[++n] = $0 }
        END {
            if (n != 3 || rows[1] !~ /^mq_hctx=2:[0-9]+:[0-9]+:0-4$/ ||
                rows[2] !~ /^mq_hctx=10:[0-9]+:[0-9]+:8-12$/ ||
                rows[3] !~ /^mq_hctx=11:[0-9]+:[0-9]+:-$/) exit 1
        }
    ' "$poll_snapshot_out"
expect_ok "poll snapshot cleans its private pre-retention state directory" \
    sh -c 'test -d "$1" && test -z "$(find "$1" -mindepth 1 -maxdepth 1 -print -quit)"' \
        sh "$poll_state_parent"

# Equivalent textual CPU sets must leave the complete frame and SHA unchanged.
printf '%s\n' 8-12 >"$poll_mq/10/cpu_list"
printf '%s\n' 0-4 >"$poll_mq/2/cpu_list"
poll_equiv_out="$tmp/poll-snapshot-equivalent.out"
poll_equiv_err="$tmp/poll-snapshot-equivalent.err"
poll_equiv_rc=0
env EXITOS_CAMPAIGN_TEST_MODE=1 "${poll_fixture_env[@]}" \
    bash tools/campaign-cstate-keeper.sh --test-poll-snapshot \
    >"$poll_equiv_out" 2>"$poll_equiv_err" || poll_equiv_rc=$?
expect_ok "semantically equivalent CPU-list text produces the identical generation frame" \
    sh -c 'test "$1" -eq 0 && test ! -s "$2" && cmp -s -- "$3" "$4"' \
        sh "$poll_equiv_rc" "$poll_equiv_err" \
        "$poll_snapshot_out" "$poll_equiv_out"

poll_expect_link_race_refusal() {
    local label=$1 hook_name=$2 marker=$3 link_path=$4 replacement_target=$5
    local expected_code=$6 suffix=$7 retained_link line helper_pid=""
    local marker_start="" capture_pid="" capture_start="" reached=0 rc=0
    local campaign_pid=""
    retained_link="$link_path.retained-$suffix"
    env EXITOS_CAMPAIGN_TEST_MODE=1 "${poll_fixture_env[@]}" \
        "$hook_name=1" EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1=1 \
        bash tools/campaign-cstate-keeper.sh --test-poll-snapshot \
        >"$tmp/poll-link-race-$suffix.out" \
        2>"$tmp/poll-link-race-$suffix.err" & campaign_pid=$!
    for _ in $(seq 1 200); do
        line=$(grep -E "^$marker pid=[1-9][0-9]* starttime=[1-9][0-9]*$" \
            "$tmp/poll-link-race-$suffix.err" 2>/dev/null || true)
        if [[ $line =~ ^$marker\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
            helper_pid=${BASH_REMATCH[1]}
            marker_start=${BASH_REMATCH[2]}
            break
        fi
        line=$(grep -E '^POLL_SNAPSHOT_CAPTURE1_READY pid=[1-9][0-9]* starttime=[1-9][0-9]*$' \
            "$tmp/poll-link-race-$suffix.err" 2>/dev/null || true)
        if [[ $line =~ ^POLL_SNAPSHOT_CAPTURE1_READY\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
            kill -CONT "${BASH_REMATCH[1]}" 2>/dev/null || true
            break
        fi
        kill -0 "$campaign_pid" 2>/dev/null || break
        sleep 0.01
    done
    if [[ $helper_pid =~ ^[1-9][0-9]*$ ]] &&
       wait_for_stopped_state "$helper_pid" &&
       [ "$(proc_starttime "$helper_pid")" = "$marker_start" ]; then
        reached=1
        mv -- "$link_path" "$retained_link"
        ln -s -- "$replacement_target" "$link_path"
        kill -CONT "$helper_pid" 2>/dev/null || true
        for _ in $(seq 1 200); do
            line=$(grep -E '^POLL_SNAPSHOT_CAPTURE1_READY pid=[1-9][0-9]* starttime=[1-9][0-9]*$' \
                "$tmp/poll-link-race-$suffix.err" 2>/dev/null || true)
            if [[ $line =~ ^POLL_SNAPSHOT_CAPTURE1_READY\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
                capture_pid=${BASH_REMATCH[1]}
                capture_start=${BASH_REMATCH[2]}
                break
            fi
            kill -0 "$campaign_pid" 2>/dev/null || break
            sleep 0.01
        done
        if [ -e "$retained_link" ] || [ -L "$retained_link" ]; then
            rm -f -- "$link_path"
            mv -- "$retained_link" "$link_path"
        fi
        if [ "$capture_pid" = "$helper_pid" ] &&
           wait_for_stopped_state "$capture_pid" &&
           [ "$(proc_starttime "$capture_pid")" = "$capture_start" ]; then
            kill -CONT "$capture_pid" 2>/dev/null || true
        fi
    fi
    if ! wait_for_exit_or_zombie "$campaign_pid" 400; then
        [ -z "$helper_pid" ] || bounded_terminate_and_wait "$helper_pid" 2>/dev/null || true
        bounded_terminate_and_wait "$campaign_pid" 2>/dev/null || rc=$?
    else
        wait "$campaign_pid" 2>/dev/null || rc=$?
    fi
    if [ -e "$retained_link" ] || [ -L "$retained_link" ]; then
        rm -f -- "$link_path"
        mv -- "$retained_link" "$link_path"
    fi
    expect_ok "$label" \
        sh -c 'test "$1" -eq 1 && test "$2" -eq 1 && test ! -s "$3" && grep -Fxq "poll snapshot: $4" "$5" && test ! -e "/proc/$6/status" && test -z "$(find "$7" -mindepth 1 -maxdepth 1 -print -quit)"' \
            sh "$reached" "$rc" "$tmp/poll-link-race-$suffix.out" \
            "$expected_code" "$tmp/poll-link-race-$suffix.err" \
            "${helper_pid:-0}" "$poll_state_parent"
}

poll_ns_replacement="$poll_ctrl/nvme0n1.replacement"
cp -a -- "$poll_ns" "$poll_ns_replacement"
poll_expect_link_race_refusal \
    "poll snapshot rejects a block alias replaced between lstat and follow-open" \
    EXITOS_TEST_POLL_PAUSE_BEFORE_ALIAS_OPEN POLL_SNAPSHOT_ALIAS_READY \
    "$poll_sys/dev/block/259:0" "$poll_ns_replacement" \
    whole_alias_changed block-alias
rm -rf -- "$poll_ns_replacement"

poll_expect_link_race_refusal \
    "poll snapshot rejects a device link replaced between lstat and follow-open" \
    EXITOS_TEST_POLL_PAUSE_BEFORE_DEVICE_LINK_OPEN POLL_SNAPSHOT_DEVICE_LINK_READY \
    "$poll_ns/device" "$poll_pci" \
    whole_controller_alias_changed device-link

# Freeze the first mq membership list, add a fully valid hctx, then restore the
# original membership at the capture-1 pause.  An implementation that lists
# only once per capture will incorrectly produce two identical bodies; a
# correct capture must reject the within-capture membership drift directly.
poll_mq_race_out="$tmp/poll-mq-race.out"
poll_mq_race_err="$tmp/poll-mq-race.err"
env EXITOS_CAMPAIGN_TEST_MODE=1 "${poll_fixture_env[@]}" \
    EXITOS_TEST_POLL_PAUSE_AFTER_MQ_LIST=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1=1 \
    bash tools/campaign-cstate-keeper.sh --test-poll-snapshot \
    >"$poll_mq_race_out" 2>"$poll_mq_race_err" & poll_mq_race_campaign_pid=$!
poll_mq_race_helper_pid=""
poll_mq_race_marker_start=""
poll_mq_race_reached=0
for _ in $(seq 1 200); do
    poll_mq_race_line=$(sed -n \
        '/^POLL_SNAPSHOT_MQ_LIST_READY pid=[1-9][0-9]* starttime=[1-9][0-9]*$/p' \
        "$poll_mq_race_err" 2>/dev/null)
    if [[ $poll_mq_race_line =~ ^POLL_SNAPSHOT_MQ_LIST_READY\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
        poll_mq_race_helper_pid=${BASH_REMATCH[1]}
        poll_mq_race_marker_start=${BASH_REMATCH[2]}
        break
    fi
    poll_mq_race_line=$(sed -n \
        '/^POLL_SNAPSHOT_CAPTURE1_READY pid=[1-9][0-9]* starttime=[1-9][0-9]*$/p' \
        "$poll_mq_race_err" 2>/dev/null)
    if [[ $poll_mq_race_line =~ ^POLL_SNAPSHOT_CAPTURE1_READY\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
        poll_mq_race_helper_pid=${BASH_REMATCH[1]}
        kill -CONT "$poll_mq_race_helper_pid" 2>/dev/null || true
        poll_mq_race_helper_pid=""
        break
    fi
    kill -0 "$poll_mq_race_campaign_pid" 2>/dev/null || break
    sleep 0.01
done
if [[ $poll_mq_race_helper_pid =~ ^[1-9][0-9]*$ ]] &&
   wait_for_stopped_state "$poll_mq_race_helper_pid" &&
   [ "$(proc_starttime "$poll_mq_race_helper_pid")" = "$poll_mq_race_marker_start" ]; then
    poll_mq_race_reached=1
    mkdir "$poll_mq/12"
    printf '%s\n' '14, 13' >"$poll_mq/12/cpu_list"
    kill -CONT "$poll_mq_race_helper_pid" 2>/dev/null || true
    poll_mq_capture1_pid=""
    poll_mq_capture1_start=""
    for _ in $(seq 1 200); do
        poll_mq_race_line=$(sed -n \
            '/^POLL_SNAPSHOT_CAPTURE1_READY pid=[1-9][0-9]* starttime=[1-9][0-9]*$/p' \
            "$poll_mq_race_err" 2>/dev/null)
        if [[ $poll_mq_race_line =~ ^POLL_SNAPSHOT_CAPTURE1_READY\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
            poll_mq_capture1_pid=${BASH_REMATCH[1]}
            poll_mq_capture1_start=${BASH_REMATCH[2]}
            break
        fi
        kill -0 "$poll_mq_race_campaign_pid" 2>/dev/null || break
        sleep 0.01
    done
    if [ "$poll_mq_capture1_pid" = "$poll_mq_race_helper_pid" ] &&
       wait_for_stopped_state "$poll_mq_capture1_pid" &&
       [ "$(proc_starttime "$poll_mq_capture1_pid")" = "$poll_mq_capture1_start" ]; then
        rm -rf -- "$poll_mq/12"
        kill -CONT "$poll_mq_capture1_pid" 2>/dev/null || true
    fi
fi
poll_mq_race_rc=0
if ! wait_for_exit_or_zombie "$poll_mq_race_campaign_pid" 400; then
    [ -z "$poll_mq_race_helper_pid" ] ||
        bounded_terminate_and_wait "$poll_mq_race_helper_pid" 2>/dev/null || true
    bounded_terminate_and_wait "$poll_mq_race_campaign_pid" 2>/dev/null ||
        poll_mq_race_rc=$?
else
    wait "$poll_mq_race_campaign_pid" 2>/dev/null || poll_mq_race_rc=$?
fi
rm -rf -- "$poll_mq/12"
expect_ok "poll snapshot rejects an hctx added after its frozen mq membership list" \
    sh -c 'test "$1" -eq 1 && test "$2" -eq 1 && test ! -s "$3" && grep -Fxq "poll snapshot: mq_membership_unstable" "$4" && test ! -e "/proc/$5/status" && test -z "$(find "$6" -mindepth 1 -maxdepth 1 -print -quit)"' \
        sh "$poll_mq_race_reached" "$poll_mq_race_rc" \
        "$poll_mq_race_out" "$poll_mq_race_err" \
        "${poll_mq_race_helper_pid:-0}" "$poll_state_parent"

poll_expect_refusal() {
    local label=$1 code=$2 suffix=$3 rc=0
    shift 3
    env EXITOS_CAMPAIGN_TEST_MODE=1 "${poll_fixture_env[@]}" "$@" \
        bash tools/campaign-cstate-keeper.sh --test-poll-snapshot \
        >"$tmp/poll-refuse-$suffix.out" \
        2>"$tmp/poll-refuse-$suffix.err" || rc=$?
    expect_ok "$label" \
        sh -c 'test "$1" -eq 1 && test ! -s "$2" && test "$(wc -l <"$4")" -eq 1 && grep -Fxq "poll snapshot: $3" "$4" && test -z "$(find "$5" -mindepth 1 -maxdepth 1 -print -quit)"' \
            sh "$rc" "$tmp/poll-refuse-$suffix.out" "$code" \
            "$tmp/poll-refuse-$suffix.err" "$poll_state_parent"
}

mv -- "$poll_mq" "$poll_mq.saved"
mkdir "$poll_mq"
poll_expect_refusal "poll snapshot refuses an empty mq generation" \
    mq_empty empty-mq
rmdir -- "$poll_mq"
mv -- "$poll_mq.saved" "$poll_mq"

poll_symlink_target="$tmp/poll-symlink-hctx"
mkdir "$poll_symlink_target"
printf '%s\n' 3 >"$poll_symlink_target/cpu_list"
ln -s -- "$poll_symlink_target" "$poll_mq/3"
poll_expect_refusal "poll snapshot refuses a numeric hctx symlink" \
    mq_hctx_symlink hctx-symlink
rm -- "$poll_mq/3"

mv -- "$poll_mq/2/cpu_list" "$poll_mq/2/cpu_list.saved"
poll_expect_refusal "poll snapshot refuses an hctx missing cpu_list" \
    mq_cpu_list_missing missing-cpu-list
mv -- "$poll_mq/2/cpu_list.saved" "$poll_mq/2/cpu_list"

printf '%s' 0-4 >"$poll_mq/2/cpu_list"
poll_expect_refusal "poll snapshot refuses a cpu_list without its final LF" \
    mq_cpu_list_malformed cpu-list-no-lf
printf '%s\n' 0-4 >"$poll_mq/2/cpu_list"

printf '0,\t2\n' >"$poll_mq/2/cpu_list"
poll_expect_refusal "poll snapshot rejects TAB instead of treating it as Linux cpu-list spacing" \
    mq_cpu_list_malformed cpu-list-tab
printf '%s\n' 0-4 >"$poll_mq/2/cpu_list"

: >"$poll_mq/not-an-hctx"
poll_expect_refusal "poll snapshot rejects every nonnumeric mq membership entry" \
    mq_hctx_name_malformed mq-nonnumeric-entry
rm -- "$poll_mq/not-an-hctx"

printf '%s' 72623859790382856 >"$poll_diskseq_fd"
poll_expect_refusal "poll snapshot refuses a fake fd diskseq without its final LF" \
    fake_diskseq_malformed diskseq-no-lf
printf '%s\n' 72623859790382856 >"$poll_diskseq_fd"

printf '%s\n' 72623859790382857 >"$poll_ns/diskseq"
poll_expect_refusal "poll snapshot refuses disagreement between fd and sysfs diskseq" \
    diskseq_mismatch diskseq-mismatch
printf '%s\n' 72623859790382856 >"$poll_ns/diskseq"

# A helper-side partial write is kept inside the private state file and can
# never escape as a successful frame on the hidden CLI.  The hook pauses only
# after flushing a recognizable prefix, so an early-failure no-op cannot make
# this test pass.
poll_partial_out="$tmp/poll-partial.out"
poll_partial_err="$tmp/poll-partial.err"
poll_partial_expected="$tmp/poll-partial.expected"
printf '%s\n' snapshot_sha256=partial >"$poll_partial_expected"
env EXITOS_CAMPAIGN_TEST_MODE=1 "${poll_fixture_env[@]}" \
    EXITOS_TEST_POLL_PARTIAL_OUTPUT=1 \
    bash tools/campaign-cstate-keeper.sh --test-poll-snapshot \
    >"$poll_partial_out" 2>"$poll_partial_err" & poll_partial_campaign_pid=$!
poll_partial_helper_pid=""
poll_partial_marker_start=""
for _ in $(seq 1 200); do
    poll_partial_line=$(sed -n \
        '/^POLL_SNAPSHOT_PARTIAL_READY pid=[1-9][0-9]* starttime=[1-9][0-9]*$/p' \
        "$poll_partial_err" 2>/dev/null)
    if [[ $poll_partial_line =~ ^POLL_SNAPSHOT_PARTIAL_READY\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
        poll_partial_helper_pid=${BASH_REMATCH[1]}
        poll_partial_marker_start=${BASH_REMATCH[2]}
        break
    fi
    kill -0 "$poll_partial_campaign_pid" 2>/dev/null || break
    sleep 0.01
done
poll_partial_reached=0
poll_partial_start_before=""
poll_partial_start_after=""
poll_partial_private_file=""
poll_partial_private_exact=0
if [[ $poll_partial_helper_pid =~ ^[1-9][0-9]*$ ]] &&
   wait_for_stopped_state "$poll_partial_helper_pid"; then
    poll_partial_reached=1
    poll_partial_start_before=$(proc_starttime "$poll_partial_helper_pid" || true)
    mapfile -t poll_partial_private_files < <(
        find "$poll_state_parent" -mindepth 2 -maxdepth 2 -type f -print
    )
    if [ "${#poll_partial_private_files[@]}" -eq 1 ]; then
        poll_partial_private_file=${poll_partial_private_files[0]}
        cmp -s -- "$poll_partial_expected" "$poll_partial_private_file" &&
            poll_partial_private_exact=1
    fi
    test ! -s "$poll_partial_out" || poll_partial_private_exact=0
    poll_partial_start_after=$(proc_starttime "$poll_partial_helper_pid" || true)
    kill -CONT "$poll_partial_helper_pid" 2>/dev/null || true
fi
poll_partial_rc=0
if ! wait_for_exit_or_zombie "$poll_partial_campaign_pid" 400; then
    [ -z "$poll_partial_helper_pid" ] || bounded_terminate_and_wait "$poll_partial_helper_pid" 2>/dev/null || true
    bounded_terminate_and_wait "$poll_partial_campaign_pid" 2>/dev/null || poll_partial_rc=$?
else
    wait "$poll_partial_campaign_pid" 2>/dev/null || poll_partial_rc=$?
fi
expect_ok "poll snapshot helper writes a private partial prefix but publishes zero bytes on failure" \
    bash -c 'test "$1" -eq 1 && [[ $2 =~ ^[1-9][0-9]*$ ]] && test "$2" = "$3" && test "$3" = "$4" && test "$5" -eq 1 && test "$6" -eq 1 && test ! -s "$7" && grep -Fxq "poll snapshot: injected_partial_output" "$8" && test ! -e "/proc/$9/status" && test -z "$(find "${10}" -mindepth 1 -maxdepth 1 -print -quit)"' \
        bash "$poll_partial_reached" "$poll_partial_marker_start" \
        "$poll_partial_start_before" "$poll_partial_start_after" \
        "$poll_partial_private_exact" "$poll_partial_rc" \
        "$poll_partial_out" "$poll_partial_err" \
        "${poll_partial_helper_pid:-0}" "$poll_state_parent"

poll_sys_link="$tmp/poll-sys-link"
ln -s -- "$poll_sys" "$poll_sys_link"
poll_expect_refusal "poll snapshot rejects a symlink fake sysfs root" \
    fake_sys_root_invalid symlink-sys-root \
    EXITOS_DEVICE_SYSFS_ROOT="$poll_sys_link"
rm -- "$poll_sys_link"

# The CLI name alone and the test-mode environment alone are independently
# insufficient to unlock fake roots or diskseq in a production MODE=run.
poll_no_gate_rc=0
env "${poll_fixture_env[@]}" \
    bash tools/campaign-cstate-keeper.sh --test-poll-snapshot \
    >"$tmp/poll-no-test-gate.out" 2>"$tmp/poll-no-test-gate.err" ||
    poll_no_gate_rc=$?
expect_ok "poll snapshot hidden CLI requires the explicit pure-test gate" \
    sh -c 'test "$1" -eq 2 && test ! -s "$2" && grep -q "test-poll-snapshot is available only to the pure unit test" "$3"' \
        sh "$poll_no_gate_rc" "$tmp/poll-no-test-gate.out" \
        "$tmp/poll-no-test-gate.err"

for poll_override in EXITOS_TEST_POLL_PROC_ROOT \
                     EXITOS_TEST_BLKGETDISKSEQ_FILE \
                     EXITOS_TEST_POLL_PAUSE_BEFORE \
                     EXITOS_TEST_POLL_PAUSE_AFTER \
                     EXITOS_TEST_POLL_HELPER_PAUSE \
                     EXITOS_TEST_POLL_PAUSE_BEFORE_ALIAS_OPEN \
                     EXITOS_TEST_POLL_PAUSE_BEFORE_DEVICE_LINK_OPEN \
                     EXITOS_TEST_POLL_PAUSE_AFTER_MQ_LIST \
                     EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1 \
                     EXITOS_TEST_POLL_PARTIAL_OUTPUT; do
    case "$poll_override" in
      EXITOS_TEST_POLL_PROC_ROOT) poll_override_value=$poll_proc ;;
      EXITOS_TEST_BLKGETDISKSEQ_FILE) poll_override_value=$poll_diskseq_fd ;;
      *) poll_override_value=1 ;;
    esac
    for poll_boundary_case in forged-test-mode absent-test-mode \
                              set-empty absent-test-mode-set-empty; do
        rm -f -- "$controller_boundary_resolver_ran" \
            "$controller_boundary_qdsplit_ran"
        poll_boundary_rc=0
        poll_boundary_env=(
            -u EXITOS_CAMPAIGN_TEST_MODE
            -u EXITOS_TEST_POLL_PROC_ROOT
            -u EXITOS_TEST_BLKGETDISKSEQ_FILE
            -u EXITOS_TEST_POLL_PAUSE_BEFORE
            -u EXITOS_TEST_POLL_PAUSE_AFTER
            -u EXITOS_TEST_POLL_HELPER_PAUSE
            -u EXITOS_TEST_POLL_PAUSE_BEFORE_ALIAS_OPEN
            -u EXITOS_TEST_POLL_PAUSE_BEFORE_DEVICE_LINK_OPEN
            -u EXITOS_TEST_POLL_PAUSE_AFTER_MQ_LIST
            -u EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1
            -u EXITOS_TEST_POLL_PARTIAL_OUTPUT
        )
        case "$poll_boundary_case" in
          forged-test-mode)
            poll_boundary_env+=(EXITOS_CAMPAIGN_TEST_MODE=1
                                "$poll_override=$poll_override_value")
            ;;
          absent-test-mode)
            poll_boundary_env+=("$poll_override=$poll_override_value")
            ;;
          set-empty)
            poll_boundary_env+=(EXITOS_CAMPAIGN_TEST_MODE=1
                                "$poll_override=")
            ;;
          absent-test-mode-set-empty)
            poll_boundary_env+=("$poll_override=")
            ;;
        esac
        env "${poll_boundary_env[@]}" \
            CPU=0 REPS=5 ITERS=1 SETTLE=0 \
            EXITOS_TESTDISK="$controller_boundary_resolver" \
            EXITOS_QDSPLIT_BIN="$controller_boundary_qdsplit" \
            CONTROLLER_BOUNDARY_RESOLVER_RAN="$controller_boundary_resolver_ran" \
            CONTROLLER_BOUNDARY_QDSPLIT_RAN="$controller_boundary_qdsplit_ran" \
            bash tools/campaign-cstate-keeper.sh --private-mount-namespace-run \
            >"$tmp/poll-boundary-$poll_override-$poll_boundary_case.out" \
            2>"$tmp/poll-boundary-$poll_override-$poll_boundary_case.err" ||
            poll_boundary_rc=$?
        expect_ok "$poll_override is rejected in production ($poll_boundary_case)" \
            sh -c 'test "$1" -eq 2 && test ! -e "$2" && test ! -e "$3" && test "$(wc -l <"$4")" -eq 1 && grep -Fxq "cstate campaign: poll snapshot overrides are pure-test-only" "$4"' \
                sh "$poll_boundary_rc" "$controller_boundary_resolver_ran" \
                "$controller_boundary_qdsplit_ran" \
                "$tmp/poll-boundary-$poll_override-$poll_boundary_case.err"
    done
done

# fd9 is the launcher's one reserved transfer descriptor.  A caller that
# already owns it must be rejected before the private state directory is
# created; silently replacing an inherited descriptor would violate the
# hidden CLI's fd contract.
poll_fd9_in_use_out="$tmp/poll-fd9-in-use.out"
poll_fd9_in_use_err="$tmp/poll-fd9-in-use.err"
poll_fd9_in_use_rc=0
(
    exec 9<"$poll_high_fd_sentinel"
    env EXITOS_CAMPAIGN_TEST_MODE=1 "${poll_fixture_env[@]}" \
        bash tools/campaign-cstate-keeper.sh --test-poll-snapshot
) >"$poll_fd9_in_use_out" 2>"$poll_fd9_in_use_err" || poll_fd9_in_use_rc=$?
expect_ok "poll snapshot rejects inherited fd9 before creating private state" \
    sh -c 'test "$1" -eq 1 && test ! -s "$2" && test "$(wc -l <"$3")" -eq 1 && grep -Fxq "poll snapshot: parent_fd9_in_use" "$3" && test -z "$(find "$4" -mindepth 1 -maxdepth 1 -print -quit)"' \
        sh "$poll_fd9_in_use_rc" "$poll_fd9_in_use_out" \
        "$poll_fd9_in_use_err" "$poll_state_parent"

# First pause the campaign after retaining all device objects but before the
# fd9 dup.  Replace the whole pathname, then continue to the Python pause
# before its first sysfs open.  This distinguishes a dup of the retained fd
# from reopening EXITOS_DEV, as well as auditing the helper's complete fd set.
poll_pause_out="$tmp/poll-pause.out"
poll_pause_err="$tmp/poll-pause.err"
poll_whole_retained_path="$poll_dev/nvme0n1.retained-for-pause"
poll_whole_object_id=$(stat -Lc '%d:%i' -- "$poll_whole")
poll_part_object_id=$(stat -Lc '%d:%i' -- "$poll_part")
poll_char_object_id=$(stat -Lc '%d:%i' -- "$poll_char")
poll_ctrl_object_id=$(stat -Lc '%d:%i' -- "$poll_ctrl_node")
poll_high_fd_object_id=$(stat -Lc '%d:%i' -- "$poll_high_fd_sentinel")
poll_device_object_count=$(printf '%s\n' \
    "$poll_whole_object_id" "$poll_part_object_id" \
    "$poll_char_object_id" "$poll_ctrl_object_id" | sort -u | wc -l)
poll_retained_fd_snapshot() {
    local pid=$1 fd_path fd object_id key value extra retained_count=0
    local pos flags expected
    local -a fd_paths=()
    local -A fd_for=() pos_for=() flags_for=()
    [[ $pid =~ ^[1-9][0-9]*$ ]] || return 1
    shopt -s nullglob
    fd_paths=("/proc/$pid/fd"/*)
    shopt -u nullglob
    [ "${#fd_paths[@]}" -ge 4 ] || return 1
    for fd_path in "${fd_paths[@]}"; do
        fd=${fd_path##*/}
        [[ $fd =~ ^[0-9]+$ ]] || return 1
        object_id=$(stat -Lc '%d:%i' -- "$fd_path" 2>/dev/null) || return 1
        case "|$poll_whole_object_id|$poll_part_object_id|$poll_char_object_id|$poll_ctrl_object_id|" in
          *"|$object_id|"*) ;;
          *) continue ;;
        esac
        [ -z "${fd_for[$object_id]+set}" ] || return 1
        pos=""; flags=""
        while read -r key value extra; do
            case "$key" in
              pos:)
                [ -z "$pos" ] || return 1
                [[ $value =~ ^[0-9]+$ ]] || return 1
                pos=$value
                ;;
              flags:)
                [ -z "$flags" ] || return 1
                [[ $value =~ ^0[0-7]+$ ]] || return 1
                flags=$value
                ;;
            esac
        done <"/proc/$pid/fdinfo/$fd" || return 1
        [ -n "$pos" ] && [ -n "$flags" ] || return 1
        fd_for[$object_id]=$fd
        pos_for[$object_id]=$pos
        flags_for[$object_id]=$flags
        retained_count=$((retained_count + 1))
    done
    [ "$retained_count" -eq 4 ] || return 1
    for expected in "$poll_whole_object_id" "$poll_part_object_id" \
                    "$poll_char_object_id" "$poll_ctrl_object_id"; do
        [ -n "${fd_for[$expected]+set}" ] || return 1
        printf '%s=%s:%s:%s;' "$expected" "${fd_for[$expected]}" \
            "${pos_for[$expected]}" "${flags_for[$expected]}"
    done
}
exec 200<"$poll_high_fd_sentinel"
env EXITOS_CAMPAIGN_TEST_MODE=1 "${poll_fixture_env[@]}" \
    EXITOS_TEST_POLL_PAUSE_BEFORE=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER=1 \
    EXITOS_TEST_POLL_HELPER_PAUSE=1 \
    bash tools/campaign-cstate-keeper.sh --test-poll-snapshot \
    >"$poll_pause_out" 2>"$poll_pause_err" & poll_pause_campaign_pid=$!
poll_parent_pid=""
poll_parent_marker_start=""
for _ in $(seq 1 200); do
    poll_parent_line=$(sed -n \
        '/^POLL_SNAPSHOT_PARENT_READY pid=[1-9][0-9]* starttime=[1-9][0-9]*$/p' \
        "$poll_pause_err" 2>/dev/null)
    if [[ $poll_parent_line =~ ^POLL_SNAPSHOT_PARENT_READY\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
        poll_parent_pid=${BASH_REMATCH[1]}
        poll_parent_marker_start=${BASH_REMATCH[2]}
        break
    fi
    kill -0 "$poll_pause_campaign_pid" 2>/dev/null || break
    sleep 0.01
done
poll_parent_reached=0
poll_parent_start_before=""
poll_parent_start_after=""
poll_parent_retained_before=""
poll_parent_fd9_before=present
poll_parent_fd200_identity=""
poll_replacement_object_id=""
if [ "$poll_parent_pid" = "$poll_pause_campaign_pid" ] &&
   wait_for_stopped_state "$poll_parent_pid"; then
    poll_parent_reached=1
    poll_parent_start_before=$(proc_starttime "$poll_parent_pid" || true)
    poll_parent_retained_before=$(poll_retained_fd_snapshot "$poll_parent_pid" || true)
    [ -e "/proc/$poll_parent_pid/fd/9" ] || poll_parent_fd9_before=absent
    poll_parent_fd200_identity=$(stat -Lc '%d:%i' -- "/proc/$poll_parent_pid/fd/200" 2>/dev/null || true)
    mv -- "$poll_whole" "$poll_whole_retained_path"
    printf '%s\n' 9001 >"$poll_whole"
    poll_replacement_object_id=$(stat -Lc '%d:%i' -- "$poll_whole")
    poll_parent_start_after=$(proc_starttime "$poll_parent_pid" || true)
    kill -CONT "$poll_parent_pid" 2>/dev/null || true
fi
poll_helper_pid=""
poll_helper_marker_start=""
for _ in $(seq 1 200); do
    poll_helper_line=$(sed -n \
        '/^POLL_SNAPSHOT_HELPER_READY pid=[1-9][0-9]* starttime=[1-9][0-9]*$/p' \
        "$poll_pause_err" 2>/dev/null)
    if [[ $poll_helper_line =~ ^POLL_SNAPSHOT_HELPER_READY\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
        poll_helper_pid=${BASH_REMATCH[1]}
        poll_helper_marker_start=${BASH_REMATCH[2]}
        break
    fi
    kill -0 "$poll_pause_campaign_pid" 2>/dev/null || break
    sleep 0.01
done
poll_helper_reached=0
poll_helper_fd_token=""
poll_helper_fd9_identity=""
poll_helper_fd9_flags=""
poll_helper_fd9_flags_decimal=0
poll_helper_cmdline_token=""
poll_helper_ppid=""
poll_helper_stdio_clean=1
poll_helper_start_before=""
poll_helper_start_after=""
if [[ $poll_helper_pid =~ ^[1-9][0-9]*$ ]] &&
   wait_for_stopped_state "$poll_helper_pid"; then
    poll_helper_reached=1
    poll_helper_start_before=$(proc_starttime "$poll_helper_pid" || true)
    poll_helper_argv=()
    mapfile -d '' -t poll_helper_argv <"/proc/$poll_helper_pid/cmdline" || true
    if [ "${#poll_helper_argv[@]}" -ge 4 ]; then
        poll_helper_cmdline_token="${poll_helper_argv[0]##*/}|${poll_helper_argv[1]}|${poll_helper_argv[2]}|${poll_helper_argv[3]}"
    fi
    while read -r poll_helper_status_key poll_helper_status_value poll_helper_status_extra; do
        if [ "$poll_helper_status_key" = PPid: ]; then
            [ -z "$poll_helper_ppid" ] || poll_helper_ppid=duplicate
            poll_helper_ppid=$poll_helper_status_value
        fi
    done <"/proc/$poll_helper_pid/status" || poll_helper_ppid=unreadable
    poll_helper_fds=()
    shopt -s nullglob
    poll_helper_fd_paths=("/proc/$poll_helper_pid/fd"/*)
    shopt -u nullglob
    poll_helper_fd_audit=1
    declare -A poll_helper_fd_seen=()
    for poll_helper_fd_path in "${poll_helper_fd_paths[@]}"; do
        poll_helper_fd=${poll_helper_fd_path##*/}
        [[ $poll_helper_fd =~ ^[0-9]+$ ]] || poll_helper_fd_audit=0
        poll_helper_fd_object_id=$(stat -Lc '%d:%i' -- "$poll_helper_fd_path" 2>/dev/null) ||
            poll_helper_fd_audit=0
        if [ "$poll_helper_fd" -le 2 ]; then
            case "|$poll_whole_object_id|$poll_part_object_id|$poll_char_object_id|$poll_ctrl_object_id|" in
              *"|$poll_helper_fd_object_id|"*) poll_helper_stdio_clean=0 ;;
            esac
        fi
        poll_helper_fd_seen[$poll_helper_fd]=1
    done
    if [ "$poll_helper_fd_audit" -eq 1 ] &&
       [ "${#poll_helper_fd_paths[@]}" -eq 4 ] &&
       [ "${poll_helper_fd_seen[0]+yes}" = yes ] &&
       [ "${poll_helper_fd_seen[1]+yes}" = yes ] &&
       [ "${poll_helper_fd_seen[2]+yes}" = yes ] &&
       [ "${poll_helper_fd_seen[9]+yes}" = yes ]; then
        poll_helper_fd_token=0,1,2,9
    fi
    poll_helper_fd9_identity=$(stat -Lc '%d:%i' -- "/proc/$poll_helper_pid/fd/9" 2>/dev/null || true)
    poll_helper_fd9_flags=$(awk '$1 == "flags:" { value=$2; count++ } END { if (count == 1) print value }' \
        "/proc/$poll_helper_pid/fdinfo/9" 2>/dev/null || true)
    if [[ $poll_helper_fd9_flags =~ ^0[0-7]+$ ]]; then
        poll_helper_fd9_flags_decimal=$((8#$poll_helper_fd9_flags))
    fi
    poll_helper_start_after=$(proc_starttime "$poll_helper_pid" || true)
    kill -CONT "$poll_helper_pid" 2>/dev/null || true
fi
poll_parent_after_pid=""
poll_parent_after_marker_start=""
for _ in $(seq 1 200); do
    poll_parent_after_line=$(sed -n \
        '/^POLL_SNAPSHOT_PARENT_AFTER pid=[1-9][0-9]* starttime=[1-9][0-9]*$/p' \
        "$poll_pause_err" 2>/dev/null)
    if [[ $poll_parent_after_line =~ ^POLL_SNAPSHOT_PARENT_AFTER\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
        poll_parent_after_pid=${BASH_REMATCH[1]}
        poll_parent_after_marker_start=${BASH_REMATCH[2]}
        break
    fi
    kill -0 "$poll_pause_campaign_pid" 2>/dev/null || break
    sleep 0.01
done
poll_parent_after_reached=0
poll_parent_after_start_before=""
poll_parent_after_start_after=""
poll_parent_retained_after=""
poll_parent_fd9_after=present
if [ "$poll_parent_after_pid" = "$poll_pause_campaign_pid" ] &&
   wait_for_stopped_state "$poll_parent_after_pid"; then
    poll_parent_after_reached=1
    poll_parent_after_start_before=$(proc_starttime "$poll_parent_after_pid" || true)
    poll_parent_retained_after=$(poll_retained_fd_snapshot "$poll_parent_after_pid" || true)
    [ -e "/proc/$poll_parent_after_pid/fd/9" ] || poll_parent_fd9_after=absent
    poll_parent_after_start_after=$(proc_starttime "$poll_parent_after_pid" || true)
    kill -CONT "$poll_parent_after_pid" 2>/dev/null || true
fi
poll_pause_rc=0
if ! wait_for_exit_or_zombie "$poll_pause_campaign_pid" 400; then
    [ -z "$poll_helper_pid" ] || bounded_terminate_and_wait "$poll_helper_pid" 2>/dev/null || true
    bounded_terminate_and_wait "$poll_pause_campaign_pid" 2>/dev/null || poll_pause_rc=$?
else
    wait "$poll_pause_campaign_pid" 2>/dev/null || poll_pause_rc=$?
fi
exec 200<&-
if [ -e "$poll_whole_retained_path" ]; then
    rm -f -- "$poll_whole"
    mv -- "$poll_whole_retained_path" "$poll_whole"
fi
expect_ok "pre-dup pause replaced whole pathname after retaining four distinct objects" \
    bash -c 'test "$1" -eq 1 && test "$2" -eq 4 && [[ $3 =~ ^[1-9][0-9]*$ ]] && test "$3" = "$4" && test "$4" = "$5" && test -n "$6" && test "$7" = absent && test "$8" != "$9" && test "${10}" = "${11}"' \
        sh "$poll_parent_reached" "$poll_device_object_count" \
        "$poll_parent_marker_start" "$poll_parent_start_before" \
        "$poll_parent_start_after" "$poll_parent_retained_before" \
        "$poll_parent_fd9_before" "$poll_whole_object_id" \
        "$poll_replacement_object_id" "$poll_parent_fd200_identity" \
        "$poll_high_fd_object_id"
expect_ok "stopped poll helper has only 0,1,2,9 and fd9 is retained whole, read-only, CLOEXEC" \
    bash -c 'test "$1" -eq 1 && test "$2" = 0,1,2,9 && test "$3" = "$4" && test "$3" != "$5" && [[ $6 =~ ^[1-9][0-9]*$ ]] && test "$6" = "$7" && test "$7" = "$8" && test $(( $9 & 3 )) -eq 0 && test $(( $9 & 02000000 )) -ne 0 && [[ ${10} =~ ^python3([.][0-9]+)?\|-I\|-S\|-$ ]] && test "${11}" = "${12}" && test "${13}" -eq 1' \
        sh "$poll_helper_reached" "$poll_helper_fd_token" \
        "$poll_helper_fd9_identity" "$poll_whole_object_id" \
        "$poll_replacement_object_id" "$poll_helper_marker_start" \
        "$poll_helper_start_before" "$poll_helper_start_after" \
        "$poll_helper_fd9_flags_decimal" "$poll_helper_cmdline_token" \
        "$poll_helper_ppid" "$poll_pause_campaign_pid" \
        "$poll_helper_stdio_clean"
expect_ok "snapshot leaves all four parent retained fd numbers, identities, flags, and offsets unchanged" \
    bash -c 'test "$1" -eq 1 && [[ $2 =~ ^[1-9][0-9]*$ ]] && test "$2" = "$3" && test "$3" = "$4" && test "$5" = absent && test -n "$6" && test "$6" = "$7"' \
        sh "$poll_parent_after_reached" "$poll_parent_after_marker_start" \
        "$poll_parent_after_start_before" "$poll_parent_after_start_after" \
        "$poll_parent_fd9_after" "$poll_parent_retained_before" \
        "$poll_parent_retained_after"
expect_ok "continued poll helper is boundedly reaped and leaves no PID, fd9, or private state" \
    sh -c 'test "$1" -eq 0 && test ! -e "/proc/$2/status" && test ! -e "/proc/$2/fd/9" && cmp -s -- "$3" "$4" && test -z "$(find "$5" -mindepth 1 -maxdepth 1 -print -quit)"' \
        sh "$poll_pause_rc" "${poll_helper_pid:-0}" "$poll_golden" \
        "$poll_pause_out" "$poll_state_parent"

# Re-hash a helper-produced private frame after replacing only one canonical
# CPU token.  These inputs all match the broad wire grammar, so rejection
# proves the parent Bash parser independently enforces canonical interval
# semantics instead of trusting either the helper or the SHA header.
poll_expect_parent_cpu_token_refusal() {
    local suffix=$1 bad_cpu=$2 label=$3 campaign_pid="" parent_pid=""
    local marker_start="" line="" reached=0 mutate_rc=1 rc=0
    local -a frame_files=()
    env EXITOS_CAMPAIGN_TEST_MODE=1 "${poll_fixture_env[@]}" \
        EXITOS_TEST_POLL_PAUSE_AFTER=1 \
        bash tools/campaign-cstate-keeper.sh --test-poll-snapshot \
        >"$tmp/poll-frame-$suffix.out" \
        2>"$tmp/poll-frame-$suffix.err" & campaign_pid=$!
    for _ in $(seq 1 200); do
        line=$(sed -n \
            '/^POLL_SNAPSHOT_PARENT_AFTER pid=[1-9][0-9]* starttime=[1-9][0-9]*$/p' \
            "$tmp/poll-frame-$suffix.err" 2>/dev/null)
        if [[ $line =~ ^POLL_SNAPSHOT_PARENT_AFTER\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
            parent_pid=${BASH_REMATCH[1]}
            marker_start=${BASH_REMATCH[2]}
            break
        fi
        kill -0 "$campaign_pid" 2>/dev/null || break
        sleep 0.01
    done
    if [ "$parent_pid" = "$campaign_pid" ] &&
       wait_for_stopped_state "$parent_pid" &&
       [ "$(proc_starttime "$parent_pid")" = "$marker_start" ]; then
        reached=1
        mapfile -t frame_files < <(
            find "$poll_state_parent" -mindepth 2 -maxdepth 2 -type f -print
        )
        if [ "${#frame_files[@]}" -eq 1 ]; then
            mutate_rc=0
            python3 - "${frame_files[0]}" "$bad_cpu" <<'PY' || mutate_rc=$?
import hashlib
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
cpu = sys.argv[2].encode("ascii", "strict")
data = path.read_bytes()
header, separator, body = data.partition(b"\n")
if not separator or not body.endswith(b"\n"):
    raise SystemExit(90)
lines = body.splitlines(keepends=True)
matches = [index for index, row in enumerate(lines)
           if row.startswith(b"mq_hctx=2:")]
if len(matches) != 1:
    raise SystemExit(91)
index = matches[0]
row = lines[index]
if not row.endswith(b"\n"):
    raise SystemExit(92)
prefix, colon, _ = row[:-1].rpartition(b":")
if not colon:
    raise SystemExit(93)
lines[index] = prefix + b":" + cpu + b"\n"
body = b"".join(lines)
digest = hashlib.sha256(body).hexdigest().encode("ascii")
path.write_bytes(b"snapshot_sha256=" + digest + b"\n" + body)
PY
        fi
        kill -CONT "$parent_pid" 2>/dev/null || true
    fi
    if ! wait_for_exit_or_zombie "$campaign_pid" 400; then
        bounded_terminate_and_wait "$campaign_pid" 2>/dev/null || rc=$?
    else
        wait "$campaign_pid" 2>/dev/null || rc=$?
    fi
    expect_ok "$label" \
        sh -c 'test "$1" -eq 1 && test "$2" -eq 0 && test "$3" -eq 1 && test ! -s "$4" && grep -Fxq "poll snapshot: frame_malformed" "$5" && test ! -e "/proc/$6/status" && test -z "$(find "$7" -mindepth 1 -maxdepth 1 -print -quit)"' \
            sh "$reached" "$mutate_rc" "$rc" \
            "$tmp/poll-frame-$suffix.out" "$tmp/poll-frame-$suffix.err" \
            "${parent_pid:-0}" "$poll_state_parent"
}

poll_expect_parent_cpu_token_refusal reversed-range 4-3 \
    "parent frame parser rejects a reversed canonical CPU range"
poll_expect_parent_cpu_token_refusal singleton-range 4-4 \
    "parent frame parser rejects a noncanonical singleton CPU range"
poll_expect_parent_cpu_token_refusal overlap '0-2,2-4' \
    "parent frame parser rejects overlapping canonical CPU ranges"
poll_expect_parent_cpu_token_refusal adjacent '0-2,3-4' \
    "parent frame parser rejects adjacent unmerged canonical CPU ranges"
poll_expect_parent_cpu_token_refusal numeric-unsorted '4,0-2' \
    "parent frame parser rejects numerically unsorted canonical CPU ranges"
poll_expect_parent_cpu_token_refusal trailing-comma '0-4,' \
    "parent frame parser rejects a CPU token with a swallowed trailing field"
poll_expect_parent_cpu_token_refusal trailing-colon '0-4:' \
    "parent frame parser rejects an mq row with a swallowed trailing field"

# A second pause occurs inside that same Python helper after capture 1.  A
# valid sysfs change must make capture 2 differ, fail as snapshot_unstable,
# and publish zero bytes to the caller.
poll_unstable_out="$tmp/poll-unstable.out"
poll_unstable_err="$tmp/poll-unstable.err"
env EXITOS_CAMPAIGN_TEST_MODE=1 "${poll_fixture_env[@]}" \
    EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1=1 \
    bash tools/campaign-cstate-keeper.sh --test-poll-snapshot \
    >"$poll_unstable_out" 2>"$poll_unstable_err" & poll_unstable_campaign_pid=$!
poll_capture1_pid=""
poll_capture1_marker_start=""
for _ in $(seq 1 200); do
    poll_capture1_line=$(sed -n \
        '/^POLL_SNAPSHOT_CAPTURE1_READY pid=[1-9][0-9]* starttime=[1-9][0-9]*$/p' \
        "$poll_unstable_err" 2>/dev/null)
    if [[ $poll_capture1_line =~ ^POLL_SNAPSHOT_CAPTURE1_READY\ pid=([1-9][0-9]*)\ starttime=([1-9][0-9]*)$ ]]; then
        poll_capture1_pid=${BASH_REMATCH[1]}
        poll_capture1_marker_start=${BASH_REMATCH[2]}
        break
    fi
    kill -0 "$poll_unstable_campaign_pid" 2>/dev/null || break
    sleep 0.01
done
poll_capture1_reached=0
poll_capture1_start_before=""
poll_capture1_start_after=""
if [[ $poll_capture1_pid =~ ^[1-9][0-9]*$ ]] &&
   wait_for_stopped_state "$poll_capture1_pid"; then
    poll_capture1_reached=1
    poll_capture1_start_before=$(proc_starttime "$poll_capture1_pid" || true)
    printf '%s\n' 0-5 >"$poll_mq/2/cpu_list"
    poll_capture1_start_after=$(proc_starttime "$poll_capture1_pid" || true)
    kill -CONT "$poll_capture1_pid" 2>/dev/null || true
fi
poll_unstable_rc=0
if ! wait_for_exit_or_zombie "$poll_unstable_campaign_pid" 400; then
    [ -z "$poll_capture1_pid" ] || bounded_terminate_and_wait "$poll_capture1_pid" 2>/dev/null || true
    bounded_terminate_and_wait "$poll_unstable_campaign_pid" 2>/dev/null || poll_unstable_rc=$?
else
    wait "$poll_unstable_campaign_pid" 2>/dev/null || poll_unstable_rc=$?
fi
printf '%s\n' 0-4 >"$poll_mq/2/cpu_list"
expect_ok "same helper detects valid capture1-to-capture2 drift as snapshot_unstable" \
    bash -c 'test "$1" -eq 1 && [[ $2 =~ ^[1-9][0-9]*$ ]] && test "$2" = "$3" && test "$3" = "$4" && test "$5" -ne 0 && test ! -s "$6" && grep -q "poll snapshot: snapshot_unstable" "$7" && test ! -e "/proc/$8/status" && test -z "$(find "$9" -mindepth 1 -maxdepth 1 -print -quit)"' \
        sh "$poll_capture1_reached" "$poll_capture1_marker_start" \
        "$poll_capture1_start_before" "$poll_capture1_start_after" \
        "$poll_unstable_rc" "$poll_unstable_out" "$poll_unstable_err" \
        "${poll_capture1_pid:-0}" "$poll_state_parent"

expect_ok "snapshot launcher guards the sole retained-WHOLE_FD dup into fd9" \
    rg -U -q '^        if ! \{ exec 9<&"\$WHOLE_FD"; \} 2>/dev/null; then\n            echo "poll snapshot: fd9_dup_failed" >&2\n            exit 1\n        fi$' \
        tools/campaign-cstate-keeper.sh
expect_ok "snapshot source requires controller-char, whole, and generic controller inode equality" \
    rg -U -q 'inode_token\(controller_info\) != inode_token\(generic_controller_info\).*\n.*inode_token\(controller_info\) != inode_token\(controller_char_info\)' \
        tools/campaign-cstate-keeper.sh

# The system C header is an independent oracle for the Python ioctl request.
poll_ioctl_c="$tmp/poll-ioctl.c"
poll_ioctl_bin="$tmp/poll-ioctl"
printf '%s\n' \
    '#include <linux/fs.h>' \
    '#include <stdio.h>' \
    'int main(void) {' \
    '    printf("0x%lx\n", (unsigned long)BLKGETDISKSEQ);' \
    '    return 0;' \
    '}' >"$poll_ioctl_c"
poll_ioctl_cc_rc=0
cc "$poll_ioctl_c" -o "$poll_ioctl_bin" || poll_ioctl_cc_rc=$?
poll_ioctl_header=""
if [ "$poll_ioctl_cc_rc" -eq 0 ]; then
    poll_ioctl_header=$($poll_ioctl_bin)
fi
expect_ok "executed fake decode and production ABI source bind the probed ioctl to an exact native uint64 buffer" \
    sh -c 'test "$1" -eq 0 && test "$2" = 0x80081280 && rg -Fq "BLKGETDISKSEQ = 0x80081280" "$3" && rg -Fq "diskseq_buffer = bytearray(8)" "$3" && rg -Fq "ioctl_rc = fcntl.ioctl(9, BLKGETDISKSEQ, diskseq_buffer, True)" "$3" && rg -Fq "if ioctl_rc != 0:" "$3" && rg -Fq "if len(diskseq_buffer) != 8:" "$3" && rg -Fq "struct.unpack(\"=Q\", diskseq_buffer)" "$3" && rg -Fq "decode_diskseq_buffer(struct.pack(\"=Q\", value))" "$3"' \
        sh "$poll_ioctl_cc_rc" "$poll_ioctl_header" \
        tools/campaign-cstate-keeper.sh

# Exercise the four real long-lived launch branches after all four device
# descriptors are retained.  An independent executable scans each live target
# PID after the close boundary; in particular, the noise sentinel may not
# certify its own fd table.  Parent-side identity checks after every child prove
# the closes remain local to the subshell; the final record proves normal
# cleanup closes the parent copies.
hygiene_bin="$tmp/hygiene-bin"
hygiene_tmp="$tmp/hygiene-tmp"
hygiene_audit="$tmp/hygiene-audit"
hygiene_mount="$tmp/hygiene-mount"
hygiene_proc="$tmp/hygiene-proc"
hygiene_fdinfo="$tmp/hygiene-mount-fdinfo"
hygiene_auditor="$tmp/hygiene-fd-auditor"
mkdir "$hygiene_bin" "$hygiene_tmp" "$hygiene_audit" \
    "$hygiene_mount" "$hygiene_proc"
printf '%s\n' 'cpu 1 2 3 4' >"$hygiene_proc/stat"
printf 'mnt_id:\t%s\n' 77 >"$hygiene_fdinfo"
hygiene_object_ids=$(printf '%s|%s|%s|%s' \
    "$(stat -Lc '%d:%i' -- "$identity_whole")" \
    "$(stat -Lc '%d:%i' -- "$identity_part")" \
    "$(stat -Lc '%d:%i' -- "$identity_char")" \
    "$(stat -Lc '%d:%i' -- "$identity_ctrl_node")")
printf '%s\n' \
    '#!/bin/bash' \
    'test "$#" -eq 2 || exit 80' \
    'helper=$1' \
    'target_pid=$2' \
    '[[ $target_pid =~ ^[1-9][0-9]*$ ]] || exit 81' \
    'target_state() {' \
    '  local line key state description count=0 found=""' \
    '  while IFS= read -r line; do' \
    '    case "$line" in' \
    '      State:*)' \
    '        read -r key state description <<<"$line" || return 1' \
    '        test "$key" = State: && [[ $state =~ ^[A-Za-z]$ ]] || return 1' \
    '        count=$((count + 1))' \
    '        found=$state' \
    '        ;;' \
    '    esac' \
    '  done <"/proc/$target_pid/status" || return 1' \
    '  test "$count" -eq 1 || return 1' \
    '  case "$found" in Z|X|x) return 1 ;; esac' \
    '  printf "%s\n" "$found"' \
    '}' \
    'target_starttime() {' \
    '  local line rest' \
    '  local -a fields=()' \
    '  IFS= read -r line <"/proc/$target_pid/stat" || return 1' \
    '  [[ $line == *") "* ]] || return 1' \
    '  rest=${line##*) }' \
    '  read -r -a fields <<<"$rest" || return 1' \
    '  test "${#fields[@]}" -ge 20 || return 1' \
    '  [[ ${fields[19]} =~ ^[1-9][0-9]*$ ]] || return 1' \
    '  printf "%s\n" "${fields[19]}"' \
    '}' \
    'state_before=$(target_state) || exit 82' \
    'starttime_before=$(target_starttime) || exit 83' \
    'test -d "/proc/$target_pid/fd" || exit 84' \
    'kill -0 "$target_pid" 2>/dev/null || exit 85' \
    'retained_open=0' \
    'shopt -s nullglob' \
    'fd_paths=("/proc/$target_pid/fd"/[0-9]*)' \
    'shopt -u nullglob' \
    'for fd_path in "${fd_paths[@]}"; do' \
    '  object_id=$(stat -Lc "%d:%i" -- "$fd_path" 2>/dev/null) || exit 86' \
    '  [[ $object_id =~ ^[0-9]+:[0-9]+$ ]] || exit 87' \
    '  case "|$EXITOS_TEST_FD_OBJECT_IDS|" in' \
    '    *"|$object_id|"*) retained_open=$((retained_open + 1)) ;;' \
    '  esac' \
    'done' \
    'state_after=$(target_state) || exit 88' \
    'starttime_after=$(target_starttime) || exit 89' \
    'test -d "/proc/$target_pid/fd" || exit 90' \
    'kill -0 "$target_pid" 2>/dev/null || exit 91' \
    'test "$starttime_before" = "$starttime_after" || exit 92' \
    'if test "$retained_open" -eq 0; then status=verified; else status=failed; fi' \
    'printf "CHILD_FD_HYGIENE helper=%s observer_pid=%s target_pid=%s retained_open=%s status=%s\n" "$helper" "$BASHPID" "$target_pid" "$retained_open" "$status" >"$EXITOS_TEST_CHILD_FD_AUDIT_DIR/$helper"' \
    'test "$retained_open" -eq 0' \
    >"$hygiene_auditor"
printf '%s\n' \
    '#!/bin/bash' \
    'test "$#" -ge 3 && test "$1" = -n && test "$2" = -20 || exit 90' \
    'shift 2' \
    'exec "$@"' \
    >"$hygiene_bin/nice"
printf '%s\n' \
    '#!/bin/bash' \
    'test "$#" -ge 3 && test "$1" = -c || exit 91' \
    'shift 2' \
    'exec "$@"' \
    >"$hygiene_bin/taskset"
printf '%s\n' '#!/bin/bash' \
    '"$EXITOS_TEST_FD_AUDITOR" qdsplit "$BASHPID"' \
    >"$hygiene_bin/qdsplit"
printf '%s\n' '#!/bin/bash' \
    '"$EXITOS_TEST_FD_AUDITOR" keeper "$BASHPID"' \
    >"$hygiene_bin/keeper"
printf '%s\n' \
    '#!/bin/bash' \
    'test "$#" -eq 1 || exit 92' \
    '"$EXITOS_TEST_FD_AUDITOR" mount-root-holder "$BASHPID" || exit 96' \
    'exec {held_fd}<"$1" || exit 93' \
    'printf "READY\t%s\t%s\t77\n" "$BASHPID" "$held_fd"' \
    'IFS= read -r command || exit 94' \
    'test "$command" = CLOSE || exit 95' \
    'exec {held_fd}<&-' \
    'printf "CLOSED\n"' \
    >"$hygiene_bin/mount-holder"
chmod 0700 "$hygiene_bin/nice" "$hygiene_bin/taskset" \
    "$hygiene_bin/qdsplit" "$hygiene_bin/keeper" \
    "$hygiene_bin/mount-holder" "$hygiene_auditor"

# A zombie still answers kill -0 and can retain an empty /proc/<pid>/fd
# directory.  Prove the independent observer rejects that state rather than
# treating an unscannable final child as a verified zero-fd result.
hygiene_zombie_pid_file="$tmp/hygiene-zombie.pid"
hygiene_zombie_audit="$hygiene_audit/zombie-probe"
python3 -c '
import os
import time

child = os.fork()
if child == 0:
    os._exit(0)
print(child, flush=True)
time.sleep(30)
os.waitpid(child, 0)
' >"$hygiene_zombie_pid_file" & hygiene_zombie_parent=$!
hygiene_zombie_pid=""
hygiene_zombie_ready=0
for _ in $(seq 1 200); do
    IFS= read -r hygiene_zombie_pid <"$hygiene_zombie_pid_file" 2>/dev/null || true
    if [[ $hygiene_zombie_pid =~ ^[1-9][0-9]*$ ]] &&
       [ -r "/proc/$hygiene_zombie_pid/status" ] &&
       [ "$(awk '$1 == "State:" { print $2 }' "/proc/$hygiene_zombie_pid/status")" = Z ]; then
        hygiene_zombie_ready=1
        break
    fi
    kill -0 "$hygiene_zombie_parent" 2>/dev/null || break
    sleep 0.01
done
hygiene_zombie_audit_rc=0
if [ "$hygiene_zombie_ready" -eq 1 ]; then
    env EXITOS_TEST_CHILD_FD_AUDIT_DIR="$hygiene_audit" \
        EXITOS_TEST_FD_OBJECT_IDS="$hygiene_object_ids" \
        "$hygiene_auditor" zombie-probe "$hygiene_zombie_pid" ||
        hygiene_zombie_audit_rc=$?
fi
kill -TERM "$hygiene_zombie_parent" 2>/dev/null || true
wait "$hygiene_zombie_parent" 2>/dev/null || true
expect_ok "independent fd auditor refuses a zombie target" \
    sh -c 'test "$1" -eq 1 && test "$2" -ne 0 && test ! -e "$3"' \
        sh "$hygiene_zombie_ready" "$hygiene_zombie_audit_rc" \
        "$hygiene_zombie_audit"

hygiene_out="$tmp/hygiene.out"
hygiene_err="$tmp/hygiene.err"
hygiene_rc=0
env CPU=0 REPS=1 ITERS=1 SETTLE=0 EXITOS_CAMPAIGN_TEST_MODE=1 \
    PATH="$hygiene_bin:$PATH" EXITOS_QDSPLIT_BIN="$hygiene_bin/qdsplit" \
    EXITOS_KEEPER_BIN="$hygiene_bin/keeper" \
    EXITOS_DEV="$identity_whole" EXITOS_TESTPART="$identity_part" \
    EXITOS_CHARDEV="$identity_char" EXITOS_TEST_CTRLDEV="$identity_ctrl_node" \
    EXITOS_EXPECT_SERIAL=SERIAL-1 EXITOS_DEVICE_SYSFS_ROOT="$identity_sys" \
    EXITOS_TEST_WHOLE_MAJMIN=259:0 EXITOS_TEST_PART_MAJMIN=259:1 \
    EXITOS_TEST_CHAR_MAJMIN=240:0 EXITOS_TEST_CTRL_MAJMIN=241:0 \
    EXITOS_TEST_CTRL_SYSFS_PATH="$identity_ctrl" \
    EXITOS_TEST_NVME_BIN="$identity_nvme" \
    IDENTITY_NVME_LOG="$identity_nvme_log" IDENTITY_CHAR_PATH="$identity_char" \
    EXITOS_TEST_CHILD_FD_AUDIT_DIR="$hygiene_audit" \
    EXITOS_TEST_FD_AUDITOR="$hygiene_auditor" \
    EXITOS_TEST_FD_OBJECT_IDS="$hygiene_object_ids" \
    EXITOS_TEST_CHILD_FD_TMP="$hygiene_tmp" \
    EXITOS_IRQ_PROC_ROOT="$hygiene_proc" EXITOS_TESTMNT="$hygiene_mount" \
    EXITOS_TEST_MOUNT_ID=77 \
    EXITOS_TEST_MOUNT_FD_HELPER="$hygiene_bin/mount-holder" \
    EXITOS_TEST_MOUNT_FD_ID_FILE="$hygiene_fdinfo" \
    bash tools/campaign-cstate-keeper.sh --test-child-fd-hygiene \
    >"$hygiene_out" 2>"$hygiene_err" || hygiene_rc=$?
expect_ok "all four child closes are subshell-local and final parent cleanup closes once" \
    sh -c 'test "$1" -eq 0 && test ! -s "$2" && test "$(grep -c "^PARENT_DEVICE_FDS phase=after-.* status=verified$" "$3")" -eq 4 && grep -q "^PARENT_DEVICE_FDS phase=cleanup status=closed$" "$3"' \
        sh "$hygiene_rc" "$hygiene_err" "$hygiene_out"
expect_ok "independent noise audit targets the exact traced live sentinel PID" \
    bash -c '
        test "$(grep -c "^TEST_SENTINEL_START [1-9][0-9]*$" "$1")" -eq 1 || exit 1
        test "$(grep -c "^TEST_SENTINEL_STOP [1-9][0-9]*$" "$1")" -eq 1 || exit 1
        start_pid=$(awk "\$1 == \"TEST_SENTINEL_START\" { print \$2 }" "$1") || exit 1
        stop_pid=$(awk "\$1 == \"TEST_SENTINEL_STOP\" { print \$2 }" "$1") || exit 1
        record=$(<"$2") || exit 1
        [[ $record =~ observer_pid=([1-9][0-9]*)\ target_pid=([1-9][0-9]*) ]] || exit 1
        observer_pid=${BASH_REMATCH[1]}
        target_pid=${BASH_REMATCH[2]}
        test "$start_pid" = "$stop_pid" && test "$start_pid" = "$target_pid" &&
            test "$observer_pid" != "$target_pid"
    ' bash "$hygiene_out" "$hygiene_audit/noise-sentinel"
for hygiene_helper in qdsplit keeper noise-sentinel mount-root-holder; do
    expect_ok "$hygiene_helper child inherits none of the four retained device fds" \
        bash -c 'record=$(<"$2") && [[ $record =~ ^CHILD_FD_HYGIENE\ helper=$1\ observer_pid=([1-9][0-9]*)\ target_pid=([1-9][0-9]*)\ retained_open=0\ status=verified$ ]] && test "${BASH_REMATCH[1]}" != "${BASH_REMATCH[2]}"' \
            bash "$hygiene_helper" "$hygiene_audit/$hygiene_helper"
done

expect_ok "run retains a safe mount directory instead of mkdir -p" \
    bash -c '
        main=$(sed -n '\''/^\[ -x "\$QDSPLIT"/,/^prepare_run_dir$/p'\'' "$1")
        verified=$(grep -n "^verify_retained_devices setup" <<<"$main" | cut -d: -f1)
        prepared=$(grep -n "^prepare_mount_dir$" <<<"$main" | cut -d: -f1)
        mounted=$(grep -n "^mount_test_partition$" <<<"$main" | cut -d: -f1)
        test -n "$verified" -a -n "$prepared" -a -n "$mounted" &&
            test "$verified" -lt "$prepared" && test "$prepared" -lt "$mounted" &&
            ! grep -q '\''mkdir -p "\$MNT"'\'' <<<"$main"
    ' bash tools/campaign-cstate-keeper.sh
expect_ok "work-directory creation is bracketed by exact mount checks" \
    bash -c '
        main=$(sed -n '\''/^mount_test_partition$/,/^prepare_output$/p'\'' "$1")
        pre=$(grep -n "verify_retained_mount workdir-pre" <<<"$main" | cut -d: -f1)
        make=$(grep -n "^prepare_run_dir$" <<<"$main" | cut -d: -f1)
        post=$(grep -n "verify_retained_mount workdir-post" <<<"$main" | cut -d: -f1)
        out=$(grep -n "^prepare_output$" <<<"$main" | cut -d: -f1)
        test -n "$pre" -a -n "$make" -a -n "$post" -a -n "$out" &&
            test "$pre" -lt "$make" && test "$make" -lt "$post" && test "$post" -lt "$out"
    ' bash tools/campaign-cstate-keeper.sh

# Pure mount state machine.  findmnt accepts exactly the one four-field kernel
# snapshot used by the launcher.  A pathname umount is an unconditional
# tripwire; the mount-fd helper is the only modeled unmount authority.
mount_bin="$tmp/mount-bin"
mount_target="$tmp/mount-target"
mount_part="$tmp/mount-part"
mount_state="$tmp/mount-state"
mount_id_file="$tmp/mount-id"
mount_majmin_file="$tmp/mount-majmin"
mount_source_file="$tmp/mount-source"
mount_fd_id_file="$tmp/mount-fd-id"
snapshot_log="$tmp/findmnt.log"
mount_log="$tmp/mount.log"
mount_fd_log="$tmp/mount-fd.log"
unsafe_umount_log="$tmp/unsafe-umount.log"
mkdir "$mount_bin" "$mount_target"
: >"$mount_part"; : >"$snapshot_log"; : >"$mount_log"
: >"$mount_fd_log"; : >"$unsafe_umount_log"
printf '%s\n' unmounted >"$mount_state"
printf '%s\n' 77 >"$mount_id_file"
printf '%s\n' 259:1 >"$mount_majmin_file"
printf '%s\n' "$mount_part" >"$mount_source_file"
printf 'mnt_id:\t%s\n' 77 >"$mount_fd_id_file"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" "$*" >>"$FAKE_SNAPSHOT_LOG"' \
    'test "$#" -eq 7 && test "$1" = --kernel && test "$2" = --mountpoint && test "$3" = "$FAKE_MOUNT_TARGET" && test "$4" = --noheadings && test "$5" = --pairs && test "$6" = -o && test "$7" = ID,MAJ:MIN,SOURCE,TARGET || exit 91' \
    'test "$(cat "$FAKE_MOUNT_STATE")" != unmounted || exit 1' \
    'printf '\''ID="%s" MAJ:MIN="%s" SOURCE="%s" TARGET="%s"\n'\'' "$(cat "$FAKE_MOUNT_ID_FILE")" "$(cat "$FAKE_MOUNT_MAJMIN_FILE")" "$(cat "$FAKE_MOUNT_SOURCE_FILE")" "$FAKE_MOUNT_TARGET"' \
    >"$mount_bin/findmnt"
printf '%s\n' '#!/bin/bash' \
    'test "$#" -eq 4 && test "$1" = --source && test "$3" = --target && test "$4" = "$FAKE_MOUNT_TARGET" || exit 92' \
    '[[ $2 == /proc/$PPID/fd/[0-9]* ]] && test -f "$2" || exit 93' \
    'printf "%s\n" "$2" >>"$FAKE_MOUNT_LOG"' \
    'printf "%s\n" mounted >"$FAKE_MOUNT_STATE"' \
    >"$mount_bin/mount"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" "$*" >>"$FAKE_UNSAFE_UMOUNT_LOG"' \
    'exit 94' \
    >"$mount_bin/umount"
printf '%s\n' '#!/bin/bash' \
    'test "$#" -eq 1 && test "$1" = "$FAKE_MOUNT_TARGET" || exit 95' \
    'exec {held_fd}<"$1" || exit 96' \
    'fd_mnt_id=$(awk '\''$1 == "mnt_id:" { print $2 }'\'' "$FAKE_MOUNT_FD_ID_FILE")' \
    '[[ $fd_mnt_id =~ ^[1-9][0-9]*$ ]] || exit 96' \
    'printf "READY\t%s\t%s\t%s\n" "$BASHPID" "$held_fd" "$fd_mnt_id"' \
    'IFS=$'\''\t'\'' read -r command expected || exit 97' \
    'case "$command" in' \
    '  CLOSE)' \
    '    printf "CLOSE\t/proc/%s/fd/%s\n" "$BASHPID" "$held_fd" >>"$FAKE_MOUNT_FD_LOG"' \
    '    printf "CLOSED\n"' \
    '    ;;' \
    '  UMOUNT)' \
    '    test "$expected" = "$fd_mnt_id" || { printf "ERROR\n"; exit 98; }' \
    '    printf "UMOUNT\t/proc/%s/fd/%s\t%s\n" "$BASHPID" "$held_fd" "$expected" >>"$FAKE_MOUNT_FD_LOG"' \
    '    test "$(cat "$FAKE_MOUNT_STATE")" = replacement || printf "%s\n" unmounted >"$FAKE_MOUNT_STATE"' \
    '    printf "UNMOUNTED\n"' \
    '    ;;' \
    '  *) printf "ERROR\n"; exit 99 ;;' \
    'esac' >"$mount_bin/mount-fd-helper"
chmod 0700 "$mount_bin/findmnt" "$mount_bin/mount" "$mount_bin/umount" \
    "$mount_bin/mount-fd-helper"

mount_test_env=(
    CPU=0 REPS=1 ITERS=1 SETTLE=0
    EXITOS_CAMPAIGN_TEST_MODE=1 EXITOS_TESTPART="$mount_part"
    EXITOS_TEST_PART_MAJMIN=259:1 EXITOS_TESTMNT="$mount_target"
    PATH="$mount_bin:$PATH" FAKE_MOUNT_TARGET="$mount_target"
    FAKE_MOUNT_STATE="$mount_state" FAKE_MOUNT_ID_FILE="$mount_id_file"
    FAKE_MOUNT_MAJMIN_FILE="$mount_majmin_file"
    FAKE_MOUNT_SOURCE_FILE="$mount_source_file"
    FAKE_MOUNT_FD_ID_FILE="$mount_fd_id_file"
    FAKE_SNAPSHOT_LOG="$snapshot_log" FAKE_MOUNT_LOG="$mount_log"
    FAKE_MOUNT_FD_LOG="$mount_fd_log"
    FAKE_UNSAFE_UMOUNT_LOG="$unsafe_umount_log"
    EXITOS_TEST_MOUNT_FD_HELPER="$mount_bin/mount-fd-helper"
    EXITOS_TEST_MOUNT_FD_ID_FILE="$mount_fd_id_file"
)

mount_out=$(env "${mount_test_env[@]}" \
    bash tools/campaign-cstate-keeper.sh --test-owned-mount-acquire \
    2>"$tmp/mount-acquire.err")
mount_acquire_rc=$?
expect_ok "owned mount cleanup is namespace-bound and never pathname-unmounts" \
    sh -c 'test "$1" -eq 0 && test "$2" = "MOUNT_ACQUIRE_RESULT status=ok mount=private-namespace" && test "$(cat "$3")" = mounted && test "$(wc -l <"$4")" -eq 1 && grep -Eq "^CLOSE[[:space:]]+/proc/[0-9]+/fd/[0-9]+$" "$5" && test ! -s "$6"' \
        sh "$mount_acquire_rc" "$mount_out" "$mount_state" "$mount_log" \
        "$mount_fd_log" "$unsafe_umount_log"
expect_ok "all mount identity reads use one complete kernel snapshot" \
    sh -c 'test -s "$1" && ! grep -Fvx -- "--kernel --mountpoint $2 --noheadings --pairs -o ID,MAJ:MIN,SOURCE,TARGET" "$1"' \
        sh "$snapshot_log" "$mount_target"

printf '%s\n' unmounted >"$mount_state"; printf '%s\n' 259:9 >"$mount_majmin_file"
: >"$mount_log"; : >"$mount_fd_log"; : >"$unsafe_umount_log"; : >"$snapshot_log"
env "${mount_test_env[@]}" \
    bash tools/campaign-cstate-keeper.sh --test-owned-mount-acquire \
    >"$tmp/mount-wrongdev.out" 2>"$tmp/mount-wrongdev.err"
mount_wrongdev_rc=$?
expect_ok "wrong mount source dev_t fails closed without umount" \
    sh -c 'test "$1" -ne 0 && test "$(cat "$2")" = mounted && test ! -s "$3" && test ! -s "$4" && grep -q "mount source dev_t mismatch" "$5"' \
        sh "$mount_wrongdev_rc" "$mount_state" "$mount_fd_log" \
        "$unsafe_umount_log" "$tmp/mount-wrongdev.err"
printf '%s\n' 259:1 >"$mount_majmin_file"

printf '%s\n' replacement >"$mount_state"; printf '%s\n' 78 >"$mount_id_file"
: >"$mount_fd_log"; : >"$unsafe_umount_log"; : >"$snapshot_log"
env "${mount_test_env[@]}" EXITOS_TEST_MOUNT_ID=77 \
    EXITOS_TEST_MOUNT_SOURCE="$mount_part" \
    bash tools/campaign-cstate-keeper.sh --test-owned-mount-cleanup \
    >"$tmp/mount-id-drift.out" 2>"$tmp/mount-id-drift.err"
mount_id_drift_rc=$?
expect_ok "private-namespace cleanup cannot unmount a replacement at MNT" \
    sh -c 'test "$1" -eq 0 && test "$(cat "$2")" = replacement && grep -Eq "^CLOSE[[:space:]]+/proc/[0-9]+/fd/[0-9]+$" "$3" && test ! -s "$4"' \
        sh "$mount_id_drift_rc" "$mount_state" "$mount_fd_log" "$unsafe_umount_log"

printf '%s\n' mounted >"$mount_state"; printf '%s\n' 88 >"$mount_id_file"
printf 'mnt_id:\t%s\n' 88 >"$mount_fd_id_file"
: >"$mount_log"; : >"$mount_fd_log"; : >"$unsafe_umount_log"; : >"$snapshot_log"
existing_mount_out=$(env "${mount_test_env[@]}" \
    bash tools/campaign-cstate-keeper.sh --test-owned-mount-acquire \
    2>"$tmp/existing-mount.err")
existing_mount_rc=$?
expect_ok "exact existing mount is accepted and never campaign-unmounted" \
    sh -c 'test "$1" -eq 0 && test "$2" = "MOUNT_ACQUIRE_RESULT status=ok existing=1 mount=preserved" && test "$(cat "$3")" = mounted && test ! -s "$4" && test ! -s "$5" && grep -Eq "^CLOSE[[:space:]]+/proc/[0-9]+/fd/[0-9]+$" "$6"' \
        sh "$existing_mount_rc" "$existing_mount_out" "$mount_state" "$mount_log" \
        "$unsafe_umount_log" "$mount_fd_log"

# Full fake campaign: execute the real setup, schedule, child lifecycle,
# pre/post identity gates, result commit, and cleanup.  All device nodes are
# regular files and all privileged commands are strict fakes.
full_qdsplit_log="$tmp/full-qdsplit.log"
full_settle_drift="$tmp/full-settle-drift"
full_results="$tmp/full-results"
mkdir "$full_results"
cpu_util_sequence="$tmp/cpu-util-sequence"
mkdir "$cpu_util_sequence"

write_cpu_util_sequence() {
    local selected_busy=$1 sibling_busy=$2 step
    local selected_idle=$((100 - selected_busy))
    local sibling_idle=$((100 - sibling_busy))
    for step in 0 1 2; do
        printf '%s\n' \
            'cpu 0 0 0 1000 0 0 0 0 0 0' \
            "cpu0 $((sibling_busy * step)) 0 0 $((1000 + sibling_idle * step)) 0 0 0 0 0 0" \
            "cpu4 $((selected_busy * step)) 0 0 $((1000 + selected_idle * step)) 0 0 0 0 0 0" \
            'procs_running 1' 'procs_blocked 0' \
            >"$cpu_util_sequence/stat.$step"
    done
}
printf '%s\n' '#!/bin/bash' \
    'test "$#" -ge 3 && test "$1" = -c || exit 90' \
    'requested_cpu=$2' \
    'shift 2' \
    'if [ "${FAKE_APP_AFFINITY_MUTATION:-none}" = wrong ]; then requested_cpu=5; fi' \
    'case "${FAKE_APP_POLICY_MUTATION:-none}" in' \
    '  none) export FAKE_APP_POLICY_VALUE=0 ;;' \
    '  idle) export FAKE_APP_POLICY_VALUE=5 ;;' \
    '  *) exit 91 ;;' \
    'esac' \
    'export FAKE_TASKSET_CPU=$requested_cpu' \
    'exec "$@"' >"$mount_bin/taskset"
printf '%s\n' '#!/bin/bash' \
    'test "$#" -ge 3 && test "$1" = -n && test "$2" = -20 || exit 92' \
    'case "${FAKE_NICE_MUTATION:-none}" in' \
    '  none) export FAKE_NICE_VALUE=-20 ;;' \
    '  ignore) export FAKE_NICE_VALUE=0 ;;' \
    '  *) exit 93 ;;' \
    'esac' \
    'shift 2' \
    'exec "$@"' >"$mount_bin/nice"
printf '%s\n' '#!/bin/bash' 'exec /bin/cat' >"$mount_bin/shuf"
printf '%s\n' '#!/bin/bash' \
    'if [ "${FAKE_SETTLE_MOUNT_ID_DRIFT:-0}" = 1 ] && [ ! -e "$FAKE_SETTLE_DRIFT_MARKER" ]; then' \
    '  : >"$FAKE_SETTLE_DRIFT_MARKER"' \
    '  printf "%s\n" 78 >"$FAKE_MOUNT_ID_FILE"' \
    '  exit 0' \
    'fi' \
    'exec /bin/sleep "$@"' >"$mount_bin/sleep"
printf '%s\n' '#!/bin/bash' \
    'app_proc_dir="$FAKE_APP_PROC_ROOT/$$"' \
    'mkdir "$app_proc_dir" || exit 84' \
    'trap '\''rm -rf -- "$app_proc_dir"'\'' EXIT' \
    'printf "Cpus_allowed_list:\t%s\n" "${FAKE_TASKSET_CPU:-unbound}" >"$app_proc_dir/status"' \
    'app_stat_fields=(S "$PPID")' \
    'while [ "${#app_stat_fields[@]}" -lt 39 ]; do app_stat_fields+=(0); done' \
    'app_stat_fields[16]=${FAKE_NICE_VALUE:-0}' \
    'app_stat_fields[38]=${FAKE_APP_POLICY_VALUE:-0}' \
    'printf "%s (fake-qdsplit) %s\n" "$$" "${app_stat_fields[*]}" >"$app_proc_dir/stat"' \
    'printf "%s\t%s\t%s\t%s\t%s\n" "${EXITOS_PAIRED-}" "${EXITOS_ONLY+x}" "${EXITOS_REQUIRE_SCATTERED:-0}" "${EXITOS_REQUIRE_CONTIG:-0}" "${EXITOS_HUGEBUF:-0}" >>"$FAKE_QDSPLIT_LOG"' \
    'printf "READY\n"' \
    'noise_cleanup_pid=' \
    'case "${FAKE_NOISE_MUTATION:-none}" in' \
    '  none) ;;' \
    '  build)' \
    '    mkdir "$FAKE_NOISE_PROC_ROOT/4242" || exit 88' \
    '    printf "%s\n" make >"$FAKE_NOISE_PROC_ROOT/4242/comm"' \
    '    ( /bin/sleep 0.12; rm -f -- "$FAKE_NOISE_PROC_ROOT/4242/comm"; rmdir -- "$FAKE_NOISE_PROC_ROOT/4242" ) &' \
    '    noise_cleanup_pid=$!' \
    '    ;;' \
    '  blocked)' \
    '    printf "%s\n" "cpu 1 2 3 4" "procs_running 1" "procs_blocked 1" >"$FAKE_NOISE_PROC_ROOT/stat"' \
    '    ( /bin/sleep 0.12; printf "%s\n" "cpu 1 2 3 4" "procs_running 1" "procs_blocked 0" >"$FAKE_NOISE_PROC_ROOT/stat" ) &' \
    '    noise_cleanup_pid=$!' \
    '    ;;' \
    '  *) exit 89 ;;' \
    'esac' \
    '/bin/sleep "${FAKE_QDSPLIT_HOLD:-0.2}"' \
    'if [ -n "$noise_cleanup_pid" ]; then wait "$noise_cleanup_pid" || exit 87; fi' \
    'if [ "${EXITOS_REQUIRE_SCATTERED:-0}" = 1 ]; then' \
    '  printf "buffer contract: scattered proven runs=2\n"' \
    '  printf "buffer pages: 100 102 ... submitted 8192-byte command span has 2 physical runs\n"' \
    'else' \
    '  printf "buffer contract: contiguous proven runs=1\n"' \
    '  printf "buffer pages: 100 101 ... submitted 8192-byte command span has 1 physical run\n"' \
    'fi' \
    'printf "fs-8k-qd1 1 10.000 9.000 11.000\n"' \
    'printf "pt-8k-qd1 1 20.000 19.000 21.000\n"' \
    'if [ "${EXITOS_PAIRED-}" = opt ]; then' \
    '  case "${FAKE_OPT_OUTPUT_MUTATION:-none}" in' \
    '    missing) ;;' \
    '    failed) printf "pt-8k-fixedbuf FAILED\n" ;;' \
    '    malformed) printf "pt-8k-fixedbuf 1 not-a-number 13.000 15.000\n" ;;' \
    '    *) printf "pt-8k-fixedbuf 1 14.250 13.000 15.000\n" ;;' \
    '  esac' \
    '  printf "pt-8k-bounce 1 17.500 16.000 18.000\n"' \
    '  printf "pt-8k-bounce-fixed 1 15.000 14.000 16.000\n"' \
    '  if [ "${FAKE_OPT_OUTPUT_MUTATION:-none}" = duplicate ]; then printf "pt-8k-fixedbuf 1 14.250 13.000 15.000\n"; fi' \
    'fi' \
    'if [ "${FAKE_OPT_OUTPUT_MUTATION:-none}" != no-post ]; then' \
    '  printf "POST_RUN_VERIFY file=exact raw=exact selected=5 guards=zero\n"' \
    'fi' \
    'case "${FAKE_IRQ_MUTATION:-increment}" in' \
    '  increment|affinity|diskseq)' \
    '    awk -v target="${FAKE_TARGET_IRQ}:" -v selected="$FAKE_SELECTED_FIELD" -v sibling="$FAKE_SIBLING_FIELD" '\''$1 == target {$selected += 1} $1 == "150:" {$selected += 2; $sibling += 3} $1 == "LOC:" {$selected += 5; $sibling += 7} $1 == "RES:" {$selected += 11; $sibling += 13} $1 == "CAL:" {$selected += 17; $sibling += 19} $1 == "TLB:" {$selected += 23; $sibling += 29} {print}'\'' "$FAKE_INTERRUPTS_FILE" >"$FAKE_INTERRUPTS_FILE.next" || exit 93' \
    '    mv -- "$FAKE_INTERRUPTS_FILE.next" "$FAKE_INTERRUPTS_FILE" || exit 94' \
    '    ;;' \
    '  zero) ;;' \
    '  wrong-cpu)' \
    '    awk -v target="${FAKE_TARGET_IRQ}:" -v sibling="$FAKE_SIBLING_FIELD" '\''$1 == target {$sibling += 1} {print}'\'' "$FAKE_INTERRUPTS_FILE" >"$FAKE_INTERRUPTS_FILE.next" || exit 98' \
    '    mv -- "$FAKE_INTERRUPTS_FILE.next" "$FAKE_INTERRUPTS_FILE" || exit 99' \
    '    ;;' \
    '  irq-number)' \
    '    awk -v target="${FAKE_TARGET_IRQ}:" -v alternate="${FAKE_ALTERNATE_IRQ}:" -v selected="$FAKE_SELECTED_FIELD" -v action="$FAKE_TARGET_ACTION" -v stale="$FAKE_STALE_ACTION" '\''$1 == target {$NF = stale} $1 == alternate {$selected += 1; $NF = action} {print}'\'' "$FAKE_INTERRUPTS_FILE" >"$FAKE_INTERRUPTS_FILE.next" || exit 95' \
    '    mv -- "$FAKE_INTERRUPTS_FILE.next" "$FAKE_INTERRUPTS_FILE" || exit 96' \
    '    ;;' \
    '  *) exit 97 ;;' \
    'esac' \
    'if [ "${FAKE_IRQ_MUTATION:-increment}" = affinity ]; then printf "%s\n" 1 >"$FAKE_TARGET_EFFECTIVE_AFFINITY"; fi' \
    'if [ "${FAKE_IRQ_MUTATION:-increment}" = diskseq ]; then printf "%s\n" 73 >"$FAKE_DISKSEQ_FILE"; fi' \
    'if [ "${FAKE_CPUFREQ_MUTATION:-none}" = governor ]; then printf "%s\n" powersave >"$FAKE_CPUFREQ_GOVERNOR_FILE"; fi' \
    'if [ "${FAKE_QDSPLIT_MOUNT_ID_DRIFT:-0}" = 1 ]; then' \
    '  printf "%s\n" 78 >"$FAKE_MOUNT_ID_FILE"' \
    'fi' \
    >"$mount_bin/qdsplit"
printf '%s\n' '#!/bin/bash' \
    'test "$#" -eq 1 || exit 90' \
    'printf "READY cpu=%s policy=SCHED_IDLE\n" "$1"' \
    'trap '\''exit 0'\'' TERM INT' \
    'while :; do /bin/sleep 0.02; done' \
    >"$mount_bin/keeper"
replace_mount_hook="$mount_bin/replace-mount"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" replacement >"$FAKE_MOUNT_STATE"' \
    'printf "%s\n" 78 >"$FAKE_MOUNT_ID_FILE"' \
    >"$replace_mount_hook"
chmod 0700 "$mount_bin/taskset" "$mount_bin/nice" "$mount_bin/shuf" "$mount_bin/sleep" "$mount_bin/qdsplit" \
    "$mount_bin/keeper" "$replace_mount_hook"

full_run_env=(
    CPU=4 REPS=1 ITERS=1 SETTLE=0
    EXITOS_CAMPAIGN_TEST_MODE=1
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS=0-2,4
    EXITOS_QDSPLIT_BIN="$mount_bin/qdsplit"
    EXITOS_KEEPER_BIN="$mount_bin/keeper"
    EXITOS_DEV="$identity_whole" EXITOS_TESTPART="$identity_part"
    EXITOS_CHARDEV="$identity_char" EXITOS_TEST_CTRLDEV="$identity_ctrl_node"
    EXITOS_EXPECT_SERIAL=SERIAL-1
    EXITOS_DEVICE_SYSFS_ROOT="$identity_sys"
    EXITOS_IRQ_PROC_ROOT="$identity_irq_proc"
    EXITOS_TEST_APP_PROC_ROOT="$identity_irq_proc"
    EXITOS_TEST_CPU_UTIL_SEQUENCE_DIR="$cpu_util_sequence"
    EXITOS_TEST_CPU_UTIL_WINDOWS=2
    EXITOS_TEST_WHOLE_MAJMIN=259:0 EXITOS_TEST_PART_MAJMIN=259:1
    EXITOS_TEST_CHAR_MAJMIN=240:0 EXITOS_TEST_CTRL_MAJMIN=241:0
    EXITOS_TEST_CTRL_SYSFS_PATH="$identity_ctrl"
    EXITOS_TEST_NVME_BIN="$identity_nvme"
    IDENTITY_NVME_LOG="$identity_nvme_log" IDENTITY_CHAR_PATH="$identity_char"
    EXITOS_TESTMNT="$mount_target" PATH="$mount_bin:$PATH"
    FAKE_MOUNT_TARGET="$mount_target" FAKE_MOUNT_STATE="$mount_state"
    FAKE_MOUNT_ID_FILE="$mount_id_file"
    FAKE_MOUNT_MAJMIN_FILE="$mount_majmin_file"
    FAKE_MOUNT_SOURCE_FILE="$mount_source_file"
    FAKE_MOUNT_FD_ID_FILE="$mount_fd_id_file"
    FAKE_SNAPSHOT_LOG="$snapshot_log" FAKE_MOUNT_LOG="$mount_log"
    FAKE_MOUNT_FD_LOG="$mount_fd_log"
    FAKE_UNSAFE_UMOUNT_LOG="$unsafe_umount_log"
    EXITOS_TEST_MOUNT_FD_HELPER="$mount_bin/mount-fd-helper"
    EXITOS_TEST_MOUNT_FD_ID_FILE="$mount_fd_id_file"
    EXITOS_TEST_REPLACE_MOUNT_HOOK="$replace_mount_hook"
    FAKE_QDSPLIT_LOG="$full_qdsplit_log"
    FAKE_NOISE_PROC_ROOT="$identity_irq_proc"
    FAKE_APP_PROC_ROOT="$identity_irq_proc"
    FAKE_INTERRUPTS_FILE="$identity_interrupts"
    FAKE_TARGET_IRQ=141 FAKE_ALTERNATE_IRQ=142
    FAKE_TARGET_ACTION=nvme0q8 FAKE_STALE_ACTION=nvme0q9
    FAKE_SELECTED_FIELD=6 FAKE_SIBLING_FIELD=2
    FAKE_TARGET_EFFECTIVE_AFFINITY="$identity_irq_proc/irq/141/effective_affinity_list"
    FAKE_DISKSEQ_FILE="$identity_ns/diskseq"
    FAKE_CPUFREQ_GOVERNOR_FILE="$policy4/scaling_governor"
    FAKE_SETTLE_DRIFT_MARKER="$full_settle_drift"
    TMPDIR="$full_results"
)

reset_full_run() {
    write_cpu_util_sequence 1 1
    printf '%s\n' unmounted >"$mount_state"
    printf '%s\n' 77 >"$mount_id_file"
    printf '%s\n' 259:1 >"$mount_majmin_file"
    printf '%s\n' "$identity_part" >"$mount_source_file"
    printf 'mnt_id:\t%s\n' 77 >"$mount_fd_id_file"
    : >"$snapshot_log"; : >"$mount_log"; : >"$mount_fd_log"
    : >"$unsafe_umount_log"; : >"$full_qdsplit_log"
    : >"$identity_nvme_log"
    printf '%s\n' '72' >"$identity_ns/diskseq"
    rm -rf -- "$identity_ns/mq/10" "$identity_ns/mq/11"
    printf '%s\n' '4' >"$identity_ns/mq/7/cpu_list"
    for fake_irq in 141 142 177 178; do
        printf '%s\n' 4 >"$identity_irq_proc/irq/$fake_irq/smp_affinity_list"
        printf '%s\n' 4 >"$identity_irq_proc/irq/$fake_irq/effective_affinity_list"
    done
    printf '%s\n' performance >"$policy0/scaling_governor"
    printf '%s\n' performance >"$policy4/scaling_governor"
    printf '%s\n' performance >"$policy0/energy_performance_preference"
    printf '%s\n' performance >"$policy4/energy_performance_preference"
    printf '%s\n' 3800000 >"$policy0/scaling_min_freq"
    printf '%s\n' 3800000 >"$policy0/scaling_max_freq"
    printf '%s\n' 3800000 >"$policy4/scaling_min_freq"
    printf '%s\n' 3800000 >"$policy4/scaling_max_freq"
    printf '%s\n' \
        '           CPU0       CPU1       CPU2       CPU3       CPU4' \
        '141:         57          0          0          0        100 IR-PCI-MSIX nvme0q8' \
        '142:          0          0          0          0          0 IR-PCI-MSIX nvme0q9' \
        '150:          9          0          0          0          7 local-other' \
        'LOC:         10          0          0          0         20 Local timer interrupts' \
        'RES:          3          0          0          0          4 Rescheduling interrupts' \
        'CAL:          5          0          0          0          6 Function call interrupts' \
        'TLB:          7          0          0          0          8 TLB shootdowns' \
        'ERR:        999' \
        >"$identity_interrupts"
    printf '%s\n' 'cpu 1 2 3 4' 'procs_running 1' 'procs_blocked 0' \
        >"$identity_proc_stat"
    rm -f -- "$identity_irq_proc/4242/comm"
    rmdir -- "$identity_irq_proc/4242" 2>/dev/null || true
    rm -f -- "$full_settle_drift"
}

reset_full_run
write_cpu_util_sequence 20 1
selected_busy_out="$full_results/selected-busy.tsv"
env "${full_run_env[@]}" OUT="$selected_busy_out" \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/selected-busy.out" 2>"$tmp/selected-busy.err"
selected_busy_rc=$?
expect_ok "selected CPU above 15 percent is recorded without aborting the campaign" \
    sh -c 'test "$1" -eq 0 && test "$(wc -l <"$2")" -eq 4 && test "$(wc -l <"$3")" -eq 6 && grep -q "threshold exceeded (record-only)" "$4" && awk -F "\t" '\''NR > 2 && $19 != "selected_cpu=4@selected_max_bp=2000@sibling_cpu=0@sibling_max_bp=100@windows=2@threshold_lt_pct=15@threshold_exceeded=yes" { exit 1 } END { if (NR != 6) exit 1 }'\'' "$3"' \
        sh "$selected_busy_rc" "$full_qdsplit_log" "$selected_busy_out" \
        "$tmp/selected-busy.err"

reset_full_run
write_cpu_util_sequence 1 20
sibling_busy_out="$full_results/sibling-busy.tsv"
env "${full_run_env[@]}" OUT="$sibling_busy_out" \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/sibling-busy.out" 2>"$tmp/sibling-busy.err"
sibling_busy_rc=$?
expect_ok "SMT sibling above 15 percent is recorded without aborting the campaign" \
    sh -c 'test "$1" -eq 0 && test "$(wc -l <"$2")" -eq 4 && test "$(wc -l <"$3")" -eq 6 && grep -q "threshold exceeded (record-only)" "$4" && awk -F "\t" '\''NR > 2 && $19 != "selected_cpu=4@selected_max_bp=100@sibling_cpu=0@sibling_max_bp=2000@windows=2@threshold_lt_pct=15@threshold_exceeded=yes" { exit 1 } END { if (NR != 6) exit 1 }'\'' "$3"' \
        sh "$sibling_busy_rc" "$full_qdsplit_log" "$sibling_busy_out" \
        "$tmp/sibling-busy.err"

reset_full_run
printf '%s\n' powersave >"$policy0/scaling_governor"
naked_powersave_out="$full_results/naked-powersave.tsv"
env "${full_run_env[@]}" OUT="$naked_powersave_out" \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/naked-powersave.out" 2>"$tmp/naked-powersave.err"
naked_powersave_rc=$?
expect_ok "naked powersave run fails before mount, OUT, or qdsplit side effects" \
    sh -c 'test "$1" -ne 0 && test ! -s "$2" && test ! -e "$3" && test ! -s "$4" && test ! -s "$5" && grep -q "required fixed-max-3800000 cpufreq profile" "$6"' \
        sh "$naked_powersave_rc" "$mount_log" "$naked_powersave_out" \
        "$full_qdsplit_log" "$identity_nvme_log" "$tmp/naked-powersave.err"

reset_full_run
missing_full_part="$tmp/identity-dev/missing-p1"
missing_full_mnt="$tmp/missing-full-mnt"
missing_full_out="$full_results/missing-p1.tsv"
env "${full_run_env[@]}" EXITOS_TESTPART="$missing_full_part" \
    EXITOS_TESTMNT="$missing_full_mnt" FAKE_MOUNT_TARGET="$missing_full_mnt" \
    OUT="$missing_full_out" \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/missing-full.out" 2>"$tmp/missing-full.err"
missing_full_rc=$?
expect_ok "dynamic missing p1 fails before mount, mkdir, OUT, or qdsplit" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && test ! -e "$3" && test ! -s "$4" && test ! -s "$5"' \
        sh "$missing_full_rc" "$missing_full_mnt" "$missing_full_out" \
        "$mount_log" "$full_qdsplit_log"

reset_full_run
pre_device_out="$full_results/pre-device.tsv"
env "${full_run_env[@]}" OUT="$pre_device_out" \
    EXITOS_TEST_FAILPOINT=device:cell-1-normal-off-pre \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/pre-device.out" 2>"$tmp/pre-device.err"
pre_device_rc=$?
expect_ok "dynamic pre-device failure launches no qdsplit and commits no result" \
    sh -c 'test "$1" -ne 0 && test ! -s "$2" && test "$(wc -l <"$3")" -eq 2 && ! grep -q "^results:" "$4"' \
        sh "$pre_device_rc" "$full_qdsplit_log" "$pre_device_out" "$tmp/pre-device.out"

reset_full_run
pre_mount_out="$full_results/pre-mount.tsv"
env "${full_run_env[@]}" OUT="$pre_mount_out" \
    EXITOS_TEST_FAILPOINT=mount:cell-1-normal-off-pre \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/pre-mount.out" 2>"$tmp/pre-mount.err"
pre_mount_rc=$?
expect_ok "dynamic pre-mount failure launches no qdsplit and commits no result" \
    sh -c 'test "$1" -ne 0 && test ! -s "$2" && test "$(wc -l <"$3")" -eq 2 && ! grep -q "^results:" "$4"' \
        sh "$pre_mount_rc" "$full_qdsplit_log" "$pre_mount_out" "$tmp/pre-mount.out"

reset_full_run
settle_drift_out="$full_results/settle-drift.tsv"
env "${full_run_env[@]}" OUT="$settle_drift_out" SETTLE=1 \
    FAKE_SETTLE_MOUNT_ID_DRIFT=1 \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/settle-drift.out" 2>"$tmp/settle-drift.err"
settle_drift_rc=$?
expect_ok "settle-window mount drift is caught by the immediate pre-launch gate" \
    sh -c 'test "$1" -ne 0 && test ! -s "$2" && test "$(wc -l <"$3")" -eq 2 && grep -q "mount ID identity changed" "$4"' \
        sh "$settle_drift_rc" "$full_qdsplit_log" "$settle_drift_out" \
        "$tmp/settle-drift.err"

reset_full_run
post_device_out="$full_results/post-device.tsv"
env "${full_run_env[@]}" OUT="$post_device_out" \
    EXITOS_TEST_FAILPOINT=device:cell-1-normal-off-post \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/post-device.out" 2>"$tmp/post-device.err"
post_device_rc=$?
expect_ok "dynamic post-device failure commits no measured row" \
    sh -c 'test "$1" -ne 0 && test "$(wc -l <"$2")" -eq 1 && test "$(wc -l <"$3")" -eq 2 && ! grep -q "^results:" "$4"' \
        sh "$post_device_rc" "$full_qdsplit_log" "$post_device_out" \
        "$tmp/post-device.out"

reset_full_run
post_mount_out="$full_results/post-mount.tsv"
env "${full_run_env[@]}" OUT="$post_mount_out" \
    FAKE_QDSPLIT_MOUNT_ID_DRIFT=1 \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/post-mount.out" 2>"$tmp/post-mount.err"
post_mount_rc=$?
expect_ok "dynamic post mount-ID drift commits no measured row" \
    sh -c 'test "$1" -ne 0 && test "$(wc -l <"$2")" -eq 1 && test "$(wc -l <"$3")" -eq 2 && ! grep -q "^results:" "$4" && grep -q "mount ID identity changed" "$5"' \
        sh "$post_mount_rc" "$full_qdsplit_log" "$post_mount_out" \
        "$tmp/post-mount.out" "$tmp/post-mount.err"

reset_full_run
cpufreq_drift_out="$full_results/cpufreq-drift.tsv"
env "${full_run_env[@]}" OUT="$cpufreq_drift_out" \
    FAKE_CPUFREQ_MUTATION=governor \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/cpufreq-drift.out" 2>"$tmp/cpufreq-drift.err"
cpufreq_drift_rc=$?
expect_ok "post-cell cpufreq profile drift commits no measured row" \
    sh -c 'test "$1" -ne 0 && test "$(wc -l <"$2")" -eq 1 && test "$(wc -l <"$3")" -eq 2 && grep -q "cpufreq profile" "$4"' \
        sh "$cpufreq_drift_rc" "$full_qdsplit_log" "$cpufreq_drift_out" \
        "$tmp/cpufreq-drift.err"

reset_full_run
replacement_out="$full_results/replacement.tsv"
env "${full_run_env[@]}" OUT="$replacement_out" \
    EXITOS_TEST_FAILPOINT=cleanup:replacement \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/replacement.out" 2>"$tmp/replacement.err"
replacement_rc=$?
expect_ok "dynamic cleanup replacement is preserved without pathname umount" \
    sh -c 'test "$1" -eq 0 && test "$(cat "$2")" = replacement && test "$(wc -l <"$3")" -eq 4 && test "$(wc -l <"$4")" -eq 6 && test ! -s "$5" && grep -Eq "^CLOSE[[:space:]]+/proc/[0-9]+/fd/[0-9]+$" "$6" && grep -q "^results:" "$7"' \
        sh "$replacement_rc" "$mount_state" "$full_qdsplit_log" \
        "$replacement_out" "$unsafe_umount_log" "$mount_fd_log" "$tmp/replacement.out"
expect_ok "primary full run still selects only the original qdsplit 8k pair" \
    awk -F '\t' 'NF != 5 || $1 != "8k" || $2 != "" { exit 1 } END { if (NR != 4) exit 1 }' \
        "$full_qdsplit_log"
expect_ok "full fake run takes one strict snapshot per mount identity read" \
    sh -c 'test "$(wc -l <"$1")" -eq 12 && ! grep -Fvx -- "--kernel --mountpoint $2 --noheadings --pairs -o ID,MAJ:MIN,SOURCE,TARGET" "$1"' \
        sh "$snapshot_log" "$mount_target"
expect_ok "successful cells record dynamic hctx, qid, IRQ, and interference deltas" \
    awk -F '\t' '
        NR == 1 {
            if ($0 != "# campaign=cstate-primary required_profile=fixed-max-3800000 expected_cells=4") exit 1
            next
        }
        NR == 2 {
            if ($9 != "selected_cpu" || $10 != "sibling_cpu" ||
                $11 != "hctx" || $12 != "qid" || $13 != "irq" ||
                $14 != "target_irq_delta" ||
                $15 != "selected_non_target_irq_delta" ||
                $16 != "sibling_non_target_irq_delta" ||
                $17 != "cpufreq_profile" ||
                $18 != "execution_contract" ||
                $19 != "core_util_contract") exit 1
            next
        }
        $9 != 4 || $10 != 0 || $11 != 7 || $12 != 8 || $13 != 141 ||
        $14 != 1 || $15 != 58 || $16 != 71 || $17 != expected ||
        $18 != execution || $19 != core_util { exit 1 }
        END { if (NR != 6) exit 1 }
    ' expected="$expected_cpufreq_profile" execution="$expected_execution_contract" \
        core_util="$expected_core_util_contract" \
        "$replacement_out"

# The optimization screen uses the same full safety state machine, but its
# explicit CLI selects qdsplit's five opt arms and commits a row only after
# exact timing and post-run readback records have been validated.
reset_full_run
opt_full_out="$full_results/opt-full.tsv"
env "${full_run_env[@]}" OUT="$opt_full_out" \
    EXITOS_TEST_SENTINEL_INTERVAL=0.01 EXITOS_TEST_SENTINEL_TRACE=1 \
    bash tools/campaign-cstate-keeper.sh --test-opt-full-run \
    >"$tmp/opt-full.out" 2>"$tmp/opt-full.err"
opt_full_rc=$?
expect_ok "full fake opt run passes its shared gates and selects exact PAIRED=opt" \
    awk -F '\t' -v rc="$opt_full_rc" '
        BEGIN { if (rc != 0) exit 1 }
        NF != 5 || $1 != "opt" || $2 != "" { exit 1 }
        END { if (NR != 4) exit 1 }
    ' "$full_qdsplit_log"
expect_ok "opt rows strictly record five medians and the three requested differences" \
    awk -F '\t' -v expected="$expected_cpufreq_profile" '
        NR == 1 {
            if ($0 != "# campaign=cstate-opt required_profile=fixed-max-3800000 expected_cells=4") exit 1
            next
        }
        NR == 2 {
            if ($5 != "fs_median_us" || $6 != "pt_median_us" ||
                $7 != "pt_fixedbuf_median_us" || $8 != "pt_bounce_median_us" ||
                $9 != "pt_bounce_fixed_median_us" || $10 != "GUP_saved_us" ||
                $11 != "staged_saved_us" || $12 != "bounce_net_us" ||
                $13 != "buffer_layout" || $18 != "irq" ||
                $22 != "cpufreq_profile" || $23 != "execution_contract" ||
                $24 != "core_util_contract") exit 1
            next
        }
        $5 != "10.000" || $6 != "20.000" || $7 != "14.250" ||
        $8 != "17.500" || $9 != "15.000" || $10 != "5.750" ||
        $11 != "2.500" || $12 != "2.500" || $13 !~ /^bytes=8192,runs=/ ||
        $14 != 4 || $15 != 0 || $16 != 7 || $17 != 8 || $18 != 141 ||
        $19 != 1 || $20 != 58 || $21 != 71 || $22 != expected ||
        $23 != execution || $24 != core_util { exit 1 }
        END { if (NR != 6) exit 1 }
    ' execution="$expected_execution_contract" core_util="$expected_core_util_contract" \
        "$opt_full_out"
expect_ok "normal opt exit leaves no background sentinel process" \
    bash -c '
        mapfile -t started < <(sed -n "s/^TEST_SENTINEL_START //p" "$1")
        mapfile -t stopped < <(sed -n "s/^TEST_SENTINEL_STOP //p" "$1")
        test "${#started[@]}" -eq 4 && test "${#stopped[@]}" -eq 4 || exit 1
        test "${started[*]}" = "${stopped[*]}" || exit 1
        for pid in "${started[@]}"; do
            [[ $pid =~ ^[1-9][0-9]*$ ]] && ! kill -0 "$pid" 2>/dev/null || exit 1
        done
    ' bash "$tmp/opt-full.out"

for opt_mutation in missing duplicate failed malformed no-post; do
    reset_full_run
    opt_bad_out="$full_results/opt-$opt_mutation.tsv"
    env "${full_run_env[@]}" OUT="$opt_bad_out" \
        FAKE_OPT_OUTPUT_MUTATION="$opt_mutation" \
        bash tools/campaign-cstate-keeper.sh --test-opt-full-run \
        >"$tmp/opt-$opt_mutation.out" 2>"$tmp/opt-$opt_mutation.err"
    opt_bad_rc=$?
    expect_ok "opt $opt_mutation output fails closed before committing a row" \
        sh -c 'test "$1" -ne 0 && test "$(wc -l <"$2")" -eq 1 && test "$(wc -l <"$3")" -eq 2 && ! grep -q "^results:" "$4"' \
            sh "$opt_bad_rc" "$full_qdsplit_log" "$opt_bad_out" \
            "$tmp/opt-$opt_mutation.out"
done

for noise_kind in build blocked; do
    reset_full_run
    noise_out="$full_results/noise-$noise_kind.tsv"
    env "${full_run_env[@]}" OUT="$noise_out" \
        EXITOS_TEST_SENTINEL_INTERVAL=0.01 FAKE_NOISE_MUTATION="$noise_kind" \
        bash tools/campaign-cstate-keeper.sh --test-opt-full-run \
        >"$tmp/noise-$noise_kind.out" 2>"$tmp/noise-$noise_kind.err"
    noise_rc=$?
    expect_ok "other-core $noise_kind activity does not reject a locally idle campaign" \
        sh -c 'test "$1" -eq 0 && test "$(wc -l <"$2")" -eq 4 && test "$(wc -l <"$3")" -eq 6 && grep -q "^results:" "$4" && ! grep -q "noisy cell" "$5"' \
            sh "$noise_rc" "$full_qdsplit_log" "$noise_out" \
            "$tmp/noise-$noise_kind.out" "$tmp/noise-$noise_kind.err"
done

expect_ok "target-core utilization gate is immediately before launch lifecycle" \
    bash -c '
        cell=$(sed -n '\''/^[[:space:]]*sleep "\$SETTLE"$/,/^[[:space:]]*ready=0$/p'\'' "$1")
        gate=$(grep -n '\''verify_target_core_utilization'\'' <<<"$cell" | tail -1 | cut -d: -f1)
        sentinel=$(grep -n '\''start_cell_noise_sentinel'\'' <<<"$cell" | tail -1 | cut -d: -f1)
        launch=$(grep -n '\''launch_qdsplit "\$TMP/app.out"'\'' <<<"$cell" | tail -1 | cut -d: -f1)
        test -n "$gate" -a -n "$sentinel" -a -n "$launch" &&
        test "$gate" -lt "$sentinel" && test "$sentinel" -lt "$launch"
    ' bash tools/campaign-cstate-keeper.sh

expect_ok "global compiler names and procs_blocked are not campaign rejection criteria" \
    sh -c '! rg -n "foreign-build|malformed-procs-blocked|kind=procs_blocked" "$1"' \
        sh tools/campaign-cstate-keeper.sh

for execution_mutation in nice policy affinity; do
    reset_full_run
    execution_bad_out="$full_results/execution-$execution_mutation.tsv"
    case "$execution_mutation" in
      nice) execution_env=(FAKE_NICE_MUTATION=ignore) ;;
      policy) execution_env=(FAKE_APP_POLICY_MUTATION=idle) ;;
      affinity) execution_env=(FAKE_APP_AFFINITY_MUTATION=wrong) ;;
    esac
    env "${full_run_env[@]}" "${execution_env[@]}" OUT="$execution_bad_out" \
        bash tools/campaign-cstate-keeper.sh --test-full-run \
        >"$tmp/execution-$execution_mutation.out" \
        2>"$tmp/execution-$execution_mutation.err"
    execution_bad_rc=$?
    expect_ok "$execution_mutation execution drift fails after READY and before a measured row" \
        sh -c 'test "$1" -ne 0 && test "$(wc -l <"$2")" -eq 1 && test "$(wc -l <"$3")" -eq 2 && grep -q "qdsplit execution contract" "$4"' \
            sh "$execution_bad_rc" "$full_qdsplit_log" "$execution_bad_out" \
            "$tmp/execution-$execution_mutation.err"
done

reset_full_run
signal_sentinel_out="$full_results/signal-sentinel.tsv"
env "${full_run_env[@]}" OUT="$signal_sentinel_out" \
    EXITOS_TEST_SENTINEL_INTERVAL=0.01 EXITOS_TEST_SENTINEL_TRACE=1 \
    FAKE_QDSPLIT_HOLD=5 \
    bash tools/campaign-cstate-keeper.sh --test-opt-full-run \
    >"$tmp/signal-sentinel.out" 2>"$tmp/signal-sentinel.err" &
signal_campaign_pid=$!
for _ in $(seq 1 200); do
    grep -q '^TEST_SENTINEL_START ' "$tmp/signal-sentinel.out" 2>/dev/null && break
    kill -0 "$signal_campaign_pid" 2>/dev/null || break
    /bin/sleep 0.01
done
signal_sentinel_pid=$(awk '$1 == "TEST_SENTINEL_START" { print $2; exit }' \
    "$tmp/signal-sentinel.out")
kill -TERM "$signal_campaign_pid" 2>/dev/null || true
signal_campaign_rc=0
wait "$signal_campaign_pid" || signal_campaign_rc=$?
expect_ok "signal exit reaps the active background sentinel" \
    bash -c 'test "$1" -eq 143 && [[ $2 =~ ^[1-9][0-9]*$ ]] && ! kill -0 "$2" 2>/dev/null' \
        bash "$signal_campaign_rc" "$signal_sentinel_pid"

# A second successful epoch uses different nonzero hctx/qid/IRQ values.  This
# prevents a future implementation from satisfying the primary positive case
# by embedding its fixture constants.
reset_full_run
printf '%s\n' 1 >"$identity_ns/mq/7/cpu_list"
mkdir -p "$identity_ns/mq/11"
printf '%s\n' 4 >"$identity_ns/mq/11/cpu_list"
printf '%s\n' \
    '           CPU0       CPU1       CPU2       CPU3       CPU4' \
    '177:         83          0          0          0        300 IR-PCI-MSIX nvme0q12' \
    '178:          0          0          0          0          0 IR-PCI-MSIX nvme0q13' \
    '150:         19          0          0          0         17 local-other' \
    'LOC:         20          0          0          0         30 Local timer interrupts' \
    'RES:         13          0          0          0         14 Rescheduling interrupts' \
    'CAL:         15          0          0          0         16 Function call interrupts' \
    'TLB:         17          0          0          0         18 TLB shootdowns' \
    'ERR:       1999' \
    >"$identity_interrupts"
second_mapping_out="$full_results/second-mapping.tsv"
env "${full_run_env[@]}" OUT="$second_mapping_out" \
    FAKE_TARGET_IRQ=177 FAKE_ALTERNATE_IRQ=178 \
    FAKE_TARGET_ACTION=nvme0q12 FAKE_STALE_ACTION=nvme0q13 \
    FAKE_TARGET_EFFECTIVE_AFFINITY="$identity_irq_proc/irq/177/effective_affinity_list" \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/second-mapping.out" 2>"$tmp/second-mapping.err"
second_mapping_rc=$?
expect_ok "second nonzero hctx/qid/IRQ mapping is resolved dynamically" \
    awk -F '\t' -v rc="$second_mapping_rc" -v expected="$expected_cpufreq_profile" \
        -v execution="$expected_execution_contract" \
        -v core_util="$expected_core_util_contract" '
        BEGIN { if (rc != 0) exit 1 }
        NR <= 2 { next }
        $9 != 4 || $10 != 0 || $11 != 11 || $12 != 12 || $13 != 177 ||
        $14 != 1 || $15 != 58 || $16 != 71 || $17 != expected ||
        $18 != execution || $19 != core_util { exit 1 }
        END { if (NR != 6) exit 1 }
    ' "$second_mapping_out"

reset_full_run
printf '%s\n' 1 >"$identity_ns/mq/7/cpu_list"
wrong_hctx_out="$full_results/wrong-hctx.tsv"
env "${full_run_env[@]}" OUT="$wrong_hctx_out" \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/wrong-hctx.out" 2>"$tmp/wrong-hctx.err"
wrong_hctx_rc=$?
expect_ok "wrong hctx mapping fails before qdsplit or the first result" \
    sh -c 'test "$1" -ne 0 && test ! -s "$2" && test "$(wc -l <"$3")" -eq 2 && grep -q "exactly one hctx" "$4"' \
        sh "$wrong_hctx_rc" "$full_qdsplit_log" "$wrong_hctx_out" \
        "$tmp/wrong-hctx.err"

reset_full_run
zero_irq_out="$full_results/zero-irq.tsv"
env "${full_run_env[@]}" OUT="$zero_irq_out" FAKE_IRQ_MUTATION=zero \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/zero-irq.out" 2>"$tmp/zero-irq.err"
zero_irq_rc=$?
expect_ok "zero target IRQ delta fails before the first result" \
    sh -c 'test "$1" -ne 0 && test "$(wc -l <"$2")" -eq 1 && test "$(wc -l <"$3")" -eq 2 && grep -q "target IRQ delta is not positive" "$4"' \
        sh "$zero_irq_rc" "$full_qdsplit_log" "$zero_irq_out" \
        "$tmp/zero-irq.err"

reset_full_run
wrong_cpu_irq_out="$full_results/wrong-cpu-irq.tsv"
env "${full_run_env[@]}" OUT="$wrong_cpu_irq_out" FAKE_IRQ_MUTATION=wrong-cpu \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/wrong-cpu-irq.out" 2>"$tmp/wrong-cpu-irq.err"
wrong_cpu_irq_rc=$?
expect_ok "target IRQ delivery on the SMT sibling fails before the first result" \
    sh -c 'test "$1" -ne 0 && test "$(wc -l <"$2")" -eq 1 && test "$(wc -l <"$3")" -eq 2 && grep -q "outside selected CPU" "$4"' \
        sh "$wrong_cpu_irq_rc" "$full_qdsplit_log" "$wrong_cpu_irq_out" \
        "$tmp/wrong-cpu-irq.err"

reset_full_run
old_irq_out="$full_results/old-irq.tsv"
env "${full_run_env[@]}" OUT="$old_irq_out" FAKE_IRQ_MUTATION=irq-number \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/old-irq.out" 2>"$tmp/old-irq.err"
old_irq_rc=$?
expect_ok "stale IRQ number fails before the first result" \
    sh -c 'test "$1" -ne 0 && test "$(wc -l <"$2")" -eq 1 && test "$(wc -l <"$3")" -eq 2 && grep -q "IRQ binding changed" "$4"' \
        sh "$old_irq_rc" "$full_qdsplit_log" "$old_irq_out" \
        "$tmp/old-irq.err"

reset_full_run
affinity_drift_out="$full_results/affinity-drift.tsv"
env "${full_run_env[@]}" OUT="$affinity_drift_out" FAKE_IRQ_MUTATION=affinity \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/affinity-drift.out" 2>"$tmp/affinity-drift.err"
affinity_drift_rc=$?
expect_ok "target IRQ effective-affinity drift fails before the first result" \
    sh -c 'test "$1" -ne 0 && test "$(wc -l <"$2")" -eq 1 && test "$(wc -l <"$3")" -eq 2 && grep -q "IRQ affinity" "$4"' \
        sh "$affinity_drift_rc" "$full_qdsplit_log" "$affinity_drift_out" \
        "$tmp/affinity-drift.err"

reset_full_run
diskseq_drift_out="$full_results/diskseq-drift.tsv"
env "${full_run_env[@]}" OUT="$diskseq_drift_out" FAKE_IRQ_MUTATION=diskseq \
    bash tools/campaign-cstate-keeper.sh --test-full-run \
    >"$tmp/diskseq-drift.out" 2>"$tmp/diskseq-drift.err"
diskseq_drift_rc=$?
expect_ok "diskseq drift fails before the first result" \
    sh -c 'test "$1" -ne 0 && test "$(wc -l <"$2")" -eq 1 && test "$(wc -l <"$3")" -eq 2 && grep -q "diskseq" "$4"' \
        sh "$diskseq_drift_rc" "$full_qdsplit_log" "$diskseq_drift_out" \
        "$tmp/diskseq-drift.err"

mkdir -p "$tmp/mnt" "$tmp/results"
paths=$(CPU=0 REPS=1 ITERS=1 SETTLE=0 TMPDIR="$tmp/results" \
    EXITOS_TESTMNT="$tmp/mnt" EXITOS_CAMPAIGN_TEST_MODE=1 \
    bash tools/campaign-cstate-keeper.sh --test-prepare-paths)
expect_ok "default output is an exclusive regular file in TMPDIR" \
    grep -q "^OUTPUT=$tmp/results/exitos-cstate-keeper\." <<<"$paths"
expect_ok "mount work directory is randomized, private, and owned" \
    grep -q "^WORKDIR=$tmp/mnt/\.exitos-cstate\..* mode=700 owner=$(id -u)$" <<<"$paths"

existing_out="$tmp/results/existing.tsv"
: >"$existing_out"
expect_fail "explicit existing output is not clobbered" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 OUT="$existing_out" \
        EXITOS_TESTMNT="$tmp/mnt" EXITOS_CAMPAIGN_TEST_MODE=1 \
        bash tools/campaign-cstate-keeper.sh --test-prepare-paths
ln -s "$tmp/results/target" "$tmp/results/link.tsv"
expect_fail "explicit output symlink is refused" \
    env CPU=0 REPS=1 ITERS=1 SETTLE=0 OUT="$tmp/results/link.tsv" \
        EXITOS_TESTMNT="$tmp/mnt" EXITOS_CAMPAIGN_TEST_MODE=1 \
        bash tools/campaign-cstate-keeper.sh --test-prepare-paths

# Hold the campaign after it has opened OUT, replace the pathname, then let it
# write.  A retained descriptor writes the original inode; any pathname reopen
# follows the replacement symlink and clobbers victim instead.
retained_out="$tmp/results/retained.tsv"
retained_inode="$tmp/results/retained-inode.tsv"
victim="$tmp/results/victim"
retained_log="$tmp/results/retained.log"
printf '%s\n' DO_NOT_TOUCH >"$victim"
CPU=0 REPS=1 ITERS=1 SETTLE=0 OUT="$retained_out" \
    EXITOS_CAMPAIGN_TEST_MODE=1 \
    bash tools/campaign-cstate-keeper.sh --test-output-retained \
    >"$retained_log" 2>&1 & retained_pid=$!
for _ in $(seq 1 100); do
    grep -q '^OUTPUT_FD_READY$' "$retained_log" 2>/dev/null && break
    kill -0 "$retained_pid" 2>/dev/null || break
    sleep 0.01
done
if grep -q '^OUTPUT_FD_READY$' "$retained_log" 2>/dev/null; then
    mv -- "$retained_out" "$retained_inode"
    ln -s -- "$victim" "$retained_out"
    kill -CONT "$retained_pid" 2>/dev/null || true
    wait "$retained_pid"; retained_rc=$?
    expect_ok "retained-output test process completes" test "$retained_rc" -eq 0
    expect_ok "post-replacement result stays on retained inode" \
        grep -q '^RETAINED_FD_WRITE$' "$retained_inode"
    expect_ok "post-replacement result never follows OUT symlink" \
        sh -c 'test "$(cat "$1")" = DO_NOT_TOUCH' sh "$victim"
else
    kill -CONT "$retained_pid" 2>/dev/null || true
    wait "$retained_pid" 2>/dev/null || true
    nok "retained-output test mode reaches its synchronization point"
    nok "post-replacement result stays on retained inode"
    nok "post-replacement result never follows OUT symlink"
fi

if rg -n 'EXITOS_ONLY=-1|export[[:space:]]+EXITOS_ONLY=' tools/campaign-cstate-keeper.sh \
        >"$tmp/bad-only"; then
    nok "campaign contains no invalid all-arms EXITOS_ONLY assignment"
else
    ok "campaign contains no invalid all-arms EXITOS_ONLY assignment"
fi

expect_ok "campaign actually unsets EXITOS_ONLY" \
    rg -q '^[[:space:]]*unset EXITOS_ONLY([[:space:]]|$)' tools/campaign-cstate-keeper.sh
expect_ok "campaign has no global sync command" \
    sh -c '! rg -n "^[[:space:]]*sync([[:space:]]|$)" tools/campaign-cstate-keeper.sh'
expect_ok "campaign never evals resolver output" \
    sh -c '! rg -n "^[[:space:]]*eval([[:space:]]|$)" tools/campaign-cstate-keeper.sh'
expect_ok "campaign result path is never reopened after exclusive creation" \
    sh -c '! rg -n "tee[[:space:]].*\\\$OUT|exec[[:space:]]+3>>.*\\\$OUT" tools/campaign-cstate-keeper.sh'
expect_ok "campaign never removes CURRENT_FILE without owned-directory proof" \
    sh -c '! rg -n "rm[[:space:]].*CURRENT_FILE" tools/campaign-cstate-keeper.sh'
expect_ok "INT explicitly terminates the campaign" \
    rg -q "^trap 'exit 130' INT$" tools/campaign-cstate-keeper.sh
expect_ok "TERM explicitly terminates the campaign" \
    rg -q "^trap 'exit 143' TERM$" tools/campaign-cstate-keeper.sh
expect_ok "qdsplit implements the scattered-buffer contract" \
    rg -q 'EXITOS_REQUIRE_SCATTERED' attribution/qdsplit.c

# Task 2A freezes every byte of both historical plan CLIs before adding the
# independent block-poll screen.  These are literal goldens, including their
# one final newline; command substitution would discard that newline and make
# a byte-level regression invisible.
legacy_plan_actual="$tmp/legacy-plan.actual"
legacy_plan_golden="$tmp/legacy-plan.golden"
printf '%s\n' \
    'PLAN cpu=0 sibling=4 placebo=1 placebo_scope=same-package' \
    'PLAN_ENV EXITOS_ONLY=unset' \
    'PLAN_EXECUTION selected_cpu=0 sibling_cpu=4 prelaunch_windows=30 threshold_lt_pct=15 policy=SCHED_OTHER nice=-20' \
    'PLAN_SCREEN required_profile=fixed-max-3800000 keeper_modes=off,same cells=20' \
    'CELL rep=1 buffer=normal keeper=off keeper_cpu=-' \
    'CELL rep=1 buffer=normal keeper=same keeper_cpu=0' \
    'CELL rep=1 buffer=contiguous keeper=off keeper_cpu=-' \
    'CELL rep=1 buffer=contiguous keeper=same keeper_cpu=0' \
    'CELL rep=2 buffer=normal keeper=off keeper_cpu=-' \
    'CELL rep=2 buffer=normal keeper=same keeper_cpu=0' \
    'CELL rep=2 buffer=contiguous keeper=off keeper_cpu=-' \
    'CELL rep=2 buffer=contiguous keeper=same keeper_cpu=0' \
    'CELL rep=3 buffer=normal keeper=off keeper_cpu=-' \
    'CELL rep=3 buffer=normal keeper=same keeper_cpu=0' \
    'CELL rep=3 buffer=contiguous keeper=off keeper_cpu=-' \
    'CELL rep=3 buffer=contiguous keeper=same keeper_cpu=0' \
    'CELL rep=4 buffer=normal keeper=off keeper_cpu=-' \
    'CELL rep=4 buffer=normal keeper=same keeper_cpu=0' \
    'CELL rep=4 buffer=contiguous keeper=off keeper_cpu=-' \
    'CELL rep=4 buffer=contiguous keeper=same keeper_cpu=0' \
    'CELL rep=5 buffer=normal keeper=off keeper_cpu=-' \
    'CELL rep=5 buffer=normal keeper=same keeper_cpu=0' \
    'CELL rep=5 buffer=contiguous keeper=off keeper_cpu=-' \
    'CELL rep=5 buffer=contiguous keeper=same keeper_cpu=0' \
    >"$legacy_plan_golden"
env CPU=0 REPS=5 EXITOS_KEEPER_MODES=off,same \
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS=0-2,4 \
    EXITOS_QDSPLIT_BIN=/definitely/missing/qdsplit \
    EXITOS_KEEPER_BIN=/definitely/missing/keeper \
    EXITOS_TESTDISK=/definitely/missing/testdisk \
    EXITOS_ONLY=-1 \
    EXITOS_TEST_POLL_PROC_ROOT=/poison/poll-proc \
    EXITOS_TEST_BLKGETDISKSEQ_FILE=/poison/poll-diskseq \
    EXITOS_TEST_POLL_PAUSE_BEFORE=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER=1 \
    EXITOS_TEST_POLL_HELPER_PAUSE=1 \
    EXITOS_TEST_POLL_PAUSE_BEFORE_ALIAS_OPEN=1 \
    EXITOS_TEST_POLL_PAUSE_BEFORE_DEVICE_LINK_OPEN=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER_MQ_LIST=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1=1 \
    EXITOS_TEST_POLL_PARTIAL_OUTPUT=1 \
    bash tools/campaign-cstate-keeper.sh --plan \
    >"$legacy_plan_actual" 2>"$tmp/legacy-plan.err"
expect_ok "historical --plan stdout retains its complete byte-level golden" \
    cmp -s -- "$legacy_plan_golden" "$legacy_plan_actual"

legacy_opt_plan_actual="$tmp/legacy-opt-plan.actual"
legacy_opt_plan_golden="$tmp/legacy-opt-plan.golden"
printf '%s\n' \
    'PLAN cpu=0 sibling=4 placebo=1 placebo_scope=same-package' \
    'PLAN_ENV EXITOS_ONLY=unset' \
    'PLAN_EXECUTION selected_cpu=0 sibling_cpu=4 prelaunch_windows=30 threshold_lt_pct=15 policy=SCHED_OTHER nice=-20' \
    'PLAN_SCREEN screen=opt required_profile=fixed-max-3800000 keeper_modes=off,same cells=20' \
    'PLAN_ARMS EXITOS_PAIRED=opt arms=0:fs-8k-qd1,1:pt-8k-qd1,15:pt-8k-fixedbuf,16:pt-8k-bounce,17:pt-8k-bounce-fixed' \
    'CELL rep=1 buffer=normal keeper=off keeper_cpu=-' \
    'CELL rep=1 buffer=normal keeper=same keeper_cpu=0' \
    'CELL rep=1 buffer=contiguous keeper=off keeper_cpu=-' \
    'CELL rep=1 buffer=contiguous keeper=same keeper_cpu=0' \
    'CELL rep=2 buffer=normal keeper=off keeper_cpu=-' \
    'CELL rep=2 buffer=normal keeper=same keeper_cpu=0' \
    'CELL rep=2 buffer=contiguous keeper=off keeper_cpu=-' \
    'CELL rep=2 buffer=contiguous keeper=same keeper_cpu=0' \
    'CELL rep=3 buffer=normal keeper=off keeper_cpu=-' \
    'CELL rep=3 buffer=normal keeper=same keeper_cpu=0' \
    'CELL rep=3 buffer=contiguous keeper=off keeper_cpu=-' \
    'CELL rep=3 buffer=contiguous keeper=same keeper_cpu=0' \
    'CELL rep=4 buffer=normal keeper=off keeper_cpu=-' \
    'CELL rep=4 buffer=normal keeper=same keeper_cpu=0' \
    'CELL rep=4 buffer=contiguous keeper=off keeper_cpu=-' \
    'CELL rep=4 buffer=contiguous keeper=same keeper_cpu=0' \
    'CELL rep=5 buffer=normal keeper=off keeper_cpu=-' \
    'CELL rep=5 buffer=normal keeper=same keeper_cpu=0' \
    'CELL rep=5 buffer=contiguous keeper=off keeper_cpu=-' \
    'CELL rep=5 buffer=contiguous keeper=same keeper_cpu=0' \
    >"$legacy_opt_plan_golden"
env CPU=0 REPS=5 EXITOS_KEEPER_MODES=off,same \
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS=0-2,4 \
    EXITOS_QDSPLIT_BIN=/definitely/missing/qdsplit \
    EXITOS_KEEPER_BIN=/definitely/missing/keeper \
    EXITOS_TESTDISK=/definitely/missing/testdisk \
    EXITOS_ONLY=-1 \
    EXITOS_TEST_POLL_PROC_ROOT=/poison/poll-proc \
    EXITOS_TEST_BLKGETDISKSEQ_FILE=/poison/poll-diskseq \
    EXITOS_TEST_POLL_PAUSE_BEFORE=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER=1 \
    EXITOS_TEST_POLL_HELPER_PAUSE=1 \
    EXITOS_TEST_POLL_PAUSE_BEFORE_ALIAS_OPEN=1 \
    EXITOS_TEST_POLL_PAUSE_BEFORE_DEVICE_LINK_OPEN=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER_MQ_LIST=1 \
    EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1=1 \
    EXITOS_TEST_POLL_PARTIAL_OUTPUT=1 \
    bash tools/campaign-cstate-keeper.sh --opt-plan \
    >"$legacy_opt_plan_actual" 2>"$tmp/legacy-opt-plan.err"
expect_ok "historical --opt-plan stdout retains its complete byte-level golden" \
    cmp -s -- "$legacy_opt_plan_golden" "$legacy_opt_plan_actual"

tree_fingerprint() {
    local root=$1
    (
        cd "$root" || exit 1
        LC_ALL=C find . -printf '%P\t%y\t%m\t%u\t%g\t%s\t%l\n' |
            LC_ALL=C sort
        while IFS= read -r path; do
            sha256sum -- "$path"
        done < <(LC_ALL=C find . -type f -print | LC_ALL=C sort)
    ) | sha256sum | awk '{ print $1 }'
}

# The abstract block-poll plan is deliberately exercised with every runtime
# helper absent and inherited selector/staging values poisoned.  The caller's
# stdout redirection is outside the guarded tree; the script itself may create
# nothing there.
blockpoll_guard="$tmp/blockpoll-guard"
blockpoll_plan_side="$blockpoll_guard/plan-side"
mkdir -p "$blockpoll_plan_side" "$blockpoll_guard/device-sentinels"
printf '%s\n' whole-before >"$blockpoll_guard/device-sentinels/whole"
printf '%s\n' part-before >"$blockpoll_guard/device-sentinels/part"
printf '%s\n' char-before >"$blockpoll_guard/device-sentinels/char"
chmod 0444 "$blockpoll_guard/device-sentinels/whole" \
    "$blockpoll_guard/device-sentinels/part" \
    "$blockpoll_guard/device-sentinels/char"

blockpoll_plan_golden="$tmp/blockpoll-plan.golden"
printf '%s\n' \
    'PLAN cpu=0 sibling=4 placebo=1 placebo_scope=same-package' \
    'PLAN_ENV EXITOS_PAIRED=blockpoll EXITOS_ONLY=unset EXITOS_STAGE=0' \
    'PLAN_EXECUTION selected_cpu=0 sibling_cpu=4 prelaunch_windows=30 threshold_lt_pct=15 policy=SCHED_OTHER nice=-20' \
    'PLAN_SCREEN screen=blockpoll required_profile=fixed-max-3800000 io_size=8KiB io_bytes=8192 layouts=scattered,contiguous keeper_modes=off,same reps=5 cells=20 arms_per_cell=3' \
    'PLAN_ARMS arms=0:fs-8k-qd1,18:prod-pt-8k-iopoll,19:prod-blk-8k-iopoll' \
    'CELL cell=1 rep=1 layout=scattered keeper=off keeper_cpu=- order_offset=0 arm_order=fs-8k-qd1,prod-pt-8k-iopoll,prod-blk-8k-iopoll' \
    'CELL cell=2 rep=1 layout=scattered keeper=same keeper_cpu=0 order_offset=1 arm_order=prod-pt-8k-iopoll,prod-blk-8k-iopoll,fs-8k-qd1' \
    'CELL cell=3 rep=1 layout=contiguous keeper=off keeper_cpu=- order_offset=2 arm_order=prod-blk-8k-iopoll,fs-8k-qd1,prod-pt-8k-iopoll' \
    'CELL cell=4 rep=1 layout=contiguous keeper=same keeper_cpu=0 order_offset=0 arm_order=fs-8k-qd1,prod-pt-8k-iopoll,prod-blk-8k-iopoll' \
    'CELL cell=5 rep=2 layout=scattered keeper=off keeper_cpu=- order_offset=1 arm_order=prod-pt-8k-iopoll,prod-blk-8k-iopoll,fs-8k-qd1' \
    'CELL cell=6 rep=2 layout=scattered keeper=same keeper_cpu=0 order_offset=2 arm_order=prod-blk-8k-iopoll,fs-8k-qd1,prod-pt-8k-iopoll' \
    'CELL cell=7 rep=2 layout=contiguous keeper=off keeper_cpu=- order_offset=0 arm_order=fs-8k-qd1,prod-pt-8k-iopoll,prod-blk-8k-iopoll' \
    'CELL cell=8 rep=2 layout=contiguous keeper=same keeper_cpu=0 order_offset=1 arm_order=prod-pt-8k-iopoll,prod-blk-8k-iopoll,fs-8k-qd1' \
    'CELL cell=9 rep=3 layout=scattered keeper=off keeper_cpu=- order_offset=2 arm_order=prod-blk-8k-iopoll,fs-8k-qd1,prod-pt-8k-iopoll' \
    'CELL cell=10 rep=3 layout=scattered keeper=same keeper_cpu=0 order_offset=0 arm_order=fs-8k-qd1,prod-pt-8k-iopoll,prod-blk-8k-iopoll' \
    'CELL cell=11 rep=3 layout=contiguous keeper=off keeper_cpu=- order_offset=1 arm_order=prod-pt-8k-iopoll,prod-blk-8k-iopoll,fs-8k-qd1' \
    'CELL cell=12 rep=3 layout=contiguous keeper=same keeper_cpu=0 order_offset=2 arm_order=prod-blk-8k-iopoll,fs-8k-qd1,prod-pt-8k-iopoll' \
    'CELL cell=13 rep=4 layout=scattered keeper=off keeper_cpu=- order_offset=0 arm_order=fs-8k-qd1,prod-pt-8k-iopoll,prod-blk-8k-iopoll' \
    'CELL cell=14 rep=4 layout=scattered keeper=same keeper_cpu=0 order_offset=1 arm_order=prod-pt-8k-iopoll,prod-blk-8k-iopoll,fs-8k-qd1' \
    'CELL cell=15 rep=4 layout=contiguous keeper=off keeper_cpu=- order_offset=2 arm_order=prod-blk-8k-iopoll,fs-8k-qd1,prod-pt-8k-iopoll' \
    'CELL cell=16 rep=4 layout=contiguous keeper=same keeper_cpu=0 order_offset=0 arm_order=fs-8k-qd1,prod-pt-8k-iopoll,prod-blk-8k-iopoll' \
    'CELL cell=17 rep=5 layout=scattered keeper=off keeper_cpu=- order_offset=1 arm_order=prod-pt-8k-iopoll,prod-blk-8k-iopoll,fs-8k-qd1' \
    'CELL cell=18 rep=5 layout=scattered keeper=same keeper_cpu=0 order_offset=2 arm_order=prod-blk-8k-iopoll,fs-8k-qd1,prod-pt-8k-iopoll' \
    'CELL cell=19 rep=5 layout=contiguous keeper=off keeper_cpu=- order_offset=0 arm_order=fs-8k-qd1,prod-pt-8k-iopoll,prod-blk-8k-iopoll' \
    'CELL cell=20 rep=5 layout=contiguous keeper=same keeper_cpu=0 order_offset=1 arm_order=prod-pt-8k-iopoll,prod-blk-8k-iopoll,fs-8k-qd1' \
    >"$blockpoll_plan_golden"

blockpoll_common_env=(
    CPU=0 REPS=5 EXITOS_KEEPER_MODES=off,same
    EXITOS_CPU_SYSFS_ROOT="$tmp/sys" EXITOS_ALLOWED_CPUS=0-2,4
    EXITOS_QDSPLIT_BIN=/definitely/missing/qdsplit
    EXITOS_KEEPER_BIN=/definitely/missing/keeper
    EXITOS_TESTDISK=/definitely/missing/testdisk
    EXITOS_ONLY=-1 EXITOS_PAIRED=poison EXITOS_STAGE=1 EXITOS_BIGSZ=4096
    EXITOS_DEV="$blockpoll_guard/device-sentinels/whole"
    EXITOS_TESTPART="$blockpoll_guard/device-sentinels/part"
    EXITOS_CHARDEV="$blockpoll_guard/device-sentinels/char"
    EXITOS_TEST_POLL_PROC_ROOT=/poison/poll-proc
    EXITOS_TEST_BLKGETDISKSEQ_FILE=/poison/poll-diskseq
    EXITOS_TEST_POLL_PAUSE_BEFORE=1
    EXITOS_TEST_POLL_PAUSE_AFTER=1
    EXITOS_TEST_POLL_HELPER_PAUSE=1
    EXITOS_TEST_POLL_PAUSE_BEFORE_ALIAS_OPEN=1
    EXITOS_TEST_POLL_PAUSE_BEFORE_DEVICE_LINK_OPEN=1
    EXITOS_TEST_POLL_PAUSE_AFTER_MQ_LIST=1
    EXITOS_TEST_POLL_PAUSE_AFTER_CAPTURE1=1
    EXITOS_TEST_POLL_PARTIAL_OUTPUT=1
)
blockpoll_plan_actual="$tmp/blockpoll-plan.actual"
blockpoll_plan_err="$tmp/blockpoll-plan.err"
plan_side_before=$(tree_fingerprint "$blockpoll_plan_side")
plan_sys_before=$(tree_fingerprint "$tmp/sys")
plan_device_before=$(tree_fingerprint "$blockpoll_guard/device-sentinels")
blockpoll_plan_rc=0
env "${blockpoll_common_env[@]}" \
    OUT="$blockpoll_plan_side/must-not-exist.tsv" \
    TMPDIR="$blockpoll_plan_side/must-not-exist-tmp" \
    EXITOS_TESTMNT="$blockpoll_plan_side/must-not-exist-mnt" \
    bash tools/campaign-cstate-keeper.sh --blockpoll-plan \
    >"$blockpoll_plan_actual" 2>"$blockpoll_plan_err" || blockpoll_plan_rc=$?
plan_side_after=$(tree_fingerprint "$blockpoll_plan_side")
plan_sys_after=$(tree_fingerprint "$tmp/sys")
plan_device_after=$(tree_fingerprint "$blockpoll_guard/device-sentinels")

expect_ok "blockpoll plan is abstract and succeeds with runtime helpers absent" \
    test "$blockpoll_plan_rc" -eq 0
expect_ok "blockpoll plan stdout matches the complete byte-level golden" \
    cmp -s -- "$blockpoll_plan_golden" "$blockpoll_plan_actual"
expect_ok "blockpoll plan has twenty unique logical cells with three exact arms each" \
    awk '
        /^CELL / {
            delete field
            for (i = 2; i <= NF; i++) {
                split($i, pair, "=")
                field[pair[1]] = pair[2]
            }
            key = field["rep"] SUBSEP field["layout"] SUBSEP field["keeper"]
            if (++seen[key] != 1) bad = 1
            if (field["cell"] != ++cells) bad = 1
            count = split(field["arm_order"], arm, ",")
            if (count != 3 || arm[1] == arm[2] || arm[1] == arm[3] ||
                arm[2] == arm[3]) bad = 1
            for (j = 1; j <= count; j++)
                if (arm[j] != "fs-8k-qd1" &&
                    arm[j] != "prod-pt-8k-iopoll" &&
                    arm[j] != "prod-blk-8k-iopoll") bad = 1
        }
        END { exit bad || cells != 20 || length(seen) != 20 }
    ' "$blockpoll_plan_actual"
expect_ok "blockpoll plan offsets rotate first arms with exact 7/7/6 balance" \
    awk '
        /^CELL / {
            delete field
            for (i = 2; i <= NF; i++) {
                split($i, pair, "=")
                field[pair[1]] = pair[2]
            }
            split(field["arm_order"], arm, ",")
            offset[field["order_offset"]]++
            first[arm[1]]++
        }
        END {
            exit offset[0] != 7 || offset[1] != 7 || offset[2] != 6 ||
                 first["fs-8k-qd1"] != 7 ||
                 first["prod-pt-8k-iopoll"] != 7 ||
                 first["prod-blk-8k-iopoll"] != 6
        }
    ' "$blockpoll_plan_actual"
expect_ok "successful blockpoll plan emits no stderr" \
    test ! -s "$blockpoll_plan_err"
expect_ok "blockpoll plan creates no OUT, TMPDIR, mount, or other guarded artifact" \
    sh -c 'test "$1" = "$2" && test ! -e "$3" && test ! -e "$4" && test ! -e "$5"' \
        sh "$plan_side_before" "$plan_side_after" \
        "$blockpoll_plan_side/must-not-exist.tsv" \
        "$blockpoll_plan_side/must-not-exist-tmp" \
        "$blockpoll_plan_side/must-not-exist-mnt"
expect_ok "blockpoll plan leaves fake sysfs and device sentinels byte-for-byte unchanged" \
    sh -c 'test "$1" = "$2" && test "$3" = "$4"' \
        sh "$plan_sys_before" "$plan_sys_after" \
        "$plan_device_before" "$plan_device_after"

# Metadata-only archive: the only authorized mutation is one new directory in
# a pre-existing safe parent.  It must contain exactly the plan, manifest, and
# final seal; stdout remains the pure plan bytes.
archive_parent="$tmp/blockpoll-archive-parent"
mkdir "$archive_parent"
chmod 0700 "$archive_parent"
archive_result="$archive_parent/result-1"
archive_stdout="$tmp/blockpoll-archive.stdout"
archive_err="$tmp/blockpoll-archive.err"
archive_guard_before=$(tree_fingerprint "$blockpoll_guard/device-sentinels")
archive_sys_before=$(tree_fingerprint "$tmp/sys")
archive_rc=0
env "${blockpoll_common_env[@]}" \
    OUT="$blockpoll_guard/must-not-exist-archive.tsv" \
    TMPDIR="$blockpoll_guard/must-not-exist-archive-tmp" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan "$archive_result" \
    >"$archive_stdout" 2>"$archive_err" || archive_rc=$?
expect_ok "metadata-only archive creates one fresh result directory" \
    test "$archive_rc" -eq 0
expect_ok "archive stdout and plan.txt are byte-identical to pure blockpoll plan" \
    sh -c 'cmp -s -- "$1" "$2" && cmp -s -- "$2" "$3"' \
        sh "$blockpoll_plan_golden" "$archive_stdout" "$archive_result/plan.txt"

archive_entries=
if [ -d "$archive_result" ]; then
    archive_entries=$(find "$archive_result" -mindepth 1 -maxdepth 1 \
        -printf '%f\n' | LC_ALL=C sort)
fi
expect_ok "archive writes only plan, manifest, and final completion seal" \
    sh -c 'test "$1" = "$2" && test "$(stat -Lc "%a:%u" -- "$3")" = "700:$(id -u)"' \
        sh "$archive_entries" \
        "$(printf '%s\n' ARCHIVE_COMPLETE manifest.txt plan.txt)" \
        "$archive_result"

plan_sha=$(sha256sum -- "$blockpoll_plan_golden" | awk '{ print $1 }')
campaign_sha=$(sha256sum -- tools/campaign-cstate-keeper.sh | awk '{ print $1 }')
qdsplit_sha=$(sha256sum -- attribution/qdsplit.c | awk '{ print $1 }')
iopath_sha=$(sha256sum -- src/iopath.c | awk '{ print $1 }')
intercept_sha=$(sha256sum -- src/intercept.c | awk '{ print $1 }')
readme_sha=$(sha256sum -- tools/README.md | awk '{ print $1 }')
archive_manifest_golden="$tmp/blockpoll-manifest.golden"
printf '%s\n' \
    'format=exitos-cstate-blockpoll-plan-archive' \
    'version=1' \
    'mode=metadata-only' \
    "plan_sha256=$plan_sha" \
    "source_sha256 tools/campaign-cstate-keeper.sh=$campaign_sha" \
    "source_sha256 attribution/qdsplit.c=$qdsplit_sha" \
    "source_sha256 src/iopath.c=$iopath_sha" \
    "source_sha256 src/intercept.c=$intercept_sha" \
    "source_sha256 tools/README.md=$readme_sha" \
    >"$archive_manifest_golden"
expect_ok "archive manifest records exact format, mode, plan SHA, and current source hashes" \
    cmp -s -- "$archive_manifest_golden" "$archive_result/manifest.txt"
manifest_sha=missing
if [ -f "$archive_result/manifest.txt" ]; then
    manifest_sha=$(sha256sum -- "$archive_result/manifest.txt" | awk '{ print $1 }')
fi
archive_seal_golden="$tmp/blockpoll-seal.golden"
printf '%s\n' \
    'format=exitos-cstate-blockpoll-plan-archive' \
    'version=1' \
    'mode=metadata-only' \
    "plan_sha256=$plan_sha" \
    "manifest_sha256=$manifest_sha" \
    'status=complete' \
    >"$archive_seal_golden"
expect_ok "archive completion seal binds the completed manifest and plan" \
    cmp -s -- "$archive_seal_golden" "$archive_result/ARCHIVE_COMPLETE"

archive_guard_after=$(tree_fingerprint "$blockpoll_guard/device-sentinels")
archive_sys_after=$(tree_fingerprint "$tmp/sys")
expect_ok "archive leaves devices, sysfs, OUT, TMPDIR, and mount tree untouched" \
    sh -c 'test "$1" = "$2" && test "$3" = "$4" && test ! -e "$5" && test ! -e "$6" && test ! -e "$7"' \
        sh "$archive_guard_before" "$archive_guard_after" \
        "$archive_sys_before" "$archive_sys_after" \
        "$blockpoll_guard/must-not-exist-archive.tsv" \
        "$blockpoll_guard/must-not-exist-archive-tmp" \
        "$blockpoll_guard/must-not-exist-archive-mnt"

archive_runtime_override_result="$archive_parent/result-runtime-overrides"
archive_runtime_override_out="$tmp/archive-runtime-overrides.out"
archive_runtime_override_err="$tmp/archive-runtime-overrides.err"
archive_runtime_override_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TEST_CTRLDEV=/ignored/controller \
    EXITOS_TEST_CTRL_MAJMIN=999:999 \
    EXITOS_TEST_CTRL_SYSFS_PATH=/ignored/sysfs/controller \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh \
        --archive-plan "$archive_runtime_override_result" \
    >"$archive_runtime_override_out" 2>"$archive_runtime_override_err" ||
        archive_runtime_override_rc=$?
expect_ok "metadata-only archive ignores controller and poll runtime overrides" \
    sh -c 'test "$1" -eq 0 && test ! -s "$2" && cmp -s -- "$3" "$4" && test -f "$5/plan.txt" && test -f "$5/manifest.txt" && test -f "$5/ARCHIVE_COMPLETE"' \
        sh "$archive_runtime_override_rc" "$archive_runtime_override_err" \
        "$blockpoll_plan_golden" "$archive_runtime_override_out" \
        "$archive_runtime_override_result"

archive_before_second=missing
[ ! -d "$archive_result" ] || archive_before_second=$(tree_fingerprint "$archive_result")
archive_second_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan "$archive_result" \
    >"$tmp/archive-second.out" 2>"$tmp/archive-second.err" || archive_second_rc=$?
archive_after_second=missing
[ ! -d "$archive_result" ] || archive_after_second=$(tree_fingerprint "$archive_result")
expect_ok "a second archive to the same directory is rejected without mutation" \
    sh -c 'test "$1" -ne 0 && test "$2" = "$3" && grep -q "archive result directory must not already exist" "$4"' \
        sh "$archive_second_rc" "$archive_before_second" \
        "$archive_after_second" "$tmp/archive-second.err"

archive_link="$archive_parent/result-link"
ln -s -- "$archive_parent/dangling-victim" "$archive_link"
archive_link_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan "$archive_link" \
    >"$tmp/archive-link.out" 2>"$tmp/archive-link.err" || archive_link_rc=$?
expect_ok "archive rejects a symlink result without following it" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive result directory must not already exist" "$3"' \
        sh "$archive_link_rc" "$archive_parent/dangling-victim" \
        "$tmp/archive-link.err"

archive_relative_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan relative-result \
    >"$tmp/archive-relative.out" 2>"$tmp/archive-relative.err" || archive_relative_rc=$?
expect_ok "archive rejects a relative result path" \
    sh -c 'test "$1" -ne 0 && grep -q "archive result directory must be an absolute safe path" "$2"' \
        sh "$archive_relative_rc" "$tmp/archive-relative.err"

archive_nested="$archive_parent/missing-parent/result"
archive_nested_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan "$archive_nested" \
    >"$tmp/archive-nested.out" 2>"$tmp/archive-nested.err" || archive_nested_rc=$?
expect_ok "archive never mkdir -p creates a missing parent" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive parent must already exist" "$3"' \
        sh "$archive_nested_rc" "$archive_parent/missing-parent" \
        "$tmp/archive-nested.err"

archive_writable_parent="$tmp/archive-world-writable"
mkdir "$archive_writable_parent"
chmod 0777 "$archive_writable_parent"
archive_writable_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_writable_parent/result" \
    >"$tmp/archive-writable.out" 2>"$tmp/archive-writable.err" || archive_writable_rc=$?
expect_ok "archive rejects a group/world-writable parent" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive parent permissions are unsafe" "$3"' \
        sh "$archive_writable_rc" "$archive_writable_parent/result" \
        "$tmp/archive-writable.err"

archive_unowned_parent="$tmp/archive-unowned"
mkdir "$archive_unowned_parent"
chmod 0700 "$archive_unowned_parent"
chown 65534:65534 "$archive_unowned_parent"
archive_unowned_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_unowned_parent/result" \
    >"$tmp/archive-unowned.out" 2>"$tmp/archive-unowned.err" || archive_unowned_rc=$?
expect_ok "archive rejects a parent not owned by the current uid" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive parent is not owned by the current uid" "$3"' \
        sh "$archive_unowned_rc" "$archive_unowned_parent/result" \
        "$tmp/archive-unowned.err"

archive_parent_link="$tmp/archive-parent-link"
ln -s -- "$archive_parent" "$archive_parent_link"
archive_parent_link_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_parent_link/result-via-parent-link" \
    >"$tmp/archive-parent-link.out" 2>"$tmp/archive-parent-link.err" || archive_parent_link_rc=$?
expect_ok "archive rejects a symlink parent" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive parent must be a real canonical directory" "$3"' \
        sh "$archive_parent_link_rc" "$archive_parent/result-via-parent-link" \
        "$tmp/archive-parent-link.err"

archive_dot_base="$archive_parent/dot-base"
mkdir "$archive_dot_base"
chmod 0700 "$archive_dot_base"
archive_dot_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_dot_base/../dot-result" \
    >"$tmp/archive-dot.out" 2>"$tmp/archive-dot.err" || archive_dot_rc=$?
expect_ok "archive rejects dot components in the target" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive result directory must be an absolute safe path" "$3"' \
        sh "$archive_dot_rc" "$archive_parent/dot-result" "$tmp/archive-dot.err"

archive_testmnt="$archive_parent/testmnt"
mkdir "$archive_testmnt"
chmod 0700 "$archive_testmnt"
archive_in_mnt_rc=0
env "${blockpoll_common_env[@]}" EXITOS_TESTMNT="$archive_testmnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_testmnt/forbidden-result" \
    >"$tmp/archive-in-mnt.out" 2>"$tmp/archive-in-mnt.err" || archive_in_mnt_rc=$?
expect_ok "archive rejects a result inside EXITOS_TESTMNT" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive result must not be inside EXITOS_TESTMNT" "$3"' \
        sh "$archive_in_mnt_rc" "$archive_testmnt/forbidden-result" \
        "$tmp/archive-in-mnt.err"

archive_relative_mnt_result="$archive_parent/relative-mnt-result"
archive_relative_mnt_rc=0
env "${blockpoll_common_env[@]}" EXITOS_TESTMNT=relative-testmnt \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_relative_mnt_result" \
    >"$tmp/archive-relative-mnt.out" \
    2>"$tmp/archive-relative-mnt.err" || archive_relative_mnt_rc=$?
expect_ok "archive rejects a relative EXITOS_TESTMNT before creating a result" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "EXITOS_TESTMNT must be an absolute path" "$3"' \
        sh "$archive_relative_mnt_rc" "$archive_relative_mnt_result" \
        "$tmp/archive-relative-mnt.err"

archive_root_mnt_result="$archive_parent/root-mnt-result"
archive_root_mnt_rc=0
env "${blockpoll_common_env[@]}" EXITOS_TESTMNT=/ \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_root_mnt_result" \
    >"$tmp/archive-root-mnt.out" \
    2>"$tmp/archive-root-mnt.err" || archive_root_mnt_rc=$?
expect_ok "archive treats root EXITOS_TESTMNT as containing every absolute result" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive result must not be inside EXITOS_TESTMNT" "$3"' \
        sh "$archive_root_mnt_rc" "$archive_root_mnt_result" \
        "$tmp/archive-root-mnt.err"

archive_alias_mnt_real="$archive_parent/alias-mnt-real"
archive_alias_mnt_link="$archive_parent/alias-mnt-link"
mkdir "$archive_alias_mnt_real"
chmod 0700 "$archive_alias_mnt_real"
ln -s -- "$archive_alias_mnt_real" "$archive_alias_mnt_link"
archive_alias_mnt_result="$archive_alias_mnt_real/alias-forbidden-result"
archive_alias_mnt_rc=0
env "${blockpoll_common_env[@]}" EXITOS_TESTMNT="$archive_alias_mnt_link" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_alias_mnt_result" \
    >"$tmp/archive-alias-mnt.out" \
    2>"$tmp/archive-alias-mnt.err" || archive_alias_mnt_rc=$?
expect_ok "archive resolves an EXITOS_TESTMNT symlink alias for containment" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive result must not be inside EXITOS_TESTMNT" "$3"' \
        sh "$archive_alias_mnt_rc" "$archive_alias_mnt_result" \
        "$tmp/archive-alias-mnt.err"

archive_retain_mnt="$tmp/archive-retain-mnt"
archive_retain_parent="$archive_retain_mnt/parent"
archive_retain_original="$tmp/archive-retained-original"
archive_retain_replacement="$tmp/archive-retained-replacement"
mkdir -p "$archive_retain_parent" "$archive_retain_replacement"
chmod 0700 "$archive_retain_mnt" "$archive_retain_parent" \
    "$archive_retain_replacement"
archive_retain_hook="$tmp/archive-retain-before-containment-hook"
printf '%s\n' \
    '#!/bin/bash' \
    'set -eu' \
    'parent=$1' \
    'mv -- "$parent" "$EXITOS_TEST_ARCHIVE_RETAINED_ORIGINAL"' \
    'ln -s -- "$EXITOS_TEST_ARCHIVE_RETAINED_REPLACEMENT" "$parent"' \
    >"$archive_retain_hook"
chmod 0700 "$archive_retain_hook"
archive_retain_result="$archive_retain_parent/result"
archive_retain_rc=0
env "${blockpoll_common_env[@]}" EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_TEST_ARCHIVE_AFTER_PARENT_RETAIN_HOOK="$archive_retain_hook" \
    EXITOS_TEST_ARCHIVE_RETAINED_ORIGINAL="$archive_retain_original" \
    EXITOS_TEST_ARCHIVE_RETAINED_REPLACEMENT="$archive_retain_replacement" \
    EXITOS_TESTMNT="$archive_retain_mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_retain_result" \
    >"$tmp/archive-retain.out" \
    2>"$tmp/archive-retain.err" || archive_retain_rc=$?
expect_ok "archive containment uses the retained pre-swap parent identity" \
    sh -c 'test "$1" -ne 0 && test -d "$2" && test -L "$3" && test ! -e "$2/result" && test ! -e "$4/result" && grep -q "archive result must not be inside EXITOS_TESTMNT" "$5"' \
        sh "$archive_retain_rc" "$archive_retain_original" \
        "$archive_retain_parent" "$archive_retain_replacement" \
        "$tmp/archive-retain.err"

archive_retain_prod_parent="$tmp/archive-retain-prod-parent"
archive_retain_prod_original="$tmp/archive-retain-prod-original"
archive_retain_prod_replacement="$tmp/archive-retain-prod-replacement"
mkdir "$archive_retain_prod_parent" "$archive_retain_prod_replacement"
chmod 0700 "$archive_retain_prod_parent" "$archive_retain_prod_replacement"
archive_retain_prod_result="$archive_retain_prod_parent/result"
archive_retain_prod_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TEST_ARCHIVE_AFTER_PARENT_RETAIN_HOOK="$archive_retain_hook" \
    EXITOS_TEST_ARCHIVE_RETAINED_ORIGINAL="$archive_retain_prod_original" \
    EXITOS_TEST_ARCHIVE_RETAINED_REPLACEMENT="$archive_retain_prod_replacement" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_retain_prod_result" \
    >"$tmp/archive-retain-prod.out" \
    2>"$tmp/archive-retain-prod.err" || archive_retain_prod_rc=$?
expect_ok "production archive rejects the pre-containment parent hook" \
    sh -c 'test "$1" -ne 0 && test -d "$2" && test ! -e "$3" && test ! -e "$2/result" && grep -q "archive test hook is pure-test-only" "$4"' \
        sh "$archive_retain_prod_rc" "$archive_retain_prod_parent" \
        "$archive_retain_prod_original" "$tmp/archive-retain-prod.err"

archive_unmounted_mnt="$tmp/archive-unmounted-same-dev-mnt"
archive_unmounted_parent="$tmp/archive-unmounted-same-dev-parent"
mkdir "$archive_unmounted_mnt" "$archive_unmounted_parent"
chmod 0700 "$archive_unmounted_mnt" "$archive_unmounted_parent"
archive_unmounted_result="$archive_unmounted_parent/result"
archive_unmounted_rc=0
env "${blockpoll_common_env[@]}" EXITOS_TESTMNT="$archive_unmounted_mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_unmounted_result" \
    >"$tmp/archive-unmounted.out" \
    2>"$tmp/archive-unmounted.err" || archive_unmounted_rc=$?
expect_ok "an unmounted EXITOS_TESTMNT does not reject another same-filesystem archive" \
    sh -c 'test "$1" -eq 0 && cmp -s -- "$2" "$3" && test -f "$4/ARCHIVE_COMPLETE"' \
        sh "$archive_unmounted_rc" "$blockpoll_plan_golden" \
        "$tmp/archive-unmounted.out" "$archive_unmounted_result"

archive_parent_swap_root="$tmp/archive-parent-swap"
archive_parent_swap="$archive_parent_swap_root/parent"
mkdir -p "$archive_parent_swap"
chmod 0700 "$archive_parent_swap_root" "$archive_parent_swap"
archive_parent_swap_hook="$tmp/archive-parent-swap-hook"
printf '%s\n' \
    '#!/bin/bash' \
    'set -eu' \
    'parent=$1' \
    'mv -- "$parent" "$parent.retained"' \
    'mkdir -- "$parent"' \
    'chmod 0700 "$parent"' \
    >"$archive_parent_swap_hook"
chmod 0700 "$archive_parent_swap_hook"
archive_parent_swap_result="$archive_parent_swap/result"
archive_parent_swap_rc=0
env "${blockpoll_common_env[@]}" EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_TEST_ARCHIVE_AFTER_PARENT_CHECK_HOOK="$archive_parent_swap_hook" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_parent_swap_result" \
    >"$tmp/archive-parent-swap.out" \
    2>"$tmp/archive-parent-swap.err" || archive_parent_swap_rc=$?
expect_ok "archive detects a parent pathname swap without writing either directory" \
    sh -c 'test "$1" -ne 0 && test -d "$2.retained" && test -d "$2" && test ! -e "$2.retained/result" && test ! -e "$2/result" && grep -q "archive parent identity changed after validation" "$3"' \
        sh "$archive_parent_swap_rc" "$archive_parent_swap" \
        "$tmp/archive-parent-swap.err"

archive_result_swap_parent="$tmp/archive-result-swap-parent"
archive_result_swap_victim="$tmp/archive-result-swap-victim"
mkdir "$archive_result_swap_parent" "$archive_result_swap_victim"
chmod 0700 "$archive_result_swap_parent" "$archive_result_swap_victim"
archive_result_swap_hook="$tmp/archive-result-swap-hook"
printf '%s\n' \
    '#!/bin/bash' \
    'set -eu' \
    'ln -s -- "$EXITOS_TEST_ARCHIVE_HOOK_VICTIM" "$2"' \
    >"$archive_result_swap_hook"
chmod 0700 "$archive_result_swap_hook"
archive_result_swap_result="$archive_result_swap_parent/result"
archive_result_swap_rc=0
env "${blockpoll_common_env[@]}" EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_TEST_ARCHIVE_AFTER_PARENT_CHECK_HOOK="$archive_result_swap_hook" \
    EXITOS_TEST_ARCHIVE_HOOK_VICTIM="$archive_result_swap_victim" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_result_swap_result" \
    >"$tmp/archive-result-swap.out" \
    2>"$tmp/archive-result-swap.err" || archive_result_swap_rc=$?
expect_ok "archive refuses a result symlink inserted after validation without false seal" \
    sh -c 'test "$1" -ne 0 && test -L "$2" && test -z "$(find "$3" -mindepth 1 -maxdepth 1 -print -quit)" && test ! -e "$3/ARCHIVE_COMPLETE" && grep -q "archive result directory must not already exist" "$4"' \
        sh "$archive_result_swap_rc" "$archive_result_swap_result" \
        "$archive_result_swap_victim" "$tmp/archive-result-swap.err"

archive_seal_fsync_parent="$tmp/archive-seal-fsync-parent"
mkdir "$archive_seal_fsync_parent"
chmod 0700 "$archive_seal_fsync_parent"
archive_seal_fsync_result="$archive_seal_fsync_parent/result"
archive_seal_fsync_rc=0
env "${blockpoll_common_env[@]}" EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_TEST_ARCHIVE_FAIL_AFTER_SEAL_RENAME=1 \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_seal_fsync_result" \
    >"$tmp/archive-seal-fsync.out" \
    2>"$tmp/archive-seal-fsync.err" || archive_seal_fsync_rc=$?
archive_seal_fsync_entries=
if [ -d "$archive_seal_fsync_result" ]; then
    archive_seal_fsync_entries=$(find "$archive_seal_fsync_result" \
        -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort)
fi
expect_ok "archive rolls back a published seal when post-rename fsync fails" \
    sh -c 'test "$1" -ne 0 && test -f "$2/plan.txt" && test -f "$2/manifest.txt" && test ! -e "$2/ARCHIVE_COMPLETE" && test "$3" = "$4" && grep -q "injected archive seal directory fsync failure" "$5"' \
        sh "$archive_seal_fsync_rc" "$archive_seal_fsync_result" \
        "$archive_seal_fsync_entries" \
        "$(printf '%s\n' manifest.txt plan.txt)" \
        "$tmp/archive-seal-fsync.err"

archive_seal_hook_prod_parent="$tmp/archive-seal-hook-prod-parent"
mkdir "$archive_seal_hook_prod_parent"
chmod 0700 "$archive_seal_hook_prod_parent"
archive_seal_hook_prod_result="$archive_seal_hook_prod_parent/result"
archive_seal_hook_prod_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TEST_ARCHIVE_FAIL_AFTER_SEAL_RENAME=1 \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_seal_hook_prod_result" \
    >"$tmp/archive-seal-hook-prod.out" \
    2>"$tmp/archive-seal-hook-prod.err" || archive_seal_hook_prod_rc=$?
expect_ok "production archive rejects the post-rename fsync hook" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive test hook is pure-test-only" "$3"' \
        sh "$archive_seal_hook_prod_rc" "$archive_seal_hook_prod_result" \
        "$tmp/archive-seal-hook-prod.err"

archive_producer_fail_parent="$tmp/archive-producer-fail-parent"
mkdir "$archive_producer_fail_parent"
chmod 0700 "$archive_producer_fail_parent"
archive_producer_fail_result="$archive_producer_fail_parent/result"
archive_producer_fail_rc=0
env "${blockpoll_common_env[@]}" EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_TEST_BLOCKPOLL_PLAN_PRODUCER_FAIL=1 \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_producer_fail_result" \
    >"$tmp/archive-producer-fail.out" \
    2>"$tmp/archive-producer-fail.err" || archive_producer_fail_rc=$?
expect_ok "archive captures producer rc stderr and partial stdout before mkdir" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && test ! -s "$3" && grep -q "injected blockpoll plan producer failure" "$4"' \
        sh "$archive_producer_fail_rc" "$archive_producer_fail_result" \
        "$tmp/archive-producer-fail.out" "$tmp/archive-producer-fail.err"

archive_producer_hook_prod_parent="$tmp/archive-producer-hook-prod-parent"
mkdir "$archive_producer_hook_prod_parent"
chmod 0700 "$archive_producer_hook_prod_parent"
archive_producer_hook_prod_result="$archive_producer_hook_prod_parent/result"
archive_producer_hook_prod_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TEST_BLOCKPOLL_PLAN_PRODUCER_FAIL=1 \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_producer_hook_prod_result" \
    >"$tmp/archive-producer-hook-prod.out" \
    2>"$tmp/archive-producer-hook-prod.err" || archive_producer_hook_prod_rc=$?
expect_ok "production archive rejects the blockpoll producer hook" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive test hook is pure-test-only" "$3"' \
        sh "$archive_producer_hook_prod_rc" \
        "$archive_producer_hook_prod_result" \
        "$tmp/archive-producer-hook-prod.err"

archive_hook_production_parent="$tmp/archive-hook-production-parent"
mkdir "$archive_hook_production_parent"
chmod 0700 "$archive_hook_production_parent"
archive_hook_production_result="$archive_hook_production_parent/result"
archive_hook_production_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TEST_ARCHIVE_AFTER_PARENT_CHECK_HOOK="$archive_result_swap_hook" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_hook_production_result" \
    >"$tmp/archive-hook-production.out" \
    2>"$tmp/archive-hook-production.err" || archive_hook_production_rc=$?
expect_ok "production archive rejects the deterministic race hook" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive test hook is pure-test-only" "$3"' \
        sh "$archive_hook_production_rc" "$archive_hook_production_result" \
        "$tmp/archive-hook-production.err"

# Let the plan hash succeed, then inject failure before the first source hash.
# A partial metadata attempt may retain plan.txt for diagnosis, but it must
# never claim ARCHIVE_COMPLETE and must not spill any other file.
archive_partial="$archive_parent/partial-result"
archive_partial_rc=0
env "${blockpoll_common_env[@]}" EXITOS_CAMPAIGN_TEST_MODE=1 \
    EXITOS_TEST_ARCHIVE_HASH_FAIL_AFTER_PLAN=1 \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan "$archive_partial" \
    >"$tmp/archive-partial.out" 2>"$tmp/archive-partial.err" || archive_partial_rc=$?
archive_partial_entries=
if [ -d "$archive_partial" ]; then
    archive_partial_entries=$(find "$archive_partial" -mindepth 1 -maxdepth 1 \
        -printf '%f\n' | LC_ALL=C sort)
fi
expect_ok "archive failure after plan creation leaves no false completion seal" \
    sh -c 'test "$1" -ne 0 && test -d "$2" && test -f "$2/plan.txt" && test ! -e "$2/ARCHIVE_COMPLETE" && test "$3" = plan.txt && grep -q "injected archive hash failure after plan" "$4"' \
        sh "$archive_partial_rc" "$archive_partial" "$archive_partial_entries" \
        "$tmp/archive-partial.err"

archive_malicious_bin="$tmp/archive-malicious-bin"
archive_malicious_marker="$tmp/archive-malicious-sha256sum.executed"
mkdir "$archive_malicious_bin"
printf '%s\n' \
    '#!/bin/bash' \
    'printf "%s\n" executed >"$EXITOS_TEST_SHA256SUM_MARKER"' \
    'exit 88' \
    >"$archive_malicious_bin/sha256sum"
chmod 0700 "$archive_malicious_bin/sha256sum"
archive_malicious_parent="$tmp/archive-malicious-parent"
mkdir "$archive_malicious_parent"
chmod 0700 "$archive_malicious_parent"
archive_malicious_result="$archive_malicious_parent/result"
archive_malicious_rc=0
env "${blockpoll_common_env[@]}" \
    PATH="$archive_malicious_bin:$PATH" \
    EXITOS_TEST_SHA256SUM_MARKER="$archive_malicious_marker" \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_malicious_result" \
    >"$tmp/archive-malicious.out" \
    2>"$tmp/archive-malicious.err" || archive_malicious_rc=$?
archive_malicious_entries=
if [ -d "$archive_malicious_result" ]; then
    archive_malicious_entries=$(find "$archive_malicious_result" \
        -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort)
fi
expect_ok "archive hashes internally and never executes PATH sha256sum" \
    sh -c 'test "$1" -eq 0 && test ! -e "$2" && cmp -s -- "$3" "$4" && test "$5" = "$6"' \
        sh "$archive_malicious_rc" "$archive_malicious_marker" \
        "$blockpoll_plan_golden" "$tmp/archive-malicious.out" \
        "$archive_malicious_entries" \
        "$(printf '%s\n' ARCHIVE_COMPLETE manifest.txt plan.txt)"

archive_hash_hook_prod_parent="$tmp/archive-hash-hook-prod-parent"
mkdir "$archive_hash_hook_prod_parent"
chmod 0700 "$archive_hash_hook_prod_parent"
archive_hash_hook_prod_result="$archive_hash_hook_prod_parent/result"
archive_hash_hook_prod_rc=0
env "${blockpoll_common_env[@]}" \
    EXITOS_TEST_ARCHIVE_HASH_FAIL_AFTER_PLAN=1 \
    EXITOS_TESTMNT="$blockpoll_guard/must-not-exist-archive-mnt" \
    bash tools/campaign-cstate-keeper.sh --archive-plan \
        "$archive_hash_hook_prod_result" \
    >"$tmp/archive-hash-hook-prod.out" \
    2>"$tmp/archive-hash-hook-prod.err" || archive_hash_hook_prod_rc=$?
expect_ok "production archive rejects the internal hash failure hook" \
    sh -c 'test "$1" -ne 0 && test ! -e "$2" && grep -q "archive test hook is pure-test-only" "$3"' \
        sh "$archive_hash_hook_prod_rc" "$archive_hash_hook_prod_result" \
        "$tmp/archive-hash-hook-prod.err"

echo "1..$n  ($fail failed)"
test "$fail" -eq 0
