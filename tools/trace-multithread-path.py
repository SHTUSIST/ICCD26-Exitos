#!/usr/bin/env python3
"""Parse Exitos multithread path traces.

The parser consumes only the explicit line protocol emitted by the companion
diagnostic tracer.  Its JSON is deliberately marked diagnostic-only and does
not calculate an operation rate.
"""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
import math
import os
import platform
import re
import signal
import stat
import subprocess
import sys
import time
from pathlib import Path


class TraceError(ValueError):
    pass


class ProcessGone(TraceError):
    pass


U16_MAX = (1 << 16) - 1
U32_MAX = (1 << 32) - 1
U64_MAX = (1 << 64) - 1
PID_MAX = (1 << 31) - 1
DEV_MAJOR_MAX = (1 << 12) - 1
DEV_MINOR_MAX = (1 << 20) - 1


REQUEST_TIME_ORDER = (
    "enter_ns",
    "tag_enter_ns",
    "tag_exit_ns",
    "map_enter_ns",
    "pages_enter_ns",
    "pages_exit_ns",
    "map_exit_ns",
    "queue_enter_ns",
    "dma_map_ns",
    "start_ns",
    "issue_ns",
    "queue_exit_ns",
    "first_poll_ns",
    "complete_ns",
    "exit_ns",
)

REQUEST_CPU_FIELDS = (
    "cpu_enter",
    "cpu_map",
    "cpu_queue",
    "cpu_complete",
    "iopoll_check_cpu",
    "iopoll_cpu",
    "blk_poll_cpu",
    "nvme_poll_cpu",
)

REQUEST_UINT_DOMAINS = {
    "tgid": PID_MAX,
    "tid": PID_MAX,
    "seq": U64_MAX,
    "lba": U64_MAX,
    "bytes": U32_MAX,
    "hctx": U32_MAX,
    "qid": U16_MAX,
    "polled": U32_MAX,
    "cmd_flags": U32_MAX,
    **{field: U32_MAX for field in REQUEST_CPU_FIELDS},
    **{field: U64_MAX for field in REQUEST_TIME_ORDER},
    **{
        f"{layer}_{suffix}": U64_MAX
        for layer in ("iopoll_check", "iopoll", "blk_poll", "nvme_poll")
        for suffix in ("enter_ns", "exit_ns", "count", "duration_ns")
    },
}

POLL_LAYERS = ("iopoll_check", "iopoll", "blk_poll", "nvme_poll")
REQ_POLLED_MASK = 1 << 22

DRIFT_FIELDS = (
    "block_shape_errors",
    "block_errors",
    "collisions",
    "order_errors",
    "cpu_drift",
    "dev_drift",
    "lba_drift",
    "hctx_drift",
    "qid_drift",
    "poll_drift",
    "unpolled",
)

SEGMENTS = {
    "tag": ("tag_enter_ns", "tag_exit_ns"),
    "user_map": ("map_enter_ns", "map_exit_ns"),
    "page_pin": ("pages_enter_ns", "pages_exit_ns"),
    "pre_submit_map_tag": ("enter_ns", "queue_enter_ns"),
    "queue_to_issue": ("queue_enter_ns", "issue_ns"),
    "issue_to_queue_return": ("issue_ns", "queue_exit_ns"),
    "issue_to_first_poll": ("issue_ns", "first_poll_ns"),
    "queue_return_to_first_poll": ("queue_exit_ns", "first_poll_ns"),
    "first_poll_to_complete": ("first_poll_ns", "complete_ns"),
    "complete_to_syscall_exit": ("complete_ns", "exit_ns"),
    "observed_call": ("enter_ns", "exit_ns"),
}

REQUIRED_PROBES = {
    "tracepoint:raw_syscalls:sys_enter": {"id"},
    "tracepoint:raw_syscalls:sys_exit": {"id", "ret"},
    "fentry:vmlinux:blk_rq_map_user_iov": {"q", "rq", "map_data", "iter", "gfp_mask"},
    "fexit:vmlinux:blk_rq_map_user_iov": {"q", "rq", "map_data", "iter", "gfp_mask", "retval"},
    "fentry:vmlinux:bio_iov_iter_get_pages": {"bio", "iter", "len_align_mask"},
    "fexit:vmlinux:bio_iov_iter_get_pages": {"bio", "iter", "len_align_mask", "retval"},
    "fentry:vmlinux:blk_mq_get_tag": {"data"},
    "fexit:vmlinux:blk_mq_get_tag": {"data", "retval"},
    "fentry:vmlinux:blk_mq_start_request": {"rq"},
    "fentry:vmlinux:blk_mq_end_request": {"rq", "error"},
    "fexit:nvme:nvme_map_data": {"req", "retval"},
    "fentry:nvme:nvme_queue_rq": {"hctx", "bd"},
    "fexit:nvme:nvme_queue_rq": {"hctx", "bd", "retval"},
    "fentry:vmlinux:blk_mq_poll": {"q", "cookie", "iob", "flags"},
    "fexit:vmlinux:blk_mq_poll": {"q", "cookie", "iob", "flags", "retval"},
    "fentry:nvme:nvme_poll": {"hctx", "iob"},
    "fexit:nvme:nvme_poll": {"hctx", "iob", "retval"},
    "fentry:vmlinux:io_do_iopoll": {"ctx", "force_nonspin"},
    "fexit:vmlinux:io_do_iopoll": {"ctx", "force_nonspin", "retval"},
    "fentry:vmlinux:io_iopoll_check": {"ctx", "min"},
    "fexit:vmlinux:io_iopoll_check": {"ctx", "min", "retval"},
    "tracepoint:block:block_rq_issue": {"dev", "sector", "bytes", "rwbs"},
    "tracepoint:block:block_rq_complete": {"dev", "sector", "error", "rwbs"},
}


def parse_fields(line: str) -> tuple[str, dict[str, str]]:
    words = line.split()
    if not words:
        raise TraceError("empty trace line")
    fields: dict[str, str] = {}
    for token in words[1:]:
        if token.count("=") != 1:
            raise TraceError(f"malformed field: {token!r}")
        key, value = token.split("=", 1)
        if not key or not value:
            raise TraceError(f"malformed field: {token!r}")
        if key in fields:
            raise TraceError(f"duplicate field: {key}")
        fields[key] = value
    return words[0], fields


def parse_bounded_uint(
    text: str, label: str, maximum: int, minimum: int = 0
) -> int:
    if (
        not isinstance(text, str)
        or not text
        or not text.isascii()
        or not text.isdecimal()
        or len(text) > len(str(maximum))
        or (len(text) == len(str(maximum)) and text > str(maximum))
    ):
        raise TraceError(f"field {label} is outside its unsigned integer domain")
    try:
        value = int(text, 10)
    except ValueError as exc:
        raise TraceError(f"field {label} is outside its unsigned integer domain") from exc
    if value < minimum or value > maximum:
        raise TraceError(f"field {label} is outside its unsigned integer domain")
    return value


def parse_bounded_sint(text: str, label: str, minimum: int, maximum: int) -> int:
    if not isinstance(text, str) or not re.fullmatch(r"-?[0-9]+", text):
        raise TraceError(f"field {label} is outside its signed integer domain")
    digits = text[1:] if text.startswith("-") else text
    if len(digits) > max(len(str(abs(minimum))), len(str(abs(maximum)))):
        raise TraceError(f"field {label} is outside its signed integer domain")
    try:
        value = int(text, 10)
    except ValueError as exc:
        raise TraceError(f"field {label} is outside its signed integer domain") from exc
    if value < minimum or value > maximum:
        raise TraceError(f"field {label} is outside its signed integer domain")
    return value


def as_uint(
    fields: dict[str, str], key: str, maximum: int = U64_MAX, minimum: int = 0
) -> int:
    try:
        text = fields[key]
    except KeyError as exc:
        raise TraceError(f"missing field: {key}") from exc
    return parse_bounded_uint(text, key, maximum, minimum)


def request_uint(fields: dict[str, str], key: str, minimum: int = 0) -> int:
    try:
        maximum = REQUEST_UINT_DOMAINS[key]
    except KeyError as exc:
        raise TraceError(f"request field has no integer domain: {key}") from exc
    return as_uint(fields, key, maximum, minimum)


def parse_uint_csv(fields: dict[str, str], key: str, maximum: int) -> set[int]:
    try:
        parts = fields[key].split(",")
    except KeyError as exc:
        raise TraceError(f"missing field: {key}") from exc
    if not parts:
        raise TraceError(f"field {key} is not an unsigned decimal CSV")
    values = [parse_bounded_uint(part, key, maximum) for part in parts]
    if len(values) != len(set(values)):
        raise TraceError(f"field {key} contains duplicate values")
    return set(values)


def parse_range(text: str, label: str, maximum: int) -> tuple[int, int]:
    parts = text.split(":")
    if len(parts) != 2:
        raise TraceError(f"field {label} is not an unsigned range")
    start, end = (parse_bounded_uint(part, label, maximum) for part in parts)
    if end < start:
        raise TraceError(f"field {label} has an inverted range")
    return start, end


