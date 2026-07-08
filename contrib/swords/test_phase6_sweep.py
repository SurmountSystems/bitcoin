#!/usr/bin/env python3
"""Unit tests for contrib/swords/phase6_sweep.py."""
from __future__ import annotations

import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from contextlib import redirect_stderr
from datetime import datetime, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
JUSTFILE_PATH = ROOT / "justfile"
MODULE_PATH = ROOT / "contrib" / "swords" / "phase6_sweep.py"
BENCHSTATS_PARSE_PATH = ROOT / "contrib" / "swords" / "benchstats_parse.py"


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
    "blockindexsync=2 txindexbatch=100 chainstate_maxreaders=126"
)

LMDB_PARALLELISM_TXINDEX_500 = (
    "2026-07-06T16:00:01Z Swords LMDB parallelism: benchstats=1 par=8 "
    "coinprefetchpar=4 utxoencodepar=2 blockdecompresspar=2 flushsnapshot=0 "
    "blockindexsync=2 txindexbatch=500 chainstate_maxreaders=126"
)


def benchstats_line(
    ts: str,
    disk_total: float = 5000.0,
    txindex: float = 0.0,
    *,
    readers_full: int = 0,
    prefetch_hit: int = 0,
    flush_lmdb_ms: float = 0.0,
) -> str:
    return (
        f"{ts} benchstats: block disk={disk_total:.1f}ms decompress=0.0ms par_jobs=0 "
        f"prefetch_hit={prefetch_hit} prefetch_wait=0.0ms | coin prevouts=0 miss=0 lmdb=0.0ms "
        f"decode=0.0ms warm=0.0ms skip_thr=0 skip_wrk=0 readers_full={readers_full} | "
        f"flush chainstate=0.0ms block_index=0.0ms encode=0.0ms lmdb={flush_lmdb_ms:.1f}ms stale=0 "
        f"parallel=0 txindex={txindex:.1f}ms blkidx_sync=0 connect_cs=0.0ms | "
        "wait cs_main_hold=0.0ms flush_mutex_wait=0.0ms blkidx_mutex_wait=0.0ms "
        "abc_idle=0.0ms script_jobs=0 script_done=0 | blocks=1000"
    )


def series_lines(
    rollup_count: int = 3,
    *,
    lmdb_line: str = LMDB_PARALLELISM,
) -> list[str]:
    start = datetime(2026, 7, 6, 16, 0, 0)
    lines = [RUN_STARTED, lmdb_line]
    for i in range(1, rollup_count + 1):
        ts = start + timedelta(seconds=100 * i)
        lines.append(benchstats_line(ts.strftime("%Y-%m-%dT%H:%M:%SZ")))
    return lines


