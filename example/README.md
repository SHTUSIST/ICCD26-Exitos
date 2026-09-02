# Examples

There is one example here, and it can be run in either of two worker
arrangements.

[`fio-bpftime-e2e`](fio-bpftime-e2e/README.md) runs an unchanged fio: not
modified, not recompiled, and not aware that anything is intercepting it. What
the comparison shows is therefore what an application gets without being ported.
The [`multi-process`](fio-bpftime-e2e/multi-process/README.md) entry beneath it
runs the same matrix with the workers arranged as separate processes instead of
job threads.

## The two arms

The runner fixes both arms; neither is decided by whatever environment the shell
happens to carry.

* A, `plain-fio`: the fio command with no Exitos environment and no Exitos
  loader.
* B, `exitos`: the same fio command under an Exitos frontend, with
  `EXITOS_FASTPATH=1` and `EXITOS_IOPATH=uringpoll` set by
  `fio-bpftime-e2e/run.sh`. Every timed write is submitted as an NVMe command on
  an io_uring ring created with `IORING_SETUP_IOPOLL`. The filesystem and the
  ordinary block layer are both out of the path, and completion is polled rather
  than waited on. A timed `fdatasync` is answered without entering the kernel and
  issues a device command only when the namespace has a volatile write cache it
  cannot cover with FUA.

## Fixed terms

Every cell is fio with 4 KiB writes, `direct=1`, `fdatasync=1`,
`ioengine=psync` and application `iodepth=1`. The worker counts are 1, 2, 4 and
8. `--repeats 2` places the arms A-B and then B-A, so each arm is measured once
in each position at every worker count.

At 4 KiB the scatter-gather question does not arise: one block is a single
physically contiguous transfer, so no multi-segment descriptor list is built and
nothing has to be staged into a contiguous bounce buffer first. That question
only starts above 4 KiB.

## Why the worker count is the queue depth

Interception covers `write`, `pwrite64`, `writev`, `pwritev`, `pwritev2` and
`fdatasync`. It does not cover `io_uring_enter`. An asynchronous fio engine at
`iodepth=N` would therefore leave the B arm with nothing to take over, and the
counter checks below would fail every cell. With a synchronous engine at
application `iodepth=1`, N workers is N writes in flight, so the worker count is
the queue depth. That is the only way this example produces a queue depth at
all.

## The two worker arrangements

`--worker-mode` selects how the workers are laid out. The default is `thread`.

* `thread`, the parent entry: one fio process, `--thread=1`, `--numjobs=N`. The
  N job threads share one address space and one set of Exitos process state —
  one registration table, one set of counters, one donor pool.
* `process`, the [`multi-process`](fio-bpftime-e2e/multi-process/README.md)
  entry: N independent fio processes, one job each (`--numjobs=1`,
  `--thread=0`), started together and pinned one per CPU, each writing its own
  file, each with a private donor directory and a donor pool one file wide.

A single fio with `--thread=0` cannot stand in for process mode. fio forks after
start, so every child would inherit the same `EXITOS_DONOR_DIR`; the donor pool
is created lazily on the first intercepted `fallocate` and its files are opened
`O_EXCL`, so the workers would collide on `donor-0000.dat` and every one but the
first would be refused.

Running the same worker counts both ways separates what the workers share from
what each worker holds on its own. The two arrangements are different workloads,
so the summary keeps them apart: its table has a `worker_mode` column and never
pools a thread cell with a process cell.

## How the example proves the shortcut ran

`summarize.py` checks the B arm's counters exactly, rather than inferring the
shortcut from the data coming back correct. It requires the taken-over write
count to equal the timed write count, the taken-over `fdatasync` count to equal
the timed `fdatasync` count, no write handed back to ext4, no registration
refused, and at least one mapping refresh per file. A correct readback on its
own cannot tell a shortcut that ran and was right from a shortcut that never
ran; these counters are what separate the two.

## Running it

Use `--plan` first; it prints the normalized fio command and creates nothing.
Then repeat the same command with `--run`.

`--device` and `--expect-identity` are required whenever the B arm runs: the
runner strips every inherited `EXITOS_*` name before launching fio, so `--device`
is the only channel that reaches `EXITOS_DEV`, and without it the shortcut
refuses to register any file on a real namespace. The runner also checks that the
nvme driver has poll queues before it creates anything.

Thread mode is the parent runner:

```sh
example/fio-bpftime-e2e/run.sh --run \
  --mount /mnt/exitos_fs \
  --result-dir /root/exitos-artifacts/fio-e2e-thread-001 \
  --cpus 0,1,2,3,4,5,6,7 \
  --device /dev/nvmeXnYpZ \
  --expect-identity <namespace WWID> \
  --active-so /absolute/path/to/this/repo/libexitos_bpftime_active.so \
  --transformer /absolute/path/to/bpftime/build/attach/text_segment_transformer/libbpftime-agent-transformer.so
```

Process mode takes the same arguments, minus `--worker-mode`, which that entry
fixes for you and refuses if you pass it. It needs `--frontend preload`: fio ends
a `--thread=0` job with `_exit()`, which skips DSO destructors, and only the
symbol-interposition frontend interposes `_exit` to publish the counters from a
worker that exits that way.

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

Give each arrangement a `--result-dir` of its own, outside the mount. Everything
a run produces is written under that directory, which the operator chooses;
nothing is recorded inside this repository. The runner also creates a temporary
working directory inside `--mount` for the target files and the donor pool: one
file of `--file-mib` per worker plus a pool of the same width, about 1 GiB at the
default 64 MiB and eight workers, released when the cell finishes.

## Host requirement

`uringpoll` needs the nvme driver to have poll queues. That is a
driver-probe-time setting, not something a run can switch on for itself. The
device transaction in `tools/` sets it up; running the example outside that
transaction leaves it to the operator to arrange.

For a narrower device-level check of the passthrough path, see
[`tests/uring-passthrough-qd`](../tests/uring-passthrough-qd/README.md), which
now lives with the tests.
