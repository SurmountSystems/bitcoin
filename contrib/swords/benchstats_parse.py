#!/usr/bin/env python3
"""Parse benchstats rollups and run metadata from Swords debug.log.

``unaccounted_ms`` is only computed for rollup_index > 1, using inter-rollup
``interval_wall_ms`` (not run-start → first rollup, which includes startup).

``_accounted_ms`` includes ``abc_idle`` but excludes ``cs_main_hold``,
``flush_mutex_wait``, and ``blkidx_mutex_wait``. Mutex waits are embedded in
``chainstate_flush`` / ``block_index_flush`` totals; ``cs_main_hold`` overlaps
connect work. Those three fields are standalone diagnostics only.

Three distinct block-rate metrics (do not mix when comparing outputs):

- ``interval_implied_blk_per_s`` — per inter-rollup interval (series/CSV only).
- ``implied_blk_per_s`` in milestones — cumulative avg since run start.
- ``implied_blk_per_s`` in summary — sustained rate rollup 1→last (excludes startup gap).
"""
from __future__ import annotations

import os
import re
from datetime import datetime
from pathlib import Path
from typing import Iterable

TS_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z)\s+"
)
SWORDS_RUN_STARTED_RE = re.compile(
    r"=== Swords run started (?P<run_ts>\S+) \(datadir=(?P<datadir>\S+) benchstats=(?P<benchstats>\d+)\) ==="
)
SWORDS_LMDB_PARALLELISM_RE = re.compile(
    r"Swords LMDB parallelism: benchstats=(?P<benchstats>\d+) par=(?P<par>\d+) "
    r"coinprefetchpar=(?P<coinprefetchpar>\d+) utxoencodepar=(?P<utxoencodepar>\d+) "
    r"blockdecompresspar=(?P<blockdecompresspar>\d+) flushsnapshot=(?P<flushsnapshot>\d+) "
    r"blockindexsync=(?P<blockindexsync>\d+) txindexbatch=(?P<txindexbatch>\d+) "
    r"chainstate_maxreaders=(?P<chainstate_maxreaders>\d+)"
)
BENCHSTATS_RE = re.compile(r"benchstats:")

# Zero-valued wait section matching MaybeLogBenchStats output (for test fixtures).
BENCHSTATS_WAIT_SECTION_ZERO = (
    "wait cs_main_hold=0.0ms flush_mutex_wait=0.0ms blkidx_mutex_wait=0.0ms "
    "abc_idle=0.0ms script_jobs=0 script_done=0 | "
)

# Canonical field names in rollup dicts (missing keys default to 0).
ROLLUP_FLOAT_FIELDS = (
    "disk",
    "decompress",
    "prefetch_wait",
    "coin_lmdb",
    "coin_decode",
    "coin_warm",
    "chainstate_flush",
    "block_index_flush",
    "encode",
    "flush_lmdb",
    "txindex",
    "connect_cs",
    "cs_main_hold",
    "flush_mutex_wait",
    "blkidx_mutex_wait",
    "abc_idle",
)
ROLLUP_INT_FIELDS = (
    "par_jobs",
    "prefetch_hit",
    "coin_prevouts",
    "coin_miss",
    "skip_thr",
    "skip_wrk",
    "readers_full",
    "stale",
    "parallel",
    "blkidx_sync",
    "script_jobs",
    "script_done",
    "blocks",
)

# Aliases from log tokens -> canonical names.
_FIELD_ALIASES = {
    "lmdb": "coin_lmdb",
    "decode": "coin_decode",
    "warm": "coin_warm",
    "chainstate": "chainstate_flush",
    "block_index": "block_index_flush",
    "prevouts": "coin_prevouts",
    "miss": "coin_miss",
}


def parse_ts(line: str) -> datetime | None:
    m = TS_RE.match(line)
    if not m:
        return None
    raw = m.group("ts")
    if "." in raw:
        return datetime.strptime(raw, "%Y-%m-%dT%H:%M:%S.%fZ")
    return datetime.strptime(raw, "%Y-%m-%dT%H:%M:%SZ")


