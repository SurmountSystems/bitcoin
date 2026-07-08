#!/usr/bin/env python3
"""Phase 6 parameter sweep matrix: one knob at a time IBD operator workflow."""
from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchstats_parse

STATE_FILENAME = "phase6-sweep-state.json"

# Swords IBD defaults shared by every sweep run (one overlay knob per variant).
BASELINE_ARGS: dict[str, str | int] = {
    "benchstats": 1,
    "blockindexsync": 2,
    "txindexbatch": 100,
    "shrinkdebugfile": 0,
}

SWEEP_VARIANTS: tuple[dict[str, Any], ...] = (
    {
        "id": "baseline",
        "label": "baseline",
        "description": "Swords IBD defaults (benchstats=1, blockindexsync=2, txindexbatch=100)",
        "args_overlay": {},
        "optional": False,
    },
    {
        "id": "prefetch_serial",
        "label": "prefetch_serial",
        "description": "Serial coin prefetch baseline (-coinprefetchpar=1) for A/B vs default parallelism",
        "args_overlay": {"coinprefetchpar": 1},
        "optional": False,
    },
    {
        "id": "flush_128m",
        "label": "flush_128m",
        "description": "Raise chainstate LMDB write batch to 128 MiB (-dbbatchsize=134217728)",
        "args_overlay": {"dbbatchsize": 134217728},
        "optional": False,
    },
    {
        "id": "flush_256m",
        "label": "flush_256m",
        "description": "Raise chainstate LMDB write batch to 256 MiB (-dbbatchsize=268435456)",
        "args_overlay": {"dbbatchsize": 268435456},
        "optional": False,
    },
    {
        "id": "script_par",
        "label": "script_par",
        "description": "Explicit script-check threads (-par=N-1; override with SWORDS_SWEEP_PAR)",
        "args_overlay": {},  # filled by script_par_value() at merge time
        "optional": False,
        "dynamic_overlay": "script_par",
    },
    {
        "id": "explicit_parallel",
        "label": "explicit_parallel",
        "description": "Explicit pool sizes: coinprefetchpar=8 blockdecompresspar=4 utxoencodepar=8 (last: worsens readers_full)",
        "args_overlay": {
            "coinprefetchpar": 8,
            "blockdecompresspar": 4,
            "utxoencodepar": 8,
        },
        "optional": False,
    },
    {
        "id": "txindex_500",
        "label": "txindex_500",
        "description": "Optional: larger txindex batch (-txindexbatch=500) if txindex pressure",
        "args_overlay": {"txindexbatch": 500},
        "optional": True,
    },
)

MATRIX_METRIC_FIELDS = (
    "wall_s",
    "disk_per_blk_ms",
    "connect_cs_per_blk_ms",
    "flush_lmdb_per_blk_ms",
    "txindex_per_blk_ms",
    "unaccounted_per_blk_ms",
    "implied_blk_per_s",
    "readers_full",
    "prefetch_hit",
)

# Count metrics: show absolute delta (+N) when baseline is zero and pct is undefined.
COUNT_DELTA_METRICS = frozenset({"readers_full", "prefetch_hit"})

# Controlled sweep campaign order (prefetch_serial control before baseline auto-prefetch).
CAMPAIGN_VARIANT_ORDER: tuple[str, ...] = (
    "prefetch_serial",
    "baseline",
    "txindex_500",
    "flush_128m",
    "flush_256m",
    "script_par",
    "explicit_parallel",
)

PREFETCH_GATE_MILESTONE = 120
SPOT_CHECK_MILESTONES: tuple[int, ...] = (200, 250)
DEFAULT_FLUSH_HOTSPOT_MS = 1.0

# Keys present in the Swords LMDB parallelism log line (for run_metadata cross-check).
LMDB_METADATA_KEYS = frozenset(
    {
        "benchstats",
        "par",
        "coinprefetchpar",
        "utxoencodepar",
        "blockdecompresspar",
        "blockindexsync",
        "txindexbatch",
    }
)

PHASE6_WORKFLOW = """
Operator workflow (one knob per run; reset between variants):
  just sweep-campaign                # gate status + next recommended variant
  just sweep-apply <variant>         # write merged bitcoin.conf (stale keys stripped)
  SWORDS_SWEEP_VARIANT=<variant> just reset-datadir && just sweep-start <variant>
  # IBD to rollup milestones 60, 110, 120, 200, 250 (see DEFAULT_MILESTONES)
  just phase0-check                  # periodic during IBD (readers_full, prefetch)
  just phase3                        # milestone scorecard vs archive
  just sweep-finish <variant>        # post-IBD reset archives debug.log + sweep-record
  # baseline sweep-finish: require rollup #120 in archive (prefetch gate fails closed otherwise)
  just sweep-compare

Variant labeling: sweep-finish exports SWORDS_SWEEP_VARIANT=<variant> for archive stamping
and sweep-record mismatch checks. Each `just` recipe is a separate process — set
SWORDS_SWEEP_VARIANT on the same shell line for manual reset-datadir (e.g. before sweep-start).
reset-datadir writes sweep_variant= to archived meta.txt when the env is set.

Per-variant cycle: sweep-apply → SWORDS_SWEEP_VARIANT=<variant> just reset-datadir && just sweep-start <variant> → phase0-check (periodic) → phase3 → sweep-finish

Campaign order: prefetch_serial → baseline → txindex_500 (optional) → flush_* (if hotspot) → script_par → explicit_parallel (after baseline prefetch gate)

Alternative (review only): just sweep-conf VARIANT prints overlay to profile_logs/phase6-<variant>.conf

Startup line logs effective flags:
  grep 'Swords LMDB parallelism' <datadir>/debug.log

State file (outside datadir):
  {state_path}
"""

