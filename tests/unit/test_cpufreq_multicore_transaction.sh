#!/bin/bash
# Pure fake-sysfs tests for the atomic multi-core cpufreq transaction.
# This test must never resolve, read, or write the host's real /sys.
set -u
set -o pipefail
set -f
cd "$(dirname "$0")/../.."

SCRIPT=${EXITOS_CPUFREQ_MULTI_SCRIPT_UNDER_TEST:-tools/cpufreq-multicore-transaction.sh}
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

tmp=$(mktemp -d /tmp/exitos-cpufreq-multicore-test.XXXXXX) || exit 1
case "$tmp" in /tmp/exitos-cpufreq-multicore-test.*) ;; *) exit 1 ;; esac
[ -n "$tmp" ] && [ "$tmp" != / ] && [ "$tmp" != /sys ] &&
    [[ $tmp != /sys/* ]] || exit 1

owned_test_pids() {
    local proc pid command
    for proc in /proc/[0-9]*; do
        pid=${proc##*/}
        [ "$pid" != "$$" ] || continue
        command=$(tr '\0' ' ' 2>/dev/null <"$proc/cmdline") || continue
        case "$command" in
          *"$tmp/"*) printf '%s\n' "$pid" ;;
        esac
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
write_action="$tmp/write.action"
event_log="$tmp/event.log"
lock_order_log="$tmp/lock-order.log"
lock_probe_log="$tmp/lock-probe.log"
keeper_state="$tmp/keeper-state"
child_marker="$tmp/child.marker"
child_pid_file="$tmp/child.pid"
argv_log="$tmp/argv.log"
attest_log="$tmp/attest.log"
apply_marker="$tmp/apply.marker"
attest_reread_marker="$tmp/attest-reread.marker"
offline_online_arm="$tmp/offline-online.arm"
offline_online_marker="$tmp/offline-online.marker"

make_cpu() {
    local cpu=$1 siblings=$2 policy=$3
    mkdir -p "$sys/cpu$cpu/topology"
    printf '%s\n' 1 >"$sys/cpu$cpu/online"
    printf '%s\n' "$siblings" >"$sys/cpu$cpu/topology/thread_siblings_list"
    ln -s "../cpufreq/$policy" "$sys/cpu$cpu/cpufreq"
}

make_policy() {
    local name=$1 related=$2 governor=$3 epp=$4 min=$5 max=$6
    local policy="$sys/cpufreq/$name"
    mkdir -p "$policy"
    printf '%s\n' intel_pstate >"$policy/scaling_driver"
    printf '%s\n' "$governor" >"$policy/scaling_governor"
    printf '%s\n' 'performance powersave schedutil' >"$policy/scaling_available_governors"
    printf '%s\n' "$epp" >"$policy/energy_performance_preference"
    printf '%s\n' 'default performance balance_performance balance_power power' \
        >"$policy/energy_performance_available_preferences"
    printf '%s\n' "$min" >"$policy/scaling_min_freq"
    printf '%s\n' "$max" >"$policy/scaling_max_freq"
    printf '%s\n' 3800000 >"$policy/scaling_cur_freq"
    printf '%s\n' 800000 >"$policy/cpuinfo_min_freq"
    printf '%s\n' 3800000 >"$policy/cpuinfo_max_freq"
    printf '%s\n' "$related" >"$policy/affected_cpus"
    printf '%s\n' "$related" >"$policy/related_cpus"
}

reset_machine() {
    rm -rf -- "$sys" "$locks" "$keeper_state"
    mkdir -p "$sys/cpufreq" "$locks" "$keeper_state"
    chmod 0700 "$locks"
    make_policy policy2 '10 2' powersave balance_power 800000 3000000
    make_policy policy8 8 schedutil balance_performance 900000 3200000
    make_policy policy16 16 performance performance 1000000 3400000
    make_cpu 2 '10,2' policy2
    make_cpu 10 '2,10' policy2
    make_cpu 8 '8,16' policy8
    make_cpu 16 '16,8' policy16
    printf '%s\n' '2,8,10,16' >"$sys/online"
    : >"$write_log"
    : >"$event_log"
    : >"$lock_order_log"
    : >"$lock_probe_log"
    : >"$attest_log"
    printf '%s\n' normal >"$write_action"
    rm -f -- "$child_marker" "$child_pid_file" "$argv_log" "$apply_marker" \
        "$attest_reread_marker" "$offline_online_arm" "$offline_online_marker"
}

writer="$tmp/fake-write"
after_lock_noop="$tmp/after-lock-noop"
after_lock_profile="$tmp/after-lock-profile"
after_lock_topology="$tmp/after-lock-topology"
after_lock_arm_online="$tmp/after-lock-arm-online"
observer="$tmp/observer"
sleeper="$tmp/sleeper"

