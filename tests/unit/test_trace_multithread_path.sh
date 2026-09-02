#!/usr/bin/env bash
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
PARSER="$ROOT/tools/trace-multithread-path.py"
TMP=$(mktemp -d)
trap 'rm -rf -- "$TMP"' EXIT HUP INT TERM

passed=0
failed=0

ok() {
    passed=$((passed + 1))
    printf 'ok %d - %s\n' "$passed" "$1"
}

not_ok() {
    failed=$((failed + 1))
    printf 'not ok - %s\n' "$1" >&2
}

expect_ok() {
    local label=$1
    shift
    if "$@"; then ok "$label"; else not_ok "$label"; fi
}

expect_fail() {
    local label=$1
    shift
    if "$@"; then not_ok "$label"; else ok "$label"; fi
}

expected="$TMP/expected.json"
printf '%s\n' '{"schema":1,"pass":"path","pass_id":"p1","tgids":[101],"cpus":[2],"syscalls":[426],"dev":[259,8],"lba":[4096,8191],"bytes":4096,"hctxs":[2],"qids":[3]}' >"$expected"

parse_trace() {
    python3 "$PARSER" parse --expected "$expected" "$@"
}

valid="$TMP/valid.trace"
python3 - "$valid" <<'PY'
import sys

with open(sys.argv[1], "w", encoding="ascii") as output:
    print("TRACE_BEGIN schema=1 marker=TRACE_DIAGNOSTIC_ONLY pass=path pass_id=p1 tgids=101 cpus=2 syscalls=426 dev=259:8 lba=4096:8191 hctxs=2 qids=3", file=output)
    serial = 0
    for lane in range(8):
        tid = 102 + lane
        for seq in range(1, 129):
            base = 100 + serial * 3000
            req = 0xABC if serial == 0 else 0x100000 + serial
            print(
                f"TRACE_REQUEST pass_id=p1 tgid=101 tid={tid} seq={seq} "
                f"req=0x{req:x} issue_req=0x{req:x} complete_req=0x{req:x} "
                "dev=259:8 lba=5000 bytes=4096 hctx=2 qid=3 polled=1 cmd_flags=4194305 "
                "cpu_enter=2 cpu_map=2 cpu_queue=2 cpu_complete=2 "
                "iopoll_check_cpu=2 iopoll_cpu=2 blk_poll_cpu=2 nvme_poll_cpu=2 "
                f"enter_ns={base} tag_enter_ns={base+50} tag_exit_ns={base+80} "
                f"map_enter_ns={base+100} pages_enter_ns={base+120} pages_exit_ns={base+160} "
                f"map_exit_ns={base+200} queue_enter_ns={base+250} dma_map_ns={base+350} "
                f"start_ns={base+400} issue_ns={base+420} "
                f"queue_exit_ns={base+500} first_poll_ns={base+600} "
                f"iopoll_check_enter_ns={base+600} iopoll_check_exit_ns={base+2100} iopoll_check_count=2 iopoll_check_duration_ns=1550 "
                f"iopoll_enter_ns={base+650} iopoll_exit_ns={base+2050} iopoll_count=2 iopoll_duration_ns=1450 "
                f"blk_poll_enter_ns={base+700} blk_poll_exit_ns={base+2000} blk_poll_count=2 blk_poll_duration_ns=1350 "
                f"nvme_poll_enter_ns={base+750} nvme_poll_exit_ns={base+1950} nvme_poll_count=2 nvme_poll_duration_ns=1250 "
                f"complete_ns={base+1900} exit_ns={base+2300}",
                file=output,
            )
            serial += 1
    print("TRACE_END pass_id=p1 candidates=1024 emitted=1024 matched=1024 issues=1024 completes=1024 block_issues=1024 block_completes=1024 iopoll_checks=2048 iopolls=2048 blk_polls=2048 nvme_polls=2048 block_shape_errors=0 block_errors=0 collisions=0 order_errors=0 cpu_drift=0 dev_drift=0 lba_drift=0 hctx_drift=0 qid_drift=0 poll_drift=0 unpolled=0 unmatched=0", file=output)
PY

expect_ok "a complete single-pass request is accepted" \
    parse_trace --input "$valid" --transport-lost 0 --output "$TMP/summary.json"
expect_ok "accepted output is explicitly diagnostic-only" \
    python3 -c 'import hashlib,json,sys; d=json.load(open(sys.argv[1])); assert d["marker"] == "TRACE_DIAGNOSTIC_ONLY"; assert d["pass"] == "path"; assert d["quality"]["matched_per_mille"] == 1000; assert d["expected_config_sha256"] == hashlib.sha256(open(sys.argv[2],"rb").read()).hexdigest(); assert d["trace_sha256"] == hashlib.sha256(open(sys.argv[3],"rb").read()).hexdigest()' "$TMP/summary.json" "$expected" "$valid"
expect_ok "accepted output does not publish a performance rate" \
    sh -c 'test -s "$1" && ! grep -Eqi "throughput|iops|ops_per|requests_per" "$1"' sh "$TMP/summary.json"
expect_ok "same-pass diagnostic segments expose deterministic percentiles" \
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d["cross_pass_arithmetic"] == "FORBIDDEN"; assert d["segments_ns"]["pre_submit_map_tag"]["p99"] == 250; assert d["segments_ns"]["queue_to_issue"]["p99"] == 170; assert d["segments_ns"]["queue_return_to_first_poll"]["p99"] == 100; assert d["segments_ns"]["accumulated_poll_cpu"]["p99"] == 1550; assert d["segments_ns"]["first_poll_to_complete"]["p99"] == 1300; assert d["segments_ns"]["complete_to_syscall_exit"]["p99"] == 400; assert len(d["records_per_tid"]) == 8 and set(d["records_per_tid"].values()) == {128}' "$TMP/summary.json"

expect_ok "bounded integer parser accepts exact ABI maxima" \
    python3 -c 'import importlib.util,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); assert m.parse_bounded_uint(str(m.U64_MAX),"u64",m.U64_MAX) == m.U64_MAX; assert m.parse_bounded_uint(str(m.U32_MAX),"u32",m.U32_MAX) == m.U32_MAX; assert m.parse_bounded_uint(str(m.PID_MAX),"pid",m.PID_MAX) == m.PID_MAX; assert m.parse_bounded_uint(str(m.U16_MAX),"u16",m.U16_MAX) == m.U16_MAX' \
        "$PARSER"
expect_fail "bounded integer parser rejects values above unsigned long long" \
    python3 -c 'import importlib.util,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); m.parse_bounded_uint(str(m.U64_MAX+1),"u64",m.U64_MAX)' \
        "$PARSER"
expect_fail "bounded integer parser rejects negative record values" \
    python3 -c 'import importlib.util,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); m.parse_bounded_uint("-1","u64",m.U64_MAX)' \
        "$PARSER"
expect_ok "request integer fields have explicit ABI domains" \
    python3 -c 'import importlib.util,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); d=m.REQUEST_UINT_DOMAINS; assert d["tgid"] == m.PID_MAX and d["tid"] == m.PID_MAX and d["seq"] == m.U64_MAX; assert d["cpu_enter"] == m.U32_MAX and d["hctx"] == m.U32_MAX and d["qid"] == m.U16_MAX; assert d["cmd_flags"] == m.U32_MAX and d["enter_ns"] == m.U64_MAX and d["iopoll_count"] == m.U64_MAX' \
        "$PARSER"

begin_not_first="$TMP/begin-not-first.trace"
python3 - "$valid" "$begin_not_first" <<'PY'
import sys
lines=open(sys.argv[1],encoding="ascii").read().splitlines()
lines[0],lines[1]=lines[1],lines[0]
open(sys.argv[2],"w",encoding="ascii").write("\n".join(lines)+"\n")
PY
expect_fail "TRACE_BEGIN must be the first nonempty record" \
    parse_trace --input "$begin_not_first" --transport-lost 0 \
        --output "$TMP/begin-not-first.json"

end_not_last="$TMP/end-not-last.trace"
python3 - "$valid" "$end_not_last" <<'PY'
import sys
lines=open(sys.argv[1],encoding="ascii").read().splitlines()
lines[-1],lines[-2]=lines[-2],lines[-1]
open(sys.argv[2],"w",encoding="ascii").write("\n".join(lines)+"\n")
PY
expect_fail "TRACE_END must be the last nonempty record" \
    parse_trace --input "$end_not_last" --transport-lost 0 \
        --output "$TMP/end-not-last.json"

end_before_body="$TMP/end-before-body.trace"
python3 - "$valid" "$end_before_body" <<'PY'
import sys
lines=open(sys.argv[1],encoding="ascii").read().splitlines()
lines=[lines[0],lines[-1]]+lines[1:-1]
open(sys.argv[2],"w",encoding="ascii").write("\n".join(lines)+"\n")
PY
expect_fail "all trace body records must be enclosed by BEGIN and END" \
    parse_trace --input "$end_before_body" --transport-lost 0 \
        --output "$TMP/end-before-body.json"

missing_req="$TMP/missing-req.trace"
sed 's/ req=0xabc / /' "$valid" >"$missing_req"
expect_fail "a request record without a nonzero request token is rejected" \
    parse_trace --input "$missing_req" --transport-lost 0 --output "$TMP/missing-req.json"

negative="$TMP/negative.trace"
sed 's/ complete_ns=2000 / complete_ns=500 /' "$valid" >"$negative"
expect_fail "a negative or out-of-order request duration is rejected" \
    parse_trace --input "$negative" --transport-lost 0 --output "$TMP/negative.json"

over_ull="$TMP/over-ull.trace"
sed '0,/ enter_ns=[0-9][0-9]*/s// enter_ns=18446744073709551616/' "$valid" >"$over_ull"
expect_fail "a record timestamp above unsigned long long is rejected" \
    parse_trace --input "$over_ull" --transport-lost 0 --output "$TMP/over-ull.json"

over_pointer="$TMP/over-pointer.trace"
sed '0,/ req=0x[0-9a-f]*/s// req=0x10000000000000000/' "$valid" >"$over_pointer"
expect_fail "a request pointer above 64 bits is rejected" \
    parse_trace --input "$over_pointer" --transport-lost 0 \
        --output "$TMP/over-pointer.json"

long_decimal="$TMP/long-decimal.trace"
python3 - "$valid" "$long_decimal" <<'PY'
import sys
text = open(sys.argv[1], encoding="ascii").read()
text = text.replace("candidates=1024", "candidates=" + "9" * 5000, 1)
open(sys.argv[2], "w", encoding="ascii").write(text)
PY
long_decimal_rc=0
parse_trace --input "$long_decimal" --transport-lost 0 \
    --output "$TMP/long-decimal.json" >/dev/null 2>"$TMP/long-decimal.err" || \
    long_decimal_rc=$?
if [ "$long_decimal_rc" -ne 0 ] &&
   grep -q '^trace rejected: ' "$TMP/long-decimal.err" &&
   ! grep -q 'Traceback\|ValueError' "$TMP/long-decimal.err"; then
    ok "overlong decimal is a clean TraceError without a Python traceback"
else
    not_ok "overlong decimal is a clean TraceError without a Python traceback"
fi

mismatched_token="$TMP/mismatched-token.trace"
sed 's/ complete_req=0xabc / complete_req=0xdef /' "$valid" >"$mismatched_token"
expect_fail "issue and completion must carry the same request token" \
    parse_trace --input "$mismatched_token" --transport-lost 0 --output "$TMP/mismatched-token.json"

unpaired_count="$TMP/unpaired-count.trace"
sed 's/ issues=1024 completes=1024 / issues=1024 completes=1023 /' "$valid" >"$unpaired_count"
expect_fail "aggregate issue and completion counts must be paired" \
    parse_trace --input "$unpaired_count" --transport-lost 0 --output "$TMP/unpaired-count.json"

block_unpaired="$TMP/block-unpaired.trace"
sed 's/ block_issues=1024 block_completes=1024 / block_issues=1024 block_completes=1023 /' "$valid" >"$block_unpaired"
expect_fail "block tracepoint aggregate issue and completion counts must be paired" \
    parse_trace --input "$block_unpaired" --transport-lost 0 --output "$TMP/block-unpaired.json"

