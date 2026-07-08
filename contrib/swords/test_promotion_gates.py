#!/usr/bin/env python3
"""Unit tests for promotion_gates.py (DEBUG_ONLY promotion prerequisites)."""
from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONTRIB = ROOT / "contrib" / "swords"

RUN_STARTED = (
    "2026-07-06T16:00:00Z === Swords run started 2026-07-06T16:00:00Z "
    "(datadir=/tmp/.bitcoin-swords benchstats=1) ==="
)


def _load_module(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, CONTRIB / filename)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def benchstats_line(
    ts: str,
    *,
    readers_full: int = 0,
    prefetch_hit: int = 5,
) -> str:
    return (
        f"{ts} benchstats: block disk=5000.0ms decompress=0.0ms par_jobs=0 "
        f"prefetch_hit={prefetch_hit} prefetch_wait=0.0ms | coin prevouts=128 miss=0 lmdb=0.0ms "
        f"decode=0.0ms warm=0.0ms skip_thr=0 skip_wrk=0 readers_full={readers_full} | "
        f"flush chainstate=0.0ms block_index=0.0ms encode=0.0ms lmdb=0.0ms stale=0 "
        f"parallel=0 txindex=0.0ms blkidx_sync=0 connect_cs=0.0ms | "
        "wait cs_main_hold=0.0ms flush_mutex_wait=0.0ms blkidx_mutex_wait=0.0ms "
        "abc_idle=0.0ms script_jobs=0 script_done=0 | blocks=1000"
    )


def build_campaign_log(
    archive: Path,
    *,
    seconds_per_rollup: float,
    readers_full: int = 0,
    prefetch_hit: int = 5,
    rollup_count: int = 250,
) -> Path:
    """Synthetic debug.log with rollup_count benchstats lines (milestone indices 1..N)."""
    archive.mkdir(parents=True, exist_ok=True)
    log = archive / "debug.log"
    start = datetime(2026, 7, 6, 16, 0, 0)
    lines = [RUN_STARTED]
    for i in range(1, rollup_count + 1):
        ts = start + timedelta(seconds=i * seconds_per_rollup)
        lines.append(
            benchstats_line(
                ts.strftime("%Y-%m-%dT%H:%M:%SZ"),
                readers_full=readers_full,
                prefetch_hit=prefetch_hit,
            )
        )
    log.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return log