def parse_pair(
    text: str, label: str, first_maximum: int, second_maximum: int
) -> tuple[int, int]:
    parts = text.split(":")
    if len(parts) != 2:
        raise TraceError(f"field {label} is not an unsigned pair")
    return (
        parse_bounded_uint(parts[0], label, first_maximum),
        parse_bounded_uint(parts[1], label, second_maximum),
    )


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def summarize_values(values: list[int]) -> dict[str, int]:
    if not values:
        return {"samples": 0, "p50": 0, "p95": 0, "p99": 0}
    return {
        "samples": len(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
    }


EXPECTED_KEYS = {
    "schema",
    "pass",
    "pass_id",
    "tgids",
    "cpus",
    "syscalls",
    "dev",
    "lba",
    "bytes",
    "hctxs",
    "qids",
}


def validate_expected(value: object, raw: bytes) -> tuple[dict[str, object], str]:
    if not isinstance(value, dict) or set(value) != EXPECTED_KEYS or value.get("schema") != 1:
        raise TraceError("expected configuration has the wrong schema")
    if value.get("pass") != "path":
        raise TraceError("expected configuration is not a path pass")
    pass_id = value.get("pass_id")
    if not isinstance(pass_id, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", pass_id):
        raise TraceError("expected configuration has an invalid pass identifier")
    expected_domains = {
        "tgids": PID_MAX,
        "cpus": U32_MAX,
        "syscalls": U32_MAX,
        "hctxs": U32_MAX,
        "qids": U16_MAX,
    }
    for key, maximum in expected_domains.items():
        items = value.get(key)
        if (
            not isinstance(items, list)
            or not items
            or any(
                type(item) is not int or item < 0 or item > maximum
                for item in items
            )
            or len(items) != len(set(items))
        ):
            raise TraceError(f"expected configuration has an invalid {key} allowlist")
    dev = value.get("dev")
    if (
        not isinstance(dev, list)
        or len(dev) != 2
        or type(dev[0]) is not int
        or type(dev[1]) is not int
        or not (0 <= dev[0] <= DEV_MAJOR_MAX)
        or not (0 <= dev[1] <= DEV_MINOR_MAX)
    ):
        raise TraceError("expected configuration has an invalid dev pair")
    lba = value.get("lba")
    if (
        not isinstance(lba, list)
        or len(lba) != 2
        or any(type(item) is not int or item < 0 or item > U64_MAX for item in lba)
    ):
        raise TraceError("expected configuration has an invalid lba pair")
    if value["lba"][1] < value["lba"][0]:  # type: ignore[index]
        raise TraceError("expected configuration has an inverted LBA range")
    if value.get("bytes") != 4096:
        raise TraceError("expected configuration is not exactly 4 KiB")
    return value, hashlib.sha256(raw).hexdigest()


def load_expected(path: Path | dict[str, object]) -> tuple[dict[str, object], str]:
    if isinstance(path, dict):
        raw = read_retained_file(path, "expected configuration")
    else:
        raw = path.read_bytes()
    try:
        value = json.loads(raw.decode("ascii"))
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise TraceError("expected configuration is not ASCII JSON") from exc
    return validate_expected(value, raw)


def parse_trace(
    path: Path | dict[str, object],
    transport_lost: int,
    expected_path: Path | dict[str, object],
) -> dict[str, object]:
    if transport_lost != 0:
        raise TraceError("transport reported lost events")

    expected, expected_sha256 = load_expected(expected_path)
    if isinstance(path, dict):
        trace_bytes = read_retained_file(path, "trace output")
    else:
        trace_bytes = path.read_bytes()
    records = [parse_fields(line) for line in trace_bytes.decode("ascii").splitlines() if line]
    if not records or records[0][0] != "TRACE_BEGIN":
        raise TraceError("TRACE_BEGIN is not the first trace record")
    if records[-1][0] != "TRACE_END":
        raise TraceError("TRACE_END is not the last trace record")
    allowed_record_types = {"TRACE_BEGIN", "TRACE_NOISE", "TRACE_REQUEST", "TRACE_END"}
    unknown_record_types = sorted({kind for kind, _fields in records} - allowed_record_types)
    if unknown_record_types:
        raise TraceError(f"unknown trace record type: {unknown_record_types[0]}")
    begins = [fields for kind, fields in records if kind == "TRACE_BEGIN"]
    ends = [fields for kind, fields in records if kind == "TRACE_END"]
    requests = [fields for kind, fields in records if kind == "TRACE_REQUEST"]
    noise_records = [fields for kind, fields in records if kind == "TRACE_NOISE"]
    if len(begins) != 1 or len(ends) != 1:
        raise TraceError("trace must contain exactly one begin and one end record")
    begin = begins[0]
    end = ends[0]
    if begin.get("schema") != "1":
        raise TraceError("unsupported trace schema")
    if begin.get("marker") != "TRACE_DIAGNOSTIC_ONLY":
        raise TraceError("trace is not marked diagnostic-only")
    if begin.get("pass") != "path":
        raise TraceError("unsupported or mixed trace pass")
    if begin.get("pass_id") != expected["pass_id"] or end.get("pass_id") != begin.get("pass_id"):
        raise TraceError("pass identifier mismatch")
    allowed_tgids = parse_uint_csv(begin, "tgids", PID_MAX)
    if len(allowed_tgids) != 1:
        raise TraceError("trace contract requires exactly one TGID")
    allowed_cpus = parse_uint_csv(begin, "cpus", U32_MAX)
    allowed_syscalls = parse_uint_csv(begin, "syscalls", U32_MAX)
    allowed_hctxs = parse_uint_csv(begin, "hctxs", U32_MAX)
    allowed_qids = parse_uint_csv(begin, "qids", U16_MAX)
    try:
        allowed_dev = parse_pair(
            begin["dev"], "dev", DEV_MAJOR_MAX, DEV_MINOR_MAX
        )
        lba_start, lba_end = parse_range(begin["lba"], "lba", U64_MAX)
    except KeyError as exc:
        raise TraceError(f"missing field: {exc.args[0]}") from exc
    expected_lists = {
        "tgids": allowed_tgids,
        "cpus": allowed_cpus,
        "syscalls": allowed_syscalls,
        "hctxs": allowed_hctxs,
        "qids": allowed_qids,
    }
    for key, actual in expected_lists.items():
        if actual != set(expected[key]):  # type: ignore[arg-type]
            raise TraceError(f"trace {key} allowlist disagrees with expected configuration")
    if allowed_dev != tuple(expected["dev"]):  # type: ignore[arg-type]
        raise TraceError("trace device disagrees with expected configuration")
    if (lba_start, lba_end) != tuple(expected["lba"]):  # type: ignore[arg-type]
        raise TraceError("trace LBA range disagrees with expected configuration")
    matched = as_uint(end, "matched")
    candidates = as_uint(end, "candidates")
    if candidates == 0:
        raise TraceError("trace contains no candidate requests")
    if len(requests) != as_uint(end, "emitted"):
        raise TraceError("emitted request count disagrees with trace")
    if matched != len(requests):
        raise TraceError("matched request count disagrees with trace")
    if matched * 1000 < candidates * 999:
        raise TraceError("matched request ratio is below 99.9 percent")
    unmatched = as_uint(end, "unmatched")
    if unmatched != candidates - matched:
        raise TraceError("unmatched request count disagrees with trace")
    issues = as_uint(end, "issues")
    completes = as_uint(end, "completes")
    if issues != completes or issues != matched:
        raise TraceError("aggregate issue/completion counts are not paired")
    block_issues = as_uint(end, "block_issues")
    block_completes = as_uint(end, "block_completes")
    if block_issues != block_completes or block_issues != matched:
        raise TraceError("block tracepoint issue/completion counts are not paired")
    poll_totals = {
        "iopoll_check": as_uint(end, "iopoll_checks"),
        "iopoll": as_uint(end, "iopolls"),
        "blk_poll": as_uint(end, "blk_polls"),
        "nvme_poll": as_uint(end, "nvme_polls"),
    }
    if any(value < matched for value in poll_totals.values()):
        raise TraceError("aggregate polling counts do not cover every matched request")
    for field in DRIFT_FIELDS:
        if as_uint(end, field) != 0:
            raise TraceError(f"tracer reported nonzero {field}")
    for noise in noise_records:
        if noise.get("pass_id") != begin["pass_id"]:
            raise TraceError("noise record belongs to another pass")
        if noise.get("kind") not in {"irq", "wakeup", "switch"}:
            raise TraceError("noise record has an unsupported kind")
        as_uint(noise, "count")

    request_lifetimes: dict[int, list[tuple[int, int, tuple[int, int, int]]]] = {}
    seen_identities: set[tuple[int, int, int]] = set()
    tid_intervals: dict[tuple[int, int], list[tuple[int, int, int]]] = {}
    observed_poll_totals = {layer: 0 for layer in POLL_LAYERS}
    for request in requests:
        if request.get("pass_id") != begin["pass_id"]:
            raise TraceError("request belongs to another pass")
        token = request.get("req", "")
        if (
            not re.fullmatch(r"0x[0-9a-fA-F]{1,16}", token)
            or int(token, 16) == 0
        ):
            raise TraceError("request record has no nonzero request token")
        token_value = int(token, 16)
        if request.get("issue_req") != token or request.get("complete_req") != token:
            raise TraceError("issue/completion request token mismatch")
        timeline = [request_uint(request, key) for key in REQUEST_TIME_ORDER]
        if any(timestamp == 0 for timestamp in timeline):
            raise TraceError("request record has a missing timestamp")
        if any(after < before for before, after in zip(timeline, timeline[1:])):
            raise TraceError("request record has a negative or out-of-order duration")
        request_tgid = request_uint(request, "tgid")
        request_tid = request_uint(request, "tid")
        request_seq = request_uint(request, "seq", 1)
        if request_tgid not in allowed_tgids:
            raise TraceError("request TGID is outside the allowlist")
        identity = (request_tgid, request_tid, request_seq)
        if identity in seen_identities:
            raise TraceError("duplicate TGID/TID/sequence identity")
        seen_identities.add(identity)
        request_lifetimes.setdefault(token_value, []).append(
            (
                request_uint(request, "map_enter_ns"),
                request_uint(request, "complete_ns"),
                identity,
            )
        )
        tid_intervals.setdefault((request_tgid, request_tid), []).append(
            (
                request_uint(request, "enter_ns"),
                request_uint(request, "exit_ns"),
                request_seq,
            )
        )
        if any(
            request_uint(request, field) not in allowed_cpus
            for field in REQUEST_CPU_FIELDS
        ):
            raise TraceError("request CPU is outside the allowlist")
        try:
            request_dev = parse_pair(
                request["dev"], "request dev", DEV_MAJOR_MAX, DEV_MINOR_MAX
            )
        except KeyError as exc:
            raise TraceError("missing field: dev") from exc
        if request_dev != allowed_dev:
            raise TraceError("request device is outside the allowlist")
        request_lba = request_uint(request, "lba")
        if request_lba < lba_start or request_lba > lba_end:
            raise TraceError("request LBA is outside the allowlist")
        if request_uint(request, "bytes") != 4096:
            raise TraceError("request is not exactly 4 KiB")
        if request_uint(request, "hctx") not in allowed_hctxs:
            raise TraceError("request hctx is outside the allowlist")
        if request_uint(request, "qid") not in allowed_qids:
            raise TraceError("request qid is outside the allowlist")
        cmd_flags = request_uint(request, "cmd_flags")
        if request_uint(request, "polled") != 1 or not (cmd_flags & REQ_POLLED_MASK):
            raise TraceError("request was not submitted with REQ_POLLED")
        first_poll_ns = request_uint(request, "first_poll_ns")
        completion_ns = request_uint(request, "complete_ns")
        queue_exit_ns = request_uint(request, "queue_exit_ns")
        for layer in POLL_LAYERS:
            count = request_uint(request, f"{layer}_count")
            duration = request_uint(request, f"{layer}_duration_ns")
            enter = request_uint(request, f"{layer}_enter_ns")
            exit_ns = request_uint(request, f"{layer}_exit_ns")
            if (
                count == 0
                or duration == 0
                or enter < queue_exit_ns
                or enter > completion_ns
                or exit_ns < completion_ns
                or exit_ns < enter
            ):
                raise TraceError(f"request has incomplete {layer} participation")
            observed_poll_totals[layer] += count
        if first_poll_ns != request_uint(request, "iopoll_check_enter_ns"):
            raise TraceError("first poll timestamp is not bound to io_iopoll_check entry")

    if observed_poll_totals != poll_totals:
        raise TraceError("aggregate polling counts disagree with request records")

    for intervals in tid_intervals.values():
        intervals.sort()
        for previous, current in zip(intervals, intervals[1:]):
            previous_enter, previous_exit, previous_seq = previous
            current_enter, _current_exit, current_seq = current
            if current_enter < previous_exit:
                raise TraceError("one TID has overlapping requests instead of QD1")
            if current_seq <= previous_seq:
                raise TraceError("one TID has non-monotonic request sequence numbers")

    for lifetimes in request_lifetimes.values():
        lifetimes.sort()
        for previous, current in zip(lifetimes, lifetimes[1:]):
            _previous_start, previous_end, _previous_identity = previous
            current_start, _current_end, _current_identity = current
            if current_start < previous_end:
                raise TraceError("one request pointer has overlapping lifetimes")

    segment_summary: dict[str, dict[str, int]] = {}
    for name, (start_key, end_key) in SEGMENTS.items():
        values = [
            request_uint(request, end_key) - request_uint(request, start_key)
            for request in requests
        ]
        segment_summary[name] = summarize_values(values)
    segment_summary["accumulated_poll_cpu"] = summarize_values(
        [request_uint(request, "iopoll_check_duration_ns") for request in requests]
    )

    records_per_tid: dict[str, int] = {}
    for _tgid, tid, _seq in seen_identities:
        key = str(tid)
        records_per_tid[key] = records_per_tid.get(key, 0) + 1

    summary: dict[str, object] = {
        "schema": 1,
        "marker": "TRACE_DIAGNOSTIC_ONLY",
        "pass": "path",
        "pass_id": begin["pass_id"],
        "quality": {
            "candidates": candidates,
            "matched": matched,
            "matched_per_mille": matched * 1000 // candidates,
            "transport_lost": transport_lost,
        },
        "request_records": len(requests),
        "observed_tgids": sorted({tgid for tgid, _tid, _seq in seen_identities}),
        "observed_tids": sorted({tid for _tgid, tid, _seq in seen_identities}),
        "records_per_tid": dict(sorted(records_per_tid.items(), key=lambda item: int(item[0]))),
        "trace_sha256": hashlib.sha256(trace_bytes).hexdigest(),
        "expected_config_sha256": expected_sha256,
        "segments_ns": segment_summary,
        "optional_noise": {
            kind: sum(as_uint(record, "count") for record in noise_records if record["kind"] == kind)
            for kind in ("irq", "wakeup", "switch")
        },
        "cross_pass_arithmetic": "FORBIDDEN",
    }
    validate_runtime_shape(
        summary,
        TRACE_THREAD_COUNT,
        TRACE_PER_THREAD_CANDIDATES,
        TRACE_EXPECTED_CANDIDATES,
    )
    return summary


def validate_inventory(inventory: object, raw: bytes) -> dict[str, object]:
    if not isinstance(inventory, dict) or inventory.get("schema") != 1:
        raise TraceError("unsupported probe inventory schema")
    kernel_release = inventory.get("kernel_release")
    if not isinstance(kernel_release, str) or not re.fullmatch(
        r"[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:[-+.][A-Za-z0-9_.+-]+)*",
        kernel_release,
    ):
        raise TraceError("probe inventory has an invalid kernel release")
    btf_sha256 = inventory.get("btf_sha256")
    if not isinstance(btf_sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", btf_sha256):
        raise TraceError("probe inventory has no canonical BTF SHA-256")
    bpftrace_version = inventory.get("bpftrace_version")
    if not isinstance(bpftrace_version, str) or not re.fullmatch(r"bpftrace v0\.25\.[0-9]+", bpftrace_version):
        raise TraceError("probe inventory is not from the supported bpftrace 0.25 ABI")
    probes = inventory.get("probes")
    if not isinstance(probes, dict):
        raise TraceError("probe inventory has no probe map")
    for probe, required_fields in REQUIRED_PROBES.items():
        actual_fields = probes.get(probe)
        if not isinstance(actual_fields, list) or any(not isinstance(field, str) for field in actual_fields):
            raise TraceError(f"required probe is missing: {probe}")
        missing_fields = sorted(required_fields - set(actual_fields))
        if missing_fields:
            raise TraceError(f"probe {probe} is missing field: {missing_fields[0]}")
    return {
        "schema": 1,
        "marker": "TRACE_DIAGNOSTIC_ONLY",
        "status": "PROBE_ABI_VERIFIED",
        "kernel_release": kernel_release,
        "btf_sha256": btf_sha256,
        "bpftrace_version": bpftrace_version,
        "inventory_sha256": hashlib.sha256(raw).hexdigest(),
        "required_probes": sorted(REQUIRED_PROBES),
        "cross_pass_arithmetic": "FORBIDDEN",
    }


def preflight_inventory(path: Path) -> dict[str, object]:
    raw = path.read_bytes()
    try:
        inventory = json.loads(raw.decode("ascii"))
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise TraceError("probe inventory is not ASCII JSON") from exc
    return validate_inventory(inventory, raw)


def run_readonly(command: list[str], pass_fds: tuple[int, ...] = ()) -> str:
    try:
        completed = subprocess.run(
            command,
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            timeout=10,
            pass_fds=pass_fds,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise TraceError(f"read-only probe query failed: {command[0]}") from exc
    if completed.returncode != 0:
        raise TraceError(f"read-only probe query returned {completed.returncode}: {command[-1]}")
    if completed.stderr:
        raise TraceError(f"read-only probe query wrote stderr: {command[-1]}")
    return completed.stdout


def collect_inventory(bpftrace: Path, btf: Path, kernel_release: str) -> dict[str, object]:
    retained_inputs = {
        "bpftrace": open_retained_input(bpftrace, "bpftrace executable", executable=True),
        "btf": open_retained_input(btf, "BTF input"),
    }
    bpftrace_fd = int(retained_inputs["bpftrace"]["fd"])
    btf_bytes = bytes(retained_inputs["btf"]["bytes"])
    if not btf_bytes:
        raise TraceError("BTF file is empty")
    bpftrace_exec = f"/proc/self/fd/{bpftrace_fd}"
    version_lines = run_readonly(
        [bpftrace_exec, "--version"], (bpftrace_fd,)
    ).splitlines()
    if len(version_lines) != 1:
        raise TraceError("bpftrace version query did not return exactly one line")

    probes: dict[str, list[str]] = {}
    for probe, required_fields in REQUIRED_PROBES.items():
        lines = run_readonly(
            [bpftrace_exec, "-lv", probe], (bpftrace_fd,)
        ).splitlines()
        if not lines or lines[0].strip() != probe:
            raise TraceError(f"verbose listing did not identify exact probe: {probe}")
        fields: list[str] = []
        for line in lines[1:]:
            match = re.search(
                r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]\r\n]*\])?\s*$",
                line,
            )
            if match:
                fields.append(match.group(1))
        if len(fields) != len(set(fields)):
            raise TraceError(f"verbose listing repeats a field: {probe}")
        missing = sorted(required_fields - set(fields))
        if missing:
            raise TraceError(f"probe {probe} is missing field: {missing[0]}")
        probes[probe] = fields

    inventory: dict[str, object] = {
        "schema": 1,
        "kernel_release": kernel_release,
        "btf_sha256": hashlib.sha256(btf_bytes).hexdigest(),
        "bpftrace_version": version_lines[0],
        "probes": probes,
    }
    canonical = json.dumps(inventory, sort_keys=True, separators=(",", ":")).encode("ascii")
    validate_inventory(inventory, canonical)
    return inventory