# Helpers are generated once so the tested wrapper can retain their executable
# identity. Their behavior is selected only through private fake-test state.
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'path=$1' \
    'value=$2' \
    'policy=${path%/*}; policy=${policy##*/}' \
    'field=${path##*/}' \
    'mode=$(<"$FAKE_WRITE_ACTION")' \
    'printf "%s/%s=%s\n" "$policy" "$field" "$value" >>"$FAKE_WRITE_LOG"' \
    'if [ "$mode" = fail-policy8-apply ] && [ "$policy/$field/$value" = policy8/energy_performance_preference/performance ]; then exit 19; fi' \
    'if [ "$mode" = slow-policy8-apply ] && [ "$policy/$field/$value" = policy8/scaling_governor/performance ]; then : >"$APPLY_MARKER"; sleep 0.2; fi' \
    'if [ "$mode" = ignore-policy8-restore ] && [ "$policy/$field/$value" = policy8/scaling_governor/schedutil ]; then exit 0; fi' \
    'if [ "$mode" = probe-locks-during-restore ] && [ "$policy/$field/$value" = policy8/scaling_governor/schedutil ]; then' \
    '  for inherited in /proc/self/fd/*; do target=$(readlink "$inherited" 2>/dev/null || true); case "$target" in "$EXITOS_CPUFREQ_LOCK_ROOT"/exitos-cpufreq/*.lock) inherited_fd=${inherited##*/}; exec {inherited_fd}>&- ;; esac; done' \
    '  for checked in policy2 policy8 policy16; do exec {probe_fd}>"$EXITOS_CPUFREQ_LOCK_ROOT/exitos-cpufreq/$checked.lock" || exit 60; if "$REAL_FLOCK" -n "$probe_fd"; then state=unlocked; else state=held; fi; printf "%s=%s\n" "$checked" "$state" >>"$FAKE_LOCK_PROBE_LOG"; exec {probe_fd}>&-; done' \
    'fi' \
    'printf "%s\n" "$value" >"$path"' \
    'if [ "$mode" = drift-cpuinfo-max-after-apply ] && [ "$policy/$field/$value" = policy16/energy_performance_preference/performance ]; then printf "%s\n" 3799000 >"$FAKE_CPUINFO_DRIFT_PATH"; fi' \
    >"$writer"
chmod 0700 "$writer"

keeper="$tmp/fake-keeper"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'cpu=$1' \
    'mode=${KEEPER_ACTION:-normal}' \
    'if [ "$mode" != lie-about-sched ]; then /usr/bin/chrt --idle --pid 0 $$ || exit 45; fi' \
    'touch "$KEEPER_STATE/started.$cpu"' \
    'printf "KEEPER_START cpu=%s pid=%s\n" "$cpu" "$$" >>"$FAKE_EVENT_LOG"' \
    'stop() { if [ "$mode" = drop-frequency-on-stop ] && [ "$cpu" = 8 ]; then printf "%s\n" 3300000 >"$KEEPER_DROP_FREQ_PATH"; fi; rm -f -- "$KEEPER_STATE/started.$cpu"; printf "KEEPER_STOP cpu=%s pid=%s\n" "$cpu" "$$" >>"$FAKE_EVENT_LOG"; exit 0; }' \
    'if [ "$mode" = stubborn ]; then trap "" INT QUIT TERM HUP; else trap stop INT QUIT TERM HUP; fi' \
    'for _ in $(seq 1 200); do count=$(find "$KEEPER_STATE" -maxdepth 1 -type f -name "started.*" | wc -l); [ "$count" -eq "$EXPECTED_KEEPERS" ] && break; sleep 0.005; done' \
    '[ "${count:-0}" -eq "$EXPECTED_KEEPERS" ] || exit 44' \
    'printf "READY cpu=%s policy=SCHED_IDLE\n" "$cpu"' \
    'while :; do sleep 0.02; done' >"$keeper"
chmod 0700 "$keeper"

real_flock=$(command -v flock)
fake_flock="$tmp/fake-flock"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'fd=${!#}' \
    'target=$(readlink "/proc/self/fd/$fd") || exit 90' \
    'base=${target##*/}; base=${base%.lock}' \
    'printf "%s\n" "$base" >>"$FAKE_LOCK_ORDER_LOG"' \
    'exec "$REAL_FLOCK" "$@"' >"$fake_flock"
chmod 0700 "$fake_flock"

printf '%s\n' '#!/bin/bash' 'exit 0' >"$after_lock_noop"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" ondemand >"$EXITOS_CPU_SYSFS_ROOT/cpufreq/policy8/scaling_governor"' \
    'printf "%s\n" "performance powersave schedutil ondemand" >"$EXITOS_CPU_SYSFS_ROOT/cpufreq/policy8/scaling_available_governors"' \
    >"$after_lock_profile"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" "16,20" >"$EXITOS_CPU_SYSFS_ROOT/cpufreq/policy16/related_cpus"' \
    >"$after_lock_topology"
printf '%s\n' '#!/bin/bash' ': >"$OFFLINE_ONLINE_ARM"' >"$after_lock_arm_online"
chmod 0700 "$after_lock_noop" "$after_lock_profile" "$after_lock_topology" \
    "$after_lock_arm_online"

printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'for policy in policy2 policy8 policy16; do' \
    '  p="$EXITOS_CPU_SYSFS_ROOT/cpufreq/$policy"' \
    '  [ "$(<"$p/scaling_governor")" = performance ] || exit 21' \
    '  [ "$(<"$p/energy_performance_preference")" = performance ] || exit 22' \
    '  [ "$(<"$p/scaling_min_freq")" = 3800000 ] || exit 23' \
    '  [ "$(<"$p/scaling_max_freq")" = 3800000 ] || exit 24' \
    'done' \
    'fd=${EXITOS_CPUFREQ_ATTEST_FD:-}' \
    'token=${EXITOS_CPUFREQ_ATTEST_TOKEN:-}' \
    '[[ $fd =~ ^[0-9]+$ && $token =~ ^[0-9a-f]{32}$ ]] || exit 25' \
    '[ -r "/proc/self/fd/$fd" ] || exit 26' \
    'meta=$(stat -Lc "%a:%u:%h" "/proc/self/fd/$fd") || exit 27' \
    '[ "$meta" = "400:$(id -u):0" ] || exit 28' \
    'case "$(readlink "/proc/self/fd/$fd")" in *" (deleted)") ;; *) exit 29 ;; esac' \
    'attest=$(<"/proc/self/fd/$fd")' \
    'attest_again=$(<"/proc/self/fd/$fd")' \
    '[ "$attest_again" = "$attest" ] || exit 37' \
    'if printf tamper >&"$fd" 2>/dev/null; then exit 38; fi' \
    'grep -q "^EXITOS_CPUFREQ_ATTEST version=1 token=$token requested_cpus=8,2 canonical_selected_cpus=2,8 logical_cpus=2,8,10,16 policy_count=3 target_khz=3800000 control_profile_verified=yes actual_frequency_verified=post-keeper-pre-fork locks_held=yes$" <<<"$attest" || exit 30' \
    'grep -Eq "^EXITOS_CPUFREQ_ATTEST_POLICY index=1 path=.*/policy2 identity=[0-9]+:[0-9]+ related_cpus=2,10 cpuinfo_max_khz=3800000 governor=performance epp=performance min_khz=3800000 max_khz=3800000$" <<<"$attest" || exit 31' \
    'grep -Eq "^EXITOS_CPUFREQ_ATTEST_POLICY index=2 path=.*/policy8 identity=[0-9]+:[0-9]+ related_cpus=8 cpuinfo_max_khz=3800000 governor=performance epp=performance min_khz=3800000 max_khz=3800000$" <<<"$attest" || exit 32' \
    'grep -Eq "^EXITOS_CPUFREQ_ATTEST_POLICY index=3 path=.*/policy16 identity=[0-9]+:[0-9]+ related_cpus=16 cpuinfo_max_khz=3800000 governor=performance epp=performance min_khz=3800000 max_khz=3800000$" <<<"$attest" || exit 33' \
    'grep -q "^EXITOS_CPUFREQ_ATTEST_ACTUAL logical_cpus=2:3800000,8:3800000,10:3800000,16:3800000 tolerance_khz=38000 stable_samples=3 loaded_khz=2:3800000,8:3800000,10:3800000,16:3800000 concurrent_keeper_verification=yes keepers_stopped_before_final=yes verification_scope=post-keeper-pre-fork$" <<<"$attest" || exit 34' \
    'printf "%s\n" "$attest" >"$ATTEST_LOG"' \
    'for inherited in /proc/self/fd/*; do case "$(readlink "$inherited" 2>/dev/null || true)" in *"/exitos-cpufreq/"*.lock|*"/fake-keeper"|*"/fake-write"|*"/fake-flock"|*"/after-lock-"*) exit 39 ;; esac; done' \
    'for policy in policy2 policy8 policy16; do exec 8>"$EXITOS_CPUFREQ_LOCK_ROOT/exitos-cpufreq/$policy.lock"; if flock -n 8; then exit 35; fi; exec 8>&-; done' \
    'chrt -p $$ | grep -q "SCHED_OTHER" || exit 36' \
    'printf "CHILD_START pid=%s\n" "$$" >>"$FAKE_EVENT_LOG"' \
    ': >"$CHILD_MARKER"' \
    'printf "<%s>\n" "$@" >"$ARGV_LOG"' \
    'exit "${OBSERVER_RC:-0}"' >"$observer"
chmod 0700 "$observer"

printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'trap "" INT QUIT TERM HUP' \
    'printf "%s\n" "$$" >"$CHILD_PID_FILE"' \
    ': >"$CHILD_MARKER"' \
    '(trap "" INT QUIT TERM HUP; while :; do sleep 1; done) &' \
    'while :; do sleep 1; done' >"$sleeper"
chmod 0700 "$sleeper"

run_multi_after() {
    local after=$1
    shift
    EXITOS_CPUFREQ_MULTI_TEST_MODE=1 \
    EXITOS_CPU_SYSFS_ROOT="$sys" \
    EXITOS_CPUFREQ_LOCK_ROOT="$locks" \
    EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
    EXITOS_CPUFREQ_MULTI_FLOCK_BIN="$fake_flock" \
    EXITOS_CPUFREQ_MULTI_AFTER_LOCK_HELPER="$after" \
    EXITOS_KEEPER_BIN="$keeper" \
    FAKE_WRITE_LOG="$write_log" FAKE_WRITE_ACTION="$write_action" \
    FAKE_LOCK_ORDER_LOG="$lock_order_log" REAL_FLOCK="$real_flock" \
    FAKE_LOCK_PROBE_LOG="$lock_probe_log" \
    FAKE_EVENT_LOG="$event_log" KEEPER_STATE="$keeper_state" \
    EXPECTED_KEEPERS="${EXPECTED_KEEPERS_OVERRIDE:-4}" \
    KEEPER_ACTION="${KEEPER_ACTION:-normal}" \
    KEEPER_DROP_FREQ_PATH="$sys/cpufreq/policy8/scaling_cur_freq" \
    CHILD_MARKER="$child_marker" \
    CHILD_PID_FILE="$child_pid_file" ARGV_LOG="$argv_log" \
    ATTEST_LOG="$attest_log" APPLY_MARKER="$apply_marker" \
    ATTEST_REREAD_MARKER="$attest_reread_marker" \
    OFFLINE_ONLINE_ARM="$offline_online_arm" \
    OFFLINE_ONLINE_MARKER="$offline_online_marker" \
    FAKE_CPUINFO_DRIFT_PATH="$sys/cpufreq/policy8/cpuinfo_max_freq" \
        bash "$SCRIPT" "$@"
}
run_multi() { run_multi_after "$after_lock_noop" "$@"; }

