#!/bin/bash
# Transactional CPU-frequency control for every cpufreq policy represented by
# the online SMT threads of one physical core.  Fixed mode additionally performs
# a short, pre-command scaling_cur_freq verification while a SCHED_IDLE keeper
# is active on the target thread; the keeper is stopped before user code starts.
set -u -o pipefail
set -f

die() {
    echo "cpufreq core transaction: $*" >&2
    exit 2
}

usage() {
    cat >&2 <<'EOF'
usage:
  tools/cpufreq-core-transaction.sh --status-core CPU
  tools/cpufreq-core-transaction.sh --plan-fixed-core CPU FREQ_KHZ
  tools/cpufreq-core-transaction.sh --plan-performance-core CPU
  tools/cpufreq-core-transaction.sh --with-fixed-core CPU FREQ_KHZ -- COMMAND [ARG...]
  tools/cpufreq-core-transaction.sh --with-performance-core CPU -- COMMAND [ARG...]
EOF
    exit 2
}

TRUSTED_FD_REENTRY=0
SELF_FD=
TX_FD=
KEEPER_FD=
if [ "${1:-}" = --internal-run ]; then
    [ "${EXITOS_CPUFREQ_CORE_INTERNAL:-0}" = 1 ] ||
        die "internal mode is available only inside a core transaction"
    SELF_FD=${EXITOS_CPUFREQ_CORE_SELF_FD:-}
    TX_FD=${EXITOS_CPUFREQ_CORE_TX_FD:-}
    KEEPER_FD=${EXITOS_CPUFREQ_CORE_KEEPER_FD:-}
    for retained_fd in "$SELF_FD" "$TX_FD" "$KEEPER_FD"; do
        [[ $retained_fd =~ ^[0-9]+$ ]] && [ -r "/proc/self/fd/$retained_fd" ] ||
            die "internal mode is missing a retained executable handle"
    done
    [ "$0" = "/proc/self/fd/$SELF_FD" ] ||
        die "internal mode was not launched through its retained self handle"
    SELF="/proc/self/fd/$SELF_FD"
    DEFAULT_TX="/proc/self/fd/$TX_FD"
    DEFAULT_KEEPER="/proc/self/fd/$KEEPER_FD"
    TRUSTED_FD_REENTRY=1
else
    SELF_PATH=$(readlink -f -- "$0") || die "cannot resolve core transaction entrypoint"
    ROOT=$(cd "$(dirname "$SELF_PATH")/.." && pwd -P) || exit 2
    DEFAULT_TX="$ROOT/tools/cpufreq-transaction.sh"
    DEFAULT_KEEPER="$ROOT/tools/cstate-keeper"
    self_id=$(stat -Lc '%d:%i' -- "$SELF_PATH") ||
        die "cannot retain core transaction identity"
    exec {SELF_FD}<"$SELF_PATH" || die "cannot retain core transaction handle"
    SELF="/proc/self/fd/$SELF_FD"
    [ -r "$SELF" ] && [ -x "$SELF" ] &&
    [ "$(stat -Lc '%d:%i' -- "$SELF")" = "$self_id" ] ||
        die "core transaction entrypoint changed while retaining it"
fi

ACTION=
RUN_MODE=
case "${1:-}" in
  --status-core) ACTION=status ;;
  --plan-fixed-core) ACTION=plan; RUN_MODE=fixed ;;
  --plan-performance-core) ACTION=plan; RUN_MODE=performance ;;
  --with-fixed-core) ACTION=with; RUN_MODE=fixed ;;
  --with-performance-core) ACTION=with; RUN_MODE=performance ;;
  --internal-run)
    [ "${EXITOS_CPUFREQ_CORE_INTERNAL:-0}" = 1 ] ||
        die "internal mode is available only inside a core transaction"
    ACTION=internal
    ;;
  *) usage ;;
esac
shift

if [ "$ACTION" = internal ]; then
    RUN_MODE=${1:-}
    case "$RUN_MODE" in fixed|performance) ;; *) usage ;; esac
    shift
fi

