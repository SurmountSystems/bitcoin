#!/usr/bin/env python3
"""Phase 0 live health profiling helpers (CSV paths, meta.txt, CLI parsing)."""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import NamedTuple, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchstats_parse

WATCH_CSV_HEADER = (
    "timestamp_utc,block_height,cpu_pct,usr,sys,vsz,rss"
)
WATCH_CSV_COLUMNS = WATCH_CSV_HEADER.split(",")

META_TXT_FIELDS = (
    "captured_at_utc",
    "run_finished_at_utc",
    "subcommand",
    "block_height",
    "datadir",
    "pid",
    "duration_seconds",
    "perf_version",
    "profile_logs",
    "perf_data",
    "interrupted",
)

STAMP_FMT = "%Y%m%dT%H%M%SZ"
ROW_TS_FMT = "%Y-%m-%dT%H:%M:%SZ"
RESET_DATADIR_STAMP_FMT = "%Y-%m-%dT%H-%MZ"


class Phase0Config(NamedTuple):
    subcommand: str
    datadir: Path
    profile_logs: Path
    interval: int
    duration: int | None
    out: Path | None
    bitcoin_cli: Path | None
    txindex_warn_ms: float = 500.0
    prefetch_rollups: int = 3
    prefetch_min_prevouts: int = 64


MDB_READERS_FULL_RE = re.compile(r"MDB_READERS_FULL")


def utc_stamp(now: datetime | None = None) -> str:
    dt = now or datetime.now(timezone.utc)
    return dt.strftime(STAMP_FMT)


def utc_now_iso(now: datetime | None = None) -> str:
    dt = now or datetime.now(timezone.utc)
    return dt.strftime(ROW_TS_FMT)


def watch_csv_path(profile_logs: Path, stamp: str, out: Path | None = None) -> Path:
    if out is not None:
        return out
    profile_logs.mkdir(parents=True, exist_ok=True)
    return profile_logs / f"phase0-watch-{stamp}.csv"


def profile_run_dir(profile_logs: Path, stamp: str) -> Path:
    return profile_logs / stamp


def profile_perf_data(profile_logs: Path, stamp: str) -> Path:
    return profile_run_dir(profile_logs, stamp) / "perf.data"


def snapshot_path(profile_logs: Path, stamp: str, out: Path | None = None) -> Path:
    if out is not None:
        return out
    profile_logs.mkdir(parents=True, exist_ok=True)
    return profile_logs / f"phase0-snapshot-{stamp}.txt"


def format_meta_txt(fields: dict[str, str]) -> str:
    lines = []
    for key in META_TXT_FIELDS:
        if key in fields:
            lines.append(f"{key}={fields[key]}")
    for key, value in sorted(fields.items()):
        if key not in META_TXT_FIELDS:
            lines.append(f"{key}={value}")
    return "\n".join(lines) + "\n"


_META_LINE_RE = re.compile(r"^([a-z_]+)=(.*)$")