profile_is_original() {
    test "$(<"$sys/cpufreq/policy2/scaling_governor")" = powersave &&
    test "$(<"$sys/cpufreq/policy2/energy_performance_preference")" = balance_power &&
    test "$(<"$sys/cpufreq/policy2/scaling_min_freq")" = 800000 &&
    test "$(<"$sys/cpufreq/policy2/scaling_max_freq")" = 3000000 &&
    test "$(<"$sys/cpufreq/policy8/scaling_governor")" = schedutil &&
    test "$(<"$sys/cpufreq/policy8/energy_performance_preference")" = balance_performance &&
    test "$(<"$sys/cpufreq/policy8/scaling_min_freq")" = 900000 &&
    test "$(<"$sys/cpufreq/policy8/scaling_max_freq")" = 3200000 &&
    test "$(<"$sys/cpufreq/policy16/scaling_governor")" = performance &&
    test "$(<"$sys/cpufreq/policy16/energy_performance_preference")" = performance &&
    test "$(<"$sys/cpufreq/policy16/scaling_min_freq")" = 1000000 &&
    test "$(<"$sys/cpufreq/policy16/scaling_max_freq")" = 3400000
}

reset_machine
run_multi --with-fixed-cpus 8,2 3800000 -- "$observer" 'two words' '*' \
    >"$tmp/success.out" 2>"$tmp/success.err"
success_rc=$?
[ "$success_rc" -eq 0 ] || sed -n '1,80p' "$tmp/success.err" >&2
expect_ok "atomic multi-core transaction succeeds" test "$success_rc" -eq 0
expect_ok "selection, sibling union, policy dedup, and policy sort are canonical" \
    grep -q '^CPUFREQ_MULTI_APPLIED requested_cpus=8,2 canonical_selected_cpus=2,8 logical_cpus=2,8,10,16 policies=policy2,policy8,policy16 target_khz=3800000 controls_verified=yes$' "$tmp/success.out"
expect_ok "all sibling keepers reached the concurrent barrier" \
    test "$(grep -c '^KEEPER_START ' "$event_log")" -eq 4
expect_ok "all keepers stopped before the benchmark child" \
    bash -c 'test "$(grep -c "^KEEPER_STOP " "$1")" -eq 4 && stop=$(grep -n "^KEEPER_STOP " "$1" | tail -1 | cut -d: -f1) && child=$(grep -n "^CHILD_START " "$1" | cut -d: -f1) && test "$stop" -lt "$child"' _ "$event_log"
expect_ok "child receives an unlinked read-only authenticated attestation" \
    grep -q '^EXITOS_CPUFREQ_ATTEST version=1 ' "$attest_log"
expect_ok "child argv is preserved without eval or glob expansion" \
    bash -c 'grep -q "^<two words>$" "$1" && grep -q "^<\*>$" "$1"' _ "$argv_log"
expect_ok "every policy remains locked while child verifies attestation" \
    test "$success_rc" -eq 0
expect_ok "successful child is followed by exact applied-profile drift check" \
    grep -q '^CPUFREQ_MULTI_POST_COMMAND profile_unchanged=yes topology_unchanged=yes$' "$tmp/success.out"
expect_ok "every distinct original policy profile is restored exactly" profile_is_original
expect_ok "restoration is explicitly attested" \
    grep -q '^CPUFREQ_MULTI_RESTORE policies=3 original_profiles_verified=yes$' "$tmp/success.out"
expect_ok "all policy locks were acquired before the first write in numeric policy order" \
    test "$(head -1 "$write_log")" = 'policy2/scaling_governor=powersave'
expect_ok "flock acquisition itself follows canonical numeric policy order" \
    test "$(paste -sd, "$lock_order_log")" = policy2,policy8,policy16

reset_machine
printf '%s\n' probe-locks-during-restore >"$write_action"
run_multi --with-fixed-cpus 2,8 3800000 -- true \
    >"$tmp/restore-locks.out" 2>"$tmp/restore-locks.err"
restore_locks_rc=$?
expect_ok "all policy locks remain held through post-check and restoration" \
    bash -c 'test "$1" -eq 0 && test "$(paste -sd, "$2")" = policy2=held,policy8=held,policy16=held' \
        _ "$restore_locks_rc" "$lock_probe_log"

reset_machine
mkdir -p "$locks/exitos-cpufreq"
chmod 0700 "$locks/exitos-cpufreq"
exec 8>"$locks/exitos-cpufreq/policy16.lock"
chmod 0600 "$locks/exitos-cpufreq/policy16.lock"
flock 8
run_multi --with-fixed-cpus 2,8 3800000 -- true \
    >"$tmp/contended.out" 2>"$tmp/contended.err"
contended_rc=$?
exec 8>&-
expect_ok "failure to acquire the last global-order policy lock is fatal" \
    test "$contended_rc" -ne 0
expect_ok "contention case reaches flock rather than failing a lock-file precondition" \
    grep -q 'another transaction holds policy16' "$tmp/contended.err"
expect_ok "lock acquisition is all-or-nothing before any policy write" test ! -s "$write_log"

reset_machine
run_multi_after "$after_lock_topology" --with-fixed-cpus 2,8 3800000 -- true \
    >"$tmp/topology.out" 2>"$tmp/topology.err"
topology_rc=$?
expect_ok "post-lock topology drift is fatal" test "$topology_rc" -ne 0
expect_ok "post-lock topology drift is detected before writes" test ! -s "$write_log"

reset_machine
run_multi_after "$after_lock_profile" --with-fixed-cpus 2,8 3800000 -- true \
    >"$tmp/postlock.out" 2>"$tmp/postlock.err"
postlock_rc=$?
expect_ok "post-lock profile is the authoritative restoration snapshot" \
    bash -c 'test "$1" -eq 0 && test "$(<"$2")" = ondemand' \
        _ "$postlock_rc" "$sys/cpufreq/policy8/scaling_governor"