def render_template(
    template_path: Path,
    inventory_path: Path,
    expected_path: Path,
    output_path: Path,
    output_descriptor: int | None = None,
) -> dict[str, object]:
    if output_descriptor is None and output_path.exists():
        raise TraceError("render output already exists")
    inventory_raw = inventory_path.read_bytes()
    try:
        inventory = json.loads(inventory_raw.decode("ascii"))
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise TraceError("probe inventory is not ASCII JSON") from exc
    preflight = validate_inventory(inventory, inventory_raw)
    expected, expected_sha256 = load_expected(expected_path)
    template_raw = template_path.read_bytes()
    try:
        rendered = template_raw.decode("ascii")
    except UnicodeError as exc:
        raise TraceError("trace template is not ASCII") from exc

    def values(key: str) -> list[int]:
        result = expected[key]
        if not isinstance(result, list):
            raise TraceError(f"expected configuration field is not a list: {key}")
        return [int(item) for item in result]

    def expression(key: str, variable: str) -> str:
        return "(" + " || ".join(f"{variable} == {item}" for item in values(key)) + ")"

    dev_major, dev_minor = values("dev")
    lba_start, lba_end = values("lba")
    dev_t = (dev_minor & 0xFF) | (dev_major << 8) | ((dev_minor & ~0xFF) << 12)
    replacements = {
        "@@PASS_ID@@": str(expected["pass_id"]),
        "@@TGIDS_CSV@@": ",".join(str(item) for item in values("tgids")),
        "@@CPUS_CSV@@": ",".join(str(item) for item in values("cpus")),
        "@@SYSCALLS_CSV@@": ",".join(str(item) for item in values("syscalls")),
        "@@HCTXS_CSV@@": ",".join(str(item) for item in values("hctxs")),
        "@@QIDS_CSV@@": ",".join(str(item) for item in values("qids")),
        "@@TGID_EXPR@@": expression("tgids", "pid"),
        "@@CPU_EXPR@@": expression("cpus", "cpu"),
        "@@SYSCALL_EXPR@@": expression("syscalls", "args.id"),
        "@@HCTX_EXPR@@": expression("hctxs", "$hctx"),
        "@@QID_EXPR@@": expression("qids", "$qid"),
        "@@DEV_MAJOR@@": str(dev_major),
        "@@DEV_MINOR@@": str(dev_minor),
        "@@DEV_T@@": str(dev_t),
        "@@LBA_START@@": str(lba_start),
        "@@LBA_END@@": str(lba_end),
        "@@BYTES@@": str(expected["bytes"]),
    }
    present = set(re.findall(r"@@[A-Z0-9_]+@@", rendered))
    missing = sorted(set(replacements) - present)
    unknown = sorted(present - set(replacements))
    if missing:
        raise TraceError(f"trace template is missing placeholder: {missing[0]}")
    if unknown:
        raise TraceError(f"trace template has unknown placeholder: {unknown[0]}")
    for placeholder, value in replacements.items():
        rendered = rendered.replace(placeholder, value)
    if re.search(r"@@[A-Z0-9_]+@@", rendered):
        raise TraceError("rendered trace still contains a placeholder")
    encoded = rendered.encode("ascii")
    if output_descriptor is None:
        output_path.write_bytes(encoded)
    else:
        metadata = os.fstat(output_descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size != 0:
            raise TraceError("precreated render output is not an empty regular file")
        os.lseek(output_descriptor, 0, os.SEEK_SET)
        write_all(output_descriptor, encoded)
        os.fsync(output_descriptor)
    return {
        "schema": 1,
        "marker": "TRACE_DIAGNOSTIC_ONLY",
        "pass": "path",
        "pass_id": expected["pass_id"],
        "kernel_release": preflight["kernel_release"],
        "btf_sha256": preflight["btf_sha256"],
        "bpftrace_version": preflight["bpftrace_version"],
        "inventory_sha256": hashlib.sha256(inventory_raw).hexdigest(),
        "expected_config_sha256": expected_sha256,
        "template_sha256": hashlib.sha256(template_raw).hexdigest(),
        "script_sha256": hashlib.sha256(encoded).hexdigest(),
        "cross_pass_arithmetic": "FORBIDDEN",
    }


RUNTIME_TGID_SENTINEL = "SPAWNED_WORKLOAD"
TRACE_THREAD_COUNT = 8
TRACE_PER_THREAD_CANDIDATES = 128
TRACE_EXPECTED_CANDIDATES = TRACE_THREAD_COUNT * TRACE_PER_THREAD_CANDIDATES
RUNTIME_CONFIG_KEYS = EXPECTED_KEYS | {
    "thread_count",
    "per_thread_candidates",
    "expected_candidates",
}
READY_PATTERN = re.compile(
    r"BOUND_IO_READY pid=([0-9]+) expected_parent=([0-9]+) "
    r"pgid=([0-9]+) sid=([0-9]+) policy=([0-9]+) "
    r"nice=(-?[0-9]+) affinity=([0-9,-]+)"
)


def canonical_cpu_list(cpus: set[int]) -> str:
    ordered = sorted(cpus)
    if not ordered:
        raise TraceError("empty CPU allowlist")
    groups: list[str] = []
    first = last = ordered[0]
    for value in ordered[1:]:
        if value == last + 1:
            last = value
            continue
        groups.append(str(first) if first == last else f"{first}-{last}")
        first = last = value
    groups.append(str(first) if first == last else f"{first}-{last}")
    return ",".join(groups)


def materialize_runtime_config(
    path: Path, workload_pid: int
) -> tuple[dict[str, object], bytes, str, int, int, int]:
    raw = path.read_bytes()
    try:
        value = json.loads(raw.decode("ascii"))
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise TraceError("runtime configuration is not ASCII JSON") from exc
    if not isinstance(value, dict) or set(value) != RUNTIME_CONFIG_KEYS:
        raise TraceError("runtime configuration has the wrong schema")
    if value.get("tgids") != RUNTIME_TGID_SENTINEL:
        raise TraceError("runtime configuration does not bind the spawned workload TGID")
    thread_count = value.get("thread_count")
    per_thread_candidates = value.get("per_thread_candidates")
    expected_candidates = value.get("expected_candidates")
    if (
        thread_count != TRACE_THREAD_COUNT
        or per_thread_candidates != TRACE_PER_THREAD_CANDIDATES
        or expected_candidates != TRACE_EXPECTED_CANDIDATES
        or type(thread_count) is not int
        or type(per_thread_candidates) is not int
        or type(expected_candidates) is not int
    ):
        raise TraceError("runtime configuration does not bind the exact 8x128=1024 trace shape")
    materialized = dict(value)
    for key in ("thread_count", "per_thread_candidates", "expected_candidates"):
        del materialized[key]
    materialized["tgids"] = [workload_pid]
    encoded = (json.dumps(materialized, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
    expected, _expected_sha256 = validate_expected(materialized, encoded)
    return (
        expected,
        encoded,
        hashlib.sha256(raw).hexdigest(),
        thread_count,
        per_thread_candidates,
        expected_candidates,
    )


def validate_runtime_shape(
    summary: dict[str, object],
    thread_count: int,
    per_thread_candidates: int,
    expected_candidates: int,
) -> None:
    quality = summary.get("quality")
    if not isinstance(quality, dict):
        raise TraceError("runtime summary has no quality counters")
    if (
        quality.get("candidates") != expected_candidates
        or quality.get("matched") != expected_candidates
    ):
        raise TraceError("trace candidate count disagrees with the exact finite diagnostic contract")
    records_per_tid = summary.get("records_per_tid")
    if not isinstance(records_per_tid, dict) or len(records_per_tid) != thread_count:
        raise TraceError("observed TID count disagrees with the exact thread contract")
    if any(
        not isinstance(tid, str)
        or not tid.isascii()
        or not tid.isdecimal()
        or type(count) is not int
        or count != per_thread_candidates
        for tid, count in records_per_tid.items()
    ):
        raise TraceError("observed per-TID request count disagrees with the exact thread contract")
    if sum(records_per_tid.values()) != expected_candidates:
        raise TraceError("observed per-TID request counts disagree with the total contract")


def read_process_snapshot(pid: int) -> dict[str, object]:
    try:
        status_text = Path(f"/proc/{pid}/status").read_text(encoding="ascii")
        stat_text = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
        tids = sorted(
            int(item.name)
            for item in Path(f"/proc/{pid}/task").iterdir()
            if item.name.isdecimal()
        )
    except (FileNotFoundError, ProcessLookupError) as exc:
        raise ProcessGone("approved workload process disappeared") from exc
    status: dict[str, str] = {}
    for line in status_text.splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            status[key] = value.strip()
    required = ("State", "Tgid", "Pid", "PPid", "Threads")
    if any(key not in status for key in required):
        raise TraceError("approved workload has an incomplete /proc status")
    close = stat_text.rfind(")")
    if close < 0:
        raise TraceError("approved workload has a malformed /proc stat")
    stat_fields = stat_text[close + 2 :].split()
    if len(stat_fields) < 20:
        raise TraceError("approved workload has a truncated /proc stat")
    try:
        return {
            "state": status["State"].split()[0],
            "tgid": int(status["Tgid"]),
            "pid": int(status["Pid"]),
            "ppid": int(status["PPid"]),
            "threads": int(status["Threads"]),
            "tids": tids,
            "starttime": int(stat_fields[19]),
        }
    except (IndexError, ValueError) as exc:
        raise TraceError("approved workload has non-numeric /proc identity fields") from exc


def read_small_owned_regular(path: Path, label: str) -> bytes:
    flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise TraceError(f"cannot open {label} as a no-follow regular file") from exc
    try:
        metadata = os.fstat(descriptor)
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or metadata.st_nlink != 1
        ):
            raise TraceError(f"{label} is not an owned single-link regular file")
        if metadata.st_size > 4096:
            raise TraceError(f"{label} is oversized")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(descriptor, min(4097 - total, 4096))
            if not chunk:
                return b"".join(chunks)
            chunks.append(chunk)
            total += len(chunk)
            if total > 4096:
                raise TraceError(f"{label} is oversized")
    finally:
        os.close(descriptor)


def read_small_owned_regular_at(
    directory: dict[str, object], name: str, label: str
) -> bytes:
    validate_leaf_name(name, label)
    verify_retained_directory(directory)
    flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
    try:
        descriptor = os.open(name, flags, dir_fd=int(directory["fd"]))
    except OSError as exc:
        raise TraceError(f"cannot open {label} as a no-follow regular file") from exc
    try:
        metadata = os.fstat(descriptor)
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or metadata.st_nlink != 1
        ):
            raise TraceError(f"{label} is not an owned single-link regular file")
        if metadata.st_size > 4096:
            raise TraceError(f"{label} is oversized")
        raw = read_descriptor(descriptor)
        if len(raw) > 4096:
            raise TraceError(f"{label} is oversized")
        name_metadata = os.stat(
            name, dir_fd=int(directory["fd"]), follow_symlinks=False
        )
        if stable_identity(name_metadata) != stable_identity(metadata):
            raise TraceError(f"{label} changed while it was read")
        return raw
    finally:
        os.close(descriptor)


def read_descriptor(descriptor: int) -> bytes:
    chunks: list[bytes] = []
    offset = 0
    while True:
        chunk = os.pread(descriptor, 1024 * 1024, offset)
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)
        offset += len(chunk)


