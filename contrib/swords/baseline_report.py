#!/usr/bin/env python3
"""CLI for IBD microbench baselines: list, show, compare, capture."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import baseline_lib


ROOT = Path(__file__).resolve().parents[2]
STAMP_FMT = "%Y%m%dT%H%M%SZ"


def _utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime(STAMP_FMT)


def require_bench_binary(bench: Path) -> None:
    if not bench.is_file() or not os.access(bench, os.X_OK):
        print(
            "bench_bitcoin not found; run: just configure-bench && just build-bench",
            file=sys.stderr,
        )
        sys.exit(1)


def run_bench_json(bench: Path) -> dict:
    require_bench_binary(bench)
    tmp = tempfile.NamedTemporaryFile(suffix=".json", delete=False)
    tmp_path = Path(tmp.name)
    tmp.close()
    try:
        try:
            subprocess.run(
                [str(bench), "-priority-level=high", f"-output-json={tmp_path}"],
                check=True,
                stdout=subprocess.DEVNULL,
            )
        except subprocess.CalledProcessError:
            print("bench_bitcoin run failed", file=sys.stderr)
            sys.exit(1)
        with open(tmp_path, encoding="utf-8") as f:
            return json.load(f)
    finally:
        tmp_path.unlink(missing_ok=True)


def load_segment_replay(datadir: Path, root: Path) -> dict:
    log = datadir / "debug.log"
    if not log.is_file():
        return {}
    parser = root / "contrib" / "swords" / "parse-reindex-log.py"
    try:
        out = subprocess.run(
            [sys.executable, str(parser), str(datadir), "--json"],
            check=False,
            capture_output=True,
            text=True,
        )
        if out.returncode != 0 or not out.stdout.strip():
            return {}
        return json.loads(out.stdout)
    except (json.JSONDecodeError, OSError):
        return {}


def cmd_list(args: argparse.Namespace) -> int:
    baselines = baseline_lib.list_baselines(args.baselines_dir)
    if not baselines:
        print(f"No baselines under {args.baselines_dir}")
        return 0
    for path in baselines:
        try:
            doc = baseline_lib.load_baseline_doc(path)
            print(
                f"{path}\t{doc.get('host', '?')}\t"
                f"{doc.get('git_sha', '?')}\t{doc.get('captured_at', '?')}"
            )
        except (OSError, json.JSONDecodeError):
            print(f"{path}\t(unreadable)")
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    path = Path(args.baseline)
    if not path.is_file():
        print(f"Missing baseline: {path}", file=sys.stderr)
        return 1
    doc = baseline_lib.load_baseline_doc(path)
    print(baseline_lib.format_baseline_summary(doc, path))
    return 0


def cmd_compare(args: argparse.Namespace) -> int:
    baseline_path = Path(args.baseline) if args.baseline else baseline_lib.find_latest_baseline(args.baselines_dir)
    if baseline_path is None or not baseline_path.is_file():
        print("Usage: baseline_report.py compare [baseline.json]", file=sys.stderr)
        return 1

    if args.bench_json:
        with open(args.bench_json, encoding="utf-8") as f:
            bench_data = json.load(f)
    else:
        bench_data = run_bench_json(args.bench_binary)

    current_medians = baseline_lib.medians_from_bench_json(bench_data)
    if args.segment_json:
        with open(args.segment_json, encoding="utf-8") as f:
            current_segment = json.load(f)
    else:
        current_segment = load_segment_replay(args.datadir, args.root)

    baseline_doc = baseline_lib.load_baseline_doc(baseline_path)
    rows = baseline_lib.compare_baselines(baseline_doc, current_medians, current_segment)
    print(baseline_lib.format_compare_report(rows))
    return baseline_lib.compare_exit_code(rows)


def cmd_capture(args: argparse.Namespace) -> int:
    owned_tmp: Path | None = None
    if args.bench_json:
        bench_path = Path(args.bench_json)
    else:
        bench_data = run_bench_json(args.bench_binary)
        tmp = tempfile.NamedTemporaryFile(suffix=".json", delete=False)
        owned_tmp = Path(tmp.name)
        tmp.close()
        with open(owned_tmp, "w", encoding="utf-8") as f:
            json.dump(bench_data, f)
        bench_path = owned_tmp

    try:
        segment: dict | None = None
        if args.segment_json:
            with open(args.segment_json, encoding="utf-8") as f:
                seg = json.load(f)
                segment = seg or None
        else:
            seg = load_segment_replay(args.datadir, args.root)
            segment = seg or None

        host = args.host or _hostname()
        sha = args.git_sha or _git_sha(args.root)
        stamp = args.captured_at or _utc_stamp()
        out_name = f"{host}-{sha}-{stamp}.json"
        out_path = args.baselines_dir / out_name

        doc = baseline_lib.capture_from_bench_json(bench_path, host, sha, stamp, segment)
        written = baseline_lib.write_baseline_doc(doc, out_path)
        print(written)
        return 0
    finally:
        if owned_tmp is not None:
            owned_tmp.unlink(missing_ok=True)


def _hostname() -> str:
    try:
        out = subprocess.run(
            ["hostname", "-s"],
            check=False,
            capture_output=True,
            text=True,
        )
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
        out = subprocess.run(
            ["hostname"],
            check=False,
            capture_output=True,
            text=True,
        )
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip().split(".")[0] or out.stdout.strip()
    except OSError:
        pass
    return "unknown"


def _git_sha(root: Path) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
        return out.stdout.strip()
    except (subprocess.CalledProcessError, OSError):
        return "unknown"


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description="IBD microbench baseline operator CLI")
    ap.add_argument(
        "--baselines-dir",
        type=Path,
        default=baseline_lib.default_baselines_dir(ROOT),
        help="Directory for baseline JSON files",
    )
    ap.add_argument(
        "--bench-binary",
        type=Path,
        default=baseline_lib.default_bench_binary(ROOT),
        help="Path to bench_bitcoin",
    )
    ap.add_argument(
        "--datadir",
        type=Path,
        default=baseline_lib.default_datadir(),
        help="Swords datadir for segment_replay (debug.log)",
    )
    ap.add_argument("--root", type=Path, default=ROOT, help="Repository root")

    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="List baselines sorted by mtime (newest first)")

    show_p = sub.add_parser("show", help="Print summary of one baseline JSON")
    show_p.add_argument("baseline", help="Path to baseline JSON")

    compare_p = sub.add_parser("compare", help="Compare current benches to a baseline")
    compare_p.add_argument("baseline", nargs="?", default=None, help="Baseline JSON (default: latest)")
    compare_p.add_argument("--bench-json", type=Path, default=None, help="Skip bench run; use JSON output")
    compare_p.add_argument("--segment-json", type=Path, default=None, help="Segment replay JSON")

    capture_p = sub.add_parser("capture", help="Capture baseline JSON after running benches")
    capture_p.add_argument("--host", default=None)
    capture_p.add_argument("--git-sha", default=None)
    capture_p.add_argument("--captured-at", default=None)
    capture_p.add_argument("--bench-json", type=Path, default=None)
    capture_p.add_argument("--segment-json", type=Path, default=None)

    return ap


def main(argv: list[str] | None = None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)
    if args.command == "list":
        return cmd_list(args)
    if args.command == "show":
        return cmd_show(args)
    if args.command == "compare":
        return cmd_compare(args)
    if args.command == "capture":
        return cmd_capture(args)
    ap.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())