def parse_meta_txt(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        match = _META_LINE_RE.match(line)
        if match:
            out[match.group(1)] = match.group(2)
    return out


def format_watch_row(
    timestamp_utc: str,
    block_height: str,
    cpu_pct: str,
    usr: str,
    sys_pct: str,
    vsz: str,
    rss: str,
) -> str:
    return ",".join(
        [
            timestamp_utc,
            block_height,
            cpu_pct,
            usr,
            sys_pct,
            vsz,
            rss,
        ]
    )


def parse_pidstat_cpu_line(line: str, pid: str) -> tuple[str, str, str] | None:
    """Parse pidstat -u sample or Average line for usr, sys, cpu_pct (%CPU)."""
    parts = line.split()
    if not parts:
        return None
    if parts[0] == "Average:":
        if len(parts) >= 8 and parts[2] == pid:
            return parts[3], parts[4], parts[7]
        return None
    if len(parts) >= 9 and parts[3] == pid:
        return parts[4], parts[5], parts[8]
    return None


def parse_pidstat_mem_line(line: str, pid: str) -> tuple[str, str] | None:
    """Parse pidstat -r sample or Average line for VSZ and RSS."""
    parts = line.split()
    if not parts:
        return None
    if parts[0] == "Average:":
        if len(parts) >= 7 and parts[2] == pid:
            return parts[5], parts[6]
        return None
    if len(parts) >= 8 and parts[3] == pid:
        return parts[6], parts[7]
    return None


def parse_pidstat_cpu_output(text: str, pid: str) -> tuple[str, str, str]:
    """Prefer instantaneous sample rows over the Average line."""
    sample: tuple[str, str, str] | None = None
    average: tuple[str, str, str] | None = None
    for line in text.splitlines():
        parsed = parse_pidstat_cpu_line(line.strip(), pid)
        if parsed is None:
            continue
        if line.strip().startswith("Average:"):
            average = parsed
        else:
            sample = parsed
    if sample is not None:
        return sample
    if average is not None:
        return average
    return "", "", ""


def parse_pidstat_mem_output(text: str, pid: str) -> tuple[str, str]:
    sample: tuple[str, str] | None = None
    average: tuple[str, str] | None = None
    for line in text.splitlines():
        parsed = parse_pidstat_mem_line(line.strip(), pid)
        if parsed is None:
            continue
        if line.strip().startswith("Average:"):
            average = parsed
        else:
            sample = parsed
    if sample is not None:
        return sample
    if average is not None:
        return average
    return "", ""


def _run_start_line_index(lines: list[str]) -> int | None:
    """Index of the last 'Swords run started' line (for scoping health checks)."""
    last_idx: int | None = None
    for i, line in enumerate(lines):
        body = benchstats_parse.strip_ts_prefix(line)
        if benchstats_parse.SWORDS_RUN_STARTED_RE.search(body):
            last_idx = i
    return last_idx


def run_health_check(
    log_path: Path,
    *,
    txindex_warn_ms: float = 500.0,
    prefetch_rollups: int = 3,
    prefetch_min_prevouts: int = 64,
) -> tuple[int, dict]:
    """Evaluate live IBD health from debug.log tail.

    Returns (exit_code, report_dict). Exit 1 on FAIL conditions; WARN only -> exit 0.
    """
    if not log_path.is_file():
        return 1, {
            "log": str(log_path),
            "status": "FAIL",
            "failures": [f"debug.log not found: {log_path}"],
            "warnings": [],
        }

    lines = log_path.read_text(errors="replace").splitlines()
    run_idx = _run_start_line_index(lines)
    run_lines = lines[run_idx:] if run_idx is not None else lines

    failures: list[str] = []
    warnings: list[str] = []

    if run_idx is None:
        warnings.append("no 'Swords run started' marker found; scanning full log")

    for line in run_lines:
        if MDB_READERS_FULL_RE.search(line):
            failures.append("log contains MDB_READERS_FULL since run start")
            break

    series = benchstats_parse.parse_benchstats_series(run_lines)
    if not series:
        failures.append("no benchstats rollups found since run start")
        status = "FAIL" if failures else "WARN"
        exit_code = 1 if status == "FAIL" else 0
        return exit_code, {
            "log": str(log_path),
            "status": status,
            "failures": failures,
            "warnings": warnings,
            "rollup_count": 0,
        }

    latest = series[-1]
    latest_idx = int(latest["rollup_index"])
    readers_full = int(latest.get("readers_full", 0))
    if readers_full > 0:
        failures.append(
            f"readers_full={readers_full} on latest rollup #{latest_idx}"
        )

    txindex_pb = benchstats_parse.per_block_ms(latest, "txindex")
    if txindex_pb is not None and txindex_pb > txindex_warn_ms:
        warnings.append(
            f"txindex_per_blk_ms={txindex_pb:.1f} exceeds threshold {txindex_warn_ms:.1f} "
            f"on latest rollup #{latest_idx}"
        )

    tail = series[-max(1, prefetch_rollups) :]
    broken_prefetch = [
        int(r["rollup_index"])
        for r in tail
        if int(r.get("prefetch_hit", 0)) == 0
        and int(r.get("coin_prevouts", 0)) >= prefetch_min_prevouts
    ]
    if len(broken_prefetch) >= max(1, prefetch_rollups):
        warnings.append(
            f"prefetch_hit=0 with coin_prevouts>={prefetch_min_prevouts} on last "
            f"{prefetch_rollups} rollups (indices {broken_prefetch}); prefetch may be broken"
        )

    status = "FAIL" if failures else ("WARN" if warnings else "OK")
    exit_code = 1 if status == "FAIL" else 0
    summary = benchstats_parse.benchstats_summary(series)
    return exit_code, {
        "log": str(log_path),
        "status": status,
        "failures": failures,
        "warnings": warnings,
        "rollup_count": len(series),
        "latest_rollup_index": latest_idx,
        "latest_readers_full": readers_full,
        "latest_prefetch_hit": int(latest.get("prefetch_hit", 0)),
        "latest_coin_prevouts": int(latest.get("coin_prevouts", 0)),
        "latest_txindex_per_blk_ms": round(txindex_pb, 2) if txindex_pb is not None else None,
        "summary": summary,
    }


def print_health_check_report(report: dict) -> None:
    print("phase0_check")
    print(f"log\t{report.get('log', '')}")
    if report.get("rollup_count") is not None:
        print(f"rollup_count\t{report['rollup_count']}")
    if report.get("latest_rollup_index") is not None:
        print(f"latest_rollup_index\t{report['latest_rollup_index']}")
    print(f"status\t{report.get('status', 'UNKNOWN')}")
    for msg in report.get("failures") or []:
        print(f"FAIL\t{msg}")
    for msg in report.get("warnings") or []:
        print(f"WARN\t{msg}")
    summary = report.get("summary")
    if summary:
        print(
            "prefetch_health\t"
            f"max_readers_full={summary.get('max_readers_full', 0)} "
            f"min_prefetch_hit={summary.get('min_prefetch_hit', 0)}"
        )


def pgrep_datadir_pattern(datadir: str) -> str:
    """Build a pgrep -f pattern with regex metacharacters escaped."""
    return f"bitcoind.*-datadir={re.escape(datadir)}"


def resolve_env_config(subcommand: str = "") -> Phase0Config:
    """Resolve config from environment (shared by shell driver and tests)."""
    home = Path.home()
    datadir = Path(
        os.environ.get("DATADIR")
        or os.environ.get("SWORDS_DATADIR")
        or (home / ".bitcoin-swords")
    )
    profile_logs = Path(
        os.environ.get("PROFILE_LOGS")
        or os.environ.get("SWORDS_LOG_ARCHIVE")
        or (home / ".bitcoin-swords-profile-logs")
    )
    interval = int(os.environ.get("INTERVAL", "10"))
    duration_raw = os.environ.get("DURATION", "").strip()
    duration: int | None = int(duration_raw) if duration_raw else None
    out_raw = os.environ.get("OUT", "").strip()
    out = Path(out_raw) if out_raw else None
    cli_raw = os.environ.get("BITCOIN_CLI", "").strip()
    bitcoin_cli = Path(cli_raw) if cli_raw else None
    txindex_warn_ms = float(os.environ.get("TXINDEX_WARN_MS", "500"))
    prefetch_rollups = int(os.environ.get("PREFETCH_ROLLUPS", "3"))
    prefetch_min_prevouts = int(os.environ.get("PREFETCH_MIN_PREVOUTS", "64"))
    return Phase0Config(
        subcommand=subcommand,
        datadir=datadir,
        profile_logs=profile_logs,
        interval=interval,
        duration=duration,
        out=out,
        bitcoin_cli=bitcoin_cli,
        txindex_warn_ms=txindex_warn_ms,
        prefetch_rollups=prefetch_rollups,
        prefetch_min_prevouts=prefetch_min_prevouts,
    )


def shell_export_config(cfg: Phase0Config | None = None) -> str:
    """Emit shell-safe export lines for bash eval."""
    cfg = cfg or resolve_env_config()
    lines = [
        f"DATADIR={shlex.quote(str(cfg.datadir))}",
        f"PROFILE_LOGS={shlex.quote(str(cfg.profile_logs))}",
        f"INTERVAL={shlex.quote(str(cfg.interval))}",
    ]
    if cfg.duration is not None:
        lines.append(f"DURATION={shlex.quote(str(cfg.duration))}")
    if cfg.out is not None:
        lines.append(f"OUT={shlex.quote(str(cfg.out))}")
    if cfg.bitcoin_cli is not None:
        lines.append(f"BITCOIN_CLI={shlex.quote(str(cfg.bitcoin_cli))}")
    return "\n".join(lines) + "\n"


def write_profile_meta(meta_path: Path, fields: dict[str, str]) -> None:
    meta_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(format_meta_txt(fields), encoding="utf-8")


def _add_global_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--datadir",
        default=None,
        help="bitcoind datadir (default: SWORDS_DATADIR or ~/.bitcoin-swords)",
    )
    parser.add_argument(
        "--profile-logs",
        default=None,
        help="artifact root (default: SWORDS_LOG_ARCHIVE or ~/.bitcoin-swords-profile-logs)",
    )
    parser.add_argument(
        "--bitcoin-cli",
        default=None,
        help="path to bitcoin-cli (default: build/bin/bitcoin-cli or PATH)",
    )
    parser.add_argument(
        "--out",
        default=None,
        help="override output path (watch CSV or snapshot txt)",
    )