def open_retained_input(
    path: Path, label: str, *, executable: bool = False
) -> dict[str, object]:
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    except OSError as exc:
        raise TraceError(f"cannot open {label} as a retained no-follow file") from exc
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise TraceError(f"{label} is not a regular file")
        if executable and not metadata.st_mode & 0o111:
            raise TraceError(f"{label} is not executable")
        raw = read_descriptor(descriptor)
        return {
            "fd": descriptor,
            "identity": (
                metadata.st_dev,
                metadata.st_ino,
                metadata.st_mode,
                metadata.st_uid,
                metadata.st_gid,
                metadata.st_size,
                metadata.st_mtime_ns,
                metadata.st_ctime_ns,
            ),
            "bytes": raw,
            "sha256": hashlib.sha256(raw).hexdigest(),
        }
    except BaseException:
        os.close(descriptor)
        raise


def verify_retained_inputs(retained_inputs: dict[str, dict[str, object]]) -> None:
    for label, retained in retained_inputs.items():
        descriptor = int(retained["fd"])
        metadata = os.fstat(descriptor)
        identity = (
            metadata.st_dev,
            metadata.st_ino,
            metadata.st_mode,
            metadata.st_uid,
            metadata.st_gid,
            metadata.st_size,
            metadata.st_mtime_ns,
            metadata.st_ctime_ns,
        )
        if identity != retained["identity"]:
            raise TraceError(f"retained {label} identity changed")
        if hashlib.sha256(read_descriptor(descriptor)).hexdigest() != retained["sha256"]:
            raise TraceError(f"retained {label} content changed")