def strip_ts_prefix(line: str) -> str:
    m = TS_RE.match(line)
    if not m:
        return line
    ts_end = m.end("ts")
    if ts_end < len(line) and line[ts_end] == " ":
        return line[ts_end + 1 :]
    return line[ts_end:]


def p50(vals: list[float]) -> float:
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[len(s) // 2]


def default_datadir() -> Path:
    home = Path.home()
    return Path(
        os.environ.get("SWORDS_DATADIR")
        or os.environ.get("DATADIR")
        or (home / ".bitcoin-swords")
    )


def default_profile_logs() -> Path:
    home = Path.home()
    return Path(
        os.environ.get("SWORDS_LOG_ARCHIVE")
        or os.environ.get("PROFILE_LOGS")
        or (home / ".bitcoin-swords-profile-logs")
    )


def resolve_log_path(datadir_or_log: str, log_arg: str | None) -> Path:
    if log_arg:
        return Path(log_arg)
    path = Path(datadir_or_log)
    if path.is_file():
        return path
    return path / "debug.log"


DEFAULT_MILESTONES: tuple[int, ...] = (60, 110, 120, 200, 250)


def default_milestones_arg() -> str:
    """Comma-separated rollup indices for argparse --milestones defaults."""
    return ",".join(str(m) for m in DEFAULT_MILESTONES)


def parse_milestones_arg(raw: str) -> tuple[int, ...]:
    return tuple(int(x.strip()) for x in raw.split(",") if x.strip())


def resolve_archive_log(
    archive: str,
    archive_root: Path | None = None,
) -> Path:
    """Resolve archived debug.log from profile-logs dir, symlink, or direct file."""
    root = archive_root if archive_root is not None else default_profile_logs()
    target = archive
    if target == "latest":
        latest_link = root / "latest"
        if latest_link.is_symlink():
            target = str(latest_link.resolve())
        elif (root / "LATEST.txt").is_file():
            target = (root / "LATEST.txt").read_text().strip()
        else:
            raise FileNotFoundError(f"No archived logs under {root}")
    log = Path(target) / "debug.log"
    if log.is_file():
        return log
    path = Path(target)
    if path.is_file():
        return path
    raise FileNotFoundError(f"Missing archive log at {log}")


def _parse_numeric_value(raw: str) -> float | int:
    if raw.endswith("ms"):
        return float(raw[:-2])
    if "." in raw:
        return float(raw)
    return int(raw)


def _coerce_rollup_value(key: str, value: float | int) -> float | int:
    if key in ROLLUP_FLOAT_FIELDS:
        return float(value)
    return int(value)


def parse_benchstats_line(line: str) -> dict | None:
    """Parse one benchstats rollup line; returns None if not a benchstats line."""
    ts = parse_ts(line)
    body = strip_ts_prefix(line)
    if not BENCHSTATS_RE.search(body):
        return None

    out: dict = {"timestamp": ts.isoformat() + "Z" if ts else None}
    section: str | None = None
    for token in body.split():
        if token in ("benchstats:", "|"):
            continue
        if "=" not in token:
            section = token
            continue
        key, raw_val = token.split("=", 1)
        if section == "flush" and key == "lmdb":
            key = "flush_lmdb"
        elif key in _FIELD_ALIASES:
            key = _FIELD_ALIASES[key]
        value = _parse_numeric_value(raw_val)
        out[key] = _coerce_rollup_value(key, value)

    if "blocks" not in out:
        return None
    for field in ROLLUP_FLOAT_FIELDS:
        out.setdefault(field, 0.0)
    for field in ROLLUP_INT_FIELDS:
        if field != "blocks":
            out.setdefault(field, 0)
    return out


def _read_lines(lines_or_path: str | Path | Iterable[str]) -> list[str]:
    if isinstance(lines_or_path, (str, Path)):
        path = Path(lines_or_path)
        return path.read_text(errors="replace").splitlines()
    return list(lines_or_path)


def parse_run_metadata_from_lines(lines: Iterable[str]) -> dict:
    """Extract Swords run header metadata from log lines."""
    run_started: dict | None = None
    lmdb_parallelism: dict | None = None
    for line in lines:
        body = strip_ts_prefix(line)
        if m := SWORDS_RUN_STARTED_RE.search(body):
            run_started = {
                "run_ts": m.group("run_ts"),
                "datadir": m.group("datadir"),
                "benchstats": int(m.group("benchstats")),
            }
        if m := SWORDS_LMDB_PARALLELISM_RE.search(body):
            lmdb_parallelism = {
                "benchstats": int(m.group("benchstats")),
                "par": int(m.group("par")),
                "coinprefetchpar": int(m.group("coinprefetchpar")),
                "utxoencodepar": int(m.group("utxoencodepar")),
                "blockdecompresspar": int(m.group("blockdecompresspar")),
                "flushsnapshot": int(m.group("flushsnapshot")),
                "blockindexsync": int(m.group("blockindexsync")),
                "txindexbatch": int(m.group("txindexbatch")),
                "chainstate_maxreaders": int(m.group("chainstate_maxreaders")),
            }
    out: dict = {}
    if run_started is not None:
        out["run_started"] = run_started
    if lmdb_parallelism is not None:
        out["lmdb_parallelism"] = lmdb_parallelism
    return out


def parse_run_metadata(lines_or_path: str | Path | Iterable[str]) -> dict:
    """Extract Swords run header metadata from log lines or path."""
    return parse_run_metadata_from_lines(_read_lines(lines_or_path))


def run_start_ts_from_lines(lines: Iterable[str]) -> datetime | None:
    last: datetime | None = None
    for line in lines:
        body = strip_ts_prefix(line)
        if not SWORDS_RUN_STARTED_RE.search(body):
            continue
        ts = parse_ts(line)
        if ts is not None:
            last = ts
        elif m := SWORDS_RUN_STARTED_RE.search(body):
            try:
                last = datetime.strptime(m.group("run_ts"), "%Y-%m-%dT%H:%M:%SZ")
            except ValueError:
                pass
    return last


def _accounted_ms(rollup: dict) -> float:
    """Sum of instrumented ms fields for one rollup interval.

    Excludes cs_main_hold, flush_mutex_wait, and blkidx_mutex_wait (diagnostic
    only; mutex waits overlap flush totals, cs_main_hold overlaps connect work).
    """
    return (
        float(rollup.get("disk", 0.0))
        + float(rollup.get("decompress", 0.0))
        + float(rollup.get("prefetch_wait", 0.0))
        + float(rollup.get("coin_lmdb", 0.0))
        + float(rollup.get("coin_decode", 0.0))
        + float(rollup.get("coin_warm", 0.0))
        + float(rollup.get("chainstate_flush", 0.0))
        + float(rollup.get("block_index_flush", 0.0))
        + float(rollup.get("encode", 0.0))
        + float(rollup.get("flush_lmdb", 0.0))
        + float(rollup.get("txindex", 0.0))
        + float(rollup.get("connect_cs", 0.0))
        + float(rollup.get("abc_idle", 0.0))
    )


def unaccounted_ms(rollup: dict) -> float | None:
    """Inter-rollup wall ms minus summed instrumented ms (rollup_index > 1 only)."""
    if int(rollup.get("rollup_index", 0)) <= 1:
        return None
    interval = rollup.get("interval_wall_ms")
    if interval is None:
        return None
    return float(interval) - _accounted_ms(rollup)


def interval_implied_blk_per_s(rollup: dict) -> float | None:
    """Blocks/s over the inter-rollup wall interval (rollup_index > 1 only).

    Stored on series rows as ``interval_implied_blk_per_s`` (not milestone/summary
    ``implied_blk_per_s``).
    """
    if int(rollup.get("rollup_index", 0)) <= 1:
        return None
    interval_ms = rollup.get("interval_wall_ms")
    if interval_ms is None or float(interval_ms) <= 0:
        return None
    blocks = int(rollup.get("blocks", 0))
    if blocks <= 0:
        return None
    wall_s = float(interval_ms) / 1000.0
    return round(blocks / wall_s, 2)


def finish_benchstats_series(
    rollups: list[tuple[str, dict]],
    run_start: datetime | None,
) -> list[dict]:
    series: list[dict] = []
    prev_ts: datetime | None = None
    for line, rollup in rollups:
        ts = parse_ts(line)
        rollup["rollup_index"] = len(series) + 1
        if ts is not None and run_start is not None:
            rollup["wall_s"] = round((ts - run_start).total_seconds(), 1)
        else:
            rollup["wall_s"] = None
        if ts is not None and prev_ts is not None:
            rollup["interval_wall_ms"] = round(
                (ts - prev_ts).total_seconds() * 1000.0, 1
            )
            gap = unaccounted_ms(rollup)
            if gap is not None:
                rollup["unaccounted_ms"] = round(gap, 1)
            implied = interval_implied_blk_per_s(rollup)
            if implied is not None:
                rollup["interval_implied_blk_per_s"] = implied
        series.append(rollup)
        if ts is not None:
            prev_ts = ts
    return series


def parse_benchstats_series(lines_or_path: str | Path | Iterable[str]) -> list[dict]:
    """Ordered benchstats rollups with rollup_index (1-based) and timing metadata."""
    lines = _read_lines(lines_or_path)
    run_start = run_start_ts_from_lines(lines)
    rollups: list[tuple[str, dict]] = []
    for line in lines:
        rollup = parse_benchstats_line(line)
        if rollup is not None:
            rollups.append((line, rollup))
    return finish_benchstats_series(rollups, run_start)


def per_block_ms(rollup: dict, field: str) -> float | None:
    blocks = int(rollup.get("blocks", 0))
    if blocks <= 0:
        return None
    return float(rollup.get(field, 0.0)) / blocks


def milestone_implied_blk_per_s(rollup: dict) -> float | None:
    """Cumulative blocks/s up to a milestone rollup (from run start)."""
    wall_s = rollup.get("wall_s")
    if wall_s is None or wall_s <= 0:
        return None
    blocks = int(rollup.get("rollup_index", 0)) * int(rollup.get("blocks", 1000))
    if blocks <= 0:
        return None
    return round(blocks / float(wall_s), 2)


def rollup_milestones(
    series: list[dict],
    milestones: tuple[int, ...] = DEFAULT_MILESTONES,
) -> list[dict]:
    """Key metrics at rollup milestones (wall_s from run start).

    ``implied_blk_per_s`` is cumulative blocks/s since run start (milestone rate).
    """
    by_index = {int(r["rollup_index"]): r for r in series}
    out: list[dict] = []
    for idx in milestones:
        rollup = by_index.get(idx)
        if rollup is None:
            continue
        entry = {
            "rollup_index": idx,
            "wall_s": rollup.get("wall_s"),
            "blocks": rollup.get("blocks"),
            "disk_per_blk_ms": round(per_block_ms(rollup, "disk") or 0.0, 2),
            "connect_cs_per_blk_ms": round(per_block_ms(rollup, "connect_cs") or 0.0, 2),
            "flush_lmdb_per_blk_ms": round(per_block_ms(rollup, "flush_lmdb") or 0.0, 2),
            "txindex_per_blk_ms": round(per_block_ms(rollup, "txindex") or 0.0, 2),
            "unaccounted_per_blk_ms": None,
        }
        gap = rollup.get("unaccounted_ms")
        blocks = int(rollup.get("blocks", 0))
        if gap is not None and blocks > 0:
            entry["unaccounted_per_blk_ms"] = round(float(gap) / blocks, 2)
        entry["implied_blk_per_s"] = milestone_implied_blk_per_s(rollup)
        entry["readers_full"] = int(rollup.get("readers_full", 0))
        entry["prefetch_hit"] = int(rollup.get("prefetch_hit", 0))
        out.append(entry)
    return out


def benchstats_summary(series: list[dict]) -> dict:
    """Aggregate rollup stats for regression snapshots.

    ``implied_blk_per_s`` is sustained blocks/s from rollup 1 wall time to the last
    rollup (excludes startup→rollup-1 gap). ``block_span`` uses
    ``(last_idx - first_idx) * last.blocks``, which assumes uniform per-rollup block
    counts; a partial final rollup (e.g. shutdown ``force=true``) can skew the rate.
    """
    if not series:
        return {"rollup_count": 0}

    disk_pb = [per_block_ms(r, "disk") for r in series]
    connect_pb = [per_block_ms(r, "connect_cs") for r in series]
    flush_pb = [per_block_ms(r, "flush_lmdb") for r in series]
    txindex_pb = [per_block_ms(r, "txindex") for r in series]
    disk_pb_f = [v for v in disk_pb if v is not None]
    connect_pb_f = [v for v in connect_pb if v is not None]
    flush_pb_f = [v for v in flush_pb if v is not None]
    txindex_pb_f = [v for v in txindex_pb if v is not None]
    readers_full_vals = [int(r.get("readers_full", 0)) for r in series]
    prefetch_hit_vals = [int(r.get("prefetch_hit", 0)) for r in series]

    last = series[-1]
    first = series[0]
    implied_blk_per_s: float | None = None
    if (
        first.get("wall_s") is not None
        and last.get("wall_s") is not None
        and last["wall_s"] > first["wall_s"]
    ):
        block_span = (int(last["rollup_index"]) - int(first["rollup_index"])) * int(
            last.get("blocks", 1000)
        )
        if block_span > 0:
            implied_blk_per_s = round(
                block_span / (float(last["wall_s"]) - float(first["wall_s"])),
                2,
            )

    return {
        "rollup_count": len(series),
        "last_rollup_index": last["rollup_index"],
        "last_wall_s": last.get("wall_s"),
        "last_disk_per_blk_ms": round(per_block_ms(last, "disk") or 0.0, 2),
        "last_connect_cs_per_blk_ms": round(per_block_ms(last, "connect_cs") or 0.0, 2),
        "last_flush_lmdb_per_blk_ms": round(per_block_ms(last, "flush_lmdb") or 0.0, 2),
        "last_txindex_per_blk_ms": round(per_block_ms(last, "txindex") or 0.0, 2),
        "p50_disk_per_blk_ms": round(p50(disk_pb_f), 2),
        "p50_connect_cs_per_blk_ms": round(p50(connect_pb_f), 2),
        "p50_flush_lmdb_per_blk_ms": round(p50(flush_pb_f), 2),
        "p50_txindex_per_blk_ms": round(p50(txindex_pb_f), 2),
        "p50_readers_full": int(p50([float(v) for v in readers_full_vals])),
        "max_readers_full": max(readers_full_vals) if readers_full_vals else 0,
        "p50_prefetch_hit": int(p50([float(v) for v in prefetch_hit_vals])),
        "min_prefetch_hit": min(prefetch_hit_vals) if prefetch_hit_vals else 0,
        "implied_blk_per_s": implied_blk_per_s,
    }


if __name__ == "__main__":
    import sys

    if len(sys.argv) >= 2 and sys.argv[1] == "default-milestones":
        print(default_milestones_arg())
        raise SystemExit(0)
    print(
        "usage: python3 benchstats_parse.py default-milestones",
        file=sys.stderr,
    )
    raise SystemExit(1)