def build_arg_parser() -> argparse.ArgumentParser:
    parent = argparse.ArgumentParser(add_help=False)
    _add_global_args(parent)

    parser = argparse.ArgumentParser(
        prog="phase0-live-health",
        description="Phase 0 live health profiling for bitcoind IBD",
        parents=[parent],
    )

    sub = parser.add_subparsers(dest="subcommand", required=True)

    watch = sub.add_parser(
        "watch",
        help="loop pidstat + block height",
        parents=[parent],
    )
    watch.add_argument(
        "--interval",
        type=int,
        default=10,
        help="seconds between samples (default: 10)",
    )
    watch.add_argument(
        "--duration",
        type=int,
        default=None,
        help="stop after N seconds (default: run until Ctrl+C)",
    )

    profile = sub.add_parser(
        "profile",
        help="perf record -g on bitcoind PID",
        parents=[parent],
    )
    profile.add_argument(
        "--duration",
        type=int,
        default=None,
        help="perf sampling seconds (default: DURATION env or 60)",
    )

    sub.add_parser(
        "snapshot",
        help="one-shot pidstat/mpstat/iostat capture",
        parents=[parent],
    )

    sub.add_parser(
        "help",
        help="print usage (also: per-thread CPU hint via top -H -p <pid>)",
        parents=[parent],
    )

    sub.add_parser(
        "shell-export",
        help="emit shell exports from environment (used by phase0-live-health.sh)",
    )

    check_env = resolve_env_config("check")
    check = sub.add_parser(
        "check",
        help="tail debug.log for benchstats health (readers_full, prefetch, txindex)",
        parents=[parent],
    )
    check.add_argument(
        "--txindex-warn-ms",
        type=float,
        default=check_env.txindex_warn_ms,
        help=(
            "WARN when latest rollup txindex_per_blk_ms exceeds this "
            f"(default: {check_env.txindex_warn_ms}; env TXINDEX_WARN_MS)"
        ),
    )
    check.add_argument(
        "--prefetch-rollups",
        type=int,
        default=check_env.prefetch_rollups,
        help=(
            "consecutive rollups to inspect for broken prefetch "
            f"(default: {check_env.prefetch_rollups}; env PREFETCH_ROLLUPS)"
        ),
    )
    check.add_argument(
        "--prefetch-min-prevouts",
        type=int,
        default=check_env.prefetch_min_prevouts,
        help=(
            "min coin prevouts for prefetch-broken WARN "
            f"(default: {check_env.prefetch_min_prevouts}; env PREFETCH_MIN_PREVOUTS)"
        ),
    )

    return parser