CPU_RAW=${1:-}
[[ $CPU_RAW =~ ^[0-9]+$ ]] || usage
((${#CPU_RAW} <= 4)) || die "CPU decimal input is too long"
CPU=$((10#$CPU_RAW))
shift
(( CPU <= 1023 )) || die "CPU is outside the supported SCHED_IDLE keeper range"

TARGET_KHZ=
if [ "$RUN_MODE" = fixed ]; then
    TARGET_RAW=${1:-}
    [[ $TARGET_RAW =~ ^[0-9]+$ ]] || usage
    ((${#TARGET_RAW} <= 9)) || die "FREQ_KHZ decimal input is too long"
    TARGET_KHZ=$((10#$TARGET_RAW))
    shift
    (( TARGET_KHZ > 0 && TARGET_KHZ <= 100000000 )) ||
        die "FREQ_KHZ is outside the accepted numeric range"
elif [ "$ACTION" = internal ]; then
    [ "${1:-}" = 0 ] || usage
    shift
fi

EXPECTED_FINGERPRINT=
EXPECTED_APPLIED=
if [ "$ACTION" = internal ]; then
    [ "$#" -ge 3 ] || usage
    EXPECTED_FINGERPRINT=$1
    EXPECTED_APPLIED=$2
    shift 2
fi

declare -a CHILD=()
case "$ACTION" in
  with|internal)
    [ "${1:-}" = -- ] || usage
    shift
    [ "$#" -ge 1 ] || usage
    CHILD=("$@")
    ;;
  *) [ "$#" -eq 0 ] || usage ;;
esac

TEST_MODE=${EXITOS_CPUFREQ_CORE_TEST_MODE:-0}
case "$TEST_MODE" in 0|1) ;; *) die "EXITOS_CPUFREQ_CORE_TEST_MODE must be 0 or 1" ;; esac
SYSROOT_INPUT=${EXITOS_CPU_SYSFS_ROOT:-/sys/devices/system/cpu}
TX_INPUT=${EXITOS_CPUFREQ_TRANSACTION_BIN:-$DEFAULT_TX}
KEEPER_INPUT=${EXITOS_KEEPER_BIN:-$DEFAULT_KEEPER}
TMPROOT=${EXITOS_CPUFREQ_CORE_TMPROOT:-/tmp}

if [ "$TEST_MODE" = 1 ]; then
    [ "$SYSROOT_INPUT" != /sys ] && [[ $SYSROOT_INPUT != /sys/* ]] ||
        die "test mode refuses a real /sys root"
    [ "$TMPROOT" != / ] && [ "$TMPROOT" != /sys ] && [[ $TMPROOT != /sys/* ]] ||
        die "test mode refuses an unsafe temporary root"
else
    [ "$SYSROOT_INPUT" = /sys/devices/system/cpu ] ||
        die "a nonstandard sysfs root is accepted only by the pure unit test"
    [ "$TX_INPUT" = "$DEFAULT_TX" ] ||
        die "a transaction override is accepted only by the pure unit test"
    [ "$KEEPER_INPUT" = "$DEFAULT_KEEPER" ] ||
        die "a keeper override is accepted only by the pure unit test"
    [ -z "${EXITOS_CPUFREQ_CORE_TMPROOT+x}" ] ||
        die "a temporary-root override is accepted only by the pure unit test"
fi

SYSROOT=$(readlink -f -- "$SYSROOT_INPUT") || die "cannot resolve CPU sysfs root"
[ -d "$SYSROOT" ] && [ ! -L "$SYSROOT" ] || die "CPU sysfs root is not a real directory"
TMPROOT=$(readlink -f -- "$TMPROOT") || die "cannot resolve verifier temporary root"
[ -d "$TMPROOT" ] && [ ! -L "$TMPROOT" ] ||
    die "verifier temporary root is not a real directory"
if [ "$TEST_MODE" = 1 ]; then
    [ "$SYSROOT" != /sys ] && [[ $SYSROOT != /sys/* ]] ||
        die "test mode sysfs root resolves into real /sys"
    [ "$TMPROOT" != /sys ] && [[ $TMPROOT != /sys/* ]] ||
        die "test mode verifier temporary root resolves into real /sys"
fi
if [ "$TRUSTED_FD_REENTRY" = 1 ]; then
    TX_BIN="/proc/self/fd/$TX_FD"
    KEEPER_BIN="/proc/self/fd/$KEEPER_FD"
    [ -r "$TX_BIN" ] && [ -x "$TX_BIN" ] ||
        die "retained per-policy transaction is not executable"
    [ -r "$KEEPER_BIN" ] && [ -x "$KEEPER_BIN" ] ||
        die "retained SCHED_IDLE keeper is not executable"
else
    TX_PATH=$(readlink -f -- "$TX_INPUT") || die "cannot resolve per-policy transaction"
    KEEPER_PATH=$(readlink -f -- "$KEEPER_INPUT") || die "cannot resolve SCHED_IDLE keeper"
    tx_id=$(stat -Lc '%d:%i' -- "$TX_PATH") || die "cannot retain per-policy transaction identity"
    keeper_id=$(stat -Lc '%d:%i' -- "$KEEPER_PATH") || die "cannot retain SCHED_IDLE keeper identity"
    exec {TX_FD}<"$TX_PATH" || die "cannot retain per-policy transaction handle"
    exec {KEEPER_FD}<"$KEEPER_PATH" || die "cannot retain SCHED_IDLE keeper handle"
    TX_BIN="/proc/self/fd/$TX_FD"
    KEEPER_BIN="/proc/self/fd/$KEEPER_FD"
    [ -r "$TX_BIN" ] && [ -x "$TX_BIN" ] &&
    [ "$(stat -Lc '%d:%i' -- "$TX_BIN")" = "$tx_id" ] ||
        die "per-policy transaction changed while retaining it"
    [ -r "$KEEPER_BIN" ] && [ -x "$KEEPER_BIN" ] &&
    [ "$(stat -Lc '%d:%i' -- "$KEEPER_BIN")" = "$keeper_id" ] ||
        die "SCHED_IDLE keeper changed while retaining it"
fi

LIST_ERROR=
PARSED_CANONICAL=
PARSED_NORMALIZED=
declare -a PARSED_CPUS=()

parse_cpu_list() {
    local raw=$1 label=$2 token lo hi c normalized="" canonical=""
    local -a tokens=() sorted=()
    local -A seen=()
    LIST_ERROR=""
    PARSED_CANONICAL=""
    PARSED_NORMALIZED=""
    PARSED_CPUS=()
    raw=${raw//$'\n'/ }
    raw=${raw//$'\r'/ }
    raw=${raw//$'\t'/ }
    raw=${raw//,/ }
    read -r -a tokens <<<"$raw"
    [ "${#tokens[@]}" -gt 0 ] || {
        LIST_ERROR="$label is empty"
        return 1
    }
    for token in "${tokens[@]}"; do
        if [[ $token =~ ^([0-9]+)$ ]]; then
            ((${#BASH_REMATCH[1]} <= 4)) || {
                LIST_ERROR="$label contains an overlong CPU"
                return 1
            }
            lo=$((10#${BASH_REMATCH[1]}))
            hi=$lo
        elif [[ $token =~ ^([0-9]+)-([0-9]+)$ ]]; then
            ((${#BASH_REMATCH[1]} <= 4 && ${#BASH_REMATCH[2]} <= 4)) || {
                LIST_ERROR="$label contains an overlong CPU range"
                return 1
            }
            lo=$((10#${BASH_REMATCH[1]}))
            hi=$((10#${BASH_REMATCH[2]}))
            (( lo <= hi )) || {
                LIST_ERROR="$label contains a reversed CPU range"
                return 1
            }
        else
            LIST_ERROR="$label contains malformed token '$token'"
            return 1
        fi
        (( hi <= 1023 )) || {
            LIST_ERROR="$label contains CPU $hi outside supported range 0-1023"
            return 1
        }
        normalized+="${normalized:+ }$token"
        for ((c = lo; c <= hi; c++)); do
            [ -z "${seen[$c]+x}" ] || {
                LIST_ERROR="$label contains duplicate/overlapping CPU $c"
                return 1
            }
            seen[$c]=1
            PARSED_CPUS+=("$c")
        done
    done
    mapfile -t sorted < <(printf '%s\n' "${PARSED_CPUS[@]}" | LC_ALL=C sort -n)
    PARSED_CPUS=("${sorted[@]}")
    for c in "${PARSED_CPUS[@]}"; do canonical+="${canonical:+,}$c"; done
    PARSED_CANONICAL=$canonical
    PARSED_NORMALIZED=$normalized
}

list_has_cpu() {
    local wanted=$1 c
    shift
    for c in "$@"; do [ "$c" = "$wanted" ] && return 0; done
    return 1
}

read_line() {
    local path=$1 outvar=$2 value
    [ -r "$path" ] || return 1
    IFS= read -r value <"$path" || return 1
    [ -n "$value" ] || return 1
    printf -v "$outvar" '%s' "$value"
}

declare -a SIBLINGS=()
SIBLING_TOKEN=
declare -A ONLINE_SET=()
declare -A SIB_POLICY_INDEX=()
declare -A POLICY_INDEX_BY_PATH=()
declare -A POLICY_SCOPE_OWNER=()
declare -a POLICY_PATH=() POLICY_NAME=() POLICY_ID=() POLICY_REP_CPU=()
declare -a POLICY_SCOPE_NORMALIZED=() POLICY_SCOPE_CANONICAL=()
declare -a POLICY_GOVERNOR=() POLICY_EPP=() POLICY_MIN=() POLICY_MAX=()
declare -a POLICY_CUR=()
POLICY_COUNT=0
FINGERPRINT=

reset_topology_state() {
    SIBLINGS=()
    SIBLING_TOKEN=
    ONLINE_SET=()
    SIB_POLICY_INDEX=()
    POLICY_INDEX_BY_PATH=()
    POLICY_SCOPE_OWNER=()
    POLICY_PATH=(); POLICY_NAME=(); POLICY_ID=(); POLICY_REP_CPU=()
    POLICY_SCOPE_NORMALIZED=(); POLICY_SCOPE_CANONICAL=()
    POLICY_GOVERNOR=(); POLICY_EPP=(); POLICY_MIN=(); POLICY_MAX=(); POLICY_CUR=()
    POLICY_COUNT=0
    FINGERPRINT=
}

resolve_core() {
    local raw online_c sibling topo_raw topo_canon policy policy_dir policy_base
    local related_raw related_norm related_canon related_c policy_index id
    local governor epp min max cur field
    local -a related_cpus=()
    reset_topology_state

    read_line "$SYSROOT/online" raw || die "cannot read global online CPU list"
    parse_cpu_list "$raw" online || die "$LIST_ERROR"
    for online_c in "${PARSED_CPUS[@]}"; do ONLINE_SET[$online_c]=1; done
    [ -n "${ONLINE_SET[$CPU]+x}" ] || die "target CPU $CPU is offline"

    [ -d "$SYSROOT/cpu$CPU/topology" ] || die "target CPU $CPU has no topology"
    read_line "$SYSROOT/cpu$CPU/topology/thread_siblings_list" raw ||
        die "target CPU $CPU has no thread_siblings_list"
    parse_cpu_list "$raw" thread_siblings_list || die "$LIST_ERROR"
    SIBLINGS=("${PARSED_CPUS[@]}")
    SIBLING_TOKEN=$PARSED_CANONICAL
    list_has_cpu "$CPU" "${SIBLINGS[@]}" ||
        die "target CPU $CPU is absent from its thread_siblings_list"

    for sibling in "${SIBLINGS[@]}"; do
        [ -n "${ONLINE_SET[$sibling]+x}" ] || die "SMT sibling CPU $sibling is offline"
        [ -d "$SYSROOT/cpu$sibling/topology" ] ||
            die "SMT sibling CPU $sibling has no topology"
        if [ -r "$SYSROOT/cpu$sibling/online" ]; then
            read_line "$SYSROOT/cpu$sibling/online" raw ||
                die "cannot read online state for sibling CPU $sibling"
            [ "$raw" = 1 ] || die "SMT sibling CPU $sibling is offline"
        fi
        read_line "$SYSROOT/cpu$sibling/topology/thread_siblings_list" topo_raw ||
            die "SMT sibling CPU $sibling has no thread_siblings_list"
        parse_cpu_list "$topo_raw" "CPU $sibling thread_siblings_list" || die "$LIST_ERROR"
        topo_canon=$PARSED_CANONICAL
        [ "$topo_canon" = "$SIBLING_TOKEN" ] ||
            die "asymmetric SMT topology for CPU $sibling: expected=$SIBLING_TOKEN actual=$topo_canon"

        policy=$(readlink -f -- "$SYSROOT/cpu$sibling/cpufreq") ||
            die "CPU $sibling has no canonical cpufreq policy"
        policy_dir=${policy%/*}
        policy_base=${policy##*/}
        [ "$policy_dir" = "$SYSROOT/cpufreq" ] &&
        [[ $policy_base =~ ^policy[0-9]+$ ]] && [ -d "$policy" ] ||
            die "CPU $sibling cpufreq policy is outside canonical policyN namespace"
        id=$(stat -Lc '%d:%i' -- "$policy") || die "cannot retain identity for $policy_base"
        read_line "$policy/related_cpus" related_raw ||
            die "$policy_base is missing related_cpus"
        parse_cpu_list "$related_raw" "$policy_base related_cpus" || die "$LIST_ERROR"
        related_norm=$PARSED_NORMALIZED
        related_canon=$PARSED_CANONICAL
        related_cpus=("${PARSED_CPUS[@]}")
        list_has_cpu "$sibling" "${related_cpus[@]}" ||
            die "$policy_base related_cpus omits sibling CPU $sibling"

        if [ -n "${POLICY_INDEX_BY_PATH[$policy]+x}" ]; then
            policy_index=${POLICY_INDEX_BY_PATH[$policy]}
            [ "${POLICY_ID[$policy_index]}" = "$id" ] &&
            [ "${POLICY_SCOPE_NORMALIZED[$policy_index]}" = "$related_norm" ] ||
                die "duplicate policy $policy_base changed identity or exact related_cpus"
            SIB_POLICY_INDEX[$sibling]=$policy_index
            continue
        fi

        policy_index=$POLICY_COUNT
        for related_c in "${related_cpus[@]}"; do
            if [ -n "${POLICY_SCOPE_OWNER[$related_c]+x}" ]; then
                die "distinct cpufreq policies overlap on related CPU $related_c"
            fi
        done
        for related_c in "${related_cpus[@]}"; do POLICY_SCOPE_OWNER[$related_c]=$policy_index; done

        read_line "$policy/scaling_governor" governor ||
            die "$policy_base is missing scaling_governor"
        read_line "$policy/scaling_min_freq" min || die "$policy_base is missing scaling_min_freq"
        read_line "$policy/scaling_max_freq" max || die "$policy_base is missing scaling_max_freq"
        [[ $min =~ ^[0-9]+$ && $max =~ ^[0-9]+$ ]] ||
            die "$policy_base has nonnumeric frequency bounds"
        (( 10#$min <= 10#$max )) || die "$policy_base has reversed frequency bounds"
        epp=unsupported
        if [ -e "$policy/energy_performance_preference" ]; then
            read_line "$policy/energy_performance_preference" epp ||
                die "$policy_base has unreadable EPP"
        fi
        cur=unavailable
        if [ -e "$policy/scaling_cur_freq" ]; then
            read_line "$policy/scaling_cur_freq" cur ||
                die "$policy_base has unreadable scaling_cur_freq"
        fi

        POLICY_INDEX_BY_PATH[$policy]=$policy_index
        SIB_POLICY_INDEX[$sibling]=$policy_index
        POLICY_PATH[$policy_index]=$policy
        POLICY_NAME[$policy_index]=$policy_base
        POLICY_ID[$policy_index]=$id
        POLICY_REP_CPU[$policy_index]=$sibling
        POLICY_SCOPE_NORMALIZED[$policy_index]=$related_norm
        POLICY_SCOPE_CANONICAL[$policy_index]=$related_canon
        POLICY_GOVERNOR[$policy_index]=$governor
        POLICY_EPP[$policy_index]=$epp
        POLICY_MIN[$policy_index]=$min
        POLICY_MAX[$policy_index]=$max
        POLICY_CUR[$policy_index]=$cur
        POLICY_COUNT=$((POLICY_COUNT + 1))
    done

    [ "$POLICY_COUNT" -gt 0 ] || die "no cpufreq policies cover target core"
    FINGERPRINT="siblings=$SIBLING_TOKEN"
    for ((policy_index = 0; policy_index < POLICY_COUNT; policy_index++)); do
        FINGERPRINT+=";${POLICY_NAME[$policy_index]}:${POLICY_ID[$policy_index]}:${POLICY_SCOPE_CANONICAL[$policy_index]}"
    done
}

print_boundary() {
    printf 'CPUFREQ_CORE_BOUNDARY p_state_policies=all-sibling-policies core_cstate=uncontrolled package_cstate=uncontrolled turbo=uncontrolled thermal=uncontrolled actual_frequency_scope=pre-command-readback-only\n'
}

print_policy_records() {
    local i scope
    for ((i = 0; i < POLICY_COUNT; i++)); do
        scope=${POLICY_SCOPE_NORMALIZED[$i]// /,}
        printf 'CPUFREQ_CORE_POLICY index=%s representative_cpu=%s policy=%s related_cpus=%s governor=%s epp=%s min_khz=%s max_khz=%s cur_khz=%s\n' \
            "$((i + 1))" "${POLICY_REP_CPU[$i]}" "${POLICY_NAME[$i]}" "$scope" \
            "${POLICY_GOVERNOR[$i]}" "${POLICY_EPP[$i]}" \
            "${POLICY_MIN[$i]}" "${POLICY_MAX[$i]}" "${POLICY_CUR[$i]}"
    done
}

tolerance_for() {
    local target=$1 tolerance
    tolerance=$(((target + 99) / 100))
    (( tolerance >= 10000 )) || tolerance=10000
    printf '%s\n' "$tolerance"
}

build_expected_applied() {
    local i epp min max token=""
    for ((i = 0; i < POLICY_COUNT; i++)); do
        epp=unsupported
        [ "${POLICY_EPP[$i]}" = unsupported ] || epp=performance
        if [ "$RUN_MODE" = fixed ]; then
            min=$TARGET_KHZ
            max=$TARGET_KHZ
        else
            min=${POLICY_MIN[$i]}
            max=${POLICY_MAX[$i]}
        fi
        token+="${token:+;}${POLICY_NAME[$i]}:performance:$epp:$min:$max"
    done
    printf '%s\n' "$token"
}

build_current_profile_token() {
    local i token=""
    for ((i = 0; i < POLICY_COUNT; i++)); do
        token+="${token:+;}${POLICY_NAME[$i]}:${POLICY_GOVERNOR[$i]}:${POLICY_EPP[$i]}:${POLICY_MIN[$i]}:${POLICY_MAX[$i]}"
    done
    printf '%s\n' "$token"
}

resolve_core

case "$ACTION" in
  status)
    printf 'CPUFREQ_CORE_STATUS target_cpu=%s siblings=%s policy_count=%s\n' \
        "$CPU" "$SIBLING_TOKEN" "$POLICY_COUNT"
    print_policy_records
    print_boundary
    exit 0
    ;;
  plan)
    printf 'CPUFREQ_CORE_PLAN mode=%s target_cpu=%s siblings=%s policy_count=%s writes=no\n' \
        "$RUN_MODE" "$CPU" "$SIBLING_TOKEN" "$POLICY_COUNT"
    print_policy_records
    if [ "$RUN_MODE" = fixed ]; then
        TOLERANCE_KHZ=$(tolerance_for "$TARGET_KHZ")
        printf 'CPUFREQ_CORE_REQUEST mode=fixed target_khz=%s tolerance_khz=%s stable_samples=3 actual_frequency_verification=required keeper_cpu=%s\n' \
            "$TARGET_KHZ" "$TOLERANCE_KHZ" "$CPU"
    else
        printf 'CPUFREQ_CORE_REQUEST mode=performance governor=performance epp=performance bounds=preserve-per-policy actual_frequency_fixed=no\n'
    fi
    print_boundary
    exit 0
    ;;
esac

if [ "$ACTION" = internal ]; then
    [ "$FINGERPRINT" = "$EXPECTED_FINGERPRINT" ] || {
        echo "CPUFREQ_CORE_TOPOLOGY_DRIFT expected=[$EXPECTED_FINGERPRINT] actual=[$FINGERPRINT]" >&2
        exit 4
    }
    CURRENT_APPLIED=$(build_current_profile_token)
    [ "$CURRENT_APPLIED" = "$EXPECTED_APPLIED" ] || {
        echo "CPUFREQ_CORE_PROFILE_MISMATCH expected=[$EXPECTED_APPLIED] actual=[$CURRENT_APPLIED]" >&2
        exit 4
    }

    if [ "$RUN_MODE" = performance ]; then
        printf 'CPUFREQ_CORE_VERIFY mode=performance target_cpu=%s siblings=%s policies=%s control_profiles_verified=yes actual_frequency_fixed=no keeper_started=no\n' \
            "$CPU" "$SIBLING_TOKEN" "$POLICY_COUNT"
        print_boundary
        unset EXITOS_CPUFREQ_CORE_INTERNAL EXITOS_CPUFREQ_CORE_SELF_FD \
            EXITOS_CPUFREQ_CORE_TX_FD EXITOS_CPUFREQ_CORE_KEEPER_FD
        exec {SELF_FD}<&-
        exec {TX_FD}<&-
        exec {KEEPER_FD}<&-
        exec "${CHILD[@]}"
    fi

    TOLERANCE_KHZ=$(tolerance_for "$TARGET_KHZ")
    SAMPLE_ATTEMPTS=101
    SAMPLE_DELAY=0.01
    if [ "$TEST_MODE" = 1 ]; then
        SAMPLE_ATTEMPTS=20
        SAMPLE_DELAY=0.002
    fi
    STABLE_REQUIRED=3
    KEEPER_PID=
    VERIFY_DIR=

    cleanup_verifier() {
        local ignored
        if [ -n "$KEEPER_PID" ]; then
            kill -TERM "$KEEPER_PID" 2>/dev/null || true
            if wait "$KEEPER_PID" 2>/dev/null; then ignored=0; else ignored=$?; fi
            KEEPER_PID=
        fi
        if [ -n "$VERIFY_DIR" ] && [ -d "$VERIFY_DIR" ] && [ ! -L "$VERIFY_DIR" ]; then
            rm -f -- "$VERIFY_DIR/keeper.out"
            rmdir -- "$VERIFY_DIR" 2>/dev/null || true
        fi
    }
    verifier_signal() {
        local rc=$1
        trap '' INT TERM HUP QUIT
        cleanup_verifier
        exit "$rc"
    }
    trap cleanup_verifier EXIT
    trap 'verifier_signal 130' INT
    trap 'verifier_signal 143' TERM
    trap 'verifier_signal 129' HUP
    trap 'verifier_signal 131' QUIT

    [ -d "$TMPROOT" ] && [ ! -L "$TMPROOT" ] ||
        die "temporary root is not a real directory"
    VERIFY_DIR=$(mktemp -d "$TMPROOT/exitos-cpufreq-core.XXXXXX") ||
        die "cannot create private verifier directory"
    chmod 0700 "$VERIFY_DIR" || die "cannot protect verifier directory"
    /usr/bin/env --default-signal=INT,QUIT,HUP,TERM \
        "$KEEPER_BIN" "$CPU" >"$VERIFY_DIR/keeper.out" 2>&1 &
    KEEPER_PID=$!
    ready=0
    for ((attempt = 0; attempt < 200; attempt++)); do
        if [ "$(grep -Fxc "READY cpu=$CPU policy=SCHED_IDLE" "$VERIFY_DIR/keeper.out" 2>/dev/null || true)" -eq 1 ]; then
            ready=1
            break
        fi
        kill -0 "$KEEPER_PID" 2>/dev/null || break
        sleep 0.005
    done
    if [ "$ready" -ne 1 ] || ! kill -0 "$KEEPER_PID" 2>/dev/null; then
        echo "cpufreq core transaction: SCHED_IDLE verifier did not become ready" >&2
        sed -n '1,40p' "$VERIFY_DIR/keeper.out" >&2
        exit 4
    fi

    stable=0
    last_sample=unavailable
    for ((attempt = 1; attempt <= SAMPLE_ATTEMPTS; attempt++)); do
        all_match=1
        sample=""
        for sibling in "${SIBLINGS[@]}"; do
            policy_index=${SIB_POLICY_INDEX[$sibling]}
            cur=unreadable
            if ! read_line "${POLICY_PATH[$policy_index]}/scaling_cur_freq" cur ||
               [[ ! $cur =~ ^[0-9]+$ ]] || ((${#cur} > 9)); then
                all_match=0
            else
                cur_value=$((10#$cur))
                delta=$((cur_value - TARGET_KHZ))
                (( delta >= 0 )) || delta=$((-delta))
                (( delta <= TOLERANCE_KHZ )) || all_match=0
            fi
            sample+="${sample:+,}$sibling:$cur"
        done
        last_sample=$sample
        if [ "$all_match" -eq 1 ]; then
            stable=$((stable + 1))
            [ "$stable" -lt "$STABLE_REQUIRED" ] || break
        else
            stable=0
        fi
        [ "$attempt" -eq "$SAMPLE_ATTEMPTS" ] || sleep "$SAMPLE_DELAY"
    done
    if [ "$stable" -lt "$STABLE_REQUIRED" ]; then
        echo "CPUFREQ_CORE_ACTUAL_MISMATCH target_khz=$TARGET_KHZ tolerance_khz=$TOLERANCE_KHZ last_actual_khz=$last_sample stable_samples=$stable" >&2
        exit 4
    fi

    kill -TERM "$KEEPER_PID" 2>/dev/null || {
        echo "cpufreq core transaction: cannot stop SCHED_IDLE verifier" >&2
        exit 4
    }
    if wait "$KEEPER_PID"; then keeper_rc=0; else keeper_rc=$?; fi
    KEEPER_PID=
    [ "$keeper_rc" -eq 0 ] || {
        echo "cpufreq core transaction: SCHED_IDLE verifier exited $keeper_rc" >&2
        exit 4
    }
    rm -f -- "$VERIFY_DIR/keeper.out"
    rmdir -- "$VERIFY_DIR" || die "cannot remove verifier directory"
    VERIFY_DIR=
    trap - EXIT INT TERM HUP QUIT
    printf 'CPUFREQ_CORE_VERIFY mode=fixed target_cpu=%s siblings=%s target_khz=%s tolerance_khz=%s stable_samples=%s actual_khz=%s actual_frequency_verified=yes verification_scope=pre-command keeper_stopped_before_command=yes\n' \
        "$CPU" "$SIBLING_TOKEN" "$TARGET_KHZ" "$TOLERANCE_KHZ" \
        "$STABLE_REQUIRED" "$last_sample"
    print_boundary
    unset EXITOS_CPUFREQ_CORE_INTERNAL EXITOS_CPUFREQ_CORE_SELF_FD \
        EXITOS_CPUFREQ_CORE_TX_FD EXITOS_CPUFREQ_CORE_KEEPER_FD
    exec {SELF_FD}<&-
    exec {TX_FD}<&-
    exec {KEEPER_FD}<&-
    exec "${CHILD[@]}"
fi

# Parent mode snapshots the original profiles above, then composes one nested
# per-policy transaction.  The first canonical policy is outermost: if a later
# policy setup fails, every already-applied outer layer still verifies/restores.
EXPECTED_APPLIED=$(build_expected_applied)
INTERNAL_FREQ=0
[ "$RUN_MODE" = fixed ] && INTERNAL_FREQ=$TARGET_KHZ
declare -a CHAIN=(/bin/bash "$SELF" --internal-run "$RUN_MODE" "$CPU" "$INTERNAL_FREQ" \
    "$FINGERPRINT" "$EXPECTED_APPLIED" -- "${CHILD[@]}")
export EXITOS_CPUFREQ_CORE_INTERNAL=1
export EXITOS_CPUFREQ_CORE_SELF_FD=$SELF_FD
export EXITOS_CPUFREQ_CORE_TX_FD=$TX_FD
export EXITOS_CPUFREQ_CORE_KEEPER_FD=$KEEPER_FD
trusted_child_transactions=0
for ((i = POLICY_COUNT - 1; i >= 0; i--)); do
    declare -a TRUSTED_OPTION=()
    if [ "$trusted_child_transactions" -gt 0 ]; then
        TRUSTED_OPTION=(--core-trusted-child-transactions "$trusted_child_transactions")
    fi
    if [ "$RUN_MODE" = fixed ]; then
        CHAIN=(/bin/bash "$TX_BIN" --with-fixed "${POLICY_REP_CPU[$i]}" "$TARGET_KHZ" \
            --expect-policy-cpus "${POLICY_SCOPE_NORMALIZED[$i]}" \
            "${TRUSTED_OPTION[@]}" -- "${CHAIN[@]}")
    else
        CHAIN=(/bin/bash "$TX_BIN" --with-performance "${POLICY_REP_CPU[$i]}" \
            --expect-policy-cpus "${POLICY_SCOPE_NORMALIZED[$i]}" \
            "${TRUSTED_OPTION[@]}" -- "${CHAIN[@]}")
    fi
    trusted_child_transactions=$((trusted_child_transactions + 1))
done

verify_original_profiles() {
    local i path id related normalized got_gov got_epp got_min got_max
    for ((i = 0; i < POLICY_COUNT; i++)); do
        path=${POLICY_PATH[$i]}
        [ -d "$path" ] || return 1
        id=$(stat -Lc '%d:%i' -- "$path") || return 1
        [ "$id" = "${POLICY_ID[$i]}" ] || return 1
        read_line "$path/related_cpus" related || return 1
        parse_cpu_list "$related" "restored ${POLICY_NAME[$i]} related_cpus" || return 1
        normalized=$PARSED_NORMALIZED
        [ "$normalized" = "${POLICY_SCOPE_NORMALIZED[$i]}" ] || return 1
        read_line "$path/scaling_governor" got_gov || return 1
        read_line "$path/scaling_min_freq" got_min || return 1
        read_line "$path/scaling_max_freq" got_max || return 1
        got_epp=unsupported
        if [ "${POLICY_EPP[$i]}" != unsupported ]; then
            read_line "$path/energy_performance_preference" got_epp || return 1
        fi
        [ "$got_gov" = "${POLICY_GOVERNOR[$i]}" ] &&
        [ "$got_epp" = "${POLICY_EPP[$i]}" ] &&
        [ "$got_min" = "${POLICY_MIN[$i]}" ] &&
        [ "$got_max" = "${POLICY_MAX[$i]}" ] || return 1
    done
}

TX_PID=
PENDING_SIGNAL_RC=0
PENDING_SIGNAL_NAME=
SIGNAL_FORWARDED=0
forward_signal() {
    local rc=$1 signal=$2
    if [ "$PENDING_SIGNAL_RC" -eq 0 ]; then
        PENDING_SIGNAL_RC=$rc
        PENDING_SIGNAL_NAME=$signal
    fi
    trap '' INT TERM HUP QUIT
    if [ -n "$TX_PID" ]; then
        # The outer per-policy transaction already owns recursive process-group
        # forwarding for every nested transaction and the eventual user child.
        # Signal that retained process directly: wrapping it in another setsid
        # layer can leave GNU setsid's --wait monitor as the retained PID and
        # detach the actual restoration owner when the monitor is signalled.
        kill -"$signal" "$TX_PID" 2>/dev/null || true
        SIGNAL_FORWARDED=1
    fi
}
trap 'forward_signal 130 INT' INT
trap 'forward_signal 143 TERM' TERM
trap 'forward_signal 129 HUP' HUP
trap 'forward_signal 131 QUIT' QUIT

/usr/bin/env --default-signal=INT,QUIT,HUP,TERM "${CHAIN[@]}" &
TX_PID=$!
if [ "$PENDING_SIGNAL_RC" -ne 0 ] && [ "$SIGNAL_FORWARDED" -eq 0 ]; then
    # A signal can arrive after the job is forked but before $! is retained.
    # Do not let that narrow window continue into the verifier/user command.
    kill -"$PENDING_SIGNAL_NAME" "$TX_PID" 2>/dev/null || true
    SIGNAL_FORWARDED=1
fi
if wait "$TX_PID"; then chain_rc=0; else chain_rc=$?; fi
if [ "$PENDING_SIGNAL_RC" -ne 0 ]; then
    while kill -0 "$TX_PID" 2>/dev/null; do
        if wait "$TX_PID" 2>/dev/null; then ignored=0; else ignored=$?; fi
    done
    chain_rc=$PENDING_SIGNAL_RC
fi
TX_PID=
trap '' INT TERM HUP QUIT

if ! verify_original_profiles; then
    echo "CPUFREQ_CORE_RESTORE_FAILED command_rc=$chain_rc policies=$POLICY_COUNT" >&2
    exit 70
fi
printf 'CPUFREQ_CORE_RESTORE policies=%s original_profiles_verified=yes\n' "$POLICY_COUNT"
exit "$chain_rc"