mutate_and_reject() {
    local label=$1 name=$2 expression=$3
    local input="$TMP/$name.trace"
    sed "$expression" "$valid" >"$input"
    expect_fail "$label" parse_trace \
        --input "$input" --transport-lost 0 --output "$TMP/$name.json"
}

mutate_and_reject "request TGID outside the allowlist is rejected" bad-tgid \
    's/ tgid=101 tid=/ tgid=999 tid=/'
mutate_and_reject "request CPU outside the allowlist is rejected" bad-cpu \
    's/ cpu_queue=2 / cpu_queue=3 /'
mutate_and_reject "request device outside the allowlist is rejected" bad-dev \
    's/ dev=259:8 lba=5000 / dev=259:9 lba=5000 /'
mutate_and_reject "request LBA outside the allowlist is rejected" bad-lba \
    's/ lba=5000 bytes=/ lba=9000 bytes=/'
mutate_and_reject "non-4KiB request is rejected" bad-size \
    's/ bytes=4096 / bytes=8192 /'
mutate_and_reject "request hctx outside the allowlist is rejected" bad-hctx \
    's/ hctx=2 qid=/ hctx=9 qid=/'
mutate_and_reject "request qid outside the allowlist is rejected" bad-qid \
    's/ qid=3 polled=/ qid=9 polled=/'
mutate_and_reject "an unpolled queue_rq request is rejected" unpolled-request \
    's/ polled=1 / polled=0 /'
mutate_and_reject "queue_rq must preserve the REQ_POLLED command flag" bad-polled-flag \
    's/ cmd_flags=4194305 / cmd_flags=1 /'
mutate_and_reject "every matched request must enter the io_uring poll path" missing-iopoll \
    '0,/ iopoll_count=2 /s// iopoll_count=0 /'
mutate_and_reject "every matched request must enter blk_mq_poll" missing-blk-poll \
    '0,/ blk_poll_count=2 /s// blk_poll_count=0 /'
mutate_and_reject "every matched request must enter nvme_poll" missing-nvme-poll \
    '0,/ nvme_poll_count=2 /s// nvme_poll_count=0 /'
mutate_and_reject "a request record from another pass is rejected" bad-pass \
    's/TRACE_REQUEST pass_id=p1 /TRACE_REQUEST pass_id=p2 /'
mutate_and_reject "a self-declared broader CPU allowlist cannot override expected config" broad-cpu-header \
    's/ cpus=2 syscalls=/ cpus=2,3 syscalls=/'
mutate_and_reject "tracer-observed CPU drift rejects the pass" cpu-drift \
    's/ cpu_drift=0 / cpu_drift=1 /'

expect_ok "matched polling requests do not depend on any IRQ or scheduler record" \
    sh -c '! grep -Eq "TRACE_IRQ|irq=|wakeup_ns=|switch_ns=" "$1"' sh "$valid"

optional_noise="$TMP/optional-noise.trace"
sed '/^TRACE_END /i TRACE_NOISE pass_id=p1 kind=irq count=7' "$valid" >"$optional_noise"
expect_ok "optional aggregate IRQ noise does not bind request completion" \
    parse_trace --input "$optional_noise" --transport-lost 0 \
        --output "$TMP/optional-noise.json"

expect_fail "transport-level lost events reject an otherwise complete pass" \
    parse_trace --input "$valid" --transport-lost 1 --output "$TMP/lost.json"
mutate_and_reject "a tracer collision rejects the pass" collision \
    's/ collisions=0 / collisions=1 /'

low_match="$TMP/low-match.trace"
sed 's/candidates=1024 emitted=1024 matched=1024/candidates=2000 emitted=1024 matched=1024/;s/unmatched=0/unmatched=976/' "$valid" >"$low_match"
expect_fail "a pass below 99.9 percent matched requests is rejected" \
    parse_trace --input "$low_match" --transport-lost 0 --output "$TMP/low-match.json"

boundary="$TMP/boundary.trace"
sed 's/candidates=1024 emitted=1024 matched=1024/candidates=1025 emitted=1024 matched=1024/;s/unmatched=0/unmatched=1/' "$valid" >"$boundary"
expect_fail "a nonexact 1024-of-1025 pass is rejected despite exceeding 99.9 percent" \
    parse_trace --input "$boundary" --transport-lost 0 --output "$TMP/boundary.json"

one_candidate="$TMP/one-candidate.trace"
python3 - "$valid" "$one_candidate" <<'PY'
import sys
lines = open(sys.argv[1], encoding="ascii").read().splitlines()
request = next(line for line in lines if line.startswith("TRACE_REQUEST "))
end = next(line for line in lines if line.startswith("TRACE_END "))
for old, new in (("candidates=1024", "candidates=1"), ("emitted=1024", "emitted=1"), ("matched=1024", "matched=1"), ("issues=1024", "issues=1"), ("completes=1024", "completes=1"), ("block_issues=1024", "block_issues=1"), ("block_completes=1024", "block_completes=1"), ("iopoll_checks=2048", "iopoll_checks=2"), ("iopolls=2048", "iopolls=2"), ("blk_polls=2048", "blk_polls=2"), ("nvme_polls=2048", "nvme_polls=2")):
    end = end.replace(old, new)
open(sys.argv[2], "w", encoding="ascii").write(lines[0] + "\n" + request + "\n" + end + "\n")
PY
expect_fail "public parse never seals a one-candidate fixture" \
    parse_trace --input "$one_candidate" --transport-lost 0 \
        --output "$TMP/one-candidate.json"

nine_nine_nine="$TMP/999-candidates.trace"
python3 - "$valid" "$nine_nine_nine" <<'PY'
import sys
lines = open(sys.argv[1], encoding="ascii").read().splitlines()
requests = [line for line in lines if line.startswith("TRACE_REQUEST ")][:999]
end = next(line for line in lines if line.startswith("TRACE_END "))
for old, new in (("candidates=1024", "candidates=999"), ("emitted=1024", "emitted=999"), ("matched=1024", "matched=999"), ("issues=1024", "issues=999"), ("completes=1024", "completes=999"), ("block_issues=1024", "block_issues=999"), ("block_completes=1024", "block_completes=999"), ("iopoll_checks=2048", "iopoll_checks=1998"), ("iopolls=2048", "iopolls=1998"), ("blk_polls=2048", "blk_polls=1998"), ("nvme_polls=2048", "nvme_polls=1998")):
    end = end.replace(old, new)
open(sys.argv[2], "w", encoding="ascii").write("\n".join([lines[0]] + requests + [end]) + "\n")
PY
expect_fail "public parse never seals a 999-candidate fixture" \
    parse_trace --input "$nine_nine_nine" --transport-lost 0 \
        --output "$TMP/999-candidates.json"

request_mutation() {
    local output=$1 target_tid=$2 target_seq=$3 mode=$4
    python3 - "$valid" "$output" "$target_tid" "$target_seq" "$mode" <<'PY'
import sys

source, output, target_tid, target_seq, mode = sys.argv[1:]
target_tid, target_seq = int(target_tid), int(target_seq)
lines = open(source, encoding="ascii").read().splitlines()
first = next(line for line in lines if line.startswith("TRACE_REQUEST "))
first_fields = dict(field.split("=", 1) for field in first.split()[1:])
changed = 0
result = []
for line in lines:
    if not line.startswith("TRACE_REQUEST "):
        result.append(line)
        continue
    words = line.split()
    fields = dict(field.split("=", 1) for field in words[1:])
    if int(fields["tid"]) != target_tid or int(fields["seq"]) != target_seq:
        result.append(line)
        continue
    if mode in {"reuse", "overlap-pointer"}:
        fields["req"] = fields["issue_req"] = fields["complete_req"] = "0xabc"
    if mode in {"overlap-tid", "overlap-pointer"}:
        for key in tuple(fields):
            if key.endswith("_ns"):
                fields[key] = first_fields[key]
    if mode == "duplicate-identity":
        fields["seq"] = "1"
    result.append(" ".join([words[0]] + [f"{key}={fields[key]}" for key in fields]))
    changed += 1
assert changed == 1
open(output, "w", encoding="ascii").write("\n".join(result) + "\n")
PY
}

same_tid_reuse="$TMP/same-tid-reuse.trace"
request_mutation "$same_tid_reuse" 102 2 reuse
expect_ok "a completed request pointer may be reused by the next sequence on one TID" \
    parse_trace --input "$same_tid_reuse" --transport-lost 0 \
        --output "$TMP/same-tid-reuse.json"

other_tid_reuse="$TMP/other-tid-reuse.trace"
request_mutation "$other_tid_reuse" 103 1 reuse
expect_ok "a completed request pointer may be reused by a later sequence on another TID" \
    parse_trace --input "$other_tid_reuse" --transport-lost 0 \
        --output "$TMP/other-tid-reuse.json"

overlap="$TMP/overlap.trace"
request_mutation "$overlap" 102 2 overlap-tid
expect_fail "QD1 forbids overlapping requests from one TID" \
    parse_trace --input "$overlap" --transport-lost 0 --output "$TMP/overlap.json"

duplicate_identity="$TMP/duplicate-identity.trace"
request_mutation "$duplicate_identity" 102 2 duplicate-identity
expect_fail "duplicate TGID/TID/sequence identity is rejected" \
    parse_trace --input "$duplicate_identity" --transport-lost 0 --output "$TMP/duplicate-identity.json"

duplicate_req="$TMP/duplicate-req.trace"
request_mutation "$duplicate_req" 103 1 overlap-pointer
expect_fail "overlapping lifetimes for one request pointer are rejected as a collision" \
    parse_trace --input "$duplicate_req" --transport-lost 0 --output "$TMP/duplicate-req.json"

unknown="$TMP/unknown.trace"
sed '/^TRACE_END /i TRACE_EVENT pass_id=p1 stage=unvalidated req=0xabc' "$valid" >"$unknown"
expect_fail "unknown trace record types cannot be silently ignored" \
    parse_trace --input "$unknown" --transport-lost 0 --output "$TMP/unknown.json"

inventory="$TMP/inventory.json"
python3 - "$inventory" <<'PY'
import json, sys
probes = {
    "tracepoint:raw_syscalls:sys_enter": ["id"],
    "tracepoint:raw_syscalls:sys_exit": ["id", "ret"],
    "fentry:vmlinux:blk_rq_map_user_iov": ["q", "rq", "map_data", "iter", "gfp_mask"],
    "fexit:vmlinux:blk_rq_map_user_iov": ["q", "rq", "map_data", "iter", "gfp_mask", "retval"],
    "fentry:vmlinux:bio_iov_iter_get_pages": ["bio", "iter", "len_align_mask"],
    "fexit:vmlinux:bio_iov_iter_get_pages": ["bio", "iter", "len_align_mask", "retval"],
    "fentry:vmlinux:blk_mq_get_tag": ["data"],
    "fexit:vmlinux:blk_mq_get_tag": ["data", "retval"],
    "fentry:vmlinux:blk_mq_start_request": ["rq"],
    "fentry:vmlinux:blk_mq_end_request": ["rq", "error"],
    "fexit:nvme:nvme_map_data": ["req", "retval"],
    "fentry:nvme:nvme_queue_rq": ["hctx", "bd"],
    "fexit:nvme:nvme_queue_rq": ["hctx", "bd", "retval"],
    "fentry:vmlinux:blk_mq_poll": ["q", "cookie", "iob", "flags"],
    "fexit:vmlinux:blk_mq_poll": ["q", "cookie", "iob", "flags", "retval"],
    "fentry:nvme:nvme_poll": ["hctx", "iob"],
    "fexit:nvme:nvme_poll": ["hctx", "iob", "retval"],
    "fentry:vmlinux:io_do_iopoll": ["ctx", "force_nonspin"],
    "fexit:vmlinux:io_do_iopoll": ["ctx", "force_nonspin", "retval"],
    "fentry:vmlinux:io_iopoll_check": ["ctx", "min"],
    "fexit:vmlinux:io_iopoll_check": ["ctx", "min", "retval"],
    "tracepoint:block:block_rq_issue": ["dev", "sector", "bytes", "rwbs"],
    "tracepoint:block:block_rq_complete": ["dev", "sector", "error", "rwbs"],
}
json.dump({"schema": 1, "kernel_release": "6.14.0-1009-intel", "btf_sha256": "a" * 64,
           "bpftrace_version": "bpftrace v0.25.1", "probes": probes}, open(sys.argv[1], "w"))
