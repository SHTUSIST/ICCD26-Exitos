#!/bin/bash
# Transactional, per-policy CPU-frequency control for latency experiments.
#
# Success means the requested sysfs CONTROL SETTINGS were read back exactly.
# It does not mean that physical MHz stayed constant: idle states, hardware
# coordination, turbo/thermal limits, package/uncore power and PCIe power are
# outside this tool's control.
set -u -o pipefail
set -f

die() {
    echo "cpufreq transaction: $*" >&2
    exit 2
}

usage() {
    cat >&2 <<'EOF'
usage:
  tools/cpufreq-transaction.sh --status CPU
  tools/cpufreq-transaction.sh --plan-performance CPU
  tools/cpufreq-transaction.sh --plan-fixed CPU FREQ_KHZ
  tools/cpufreq-transaction.sh --with-performance CPU --expect-policy-cpus CPUS -- COMMAND [ARG...]
  tools/cpufreq-transaction.sh --with-fixed CPU FREQ_KHZ --expect-policy-cpus CPUS -- COMMAND [ARG...]

CPUS must exactly match the policy's related_cpus after whitespace
normalization.  Run a plan command first and copy its related_cpus value.
EOF
    exit 2
}

MODE=${1:-}
case "$MODE" in
  --status|--plan-performance|--plan-fixed|--with-performance|--with-fixed) ;;
  *) usage ;;
esac
shift

