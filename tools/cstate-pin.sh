#!/bin/bash
# Disable the deeper idle states on ONE core, run something, put them back.
#
# Why this exists: the completion interrupt for the queue under measurement is
# pinned to the same core that submits the command, so that core has to leave
# whatever idle state it entered before the interrupt handler can run. The exit
# latency of the deeper idle states is several times the difference being
# investigated -- so a shift in which state gets entered can produce that
# difference by itself. Holding the core in shallow states removes that
# variable.
#
# Scope: writes only to /sys/devices/system/cpu/cpu<N>/cpuidle/state*/disable for
# the single core named on the command line. Nothing global, nothing that
# affects another user's cores. The restore runs from an EXIT trap, so an
# interrupted run still puts the states back.
#
# Usage:  bash tools/cstate-pin.sh <cpu> <keep-shallowest-N> -- <command...>
#         bash tools/cstate-pin.sh 170 2 -- ./attribution/qdsplit 1000
#         bash tools/cstate-pin.sh restore 170
set -u

if [ "${1:-}" = "restore" ]; then
    cpu="$2"
    for s in /sys/devices/system/cpu/cpu"$cpu"/cpuidle/state*; do
        echo 0 > "$s/disable" 2>/dev/null
    done
    echo "cpu $cpu: all idle states re-enabled"
    exit 0
fi

CPU="$1"; KEEP="$2"; shift 2
[ "${1:-}" = "--" ] && shift

D="/sys/devices/system/cpu/cpu$CPU/cpuidle"
[ -d "$D" ] || { echo "no cpuidle for cpu $CPU" >&2; exit 1; }

declare -a SAVED=()
restore() {
    local i=0
    for s in "$D"/state*; do
        echo "${SAVED[$i]:-0}" > "$s/disable" 2>/dev/null
        i=$((i+1))
    done
    echo "cpu $CPU: idle states restored"
}
trap restore EXIT

i=0
for s in "$D"/state*; do
    SAVED+=("$(cat "$s/disable")")
    if [ "$i" -ge "$KEEP" ]; then
        echo 1 > "$s/disable" || { echo "cannot disable $s" >&2; exit 1; }
    fi
    i=$((i+1))
done

echo "cpu $CPU: keeping the $KEEP shallowest idle states, the rest disabled"
for s in "$D"/state*; do
    printf "  %-5s %-5s disable=%s exit_latency=%sus\n" \
        "$(basename "$s")" "$(cat "$s/name")" "$(cat "$s/disable")" "$(cat "$s/latency")"
done

"$@"
