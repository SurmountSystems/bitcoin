#!/usr/bin/env python3
"""Unit tests for contrib/swords/baseline_lib.py and baseline_report.py."""
from __future__ import annotations

import importlib.util
import io
import json
import os
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE_LIB_PATH = ROOT / "contrib" / "swords" / "baseline_lib.py"
BASELINE_REPORT_PATH = ROOT / "contrib" / "swords" / "baseline_report.py"


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


SAMPLE_BENCH_JSON = {
    "benchmarks": [
        {"name": "DecompressBlockPayloadSerial", "median": 1000.0},
        {"name": "DecompressBlockPayloadParallel", "median": 800.0, "real_time": 799.0},
        {"name": "ReadRawBlockCompressed", "real_time": 500.0},
    ]
}

BASELINE_DOC = {
    "host": "testhost",
    "git_sha": "abc1234",
    "captured_at": "20260706T120000Z",
    "medians_ns": {
        "DecompressBlockPayloadSerial": 1000.0,
        "DecompressBlockPayloadParallel": 800.0,
        "ReadRawBlockCompressed": 500.0,
    },
    "segment_replay": {
        "blocks": 100,
        "blocks_per_hr": 1000.0,
        "p50_load_ms": 10.0,
        "p50_connect_ms": 20.0,
        "benchstats_summary": {
            "rollup_count": 2,
            "p50_disk_per_blk_ms": 5.0,
            "p50_connect_cs_per_blk_ms": 3.0,
            "p50_flush_lmdb_per_blk_ms": 1.0,
            "implied_blk_per_s": 2.5,
        },
    },
}


class BaselineLibTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module(BASELINE_LIB_PATH, "baseline_lib")
        cls.report = load_module(BASELINE_REPORT_PATH, "baseline_report")

    def test_medians_from_bench_json(self):
        medians = self.mod.medians_from_bench_json(SAMPLE_BENCH_JSON)
        self.assertEqual(medians["DecompressBlockPayloadSerial"], 1000.0)
        self.assertEqual(medians["DecompressBlockPayloadParallel"], 800.0)
        self.assertEqual(medians["ReadRawBlockCompressed"], 500.0)

    def test_capture_doc_shape(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as tmp:
            json.dump(SAMPLE_BENCH_JSON, tmp)
            bench_path = tmp.name
        try:
            segment = {"blocks_per_hr": 900.0, "p50_load_ms": 11.0}
            doc = self.mod.capture_from_bench_json(
                bench_path, "host1", "deadbeef", "20260706T000000Z", segment
            )
            self.assertEqual(doc["host"], "host1")
            self.assertEqual(doc["git_sha"], "deadbeef")
            self.assertEqual(doc["captured_at"], "20260706T000000Z")
            self.assertEqual(len(doc["medians_ns"]), 3)
            self.assertEqual(doc["segment_replay"], segment)
        finally:
            os.unlink(bench_path)

    def test_compare_ok_warn_fail_thresholds(self):
        current = {
            "DecompressBlockPayloadSerial": 1040.0,  # +4% OK
            "DecompressBlockPayloadParallel": 1120.0,  # +15% FAIL
            "ReadRawBlockCompressed": 440.0,  # -12% WARN (|delta|>10%)
        }
        rows = self.mod.compare_baselines(BASELINE_DOC, current, {})
        by_label = {r.label: r for r in rows}
        self.assertEqual(by_label["DecompressBlockPayloadSerial"].mark, "OK")
        self.assertEqual(by_label["DecompressBlockPayloadParallel"].mark, "FAIL")
        self.assertEqual(by_label["ReadRawBlockCompressed"].mark, "WARN")
        self.assertEqual(self.mod.compare_exit_code(rows), 2)

    def test_compare_missing_bench_does_not_affect_exit(self):
        baseline = {k: v for k, v in BASELINE_DOC.items() if k != "segment_replay"}
        current = {"DecompressBlockPayloadSerial": 1000.0}
        rows = self.mod.compare_baselines(baseline, current, {})
        missing = [r for r in rows if r.label.startswith("missing bench:")]
        self.assertEqual(len(missing), 2)
        self.assertFalse(any(r.affects_exit for r in missing))
        self.assertEqual(self.mod.compare_exit_code(rows), 0)

    def test_compare_segment_replay_and_benchstats(self):
        current_segment = {
            "blocks_per_hr": 1100.0,  # +10% WARN boundary (exactly 10% -> OK per shell abs>0.10)
            "p50_load_ms": 12.0,  # +20% WARN
            "p50_connect_ms": 20.0,
            "benchstats_summary": {
                "p50_disk_per_blk_ms": 6.0,  # +20% WARN
                "p50_connect_cs_per_blk_ms": 3.0,
                "p50_flush_lmdb_per_blk_ms": 1.0,
                "implied_blk_per_s": 2.0,  # -20% WARN
            },
        }
        rows = self.mod.compare_baselines(BASELINE_DOC, BASELINE_DOC["medians_ns"], current_segment)
        by_label = {r.label: r for r in rows}
        self.assertEqual(by_label["segment.blocks_per_hr"].mark, "OK")
        self.assertEqual(by_label["segment.p50_load_ms"].mark, "WARN")
        self.assertEqual(by_label["benchstats.p50_disk_per_blk_ms"].mark, "WARN")
        self.assertEqual(by_label["benchstats.implied_blk_per_s"].mark, "WARN")
        self.assertEqual(self.mod.compare_exit_code(rows), 1)

    def test_find_latest_baseline_ordering(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            older = root / "older.json"
            newer = root / "newer.json"
            older.write_text("{}")
            newer.write_text("{}")
            now = time.time()
            os.utime(older, (now - 100, now - 100))
            os.utime(newer, (now, now))
            latest = self.mod.find_latest_baseline(root)
            self.assertEqual(latest, newer)
            listed = self.mod.list_baselines(root)
            self.assertEqual(listed[0], newer)
            self.assertEqual(listed[1], older)

    def test_format_compare_report(self):
        current = {
            "DecompressBlockPayloadSerial": 1060.0,
            "DecompressBlockPayloadParallel": 800.0,
            "ReadRawBlockCompressed": 500.0,
        }
        rows = self.mod.compare_baselines(BASELINE_DOC, current, {"blocks_per_hr": 1000.0})
        report = self.mod.format_compare_report(rows)
        self.assertIn("OK DecompressBlockPayloadParallel:", report)
        self.assertIn("segment_replay:", report)
        self.assertIn("segment.blocks_per_hr:", report)

    def test_write_and_load_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "baseline.json"
            doc = self.mod.capture_from_bench_json(
                self._write_bench_json(),
                "h",
                "s",
                "t",
                None,
            )
            self.mod.write_baseline_doc(doc, out)
            loaded = self.mod.load_baseline_doc(out)
            self.assertEqual(loaded["host"], "h")
            self.assertEqual(loaded["medians_ns"], doc["medians_ns"])

    def test_format_baseline_summary(self):
        summary = self.mod.format_baseline_summary(BASELINE_DOC, "/tmp/base.json")
        self.assertIn("baseline\t/tmp/base.json", summary)
        self.assertIn("segment_replay\tyes", summary)
        self.assertIn("benchstats\tp50_disk_per_blk_ms\t5.0", summary)

    def test_warn_only_exit_one(self):
        baseline = {k: v for k, v in BASELINE_DOC.items() if k != "segment_replay"}
        current = {
            "DecompressBlockPayloadSerial": 1000.0,
            "DecompressBlockPayloadParallel": 800.0,
            "ReadRawBlockCompressed": 440.0,  # -12% WARN, no FAIL
        }
        rows = self.mod.compare_baselines(baseline, current, {})
        self.assertEqual(self.mod.compare_exit_code(rows), 1)
        self.assertFalse(any(r.mark == "FAIL" for r in rows))

    def test_bench_boundary_five_and_ten_percent(self):
        baseline = {
            "medians_ns": {
                "BenchA": 1000.0,
                "BenchB": 500.0,
            }
        }
        current = {
            "BenchA": 1050.0,  # exactly +5% -> OK (FAIL requires delta > 5%)
            "BenchB": 450.0,  # exactly -10% -> OK (WARN requires |delta| > 10%)
        }
        rows = self.mod.compare_baselines(baseline, current, {})
        by_label = {r.label: r for r in rows}
        self.assertEqual(by_label["BenchA"].mark, "OK")
        self.assertEqual(by_label["BenchB"].mark, "OK")
        self.assertEqual(self.mod.compare_exit_code(rows), 0)

    def test_missing_segment_field_warn_exit_one(self):
        baseline = {
            "medians_ns": {"BenchA": 1000.0},
            "segment_replay": {
                "blocks_per_hr": 1000.0,
                "p50_load_ms": 10.0,
                "p50_connect_ms": 20.0,
            },
        }
        current_segment = {"blocks_per_hr": 1000.0, "p50_connect_ms": 20.0}
        rows = self.mod.compare_baselines(baseline, {"BenchA": 1000.0}, current_segment)
        by_label = {r.label: r for r in rows}
        self.assertEqual(by_label["segment.p50_load_ms"].mark, "WARN")
        self.assertIsNone(by_label["segment.p50_load_ms"].current)
        self.assertEqual(self.mod.compare_exit_code(rows), 1)

    def test_base_ns_zero_skipped_silently(self):
        baseline = {"medians_ns": {"ZeroBench": 0.0, "RealBench": 1000.0}}
        rows = self.mod.compare_baselines(baseline, {"ZeroBench": 500.0, "RealBench": 1000.0}, {})
        labels = [r.label for r in rows]
        self.assertNotIn("ZeroBench", labels)
        self.assertIn("RealBench", labels)

    def test_report_compare_with_fixtures(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            baseline_path = tmp_path / "base.json"
            bench_path = tmp_path / "bench.json"
            segment_path = tmp_path / "segment.json"
            baseline_path.write_text(json.dumps(BASELINE_DOC))
            bench_path.write_text(json.dumps(SAMPLE_BENCH_JSON))
            segment_path.write_text(
                json.dumps(
                    {
                        "blocks_per_hr": 1000.0,
                        "p50_load_ms": 10.0,
                        "p50_connect_ms": 20.0,
                        "benchstats_summary": BASELINE_DOC["segment_replay"]["benchstats_summary"],
                    }
                )
            )

            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = self.report.main(
                    [
                        "--baselines-dir",
                        str(tmp_path),
                        "compare",
                        str(baseline_path),
                        "--bench-json",
                        str(bench_path),
                        "--segment-json",
                        str(segment_path),
                    ]
                )
            self.assertEqual(rc, 0)
            self.assertIn("OK DecompressBlockPayloadSerial:", buf.getvalue())

    def test_report_capture_with_fixtures(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            bench_path = tmp_path / "bench.json"
            segment_path = tmp_path / "segment.json"
            bench_path.write_text(json.dumps(SAMPLE_BENCH_JSON))
            segment_path.write_text(json.dumps({"blocks_per_hr": 900.0}))

            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = self.report.main(
                    [
                        "--baselines-dir",
                        str(tmp_path),
                        "capture",
                        "--bench-json",
                        str(bench_path),
                        "--segment-json",
                        str(segment_path),
                        "--host",
                        "fixturehost",
                        "--git-sha",
                        "deadbeef",
                        "--captured-at",
                        "20260706T120000Z",
                    ]
                )
            self.assertEqual(rc, 0)
            out_path = Path(buf.getvalue().strip())
            self.assertTrue(out_path.is_file())
            doc = self.mod.load_baseline_doc(out_path)
            self.assertEqual(doc["host"], "fixturehost")
            self.assertEqual(doc["git_sha"], "deadbeef")
            self.assertEqual(len(doc["medians_ns"]), 3)

    def test_report_list_and_show_with_fixtures(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            older = tmp_path / "older-host-sha1-20260101T000000Z.json"
            newer = tmp_path / "newer-host-sha2-20260201T000000Z.json"
            older.write_text(json.dumps({"host": "older", "git_sha": "sha1", "captured_at": "20260101T000000Z", "medians_ns": {}}))
            newer.write_text(json.dumps(BASELINE_DOC))
            now = time.time()
            os.utime(older, (now - 100, now - 100))
            os.utime(newer, (now, now))

            list_buf = io.StringIO()
            with redirect_stdout(list_buf):
                rc = self.report.main(["--baselines-dir", str(tmp_path), "list"])
            self.assertEqual(rc, 0)
            lines = list_buf.getvalue().strip().splitlines()
            self.assertEqual(len(lines), 2)
            self.assertIn("newer", lines[0])
            self.assertIn("older", lines[1])

            show_buf = io.StringIO()
            with redirect_stdout(show_buf):
                rc = self.report.main(["show", str(newer)])
            self.assertEqual(rc, 0)
            self.assertIn("host\ttesthost", show_buf.getvalue())
            self.assertIn("segment_replay\tyes", show_buf.getvalue())

    def _write_bench_json(self) -> str:
        tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False)
        with tmp:
            json.dump(SAMPLE_BENCH_JSON, tmp)
        return tmp.name


if __name__ == "__main__":
    unittest.main()