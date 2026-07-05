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


def parse_ts(line: str) -> datetime | None:
    m = TS_RE.match(line)
    if not m:
        return None
    raw = m.group("ts")
    if "." in raw:
        return datetime.strptime(raw, "%Y-%m-%dT%H:%M:%S.%fZ")
    return datetime.strptime(raw, "%Y-%m-%dT%H:%M:%SZ")


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
        "blocks_per_hr": blocks_per_hr,
        "first_ts": first_ts,
        "last_ts": last_ts,
    }


def print_summary(stats: dict, path: Path) -> None:
    blocks = stats["blocks"]
    if not blocks:
        print(f"No 'Load block from disk' lines in {path}", file=sys.stderr)
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


def segment_replay_dict(stats: dict) -> dict | None:
    if stats["blocks"] < 2:
        return None
    out: dict = {
        "blocks": stats["blocks"],
        "p50_load_ms": round(p50(stats["load_ms"]), 2),
        "p50_connect_ms": round(p50(stats["connect_ms"]), 2),
    }
    if stats["blocks_per_hr"] > 0:
        out["blocks_per_hr"] = round(stats["blocks_per_hr"], 1)
    return out


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