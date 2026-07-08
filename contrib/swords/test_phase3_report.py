#!/usr/bin/env python3
"""Unit tests for contrib/swords/phase3_report.py."""
from __future__ import annotations

import importlib.util
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from datetime import datetime, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PHASE3_PATH = ROOT / "contrib" / "swords" / "phase3_report.py"
BENCHSTATS_PARSE_PATH = ROOT / "contrib" / "swords" / "benchstats_parse.py"
PARSE_BENCHSTATS_PATH = ROOT / "contrib" / "swords" / "parse-benchstats.py"


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


RUN_STARTED = (
    "2026-07-06T16:00:00Z === Swords run started 2026-07-06T16:00:00Z "
    "(datadir=/tmp/.bitcoin-swords benchstats=1) ==="
)

LMDB_PARALLELISM = (
    "2026-07-06T16:00:01Z Swords LMDB parallelism: benchstats=1 par=8 "
    "coinprefetchpar=4 utxoencodepar=2 blockdecompresspar=2 flushsnapshot=0 "
    "blockindexsync=0 txindexbatch=1000 chainstate_maxreaders=126"
)


def benchstats_line(ts: str, disk_total: float = 5000.0) -> str:
    return (
        f"{ts} benchstats: block disk={disk_total:.1f}ms decompress=0.0ms par_jobs=0 "
        "prefetch_hit=0 prefetch_wait=0.0ms | coin prevouts=0 miss=0 lmdb=0.0ms "
        "decode=0.0ms warm=0.0ms skip_thr=0 skip_wrk=0 readers_full=0 | "
        "flush chainstate=0.0ms block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 "
        "parallel=0 txindex=0.0ms blkidx_sync=0 connect_cs=0.0ms | "
        "wait cs_main_hold=0.0ms flush_mutex_wait=0.0ms blkidx_mutex_wait=0.0ms "
        "abc_idle=0.0ms script_jobs=0 script_done=0 | blocks=1000"
    )


def series_lines(rollup_count: int = 3) -> list[str]:
    start = datetime(2026, 7, 6, 16, 0, 0)
    lines = [RUN_STARTED, LMDB_PARALLELISM]
    for i in range(1, rollup_count + 1):
        ts = start + timedelta(seconds=100 * i)
        lines.append(benchstats_line(ts.strftime("%Y-%m-%dT%H:%M:%SZ")))
    return lines