reset_machine
printf '%s\n' 3799000 >"$sys/cpufreq/policy8/cpuinfo_max_freq"
run_multi --with-fixed-cpus 2,8 3800000 -- true \
    >"$tmp/max.out" 2>"$tmp/max.err"
max_rc=$?
expect_ok "every policy cpuinfo_max must equal the requested fixed maximum" \
    test "$max_rc" -ne 0
expect_ok "cpuinfo_max mismatch is rejected before writes" test ! -s "$write_log"

reset_machine
printf '%s\n' drift-cpuinfo-max-after-apply >"$write_action"
run_multi --with-fixed-cpus 2,8 3800000 -- touch "$child_marker" \
    >"$tmp/max-drift.out" 2>"$tmp/max-drift.err"
max_drift_rc=$?
expect_ok "same-inode cpuinfo_max drift after apply invalidates the transaction" \
    test "$max_drift_rc" -ne 0
expect_ok "cpuinfo_max drift cannot reach child with a stale attestation" \
    test ! -e "$child_marker"
expect_ok "cpuinfo_max drift failure still restores every policy" profile_is_original

attest_reread_env="$tmp/attest-reread-env"
printf '%s\n' \
    'unset BASH_ENV' \
    'trap '\''if [[ $BASH_COMMAND == read_line*cpuinfo_max_freq*attest_info_max* ]]; then trap - DEBUG; printf "%s\n" 3799000 >"$FAKE_CPUINFO_DRIFT_PATH"; : >"$ATTEST_REREAD_MARKER"; fi'\'' DEBUG' \
    >"$attest_reread_env"
chmod 0600 "$attest_reread_env"
reset_machine
BASH_ENV="$attest_reread_env" \
    run_multi --with-fixed-cpus 2,8 3800000 -- touch "$child_marker" \
    >"$tmp/attest-reread.out" 2>"$tmp/attest-reread.err"
attest_reread_rc=$?
expect_ok "attestation construction performs an independent live cpuinfo_max reread" \
    test -e "$attest_reread_marker"
expect_ok "cpuinfo_max drift at the attestation reread fails closed" \
    test "$attest_reread_rc" -ne 0
expect_ok "attestation-reread drift never reaches the child" test ! -e "$child_marker"
expect_ok "attestation-reread drift restores every policy" profile_is_original

reset_machine
run_multi --with-fixed-cpus 2,10 3800000 -- true \
    >"$tmp/core-overlap.out" 2>"$tmp/core-overlap.err"
overlap_rc=$?
expect_ok "selecting both SMT threads of one physical core is rejected as overlap" \
    test "$overlap_rc" -ne 0
expect_ok "physical-core overlap is rejected before writes" test ! -s "$write_log"

reset_machine
run_multi --with-fixed-cpus 2,2 3800000 -- true \
    >"$tmp/duplicate.out" 2>"$tmp/duplicate.err"
duplicate_rc=$?
expect_ok "duplicate selected CPU input is rejected" test "$duplicate_rc" -ne 0
expect_ok "duplicate selection is rejected before writes" test ! -s "$write_log"

reset_machine
printf '%s\n' '8,10' >"$sys/cpufreq/policy8/related_cpus"
run_multi --with-fixed-cpus 2,8 3800000 -- true \
    >"$tmp/policy-overlap.out" 2>"$tmp/policy-overlap.err"
policy_overlap_rc=$?
expect_ok "overlapping distinct policy scopes are rejected" test "$policy_overlap_rc" -ne 0
expect_ok "policy-scope overlap is rejected before writes" test ! -s "$write_log"

reset_machine
make_cpu 20 20 policy16
printf '%s\n' '2,8,10,16,20' >"$sys/online"
printf '%s\n' '16,20' >"$sys/cpufreq/policy16/related_cpus"
run_multi --with-fixed-cpus 2,8 3800000 -- true \
    >"$tmp/scope-escape.out" 2>"$tmp/scope-escape.err"
scope_escape_rc=$?
expect_ok "a policy scope extending outside selected physical cores is rejected" \
    test "$scope_escape_rc" -ne 0
expect_ok "out-of-scope policy control is rejected before writes" test ! -s "$write_log"

reset_machine
mv -- "$sys/cpufreq/policy8" "$sys/cpufreq/policy02"
rm -f -- "$sys/cpu8/cpufreq"
ln -s ../cpufreq/policy02 "$sys/cpu8/cpufreq"
run_multi --with-fixed-cpus 2,8 3800000 -- touch "$child_marker" \
    >"$tmp/policy-leading-zero.out" 2>"$tmp/policy-leading-zero.err"
policy_leading_zero_rc=$?
expect_ok "a leading-zero policy basename that duplicates a numeric policy ID is rejected" \
    test "$policy_leading_zero_rc" -ne 0
expect_ok "noncanonical or duplicate numeric policy IDs are rejected before writes" \
    test ! -s "$write_log"

reset_machine
printf '%s\n' fail-policy8-apply >"$write_action"
run_multi --with-fixed-cpus 2,8 3800000 -- touch "$child_marker" \
    >"$tmp/partial.out" 2>"$tmp/partial.err"
partial_rc=$?
[ "$partial_rc" -eq 3 ] || {
    printf 'partial_rc=%s\n' "$partial_rc" >&2
    sed -n '1,120p' "$tmp/partial.err" >&2
}
expect_ok "partial multi-policy apply fails closed" test "$partial_rc" -eq 3
expect_ok "partial apply never launches benchmark child" test ! -e "$child_marker"
expect_ok "partial apply restores every policy exactly" profile_is_original

reset_machine
printf '%s\n' 3300000 >"$sys/cpufreq/policy8/scaling_cur_freq"
run_multi --with-fixed-cpus 2,8 3800000 -- touch "$child_marker" \
    >"$tmp/actual.out" 2>"$tmp/actual.err"
