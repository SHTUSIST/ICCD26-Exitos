#!/bin/bash
# Atomic fixed-frequency transaction for multiple selected physical cores.
# All cpufreq policy locks are acquired in one deterministic global order before
# any write.  Control settings are exact; scaling_cur_freq is a bounded
# post-keeper/pre-fork observation and is not claimed to remain fixed later.
set -u -o pipefail
set -f

die() {
    echo "cpufreq multicore transaction: $*" >&2
    exit 2
}

usage() {
    cat >&2 <<'EOF'
usage:
  tools/cpufreq-multicore-transaction.sh \
      --with-fixed-cpus SELECTED_CPU_LIST FREQ_KHZ -- COMMAND [ARG...]

SELECTED_CPU_LIST is a comma-separated list containing exactly one online
logical CPU from each requested physical core.  The transaction expands every
selection to its complete online SMT sibling set and refuses overlapping cores.
EOF
    exit 2
}

[ "${1:-}" = --with-fixed-cpus ] || usage
shift
REQUESTED_CPUS=${1:-}
[[ $REQUESTED_CPUS =~ ^[0-9]+(,[0-9]+)*$ ]] || usage
shift
TARGET_RAW=${1:-}
[[ $TARGET_RAW =~ ^[0-9]+$ ]] || usage
((${#TARGET_RAW} <= 9)) || die "FREQ_KHZ decimal input is too long"
TARGET_KHZ=$((10#$TARGET_RAW))
(( TARGET_KHZ > 0 && TARGET_KHZ <= 100000000 )) ||
    die "FREQ_KHZ is outside the accepted numeric range"
shift
[ "${1:-}" = -- ] || usage
shift
[ "$#" -ge 1 ] || usage
declare -a CHILD=("$@")

TEST_MODE=${EXITOS_CPUFREQ_MULTI_TEST_MODE:-0}
case "$TEST_MODE" in 0|1) ;; *) die "EXITOS_CPUFREQ_MULTI_TEST_MODE must be 0 or 1" ;; esac
SYSROOT_INPUT=${EXITOS_CPU_SYSFS_ROOT:-/sys/devices/system/cpu}
LOCK_ROOT_INPUT=${EXITOS_CPUFREQ_LOCK_ROOT:-/run/lock}
WRITE_INPUT=${EXITOS_CPUFREQ_WRITE_HELPER:-}
KEEPER_INPUT=${EXITOS_KEEPER_BIN:-}
AFTER_LOCK_INPUT=${EXITOS_CPUFREQ_MULTI_AFTER_LOCK_HELPER:-}
FLOCK_INPUT=${EXITOS_CPUFREQ_MULTI_FLOCK_BIN:-/usr/bin/flock}

SELF_PATH=$(readlink -f -- "$0") || die "cannot resolve transaction entrypoint"
ROOT=$(cd "$(dirname "$SELF_PATH")/.." && pwd -P) || exit 2
[ -n "$KEEPER_INPUT" ] || KEEPER_INPUT="$ROOT/tools/cstate-keeper"

if [ "$TEST_MODE" = 1 ]; then
    [ "$SYSROOT_INPUT" != /sys ] && [[ $SYSROOT_INPUT != /sys/* ]] ||
        die "test mode refuses a real /sys root"
    [ -n "$WRITE_INPUT" ] && [ -x "$WRITE_INPUT" ] ||
        die "test mode requires an executable fake write helper"
    [ -n "$AFTER_LOCK_INPUT" ] && [ -x "$AFTER_LOCK_INPUT" ] ||
        die "test mode requires an executable after-lock helper"
else
    [ "$SYSROOT_INPUT" = /sys/devices/system/cpu ] ||
        die "a nonstandard sysfs root is accepted only by the pure unit test"
    [ "$LOCK_ROOT_INPUT" = /run/lock ] ||
        die "a nonstandard lock root is accepted only by the pure unit test"
    [ -z "$WRITE_INPUT" ] ||
        die "a write helper is accepted only by the pure unit test"
    [ -z "$AFTER_LOCK_INPUT" ] ||
        die "an after-lock helper is accepted only by the pure unit test"
    [ -z "${EXITOS_CPUFREQ_MULTI_FLOCK_BIN+x}" ] ||
        die "a flock override is accepted only by the pure unit test"
    [ -z "${EXITOS_KEEPER_BIN+x}" ] ||
        die "a keeper override is accepted only by the pure unit test"
    [ "$(id -u)" -eq 0 ] || die "changing cpufreq controls requires root"
fi

SYSROOT=$(readlink -f -- "$SYSROOT_INPUT") || die "cannot resolve CPU sysfs root"
[ -d "$SYSROOT" ] && [ ! -L "$SYSROOT" ] ||
    die "CPU sysfs root is not a real directory"
if [ "$TEST_MODE" = 1 ]; then
    [ "$SYSROOT" != /sys ] && [[ $SYSROOT != /sys/* ]] ||
        die "test mode sysfs root resolves into real /sys"
fi

mkdir -p -- "$LOCK_ROOT_INPUT" || die "cannot access policy lock root"
LOCK_ROOT=$(readlink -f -- "$LOCK_ROOT_INPUT") || die "cannot resolve policy lock root"
[ -d "$LOCK_ROOT" ] && [ ! -L "$LOCK_ROOT" ] ||
    die "policy lock root is not a real directory"
if [ "$TEST_MODE" = 1 ]; then
    [ "$LOCK_ROOT" != /sys ] && [[ $LOCK_ROOT != /sys/* ]] ||
        die "test mode lock root resolves into real /sys"
fi

retain_executable() {
    local input=$1 label=$2 outvar=$3 path identity fd
    path=$(readlink -f -- "$input") || die "cannot resolve $label"
    [ -f "$path" ] && [ -r "$path" ] && [ -x "$path" ] ||
        die "$label is not a readable executable file"
    identity=$(stat -Lc '%d:%i' -- "$path") || die "cannot retain $label identity"
    exec {fd}<"$path" || die "cannot retain $label handle"
    [ "$(stat -Lc '%d:%i' -- "/proc/self/fd/$fd")" = "$identity" ] ||
        die "$label changed while retaining it"
    printf -v "$outvar" '%s' "$fd"
}

KEEPER_FD=
WRITE_FD=
AFTER_LOCK_FD=
FLOCK_FD=
retain_executable "$KEEPER_INPUT" "SCHED_IDLE keeper" KEEPER_FD
KEEPER_BIN="/proc/self/fd/$KEEPER_FD"
retain_executable "$FLOCK_INPUT" "policy lock helper" FLOCK_FD
FLOCK_BIN="/proc/self/fd/$FLOCK_FD"
if [ "$TEST_MODE" = 1 ]; then
    retain_executable "$WRITE_INPUT" "fake sysfs writer" WRITE_FD
    WRITE_BIN="/proc/self/fd/$WRITE_FD"
    retain_executable "$AFTER_LOCK_INPUT" "after-lock test helper" AFTER_LOCK_FD
    AFTER_LOCK_BIN="/proc/self/fd/$AFTER_LOCK_FD"
fi

LIST_ERROR=
PARSED_CANONICAL=
declare -a PARSED_CPUS=()

parse_cpu_list() {
    local raw=$1 label=$2 token lo hi c canonical=""
    local -a tokens=() sorted=()
    local -A seen=()
    LIST_ERROR=""
    PARSED_CANONICAL=""
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
            ((${#BASH_REMATCH[1]} <= 7)) || {
                LIST_ERROR="$label contains an overlong CPU"
                return 1
            }
            lo=$((10#${BASH_REMATCH[1]}))
            hi=$lo
        elif [[ $token =~ ^([0-9]+)-([0-9]+)$ ]]; then
            ((${#BASH_REMATCH[1]} <= 7 && ${#BASH_REMATCH[2]} <= 7)) || {
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
}

list_has_cpu() {
    local wanted=$1 c
    shift
    for c in "$@"; do [ "$c" = "$wanted" ] && return 0; done
    return 1
}

read_line() {
    local path=$1 outvar=$2
    [ -r "$path" ] || return 1
    IFS= read -r "$outvar" <"$path" || return 1
    [ -n "${!outvar}" ] || return 1
}

parse_cpu_list "$REQUESTED_CPUS" "selected CPU list" || die "$LIST_ERROR"
declare -a SELECTED_CPUS=("${PARSED_CPUS[@]}")
SELECTED_LOGICAL_CANONICAL=$PARSED_CANONICAL
SELECTED_CANONICAL=

declare -a LOGICAL_CPUS=() POLICY_PATH=() POLICY_NAME=() POLICY_ID=()
declare -a POLICY_SCOPE=() POLICY_REP_CPU=()
declare -A CPU_CORE_TOKEN=() CPU_EXPECTED_ONLINE=() CPU_POLICY_PATH=() PATH_SEEN=()
POLICY_COUNT=0
LOGICAL_CANONICAL=
POLICY_TOKEN=
TOPOLOGY_FINGERPRINT=

CPU_ONLINE_STATE=
read_cpu_online_state() {
    local cpu=$1 value
    CPU_ONLINE_STATE=
    [ -d "$SYSROOT/cpu$cpu" ] || return 1
    if [ -e "$SYSROOT/cpu$cpu/online" ]; then
        read_line "$SYSROOT/cpu$cpu/online" value || return 1
        case "$value" in 0|1) CPU_ONLINE_STATE=$value ;; *) return 1 ;; esac
        return 0
    fi
    read_line "$SYSROOT/online" value || return 1
    parse_cpu_list "$value" "online CPU list" || return 1
    if list_has_cpu "$cpu" "${PARSED_CPUS[@]}"; then
        CPU_ONLINE_STATE=1
    else
        CPU_ONLINE_STATE=0
    fi
}

cpu_is_online() {
    read_cpu_online_state "$1" && [ "$CPU_ONLINE_STATE" = 1 ]
}

resolve_policy_path() {
    local cpu=$1 outvar=$2 resolved parent base
    resolved=$(readlink -f -- "$SYSROOT/cpu$cpu/cpufreq") || return 1
    parent=${resolved%/*}
    base=${resolved##*/}
    [ "$parent" = "$SYSROOT/cpufreq" ] &&
        [[ $base =~ ^policy(0|[1-9][0-9]*)$ ]] &&
        [ -d "$resolved" ] || return 1
    printf -v "$outvar" '%s' "$resolved"
}

resolve_topology() {
    local selected sibling raw token sibling_raw sibling_token sibling_online
    local path base policy_number id
    local related_raw related_token related_cpu index representative logical_token=""
    local core_record="" policy_record="" name
    local -a siblings=() related=() names=() core_representatives=() sorted_core_representatives=()
    local -A core_seen=() logical_seen=() scope_owner=() path_by_name=()
    local -A path_by_numeric=()

    LOGICAL_CPUS=()
    POLICY_PATH=()
    POLICY_NAME=()
    POLICY_ID=()
    POLICY_SCOPE=()
    POLICY_REP_CPU=()
    CPU_CORE_TOKEN=()
    CPU_EXPECTED_ONLINE=()
    CPU_POLICY_PATH=()
    PATH_SEEN=()
    POLICY_COUNT=0

    for selected in "${SELECTED_CPUS[@]}"; do
        cpu_is_online "$selected" || die "selected CPU $selected is not online"
        read_line "$SYSROOT/cpu$selected/topology/thread_siblings_list" raw ||
            die "selected CPU $selected has no thread_siblings_list"
        parse_cpu_list "$raw" "CPU $selected thread_siblings_list" || die "$LIST_ERROR"
        token=$PARSED_CANONICAL
        siblings=("${PARSED_CPUS[@]}")
        list_has_cpu "$selected" "${siblings[@]}" ||
            die "CPU $selected thread_siblings_list omits itself"
        [ -z "${core_seen[$token]+x}" ] ||
            die "selected physical cores overlap at sibling set $token"
        core_seen[$token]=$selected
        core_record+="${core_record:+;}$selected:$token"
        core_representatives+=("${siblings[0]}")
        for sibling in "${siblings[@]}"; do
            if [ -n "${CPU_CORE_TOKEN[$sibling]+x}" ] &&
               [ "${CPU_CORE_TOKEN[$sibling]}" != "$token" ]; then
                die "distinct physical-core sibling sets overlap on CPU $sibling"
            fi
            CPU_CORE_TOKEN[$sibling]=$token
            read_cpu_online_state "$sibling" ||
                die "cannot determine online state for SMT sibling CPU $sibling"
            sibling_online=$CPU_ONLINE_STATE
            CPU_EXPECTED_ONLINE[$sibling]=$sibling_online
            [ "$sibling_online" = 1 ] || continue
            read_line "$SYSROOT/cpu$sibling/topology/thread_siblings_list" sibling_raw ||
                die "SMT sibling CPU $sibling has no thread_siblings_list"
            parse_cpu_list "$sibling_raw" "CPU $sibling thread_siblings_list" ||
                die "$LIST_ERROR"
            sibling_token=$PARSED_CANONICAL
            [ "$sibling_token" = "$token" ] ||
                die "asymmetric SMT topology for CPU $sibling"
            if [ -z "${logical_seen[$sibling]+x}" ]; then
                logical_seen[$sibling]=1
                LOGICAL_CPUS+=("$sibling")
            fi
        done
    done

    mapfile -t sorted_core_representatives < <(
        printf '%s\n' "${core_representatives[@]}" | LC_ALL=C sort -n
    )
    SELECTED_CANONICAL=
    for selected in "${sorted_core_representatives[@]}"; do
        SELECTED_CANONICAL+="${SELECTED_CANONICAL:+,}$selected"
    done

    mapfile -t LOGICAL_CPUS < <(printf '%s\n' "${LOGICAL_CPUS[@]}" | LC_ALL=C sort -n)
    for sibling in "${LOGICAL_CPUS[@]}"; do
        logical_token+="${logical_token:+,}$sibling"
        resolve_policy_path "$sibling" path ||
            die "CPU $sibling has no canonical cpufreq policy"
        CPU_POLICY_PATH[$sibling]=$path
        base=${path##*/}
        policy_number=${base#policy}
        if [ -z "${PATH_SEEN[$path]+x}" ]; then
            [ -z "${path_by_name[$base]+x}" ] ||
                die "two canonical policy paths share name $base"
            [ -z "${path_by_numeric[$policy_number]+x}" ] ||
                die "two canonical policy paths share numeric ID $policy_number"
            PATH_SEEN[$path]=1
            path_by_name[$base]=$path
            path_by_numeric[$policy_number]=$path
            names+=("$base")
        fi
    done
    LOGICAL_CANONICAL=$logical_token
    mapfile -t names < <(
        for name in "${names[@]}"; do
            printf '%s\t%s\n' "${name#policy}" "$name"
        done | LC_ALL=C sort -t$'\t' -k1,1n -k2,2 | cut -f2-
    )

    for name in "${names[@]}"; do
        path=${path_by_name[$name]}
        id=$(stat -Lc '%d:%i' -- "$path") || die "cannot retain identity for $name"
        read_line "$path/related_cpus" related_raw || die "$name is missing related_cpus"
        parse_cpu_list "$related_raw" "$name related_cpus" || die "$LIST_ERROR"
        related_token=$PARSED_CANONICAL
        related=("${PARSED_CPUS[@]}")
        representative=
        for related_cpu in "${related[@]}"; do
            if [ -n "${scope_owner[$related_cpu]+x}" ]; then
                die "distinct cpufreq policies overlap on related CPU $related_cpu"
            fi
            scope_owner[$related_cpu]=$name
            [ -n "${CPU_CORE_TOKEN[$related_cpu]+x}" ] ||
                die "policy scope escapes selected physical cores at CPU $related_cpu"
            read_cpu_online_state "$related_cpu" ||
                die "cannot determine online state for $name related CPU $related_cpu"
            [ "$CPU_ONLINE_STATE" = 1 ] || continue
            if [ "${CPU_POLICY_PATH[$related_cpu]:-}" = "$path" ] &&
               { [ -z "$representative" ] || (( related_cpu < representative )); }; then
                representative=$related_cpu
            fi
        done
        [ -n "$representative" ] || die "$name covers no selected-core sibling"
        index=$POLICY_COUNT
        POLICY_PATH[$index]=$path
        POLICY_NAME[$index]=$name
        POLICY_ID[$index]=$id
        POLICY_SCOPE[$index]=$related_token
        POLICY_REP_CPU[$index]=$representative
        POLICY_COUNT=$((POLICY_COUNT + 1))
        policy_record+="${policy_record:+;}$name:$id:$related_token"
    done

    for sibling in "${LOGICAL_CPUS[@]}"; do
        [ -n "${scope_owner[$sibling]+x}" ] ||
            die "selected-core sibling CPU $sibling is outside every policy scope"
        [ "${scope_owner[$sibling]}" = "${CPU_POLICY_PATH[$sibling]##*/}" ] ||
            die "CPU $sibling policy link and related_cpus disagree"
    done
    POLICY_TOKEN=$(IFS=,; printf '%s' "${POLICY_NAME[*]}")
    TOPOLOGY_FINGERPRINT="selected=$SELECTED_CANONICAL;logical=$LOGICAL_CANONICAL;cores=$core_record;policies=$policy_record"
}

resolve_topology
PRELOCK_FINGERPRINT=$TOPOLOGY_FINGERPRINT
PRELOCK_POLICY_TOKEN=$POLICY_TOKEN

LOCK_NAMESPACE="$LOCK_ROOT/exitos-cpufreq"
old_umask=$(umask)
umask 077
mkdir -- "$LOCK_NAMESPACE" 2>/dev/null || true
umask "$old_umask"
[ -d "$LOCK_NAMESPACE" ] && [ ! -L "$LOCK_NAMESPACE" ] ||
    die "lock namespace is not a real directory"
[ "$(stat -c '%u:%a' -- "$LOCK_NAMESPACE")" = "$(id -u):700" ] ||
    die "lock namespace must be owned by this user with mode 0700"

declare -a LOCK_FDS=()
for ((i = 0; i < POLICY_COUNT; i++)); do
    lock_path="$LOCK_NAMESPACE/${POLICY_NAME[$i]}.lock"
    if [ -e "$lock_path" ] || [ -L "$lock_path" ]; then
        [ -f "$lock_path" ] && [ ! -L "$lock_path" ] ||
            die "policy lock is not a regular non-symlink file: ${POLICY_NAME[$i]}"
        [ "$(stat -c '%u:%a' -- "$lock_path")" = "$(id -u):600" ] ||
            die "policy lock must be owned by this user with mode 0600: ${POLICY_NAME[$i]}"
    fi
    old_umask=$(umask)
    umask 077
    exec {lock_fd}>"$lock_path" || die "cannot open policy lock: ${POLICY_NAME[$i]}"
    umask "$old_umask"
    LOCK_FDS+=("$lock_fd")
    "$FLOCK_BIN" -n "$lock_fd" || die "another transaction holds ${POLICY_NAME[$i]}"
done
printf 'CPUFREQ_MULTI_LOCKED policies=%s lock_order=global-numeric all_locks_before_writes=yes\n' \
    "$POLICY_TOKEN"

if [ "$TEST_MODE" = 1 ]; then
    "$AFTER_LOCK_BIN" || die "after-lock test helper failed"
fi
resolve_topology
[ "$TOPOLOGY_FINGERPRINT" = "$PRELOCK_FINGERPRINT" ] &&
[ "$POLICY_TOKEN" = "$PRELOCK_POLICY_TOKEN" ] || {
    echo "CPUFREQ_MULTI_TOPOLOGY_DRIFT phase=post-lock" >&2
    exit 4
}

READBACK_ATTEMPTS=201
READBACK_DELAY=0.01
ACTUAL_ATTEMPTS=101
ACTUAL_DELAY=0.01
CHILD_GRACE_STEPS=40
CHILD_GRACE_DELAY=0.05
if [ "$TEST_MODE" = 1 ]; then
    READBACK_ATTEMPTS=80
    READBACK_DELAY=0.002
    ACTUAL_ATTEMPTS=20
    ACTUAL_DELAY=0.002
    CHILD_GRACE_STEPS=30
    CHILD_GRACE_DELAY=0.01
fi
STABLE_REQUIRED=3
TOLERANCE_KHZ=$(((TARGET_KHZ + 99) / 100))
(( TOLERANCE_KHZ >= 10000 )) || TOLERANCE_KHZ=10000

declare -a ORIG_GOVERNOR=() ORIG_EPP=() ORIG_MIN=() ORIG_MAX=()
declare -a CPUINFO_MAX=()

word_present() {
    local words=$1 wanted=$2 word
    for word in $words; do [ "$word" = "$wanted" ] && return 0; done
    return 1
}

for ((i = 0; i < POLICY_COUNT; i++)); do
    path=${POLICY_PATH[$i]}
    read_line "$path/scaling_governor" governor || die "${POLICY_NAME[$i]} has unreadable governor"
    read_line "$path/scaling_available_governors" governors ||
        die "${POLICY_NAME[$i]} has unreadable available governors"
    read_line "$path/energy_performance_preference" epp ||
        die "${POLICY_NAME[$i]} has no readable EPP control"
    read_line "$path/energy_performance_available_preferences" epps ||
        die "${POLICY_NAME[$i]} has no readable EPP choices"
    read_line "$path/scaling_min_freq" min || die "${POLICY_NAME[$i]} has unreadable minimum"
    read_line "$path/scaling_max_freq" max || die "${POLICY_NAME[$i]} has unreadable maximum"
    read_line "$path/cpuinfo_min_freq" info_min || die "${POLICY_NAME[$i]} has unreadable cpuinfo minimum"
    read_line "$path/cpuinfo_max_freq" info_max || die "${POLICY_NAME[$i]} has unreadable cpuinfo maximum"
    for value in "$min" "$max" "$info_min" "$info_max"; do
        [[ $value =~ ^[0-9]+$ ]] && ((${#value} <= 9)) ||
            die "${POLICY_NAME[$i]} has a nonnumeric/overlong frequency field"
    done
    (( 10#$info_min <= 10#$min && 10#$min <= 10#$max && 10#$max <= 10#$info_max )) ||
        die "${POLICY_NAME[$i]} has inconsistent frequency bounds"
    [ "$((10#$info_max))" -eq "$TARGET_KHZ" ] ||
        die "${POLICY_NAME[$i]} cpuinfo_max_freq=$info_max does not equal requested maximum $TARGET_KHZ"
    [ "$governor" != userspace ] ||
        die "${POLICY_NAME[$i]} userspace governor cannot be restored safely"
    word_present "$governors" performance || die "${POLICY_NAME[$i]} lacks performance governor"
    word_present "$governors" powersave || die "${POLICY_NAME[$i]} lacks powersave staging governor"
    word_present "$governors" "$governor" || die "${POLICY_NAME[$i]} original governor is unavailable"
    word_present "$epps" performance || die "${POLICY_NAME[$i]} lacks performance EPP"
    word_present "$epps" "$epp" || die "${POLICY_NAME[$i]} original EPP is unavailable"
    ORIG_GOVERNOR[$i]=$governor
    ORIG_EPP[$i]=$epp
    ORIG_MIN[$i]=$min
    ORIG_MAX[$i]=$max
    CPUINFO_MAX[$i]=$info_max
done

topology_matches_locked() {
    local cpu raw path id i online_now
    for cpu in "${!CPU_EXPECTED_ONLINE[@]}"; do
        if read_cpu_online_state "$cpu"; then
            online_now=$CPU_ONLINE_STATE
        else
            online_now=unreadable
        fi
        [ "$online_now" = "${CPU_EXPECTED_ONLINE[$cpu]}" ] || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH cpu=$cpu field=online expected=${CPU_EXPECTED_ONLINE[$cpu]} actual=$online_now" >&2
            return 1
        }
    done
    for cpu in "${LOGICAL_CPUS[@]}"; do
        cpu_is_online "$cpu" || {
            online_now=unreadable
            [ ! -r "$SYSROOT/cpu$cpu/online" ] || IFS= read -r online_now <"$SYSROOT/cpu$cpu/online" || true
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH cpu=$cpu field=online actual=$online_now" >&2
            return 1
        }
        read_line "$SYSROOT/cpu$cpu/topology/thread_siblings_list" raw || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH cpu=$cpu field=thread_siblings_list reason=unreadable" >&2
            return 1
        }
        parse_cpu_list "$raw" "locked sibling topology" || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH cpu=$cpu field=thread_siblings_list reason=malformed" >&2
            return 1
        }
        [ "$PARSED_CANONICAL" = "${CPU_CORE_TOKEN[$cpu]}" ] || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH cpu=$cpu field=thread_siblings_list expected=${CPU_CORE_TOKEN[$cpu]} actual=$PARSED_CANONICAL" >&2
            return 1
        }
        resolve_policy_path "$cpu" path || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH cpu=$cpu field=cpufreq reason=unresolved" >&2
            return 1
        }
        [ "$path" = "${CPU_POLICY_PATH[$cpu]}" ] || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH cpu=$cpu field=cpufreq expected=${CPU_POLICY_PATH[$cpu]} actual=$path" >&2
            return 1
        }
    done
    for ((i = 0; i < POLICY_COUNT; i++)); do
        id=$(stat -Lc '%d:%i' -- "${POLICY_PATH[$i]}") || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH policy=${POLICY_NAME[$i]} field=identity reason=unreadable" >&2
            return 1
        }
        [ "$id" = "${POLICY_ID[$i]}" ] || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH policy=${POLICY_NAME[$i]} field=identity expected=${POLICY_ID[$i]} actual=$id" >&2
            return 1
        }
        read_line "${POLICY_PATH[$i]}/related_cpus" raw || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH policy=${POLICY_NAME[$i]} field=related_cpus reason=unreadable" >&2
            return 1
        }
        parse_cpu_list "$raw" "locked policy scope" || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH policy=${POLICY_NAME[$i]} field=related_cpus reason=malformed" >&2
            return 1
        }
        [ "$PARSED_CANONICAL" = "${POLICY_SCOPE[$i]}" ] || {
            echo "CPUFREQ_MULTI_TOPOLOGY_MISMATCH policy=${POLICY_NAME[$i]} field=related_cpus expected=${POLICY_SCOPE[$i]} actual=$PARSED_CANONICAL" >&2
            return 1
        }
    done
}