PY

expect_ok "kernel-6.14 polling probe inventory passes the explicit ABI preflight" \
    python3 "$PARSER" preflight --inventory "$inventory" --output "$TMP/preflight.json"
expect_ok "preflight output freezes diagnostic-only ABI evidence" \
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d["marker"] == "TRACE_DIAGNOSTIC_ONLY"; assert d["status"] == "PROBE_ABI_VERIFIED"; assert d["kernel_release"] == "6.14.0-1009-intel"; assert d["btf_sha256"] == "a"*64; required=set(d["required_probes"]); assert "fentry:vmlinux:blk_mq_poll" in required and "fexit:nvme:nvme_poll" in required and "fentry:vmlinux:io_do_iopoll" in required and "fexit:vmlinux:io_iopoll_check" in required; assert not any(":irq:" in p or ":sched:" in p for p in required)' "$TMP/preflight.json"

expect_ok "verified ABI and expected config render one build-bound trace pass" \
    python3 "$PARSER" render --inventory "$inventory" --expected "$expected" \
        --template "$ROOT/tools/trace-multithread-path.bt" \
        --output "$TMP/rendered.bt" --manifest "$TMP/rendered.json"
expect_ok "rendered trace uses only target-confirmed request pairing probes" \
    sh -c 'test -s "$1" && test -s "$2" && ! grep -q "@@" "$1" && grep -q "fentry:vmlinux:blk_mq_start_request" "$1" && grep -q "fentry:vmlinux:blk_mq_end_request" "$1" && grep -q "tracepoint:block:block_rq_issue" "$1" && grep -q "tracepoint:block:block_rq_complete" "$1" && ! grep -q "rawtracepoint\|nvme_prep_rq" "$1" && ! grep -Eqi "throughput|iops|ops_per|requests_per" "$1"' sh "$TMP/rendered.bt" "$TMP/rendered.json"
expect_ok "actual bpftrace clause count is required inventory plus BEGIN and END" \
    python3 -c 'import importlib.util,re,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); s=open(sys.argv[2],encoding="ascii").read(); clauses=re.findall(r"^(?:BEGIN|END|(?:fentry|fexit|tracepoint):[^\n]+)$",s,re.M); probes={item for item in clauses if item not in {"BEGIN","END"}}; assert probes == set(m.REQUIRED_PROBES); assert len(clauses) == len(m.REQUIRED_PROBES) + 2 == 25' \
        "$PARSER" "$TMP/rendered.bt"
expect_ok "request-pointer ownership ends at completion rather than syscall exit" \
    python3 -c 'import sys; s=open(sys.argv[1],encoding="ascii").read(); end=s.split("fentry:vmlinux:blk_mq_end_request",1)[1].split("tracepoint:block:block_rq_complete",1)[0]; out=s.split("tracepoint:raw_syscalls:sys_exit",1)[1].split("END",1)[0]; assert "$owner = @req_tid[$req]" in end and "@complete_ns[$owner]" in end and "delete(@req_tid, $req)" in end; assert "if (has_key(@req_tid, $req) && @req_tid[$req] == tid)" in out' \
        "$TMP/rendered.bt"
expect_ok "completed request state is retained by its QD1 owner sequence" \
    python3 -c 'import sys; s=open(sys.argv[1],encoding="ascii").read(); out=s.split("tracepoint:raw_syscalls:sys_exit",1)[1].split("END",1)[0]; required=("@req_seq[tid]", "@map_enter_ns[tid]", "@complete_ns[tid]", "@issue_req[tid]", "@complete_req[tid]"); assert all(item in out for item in required); assert "@map_enter_ns[$req]" not in out and "@complete_ns[$req]" not in out' \
        "$TMP/rendered.bt"
expect_ok "queue_rq records the true polled flag and polling layer evidence" \
    python3 -c 'import sys; s=open(sys.argv[1],encoding="ascii").read(); queue=s.split("fentry:nvme:nvme_queue_rq",1)[1].split("fexit:nvme:nvme_map_data",1)[0]; out=s.split("tracepoint:raw_syscalls:sys_exit",1)[1].split("END",1)[0]; assert "cmd_flags" in queue and "4194304" in queue and "@unpolled" in queue; required=("@iopoll_check_count[tid]", "@iopoll_count[tid]", "@blk_poll_count[tid]", "@nvme_poll_count[tid]", "@first_poll_ns[tid]"); assert all(item in out for item in required); assert "wakeup_ns" not in out and "switch_ns" not in out and "irq" not in out' \
        "$TMP/rendered.bt"
expect_ok "TRACE_END uses printable integer counters rather than bpftrace count_t maps" \
    python3 -c 'import re,sys; s=open(sys.argv[1],encoding="ascii").read(); f=re.search(r"printf\(\"TRACE_END [^\"]+\"",s); assert "count()" not in s and f and "%llu" in f.group(0)' "$TMP/rendered.bt"
expect_ok "render manifest freezes all three inputs and diagnostic-only semantics" \
    python3 -c 'import hashlib,json,sys; d=json.load(open(sys.argv[1])); assert d["marker"] == "TRACE_DIAGNOSTIC_ONLY"; assert d["cross_pass_arithmetic"] == "FORBIDDEN"; assert d["btf_sha256"] == "a"*64; assert d["script_sha256"] == hashlib.sha256(open(sys.argv[2],"rb").read()).hexdigest(); assert d["expected_config_sha256"] == hashlib.sha256(open(sys.argv[3],"rb").read()).hexdigest()' "$TMP/rendered.json" "$TMP/rendered.bt" "$expected"

missing_probe="$TMP/missing-probe.json"
python3 - "$inventory" "$missing_probe" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); del d["probes"]["fentry:vmlinux:blk_mq_poll"]
json.dump(d, open(sys.argv[2], "w"))
PY
expect_fail "preflight rejects a missing blk_mq_poll ABI" \
    python3 "$PARSER" preflight --inventory "$missing_probe" --output "$TMP/missing-probe-out.json"

fake_bpftrace="$TMP/fake-bpftrace"
printf '%s\n' \
    '#!/usr/bin/env bash' \
    'set -eu' \
    'printf "%s\n" "$*" >>"$FAKE_LOG"' \
    'if test "$1" = --version; then echo "bpftrace v0.25.1"; exit 0; fi' \
    'test "$1" = -lv && test "$#" -eq 2' \
    'python3 - "$FAKE_INVENTORY" "$2" <<"PY"' \
    'import json, sys' \
    'd=json.load(open(sys.argv[1])); p=sys.argv[2]' \
    'if p not in d["probes"]: raise SystemExit(7)' \
    'print(p)' \
    'for field in d["probes"][p]:' \
    '    # Match the target bpftrace 0.25 tracepoint ABI spelling.  Array' \
    '    # declarators put the extent after the field name (for example,' \
    '    # `char rwbs[10]`) instead of ending the line in a bare identifier.' \
    '    print("    char rwbs[10]" if field == "rwbs" else "    unsigned long " + field)' \
    'PY' \
    >"$fake_bpftrace"
chmod 0700 "$fake_bpftrace"
printf 'synthetic-btf\n' >"$TMP/vmlinux.btf"

expect_ok "read-only collector freezes exact bpftrace probe fields" \
    env FAKE_INVENTORY="$inventory" FAKE_LOG="$TMP/bpftrace.log" python3 "$PARSER" collect-preflight \
        --bpftrace "$fake_bpftrace" --btf "$TMP/vmlinux.btf" \
        --kernel-release 6.14.0-1009-intel --output "$TMP/collected.json"
expect_ok "collected inventory revalidates through the same ABI gate" \
    python3 "$PARSER" preflight --inventory "$TMP/collected.json" --output "$TMP/collected-preflight.json"
expect_ok "collector performs listing only and never attaches a probe" \
    sh -c 'test "$(grep -c "^--version$" "$1")" -eq 1 && ! grep -Ev "^(--version|-lv [A-Za-z0-9_:]+)$" "$1" >/dev/null && ! grep -q "nvme_prep_rq" "$1"' sh "$TMP/bpftrace.log"

ln -s "$fake_bpftrace" "$TMP/fake-bpftrace-link"
expect_fail "probe collection refuses a symlinked bpftrace executable" \
    env FAKE_INVENTORY="$inventory" FAKE_LOG="$TMP/bpftrace-link.log" \
        python3 "$PARSER" collect-preflight --bpftrace "$TMP/fake-bpftrace-link" \
        --btf "$TMP/vmlinux.btf" --kernel-release 6.14.0-1009-intel \
        --output "$TMP/symlink-collected.json"

expect_ok "runtime attach blocks handled signals across Popen handle assignment" \
    python3 -c 'import sys; s=open(sys.argv[1],encoding="ascii").read(); body=s.split("def collect_runtime(",1)[1].split("def runtime_timeout(",1)[0]; before,after=body.split("tracer = subprocess.Popen(",1); assert "signal.pthread_sigmask(signal.SIG_BLOCK" in before[-1200:]; assert "signal.pthread_sigmask(signal.SIG_SETMASK" in after[:1200]' \
        "$PARSER"

expect_ok "runtime inputs are retained through no-follow descriptors and rechecked before attach" \
    python3 -c 'import sys; s=open(sys.argv[1],encoding="ascii").read(); body=s.split("def collect_runtime(",1)[1].split("def runtime_timeout(",1)[0]; assert "O_NOFOLLOW" in s; assert "retained_inputs" in body and "verify_retained_inputs" in body; assert "pass_fds=" in body and "/proc/self/fd/" in body' \
        "$PARSER"

expect_ok "retained output directory rejects pathname rename and replacement" \
    python3 -c 'import importlib.util,pathlib,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); original=pathlib.Path(sys.argv[2]); moved=pathlib.Path(sys.argv[3]); directory=m.open_retained_directory(original,"runtime output",create=True); original.rename(moved); original.mkdir(); rejected=False; exec("try:\n m.publish_atomic(directory,\"collector-ready.txt\",b\"sealed\\n\")\nexcept m.TraceError:\n rejected=True"); assert rejected and not (moved/"collector-ready.txt").exists() and not (original/"collector-ready.txt").exists()' \
        "$PARSER" "$TMP/output-original" "$TMP/output-moved"

expect_ok "retained evidence rejects a replaced leaf and no-follow symlink" \
    python3 -c 'import importlib.util,pathlib,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); root=pathlib.Path(sys.argv[2]); directory=m.open_retained_directory(root,"runtime output",create=True); record=m.create_retained_output(directory,"trace.txt",b"trusted\n"); (root/"trace.txt").unlink(); (root/"forged").write_bytes(b"forged\n"); (root/"trace.txt").symlink_to("forged"); rejected=False; exec("try:\n m.read_retained_file(record,\"trace output\")\nexcept m.TraceError:\n rejected=True"); assert rejected' \
        "$PARSER" "$TMP/output-leaf-swap"

expect_ok "atomic publish never overwrites a target created during the race" \
    python3 -c 'import errno,importlib.util,os,pathlib,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); root=pathlib.Path(sys.argv[2]); directory=m.open_retained_directory(root,"runtime output",create=True); real_link=m.os.link; exec("def racing_link(src,dst,**kwargs):\n fd=os.open(dst,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_CLOEXEC|os.O_NOFOLLOW,0o600,dir_fd=kwargs[\"dst_dir_fd\"]); os.write(fd,b\"mutant\\n\"); os.close(fd); return real_link(src,dst,**kwargs)"); m.os.link=racing_link; rejected=False; exec("try:\n m.publish_atomic(directory,\"summary.json\",b\"sealed\\n\")\nexcept m.TraceError:\n rejected=True\nfinally:\n m.os.link=real_link"); assert rejected and (root/"summary.json").read_bytes() == b"mutant\n"' \
        "$PARSER" "$TMP/output-publish-race"

expect_ok "runtime evidence uses retained dirfd objects without output path rereads" \
    python3 -c 'import sys; s=open(sys.argv[1],encoding="ascii").read(); body=s.split("def collect_runtime(",1)[1].split("def runtime_timeout(",1)[0]; assert "open_retained_directory" in body and "verify_retained_directory" in body; assert "wait_for_completion_record_at" in body; assert "output_dir /" not in body and ".read_bytes()" not in body and ".read_text(" not in body; assert all(name in body for name in ("trace.txt","trace.stderr","collector-ready.txt","summary.json","runtime-manifest.json"))' \
        "$PARSER"

