# Single-thread io_uring passthrough queue-depth example

This example measures real queue depth in one application thread at 4 KiB.  It
is **not a fio `psync`/`numjobs` benchmark**: the timed program is the adjacent
`qd_bench`, which stages QD distinct commands, submits them as one batch, and
reaps all QD completions before continuing.  The `--fio` argument exists only
to keep the runner interface compatible with the outer device transaction and
to record tool provenance; fio is not launched by this workload.

The two measured arms are deliberately narrow:

- `ext4`: `O_DIRECT` `IORING_OP_WRITE` on an ext4 file with an IOPOLL ring.
- `passthrough`: `IORING_OP_URING_CMD` NVMe writes to the same ext4 file's
  physical extents, also with an IOPOLL ring.

There is no fallback.  A missing poll queue, unsupported opcode, incomplete
batch, mapping/identity mismatch, command error, or malformed result fails the
cell and the campaign.  QD is selected statically before the run; neither the
runner nor `qd_bench` adapts it from I/O size or observed performance.

## Build and plan

Build the adjacent benchmark first:

```bash
make tests/uring-passthrough-qd/qd_bench
```

On the test host, use the identity-first outer transaction as the device
entrypoint.  It owns target discovery, temporary ext4 setup, PCI rebinding,
poll queues, the final manifest and exact restoration.  First review its
read-only plan:

```bash
./tools/nvme-4k-thread-qd1-transaction.sh --plan \
  --repo /absolute/repo \
  --mount /mnt/exitos_fs \
  --result-dir /root/exitos-artifacts/unique-run-id \
  --active-so /absolute/repo/libexitos_bpftime_active.so \
  --transformer /absolute/bpftime/libbpftime-agent-transformer.so \
  --fio /usr/bin/fio \
  --workload-runner /absolute/repo/tests/uring-passthrough-qd/run.sh
```

Then repeat it with `--run` and the four confirmations printed by the plan:

```text
--confirm-boot <current-boot-id>
--confirm-serial EXAMPLESERIAL0001
--confirm-wwid eui.00000000000000000000000000000000
--confirm-pci-dsn 00-00-00-00-00-00-00-00
```

The serial, WWID and PCI DSN above are placeholders.  Substitute the boot id
and the device identity that the plan printed for the namespace under test.

The recorded result used this outer interface.  Although the transaction's
filename records its original thread-gate role, `--workload-runner` selects
this runner and the runner owns the real-QD matrix contract.

The outer transaction resolves the target on every run and exports the retained
namespace block device and NVMe generic character device.  With those values in
place, this is a complete dry-run invocation:

```bash
export EXITOS_TRANSACTION_WHOLE=/dev/nvme-resolved-whole
export EXITOS_TRANSACTION_GENERIC=/dev/ng-resolved-namespace

./tests/uring-passthrough-qd/run.sh --plan \
  --mount /mnt/exitos_fs \
  --result-dir /absolute/evidence/qd-plan \
  --cpus 43 \
  --active-so /absolute/repo/libexitos_bpftime_active.so \
  --transformer /absolute/bpftime/libbpftime-agent-transformer.so \
  --fio /usr/bin/fio \
  --device /dev/resolved-test-partition \
  --expect-identity eui.resolved-namespace-wwid \
  --manifest-mode outer
```

`--plan` parses and prints the immutable workload without creating a result
directory, files, cooldowns, or device I/O.  Review that output before changing
`--plan` to `--run` and use a new `--result-dir`:

```bash
./tests/uring-passthrough-qd/run.sh --run \
  --mount /mnt/exitos_fs \
  --result-dir /absolute/evidence/qd-run-001 \
  --cpus 43 \
  --active-so /absolute/repo/libexitos_bpftime_active.so \
  --transformer /absolute/bpftime/libbpftime-agent-transformer.so \
  --fio /usr/bin/fio \
  --device /dev/resolved-test-partition \
  --expect-identity eui.resolved-namespace-wwid \
  --manifest-mode outer
```

For a smaller, still-static matrix, add a unique subset such as
`--qd-list 1,4,8`.  The only accepted values are `1`, `2`, `4`, and `8`; the
default is `--qd-list 1,2,4,8`.  The order supplied selects membership only.
Each selected QD is still placed by the fixed balanced repetition schedule.

Production always executes `qd_bench` from this directory.  The binary cannot
be replaced by an environment variable outside the device-free unit test.

## Exact workload

Every cell fixes:

- one process and one application thread;
- 4 KiB per command;
- 2,097,152 commands, or 8 GiB written;
- the selected QD as both the requested and observed maximum submitted batch;
- a ten-second cooldown before timing.

Before timing, the runner creates one shared file containing a 4 KiB leading
guard, an 8 GiB data region, and a 4 KiB trailing guard.  It actually writes
zero to all 8 GiB + 8 KiB with direct I/O and synchronizes the mount, so a raw
passthrough write cannot be hidden by stale zero pages in the file cache.
Preparation, FIEMAP resolution, identity checks, ring setup, buffer allocation,
and teardown are outside the measured interval.

Both arms overwrite the same file extents and therefore the same physical
device LBAs, with the same `0x5a` buffer pattern, command count, and batch size.
Four repetitions use these QD orders:

```text
1,2,4,8
8,4,2,1
2,1,8,4
4,8,1,2
```

The arm order is `ext4,passthrough` in repetitions 0 and 3, and
`passthrough,ext4` in repetitions 1 and 2.  This gives four samples per arm and
QD with a 2/2 reversed-order balance.  It also leaves passthrough as the final
writer, so the campaign's one complete readback validates the raw path rather
than a later filesystem overwrite.

## Evidence and correctness

Every cell retains its exact argv, stdout/stderr, return code, configuration,
and `qd_bench` JSON.  The runner rejects a JSON result unless it proves:

- `threads=1`, `bs=4096`, the requested static QD, and IOPOLL enabled;
- the exact backend and opcode named above;
- `max_submitted_batch=QD`;
- exactly 2,097,152 submitted and completed commands, zero completion errors,
  and the corresponding byte and submit-call counts;
- the expected file, partition, character-device, namespace, and extent
  identity fields.

It does not hash or read back data per cell.  After every timed cell finishes,
one ordinary buffered-file readback pass scans the complete shared file and
checks that its 8 GiB data region is `0x5a` and both guards remain zero.  In
`--manifest-mode self`, one artifact SHA manifest is produced only after that
readback and summary.  In `outer` mode no inner manifest is created; the outer
transaction owns the campaign's single final manifest.

`summary.txt` and `summary.json` report all four IOPS samples, the median for
each arm at each QD, and:

```text
(passthrough median / ext4 median - 1) * 100
```

Positive percentages favor passthrough.  What a completed result directory has
to contain is in [results/README.md](results/README.md).

## Scope

The passthrough arm writes the physical extents belonging to its prepared test
file.  Run it only through the target-specific transaction on the disposable
test namespace.  The transaction, not this public workload runner, owns PCI
rebinding, poll-queue enablement, target discovery, and restoration.
