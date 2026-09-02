#!/bin/bash
# What the three build products are allowed to contain.
#
# Two defects that both shipped and both were invisible to every other test:
#
#   1. libexitos.a carried preload.o and the bpftime hook/loader, so ANY program
#      that linked the library for its API silently had its own open/write/
#      pwrite/close replaced, gained two constructors that armed interception,
#      and gained a destructor that overwrote EXITOS_STATS. The whole LD_PRELOAD
#      test group reported zero takeovers because of it.
#   2. Both .so files carried both loader constructors, so a run meant to
#      exercise symbol interposition also started the instruction rewriter and
#      the comparison between the two backends measured the same thing twice.
#
# Neither is visible from a passing unit test: the code is correct, the package
# is not.
cd "$(dirname "$0")/../.." || exit 1
n=0; bad=0
ok()  { n=$((n+1)); echo "ok $n - $1"; }
nok() { n=$((n+1)); bad=$((bad+1)); echo "not ok $n - $1"; }

make lib libexitos_preload.so libexitos_bpftime.so >/dev/null 2>&1 || { echo "Bail out! build failed"; exit 1; }

members=$(ar t libexitos.a | tr '\n' ' ')
case " $members " in
  *" preload.o "*|*" bpftime_hook.o "*|*" bpftime_loader.o "*)
      nok "libexitos.a carries no interposer (has: $members)" ;;
  *)  ok  "libexitos.a carries no interposer (has: $members)" ;;
esac

case " $members " in
  *" frontend_prepare.o "*)
      nok "libexitos.a excludes the retired open-time preparer" ;;
  *)  ok  "libexitos.a excludes the retired open-time preparer" ;;
esac

for sym in open openat write pwrite close fdatasync dup dup2 fcntl; do
  if nm libexitos.a 2>/dev/null | grep -qE "^[0-9a-f]* T $sym\$"; then
    nok "libexitos.a does not define $sym"
  else
    ok  "libexitos.a does not define $sym"
  fi
done

p_ctor=$(nm -D libexitos_preload.so 2>/dev/null | grep -c "exitos_preload_loader\|preload_loader")
b_ctor=$(nm -D libexitos_bpftime.so 2>/dev/null | grep -c "bpftime_loader\|bpftime_unloader")
if nm -D libexitos_preload.so | grep -q "exitos_bpftime_start"; then
  nok "libexitos_preload.so does not carry the instruction rewriter"
else
  ok  "libexitos_preload.so does not carry the instruction rewriter"
fi
if nm -D libexitos_bpftime.so | grep -qE " T (open|write|pwrite)\$"; then
  nok "libexitos_bpftime.so does not carry the symbol interposers"
else
  ok  "libexitos_bpftime.so does not carry the symbol interposers"
fi

# Backend 1 must interpose every entry point a program can actually reach,
# including the large-file aliases and the calls that only libc issues.
for sym in open openat open64 openat64 write pwrite pwrite64 writev pwritev \
           fdatasync fsync ftruncate close fclose dup dup2 dup3 fcntl fcntl64; do
  if nm -D libexitos_preload.so | grep -qE " T $sym\$"; then
    ok  "libexitos_preload.so interposes $sym"
  else
    nok "libexitos_preload.so interposes $sym"
  fi
done

echo "1..$n  ($bad failed)"
[ "$bad" -eq 0 ]