policy_object_matches_locked() {
    local index=$1 id raw
    id=$(stat -Lc '%d:%i' -- "${POLICY_PATH[$index]}") || return 1
    [ "$id" = "${POLICY_ID[$index]}" ] || return 1
    read_line "${POLICY_PATH[$index]}/related_cpus" raw || return 1
    parse_cpu_list "$raw" "locked policy scope" || return 1
    [ "$PARSED_CANONICAL" = "${POLICY_SCOPE[$index]}" ]
}

RESTORING=0

write_value() {
    local index=$1 field=$2 value=$3
    if [ "$RESTORING" -eq 1 ]; then
        policy_object_matches_locked "$index"
    else
        topology_matches_locked
    fi || {
        echo "cpufreq multicore transaction: policy object/topology changed before writing ${POLICY_NAME[$index]}/$field" >&2
        return 1
    }
    if [ "$TEST_MODE" = 1 ]; then
        "$WRITE_BIN" "${POLICY_PATH[$index]}/$field" "$value"
    else
        printf '%s\n' "$value" >"${POLICY_PATH[$index]}/$field"
    fi
}

set_verified() {
    local index=$1 field=$2 wanted=$3 got=unreadable attempt stable=0
    write_value "$index" "$field" "$wanted" || {
        echo "cpufreq multicore transaction: write failed: ${POLICY_NAME[$index]}/$field=$wanted" >&2
        return 1
    }
    for ((attempt = 1; attempt <= READBACK_ATTEMPTS; attempt++)); do
        if IFS= read -r got <"${POLICY_PATH[$index]}/$field"; then
            if [ "$got" = "$wanted" ]; then
                stable=$((stable + 1))
                [ "$stable" -lt "$STABLE_REQUIRED" ] || return 0
            else
                stable=0
            fi
        else
            got=unreadable
            stable=0
        fi
        [ "$attempt" -eq "$READBACK_ATTEMPTS" ] || sleep "$READBACK_DELAY"
    done
    echo "cpufreq multicore transaction: readback timeout: ${POLICY_NAME[$index]}/$field wanted=$wanted last=$got stable_samples=$stable" >&2
    return 1
}

