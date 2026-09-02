#!/usr/bin/env python3
"""Validate qd_bench cells and summarize the fixed public QD campaign."""

from __future__ import annotations

import csv
import json
import math
import pathlib
import statistics
import sys
from typing import Any


class EvidenceError(RuntimeError):
    pass


def load_object(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="ascii"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"cannot read valid JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise EvidenceError(f"JSON root is not an object: {path}")
    return value


def exact_int(data: dict[str, Any], key: str, expected: int | None = None) -> int:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise EvidenceError(f"{key} must be a nonnegative JSON integer, got {value!r}")
    if expected is not None and value != expected:
        raise EvidenceError(f"{key} mismatch: actual={value}, expected={expected}")
    return value


def exact_string(data: dict[str, Any], key: str, expected: str) -> None:
    value = data.get(key)
    if value != expected:
        raise EvidenceError(f"{key} mismatch: actual={value!r}, expected={expected!r}")


def validate_cell(argv: list[str]) -> int:
    if len(argv) != 8:
        raise EvidenceError(
            "validate-cell usage: --validate-cell JSON ARM QD OPERATIONS FILE PARTITION CHAR_DEVICE IDENTITY"
        )
    path = pathlib.Path(argv[0])
    arm, qd_text, operations_text, file_path, partition, char_device, identity = argv[1:]
    if arm not in {"ext4", "passthrough"}:
        raise EvidenceError(f"invalid arm: {arm}")
    try:
        qd, operations = int(qd_text), int(operations_text)
    except ValueError as exc:
        raise EvidenceError("QD and operations must be decimal integers") from exc
    if qd not in {1, 2, 4, 8} or operations <= 0 or operations % qd:
        raise EvidenceError(f"invalid fixed geometry: qd={qd}, operations={operations}")

    data = load_object(path)
    exact_int(data, "schema_version", 1)
    exact_int(data, "threads", 1)
    exact_int(data, "bs", 4096)
    exact_int(data, "pattern_byte", 0x5A)
    exact_int(data, "requested_qd", qd)
    # A full batch must have occurred: otherwise the QD label is not evidence
    # that this many commands were concurrently submitted.
    exact_int(data, "max_submitted_batch", qd)
    exact_int(data, "submit_calls", operations // qd)
    exact_int(data, "submitted_commands", operations)
    exact_int(data, "completed_commands", operations)
    exact_int(data, "completion_errors", 0)
    exact_int(data, "bytes", operations * 4096)
    if data.get("iopoll") is not True:
        raise EvidenceError(f"iopoll must be JSON true, got {data.get('iopoll')!r}")

    if arm == "ext4":
        exact_string(data, "backend", "ext4-iopoll")
        exact_string(data, "opcode", "IORING_OP_WRITE")
    else:
        exact_string(data, "backend", "nvme-uring-cmd-iopoll")
        exact_string(data, "opcode", "IORING_OP_URING_CMD")
    exact_string(data, "file_path", file_path)
    exact_string(data, "partition_path", partition)
    exact_string(data, "char_device_path", char_device)
    exact_string(data, "namespace_identity", identity)

    for key in (
        "file_dev_major",
        "file_dev_minor",
        "file_inode",
        "partition_dev_major",
        "partition_dev_minor",
        "char_device_major",
        "char_device_minor",
        "partition_start_lba",
    ):
        exact_int(data, key)
    if exact_int(data, "namespace_id") == 0:
        raise EvidenceError("namespace_id must be positive")
    elapsed_ns = exact_int(data, "elapsed_ns")
    if elapsed_ns == 0:
        raise EvidenceError("elapsed_ns must be positive")
    iops = data.get("iops")
    if isinstance(iops, bool) or not isinstance(iops, (int, float)):
        raise EvidenceError(f"iops must be numeric, got {iops!r}")
    if not math.isfinite(float(iops)) or float(iops) <= 0:
        raise EvidenceError(f"iops must be finite and positive, got {iops!r}")
    print(
        f"CELL_JSON_OK arm={arm} qd={qd} commands={operations} "
        f"max_submitted_batch={qd} opcode={data['opcode']} iopoll=true"
    )
    return 0


def summarize(results_path: pathlib.Path, output_path: pathlib.Path) -> int:
    try:
        with results_path.open("r", encoding="ascii", newline="") as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
    except (OSError, UnicodeError, csv.Error) as exc:
        raise EvidenceError(f"cannot read results table {results_path}: {exc}") from exc
    required = {"rep", "position", "qd", "arm", "iops", "elapsed_ns", "cell"}
    if not rows or not required.issubset(rows[0]):
        raise EvidenceError("results table is empty or lacks required columns")

    grouped: dict[tuple[int, str], list[tuple[int, float]]] = {}
    seen: set[tuple[int, int, str]] = set()
    for row in rows:
        try:
            rep = int(row["rep"])
            qd = int(row["qd"])
            iops = float(row["iops"])
            elapsed_ns = int(row["elapsed_ns"])
        except (TypeError, ValueError) as exc:
            raise EvidenceError(f"malformed results row: {row}") from exc
        arm = row["arm"]
        key = (rep, qd, arm)
        if rep not in range(4) or qd not in {1, 2, 4, 8} or arm not in {
            "ext4",
            "passthrough",
        }:
            raise EvidenceError(f"unexpected results row: {row}")
        if key in seen:
            raise EvidenceError(f"duplicate result: rep={rep}, qd={qd}, arm={arm}")
        if not math.isfinite(iops) or iops <= 0 or elapsed_ns <= 0:
            raise EvidenceError(f"nonpositive/nonfinite measurement: {row}")
        seen.add(key)
        grouped.setdefault((qd, arm), []).append((rep, iops))

    qds = sorted({qd for qd, _ in grouped})
    expected_rows = len(qds) * 2 * 4
    if len(rows) != expected_rows:
        raise EvidenceError(f"incomplete matrix: rows={len(rows)}, expected={expected_rows}")

    summary: dict[str, Any] = {
        "schema_version": 1,
        "metric": "iops",
        "threads": 1,
        "bs": 4096,
        "operations_per_cell": 2097152,
        "repetitions": 4,
        "qds": {},
    }
    lines: list[str] = []
    for qd in qds:
        arms: dict[str, dict[str, Any]] = {}
        for arm in ("ext4", "passthrough"):
            values_with_reps = sorted(grouped.get((qd, arm), []))
            if [rep for rep, _ in values_with_reps] != [0, 1, 2, 3]:
                raise EvidenceError(f"{arm}/QD{qd} does not contain reps 0..3")
            values = [value for _, value in values_with_reps]
            arms[arm] = {
                "samples_iops": values,
                "median_iops": statistics.median(values),
            }
        ext4_median = arms["ext4"]["median_iops"]
        pt_median = arms["passthrough"]["median_iops"]
        percent = (pt_median / ext4_median - 1.0) * 100.0
        summary["qds"][str(qd)] = {
            **arms,
            "passthrough_vs_ext4_percent": percent,
        }
        lines.append(
            f"QD={qd} ext4_samples_iops={arms['ext4']['samples_iops']} "
            f"ext4_median_iops={ext4_median:.3f} "
            f"passthrough_samples_iops={arms['passthrough']['samples_iops']} "
            f"passthrough_median_iops={pt_median:.3f} "
            f"passthrough_vs_ext4_percent={percent:+.3f}%"
        )

    try:
        output_path.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="ascii"
        )
    except OSError as exc:
        raise EvidenceError(f"cannot write summary {output_path}: {exc}") from exc
    print("\n".join(lines))
    return 0


def main(argv: list[str]) -> int:
    try:
        if argv and argv[0] == "--validate-cell":
            return validate_cell(argv[1:])
        if len(argv) != 2:
            raise EvidenceError("usage: summarize.py RESULTS.tsv SUMMARY.json")
        return summarize(pathlib.Path(argv[0]), pathlib.Path(argv[1]))
    except EvidenceError as exc:
        print(f"EVIDENCE_INVALID {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
