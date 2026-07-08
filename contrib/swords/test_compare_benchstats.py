#!/usr/bin/env python3
"""Unit tests for contrib/swords/compare-benchstats.py and archive resolution."""
from __future__ import annotations

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCHSTATS_PARSE_PATH = ROOT / "contrib" / "swords" / "benchstats_parse.py"
COMPARE_PATH = ROOT / "contrib" / "swords" / "compare-benchstats.py"


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


def benchstats_line(ts: str, disk_total: float, wall_offset_s: int = 0) -> str:
    return (
        f"{ts} benchstats: block disk={disk_total:.1f}ms decompress=0.0ms par_jobs=0 "
        "prefetch_hit=0 prefetch_wait=0.0ms | coin prevouts=0 miss=0 lmdb=0.0ms "
        "decode=0.0ms warm=0.0ms skip_thr=0 skip_wrk=0 readers_full=0 | "
        "flush chainstate=0.0ms block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 "
        "parallel=0 txindex=0.0ms blkidx_sync=0 connect_cs=0.0ms | "
        "wait cs_main_hold=0.0ms flush_mutex_wait=0.0ms blkidx_mutex_wait=0.0ms "
        "abc_idle=0.0ms script_jobs=0 script_done=0 | blocks=1000"
    )


class CompareBenchstatsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bp = load_module(BENCHSTATS_PARSE_PATH, "benchstats_parse")
        cls.compare = load_module(COMPARE_PATH, "compare_benchstats")

    def _write_log(self, lines: list[str]) -> Path:
        tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False)
        with tmp:
            tmp.write("\n".join(lines))
            tmp.write("\n")
        return Path(tmp.name)

    def _archive_root(self) -> tempfile.TemporaryDirectory[str]:
        return tempfile.TemporaryDirectory()

    def test_pct_delta(self):
        self.assertEqual(self.compare.pct_delta(11.0, 10.0), 10.0)
        self.assertIsNone(self.compare.pct_delta(None, 10.0))
        self.assertIsNone(self.compare.pct_delta(10.0, 0.0))

    def test_resolve_archive_log_symlink_latest(self):
        with self._archive_root() as tmp:
            root = Path(tmp)
            stamp_dir = root / "2026-07-06T16-00Z"
            stamp_dir.mkdir()
            log = stamp_dir / "debug.log"
            log.write_text("benchstats stub\n", encoding="utf-8")
            (root / "latest").symlink_to(stamp_dir.name)
            resolved = self.bp.resolve_archive_log("latest", archive_root=root)
            self.assertEqual(resolved, log)

    def test_resolve_archive_log_latest_txt_fallback(self):
        with self._archive_root() as tmp:
            root = Path(tmp)
            stamp_dir = root / "2026-07-06T17-00Z"
            stamp_dir.mkdir()
            log = stamp_dir / "debug.log"
            log.write_text("benchstats stub\n", encoding="utf-8")
            (root / "LATEST.txt").write_text(str(stamp_dir) + "\n", encoding="utf-8")
            resolved = self.bp.resolve_archive_log("latest", archive_root=root)
            self.assertEqual(resolved, log)

    def test_resolve_archive_log_direct_file(self):
        with self._archive_root() as tmp:
            root = Path(tmp)
            log = root / "standalone-debug.log"
            log.write_text("benchstats stub\n", encoding="utf-8")
            resolved = self.bp.resolve_archive_log(str(log), archive_root=root)
            self.assertEqual(resolved, log)

    def test_milestone_compare_rows_delta(self):
        current_log = self._write_log([
            RUN_STARTED,
            benchstats_line("2026-07-06T16:04:32Z", 10100.0),
        ])
        archive_log = self._write_log([
            RUN_STARTED,
            benchstats_line("2026-07-06T16:04:32Z", 20200.0),
        ])
        current_series = self.bp.parse_benchstats_series(current_log)
        archive_series = self.bp.parse_benchstats_series(archive_log)
        rows = self.compare.milestone_compare_rows(current_series, archive_series, (1,))
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(row["rollup_index"], 1)
        self.assertEqual(row["current_disk_per_blk_ms"], 10.1)
        self.assertEqual(row["archive_disk_per_blk_ms"], 20.2)
        self.assertEqual(row["disk_delta_pct"], -50.0)

    def test_milestone_compare_includes_readers_full(self):
        current_log = self._write_log([
            RUN_STARTED,
            (
                "2026-07-06T16:04:32Z benchstats: block disk=10100.0ms decompress=0.0ms par_jobs=0 "
                "prefetch_hit=10 prefetch_wait=0.0ms | coin prevouts=0 miss=0 lmdb=0.0ms "
                "decode=0.0ms warm=0.0ms skip_thr=0 skip_wrk=0 readers_full=1 | "
                "flush chainstate=0.0ms block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 "
                "parallel=0 txindex=0.0ms blkidx_sync=0 connect_cs=0.0ms | "
                "wait cs_main_hold=0.0ms flush_mutex_wait=0.0ms blkidx_mutex_wait=0.0ms "
                "abc_idle=0.0ms script_jobs=0 script_done=0 | blocks=1000"
            ),
        ])
        archive_log = self._write_log([
            RUN_STARTED,
            benchstats_line("2026-07-06T16:04:32Z", 20200.0),
        ])
        current_series = self.bp.parse_benchstats_series(current_log)
        archive_series = self.bp.parse_benchstats_series(archive_log)
        rows = self.compare.milestone_compare_rows(current_series, archive_series, (1,))
        self.assertEqual(rows[0]["current_readers_full"], 1)
        self.assertEqual(rows[0]["current_prefetch_hit"], 10)


if __name__ == "__main__":
    unittest.main()