def stable_identity(metadata: os.stat_result) -> tuple[int, int, int, int, int]:
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_uid,
        metadata.st_gid,
    )


def open_retained_directory(
    path: Path, label: str, *, create: bool = False
) -> dict[str, object]:
    absolute = path.absolute()
    if create:
        try:
            os.mkdir(absolute, 0o700)
        except OSError as exc:
            raise TraceError(f"cannot create exclusive {label}") from exc
    try:
        descriptor = os.open(
            absolute, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
        )
    except OSError as exc:
        raise TraceError(f"cannot open {label} as a retained no-follow directory") from exc
    metadata = os.fstat(descriptor)
    if (
        not stat.S_ISDIR(metadata.st_mode)
        or metadata.st_uid != os.geteuid()
        or (create and stat.S_IMODE(metadata.st_mode) != 0o700)
    ):
        os.close(descriptor)
        raise TraceError(f"{label} has an unsafe directory identity")
    result: dict[str, object] = {
        "fd": descriptor,
        "path": absolute,
        "identity": stable_identity(metadata),
        "label": label,
    }
    verify_retained_directory(result)
    return result


def verify_retained_directory(directory: dict[str, object]) -> None:
    descriptor = int(directory["fd"])
    label = str(directory["label"])
    try:
        fd_metadata = os.fstat(descriptor)
        path_metadata = os.stat(Path(directory["path"]), follow_symlinks=False)
    except (OSError, ValueError) as exc:
        raise TraceError(f"retained {label} directory binding disappeared") from exc
    if (
        not stat.S_ISDIR(path_metadata.st_mode)
        or stable_identity(fd_metadata) != directory["identity"]
        or stable_identity(path_metadata) != directory["identity"]
    ):
        raise TraceError(f"retained {label} directory binding changed")


def validate_leaf_name(name: str, label: str) -> None:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", name):
        raise TraceError(f"{label} has an invalid leaf name")


def output_file_record(
    directory: dict[str, object], name: str, descriptor: int
) -> dict[str, object]:
    metadata = os.fstat(descriptor)
    if (
        not stat.S_ISREG(metadata.st_mode)
        or metadata.st_uid != os.geteuid()
        or metadata.st_nlink != 1
    ):
        raise TraceError(f"runtime output {name} has an unsafe file identity")
    return {
        "fd": descriptor,
        "directory": directory,
        "name": name,
        "identity": stable_identity(metadata),
    }


def write_all(descriptor: int, raw: bytes) -> None:
    offset = 0
    while offset < len(raw):
        written = os.write(descriptor, raw[offset:])
        if written <= 0:
            raise TraceError("short write to retained runtime output")
        offset += written


def create_retained_output(
    directory: dict[str, object], name: str, raw: bytes = b""
) -> dict[str, object]:
    validate_leaf_name(name, "runtime output")
    verify_retained_directory(directory)
    flags = os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW
    try:
        descriptor = os.open(name, flags, 0o600, dir_fd=int(directory["fd"]))
    except OSError as exc:
        raise TraceError(f"cannot exclusively create runtime output: {name}") from exc
    try:
        record = output_file_record(directory, name, descriptor)
        write_all(descriptor, raw)
        os.fsync(descriptor)
        verify_retained_file(record, f"runtime output {name}")
        return record
    except BaseException:
        os.close(descriptor)
        raise


def verify_retained_file(record: dict[str, object], label: str) -> None:
    directory = record["directory"]
    if not isinstance(directory, dict):
        raise TraceError(f"retained {label} has no directory identity")
    verify_retained_directory(directory)
    descriptor = int(record["fd"])
    name = str(record["name"])
    try:
        fd_metadata = os.fstat(descriptor)
        name_metadata = os.stat(
            name, dir_fd=int(directory["fd"]), follow_symlinks=False
        )
    except OSError as exc:
        raise TraceError(f"retained {label} file binding disappeared") from exc
    if (
        not stat.S_ISREG(name_metadata.st_mode)
        or stable_identity(fd_metadata) != record["identity"]
        or stable_identity(name_metadata) != record["identity"]
    ):
        raise TraceError(f"retained {label} file binding changed")