PHASE6_EPILOG = """
Variants change exactly one logical knob vs baseline (explicit_parallel sets all
three pool sizes together). Do not combine overlays across runs.

script_par default: -par=(cpu_count - 1). Override: SWORDS_SWEEP_PAR=<n>
txindex_500 is optional — surfaced as optional_next when no required variant is ready;
skip manually if txindex_per_blk_ms is not a hotspot.
""" + PHASE6_WORKFLOW


class SweepStateError(Exception):
    """Raised when sweep state JSON cannot be loaded."""


def script_par_value() -> int:
    override = os.environ.get("SWORDS_SWEEP_PAR")
    if override is not None and override.strip() != "":
        try:
            val = int(override.strip())
        except ValueError as exc:
            raise ValueError(
                f"SWORDS_SWEEP_PAR must be a positive integer (got {override!r})"
            ) from exc
        return max(1, val)
    n = os.cpu_count() or 2
    return max(1, n - 1)


def sweep_state_path(profile_logs: Path | None = None) -> Path:
    root = profile_logs if profile_logs is not None else benchstats_parse.default_profile_logs()
    return root / STATE_FILENAME


def variant_by_id(variant_id: str) -> dict[str, Any]:
    for variant in SWEEP_VARIANTS:
        if variant["id"] == variant_id:
            return variant
    known = ", ".join(v["id"] for v in SWEEP_VARIANTS)
    raise KeyError(f"Unknown variant {variant_id!r}; expected one of: {known}")


def _resolved_overlay(variant: dict[str, Any]) -> dict[str, str | int]:
    overlay = dict(variant.get("args_overlay") or {})
    if variant.get("dynamic_overlay") == "script_par":
        overlay["par"] = script_par_value()
    return overlay


def merged_variant_args(variant: dict[str, Any]) -> dict[str, str | int]:
    merged = dict(BASELINE_ARGS)
    merged.update(_resolved_overlay(variant))
    return merged


def all_sweep_touched_keys() -> set[str]:
    """Union of keys from baseline args and every variant overlay (incl. dynamic par)."""
    keys: set[str] = set()
    for variant in SWEEP_VARIANTS:
        keys.update(merged_variant_args(variant))
    return keys


def overlay_keys_vs_baseline(variant: dict[str, Any]) -> set[str]:
    """Keys that differ from baseline-only args (for one-knob isolation checks)."""
    merged = merged_variant_args(variant)
    return {k for k in merged if merged[k] != BASELINE_ARGS.get(k)}


def variant_start_args(variant: dict[str, Any]) -> list[str]:
    return [f"-{key}={value}" for key, value in sorted(merged_variant_args(variant).items())]


def pct_delta(current: float | None, baseline: float | None) -> float | None:
    if current is None or baseline is None or baseline == 0:
        return None
    return round((current - baseline) / baseline * 100.0, 1)


def format_matrix_delta(field: str, current, baseline) -> str:
    """Format compare-matrix delta cell (percent or absolute for count metrics)."""
    pct = pct_delta(current, baseline)
    if pct is not None:
        return f"{pct:+.1f}%"
    if field in COUNT_DELTA_METRICS and current is not None and baseline is not None:
        cur_i = int(current)
        base_i = int(baseline)
        if cur_i != base_i:
            diff = cur_i - base_i
            return f"+{diff}" if diff > 0 else str(diff)
    return ""