expect_ok "synthetic bpftrace 0.25 status-contract fixture reaches the READY barrier" \
    python3 -c 'import importlib.util,pathlib,subprocess,sys,tempfile; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); root=pathlib.Path(tempfile.mkdtemp()); trace=root/"trace"; stderr=root/"stderr"; trace.write_text("TRACE_BEGIN schema=1 marker=TRACE_DIAGNOSTIC_ONLY pass=path pass_id=p1\n",encoding="ascii"); stderr.write_text("Attaching 25 probes...\n",encoding="ascii"); process=subprocess.Popen([sys.executable,"-c","import time; time.sleep(2)"]); exec("try:\n m.wait_for_attach_barrier(process,trace,stderr,25,1)\nfinally:\n process.kill(); process.wait()")' \
        "$PARSER"
expect_fail "TRACE_BEGIN with the wrong actual bpftrace clause total is not READY" \
    python3 -c 'import importlib.util,pathlib,subprocess,sys,tempfile; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); root=pathlib.Path(tempfile.mkdtemp()); trace=root/"trace"; stderr=root/"stderr"; trace.write_text("TRACE_BEGIN schema=1 marker=TRACE_DIAGNOSTIC_ONLY pass=path pass_id=p1\n",encoding="ascii"); stderr.write_text("Attaching 24 probes...\n",encoding="ascii"); process=subprocess.Popen([sys.executable,"-c","import time; time.sleep(2)"]); exec("try:\n m.wait_for_attach_barrier(process,trace,stderr,25,1)\nfinally:\n process.kill(); process.wait()")' \
        "$PARSER"
expect_ok "target runtime preflight binds inventory version and codegen before attach" \
    python3 -c 'import sys; s=open(sys.argv[1],encoding="ascii").read(); body=s.split("def collect_runtime(",1)[1].split("def runtime_timeout(",1)[0]; inventory=body.index("preflight_inventory("); version=body.index("--version"); codegen=body.index("--mode\", \"codegen"); attach=body.index("tracer = subprocess.Popen("); assert inventory < version < codegen < attach; assert "inventory_sha256" in body and "codegen_sha256" in body' \
        "$PARSER"

runtime_config="$TMP/runtime-config.json"
printf '%s\n' '{"schema":1,"pass":"path","pass_id":"p1","tgids":"SPAWNED_WORKLOAD","cpus":[2],"syscalls":[426],"dev":[259,8],"lba":[4096,8191],"bytes":4096,"hctxs":[2],"qids":[3],"thread_count":8,"per_thread_candidates":128,"expected_candidates":1024}' >"$runtime_config"

expect_ok "runtime configuration binds exactly eight threads by 128 candidates" \
    python3 -c 'import importlib.util,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); result=m.materialize_runtime_config(m.Path(sys.argv[2]),12345); assert result[3:] == (8,128,1024)' \
        "$PARSER" "$runtime_config"
wrong_thread_config="$TMP/wrong-thread-runtime-config.json"
sed 's/"thread_count":8/"thread_count":7/' "$runtime_config" \
    >"$wrong_thread_config"
expect_fail "runtime configuration cannot weaken the fixed eight-thread contract" \
    python3 -c 'import importlib.util,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); m.materialize_runtime_config(m.Path(sys.argv[2]),12345)' \
        "$PARSER" "$wrong_thread_config"
expect_ok "runtime shape gate accepts exactly eight observed TIDs with 128 each" \
    python3 -c 'import importlib.util,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); summary={"quality":{"candidates":1024,"matched":1024},"records_per_tid":{str(200+i):128 for i in range(8)}}; m.validate_runtime_shape(summary,8,128,1024)' \
        "$PARSER"
expect_fail "one observed TID cannot impersonate the eight-thread contract" \
    python3 -c 'import importlib.util,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); summary={"quality":{"candidates":1024,"matched":1024},"records_per_tid":{"200":1024}}; m.validate_runtime_shape(summary,8,128,1024)' \
        "$PARSER"
expect_fail "eight observed TIDs with an unbalanced 129/127 split are rejected" \
    python3 -c 'import importlib.util,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); counts=[129,127]+[128]*6; summary={"quality":{"candidates":1024,"matched":1024},"records_per_tid":{str(200+i):value for i,value in enumerate(counts)}}; m.validate_runtime_shape(summary,8,128,1024)' \
        "$PARSER"

fake_runtime_bpftrace="$TMP/fake-runtime-bpftrace"
printf '%s\n' \
    '#!/usr/bin/env python3' \
    'import os, re, signal, sys, time' \
    'if sys.argv[1:] == ["--version"]:' \
    '    print("bpftrace v0.25.1")' \
    '    raise SystemExit(0)' \
    'if len(sys.argv) == 4 and sys.argv[1:3] == ["--mode", "codegen"]:' \
    '    if os.environ.get("FAKE_CODEGEN_FAIL") == "1":' \
    '        print("synthetic codegen failure", file=sys.stderr)' \
    '        raise SystemExit(9)' \
    '    if os.environ.get("FAKE_CODEGEN_HOOK_FILE"):' \
    '        open(os.environ["FAKE_CODEGEN_HOOK_FILE"], "w", encoding="ascii").write("CODEGEN_READY\n")' \
    '        while not os.path.exists(os.environ["FAKE_CODEGEN_RELEASE_FILE"]): time.sleep(0.01)' \
    '    print("synthetic codegen for " + sys.argv[3])' \
    '    raise SystemExit(0)' \
    'if len(sys.argv) != 2: raise SystemExit(64)' \
    'text=open(sys.argv[1], encoding="ascii").read()' \
    'match=re.search(r"TRACE_BEGIN schema=1 marker=TRACE_DIAGNOSTIC_ONLY pass=path pass_id=p1 tgids=([0-9]+)", text)' \
    'if not match: raise SystemExit(65)' \
    'tgid=int(match.group(1))' \
    'if os.environ.get("FAKE_TRACER_PID_FILE"):' \
    '    with open(os.environ["FAKE_TRACER_PID_FILE"], "w", encoding="ascii") as f: f.write(str(os.getpid()) + "\n")' \
    'if os.environ.get("FAKE_NO_BEGIN") == "1":' \
    '    stop=False' \
    '    def stopped_without_begin(_signum, _frame):' \
    '        global stop; stop=True' \
    '    signal.signal(signal.SIGINT, stopped_without_begin)' \
    '    print("Attaching 25 probes...", file=sys.stderr, flush=True)' \
    '    while not stop: time.sleep(0.01)' \
    '    raise SystemExit(0)' \
    'with open(os.environ["TRACE_EVENT_LOG"], "a", encoding="ascii") as f: f.write("TRACE_BEGIN\n")' \
    'print("Attaching 24 probes..." if os.environ.get("FAKE_WRONG_ATTACHED") == "1" else "Attaching 25 probes...", file=sys.stderr, flush=True) if os.environ.get("FAKE_NO_ATTACHED") != "1" else None' \
    'print(f"TRACE_BEGIN schema=1 marker=TRACE_DIAGNOSTIC_ONLY pass=path pass_id=p1 tgids={tgid} cpus=2 syscalls=426 dev=259:8 lba=4096:8191 hctxs=2 qids=3", flush=True)' \
    'stop=False' \
    'def stopped(_signum, _frame):' \
    '    global stop; stop=True' \
    'signal.signal(signal.SIGINT, signal.SIG_IGN if os.environ.get("FAKE_IGNORE_SIGINT") == "1" else stopped)' \
    'child_pid=0' \
    'if os.environ.get("FAKE_TRACER_CHILD_PID_FILE"):' \
    '    child_pid=os.fork()' \
    '    if child_pid == 0:' \
    '        with open(os.environ["FAKE_TRACER_CHILD_PID_FILE"], "w", encoding="ascii") as f: f.write(str(os.getpid()) + "\n")' \
    '        while not stop: time.sleep(0.01)' \
    '        raise SystemExit(0)' \
    'while not stop: time.sleep(0.01)' \
    'if child_pid: os.waitpid(child_pid, 0)' \
    'counts=[1024] if os.environ.get("FAKE_ONE_THREAD") == "1" else [128] * 8' \
    'serial=0' \
    'for lane,count in enumerate(counts):' \
    '    for seq in range(1,count + 1):' \
    '        serial += 1; tid=tgid + lane; base=100 + serial * 3000; req=0x100000 + serial' \
    '        print(f"TRACE_REQUEST pass_id=p1 tgid={tgid} tid={tid} seq={seq} req=0x{req:x} issue_req=0x{req:x} complete_req=0x{req:x} dev=259:8 lba=5000 bytes=4096 hctx=2 qid=3 polled=1 cmd_flags=4194305 cpu_enter=2 cpu_map=2 cpu_queue=2 cpu_complete=2 iopoll_check_cpu=2 iopoll_cpu=2 blk_poll_cpu=2 nvme_poll_cpu=2 enter_ns={base} tag_enter_ns={base+50} tag_exit_ns={base+80} map_enter_ns={base+100} pages_enter_ns={base+120} pages_exit_ns={base+160} map_exit_ns={base+200} queue_enter_ns={base+250} dma_map_ns={base+350} start_ns={base+400} issue_ns={base+420} queue_exit_ns={base+500} first_poll_ns={base+600} iopoll_check_enter_ns={base+600} iopoll_check_exit_ns={base+2100} iopoll_check_count=2 iopoll_check_duration_ns=1550 iopoll_enter_ns={base+650} iopoll_exit_ns={base+2050} iopoll_count=2 iopoll_duration_ns=1450 blk_poll_enter_ns={base+700} blk_poll_exit_ns={base+2000} blk_poll_count=2 blk_poll_duration_ns=1350 nvme_poll_enter_ns={base+750} nvme_poll_exit_ns={base+1950} nvme_poll_count=2 nvme_poll_duration_ns=1250 complete_ns={base+1900} exit_ns={base+2300}")' \
    'print("TRACE_END pass_id=p1 candidates=1024 emitted=1024 matched=1024 issues=1024 completes=1024 block_issues=1024 block_completes=1024 iopoll_checks=2048 iopolls=2048 blk_polls=2048 nvme_polls=2048 block_shape_errors=0 block_errors=0 collisions=0 order_errors=0 cpu_drift=0 dev_drift=0 lba_drift=0 hctx_drift=0 qid_drift=0 poll_drift=0 unpolled=0 unmatched=0", flush=True)' \
    'if os.environ.get("FAKE_RUNTIME_LOST") == "1": print("Lost 1 events", file=sys.stderr, flush=True)' \
    'with open(os.environ["TRACE_EVENT_LOG"], "a", encoding="ascii") as f: f.write("TRACE_END\n")' \
    >"$fake_runtime_bpftrace"
chmod 0700 "$fake_runtime_bpftrace"

fake_workload="$TMP/fake-workload"
printf '%s\n' \
    '#!/usr/bin/env python3' \
    'import os, signal' \
    'os.setsid()' \
    'os.sched_setscheduler(0, os.SCHED_OTHER, os.sched_param(0))' \
    'os.setpriority(os.PRIO_PROCESS, 0, -20)' \
    'os.sched_setaffinity(0, {2})' \
    'print("BOUND_IO_READY pid=%d expected_parent=%d pgid=%d sid=%d policy=0 nice=-20 affinity=2" % (os.getpid(), os.getppid(), os.getpgrp(), os.getsid(0)), flush=True)' \
    'os.kill(os.getpid(), signal.SIGSTOP)' \
    'with open(os.environ["TRACE_EVENT_LOG"], "a", encoding="ascii") as f: f.write("WORKLOAD_EXEC\n")' \
    'raise SystemExit(int(os.environ.get("FAKE_WORKLOAD_RC", "0")))' \
    >"$fake_workload"
chmod 0700 "$fake_workload"