def parse_argv(argv: Sequence[str] | None = None) -> Phase0Config:
    parser = build_arg_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)

    env_cfg = resolve_env_config(subcommand=args.subcommand)

    datadir = Path(args.datadir) if args.datadir else env_cfg.datadir
    profile_logs = Path(args.profile_logs) if args.profile_logs else env_cfg.profile_logs
    bitcoin_cli = Path(args.bitcoin_cli) if args.bitcoin_cli else env_cfg.bitcoin_cli

    interval = getattr(args, "interval", env_cfg.interval)
    duration = getattr(args, "duration", None)
    if args.subcommand == "profile":
        if duration is None:
            duration = env_cfg.duration if env_cfg.duration is not None else 60
    elif duration is None:
        duration = env_cfg.duration

    out = Path(args.out) if args.out else env_cfg.out

    txindex_warn_ms = getattr(args, "txindex_warn_ms", env_cfg.txindex_warn_ms)
    prefetch_rollups = getattr(args, "prefetch_rollups", env_cfg.prefetch_rollups)
    prefetch_min_prevouts = getattr(
        args, "prefetch_min_prevouts", env_cfg.prefetch_min_prevouts
    )

    return Phase0Config(
        subcommand=args.subcommand,
        datadir=datadir,
        profile_logs=profile_logs,
        interval=interval,
        duration=duration,
        out=out,
        bitcoin_cli=bitcoin_cli,
        txindex_warn_ms=txindex_warn_ms,
        prefetch_rollups=prefetch_rollups,
        prefetch_min_prevouts=prefetch_min_prevouts,
    )


