#!/bin/bash
# Backend 2 must not break a threaded program.
#
# The instruction-rewriting backend worked on every single-threaded test writer
# and then crashed fio -- a real application -- with SIGSEGV on a worker thread
# at rip = 0xffffffffffffffff. The bpftime transformer on its own runs the same
# program to completion, and the LD_PRELOAD backend runs it too, so neither the
# rewriting nor the takeover logic is what breaks: installing our hook is.
#
# This test is the minimal shape of that failure: a program that creates worker
# threads which issue syscalls. It needs no NVMe and no root beyond what the
# rewriter itself needs.
cd "$(dirname "$0")/../.." || exit 1
n=0; bad=0
ok()  { n=$((n+1)); echo "ok $n - $1"; }
nok() { n=$((n+1)); bad=$((bad+1)); echo "not ok $n - $1"; }
skip(){ echo "# SKIP - $1"; echo "1..0  (0 failed)"; exit 0; }

SO=${EXITOS_BPFTIME_SO:-}
[ -n "$SO" ] && [ -r "$SO" ] || skip "transformer not built here (set EXITOS_BPFTIME_SO)"
make libexitos_bpftime.so >/dev/null 2>&1 || { echo "Bail out! build failed"; exit 1; }
W=$(mktemp /tmp/exitos-thr-XXXXXX)
gcc -O2 -w -D_GNU_SOURCE -o "$W" tests/integ/writer_threaded.c -lpthread || { echo "Bail out! writer build failed"; exit 1; }

# What is actually true, established by measurement:
#
#   * Loading the transformer .so on its own does NOTHING. Its agent entry point
#     checks AGENT_SO first and returns before it rewrites anything, so "the
#     transformer alone runs the program fine" is not a control -- it is a run
#     with no rewriting in it. That mistake made our own hook look guilty.
#   * Once the rewriting really happens, a threaded program dies, and it dies
#     the same way whether bpftime's own hook is left in place or ours replaces
#     it, and whether ours forwards everything with a plain jump or not. The
#     rewriting is what breaks it.
#   * Single-threaded programs are unaffected.
#
# So this test pins two things: our hook must not be worse than the rewriting
# alone, and single-threaded programs must keep working. It does not assert that
# threaded programs work, because on this bpftime build they do not.
ARM="EXITOS_BPFTIME_SO=$SO EXITOS_FILES=.nomatch"

env LD_PRELOAD=$PWD/libexitos_bpftime.so $ARM "$W" 0 >/dev/null 2>&1
st=$?
[ $st -eq 0 ] && ok "single-threaded work runs under backend 2 (exit 0)" \
              || nok "single-threaded work runs under backend 2 (exit $st)"

for t in 1 2 4; do
  "$W" $t >/dev/null 2>&1
  [ $? -eq 0 ] && ok "control: the threaded writer itself works with $t threads" \
               || nok "control: the threaded writer itself works with $t threads"

  env LD_PRELOAD=$PWD/libexitos_bpftime.so $ARM EXITOS_BPFTIME_NOHOOK=1 "$W" $t >/dev/null 2>&1
  base=$?
  env LD_PRELOAD=$PWD/libexitos_bpftime.so $ARM "$W" $t >/dev/null 2>&1
  ours=$?
  if [ $base -eq 0 ] && [ $ours -ne 0 ]; then
    nok "backend 2 is no worse than the rewriting alone at $t threads (rewriting=$base ours=$ours)"
  else
    ok "backend 2 is no worse than the rewriting alone at $t threads (rewriting=$base ours=$ours)"
  fi
  [ $base -ne 0 ] && echo "# both die at $t threads: bpftime's syscall-instruction rewriting is not safe for threaded programs on this build"
done

rm -f "$W"
echo "1..$n  ($bad failed)"
[ "$bad" -eq 0 ]
