#!/usr/bin/env python3
"""Parse debug.log bench/reindex lines into a blocks/hr summary table."""
from __future__ import annotations

import argparse
import re
import sys
from datetime import datetime
from pathlib import Path

TS_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z)\s+"
)
LOAD_RE = re.compile(r"Load block from disk: ([0-9.]+)ms")
CONNECT_RE = re.compile(r"Connect (\d+) transactions: ([0-9.]+)ms")
BATCH_ENCODE_RE = re.compile(
    r"BatchWrite: encode coins for db batch completed \(([0-9.]+)ms"
)
BATCH_WRITE_RE = re.compile(
    r"write coins (?:partial|final) batch to LMDB completed \(([0-9.]+)ms\)"
)
BENCHSTATS_RE = re.compile(r"^benchstats:")
DICT_BOOTSTRAP_COMPLETE_RE = re.compile(
    r"=== Dictionary bootstrap pass 1 complete \(IBD wall time: (\d+) seconds\) ==="
)
DICT_BOOTSTRAP_BLOCK_BYTES_RE = re.compile(
    r"block (\S+) plaintext_bytes=(\d+)"
)
DICT_BOOTSTRAP_UTXO_BYTES_RE = re.compile(
    r"utxo (\S+) plaintext_bytes=(\d+)"
)
DICT_BOOTSTRAP_NEXT_STEP_RE = re.compile(
    r"restart with -reindex for compression pass 2"
)
COMPRESSION_REPORT_RE = re.compile(
    r"=== Swords dictionary effectiveness report \(pass 2\) ==="
)
COMPRESSION_REPORT_BUCKET_WITH_HOLDOUT_RE = re.compile(
    r"^\s+(\S+): dict=(\S+) ratio=([0-9.]+) holdout=([0-9.]+) plaintext=(\S+) stored=(\S+) saved=(-?\d+)%"
)
COMPRESSION_REPORT_BUCKET_NO_HOLDOUT_RE = re.compile(
    r"^\s+(\S+): dict=(\S+) ratio=([0-9.]+) holdout=n/a plaintext=(\S+) stored=(\S+) saved=(-?\d+)%"
)
COMPRESSION_REPORT_GLOBAL_RE = re.compile(
    r"blocks_plaintext_pass1=(\S+) blocks_stored_pass2=(\S+) savings=(-?\d+)%"
)
COMPRESSION_REPORT_UTXO_GLOBAL_RE = re.compile(
    r"utxo_plaintext_pass1=(\S+) utxo_stored_pass2=(\S+) savings=(-?\d+)%"
)
COMPRESSION_REPORT_TIMING_RE = re.compile(
    r"pass1_ibd_hours=([0-9.]+) pass2_reindex_hours=([0-9.]+)"
)
COMPRESSION_REPORT_RECOMMENDATION_RE = re.compile(r"^\s+(.+)$")
COMPRESSION_REPORT_COMPLETE_RE = re.compile(
    r"Dictionary bootstrap pass 2 complete"
)


def parse_ts(line: str) -> datetime | None:
    m = TS_RE.match(line)
    if not m:
        return None
    raw = m.group("ts")
    if "." in raw:
        return datetime.strptime(raw, "%Y-%m-%dT%H:%M:%S.%fZ")
    return datetime.strptime(raw, "%Y-%m-%dT%H:%M:%SZ")


def strip_ts_prefix(line: str) -> str:
    """Return log message body with leading ISO timestamp removed, if present."""
    m = TS_RE.match(line)
    if m:
        # TS_RE ends with \\s+; strip only the timestamp and one separator space
        # so indented pass 2 bucket/recommendation lines keep leading whitespace.
        ts_end = m.end("ts")
        if ts_end < len(line) and line[ts_end] == " ":
            return line[ts_end + 1 :]
        return line[ts_end:]
    return line