orphan_group_helper="$TMP/orphan-tracer-group"
printf '%s\n' \
    '#!/usr/bin/env python3' \
    'import os, signal, time' \
    'child=os.fork()' \
    'if child:' \
    '    raise SystemExit(0)' \
    'with open(os.environ["ORPHAN_CHILD_PID_FILE"], "w", encoding="ascii") as f: f.write(str(os.getpid()) + "\n")' \
    'signal.signal(signal.SIGINT, signal.SIG_IGN)' \
    'while True: time.sleep(0.01)' \
    >"$orphan_group_helper"
chmod 0700 "$orphan_group_helper"
orphan_test_driver="$TMP/test-orphan-tracer-cleanup.py"
printf '%s\n' \
    'import importlib.util, os, pathlib, signal, subprocess, sys, time' \
    'spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1])' \
    'module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)' \
    'child_file=pathlib.Path(sys.argv[3])' \
    'env=dict(os.environ,ORPHAN_CHILD_PID_FILE=str(child_file))' \
    'process=subprocess.Popen([sys.argv[2]],start_new_session=True,env=env)' \
    'child=0' \
    'def live(pid):' \
    '    try: state=next(line for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines() if line.startswith("State:")).split()[1]' \
    '    except (FileNotFoundError,IndexError,StopIteration): return False' \
    '    return state != "Z"' \
    'try:' \
    '    deadline=time.monotonic()+2' \
    '    while time.monotonic()<deadline and (not child_file.exists() or not pathlib.Path(f"/proc/{process.pid}/status").exists() or next(line for line in pathlib.Path(f"/proc/{process.pid}/status").read_text().splitlines() if line.startswith("State:")).split()[1] != "Z"): time.sleep(0.01)' \
    '    child=int(child_file.read_text())' \
    '    try: module.stop_tracer(process,1)' \
    '    except module.TraceError as exc: assert "session did not stop after SIGINT" in str(exc)' \
    '    else: raise AssertionError("stubborn descendant should require SIGKILL")' \
    '    deadline=time.monotonic()+2' \
    '    while time.monotonic()<deadline and live(child): time.sleep(0.01)' \
    '    assert not live(child)' \
    'finally:' \
    '    try: os.killpg(process.pid,signal.SIGKILL)' \
    '    except ProcessLookupError: pass' \
    '    process.wait()' \
    >"$orphan_test_driver"
expect_ok "tracer cleanup kills a stubborn session descendant after its leader exits" \
    python3 "$orphan_test_driver" \
        "$PARSER" "$orphan_group_helper" "$TMP/orphan-child.pid"

env TRACE_EVENT_LOG="$TMP/runtime-events.log" "$fake_workload" \
    >"$TMP/bound-ready.txt" 2>"$TMP/bound-workload.err" &
bound_pid=$!
for _ in $(seq 1 200); do
    state=$(awk '$1 == "State:" {print $2}' "/proc/$bound_pid/status" 2>/dev/null || true)
    [ "$state" = T ] && break
    sleep 0.01
done
bound_starttime=$(python3 -c 'import sys; s=open(sys.argv[1],encoding="ascii").read(); print(int(s[s.rfind(")")+2:].split()[19]))' \
    "/proc/$bound_pid/stat")
expect_ok "READY identity checks reject every mutated PID/session/priority field" \
    python3 -c 'import importlib.util,pathlib,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); source=pathlib.Path(sys.argv[2]); line=source.read_text(encoding="ascii"); pid=int(sys.argv[3]); runner=int(sys.argv[4]); mutations=((f"pid={pid} ",f"pid={pid+1} "),(f"expected_parent={runner} ",f"expected_parent={runner+1} "),(f"pgid={pid} ",f"pgid={pid+1} "),(f"sid={pid} ",f"sid={pid+1} "),("policy=0 ","policy=1 "),("nice=-20 ","nice=-19 "),("affinity=2","affinity=3")); exec("def rejects(path):\n try:\n  m.verify_stopped_workload(pid,runner,path,{2})\n except m.TraceError:\n  return True\n return False"); paths=[]; [(lambda p,old,new: (p.write_text(line.replace(old,new),encoding="ascii"),paths.append(p)))(source.with_name(f"bad-ready-{index}"),old,new) for index,(old,new) in enumerate(mutations)]; assert all(rejects(path) for path in paths)' \
        "$PARSER" "$TMP/bound-ready.txt" "$bound_pid" "$$"

completion_file="$TMP/runtime-complete.txt"
env TRACE_EVENT_LOG="$TMP/runtime-events.log" \
    FAKE_TRACER_PID_FILE="$TMP/runtime-tracer.pid" \
    python3 "$PARSER" collect-runtime \
        --inventory "$TMP/collected.json" --config "$runtime_config" \
        --template "$ROOT/tools/trace-multithread-path.bt" \
        --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
        --kernel-release 6.14.0-1009-intel --output-dir "$TMP/runtime-output" \
        --workload-pid "$bound_pid" --runner-pid "$$" \
        --ready-file "$TMP/bound-ready.txt" \
        --completion-file "$completion_file" \
        --start-timeout 2 --workload-timeout 2 --stop-timeout 2 &
collector_pid=$!
collector_attached=0
for _ in $(seq 1 200); do
    if grep -q '^TRACE_COLLECTOR_READY marker=TRACE_DIAGNOSTIC_ONLY ' \
        "$TMP/runtime-output/collector-ready.txt" 2>/dev/null; then
        collector_attached=1
        break
    fi
    kill -0 "$collector_pid" 2>/dev/null || break
    sleep 0.01
done
workload_rc=99
if [ "$collector_attached" -eq 1 ] &&
   [ "$(awk '$1 == "State:" {print $2}' "/proc/$bound_pid/status" 2>/dev/null)" = T ] &&
   ! grep -q '^WORKLOAD_EXEC$' "$TMP/runtime-events.log" 2>/dev/null; then
    kill -CONT "$bound_pid"
    wait "$bound_pid"
    workload_rc=$?
    ready_start=$(sed -n 's/.* workload_starttime=\([0-9][0-9]*\) .*/\1/p' \
        "$TMP/runtime-output/collector-ready.txt")
    ready_collector=$(sed -n 's/.* collector_pid=\([0-9][0-9]*\) .*/\1/p' \
        "$TMP/runtime-output/collector-ready.txt")
    ready_hash=$(sha256sum "$TMP/runtime-output/collector-ready.txt" | awk '{print $1}')
    (set -o noclobber; printf 'TRACE_WORKLOAD_COMPLETE pid=%s starttime=%s collector_pid=%s ready_sha256=%s exit_code=%s\n' \
        "$bound_pid" "$ready_start" "$ready_collector" "$ready_hash" "$workload_rc" \
        >"$completion_file.tmp" && mv "$completion_file.tmp" "$completion_file")
fi
collector_rc=0
wait "$collector_pid" || collector_rc=$?
if kill -0 "$bound_pid" 2>/dev/null; then
    kill -KILL "$bound_pid" 2>/dev/null || true
    kill -CONT "$bound_pid" 2>/dev/null || true
fi
wait "$bound_pid" 2>/dev/null || true
if [ "$collector_attached" -eq 1 ] && [ "$workload_rc" -eq 0 ] &&
   [ "$collector_rc" -eq 0 ]; then
    ok "runtime collector attaches before resuming an approved stopped workload child"
else
    not_ok "runtime collector attaches before resuming an approved stopped workload child"
fi
expect_ok "runtime evidence freezes the initial and trace-observed thread identities" \
    python3 -c 'import hashlib,json,sys; m=json.load(open(sys.argv[1])); s=json.load(open(sys.argv[2])); p=int(sys.argv[3]); assert m["status"] == "TRACE_RUNTIME_VERIFIED" and m["scope"] == "THREAD_SINGLE_TGID_ONLY"; assert (m["thread_count"],m["per_thread_candidates"],m["expected_candidates"]) == (8,128,1024); assert m["workload_tgid"] == p and m["initial_tids"] == [p]; assert m["collector_ready_sha256"] == hashlib.sha256(open(sys.argv[4],"rb").read()).hexdigest(); assert m["completion_record_sha256"] == hashlib.sha256(open(sys.argv[5],"rb").read()).hexdigest(); assert s["quality"] == {"candidates":1024,"matched":1024,"matched_per_mille":1000,"transport_lost":0}; assert s["observed_tgids"] == [p] and s["observed_tids"] == list(range(p,p+8)); assert s["records_per_tid"] == {str(tid):128 for tid in range(p,p+8)}' \
        "$TMP/runtime-output/runtime-manifest.json" \
        "$TMP/runtime-output/summary.json" "$bound_pid" \
        "$TMP/runtime-output/collector-ready.txt" "$completion_file"
expect_ok "runtime ordering is attach, approved resume, then tracer END" \
    python3 -c 'import sys; lines=open(sys.argv[1],encoding="ascii").read().splitlines(); assert lines == ["TRACE_BEGIN","WORKLOAD_EXEC","TRACE_END"], lines' \
        "$TMP/runtime-events.log"
expect_ok "collector READY freezes workload identity, finite count, and all attached inputs" \
    python3 -c 'import hashlib,sys; paths=sys.argv[2:8]; s=open(sys.argv[1],encoding="ascii").read().strip(); words=s.split(); assert words[0] == "TRACE_COLLECTOR_READY"; f=dict(item.split("=",1) for item in words[1:]); expected_keys={"marker","scope","pass_id","runner_pid","workload_pid","workload_starttime","collector_pid","tracer_pid","required_probes","attached_probes","thread_count","per_thread_candidates","expected_candidates","runtime_config_sha256","expected_config_sha256","inventory_sha256","bpftrace_sha256","rendered_script_sha256","codegen_sha256"}; assert set(f) == expected_keys; assert (f["marker"],f["scope"],f["pass_id"]) == ("TRACE_DIAGNOSTIC_ONLY","THREAD_SINGLE_TGID_ONLY","p1"); assert (f["runner_pid"],f["workload_pid"],f["workload_starttime"],f["collector_pid"],f["tracer_pid"]) == tuple(sys.argv[8:13]); assert (f["required_probes"],f["attached_probes"]) == ("23","25"); assert (f["thread_count"],f["per_thread_candidates"],f["expected_candidates"]) == ("8","128","1024"); keys=("runtime_config_sha256","expected_config_sha256","inventory_sha256","bpftrace_sha256","rendered_script_sha256","codegen_sha256"); assert all(f[key] == hashlib.sha256(open(path,"rb").read()).hexdigest() for key,path in zip(keys,paths))' \
        "$TMP/runtime-output/collector-ready.txt" \
        "$runtime_config" "$TMP/runtime-output/expected.json" \
        "$TMP/collected.json" "$fake_runtime_bpftrace" \
        "$TMP/runtime-output/rendered.bt" "$TMP/runtime-output/codegen.txt" \
        "$$" "$bound_pid" "$bound_starttime" "$collector_pid" \
        "$(cat "$TMP/runtime-tracer.pid")"
expect_ok "runtime outputs remain diagnostic-only and publish no performance rate" \
    sh -c 'grep -q "TRACE_DIAGNOSTIC_ONLY" "$1/runtime-manifest.json" && ! grep -Eqi "throughput|iops|ops_per|requests_per" "$1/runtime-manifest.json" "$1/summary.json"' \
        sh "$TMP/runtime-output"

env TRACE_EVENT_LOG="$TMP/lost-events.log" "$fake_workload" \
    >"$TMP/lost-bound-ready.txt" 2>"$TMP/lost-bound.err" &
lost_bound_pid=$!
for _ in $(seq 1 200); do
    state=$(awk '$1 == "State:" {print $2}' "/proc/$lost_bound_pid/status" 2>/dev/null || true)
    [ "$state" = T ] && break
    sleep 0.01
done
lost_completion="$TMP/lost-complete.txt"
env TRACE_EVENT_LOG="$TMP/lost-events.log" FAKE_RUNTIME_LOST=1 \
    python3 "$PARSER" collect-runtime \
        --inventory "$TMP/collected.json" --config "$runtime_config" \
        --template "$ROOT/tools/trace-multithread-path.bt" \
        --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
        --kernel-release 6.14.0-1009-intel --output-dir "$TMP/lost-output" \
        --workload-pid "$lost_bound_pid" --runner-pid "$$" \
        --ready-file "$TMP/lost-bound-ready.txt" \
        --completion-file "$lost_completion" \
        --start-timeout 2 --workload-timeout 2 --stop-timeout 2 &
