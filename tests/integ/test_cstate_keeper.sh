#!/bin/bash
# The C-state control requested for the 8 KiB experiment: keep one exact
# logical CPU runnable only while the normal-priority benchmark is blocked.
set -u
cd "$(dirname "$0")/../.."

n=0; fail=0
ok()  { n=$((n+1)); echo "ok $n - $*"; }
nok() { n=$((n+1)); fail=1; echo "not ok $n - $*"; }

allowed=$(awk '/^Cpus_allowed_list:/ { print $2 }' /proc/self/status)
first=${allowed%%,*}; cpu=${first%%-*}
out=$(mktemp /tmp/exitos-cstate-keeper.XXXXXX)
pid=""
cleanup() {
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null || true
    fi
    rm -f "$out"
}
trap cleanup EXIT

if [ ! -x ./tools/cstate-keeper ]; then
    nok "tools/cstate-keeper is built"
    echo "1..$n  ($fail failed)"
    exit 1
fi
ok "tools/cstate-keeper is built"

./tools/cstate-keeper "$cpu" >"$out" 2>&1 & pid=$!
ready=0
for _ in $(seq 1 100); do
    if grep -q "^READY cpu=$cpu policy=SCHED_IDLE$" "$out"; then
        ready=1
        break
    fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.01
done
[ "$ready" = 1 ] && ok "keeper reports effective CPU and SCHED_IDLE policy" \
                    || nok "keeper did not become ready: $(tr '\n' ' ' < "$out")"

aff=$(taskset -pc "$pid" 2>/dev/null || true)
case "$aff" in
  *": $cpu") ok "keeper is pinned to exactly logical CPU $cpu" ;;
  *) nok "keeper affinity is not exactly CPU $cpu ($aff)" ;;
esac

cls=$(ps -o cls= -p "$pid" 2>/dev/null | tr -d ' ')
[ "$cls" = IDL ] && ok "kernel reports the keeper's scheduling class as IDL" \
                 || nok "kernel reports scheduling class '$cls', expected IDL"

kill -TERM "$pid" 2>/dev/null
if wait "$pid"; then
    ok "SIGTERM stops the keeper cleanly"
else
    nok "keeper returned failure after SIGTERM"
fi
pid=""

if ./tools/cstate-keeper -1 >/dev/null 2>&1; then
    nok "negative CPU is refused"
else
    ok "negative CPU is refused"
fi
if ./tools/cstate-keeper not-a-cpu >/dev/null 2>&1; then
    nok "non-numeric CPU is refused"
else
    ok "non-numeric CPU is refused"
fi

echo "1..$n  ($fail failed)"
[ "$fail" -eq 0 ]
