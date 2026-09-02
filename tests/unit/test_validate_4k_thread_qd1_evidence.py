#!/usr/bin/env python3
import csv
import json
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "validate-4k-thread-qd1-evidence.py"


class EvidenceToolTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(
            prefix="exitos-validate-4k-thread-qd1-evidence."
        )
        self.root = pathlib.Path(self.temp.name)
        self.cell_index = 0

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, *args):
        return subprocess.run(
            [str(TOOL), *map(str, args)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def make_cell(self, arm="C", jobs=4, file_bytes=4096 * 16,
                  expected_ios=40):
        self.cell_index += 1
        cell = self.root / f"cell-{self.cell_index}-{arm}-{jobs}"
        cell.mkdir()
        per_job = expected_ios // jobs
        fio_jobs = []
        for index in range(jobs):
            fio_jobs.append({
                "jobname": f"job-{index}",
                "error": 0,
                "write": {
                    "total_ios": per_job,
                    "iops": 1000.0 + index,
                    "lat_ns": {"mean": 100000.0},
                },
                "sync": {"lat_ns": {"N": per_job - 1,
                                       "mean": 200000.0}},
            })
        (cell / "fio.json").write_text(
            json.dumps({"jobs": fio_jobs}) + "\n", encoding="utf-8"
        )
        lines = ["ARMED (syscall instructions rewritten)"]
        for index in range(jobs):
            fd = 9 + index
            lines.append(
                "PREPARE_OUTCOME v=1 frontend=bpftime "
                f"fd={fd} mode=0 off=0 len={file_bytes} "
                "outcome=prepared stage=complete rc=0 return_rc=0 "
                "return_errno=0 rdwr_alias=1 "
                f"prepared={file_bytes} chunks=1"
            )
            lines.append(
                "PREPARE_READY frontend=bpftime "
                f"fd={fd} mode=0 off=0 len={file_bytes} "
                f"prepared={file_bytes} chunks=1 fiemap=safe unsafe_flags=0"
            )
        lines.extend([
            "PREPARE_STOP_ARMED v=1 frontend=bpftime "
            f"arrived={jobs} expected={jobs} stop_after=selected-close",
            "PREPARE_STOP_READY v=1 frontend=bpftime "
            f"arrived={jobs} expected={jobs} signal=SIGSTOP "
            "after=selected-close",
            "PREPARE_STOP_RELEASED v=1 frontend=bpftime "
            f"arrived={jobs} expected={jobs} signal=SIGCONT",
        ])
        timed_passes = ((per_job * 4096) + file_bytes - 1) // file_bytes
        lifecycle_count = jobs * (1 + timed_passes)
        lines.extend(f"job-{index} open -> FAST PATH"
                     for index in range(lifecycle_count))
        lines.append("[exitos/bpftime] hook entered 12345 times, claimed 12000")
        (cell / "stderr.txt").write_text("\n".join(lines) + "\n",
                                          encoding="utf-8")
        sync_ios = expected_ios - jobs
        stats = ((0, 0, 0, 0, jobs) if arm == "B" else
                 (expected_ios, sync_ios, 0, 0, jobs))
        (cell / "stats.txt").write_text(
            "".join(f"{value}\n" for value in stats), encoding="ascii"
        )
        return cell, file_bytes, expected_ios

    def validate_cell(self, cell, arm, jobs, file_bytes, expected_ios):
        return self.run_tool(
            "validate-cell", "--fio-json", cell / "fio.json",
            "--stderr", cell / "stderr.txt", "--stats", cell / "stats.txt",
            "--arm", arm, "--jobs", jobs, "--file-bytes", file_bytes,
            "--expected-write-ios", expected_ios,
        )

    def test_validate_cell_accepts_exact_B_and_C_evidence(self):
        for arm in ("B", "C"):
            with self.subTest(arm=arm):
                cell, file_bytes, expected_ios = self.make_cell(arm=arm)
                result = self.validate_cell(
                    cell, arm, 4, file_bytes, expected_ios
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                payload = json.loads(result.stdout)
                self.assertEqual(payload["status"], "valid")
                self.assertEqual(payload["write_ios"], expected_ios)
                self.assertEqual(payload["sync_ios"], expected_ios - 4)
                self.assertEqual(payload["prepare_failed"], 0)
                self.assertEqual(payload["fast_path_lifecycle"], 8)
                self.assertEqual(payload["stats"]["arm"], arm)

    def test_validate_cell_accepts_fio_reopen_for_each_file_pass(self):
        jobs = 4
        file_bytes = 4096 * 16
        expected_ios = jobs * 16 * 3
        cell, file_bytes, expected_ios = self.make_cell(
            arm="C", jobs=jobs, file_bytes=file_bytes,
            expected_ios=expected_ios,
        )
        result = self.validate_cell(
            cell, "C", jobs, file_bytes, expected_ios
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["fast_path_lifecycle"], jobs * (1 + 3))

    def test_validate_cell_rejects_each_required_prepare_contract(self):
        mutations = {
            "outcome-v1": ("v=1", "v=2"),
            "outcome-fd": ("fd=9", "fd=-1"),
            "outcome-mode": ("mode=0", "mode=1"),
            "outcome-off": ("off=0", "off=4096"),
            "outcome-len": ("len=65536", "len=61440"),
            "outcome-name": ("outcome=prepared", "outcome=native-skip"),
            "outcome-stage": ("stage=complete", "stage=exchange"),
            "outcome-rc": ("rc=0", "rc=-5"),
            "outcome-return": ("return_rc=0", "return_rc=-5"),
            "outcome-errno": ("return_errno=0", "return_errno=5"),
            "outcome-alias": ("rdwr_alias=1", "rdwr_alias=0"),
            "outcome-prepared": ("prepared=65536", "prepared=61440"),
            "outcome-chunks": ("chunks=1", "chunks=0"),
            "ready-fiemap": ("fiemap=safe", "fiemap=unsafe"),
            "ready-unsafe": ("unsafe_flags=0", "unsafe_flags=1"),
        }
        for label, (old, new) in mutations.items():
            with self.subTest(field=label):
                cell, file_bytes, expected_ios = self.make_cell()
                path = cell / "stderr.txt"
                text = path.read_text(encoding="utf-8")
                path.write_text(text.replace(old, new, 1), encoding="utf-8")
                result = self.validate_cell(
                    cell, "C", 4, file_bytes, expected_ios
                )
                self.assertEqual(result.returncode, 3, result.stdout)
                self.assertIn("EVIDENCE_INVALID", result.stderr)

    def test_validate_cell_rejects_failure_count_stats_and_lifecycle_drift(self):
        cases = []
        cell, file_bytes, expected_ios = self.make_cell()
        (cell / "stderr.txt").write_text(
            (cell / "stderr.txt").read_text(encoding="utf-8") +
            "PREPARE_FAILED reason=injected\n", encoding="utf-8"
        )
        cases.append(("prepare-failed", cell, file_bytes, expected_ios))

        cell, file_bytes, expected_ios = self.make_cell()
        payload = json.loads((cell / "fio.json").read_text(encoding="utf-8"))
        payload["jobs"][0]["sync"]["lat_ns"]["N"] += 1
        (cell / "fio.json").write_text(json.dumps(payload), encoding="utf-8")
        cases.append(("sync-count", cell, file_bytes, expected_ios))

        cell, file_bytes, expected_ios = self.make_cell()
        (cell / "stats.txt").write_text("39\n36\n0\n0\n4\n",
                                          encoding="ascii")
        cases.append(("stats", cell, file_bytes, expected_ios))

        cell, file_bytes, expected_ios = self.make_cell()
        text = (cell / "stderr.txt").read_text(encoding="utf-8")
        (cell / "stderr.txt").write_text(
            text.replace("job-0 open -> FAST PATH\n", "", 1),
            encoding="utf-8",
        )
        cases.append(("fast-path-count", cell, file_bytes, expected_ios))

        cell, file_bytes, expected_ios = self.make_cell()
        text = (cell / "stderr.txt").read_text(encoding="utf-8")
        (cell / "stderr.txt").write_text(
            text.replace("ARMED (syscall instructions rewritten)\n", "", 1),
            encoding="utf-8",
        )
        cases.append(("armed", cell, file_bytes, expected_ios))

        cell, file_bytes, expected_ios = self.make_cell()
        text = (cell / "stderr.txt").read_text(encoding="utf-8")
        (cell / "stderr.txt").write_text(
            text.replace("hook entered 12345 times, claimed 12000",
                         "hook entered 0 times, claimed 0"),
            encoding="utf-8",
        )
        cases.append(("hook", cell, file_bytes, expected_ios))

        cell, file_bytes, expected_ios = self.make_cell()
        text = (cell / "stderr.txt").read_text(encoding="utf-8")
        (cell / "stderr.txt").write_text(
            text.replace(
                "PREPARE_STOP_READY v=1 frontend=bpftime ",
                "PREPARE_STOP_READY v=2 frontend=bpftime ", 1,
            ),
            encoding="utf-8",
        )
        cases.append(("prepare-stop", cell, file_bytes, expected_ios))

        for label, case_cell, case_bytes, case_ios in cases:
            with self.subTest(case=label):
                result = self.validate_cell(
                    case_cell, "C", 4, case_bytes, case_ios
                )
                self.assertEqual(result.returncode, 3, result.stdout)
                self.assertIn("EVIDENCE_INVALID", result.stderr)

    @staticmethod
    def tsv_header():
        return [
            "block", "position", "config", "jobs", "arm", "iops",
            "pair_us", "write_ios", "sync_ios", "write_lat_us",
            "sync_lat_us", "fast_write", "fast_sync", "pass", "declined",
            "refresh", "hook_entries", "hook_claimed", "timed_pre_temp_c",
            "timed_post_temp_c",
        ]

    def make_tsv(self, ratio4, ratio8, temp_override=None):
        path = self.root / f"summary-{len(list(self.root.glob('summary-*')))}.tsv"
        rows = []
        orders = (
            ("4B", "4C", "8B", "8C"),
            ("8C", "8B", "4C", "4B"),
            ("4C", "4B", "8C", "8B"),
            ("8B", "8C", "4B", "4C"),
            ("8B", "8C", "4B", "4C"),
            ("4C", "4B", "8C", "8B"),
            ("8C", "8B", "4C", "4B"),
            ("4B", "4C", "8B", "8C"),
        )
        for block in range(8):
            cells = {
                "4B": ("threads4", 4, "B", 1000.0),
                "4C": ("threads4", 4, "C", 1000.0 * ratio4),
                "8B": ("threads8", 8, "B", 1800.0),
                "8C": ("threads8", 8, "C", 1800.0 * ratio8),
            }
            for position, token in enumerate(orders[block]):
                config, jobs, arm, iops = cells[token]
                expected_ios = 80
                sync_ios = expected_ios - jobs
                pre_temp = 40
                if temp_override and (block, config, arm) in temp_override:
                    pre_temp = temp_override[(block, config, arm)]
                fast_write = expected_ios if arm == "C" else 0
                fast_sync = sync_ios if arm == "C" else 0
                rows.append({
                    "block": block, "position": position, "config": config,
                    "jobs": jobs, "arm": arm, "iops": iops,
                    "pair_us": jobs * 1_000_000 / iops,
                    "write_ios": expected_ios, "sync_ios": sync_ios,
                    "write_lat_us": 10.0, "sync_lat_us": 20.0,
                    "fast_write": fast_write, "fast_sync": fast_sync,
                    "pass": 0, "declined": 0, "refresh": jobs,
                    "hook_entries": 1000, "hook_claimed": 900,
                    "timed_pre_temp_c": pre_temp,
                    "timed_post_temp_c": pre_temp,
                })
        with path.open("w", encoding="utf-8", newline="") as target:
            writer = csv.DictWriter(target, fieldnames=self.tsv_header(),
                                    delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)
        return path

    def summarize(self, path):
        return self.run_tool("summarize", "--tsv", path)

    def test_summarize_detects_8T_only_regression(self):
        result = self.summarize(self.make_tsv(1.0, 0.90))
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertNotEqual(
            payload["classification"]["threads4"]["label"],
            "stable-regression",
        )
        self.assertEqual(
            payload["classification"]["threads8"]["label"],
            "stable-regression",
        )
        self.assertEqual(
            payload["thread_scaling"]["label"],
            "stable-thread-scaling-regression",
        )
        self.assertTrue(payload["diagnosis_trigger"])

    def test_summarize_detects_scaling_only_regression(self):
        result = self.summarize(self.make_tsv(1.04, 0.98))
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertNotEqual(
            payload["classification"]["threads4"]["label"],
            "stable-regression",
        )
        self.assertNotEqual(
            payload["classification"]["threads8"]["label"],
            "stable-regression",
        )
        self.assertLess(payload["thread_scaling"]["median_pct"], -5.0)
        self.assertEqual(
            payload["thread_scaling"]["label"],
            "stable-thread-scaling-regression",
        )

    def test_summarize_temperature_invalid_blocks_conclusion(self):
        path = self.make_tsv(
            1.0, 0.90,
            {(0, "threads8", "B"): 40,
             (0, "threads8", "C"): 43},
        )
        result = self.summarize(path)
        self.assertEqual(result.returncode, 4, result.stderr)
        payload = json.loads(result.stdout)
        self.assertFalse(payload["environment"]["valid_for_conclusion"])
        self.assertEqual(
            payload["environment"]["threads8"]
                   ["paired_timed_start_temperature_delta_c"][0],
            3.0,
        )
        self.assertIsNone(payload["diagnosis_trigger"])
        self.assertIn("ENVIRONMENT_INVALID", result.stderr)

    def test_summarize_rejects_pair_us_and_exact_order_drift(self):
        path = self.make_tsv(1.0, 1.0)
        rows = list(csv.DictReader(path.read_text(encoding="utf-8").splitlines(),
                                   delimiter="\t"))
        rows[0]["pair_us"] = "999999"
        with path.open("w", encoding="utf-8", newline="") as target:
            writer = csv.DictWriter(target, fieldnames=self.tsv_header(),
                                    delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)
        self.assertEqual(self.summarize(path).returncode, 3)

        path = self.make_tsv(1.0, 1.0)
        rows = list(csv.DictReader(path.read_text(encoding="utf-8").splitlines(),
                                   delimiter="\t"))
        rows[0]["position"], rows[1]["position"] = (
            rows[1]["position"], rows[0]["position"]
        )
        with path.open("w", encoding="utf-8", newline="") as target:
            writer = csv.DictWriter(target, fieldnames=self.tsv_header(),
                                    delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)
        self.assertEqual(self.summarize(path).returncode, 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
