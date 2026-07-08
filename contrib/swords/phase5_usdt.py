#!/usr/bin/env python3
"""Phase 5 USDT/kernel tracing helpers (connectblock bpftrace, utxo flush BCC)."""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple, Sequence

from phase0_live_health import (
    META_TXT_FIELDS as PHASE0_META_TXT_FIELDS,
    STAMP_FMT,
    ROW_TS_FMT,
    parse_meta_txt,
    pgrep_datadir_pattern,
    utc_now_iso,
    utc_stamp,
    write_profile_meta,
)

CONNECTBLOCK_TEMPLATE_REL = Path("contrib/tracing/connectblock_benchmark.bt")
UTXO_FLUSH_SCRIPT_REL = Path("contrib/tracing/log_utxocache_flush.py")
CONNECTBLOCK_BINARY_PLACEHOLDER = "./build/bin/bitcoind"

PHASE5_REQUIRED_TRACEPOINTS = (
    "validation:block_connected",
    "utxocache:flush",
)

META_TXT_FIELDS = PHASE0_META_TXT_FIELDS + (
    "bitcoind_path",
    "usdt_tracepoints",
    "usdt_detection_method",
    "start_height",
    "end_height",
    "threshold_ms",
    "connectblock_bt_script",
    "connectblock_output",
    "utxo_flush_output",
    "utxo_flush_script",
    "bpftrace_version",
    "bcc_available",
)

_READELF_PROVIDER_RE = re.compile(r"^\s*Provider:\s*(\S+)\s*$")
_READELF_NAME_RE = re.compile(r"^\s*Name:\s*(\S+)\s*$")
_CONNECTBLOCK_BLOCK_RE = re.compile(
    r"Block\s+(\d+)\s+\([^)]+\)\s+\d+\s+tx\s+\d+\s+ins\s+\d+\s+sigops\s+took\s+(\d+)\s+ms"
)
_CONNECTBLOCK_BENCH_RE = re.compile(
    r"BENCH\s+(\d+)\s+blk/s\s+(\d+)\s+tx/s\s+(\d+)\s+inputs/s\s+(\d+)\s+sigops/s\s+\(height\s+(\d+)\)"
)
_FLUSH_ROW_RE = re.compile(
    r"^\s*(\d+)\s+(\S+)\s+(\d+)\s+([\d.]+\s+\S+)\s+(\S+)\s*$"
)


class Phase5Config(NamedTuple):
    subcommand: str
    datadir: Path
    profile_logs: Path
    bitcoind: Path | None
    bitcoin_cli: Path | None
    duration: int | None
    start_height: int | None
    end_height: int | None
    threshold_ms: int
    pid: str | None = None
    height: int | None = None
    stamp: str | None = None
    meta_path: Path | None = None
    input_path: Path | None = None
    output_path: Path | None = None
    bt_script: Path | None = None


class UsdtDetectionResult(NamedTuple):
    available: bool
    tracepoints: tuple[str, ...]
    method: str
    error: str = ""

    def missing_required(self) -> tuple[str, ...]:
        found = set(self.tracepoints)
        return tuple(tp for tp in PHASE5_REQUIRED_TRACEPOINTS if tp not in found)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def tracing_template_path(rel: Path) -> Path:
    return repo_root() / rel


def phase5_run_dir(profile_logs: Path, stamp: str) -> Path:
    return profile_logs / stamp


def connectblock_output_path(profile_logs: Path, stamp: str) -> Path:
    return phase5_run_dir(profile_logs, stamp) / "connectblock.txt"


def utxo_flush_output_path(profile_logs: Path, stamp: str) -> Path:
    return phase5_run_dir(profile_logs, stamp) / "utxo-flush.txt"


def connectblock_summary_path(profile_logs: Path, stamp: str) -> Path:
    return phase5_run_dir(profile_logs, stamp) / "connectblock-summary.json"


def utxo_flush_summary_path(profile_logs: Path, stamp: str) -> Path:
    return phase5_run_dir(profile_logs, stamp) / "utxo-flush-summary.json"


def _parse_readelf_tracepoints(text: str) -> list[str]:
    tracepoints: list[str] = []
    provider: str | None = None
    for line in text.splitlines():
        prov_match = _READELF_PROVIDER_RE.match(line)
        if prov_match:
            provider = prov_match.group(1)
            continue
        name_match = _READELF_NAME_RE.match(line)
        if name_match and provider:
            tracepoints.append(f"{provider}:{name_match.group(1)}")
            provider = None
    return tracepoints