def read_retained_file(record: dict[str, object], label: str) -> bytes:
    verify_retained_file(record, label)
    return read_descriptor(int(record["fd"]))


def retained_path(record: dict[str, object]) -> Path:
    return Path(f"/proc/self/fd/{int(record['fd'])}")


def verify_stopped_workload(
    workload_pid: int,
    runner_pid: int,
    ready_file: Path,
    expected_cpus: set[int],
    ready_raw: bytes | None = None,
) -> tuple[dict[str, object], str]:
    if workload_pid <= 1 or runner_pid <= 1:
        raise TraceError("invalid approved workload or runner PID")
    if runner_pid != os.getppid():
        raise TraceError("runtime collector is not a direct child of the approved runner")
    if ready_raw is None:
        ready_raw = read_small_owned_regular(
            ready_file, "approved workload READY record"
        )
    try:
        ready_lines = ready_raw.decode("ascii").splitlines()
    except UnicodeError as exc:
        raise TraceError("approved workload READY record is not ASCII") from exc
    if len(ready_lines) != 1:
        raise TraceError("approved workload READY record is not exactly one line")
    match = READY_PATTERN.fullmatch(ready_lines[0])
    if not match:
        raise TraceError("approved workload READY record is malformed")
    ready_pid, ready_parent, ready_pgid, ready_sid = (
        parse_bounded_uint(match.group(index), "READY pid", PID_MAX, 1)
        for index in range(1, 5)
    )
    policy = parse_bounded_uint(match.group(5), "READY policy", U32_MAX)
    nice = parse_bounded_sint(match.group(6), "READY nice", -20, 19)
    expected_affinity = canonical_cpu_list(expected_cpus)
    if (
        ready_pid != workload_pid
        or ready_parent != runner_pid
        or ready_pgid != workload_pid
        or ready_sid != workload_pid
        or policy != os.SCHED_OTHER
        or nice != -20
        or match.group(7) != expected_affinity
    ):
        raise TraceError("approved workload READY record disagrees with the trace contract")
    snapshot = read_process_snapshot(workload_pid)
    if (
        snapshot["state"] not in {"T", "t"}
        or snapshot["tgid"] != workload_pid
        or snapshot["pid"] != workload_pid
        or snapshot["ppid"] != runner_pid
        or snapshot["threads"] != 1
        or snapshot["tids"] != [workload_pid]
        or os.getpgid(workload_pid) != workload_pid
        or os.getsid(workload_pid) != workload_pid
        or os.sched_getscheduler(workload_pid) != os.SCHED_OTHER
        or os.getpriority(os.PRIO_PROCESS, workload_pid) != -20
        or os.sched_getaffinity(workload_pid) != expected_cpus
    ):
        raise TraceError("approved workload is not stopped with the required identity and priority")
    return snapshot, ready_lines[0]


def same_process(pid: int, starttime: int) -> dict[str, object]:
    snapshot = read_process_snapshot(pid)
    if snapshot["starttime"] != starttime or snapshot["tgid"] != pid:
        raise TraceError("approved workload PID identity changed")
    return snapshot


def wait_for_attach_barrier(
    process: subprocess.Popen[bytes],
    trace_source: Path | dict[str, object],
    stderr_source: Path | dict[str, object],
    expected_probes: int,
    timeout: int,
) -> None:
    deadline = time.monotonic() + timeout
    marker = b"TRACE_BEGIN schema=1 marker=TRACE_DIAGNOSTIC_ONLY pass=path "
    attaching = f"Attaching {expected_probes} probes...".encode("ascii")
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise TraceError(
                f"bpftrace exited before the attach barrier with status {process.returncode}"
            )
        if isinstance(trace_source, dict):
            trace_lines = read_retained_file(trace_source, "trace output").splitlines()
        else:
            try:
                trace_lines = trace_source.read_bytes().splitlines()
            except FileNotFoundError:
                trace_lines = []
        if isinstance(stderr_source, dict):
            stderr_lines = read_retained_file(
                stderr_source, "trace stderr output"
            ).splitlines()
        else:
            try:
                stderr_lines = stderr_source.read_bytes().splitlines()
            except FileNotFoundError:
                stderr_lines = []
        begin_first = bool(trace_lines) and trace_lines[0].startswith(marker)
        attaching_exact = attaching in stderr_lines
        # bpftrace prints the Attaching status before attachment, while BEGIN
        # executes only after all clauses are attached.  Requiring both records
        # therefore makes the combined barrier post-attach.
        if begin_first and attaching_exact:
            return
        time.sleep(0.01)
    raise TraceError(
        "bpftrace did not emit TRACE_BEGIN plus exact Attaching probe barrier before timeout"
    )


def wait_for_completion_record(
    path: Path,
    pid: int,
    starttime: int,
    collector_pid: int,
    ready_sha256: str,
    timeout: int,
) -> tuple[int, bytes]:
    directory = open_retained_directory(path.parent, "completion parent")
    return wait_for_completion_record_at(
        directory,
        path.name,
        pid,
        starttime,
        collector_pid,
        ready_sha256,
        timeout,
    )


def wait_for_completion_record_at(
    directory: dict[str, object],
    name: str,
    pid: int,
    starttime: int,
    collector_pid: int,
    ready_sha256: str,
    timeout: int,
) -> tuple[int, bytes]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        verify_retained_directory(directory)
        try:
            os.stat(name, dir_fd=int(directory["fd"]), follow_symlinks=False)
        except FileNotFoundError:
            time.sleep(0.01)
            continue
        except OSError as exc:
            raise TraceError("cannot inspect approved workload completion record") from exc
        else:
            raw = read_small_owned_regular_at(
                directory, name, "approved workload completion record"
            )
            try:
                lines = raw.decode("ascii").splitlines()
            except UnicodeError as exc:
                raise TraceError("approved workload completion record is not ASCII") from exc
            if len(lines) != 1:
                raise TraceError("approved workload completion record is not exactly one line")
            match = re.fullmatch(
                r"TRACE_WORKLOAD_COMPLETE pid=([0-9]+) starttime=([0-9]+) "
                r"collector_pid=([0-9]+) ready_sha256=([0-9a-f]{64}) "
                r"exit_code=(-?[0-9]+)",
                lines[0],
            )
            if not match:
                raise TraceError("approved workload completion record is malformed")
            completion_pid = parse_bounded_uint(
                match.group(1), "completion pid", PID_MAX, 1
            )
            completion_starttime = parse_bounded_uint(
                match.group(2), "completion starttime", U64_MAX
            )
            completion_collector = parse_bounded_uint(
                match.group(3), "completion collector pid", PID_MAX, 1
            )
            completion_exit = parse_bounded_sint(
                match.group(5), "completion exit code", -255, 255
            )
            if (
                completion_pid != pid
                or completion_starttime != starttime
                or completion_collector != collector_pid
                or match.group(4) != ready_sha256
            ):
                raise TraceError("approved workload completion record is malformed")
            try:
                snapshot = read_process_snapshot(pid)
            except ProcessGone:
                snapshot = None
            if snapshot is not None:
                if snapshot["starttime"] != starttime:
                    raise TraceError("approved workload PID was reused before completion")
                if snapshot["state"] != "Z":
                    raise TraceError("approved runner declared completion before workload exit")
            return completion_exit, raw
    raise TraceError("approved runner did not publish workload completion in time")


def process_group_live(pgid: int) -> bool:
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            stat_text = (entry / "stat").read_text(encoding="ascii")
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
        close = stat_text.rfind(")")
        if close < 0:
            continue
        fields = stat_text[close + 2 :].split()
        if len(fields) >= 3 and fields[0] != "Z" and int(fields[2]) == pgid:
            return True
    return False


