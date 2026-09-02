#!/bin/bash
# Pure fake-sysfs tests.  This file never writes /sys and never changes a CPU.
set -u
cd "$(dirname "$0")/../.."

SCRIPT=tools/cpufreq-transaction.sh
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
expect_rc() {
    local label=$1 wanted=$2 got
    shift 2
    "$@"
    got=$?
    if [ "$got" -eq "$wanted" ]; then
        ok "$label"
    else
        nok "$label (wanted rc=$wanted, got rc=$got)"
    fi
}

tmp=$(mktemp -d /tmp/exitos-cpufreq-test.XXXXXX) || {
    echo "Bail out! cannot create private cpufreq test directory" >&2
    exit 1
}
case "$tmp" in
  /tmp/exitos-cpufreq-test.*) ;;
  *) echo "Bail out! unsafe cpufreq test directory: $tmp" >&2; exit 1 ;;
esac
[ -n "$tmp" ] && [ "$tmp" != / ] && [ "$tmp" != /sys ] && [[ $tmp != /sys/* ]] || {
    echo "Bail out! refusing unsafe cpufreq test root" >&2
    exit 1
}
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
locks="$tmp/locks"
write_log="$tmp/write.log"
action="$tmp/write.action"
mkdir -p "$locks"

reset_policy() {
    rm -rf -- "$sys"
    mkdir -p "$sys/cpu7" "$sys/cpufreq/policy7"
    ln -s ../cpufreq/policy7 "$sys/cpu7/cpufreq"
    printf '%s\n' 1 >"$sys/cpu7/online"
    printf '%s\n' 7 >"$sys/online"
    printf '%s\n' intel_pstate >"$sys/cpufreq/policy7/scaling_driver"
    printf '%s\n' powersave >"$sys/cpufreq/policy7/scaling_governor"
    printf '%s\n' 'performance powersave' >"$sys/cpufreq/policy7/scaling_available_governors"
    printf '%s\n' balance_performance >"$sys/cpufreq/policy7/energy_performance_preference"
    printf '%s\n' 'default performance balance_performance balance_power power' \
        >"$sys/cpufreq/policy7/energy_performance_available_preferences"
    printf '%s\n' 800000 >"$sys/cpufreq/policy7/scaling_min_freq"
    printf '%s\n' 3800000 >"$sys/cpufreq/policy7/scaling_max_freq"
    printf '%s\n' 800000 >"$sys/cpufreq/policy7/scaling_cur_freq"
    printf '%s\n' 800000 >"$sys/cpufreq/policy7/cpuinfo_min_freq"
    printf '%s\n' 3800000 >"$sys/cpufreq/policy7/cpuinfo_max_freq"
    printf '%s\n' 7 >"$sys/cpufreq/policy7/affected_cpus"
    printf '%s\n' 7 >"$sys/cpufreq/policy7/related_cpus"
    : >"$write_log"
    printf '%s\n' normal >"$action"
}

writer="$tmp/fake-write"
cat >"$writer" <<'EOF'
#!/bin/bash
set -u
path=$1
value=$2
base=${path##*/}
mode=$(cat "$FAKE_WRITE_ACTION")
printf '%s=%s\n' "$base" "$value" >>"$FAKE_WRITE_LOG"
case "$mode:$base:$value" in
  ignore-apply-governor:scaling_governor:performance) exit 0 ;;
  fail-apply-epp:energy_performance_preference:performance) exit 19 ;;
  ignore-restore-governor:scaling_governor:powersave) exit 0 ;;
