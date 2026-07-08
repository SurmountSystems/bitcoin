#!/usr/bin/env python3
"""Phase 3 operator report: benchstats summary, milestones, optional archive compare."""
from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchstats_parse

_SWORDS_DIR = Path(__file__).resolve().parent

# Hyphenated CLI scripts (compare-benchstats.py, parse-benchstats.py) cannot be
# imported as normal packages; importlib loads them once and shares benchstats_parse.


def _load_sibling_module(stem: str):
    path = _SWORDS_DIR / f"{stem}.py"
    spec = importlib.util.spec_from_file_location(stem.replace("-", "_"), path)
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_compare = _load_sibling_module("compare-benchstats")
_parse_benchstats = _load_sibling_module("parse-benchstats")
milestone_compare_rows = _compare.milestone_compare_rows
print_compare_table = _compare.print_compare_table
print_table = _parse_benchstats.print_table

PHASE3_POINTERS = (
    "just phase0-check",
    "just watch-cpu",
    "just profile-ibd",
    "just phase0-snapshot",
)


def build_report(
    log: Path,
    milestones: tuple[int, ...],
    archive: str | None = "latest",
    archive_log_path: str | None = None,
    archive_root: Path | None = None,
    warnings: list[str] | None = None,
) -> dict:
    series = benchstats_parse.parse_benchstats_series(log)
    run_metadata = benchstats_parse.parse_run_metadata(log)
    milestone_rows = benchstats_parse.rollup_milestones(series, milestones)
    summary = benchstats_parse.benchstats_summary(series)

    report: dict = {
        "log": str(log),
        "run_metadata": run_metadata,
        "summary": summary,
        "milestones": milestone_rows,
        "series": series,
        "pointers": list(PHASE3_POINTERS),
        "archive_compare": None,
    }

    notes = warnings if warnings is not None else []

    archive_log: Path | None = None
    if archive_log_path:
        archive_log = Path(archive_log_path)
        if not archive_log.is_file():
            notes.append(f"archive log not found: {archive_log_path}")
            archive_log = None
    elif archive is not None:
        try:
            archive_log = benchstats_parse.resolve_archive_log(
                archive,
                archive_root=archive_root,
            )
        except FileNotFoundError as exc:
            notes.append(str(exc))
            archive_log = None

    if archive_log is not None and archive_log.resolve() == log.resolve():
        notes.append("archive log same as current log; skipping compare")
        archive_log = None

    if archive_log is not None:
        archive_series = benchstats_parse.parse_benchstats_series(archive_log)
        compare_rows = milestone_compare_rows(series, archive_series, milestones)
        cur_summary = summary
        arc_summary = benchstats_parse.benchstats_summary(archive_series)
        report["archive_compare"] = {
            "archive_log": str(archive_log),
            "milestones": compare_rows,
            "current_summary": cur_summary,
            "archive_summary": arc_summary,
        }

    return report


def print_human_report(report: dict) -> None:
    log = Path(report["log"])
    series = report["series"]
    print_table(
        series,
        report["milestones"],
        report["summary"],
        report["run_metadata"],
        log,
    )

    archive_compare = report.get("archive_compare")
    if archive_compare:
        print()
        print("archive_compare")
        print_compare_table(
            log,
            Path(archive_compare["archive_log"]),
            archive_compare["milestones"],
            archive_compare["current_summary"],
            archive_compare["archive_summary"],
        )

    print()
    print("phase3_pointers")
    for pointer in report.get("pointers", PHASE3_POINTERS):
        print(f"  {pointer}")


PHASE3_EPILOG = """
Rate metrics (do not mix):
  interval_implied_blk_per_s — per inter-rollup interval (series/CSV only)
  implied_blk_per_s — cumulative milestone rate or summary sustained rate

Archive compare: unlike compare-benchstats, a missing/invalid archive is skipped
with a stderr note (exit 0). Use compare-benchstats for strict archive checks.
"""


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Phase 3 unified benchstats report for current debug.log",
        epilog=PHASE3_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "datadir",
        nargs="?",
        default=None,
        help="datadir or debug.log (default: SWORDS_DATADIR)",
    )
    ap.add_argument("--log", dest="log_path", default=None, help="explicit debug.log path")
    ap.add_argument(
        "--archive",
        default="latest",
        help="archive dir/path for milestone compare, or 'none' to skip (default: latest)",
    )
    ap.add_argument(
        "--archive-log",
        dest="archive_log_path",
        default=None,
        help="explicit archived debug.log path (overrides --archive)",
    )
    ap.add_argument("--json", action="store_true", help="emit full machine-readable report")
    ap.add_argument(
        "--milestones",
        default=benchstats_parse.default_milestones_arg(),
        help=f"comma-separated rollup indices (default: {benchstats_parse.default_milestones_arg()})",
    )
    args = ap.parse_args()

    milestones = benchstats_parse.parse_milestones_arg(args.milestones)
    datadir_or_log = (
        args.datadir if args.datadir is not None else str(benchstats_parse.default_datadir())
    )
    log = benchstats_parse.resolve_log_path(datadir_or_log, args.log_path)
    if not log.exists():
        print(f"Missing {log}", file=sys.stderr)
        sys.exit(1)

    archive = None if args.archive == "none" else args.archive
    report_warnings: list[str] = []
    report = build_report(
        log,
        milestones,
        archive=archive,
        archive_log_path=args.archive_log_path,
        warnings=report_warnings,
    )
    for note in report_warnings:
        print(f"phase3: {note}", file=sys.stderr)

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return

    print_human_report(report)


if __name__ == "__main__":
    main()