# fio / Exitos end-to-end example

`run.sh --run` is the public end-to-end entry. Its default frontend is active
bpftime, so a successful run executes an unchanged fio process with Exitos. A
frontend setup failure is reported; the runner never silently changes to A.
Use `--frontend preload` only when explicitly comparing the loader frontend.

The finite matrix has exactly two arms:

* A / `plain-fio`: no Exitos environment or loader; fio's omitted allocation
  controls are recorded as `native`, `overwrite=0`, and `create_on_open=0`.
* B / `exitos`: the same fio command with the Exitos frontend and its selected
  file policy. The runner fixes `EXITOS_FASTPATH=1` and
  `EXITOS_IOPATH=uringpoll`, so every timed write is submitted as an NVMe command
  on an io_uring ring created with `IORING_SETUP_IOPOLL`: the filesystem and the
  ordinary block layer are both out of the path, and completion is polled rather
  than waited on.

  A timed `fdatasync` is answered without entering the kernel, and what it costs
  depends on what the namespace reports. With `queue/write_cache` = `write
  through` there is no volatile cache to drain and it issues no device command at
  all. With `write back` and `queue/fua` = 1 each write already carries FUA, so
  again no command is issued. Only on a device with a volatile cache and no FUA
  does the taken-over `fdatasync` become an NVMe FLUSH on the polled ring. The
  device transaction in `tools/` prepares a write-through namespace, so on that
  host it is the first case.

That io path needs the nvme driver to have poll queues, which is settled when
the driver probes. The device transaction in `tools/` arranges it; running this
example outside that transaction leaves the arrangement to the operator. The
runner checks `/sys/module/nvme/parameters/poll_queues` before it creates
anything and refuses with `poll_queues_disabled` rather than letting the run
fail later as a registration error that names neither the driver nor the
parameter.

Every cell uses synchronous `ioengine=psync`, application `iodepth=1`, and 4 KiB
writes. The default worker set is `1,2,4,8`; `--repeats 2` alternates A-B and
B-A. At 4 KiB the scatter-gather question does not arise: one block is a single
physically contiguous transfer, so no multi-segment descriptor list is built and
nothing has to be staged into a contiguous bounce buffer first.

Each worker count is also a queue depth, and that is why the engine is
synchronous. Interception covers `write`, `pwrite64`, `writev`, `pwritev`,
`pwritev2` and `fdatasync`; it does not cover `io_uring_enter`. An asynchronous
fio engine at `iodepth=N` would therefore leave the Exitos arm nothing to take
over and the validator would fail every cell. With a synchronous engine at
application `iodepth=1`, N workers is N writes in flight.

`--worker-mode` chooses how the workers are laid out, and the default is
`thread`:

* `thread` — one fio process with `thread=1` and `numjobs=N`. The N job threads
  share one address space and one set of Exitos process state.
* `process` — N independent fio processes, one job each (`numjobs=1`,
  `thread=0`), started together and pinned one per CPU, each writing its own
  file. They share no address space and no Exitos process state, so each worker
  gets a private donor directory and a donor pool one file wide.

A single fio with `thread=0` cannot stand in for process mode: fio forks after
start, so every child would inherit the same `EXITOS_DONOR_DIR`, and because the
donor pool is created lazily on the first intercepted `fallocate` with its files
opened `O_EXCL`, the workers would collide on `donor-0000.dat` and every one but
the first would be refused.

`summarize.py` checks that the shortcut ran rather than inferring it from
correct data. It requires the taken-over write count to equal the timed write
count, the taken-over `fdatasync` count to equal the timed `fdatasync` count, no
write handed back to ext4, no registration refused, and at least one mapping
refresh per file.

What B minus A measures is the preparation and the shortcut together, not the
shortcut alone. Arm B also carries `EXITOS_PREPARE=donor-fallocate`, which moves
ext4's first-write unwritten-extent conversion off the timed path; arm A pays
that cost inside its timed writes. Splitting the two apart is what
`tools/run-4k-thread-qd1-matrix.sh` is for.

Setup, fio timing, and one ordinary full-file readback are recorded separately.
There is no digest in the write loop. A single final manifest is created after
all cells pass. The outer device transaction owns the mount, identity evidence,
restoration, and its final manifest.

The runner creates a temporary working directory inside `--mount` and writes one
file of `--file-mib` per worker there, plus a donor pool of the same width; at
the default 64 MiB and eight workers that is roughly 1 GiB of free space,
released when the cell finishes. `--result-dir` must be outside the mount and
holds only evidence.

`--device` and `--expect-identity` are required whenever the Exitos arm runs.
`--device` is the only channel that reaches `EXITOS_DEV` — the runner strips
every inherited `EXITOS_*` name before it launches fio — and without it the
shortcut refuses to register any file on a real namespace.

Example on an already-mounted ext4 filesystem:

```sh
example/fio-bpftime-e2e/run.sh --run \
  --mount /mnt/exitos_fs \
  --result-dir /root/exitos-artifacts/fio-bpftime-e2e-001 \
  --cpus 0,1,2,3,4,5,6,7 \
  --device /dev/nvmeXnYpZ \
  --expect-identity <namespace WWID> \
  --active-so /absolute/path/to/this/repo/libexitos_bpftime_active.so \
  --transformer /absolute/path/to/bpftime/build/attach/text_segment_transformer/libbpftime-agent-transformer.so
```

That command is thread mode. For the other mode add `--worker-mode process
--frontend preload` and give it a fresh `--result-dir`, or run the process entry
[`multi-process/run.sh`](multi-process/README.md), which takes the same
arguments minus `--worker-mode` and fixes that term to `process`.

Process mode needs `--frontend preload`. fio ends a `--thread=0` job in a forked
child with `_exit()`, which skips DSO destructors, and the destructor is the only
place the counters are written. The symbol-interposition frontend interposes
`_exit` for exactly this case; the instruction-rewriting frontend does not carry
that interposer. The runner refuses the other combination with
`process_mode_requires_preload_frontend` rather than producing counter files that
read as "no takeover happened" when a takeover did happen.

Use `--plan` first to inspect the normalized fio command; it creates nothing.
