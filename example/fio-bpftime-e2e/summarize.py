#!/usr/bin/env python3
"""Validate one finite fio cell and summarize a completed example run.

The checker performs one ordinary whole-file readback per cell.  It never
hashes individual writes; the optional campaign manifest is the only digest
operation and is created by the shell runner after all cells pass.
"""
from __future__ import annotations

import argparse
import glob
import json
import pathlib
import statistics
import sys
from typing import Any


class EvidenceError(RuntimeError):
    pass


def load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"invalid JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise EvidenceError(f"JSON object required: {path}")
    return value


def readback(paths: list[pathlib.Path], expected: int, file_bytes: int) -> None:
    total = 0
    wanted = bytes([expected]) * 1024 * 1024
    for path in paths:
        if not path.is_file() or path.is_symlink():
            raise EvidenceError(f"target is not a regular file: {path}")
        if path.stat().st_size != file_bytes:
            raise EvidenceError(f"size mismatch {path}: {path.stat().st_size} != {file_bytes}")
        with path.open("rb") as source:
            while True:
                block = source.read(len(wanted))
                if not block:
                    break
                if block != bytes([expected]) * len(block):
                    raise EvidenceError(f"pattern mismatch: {path}")
        total += file_bytes
    print(f"READBACK_OK files={len(paths)} bytes={total} expected_byte=0x{expected:02x}")


def fields(text: str, marker: str) -> list[dict[str, str]]:
    records: list[dict[str, str]] = []
    for line in text.splitlines():
        words = line.split()
        if marker not in words:
            continue
        index = words.index(marker)
        record: dict[str, str] = {}
        for word in words[index + 1 :]:
            if "=" not in word:
                continue
            key, value = word.split("=", 1)
            if key in record:
                raise EvidenceError(f"duplicate marker field: {line}")
            record[key] = value
        records.append(record)
    return records


def stats(cell: pathlib.Path) -> tuple[list[int], list[pathlib.Path]]:
    paths = [pathlib.Path(value) for value in sorted(glob.glob(str(cell / "stats-*.txt")))]
    total = [0, 0, 0, 0, 0]
    for path in paths:
        try:
            values = [int(line) for line in path.read_text(encoding="ascii").splitlines()]
        except (OSError, UnicodeError, ValueError) as exc:
            raise EvidenceError(f"invalid stats {path}: {exc}") from exc
        if len(values) != 5 or any(value < 0 for value in values):
            raise EvidenceError(f"stats must contain five nonnegative integers: {path}")
        total = [left + right for left, right in zip(total, values)]
    return total, paths


def validate_markers(stderr: str, frontend: str, threads: int, file_bytes: int) -> None:
    ready = fields(stderr, "PREPARE_READY")
    outcome = fields(stderr, "PREPARE_OUTCOME")
    if len(ready) != threads or len(outcome) != threads:
        raise EvidenceError(f"prepare marker count {len(ready)}/{len(outcome)} != {threads}")
    for record in ready:
        required = {"frontend", "mode", "off", "len", "prepared", "chunks", "fiemap", "unsafe_flags"}
        if not required.issubset(record):
            raise EvidenceError(f"PREPARE_READY missing fields: {record}")
        if record["frontend"] != frontend or record["mode"] != "0" or record["off"] != "0":
            raise EvidenceError(f"PREPARE_READY frontend/range mismatch: {record}")
        if int(record["len"]) != file_bytes or int(record["prepared"]) != file_bytes:
            raise EvidenceError(f"PREPARE_READY length mismatch: {record}")
        if int(record["chunks"]) <= 0 or record["fiemap"] != "safe" or int(record["unsafe_flags"], 0) != 0:
            raise EvidenceError(f"PREPARE_READY safety fields invalid: {record}")
    for record in outcome:
        required = {"v", "frontend", "fd", "mode", "off", "len", "outcome", "stage", "rc", "return_rc", "return_errno", "rdwr_alias", "prepared", "chunks"}
        if not required.issubset(record):
            raise EvidenceError(f"PREPARE_OUTCOME missing fields: {record}")
        if record["v"] != "1" or record["frontend"] != frontend or record["outcome"] != "prepared" or record["stage"] != "complete":
            raise EvidenceError(f"PREPARE_OUTCOME status mismatch: {record}")
        for key in ("mode", "off", "rc", "return_rc", "return_errno"):
            if int(record[key], 0) != 0:
                raise EvidenceError(f"PREPARE_OUTCOME failure field {key}: {record}")
        if int(record["len"]) != file_bytes or int(record["prepared"]) != file_bytes or int(record["rdwr_alias"]) != 1 or int(record["chunks"]) <= 0:
            raise EvidenceError(f"PREPARE_OUTCOME geometry mismatch: {record}")


