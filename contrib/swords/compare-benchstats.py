#!/usr/bin/env python3
"""Compare benchstats milestones: current debug.log vs archived log."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchstats_parse


def pct_delta(current: float | None, baseline: float | None) -> float | None:
    if current is None or baseline is None or baseline == 0:
        return None
    return round((current - baseline) / baseline * 100.0, 1)


def milestone_compare_rows(
    current_series: list[dict],
    archive_series: list[dict],
    milestones: tuple[int, ...],
) -> list[dict]:
    cur_by = {int(r["rollup_index"]): r for r in current_series}
    arc_by = {int(r["rollup_index"]): r for r in archive_series}
    cur_ms = benchstats_parse.rollup_milestones(current_series, milestones)
    arc_ms = benchstats_parse.rollup_milestones(archive_series, milestones)
    cur_map = {m["rollup_index"]: m for m in cur_ms}
    arc_map = {m["rollup_index"]: m for m in arc_ms}

    rows: list[dict] = []
    for idx in milestones:
        cur_m = cur_map.get(idx)
        arc_m = arc_map.get(idx)
        cur_r = cur_by.get(idx, {})
        arc_r = arc_by.get(idx, {})
        cur_implied = benchstats_parse.milestone_implied_blk_per_s(cur_r) if cur_r else None
        arc_implied = benchstats_parse.milestone_implied_blk_per_s(arc_r) if arc_r else None
        row = {
            "rollup_index": idx,
            "current_wall_s": cur_m.get("wall_s") if cur_m else None,
            "archive_wall_s": arc_m.get("wall_s") if arc_m else None,
            "current_disk_per_blk_ms": cur_m.get("disk_per_blk_ms") if cur_m else None,
            "archive_disk_per_blk_ms": arc_m.get("disk_per_blk_ms") if arc_m else None,
            "disk_delta_pct": pct_delta(
                cur_m.get("disk_per_blk_ms") if cur_m else None,
                arc_m.get("disk_per_blk_ms") if arc_m else None,
            ),
            "current_connect_cs_per_blk_ms": cur_m.get("connect_cs_per_blk_ms") if cur_m else None,
            "archive_connect_cs_per_blk_ms": arc_m.get("connect_cs_per_blk_ms") if arc_m else None,
            "connect_cs_delta_pct": pct_delta(
                cur_m.get("connect_cs_per_blk_ms") if cur_m else None,
                arc_m.get("connect_cs_per_blk_ms") if arc_m else None,
            ),
            "current_flush_lmdb_per_blk_ms": cur_m.get("flush_lmdb_per_blk_ms") if cur_m else None,
            "archive_flush_lmdb_per_blk_ms": arc_m.get("flush_lmdb_per_blk_ms") if arc_m else None,
            "flush_lmdb_delta_pct": pct_delta(
                cur_m.get("flush_lmdb_per_blk_ms") if cur_m else None,
                arc_m.get("flush_lmdb_per_blk_ms") if arc_m else None,
            ),
            "current_implied_blk_per_s": cur_implied,
            "archive_implied_blk_per_s": arc_implied,
            "implied_blk_per_s_delta_pct": pct_delta(cur_implied, arc_implied),
            "current_readers_full": cur_m.get("readers_full") if cur_m else None,
            "archive_readers_full": arc_m.get("readers_full") if arc_m else None,
            "current_prefetch_hit": cur_m.get("prefetch_hit") if cur_m else None,
            "archive_prefetch_hit": arc_m.get("prefetch_hit") if arc_m else None,
        }
        rows.append(row)
    return rows


def print_compare_table(
    current_log: Path,
    archive_log: Path,
    rows: list[dict],
    cur_summary: dict,
    arc_summary: dict,
) -> None:
    print(f"current_log\t{current_log}")
    print(f"archive_log\t{archive_log}")
    print(
        "summary_implied_blk_per_s\t"
        f"current={cur_summary.get('implied_blk_per_s')} "
        f"archive={arc_summary.get('implied_blk_per_s')}"
    )
    print(
        "summary_prefetch_health\t"
        f"current_max_readers_full={cur_summary.get('max_readers_full', 0)} "
        f"archive_max_readers_full={arc_summary.get('max_readers_full', 0)} "
        f"current_min_prefetch_hit={cur_summary.get('min_prefetch_hit', 0)} "
        f"archive_min_prefetch_hit={arc_summary.get('min_prefetch_hit', 0)}"
    )
    print(
        "milestone\twall_s\t\tdisk_ms/blk\t\tconnect_cs_ms/blk\t"
        "flush_lmdb_ms/blk\timplied_blk/s\t(delta%)\treaders_full\tprefetch_hit"
    )
    for row in rows:
        idx = row["rollup_index"]
        cw = row.get("current_wall_s")
        aw = row.get("archive_wall_s")
        wall = ""
        if cw is not None or aw is not None:
            cw_s = "" if cw is None else f"{cw:.1f}"
            aw_s = "" if aw is None else f"{aw:.1f}"
            wall = f"{cw_s}/{aw_s}"

        def pair(cur_key: str, arc_key: str, delta_key: str) -> str:
            cur = row.get(cur_key)
            arc = row.get(arc_key)
            delta = row.get(delta_key)
            cur_s = "" if cur is None else f"{cur:.2f}"
            arc_s = "" if arc is None else f"{arc:.2f}"
            delta_s = "" if delta is None else f"{delta:+.1f}%"
            return f"{cur_s}/{arc_s} ({delta_s})" if delta_s else f"{cur_s}/{arc_s}"

        disk = pair("current_disk_per_blk_ms", "archive_disk_per_blk_ms", "disk_delta_pct")
        connect = pair(
            "current_connect_cs_per_blk_ms",
            "archive_connect_cs_per_blk_ms",
            "connect_cs_delta_pct",
        )
        flush = pair(
            "current_flush_lmdb_per_blk_ms",
            "archive_flush_lmdb_per_blk_ms",
            "flush_lmdb_delta_pct",
        )
        implied = pair(
            "current_implied_blk_per_s",
            "archive_implied_blk_per_s",
            "implied_blk_per_s_delta_pct",
        )

        def int_pair(cur_key: str, arc_key: str) -> str:
            cur = row.get(cur_key)
            arc = row.get(arc_key)
            cur_s = "" if cur is None else str(cur)
            arc_s = "" if arc is None else str(arc)
            return f"{cur_s}/{arc_s}"

        readers = int_pair("current_readers_full", "archive_readers_full")
        prefetch = int_pair("current_prefetch_hit", "archive_prefetch_hit")
        print(f"#{idx}\t{wall}\t{disk}\t{connect}\t{flush}\t{implied}\t{readers}\t{prefetch}")


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Compare benchstats milestones between current and archived debug.log",
    )
    ap.add_argument(
        "datadir",
        nargs="?",
        default=None,
        help="current datadir or debug.log (default: SWORDS_DATADIR)",
    )
    ap.add_argument("--log", dest="log_path", default=None, help="explicit current debug.log path")
    ap.add_argument(
        "--archive",
        default="latest",
        help="archive dir/path or 'latest' (default: profile-logs/latest)",
    )
    ap.add_argument(
        "--archive-log",
        dest="archive_log_path",
        default=None,
        help="explicit archived debug.log path (overrides --archive)",
    )
    ap.add_argument("--json", action="store_true", help="emit JSON comparison")
    ap.add_argument(
        "--milestones",
        default=benchstats_parse.default_milestones_arg(),
        help=f"comma-separated rollup indices (default: {benchstats_parse.default_milestones_arg()})",
    )
    args = ap.parse_args()

    milestones = benchstats_parse.parse_milestones_arg(args.milestones)
    datadir_or_log = args.datadir if args.datadir is not None else str(benchstats_parse.default_datadir())
    current_log = benchstats_parse.resolve_log_path(datadir_or_log, args.log_path)
    if not current_log.exists():
        print(f"Missing current log {current_log}", file=sys.stderr)
        sys.exit(1)

    try:
        if args.archive_log_path:
            archive_log = Path(args.archive_log_path)
            if not archive_log.is_file():
                raise FileNotFoundError(f"Missing archive log {archive_log}")
        else:
            archive_log = benchstats_parse.resolve_archive_log(args.archive)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)

    current_series = benchstats_parse.parse_benchstats_series(current_log)
    archive_series = benchstats_parse.parse_benchstats_series(archive_log)
    rows = milestone_compare_rows(current_series, archive_series, milestones)
    cur_summary = benchstats_parse.benchstats_summary(current_series)
    arc_summary = benchstats_parse.benchstats_summary(archive_series)

    if args.json:
        payload = {
            "current_log": str(current_log),
            "archive_log": str(archive_log),
            "milestones": rows,
            "current_summary": cur_summary,
            "archive_summary": arc_summary,
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
        return

    print_compare_table(current_log, archive_log, rows, cur_summary, arc_summary)


if __name__ == "__main__":
    main()