set_bounds() {
    local index=$1 new_min=$2 new_max=$3 cur_min cur_max
    read_line "${POLICY_PATH[$index]}/scaling_min_freq" cur_min || return 1
    read_line "${POLICY_PATH[$index]}/scaling_max_freq" cur_max || return 1
    [[ $cur_min =~ ^[0-9]+$ && $cur_max =~ ^[0-9]+$ ]] || return 1
    if (( 10#$new_max < 10#$cur_min )); then
        set_verified "$index" scaling_min_freq "$new_min" &&
            set_verified "$index" scaling_max_freq "$new_max"
    else
        set_verified "$index" scaling_max_freq "$new_max" &&
            set_verified "$index" scaling_min_freq "$new_min"
    fi
}

verify_profile() {
    local index=$1 want_gov=$2 want_epp=$3 want_min=$4 want_max=$5
    local got_gov got_epp got_min got_max
    topology_matches_locked || return 1
    read_line "${POLICY_PATH[$index]}/scaling_governor" got_gov || return 1
    read_line "${POLICY_PATH[$index]}/energy_performance_preference" got_epp || return 1
    read_line "${POLICY_PATH[$index]}/scaling_min_freq" got_min || return 1
    read_line "${POLICY_PATH[$index]}/scaling_max_freq" got_max || return 1
    [ "$got_gov" = "$want_gov" ] && [ "$got_epp" = "$want_epp" ] &&
        [ "$got_min" = "$want_min" ] && [ "$got_max" = "$want_max" ]
}

verify_profile_fields() {
    local index=$1 want_gov=$2 want_epp=$3 want_min=$4 want_max=$5
    local got_gov got_epp got_min got_max
    policy_object_matches_locked "$index" || return 1
    read_line "${POLICY_PATH[$index]}/scaling_governor" got_gov || return 1
    read_line "${POLICY_PATH[$index]}/energy_performance_preference" got_epp || return 1
    read_line "${POLICY_PATH[$index]}/scaling_min_freq" got_min || return 1
    read_line "${POLICY_PATH[$index]}/scaling_max_freq" got_max || return 1
    [ "$got_gov" = "$want_gov" ] && [ "$got_epp" = "$want_epp" ] &&
        [ "$got_min" = "$want_min" ] && [ "$got_max" = "$want_max" ]
}

verify_all_applied() {
    local i live_info_max
    for ((i = 0; i < POLICY_COUNT; i++)); do
        read_line "${POLICY_PATH[$i]}/cpuinfo_max_freq" live_info_max || return 1
        [[ $live_info_max =~ ^[0-9]+$ ]] && ((${#live_info_max} <= 9)) || return 1
        [ "$((10#$live_info_max))" -eq "$TARGET_KHZ" ] &&
        [ "$live_info_max" = "${CPUINFO_MAX[$i]}" ] || return 1
        verify_profile "$i" performance performance "$TARGET_KHZ" "$TARGET_KHZ" || return 1
    done
}

TX_ACTIVE=1
FINALIZING=0
CHILD_PID=
CHILD_STARTING=0
PENDING_SIGNAL_RC=0
PENDING_SIGNAL_NAME=
declare -a KEEPER_PIDS=() KEEPER_OUTPUTS=()
KEEPER_STARTING=0
VERIFY_DIR=
ATTEST_FD=
ATTEST_PATH=

cleanup_keepers() {
    local pid alive step keeper_rc output rc=0
    [ "${#KEEPER_PIDS[@]}" -gt 0 ] || return 0
    for pid in "${KEEPER_PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    for ((step = 0; step < CHILD_GRACE_STEPS; step++)); do
        alive=0
        for pid in "${KEEPER_PIDS[@]}"; do
            kill -0 "$pid" 2>/dev/null && alive=1
        done
        [ "$alive" -eq 1 ] || break
        sleep "$CHILD_GRACE_DELAY"
    done
    for pid in "${KEEPER_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            rc=1
            kill -KILL "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${KEEPER_PIDS[@]}"; do
        if wait "$pid" 2>/dev/null; then keeper_rc=0; else keeper_rc=$?; fi
        [ "$keeper_rc" -eq 0 ] || rc=1
    done
    KEEPER_PIDS=()
    if [ -n "$VERIFY_DIR" ] && [ -d "$VERIFY_DIR" ] && [ ! -L "$VERIFY_DIR" ]; then
        for output in "${KEEPER_OUTPUTS[@]}"; do rm -f -- "$output"; done
        rmdir -- "$VERIFY_DIR" 2>/dev/null || true
    fi
    KEEPER_OUTPUTS=()
    VERIFY_DIR=
    return "$rc"
}

keeper_is_sched_idle() {
    local pid=$1 key colon value rest matches=0
    [ -r "/proc/$pid/sched" ] || return 1
    while read -r key colon value rest; do
        if [ "$key" = policy ] && [ "$colon" = : ]; then
            [ "$value" = 5 ] || return 1
            matches=$((matches + 1))
        fi
    done <"/proc/$pid/sched"
    [ "$matches" -eq 1 ]
}

restore_profiles() {
    local i rc=0
    RESTORING=1
    for ((i = POLICY_COUNT - 1; i >= 0; i--)); do
        set_verified "$i" scaling_governor powersave || rc=1
        set_bounds "$i" "${ORIG_MIN[$i]}" "${ORIG_MAX[$i]}" || rc=1
        set_verified "$i" scaling_governor "${ORIG_GOVERNOR[$i]}" || rc=1
        set_verified "$i" energy_performance_preference "${ORIG_EPP[$i]}" || rc=1
        verify_profile_fields "$i" "${ORIG_GOVERNOR[$i]}" "${ORIG_EPP[$i]}" \
            "${ORIG_MIN[$i]}" "${ORIG_MAX[$i]}" || rc=1
    done
    RESTORING=0
    [ "$rc" -eq 0 ] || return 1
    TX_ACTIVE=0
    printf 'CPUFREQ_MULTI_RESTORE policies=%s original_profiles_verified=yes\n' "$POLICY_COUNT"
}

finalize() {
    local command_rc=$? restore_rc=0
    [ "$FINALIZING" -eq 0 ] || exit 70
    FINALIZING=1
    trap - EXIT
    trap '' INT QUIT TERM HUP
    cleanup_keepers || true
    if [ -n "$ATTEST_PATH" ] && [ -f "$ATTEST_PATH" ] && [ ! -L "$ATTEST_PATH" ]; then
        rm -f -- "$ATTEST_PATH" || restore_rc=1
        ATTEST_PATH=
    fi
    if [ "$TX_ACTIVE" -eq 1 ]; then
        restore_profiles || restore_rc=1
    fi
    if [ "$restore_rc" -ne 0 ]; then
        echo "CPUFREQ_MULTI_RESTORE_FAILED command_rc=$command_rc policies=$POLICY_COUNT" >&2
        exit 70
    fi
    exit "$command_rc"
}

terminate_child_group() {
    local sig=$1 step
    [ -n "$CHILD_PID" ] || return 0
    kill -"$sig" -- "-$CHILD_PID" 2>/dev/null || kill -"$sig" "$CHILD_PID" 2>/dev/null || true
    for ((step = 0; step < CHILD_GRACE_STEPS; step++)); do
        kill -0 -- "-$CHILD_PID" 2>/dev/null || break
        sleep "$CHILD_GRACE_DELAY"
    done
    if kill -0 -- "-$CHILD_PID" 2>/dev/null; then
        echo "cpufreq multicore transaction: child ignored $sig; escalating process group to KILL" >&2
        kill -KILL -- "-$CHILD_PID" 2>/dev/null || kill -KILL "$CHILD_PID" 2>/dev/null || true
    fi
    wait "$CHILD_PID" 2>/dev/null || true
    CHILD_PID=
}

handle_signal() {
    local rc=$1 sig=$2
    if [ "$PENDING_SIGNAL_RC" -eq 0 ]; then
        PENDING_SIGNAL_RC=$rc
        PENDING_SIGNAL_NAME=$sig
    fi
    [ "$CHILD_STARTING" -eq 0 ] && [ "$KEEPER_STARTING" -eq 0 ] || return 0
    trap '' INT QUIT TERM HUP
    cleanup_keepers || true
    terminate_child_group "$sig"
    exit "$rc"
}

trap finalize EXIT
trap 'handle_signal 129 HUP' HUP
trap 'handle_signal 130 INT' INT
trap 'handle_signal 131 QUIT' QUIT
trap 'handle_signal 143 TERM' TERM

for ((i = 0; i < POLICY_COUNT; i++)); do
    set_verified "$i" scaling_governor powersave || exit 3
    set_bounds "$i" "$TARGET_KHZ" "$TARGET_KHZ" || exit 3
    set_verified "$i" scaling_governor performance || exit 3
    set_verified "$i" energy_performance_preference performance || exit 3
    verify_profile "$i" performance performance "$TARGET_KHZ" "$TARGET_KHZ" || exit 3
done
verify_all_applied || exit 3
printf 'CPUFREQ_MULTI_APPLIED requested_cpus=%s canonical_selected_cpus=%s logical_cpus=%s policies=%s target_khz=%s controls_verified=yes\n' \
    "$REQUESTED_CPUS" "$SELECTED_CANONICAL" "$LOGICAL_CANONICAL" "$POLICY_TOKEN" "$TARGET_KHZ"

old_umask=$(umask)
umask 077
VERIFY_DIR=$(mktemp -d "$LOCK_NAMESPACE/verify.XXXXXX") || die "cannot create verifier directory"
umask "$old_umask"
chmod 0700 "$VERIFY_DIR" || die "cannot protect verifier directory"
for cpu in "${LOGICAL_CPUS[@]}"; do
    output="$VERIFY_DIR/keeper.$cpu.out"
    KEEPER_OUTPUTS+=("$output")
    KEEPER_STARTING=1
    /usr/bin/env --default-signal=INT,QUIT,HUP,TERM \
        "$KEEPER_BIN" "$cpu" >"$output" 2>&1 &
    KEEPER_PIDS+=("$!")
    KEEPER_STARTING=0
    if [ "$PENDING_SIGNAL_RC" -ne 0 ]; then
        handle_signal "$PENDING_SIGNAL_RC" "$PENDING_SIGNAL_NAME"
    fi
done

ready=0
for ((attempt = 0; attempt < 200; attempt++)); do
    ready=1
    for ((i = 0; i < ${#LOGICAL_CPUS[@]}; i++)); do
        cpu=${LOGICAL_CPUS[$i]}
        pid=${KEEPER_PIDS[$i]}
        if [ "$(grep -Fxc "READY cpu=$cpu policy=SCHED_IDLE" "${KEEPER_OUTPUTS[$i]}" 2>/dev/null || true)" -ne 1 ] ||
           ! keeper_is_sched_idle "$pid"; then
            ready=0
        fi
        kill -0 "$pid" 2>/dev/null || ready=0
    done
    [ "$ready" -eq 0 ] || break
    sleep 0.005
done
if [ "$ready" -ne 1 ]; then
    echo "CPUFREQ_MULTI_KEEPER_FAILED logical_cpus=$LOGICAL_CANONICAL" >&2
    exit 4
fi

stable=0
last_actual=unavailable
for ((attempt = 1; attempt <= ACTUAL_ATTEMPTS; attempt++)); do
    all_match=1
    sample=""
    for cpu in "${LOGICAL_CPUS[@]}"; do
        path=${CPU_POLICY_PATH[$cpu]}
        cur=unreadable
        if ! read_line "$path/scaling_cur_freq" cur ||
           [[ ! $cur =~ ^[0-9]+$ ]] || ((${#cur} > 9)); then
            all_match=0
        else
            cur_value=$((10#$cur))
            delta=$((cur_value - TARGET_KHZ))
            (( delta >= 0 )) || delta=$((-delta))
            (( delta <= TOLERANCE_KHZ )) || all_match=0
        fi
        sample+="${sample:+,}$cpu:$cur"
    done
    last_actual=$sample
    if [ "$all_match" -eq 1 ]; then
        stable=$((stable + 1))
        [ "$stable" -lt "$STABLE_REQUIRED" ] || break
    else
        stable=0
    fi
    [ "$attempt" -eq "$ACTUAL_ATTEMPTS" ] || sleep "$ACTUAL_DELAY"
done
if [ "$stable" -lt "$STABLE_REQUIRED" ]; then
    echo "CPUFREQ_MULTI_ACTUAL_MISMATCH target_khz=$TARGET_KHZ tolerance_khz=$TOLERANCE_KHZ last_actual_khz=$last_actual stable_samples=$stable" >&2
    exit 4
fi
loaded_actual=$last_actual

# Stop all keepers as one bounded set. A non-cooperative keeper makes the
# transaction fail closed rather than overlapping verifier load with the child.
if ! cleanup_keepers; then
    echo "CPUFREQ_MULTI_KEEPER_FAILED reason=non-cooperative-stop" >&2
    exit 4
fi

topology_matches_locked || {
    echo "CPUFREQ_MULTI_TOPOLOGY_DRIFT phase=pre-command" >&2
    exit 4
}
verify_all_applied || {
    echo "CPUFREQ_MULTI_PROFILE_DRIFT phase=pre-command" >&2
    exit 4
}

# Keeper termination is itself a state transition.  Repeat the complete stable
# sample after every keeper is gone, so "immediate-pre-command" never describes
# only the earlier loaded state.
stable=0
last_actual=unavailable
for ((attempt = 1; attempt <= ACTUAL_ATTEMPTS; attempt++)); do
    all_match=1
    sample=""
    for cpu in "${LOGICAL_CPUS[@]}"; do
        path=${CPU_POLICY_PATH[$cpu]}
        cur=unreadable
        if ! read_line "$path/scaling_cur_freq" cur ||
           [[ ! $cur =~ ^[0-9]+$ ]] || ((${#cur} > 9)); then
            all_match=0
        else
            cur_value=$((10#$cur))
            delta=$((cur_value - TARGET_KHZ))
            (( delta >= 0 )) || delta=$((-delta))
            (( delta <= TOLERANCE_KHZ )) || all_match=0
        fi
        sample+="${sample:+,}$cpu:$cur"
    done
    last_actual=$sample
    if [ "$all_match" -eq 1 ]; then
        stable=$((stable + 1))
        [ "$stable" -lt "$STABLE_REQUIRED" ] || break
    else
        stable=0
    fi
    [ "$attempt" -eq "$ACTUAL_ATTEMPTS" ] || sleep "$ACTUAL_DELAY"
done
if [ "$stable" -lt "$STABLE_REQUIRED" ]; then
    echo "CPUFREQ_MULTI_ACTUAL_MISMATCH phase=post-keeper target_khz=$TARGET_KHZ tolerance_khz=$TOLERANCE_KHZ last_actual_khz=$last_actual stable_samples=$stable" >&2
    exit 4
fi

printf 'CPUFREQ_MULTI_ACTUAL target_khz=%s tolerance_khz=%s stable_samples=%s actual_khz=%s loaded_khz=%s verification_scope=post-keeper-pre-fork keeper_mode=SCHED_IDLE keepers_concurrent=yes keepers_stopped_before_final=yes\n' \
    "$TARGET_KHZ" "$TOLERANCE_KHZ" "$STABLE_REQUIRED" "$last_actual" "$loaded_actual"

TOKEN=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n') || die "cannot generate attestation token"
[[ $TOKEN =~ ^[0-9a-f]{32}$ ]] || die "invalid attestation token generation"
old_umask=$(umask)
umask 077
ATTEST_PATH=$(mktemp "$LOCK_NAMESPACE/attest.XXXXXX") || die "cannot create attestation"
umask "$old_umask"
{
    printf 'EXITOS_CPUFREQ_ATTEST version=1 token=%s requested_cpus=%s canonical_selected_cpus=%s logical_cpus=%s policy_count=%s target_khz=%s control_profile_verified=yes actual_frequency_verified=post-keeper-pre-fork locks_held=yes\n' \
        "$TOKEN" "$REQUESTED_CPUS" "$SELECTED_CANONICAL" "$LOGICAL_CANONICAL" \
        "$POLICY_COUNT" "$TARGET_KHZ"
    for ((i = 0; i < POLICY_COUNT; i++)); do
        read_line "${POLICY_PATH[$i]}/cpuinfo_max_freq" attest_info_max ||
            die "cannot attest ${POLICY_NAME[$i]} cpuinfo maximum"
        [ "$attest_info_max" = "${CPUINFO_MAX[$i]}" ] &&
        [ "$((10#$attest_info_max))" -eq "$TARGET_KHZ" ] ||
            die "${POLICY_NAME[$i]} cpuinfo maximum drifted while creating attestation"
        printf 'EXITOS_CPUFREQ_ATTEST_POLICY index=%s path=%s identity=%s related_cpus=%s cpuinfo_max_khz=%s governor=performance epp=performance min_khz=%s max_khz=%s\n' \
            "$((i + 1))" "${POLICY_PATH[$i]}" "${POLICY_ID[$i]}" \
            "${POLICY_SCOPE[$i]}" "$attest_info_max" "$TARGET_KHZ" "$TARGET_KHZ"
    done
    printf 'EXITOS_CPUFREQ_ATTEST_ACTUAL logical_cpus=%s tolerance_khz=%s stable_samples=%s loaded_khz=%s concurrent_keeper_verification=yes keepers_stopped_before_final=yes verification_scope=post-keeper-pre-fork\n' \
        "$last_actual" "$TOLERANCE_KHZ" "$STABLE_REQUIRED" "$loaded_actual"
} >"$ATTEST_PATH" || die "cannot write attestation"
chmod 0400 "$ATTEST_PATH" || die "cannot seal attestation permissions"
attest_identity=$(stat -Lc '%d:%i' -- "$ATTEST_PATH") || die "cannot retain attestation identity"
exec {ATTEST_FD}<"$ATTEST_PATH" || die "cannot retain attestation FD"
[ "$(stat -Lc '%d:%i' -- "/proc/self/fd/$ATTEST_FD")" = "$attest_identity" ] ||
    die "attestation identity changed while retaining it"
rm -f -- "$ATTEST_PATH" || die "cannot unlink attestation path"
ATTEST_PATH=
[ "$(stat -Lc '%a:%u:%h' -- "/proc/self/fd/$ATTEST_FD")" = "400:$(id -u):0" ] ||
    die "retained attestation is not private, read-only, and unlinked"
export EXITOS_CPUFREQ_ATTEST_FD=$ATTEST_FD
export EXITOS_CPUFREQ_ATTEST_TOKEN=$TOKEN

CHILD_STARTING=1
(
    for inherited_fd in "${LOCK_FDS[@]}" "$KEEPER_FD" "$FLOCK_FD" "$WRITE_FD" "$AFTER_LOCK_FD"; do
        [ -n "$inherited_fd" ] || continue
        exec {inherited_fd}<&-
    done
    exec /usr/bin/env --default-signal=INT,QUIT,HUP,TERM \
        setsid --wait -- "${CHILD[@]}"
) &
CHILD_PID=$!
CHILD_STARTING=0
if [ "$PENDING_SIGNAL_RC" -ne 0 ]; then
    handle_signal "$PENDING_SIGNAL_RC" "$PENDING_SIGNAL_NAME"
fi
if wait "$CHILD_PID"; then CHILD_RC=0; else CHILD_RC=$?; fi
CHILD_PID=

if topology_matches_locked; then
    topology_result=yes
else
    topology_result=no
fi
if [ "$topology_result" = yes ] && verify_all_applied; then
    printf 'CPUFREQ_MULTI_POST_COMMAND profile_unchanged=yes topology_unchanged=yes\n'
else
    echo "CPUFREQ_MULTI_PROFILE_DRIFT command_rc=$CHILD_RC topology_unchanged=$topology_result" >&2
    CHILD_RC=71
fi
exit "$CHILD_RC"
