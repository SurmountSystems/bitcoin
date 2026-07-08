#!/usr/bin/env python3
"""IBD microbench baseline capture and compare (shared by shell wrappers and CLI).

When ``segment_replay.benchstats_summary`` is present in a baseline, compare checks
``p50_disk_per_blk_ms``, ``p50_connect_cs_per_blk_ms``, ``p50_flush_lmdb_per_blk_ms``,
and ``implied_blk_per_s`` (WARN if |delta| > 10%; exit 1). ``rollup_count`` is shown in
``show`` output but intentionally not compared.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, NamedTuple, Sequence

# Bench medians: positive delta (slower) > 5% => FAIL; |delta| > 10% => WARN.
BENCH_FAIL_REGRESSION = 0.05
WARN_ABS_DELTA = 0.10

SEGMENT_FIELDS = ("blocks_per_hr", "p50_load_ms", "p50_connect_ms")
BENCHSTATS_FIELDS = (
    "p50_disk_per_blk_ms",
    "p50_connect_cs_per_blk_ms",
    "p50_flush_lmdb_per_blk_ms",
    "implied_blk_per_s",
)


class CompareRow(NamedTuple):
    mark: str
    label: str
    baseline: float | None
    current: float | None
    delta_pct: float | None
    affects_exit: bool = True


def default_baselines_dir(root: Path | None = None) -> Path:
    base = root or Path(__file__).resolve().parents[2]
    return base / "share" / "swords" / "bench-baselines"


def default_bench_binary(root: Path | None = None) -> Path:
    base = root or Path(__file__).resolve().parents[2]
    return base / "build" / "bin" / "bench_bitcoin"


def default_datadir() -> Path:
    home = Path.home()
    return Path(
        os.environ.get("SWORDS_DATADIR")
        or os.environ.get("DATADIR")
        or (home / ".bitcoin-swords")
    )


def medians_from_bench_json(data: dict[str, Any]) -> dict[str, float]:
    medians: dict[str, float] = {}
    for entry in data.get("benchmarks", []):
        name = entry.get("name")
        if not name:
            continue
        medians[str(name)] = float(entry.get("median", entry.get("real_time", 0)) or 0)
    return medians


def capture_from_bench_json(
    bench_output_path: str | Path,
    host: str,
    sha: str,
    stamp: str,
    segment_replay: dict[str, Any] | None = None,
) -> dict[str, Any]:
    with open(bench_output_path, encoding="utf-8") as f:
        data = json.load(f)
    doc: dict[str, Any] = {
        "host": host,
        "git_sha": sha,
        "captured_at": stamp,
        "medians_ns": medians_from_bench_json(data),
    }
    if segment_replay:
        doc["segment_replay"] = segment_replay
    return doc


def write_baseline_doc(doc: dict[str, Any], out_path: str | Path) -> Path:
    path = Path(out_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
    return path


def find_latest_baseline(baselines_dir: str | Path) -> Path | None:
    root = Path(baselines_dir)
    if not root.is_dir():
        return None
    candidates = [p for p in root.glob("*.json") if p.is_file()]
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


def _pct_delta(current: float, baseline: float) -> float:
    return (current - baseline) / baseline * 100.0


def _compare_bench_row(name: str, base_ns: float, cur_ns: float | None) -> CompareRow | None:
    if base_ns <= 0:
        return None
    if cur_ns is None:
        return CompareRow(
            mark="WARN",
            label=f"missing bench: {name}",
            baseline=base_ns,
            current=None,
            delta_pct=None,
            affects_exit=False,
        )
    delta = (cur_ns - base_ns) / base_ns
    pct = _pct_delta(cur_ns, base_ns)
    mark = "OK"
    if delta > BENCH_FAIL_REGRESSION:
        mark = "FAIL"
    elif abs(delta) > WARN_ABS_DELTA:
        mark = "WARN"
    return CompareRow(
        mark=mark,
        label=name,
        baseline=base_ns,
        current=cur_ns,
        delta_pct=pct,
    )


def _compare_segment_field(
    prefix: str,
    field: str,
    base_val: float,
    cur_val: float | None,
) -> CompareRow | None:
    if base_val is None or base_val <= 0:
        return None
    label = f"{prefix}{field}"
    if cur_val is None:
        return CompareRow(
            mark="WARN",
            label=label,
            baseline=base_val,
            current=None,
            delta_pct=None,
        )
    delta = (cur_val - base_val) / base_val
    pct = _pct_delta(cur_val, base_val)
    mark = "OK"
    if abs(delta) > WARN_ABS_DELTA:
        mark = "WARN"
    return CompareRow(
        mark=mark,
        label=label,
        baseline=base_val,
        current=cur_val,
        delta_pct=pct,
    )


def compare_baselines(
    baseline_doc: dict[str, Any],
    current_medians_ns: dict[str, float],
    current_segment: dict[str, Any] | None,
) -> list[CompareRow]:
    rows: list[CompareRow] = []
    for name, base_ns in sorted(baseline_doc.get("medians_ns", {}).items()):
        cur_ns = current_medians_ns.get(name)
        row = _compare_bench_row(name, float(base_ns), cur_ns)
        if row is not None:
            rows.append(row)

    base_seg = baseline_doc.get("segment_replay") or {}
    cur_seg = current_segment or {}
    if base_seg:
        for field in SEGMENT_FIELDS:
            base_val = base_seg.get(field)
            if base_val is None:
                continue
            row = _compare_segment_field("segment.", field, float(base_val), _float_or_none(cur_seg.get(field)))
            if row is not None:
                rows.append(row)

        base_bs = base_seg.get("benchstats_summary") or {}
        if base_bs:
            cur_bs = cur_seg.get("benchstats_summary") or {}
            for field in BENCHSTATS_FIELDS:
                base_val = base_bs.get(field)
                if base_val is None:
                    continue
                row = _compare_segment_field(
                    "benchstats.",
                    field,
                    float(base_val),
                    _float_or_none(cur_bs.get(field)),
                )
                if row is not None:
                    rows.append(row)

    return rows


def _float_or_none(val: Any) -> float | None:
    if val is None:
        return None
    try:
        return float(val)
    except (TypeError, ValueError):
        return None


def compare_exit_code(rows: Sequence[CompareRow]) -> int:
    if any(r.mark == "FAIL" and r.affects_exit for r in rows):
        return 2
    if any(r.mark == "WARN" and r.affects_exit for r in rows):
        return 1
    return 0


def format_compare_report(rows: list[CompareRow], *, segment_header: bool = True) -> str:
    lines: list[str] = []
    segment_printed = False
    for row in rows:
        if row.label.startswith("segment.") or row.label.startswith("benchstats."):
            if segment_header and not segment_printed:
                lines.append("segment_replay:")
                segment_printed = True
            lines.append(_format_segment_row(row))
            continue
        if row.label.startswith("missing bench:"):
            lines.append(f"WARN {row.label}")
            continue
        lines.append(_format_bench_row(row))
    return "\n".join(lines)


def _format_bench_row(row: CompareRow) -> str:
    if row.delta_pct is None:
        return (
            f"{row.mark} {row.label}: "
            f"baseline={row.baseline:.0f}ns current={row.current:.0f}ns"
        )
    return (
        f"{row.mark} {row.label}: "
        f"baseline={row.baseline:.0f}ns current={row.current:.0f}ns "
        f"({row.delta_pct:+.1f}%)"
    )


def _format_segment_row(row: CompareRow) -> str:
    if row.current is None:
        return f"WARN {row.label}: missing in current capture"
    if row.delta_pct is None:
        return f"{row.mark} {row.label}: baseline={row.baseline} current={row.current}"
    return (
        f"{row.mark} {row.label}: baseline={row.baseline} current={row.current} "
        f"({row.delta_pct:+.1f}%)"
    )


def list_baselines(baselines_dir: str | Path) -> list[Path]:
    root = Path(baselines_dir)
    if not root.is_dir():
        return []
    return sorted(
        (p for p in root.glob("*.json") if p.is_file()),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )


def load_baseline_doc(path: str | Path) -> dict[str, Any]:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def format_baseline_summary(doc: dict[str, Any], path: str | Path | None = None) -> str:
    lines: list[str] = []
    if path is not None:
        lines.append(f"baseline\t{path}")
    lines.append(f"host\t{doc.get('host', '')}")
    lines.append(f"git_sha\t{doc.get('git_sha', '')}")
    lines.append(f"captured_at\t{doc.get('captured_at', '')}")
    medians = doc.get("medians_ns") or {}
    lines.append(f"benchmarks\t{len(medians)}")
    for name in sorted(medians):
        lines.append(f"median_ns\t{name}\t{medians[name]:.0f}")
    seg = doc.get("segment_replay")
    if seg:
        lines.append("segment_replay\tyes")
        for key in ("blocks", "blocks_per_hr", "p50_load_ms", "p50_connect_ms"):
            if key in seg:
                lines.append(f"segment\t{key}\t{seg[key]}")
        bs = seg.get("benchstats_summary") or {}
        if bs:
            lines.append(f"benchstats_summary\trollup_count={bs.get('rollup_count', 0)}")
            for key in BENCHSTATS_FIELDS:
                if key in bs:
                    lines.append(f"benchstats\t{key}\t{bs[key]}")
    else:
        lines.append("segment_replay\tno")
    return "\n".join(lines)