actual_rc=$?
[ "$actual_rc" -eq 4 ] || {
    printf 'actual_rc=%s\n' "$actual_rc" >&2
    sed -n '1,120p' "$tmp/actual.err" >&2
}
expect_ok "actual-frequency mismatch on one sibling fails closed" test "$actual_rc" -eq 4
expect_ok "actual-frequency mismatch refuses to launch child" test ! -e "$child_marker"
expect_ok "actual-frequency failure restores every policy exactly" profile_is_original

reset_machine
KEEPER_ACTION=drop-frequency-on-stop \
    run_multi --with-fixed-cpus 2,8 3800000 -- touch "$child_marker" \
    >"$tmp/post-keeper-drop.out" 2>"$tmp/post-keeper-drop.err"
post_keeper_drop_rc=$?
expect_ok "frequency drop caused by keeper shutdown invalidates post-keeper verification" \
    test "$post_keeper_drop_rc" -eq 4
expect_ok "post-keeper frequency drop cannot reach child" test ! -e "$child_marker"
expect_ok "post-keeper frequency drop still restores every policy" profile_is_original

reset_machine
KEEPER_ACTION=lie-about-sched \
    run_multi --with-fixed-cpus 2,8 3800000 -- touch "$child_marker" \
    >"$tmp/keeper-policy-lie.out" 2>"$tmp/keeper-policy-lie.err"
keeper_policy_lie_rc=$?
expect_ok "a keeper that merely prints SCHED_IDLE without entering it is rejected" \
    test "$keeper_policy_lie_rc" -eq 4
expect_ok "false keeper policy attestation cannot reach child" test ! -e "$child_marker"
expect_ok "false keeper policy failure restores every policy" profile_is_original

reset_machine
timeout --signal=TERM --kill-after=1 2 env --default-signal=INT,QUIT,HUP,TERM \
    EXITOS_CPUFREQ_MULTI_TEST_MODE=1 EXITOS_CPU_SYSFS_ROOT="$sys" \
    EXITOS_CPUFREQ_LOCK_ROOT="$locks" EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
    EXITOS_CPUFREQ_MULTI_AFTER_LOCK_HELPER="$after_lock_noop" \
    EXITOS_KEEPER_BIN="$keeper" FAKE_WRITE_LOG="$write_log" \
    FAKE_WRITE_ACTION="$write_action" FAKE_EVENT_LOG="$event_log" \
    KEEPER_STATE="$keeper_state" EXPECTED_KEEPERS=4 KEEPER_ACTION=stubborn \
    CHILD_MARKER="$child_marker" CHILD_PID_FILE="$child_pid_file" \
    ARGV_LOG="$argv_log" ATTEST_LOG="$attest_log" APPLY_MARKER="$apply_marker" \
    bash "$SCRIPT" --with-fixed-cpus 2,8 3800000 -- touch "$child_marker" \
    >"$tmp/stubborn-keeper.out" 2>"$tmp/stubborn-keeper.err"
stubborn_keeper_rc=$?
expect_ok "non-cooperative actual-frequency keepers fail within a bounded interval" \
    test "$stubborn_keeper_rc" -eq 4
expect_ok "non-cooperative keepers never overlap the child" test ! -e "$child_marker"
expect_ok "non-cooperative keeper failure restores every policy" profile_is_original

reset_machine
drifter="$tmp/profile-drifter"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" 3700000 >"$EXITOS_CPU_SYSFS_ROOT/cpufreq/policy8/scaling_min_freq"' \
    >"$drifter"
chmod 0700 "$drifter"
run_multi --with-fixed-cpus 2,8 3800000 -- "$drifter" \
    >"$tmp/drift.out" 2>"$tmp/drift.err"
drift_rc=$?
[ "$drift_rc" -eq 71 ] || {
    printf 'drift_rc=%s\n' "$drift_rc" >&2
    sed -n '1,120p' "$tmp/drift.err" >&2
}
expect_ok "control-profile drift during child invalidates child success" \
    test "$drift_rc" -eq 71
expect_ok "profile drift path restores every policy exactly" profile_is_original

reset_machine
topology_drifter="$tmp/topology-drifter"
printf '%s\n' '#!/bin/bash' \
    'printf "%s\n" 8 >"$EXITOS_CPU_SYSFS_ROOT/cpu8/topology/thread_siblings_list"' \
    >"$topology_drifter"
chmod 0700 "$topology_drifter"
run_multi --with-fixed-cpus 2,8 3800000 -- "$topology_drifter" \
    >"$tmp/child-topology-drift.out" 2>"$tmp/child-topology-drift.err"
child_topology_rc=$?
expect_ok "topology drift during child invalidates child success" \
    test "$child_topology_rc" -eq 71
expect_ok "topology drift still restores every unchanged policy object exactly" \
    profile_is_original

reset_machine
alternate_attest="$tmp/alternate-attest"
alternate_dumper="$tmp/alternate-dumper"
printf '%s\n' '#!/bin/bash' \
    'set -u' \
    'fd=${EXITOS_CPUFREQ_ATTEST_FD:?}' \
    'token=${EXITOS_CPUFREQ_ATTEST_TOKEN:?}' \
    'grep -q "^EXITOS_CPUFREQ_ATTEST version=1 token=$token requested_cpus=8,10 canonical_selected_cpus=2,8 logical_cpus=2,8,10,16 " "/proc/self/fd/$fd" || exit 91' \
    'printf "%s\n" ok >"$ALTERNATE_ATTEST"' >"$alternate_dumper"