def _parse_tplist_tracepoints(text: str) -> list[str]:
    tracepoints: list[str] = []
    for line in text.splitlines():
        line = line.strip()
        if not line or not line.startswith("b'"):
            continue
        match = re.match(r"b'([^']+)':b'([^']+)'", line)
        if match:
            tracepoints.append(f"{match.group(1)}:{match.group(2)}")
    return tracepoints


def _run_capture(cmd: Sequence[str]) -> tuple[int, str, str]:
    try:
        proc = subprocess.run(
            list(cmd),
            check=False,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError:
        return 127, "", f"{cmd[0]} not found"
    return proc.returncode, proc.stdout, proc.stderr


def detect_usdt(bitcoind_path: Path) -> UsdtDetectionResult:
    """Check whether bitcoind has USDT tracepoints (readelf, then tplist)."""
    path = Path(bitcoind_path)
    if not path.is_file():
        return UsdtDetectionResult(
            available=False,
            tracepoints=(),
            method="none",
            error=f"bitcoind binary not found: {path}",
        )

    readelf_code, readelf_stdout, readelf_stderr = _run_capture(["readelf", "-n", str(path)])
    readelf_has_stapsdt = readelf_code == 0 and "NT_STAPSDT" in readelf_stdout

    if readelf_has_stapsdt:
        tracepoints = _parse_readelf_tracepoints(readelf_stdout)
        if tracepoints:
            return UsdtDetectionResult(
                available=True,
                tracepoints=tuple(sorted(set(tracepoints))),
                method="readelf",
            )

    tplist_missing = True
    last_tplist_stderr = ""
    for tplist_cmd in ("tplist", "tplist-bpfcc"):
        code, stdout, stderr = _run_capture([tplist_cmd, "-l", str(path)])
        if code == 127:
            continue
        tplist_missing = False
        if code == 0 and stdout.strip():
            tracepoints = _parse_tplist_tracepoints(stdout)
            if tracepoints:
                return UsdtDetectionResult(
                    available=True,
                    tracepoints=tuple(sorted(set(tracepoints))),
                    method=tplist_cmd,
                )
        last_tplist_stderr = stderr

    if readelf_has_stapsdt:
        return UsdtDetectionResult(
            available=False,
            tracepoints=(),
            method="readelf",
            error="USDT notes present but could not parse tracepoints",
        )

    if readelf_code == 127:
        return UsdtDetectionResult(
            available=False,
            tracepoints=(),
            method="none",
            error="readelf not found; cannot check USDT tracepoints",
        )

    if not readelf_has_stapsdt:
        if tplist_missing:
            return UsdtDetectionResult(
                available=False,
                tracepoints=(),
                method="readelf",
                error=(
                    "USDT tracepoints not found (readelf saw no NT_STAPSDT; "
                    "tplist not installed). Rebuild with -DENABLE_USDT=ON."
                ),
            )
        return UsdtDetectionResult(
            available=False,
            tracepoints=(),
            method="readelf",
            error="USDT tracepoints not found; rebuild bitcoind with -DENABLE_USDT=ON.",
        )

    return UsdtDetectionResult(
        available=False,
        tracepoints=(),
        method="readelf",
        error=last_tplist_stderr.strip() or "failed to enumerate USDT tracepoints",
    )


def render_connectblock_bt(
    bitcoind_path: Path,
    out_path: Path,
    template_path: Path | None = None,
) -> Path:
    """Render connectblock_benchmark.bt with the running bitcoind path."""
    template = template_path or tracing_template_path(CONNECTBLOCK_TEMPLATE_REL)
    text = template.read_text(encoding="utf-8")
    resolved = str(Path(bitcoind_path).resolve())
    if CONNECTBLOCK_BINARY_PLACEHOLDER not in text:
        raise ValueError(
            f"template missing placeholder {CONNECTBLOCK_BINARY_PLACEHOLDER!r}: {template}"
        )
    rendered = text.replace(CONNECTBLOCK_BINARY_PLACEHOLDER, resolved)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(rendered, encoding="utf-8")
    return out_path


def connectblock_cmd(
    bitcoind_path: Path,
    start_height: int,
    end_height: int,
    threshold_ms: int,
    bt_script: Path,
) -> list[str]:
    """Build bpftrace command for connectblock benchmarking."""
    return [
        "bpftrace",
        str(bt_script),
        str(start_height),
        str(end_height),
        str(threshold_ms),
    ]


def utxo_flush_cmd(
    pid: str | int,
    script_path: Path | None = None,
) -> list[str]:
    """Build python3 log_utxocache_flush.py command (runs until interrupted)."""
    script = script_path or tracing_template_path(UTXO_FLUSH_SCRIPT_REL)
    return ["python3", str(script), str(pid)]


def resolve_bitcoind_path(
    bitcoind: Path | None = None,
    pid: str | int | None = None,
) -> Path:
    if bitcoind is not None and bitcoind.is_file():
        return bitcoind.resolve()
    if pid is not None:
        exe = Path(f"/proc/{pid}/exe")
        if exe.exists():
            try:
                return exe.resolve()
            except OSError:
                pass
    default = repo_root() / "build" / "bin" / "bitcoind"
    if default.is_file():
        return default.resolve()
    if bitcoind is not None and bitcoind.exists():
        return bitcoind.resolve()
    raise FileNotFoundError(
        "bitcoind binary not found; set BITCOIND or start bitcoind"
    )


def default_connectblock_start_height(current_height: int) -> int:
    if current_height <= 0:
        return 0
    return max(0, current_height - 1000)


def parse_connectblock_output(text: str) -> dict[str, object]:
    slow_blocks: list[dict[str, int]] = []
    bench_samples: list[dict[str, int]] = []
    for line in text.splitlines():
        block_match = _CONNECTBLOCK_BLOCK_RE.search(line)
        if block_match:
            slow_blocks.append(
                {
                    "height": int(block_match.group(1)),
                    "duration_ms": int(block_match.group(2)),
                }
            )
            continue
        bench_match = _CONNECTBLOCK_BENCH_RE.search(line)
        if bench_match:
            bench_samples.append(
                {
                    "blocks_per_s": int(bench_match.group(1)),
                    "tx_per_s": int(bench_match.group(2)),
                    "inputs_per_s": int(bench_match.group(3)),
                    "sigops_per_s": int(bench_match.group(4)),
                    "height": int(bench_match.group(5)),
                }
            )
    summary: dict[str, object] = {
        "slow_block_count": len(slow_blocks),
        "bench_sample_count": len(bench_samples),
        "slow_blocks": slow_blocks,
        "bench_samples": bench_samples,
    }
    if slow_blocks:
        durations = [b["duration_ms"] for b in slow_blocks]
        summary["slow_block_max_ms"] = max(durations)
        summary["slow_block_min_ms"] = min(durations)
    return summary


def parse_flush_output(text: str) -> dict[str, object]:
    flushes: list[dict[str, object]] = []
    for line in text.splitlines():
        if "Duration" in line and "Mode" in line:
            continue
        if "Logging utxocache flushes" in line:
            continue
        match = _FLUSH_ROW_RE.match(line)
        if not match:
            continue
        flushes.append(
            {
                "duration_us": int(match.group(1)),
                "mode": match.group(2),
                "coins_count": int(match.group(3)),
                "memory_usage": match.group(4),
                "flush_for_prune": match.group(5) == "True",
            }
        )
    summary: dict[str, object] = {
        "flush_count": len(flushes),
        "flushes": flushes,
    }
    if flushes:
        durations = [int(f["duration_us"]) for f in flushes]
        summary["duration_us_max"] = max(durations)
        summary["duration_us_min"] = min(durations)
        summary["duration_us_total"] = sum(durations)
    return summary


def write_connectblock_summary(path: Path, text: str) -> dict[str, object]:
    summary = parse_connectblock_output(text)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


def write_flush_summary(path: Path, text: str) -> dict[str, object]:
    summary = parse_flush_output(text)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


def resolve_env_config(subcommand: str = "") -> Phase5Config:
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
    bitcoind_raw = os.environ.get("BITCOIND", "").strip()
    bitcoind = Path(bitcoind_raw) if bitcoind_raw else None
    cli_raw = os.environ.get("BITCOIN_CLI", "").strip()
    bitcoin_cli = Path(cli_raw) if cli_raw else None
    duration_raw = os.environ.get("DURATION", "").strip()
    duration: int | None = int(duration_raw) if duration_raw else None
    start_raw = os.environ.get("START", "").strip()
    start_height: int | None = int(start_raw) if start_raw else None
    end_raw = os.environ.get("END", "").strip()
    end_height: int | None = int(end_raw) if end_raw else None
    threshold_ms = int(os.environ.get("THRESHOLD_MS", "25"))
    return Phase5Config(
        subcommand=subcommand,
        datadir=datadir,
        profile_logs=profile_logs,
        bitcoind=bitcoind,
        bitcoin_cli=bitcoin_cli,
        duration=duration,
        start_height=start_height,
        end_height=end_height,
        threshold_ms=threshold_ms,
    )


def shell_export_config(cfg: Phase5Config | None = None) -> str:
    cfg = cfg or resolve_env_config()
    lines = [
        f"DATADIR={shlex.quote(str(cfg.datadir))}",
        f"PROFILE_LOGS={shlex.quote(str(cfg.profile_logs))}",
        f"THRESHOLD_MS={shlex.quote(str(cfg.threshold_ms))}",
    ]
    if cfg.bitcoind is not None:
        lines.append(f"BITCOIND={shlex.quote(str(cfg.bitcoind))}")
    if cfg.bitcoin_cli is not None:
        lines.append(f"BITCOIN_CLI={shlex.quote(str(cfg.bitcoin_cli))}")
    if cfg.duration is not None:
        lines.append(f"DURATION={shlex.quote(str(cfg.duration))}")
    if cfg.start_height is not None:
        lines.append(f"START={shlex.quote(str(cfg.start_height))}")
    if cfg.end_height is not None:
        lines.append(f"END={shlex.quote(str(cfg.end_height))}")
    return "\n".join(lines) + "\n"


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
        "--bitcoind",
        default=None,
        help="path to bitcoind binary (default: BITCOIND or /proc/<pid>/exe)",
    )
    parser.add_argument(
        "--bitcoin-cli",
        default=None,
        help="path to bitcoin-cli (default: build/bin/bitcoin-cli or PATH)",
    )


