#!/usr/bin/env python3
"""Unit tests for contrib/swords/benchstats_parse.py."""
from __future__ import annotations

import csv
import importlib.util
import io
import tempfile
import unittest
from datetime import datetime, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "contrib" / "swords" / "benchstats_parse.py"
PARSE_BENCHSTATS_PATH = ROOT / "contrib" / "swords" / "parse-benchstats.py"


def load_module(path: Path = MODULE_PATH, name: str = "benchstats_parse"):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


NEW_BENCHSTATS_LINE = (
    "2026-07-06T16:47:35Z benchstats: block disk=5.5ms decompress=0.0ms par_jobs=0 "
    "prefetch_hit=0 prefetch_wait=0.0ms | coin prevouts=32 miss=32 lmdb=0.0ms "
    "decode=0.0ms warm=0.0ms skip_thr=999 skip_wrk=0 readers_full=0 | "
    "flush chainstate=0.0ms block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 "
    "parallel=0 txindex=2.0ms blkidx_sync=0 connect_cs=7.4ms | "
    "wait cs_main_hold=120.5ms flush_mutex_wait=3.2ms blkidx_mutex_wait=1.1ms "
    "abc_idle=50.0ms script_jobs=8000 script_done=8000 | blocks=1000"
)

WAIT_ZERO = (
    "wait cs_main_hold=0.0ms flush_mutex_wait=0.0ms blkidx_mutex_wait=0.0ms "
    "abc_idle=0.0ms script_jobs=0 script_done=0 | "
)

OLD_BENCHSTATS_LINE = (
    "2026-07-06T10:00:00Z benchstats: block disk=12.0ms decompress=1.0ms par_jobs=1 "
    "prefetch_hit=2 prefetch_wait=0.5ms | coin prevouts=10 miss=5 lmdb=1.0ms "
    "decode=0.5ms warm=0.2ms skip_thr=0 skip_wrk=0 readers_full=0 | "
    "flush chainstate=3.0ms block_index=1.0ms encode=0.5ms lmdb=4.0ms stale=0 "
    "parallel=0 txindex=0.0ms blkidx_sync=1 | "
    f"{WAIT_ZERO}blocks=1000"
)

OLD_MINIMAL_BENCHSTATS_LINE = (
    "2026-07-06T10:00:00Z benchstats: block disk=12.0ms decompress=1.0ms par_jobs=1 "
    "prefetch_hit=2 prefetch_wait=0.5ms | coin prevouts=10 miss=5 lmdb=1.0ms "
    "decode=0.5ms warm=0.2ms skip_thr=0 skip_wrk=0 readers_full=0 | "
    "flush chainstate=3.0ms block_index=1.0ms encode=0.5ms lmdb=4.0ms stale=0 "
    f"txindex=0.0ms blkidx_sync=1 | {WAIT_ZERO}blocks=1000"
)

RUN_STARTED = (
    "2026-07-06T16:00:00Z === Swords run started 2026-07-06T16:00:00Z "
    "(datadir=/home/user/.bitcoin-swords benchstats=1) ==="
)

LMDB_PARALLELISM = (
    "2026-07-06T16:00:01Z Swords LMDB parallelism: benchstats=1 par=8 "
    "coinprefetchpar=4 utxoencodepar=2 blockdecompresspar=2 flushsnapshot=0 "
    "blockindexsync=0 txindexbatch=1000 chainstate_maxreaders=126"
)


class BenchstatsParseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()

    def _write_log(self, lines: list[str]) -> Path:
        tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False)
        with tmp:
            tmp.write("\n".join(lines))
            tmp.write("\n")
        return Path(tmp.name)

    def test_parse_benchstats_line_new_format(self):
        parsed = self.mod.parse_benchstats_line(NEW_BENCHSTATS_LINE)
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed["disk"], 5.5)
        self.assertEqual(parsed["connect_cs"], 7.4)
        self.assertEqual(parsed["flush_lmdb"], 0.0)
        self.assertEqual(parsed["txindex"], 2.0)
        self.assertEqual(parsed["blocks"], 1000)
        self.assertEqual(parsed["coin_lmdb"], 0.0)
        self.assertEqual(parsed["coin_prevouts"], 32)
        self.assertEqual(parsed["coin_miss"], 32)
        self.assertEqual(parsed["cs_main_hold"], 120.5)
        self.assertEqual(parsed["flush_mutex_wait"], 3.2)
        self.assertEqual(parsed["blkidx_mutex_wait"], 1.1)
        self.assertEqual(parsed["abc_idle"], 50.0)
        self.assertEqual(parsed["script_jobs"], 8000)
        self.assertEqual(parsed["script_done"], 8000)
        self.assertNotIn("prevouts", parsed)
        self.assertNotIn("miss", parsed)

    def test_parse_benchstats_line_old_format_missing_connect_cs(self):
        parsed = self.mod.parse_benchstats_line(OLD_BENCHSTATS_LINE)
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed["disk"], 12.0)
        self.assertEqual(parsed["flush_lmdb"], 4.0)
        self.assertEqual(parsed.get("connect_cs", 0.0), 0.0)
        self.assertEqual(parsed.get("abc_idle", 0.0), 0.0)
        self.assertEqual(parsed.get("script_jobs", 0), 0)
        self.assertNotIn("connect_cs", OLD_BENCHSTATS_LINE)

    def test_parse_benchstats_line_old_format_missing_parallel(self):
        parsed = self.mod.parse_benchstats_line(OLD_MINIMAL_BENCHSTATS_LINE)
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed.get("connect_cs", 0.0), 0.0)
        self.assertEqual(parsed.get("parallel", 0), 0)
        self.assertNotIn("connect_cs", OLD_MINIMAL_BENCHSTATS_LINE)
        self.assertNotIn("parallel", OLD_MINIMAL_BENCHSTATS_LINE)

    def test_parse_run_metadata(self):
        log = self._write_log([RUN_STARTED, LMDB_PARALLELISM, NEW_BENCHSTATS_LINE])
        meta = self.mod.parse_run_metadata(log)
        self.assertIn("run_started", meta)
        self.assertEqual(meta["run_started"]["benchstats"], 1)
        self.assertEqual(meta["run_started"]["datadir"], "/home/user/.bitcoin-swords")
        self.assertIn("lmdb_parallelism", meta)
        self.assertEqual(meta["lmdb_parallelism"]["par"], 8)
        self.assertEqual(meta["lmdb_parallelism"]["chainstate_maxreaders"], 126)

    def test_parse_benchstats_series_rollup_index_and_wall_s(self):
        start = datetime(2026, 7, 6, 16, 0, 0)
        lines = [RUN_STARTED]
        for i in range(1, 4):
            ts = start + timedelta(seconds=100 * i)
            lines.append(
                f"{ts.strftime('%Y-%m-%dT%H:%M:%SZ')} benchstats: block disk={i}.0ms "
                "decompress=0.0ms par_jobs=0 prefetch_hit=0 prefetch_wait=0.0ms | "
                "coin prevouts=0 miss=0 lmdb=0.0ms decode=0.0ms warm=0.0ms "
                "skip_thr=0 skip_wrk=0 readers_full=0 | flush chainstate=0.0ms "
                "block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 parallel=0 "
                f"txindex=0.0ms blkidx_sync=0 connect_cs=1.0ms | {WAIT_ZERO}blocks=1000"
            )
        log = self._write_log(lines)
        series = self.mod.parse_benchstats_series(log)
        self.assertEqual(len(series), 3)
        self.assertEqual(series[0]["rollup_index"], 1)
        self.assertEqual(series[2]["rollup_index"], 3)
        self.assertEqual(series[0]["wall_s"], 100.0)
        self.assertNotIn("unaccounted_ms", series[0])
        self.assertNotIn("interval_wall_ms", series[0])
        self.assertEqual(series[1]["interval_wall_ms"], 100000.0)
        self.assertIn("unaccounted_ms", series[1])
        self.assertNotIn("interval_implied_blk_per_s", series[0])
        self.assertEqual(series[1]["interval_implied_blk_per_s"], 10.0)
        self.assertEqual(series[2]["interval_implied_blk_per_s"], 10.0)

    def test_interval_implied_blk_per_s_helper(self):
        rollup = {
            "rollup_index": 2,
            "blocks": 1000,
            "interval_wall_ms": 50000.0,
        }
        self.assertEqual(self.mod.interval_implied_blk_per_s(rollup), 20.0)
        self.assertIsNone(self.mod.interval_implied_blk_per_s({"rollup_index": 1, "blocks": 1000}))

    def test_unaccounted_ms_interval_gap(self):
        rollup = {
            "rollup_index": 2,
            "disk": 100.0,
            "decompress": 10.0,
            "prefetch_wait": 5.0,
            "coin_lmdb": 1.0,
            "coin_decode": 1.0,
            "coin_warm": 1.0,
            "chainstate_flush": 2.0,
            "block_index_flush": 2.0,
            "encode": 2.0,
            "flush_lmdb": 3.0,
            "txindex": 4.0,
            "connect_cs": 50.0,
            "flush_mutex_wait": 2.0,
            "blkidx_mutex_wait": 1.0,
            "abc_idle": 3.0,
            "interval_wall_ms": 200.0,
        }
        gap = self.mod.unaccounted_ms(rollup)
        self.assertIsNotNone(gap)
        # Mutex waits (3.0ms) excluded from _accounted_ms though present in rollup.
        self.assertAlmostEqual(gap, 16.0)

    def test_unaccounted_ms_excludes_mutex_waits_overlap(self):
        """Mutex waits embedded in flush totals must not lower unaccounted_ms."""
        rollup = {
            "rollup_index": 2,
            "chainstate_flush": 50.0,
            "block_index_flush": 30.0,
            "flush_mutex_wait": 20.0,
            "blkidx_mutex_wait": 10.0,
            "interval_wall_ms": 100.0,
        }
        gap = self.mod.unaccounted_ms(rollup)
        self.assertIsNotNone(gap)
        self.assertAlmostEqual(gap, 20.0)

    def test_rollup_milestones_reference_values(self):
        """Synthetic log reproducing archived milestone disk ms/blk labels."""
        start = datetime(2026, 7, 6, 16, 0, 0)
        milestone_specs = {
            110: (272, 10.1),
            200: (770, 37.0),
            250: (1569, 55.8),
        }
        lines = [RUN_STARTED]
        for idx in range(1, 251):
            if idx in milestone_specs:
                wall_s, disk_per_blk = milestone_specs[idx]
                disk_total = disk_per_blk * 1000
            else:
                wall_s = idx * 10
                disk_total = 5.0 * 1000
            ts = start + timedelta(seconds=wall_s)
            lines.append(
                f"{ts.strftime('%Y-%m-%dT%H:%M:%SZ')} benchstats: block disk={disk_total:.1f}ms "
                "decompress=0.0ms par_jobs=0 prefetch_hit=0 prefetch_wait=0.0ms | "
                "coin prevouts=0 miss=0 lmdb=0.0ms decode=0.0ms warm=0.0ms "
                "skip_thr=0 skip_wrk=0 readers_full=0 | flush chainstate=0.0ms "
                "block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 parallel=0 "
                f"txindex=0.0ms blkidx_sync=0 connect_cs=0.0ms | {WAIT_ZERO}blocks=1000"
            )
        series = self.mod.parse_benchstats_series(lines)
        milestones = self.mod.rollup_milestones(series, (60, 110, 120, 200, 250))
        by_idx = {m["rollup_index"]: m for m in milestones}
        self.assertAlmostEqual(by_idx[110]["wall_s"], 272.0)
        self.assertAlmostEqual(by_idx[110]["disk_per_blk_ms"], 10.1)
        self.assertAlmostEqual(by_idx[200]["wall_s"], 770.0)
        self.assertAlmostEqual(by_idx[200]["disk_per_blk_ms"], 37.0)
        self.assertAlmostEqual(by_idx[250]["wall_s"], 1569.0)
        self.assertAlmostEqual(by_idx[250]["disk_per_blk_ms"], 55.8)

    def test_rollup_milestones_includes_implied_blk_per_s(self):
        start = datetime(2026, 7, 6, 16, 0, 0)
        lines = [RUN_STARTED]
        for i in range(1, 4):
            ts = start + timedelta(seconds=100 * i)
            lines.append(
                f"{ts.strftime('%Y-%m-%dT%H:%M:%SZ')} benchstats: block disk=1000.0ms "
                "decompress=0.0ms par_jobs=0 prefetch_hit=0 prefetch_wait=0.0ms | "
                "coin prevouts=0 miss=0 lmdb=0.0ms decode=0.0ms warm=0.0ms "
                "skip_thr=0 skip_wrk=0 readers_full=0 | flush chainstate=0.0ms "
                "block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 parallel=0 "
                f"txindex=0.0ms blkidx_sync=0 connect_cs=0.0ms | {WAIT_ZERO}blocks=1000"
            )
        series = self.mod.parse_benchstats_series(lines)
        milestones = self.mod.rollup_milestones(series, (3,))
        self.assertEqual(len(milestones), 1)
        self.assertEqual(milestones[0]["implied_blk_per_s"], 10.0)

    def test_benchstats_summary(self):
        start = datetime(2026, 7, 6, 16, 0, 0)
        lines = [RUN_STARTED]
        for i in range(1, 6):
            ts = start + timedelta(seconds=200 * i)
            lines.append(
                f"{ts.strftime('%Y-%m-%dT%H:%M:%SZ')} benchstats: block disk={i * 1000}.0ms "
                "decompress=0.0ms par_jobs=0 prefetch_hit=0 prefetch_wait=0.0ms | "
                "coin prevouts=0 miss=0 lmdb=0.0ms decode=0.0ms warm=0.0ms "
                "skip_thr=0 skip_wrk=0 readers_full=0 | flush chainstate=0.0ms "
                "block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 parallel=0 "
                f"txindex=0.0ms blkidx_sync=0 connect_cs=0.0ms | {WAIT_ZERO}blocks=1000"
            )
        series = self.mod.parse_benchstats_series(lines)
        summary = self.mod.benchstats_summary(series)
        self.assertEqual(summary["rollup_count"], 5)
        self.assertEqual(summary["last_rollup_index"], 5)
        self.assertEqual(summary["last_disk_per_blk_ms"], 5.0)
        self.assertIsNotNone(summary["implied_blk_per_s"])
        self.assertIn("p50_readers_full", summary)
        self.assertIn("max_readers_full", summary)
        self.assertIn("p50_prefetch_hit", summary)
        self.assertIn("min_prefetch_hit", summary)

    def test_benchstats_summary_prefetch_health_values(self):
        start = datetime(2026, 7, 6, 16, 0, 0)
        lines = [RUN_STARTED]
        specs = [(2, 7), (0, 3), (5, 10)]
        for i, (readers_full, prefetch_hit) in enumerate(specs, start=1):
            ts = start + timedelta(seconds=100 * i)
            lines.append(
                f"{ts.strftime('%Y-%m-%dT%H:%M:%SZ')} benchstats: block disk=1000.0ms "
                "decompress=0.0ms par_jobs=0 "
                f"prefetch_hit={prefetch_hit} prefetch_wait=0.0ms | "
                "coin prevouts=0 miss=0 lmdb=0.0ms decode=0.0ms warm=0.0ms "
                f"skip_thr=0 skip_wrk=0 readers_full={readers_full} | "
                "flush chainstate=0.0ms block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 "
                "parallel=0 txindex=0.0ms blkidx_sync=0 connect_cs=0.0ms | "
                f"{WAIT_ZERO}blocks=1000"
            )
        series = self.mod.parse_benchstats_series(lines)
        summary = self.mod.benchstats_summary(series)
        self.assertEqual(summary["max_readers_full"], 5)
        self.assertEqual(summary["p50_readers_full"], 2)
        self.assertEqual(summary["min_prefetch_hit"], 3)
        self.assertEqual(summary["p50_prefetch_hit"], 7)

    def test_default_milestones_arg_matches_constant(self):
        self.assertEqual(
            self.mod.default_milestones_arg(),
            "60,110,120,200,250",
        )

    def test_rollup_milestones_includes_readers_full_and_prefetch_hit(self):
        start = datetime(2026, 7, 6, 16, 0, 0)
        lines = [RUN_STARTED]
        for i in range(1, 3):
            ts = start + timedelta(seconds=100 * i)
            lines.append(
                f"{ts.strftime('%Y-%m-%dT%H:%M:%SZ')} benchstats: block disk=1000.0ms "
                "decompress=0.0ms par_jobs=0 prefetch_hit=5 prefetch_wait=0.0ms | "
                "coin prevouts=80 miss=0 lmdb=0.0ms decode=0.0ms warm=0.0ms "
                "skip_thr=0 skip_wrk=0 readers_full=2 | flush chainstate=0.0ms "
                "block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 parallel=0 "
                f"txindex=0.0ms blkidx_sync=0 connect_cs=0.0ms | {WAIT_ZERO}blocks=1000"
            )
        series = self.mod.parse_benchstats_series(lines)
        milestones = self.mod.rollup_milestones(series, (2,))
        self.assertEqual(milestones[0]["readers_full"], 2)
        self.assertEqual(milestones[0]["prefetch_hit"], 5)

    def test_default_milestones_constant(self):
        self.assertEqual(self.mod.DEFAULT_MILESTONES, (60, 110, 120, 200, 250))

    def test_parse_benchstats_csv_columns(self):
        parse_mod = load_module(PARSE_BENCHSTATS_PATH, "parse_benchstats")
        start = datetime(2026, 7, 6, 16, 0, 0)
        lines = [RUN_STARTED, LMDB_PARALLELISM]
        for i in range(1, 3):
            ts = start + timedelta(seconds=100 * i)
            lines.append(
                f"{ts.strftime('%Y-%m-%dT%H:%M:%SZ')} benchstats: block disk=1000.0ms "
                "decompress=0.0ms par_jobs=3 prefetch_hit=7 prefetch_wait=0.0ms | "
                "coin prevouts=0 miss=0 lmdb=0.0ms decode=0.0ms warm=0.0ms "
                "skip_thr=0 skip_wrk=0 readers_full=2 | flush chainstate=0.0ms "
                "block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=1 parallel=4 "
                "txindex=0.0ms blkidx_sync=5 connect_cs=1.0ms | "
                "wait cs_main_hold=10.0ms flush_mutex_wait=2.0ms blkidx_mutex_wait=1.0ms "
                "abc_idle=5.0ms script_jobs=100 script_done=100 | blocks=1000"
            )
        series = self.mod.parse_benchstats_series(lines)
        rows = parse_mod.series_csv_rows(series)
        buf = io.StringIO()
        writer = csv.DictWriter(buf, fieldnames=parse_mod.CSV_COLUMNS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
        reader = csv.DictReader(io.StringIO(buf.getvalue()))
        self.assertEqual(reader.fieldnames, list(parse_mod.CSV_COLUMNS))
        data = list(reader)
        self.assertEqual(len(data), 2)
        self.assertEqual(data[0]["par_jobs"], "3")
        self.assertEqual(data[0]["parallel"], "4")
        self.assertEqual(data[0]["stale"], "1")
        self.assertEqual(data[0]["readers_full"], "2")
        self.assertEqual(data[0]["prefetch_hit"], "7")
        self.assertEqual(data[0]["blkidx_sync"], "5")
        self.assertEqual(data[0].get("interval_implied_blk_per_s", ""), "")
        self.assertEqual(data[1]["interval_implied_blk_per_s"], "10.0")
        self.assertEqual(data[0]["cs_main_hold"], "10.0")
        self.assertEqual(data[0]["flush_mutex_wait"], "2.0")
        self.assertEqual(data[0]["abc_idle"], "5.0")
        self.assertEqual(data[0]["script_jobs"], "100")
        self.assertEqual(data[0]["script_done"], "100")
        self.assertEqual(data[0]["cs_main_hold_per_blk_ms"], "0.01")
        self.assertEqual(data[0]["abc_idle_per_blk_ms"], "0.01")


if __name__ == "__main__":
    unittest.main()