@unittest.skipUnless(shutil.which("just"), "just not installed")
class ResetDatadirIntegrationTests(unittest.TestCase):
    def test_reset_datadir_archives_benchstats(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            datadir = root / "datadir"
            archive_root = root / "archives"
            datadir.mkdir()
            (datadir / "debug.log").write_text("\n".join(series_lines(2)) + "\n")

            env = {
                **os.environ,
                "SWORDS_DATADIR": str(datadir),
                "SWORDS_LOG_ARCHIVE": str(archive_root),
                "HOME": str(root / "home"),
            }
            result = subprocess.run(
                ["just", "reset-datadir"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            self.assertFalse((datadir / "debug.log").exists())
            latest_txt = archive_root / "LATEST.txt"
            self.assertTrue(latest_txt.is_file())
            dest = Path(latest_txt.read_text().strip())
            self.assertTrue((dest / "debug.log").is_file())
            self.assertTrue((dest / "benchstats.json").is_file())
            self.assertTrue((dest / "benchstats.csv").is_file())

            meta = (dest / "meta.txt").read_text()
            self.assertIn("benchstats_json=", meta)
            self.assertIn("benchstats_csv=", meta)
            self.assertIn("convenience caches", result.stdout)
            self.assertEqual(oct(archive_root.stat().st_mode & 0o777), "0o700")
            self.assertEqual(oct(dest.stat().st_mode & 0o777), "0o700")

            bench_json = json.loads((dest / "benchstats.json").read_text())
            self.assertEqual(
                bench_json["series"][1]["interval_implied_blk_per_s"],
                10.0,
            )
            csv_header = (dest / "benchstats.csv").read_text().splitlines()[0]
            self.assertIn("interval_implied_blk_per_s", csv_header)

            latest_link = archive_root / "latest"
            self.assertTrue(latest_link.is_symlink())

    def test_reset_datadir_archives_sweep_variant_when_env_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            datadir = root / "datadir"
            archive_root = root / "archives"
            datadir.mkdir()
            (datadir / "debug.log").write_text("\n".join(series_lines(1)) + "\n")

            env = {
                **os.environ,
                "SWORDS_DATADIR": str(datadir),
                "SWORDS_LOG_ARCHIVE": str(archive_root),
                "SWORDS_SWEEP_VARIANT": "baseline",
                "HOME": str(root / "home"),
            }
            result = subprocess.run(
                ["just", "reset-datadir"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            dest = Path((archive_root / "LATEST.txt").read_text().strip())
            meta = (dest / "meta.txt").read_text()
            self.assertIn("sweep_variant=baseline", meta)

    def test_reset_datadir_omits_sweep_variant_when_env_unset(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            datadir = root / "datadir"
            archive_root = root / "archives"
            datadir.mkdir()
            (datadir / "debug.log").write_text("\n".join(series_lines(1)) + "\n")

            env = {
                **os.environ,
                "SWORDS_DATADIR": str(datadir),
                "SWORDS_LOG_ARCHIVE": str(archive_root),
                "HOME": str(root / "home"),
            }
            env.pop("SWORDS_SWEEP_VARIANT", None)
            result = subprocess.run(
                ["just", "reset-datadir"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            dest = Path((archive_root / "LATEST.txt").read_text().strip())
            meta = (dest / "meta.txt").read_text()
            self.assertNotIn("sweep_variant=", meta)

    def test_reset_datadir_chmods_archive_root_without_debug_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            datadir = root / "datadir"
            archive_root = root / "archives"
            datadir.mkdir()

            env = {
                **os.environ,
                "SWORDS_DATADIR": str(datadir),
                "SWORDS_LOG_ARCHIVE": str(archive_root),
                "HOME": str(root / "home"),
            }
            result = subprocess.run(
                ["just", "reset-datadir"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(archive_root.is_dir())
            self.assertEqual(oct(archive_root.stat().st_mode & 0o777), "0o700")
            self.assertFalse((archive_root / "LATEST.txt").exists())

    def test_reset_datadir_chmod_failure_still_exits_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            datadir = root / "datadir"
            archive_root = root / "archives"
            fake_bin = root / "bin"
            fake_bin.mkdir()
            fake_chmod = fake_bin / "chmod"
            fake_chmod.write_text("#!/bin/sh\nexit 1\n")
            fake_chmod.chmod(0o755)

            datadir.mkdir()
            (datadir / "debug.log").write_text("\n".join(series_lines(1)) + "\n")

            env = {
                **os.environ,
                "SWORDS_DATADIR": str(datadir),
                "SWORDS_LOG_ARCHIVE": str(archive_root),
                "HOME": str(root / "home"),
                "PATH": f"{fake_bin}:{os.environ['PATH']}",
            }
            result = subprocess.run(
                ["just", "reset-datadir"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((archive_root / "LATEST.txt").is_file())

    def test_sweep_finish_stamps_variant_and_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            datadir = root / "datadir"
            archive_root = root / "archives"
            datadir.mkdir()
            (datadir / "debug.log").write_text("\n".join(series_lines(1)) + "\n")

            env = {
                **os.environ,
                "SWORDS_DATADIR": str(datadir),
                "SWORDS_LOG_ARCHIVE": str(archive_root),
                "HOME": str(root / "home"),
            }
            result = subprocess.run(
                ["just", "sweep-finish", "baseline"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

            dest = Path((archive_root / "LATEST.txt").read_text().strip())
            meta = (dest / "meta.txt").read_text()
            self.assertIn("sweep_variant=baseline", meta)

            phase6 = load_module(ROOT / "contrib" / "swords" / "phase6_sweep.py", "phase6_finish")
            state = phase6.load_sweep_state(archive_root)
            self.assertIn("baseline", state.get("variants", {}))
            self.assertEqual(
                state["variants"]["baseline"]["archive_path"],
                str(dest.resolve()),
            )


class Phase3ReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module(PHASE3_PATH, "phase3_report")

    def _write_log(self, lines: list[str]) -> Path:
        tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False)
        with tmp:
            tmp.write("\n".join(lines))
            tmp.write("\n")
        return Path(tmp.name)

    def _series_log(self, rollup_count: int = 3) -> Path:
        return self._write_log(series_lines(rollup_count))

    def test_build_report_json_shape(self):
        log = self._series_log(3)
        report = self.mod.build_report(log, (1, 2), archive=None)
        self.assertEqual(report["log"], str(log))
        self.assertIn("run_started", report["run_metadata"])
        self.assertIn("lmdb_parallelism", report["run_metadata"])
        self.assertEqual(report["summary"]["rollup_count"], 3)
        self.assertEqual(len(report["milestones"]), 2)
        self.assertEqual(len(report["series"]), 3)
        self.assertIsNone(report["archive_compare"])
        self.assertEqual(
            report["pointers"],
            [
                "just phase0-check",
                "just watch-cpu",
                "just profile-ibd",
                "just phase0-snapshot",
            ],
        )
        self.assertEqual(report["series"][1]["interval_implied_blk_per_s"], 10.0)
        self.assertEqual(report["milestones"][0]["implied_blk_per_s"], 10.0)

    def test_build_report_archive_compare(self):
        current_log = self._series_log(2)
        archive_log = self._write_log([
            RUN_STARTED,
            benchstats_line("2026-07-06T16:01:40Z", disk_total=20000.0),
        ])
        report = self.mod.build_report(
            current_log,
            (1,),
            archive=None,
            archive_log_path=str(archive_log),
        )
        self.assertIsNotNone(report["archive_compare"])
        assert report["archive_compare"] is not None
        self.assertEqual(report["archive_compare"]["archive_log"], str(archive_log))
        row = report["archive_compare"]["milestones"][0]
        self.assertEqual(row["rollup_index"], 1)
        self.assertIsNotNone(row["disk_delta_pct"])

    def test_build_report_archive_none_no_warning(self):
        log = self._series_log(1)
        warnings: list[str] = []
        report = self.mod.build_report(log, (1,), archive=None, warnings=warnings)
        self.assertIsNone(report["archive_compare"])
        self.assertEqual(warnings, [])

    def test_build_report_same_log_skip_warns(self):
        log = self._series_log(1)
        warnings: list[str] = []
        report = self.mod.build_report(
            log,
            (1,),
            archive=None,
            archive_log_path=str(log),
            warnings=warnings,
        )
        self.assertIsNone(report["archive_compare"])
        self.assertTrue(any("same as current log" in w for w in warnings))

    def test_build_report_invalid_archive_log_warns(self):
        log = self._series_log(1)
        warnings: list[str] = []
        report = self.mod.build_report(
            log,
            (1,),
            archive=None,
            archive_log_path="/nonexistent/debug.log",
            warnings=warnings,
        )
        self.assertIsNone(report["archive_compare"])
        self.assertTrue(any("archive log not found" in w for w in warnings))

    def test_build_report_missing_archive_warns(self):
        log = self._series_log(1)
        with tempfile.TemporaryDirectory() as tmp:
            warnings: list[str] = []
            report = self.mod.build_report(
                log,
                (1,),
                archive="latest",
                archive_root=Path(tmp),
                warnings=warnings,
            )
        self.assertIsNone(report["archive_compare"])
        self.assertTrue(warnings)

    def test_human_report_contains_key_lines(self):
        log = self._series_log(2)
        report = self.mod.build_report(log, (1,), archive=None)
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.mod.print_human_report(report)
        text = buf.getvalue()
        self.assertIn("benchstats_rollups", text)
        self.assertIn("summary_implied_blk_per_s", text)
        self.assertIn("milestone\t#1", text)
        self.assertIn("phase3_pointers", text)
        self.assertIn("just phase0-check", text)
        self.assertIn("just watch-cpu", text)
        self.assertIn("summary_prefetch_health", text)
        self.assertIn("readers_full", text)
        self.assertIn("prefetch_hit", text)

    def test_cli_json_output(self):
        log = self._series_log(2)
        out = subprocess.run(
            [
                "python3",
                str(PHASE3_PATH),
                "--log",
                str(log),
                "--json",
                "--archive",
                "none",
                "--milestones",
                "1",
            ],
            check=True,
            capture_output=True,
            text=True,
            cwd=str(ROOT),
        )
        payload = json.loads(out.stdout)
        self.assertIn("summary", payload)
        self.assertIn("series", payload)
        self.assertIn("pointers", payload)
        self.assertIn("implied_blk_per_s", payload["milestones"][0])
        self.assertIn("interval_implied_blk_per_s", payload["series"][1])

    def test_cli_archive_none_no_stderr_warning(self):
        log = self._series_log(1)
        out = subprocess.run(
            ["python3", str(PHASE3_PATH), "--log", str(log), "--archive", "none"],
            check=True,
            capture_output=True,
            text=True,
            cwd=str(ROOT),
        )
        self.assertEqual(out.stderr, "")
        self.assertIn("benchstats_rollups", out.stdout)

    def test_cli_invalid_archive_log_warns_stderr(self):
        log = self._series_log(1)
        out = subprocess.run(
            [
                "python3",
                str(PHASE3_PATH),
                "--log",
                str(log),
                "--archive-log",
                "/nonexistent/debug.log",
            ],
            check=True,
            capture_output=True,
            text=True,
            cwd=str(ROOT),
        )
        self.assertIn("archive log not found", out.stderr)

    @unittest.skipUnless(shutil.which("just"), "just not installed")
    def test_export_benchstats_subprocess(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            datadir = root / "datadir"
            out_dir = root / "export"
            datadir.mkdir()
            (datadir / "debug.log").write_text("\n".join(series_lines(2)) + "\n")

            env = {**os.environ, "SWORDS_DATADIR": str(datadir), "HOME": str(root / "home")}
            result = subprocess.run(
                ["just", "export-benchstats", str(out_dir)],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((out_dir / "benchstats.json").is_file())
            self.assertTrue((out_dir / "benchstats.csv").is_file())
            bench_json = json.loads((out_dir / "benchstats.json").read_text())
            self.assertEqual(bench_json["summary"]["rollup_count"], 2)


if __name__ == "__main__":
    unittest.main()