CPU_RAW=${1:-}
[[ $CPU_RAW =~ ^[0-9]+$ ]] || usage
((${#CPU_RAW} <= 7)) || die "CPU decimal input is too long"
CPU=$((10#$CPU_RAW))
shift
(( CPU <= 1048575 )) || die "CPU number is unreasonably large"

TARGET_KHZ=
case "$MODE" in
  --plan-fixed|--with-fixed)
    TARGET_RAW=${1:-}
    [[ $TARGET_RAW =~ ^[0-9]+$ ]] || usage
    ((${#TARGET_RAW} <= 9)) || die "FREQ_KHZ decimal input is too long"
    TARGET_KHZ=$((10#$TARGET_RAW))
    shift
    (( TARGET_KHZ > 0 && TARGET_KHZ <= 100000000 )) ||
        die "FREQ_KHZ is outside the accepted numeric range"
    ;;
esac

EXPECTED_SCOPE=
TRUSTED_CHILD_TRANSACTIONS=0
declare -a CHILD=()
case "$MODE" in
  --with-performance|--with-fixed)
    [ "${1:-}" = --expect-policy-cpus ] || usage
    [ "$#" -ge 2 ] || usage
    EXPECTED_SCOPE=$2
    shift 2
    if [ "${1:-}" = --core-trusted-child-transactions ]; then
        [ "$#" -ge 2 ] || usage
        TRUSTED_DEPTH_RAW=$2
        [[ $TRUSTED_DEPTH_RAW =~ ^[0-9]+$ ]] || usage
        ((${#TRUSTED_DEPTH_RAW} <= 4)) ||
            die "trusted child transaction depth is too long"
        TRUSTED_CHILD_TRANSACTIONS=$((10#$TRUSTED_DEPTH_RAW))
        shift 2
        (( TRUSTED_CHILD_TRANSACTIONS >= 1 && TRUSTED_CHILD_TRANSACTIONS <= 1023 )) ||
            die "trusted child transaction depth is outside 1-1023"
    fi
    [ "${1:-}" = -- ] || usage
    shift
    [ "$#" -ge 1 ] || usage
    CHILD=("$@")
    ;;
  *) [ "$#" -eq 0 ] || usage ;;
esac

validate_trusted_child_protocol() {
    local tx_fd expected_depth index
    [ "$TRUSTED_CHILD_TRANSACTIONS" -gt 0 ] || return 0
    [ "${EXITOS_CPUFREQ_CORE_INTERNAL:-0}" = 1 ] || return 1
    tx_fd=${EXITOS_CPUFREQ_CORE_TX_FD:-}
    [[ $tx_fd =~ ^[0-9]+$ ]] || return 1
    [ -r "/proc/self/fd/$tx_fd" ] && [ -x "/proc/self/fd/$tx_fd" ] || return 1
    [ "${CHILD[0]:-}" = /bin/bash ] &&
    [ "${CHILD[1]:-}" = "/proc/self/fd/$tx_fd" ] || return 1
    case "${CHILD[2]:-}" in
      --with-fixed) index=5 ;;
      --with-performance) index=4 ;;
      *) return 1 ;;
    esac
    [ "${CHILD[$index]:-}" = --expect-policy-cpus ] || return 1
    index=$((index + 2))
    expected_depth=$((TRUSTED_CHILD_TRANSACTIONS - 1))
    if [ "$expected_depth" -gt 0 ]; then
        [ "${CHILD[$index]:-}" = --core-trusted-child-transactions ] &&
        [ "${CHILD[$((index + 1))]:-}" = "$expected_depth" ] || return 1
        index=$((index + 2))
    else
        [ "${CHILD[$index]:-}" != --core-trusted-child-transactions ] || return 1
    fi
    [ "${CHILD[$index]:-}" = -- ]
}

validate_trusted_child_protocol ||
    die "invalid trusted nested transaction protocol"

TEST_MODE=${EXITOS_CPUFREQ_TEST_MODE:-0}
SYSROOT_INPUT=${EXITOS_CPU_SYSFS_ROOT:-/sys/devices/system/cpu}
LOCK_ROOT=${EXITOS_CPUFREQ_LOCK_ROOT:-/run/lock}
WRITE_HELPER=${EXITOS_CPUFREQ_WRITE_HELPER:-}
AFTER_LOCK_HELPER=${EXITOS_CPUFREQ_AFTER_LOCK_HELPER:-}

case "$TEST_MODE" in 0|1) ;; *) die "EXITOS_CPUFREQ_TEST_MODE must be 0 or 1" ;; esac
if [ "$TEST_MODE" = 1 ]; then
    [ "$SYSROOT_INPUT" != /sys ] && [[ $SYSROOT_INPUT != /sys/* ]] ||
        die "test mode refuses a real /sys root"
    [ -n "$WRITE_HELPER" ] && [ -x "$WRITE_HELPER" ] ||
        die "test mode requires an executable fake write helper"
    [ -z "$AFTER_LOCK_HELPER" ] || [ -x "$AFTER_LOCK_HELPER" ] ||
        die "test after-lock helper is not executable"
else
    [ "$SYSROOT_INPUT" = /sys/devices/system/cpu ] ||
        die "a nonstandard sysfs root is accepted only by the pure unit test"
    [ -z "$WRITE_HELPER" ] ||
        die "a write helper is accepted only by the pure unit test"
    [ -z "$AFTER_LOCK_HELPER" ] ||
        die "an after-lock helper is accepted only by the pure unit test"
    [ "$LOCK_ROOT" = /run/lock ] ||
        die "a nonstandard lock root is accepted only by the pure unit test"
fi

# intel_pstate+HWP control changes can become visible a few milliseconds after
# the sysfs write returns.  Require three consecutive exact samples within a
# strict deadline; a single transient value is not enough to launch a test or
# declare restoration successful.  Unit tests use a shorter wall-clock budget.
READBACK_ATTEMPTS=201
READBACK_DELAY=0.01
if [ "$TEST_MODE" = 1 ]; then
    READBACK_ATTEMPTS=80
    READBACK_DELAY=0.002
fi
READBACK_STABLE_SAMPLES=3

SYSROOT=$(readlink -f -- "$SYSROOT_INPUT") || die "cannot resolve CPU sysfs root"
[ -d "$SYSROOT" ] || die "CPU sysfs root is not a directory"
if [ "$TEST_MODE" = 1 ]; then
    [ "$SYSROOT" != /sys ] && [[ $SYSROOT != /sys/* ]] ||
        die "test mode sysfs root resolves into real /sys"
fi
CPU_DIR="$SYSROOT/cpu$CPU"
[ -d "$CPU_DIR" ] || die "CPU $CPU does not exist"

normalize_space_list() {
    local raw=$1 token
    raw=${raw//$'\n'/ }
    raw=${raw//$'\t'/ }
    raw=${raw//,/ }
    # shellcheck disable=SC2086 # intentional whitespace normalization
    set -- $raw
    [ "$#" -gt 0 ] || return 1
    for token in "$@"; do
        [[ $token =~ ^[0-9]+(-[0-9]+)?$ ]] || return 1
    done
    printf '%s' "$1"
    shift
    while [ "$#" -gt 0 ]; do
        printf ' %s' "$1"
        shift
    done
    printf '\n'
}

cpu_list_contains() {
    local list=$1 wanted=$2 token lo hi
    list=${list//,/ }
    for token in $list; do
        if [[ $token =~ ^([0-9]+)-([0-9]+)$ ]]; then
            lo=$((10#${BASH_REMATCH[1]}))
            hi=$((10#${BASH_REMATCH[2]}))
            (( wanted >= lo && wanted <= hi )) && return 0
        elif [[ $token =~ ^[0-9]+$ ]]; then
            (( wanted == 10#$token )) && return 0
        else
            return 2
        fi
    done
    return 1
}

if [ -r "$CPU_DIR/online" ]; then
    [ "$(<"$CPU_DIR/online")" = 1 ] || die "CPU $CPU is offline"
else
    [ -r "$SYSROOT/online" ] || die "cannot prove CPU $CPU is online"
    cpu_list_contains "$(<"$SYSROOT/online")" "$CPU" ||
        die "CPU $CPU is not in the online CPU list"
fi

resolve_policy() {
    local got dir base
    got=$(readlink -f -- "$CPU_DIR/cpufreq") || return 1
    dir=${got%/*}
    base=${got##*/}
    [ "$dir" = "$SYSROOT/cpufreq" ] || return 1
    [[ $base =~ ^policy[0-9]+$ ]] || return 1
    [ -d "$got" ] || return 1
    printf '%s\n' "$got"
}

POLICY=$(resolve_policy) || die "CPU $CPU has no canonical cpufreq policy"
POLICY_NAME=${POLICY##*/}
POLICY_ID=$(stat -Lc '%d:%i' -- "$POLICY") || die "cannot retain policy identity"

read_required() {
    local field=$1 value
    [ -r "$POLICY/$field" ] || die "missing policy field: $field"
    IFS= read -r value <"$POLICY/$field" || die "cannot read policy field: $field"
    [ -n "$value" ] || die "empty policy field: $field"
    printf '%s\n' "$value"
}

check_identity() {
    local got got_id online
    got=$(resolve_policy) || return 1
    [ "$got" = "$POLICY" ] || return 1
    got_id=$(stat -Lc '%d:%i' -- "$got") || return 1
    [ "$got_id" = "$POLICY_ID" ] || return 1
    if [ -r "$CPU_DIR/online" ]; then
        online=$(<"$CPU_DIR/online") || return 1
        [ "$online" = 1 ] || return 1
    fi
}

DRIVER=$(read_required scaling_driver) || exit $?
GOVERNOR=$(read_required scaling_governor) || exit $?
AVAILABLE_GOVERNORS=$(read_required scaling_available_governors) || exit $?
MIN_KHZ=$(read_required scaling_min_freq) || exit $?
MAX_KHZ=$(read_required scaling_max_freq) || exit $?
CPUINFO_MIN_KHZ=$(read_required cpuinfo_min_freq) || exit $?
CPUINFO_MAX_KHZ=$(read_required cpuinfo_max_freq) || exit $?
AFFECTED_RAW=$(read_required affected_cpus) || exit $?
AFFECTED=$(normalize_space_list "$AFFECTED_RAW") || die "affected_cpus is empty"
RELATED_RAW=$(read_required related_cpus) || exit $?
RELATED=$(normalize_space_list "$RELATED_RAW") || die "related_cpus is empty"
AFFECTED_TOKEN=${AFFECTED// /,}
RELATED_TOKEN=${RELATED// /,}
CUR_KHZ=unavailable
[ ! -r "$POLICY/scaling_cur_freq" ] || {
    CUR_KHZ=$(read_required scaling_cur_freq) || exit $?
}
EPP=unsupported
AVAILABLE_EPP=
if [ -e "$POLICY/energy_performance_preference" ]; then
    EPP=$(read_required energy_performance_preference) || exit $?
    AVAILABLE_EPP=$(read_required energy_performance_available_preferences) || exit $?
fi

for value in "$MIN_KHZ" "$MAX_KHZ" "$CPUINFO_MIN_KHZ" "$CPUINFO_MAX_KHZ"; do
    [[ $value =~ ^[0-9]+$ ]] || die "a frequency field is not an integer"
done
(( 10#$CPUINFO_MIN_KHZ <= 10#$MIN_KHZ &&
   10#$MIN_KHZ <= 10#$MAX_KHZ &&
   10#$MAX_KHZ <= 10#$CPUINFO_MAX_KHZ )) ||
    die "frequency bounds are internally inconsistent"

word_present() {
    local words=$1 wanted=$2 word
    for word in $words; do [ "$word" = "$wanted" ] && return 0; done
    return 1
}

print_status() {
    printf 'CPUFREQ_STATUS cpu=%s policy=%s affected_cpus=%s related_cpus=%s\n' \
        "$CPU" "$POLICY_NAME" "$AFFECTED_TOKEN" "$RELATED_TOKEN"
    printf 'CONTROL driver=%s governor=%s epp=%s\n' "$DRIVER" "$GOVERNOR" "$EPP"
    printf 'FREQUENCY min_khz=%s max_khz=%s cur_khz=%s cpuinfo_min_khz=%s cpuinfo_max_khz=%s\n' \
        "$MIN_KHZ" "$MAX_KHZ" "$CUR_KHZ" "$CPUINFO_MIN_KHZ" "$CPUINFO_MAX_KHZ"
}

preflight_profile() {
    word_present "$AVAILABLE_GOVERNORS" performance ||
        die "performance governor is unavailable"
    [ "$GOVERNOR" != userspace ] ||
        die "userspace governor restoration requires scaling_setspeed support"
    if [ "$EPP" != unsupported ]; then
        word_present "$AVAILABLE_EPP" performance ||
            die "performance EPP is unavailable"
    fi
    if [ "$GOVERNOR" = performance ]; then
        word_present "$AVAILABLE_GOVERNORS" powersave ||
            die "original performance governor has no safe powersave staging governor for bounds restoration"
    fi
    if [ -n "$TARGET_KHZ" ]; then
        (( TARGET_KHZ >= 10#$CPUINFO_MIN_KHZ &&
           TARGET_KHZ <= 10#$CPUINFO_MAX_KHZ )) ||
            die "fixed target is outside cpuinfo frequency bounds"
    fi
}

case "$MODE" in
  --status)
    print_status
    exit 0
    ;;
  --plan-performance|--plan-fixed)
    preflight_profile
    plan_mode=performance
    [ "$MODE" = --plan-fixed ] && plan_mode=fixed
    printf 'CPUFREQ_PLAN mode=%s cpu=%s policy=%s affected_cpus=%s related_cpus=%s\n' \
        "$plan_mode" "$CPU" "$POLICY_NAME" "$AFFECTED_TOKEN" "$RELATED_TOKEN"
    printf 'REQUEST governor=performance epp=%s min_khz=%s max_khz=%s\n' \
        "$([ "$EPP" = unsupported ] && printf unsupported || printf performance)" \
        "$([ -n "$TARGET_KHZ" ] && printf '%s' "$TARGET_KHZ" || printf '%s' "$MIN_KHZ")" \
        "$([ -n "$TARGET_KHZ" ] && printf '%s' "$TARGET_KHZ" || printf '%s' "$MAX_KHZ")"
    printf 'VERIFY control_settings=exact_readback restore=exact_readback\n'
    printf 'UNCONTROLLED core_cstate package_cstate uncore pcie_aspm turbo thermal actual_mhz\n'
    exit 0
    ;;
esac

preflight_profile
EXPECTED_SCOPE=$(normalize_space_list "$EXPECTED_SCOPE") || die "expected policy scope is empty"

verify_expected_scope() {
    local raw current
    [ -r "$POLICY/related_cpus" ] || return 1
    IFS= read -r raw <"$POLICY/related_cpus" || return 1
    current=$(normalize_space_list "$raw") || return 1
    [ "$EXPECTED_SCOPE" = "$current" ] || return 1
    cpu_list_contains "$current" "$CPU"
}

verify_expected_scope ||
    die "policy scope mismatch: expected related_cpus=[$EXPECTED_SCOPE], actual=[$RELATED]"
check_identity || die "policy identity changed during preflight"

if [ "$TEST_MODE" != 1 ]; then
    [ "$(id -u)" -eq 0 ] || die "changing cpufreq controls requires root"
fi
mkdir -p -- "$LOCK_ROOT" || die "cannot access policy lock directory"
LOCK_NAMESPACE="$LOCK_ROOT/exitos-cpufreq"
old_umask=$(umask)
umask 077
mkdir -- "$LOCK_NAMESPACE" 2>/dev/null || true
umask "$old_umask"
[ -d "$LOCK_NAMESPACE" ] && [ ! -L "$LOCK_NAMESPACE" ] ||
    die "lock namespace is not a real directory: $LOCK_NAMESPACE"
lock_owner=$(stat -c '%u' -- "$LOCK_NAMESPACE") || die "cannot stat lock namespace"
lock_mode=$(stat -c '%a' -- "$LOCK_NAMESPACE") || die "cannot stat lock namespace"
[ "$lock_owner" = "$(id -u)" ] && [ "$lock_mode" = 700 ] ||
    die "lock namespace must be owned by this user with mode 0700"
LOCK_PATH="$LOCK_NAMESPACE/$POLICY_NAME.lock"
if [ -e "$LOCK_PATH" ] || [ -L "$LOCK_PATH" ]; then
    [ -f "$LOCK_PATH" ] && [ ! -L "$LOCK_PATH" ] ||
        die "policy lock is not a regular non-symlink file"
fi
old_umask=$(umask)
umask 077
exec 9>"$LOCK_PATH" || die "cannot open policy lock"
umask "$old_umask"
flock -n 9 || die "another transaction holds $POLICY_NAME"
check_identity || die "policy identity changed while acquiring the lock"
if [ -n "$AFTER_LOCK_HELPER" ]; then
    "$AFTER_LOCK_HELPER" || die "test after-lock helper failed"
fi
if ! verify_expected_scope; then
    current_scope=unreadable
    if IFS= read -r current_scope <"$POLICY/related_cpus"; then
        current_scope=$(normalize_space_list "$current_scope") || current_scope=malformed
    fi
    die "policy scope changed after lock: expected=[$EXPECTED_SCOPE], actual=[$current_scope]"
fi

# The reads used by status/plan and early rejection happened before acquiring
# the cooperative policy lock.  They are not an authoritative transaction
# snapshot: another instance may have been holding a temporary profile then.
# Reload every mutable control and every bound after the lock/scope checks, and
# rerun preflight, before declaring these values to be the restoration target.
DRIVER=$(read_required scaling_driver) || exit $?
GOVERNOR=$(read_required scaling_governor) || exit $?
AVAILABLE_GOVERNORS=$(read_required scaling_available_governors) || exit $?
MIN_KHZ=$(read_required scaling_min_freq) || exit $?
MAX_KHZ=$(read_required scaling_max_freq) || exit $?
CPUINFO_MIN_KHZ=$(read_required cpuinfo_min_freq) || exit $?
CPUINFO_MAX_KHZ=$(read_required cpuinfo_max_freq) || exit $?
EPP=unsupported
AVAILABLE_EPP=
if [ -e "$POLICY/energy_performance_preference" ]; then
    EPP=$(read_required energy_performance_preference) || exit $?
    AVAILABLE_EPP=$(read_required energy_performance_available_preferences) || exit $?
fi
for value in "$MIN_KHZ" "$MAX_KHZ" "$CPUINFO_MIN_KHZ" "$CPUINFO_MAX_KHZ"; do
    [[ $value =~ ^[0-9]+$ ]] || die "a post-lock frequency field is not an integer"
done
(( 10#$CPUINFO_MIN_KHZ <= 10#$MIN_KHZ &&
   10#$MIN_KHZ <= 10#$MAX_KHZ &&
   10#$MAX_KHZ <= 10#$CPUINFO_MAX_KHZ )) ||
    die "post-lock frequency bounds are internally inconsistent"
preflight_profile

ORIG_GOVERNOR=$GOVERNOR
ORIG_EPP=$EPP
ORIG_MIN_KHZ=$MIN_KHZ
ORIG_MAX_KHZ=$MAX_KHZ
BOUNDS_GOVERNOR=$ORIG_GOVERNOR
if [ "$BOUNDS_GOVERNOR" = performance ]; then
    BOUNDS_GOVERNOR=powersave
fi
TX_ACTIVE=1
FINALIZING=0
CHILD_PID=
CHILD_STARTING=0
PENDING_SIGNAL_RC=
PENDING_SIGNAL_NAME=

write_value() {
    local field=$1 value=$2
    check_identity || {
        echo "cpufreq transaction: policy identity changed before writing $field" >&2
        return 1
    }
    if [ "$TEST_MODE" = 1 ]; then
        "$WRITE_HELPER" "$POLICY/$field" "$value"
    else
        printf '%s\n' "$value" >"$POLICY/$field"
    fi
}

set_verified() {
    local field=$1 wanted=$2 got=unreadable attempt stable=0
    if ! write_value "$field" "$wanted"; then
        echo "cpufreq transaction: write failed: $field=$wanted" >&2
        return 1
    fi
    for ((attempt = 1; attempt <= READBACK_ATTEMPTS; attempt++)); do
        if IFS= read -r got <"$POLICY/$field"; then
            if [ "$got" = "$wanted" ]; then
                stable=$((stable + 1))
                if [ "$stable" -ge "$READBACK_STABLE_SAMPLES" ]; then
                    return 0
                fi
            else
                stable=0
            fi
        else
            got=unreadable
            stable=0
        fi
        [ "$attempt" -eq "$READBACK_ATTEMPTS" ] || sleep "$READBACK_DELAY"
    done
    echo "cpufreq transaction: readback timeout: $field wanted=$wanted last=$got stable_samples=$stable" >&2
    return 1
}

set_bounds() {
    local new_min=$1 new_max=$2 cur_min cur_max
    IFS= read -r cur_min <"$POLICY/scaling_min_freq" || return 1
    IFS= read -r cur_max <"$POLICY/scaling_max_freq" || return 1
    if (( 10#$new_max < 10#$cur_min )); then
        set_verified scaling_min_freq "$new_min" &&
            set_verified scaling_max_freq "$new_max"
    else
        set_verified scaling_max_freq "$new_max" &&
            set_verified scaling_min_freq "$new_min"
    fi
}

verify_profile() {
    local want_gov=$1 want_epp=$2 want_min=$3 want_max=$4
    local got_gov got_epp=unsupported got_min got_max
    check_identity || return 1
    IFS= read -r got_gov <"$POLICY/scaling_governor" || return 1
    IFS= read -r got_min <"$POLICY/scaling_min_freq" || return 1
    IFS= read -r got_max <"$POLICY/scaling_max_freq" || return 1
    if [ "$want_epp" != unsupported ]; then
        IFS= read -r got_epp <"$POLICY/energy_performance_preference" || return 1
    fi
    [ "$got_gov" = "$want_gov" ] && [ "$got_epp" = "$want_epp" ] &&
        [ "$got_min" = "$want_min" ] && [ "$got_max" = "$want_max" ]
}

set_governor_if_needed() {
    local wanted=$1 got
    IFS= read -r got <"$POLICY/scaling_governor" || return 1
    [ "$got" = "$wanted" ] || set_verified scaling_governor "$wanted"
}

stage_bounds_governor() {
    # This is also a compensating write after any timed-out apply.  Do not
    # optimize it away merely because an older value is still visible: a
    # queued policy refresh must observe this newer restoration request.
    set_verified scaling_governor "$BOUNDS_GOVERNOR"
}

stage_bounds_governor_if_needed() {
    set_governor_if_needed "$BOUNDS_GOVERNOR"
}

restore_profile() {
    local rc=0
    # Leave performance before changing bounds.  This avoids coupling an HWP
    # policy-mode transition with an asynchronous frequency-limit refresh.  If
    # performance itself was the locked original, powersave is a verified
    # temporary staging governor.
    # Always restore all four fields: the child may have drifted bounds even in
    # a governor-only transaction.
    stage_bounds_governor || rc=1
    set_bounds "$ORIG_MIN_KHZ" "$ORIG_MAX_KHZ" || rc=1
    # This final write is also compensating: do not let an old visible value
    # suppress the newest restoration request after staging itself timed out.
    set_verified scaling_governor "$ORIG_GOVERNOR" || rc=1
    if [ "$ORIG_EPP" != unsupported ]; then
        set_verified energy_performance_preference "$ORIG_EPP" || rc=1
    fi
    verify_profile "$ORIG_GOVERNOR" "$ORIG_EPP" "$ORIG_MIN_KHZ" "$ORIG_MAX_KHZ" || rc=1
    if [ "$rc" -eq 0 ]; then
        printf 'CPUFREQ_VERIFY phase=restored governor=%s epp=%s min_khz=%s max_khz=%s\n' \
            "$ORIG_GOVERNOR" "$ORIG_EPP" "$ORIG_MIN_KHZ" "$ORIG_MAX_KHZ"
        TX_ACTIVE=0
        return 0
    fi
    return 1
}

finish() {
    local command_rc=$? restore_rc=0
    [ "$FINALIZING" -eq 0 ] || exit 70
    FINALIZING=1
    trap - EXIT
    # A second catchable signal must not interrupt the restoration transaction.
    # SIGKILL and machine failure remain inherently outside a shell's control.
    trap '' INT TERM HUP QUIT
    if [ "$TX_ACTIVE" -eq 1 ]; then
        restore_profile || restore_rc=1
    fi
    if [ "$restore_rc" -ne 0 ]; then
        echo "CPUFREQ_RESTORE_FAILED command_rc=$command_rc policy=$POLICY_NAME" >&2
        exit 70
    fi
    exit "$command_rc"
}

handle_signal() {
    local rc=$1 sig=$2 i grace_steps=40
    if [ "$CHILD_STARTING" -eq 1 ]; then
        PENDING_SIGNAL_RC=$rc
        PENDING_SIGNAL_NAME=$sig
        return 0
    fi
    trap '' INT TERM HUP QUIT
    if [ -n "$CHILD_PID" ]; then
        kill -"$sig" -- "-$CHILD_PID" 2>/dev/null || kill -"$sig" "$CHILD_PID" 2>/dev/null || true
        # An ordinary benchmark gets the historical two-second grace.  A core
        # transaction explicitly authenticates the retained-FD nested chain
        # above and grants each remaining restoration owner another bounded
        # 16 seconds.  One production policy restore has at most five 2-second
        # readback windows, plus one in-flight window and margin; the innermost
        # ordinary child still has only the original two seconds.
        grace_steps=$((grace_steps + TRUSTED_CHILD_TRANSACTIONS * 320))
        for ((i = 0; i < grace_steps; i++)); do
            kill -0 -- "-$CHILD_PID" 2>/dev/null || break
            sleep 0.05
        done
        if kill -0 -- "-$CHILD_PID" 2>/dev/null; then
            if [ "$TRUSTED_CHILD_TRANSACTIONS" -gt 0 ]; then
                echo "cpufreq transaction: trusted nested restoration exceeded bounded grace; escalating process group to KILL" >&2
            else
                echo "cpufreq transaction: child ignored $sig; escalating process group to KILL" >&2
            fi
            kill -KILL -- "-$CHILD_PID" 2>/dev/null || kill -KILL "$CHILD_PID" 2>/dev/null || true
        fi
        wait "$CHILD_PID" 2>/dev/null || true
        CHILD_PID=
    fi
    exit "$rc"
}

trap finish EXIT
trap 'handle_signal 130 INT' INT
trap 'handle_signal 143 TERM' TERM
trap 'handle_signal 129 HUP' HUP
trap 'handle_signal 131 QUIT' QUIT

APPLIED_MIN=$ORIG_MIN_KHZ
APPLIED_MAX=$ORIG_MAX_KHZ
APPLIED_MODE=performance
if [ -n "$TARGET_KHZ" ]; then
    # Establish the requested bounds before entering the HWP performance
    # profile.  The test host exposed a short write-to-policy refresh delay, so
    # the stages are kept separate and exact bounded readback remains
    # authoritative.
    if ! stage_bounds_governor_if_needed; then exit 3; fi
    if ! set_bounds "$TARGET_KHZ" "$TARGET_KHZ"; then exit 3; fi
    APPLIED_MIN=$TARGET_KHZ
    APPLIED_MAX=$TARGET_KHZ
    APPLIED_MODE=fixed
fi
if ! set_verified scaling_governor performance; then exit 3; fi
if [ "$EPP" != unsupported ]; then
    if ! set_verified energy_performance_preference performance; then exit 3; fi
fi
APPLIED_EPP=unsupported
[ "$EPP" = unsupported ] || APPLIED_EPP=performance
if ! verify_profile performance "$APPLIED_EPP" "$APPLIED_MIN" "$APPLIED_MAX"; then
    echo "cpufreq transaction: complete applied profile did not verify" >&2
    exit 3
fi
printf 'CPUFREQ_VERIFY phase=applied mode=%s governor=performance epp=%s min_khz=%s max_khz=%s\n' \
    "$APPLIED_MODE" "$APPLIED_EPP" "$APPLIED_MIN" "$APPLIED_MAX"
printf 'CPUFREQ_NOTE settings_verified=yes hardware_frequency_fixed=no\n'
APPLIED_CUR_KHZ=unavailable
if [ -r "$POLICY/scaling_cur_freq" ]; then
    IFS= read -r APPLIED_CUR_KHZ <"$POLICY/scaling_cur_freq" || APPLIED_CUR_KHZ=unavailable
fi
printf 'CPUFREQ_OBSERVATION scaling_cur_freq_khz=%s pass_criterion=no\n' "$APPLIED_CUR_KHZ"

# A new session lets signal handlers forward to the complete benchmark process
# group.  Close fd 9 in the child so a daemon cannot retain the policy lock.
CHILD_STARTING=1
/usr/bin/env --default-signal=INT,QUIT,HUP,TERM \
    setsid --wait -- "${CHILD[@]}" 9>&- &
CHILD_PID=$!
CHILD_STARTING=0
if [ -n "$PENDING_SIGNAL_RC" ]; then
    handle_signal "$PENDING_SIGNAL_RC" "$PENDING_SIGNAL_NAME"
fi
wait "$CHILD_PID"
CHILD_RC=$?
CHILD_PID=
if verify_profile performance "$APPLIED_EPP" "$APPLIED_MIN" "$APPLIED_MAX"; then
    printf 'CPUFREQ_VERIFY phase=post-command mode=%s profile_unchanged=yes\n' "$APPLIED_MODE"
else
    echo "CPUFREQ_PROFILE_DRIFT command_rc=$CHILD_RC policy=$POLICY_NAME" >&2
    CHILD_RC=71
fi
exit "$CHILD_RC"