def usage_text(datadir: str | None = None) -> str:
    dd = datadir or os.environ.get("DATADIR") or os.environ.get("SWORDS_DATADIR") or "~/.bitcoin-swords"
    pgrep_pat = pgrep_datadir_pattern(str(Path(dd).expanduser()))
    return f"""Phase 0 live health profiling (run alongside bitcoind during IBD)

Subcommands:
  watch     Loop pidstat + block height -> phase0-watch-<stamp>.csv
  profile   perf record -g (default 60s) -> <stamp>/perf.data + meta.txt
  snapshot  One-shot pidstat/mpstat/iostat -> phase0-snapshot-<stamp>.txt
  check     Tail debug.log benchstats: FAIL on readers_full/MDB_READERS_FULL; WARN on txindex/prefetch

Environment:
  DATADIR / SWORDS_DATADIR          bitcoind data directory (default ~/.bitcoin-swords)
  PROFILE_LOGS / SWORDS_LOG_ARCHIVE artifact directory

Timestamp formats:
  Row timestamps (watch CSV, snapshot body): {ROW_TS_FMT}  e.g. 2026-07-06T15:30:55Z
  Filenames + meta.txt captured_at_utc:    {STAMP_FMT}  e.g. 20260706T153045Z
  reset-datadir debug.log archives:        {RESET_DATADIR_STAMP_FMT}  e.g. 2026-07-06T15-30Z
  (Phase 0 uses compact stamps with seconds; reset-datadir archives omit seconds.)

Per-thread CPU (optional):
  top -H -p $(pgrep -f '{pgrep_pat}' | head -1)

Just recipes:
  just watch-cpu
  just profile-ibd
  just phase0-snapshot
  just phase0-check

check behavior:
  FAIL (exit 1): readers_full>0 on latest rollup OR MDB_READERS_FULL since run start
  WARN (exit 0): txindex_per_blk_ms above threshold OR prefetch_hit=0 with high prevouts
"""


def main(argv: Sequence[str] | None = None) -> int:
    if argv is not None and len(argv) >= 1 and argv[0] == "shell-export":
        print(shell_export_config(), end="")
        return 0

    cfg = parse_argv(argv)
    if cfg.subcommand == "shell-export":
        print(shell_export_config(cfg), end="")
        return 0
    if cfg.subcommand == "help":
        print(usage_text(str(cfg.datadir)), end="")
        return 0
    if cfg.subcommand == "check":
        log_path = cfg.datadir / "debug.log"
        exit_code, report = run_health_check(
            log_path,
            txindex_warn_ms=cfg.txindex_warn_ms,
            prefetch_rollups=cfg.prefetch_rollups,
            prefetch_min_prevouts=cfg.prefetch_min_prevouts,
        )
        print_health_check_report(report)
        return exit_code
    print(
        f"Use contrib/swords/phase0-live-health.sh for live capture "
        f"(parsed subcommand={cfg.subcommand})",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())