def build_arg_parser() -> argparse.ArgumentParser:
    parent = argparse.ArgumentParser(add_help=False)
    _add_global_args(parent)

    parser = argparse.ArgumentParser(
        prog="phase5-usdt",
        description="Phase 5 USDT/kernel tracing for bitcoind IBD",
        parents=[parent],
    )
    sub = parser.add_subparsers(dest="subcommand", required=True)

    sub.add_parser(
        "check-usdt",
        help="verify bitcoind has USDT tracepoints",
        parents=[parent],
    )

    connectblock = sub.add_parser(
        "connectblock",
        help="print bpftrace connectblock command (dry-run)",
        parents=[parent],
    )
    connectblock.add_argument("--start", type=int, default=None)
    connectblock.add_argument("--end", type=int, default=0)
    connectblock.add_argument("--threshold-ms", type=int, default=25)
    connectblock.add_argument("--pid", default=None)
    connectblock.add_argument("--bt-script", default=None)

    utxo_flush = sub.add_parser(
        "utxo-flush",
        help="print utxo flush logger command (dry-run)",
        parents=[parent],
    )
    utxo_flush.add_argument("--pid", required=True)
    utxo_flush.add_argument("--duration", type=int, default=60)

    run_dir = sub.add_parser("run-dir", help="print stamp run directory", parents=[parent])
    run_dir.add_argument("--stamp", required=True)

    default_start = sub.add_parser(
        "default-start-height",
        help="print default connectblock start height",
        parents=[parent],
    )
    default_start.add_argument("--height", type=int, required=True)

    render_bt = sub.add_parser(
        "render-bt",
        help="render connectblock_benchmark.bt with bitcoind path",
        parents=[parent],
    )
    render_bt.add_argument("--output", required=True)

    write_cb_summary = sub.add_parser(
        "write-connectblock-summary",
        help="write connectblock-summary.json from captured output",
        parents=[parent],
    )
    write_cb_summary.add_argument("--input", required=True)
    write_cb_summary.add_argument("--output", required=True)

    write_flush_summary_p = sub.add_parser(
        "write-flush-summary",
        help="write utxo-flush-summary.json from captured output",
        parents=[parent],
    )
    write_flush_summary_p.add_argument("--input", required=True)
    write_flush_summary_p.add_argument("--output", required=True)

    write_meta = sub.add_parser(
        "write-meta",
        help="write meta.txt from JSON on stdin",
        parents=[parent],
    )
    write_meta.add_argument("--meta-path", required=True)

    resolve_bin = sub.add_parser(
        "resolve-bitcoind",
        help="print resolved bitcoind binary path",
        parents=[parent],
    )
    resolve_bin.add_argument("--pid", default=None)

    sub.add_parser("utc-now", help="print UTC ISO timestamp")
    sub.add_parser("utc-stamp", help="print compact UTC stamp")
    sub.add_parser("pgrep-pattern", help="print pgrep datadir pattern", parents=[parent])

    sub.add_parser(
        "help",
        help="print usage",
        parents=[parent],
    )

    sub.add_parser(
        "shell-export",
        help="emit shell exports from environment (used by phase5-usdt.sh)",
    )

    return parser


