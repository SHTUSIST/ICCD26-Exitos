#!/bin/bash
# Resolve the three CPUs used by the C-state keeper experiment.  Output is one
# tab-separated record:
#   measurement_cpu  smt_sibling  different_core_placebo  placebo_scope
#
# EXITOS_CPU_SYSFS_ROOT and EXITOS_ALLOWED_CPUS exist so the resolver can be
# tested without consulting or perturbing the running machine.
set -euo pipefail

die() { echo "cstate topology: $*" >&2; exit 2; }

[[ $# -eq 1 && $1 =~ ^[0-9]+$ ]] || die "usage: $0 CPU"
cpu=$1
sysroot=${EXITOS_CPU_SYSFS_ROOT:-/sys/devices/system/cpu}
allowed=${EXITOS_ALLOWED_CPUS:-}
if [ -z "$allowed" ]; then
    allowed=$(awk '/^Cpus_allowed_list:/ { print $2; exit }' /proc/self/status)
fi
[ -n "$allowed" ] || die "empty allowed CPU list"

declare -a cpus=()
declare -A seen=()
IFS=',' read -r -a fields <<<"$allowed"
for field in "${fields[@]}"; do
    if [[ $field =~ ^([0-9]+)$ ]]; then
        lo=$((10#${BASH_REMATCH[1]}))
        hi=$lo
    elif [[ $field =~ ^([0-9]+)-([0-9]+)$ ]]; then
        lo=$((10#${BASH_REMATCH[1]}))
        hi=$((10#${BASH_REMATCH[2]}))
        (( lo <= hi )) || die "reversed CPU range '$field'"
    else
        die "malformed allowed CPU list '$allowed'"
    fi
    # A real allowed list is bounded by the machine's CPU count.  Bound a fake
    # or malformed input too, so it cannot turn this parser into a long loop.
    (( hi - lo <= 1048576 )) || die "CPU range '$field' is unreasonably large"
    for ((c = lo; c <= hi; c++)); do
        if [[ -z ${seen[$c]+x} ]]; then
            cpus+=("$c")
            seen[$c]=1
        fi
    done
done

[[ -n ${seen[$cpu]+x} ]] || die "CPU $cpu is outside allowed list '$allowed'"
[ -r "$sysroot/online" ] || die "missing online CPU list under $sysroot"
IFS= read -r online_list <"$sysroot/online" || die "cannot read online CPU list"
declare -A online_set=()
IFS=',' read -r -a fields <<<"$online_list"
for field in "${fields[@]}"; do
    if [[ $field =~ ^([0-9]+)$ ]]; then
        lo=$((10#${BASH_REMATCH[1]})); hi=$lo
    elif [[ $field =~ ^([0-9]+)-([0-9]+)$ ]]; then
        lo=$((10#${BASH_REMATCH[1]})); hi=$((10#${BASH_REMATCH[2]}))
        (( lo <= hi )) || die "reversed online CPU range '$field'"
    else
        die "malformed online CPU list '$online_list'"
    fi
    (( hi - lo <= 1048576 )) || die "online CPU range '$field' is unreasonably large"
    for ((c = lo; c <= hi; c++)); do online_set[$c]=1; done
done
[[ -n ${online_set[$cpu]+x} ]] || die "CPU $cpu is not online"
topo="$sysroot/cpu$cpu/topology"
[ -d "$topo" ] || die "no topology for CPU $cpu under $sysroot"

read_field() {
    local path=$1 value
    [ -r "$path" ] || return 1
    IFS= read -r value <"$path" || return 1
    [[ $value =~ ^-?[0-9]+$ ]] || return 1
    printf '%s\n' "$value"
}

target_pkg=$(read_field "$topo/physical_package_id") || die "invalid package id for CPU $cpu"
target_core=$(read_field "$topo/core_id") || die "invalid core id for CPU $cpu"
[ -r "$topo/thread_siblings_list" ] || die "missing SMT sibling list for CPU $cpu"
IFS= read -r sibling_list <"$topo/thread_siblings_list"

declare -A sibling_set=()
IFS=',' read -r -a fields <<<"$sibling_list"
for field in "${fields[@]}"; do
    if [[ $field =~ ^([0-9]+)$ ]]; then
        lo=$((10#${BASH_REMATCH[1]})); hi=$lo
    elif [[ $field =~ ^([0-9]+)-([0-9]+)$ ]]; then
        lo=$((10#${BASH_REMATCH[1]})); hi=$((10#${BASH_REMATCH[2]}))
        (( lo <= hi )) || die "reversed SMT range '$field'"
    else
        die "malformed SMT list '$sibling_list'"
    fi
    (( hi - lo <= 1048576 )) || die "SMT range '$field' is unreasonably large"
    for ((c = lo; c <= hi; c++)); do sibling_set[$c]=1; done
done

sibling=""
for c in "${cpus[@]}"; do
    if [ "$c" != "$cpu" ] && [[ -n ${sibling_set[$c]+x} ]] && \
       [[ -n ${online_set[$c]+x} ]] && \
       [ -d "$sysroot/cpu$c/topology" ]; then
        sibling=$c
        break
    fi
done
[ -n "$sibling" ] || die "CPU $cpu has no distinct allowed online SMT sibling"

placebo_same=""
placebo_other=""
for c in "${cpus[@]}"; do
    [ "$c" != "$cpu" ] || continue
    [ "$c" != "$sibling" ] || continue
    [[ -n ${online_set[$c]+x} ]] || continue
    ctopo="$sysroot/cpu$c/topology"
    [ -d "$ctopo" ] || continue
    c_pkg=$(read_field "$ctopo/physical_package_id") || continue
    c_core=$(read_field "$ctopo/core_id") || continue
    # A placebo must be a different physical core, not merely another thread
    # on the target core.  Same package is preferred to control package effects.
    if [ "$c_pkg" = "$target_pkg" ] && [ "$c_core" = "$target_core" ]; then
        continue
    fi
    if [ "$c_pkg" = "$target_pkg" ]; then
        [ -n "$placebo_same" ] || placebo_same=$c
    else
        [ -n "$placebo_other" ] || placebo_other=$c
    fi
done

if [ -n "$placebo_same" ]; then
    placebo=$placebo_same
    scope=same-package
elif [ -n "$placebo_other" ]; then
    placebo=$placebo_other
    scope=other-package
else
    die "CPU $cpu has no allowed online different-core placebo"
fi

printf '%s\t%s\t%s\t%s\n' "$cpu" "$sibling" "$placebo" "$scope"
