# Archived qdsplit campaign-completeness tests

These files preserve the historical qdsplit safety, C-state campaign-plan, and
fixed-BDF NVMe poll-rebind transaction test work. They belong
to an earlier completeness/injection detour and are retained only in case
parts of that work become useful later.

This directory is outside `tests/unit`, so the default Makefile wildcards do
not compile or run these tests. Do not run these archived tests as part of the
current correctness/performance work. The qdsplit include was adjusted only
for this archive directory's extra path depth, so the preserved source can be
revived deliberately later without moving it back first.

One subject is not shipped: `test_nvme_poll_rebind_transaction.sh` drives
`tools/nvme-poll-rebind-transaction.sh`, which is not part of this release.
Reviving that test means supplying the script and pointing
`EXITOS_NVME_REBIND_SCRIPT_UNDER_TEST` at it.