def stop_tracer(process: subprocess.Popen[bytes], timeout: int) -> int:
    pgid = process.pid
    try:
        os.killpg(pgid, signal.SIGINT)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        process.poll()
        if not process_group_live(pgid):
            return process.wait() if process.returncode is None else process.returncode
        time.sleep(0.01)
    try:
        os.killpg(pgid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()
    kill_deadline = time.monotonic() + timeout
    while time.monotonic() < kill_deadline:
        if not process_group_live(pgid):
            break
        time.sleep(0.01)
    if process_group_live(pgid):
        raise TraceError("bpftrace session survived SIGKILL")
    raise TraceError("bpftrace session did not stop after SIGINT")


def scan_transport_lost(path: Path) -> int:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise TraceError("cannot read bpftrace stderr") from exc
    return scan_transport_lost_bytes(raw)


def scan_transport_lost_bytes(raw: bytes) -> int:
    try:
        text = raw.decode("utf-8")
    except UnicodeError as exc:
        raise TraceError("bpftrace stderr is not UTF-8") from exc
    total = 0
    for line in text.splitlines():
        if re.search(r"\blost\b", line, re.IGNORECASE):
            matches = re.findall(r"\blost\s+([0-9]+)\s+events?\b", line, re.IGNORECASE)
            if not matches:
                raise TraceError("bpftrace reported an unparseable lost-event diagnostic")
            total += sum(
                parse_bounded_uint(item, "lost events", U64_MAX)
                for item in matches
            )
            if total > U64_MAX:
                raise TraceError("bpftrace lost-event total exceeds u64")
    return total


def publish_atomic(
    directory: dict[str, object], name: str, raw: bytes
) -> dict[str, object]:
    validate_leaf_name(name, "atomic runtime output")
    temporary = f"tmp-{os.getpid():x}-{time.monotonic_ns():x}"
    temporary_record = create_retained_output(directory, temporary, raw)
    descriptor = int(directory["fd"])
    try:
        verify_retained_directory(directory)
        try:
            os.link(
                temporary,
                name,
                src_dir_fd=descriptor,
                dst_dir_fd=descriptor,
                follow_symlinks=False,
            )
        except FileExistsError as exc:
            raise TraceError(f"atomic runtime output already exists: {name}") from exc
        except OSError as exc:
            if exc.errno in {errno.EEXIST, errno.ELOOP}:
                raise TraceError(f"atomic runtime output already exists: {name}") from exc
            raise TraceError(f"cannot atomically publish runtime output: {name}") from exc
        os.unlink(temporary, dir_fd=descriptor)
        temporary_record["name"] = name
        os.fsync(descriptor)
        verify_retained_file(temporary_record, f"atomic runtime output {name}")
        return temporary_record
    except BaseException:
        try:
            os.unlink(temporary, dir_fd=descriptor)
        except OSError:
            pass
        raise


def collect_runtime(
    inventory_path: Path,
    config_path: Path,
    template_path: Path,
    bpftrace: Path,
    btf: Path,
    kernel_release: str,
    output_dir: Path,
    workload_pid: int,
    runner_pid: int,
    ready_file: Path,
    completion_file: Path,
    start_timeout: int,
    workload_timeout: int,
    stop_timeout: int,
) -> dict[str, object]:
    completion_directory = open_retained_directory(
        completion_file.parent, "completion parent"
    )
    validate_leaf_name(completion_file.name, "completion record")
    try:
        os.stat(
            completion_file.name,
            dir_fd=int(completion_directory["fd"]),
            follow_symlinks=False,
        )
    except FileNotFoundError:
        pass
    except OSError as exc:
        raise TraceError("cannot inspect approved workload completion record") from exc
    else:
        raise TraceError("approved workload completion record already exists")
    retained_inputs: dict[str, dict[str, object]] = {
        "inventory": open_retained_input(inventory_path, "probe inventory"),
        "runtime config": open_retained_input(config_path, "runtime configuration"),
        "template": open_retained_input(template_path, "trace template"),
        "bpftrace": open_retained_input(
            bpftrace, "bpftrace executable", executable=True
        ),
        "BTF": open_retained_input(btf, "BTF input"),
        "READY": open_retained_input(ready_file, "approved workload READY record"),
    }
    inventory_source = retained_path(retained_inputs["inventory"])
    config_source = retained_path(retained_inputs["runtime config"])
    template_source = retained_path(retained_inputs["template"])
    (
        expected,
        expected_bytes,
        runtime_config_sha256,
        thread_count,
        per_thread_candidates,
        expected_candidates,
    ) = materialize_runtime_config(config_source, workload_pid)
    if runtime_config_sha256 != retained_inputs["runtime config"]["sha256"]:
        raise TraceError("runtime configuration disagrees with its retained input")
    expected_cpus = set(expected["cpus"])  # type: ignore[arg-type]
    initial, ready_record = verify_stopped_workload(
        workload_pid,
        runner_pid,
        ready_file,
        expected_cpus,
        bytes(retained_inputs["READY"]["bytes"]),
    )
    ready_input_sha256 = hashlib.sha256((ready_record + "\n").encode("ascii")).hexdigest()
    if ready_input_sha256 != retained_inputs["READY"]["sha256"]:
        raise TraceError("READY record disagrees with its retained input")
    starttime = int(initial["starttime"])

    preflight = preflight_inventory(inventory_source)
    if preflight["inventory_sha256"] != retained_inputs["inventory"]["sha256"]:
        raise TraceError("probe inventory disagrees with its retained input")
    if preflight["kernel_release"] != kernel_release:
        raise TraceError("live kernel release disagrees with the frozen inventory")
    btf_bytes = bytes(retained_inputs["BTF"]["bytes"])
    if not btf_bytes or hashlib.sha256(btf_bytes).hexdigest() != preflight["btf_sha256"]:
        raise TraceError("live BTF disagrees with the frozen inventory")
    bpftrace_sha256 = str(retained_inputs["bpftrace"]["sha256"])
    bpftrace_fd = int(retained_inputs["bpftrace"]["fd"])
    bpftrace_exec = f"/proc/self/fd/{bpftrace_fd}"
    version_lines = run_readonly(
        [bpftrace_exec, "--version"], (bpftrace_fd,)
    ).splitlines()
    if version_lines != [preflight["bpftrace_version"]]:
        raise TraceError("live bpftrace version disagrees with the frozen inventory")

    output_directory = open_retained_directory(
        output_dir, "runtime output", create=True
    )
    verify_retained_directory(output_directory)
    expected_output = create_retained_output(
        output_directory, "expected.json", expected_bytes
    )
    rendered_output = create_retained_output(output_directory, "rendered.bt")
    render_manifest = render_template(
        template_source,
        inventory_source,
        retained_path(expected_output),
        retained_path(rendered_output),
        output_descriptor=int(rendered_output["fd"]),
    )
    expected_output_bytes = read_retained_file(
        expected_output, "materialized expected configuration"
    )
    rendered_output_bytes = read_retained_file(
        rendered_output, "rendered trace script"
    )
    if (
        render_manifest["template_sha256"] != retained_inputs["template"]["sha256"]
        or render_manifest["inventory_sha256"] != retained_inputs["inventory"]["sha256"]
        or render_manifest["expected_config_sha256"]
        != hashlib.sha256(expected_output_bytes).hexdigest()
        or render_manifest["script_sha256"]
        != hashlib.sha256(rendered_output_bytes).hexdigest()
    ):
        raise TraceError("render manifest disagrees with retained critical inputs")
    render_manifest_output = publish_atomic(
        output_directory, "render-manifest.json", encode_json(render_manifest)
    )
    rendered_fd = int(rendered_output["fd"])
    rendered_exec = f"/proc/self/fd/{rendered_fd}"
    codegen = run_readonly(
        [bpftrace_exec, "--mode", "codegen", rendered_exec],
        (bpftrace_fd, rendered_fd),
    )
    codegen_output = create_retained_output(
        output_directory, "codegen.txt", codegen.encode("utf-8")
    )
    verify_retained_inputs(retained_inputs)
    for record, label in (
        (expected_output, "materialized expected configuration"),
        (rendered_output, "rendered trace script"),
        (render_manifest_output, "render manifest"),
        (codegen_output, "codegen output"),
    ):
        verify_retained_file(record, label)
    after_codegen, after_codegen_ready = verify_stopped_workload(
        workload_pid,
        runner_pid,
        ready_file,
        expected_cpus,
        bytes(retained_inputs["READY"]["bytes"]),
    )
    if after_codegen != initial or after_codegen_ready != ready_record:
        raise TraceError("approved workload identity changed during codegen")
    expected_config_sha256 = hashlib.sha256(expected_bytes).hexdigest()
    rendered_script_sha256 = hashlib.sha256(rendered_output_bytes).hexdigest()
    codegen_sha256 = hashlib.sha256(
        read_retained_file(codegen_output, "codegen output")
    ).hexdigest()

    trace_output = create_retained_output(output_directory, "trace.txt")
    trace_stderr_output = create_retained_output(
        output_directory, "trace.stderr"
    )
    tracer: subprocess.Popen[bytes] | None = None
    runtime_error: BaseException | None = None
    workload_exit_code: int | None = None
    completion_record_bytes: bytes | None = None
    tracer_exit_code: int | None = None
    collector_ready_record: str | None = None
    handled_signals = (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)
    previous_handlers: dict[signal.Signals, object] = {}

    def collector_interrupted(signum: int, _frame: object) -> None:
        for handled in previous_handlers:
            signal.signal(handled, signal.SIG_IGN)
        raise TraceError(f"collector interrupted by {signal.Signals(signum).name}")

    def restore_child_signal_mask() -> None:
        signal.pthread_sigmask(signal.SIG_SETMASK, blocked_mask)

    try:
        for handled in handled_signals:
            previous_handlers[handled] = signal.signal(
                handled, collector_interrupted
            )
        blocked_mask = signal.pthread_sigmask(signal.SIG_BLOCK, handled_signals)
        try:
            tracer = subprocess.Popen(
                [bpftrace_exec, rendered_exec],
                stdin=subprocess.DEVNULL,
                stdout=int(trace_output["fd"]),
                stderr=int(trace_stderr_output["fd"]),
                start_new_session=True,
                close_fds=True,
                pass_fds=(bpftrace_fd, rendered_fd),
                preexec_fn=restore_child_signal_mask,
            )
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, blocked_mask)
        attached_probes = len(REQUIRED_PROBES) + 2
        wait_for_attach_barrier(
            tracer,
            trace_output,
            trace_stderr_output,
            attached_probes,
            start_timeout,
        )
        before_resume, before_resume_ready = verify_stopped_workload(
            workload_pid,
            runner_pid,
            ready_file,
            expected_cpus,
            bytes(retained_inputs["READY"]["bytes"]),
        )
        if before_resume["starttime"] != starttime or before_resume_ready != ready_record:
            raise TraceError("approved workload identity changed before collector READY")
        collector_ready_record = (
            "TRACE_COLLECTOR_READY marker=TRACE_DIAGNOSTIC_ONLY "
            "scope=THREAD_SINGLE_TGID_ONLY "
            f"pass_id={expected['pass_id']} runner_pid={runner_pid} "
            f"workload_pid={workload_pid} workload_starttime={starttime} "
            f"collector_pid={os.getpid()} tracer_pid={tracer.pid} "
            f"required_probes={len(REQUIRED_PROBES)} "
            f"attached_probes={attached_probes} "
            f"thread_count={thread_count} "
            f"per_thread_candidates={per_thread_candidates} "
            f"expected_candidates={expected_candidates} "
            f"runtime_config_sha256={runtime_config_sha256} "
            f"expected_config_sha256={expected_config_sha256} "
            f"inventory_sha256={preflight['inventory_sha256']} "
            f"bpftrace_sha256={bpftrace_sha256} "
            f"rendered_script_sha256={rendered_script_sha256} "
            f"codegen_sha256={codegen_sha256}"
        )
        collector_ready_bytes = (collector_ready_record + "\n").encode("ascii")
        collector_ready_sha256 = hashlib.sha256(collector_ready_bytes).hexdigest()
        publish_atomic(
            output_directory, "collector-ready.txt", collector_ready_bytes
        )
        workload_exit_code, completion_record_bytes = wait_for_completion_record_at(
            completion_directory,
            completion_file.name,
            workload_pid,
            starttime,
            os.getpid(),
            collector_ready_sha256,
            workload_timeout,
        )
    except BaseException as exc:
        runtime_error = exc
    finally:
        if tracer is not None:
            try:
                tracer_exit_code = stop_tracer(tracer, stop_timeout)
            except BaseException as exc:
                if runtime_error is None:
                    runtime_error = exc
        for handled, previous in previous_handlers.items():
            signal.signal(handled, previous)
    if runtime_error is not None:
        raise runtime_error
    if workload_exit_code != 0:
        raise TraceError(f"approved workload exited with status {workload_exit_code}")
    if tracer_exit_code != 0:
        raise TraceError(f"bpftrace exited with status {tracer_exit_code}")
    if collector_ready_record is None:
        raise TraceError("collector READY record was not published")
    if completion_record_bytes is None:
        raise TraceError("validated completion record was not retained")
    collector_ready_sha256 = hashlib.sha256(
        (collector_ready_record + "\n").encode("ascii")
    ).hexdigest()

    os.fsync(int(trace_output["fd"]))
    os.fsync(int(trace_stderr_output["fd"]))
    trace_bytes = read_retained_file(trace_output, "trace output")
    trace_stderr_bytes = read_retained_file(
        trace_stderr_output, "trace stderr output"
    )
    transport_lost = scan_transport_lost_bytes(trace_stderr_bytes)
    verify_retained_inputs(retained_inputs)
    summary = parse_trace(trace_output, transport_lost, expected_output)
    validate_runtime_shape(
        summary, thread_count, per_thread_candidates, expected_candidates
    )
    summary_bytes = encode_json(summary)
    publish_atomic(output_directory, "summary.json", summary_bytes)
    manifest = {
        "schema": 1,
        "marker": "TRACE_DIAGNOSTIC_ONLY",
        "status": "TRACE_RUNTIME_VERIFIED",
        "scope": "THREAD_SINGLE_TGID_ONLY",
        "pass": "path",
        "pass_id": expected["pass_id"],
        "runner_pid": runner_pid,
        "workload_tgid": workload_pid,
        "initial_tids": initial["tids"],
        "ready_record": ready_record,
        "collector_ready_record": collector_ready_record,
        "collector_ready_sha256": collector_ready_sha256,
        "completion_record_sha256": hashlib.sha256(completion_record_bytes).hexdigest(),
        "runtime_config_sha256": runtime_config_sha256,
        "thread_count": thread_count,
        "per_thread_candidates": per_thread_candidates,
        "expected_candidates": expected_candidates,
        "expected_config_sha256": expected_config_sha256,
        "inventory_sha256": preflight["inventory_sha256"],
        "btf_sha256": preflight["btf_sha256"],
        "bpftrace_sha256": bpftrace_sha256,
        "rendered_script_sha256": rendered_script_sha256,
        "codegen_sha256": codegen_sha256,
        "trace_sha256": hashlib.sha256(trace_bytes).hexdigest(),
        "trace_stderr_sha256": hashlib.sha256(trace_stderr_bytes).hexdigest(),
        "summary_sha256": hashlib.sha256(summary_bytes).hexdigest(),
        "workload_exit_code": workload_exit_code,
        "tracer_exit_code": tracer_exit_code,
        "transport_lost": transport_lost,
        "cross_pass_arithmetic": "FORBIDDEN",
    }
    publish_atomic(output_directory, "runtime-manifest.json", encode_json(manifest))
    return manifest