class TestPromotionGates(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = _load_module("promotion_gates", "promotion_gates.py")

    def test_debug_only_flags_present_in_repo_init(self):
        checks = self.mod.debug_only_flags_present(self.mod.INIT_CPP)
        self.assertEqual(len(checks), 4)
        self.assertTrue(all(c.passed for c in checks), [c.detail for c in checks])

    def test_debug_only_detects_promoted_flag(self):
        with tempfile.TemporaryDirectory() as tmp:
            init_cpp = Path(tmp) / "init.cpp"
            init_cpp.write_text(
                'argsman.AddArg("-benchstats=<n>", "x", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);\n',
                encoding="utf-8",
            )
            checks = self.mod.debug_only_flags_present(init_cpp)
            bench = next(c for c in checks if c.name == "debug_only_benchstats")
            self.assertFalse(bench.passed)

    def test_baseline_beats_prefetch_serial_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            baseline_dir = Path(tmp) / "baseline"
            serial_dir = Path(tmp) / "prefetch_serial"
            # Faster baseline: higher implied_blk_per_s at milestones 120/200/250.
            build_campaign_log(baseline_dir, seconds_per_rollup=333.0)
            build_campaign_log(serial_dir, seconds_per_rollup=500.0)
            state = {
                "variants": {
                    "baseline": {
                        "archive_path": str(baseline_dir),
                        "archive_log": str(baseline_dir / "debug.log"),
                        "recorded_at": "2026-07-06T14:00:00Z",
                    },
                    "prefetch_serial": {
                        "archive_path": str(serial_dir),
                        "archive_log": str(serial_dir / "debug.log"),
                        "recorded_at": "2026-07-06T14:00:00Z",
                    },
                }
            }
            check = self.mod.baseline_beats_prefetch_serial(state, Path(tmp))
            self.assertTrue(check.passed, check.detail)

    def test_baseline_beats_prefetch_serial_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            baseline_dir = Path(tmp) / "baseline"
            serial_dir = Path(tmp) / "prefetch_serial"
            build_campaign_log(baseline_dir, seconds_per_rollup=500.0)
            build_campaign_log(serial_dir, seconds_per_rollup=333.0)
            state = {
                "variants": {
                    "baseline": {
                        "archive_path": str(baseline_dir),
                        "archive_log": str(baseline_dir / "debug.log"),
                        "recorded_at": "2026-07-06T14:00:00Z",
                    },
                    "prefetch_serial": {
                        "archive_path": str(serial_dir),
                        "archive_log": str(serial_dir / "debug.log"),
                        "recorded_at": "2026-07-06T14:00:00Z",
                    },
                }
            }
            check = self.mod.baseline_beats_prefetch_serial(state, Path(tmp))
            self.assertFalse(check.passed)
            self.assertIn("#120", check.detail)

    def test_baseline_ibd_depth_gate_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "baseline"
            build_campaign_log(archive, seconds_per_rollup=400.0, rollup_count=250)
            state = {
                "variants": {
                    "baseline": {
                        "archive_path": str(archive),
                        "archive_log": str(archive / "debug.log"),
                        "recorded_at": "2026-07-06T14:00:00Z",
                    }
                }
            }
            check = self.mod.baseline_ibd_depth_gate(state)
            self.assertTrue(check.passed, check.detail)

    def test_baseline_ibd_depth_gate_fail_shallow(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "baseline"
            build_campaign_log(archive, seconds_per_rollup=400.0, rollup_count=120)
            state = {
                "variants": {
                    "baseline": {
                        "archive_path": str(archive),
                        "archive_log": str(archive / "debug.log"),
                        "recorded_at": "2026-07-06T14:00:00Z",
                    }
                }
            }
            check = self.mod.baseline_ibd_depth_gate(state)
            self.assertFalse(check.passed)
            self.assertIn("#250", check.detail)

    def test_evaluate_blocked_without_sweep_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp) / "logs"
            profile_logs.mkdir()
            checks = self.mod.evaluate_promotion_gates(profile_logs)
            self.assertFalse(self.mod.promotion_ready(checks))
            names = {c.name for c in checks if not c.passed}
            self.assertIn("recorded_prefetch_serial", names)
            self.assertIn("recorded_baseline", names)

    def test_main_json_exit_blocked(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp) / "logs"
            profile_logs.mkdir()
            proc = subprocess.run(
                [
                    sys.executable,
                    str(CONTRIB / "promotion_gates.py"),
                    "--profile-logs",
                    str(profile_logs),
                    "--json",
                ],
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertEqual(proc.returncode, 1)
            payload = json.loads(proc.stdout)
            self.assertFalse(payload["ready"])
            self.assertGreater(len(payload["operator_checklist"]), 0)

    def test_corrupt_sweep_state_reports_blocked(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp) / "logs"
            profile_logs.mkdir()
            (profile_logs / "phase6-sweep-state.json").write_text("{bad", encoding="utf-8")
            checks = self.mod.evaluate_promotion_gates(profile_logs)
            self.assertFalse(self.mod.promotion_ready(checks))
            gate = next(c for c in checks if c.name == "sweep_state_load")
            self.assertFalse(gate.passed)
            self.assertIn("corrupt state file", gate.detail)

    def test_corrupt_sweep_state_cli_blocked_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_logs = Path(tmp) / "logs"
            profile_logs.mkdir()
            (profile_logs / "phase6-sweep-state.json").write_text("{bad", encoding="utf-8")
            proc = subprocess.run(
                [
                    sys.executable,
                    str(CONTRIB / "promotion_gates.py"),
                    "--profile-logs",
                    str(profile_logs),
                ],
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertEqual(proc.returncode, 1)
            self.assertIn("status\tblocked", proc.stdout)
            self.assertIn("gate\tsweep_state_load\tfail", proc.stdout)

    def test_baseline_prefetch_gate_pass_from_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "baseline"
            build_campaign_log(archive, seconds_per_rollup=400.0, rollup_count=120)
            state = {
                "variants": {
                    "baseline": {
                        "archive_path": str(archive),
                        "archive_log": str(archive / "debug.log"),
                        "recorded_at": "2026-07-06T14:00:00Z",
                    }
                }
            }
            check = self.mod.baseline_prefetch_gate(state)
            self.assertTrue(check.passed, check.detail)
            self.assertIn("#120", check.detail)

    def test_baseline_prefetch_gate_rejects_summary_only_without_log_rollup(self):
        """Summary-only sweep state must not pass promotion prefetch gate."""
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "baseline"
            build_campaign_log(archive, seconds_per_rollup=400.0, rollup_count=50)
            state = {
                "variants": {
                    "baseline": {
                        "archive_path": str(archive),
                        "archive_log": str(archive / "debug.log"),
                        "recorded_at": "2026-07-06T14:00:00Z",
                        "summary": {
                            "max_readers_full": 0,
                            "min_prefetch_hit": 5,
                        },
                    }
                }
            }
            check = self.mod.baseline_prefetch_gate(state)
            self.assertFalse(check.passed)
            self.assertIn("#120", check.detail)

    def test_baseline_prefetch_gate_fail_unhealthy_rollup_120(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "baseline"
            build_campaign_log(
                archive,
                seconds_per_rollup=400.0,
                rollup_count=120,
                readers_full=1,
                prefetch_hit=0,
            )
            state = {
                "variants": {
                    "baseline": {
                        "archive_path": str(archive),
                        "archive_log": str(archive / "debug.log"),
                        "recorded_at": "2026-07-06T14:00:00Z",
                    }
                }
            }
            check = self.mod.baseline_prefetch_gate(state)
            self.assertFalse(check.passed)
            self.assertIn("readers_full=1", check.detail)

    def test_debug_only_multiline_addarg_span(self):
        """Multi-line AddArg with strprintf must still detect DEBUG_ONLY."""
        with tempfile.TemporaryDirectory() as tmp:
            init_cpp = Path(tmp) / "init.cpp"
            init_cpp.write_text(
                'argsman.AddArg("-utxoencodepar=<n>",\n'
                '               strprintf("Parallel UTXO encode worker threads (default: %d)", 0),\n'
                "               ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, "
                "OptionsCategory::OPTIONS);\n",
                encoding="utf-8",
            )
            checks = self.mod.debug_only_flags_present(init_cpp)
            utxo = next(c for c in checks if c.name == "debug_only_utxoencodepar")
            self.assertTrue(utxo.passed, utxo.detail)

    def test_debug_only_all_four_required(self):
        """Promotion gate fails when any of the four flags lacks DEBUG_ONLY."""
        with tempfile.TemporaryDirectory() as tmp:
            init_cpp = Path(tmp) / "init.cpp"
            init_cpp.write_text(
                'argsman.AddArg("-benchstats=<n>", "x", '
                "ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::OPTIONS);\n"
                'argsman.AddArg("-coinprefetchpar=<n>", "x", '
                "ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::OPTIONS);\n"
                'argsman.AddArg("-blockdecompresspar=<n>", "x", '
                "ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::OPTIONS);\n"
                'argsman.AddArg("-utxoencodepar=<n>", "x", '
                "ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);\n",
                encoding="utf-8",
            )
            checks = self.mod.debug_only_flags_present(init_cpp)
            self.assertEqual(len(checks), 4)
            failed = [c for c in checks if not c.passed]
            self.assertEqual(len(failed), 1)
            self.assertEqual(failed[0].name, "debug_only_utxoencodepar")


if __name__ == "__main__":
    unittest.main()