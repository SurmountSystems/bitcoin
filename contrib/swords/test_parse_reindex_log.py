#!/usr/bin/env python3
"""Unit tests for contrib/swords/parse-reindex-log.py."""
from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "contrib" / "swords" / "parse-reindex-log.py"


def load_parser():
    spec = importlib.util.spec_from_file_location("parse_reindex_log", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class ParseReindexLogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.parser = load_parser()

    def _write_log(self, lines: list[str]) -> Path:
        tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False)
        with tmp:
            tmp.write("\n".join(lines))
            tmp.write("\n")
        return Path(tmp.name)

    def test_dict_bootstrap_without_load_lines(self):
        log = self._write_log([
            "=== Dictionary bootstrap pass 1 complete (IBD wall time: 42 seconds) ===",
            "  block BLK_P2TR_POST_ORD plaintext_bytes=4096",
            "  utxo UTXO_P2WPKH plaintext_bytes=0",
            "Dictionary bootstrap pass 1 complete; restart with -reindex for compression pass 2",
        ])
        stats = self.parser.parse_log(log)
        self.assertEqual(stats["blocks"], 0)
        self.assertIsNotNone(stats["dict_bootstrap"])
        db = stats["dict_bootstrap"]
        self.assertEqual(db["state"], "pass1_complete")
        self.assertEqual(db["pass1_wall_seconds"], 42)
        self.assertEqual(db["block_plaintext_bytes"]["BLK_P2TR_POST_ORD"], 4096)
        self.assertEqual(
            db["next_step"],
            "restart with -reindex for compression pass 2",
        )

    def test_segment_replay_dict_includes_dict_bootstrap_when_blocks_lt_2(self):
        stats = {
            "blocks": 0,
            "load_ms": [],
            "connect_ms": [],
            "blocks_per_hr": 0.0,
            "dict_bootstrap": {
                "state": "pass1_complete",
                "pass1_wall_seconds": 7,
                "block_plaintext_bytes": {},
                "utxo_plaintext_bytes": {},
            },
        }
        seg = self.parser.segment_replay_dict(stats)
        self.assertIsNotNone(seg)
        self.assertIn("dict_bootstrap", seg)
        self.assertNotIn("blocks", seg)

    def test_compression_report_pass2(self):
        log = self._write_log([
            "=== Swords dictionary effectiveness report (pass 2) ===",
            "Block buckets:",
            "  BLK_P2TR_POST_ORD: dict=384KiB ratio=3.42 holdout=2.50 plaintext=4KiB stored=1KiB saved=71%",
            "UTXO buckets:",
            "  UTXO_P2WPKH: dict=48KiB ratio=2.10 holdout=n/a plaintext=256B stored=96B saved=62%",
            "Global:",
            "  blocks_plaintext_pass1=4KiB blocks_stored_pass2=1KiB savings=75%",
            "  utxo_plaintext_pass1=256B utxo_stored_pass2=96B savings=62%",
            "  pass1_ibd_hours=1.50 pass2_reindex_hours=0.25",
            "Recommendations:",
            "  BLK_P2TR_POST_ORD: holdout=3.50 at 3840KiB (94% of 4096KiB max); pass 2 savings=27% < 50% — rerun pass 1 IBD for more samples",
            "Dictionary bootstrap pass 2 complete; typed dictionary compression active",
        ])
        stats = self.parser.parse_log(log)
        self.assertEqual(stats["blocks"], 0)
        self.assertIsNotNone(stats["compression_report"])
        report = stats["compression_report"]
        self.assertEqual(report["state"], "complete")
        self.assertEqual(
            report["block_buckets"]["BLK_P2TR_POST_ORD"]["saved_percent"],
            71,
        )
        self.assertEqual(
            report["utxo_buckets"]["UTXO_P2WPKH"]["ratio"],
            2.10,
        )
        self.assertEqual(
            report["block_buckets"]["BLK_P2TR_POST_ORD"]["holdout"],
            2.50,
        )
        self.assertIsNone(report["utxo_buckets"]["UTXO_P2WPKH"]["holdout"])
        self.assertEqual(report["global"]["pass2_reindex_hours"], 0.25)
        self.assertEqual(len(report["recommendations"]), 1)
        self.assertIn("BLK_P2TR_POST_ORD", report["recommendations"][0])

    def test_compression_report_pass2_with_timestamp_prefix(self):
        log = self._write_log([
            "2026-07-05T12:34:56.789Z === Swords dictionary effectiveness report (pass 2) ===",
            "2026-07-05T12:34:56.790Z Block buckets:",
            "2026-07-05T12:34:56.791Z   BLK_P2TR_POST_ORD: dict=384KiB ratio=3.42 holdout=2.50 plaintext=4KiB stored=1KiB saved=71%",
            "2026-07-05T12:34:56.792Z Recommendations:",
            "2026-07-05T12:34:56.793Z   BLK_P2TR_POST_ORD: holdout=3.50 at 3840KiB (94% of 4096KiB max); pass 2 savings=27% < 50% — rerun pass 1 IBD for more samples",
            "2026-07-05T12:34:56.794Z Dictionary bootstrap pass 2 complete; typed dictionary compression active",
        ])
        stats = self.parser.parse_log(log)
        report = stats["compression_report"]
        self.assertIsNotNone(report)
        self.assertEqual(
            report["block_buckets"]["BLK_P2TR_POST_ORD"]["saved_percent"],
            71,
        )
        self.assertEqual(
            report["block_buckets"]["BLK_P2TR_POST_ORD"]["holdout"],
            2.50,
        )
        self.assertEqual(len(report["recommendations"]), 1)
        self.assertIn("BLK_P2TR_POST_ORD", report["recommendations"][0])

    def test_compression_report_blocks_zero_segment_replay(self):
        stats = {
            "blocks": 0,
            "load_ms": [],
            "connect_ms": [],
            "blocks_per_hr": 0.0,
            "compression_report": {
                "state": "complete",
                "block_buckets": {},
                "utxo_buckets": {},
                "global": {"pass2_reindex_hours": 0.5},
            },
        }
        seg = self.parser.segment_replay_dict(stats)
        self.assertIsNotNone(seg)
        self.assertIn("compression_report", seg)
        self.assertNotIn("blocks", seg)

    def test_latest_pass2_banner_resets_bucket_accumulators(self):
        log = self._write_log([
            "=== Swords dictionary effectiveness report (pass 2) ===",
            "Block buckets:",
            "  BLK_SCRIPTSIG: dict=32KiB ratio=1.10 holdout=1.20 plaintext=1KiB stored=900B saved=12%",
            "=== Swords dictionary effectiveness report (pass 2) ===",
            "Block buckets:",
            "  BLK_P2TR_POST_ORD: dict=384KiB ratio=3.42 holdout=2.50 plaintext=4KiB stored=1KiB saved=71%",
            "Dictionary bootstrap pass 2 complete; typed dictionary compression active",
        ])
        stats = self.parser.parse_log(log)
        report = stats["compression_report"]
        self.assertEqual(
            list(report["block_buckets"].keys()),
            ["BLK_P2TR_POST_ORD"],
        )

    def test_latest_completion_banner_resets_bucket_accumulators(self):
        log = self._write_log([
            "=== Dictionary bootstrap pass 1 complete (IBD wall time: 10 seconds) ===",
            "  block BLK_SCRIPTSIG plaintext_bytes=999",
            "=== Dictionary bootstrap pass 1 complete (IBD wall time: 20 seconds) ===",
            "  block BLK_P2TR_POST_ORD plaintext_bytes=512",
        ])
        stats = self.parser.parse_log(log)
        db = stats["dict_bootstrap"]
        self.assertEqual(db["pass1_wall_seconds"], 20)
        self.assertEqual(db["block_plaintext_bytes"], {"BLK_P2TR_POST_ORD": 512})
        self.assertNotIn("BLK_SCRIPTSIG", db["block_plaintext_bytes"])


if __name__ == "__main__":
    unittest.main()