lost_collector_pid=$!
lost_attached=0
for _ in $(seq 1 200); do
    if grep -q '^TRACE_COLLECTOR_READY marker=TRACE_DIAGNOSTIC_ONLY ' \
        "$TMP/lost-output/collector-ready.txt" 2>/dev/null; then
        lost_attached=1
        break
    fi
    kill -0 "$lost_collector_pid" 2>/dev/null || break
    sleep 0.01
done
lost_workload_rc=99
if [ "$lost_attached" -eq 1 ]; then
    kill -CONT "$lost_bound_pid"
    wait "$lost_bound_pid"
    lost_workload_rc=$?
    lost_start=$(sed -n 's/.* workload_starttime=\([0-9][0-9]*\) .*/\1/p' \
        "$TMP/lost-output/collector-ready.txt")
    lost_collector=$(sed -n 's/.* collector_pid=\([0-9][0-9]*\) .*/\1/p' \
        "$TMP/lost-output/collector-ready.txt")
    lost_ready_hash=$(sha256sum "$TMP/lost-output/collector-ready.txt" | awk '{print $1}')
    (set -o noclobber; printf 'TRACE_WORKLOAD_COMPLETE pid=%s starttime=%s collector_pid=%s ready_sha256=%s exit_code=%s\n' \
        "$lost_bound_pid" "$lost_start" "$lost_collector" "$lost_ready_hash" "$lost_workload_rc" \
        >"$lost_completion.tmp" && mv "$lost_completion.tmp" "$lost_completion")
fi
lost_collector_rc=0
wait "$lost_collector_pid" || lost_collector_rc=$?
if kill -0 "$lost_bound_pid" 2>/dev/null; then
    kill -KILL "$lost_bound_pid" 2>/dev/null || true
    kill -CONT "$lost_bound_pid" 2>/dev/null || true
fi
wait "$lost_bound_pid" 2>/dev/null || true
if [ "$lost_attached" -eq 1 ] && [ "$lost_workload_rc" -eq 0 ] &&
   [ "$lost_collector_rc" -ne 0 ] &&
   grep -q '^Lost 1 events$' "$TMP/lost-output/trace.stderr" &&
   [ ! -e "$TMP/lost-output/summary.json" ] &&
   [ ! -e "$TMP/lost-output/runtime-manifest.json" ]; then
    ok "runtime collector derives transport loss from stderr and rejects the pass"
else
    not_ok "runtime collector derives transport loss from stderr and rejects the pass"
fi

mismatch_config="$TMP/mismatch-runtime-config.json"
cp "$runtime_config" "$mismatch_config"
env TRACE_EVENT_LOG="$TMP/mismatch-events.log" "$fake_workload" \
    >"$TMP/mismatch-bound-ready.txt" 2>"$TMP/mismatch-bound.err" &
mismatch_bound_pid=$!
for _ in $(seq 1 200); do
    state=$(awk '$1 == "State:" {print $2}' "/proc/$mismatch_bound_pid/status" 2>/dev/null || true)
    [ "$state" = T ] && break
    sleep 0.01
done
mismatch_completion="$TMP/mismatch-complete.txt"
env TRACE_EVENT_LOG="$TMP/mismatch-events.log" FAKE_ONE_THREAD=1 \
    python3 "$PARSER" collect-runtime \
        --inventory "$TMP/collected.json" --config "$mismatch_config" \
        --template "$ROOT/tools/trace-multithread-path.bt" \
        --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
        --kernel-release 6.14.0-1009-intel --output-dir "$TMP/mismatch-output" \
        --workload-pid "$mismatch_bound_pid" --runner-pid "$$" \
        --ready-file "$TMP/mismatch-bound-ready.txt" \
        --completion-file "$mismatch_completion" \
        --start-timeout 2 --workload-timeout 2 --stop-timeout 2 \
        >/dev/null 2>"$TMP/mismatch-collector.err" &
mismatch_collector_pid=$!
mismatch_attached=0
for _ in $(seq 1 200); do
    if grep -q '^TRACE_COLLECTOR_READY marker=TRACE_DIAGNOSTIC_ONLY ' \
        "$TMP/mismatch-output/collector-ready.txt" 2>/dev/null; then
        mismatch_attached=1
        break
    fi
    kill -0 "$mismatch_collector_pid" 2>/dev/null || break
    sleep 0.01
done
mismatch_workload_rc=99
if [ "$mismatch_attached" -eq 1 ]; then
    kill -CONT "$mismatch_bound_pid"
    wait "$mismatch_bound_pid"
    mismatch_workload_rc=$?
    mismatch_start=$(sed -n 's/.* workload_starttime=\([0-9][0-9]*\) .*/\1/p' \
        "$TMP/mismatch-output/collector-ready.txt")
    mismatch_collector=$(sed -n 's/.* collector_pid=\([0-9][0-9]*\) .*/\1/p' \
        "$TMP/mismatch-output/collector-ready.txt")
    mismatch_ready_hash=$(sha256sum "$TMP/mismatch-output/collector-ready.txt" | awk '{print $1}')
    (set -o noclobber; printf 'TRACE_WORKLOAD_COMPLETE pid=%s starttime=%s collector_pid=%s ready_sha256=%s exit_code=%s\n' \
        "$mismatch_bound_pid" "$mismatch_start" "$mismatch_collector" \
        "$mismatch_ready_hash" "$mismatch_workload_rc" >"$mismatch_completion.tmp" &&
        mv "$mismatch_completion.tmp" "$mismatch_completion")
fi
mismatch_collector_rc=0
wait "$mismatch_collector_pid" || mismatch_collector_rc=$?
if kill -0 "$mismatch_bound_pid" 2>/dev/null; then
    kill -KILL "$mismatch_bound_pid" 2>/dev/null || true
    kill -CONT "$mismatch_bound_pid" 2>/dev/null || true
fi
wait "$mismatch_bound_pid" 2>/dev/null || true
if [ "$mismatch_attached" -eq 1 ] && [ "$mismatch_workload_rc" -eq 0 ] &&
   [ "$mismatch_collector_rc" -ne 0 ] &&
   [ ! -e "$TMP/mismatch-output/summary.json" ] &&
   [ ! -e "$TMP/mismatch-output/runtime-manifest.json" ] &&
   grep -q 'observed TID count disagrees' "$TMP/mismatch-collector.err"; then
    ok "runtime collector rejects one TID impersonating eight trace workers"
else
    not_ok "runtime collector rejects one TID impersonating eight trace workers"
fi

env TRACE_EVENT_LOG="$TMP/codegen-events.log" "$fake_workload" \
    >"$TMP/codegen-bound-ready.txt" 2>"$TMP/codegen-bound.err" &
codegen_bound_pid=$!
for _ in $(seq 1 200); do
    state=$(awk '$1 == "State:" {print $2}' "/proc/$codegen_bound_pid/status" 2>/dev/null || true)
    [ "$state" = T ] && break
    sleep 0.01
done
codegen_rc=0
env TRACE_EVENT_LOG="$TMP/codegen-events.log" FAKE_CODEGEN_FAIL=1 \
    python3 "$PARSER" collect-runtime \
        --inventory "$TMP/collected.json" --config "$runtime_config" \
        --template "$ROOT/tools/trace-multithread-path.bt" \
        --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
        --kernel-release 6.14.0-1009-intel --output-dir "$TMP/codegen-output" \
        --workload-pid "$codegen_bound_pid" --runner-pid "$$" \
        --ready-file "$TMP/codegen-bound-ready.txt" \
        --completion-file "$TMP/codegen-complete.txt" \
        --start-timeout 2 --workload-timeout 2 --stop-timeout 2 \
        >/dev/null 2>"$TMP/codegen-collector.err" || codegen_rc=$?
codegen_state=$(awk '$1 == "State:" {print $2}' \
    "/proc/$codegen_bound_pid/status" 2>/dev/null || true)
if kill -0 "$codegen_bound_pid" 2>/dev/null; then
    kill -KILL "$codegen_bound_pid" 2>/dev/null || true
    kill -CONT "$codegen_bound_pid" 2>/dev/null || true
fi
wait "$codegen_bound_pid" 2>/dev/null || true
if [ "$codegen_rc" -ne 0 ] && [ "$codegen_state" = T ] &&
   [ ! -e "$TMP/codegen-events.log" ] &&
   [ ! -e "$TMP/codegen-output/collector-ready.txt" ] &&
   grep -q 'read-only probe query returned 9' "$TMP/codegen-collector.err"; then
    ok "failed no-attach codegen leaves the approved workload stopped"
else
    not_ok "failed no-attach codegen leaves the approved workload stopped"
fi

env TRACE_EVENT_LOG="$TMP/stale-events.log" "$fake_workload" \
    >"$TMP/stale-bound-ready.txt" 2>"$TMP/stale-bound.err" &
stale_bound_pid=$!
for _ in $(seq 1 200); do
    state=$(awk '$1 == "State:" {print $2}' "/proc/$stale_bound_pid/status" 2>/dev/null || true)
    [ "$state" = T ] && break
    sleep 0.01
done
env TRACE_EVENT_LOG="$TMP/stale-events.log" \
    FAKE_CODEGEN_HOOK_FILE="$TMP/stale-codegen-hook" \
    FAKE_CODEGEN_RELEASE_FILE="$TMP/stale-codegen-release" \
    FAKE_TRACER_PID_FILE="$TMP/stale-tracer.pid" \
    python3 "$PARSER" collect-runtime \
        --inventory "$TMP/collected.json" --config "$runtime_config" \
        --template "$ROOT/tools/trace-multithread-path.bt" \
        --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
        --kernel-release 6.14.0-1009-intel --output-dir "$TMP/stale-output" \
        --workload-pid "$stale_bound_pid" --runner-pid "$$" \
        --ready-file "$TMP/stale-bound-ready.txt" \
        --completion-file "$TMP/stale-complete.txt" \
        --start-timeout 2 --workload-timeout 2 --stop-timeout 2 \
        >/dev/null 2>"$TMP/stale-collector.err" &
stale_collector_pid=$!
stale_hook=0
for _ in $(seq 1 300); do
    if [ -s "$TMP/stale-codegen-hook" ]; then stale_hook=1; break; fi
    kill -0 "$stale_collector_pid" 2>/dev/null || break
    sleep 0.01
done
stale_workload_rc=99
if [ "$stale_hook" -eq 1 ]; then
    kill -CONT "$stale_bound_pid"
    wait "$stale_bound_pid"
    stale_workload_rc=$?
    touch "$TMP/stale-codegen-release"
fi
stale_collector_rc=0
wait "$stale_collector_pid" || stale_collector_rc=$?
if kill -0 "$stale_bound_pid" 2>/dev/null; then
    kill -KILL "$stale_bound_pid" 2>/dev/null || true
    kill -CONT "$stale_bound_pid" 2>/dev/null || true
fi
wait "$stale_bound_pid" 2>/dev/null || true
if [ "$stale_hook" -eq 1 ] && [ "$stale_workload_rc" -eq 0 ] &&
   [ "$stale_collector_rc" -ne 0 ] &&
   [ ! -e "$TMP/stale-tracer.pid" ] &&
   ! grep -q '^TRACE_BEGIN$' "$TMP/stale-events.log" 2>/dev/null &&
   [ ! -e "$TMP/stale-output/collector-ready.txt" ] &&
   [ ! -e "$TMP/stale-output/runtime-manifest.json" ] &&
   grep -q 'approved workload process disappeared' "$TMP/stale-collector.err"; then
    ok "workload identity is fully revalidated after codegen and before tracer attach"
else
    not_ok "workload identity is fully revalidated after codegen and before tracer attach"
fi

env TRACE_EVENT_LOG="$TMP/attach-timeout-events.log" "$fake_workload" \
    >"$TMP/attach-timeout-bound-ready.txt" \
    2>"$TMP/attach-timeout-bound.err" &
attach_timeout_bound_pid=$!
for _ in $(seq 1 200); do
    state=$(awk '$1 == "State:" {print $2}' "/proc/$attach_timeout_bound_pid/status" 2>/dev/null || true)
    [ "$state" = T ] && break
    sleep 0.01
