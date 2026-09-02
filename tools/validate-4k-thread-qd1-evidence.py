#!/usr/bin/env python3
"""Device-free validator for the fixed 4 KiB/QD1 thread experiment."""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import re
import statistics
import sys
from dataclasses import dataclass
from typing import Any, Iterable


INVALID_EXIT = 3
ENVIRONMENT_INVALID_EXIT = 4
CONFIGS = {"threads4": 4, "threads8": 8}
ARMS = ("B", "C")
ORDERS = (
    (("threads4", "B"), ("threads4", "C"),
     ("threads8", "B"), ("threads8", "C")),
    (("threads8", "C"), ("threads8", "B"),
     ("threads4", "C"), ("threads4", "B")),
    (("threads4", "C"), ("threads4", "B"),
     ("threads8", "C"), ("threads8", "B")),
    (("threads8", "B"), ("threads8", "C"),
     ("threads4", "B"), ("threads4", "C")),
    (("threads8", "B"), ("threads8", "C"),
     ("threads4", "B"), ("threads4", "C")),
    (("threads4", "C"), ("threads4", "B"),
     ("threads8", "C"), ("threads8", "B")),
    (("threads8", "C"), ("threads8", "B"),
     ("threads4", "C"), ("threads4", "B")),
    (("threads4", "B"), ("threads4", "C"),
     ("threads8", "B"), ("threads8", "C")),
)
STATS_NAMES = ("fast_write", "fast_sync", "pass", "declined", "refresh")
BLOCK_BYTES = 4096


class EvidenceError(Exception):
    """The supplied evidence is incomplete, malformed, or contradictory."""


@dataclass(frozen=True)
class Marker:
    line_number: int
    fields: dict[str, str]


def fail(message: str) -> None:
    raise EvidenceError(message)


def read_text(path: pathlib.Path, label: str) -> str:
    try:
        if not path.is_file():
            fail(f"{label}_not_regular_file={path}")
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        fail(f"{label}_not_utf8={path}:{error}")
    except OSError as error:
        fail(f"{label}_read_failed={path}:{error}")
    raise AssertionError("unreachable")


def load_json(path: pathlib.Path, label: str) -> Any:
    text = read_text(path, label)
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        fail(f"{label}_invalid_json={path}:{error}")