def _parse_conf_lines(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        out[key.strip()] = value.strip()
    return out


def _format_conf_lines(args: dict[str, str | int]) -> list[str]:
    header = [
        "# Phase 6 sweep overlay (merge with your bitcoin.conf or use variant_start_args)",
        f"# generated_at_utc={utc_now_iso()}",
        "",
    ]
    body = [f"{key}={value}" for key, value in sorted(args.items())]
    return header + body


def _reset_sweep_keys_in_conf(base_args: dict[str, str]) -> dict[str, str]:
    """Strip stale sweep keys before applying the next variant overlay."""
    touched = all_sweep_touched_keys()
    baseline_keys = set(BASELINE_ARGS)
    out = dict(base_args)
    for key in touched:
        if key in baseline_keys:
            out[key] = str(BASELINE_ARGS[key])
        else:
            out.pop(key, None)
    return out


def render_bitcoin_conf(
    baseline_path: Path | None,
    variant: dict[str, Any],
    out_path: Path,
) -> Path:
    merged = merged_variant_args(variant)
    if baseline_path is not None and baseline_path.is_file():
        base_args = _parse_conf_lines(baseline_path.read_text(errors="replace"))
        base_args = _reset_sweep_keys_in_conf(base_args)
        base_args.update({k: str(v) for k, v in merged.items()})
        lines = _format_conf_lines({k: base_args[k] for k in sorted(base_args)})
    else:
        lines = _format_conf_lines(merged)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return out_path


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def load_sweep_state(profile_logs: Path | None = None) -> dict[str, Any]:
    path = sweep_state_path(profile_logs)
    if not path.is_file():
        return {"variants": {}, "updated_at": None}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SweepStateError(
            f"error: corrupt state file {path}: {exc}\n"
            f"  hint: mv {path} {path}.bak"
        ) from exc
    data.setdefault("variants", {})
    return data


def save_sweep_state(state: dict[str, Any], profile_logs: Path | None = None) -> Path:
    path = sweep_state_path(profile_logs)
    path.parent.mkdir(parents=True, exist_ok=True)
    state["updated_at"] = utc_now_iso()
    path.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def variant_is_recorded(entry: dict[str, Any] | None) -> bool:
    if not entry:
        return False
    return bool(entry.get("archive_path")) and bool(entry.get("recorded_at"))


def variant_status_label(variant: dict[str, Any], entry: dict[str, Any] | None) -> str:
    if variant_is_recorded(entry):
        status = "done"
    elif entry:
        status = "incomplete"
    else:
        status = "pending"
    if variant.get("optional"):
        status = f"{status}+optional"
    return status


def resolve_archive_dir(archive: str, profile_logs: Path | None = None) -> Path:
    log = benchstats_parse.resolve_archive_log(archive, archive_root=profile_logs)
    return log.parent


def archive_stamp(archive_dir: Path) -> str:
    meta = archive_dir / "meta.txt"
    if meta.is_file():
        for line in meta.read_text(encoding="utf-8").splitlines():
            if line.startswith("archived_at_utc="):
                return line.split("=", 1)[1].strip()
    return archive_dir.name


def find_variant_for_archive(state: dict[str, Any], archive_path: str) -> str | None:
    for vid, entry in state.get("variants", {}).items():
        if entry.get("archive_path") == archive_path:
            return vid
    return None


def validate_run_metadata_overlay(
    variant: dict[str, Any],
    run_metadata: dict[str, Any],
) -> list[str]:
    """Compare logged LMDB parallelism flags against expected variant overlay."""
    warnings: list[str] = []
    expected = merged_variant_args(variant)
    observed = run_metadata.get("lmdb_parallelism") or {}
    for key in sorted(k for k in expected if k in LMDB_METADATA_KEYS):
        exp = expected.get(key)
        obs = observed.get(key)
        if obs is not None and exp is not None and int(obs) != int(exp):
            warnings.append(f"{key}: expected {exp}, observed {obs}")
    return warnings


def record_variant_complete(
    state: dict[str, Any],
    variant_id: str,
    archive_path: str | Path,
    *,
    started_at: str | None = None,
    milestones: tuple[int, ...] = benchstats_parse.DEFAULT_MILESTONES,
    profile_logs: Path | None = None,
    force: bool = False,
) -> tuple[dict[str, Any], list[str]]:
    variant = variant_by_id(variant_id)
    archive = Path(archive_path)
    log_path = archive / "debug.log"
    if not log_path.is_file():
        log_path = archive
    if not log_path.is_file():
        raise FileNotFoundError(f"Missing archived debug.log under {archive_path}")

    archive_dir = archive if archive.is_dir() else archive.parent
    archive_resolved = str(archive_dir.resolve())

    assigned = find_variant_for_archive(state, archive_resolved)
    if assigned is not None and assigned != variant_id and not force:
        raise ValueError(
            f"archive {archive_resolved} already assigned to variant {assigned!r}; "
            "use --force to reassign"
        )

    env_variant = os.environ.get("SWORDS_SWEEP_VARIANT", "").strip()
    if env_variant and env_variant != variant_id:
        raise ValueError(
            f"SWORDS_SWEEP_VARIANT={env_variant!r} does not match --variant {variant_id!r}"
        )

    series = benchstats_parse.parse_benchstats_series(log_path)
    milestone_rows = benchstats_parse.rollup_milestones(series, milestones)
    summary = benchstats_parse.benchstats_summary(series)
    run_metadata = benchstats_parse.parse_run_metadata(log_path)

    metadata_warnings = validate_run_metadata_overlay(variant, run_metadata)

    if started_at is None:
        rs = run_metadata.get("run_started") or {}
        started_at = rs.get("run_ts")

    prior = state.get("variants", {}).get(variant_id)
    overwrite_warning: list[str] = []
    if variant_is_recorded(prior):
        overwrite_warning.append(
            f"warning: overwriting prior record from {prior.get('recorded_at')}"
        )

    entry = {
        "variant_id": variant_id,
        "archive_path": archive_resolved,
        "archive_log": str(log_path),
        "archive_stamp": archive_stamp(archive_dir),
        "recorded_at": utc_now_iso(),
        "started_at": started_at,
        "milestones": milestone_rows,
        "summary": summary,
        "run_metadata": run_metadata,
        "expected_overlay": {
            k: expected_v
            for k, expected_v in merged_variant_args(variant).items()
            if k in overlay_keys_vs_baseline(variant) or k in BASELINE_ARGS
        },
        "metadata_warnings": metadata_warnings,
    }
    state.setdefault("variants", {})[variant_id] = entry
    save_sweep_state(state, profile_logs=profile_logs)
    return entry, overwrite_warning + [
        f"warning: run_metadata mismatch: {w}" for w in metadata_warnings
    ]


def flush_hotspot_threshold_ms() -> float:
    raw = os.environ.get("SWORDS_SWEEP_FLUSH_HOTSPOT_MS", "").strip()
    if not raw:
        return DEFAULT_FLUSH_HOTSPOT_MS
    try:
        return max(0.0, float(raw))
    except ValueError:
        print(
            f"warning: invalid SWORDS_SWEEP_FLUSH_HOTSPOT_MS={raw!r}; "
            f"using default {DEFAULT_FLUSH_HOTSPOT_MS}",
            file=sys.stderr,
        )
        return DEFAULT_FLUSH_HOTSPOT_MS


def _baseline_prefetch_metrics_pass(entry: dict[str, Any]) -> bool:
    milestones = entry.get("milestones") or []
    m120 = next(
        (m for m in milestones if int(m.get("rollup_index", 0)) == PREFETCH_GATE_MILESTONE),
        None,
    )
    if m120 is not None:
        return (
            int(m120.get("readers_full", 0)) == 0
            and int(m120.get("prefetch_hit", 0)) > 0
        )
    if milestones:
        return False
    summary = entry.get("summary") or {}
    return (
        int(summary.get("max_readers_full", 999)) == 0
        and int(summary.get("min_prefetch_hit", 0)) > 0
    )


def baseline_passes_prefetch_gate(state: dict[str, Any]) -> bool:
    """True when baseline is recorded with readers_full=0 and prefetch_hit>0 at #120.

    Fails closed when milestone rows exist but rollup #120 is absent. When no milestone
    rows are stored, falls back to summary max_readers_full / min_prefetch_hit.
    """
    entry = state.get("variants", {}).get("baseline")
    if not variant_is_recorded(entry):
        return False
    return _baseline_prefetch_metrics_pass(entry)


def _prefetch_gate_failure_reason(state: dict[str, Any]) -> str:
    entry = state.get("variants", {}).get("baseline")
    if not variant_is_recorded(entry):
        return "baseline not recorded"
    milestones = entry.get("milestones") or []
    m120 = next(
        (m for m in milestones if int(m.get("rollup_index", 0)) == PREFETCH_GATE_MILESTONE),
        None,
    )
    if m120 is not None:
        rf = int(m120.get("readers_full", 0))
        ph = int(m120.get("prefetch_hit", 0))
        return (
            f"baseline prefetch gate failed at #{PREFETCH_GATE_MILESTONE}: "
            f"readers_full={rf}, prefetch_hit={ph} (need readers_full=0, prefetch_hit>0)"
        )
    if milestones:
        indices = sorted(int(m.get("rollup_index", 0)) for m in milestones)
        return (
            f"baseline prefetch gate blocked: milestone #{PREFETCH_GATE_MILESTONE} not in record "
            f"(have {indices}); do not sweep-finish baseline until rollup "
            f"#{PREFETCH_GATE_MILESTONE}"
        )
    summary = entry.get("summary") or {}
    return (
        f"baseline prefetch gate failed (summary): "
        f"max_readers_full={summary.get('max_readers_full')}, "
        f"min_prefetch_hit={summary.get('min_prefetch_hit')}"
    )


def flush_hotspot_detected(state: dict[str, Any]) -> bool:
    """True when any recorded variant shows flush_lmdb_per_blk_ms above threshold."""
    threshold = flush_hotspot_threshold_ms()
    for entry in state.get("variants", {}).values():
        if not variant_is_recorded(entry):
            continue
        summary = entry.get("summary") or {}
        p50 = summary.get("p50_flush_lmdb_per_blk_ms")
        if p50 is not None and float(p50) > threshold:
            return True
        for milestone in entry.get("milestones") or []:
            val = milestone.get("flush_lmdb_per_blk_ms")
            if val is not None and float(val) > threshold:
                return True
    return False


def _prior_campaign_satisfied(vid: str, gates: dict[str, dict[str, str]]) -> bool:
    idx = CAMPAIGN_VARIANT_ORDER.index(vid)
    for prior_vid in CAMPAIGN_VARIANT_ORDER[:idx]:
        prior = gates[prior_vid]
        status = prior["status"]
        if status in ("done", "skipped"):
            continue
        if status == "ready+optional":
            continue
        return False
    return True


def _first_blocking_prior(vid: str, gates: dict[str, dict[str, str]]) -> str:
    idx = CAMPAIGN_VARIANT_ORDER.index(vid)
    for prior_vid in CAMPAIGN_VARIANT_ORDER[:idx]:
        prior = gates[prior_vid]
        if prior["status"] in ("done", "skipped", "ready+optional"):
            continue
        return f"waiting on {prior_vid} ({prior['status']}: {prior['reason']})"
    return "prior variants not satisfied"


def evaluate_campaign_gates(state: dict[str, Any]) -> dict[str, dict[str, str]]:
    """Per-variant campaign gate status from recorded sweep data."""
    variants = state.get("variants", {})
    gates: dict[str, dict[str, str]] = {}
    prefetch_gate_ok = baseline_passes_prefetch_gate(state)
    flush_hot = flush_hotspot_detected(state)
    threshold = flush_hotspot_threshold_ms()

    for vid in CAMPAIGN_VARIANT_ORDER:
        variant = variant_by_id(vid)
        entry = variants.get(vid)

        if variant_is_recorded(entry):
            gates[vid] = {"status": "done", "reason": "recorded"}
            continue

        if not _prior_campaign_satisfied(vid, gates):
            gates[vid] = {
                "status": "blocked",
                "reason": _first_blocking_prior(vid, gates),
            }
            continue

        if vid in ("flush_128m", "flush_256m") and not flush_hot:
            gates[vid] = {
                "status": "skipped",
                "reason": (
                    f"flush_lmdb_per_blk_ms <= {threshold} ms/blk across recorded variants "
                    "(no flush hotspot)"
                ),
            }
            continue

        if vid == "explicit_parallel" and not prefetch_gate_ok:
            gates[vid] = {
                "status": "blocked",
                "reason": _prefetch_gate_failure_reason(state),
            }
            continue

        if variant.get("optional"):
            gates[vid] = {
                "status": "ready+optional",
                "reason": "optional: run if txindex_per_blk_ms is a hotspot",
            }
        else:
            gates[vid] = {"status": "ready", "reason": "prior variants satisfied"}

    return gates


def spot_check_hints(milestone_index: int) -> list[str] | None:
    """Phase 5 USDT spot-check guidance at milestones 200 and 250 (diagnostic only)."""
    if milestone_index not in SPOT_CHECK_MILESTONES:
        return None
    return [
        f"rollup #{milestone_index} spot-check (diagnostic, not a gate):",
        "  sudo -E just profile-connectblock  # par_jobs=0 in benchstats; traces ConnectBlock path",
        "  sudo -E just profile-utxo-flush    # investigate flush LMDB spikes",
    ]


def spot_check_hints_for_milestones(milestones: Iterable[int]) -> list[str]:
    hints: list[str] = []
    for idx in milestones:
        block = spot_check_hints(idx)
        if block:
            hints.extend(block)
    return hints


def optional_next_variant(state: dict[str, Any]) -> dict[str, Any] | None:
    """First optional variant that is ready but not the required next step."""
    gates = evaluate_campaign_gates(state)
    for vid in CAMPAIGN_VARIANT_ORDER:
        if gates[vid]["status"] == "ready+optional":
            return variant_by_id(vid)
    return None


def next_recommended_variant(state: dict[str, Any]) -> dict[str, Any] | None:
    """Prefer required ``ready`` variants; fall back to ``ready+optional`` when none remain."""
    gates = evaluate_campaign_gates(state)
    for vid in CAMPAIGN_VARIANT_ORDER:
        if gates[vid]["status"] == "ready":
            return variant_by_id(vid)
    for vid in CAMPAIGN_VARIANT_ORDER:
        if gates[vid]["status"] == "ready+optional":
            return variant_by_id(vid)
    return None


def next_recommended_variant_reason(state: dict[str, Any]) -> str | None:
    nxt = next_recommended_variant(state)
    if nxt is None:
        return None
    return evaluate_campaign_gates(state)[nxt["id"]]["reason"]


def build_compare_matrix(
    state: dict[str, Any],
    profile_logs: Path | None = None,
    milestones: tuple[int, ...] = benchstats_parse.DEFAULT_MILESTONES,
) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    variants_meta: list[dict[str, Any]] = []
    notes: list[str] = []

    for variant in SWEEP_VARIANTS:
        vid = variant["id"]
        entry = state.get("variants", {}).get(vid)
        variants_meta.append(
            {
                "id": vid,
                "label": variant["label"],
                "optional": bool(variant.get("optional")),
                "recorded": variant_is_recorded(entry),
            }
        )
        if not variant_is_recorded(entry):
            notes.append(f"variant {vid}: not recorded")
            continue

        log_path = Path(entry.get("archive_log") or "")
        if not log_path.is_file():
            archive_dir = Path(entry.get("archive_path", ""))
            log_path = archive_dir / "debug.log"
        if not log_path.is_file():
            notes.append(f"variant {vid}: missing archive log at {log_path}")
            continue

        series = benchstats_parse.parse_benchstats_series(log_path)
        milestone_rows = benchstats_parse.rollup_milestones(series, milestones)
        by_idx = {int(m["rollup_index"]): m for m in milestone_rows}
        summary = benchstats_parse.benchstats_summary(series)

        for idx in milestones:
            m = by_idx.get(idx)
            row = {
                "variant_id": vid,
                "variant_label": variant["label"],
                "rollup_index": idx,
                "archive_log": str(log_path),
                "summary_implied_blk_per_s": summary.get("implied_blk_per_s"),
            }
            if m is None:
                row["missing_milestone"] = True
                for field in MATRIX_METRIC_FIELDS:
                    row[field] = None
                    row[f"{field}_delta_pct"] = None
                    row[f"{field}_delta_display"] = ""
            else:
                row["missing_milestone"] = False
                for field in MATRIX_METRIC_FIELDS:
                    row[field] = m.get(field)
                    row[f"{field}_delta_pct"] = None
                    row[f"{field}_delta_display"] = ""
            rows.append(row)

    baseline_by_idx = {
        int(r["rollup_index"]): r
        for r in rows
        if r.get("variant_id") == "baseline" and not r.get("missing_milestone")
    }
    for row in rows:
        if row.get("variant_id") == "baseline" or row.get("missing_milestone"):
            continue
        base = baseline_by_idx.get(int(row["rollup_index"]))
        if base is None:
            continue
        for field in MATRIX_METRIC_FIELDS:
            cur = row.get(field)
            base_val = base.get(field)
            row[f"{field}_delta_pct"] = pct_delta(cur, base_val)
            row[f"{field}_delta_display"] = format_matrix_delta(field, cur, base_val)

    return {
        "milestones": list(milestones),
        "variants": variants_meta,
        "rows": rows,
        "notes": notes,
    }


def _print_campaign_recommendations(
    state: dict[str, Any],
    gates: dict[str, dict[str, str]],
) -> None:
    next_var = next_recommended_variant(state)
    opt_var = optional_next_variant(state)
    if next_var is not None:
        reason = gates[next_var["id"]]["reason"]
        print(f"next_recommended\t{next_var['id']}")
        print(f"next_reason\t{reason}")
        print(f"next_command\tjust sweep-apply {next_var['id']}")
    else:
        print("next_recommended\t(campaign complete or all remaining variants blocked/skipped)")
    if opt_var is not None and (next_var is None or opt_var["id"] != next_var["id"]):
        print(f"optional_next\t{opt_var['id']}")
        print(f"optional_reason\t{gates[opt_var['id']]['reason']}")
        print(f"optional_command\tjust sweep-apply {opt_var['id']}")


def print_campaign(state: dict[str, Any], profile_logs: Path | None = None) -> None:
    state_path = sweep_state_path(profile_logs)
    gates = evaluate_campaign_gates(state)
    threshold = flush_hotspot_threshold_ms()

    print("phase6_sweep_campaign")
    print(f"state_file\t{state_path}")
    print(f"flush_hotspot_threshold_ms\t{threshold}")
    print(f"baseline_prefetch_gate\t{'pass' if baseline_passes_prefetch_gate(state) else 'fail'}")
    print(f"flush_hotspot_detected\t{flush_hotspot_detected(state)}")
    print(
        f"baseline_gate_note\trollup #{PREFETCH_GATE_MILESTONE} required in baseline record "
        "(fails closed if missing)"
    )
    print()
    print("campaign_order")
    for i, vid in enumerate(CAMPAIGN_VARIANT_ORDER, start=1):
        gate = gates[vid]
        variant = variant_by_id(vid)
        print(f"{i}.\t{vid}\t{gate['status']}\t{gate['reason']}")
        if variant.get("optional"):
            print("\t\t(optional)")
    print()
    _print_campaign_recommendations(state, gates)
    print()
    print("spot_checks (milestones 200, 250 — diagnostic only)")
    for idx in SPOT_CHECK_MILESTONES:
        for line in spot_check_hints(idx) or []:
            print(f"  {line}")


def print_sweep_plan(state: dict[str, Any], profile_logs: Path | None = None) -> None:
    state_path = sweep_state_path(profile_logs)
    print("phase6_parameter_sweep")
    print(f"state_file\t{state_path}")
    print()
    print("variants (one knob per run; just reset-datadir between each)")
    next_var = next_recommended_variant(state)
    gates = evaluate_campaign_gates(state)
    variants = state.get("variants", {})
    for i, variant in enumerate(SWEEP_VARIANTS, start=1):
        vid = variant["id"]
        entry = variants.get(vid)
        status = variant_status_label(variant, entry)
        marker = ""
        if next_var is not None and next_var["id"] == vid and not variant_is_recorded(entry):
            marker = "\tnext"
        gate = gates.get(vid)
        gate_s = ""
        if gate and gate["status"] not in ("done", "ready", "ready+optional"):
            gate_s = f"\tcampaign={gate['status']}"
        overlay = _resolved_overlay(variant)
        overlay_s = ",".join(f"{k}={v}" for k, v in sorted(overlay.items())) or "(baseline only)"
        print(
            f"{i}.\t{vid}\t{status}{marker}{gate_s}\t{variant['description']}\n"
            f"\toverlay\t{overlay_s}"
        )
    print()
    _print_campaign_recommendations(state, gates)
    print()
    print(PHASE6_WORKFLOW.format(state_path=state_path).strip())


def print_compare_matrix(matrix: dict[str, Any]) -> None:
    print("phase6_compare_matrix")
    delta_fields = [f"{f}_delta_pct" for f in MATRIX_METRIC_FIELDS]
    header = (
        ["variant", "milestone"]
        + list(MATRIX_METRIC_FIELDS)
        + delta_fields
        + ["summary_implied_blk_per_s"]
    )
    print("\t".join(header))

    rows = matrix.get("rows", [])
    milestones_hit: dict[str, int] = {}
    milestones_total = len(matrix.get("milestones") or [])
    for row in rows:
        vid = row.get("variant_id", "")
        if row.get("missing_milestone"):
            milestones_hit.setdefault(vid, 0)
            cells = [
                vid,
                f"#{row.get('rollup_index')}",
            ]
            cells.extend(["—"] * len(MATRIX_METRIC_FIELDS))
            cells.extend([""] * len(delta_fields))
            cells.append("")
            print("\t".join(cells))
        else:
            milestones_hit[vid] = milestones_hit.get(vid, 0) + 1
            cells = [
                vid,
                f"#{row.get('rollup_index')}",
            ]
            for field in MATRIX_METRIC_FIELDS:
                val = row.get(field)
                cells.append("" if val is None else str(val))
            for field in delta_fields:
                display_key = field.replace("_delta_pct", "_delta_display")
                display = row.get(display_key)
                if display:
                    cells.append(display)
                else:
                    val = row.get(field)
                    cells.append("" if val is None else f"{val:+.1f}%")
            cells.append(
                ""
                if row.get("summary_implied_blk_per_s") is None
                else str(row["summary_implied_blk_per_s"])
            )
            print("\t".join(cells))

    if milestones_total and milestones_hit:
        print()
        print("milestones_hit")
        for variant in SWEEP_VARIANTS:
            vid = variant["id"]
            if vid in milestones_hit:
                print(f"  {vid}\t{milestones_hit[vid]}/{milestones_total}")

    missing = [n for n in matrix.get("notes", []) if n]
    if missing:
        print()
        print("notes")
        for note in missing:
            print(f"  {note}")
    if not any(not r.get("missing_milestone") for r in rows):
        print()
        print("notes\t(no recorded variants with milestone data; run just sweep-record <variant>)")


def print_status(state: dict[str, Any], profile_logs: Path | None = None) -> None:
    path = sweep_state_path(profile_logs)
    print(f"state_file\t{path}")
    print(f"updated_at\t{state.get('updated_at')}")
    print()
    print("variants")
    variants = state.get("variants", {})
    next_var = next_recommended_variant(state)
    for variant in SWEEP_VARIANTS:
        vid = variant["id"]
        entry = variants.get(vid)
        status = variant_status_label(variant, entry)
        marker = ""
        if next_var is not None and next_var["id"] == vid and not variant_is_recorded(entry):
            marker = "\tnext"
        line = f"{vid}\t{status}{marker}"
        if variant_is_recorded(entry):
            line += (
                f"\tarchive={entry.get('archive_path')}"
                f"\trecorded_at={entry.get('recorded_at')}"
                f"\tstarted_at={entry.get('started_at')}"
            )
        elif entry:
            line += "\t(incomplete entry — re-run just sweep-record)"
        print(line)
    gates = evaluate_campaign_gates(state)
    print()
    _print_campaign_recommendations(state, gates)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Phase 6 parameter sweep matrix operator workflow",
        epilog=PHASE6_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "command",
        choices=("plan", "status", "campaign", "record", "compare", "conf", "args", "json"),
        help="operator subcommand",
    )
    ap.add_argument("--variant", "-v", help="sweep variant id")
    ap.add_argument(
        "--archive",
        default="latest",
        help="archive dir/path for record (default: latest)",
    )
    ap.add_argument(
        "--profile-logs",
        default=None,
        help="profile log archive root (default: SWORDS_LOG_ARCHIVE)",
    )
    ap.add_argument(
        "--datadir",
        default=None,
        help="datadir for baseline bitcoin.conf merge (default: SWORDS_DATADIR)",
    )
    ap.add_argument(
        "--out",
        default=None,
        help="output path for conf subcommand",
    )
    ap.add_argument(
        "--milestones",
        default=benchstats_parse.default_milestones_arg(),
        help="comma-separated rollup indices for record/compare",
    )
    ap.add_argument(
        "--force",
        action="store_true",
        help="allow record to reassign archive or overwrite prior entry",
    )
    args = ap.parse_args()

    profile_logs = (
        Path(args.profile_logs)
        if args.profile_logs
        else benchstats_parse.default_profile_logs()
    )

    try:
        milestones = benchstats_parse.parse_milestones_arg(args.milestones)
    except ValueError:
        print(
            f"error: invalid --milestones {args.milestones!r}; "
            "expected comma-separated positive integers",
            file=sys.stderr,
        )
        return 1
    if not milestones:
        print("error: --milestones must list at least one rollup index", file=sys.stderr)
        return 1

    if os.environ.get("SWORDS_SWEEP_PAR", "").strip():
        try:
            script_par_value()
        except ValueError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1

    try:
        state = load_sweep_state(profile_logs)
    except SweepStateError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if args.command == "plan":
        print_sweep_plan(state, profile_logs=profile_logs)
        return 0

    if args.command == "status":
        print_status(state, profile_logs=profile_logs)
        return 0

    if args.command == "campaign":
        print_campaign(state, profile_logs=profile_logs)
        return 0

    if args.command == "json":
        next_var = next_recommended_variant(state)
        opt_var = optional_next_variant(state)
        gates = evaluate_campaign_gates(state)
        payload = {
            "baseline_args": BASELINE_ARGS,
            "campaign_variant_order": list(CAMPAIGN_VARIANT_ORDER),
            "campaign_gates": gates,
            "baseline_prefetch_gate_pass": baseline_passes_prefetch_gate(state),
            "baseline_prefetch_gate_milestone": PREFETCH_GATE_MILESTONE,
            "flush_hotspot_detected": flush_hotspot_detected(state),
            "flush_hotspot_threshold_ms": flush_hotspot_threshold_ms(),
            "spot_check_milestones": list(SPOT_CHECK_MILESTONES),
            "variants": [
                {
                    **{k: v for k, v in variant.items() if k != "args_overlay"},
                    "merged_args": merged_variant_args(variant),
                    "overlay_keys": sorted(overlay_keys_vs_baseline(variant)),
                }
                for variant in SWEEP_VARIANTS
            ],
            "state": state,
            "compare_matrix": build_compare_matrix(state, profile_logs, milestones),
            "next_recommended": next_var["id"] if next_var is not None else None,
            "next_recommended_reason": (
                gates[next_var["id"]]["reason"] if next_var is not None else None
            ),
            "optional_next": opt_var["id"] if opt_var is not None else None,
            "optional_next_reason": (
                gates[opt_var["id"]]["reason"]
                if opt_var is not None
                and (next_var is None or opt_var["id"] != next_var["id"])
                else None
            ),
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
        return 0

    if args.command in ("conf", "args", "record") and not args.variant:
        print("error: --variant required", file=sys.stderr)
        return 1

    try:
        if args.command == "conf":
            variant = variant_by_id(args.variant)
            datadir = (
                Path(args.datadir)
                if args.datadir
                else benchstats_parse.default_datadir()
            )
            baseline_conf = datadir / "bitcoin.conf"
            out = (
                Path(args.out)
                if args.out
                else profile_logs / f"phase6-{args.variant}.conf"
            )
            render_bitcoin_conf(
                baseline_conf if baseline_conf.is_file() else None,
                variant,
                out,
            )
            print(f"conf_written\t{out}")
            print("start_args\t" + " ".join(variant_start_args(variant)))
            return 0

        if args.command == "args":
            variant = variant_by_id(args.variant)
            for arg in variant_start_args(variant):
                print(arg)
            return 0

        if args.command == "record":
            variant_by_id(args.variant)
            try:
                archive_dir = resolve_archive_dir(args.archive, profile_logs=profile_logs)
            except FileNotFoundError as exc:
                print(str(exc), file=sys.stderr)
                return 1
            try:
                entry, notices = record_variant_complete(
                    state,
                    args.variant,
                    archive_dir,
                    milestones=milestones,
                    profile_logs=profile_logs,
                    force=args.force,
                )
            except ValueError as exc:
                print(f"error: {exc}", file=sys.stderr)
                return 1
            for notice in notices:
                print(notice, file=sys.stderr)
            print(f"recorded\t{args.variant}")
            print(f"archive_path\t{entry['archive_path']}")
            print(f"archive_log\t{entry['archive_log']}")
            print(f"archive_stamp\t{entry.get('archive_stamp')}")
            print(f"started_at\t{entry.get('started_at')}")
            recorded_indices = [
                int(m["rollup_index"])
                for m in entry.get("milestones") or []
                if m.get("rollup_index") is not None
            ]
            hints = spot_check_hints_for_milestones(recorded_indices)
            if hints:
                print()
                print("spot_check_hints")
                for line in hints:
                    print(line)
            return 0

    except KeyError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.command == "compare":
        matrix = build_compare_matrix(state, profile_logs, milestones)
        print_compare_matrix(matrix)
        return 0

    print(f"unhandled command: {args.command}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())