done
attach_timeout_rc=0
env TRACE_EVENT_LOG="$TMP/attach-timeout-events.log" FAKE_NO_BEGIN=1 \
    FAKE_TRACER_PID_FILE="$TMP/attach-timeout-tracer.pid" \
    python3 "$PARSER" collect-runtime \
        --inventory "$TMP/collected.json" --config "$runtime_config" \
        --template "$ROOT/tools/trace-multithread-path.bt" \
        --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
        --kernel-release 6.14.0-1009-intel --output-dir "$TMP/attach-timeout-output" \
        --workload-pid "$attach_timeout_bound_pid" --runner-pid "$$" \
        --ready-file "$TMP/attach-timeout-bound-ready.txt" \
        --completion-file "$TMP/attach-timeout-complete.txt" \
        --start-timeout 1 --workload-timeout 2 --stop-timeout 2 \
        >/dev/null 2>"$TMP/attach-timeout-collector.err" || attach_timeout_rc=$?
attach_timeout_state=$(awk '$1 == "State:" {print $2}' \
    "/proc/$attach_timeout_bound_pid/status" 2>/dev/null || true)
attach_timeout_tracer=$(cat "$TMP/attach-timeout-tracer.pid" 2>/dev/null || true)
attach_timeout_tracer_live=0
if [ -n "$attach_timeout_tracer" ] && kill -0 "$attach_timeout_tracer" 2>/dev/null; then
    attach_timeout_tracer_live=1
fi
if kill -0 "$attach_timeout_bound_pid" 2>/dev/null; then
    kill -KILL "$attach_timeout_bound_pid" 2>/dev/null || true
    kill -CONT "$attach_timeout_bound_pid" 2>/dev/null || true
fi
wait "$attach_timeout_bound_pid" 2>/dev/null || true
if [ "$attach_timeout_rc" -ne 0 ] && [ "$attach_timeout_state" = T ] &&
   [ "$attach_timeout_tracer_live" -eq 0 ] &&
   [ ! -e "$TMP/attach-timeout-events.log" ] &&
   [ ! -e "$TMP/attach-timeout-output/collector-ready.txt" ] &&
   grep -q 'did not emit TRACE_BEGIN' "$TMP/attach-timeout-collector.err"; then
    ok "attach timeout reaps the tracer and leaves the approved workload stopped"
else
    not_ok "attach timeout reaps the tracer and leaves the approved workload stopped"
fi

env TRACE_EVENT_LOG="$TMP/completion-timeout-events.log" "$fake_workload" \
    >"$TMP/completion-timeout-bound-ready.txt" \
    2>"$TMP/completion-timeout-bound.err" &
completion_timeout_bound_pid=$!
for _ in $(seq 1 200); do
    state=$(awk '$1 == "State:" {print $2}' "/proc/$completion_timeout_bound_pid/status" 2>/dev/null || true)
    [ "$state" = T ] && break
    sleep 0.01
done
completion_timeout_rc=0
env TRACE_EVENT_LOG="$TMP/completion-timeout-events.log" \
    FAKE_TRACER_PID_FILE="$TMP/completion-timeout-tracer.pid" \
    python3 "$PARSER" collect-runtime \
        --inventory "$TMP/collected.json" --config "$runtime_config" \
        --template "$ROOT/tools/trace-multithread-path.bt" \
        --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
        --kernel-release 6.14.0-1009-intel --output-dir "$TMP/completion-timeout-output" \
        --workload-pid "$completion_timeout_bound_pid" --runner-pid "$$" \
        --ready-file "$TMP/completion-timeout-bound-ready.txt" \
        --completion-file "$TMP/completion-timeout-complete.txt" \
        --start-timeout 2 --workload-timeout 1 --stop-timeout 2 \
        >/dev/null 2>"$TMP/completion-timeout-collector.err" || completion_timeout_rc=$?
completion_timeout_state=$(awk '$1 == "State:" {print $2}' \
    "/proc/$completion_timeout_bound_pid/status" 2>/dev/null || true)
completion_timeout_tracer=$(cat "$TMP/completion-timeout-tracer.pid" 2>/dev/null || true)
completion_timeout_tracer_live=0
if [ -n "$completion_timeout_tracer" ] &&
   kill -0 "$completion_timeout_tracer" 2>/dev/null; then
    completion_timeout_tracer_live=1
fi
if kill -0 "$completion_timeout_bound_pid" 2>/dev/null; then
    kill -KILL "$completion_timeout_bound_pid" 2>/dev/null || true
    kill -CONT "$completion_timeout_bound_pid" 2>/dev/null || true
fi
wait "$completion_timeout_bound_pid" 2>/dev/null || true
if [ "$completion_timeout_rc" -ne 0 ] && [ "$completion_timeout_state" = T ] &&
   [ "$completion_timeout_tracer_live" -eq 0 ] &&
   ! grep -q 'WORKLOAD_EXEC' "$TMP/completion-timeout-events.log" &&
   [ ! -e "$TMP/completion-timeout-output/runtime-manifest.json" ] &&
   grep -q 'did not publish workload completion' \
       "$TMP/completion-timeout-collector.err"; then
    ok "completion timeout reaps the tracer without resuming the approved workload"
else
    not_ok "completion timeout reaps the tracer without resuming the approved workload"
fi

collector_signal_cleanup_case() {
    local signal_name=$1 label=$2 stem
    stem=$(printf '%s' "$signal_name" | tr 'A-Z' 'a-z')
    local event_log="$TMP/$stem-signal-events.log"
    local ready_file="$TMP/$stem-signal-bound-ready.txt"
    local completion="$TMP/$stem-signal-complete.txt"
    local output="$TMP/$stem-signal-output"
    local tracer_pid_file="$TMP/$stem-signal-tracer.pid"
    local child_pid_file="$TMP/$stem-signal-child.pid"
    local collector_err="$TMP/$stem-signal-collector.err"

    env TRACE_EVENT_LOG="$event_log" "$fake_workload" \
        >"$ready_file" 2>"$TMP/$stem-signal-bound.err" &
    local workload_pid=$!
    local state=
    for _ in $(seq 1 200); do
        state=$(awk '$1 == "State:" {print $2}' \
            "/proc/$workload_pid/status" 2>/dev/null || true)
        [ "$state" = T ] && break
        sleep 0.01
    done

    env TRACE_EVENT_LOG="$event_log" \
        FAKE_TRACER_PID_FILE="$tracer_pid_file" \
        FAKE_TRACER_CHILD_PID_FILE="$child_pid_file" \
        python3 "$PARSER" collect-runtime \
            --inventory "$TMP/collected.json" --config "$runtime_config" \
            --template "$ROOT/tools/trace-multithread-path.bt" \
            --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
            --kernel-release 6.14.0-1009-intel --output-dir "$output" \
            --workload-pid "$workload_pid" --runner-pid "$$" \
            --ready-file "$ready_file" --completion-file "$completion" \
            --start-timeout 2 --workload-timeout 10 --stop-timeout 2 \
            >/dev/null 2>"$collector_err" &
    local collector_pid=$!
    local attached=0
    for _ in $(seq 1 300); do
        if grep -q '^TRACE_COLLECTOR_READY marker=TRACE_DIAGNOSTIC_ONLY ' \
            "$output/collector-ready.txt" 2>/dev/null &&
           [ -s "$tracer_pid_file" ] && [ -s "$child_pid_file" ]; then
            attached=1
            break
        fi
        kill -0 "$collector_pid" 2>/dev/null || break
        sleep 0.01
    done
    if [ "$attached" -eq 1 ]; then
        kill -s "$signal_name" "$collector_pid" 2>/dev/null || true
    fi
    local collector_rc=0
    wait "$collector_pid" || collector_rc=$?
    local workload_state tracer_pid child_pid tracer_live=0 child_live=0
    workload_state=$(awk '$1 == "State:" {print $2}' \
        "/proc/$workload_pid/status" 2>/dev/null || true)
    tracer_pid=$(cat "$tracer_pid_file" 2>/dev/null || true)
    child_pid=$(cat "$child_pid_file" 2>/dev/null || true)
    if [ -n "$tracer_pid" ] && kill -0 "$tracer_pid" 2>/dev/null; then
        tracer_live=1
    fi
    if [ -n "$child_pid" ] && kill -0 "$child_pid" 2>/dev/null; then
        child_live=1
    fi
    if [ "$tracer_live" -eq 1 ] && [ -n "$tracer_pid" ]; then
        kill -KILL -- "-$tracer_pid" 2>/dev/null || true
    elif [ "$child_live" -eq 1 ] && [ -n "$child_pid" ]; then
        kill -KILL "$child_pid" 2>/dev/null || true
    fi
    if kill -0 "$workload_pid" 2>/dev/null; then
        kill -KILL "$workload_pid" 2>/dev/null || true
        kill -CONT "$workload_pid" 2>/dev/null || true
    fi
    wait "$workload_pid" 2>/dev/null || true

    if [ "$attached" -eq 1 ] && [ "$collector_rc" -ne 0 ] &&
       [ "$workload_state" = T ] && [ "$tracer_live" -eq 0 ] &&
       [ "$child_live" -eq 0 ] &&
       ! grep -q '^WORKLOAD_EXEC$' "$event_log" 2>/dev/null &&
       [ ! -e "$output/runtime-manifest.json" ] &&
       grep -q "collector interrupted by SIG$signal_name" "$collector_err"; then
        ok "$label"
    else
        not_ok "$label"
    fi
}

collector_signal_cleanup_case TERM \
    "SIGTERM reaps the independent tracer session and preserves workload STOP"
collector_signal_cleanup_case HUP \
    "SIGHUP reaps the independent tracer session and preserves workload STOP"
collector_signal_cleanup_case INT \
    "SIGINT reaps the independent tracer session and preserves workload STOP"

bad_timeout_rc=0
python3 "$PARSER" collect-runtime \
    --inventory "$TMP/collected.json" --config "$runtime_config" \
    --template "$ROOT/tools/trace-multithread-path.bt" \
    --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
    --kernel-release 6.14.0-1009-intel --output-dir "$TMP/bad-timeout-output" \
    --workload-pid 1 --runner-pid "$$" --ready-file "$TMP/absent-ready" \
    --completion-file "$TMP/absent-completion" \
    --start-timeout 0 --workload-timeout 2 --stop-timeout 2 \
    >/dev/null 2>"$TMP/bad-timeout.err" || bad_timeout_rc=$?
if [ "$bad_timeout_rc" -ne 0 ] &&
   grep -q 'timeout must be an integer from 1 through 300 seconds' \
       "$TMP/bad-timeout.err" &&
   [ ! -e "$TMP/bad-timeout-output" ]; then
    ok "invalid timeout input is rejected before any runtime action"
else
    not_ok "invalid timeout input is rejected before any runtime action"
fi
expect_ok "runtime collector has no workload spawn or resume path" \
    python3 -c 'import sys; s=open(sys.argv[1],encoding="ascii").read(); body=s.split("def collect_runtime(",1)[1].split("def runtime_timeout(",1)[0]; assert "signal.SIGCONT" not in body; assert body.count("subprocess.Popen(") == 1; assert "[bpftrace_exec, rendered_exec]" in body' "$PARSER"

self_spawn_rc=0
python3 "$PARSER" collect-runtime \
    --inventory "$TMP/collected.json" --config "$runtime_config" \
    --template "$ROOT/tools/trace-multithread-path.bt" \
    --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
    --kernel-release 6.14.0-1009-intel --output-dir "$TMP/self-spawn-output" \
    --workload-pid 1 --runner-pid "$$" --ready-file "$TMP/absent-ready" \
    --completion-file "$TMP/absent-completion" -- /bin/true \
    >/dev/null 2>"$TMP/self-spawn.err" || self_spawn_rc=$?
if [ "$self_spawn_rc" -ne 0 ] &&
   grep -q 'unrecognized arguments.*bin/true' "$TMP/self-spawn.err" &&
   [ ! -e "$TMP/self-spawn-output" ]; then
    ok "runtime CLI refuses a workload command instead of bypassing the approved runner"
else
    not_ok "runtime CLI refuses a workload command instead of bypassing the approved runner"
fi

env TRACE_EVENT_LOG="$TMP/stubborn-events.log" "$fake_workload" \
    >"$TMP/stubborn-bound-ready.txt" 2>"$TMP/stubborn-bound.err" &