esac
# Model the conservative HWP failure mode the transaction must not depend on:
# a scaling_min_freq change made under performance never reaches the visible
# policy.  Production stages bounds outside performance and uses exact bounded
# readback, so this fails closed even though the test host's observed lag was
# temporary.
if [ "$mode" = hwp-performance-min-ignore ] &&
   [ "$base" = scaling_min_freq ] &&
   [ "$(cat "${path%/*}/scaling_governor")" = performance ]; then
    exit 0
fi
if [ "$mode" = delayed-min-readback ] && [ "$base" = scaling_min_freq ]; then
    # Emulate the short write-to-readback visibility lag observed from
    # intel_pstate+HWP on the test host.  Detach the delayed update from this
    # helper so the caller can begin polling before it becomes visible.
    ( sleep 0.02; printf '%s\n' "$value" >"$path" ) \
        </dev/null >/dev/null 2>&1 &
    exit 0
fi
if { [ "$mode" = delayed-governor-coalesced ] ||
     [ "$mode" = delayed-powersave-coalesced ]; } &&
   [ "$base" = scaling_governor ]; then
    request="${path}.requested"
    printf '%s\n' "$value" >"$request"
    delayed_value=performance
    [ "$mode" != delayed-powersave-coalesced ] || delayed_value=powersave
    if [ "$value" = "$delayed_value" ]; then
        # CPUFreq-style work: execution is delayed, but it consumes the newest
        # request rather than carrying the old value in the work item.
        ( sleep 0.6; cat "$request" >"$path" ) \
            </dev/null >/dev/null 2>&1 &
    else
        printf '%s\n' "$value" >"$path"
    fi
    exit 0
fi
printf '%s\n' "$value" >"$path"
EOF
chmod 0700 "$writer"

run_tool() {
    EXITOS_CPUFREQ_TEST_MODE=1 \
    EXITOS_CPU_SYSFS_ROOT="$sys" \
    EXITOS_CPUFREQ_LOCK_ROOT="$locks" \
    EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
    FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$action" \
        bash "$SCRIPT" "$@"
}

reset_policy
status=$(run_tool --status 7 2>"$tmp/status.err")
expect_ok "status reports the canonical policy and exact CPU scope" \
    grep -q '^CPUFREQ_STATUS cpu=7 policy=policy7 affected_cpus=7 related_cpus=7$' <<<"$status"
expect_ok "status reports intel_pstate powersave and EPP" \
    grep -q '^CONTROL driver=intel_pstate governor=powersave epp=balance_performance$' <<<"$status"
expect_ok "status is read-only" test ! -s "$write_log"

plan=$(run_tool --plan-performance 7 2>"$tmp/plan.err")
expect_ok "performance plan states the requested changes" \
    grep -q '^CPUFREQ_PLAN mode=performance cpu=7 policy=policy7 affected_cpus=7 related_cpus=7$' <<<"$plan"
expect_ok "plan explicitly says C-state and actual MHz are uncontrolled" \
    grep -q '^UNCONTROLLED core_cstate package_cstate uncore pcie_aspm turbo thermal actual_mhz$' <<<"$plan"
expect_ok "performance plan does not write fake sysfs" test ! -s "$write_log"

fixed_plan=$(run_tool --plan-fixed 7 3000000 2>"$tmp/fixed-plan.err")
expect_ok "fixed plan names the exact requested bounds" \
    grep -q '^REQUEST governor=performance epp=performance min_khz=3000000 max_khz=3000000$' <<<"$fixed_plan"
expect_ok "fixed plan is also read-only" test ! -s "$write_log"

printf '%s\n' powersave >"$sys/cpufreq/policy7/scaling_available_governors"
expect_fail "an unavailable performance governor is rejected in preflight" \
    run_tool --plan-performance 7
expect_ok "unavailable governor rejection performs no write" test ! -s "$write_log"
reset_policy
rm -f -- "$sys/cpufreq/policy7/scaling_driver"
expect_fail "a missing required control field is rejected" run_tool --status 7
reset_policy
printf '%s\n' performance >"$sys/cpufreq/policy7/scaling_governor"
printf '%s\n' performance >"$sys/cpufreq/policy7/energy_performance_preference"
printf '%s\n' performance >"$sys/cpufreq/policy7/scaling_available_governors"
expect_fail "original performance without a powersave staging governor is rejected" \
    run_tool --with-fixed 7 3000000 --expect-policy-cpus 7 -- true
expect_ok "missing staging governor is rejected before the first write" \
    test ! -s "$write_log"
reset_policy
printf '%s\n' 0 >"$sys/cpu7/online"
expect_fail "an offline CPU is rejected" run_tool --status 7
reset_policy

huge_uint=$(printf '9%.0s' {1..200})
expect_rc "overlong CPU input is rejected before Bash integer conversion" 2 \
    run_tool --status "$huge_uint"
expect_rc "overlong frequency input is rejected before Bash integer conversion" 2 \
    run_tool --plan-fixed 7 "$huge_uint"

reset_policy
expect_rc "trusted nesting marker is refused outside the core wrapper protocol" 2 \
    run_tool --with-performance 7 --expect-policy-cpus 7 \
        --core-trusted-child-transactions 1 -- true
expect_ok "unauthorized trusted nesting marker is rejected before writes" \
    test ! -s "$write_log"
expect_rc "zero trusted nesting depth is rejected instead of weakening the protocol" 2 \
    run_tool --with-performance 7 --expect-policy-cpus 7 \
        --core-trusted-child-transactions 0 -- true
expect_ok "invalid trusted nesting depth is rejected before writes" \
    test ! -s "$write_log"

real_sys_link="$tmp/real-sys-link"
ln -s /sys/devices/system/cpu "$real_sys_link"
expect_fail "test mode rejects a pathname alias that resolves into real /sys" \
    env EXITOS_CPUFREQ_TEST_MODE=1 EXITOS_CPU_SYSFS_ROOT="$real_sys_link" \
        EXITOS_CPUFREQ_LOCK_ROOT="$locks" EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
        FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$action" \
        bash "$SCRIPT" --status 0

printf '%s\n' '7 8' >"$sys/cpufreq/policy7/affected_cpus"
printf '%s\n' '7 8' >"$sys/cpufreq/policy7/related_cpus"
expect_fail "execution refuses an unacknowledged multi-CPU policy" \
    run_tool --with-performance 7 --expect-policy-cpus 7 -- true
expect_ok "scope refusal happens before the first write" test ! -s "$write_log"
printf '%s\n' 7 >"$sys/cpufreq/policy7/affected_cpus"
printf '%s\n' 7 >"$sys/cpufreq/policy7/related_cpus"

reset_policy
victim="$tmp/lock-victim"
printf '%s\n' LOCK-VICTIM >"$victim"
rm -rf -- "$locks"
mkdir -p "$locks"
ln -s "$victim" "$locks/exitos-cpufreq"
expect_rc "a hostile pre-created lock namespace is refused" 2 \
    run_tool --with-performance 7 --expect-policy-cpus 7 -- true
expect_ok "lock namespace attack cannot truncate its target" \
    test "$(cat "$victim")" = LOCK-VICTIM
expect_ok "lock namespace refusal happens before cpufreq writes" test ! -s "$write_log"
rm -f -- "$locks/exitos-cpufreq"

reset_policy
after_lock="$tmp/after-lock"
cat >"$after_lock" <<'EOF'
#!/bin/bash
printf '%s\n' '7 8' >"$EXITOS_CPU_SYSFS_ROOT/cpufreq/policy7/related_cpus"
EOF
chmod 0700 "$after_lock"
EXITOS_CPUFREQ_AFTER_LOCK_HELPER="$after_lock" expect_rc \
    "related_cpus drift after lock is refused before the first write" 2 \
    run_tool --with-performance 7 --expect-policy-cpus 7 -- true
expect_ok "post-lock scope drift refusal performs no cpufreq write" test ! -s "$write_log"
reset_policy

post_lock_profile="$tmp/post-lock-profile"
cat >"$post_lock_profile" <<'EOF'
#!/bin/bash
policy=$EXITOS_CPU_SYSFS_ROOT/cpufreq/policy7
printf '%s\n' performance >"$policy/scaling_governor"
printf '%s\n' performance >"$policy/energy_performance_preference"
printf '%s\n' 1600000 >"$policy/scaling_min_freq"
printf '%s\n' 3200000 >"$policy/scaling_max_freq"
EOF
chmod 0700 "$post_lock_profile"
EXITOS_CPUFREQ_AFTER_LOCK_HELPER="$post_lock_profile" run_tool \
    --with-fixed 7 3000000 --expect-policy-cpus 7 -- true \
    >"$tmp/post-lock-profile.out" 2>"$tmp/post-lock-profile.err"
post_lock_profile_rc=$?
expect_ok "post-lock authoritative profile can run a transaction" \
    test "$post_lock_profile_rc" -eq 0
expect_ok "restoration uses the post-lock snapshot, not stale pre-lock values" \
    bash -c 'test "$(cat "$1")" = performance && test "$(cat "$2")" = performance && test "$(cat "$3")" = 1600000 && test "$(cat "$4")" = 3200000' \
        _ "$sys/cpufreq/policy7/scaling_governor" \
        "$sys/cpufreq/policy7/energy_performance_preference" \
        "$sys/cpufreq/policy7/scaling_min_freq" \
        "$sys/cpufreq/policy7/scaling_max_freq"
reset_policy

observer="$tmp/observe"
cat >"$observer" <<'EOF'
#!/bin/bash
policy=$EXITOS_CPU_SYSFS_ROOT/cpufreq/policy7
printf '%s %s %s %s\n' \
    "$(cat "$policy/scaling_governor")" \
    "$(cat "$policy/energy_performance_preference")" \
    "$(cat "$policy/scaling_min_freq")" \
    "$(cat "$policy/scaling_max_freq")" >"$OBSERVED"
EOF
chmod 0700 "$observer"

reset_policy
OBSERVED="$tmp/observed" run_tool --with-performance 7 \
    --expect-policy-cpus 7 -- "$observer" >"$tmp/perf.out" 2>"$tmp/perf.err"
expect_ok "performance transaction returns success" test $? -eq 0
expect_ok "child starts only after governor and EPP are applied" \
    test "$(cat "$tmp/observed")" = 'performance performance 800000 3800000'
expect_ok "performance transaction emits an applied verification record" \
    grep -q '^CPUFREQ_VERIFY phase=applied mode=performance governor=performance epp=performance min_khz=800000 max_khz=3800000$' "$tmp/perf.out"
expect_ok "current frequency is reported only as a non-gating observation" \
    grep -q '^CPUFREQ_OBSERVATION scaling_cur_freq_khz=800000 pass_criterion=no$' "$tmp/perf.out"
expect_ok "performance transaction restores governor" \
    test "$(cat "$sys/cpufreq/policy7/scaling_governor")" = powersave
expect_ok "performance transaction restores EPP" \
    test "$(cat "$sys/cpufreq/policy7/energy_performance_preference")" = balance_performance
expect_ok "performance transaction verifies restoration" \
    grep -q '^CPUFREQ_VERIFY phase=restored governor=powersave epp=balance_performance min_khz=800000 max_khz=3800000$' "$tmp/perf.out"
expect_ok "performance profile is rechecked after the child" \
    grep -q '^CPUFREQ_VERIFY phase=post-command mode=performance profile_unchanged=yes$' "$tmp/perf.out"

reset_policy
printf '%s\n' ignore-apply-governor >"$action"
rm -f -- "$tmp/should-not-run"
expect_fail "a write/readback mismatch is fatal" \
    run_tool --with-performance 7 --expect-policy-cpus 7 -- \
        touch "$tmp/should-not-run"
expect_ok "readback mismatch refuses to launch the child" \
    test ! -e "$tmp/should-not-run"
expect_ok "readback mismatch leaves the original governor restored" \
    test "$(cat "$sys/cpufreq/policy7/scaling_governor")" = powersave

reset_policy
printf '%s\n' delayed-governor-coalesced >"$action"
run_tool --with-performance 7 --expect-policy-cpus 7 -- true \
    >"$tmp/delayed-governor.out" 2>"$tmp/delayed-governor.err"
delayed_governor_rc=$?
# Wait past the fake worker's 600 ms execution point before checking that
# restoration's newer request won.
sleep 0.5
expect_ok "a timed-out delayed governor apply returns setup failure after restoration" \
    test "$delayed_governor_rc" -eq 3
expect_ok "restoration supersedes a coalesced governor update that executes after timeout" \
    bash -c 'test "$(cat "$1")" = powersave && test "$(cat "$2")" = balance_performance && test "$(cat "$3")" = 800000 && test "$(cat "$4")" = 3800000' \
        _ "$sys/cpufreq/policy7/scaling_governor" \
        "$sys/cpufreq/policy7/energy_performance_preference" \
        "$sys/cpufreq/policy7/scaling_min_freq" \
        "$sys/cpufreq/policy7/scaling_max_freq"

reset_policy
printf '%s\n' performance >"$sys/cpufreq/policy7/scaling_governor"
printf '%s\n' performance >"$sys/cpufreq/policy7/energy_performance_preference"
printf '%s\n' 1600000 >"$sys/cpufreq/policy7/scaling_min_freq"
printf '%s\n' 3200000 >"$sys/cpufreq/policy7/scaling_max_freq"
printf '%s\n' delayed-powersave-coalesced >"$action"
run_tool --with-fixed 7 3000000 --expect-policy-cpus 7 -- true \
    >"$tmp/delayed-powersave.out" 2>"$tmp/delayed-powersave.err"
delayed_powersave_rc=$?
sleep 0.7
expect_ok "a timed-out staging restore is reported as restoration failure" \
    test "$delayed_powersave_rc" -eq 70
expect_ok "final governor compensation wins after a timed-out powersave staging request" \
    bash -c 'test "$(cat "$1")" = performance && test "$(cat "$2")" = performance && test "$(cat "$3")" = 1600000 && test "$(cat "$4")" = 3200000' \
        _ "$sys/cpufreq/policy7/scaling_governor" \
        "$sys/cpufreq/policy7/energy_performance_preference" \
        "$sys/cpufreq/policy7/scaling_min_freq" \
        "$sys/cpufreq/policy7/scaling_max_freq"

reset_policy
printf '%s\n' fail-apply-epp >"$action"
rm -f -- "$tmp/should-not-run"
expect_fail "a partial setup failure is fatal" \
    run_tool --with-performance 7 --expect-policy-cpus 7 -- \
        touch "$tmp/should-not-run"
expect_ok "partial setup failure refuses to launch the child" \
    test ! -e "$tmp/should-not-run"
expect_ok "partial setup failure restores the already changed governor" \
    test "$(cat "$sys/cpufreq/policy7/scaling_governor")" = powersave

reset_policy
run_tool --with-performance 7 --expect-policy-cpus 7 -- \
    bash -c 'exit 23' >"$tmp/child.out" 2>"$tmp/child.err"
child_rc=$?
expect_ok "child failure status is propagated" test "$child_rc" -eq 23
expect_ok "child failure still restores governor and EPP" \
    bash -c 'test "$(cat "$1")" = powersave && test "$(cat "$2")" = balance_performance' \
        _ "$sys/cpufreq/policy7/scaling_governor" \
        "$sys/cpufreq/policy7/energy_performance_preference"

reset_policy
drifter="$tmp/drift-policy"
cat >"$drifter" <<'EOF'
#!/bin/bash
printf '%s\n' powersave >"$EXITOS_CPU_SYSFS_ROOT/cpufreq/policy7/scaling_governor"
EOF
chmod 0700 "$drifter"
run_tool --with-performance 7 --expect-policy-cpus 7 -- "$drifter" \
    >"$tmp/drift.out" 2>"$tmp/drift.err"
drift_rc=$?
expect_ok "profile drift during the child invalidates a nominally successful command" \
    test "$drift_rc" -eq 71
expect_ok "profile drift is reported explicitly" \
    grep -q '^CPUFREQ_PROFILE_DRIFT command_rc=0 policy=policy7$' "$tmp/drift.err"
expect_ok "profile drift path still restores the original profile" \
    bash -c 'test "$(cat "$1")" = powersave && test "$(cat "$2")" = balance_performance' \
        _ "$sys/cpufreq/policy7/scaling_governor" \
        "$sys/cpufreq/policy7/energy_performance_preference"

reset_policy
bounds_drifter="$tmp/drift-bounds"
cat >"$bounds_drifter" <<'EOF'
#!/bin/bash
policy=$EXITOS_CPU_SYSFS_ROOT/cpufreq/policy7
printf '%s\n' 1200000 >"$policy/scaling_min_freq"
printf '%s\n' 3300000 >"$policy/scaling_max_freq"
EOF
chmod 0700 "$bounds_drifter"
run_tool --with-performance 7 --expect-policy-cpus 7 -- "$bounds_drifter" \
    >"$tmp/bounds-drift.out" 2>"$tmp/bounds-drift.err"
bounds_drift_rc=$?
expect_ok "performance-mode bounds drift is reported as profile drift after successful restoration" \
    test "$bounds_drift_rc" -eq 71
expect_ok "performance-mode bounds drift restores the complete original profile" \
    bash -c 'test "$(cat "$1")" = powersave && test "$(cat "$2")" = balance_performance && test "$(cat "$3")" = 800000 && test "$(cat "$4")" = 3800000' \
        _ "$sys/cpufreq/policy7/scaling_governor" \
        "$sys/cpufreq/policy7/energy_performance_preference" \
        "$sys/cpufreq/policy7/scaling_min_freq" \
        "$sys/cpufreq/policy7/scaling_max_freq"

reset_policy
ready="$tmp/signal-ready"
child_pid_file="$tmp/signal-child-pid"
sleeper="$tmp/signal-child"
cat >"$sleeper" <<'EOF'
#!/bin/bash
trap 'exit 0' INT QUIT TERM HUP
printf '%s\n' "$$" >"$CHILD_PID_FILE"
: >"$READY_FILE"
while :; do sleep 1; done
EOF
chmod 0700 "$sleeper"
rm -f -- "$ready" "$child_pid_file"
EXITOS_CPUFREQ_TEST_MODE=1 EXITOS_CPU_SYSFS_ROOT="$sys" \
EXITOS_CPUFREQ_LOCK_ROOT="$locks" EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$action" \
READY_FILE="$ready" CHILD_PID_FILE="$child_pid_file" \
    bash "$SCRIPT" --with-performance 7 --expect-policy-cpus 7 -- "$sleeper" \
    >"$tmp/signal.out" 2>"$tmp/signal.err" &
transaction_pid=$!
for ((wait_i = 0; wait_i < 200; wait_i++)); do
    [ ! -e "$ready" ] || break
    sleep 0.01
done
expect_ok "signal test reaches the verified child, not an apply race" test -e "$ready"
kill -TERM "$transaction_pid"
wait "$transaction_pid"
signal_rc=$?
expect_ok "TERM is reported as exit 143" test "$signal_rc" -eq 143
expect_ok "TERM path restores governor and EPP" \
    bash -c 'test "$(cat "$1")" = powersave && test "$(cat "$2")" = balance_performance' \
        _ "$sys/cpufreq/policy7/scaling_governor" \
        "$sys/cpufreq/policy7/energy_performance_preference"
if [ -s "$child_pid_file" ]; then signal_child_pid=$(<"$child_pid_file"); else signal_child_pid=0; fi
expect_fail "TERM leaves no benchmark child running" \
    bash -c 'kill -0 "$1" 2>/dev/null' _ "$signal_child_pid"

run_real_disposition_signal_case() {
    local sig=$1 expected_rc=$2 case_name ready_file pid_file timeout_file
    local transaction_pid watchdog_pid transaction_rc child_pid=0
    case_name=${sig,,}
    ready_file="$tmp/${case_name}-ready"
    pid_file="$tmp/${case_name}-child-pid"
    timeout_file="$tmp/${case_name}-timed-out"
    reset_policy
    rm -f -- "$ready_file" "$pid_file" "$timeout_file"
    env --default-signal=INT,QUIT,HUP,TERM \
        EXITOS_CPUFREQ_TEST_MODE=1 EXITOS_CPU_SYSFS_ROOT="$sys" \
        EXITOS_CPUFREQ_LOCK_ROOT="$locks" EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
        FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$action" \
        READY_FILE="$ready_file" CHILD_PID_FILE="$pid_file" \
        bash "$SCRIPT" --with-performance 7 --expect-policy-cpus 7 -- "$sleeper" \
        >"$tmp/${case_name}.out" 2>"$tmp/${case_name}.err" &
    transaction_pid=$!
    for ((wait_i = 0; wait_i < 200; wait_i++)); do
        [ ! -e "$ready_file" ] || break
        kill -0 "$transaction_pid" 2>/dev/null || break
        sleep 0.01
    done
    expect_ok "$sig test reaches the verified child" test -e "$ready_file"
    (
        sleep 1
        : >"$timeout_file"
        if [ -s "$pid_file" ]; then
            child_pid=$(<"$pid_file")
            kill -KILL -- "-$child_pid" 2>/dev/null || kill -KILL "$child_pid" 2>/dev/null || true
        fi
        kill -KILL "$transaction_pid" 2>/dev/null || true
    ) &
    watchdog_pid=$!
    kill -"$sig" "$transaction_pid"
    if wait "$transaction_pid" 2>/dev/null; then transaction_rc=0; else transaction_rc=$?; fi
    kill -TERM "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
    expect_ok "$sig returns within one second without escalation" test ! -e "$timeout_file"
    expect_ok "$sig is reported as exit $expected_rc" test "$transaction_rc" -eq "$expected_rc"
    expect_ok "$sig restores the complete original profile" \
        bash -c 'test "$(cat "$1")" = powersave && test "$(cat "$2")" = balance_performance && test "$(cat "$3")" = 800000 && test "$(cat "$4")" = 3800000' \
            _ "$sys/cpufreq/policy7/scaling_governor" \
            "$sys/cpufreq/policy7/energy_performance_preference" \
            "$sys/cpufreq/policy7/scaling_min_freq" \
            "$sys/cpufreq/policy7/scaling_max_freq"
    if [ -s "$pid_file" ]; then child_pid=$(<"$pid_file"); fi
    expect_fail "$sig leaves no benchmark process group" \
        bash -c 'kill -0 -- "-$1" 2>/dev/null' _ "$child_pid"
}

run_real_disposition_signal_case INT 130
run_real_disposition_signal_case QUIT 131

reset_policy
stubborn_ready="$tmp/stubborn-ready"
stubborn_pid_file="$tmp/stubborn-child-pid"
stubborn="$tmp/stubborn-child"
cat >"$stubborn" <<'EOF'
#!/bin/bash
trap '' INT TERM HUP QUIT
printf '%s\n' "$$" >"$CHILD_PID_FILE"
: >"$READY_FILE"
while :; do sleep 1; done
EOF
chmod 0700 "$stubborn"
rm -f -- "$stubborn_ready" "$stubborn_pid_file"
EXITOS_CPUFREQ_TEST_MODE=1 EXITOS_CPU_SYSFS_ROOT="$sys" \
EXITOS_CPUFREQ_LOCK_ROOT="$locks" EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$action" \
READY_FILE="$stubborn_ready" CHILD_PID_FILE="$stubborn_pid_file" \
    bash "$SCRIPT" --with-performance 7 --expect-policy-cpus 7 -- "$stubborn" \
    >"$tmp/stubborn.out" 2>"$tmp/stubborn.err" &
stubborn_transaction_pid=$!
for ((wait_i = 0; wait_i < 200; wait_i++)); do
    [ ! -e "$stubborn_ready" ] || break
    sleep 0.01
done
expect_ok "stubborn-child test reaches the verified child" test -e "$stubborn_ready"
(
    sleep 3
    kill -KILL "$stubborn_transaction_pid" 2>/dev/null || true
) &
watchdog_pid=$!
kill -TERM "$stubborn_transaction_pid"
wait "$stubborn_transaction_pid"
stubborn_rc=$?
kill -TERM "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
expect_ok "TERM escalates a non-cooperative child and returns 143" test "$stubborn_rc" -eq 143
expect_ok "non-cooperative child path still restores the full performance profile" \
    bash -c 'test "$(cat "$1")" = powersave && test "$(cat "$2")" = balance_performance && test "$(cat "$3")" = 800000 && test "$(cat "$4")" = 3800000' \
        _ "$sys/cpufreq/policy7/scaling_governor" \
        "$sys/cpufreq/policy7/energy_performance_preference" \
        "$sys/cpufreq/policy7/scaling_min_freq" \
        "$sys/cpufreq/policy7/scaling_max_freq"
if [ -s "$stubborn_pid_file" ]; then
    stubborn_child_pid=$(<"$stubborn_pid_file")
    kill -KILL -- "-$stubborn_child_pid" 2>/dev/null || true
fi

reset_policy
OBSERVED="$tmp/observed-fixed" run_tool --with-fixed 7 3000000 \
    --expect-policy-cpus 7 -- "$observer" >"$tmp/fixed.out" 2>"$tmp/fixed.err"
expect_ok "fixed transaction returns success" test $? -eq 0
expect_ok "child sees verified fixed bounds" \
    test "$(cat "$tmp/observed-fixed")" = 'performance performance 3000000 3000000'
expect_ok "fixed transaction emits exact applied bounds" \
    grep -q '^CPUFREQ_VERIFY phase=applied mode=fixed governor=performance epp=performance min_khz=3000000 max_khz=3000000$' "$tmp/fixed.out"
expect_ok "fixed transaction restores both original bounds" \
    bash -c 'test "$(cat "$1")" = 800000 && test "$(cat "$2")" = 3800000' \
        _ "$sys/cpufreq/policy7/scaling_min_freq" \
        "$sys/cpufreq/policy7/scaling_max_freq"

reset_policy
printf '%s\n' hwp-performance-min-ignore >"$action"
OBSERVED="$tmp/observed-hwp-fixed" run_tool --with-fixed 7 3000000 \
    --expect-policy-cpus 7 -- "$observer" \
    >"$tmp/hwp-fixed.out" 2>"$tmp/hwp-fixed.err"
hwp_fixed_rc=$?
expect_ok "fixed transaction handles an HWP driver that ignores min changes under performance" \
    test "$hwp_fixed_rc" -eq 0
expect_ok "HWP-ordered fixed transaction exposes exact bounds to the child" \
    test "$(cat "$tmp/observed-hwp-fixed" 2>/dev/null)" = \
        'performance performance 3000000 3000000'
apply_min_line=$(grep -n '^scaling_min_freq=3000000$' "$write_log" | head -1 | cut -d: -f1)
apply_gov_line=$(grep -n '^scaling_governor=performance$' "$write_log" | head -1 | cut -d: -f1)
restore_gov_line=$(grep -n '^scaling_governor=powersave$' "$write_log" | head -1 | cut -d: -f1)
restore_min_line=$(grep -n '^scaling_min_freq=800000$' "$write_log" | tail -1 | cut -d: -f1)
expect_ok "fixed apply writes the target minimum before entering performance" \
    bash -c 'test -n "$1" && test -n "$2" && test "$1" -lt "$2"' \
        _ "$apply_min_line" "$apply_gov_line"
expect_ok "fixed restore leaves performance before lowering the minimum" \
    bash -c 'test -n "$1" && test -n "$2" && test "$1" -lt "$2"' \
        _ "$restore_gov_line" "$restore_min_line"
expect_ok "HWP-ordered fixed transaction restores the complete original profile" \
    bash -c 'test "$(cat "$1")" = powersave && test "$(cat "$2")" = balance_performance && test "$(cat "$3")" = 800000 && test "$(cat "$4")" = 3800000' \
        _ "$sys/cpufreq/policy7/scaling_governor" \
        "$sys/cpufreq/policy7/energy_performance_preference" \
        "$sys/cpufreq/policy7/scaling_min_freq" \
        "$sys/cpufreq/policy7/scaling_max_freq"

reset_policy
printf '%s\n' performance >"$sys/cpufreq/policy7/scaling_governor"
printf '%s\n' performance >"$sys/cpufreq/policy7/energy_performance_preference"
printf '%s\n' 1600000 >"$sys/cpufreq/policy7/scaling_min_freq"
printf '%s\n' 3200000 >"$sys/cpufreq/policy7/scaling_max_freq"
printf '%s\n' hwp-performance-min-ignore >"$action"
OBSERVED="$tmp/observed-original-performance" run_tool --with-fixed 7 3000000 \
    --expect-policy-cpus 7 -- "$observer" \
    >"$tmp/original-performance.out" 2>"$tmp/original-performance.err"
original_performance_rc=$?
expect_ok "fixed transaction can stage bounds when the original governor is performance" \
    test "$original_performance_rc" -eq 0
expect_ok "original-performance transaction verifies fixed bounds before the child" \
    test "$(cat "$tmp/observed-original-performance" 2>/dev/null)" = \
        'performance performance 3000000 3000000'
expect_ok "original-performance transaction restores its exact governor, EPP, and bounds" \
    bash -c 'test "$(cat "$1")" = performance && test "$(cat "$2")" = performance && test "$(cat "$3")" = 1600000 && test "$(cat "$4")" = 3200000' \
        _ "$sys/cpufreq/policy7/scaling_governor" \
        "$sys/cpufreq/policy7/energy_performance_preference" \
        "$sys/cpufreq/policy7/scaling_min_freq" \
        "$sys/cpufreq/policy7/scaling_max_freq"

reset_policy
printf '%s\n' delayed-min-readback >"$action"
OBSERVED="$tmp/observed-delayed-fixed" run_tool --with-fixed 7 3000000 \
    --expect-policy-cpus 7 -- "$observer" \
    >"$tmp/delayed-fixed.out" 2>"$tmp/delayed-fixed.err"
delayed_fixed_rc=$?
# Let the fake driver's detached visibility updates settle even on the old RED
# implementation, so they cannot contaminate the following independent case.
sleep 0.08
expect_ok "fixed transaction waits for delayed HWP readback" \
    test "$delayed_fixed_rc" -eq 0
expect_ok "delayed HWP readback is verified before launching the child" \
    test "$(cat "$tmp/observed-delayed-fixed" 2>/dev/null)" = \
        'performance performance 3000000 3000000'
expect_ok "delayed HWP transaction restores exact original bounds" \
    bash -c 'test "$(cat "$1")" = 800000 && test "$(cat "$2")" = 3800000' \
        _ "$sys/cpufreq/policy7/scaling_min_freq" \
        "$sys/cpufreq/policy7/scaling_max_freq"

reset_policy
printf '%s\n' 2000000 >"$sys/cpufreq/policy7/scaling_min_freq"
run_tool --with-fixed 7 1000000 --expect-policy-cpus 7 -- true \
    >"$tmp/fixed-low.out" 2>"$tmp/fixed-low.err"
fixed_low_rc=$?
expect_ok "fixed target below current minimum succeeds through a valid ordering" \
    test "$fixed_low_rc" -eq 0
min_line=$(grep -n '^scaling_min_freq=1000000$' "$write_log" | head -1 | cut -d: -f1)
max_line=$(grep -n '^scaling_max_freq=1000000$' "$write_log" | head -1 | cut -d: -f1)
expect_ok "lower fixed target writes min before max to avoid min>max" \
    bash -c 'test "$1" -lt "$2"' _ "$min_line" "$max_line"

reset_policy
expect_fail "fixed target above cpuinfo_max is refused" \
    run_tool --with-fixed 7 3800001 --expect-policy-cpus 7 -- true
expect_ok "out-of-range target is refused before writes" test ! -s "$write_log"

reset_policy
printf '%s\n' ignore-restore-governor >"$action"
restore_mismatch_run() {
    run_tool --with-performance 7 --expect-policy-cpus 7 -- true \
        >"$tmp/restore.out" 2>"$tmp/restore.err"
}
expect_fail "a restoration readback mismatch overrides child success" \
    restore_mismatch_run
expect_ok "restoration failure is explicit" \
    grep -q 'CPUFREQ_RESTORE_FAILED' "$tmp/restore.err"

expect_ok "transaction implementation exists" test -f "$SCRIPT"
expect_ok "implementation contains no eval" \
    bash -c 'test -f "$1" && ! grep -Eq "(^|[^[:alnum:]_])eval([[:space:]]|$)" "$1"' _ "$SCRIPT"
expect_ok "implementation never writes a global intel_pstate control" \
    bash -c 'test -f "$1" && ! grep -Eq "no_turbo|min_perf_pct|max_perf_pct|hwp_dynamic_boost" "$1"' _ "$SCRIPT"
expect_ok "the complete pure test leaves no task-owned process" \
    bash -c 'test -z "$1"' _ "$(owned_test_pids)"

echo "1..$n"
exit "$failed"
