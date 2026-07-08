#!/usr/bin/env python3
"""Parse benchstats rollups from Swords debug.log."""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchstats_parse


CSV_COLUMNS = (
    "rollup_index",
    "timestamp",
    "wall_s",
    "interval_wall_ms",
    "unaccounted_ms",
    "interval_implied_blk_per_s",
    "blocks",
    "disk",
    "decompress",
    "connect_cs",
    "flush_lmdb",
    "txindex",
    "cs_main_hold",
    "flush_mutex_wait",
    "blkidx_mutex_wait",
    "abc_idle",
    "script_jobs",
    "script_done",
    "disk_per_blk_ms",
    "connect_cs_per_blk_ms",
    "flush_lmdb_per_blk_ms",
    "txindex_per_blk_ms",
    "cs_main_hold_per_blk_ms",
    "flush_mutex_wait_per_blk_ms",
    "blkidx_mutex_wait_per_blk_ms",
    "abc_idle_per_blk_ms",
    "parallel",
    "stale",
    "readers_full",
    "prefetch_hit",
    "blkidx_sync",
    "par_jobs",
)


def series_csv_rows(series: list[dict]) -> list[dict]:
    rows: list[dict] = []
    for rollup in series:
        blocks = int(rollup.get("blocks", 0))
        row = {col: rollup.get(col) for col in CSV_COLUMNS if col in rollup}
        row["rollup_index"] = rollup["rollup_index"]
        row["timestamp"] = rollup.get("timestamp")
        row["wall_s"] = rollup.get("wall_s")
        row["interval_wall_ms"] = rollup.get("interval_wall_ms")
        row["unaccounted_ms"] = rollup.get("unaccounted_ms")
        if "interval_implied_blk_per_s" in rollup:
            row["interval_implied_blk_per_s"] = rollup["interval_implied_blk_per_s"]
        row["blocks"] = blocks
        for int_field in (
            "parallel",
            "stale",
            "readers_full",
            "prefetch_hit",
            "blkidx_sync",
            "par_jobs",
        ):
            if int_field in rollup:
                row[int_field] = rollup[int_field]
        row["disk"] = rollup.get("disk", 0.0)
        row["decompress"] = rollup.get("decompress", 0.0)
        row["connect_cs"] = rollup.get("connect_cs", 0.0)
        row["flush_lmdb"] = rollup.get("flush_lmdb", 0.0)
        row["txindex"] = rollup.get("txindex", 0.0)
        row["cs_main_hold"] = rollup.get("cs_main_hold", 0.0)
        row["flush_mutex_wait"] = rollup.get("flush_mutex_wait", 0.0)
        row["blkidx_mutex_wait"] = rollup.get("blkidx_mutex_wait", 0.0)
        row["abc_idle"] = rollup.get("abc_idle", 0.0)
        for int_field in ("script_jobs", "script_done"):
            if int_field in rollup:
                row[int_field] = rollup[int_field]
        if blocks > 0:
            row["disk_per_blk_ms"] = round(benchstats_parse.per_block_ms(rollup, "disk") or 0.0, 2)
            row["connect_cs_per_blk_ms"] = round(
                benchstats_parse.per_block_ms(rollup, "connect_cs") or 0.0, 2
            )
            row["flush_lmdb_per_blk_ms"] = round(
                benchstats_parse.per_block_ms(rollup, "flush_lmdb") or 0.0, 2
            )
            row["txindex_per_blk_ms"] = round(
                benchstats_parse.per_block_ms(rollup, "txindex") or 0.0, 2
            )
            row["cs_main_hold_per_blk_ms"] = round(
                benchstats_parse.per_block_ms(rollup, "cs_main_hold") or 0.0, 2
            )
            row["flush_mutex_wait_per_blk_ms"] = round(
                benchstats_parse.per_block_ms(rollup, "flush_mutex_wait") or 0.0, 2
            )
            row["blkidx_mutex_wait_per_blk_ms"] = round(
                benchstats_parse.per_block_ms(rollup, "blkidx_mutex_wait") or 0.0, 2
            )
            row["abc_idle_per_blk_ms"] = round(
                benchstats_parse.per_block_ms(rollup, "abc_idle") or 0.0, 2
            )
        rows.append(row)
    return rows