def run_cli(*cli_args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    merged_env = {**os.environ, **(env or {})}
    return subprocess.run(
        ["python3", str(MODULE_PATH), *cli_args],
        cwd=str(ROOT),
        env=merged_env,
        capture_output=True,
        text=True,
    )


class Phase6SweepTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module(MODULE_PATH, "phase6_sweep")
        cls.bs = load_module(BENCHSTATS_PARSE_PATH, "benchstats_parse_phase6")

    def test_variant_ordering(self):
        ids = [v["id"] for v in self.mod.SWEEP_VARIANTS]
        self.assertEqual(
            ids,
            [
                "baseline",
                "prefetch_serial",
                "flush_128m",
                "flush_256m",
                "script_par",
                "explicit_parallel",
                "txindex_500",
            ],
        )

    def test_campaign_variant_order(self):
        self.assertEqual(
            self.mod.CAMPAIGN_VARIANT_ORDER,
            (
                "prefetch_serial",
                "baseline",
                "txindex_500",
                "flush_128m",
                "flush_256m",
                "script_par",
                "explicit_parallel",
            ),
        )

    def test_one_knob_isolation(self):
        baseline = self.mod.variant_by_id("baseline")
        baseline_keys = self.mod.overlay_keys_vs_baseline(baseline)
        self.assertEqual(baseline_keys, set())

        expected_overlays = {
            "prefetch_serial": {"coinprefetchpar"},
            "flush_128m": {"dbbatchsize"},
            "flush_256m": {"dbbatchsize"},
            "explicit_parallel": {"coinprefetchpar", "blockdecompresspar", "utxoencodepar"},
            "script_par": {"par"},
            "txindex_500": {"txindexbatch"},
        }
        for vid, keys in expected_overlays.items():
            variant = self.mod.variant_by_id(vid)
            self.assertEqual(self.mod.overlay_keys_vs_baseline(variant), keys, vid)

        flush_128 = self.mod.merged_variant_args(self.mod.variant_by_id("flush_128m"))
        flush_256 = self.mod.merged_variant_args(self.mod.variant_by_id("flush_256m"))
        self.assertNotEqual(flush_128["dbbatchsize"], flush_256["dbbatchsize"])
        for key in self.mod.BASELINE_ARGS:
            if key != "dbbatchsize":
                self.assertEqual(flush_128[key], self.mod.BASELINE_ARGS[key])
                self.assertEqual(flush_256[key], self.mod.BASELINE_ARGS[key])

    def test_script_par_default_and_override(self):
        variant = self.mod.variant_by_id("script_par")
        with unittest.mock.patch.object(self.mod.os, "cpu_count", return_value=16):
            self.assertEqual(self.mod.script_par_value(), 15)
        merged = self.mod.merged_variant_args(variant)
        self.assertEqual(merged["par"], 15)

        env = {**os.environ, "SWORDS_SWEEP_PAR": "4"}
        with unittest.mock.patch.dict(os.environ, env, clear=False):
            self.assertEqual(self.mod.script_par_value(), 4)

    def test_render_bitcoin_conf_merge_and_overlay(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline = root / "bitcoin.conf"
            baseline.write_text(
                "server=1\n# comment\nblockindexsync=1\ntxindex=1\n",
                encoding="utf-8",
            )
            variant = self.mod.variant_by_id("flush_128m")
            out = root / "out.conf"
            self.mod.render_bitcoin_conf(baseline, variant, out)
            text = out.read_text(encoding="utf-8")
            self.assertIn("dbbatchsize=134217728", text)
            self.assertIn("blockindexsync=2", text)
            self.assertIn("server=1", text)
            self.assertIn("txindex=1", text)

    def test_render_bitcoin_conf_prefetch_serial_strips_stale_coinprefetchpar(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline = root / "bitcoin.conf"
            baseline.write_text("server=1\n", encoding="utf-8")
            explicit = self.mod.variant_by_id("explicit_parallel")
            prefetch = self.mod.variant_by_id("prefetch_serial")
            mid = root / "explicit.conf"
            self.mod.render_bitcoin_conf(baseline, explicit, mid)
            self.assertIn("coinprefetchpar=8", mid.read_text(encoding="utf-8"))

            stale_baseline = root / "stale.conf"
            stale_baseline.write_text(mid.read_text(encoding="utf-8"), encoding="utf-8")
            out = root / "prefetch.conf"
            self.mod.render_bitcoin_conf(stale_baseline, prefetch, out)
            text = out.read_text(encoding="utf-8")
            self.assertIn("coinprefetchpar=1", text)
            self.assertNotIn("coinprefetchpar=8", text)
            self.assertNotIn("blockdecompresspar=4", text)
            self.assertNotIn("utxoencodepar=8", text)

    def test_format_matrix_delta_absolute_for_zero_baseline(self):
        self.assertEqual(self.mod.format_matrix_delta("readers_full", 3, 0), "+3")
        self.assertEqual(self.mod.format_matrix_delta("prefetch_hit", 5, 0), "+5")
        self.assertEqual(self.mod.format_matrix_delta("disk_per_blk_ms", 3.0, 0), "")

    def test_render_bitcoin_conf_strips_stale_sweep_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline = root / "bitcoin.conf"
            baseline.write_text(
                "server=1\ndbbatchsize=134217728\nblockindexsync=2\n",
                encoding="utf-8",
            )
            flush_a = self.mod.variant_by_id("flush_128m")
            flush_b = self.mod.variant_by_id("script_par")
            mid = root / "flush.conf"
            self.mod.render_bitcoin_conf(baseline, flush_a, mid)

            stale_baseline = root / "stale.conf"
            stale_baseline.write_text(mid.read_text(encoding="utf-8"), encoding="utf-8")
            out = root / "script.conf"
            self.mod.render_bitcoin_conf(stale_baseline, flush_b, out)
            text = out.read_text(encoding="utf-8")
            self.assertNotIn("dbbatchsize=", text)
            self.assertIn("server=1", text)

    def test_variant_start_args_sorted(self):
        variant = self.mod.variant_by_id("explicit_parallel")
        args = self.mod.variant_start_args(variant)
        self.assertEqual(args, sorted(args, key=lambda s: s.lower()))
        self.assertIn("-coinprefetchpar=8", args)
        self.assertIn("-blockdecompresspar=4", args)
        self.assertIn("-utxoencodepar=8", args)

    def test_state_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            state = self.mod.load_sweep_state(profile_logs)
            self.assertEqual(state.get("variants"), {})
            state_path = self.mod.save_sweep_state(
                {"variants": {"baseline": {"archive_path": "/tmp/a"}}},
                profile_logs,
            )
            self.assertTrue(state_path.is_file())
            loaded = self.mod.load_sweep_state(profile_logs)
            self.assertIn("baseline", loaded["variants"])
            self.assertIsNotNone(loaded.get("updated_at"))

    def test_compare_matrix_from_fixture_logs(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            archive_a = profile_logs / "run-a"
            archive_b = profile_logs / "run-b"
            archive_a.mkdir()
            archive_b.mkdir()
            (archive_a / "debug.log").write_text("\n".join(series_lines(2)) + "\n")
            (archive_b / "debug.log").write_text(
                "\n".join(series_lines(2)) + "\n",
                encoding="utf-8",
            )

            state = {"variants": {}}
            self.mod.record_variant_complete(
                state,
                "baseline",
                archive_a,
                milestones=(1, 2),
                profile_logs=profile_logs,
            )
            self.mod.record_variant_complete(
                state,
                "flush_128m",
                archive_b,
                milestones=(1, 2),
                profile_logs=profile_logs,
            )

            matrix = self.mod.build_compare_matrix(state, profile_logs, milestones=(1, 2))
            self.assertEqual(len(matrix["rows"]), 4)
            rows_by_variant = {}
            for row in matrix["rows"]:
                rows_by_variant.setdefault(row["variant_id"], []).append(row)
            self.assertEqual(set(rows_by_variant), {"baseline", "flush_128m"})
            for rows in rows_by_variant.values():
                self.assertEqual(len(rows), 2)
                self.assertFalse(rows[0].get("missing_milestone"))
            flush_row = rows_by_variant["flush_128m"][0]
            self.assertIn("disk_per_blk_ms_delta_pct", flush_row)
            self.assertIn("readers_full", flush_row)
            self.assertIn("readers_full_delta_pct", flush_row)
            self.assertIn("prefetch_hit", flush_row)
            self.assertIn("prefetch_hit_delta_pct", flush_row)

    def test_default_milestones_include_120(self):
        self.assertIn(120, self.bs.DEFAULT_MILESTONES)
        self.assertEqual(self.bs.DEFAULT_MILESTONES, (60, 110, 120, 200, 250))

    def test_next_recommended_requires_complete_record(self):
        state = {
            "variants": {
                "baseline": {},
                "prefetch_serial": {},
                "flush_128m": {},
                "flush_256m": {},
                "script_par": {},
                "explicit_parallel": {},
            }
        }
        nxt = self.mod.next_recommended_variant(state)
        self.assertIsNotNone(nxt)
        self.assertEqual(nxt["id"], "prefetch_serial")

        state["variants"]["prefetch_serial"] = {
            "archive_path": "/tmp/prefetch",
            "recorded_at": "2026-07-06T12:00:00Z",
        }
        nxt = self.mod.next_recommended_variant(state)
        self.assertEqual(nxt["id"], "baseline")

    def test_next_recommended_skips_optional_until_required_done(self):
        state = {"variants": {}}
        for vid in self.mod.CAMPAIGN_VARIANT_ORDER:
            if vid == "txindex_500":
                continue
            state["variants"][vid] = {
                "archive_path": f"/tmp/{vid}",
                "recorded_at": "2026-07-06T12:00:00Z",
                "milestones": [
                    {
                        "rollup_index": 120,
                        "readers_full": 0,
                        "prefetch_hit": 5,
                        "flush_lmdb_per_blk_ms": 0.5,
                    }
                ],
            }
        nxt = self.mod.next_recommended_variant(state)
        self.assertIsNotNone(nxt)
        self.assertEqual(nxt["id"], "txindex_500")

    def _recorded_baseline_entry(
        self,
        *,
        readers_full: int = 0,
        prefetch_hit: int = 5,
        flush_lmdb_per_blk_ms: float = 0.5,
        rollup_index: int = 120,
    ) -> dict:
        return {
            "archive_path": "/tmp/baseline",
            "recorded_at": "2026-07-06T12:00:00Z",
            "milestones": [
                {
                    "rollup_index": rollup_index,
                    "readers_full": readers_full,
                    "prefetch_hit": prefetch_hit,
                    "flush_lmdb_per_blk_ms": flush_lmdb_per_blk_ms,
                }
            ],
            "summary": {
                "p50_flush_lmdb_per_blk_ms": flush_lmdb_per_blk_ms,
                "max_readers_full": readers_full,
                "min_prefetch_hit": prefetch_hit,
            },
        }

    def test_baseline_passes_prefetch_gate_at_milestone_120(self):
        state = {
            "variants": {
                "baseline": self._recorded_baseline_entry(
                    readers_full=0, prefetch_hit=3, rollup_index=120
                )
            }
        }
        self.assertTrue(self.mod.baseline_passes_prefetch_gate(state))

    def test_baseline_fails_prefetch_gate_when_readers_full(self):
        state = {
            "variants": {
                "baseline": self._recorded_baseline_entry(
                    readers_full=2, prefetch_hit=3, rollup_index=120
                )
            }
        }
        self.assertFalse(self.mod.baseline_passes_prefetch_gate(state))

    def test_baseline_fails_prefetch_gate_when_prefetch_hit_zero_at_120(self):
        state = {
            "variants": {
                "baseline": self._recorded_baseline_entry(
                    readers_full=0, prefetch_hit=0, rollup_index=120
                )
            }
        }
        self.assertFalse(self.mod.baseline_passes_prefetch_gate(state))
        reason = self.mod._prefetch_gate_failure_reason(state)
        self.assertIn("prefetch_hit=0", reason)

    def test_baseline_fails_prefetch_gate_when_milestone_120_missing(self):
        entry = self._recorded_baseline_entry(
            readers_full=0, prefetch_hit=5, rollup_index=60
        )
        entry["milestones"].append(
            {
                "rollup_index": 110,
                "readers_full": 0,
                "prefetch_hit": 8,
                "flush_lmdb_per_blk_ms": 0.5,
            }
        )
        state = {"variants": {"baseline": entry}}
        self.assertFalse(self.mod.baseline_passes_prefetch_gate(state))
        reason = self.mod._prefetch_gate_failure_reason(state)
        self.assertIn("milestone #120 not in record", reason)
        self.assertIn("[60, 110]", reason)

    def test_baseline_passes_prefetch_gate_summary_only_fallback(self):
        state = {
            "variants": {
                "baseline": {
                    "archive_path": "/tmp/baseline",
                    "recorded_at": "2026-07-06T12:00:00Z",
                    "milestones": [],
                    "summary": {
                        "max_readers_full": 0,
                        "min_prefetch_hit": 4,
                    },
                }
            }
        }
        self.assertTrue(self.mod.baseline_passes_prefetch_gate(state))

    def test_baseline_fails_prefetch_gate_summary_only_when_prefetch_hit_zero(self):
        state = {
            "variants": {
                "baseline": {
                    "archive_path": "/tmp/baseline",
                    "recorded_at": "2026-07-06T12:00:00Z",
                    "milestones": [],
                    "summary": {
                        "max_readers_full": 0,
                        "min_prefetch_hit": 0,
                    },
                }
            }
        }
        self.assertFalse(self.mod.baseline_passes_prefetch_gate(state))

    def test_gate_blocks_explicit_parallel_when_baseline_readers_full(self):
        state = {"variants": {}}
        for vid in ("prefetch_serial", "baseline", "script_par"):
            if vid == "baseline":
                entry = self._recorded_baseline_entry(readers_full=3, prefetch_hit=5)
            else:
                entry = {
                    "archive_path": f"/tmp/{vid}",
                    "recorded_at": "2026-07-06T12:00:00Z",
                }
            state["variants"][vid] = entry
        gates = self.mod.evaluate_campaign_gates(state)
        self.assertEqual(gates["explicit_parallel"]["status"], "blocked")
        self.assertIn("readers_full=3", gates["explicit_parallel"]["reason"])
        nxt = self.mod.next_recommended_variant(state)
        self.assertNotEqual(nxt["id"], "explicit_parallel")

    def test_gate_allows_explicit_parallel_when_baseline_passes(self):
        state = {"variants": {}}
        for vid in self.mod.CAMPAIGN_VARIANT_ORDER:
            if vid == "txindex_500":
                continue
            if vid == "baseline":
                entry = self._recorded_baseline_entry(readers_full=0, prefetch_hit=8)
            else:
                entry = {
                    "archive_path": f"/tmp/{vid}",
                    "recorded_at": "2026-07-06T12:00:00Z",
                }
            state["variants"][vid] = entry
        gates = self.mod.evaluate_campaign_gates(state)
        self.assertEqual(gates["explicit_parallel"]["status"], "done")
        nxt = self.mod.next_recommended_variant(state)
        self.assertEqual(nxt["id"], "txindex_500")

    def test_flush_variants_skipped_without_hotspot(self):
        state = {
            "variants": {
                "prefetch_serial": {
                    "archive_path": "/tmp/prefetch",
                    "recorded_at": "2026-07-06T12:00:00Z",
                },
                "baseline": self._recorded_baseline_entry(flush_lmdb_per_blk_ms=0.4),
            }
        }
        gates = self.mod.evaluate_campaign_gates(state)
        self.assertEqual(gates["flush_128m"]["status"], "skipped")
        self.assertEqual(gates["flush_256m"]["status"], "skipped")
        nxt = self.mod.next_recommended_variant(state)
        self.assertEqual(nxt["id"], "script_par")
        opt = self.mod.optional_next_variant(state)
        self.assertIsNotNone(opt)
        self.assertEqual(opt["id"], "txindex_500")

    def test_flush_variants_ready_when_hotspot_detected(self):
        state = {
            "variants": {
                "prefetch_serial": {
                    "archive_path": "/tmp/prefetch",
                    "recorded_at": "2026-07-06T12:00:00Z",
                },
                "baseline": self._recorded_baseline_entry(flush_lmdb_per_blk_ms=1.5),
            }
        }
        self.assertTrue(self.mod.flush_hotspot_detected(state))
        gates = self.mod.evaluate_campaign_gates(state)
        self.assertEqual(gates["flush_128m"]["status"], "ready")
        nxt = self.mod.next_recommended_variant(state)
        self.assertEqual(nxt["id"], "flush_128m")
        opt = self.mod.optional_next_variant(state)
        self.assertEqual(opt["id"], "txindex_500")

    def test_next_recommended_prefers_required_ready_over_optional(self):
        state = {
            "variants": {
                "prefetch_serial": {
                    "archive_path": "/tmp/prefetch",
                    "recorded_at": "2026-07-06T12:00:00Z",
                },
                "baseline": self._recorded_baseline_entry(flush_lmdb_per_blk_ms=0.4),
            }
        }
        nxt = self.mod.next_recommended_variant(state)
        self.assertEqual(nxt["id"], "script_par")
        self.assertEqual(self.mod.optional_next_variant(state)["id"], "txindex_500")

    def test_campaign_next_variant_ordering(self):
        state = {"variants": {}}
        nxt = self.mod.next_recommended_variant(state)
        self.assertEqual(nxt["id"], "prefetch_serial")
        state["variants"]["prefetch_serial"] = {
            "archive_path": "/tmp/prefetch",
            "recorded_at": "2026-07-06T12:00:00Z",
        }
        self.assertEqual(self.mod.next_recommended_variant(state)["id"], "baseline")

    def test_spot_check_hints_milestones_200_and_250(self):
        hints_200 = self.mod.spot_check_hints(200)
        hints_250 = self.mod.spot_check_hints(250)
        self.assertIsNotNone(hints_200)
        self.assertIsNotNone(hints_250)
        self.assertTrue(any("profile-connectblock" in h for h in hints_200))
        self.assertTrue(any("profile-utxo-flush" in h for h in hints_250))
        self.assertTrue(any("par_jobs=0" in h for h in hints_200))
        self.assertIsNone(self.mod.spot_check_hints(120))

    def test_spot_check_hints_for_milestones_dedupes_order(self):
        hints = self.mod.spot_check_hints_for_milestones([200, 250])
        self.assertEqual(len(hints), 6)
        self.assertIn("profile-connectblock", "\n".join(hints))

    def test_record_resolves_latest_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            archive = profile_logs / "2026-07-06T12-00Z"
            archive.mkdir()
            (archive / "debug.log").write_text("\n".join(series_lines(1)) + "\n")
            (profile_logs / "LATEST.txt").write_text(str(archive) + "\n")

            state = self.mod.load_sweep_state(profile_logs)
            entry, _ = self.mod.record_variant_complete(
                state,
                "baseline",
                self.mod.resolve_archive_dir("latest", profile_logs),
                milestones=(1,),
                profile_logs=profile_logs,
            )
            self.assertEqual(entry["archive_path"], str(archive.resolve()))
            self.assertEqual(len(entry["milestones"]), 1)
            self.assertEqual(entry["started_at"], "2026-07-06T16:00:00Z")

    def test_record_metadata_mismatch_warns(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            archive = profile_logs / "run-txindex"
            archive.mkdir()
            (archive / "debug.log").write_text(
                "\n".join(series_lines(1, lmdb_line=LMDB_PARALLELISM_TXINDEX_500)) + "\n"
            )
            state = {"variants": {}}
            entry, notices = self.mod.record_variant_complete(
                state,
                "baseline",
                archive,
                milestones=(1,),
                profile_logs=profile_logs,
            )
            self.assertTrue(any("txindexbatch" in n for n in entry["metadata_warnings"]))
            self.assertTrue(any("run_metadata mismatch" in n for n in notices))

    def test_record_rejects_reassigned_archive_without_force(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            archive = profile_logs / "shared"
            archive.mkdir()
            (archive / "debug.log").write_text("\n".join(series_lines(1)) + "\n")
            state = {"variants": {}}
            self.mod.record_variant_complete(
                state, "baseline", archive, milestones=(1,), profile_logs=profile_logs
            )
            with self.assertRaises(ValueError):
                self.mod.record_variant_complete(
                    state, "flush_128m", archive, milestones=(1,), profile_logs=profile_logs
                )

    def test_record_rejects_env_variant_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            archive = profile_logs / "run-a"
            archive.mkdir()
            (archive / "debug.log").write_text("\n".join(series_lines(1)) + "\n")
            state = {"variants": {}}
            with unittest.mock.patch.dict(os.environ, {"SWORDS_SWEEP_VARIANT": "flush_128m"}):
                with self.assertRaises(ValueError) as ctx:
                    self.mod.record_variant_complete(
                        state,
                        "baseline",
                        archive,
                        milestones=(1,),
                        profile_logs=profile_logs,
                    )
            self.assertIn("does not match", str(ctx.exception))

    def test_record_overwrite_warning(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            archive = profile_logs / "run-a"
            archive.mkdir()
            (archive / "debug.log").write_text("\n".join(series_lines(1)) + "\n")
            state = {"variants": {}}
            self.mod.record_variant_complete(
                state, "baseline", archive, milestones=(1,), profile_logs=profile_logs
            )
            _, notices = self.mod.record_variant_complete(
                state, "baseline", archive, milestones=(1,), profile_logs=profile_logs
            )
            self.assertTrue(any("overwriting prior record" in n for n in notices))

    def test_print_compare_matrix_missing_milestone_placeholders(self):
        matrix = {
            "milestones": [1, 2],
            "rows": [
                {
                    "variant_id": "baseline",
                    "rollup_index": 1,
                    "missing_milestone": False,
                    "wall_s": 1.0,
                    "disk_per_blk_ms": 2.0,
                    "connect_cs_per_blk_ms": 3.0,
                    "flush_lmdb_per_blk_ms": 4.0,
                    "txindex_per_blk_ms": 5.0,
                    "unaccounted_per_blk_ms": 0.1,
                    "implied_blk_per_s": 10.0,
                    "wall_s_delta_pct": None,
                    "disk_per_blk_ms_delta_pct": None,
                    "connect_cs_per_blk_ms_delta_pct": None,
                    "flush_lmdb_per_blk_ms_delta_pct": None,
                    "txindex_per_blk_ms_delta_pct": None,
                    "unaccounted_per_blk_ms_delta_pct": None,
                    "implied_blk_per_s_delta_pct": None,
                    "readers_full": 0,
                    "readers_full_delta_pct": None,
                    "readers_full_delta_display": "",
                    "prefetch_hit": 0,
                    "prefetch_hit_delta_pct": None,
                    "prefetch_hit_delta_display": "",
                    "summary_implied_blk_per_s": 10.0,
                },
                {
                    "variant_id": "baseline",
                    "rollup_index": 2,
                    "missing_milestone": True,
                    "wall_s": None,
                    "disk_per_blk_ms": None,
                    "connect_cs_per_blk_ms": None,
                    "flush_lmdb_per_blk_ms": None,
                    "txindex_per_blk_ms": None,
                    "unaccounted_per_blk_ms": None,
                    "implied_blk_per_s": None,
                    "wall_s_delta_pct": None,
                    "disk_per_blk_ms_delta_pct": None,
                    "connect_cs_per_blk_ms_delta_pct": None,
                    "flush_lmdb_per_blk_ms_delta_pct": None,
                    "txindex_per_blk_ms_delta_pct": None,
                    "unaccounted_per_blk_ms_delta_pct": None,
                    "implied_blk_per_s_delta_pct": None,
                    "readers_full": None,
                    "readers_full_delta_pct": None,
                    "readers_full_delta_display": "",
                    "prefetch_hit": None,
                    "prefetch_hit_delta_pct": None,
                    "prefetch_hit_delta_display": "",
                    "summary_implied_blk_per_s": None,
                },
            ],
            "notes": [],
        }
        buf = io.StringIO()
        with redirect_stderr(buf):
            pass
        out = io.StringIO()
        with unittest.mock.patch("sys.stdout", out):
            self.mod.print_compare_matrix(matrix)
        text = out.getvalue()
        self.assertIn("#2", text)
        self.assertIn("—", text)
        self.assertIn("milestones_hit", text)

    def test_load_sweep_state_corrupt_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            path = self.mod.sweep_state_path(profile_logs)
            path.write_text("{not json", encoding="utf-8")
            with self.assertRaises(self.mod.SweepStateError) as ctx:
                self.mod.load_sweep_state(profile_logs)
            self.assertIn("corrupt state file", str(ctx.exception))

    def test_cli_invalid_variant(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = run_cli(
                "args",
                "--variant",
                "bogus",
                "--profile-logs",
                str(tmp),
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("error:", result.stderr)
            self.assertIn("Unknown variant", result.stderr)

    def test_cli_bad_swords_sweep_par(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = run_cli(
                "args",
                "--variant",
                "script_par",
                "--profile-logs",
                str(tmp),
                env={"SWORDS_SWEEP_PAR": "notanumber"},
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("SWORDS_SWEEP_PAR", result.stderr)

    def test_cli_invalid_milestones(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = run_cli(
                "compare",
                "--milestones",
                "bad",
                "--profile-logs",
                str(tmp),
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("invalid --milestones", result.stderr)

    def test_cli_corrupt_state_exit_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            path = profile_logs / "phase6-sweep-state.json"
            path.write_text("{bad", encoding="utf-8")
            result = run_cli("status", "--profile-logs", str(profile_logs))
            self.assertEqual(result.returncode, 2)
            self.assertIn("corrupt state file", result.stderr)

    def test_cli_record_missing_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = run_cli(
                "record",
                "--variant",
                "baseline",
                "--archive",
                "latest",
                "--profile-logs",
                str(tmp),
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("No archived logs", result.stderr)

    def test_cli_args_subcommand(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = run_cli(
                "args",
                "--variant",
                "baseline",
                "--profile-logs",
                str(tmp),
            )
            self.assertEqual(result.returncode, 0)
            self.assertIn("-benchstats=1", result.stdout)

    def test_cli_plan_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            out = subprocess.run(
                [
                    "python3",
                    str(MODULE_PATH),
                    "json",
                    "--profile-logs",
                    str(profile_logs),
                    "--milestones",
                    "1",
                ],
                check=True,
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            payload = json.loads(out.stdout)
            self.assertIn("variants", payload)
            self.assertIn("compare_matrix", payload)
            self.assertEqual(payload["next_recommended"], "prefetch_serial")
            self.assertIn("campaign_gates", payload)
            self.assertIn("optional_next", payload)
            self.assertEqual(payload["baseline_prefetch_gate_milestone"], 120)

    def test_cli_campaign_subcommand(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            result = run_cli("campaign", "--profile-logs", str(profile_logs))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("phase6_sweep_campaign", result.stdout)
            self.assertIn("campaign_order", result.stdout)
            self.assertIn("baseline_prefetch_gate", result.stdout)
            self.assertIn("next_recommended\tprefetch_serial", result.stdout)
            self.assertIn("profile-connectblock", result.stdout)
            self.assertIn("rollup #120 required", result.stdout)
            self.assertNotIn("per_variant_cycle", result.stdout)

    def test_cli_invalid_flush_hotspot_ms_warns(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = run_cli(
                "campaign",
                "--profile-logs",
                str(tmp),
                env={"SWORDS_SWEEP_FLUSH_HOTSPOT_MS": "not-a-number"},
            )
            self.assertEqual(result.returncode, 0)
            self.assertIn("invalid SWORDS_SWEEP_FLUSH_HOTSPOT_MS", result.stderr)
            self.assertIn("flush_hotspot_threshold_ms\t1.0", result.stdout)

    def test_flush_hotspot_threshold_invalid_env_falls_back(self):
        with unittest.mock.patch.dict(
            os.environ, {"SWORDS_SWEEP_FLUSH_HOTSPOT_MS": "bad"}, clear=False
        ):
            with redirect_stderr(io.StringIO()) as err:
                val = self.mod.flush_hotspot_threshold_ms()
            self.assertEqual(val, self.mod.DEFAULT_FLUSH_HOTSPOT_MS)
            self.assertIn("invalid SWORDS_SWEEP_FLUSH_HOTSPOT_MS", err.getvalue())

    @unittest.skipUnless(shutil.which("just"), "just not installed")
    def test_just_sweep_campaign(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            env = {**os.environ, "SWORDS_LOG_ARCHIVE": str(profile_logs)}
            result = subprocess.run(
                ["just", "sweep-campaign"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("phase6_sweep_campaign", result.stdout)
            self.assertIn("next_recommended", result.stdout)
            self.assertIn("prefetch_serial", result.stdout)

    @unittest.skipUnless(shutil.which("just"), "just not installed")
    def test_just_phase6_plan(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            env = {
                **os.environ,
                "SWORDS_LOG_ARCHIVE": str(profile_logs),
            }
            result = subprocess.run(
                ["just", "phase6"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("phase6_parameter_sweep", result.stdout)
            self.assertIn("just sweep-apply", result.stdout)
            self.assertIn("prefetch_serial", result.stdout)
            self.assertIn("just sweep-finish <variant>", result.stdout)
            self.assertIn("just phase0-check", result.stdout)

    @unittest.skipUnless(shutil.which("just"), "just not installed")
    def test_just_sweep_conf(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            env = {
                **os.environ,
                "SWORDS_LOG_ARCHIVE": str(profile_logs),
                "SWORDS_DATADIR": str(Path(tmp) / "datadir"),
            }
            result = subprocess.run(
                ["just", "sweep-conf", "flush_128m"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("conf_written", result.stdout)
            self.assertIn("dbbatchsize=134217728", result.stdout)

    @unittest.skipUnless(shutil.which("just"), "just not installed")
    def test_just_sweep_args(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp)
            env = {**os.environ, "SWORDS_LOG_ARCHIVE": str(profile_logs)}
            result = subprocess.run(
                ["just", "sweep-args", "baseline"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("-benchstats=1", result.stdout)


class JustfileSweepVariantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.justfile = JUSTFILE_PATH.read_text()

    def _recipe_body(self, recipe: str) -> str:
        for needle in (f"\n{recipe} ", f"\n{recipe}:"):
            try:
                return self.justfile[self.justfile.index(needle) :]
            except ValueError:
                continue
        raise ValueError(f"recipe {recipe!r} not found in justfile")

    def test_sweep_start_does_not_export_variant(self):
        body = self._recipe_body("sweep-start")
        end = body.index("\n\n[doc(")
        block = body[:end]
        self.assertNotIn("SWORDS_SWEEP_VARIANT", block)

    def test_sweep_finish_exports_variant_before_reset_datadir(self):
        body = self._recipe_body("sweep-finish")
        end = body.index("\n\n[doc(")
        block = body[:end]
        self.assertIn('export SWORDS_SWEEP_VARIANT="{{VARIANT}}"', block)
        export_pos = block.index("export SWORDS_SWEEP_VARIANT")
        reset_pos = block.index("just reset-datadir")
        record_pos = block.index("just sweep-record")
        self.assertLess(export_pos, reset_pos)
        self.assertLess(reset_pos, record_pos)

    def test_reset_datadir_chmods_archive_root(self):
        body = self._recipe_body("reset-datadir")
        end = body.index("\n\n[doc(")
        block = body[:end]
        archive_root_pos = block.index('archive_root="{{profile_logs}}"')
        debug_log_pos = block.index('if [[ -f "$datadir/debug.log" ]]')
        chmod_root_pos = block.index('chmod 700 "$archive_root" 2>/dev/null || true')
        self.assertLess(archive_root_pos, chmod_root_pos)
        self.assertLess(chmod_root_pos, debug_log_pos)
        self.assertIn('chmod 700 "$dest" 2>/dev/null || true', block)

    def test_phase6_workflow_documents_variant_stamping(self):
        self.mod = load_module(MODULE_PATH, "phase6_sweep_variant_doc")
        self.assertIn("sweep-finish exports SWORDS_SWEEP_VARIANT", self.mod.PHASE6_WORKFLOW)
        self.assertIn("sweep_variant=", self.mod.PHASE6_WORKFLOW)
        self.assertNotIn("sweep-start and sweep-finish export", self.mod.PHASE6_WORKFLOW)


if __name__ == "__main__":
    unittest.main()