chmod 0700 "$alternate_dumper"
ALTERNATE_ATTEST="$alternate_attest" \
    run_multi --with-fixed-cpus 8,10 3800000 -- "$alternate_dumper" \
    >"$tmp/alternate.out" 2>"$tmp/alternate.err"
alternate_rc=$?
expect_ok "alternate SMT selection maps to the same canonical physical-core IDs" \
    bash -c 'test "$1" -eq 0 && test "$(<"$2")" = ok' \
        _ "$alternate_rc" "$alternate_attest"
expect_ok "alternate SMT selection restores every policy" profile_is_original

reset_machine
printf '%s\n' 0 >"$sys/cpu10/online"
printf '%s\n' '2,8,16' >"$sys/online"
make_policy policy10 10 powersave balance_power 800000 3000000
rm -f -- "$sys/cpu10/cpufreq"
ln -s ../cpufreq/policy10 "$sys/cpu10/cpufreq"
EXPECTED_KEEPERS_OVERRIDE=3 \
    run_multi --with-fixed-cpus 2,8 3800000 -- true \
    >"$tmp/offline-sibling.out" 2>"$tmp/offline-sibling.err"
offline_sibling_rc=$?
expect_ok "offline SMT siblings are excluded from the canonical logical CPU union" \
    bash -c 'test "$1" -eq 0 && grep -q "logical_cpus=2,8,16 " "$2"' \
        _ "$offline_sibling_rc" "$tmp/offline-sibling.out"
expect_ok "offline SMT siblings never receive a keeper" \
    bash -c 'test "$(grep -c "^KEEPER_START " "$1")" -eq 3 && ! grep -q "^KEEPER_START cpu=10 " "$1"' \
        _ "$event_log"
expect_ok "offline SMT siblings cannot add a policy write target" \
    bash -c '! grep -q "^policy10/" "$1"' _ "$write_log"
expect_ok "offline SMT sibling handling restores every selected policy" profile_is_original

postlock_online_env="$tmp/postlock-online-env"
printf '%s\n' \
    'unset BASH_ENV' \
    'trap '\''if [[ -e ${OFFLINE_ONLINE_ARM:-/nonexistent} && $BASH_COMMAND == *TOPOLOGY_FINGERPRINT*PRELOCK_FINGERPRINT* ]]; then trap - DEBUG; printf "%s\n" 1 >"$EXITOS_CPU_SYSFS_ROOT/cpu10/online"; printf "%s\n" "2,8,10,16" >"$EXITOS_CPU_SYSFS_ROOT/online"; : >"$OFFLINE_ONLINE_MARKER"; fi'\'' DEBUG' \
    >"$postlock_online_env"
chmod 0600 "$postlock_online_env"
reset_machine
printf '%s\n' 0 >"$sys/cpu10/online"
printf '%s\n' '2,8,16' >"$sys/online"
rm -f -- "$sys/cpu10/cpufreq"
BASH_ENV="$postlock_online_env" EXPECTED_KEEPERS_OVERRIDE=3 \
    run_multi_after "$after_lock_arm_online" \
        --with-fixed-cpus 2,8 3800000 -- touch "$child_marker" \
    >"$tmp/offline-online-drift.out" 2>"$tmp/offline-online-drift.err"
offline_online_drift_rc=$?
expect_ok "offline-to-online drift is injected after the post-lock topology snapshot" \
    test -e "$offline_online_marker"
expect_ok "offline-to-online drift fails closed" test "$offline_online_drift_rc" -ne 0
expect_ok "offline-to-online drift cannot begin the forward policy apply" \
    test "$(head -1 "$write_log")" = policy16/scaling_governor=powersave
expect_ok "offline-to-online drift starts neither keepers nor child" \
    bash -c '! grep -q "^KEEPER_START " "$1" && test ! -e "$2"' _ \
        "$event_log" "$child_marker"
expect_ok "offline-to-online drift still restores every policy" profile_is_original

run_signal_case() {
    local sig=$1 expected_rc=$2 lower=${1,,}
    local tx_pid rc child_pid=0
    reset_machine
    env --default-signal=INT,QUIT,HUP,TERM \
        EXITOS_CPUFREQ_MULTI_TEST_MODE=1 EXITOS_CPU_SYSFS_ROOT="$sys" \
        EXITOS_CPUFREQ_LOCK_ROOT="$locks" EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
        EXITOS_CPUFREQ_MULTI_AFTER_LOCK_HELPER="$after_lock_noop" \
        EXITOS_KEEPER_BIN="$keeper" FAKE_WRITE_LOG="$write_log" \
        FAKE_WRITE_ACTION="$write_action" FAKE_EVENT_LOG="$event_log" \
        KEEPER_STATE="$keeper_state" EXPECTED_KEEPERS=4 \
        CHILD_MARKER="$child_marker" CHILD_PID_FILE="$child_pid_file" \
        ARGV_LOG="$argv_log" ATTEST_LOG="$attest_log" \
        bash "$SCRIPT" --with-fixed-cpus 2,8 3800000 -- "$sleeper" \
        >"$tmp/signal-$lower.out" 2>"$tmp/signal-$lower.err" &
    tx_pid=$!
    for _ in $(seq 1 300); do
        [ -e "$child_marker" ] && break
        kill -0 "$tx_pid" 2>/dev/null || break
        sleep 0.01
    done
    expect_ok "$sig test reaches the benchmark child" test -e "$child_marker"
    kill -"$sig" "$tx_pid"
    if wait "$tx_pid"; then rc=0; else rc=$?; fi
    [ "$rc" -eq "$expected_rc" ] || {
        printf '%s_rc=%s expected=%s\n' "$sig" "$rc" "$expected_rc" >&2
        sed -n '1,120p' "$tmp/signal-$lower.err" >&2
    }
    expect_ok "$sig returns the conventional status within bounded cleanup" \
        test "$rc" -eq "$expected_rc"
    expect_ok "$sig restores every policy exactly" profile_is_original
    [ ! -s "$child_pid_file" ] || child_pid=$(<"$child_pid_file")
    expect_fail "$sig leaves no benchmark process group" \
        bash -c 'kill -0 -- "-$1" 2>/dev/null' _ "$child_pid"
}