def print_table(
    series: list[dict],
    milestones: list[dict],
    summary: dict,
    run_metadata: dict,
    path: Path,
) -> None:
    print(f"log\t{path}")
    if run_metadata.get("run_started"):
        rs = run_metadata["run_started"]
        print(f"run_started\t{rs.get('run_ts', '')}\tbenchstats={rs.get('benchstats', '')}")
    if run_metadata.get("lmdb_parallelism"):
        lp = run_metadata["lmdb_parallelism"]
        print(
            "lmdb_parallelism\t"
            f"par={lp.get('par')} coinprefetchpar={lp.get('coinprefetchpar')} "
            f"utxoencodepar={lp.get('utxoencodepar')} blockdecompresspar={lp.get('blockdecompresspar')}"
        )
    if not series:
        print("benchstats_rollups\t0")
        return

    print(f"benchstats_rollups\t{len(series)}")
    print("summary_rollup_count\t{}".format(summary.get("rollup_count", 0)))
    if summary.get("implied_blk_per_s") is not None:
        print(f"summary_implied_blk_per_s\t{summary['implied_blk_per_s']}")
    print(
        "summary_p50_ms_per_blk\t"
        f"disk={summary.get('p50_disk_per_blk_ms', 0)} "
        f"connect_cs={summary.get('p50_connect_cs_per_blk_ms', 0)} "
        f"flush_lmdb={summary.get('p50_flush_lmdb_per_blk_ms', 0)} "
        f"txindex={summary.get('p50_txindex_per_blk_ms', 0)}"
    )
    print(
        "summary_prefetch_health\t"
        f"p50_readers_full={summary.get('p50_readers_full', 0)} "
        f"max_readers_full={summary.get('max_readers_full', 0)} "
        f"p50_prefetch_hit={summary.get('p50_prefetch_hit', 0)} "
        f"min_prefetch_hit={summary.get('min_prefetch_hit', 0)}"
    )

    print(
        "milestone\trollup\twall_s\tdisk_ms/blk\tconnect_cs_ms/blk\t"
        "flush_lmdb_ms/blk\timplied_blk_per_s\treaders_full\tprefetch_hit"
    )
    by_index = {int(r["rollup_index"]): r for r in series}
    for m in milestones:
        idx = m["rollup_index"]
        rollup = by_index.get(idx, {})
        implied = m.get("implied_blk_per_s")
        implied_str = "" if implied is None else f"{implied:.2f}"
        wall = m.get("wall_s")
        wall_str = "" if wall is None else f"{wall:.1f}"
        print(
            f"milestone\t#{idx}\t{wall_str}\t"
            f"{m.get('disk_per_blk_ms', 0):.2f}\t"
            f"{m.get('connect_cs_per_blk_ms', 0):.2f}\t"
            f"{m.get('flush_lmdb_per_blk_ms', 0):.2f}\t"
            f"{implied_str}\t"
            f"{m.get('readers_full', 0)}\t"
            f"{m.get('prefetch_hit', 0)}"
        )


def write_csv(rows: list[dict], out_path: Path | None) -> None:
    if out_path is not None:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with out_path.open("w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=CSV_COLUMNS, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(rows)
        return
    writer = csv.DictWriter(sys.stdout, fieldnames=CSV_COLUMNS, extrasaction="ignore")
    writer.writeheader()
    writer.writerows(rows)


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Parse benchstats rollups from debug.log",
        epilog=(
            "unaccounted_ms is omitted on rollup #1 (inter-rollup intervals only; "
            "rollup #1 would include startup wall time). "
            "CSV interval_implied_blk_per_s is per inter-rollup wall interval; "
            "milestone/summary implied_blk_per_s are cumulative/sustained rates."
        ),
    )
    ap.add_argument(
        "datadir",
        nargs="?",
        default=None,
        help="datadir (uses debug.log) or direct path to debug.log",
    )
    ap.add_argument("--log", dest="log_path", default=None, help="explicit debug.log path")
    ap.add_argument("--json", action="store_true", help="emit JSON (series + milestones + summary)")
    ap.add_argument("--csv", action="store_true", help="emit time series CSV to stdout or OUT=")
    ap.add_argument(
        "--milestones",
        default=benchstats_parse.default_milestones_arg(),
        help=f"comma-separated rollup indices (default: {benchstats_parse.default_milestones_arg()})",
    )
    args = ap.parse_args()

    datadir_or_log = args.datadir if args.datadir is not None else str(benchstats_parse.default_datadir())
    log = benchstats_parse.resolve_log_path(datadir_or_log, args.log_path)
    if not log.exists():
        print(f"Missing {log}", file=sys.stderr)
        sys.exit(1)

    milestones_arg = benchstats_parse.parse_milestones_arg(args.milestones)
    series = benchstats_parse.parse_benchstats_series(log)
    run_metadata = benchstats_parse.parse_run_metadata(log)
    milestones = benchstats_parse.rollup_milestones(series, milestones_arg)
    summary = benchstats_parse.benchstats_summary(series)

    if args.json:
        payload = {
            "log": str(log),
            "run_metadata": run_metadata,
            "milestones": milestones,
            "summary": summary,
            "series": series,
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
        return

    if args.csv:
        out_raw = os.environ.get("OUT", "").strip()
        out_path = Path(out_raw) if out_raw else None
        write_csv(series_csv_rows(series), out_path)
        return

    print_table(series, milestones, summary, run_metadata, log)


if __name__ == "__main__":
    main()