def require_json_int(value: Any, label: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        fail(f"{label}_not_integer_at_least_{minimum}={value!r}")
    return value


def require_finite_number(value: Any, label: str, *, positive: bool) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        fail(f"{label}_not_number={value!r}")
    number = float(value)
    if not math.isfinite(number) or (positive and number <= 0):
        fail(f"{label}_invalid_number={value!r}")
    return number


def parse_canonical_uint(value: str, label: str) -> int:
    if not re.fullmatch(r"0|[1-9][0-9]*", value):
        fail(f"{label}_not_canonical_uint={value!r}")
    return int(value)


def parse_int_field(fields: dict[str, str], key: str, label: str,
                    *, minimum: int | None = None) -> int:
    if key not in fields:
        fail(f"{label}_missing_field={key}")
    value = fields[key]
    if not re.fullmatch(r"-?(0|[1-9][0-9]*)", value):
        fail(f"{label}_{key}_not_canonical_int={value!r}")
    parsed = int(value)
    if minimum is not None and parsed < minimum:
        fail(f"{label}_{key}_below_{minimum}={parsed}")
    return parsed


def require_field(fields: dict[str, str], key: str, expected: str,
                  label: str) -> None:
    if key not in fields:
        fail(f"{label}_missing_field={key}")
    if fields[key] != expected:
        fail(f"{label}_{key}=expected:{expected},actual:{fields[key]}")


def collect_markers(lines: list[str], name: str) -> list[Marker]:
    pattern = re.compile(rf"(?:^|\s){re.escape(name)}(?:\s|$)")
    records: list[Marker] = []
    for line_number, line in enumerate(lines, start=1):
        match = pattern.search(line)
        if not match:
            continue
        tokens = line[match.start():].strip().split()
        if not tokens or tokens[0] != name:
            fail(f"{name}_parse_internal_error=line:{line_number}")
        fields: dict[str, str] = {}
        for token in tokens[1:]:
            if "=" not in token:
                fail(f"{name}_non_field_token=line:{line_number},token:{token}")
            key, value = token.split("=", 1)
            if not key or not value:
                fail(f"{name}_empty_key_or_value=line:{line_number},token:{token}")
            if key in fields:
                fail(f"{name}_duplicate_field=line:{line_number},field:{key}")
            fields[key] = value
        records.append(Marker(line_number, fields))
    return records


def validate_fio(payload: Any, jobs: int, expected_write_ios: int) -> tuple[int, int]:
    if not isinstance(payload, dict):
        fail("fio_root_not_object")
    fio_jobs = payload.get("jobs")
    if not isinstance(fio_jobs, list) or len(fio_jobs) != jobs:
        actual = len(fio_jobs) if isinstance(fio_jobs, list) else "not-list"
        fail(f"fio_job_count=expected:{jobs},actual:{actual}")
    if expected_write_ios % jobs != 0:
        fail(f"expected_write_ios_not_divisible_by_jobs={expected_write_ios}:{jobs}")
    expected_per_job = expected_write_ios // jobs
    if expected_per_job < 1:
        fail("expected_write_ios_too_small")
    total_write = 0
    total_sync = 0
    for index, job in enumerate(fio_jobs):
        label = f"fio_job_{index}"
        if not isinstance(job, dict):
            fail(f"{label}_not_object")
        error = require_json_int(job.get("error"), f"{label}_error")
        if error != 0:
            fail(f"{label}_error_nonzero={error}")
        write = job.get("write")
        if not isinstance(write, dict):
            fail(f"{label}_write_not_object")
        write_ios = require_json_int(
            write.get("total_ios"), f"{label}_write_total_ios", 1
        )
        if write_ios != expected_per_job:
            fail(
                f"{label}_write_total_ios=expected:{expected_per_job},"
                f"actual:{write_ios}"
            )
        require_finite_number(write.get("iops"), f"{label}_write_iops",
                              positive=True)
        sync = job.get("sync")
        if not isinstance(sync, dict) or not isinstance(sync.get("lat_ns"), dict):
            fail(f"{label}_sync_lat_ns_not_object")
        sync_ios = require_json_int(
            sync["lat_ns"].get("N"), f"{label}_sync_lat_ns_N"
        )
        if sync_ios != write_ios - 1:
            fail(
                f"{label}_sync_ios=expected:{write_ios - 1},actual:{sync_ios}"
            )
        total_write += write_ios
        total_sync += sync_ios
    expected_sync_ios = expected_write_ios - jobs
    if total_write != expected_write_ios:
        fail(f"write_ios=expected:{expected_write_ios},actual:{total_write}")
    if total_sync != expected_sync_ios:
        fail(f"sync_ios=expected:{expected_sync_ios},actual:{total_sync}")
    return total_write, total_sync


def validate_prepare(stderr_text: str, jobs: int, file_bytes: int,
                     expected_write_ios: int,
                     frontend: str) -> tuple[int, int, int]:
    lines = stderr_text.splitlines()
    ready = collect_markers(lines, "PREPARE_READY")
    outcomes = collect_markers(lines, "PREPARE_OUTCOME")
    failed = collect_markers(lines, "PREPARE_FAILED")
    stop_armed = collect_markers(lines, "PREPARE_STOP_ARMED")
    stop_ready = collect_markers(lines, "PREPARE_STOP_READY")
    stop_released = collect_markers(lines, "PREPARE_STOP_RELEASED")
    if failed:
        fail(f"prepare_failed_count=expected:0,actual:{len(failed)}")
    if len(ready) != jobs or len(outcomes) != jobs:
        fail(
            f"prepare_marker_count=ready:{len(ready)},outcome:{len(outcomes)},"
            f"expected_each:{jobs}"
        )
    if (len(stop_armed), len(stop_ready), len(stop_released)) != (1, 1, 1):
        fail(
            "prepare_stop_marker_count="
            f"armed:{len(stop_armed)},ready:{len(stop_ready)},"
            f"released:{len(stop_released)},expected_each:1"
        )

    for index, marker in enumerate(outcomes):
        label = f"PREPARE_OUTCOME_{index}"
        fields = marker.fields
        require_field(fields, "v", "1", label)
        require_field(fields, "frontend", frontend, label)
        parse_int_field(fields, "fd", label, minimum=0)
        if parse_int_field(fields, "mode", label) != 0:
            fail(f"{label}_mode_not_zero")
        if parse_int_field(fields, "off", label) != 0:
            fail(f"{label}_off_not_zero")
        if parse_int_field(fields, "len", label) != file_bytes:
            fail(f"{label}_len_not_file_bytes")
        require_field(fields, "outcome", "prepared", label)
        require_field(fields, "stage", "complete", label)
        for key in ("rc", "return_rc", "return_errno"):
            if parse_int_field(fields, key, label) != 0:
                fail(f"{label}_{key}_not_zero")
        if parse_int_field(fields, "rdwr_alias", label) != 1:
            fail(f"{label}_rdwr_alias_not_one")
        if parse_int_field(fields, "prepared", label) != file_bytes:
            fail(f"{label}_prepared_not_file_bytes")
        parse_int_field(fields, "chunks", label, minimum=1)

    for index, marker in enumerate(ready):
        label = f"PREPARE_READY_{index}"
        fields = marker.fields
        require_field(fields, "frontend", frontend, label)
        parse_int_field(fields, "fd", label, minimum=0)
        if parse_int_field(fields, "mode", label) != 0:
            fail(f"{label}_mode_not_zero")
        if parse_int_field(fields, "off", label) != 0:
            fail(f"{label}_off_not_zero")
        if parse_int_field(fields, "len", label) != file_bytes:
            fail(f"{label}_len_not_file_bytes")
        if parse_int_field(fields, "prepared", label) != file_bytes:
            fail(f"{label}_prepared_not_file_bytes")
        parse_int_field(fields, "chunks", label, minimum=1)
        require_field(fields, "fiemap", "safe", label)
        if parse_int_field(fields, "unsafe_flags", label) != 0:
            fail(f"{label}_unsafe_flags_not_zero")

    for index, (outcome, ready_record) in enumerate(zip(outcomes, ready)):
        outcome_fd = parse_int_field(outcome.fields, "fd", f"outcome_pair_{index}")
        ready_fd = parse_int_field(ready_record.fields, "fd", f"ready_pair_{index}")
        if outcome_fd != ready_fd:
            fail(
                f"prepare_pair_fd_mismatch=index:{index},"
                f"outcome:{outcome_fd},ready:{ready_fd}"
            )
        if outcome.line_number >= ready_record.line_number:
            fail(f"prepare_pair_order_invalid=index:{index}")

    stop_contract = (
        (stop_armed[0], "PREPARE_STOP_ARMED", "SIGSTOP", False),
        (stop_ready[0], "PREPARE_STOP_READY", "SIGSTOP", True),
        (stop_released[0], "PREPARE_STOP_RELEASED", "SIGCONT", False),
    )
    for marker, label, signal, has_after in stop_contract:
        fields = marker.fields
        require_field(fields, "v", "1", label)
        require_field(fields, "frontend", frontend, label)
        if parse_int_field(fields, "arrived", label) != jobs:
            fail(f"{label}_arrived_not_jobs")
        if parse_int_field(fields, "expected", label) != jobs:
            fail(f"{label}_expected_not_jobs")
        if label == "PREPARE_STOP_ARMED":
            require_field(fields, "stop_after", "selected-close", label)
        else:
            require_field(fields, "signal", signal, label)
        if has_after:
            require_field(fields, "after", "selected-close", label)
    if not (
        ready[-1].line_number < stop_armed[0].line_number <
        stop_ready[0].line_number < stop_released[0].line_number
    ):
        fail("prepare_stop_marker_order_invalid")

    fast_path_count = sum(line.count("-> FAST PATH") for line in lines)
    if file_bytes % BLOCK_BYTES != 0 or expected_write_ios % jobs != 0:
        fail("fast_path_lifecycle_geometry_invalid")
    per_job_bytes = (expected_write_ios // jobs) * BLOCK_BYTES
    timed_file_passes = (per_job_bytes + file_bytes - 1) // file_bytes
    # fio opens once during serialized setup, then closes/reopens the file for
    # every pass needed to satisfy io_size > size.  The real 4T cell writes
    # eight file passes (4 * (1 + 8) = 36 lifecycle registrations); 8T writes
    # four (8 * (1 + 4) = 40).  Every one must still register successfully.
    expected_fast_path = jobs * (1 + timed_file_passes)
    if fast_path_count != expected_fast_path:
        fail(
            f"fast_path_lifecycle=expected:{expected_fast_path},"
            f"actual:{fast_path_count}"
        )
    armed_count = sum(line.count("ARMED (syscall instructions rewritten)")
                      for line in lines)
    if armed_count != 1:
        fail(f"transformer_armed_count=expected:1,actual:{armed_count}")
    hook_pattern = re.compile(r"hook entered ([0-9]+) times, claimed ([0-9]+)")
    hook_rows = [match for line in lines for match in hook_pattern.findall(line)]
    if len(hook_rows) != 1:
        fail(f"hook_counter_count=expected:1,actual:{len(hook_rows)}")
    hook_entries, hook_claimed = map(int, hook_rows[0])
    if hook_entries <= 0 or hook_claimed <= 0 or hook_claimed > hook_entries:
        fail(
            f"hook_counter_invalid=entered:{hook_entries},claimed:{hook_claimed}"
        )
    return fast_path_count, hook_entries, hook_claimed


def validate_stats(path: pathlib.Path, arm: str, jobs: int,
                   expected_write_ios: int, expected_sync_ios: int) -> dict[str, int | str]:
    raw_lines = read_text(path, "stats").splitlines()
    if len(raw_lines) != 5:
        fail(f"stats_line_count=expected:5,actual:{len(raw_lines)}")
    values = [parse_canonical_uint(line, f"stats_{STATS_NAMES[index]}")
              for index, line in enumerate(raw_lines)]
    expected = ([0, 0, 0, 0, jobs] if arm == "B" else
                [expected_write_ios, expected_sync_ios, 0, 0, jobs])
    if values != expected:
        fail(f"stats_exact=arm:{arm},expected:{expected},actual:{values}")
    result: dict[str, int | str] = {"arm": arm}
    result.update(dict(zip(STATS_NAMES, values)))
    return result


def validate_cell(args: argparse.Namespace) -> dict[str, Any]:
    if args.jobs <= 0 or args.file_bytes <= 0 or args.expected_write_ios <= 0:
        fail("jobs_file_bytes_and_expected_write_ios_must_be_positive")
    fio_payload = load_json(args.fio_json, "fio_json")
    write_ios, sync_ios = validate_fio(
        fio_payload, args.jobs, args.expected_write_ios
    )
    stderr_text = read_text(args.stderr, "stderr")
    fast_path_count, hook_entries, hook_claimed = validate_prepare(
        stderr_text, args.jobs, args.file_bytes, write_ios, args.frontend
    )
    stats = validate_stats(
        args.stats, args.arm, args.jobs, write_ios, sync_ios
    )
    return {
        "format": "exitos-4k-thread-qd1-cell-evidence-v1",
        "status": "valid",
        "arm": args.arm,
        "frontend": args.frontend,
        "jobs": args.jobs,
        "file_bytes": args.file_bytes,
        "write_ios": write_ios,
        "sync_ios": sync_ios,
        "prepare_ready": args.jobs,
        "prepare_outcome_v1": args.jobs,
        "prepare_failed": 0,
        "fast_path_lifecycle": fast_path_count,
        "hook_entries": hook_entries,
        "hook_claimed": hook_claimed,
        "stats": stats,
    }


def parse_tsv_int(row: dict[str, str], field: str, row_number: int,
                  *, minimum: int = 0) -> int:
    if field not in row:
        fail(f"tsv_missing_column={field}")
    value = row[field]
    parsed = parse_canonical_uint(value, f"tsv_row_{row_number}_{field}")
    if parsed < minimum:
        fail(f"tsv_row_{row_number}_{field}_below_{minimum}={parsed}")
    return parsed


def parse_tsv_float(row: dict[str, str], field: str, row_number: int,
                    *, positive: bool) -> float:
    if field not in row:
        fail(f"tsv_missing_column={field}")
    try:
        parsed = float(row[field])
    except ValueError:
        fail(f"tsv_row_{row_number}_{field}_not_float={row[field]!r}")
    if not math.isfinite(parsed) or (positive and parsed <= 0):
        fail(f"tsv_row_{row_number}_{field}_invalid={parsed}")
    return parsed


def load_tsv(path: pathlib.Path) -> list[dict[str, Any]]:
    required = {
        "block", "position", "config", "jobs", "arm", "iops", "pair_us",
        "write_ios", "sync_ios", "write_lat_us", "sync_lat_us",
        "fast_write", "fast_sync", "pass", "declined", "refresh",
        "hook_entries", "hook_claimed", "timed_pre_temp_c",
        "timed_post_temp_c",
    }
    text = read_text(path, "tsv")
    try:
        reader = csv.DictReader(text.splitlines(), delimiter="\t")
        if reader.fieldnames is None:
            fail("tsv_header_missing")
        if len(reader.fieldnames) != len(set(reader.fieldnames)):
            fail("tsv_duplicate_header")
        missing = sorted(required - set(reader.fieldnames))
        if missing:
            fail(f"tsv_missing_columns={','.join(missing)}")
        raw_rows = list(reader)
    except csv.Error as error:
        fail(f"tsv_parse_error={error}")
    if len(raw_rows) != 32:
        fail(f"tsv_cell_count=expected:32,actual:{len(raw_rows)}")

    parsed_rows: list[dict[str, Any]] = []
    keys: set[tuple[int, str, str]] = set()
    positions: dict[int, set[int]] = {block: set() for block in range(8)}
    write_counts: set[int] = set()
    for row_number, raw in enumerate(raw_rows, start=2):
        if None in raw or any(value is None for value in raw.values()):
            fail(f"tsv_row_{row_number}_shape_invalid")
        block = parse_tsv_int(raw, "block", row_number)
        position = parse_tsv_int(raw, "position", row_number)
        if block not in range(8) or position not in range(4):
            fail(f"tsv_row_{row_number}_block_or_position_invalid")
        config = raw["config"]
        if config not in CONFIGS:
            fail(f"tsv_row_{row_number}_config_invalid={config}")
        jobs = parse_tsv_int(raw, "jobs", row_number, minimum=1)
        if jobs != CONFIGS[config]:
            fail(f"tsv_row_{row_number}_jobs_config_mismatch={jobs}:{config}")
        arm = raw["arm"]
        if arm not in ARMS:
            fail(f"tsv_row_{row_number}_arm_invalid={arm}")
        key = (block, config, arm)
        if key in keys:
            fail(f"tsv_duplicate_cell={key}")
        keys.add(key)
        if position in positions[block]:
            fail(f"tsv_duplicate_position=block:{block},position:{position}")
        positions[block].add(position)

        iops = parse_tsv_float(raw, "iops", row_number, positive=True)
        pair_us = parse_tsv_float(raw, "pair_us", row_number, positive=True)
        expected_pair_us = jobs * 1_000_000.0 / iops
        if not math.isclose(pair_us, expected_pair_us, rel_tol=1e-8,
                            abs_tol=1e-6):
            fail(
                f"tsv_row_{row_number}_pair_us=expected:{expected_pair_us},"
                f"actual:{pair_us}"
            )
        write_ios = parse_tsv_int(raw, "write_ios", row_number, minimum=1)
        sync_ios = parse_tsv_int(raw, "sync_ios", row_number)
        if sync_ios != write_ios - jobs:
            fail(
                f"tsv_row_{row_number}_sync_ios=expected:{write_ios - jobs},"
                f"actual:{sync_ios}"
            )
        write_counts.add(write_ios)
        fast_write = parse_tsv_int(raw, "fast_write", row_number)
        fast_sync = parse_tsv_int(raw, "fast_sync", row_number)
        pass_count = parse_tsv_int(raw, "pass", row_number)
        declined = parse_tsv_int(raw, "declined", row_number)
        refresh = parse_tsv_int(raw, "refresh", row_number)
        expected_stats = ((0, 0, 0, 0, jobs) if arm == "B" else
                          (write_ios, sync_ios, 0, 0, jobs))
        actual_stats = (fast_write, fast_sync, pass_count, declined, refresh)
        if actual_stats != expected_stats:
            fail(
                f"tsv_row_{row_number}_stats=expected:{expected_stats},"
                f"actual:{actual_stats}"
            )
        hook_entries = parse_tsv_int(raw, "hook_entries", row_number, minimum=1)
        hook_claimed = parse_tsv_int(raw, "hook_claimed", row_number, minimum=1)
        if hook_claimed > hook_entries:
            fail(f"tsv_row_{row_number}_hook_claimed_exceeds_entries")
        pre_temp = parse_tsv_float(
            raw, "timed_pre_temp_c", row_number, positive=False
        )
        post_temp = parse_tsv_float(
            raw, "timed_post_temp_c", row_number, positive=False
        )
        if not 0 <= pre_temp <= 100 or not 0 <= post_temp <= 100:
            fail(f"tsv_row_{row_number}_temperature_out_of_range")
        parse_tsv_float(raw, "write_lat_us", row_number, positive=False)
        parse_tsv_float(raw, "sync_lat_us", row_number, positive=False)
        parsed_rows.append({
            "block": block, "position": position, "config": config,
            "jobs": jobs, "arm": arm, "iops": iops, "pair_us": pair_us,
            "write_ios": write_ios, "sync_ios": sync_ios,
            "timed_pre_temp_c": pre_temp,
            "timed_post_temp_c": post_temp,
        })
    expected_keys = {
        (block, config, arm)
        for block in range(8) for config in CONFIGS for arm in ARMS
    }
    if keys != expected_keys:
        fail("tsv_matrix_keys_incomplete")
    if any(value != {0, 1, 2, 3} for value in positions.values()):
        fail("tsv_block_positions_incomplete")
    actual_orders = []
    for block in range(8):
        block_rows = sorted(
            (row for row in parsed_rows if row["block"] == block),
            key=lambda row: row["position"],
        )
        actual_orders.append(tuple((row["config"], row["arm"])
                                   for row in block_rows))
    if tuple(actual_orders) != ORDERS:
        fail("tsv_exact_balanced_order_mismatch")
    if len(write_counts) != 1:
        fail(f"tsv_write_volume_not_fixed={sorted(write_counts)}")
    return parsed_rows


def classify_effect(values: list[float]) -> dict[str, Any]:
    median = statistics.median(values)
    negative = sum(value < 0 for value in values)
    positive = sum(value > 0 for value in values)
    if median <= -5.0 and negative >= 7:
        label = "stable-regression"
    elif median >= 5.0 and positive >= 7:
        label = "stable-improvement"
    elif abs(median) < 5.0 and negative < 7 and positive < 7:
        label = "equivalent-at-5pct-gate"
    else:
        label = "inconclusive"
    return {
        "median_pct": median,
        "negative_pairs": negative,
        "positive_pairs": positive,
        "label": label,
    }


def summarize(args: argparse.Namespace) -> tuple[dict[str, Any], bool]:
    rows = load_tsv(args.tsv)
    by_key = {
        (row["block"], row["config"], row["arm"]): row for row in rows
    }
    paired: dict[str, list[float]] = {}
    paired_pair_us: dict[str, list[float]] = {}
    classifications: dict[str, dict[str, Any]] = {}
    median_iops: dict[str, float] = {}
    median_pair_us: dict[str, float] = {}
    environment: dict[str, Any] = {
        "timed_start_temperature_max_delta_c": args.max_start_temp_delta_c
    }
    environment_valid = True
    for config in CONFIGS:
        for arm in ARMS:
            values = [by_key[(block, config, arm)]["iops"] for block in range(8)]
            median_iops[f"{config}:{arm}"] = statistics.median(values)
            pair_values = [by_key[(block, config, arm)]["pair_us"]
                           for block in range(8)]
            median_pair_us[f"{config}:{arm}"] = statistics.median(pair_values)
        effects = [
            100.0 * (
                by_key[(block, config, "C")]["iops"] /
                by_key[(block, config, "B")]["iops"] - 1.0
            )
            for block in range(8)
        ]
        paired[config] = effects
        paired_pair_us[config] = [
            100.0 * (
                by_key[(block, config, "C")]["pair_us"] /
                by_key[(block, config, "B")]["pair_us"] - 1.0
            )
            for block in range(8)
        ]
        classifications[config] = classify_effect(effects)
        temperature_deltas = [
            abs(
                by_key[(block, config, "C")]["timed_pre_temp_c"] -
                by_key[(block, config, "B")]["timed_pre_temp_c"]
            )
            for block in range(8)
        ]
        within = all(
            delta <= args.max_start_temp_delta_c for delta in temperature_deltas
        )
        environment[config] = {
            "paired_timed_start_temperature_delta_c": temperature_deltas,
            "all_pairs_within_gate": within,
        }
        environment_valid = environment_valid and within

    ratio_of_ratios = []
    for block in range(8):
        ratio4 = (by_key[(block, "threads4", "C")]["iops"] /
                  by_key[(block, "threads4", "B")]["iops"])
        ratio8 = (by_key[(block, "threads8", "C")]["iops"] /
                  by_key[(block, "threads8", "B")]["iops"])
        ratio_of_ratios.append(100.0 * (ratio8 / ratio4 - 1.0))
    scaling = classify_effect(ratio_of_ratios)
    if scaling["label"] == "stable-regression":
        scaling["label"] = "stable-thread-scaling-regression"
    elif scaling["label"] == "stable-improvement":
        scaling["label"] = "stable-thread-scaling-improvement"
    else:
        scaling["label"] = "no-stable-thread-scaling-loss"
    scaling["paired_ratio_of_ratios_pct"] = ratio_of_ratios

    raw_trigger = (
        any(value["label"] == "stable-regression"
            for value in classifications.values()) or
        scaling["label"] == "stable-thread-scaling-regression"
    )
    environment["valid_for_conclusion"] = environment_valid
    result = {
        "format": "exitos-4k-thread-qd1-summary-v1",
        "cells": len(rows),
        "median_iops": median_iops,
        "median_pair_us": median_pair_us,
        "paired_c_vs_b_iops_pct": paired,
        "paired_c_vs_b_pair_us_pct": paired_pair_us,
        "classification": classifications,
        "thread_scaling": scaling,
        "environment": environment,
        "raw_diagnosis_trigger": raw_trigger,
        "diagnosis_trigger": raw_trigger if environment_valid else None,
        "conclusion_valid": environment_valid,
    }
    return result, environment_valid


def emit_json(payload: dict[str, Any], output: pathlib.Path | None) -> None:
    rendered = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if output is not None:
        try:
            output.write_text(rendered, encoding="utf-8")
        except OSError as error:
            fail(f"output_write_failed={output}:{error}")
    sys.stdout.write(rendered)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate fixed 4 KiB/QD1 thread experiment evidence"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    cell = subparsers.add_parser(
        "validate-cell", help="validate one B or C cell"
    )
    cell.add_argument("--fio-json", type=pathlib.Path, required=True)
    cell.add_argument("--stderr", type=pathlib.Path, required=True)
    cell.add_argument("--stats", type=pathlib.Path, required=True)
    cell.add_argument("--arm", choices=ARMS, required=True)
    cell.add_argument("--jobs", type=int, required=True)
    cell.add_argument("--file-bytes", type=int, required=True)
    cell.add_argument("--expected-write-ios", type=int, required=True)
    cell.add_argument("--frontend", default="bpftime",
                      choices=("bpftime", "ld_preload"))

    summary = subparsers.add_parser(
        "summarize", help="validate and summarize the complete 32-cell TSV"
    )
    summary.add_argument("--tsv", type=pathlib.Path, required=True)
    summary.add_argument("--output", type=pathlib.Path)
    summary.add_argument("--max-start-temp-delta-c", type=float, default=2.0)
    summary.add_argument("--allow-invalid-environment", action="store_true")
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "validate-cell":
            emit_json(validate_cell(args), None)
            return 0
        if not math.isfinite(args.max_start_temp_delta_c) or \
           args.max_start_temp_delta_c < 0:
            fail("max_start_temp_delta_c_invalid")
        payload, environment_valid = summarize(args)
        emit_json(payload, args.output)
        if not environment_valid and not args.allow_invalid_environment:
            print(
                "ENVIRONMENT_INVALID timed_start_temperature_gate_failed",
                file=sys.stderr,
            )
            return ENVIRONMENT_INVALID_EXIT
        return 0
    except EvidenceError as error:
        print(f"EVIDENCE_INVALID {error}", file=sys.stderr)
        return INVALID_EXIT


if __name__ == "__main__":
    raise SystemExit(main())