def parse_argv(argv: Sequence[str] | None = None) -> Phase5Config:
    parser = build_arg_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)
    env_cfg = resolve_env_config(subcommand=args.subcommand)

    datadir = Path(args.datadir) if args.datadir else env_cfg.datadir
    profile_logs = (
        Path(args.profile_logs) if args.profile_logs else env_cfg.profile_logs
    )
    bitcoind = Path(args.bitcoind) if getattr(args, "bitcoind", None) else env_cfg.bitcoind
    bitcoin_cli = Path(args.bitcoin_cli) if getattr(args, "bitcoin_cli", None) else env_cfg.bitcoin_cli

    threshold_ms = getattr(args, "threshold_ms", env_cfg.threshold_ms)
    start_height = getattr(args, "start", env_cfg.start_height)
    end_height = getattr(args, "end", env_cfg.end_height)
    if end_height is None:
        end_height = 0

    duration = getattr(args, "duration", None)
    if duration is None:
        duration = env_cfg.duration

    pid = getattr(args, "pid", None)
    height = getattr(args, "height", None)
    stamp = getattr(args, "stamp", None)
    meta_path = Path(args.meta_path) if getattr(args, "meta_path", None) else None
    input_path = Path(args.input) if getattr(args, "input", None) else None
    output_path = Path(args.output) if getattr(args, "output", None) else None
    bt_script_raw = getattr(args, "bt_script", None)
    bt_script = Path(bt_script_raw) if bt_script_raw else None

    return Phase5Config(
        subcommand=args.subcommand,
        datadir=datadir,
        profile_logs=profile_logs,
        bitcoind=bitcoind,
        bitcoin_cli=bitcoin_cli,
        duration=duration,
        start_height=start_height,
        end_height=end_height,
        threshold_ms=threshold_ms,
        pid=pid,
        height=height,
        stamp=stamp,
        meta_path=meta_path,
        input_path=input_path,
        output_path=output_path,
        bt_script=bt_script,
    )