def runtime_timeout(text: str) -> int:
    if not text.isascii() or not text.isdecimal():
        raise argparse.ArgumentTypeError(
            "timeout must be an integer from 1 through 300 seconds"
        )
    try:
        value = parse_bounded_uint(text, "timeout", 300, 1)
    except TraceError as exc:
        raise argparse.ArgumentTypeError(
            "timeout must be an integer from 1 through 300 seconds"
        ) from exc
    if not (1 <= value <= 300):
        raise argparse.ArgumentTypeError(
            "timeout must be an integer from 1 through 300 seconds"
        )
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    parse = sub.add_parser("parse")
    parse.add_argument("--expected", type=Path, required=True)
    parse.add_argument("--input", type=Path, required=True)
    parse.add_argument("--transport-lost", type=int, required=True)
    parse.add_argument("--output", type=Path, required=True)
    preflight = sub.add_parser("preflight")
    preflight.add_argument("--inventory", type=Path, required=True)
    preflight.add_argument("--output", type=Path, required=True)
    collect = sub.add_parser("collect-preflight")
    collect.add_argument("--bpftrace", type=Path, required=True)
    collect.add_argument("--btf", type=Path, default=Path("/sys/kernel/btf/vmlinux"))
    collect.add_argument("--kernel-release", default=platform.release())
    collect.add_argument("--output", type=Path, required=True)
    render = sub.add_parser("render")
    render.add_argument("--inventory", type=Path, required=True)
    render.add_argument("--expected", type=Path, required=True)
    render.add_argument("--template", type=Path, required=True)
    render.add_argument("--output", type=Path, required=True)
    render.add_argument("--manifest", type=Path, required=True)
    runtime = sub.add_parser("collect-runtime")
    runtime.add_argument("--inventory", type=Path, required=True)
    runtime.add_argument("--config", type=Path, required=True)
    runtime.add_argument("--template", type=Path, required=True)
    runtime.add_argument("--bpftrace", type=Path, required=True)
    runtime.add_argument("--btf", type=Path, default=Path("/sys/kernel/btf/vmlinux"))
    runtime.add_argument("--kernel-release", default=platform.release())
    runtime.add_argument("--output-dir", type=Path, required=True)
    runtime.add_argument("--workload-pid", type=int, required=True)
    runtime.add_argument("--runner-pid", type=int, required=True)
    runtime.add_argument("--ready-file", type=Path, required=True)
    runtime.add_argument("--completion-file", type=Path, required=True)
    runtime.add_argument("--start-timeout", type=runtime_timeout, default=10)
    runtime.add_argument("--workload-timeout", type=runtime_timeout, default=30)
    runtime.add_argument("--stop-timeout", type=runtime_timeout, default=10)
    return parser


def write_json(output: Path, value: dict[str, object]) -> None:
    encoded = encode_json(value).decode("ascii")
    if str(output) == "-":
        print(encoded, end="")
    else:
        output.write_text(encoded, encoding="ascii")


def encode_json(value: dict[str, object]) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("ascii")


def main() -> int:
    args = build_parser().parse_args()
    try:
        if args.command == "parse":
            result = parse_trace(args.input, args.transport_lost, args.expected)
        elif args.command == "preflight":
            result = preflight_inventory(args.inventory)
        elif args.command == "collect-preflight":
            result = collect_inventory(args.bpftrace, args.btf, args.kernel_release)
        elif args.command == "render":
            if args.manifest.exists():
                raise TraceError("render manifest already exists")
            result = render_template(args.template, args.inventory, args.expected, args.output)
        else:
            result = collect_runtime(
                args.inventory,
                args.config,
                args.template,
                args.bpftrace,
                args.btf,
                args.kernel_release,
                args.output_dir,
                args.workload_pid,
                args.runner_pid,
                args.ready_file,
                args.completion_file,
                args.start_timeout,
                args.workload_timeout,
                args.stop_timeout,
            )
    except (OSError, UnicodeError, TraceError) as exc:
        print(f"trace rejected: {exc}", file=sys.stderr)
        return 2
    if args.command == "render":
        write_json(args.manifest, result)
    elif args.command != "collect-runtime":
        write_json(args.output, result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