def validate_cell(cell: pathlib.Path) -> dict[str, Any]:
    meta = load(cell / "meta.json")
    fio = load(cell / "fio.json")
    if meta.get("schema") != "exitos-fio-bpftime-e2e-cell-v1":
        raise EvidenceError(f"unsupported cell schema: {cell}")
    arm = meta.get("arm")
    if arm not in {"A", "B"}:
        raise EvidenceError(f"invalid arm: {cell}")
    threads = int(meta["threads"])
    file_bytes = int(meta["file_bytes"])
    bs_bytes = int(meta["bs_bytes"])
    jobs = fio.get("jobs")
    if not isinstance(jobs, list) or len(jobs) != threads:
        raise EvidenceError(f"job count mismatch in {cell}")
    write_ios = 0
    sync_ios = 0
    iops = 0.0
    for job in jobs:
        if not isinstance(job, dict) or int(job.get("error", -1)) != 0:
            raise EvidenceError(f"fio job failed in {cell}")
        write = job.get("write", {})
        sync = job.get("sync", {})
        ios = int(write.get("total_ios", 0))
        sync_n = int(sync.get("lat_ns", {}).get("N", 0))
        value = float(write.get("iops", 0.0))
        if ios <= 0 or sync_n <= 0 or value <= 0:
            raise EvidenceError(f"fio timing denominator is zero in {cell}")
        write_ios += ios
        sync_ios += sync_n
        iops += value
    expected_ios = threads * (file_bytes // bs_bytes)
    if write_ios != expected_ios:
        raise EvidenceError(f"write count {write_ios} != {expected_ios} in {cell}")
    if sync_ios not in {write_ios - threads, write_ios}:
        raise EvidenceError(f"sync count {sync_ios} is not a complete fdatasync stream in {cell}")
    stderr = (cell / "stderr.txt").read_text(encoding="utf-8", errors="replace")
    stat_values, stat_paths = stats(cell)
    if arm == "A":
        if stat_paths or fields(stderr, "PREPARE_READY") or fields(stderr, "PREPARE_OUTCOME"):
            raise EvidenceError(f"plain arm contains Exitos records: {cell}")
        if (cell / "prepare.txt").read_text() != "PREPARE_NOT_APPLICABLE arm=A\n":
            raise EvidenceError(f"plain setup record missing: {cell}")
        expected_stats = None
    else:
        expected_frontend = "bpftime" if meta.get("frontend") == "bpftime" else "ld_preload"
        validate_markers(stderr, expected_frontend, threads, file_bytes)
        if not stat_paths:
            raise EvidenceError(f"Exitos arm has no counters: {cell}")
        # Counters are FAST_WRITE, FAST_SYNC, PASS, DECLINED, REFRESH.
        # The Exitos arm takes the shortcut, so every timed write and every
        # timed fdatasync has to appear as taken over, with nothing handed
        # back and no registration refused. A correct readback cannot tell
        # "the shortcut ran and was right" from "the shortcut never ran";
        # these counters are what separate the two, so they are checked
        # exactly rather than loosely.
        fast_write, fast_sync, passed, declined, refresh = stat_values
        if fast_write != write_ios:
            raise EvidenceError(
                f"fast_write {fast_write} != {write_ios} in {cell}")
        if fast_sync != sync_ios:
            raise EvidenceError(
                f"fast_sync {fast_sync} != {sync_ios} in {cell}")
        if passed or declined:
            raise EvidenceError(
                f"shortcut fell back: pass={passed} declined={declined} in {cell}")
        if refresh < threads:
            raise EvidenceError(
                f"refresh {refresh} < one per file ({threads}) in {cell}")
    readback_text = (cell / "readback.txt").read_text(encoding="utf-8")
    wanted = f"READBACK_OK files={threads} bytes={threads * file_bytes} expected_byte=0x5a"
    if wanted not in readback_text.splitlines():
        raise EvidenceError(f"readback proof missing in {cell}")
    return {"cell": cell.name, "arm": arm, "arm_name": meta["arm_name"], "threads": threads, "workers": threads, "rep": int(meta["rep"]), "iops": iops, "write_ios": write_ios, "sync_ios": sync_ios, "worker_mode": meta.get("worker_mode", "thread")}


def summarize(result_dir: pathlib.Path) -> None:
    rows = []
    for cell in sorted((result_dir / "cells").iterdir()):
        if cell.is_dir() and (cell / "meta.json").exists():
            rows.append(validate_cell(cell))
    if not rows:
        raise EvidenceError("no completed cells")
    print(f"SUMMARY_OK cells={len(rows)}")
    # Thread and process cells are never pooled: they are different workloads,
    # and averaging them would hide exactly the comparison they exist to make.
    groups: dict[tuple[int, str, str], list[float]] = {}
    for row in rows:
        groups.setdefault(
            (row["workers"], row["worker_mode"], row["arm"]), []).append(row["iops"])
    print("workers worker_mode arm samples median_iops speedup_vs_plain_pct")
    for mode in sorted({row["worker_mode"] for row in rows}):
        for workers in sorted({row["workers"] for row in rows if row["worker_mode"] == mode}):
            key_a = (workers, mode, "A")
            baseline = statistics.median(groups[key_a]) if key_a in groups else None
            for arm in ("A", "B"):
                values = groups.get((workers, mode, arm))
                if not values:
                    continue
                median = statistics.median(values)
                speedup = "NA" if baseline is None else f"{(median / baseline - 1.0) * 100.0:.3f}"
                print(f"{workers} {mode} {arm} {len(values)} {median:.3f} {speedup}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check-pattern", action="store_true")
    parser.add_argument("--check-cell", type=pathlib.Path)
    parser.add_argument("--result-dir", type=pathlib.Path)
    parser.add_argument("--expected-byte", default="0x5a")
    parser.add_argument("--file-bytes", type=int)
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args()
    try:
        if args.check_pattern:
            if args.file_bytes is None or not args.paths:
                raise EvidenceError("--check-pattern needs --file-bytes and target files")
            readback([pathlib.Path(value) for value in args.paths], int(args.expected_byte, 0), args.file_bytes)
        elif args.check_cell is not None:
            row = validate_cell(args.check_cell)
            print(f"CELL_OK cell={row['cell']} arm={row['arm']} threads={row['threads']} iops={row['iops']:.3f}")
        elif args.result_dir is not None:
            summarize(args.result_dir)
        else:
            raise EvidenceError("select --check-pattern, --check-cell, or --result-dir")
    except (EvidenceError, OSError, ValueError, KeyError) as exc:
        print(f"E2E_EVIDENCE_REFUSE {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