def usage_text(datadir: str | None = None) -> str:
    dd = (
        datadir
        or os.environ.get("DATADIR")
        or os.environ.get("SWORDS_DATADIR")
        or "~/.bitcoin-swords"
    )
    pgrep_pat = pgrep_datadir_pattern(str(Path(dd).expanduser()))
    script = repo_root() / "contrib" / "swords" / "phase5-usdt.sh"
    return f"""Phase 5 USDT/kernel tracing (run alongside bitcoind during IBD)

Subcommands:
  check-usdt    Verify bitcoind has USDT tracepoints (readelf or tplist)
  connectblock  bpftrace validation:block_connected latency (needs root)
  utxo-flush    BCC utxocache:flush logger (needs root + python-bcc)

Environment:
  DATADIR / SWORDS_DATADIR          bitcoind data directory (default ~/.bitcoin-swords)
  PROFILE_LOGS / SWORDS_LOG_ARCHIVE artifact directory
  BITCOIND                          bitcoind binary (fallback when node not running)
  START / END / THRESHOLD_MS        connectblock window (END=0 live)
  DURATION                          utxo-flush seconds (default 60)

Timestamp formats:
  Row timestamps: {ROW_TS_FMT}
  Filenames + meta.txt captured_at_utc: {STAMP_FMT}

Requires:
  bitcoind built with -DENABLE_USDT=ON
  bpftrace (connectblock) and BCC/python-bcc (utxo-flush), typically as root

check-usdt:
  Uses running bitcoind for /proc/<pid>/exe when available; otherwise BITCOIND.
  just check-usdt does not require root.

PID discovery:
  pgrep -f '{pgrep_pat}'

Just recipes:
  just check-usdt
  sudo -E just profile-connectblock
  sudo -E just profile-utxo-flush

Shell (with env preserved):
  sudo -E bash {script} connectblock
  sudo -E bash {script} utxo-flush
"""


