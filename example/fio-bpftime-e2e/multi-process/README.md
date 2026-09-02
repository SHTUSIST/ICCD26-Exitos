# The same matrix, run as separate processes

This runs exactly the matrix the parent example runs, with one term changed: the
workers are independent processes rather than job threads in one process.

Everything else is the parent's: an unchanged fio, 4 KiB writes, `direct=1`,
`fdatasync=1`, the synchronous `psync` engine at application `iodepth=1`, worker
counts 1, 2, 4 and 8, and the two arms A (plain fio) and B (the same command
under Exitos taking the shortcut).

## What the comparison is for

In the parent, the N workers are job threads inside one fio process. They share
one address space and one set of Exitos process state: one registration table,
one set of counters, one donor pool.

Here the N workers are N fio processes, one job each, started together and
pinned one per CPU. They share no address space and no Exitos process state, so
each worker gets a private donor directory and a donor pool one file wide.

Running the same worker counts both ways separates what the workers share from
what each worker holds on its own. If a result only appears in one arrangement,
the sharing is where to look.

A single fio with `thread=0` cannot stand in for this. fio forks after start, so
every child would inherit the same `EXITOS_DONOR_DIR`; the donor pool is created
lazily on the first intercepted `fallocate` and its files are opened `O_EXCL`, so
the workers would collide on `donor-0000.dat` and every one but the first would
be refused.

## Which frontend this needs, and why

Pass `--frontend preload`. fio ends a `--thread=0` job in a forked child with
`_exit()`, which skips DSO destructors, and the destructor is the only place
Exitos writes its counters. The symbol-interposition frontend interposes `_exit`
for exactly this case, and the runner arms it here with
`EXITOS_STATS_ON_WORKER_EXIT=1`. The instruction-rewriting frontend does not
carry that interposer, so with it a worker that took over every write would
publish nothing and the summary would refuse the cell for a takeover that did
happen. The parent runner refuses that combination outright rather than let the
refusal look like a measurement result.

## Running it

The arguments are the parent's, minus `--worker-mode`, which is fixed here and
refused if passed. `--device` and `--expect-identity` are required whenever the
Exitos arm runs. Use `--plan` first; it prints the normalized fio command and
creates nothing.

```sh
example/fio-bpftime-e2e/multi-process/run.sh --run \
  --mount /mnt/exitos_fs \
  --result-dir /root/exitos-artifacts/fio-e2e-process-001 \
  --cpus 0,1,2,3,4,5,6,7 \
  --device /dev/nvmeXnYpZ \
  --expect-identity <namespace WWID> \
  --frontend preload \
  --preload-so /absolute/path/to/this/repo/libexitos_preload.so
```

Give it a `--result-dir` of its own. The summary groups by worker mode and never
pools a thread cell with a process cell: they are different workloads, and
averaging them would hide the comparison they exist to make.