stubborn_bound_pid=$!
for _ in $(seq 1 200); do
    state=$(awk '$1 == "State:" {print $2}' "/proc/$stubborn_bound_pid/status" 2>/dev/null || true)
    [ "$state" = T ] && break
    sleep 0.01
done
stubborn_completion="$TMP/stubborn-complete.txt"
env TRACE_EVENT_LOG="$TMP/stubborn-events.log" FAKE_IGNORE_SIGINT=1 \
    FAKE_TRACER_PID_FILE="$TMP/stubborn-tracer.pid" \
    python3 "$PARSER" collect-runtime \
        --inventory "$TMP/collected.json" --config "$runtime_config" \
        --template "$ROOT/tools/trace-multithread-path.bt" \
        --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
        --kernel-release 6.14.0-1009-intel --output-dir "$TMP/stubborn-output" \
        --workload-pid "$stubborn_bound_pid" --runner-pid "$$" \
        --ready-file "$TMP/stubborn-bound-ready.txt" \
        --completion-file "$stubborn_completion" \
        --start-timeout 2 --workload-timeout 2 --stop-timeout 1 \
        >/dev/null 2>"$TMP/stubborn-collector.err" &
stubborn_collector_pid=$!
stubborn_attached=0
for _ in $(seq 1 200); do
    if grep -q '^TRACE_COLLECTOR_READY marker=TRACE_DIAGNOSTIC_ONLY ' \
        "$TMP/stubborn-output/collector-ready.txt" 2>/dev/null; then
        stubborn_attached=1
        break
    fi
    kill -0 "$stubborn_collector_pid" 2>/dev/null || break
    sleep 0.01
done
stubborn_workload_rc=99
if [ "$stubborn_attached" -eq 1 ]; then
    kill -CONT "$stubborn_bound_pid"
    wait "$stubborn_bound_pid"
    stubborn_workload_rc=$?
    stubborn_start=$(sed -n 's/.* workload_starttime=\([0-9][0-9]*\) .*/\1/p' \
        "$TMP/stubborn-output/collector-ready.txt")
    stubborn_collector=$(sed -n 's/.* collector_pid=\([0-9][0-9]*\) .*/\1/p' \
        "$TMP/stubborn-output/collector-ready.txt")
    stubborn_ready_hash=$(sha256sum "$TMP/stubborn-output/collector-ready.txt" | awk '{print $1}')
    (set -o noclobber; printf 'TRACE_WORKLOAD_COMPLETE pid=%s starttime=%s collector_pid=%s ready_sha256=%s exit_code=%s\n' \
        "$stubborn_bound_pid" "$stubborn_start" "$stubborn_collector" \
        "$stubborn_ready_hash" "$stubborn_workload_rc" >"$stubborn_completion.tmp" &&
        mv "$stubborn_completion.tmp" "$stubborn_completion")
fi
stubborn_collector_rc=0
wait "$stubborn_collector_pid" || stubborn_collector_rc=$?
stubborn_tracer=$(cat "$TMP/stubborn-tracer.pid" 2>/dev/null || true)
stubborn_tracer_live=0
if [ -n "$stubborn_tracer" ] && kill -0 "$stubborn_tracer" 2>/dev/null; then
    stubborn_tracer_live=1
fi
if kill -0 "$stubborn_bound_pid" 2>/dev/null; then
    kill -KILL "$stubborn_bound_pid" 2>/dev/null || true
    kill -CONT "$stubborn_bound_pid" 2>/dev/null || true
fi
wait "$stubborn_bound_pid" 2>/dev/null || true
if [ "$stubborn_attached" -eq 1 ] && [ "$stubborn_workload_rc" -eq 0 ] &&
   [ "$stubborn_collector_rc" -ne 0 ] && [ "$stubborn_tracer_live" -eq 0 ] &&
   [ ! -e "$TMP/stubborn-output/runtime-manifest.json" ] &&
   grep -q 'did not stop after SIGINT' "$TMP/stubborn-collector.err"; then
    ok "a tracer that ignores SIGINT is killed, reaped, and cannot seal evidence"
else
    not_ok "a tracer that ignores SIGINT is killed, reaped, and cannot seal evidence"
fi

expect_ok "completion handshake returns the exit status bound to the full READY hash" \
    python3 -c 'import importlib.util,os,pathlib,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); path=pathlib.Path(sys.argv[2]); ready_hash="a"*64; raw=f"TRACE_WORKLOAD_COMPLETE pid=99999999 starttime=123 collector_pid={os.getpid()} ready_sha256={ready_hash} exit_code=7\n".encode("ascii"); path.write_bytes(raw); assert m.wait_for_completion_record(path,99999999,123,os.getpid(),ready_hash,1) == (7,raw)' \
        "$PARSER" "$TMP/exact-completion"
expect_ok "completion handshake rejects mutated identity or full READY hash" \
    python3 -c 'import importlib.util,os,pathlib,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); root=pathlib.Path(sys.argv[2]); collector=os.getpid(); ready_hash="a"*64; line=f"TRACE_WORKLOAD_COMPLETE pid=99999999 starttime=123 collector_pid={collector} ready_sha256={ready_hash} exit_code=0\n"; mutations=(("pid=99999999 ","pid=99999998 "),("starttime=123 ","starttime=124 "),(f"collector_pid={collector} ",f"collector_pid={collector+1} "),(f"ready_sha256={ready_hash} ","ready_sha256="+"b"*64+" ")); exec("def rejects(path):\n try:\n  m.wait_for_completion_record(path,99999999,123,collector,ready_hash,1)\n except m.TraceError:\n  return True\n return False"); paths=[]; [(lambda p,old,new: (p.write_text(line.replace(old,new),encoding="ascii"),paths.append(p)))(root.with_name(f"bad-completion-{index}"),old,new) for index,(old,new) in enumerate(mutations)]; assert all(rejects(path) for path in paths)' \
        "$PARSER" "$TMP/completion-mutation-root"

expect_fail "completion handshake refuses a symlink even when its target has valid fields" \
    python3 -c 'import importlib.util,os,pathlib,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); target=pathlib.Path(sys.argv[2]); link=pathlib.Path(sys.argv[3]); pid=99999999; start=123; ready_hash="a"*64; target.write_text(f"TRACE_WORKLOAD_COMPLETE pid={pid} starttime={start} collector_pid={os.getpid()} ready_sha256={ready_hash} exit_code=0\n",encoding="ascii"); link.symlink_to(target); m.wait_for_completion_record(link,pid,start,os.getpid(),ready_hash,1)' \
        "$PARSER" "$TMP/completion-target" "$TMP/completion-link"

expect_ok "validated completion evidence survives replacement with a symlink" \
    python3 -c 'import hashlib,importlib.util,os,pathlib,sys; spec=importlib.util.spec_from_file_location("trace_path",sys.argv[1]); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); path=pathlib.Path(sys.argv[2]); target=pathlib.Path(sys.argv[3]); ready_hash="a"*64; raw=f"TRACE_WORKLOAD_COMPLETE pid=99999999 starttime=123 collector_pid={os.getpid()} ready_sha256={ready_hash} exit_code=0\n".encode("ascii"); path.write_bytes(raw); rc,validated=m.wait_for_completion_record(path,99999999,123,os.getpid(),ready_hash,1); path.unlink(); target.write_bytes(b"forged\n"); path.symlink_to(target); assert rc == 0 and validated == raw; assert hashlib.sha256(validated).hexdigest() != hashlib.sha256(path.read_bytes()).hexdigest()' \
        "$PARSER" "$TMP/replaced-completion" "$TMP/forged-completion"
expect_ok "runtime manifest hashes only the no-follow-validated completion bytes" \
    python3 -c 'import sys; s=open(sys.argv[1],encoding="ascii").read(); body=s.split("def collect_runtime(",1)[1].split("def runtime_timeout(",1)[0]; assert "completion_file.read_bytes" not in body; assert "completion_record_bytes" in body and "hashlib.sha256(completion_record_bytes)" in body' \
        "$PARSER"

env TRACE_EVENT_LOG="$TMP/workload-fail-events.log" FAKE_WORKLOAD_RC=7 \
    "$fake_workload" >"$TMP/workload-fail-bound-ready.txt" \
    2>"$TMP/workload-fail-bound.err" &
workload_fail_bound_pid=$!
for _ in $(seq 1 200); do
    state=$(awk '$1 == "State:" {print $2}' "/proc/$workload_fail_bound_pid/status" 2>/dev/null || true)
    [ "$state" = T ] && break
    sleep 0.01
done
workload_fail_completion="$TMP/workload-fail-complete.txt"
env TRACE_EVENT_LOG="$TMP/workload-fail-events.log" python3 "$PARSER" collect-runtime \
        --inventory "$TMP/collected.json" --config "$runtime_config" \
        --template "$ROOT/tools/trace-multithread-path.bt" \
        --bpftrace "$fake_runtime_bpftrace" --btf "$TMP/vmlinux.btf" \
        --kernel-release 6.14.0-1009-intel --output-dir "$TMP/workload-fail-output" \
        --workload-pid "$workload_fail_bound_pid" --runner-pid "$$" \
        --ready-file "$TMP/workload-fail-bound-ready.txt" \
        --completion-file "$workload_fail_completion" \
        --start-timeout 2 --workload-timeout 2 --stop-timeout 2 \
        >/dev/null 2>"$TMP/workload-fail-collector.err" &
workload_fail_collector_pid=$!
workload_fail_attached=0
for _ in $(seq 1 200); do
    if grep -q '^TRACE_COLLECTOR_READY marker=TRACE_DIAGNOSTIC_ONLY ' \
        "$TMP/workload-fail-output/collector-ready.txt" 2>/dev/null; then
        workload_fail_attached=1
        break
    fi
    kill -0 "$workload_fail_collector_pid" 2>/dev/null || break
    sleep 0.01
done
workload_fail_rc=99
if [ "$workload_fail_attached" -eq 1 ]; then
    kill -CONT "$workload_fail_bound_pid"
    wait "$workload_fail_bound_pid"
    workload_fail_rc=$?
    workload_fail_start=$(sed -n 's/.* workload_starttime=\([0-9][0-9]*\) .*/\1/p' \
        "$TMP/workload-fail-output/collector-ready.txt")
    workload_fail_collector=$(sed -n 's/.* collector_pid=\([0-9][0-9]*\) .*/\1/p' \
        "$TMP/workload-fail-output/collector-ready.txt")
    workload_fail_ready_hash=$(sha256sum "$TMP/workload-fail-output/collector-ready.txt" | awk '{print $1}')
    (set -o noclobber; printf 'TRACE_WORKLOAD_COMPLETE pid=%s starttime=%s collector_pid=%s ready_sha256=%s exit_code=%s\n' \
        "$workload_fail_bound_pid" "$workload_fail_start" \
        "$workload_fail_collector" "$workload_fail_ready_hash" "$workload_fail_rc" \
        >"$workload_fail_completion.tmp" &&
        mv "$workload_fail_completion.tmp" "$workload_fail_completion")
fi
workload_fail_collector_rc=0
wait "$workload_fail_collector_pid" || workload_fail_collector_rc=$?
if kill -0 "$workload_fail_bound_pid" 2>/dev/null; then
    kill -KILL "$workload_fail_bound_pid" 2>/dev/null || true
    kill -CONT "$workload_fail_bound_pid" 2>/dev/null || true
fi
wait "$workload_fail_bound_pid" 2>/dev/null || true
if [ "$workload_fail_attached" -eq 1 ] && [ "$workload_fail_rc" -eq 7 ] &&
   [ "$workload_fail_collector_rc" -ne 0 ] &&
   [ ! -e "$TMP/workload-fail-output/summary.json" ] &&
   [ ! -e "$TMP/workload-fail-output/runtime-manifest.json" ] &&
   grep -q 'approved workload exited with status 7' \
       "$TMP/workload-fail-collector.err"; then
    ok "a nonzero approved workload status cannot seal trace evidence"
else
    not_ok "a nonzero approved workload status cannot seal trace evidence"
fi

printf '1..%d\n' "$((passed + failed))"
printf '# passed=%d failed=%d\n' "$passed" "$failed"
test "$failed" -eq 0