def _resolve_start_height(cfg: Phase5Config, current_height: int | None) -> int:
    if cfg.start_height is not None:
        return cfg.start_height
    if current_height is not None:
        return default_connectblock_start_height(current_height)
    return 0


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
    if cfg.subcommand == "utc-now":
        print(utc_now_iso(), end="")
        return 0
    if cfg.subcommand == "utc-stamp":
        print(utc_stamp(), end="")
        return 0
    if cfg.subcommand == "pgrep-pattern":
        print(pgrep_datadir_pattern(str(cfg.datadir)), end="")
        return 0
    if cfg.subcommand == "run-dir":
        assert cfg.stamp is not None
        print(phase5_run_dir(cfg.profile_logs, cfg.stamp), end="")
        return 0
    if cfg.subcommand == "default-start-height":
        assert cfg.height is not None
        print(default_connectblock_start_height(cfg.height), end="")
        return 0
    if cfg.subcommand == "render-bt":
        if cfg.bitcoind is None or cfg.output_path is None:
            print("render-bt requires --bitcoind and --output", file=sys.stderr)
            return 1
        rendered = render_connectblock_bt(cfg.bitcoind, cfg.output_path)
        print(rendered, end="")
        return 0
    if cfg.subcommand == "write-connectblock-summary":
        assert cfg.input_path is not None and cfg.output_path is not None
        text = cfg.input_path.read_text(encoding="utf-8")
        write_connectblock_summary(cfg.output_path, text)
        return 0
    if cfg.subcommand == "write-flush-summary":
        assert cfg.input_path is not None and cfg.output_path is not None
        text = cfg.input_path.read_text(encoding="utf-8")
        write_flush_summary(cfg.output_path, text)
        return 0
    if cfg.subcommand == "write-meta":
        assert cfg.meta_path is not None
        fields = json.load(sys.stdin)
        write_profile_meta(cfg.meta_path, {k: str(v) for k, v in fields.items()})
        print(cfg.meta_path, end="")
        return 0
    if cfg.subcommand == "resolve-bitcoind":
        try:
            bitcoind_path = resolve_bitcoind_path(
                bitcoind=cfg.bitcoind,
                pid=cfg.pid,
            )
        except FileNotFoundError as exc:
            print(str(exc), file=sys.stderr)
            return 1
        print(bitcoind_path, end="")
        return 0

    if cfg.subcommand == "check-usdt":
        try:
            bitcoind_path = resolve_bitcoind_path(
                bitcoind=cfg.bitcoind,
                pid=cfg.pid,
            )
        except FileNotFoundError as exc:
            print(str(exc), flush=True)
            return 1
        result = detect_usdt(bitcoind_path)
        if result.available:
            print(f"USDT available via {result.method}: {bitcoind_path}")
            print("tracepoints:", ", ".join(result.tracepoints))
            missing = result.missing_required()
            if missing:
                print("missing required:", ", ".join(missing))
                return 1
            return 0
        print(result.error or "USDT not available", flush=True)
        return 1

    if cfg.subcommand == "connectblock":
        try:
            bitcoind_path = resolve_bitcoind_path(
                bitcoind=cfg.bitcoind,
                pid=cfg.pid,
            )
        except FileNotFoundError as exc:
            print(str(exc), flush=True)
            return 1
        start = _resolve_start_height(cfg, cfg.height)
        end = cfg.end_height if cfg.end_height is not None else 0
        bt_script = cfg.bt_script
        if bt_script is None:
            bt_script = phase5_run_dir(cfg.profile_logs, "dry-run") / "connectblock_benchmark.bt"
            render_connectblock_bt(bitcoind_path, bt_script)
        cmd = connectblock_cmd(bitcoind_path, start, end, cfg.threshold_ms, bt_script)
        print(shlex.join(cmd))
        return 0

    if cfg.subcommand == "utxo-flush":
        if cfg.pid is None:
            print("utxo-flush requires --pid", flush=True)
            return 1
        cmd = utxo_flush_cmd(cfg.pid)
        print(shlex.join(cmd))
        return 0

    print(
        f"Use contrib/swords/phase5-usdt.sh for live capture "
        f"(parsed subcommand={cfg.subcommand})",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())