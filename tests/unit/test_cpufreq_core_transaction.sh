#!/bin/bash
# Pure fake-sysfs tests for the physical-core cpufreq transaction wrapper.
# No invocation in this file may resolve, read, or write the host's real /sys.
set -u
cd "$(dirname "$0")/../.."

SCRIPT=tools/cpufreq-core-transaction.sh
TX=tools/cpufreq-transaction.sh
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

tmp=$(mktemp -d /tmp/exitos-cpufreq-core-test.XXXXXX) || exit 1
case "$tmp" in /tmp/exitos-cpufreq-core-test.*) ;; *) exit 1 ;; esac
[ -n "$tmp" ] && [ "$tmp" != / ] && [ "$tmp" != /sys ] &&
    [[ $tmp != /sys/* ]] || exit 1
declare -a exact_helper_pids=()
owned_test_pids() {
    local proc pid command
    for proc in /proc/[0-9]*; do
        pid=${proc##*/}
        [ "$pid" != "$$" ] || continue
        command=$(tr '\0' ' ' 2>/dev/null <"$proc/cmdline") || continue
        case "$command" in
          *"$tmp/"*)
            case "$command" in
              *cpufreq-transaction.sh*|*cpufreq-core-transaction.sh*|*"$tmp/sleeper"*|*"$tmp/fake-keeper"*)
                printf '%s\n' "$pid"
                ;;
            esac
            ;;
        esac
    done
}
cleanup() {
    local pids pid
    for pid in "${exact_helper_pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    pids=$(owned_test_pids)
    [ -z "$pids" ] || kill -TERM $pids 2>/dev/null || true
    for _ in $(seq 1 100); do
        pids=$(owned_test_pids)
        [ -n "$pids" ] || break
        sleep 0.01
    done
    pids=$(owned_test_pids)
    [ -z "$pids" ] || kill -KILL $pids 2>/dev/null || true
    for pid in "${exact_helper_pids[@]}"; do
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    rm -rf -- "$tmp"
}
trap cleanup EXIT