def p50(vals: list[float]) -> float:
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[len(s) // 2]


def parse_log(path: Path) -> dict:
    blocks = 0
    load_ms: list[float] = []
    connect_ms: list[float] = []
    batch_encode_ms: list[float] = []
    batch_write_ms: list[float] = []
    benchstats: list[str] = []
    dict_bootstrap: dict | None = None
    dict_block_bytes: dict[str, int] = {}
    dict_utxo_bytes: dict[str, int] = {}
    dict_next_step = False
    compression_report: dict | None = None
    compression_block_buckets: dict[str, dict] = {}
    compression_utxo_buckets: dict[str, dict] = {}
    compression_global: dict[str, str | int | float] = {}
    compression_recommendations: list[str] = []
    in_compression_report = False
    compression_section: str | None = None
    first_ts: datetime | None = None
    last_ts: datetime | None = None

    for line in path.read_text(errors="replace").splitlines():
        ts = parse_ts(line)
        if m := LOAD_RE.search(line):
            load_ms.append(float(m.group(1)))
            blocks += 1
            if ts is not None:
                if first_ts is None:
                    first_ts = ts
                last_ts = ts
        if m := CONNECT_RE.search(line):
            connect_ms.append(float(m.group(2)))
        if m := BATCH_ENCODE_RE.search(line):
            batch_encode_ms.append(float(m.group(1)))
        if m := BATCH_WRITE_RE.search(line):
            batch_write_ms.append(float(m.group(1)))
        if BENCHSTATS_RE.search(line):
            benchstats.append(line)
        if m := DICT_BOOTSTRAP_COMPLETE_RE.search(line):
            dict_bootstrap = {"pass1_wall_seconds": int(m.group(1))}
            dict_block_bytes = {}
            dict_utxo_bytes = {}
            dict_next_step = False
        if m := DICT_BOOTSTRAP_BLOCK_BYTES_RE.search(line):
            dict_block_bytes[m.group(1)] = int(m.group(2))
        if m := DICT_BOOTSTRAP_UTXO_BYTES_RE.search(line):
            dict_utxo_bytes[m.group(1)] = int(m.group(2))
        if DICT_BOOTSTRAP_NEXT_STEP_RE.search(line):
            dict_next_step = True
        if COMPRESSION_REPORT_RE.search(strip_ts_prefix(line)):
            compression_report = {"state": "complete"}
            compression_block_buckets = {}
            compression_utxo_buckets = {}
            compression_global = {}
            compression_recommendations = []
            in_compression_report = True
            compression_section = None
        if in_compression_report:
            body = strip_ts_prefix(line)
            if "Block buckets:" in body:
                compression_section = "block"
            elif "UTXO buckets:" in body:
                compression_section = "utxo"
            elif "Global:" in body:
                compression_section = "global"
            elif "Recommendations:" in body:
                compression_section = "recommendations"
            elif m := COMPRESSION_REPORT_BUCKET_WITH_HOLDOUT_RE.search(body):
                entry = {
                    "dict": m.group(2),
                    "ratio": float(m.group(3)),
                    "holdout": float(m.group(4)),
                    "plaintext": m.group(5),
                    "stored": m.group(6),
                    "saved_percent": int(m.group(7)),
                }
                if compression_section == "block":
                    compression_block_buckets[m.group(1)] = entry
                elif compression_section == "utxo":
                    compression_utxo_buckets[m.group(1)] = entry
            elif m := COMPRESSION_REPORT_BUCKET_NO_HOLDOUT_RE.search(body):
                entry = {
                    "dict": m.group(2),
                    "ratio": float(m.group(3)),
                    "holdout": None,
                    "plaintext": m.group(4),
                    "stored": m.group(5),
                    "saved_percent": int(m.group(6)),
                }
                if compression_section == "block":
                    compression_block_buckets[m.group(1)] = entry
                elif compression_section == "utxo":
                    compression_utxo_buckets[m.group(1)] = entry
            elif m := COMPRESSION_REPORT_GLOBAL_RE.search(body):
                compression_global["blocks_plaintext_pass1"] = m.group(1)
                compression_global["blocks_stored_pass2"] = m.group(2)
                compression_global["blocks_savings_percent"] = int(m.group(3))
            elif m := COMPRESSION_REPORT_UTXO_GLOBAL_RE.search(body):
                compression_global["utxo_plaintext_pass1"] = m.group(1)
                compression_global["utxo_stored_pass2"] = m.group(2)
                compression_global["utxo_savings_percent"] = int(m.group(3))
            elif m := COMPRESSION_REPORT_TIMING_RE.search(body):
                compression_global["pass1_ibd_hours"] = float(m.group(1))
                compression_global["pass2_reindex_hours"] = float(m.group(2))
            elif compression_section == "recommendations":
                if m := COMPRESSION_REPORT_RECOMMENDATION_RE.match(body):
                    compression_recommendations.append(m.group(1))
            elif COMPRESSION_REPORT_COMPLETE_RE.search(body):
                in_compression_report = False

    if dict_bootstrap is not None:
        dict_bootstrap["block_plaintext_bytes"] = dict_block_bytes
        dict_bootstrap["utxo_plaintext_bytes"] = dict_utxo_bytes
        if dict_next_step:
            dict_bootstrap["next_step"] = "restart with -reindex for compression pass 2"
        dict_bootstrap["state"] = "pass1_complete"

    if compression_report is not None:
        compression_report["block_buckets"] = compression_block_buckets
        compression_report["utxo_buckets"] = compression_utxo_buckets
        compression_report["global"] = compression_global
        if compression_recommendations:
            compression_report["recommendations"] = compression_recommendations

    blocks_per_hr = 0.0
    if blocks >= 2 and first_ts is not None and last_ts is not None:
        elapsed_s = (last_ts - first_ts).total_seconds()
        if elapsed_s > 0:
            blocks_per_hr = blocks / elapsed_s * 3600.0

    return {
        "blocks": blocks,
        "load_ms": load_ms,
        "connect_ms": connect_ms,
        "batch_encode_ms": batch_encode_ms,
        "batch_write_ms": batch_write_ms,
        "benchstats": benchstats,
        "dict_bootstrap": dict_bootstrap,
        "compression_report": compression_report,
        "blocks_per_hr": blocks_per_hr,
        "first_ts": first_ts,
        "last_ts": last_ts,
    }


def print_compression_report(stats: dict) -> None:
    if not stats.get("compression_report"):
        return
    report = stats["compression_report"]
    print(f"compression_report_state\t{report.get('state', '')}")
    for name, entry in sorted(report.get("block_buckets", {}).items()):
        holdout = entry.get("holdout")
        holdout_str = "" if holdout is None else holdout
        print(
            f"compression_report_block\t{name}\t"
            f"{entry.get('dict', '')}\t{entry.get('ratio', 0)}\t"
            f"{holdout_str}\t{entry.get('plaintext', '')}\t{entry.get('stored', '')}\t"
            f"{entry.get('saved_percent', 0)}"
        )
    for name, entry in sorted(report.get("utxo_buckets", {}).items()):
        holdout = entry.get("holdout")
        holdout_str = "" if holdout is None else holdout
        print(
            f"compression_report_utxo\t{name}\t"
            f"{entry.get('dict', '')}\t{entry.get('ratio', 0)}\t"
            f"{holdout_str}\t{entry.get('plaintext', '')}\t{entry.get('stored', '')}\t"
            f"{entry.get('saved_percent', 0)}"
        )
    for key, value in sorted(report.get("global", {}).items()):
        print(f"compression_report_global\t{key}\t{value}")
    for recommendation in report.get("recommendations", []):
        print(f"compression_report_recommendation\t{recommendation}")


def print_dict_bootstrap(stats: dict) -> None:
    if not stats.get("dict_bootstrap"):
        return
    db = stats["dict_bootstrap"]
    print(f"dict_bootstrap_state\t{db.get('state', '')}")
    print(f"dict_bootstrap_wall_seconds\t{db.get('pass1_wall_seconds', 0)}")
    if db.get("next_step"):
        print(f"dict_bootstrap_next_step\t{db['next_step']}")
    for name, nbytes in sorted(db.get("block_plaintext_bytes", {}).items()):
        print(f"dict_bootstrap_block_bytes\t{name}\t{nbytes}")
    for name, nbytes in sorted(db.get("utxo_plaintext_bytes", {}).items()):
        print(f"dict_bootstrap_utxo_bytes\t{name}\t{nbytes}")


def print_summary(stats: dict, path: Path) -> None:
    blocks = stats["blocks"]
    if not blocks:
        print(f"No 'Load block from disk' lines in {path}", file=sys.stderr)
        print_dict_bootstrap(stats)
        print_compression_report(stats)
        return

    print("metric\tvalue")
    print(f"blocks\t{blocks}")
    print(f"p50_load_ms\t{p50(stats['load_ms']):.2f}")
    print(f"p50_connect_ms\t{p50(stats['connect_ms']):.2f}")
    print(f"p50_batch_encode_ms\t{p50(stats['batch_encode_ms']):.2f}")
    print(f"p50_batch_write_ms\t{p50(stats['batch_write_ms']):.2f}")
    if stats["blocks_per_hr"] > 0:
        print(f"blocks_per_hr\t{stats['blocks_per_hr']:.1f}")
    if stats["first_ts"] and stats["last_ts"]:
        print(f"segment_start\t{stats['first_ts'].isoformat()}Z")
        print(f"segment_end\t{stats['last_ts'].isoformat()}Z")
    for line in stats["benchstats"]:
        print(line)
    print_dict_bootstrap(stats)
    print_compression_report(stats)


def segment_replay_dict(stats: dict) -> dict | None:
    out: dict = {}
    if stats["blocks"] >= 2:
        out = {
            "blocks": stats["blocks"],
            "p50_load_ms": round(p50(stats["load_ms"]), 2),
            "p50_connect_ms": round(p50(stats["connect_ms"]), 2),
        }
        if stats["blocks_per_hr"] > 0:
            out["blocks_per_hr"] = round(stats["blocks_per_hr"], 1)
    if stats.get("dict_bootstrap"):
        out["dict_bootstrap"] = stats["dict_bootstrap"]
    if stats.get("compression_report"):
        out["compression_report"] = stats["compression_report"]
    return out or None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("datadir", nargs="?", default=str(Path.home() / ".bitcoin-swords"))
    ap.add_argument("--json", action="store_true", help="emit segment_replay JSON only")
    args = ap.parse_args()
    log = Path(args.datadir) / "debug.log"
    if not log.exists():
        print(f"Missing {log}", file=sys.stderr)
        sys.exit(1)
    stats = parse_log(log)
    if args.json:
        import json

        seg = segment_replay_dict(stats)
        print(json.dumps(seg or {}, indent=2, sort_keys=True))
        return
    print_summary(stats, log)


if __name__ == "__main__":
    main()