run_signal_case HUP 129
run_signal_case INT 130
run_signal_case QUIT 131
run_signal_case TERM 143

signal_before_pid_env="$tmp/signal-before-child-pid-env"
printf '%s\n' \
    'unset BASH_ENV' \
    'trap '\''if [[ $BASH_COMMAND == "CHILD_PID=\$!" ]]; then trap - DEBUG; handle_signal 143 TERM; fi'\'' DEBUG' \
    >"$signal_before_pid_env"
chmod 0600 "$signal_before_pid_env"
reset_machine
BASH_ENV="$signal_before_pid_env" \
    run_multi --with-fixed-cpus 2,8 3800000 -- "$sleeper" \
    >"$tmp/signal-before-pid.out" 2>"$tmp/signal-before-pid.err"
signal_before_pid_rc=$?
expect_ok "TERM in the fork-to-CHILD_PID publication window is retained" \
    test "$signal_before_pid_rc" -eq 143
expect_ok "fork-publication TERM restores every policy" profile_is_original
if [ -s "$child_pid_file" ]; then before_pid_child=$(<"$child_pid_file"); else before_pid_child=0; fi
expect_ok "fork-publication TERM leaves no benchmark process group" \
    bash -c 'test "$1" -eq 0 || ! kill -0 -- "-$1" 2>/dev/null' _ "$before_pid_child"

reset_machine
printf '%s\n' slow-policy8-apply >"$write_action"
env --default-signal=INT,QUIT,HUP,TERM \
    EXITOS_CPUFREQ_MULTI_TEST_MODE=1 EXITOS_CPU_SYSFS_ROOT="$sys" \
    EXITOS_CPUFREQ_LOCK_ROOT="$locks" EXITOS_CPUFREQ_WRITE_HELPER="$writer" \
    EXITOS_CPUFREQ_MULTI_FLOCK_BIN="$fake_flock" \
    EXITOS_CPUFREQ_MULTI_AFTER_LOCK_HELPER="$after_lock_noop" \
    EXITOS_KEEPER_BIN="$keeper" FAKE_WRITE_LOG="$write_log" \
    FAKE_WRITE_ACTION="$write_action" FAKE_EVENT_LOG="$event_log" \
    FAKE_LOCK_ORDER_LOG="$lock_order_log" REAL_FLOCK="$real_flock" \
    KEEPER_STATE="$keeper_state" EXPECTED_KEEPERS=4 \
    CHILD_MARKER="$child_marker" CHILD_PID_FILE="$child_pid_file" \
    ARGV_LOG="$argv_log" ATTEST_LOG="$attest_log" APPLY_MARKER="$apply_marker" \
    bash "$SCRIPT" --with-fixed-cpus 2,8 3800000 -- touch "$child_marker" \
    >"$tmp/signal-apply.out" 2>"$tmp/signal-apply.err" &
apply_tx_pid=$!
for _ in $(seq 1 200); do
    [ -e "$apply_marker" ] && break
    kill -0 "$apply_tx_pid" 2>/dev/null || break
    sleep 0.005
done
expect_ok "apply-signal test reaches a partial multi-policy apply" test -e "$apply_marker"
kill -TERM "$apply_tx_pid"
if wait "$apply_tx_pid"; then apply_signal_rc=0; else apply_signal_rc=$?; fi
expect_ok "TERM during apply is caught and reported conventionally" \
    test "$apply_signal_rc" -eq 143
expect_ok "TERM during apply refuses to launch child" test ! -e "$child_marker"
expect_ok "TERM during partial apply restores every policy" profile_is_original

reset_machine
printf '%s\n' ignore-policy8-restore >"$write_action"
run_multi --with-fixed-cpus 2,8 3800000 -- true \
    >"$tmp/restore-fail.out" 2>"$tmp/restore-fail.err"
restore_rc=$?
expect_ok "restore readback failure overrides successful child" test "$restore_rc" -eq 70
expect_ok "restore failure is explicit" \
    grep -q '^CPUFREQ_MULTI_RESTORE_FAILED ' "$tmp/restore-fail.err"
expect_ok "one restore failure does not skip restoration of other policies" \
    bash -c 'test "$(<"$1")" = powersave && test "$(<"$2")" = performance' \
        _ "$sys/cpufreq/policy2/scaling_governor" \
        "$sys/cpufreq/policy16/scaling_governor"

expect_ok "implementation exists" test -f "$SCRIPT"
expect_ok "implementation contains no eval" \
    bash -c 'test -f "$1" && ! grep -Eq "(^|[^[:alnum:]_])eval([[:space:]]|$)" "$1"' _ "$SCRIPT"
expect_ok "implementation never changes scheduling policy or process nice" \
    bash -c 'test -f "$1" && ! grep -Eq "chrt|SCHED_FIFO|SCHED_RR|renice|setpriority" "$1"' _ "$SCRIPT"
expect_ok "implementation never writes global intel_pstate controls" \
    bash -c 'test -f "$1" && ! grep -Eq "no_turbo|min_perf_pct|max_perf_pct|hwp_dynamic_boost" "$1"' _ "$SCRIPT"
expect_ok "complete pure test leaves no task-owned helper process" \
    bash -c 'test -z "$1"' _ "$(owned_test_pids)"

echo "1..$n"
exit "$failed"