stop_exact_helper() {
    local pid=$1
    [ -n "$pid" ] || return 0
    kill -TERM "$pid" 2>/dev/null || true
    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.01
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

sys="$tmp/sys"
locks="$tmp/locks"
state="$tmp/state"
write_log="$tmp/write.log"
write_action="$tmp/write.action"
event_log="$tmp/event.log"
child_marker="$tmp/child.marker"
argv_log="$tmp/argv.log"
slow_helper_pid_log="$tmp/slow-helper.pids"

make_policy() {
    local cpu=$1 min=$2 max=$3 epp=$4
    local policy="$sys/cpufreq/policy$cpu"
    mkdir -p "$sys/cpu$cpu/topology" "$policy"
    ln -s "../cpufreq/policy$cpu" "$sys/cpu$cpu/cpufreq"
    printf '%s\n' 1 >"$sys/cpu$cpu/online"
    printf '%s\n' '84,170' >"$sys/cpu$cpu/topology/thread_siblings_list"
    printf '%s\n' intel_pstate >"$policy/scaling_driver"
    printf '%s\n' powersave >"$policy/scaling_governor"
    printf '%s\n' 'performance powersave' >"$policy/scaling_available_governors"
    printf '%s\n' "$epp" >"$policy/energy_performance_preference"
    printf '%s\n' 'default performance balance_performance balance_power power' \
        >"$policy/energy_performance_available_preferences"
    printf '%s\n' "$min" >"$policy/scaling_min_freq"
    printf '%s\n' "$max" >"$policy/scaling_max_freq"
    printf '%s\n' 2000000 >"$policy/scaling_cur_freq"
    printf '%s\n' 800000 >"$policy/cpuinfo_min_freq"
    printf '%s\n' 3800000 >"$policy/cpuinfo_max_freq"
    printf '%s\n' "$cpu" >"$policy/affected_cpus"
    printf '%s\n' "$cpu" >"$policy/related_cpus"
}

reset_core() {
    rm -rf -- "$sys" "$locks" "$state"
    mkdir -p "$sys/cpufreq" "$locks" "$state"
    make_policy 84 800000 3800000 balance_power
    make_policy 170 900000 3700000 balance_performance
    printf '%s\n' '84,170' >"$sys/online"
    : >"$write_log"
    : >"$event_log"
    rm -f -- "$child_marker" "$argv_log" "$slow_helper_pid_log"
    printf '%s\n' normal >"$write_action"
}

writer="$tmp/fake-write"
cat >"$writer" <<'EOF'
#!/bin/bash
set -u
path=$1
value=$2
policy=${path%/*}
policy=${policy##*/}
field=${path##*/}
mode=$(cat "$FAKE_WRITE_ACTION")
printf '%s/%s=%s\n' "$policy" "$field" "$value" >>"$FAKE_WRITE_LOG"
if [ "$mode" = second-policy-setup-fail ] && [ "$policy" = policy170 ] &&
   [ "$field" = scaling_governor ] && [ "$value" = performance ]; then
    exit 19
fi
if [ "$mode" = slow-inner-restore ] && [ "$policy" = policy170 ]; then
    case "$field=$value" in
      scaling_governor=powersave|energy_performance_preference=balance_performance|scaling_min_freq=900000|scaling_max_freq=3700000)
        # Keep each individual fake sysfs readback below the transaction's
        # 80 * 2 ms test deadline, while making the complete five-write inner
        # restore exceed the outer transaction's historical two-second grace.
        sleep 0.31
        (
            sleep 0.13
            printf '%s\n' "$value" >"$path"
        ) </dev/null >/dev/null 2>&1 &
        printf '%s\n' "$!" >>"$SLOW_HELPER_PID_LOG"
        exit 0
        ;;
    esac
fi
printf '%s\n' "$value" >"$path"
EOF
chmod 0700 "$writer"

keeper="$tmp/fake-keeper"
cat >"$keeper" <<'EOF'
#!/bin/bash
set -u
[[ $# -eq 1 && $1 =~ ^[0-9]+$ ]] || exit 2
printf 'KEEPER_START cpu=%s pid=%s\n' "$1" "$$" >>"$FAKE_EVENT_LOG"
stop() {
    printf 'KEEPER_STOP cpu=%s pid=%s\n' "$1" "$$" >>"$FAKE_EVENT_LOG"
    exit 0
}
trap 'stop "$1"' INT TERM HUP
printf 'READY cpu=%s policy=SCHED_IDLE\n' "$1"
while :; do sleep 0.01; done
EOF
chmod 0700 "$keeper"

disposition_keeper="$tmp/disposition-keeper"
cat >"$disposition_keeper" <<'EOF'
#!/bin/bash
set -u
case "$(trap -p INT)" in *"''"*) exit 81 ;; esac
case "$(trap -p QUIT)" in *"''"*) exit 82 ;; esac
stop() { exit 0; }
trap stop INT QUIT TERM HUP
printf 'READY cpu=%s policy=SCHED_IDLE\n' "$1"
while :; do sleep 0.01; done
EOF
chmod 0700 "$disposition_keeper"

observer="$tmp/observer"
cat >"$observer" <<'EOF'
#!/bin/bash
set -u
printf 'CHILD_START pid=%s\n' "$$" >>"$FAKE_EVENT_LOG"
: >"$CHILD_MARKER"
for policy in policy84 policy170; do
    p="$EXITOS_CPU_SYSFS_ROOT/cpufreq/$policy"
    printf '%s %s %s %s %s\n' "$policy" \
        "$(cat "$p/scaling_governor")" \
        "$(cat "$p/energy_performance_preference")" \
        "$(cat "$p/scaling_min_freq")" \
        "$(cat "$p/scaling_max_freq")"
done
printf '<%s>\n' "$@" >"$ARGV_LOG"
exit "${OBSERVER_RC:-0}"
EOF
chmod 0700 "$observer"

sleeper="$tmp/sleeper"
cat >"$sleeper" <<'EOF'
#!/bin/bash
set -u
trap 'exit 0' TERM INT QUIT HUP
printf 'CHILD_START pid=%s\n' "$$" >>"$FAKE_EVENT_LOG"
printf '%s\n' "$$" >"$CHILD_MARKER"
while :; do sleep 0.05; done
EOF
chmod 0700 "$sleeper"

mutant_tx="$tmp/single-policy-mutant"
cat >"$mutant_tx" <<'EOF'
#!/bin/bash
set -u
printf '%s\n' "$*" >>"$MUTANT_LOG"
mode=${1:-}
cpu=${2:-}
if [ "$cpu" = 170 ]; then
    while [ "$#" -gt 0 ] && [ "$1" != -- ]; do shift; done
    [ "${1:-}" = -- ] || exit 2
    shift
    exec "$@"
fi
exec "$REAL_CPUFREQ_TX" "$@"
EOF
chmod 0700 "$mutant_tx"

# Inject the same state transition as TERM arriving after the retained
# transaction has been forked but immediately before TX_PID=$! stores its PID.
# BASH_ENV is unset before the tested script starts, so nested transactions and
# the user command cannot inherit the DEBUG hook.
signal_before_pid_env="$tmp/signal-before-pid-env"
cat >"$signal_before_pid_env" <<'EOF'
unset BASH_ENV
trap '
    if [[ $BASH_COMMAND == "TX_PID=\$!" ]]; then
        trap - DEBUG
        forward_signal 143 TERM
    fi
' DEBUG
EOF
chmod 0600 "$signal_before_pid_env"

run_core_with_tx() {
    local tx=$1
    shift
    EXITOS_CPUFREQ_CORE_TEST_MODE=1 \
    EXITOS_CPUFREQ_TEST_MODE=1 \
    EXITOS_CPU_SYSFS_ROOT="$sys" \
    EXITOS_CPUFREQ_LOCK_ROOT="$locks" \
    EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
    EXITOS_CPUFREQ_TRANSACTION_BIN="$tx" \
    EXITOS_KEEPER_BIN="$keeper" \
    EXITOS_CPUFREQ_CORE_TMPROOT="$state" \
    FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$write_action" \
    FAKE_EVENT_LOG="$event_log" CHILD_MARKER="$child_marker" \
    ARGV_LOG="$argv_log" REAL_CPUFREQ_TX="$TX" MUTANT_LOG="$tmp/mutant.log" \
    SLOW_HELPER_PID_LOG="$slow_helper_pid_log" \
        bash "$SCRIPT" "$@"
}

run_core() { run_core_with_tx "$TX" "$@"; }

profile_is_original() {
    test "$(cat "$sys/cpufreq/policy84/scaling_governor")" = powersave &&
    test "$(cat "$sys/cpufreq/policy84/energy_performance_preference")" = balance_power &&
    test "$(cat "$sys/cpufreq/policy84/scaling_min_freq")" = 800000 &&
    test "$(cat "$sys/cpufreq/policy84/scaling_max_freq")" = 3800000 &&
    test "$(cat "$sys/cpufreq/policy170/scaling_governor")" = powersave &&
    test "$(cat "$sys/cpufreq/policy170/energy_performance_preference")" = balance_performance &&
    test "$(cat "$sys/cpufreq/policy170/scaling_min_freq")" = 900000 &&
    test "$(cat "$sys/cpufreq/policy170/scaling_max_freq")" = 3700000
}

reset_core
status=$(run_core --status-core 170 2>"$tmp/status.err")
status_rc=$?
expect_ok "core status succeeds on two online SMT policies" test "$status_rc" -eq 0
expect_ok "status normalizes target SMT siblings and counts both policies" \
    grep -q '^CPUFREQ_CORE_STATUS target_cpu=170 siblings=84,170 policy_count=2$' <<<"$status"
expect_ok "status emits each canonical policy and exact related scope" \
    bash -c 'grep -q "policy=policy84 related_cpus=84" <<<"$1" && grep -q "policy=policy170 related_cpus=170" <<<"$1"' _ "$status"
expect_ok "status explains cpufreq versus C-state control boundaries" \
    grep -q '^CPUFREQ_CORE_BOUNDARY p_state_policies=all-sibling-policies core_cstate=uncontrolled package_cstate=uncontrolled' <<<"$status"
expect_ok "status performs no writes and starts no keeper" \
    bash -c 'test ! -s "$1" && test ! -s "$2"' _ "$write_log" "$event_log"

fixed_plan=$(run_core --plan-fixed-core 170 2000000 2>"$tmp/fixed-plan.err")
fixed_plan_rc=$?
expect_ok "fixed-core plan succeeds without a transaction" test "$fixed_plan_rc" -eq 0
expect_ok "fixed plan names all-policy bounds and pre-command actual verification" \
    grep -q '^CPUFREQ_CORE_REQUEST mode=fixed target_khz=2000000 tolerance_khz=20000 stable_samples=3 actual_frequency_verification=required keeper_cpu=170$' <<<"$fixed_plan"
expect_ok "fixed plan is read-only and does not preheat the CPU" \
    bash -c 'test ! -s "$1" && test ! -s "$2"' _ "$write_log" "$event_log"

perf_plan=$(run_core --plan-performance-core 170 2>"$tmp/perf-plan.err")
expect_ok "performance plan explicitly refuses to claim a fixed actual frequency" \
    grep -q '^CPUFREQ_CORE_REQUEST mode=performance .*actual_frequency_fixed=no$' <<<"$perf_plan"
expect_ok "performance plan is read-only" test ! -s "$write_log"

reset_core
fixed_out=$(run_core --with-fixed-core 170 2000000 -- "$observer" \
    'two words' '*' 'semi;colon' 2>"$tmp/fixed.err")
fixed_rc=$?
expect_ok "fixed-core transaction succeeds only after both policy and actual checks" \
    test "$fixed_rc" -eq 0
expect_ok "fixed verification reports all sibling readings and a stopped preheater" \
    grep -Eq '^CPUFREQ_CORE_VERIFY mode=fixed target_cpu=170 siblings=84,170 target_khz=2000000 tolerance_khz=20000 stable_samples=3 actual_khz=84:2000000,170:2000000 actual_frequency_verified=yes verification_scope=pre-command keeper_stopped_before_command=yes$' <<<"$fixed_out"
expect_ok "child observes fixed governor EPP and bounds on both policies" \
    bash -c 'grep -q "^policy84 performance performance 2000000 2000000$" <<<"$1" && grep -q "^policy170 performance performance 2000000 2000000$" <<<"$1"' _ "$fixed_out"
expect_ok "keeper is stopped before the first user command" \
    bash -c 'stop=$(grep -n "^KEEPER_STOP " "$1" | cut -d: -f1); child=$(grep -n "^CHILD_START " "$1" | cut -d: -f1); test -n "$stop" && test -n "$child" && test "$stop" -lt "$child"' _ "$event_log"
expect_ok "fixed wrapper preserves command argv without expansion or eval" \
    test "$(cat "$argv_log")" = $'<two words>\n<*>\n<semi;colon>'
expect_ok "fixed-core success restores every original sibling-policy profile" profile_is_original
expect_ok "outer wrapper explicitly verifies all original profiles after nested restoration" \
    grep -q '^CPUFREQ_CORE_RESTORE policies=2 original_profiles_verified=yes$' <<<"$fixed_out"

reset_core
env EXITOS_CPUFREQ_CORE_TEST_MODE=1 EXITOS_CPUFREQ_TEST_MODE=1 \
    EXITOS_CPU_SYSFS_ROOT="$sys" EXITOS_CPUFREQ_LOCK_ROOT="$locks" \
    EXITOS_CPUFREQ_WRITE_HELPER="$writer" EXITOS_CPUFREQ_TRANSACTION_BIN="$TX" \
    EXITOS_KEEPER_BIN="$disposition_keeper" EXITOS_CPUFREQ_CORE_TMPROOT="$state" \
    FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$write_action" \
    FAKE_EVENT_LOG="$event_log" CHILD_MARKER="$child_marker" ARGV_LOG="$argv_log" \
    bash "$SCRIPT" --with-fixed-core 170 2000000 -- "$observer" \
    >"$tmp/keeper-disposition.out" 2>"$tmp/keeper-disposition.err"
keeper_disposition_rc=$?
expect_ok "keeper launcher resets inherited INT and QUIT dispositions" \
    test "$keeper_disposition_rc" -eq 0
expect_ok "disposition-checked keeper permits the verified user command" \
    test -e "$child_marker"
expect_ok "disposition-checked keeper transaction restores every sibling policy" \
    profile_is_original

reset_core
printf '%s\n' 3800000 >"$sys/cpufreq/policy170/scaling_cur_freq"
run_core --with-fixed-core 170 2000000 -- "$observer" \
    >"$tmp/actual-mismatch.out" 2>"$tmp/actual-mismatch.err"
actual_mismatch_rc=$?
expect_ok "one sibling at 3.8GHz rejects a nominal 2GHz fixed profile" \
    test "$actual_mismatch_rc" -ne 0
expect_ok "actual mismatch never starts the user command" test ! -e "$child_marker"
expect_ok "actual mismatch never prints actual_frequency_verified=yes" \
    sh -c '! grep -q "actual_frequency_verified=yes" "$1"' _ "$tmp/actual-mismatch.out"
expect_ok "actual mismatch restores both original profiles" profile_is_original

reset_core
: >"$tmp/mutant.log"
run_core_with_tx "$mutant_tx" --with-fixed-core 170 2000000 -- "$observer" \
    >"$tmp/mutant.out" 2>"$tmp/mutant.err"
mutant_rc=$?
expect_ok "single-policy transaction mutant cannot pass core-wide verification" \
    test "$mutant_rc" -ne 0
expect_ok "single-policy mutant never starts the user command" test ! -e "$child_marker"
expect_ok "single-policy mutant still restores the first applied policy" profile_is_original
expect_ok "mutant exercised both requested policy layers" \
    bash -c 'grep -q -- "--with-fixed 84 " "$1" && grep -q -- "--with-fixed 170 " "$1"' _ "$tmp/mutant.log"

reset_core
printf '%s\n' second-policy-setup-fail >"$write_action"
run_core --with-fixed-core 170 2000000 -- "$observer" \
    >"$tmp/setup-fail.out" 2>"$tmp/setup-fail.err"
setup_fail_rc=$?
expect_ok "second policy setup failure rejects the core transaction" test "$setup_fail_rc" -ne 0
expect_ok "second policy setup failure never starts the user command" test ! -e "$child_marker"
expect_ok "second policy setup failure restores the already-applied first policy and itself" \
    profile_is_original

reset_core
OBSERVER_RC=23 run_core --with-fixed-core 170 2000000 -- "$observer" \
    >"$tmp/child-fail.out" 2>"$tmp/child-fail.err"
child_fail_rc=$?
expect_ok "child failure status survives nested restoration" test "$child_fail_rc" -eq 23
expect_ok "child failure restores every sibling policy" profile_is_original

reset_core
BASH_ENV="$signal_before_pid_env" \
EXITOS_CPUFREQ_CORE_TEST_MODE=1 \
EXITOS_CPUFREQ_TEST_MODE=1 \
EXITOS_CPU_SYSFS_ROOT="$sys" \
EXITOS_CPUFREQ_LOCK_ROOT="$locks" \
EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
EXITOS_CPUFREQ_TRANSACTION_BIN="$TX" \
EXITOS_KEEPER_BIN="$keeper" \
EXITOS_CPUFREQ_CORE_TMPROOT="$state" \
FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$write_action" \
FAKE_EVENT_LOG="$event_log" CHILD_MARKER="$child_marker" \
ARGV_LOG="$argv_log" REAL_CPUFREQ_TX="$TX" MUTANT_LOG="$tmp/mutant.log" \
    bash "$SCRIPT" --with-fixed-core 170 2000000 -- "$observer" \
    >"$tmp/signal-before-pid.out" 2>"$tmp/signal-before-pid.err"
signal_before_pid_rc=$?
expect_ok "TERM in the retained-PID assignment window returns conventional status" \
    test "$signal_before_pid_rc" -eq 143
expect_ok "TERM in the retained-PID assignment window never starts the user command" \
    test ! -e "$child_marker"
expect_ok "TERM in the retained-PID assignment window restores every sibling policy" \
    profile_is_original
expect_ok "retained-PID assignment-window TERM leaves no task-owned process" \
    bash -c 'test -z "$1"' _ "$(owned_test_pids)"

reset_core
EXITOS_CPUFREQ_CORE_TEST_MODE=1 \
EXITOS_CPUFREQ_TEST_MODE=1 \
EXITOS_CPU_SYSFS_ROOT="$sys" \
EXITOS_CPUFREQ_LOCK_ROOT="$locks" \
EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
EXITOS_CPUFREQ_TRANSACTION_BIN="$TX" \
EXITOS_KEEPER_BIN="$keeper" \
EXITOS_CPUFREQ_CORE_TMPROOT="$state" \
FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$write_action" \
FAKE_EVENT_LOG="$event_log" CHILD_MARKER="$child_marker" \
ARGV_LOG="$argv_log" REAL_CPUFREQ_TX="$TX" MUTANT_LOG="$tmp/mutant.log" \
    bash "$SCRIPT" --with-fixed-core 170 2000000 -- "$sleeper" \
    >"$tmp/signal.out" 2>"$tmp/signal.err" & wrapper_pid=$!
for ((i = 0; i < 300; i++)); do
    [ ! -s "$child_marker" ] || break
    kill -0 "$wrapper_pid" 2>/dev/null || break
    sleep 0.01
done
expect_ok "signal test reaches the child only after fixed-core verification" test -s "$child_marker"
if kill -0 "$wrapper_pid" 2>/dev/null; then kill -TERM "$wrapper_pid"; fi
wait "$wrapper_pid"
signal_rc=$?
expect_ok "TERM returns conventional status after nested restoration" test "$signal_rc" -eq 143
expect_ok "TERM restores every sibling policy" profile_is_original
if [ -s "$child_marker" ]; then signal_child=$(cat "$child_marker"); else signal_child=0; fi
expect_fail "TERM leaves no user child running" \
    bash -c 'kill -0 "$1" 2>/dev/null' _ "$signal_child"
expect_ok "TERM leaves no wrapper, nested transaction, keeper, or child orphan" \
    bash -c 'test -z "$1"' _ "$(owned_test_pids)"

# Regression: an outer per-policy transaction must not apply the ordinary
# two-second benchmark-child escalation deadline to a trusted inner transaction
# that is still completing its own bounded restoration.  Every delayed fake
# readback below remains individually legal (130 ms < 160 ms test deadline).
reset_core
EXITOS_CPUFREQ_CORE_TEST_MODE=1 \
EXITOS_CPUFREQ_TEST_MODE=1 \
EXITOS_CPU_SYSFS_ROOT="$sys" \
EXITOS_CPUFREQ_LOCK_ROOT="$locks" \
EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
EXITOS_CPUFREQ_TRANSACTION_BIN="$TX" \
EXITOS_KEEPER_BIN="$keeper" \
EXITOS_CPUFREQ_CORE_TMPROOT="$state" \
FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$write_action" \
FAKE_EVENT_LOG="$event_log" CHILD_MARKER="$child_marker" \
ARGV_LOG="$argv_log" REAL_CPUFREQ_TX="$TX" MUTANT_LOG="$tmp/mutant.log" \
SLOW_HELPER_PID_LOG="$slow_helper_pid_log" \
    bash "$SCRIPT" --with-fixed-core 170 2000000 -- "$sleeper" \
    >"$tmp/slow-restore-signal.out" 2>"$tmp/slow-restore-signal.err" &
slow_wrapper_pid=$!
for ((i = 0; i < 300; i++)); do
    [ ! -s "$child_marker" ] || break
    kill -0 "$slow_wrapper_pid" 2>/dev/null || break
    sleep 0.01
done
expect_ok "slow-restore signal test reaches the verified user child" test -s "$child_marker"
printf '%s\n' slow-inner-restore >"$write_action"
(
    sleep 8
    : >"$tmp/slow-restore-signal.timed-out"
    kill -KILL "$slow_wrapper_pid" 2>/dev/null || true
) &
slow_watchdog_pid=$!
exact_helper_pids+=("$slow_watchdog_pid")
kill -TERM "$slow_wrapper_pid"
if wait "$slow_wrapper_pid" 2>/dev/null; then slow_wrapper_rc=0; else slow_wrapper_rc=$?; fi
kill -TERM "$slow_watchdog_pid" 2>/dev/null || true
wait "$slow_watchdog_pid" 2>/dev/null || true
exact_helper_pids=()
for ((i = 0; i < 200; i++)); do
    slow_helpers_alive=0
    if [ -s "$slow_helper_pid_log" ]; then
        while IFS= read -r helper_pid; do
            kill -0 "$helper_pid" 2>/dev/null && slow_helpers_alive=1
        done <"$slow_helper_pid_log"
    fi
    [ "$slow_helpers_alive" -eq 1 ] || break
    sleep 0.01
done
expect_ok "trusted slow inner restore finishes within its explicit outer deadline" \
    test ! -e "$tmp/slow-restore-signal.timed-out"
expect_ok "TERM survives trusted slow nested restoration as exit 143" \
    test "$slow_wrapper_rc" -eq 143
expect_ok "trusted slow nested TERM restores all four fields on policies 84 and 170" \
    profile_is_original
expect_ok "trusted slow nested TERM exercised all five bounded inner restore writes" \
    test "$(wc -l <"$slow_helper_pid_log" 2>/dev/null || printf 0)" -eq 5
expect_ok "trusted slow nested TERM leaves no delayed readback helper" \
    test "$slow_helpers_alive" -eq 0
expect_ok "trusted slow nested TERM leaves no wrapper, transaction, keeper, or child orphan" \
    bash -c 'test -z "$1"' _ "$(owned_test_pids)"

run_real_core_signal_case() {
    local sig=$1 expected_rc=$2 case_name timeout_file wrapper_pid watchdog_pid
    local wrapper_rc child_pid=0 pids
    case_name=${sig,,}
    timeout_file="$tmp/core-${case_name}-timed-out"
    reset_core
    rm -f -- "$timeout_file"
    env --default-signal=INT,QUIT,HUP,TERM \
        EXITOS_CPUFREQ_CORE_TEST_MODE=1 \
        EXITOS_CPUFREQ_TEST_MODE=1 \
        EXITOS_CPU_SYSFS_ROOT="$sys" \
        EXITOS_CPUFREQ_LOCK_ROOT="$locks" \
        EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
        EXITOS_CPUFREQ_TRANSACTION_BIN="$TX" \
        EXITOS_KEEPER_BIN="$keeper" \
        EXITOS_CPUFREQ_CORE_TMPROOT="$state" \
        FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$write_action" \
        FAKE_EVENT_LOG="$event_log" CHILD_MARKER="$child_marker" \
        ARGV_LOG="$argv_log" REAL_CPUFREQ_TX="$TX" MUTANT_LOG="$tmp/mutant.log" \
        bash "$SCRIPT" --with-fixed-core 170 2000000 -- "$sleeper" \
        >"$tmp/core-${case_name}.out" 2>"$tmp/core-${case_name}.err" &
    wrapper_pid=$!
    for ((i = 0; i < 300; i++)); do
        [ ! -s "$child_marker" ] || break
        kill -0 "$wrapper_pid" 2>/dev/null || break
        sleep 0.01
    done
    expect_ok "core $sig reaches the child only after verification" test -s "$child_marker"
    (
        sleep 1
        : >"$timeout_file"
        pids=$(owned_test_pids)
        [ -z "$pids" ] || kill -KILL $pids 2>/dev/null || true
    ) &
    watchdog_pid=$!
    kill -"$sig" "$wrapper_pid"
    if wait "$wrapper_pid" 2>/dev/null; then wrapper_rc=0; else wrapper_rc=$?; fi
    kill -TERM "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
    expect_ok "core $sig returns within one second without escalation" test ! -e "$timeout_file"
    expect_ok "core $sig is reported as exit $expected_rc" test "$wrapper_rc" -eq "$expected_rc"
    expect_ok "core $sig restores every sibling policy" profile_is_original
    if [ -s "$child_marker" ]; then child_pid=$(<"$child_marker"); fi
    expect_fail "core $sig leaves no user process group" \
        bash -c 'kill -0 -- "-$1" 2>/dev/null' _ "$child_pid"
    expect_ok "core $sig leaves no wrapper, transaction, keeper, or child" \
        bash -c 'test -z "$1"' _ "$(owned_test_pids)"
    expect_ok "core $sig leaves no verifier temporary directory" \
        bash -c 'test -z "$(find "$1" -mindepth 1 -maxdepth 1 -print -quit)"' _ "$state"
}

run_real_core_signal_case INT 130
run_real_core_signal_case QUIT 131

reset_core
run_core --with-performance-core 170 -- "$observer" \
    >"$tmp/perf-child.out" 2>"$tmp/perf.err"
perf_rc=$?
perf_out=$(cat "$tmp/perf-child.out")
expect_ok "performance-core transaction succeeds across both sibling policies" test "$perf_rc" -eq 0
expect_ok "performance-core verifies controls but does not claim actual frequency fixed" \
    grep -q '^CPUFREQ_CORE_VERIFY mode=performance .*control_profiles_verified=yes actual_frequency_fixed=no keeper_started=no$' <<<"$perf_out"
expect_ok "performance-core does not invoke the verifier keeper" \
    sh -c '! grep -q "^KEEPER_" "$1"' _ "$event_log"
expect_ok "performance-core restores every original profile" profile_is_original

# Topology and policy-scope parsers fail closed before invoking either writer or keeper.
reset_core
printf '%s\n' '84,84,170' >"$sys/cpu170/topology/thread_siblings_list"
expect_fail "duplicate SMT CPU is rejected" run_core --status-core 170
expect_ok "duplicate SMT rejection is read-only" test ! -s "$write_log"

reset_core
printf '%s\n' 170 >"$sys/online"
expect_fail "offline SMT sibling is rejected" run_core --plan-fixed-core 170 2000000

reset_core
printf '%s\n' '84,1024' >"$sys/cpu170/topology/thread_siblings_list"
expect_fail "out-of-bound SMT CPU is rejected" run_core --status-core 170

reset_core
rm -f -- "$sys/cpu84/cpufreq"
expect_fail "missing sibling canonical cpufreq policy is rejected" run_core --status-core 170

reset_core
printf '%s\n' '84 170' >"$sys/cpufreq/policy84/related_cpus"
printf '%s\n' '170' >"$sys/cpufreq/policy170/related_cpus"
expect_fail "overlapping distinct cpufreq policy scopes are rejected" run_core --status-core 170

reset_core
printf '%s\n' '84 84' >"$sys/cpufreq/policy84/related_cpus"
expect_fail "duplicate CPU inside related_cpus is rejected" run_core --status-core 170

reset_core
printf '%s\n' 84 >"$sys/cpufreq/policy170/related_cpus"
expect_fail "a sibling policy whose exact scope omits that sibling is rejected" \
    run_core --status-core 170

huge=$(printf '9%.0s' {1..100})
expect_fail "overlong target CPU is rejected before integer conversion" \
    run_core --status-core "$huge"
expect_fail "overlong target frequency is rejected before integer conversion" \
    run_core --plan-fixed-core 170 "$huge"

real_sys_alias="$tmp/real-sys-alias"
ln -s /sys/devices/system/cpu "$real_sys_alias"
expect_fail "pure test mode rejects a root resolving into real sysfs" \
    env EXITOS_CPUFREQ_CORE_TEST_MODE=1 EXITOS_CPU_SYSFS_ROOT="$real_sys_alias" \
        EXITOS_CPUFREQ_TRANSACTION_BIN="$TX" EXITOS_KEEPER_BIN="$keeper" \
        bash "$SCRIPT" --status-core 170
reset_core
expect_fail "pure test mode rejects a temporary root resolving into real sysfs" \
    env EXITOS_CPUFREQ_CORE_TEST_MODE=1 EXITOS_CPU_SYSFS_ROOT="$sys" \
        EXITOS_CPUFREQ_TRANSACTION_BIN="$TX" EXITOS_KEEPER_BIN="$keeper" \
        EXITOS_CPUFREQ_CORE_TMPROOT="$real_sys_alias" \
        bash "$SCRIPT" --plan-fixed-core 170 2000000

entry_alias_root="$tmp/entry-alias"
entry_alias_marker="$tmp/entry-alias-sentinel-ran"
mkdir -p "$entry_alias_root/tools"
ln -s "$(readlink -f -- "$SCRIPT")" \
    "$entry_alias_root/tools/cpufreq-core-transaction.sh"
cat >"$entry_alias_root/tools/cpufreq-transaction.sh" <<'EOF'
#!/bin/bash
: >"$ENTRY_ALIAS_MARKER"
exit 97
EOF
cat >"$entry_alias_root/tools/cstate-keeper" <<'EOF'
#!/bin/bash
: >"$ENTRY_ALIAS_MARKER"
exit 98
EOF
chmod 0700 "$entry_alias_root/tools/cpufreq-transaction.sh" \
    "$entry_alias_root/tools/cstate-keeper"
reset_core
env EXITOS_CPUFREQ_CORE_TEST_MODE=1 EXITOS_CPUFREQ_TEST_MODE=1 \
    EXITOS_CPU_SYSFS_ROOT="$sys" EXITOS_CPUFREQ_LOCK_ROOT="$locks" \
    EXITOS_CPUFREQ_WRITE_HELPER="$writer" EXITOS_CPUFREQ_CORE_TMPROOT="$state" \
    FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$write_action" \
    FAKE_EVENT_LOG="$event_log" CHILD_MARKER="$child_marker" ARGV_LOG="$argv_log" \
    ENTRY_ALIAS_MARKER="$entry_alias_marker" \
    bash "$entry_alias_root/tools/cpufreq-core-transaction.sh" \
        --with-performance-core 170 -- "$observer" \
        >"$tmp/entry-alias.out" 2>"$tmp/entry-alias.err"
entry_alias_rc=$?
expect_ok "entry symlink resolves defaults from the canonical trusted project root" \
    test "$entry_alias_rc" -eq 0
expect_ok "entry symlink never executes dependency sentinels beside the symlink" \
    test ! -e "$entry_alias_marker"
expect_ok "entry symlink transaction restores every sibling policy" profile_is_original
expect_ok "entry symlink transaction leaves no task-owned process" \
    bash -c 'test -z "$1"' _ "$(owned_test_pids)"

run_atomic_dependency_replacement_case() {
    local kind=$1 case_root case_self case_tx case_keeper replacement target
    local marker connected release feeder_pid wrapper_pid watchdog_pid timeout_file
    local wrapper_rc old_id new_id pids
    case_root="$tmp/atomic-$kind"
    case_self="$case_root/tools/cpufreq-core-transaction.sh"
    case_tx="$case_root/tools/cpufreq-transaction.sh"
    case_keeper="$case_root/tools/cstate-keeper"
    replacement="$case_root/replacement"
    marker="$case_root/replacement-sentinel-ran"
    connected="$case_root/initial-check-complete"
    release="$case_root/release-online"
    timeout_file="$case_root/wrapper-timed-out"
    rm -rf -- "$case_root"
    mkdir -p "$case_root/tools"
    cp -- "$SCRIPT" "$case_self"
    cp -- "$TX" "$case_tx"
    cp -- "$keeper" "$case_keeper"
    chmod 0700 "$case_self" "$case_tx" "$case_keeper"
    cat >"$replacement" <<'EOF'
#!/bin/bash
printf '%s\n' "$0" >>"$ATOMIC_SENTINEL_MARKER"
exit 97
EOF
    chmod 0700 "$replacement"
    case "$kind" in
      self) target=$case_self ;;
      tx) target=$case_tx ;;
      keeper) target=$case_keeper ;;
      *) return 2 ;;
    esac
    old_id=$(stat -Lc '%d:%i' -- "$target")

    reset_core
    rm -f -- "$sys/online"
    mkfifo -m 0600 "$sys/online"
    (
        trap 'exit 0' TERM HUP
        exec 8>"$sys/online"
        : >"$connected"
        while [ ! -e "$release" ]; do sleep 0.002; done
        printf '%s\n' '84,170' >&8
        exec 8>&-
    ) &
    feeder_pid=$!
    exact_helper_pids=("$feeder_pid")

    env --default-signal=INT,QUIT,HUP,TERM \
        EXITOS_CPUFREQ_CORE_TEST_MODE=1 EXITOS_CPUFREQ_TEST_MODE=1 \
        EXITOS_CPU_SYSFS_ROOT="$sys" EXITOS_CPUFREQ_LOCK_ROOT="$locks" \
        EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
        EXITOS_CPUFREQ_TRANSACTION_BIN="$case_tx" \
        EXITOS_KEEPER_BIN="$case_keeper" EXITOS_CPUFREQ_CORE_TMPROOT="$state" \
        FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$write_action" \
        FAKE_EVENT_LOG="$event_log" CHILD_MARKER="$child_marker" ARGV_LOG="$argv_log" \
        ATOMIC_SENTINEL_MARKER="$marker" \
        bash "$case_self" --with-fixed-core 170 2000000 -- "$observer" \
        >"$case_root/wrapper.out" 2>"$case_root/wrapper.err" &
    wrapper_pid=$!
    for ((i = 0; i < 500; i++)); do
        [ ! -e "$connected" ] || break
        kill -0 "$wrapper_pid" 2>/dev/null || break
        sleep 0.002
    done
    expect_ok "$kind replacement gate proves initial dependency checks completed" \
        test -e "$connected"
    # The outer resolver retains the now-unlinked FIFO descriptor.  Recreate
    # the pathname as an ordinary fake-sysfs file for the internal re-entry.
    rm -f -- "$sys/online"
    printf '%s\n' '84,170' >"$sys/online"
    mv -f -- "$replacement" "$target"
    new_id=$(stat -Lc '%d:%i' -- "$target")
    expect_ok "$kind replacement installs a distinct inode atomically" \
        test "$old_id" != "$new_id"
    : >"$release"
    (
        sleep 5
        : >"$timeout_file"
        pids=$(owned_test_pids)
        [ -z "$pids" ] || kill -KILL $pids 2>/dev/null || true
    ) &
    watchdog_pid=$!
    exact_helper_pids+=("$watchdog_pid")
    if wait "$wrapper_pid" 2>/dev/null; then wrapper_rc=0; else wrapper_rc=$?; fi
    kill -TERM "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
    stop_exact_helper "$feeder_pid"
    exact_helper_pids=()

    expect_ok "$kind replacement transaction completes through retained original handles" \
        bash -c 'test "$1" -eq 0 && test ! -e "$2"' _ "$wrapper_rc" "$timeout_file"
    expect_ok "$kind replacement never executes the replacement inode" test ! -e "$marker"
    expect_ok "$kind replacement still starts the verified user command" test -e "$child_marker"
    expect_ok "$kind replacement restores every sibling policy" profile_is_original
    expect_ok "$kind replacement leaves no task-owned process" \
        bash -c 'test -z "$1"' _ "$(owned_test_pids)"
    expect_ok "$kind replacement leaves no verifier temporary directory" \
        bash -c 'test -z "$(find "$1" -mindepth 1 -maxdepth 1 -print -quit)"' _ "$state"
}

run_atomic_dependency_replacement_case self
run_atomic_dependency_replacement_case tx
run_atomic_dependency_replacement_case keeper

expect_ok "physical-core transaction implementation exists" test -f "$SCRIPT"
expect_ok "implementation contains no eval" \
    bash -c 'test -f "$1" && ! grep -Eq "(^|[^[:alnum:]_])eval([[:space:]]|$)" "$1"' _ "$SCRIPT"
expect_ok "implementation never changes global turbo or intel_pstate percentages" \
    bash -c 'test -f "$1" && ! grep -Eq "no_turbo|min_perf_pct|max_perf_pct|hwp_dynamic_boost" "$1"' _ "$SCRIPT"
expect_ok "the complete pure test leaves no task-owned process" \
    bash -c 'test -z "$1"' _ "$(owned_test_pids)"

echo "1..$n"
exit "$failed"
