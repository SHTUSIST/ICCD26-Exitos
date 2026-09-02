#!/bin/bash
# The same finite matrix as the parent example, run with independent processes
# instead of job threads.
#
# The parent runner already carries both arrangements behind --worker-mode; this
# entry exists so the process sweep is a thing you can point at and run, with its
# own result directory, rather than a flag someone has to remember. Every other
# term is identical to the parent: same fio command, same 4 KiB writes, same
# synchronous engine at application iodepth 1, same two arms, same worker counts
# 1, 2, 4 and 8, same balanced A-B / B-A ordering.
#
# What differs is what the workers share. In the parent the N workers are job
# threads in one fio process: one address space, one set of Exitos process state.
# Here they are N separate fio processes, one job each, started together and
# pinned one per CPU, each writing its own file and holding its own Exitos state.
# Running both answers whether a result depends on that sharing.
#
# One consequence of that separation: fio ends a --thread=0 job in a forked child
# with _exit(), which skips DSO destructors, and the destructor is the only place
# Exitos writes its counters. The symbol-interposition frontend interposes _exit
# for exactly this case; the instruction-rewriting frontend does not carry that
# interposer, so process mode needs --frontend preload. The parent runner refuses
# the other combination rather than producing counter files that read as "no
# takeover happened" when a takeover did happen.
set -Eeuo pipefail
umask 077

SELF_DIR=$(cd "$(dirname "$0")" && pwd -P)
PARENT=$SELF_DIR/../run.sh

[ -x "$PARENT" ] || { printf 'FIO_E2E_PROCESS_REFUSE parent_runner_missing=%s\n' "$PARENT" >&2; exit 3; }

for arg in "$@"; do
    case "$arg" in
        --worker-mode)
            printf 'FIO_E2E_PROCESS_REFUSE worker_mode_is_fixed_here\n' >&2
            exit 3
            ;;
    esac
done

exec "$PARENT" "$@" --worker-mode process
