#!/usr/bin/env python3
"""Gate helper for DEBUG_ONLY parallelism flag promotion (Workstream F).

Promotion removes ArgsManager::DEBUG_ONLY from -benchstats, -coinprefetchpar,
-blockdecompresspar, and -utxoencodepar in src/init.cpp. Run only after the
mainnet sweep campaign passes automated exit gates below.

Automated checks (exit 1 when any fail):
  - All four flags still carry DEBUG_ONLY in src/init.cpp (not yet promoted)
  - prefetch_serial and baseline variants recorded in sweep state
  - baseline_prefetch_gate from archive log (rollup #120: readers_full=0, prefetch_hit>0)
  - baseline implied_blk_per_s beats prefetch_serial at milestones 120, 200, 250
  - baseline IBD depth >= rollup #250 with readers_full=0 and prefetch_hit>0

Operator gates (printed; not auto-checked):
  - just verify green
  - just phase0-check returns 0 during healthy IBD
  - explicit_parallel variant recorded (full campaign tail)
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import phase6_sweep

ROOT = Path(__file__).resolve().parents[2]
INIT_CPP = ROOT / "src" / "init.cpp"

DEBUG_ONLY_FLAGS: tuple[str, ...] = (
    "benchstats",
    "coinprefetchpar",
    "blockdecompresspar",
    "utxoencodepar",
)

PROMOTION_MILESTONES: tuple[int, ...] = (120, 200, 250)
PREFETCH_GATE_ROLLUP_INDEX = 120
MIN_IBD_ROLLUP_INDEX = 250
REQUIRED_RECORDED_VARIANTS: tuple[str, ...] = ("prefetch_serial", "baseline")


@dataclass(frozen=True)
class GateCheck:
    name: str
    passed: bool
    detail: str
    automated: bool = True


def _flag_line_pattern(flag: str) -> re.Pattern[str]:
    return re.compile(
        rf'AddArg\("-{re.escape(flag)}=',
        re.MULTILINE,
    )


def _addarg_span(text: str, flag: str) -> str | None:
    """Return the AddArg(...) call text for -flag= (may span multiple lines)."""
    match = _flag_line_pattern(flag).search(text)
    if match is None:
        return None
    rest = text[match.start() :]
    terminator = "OptionsCategory::OPTIONS);"
    end = rest.find(terminator)
    if end == -1:
        return rest[:800]
    return rest[: end + len(terminator)]


def debug_only_flags_present(init_cpp: Path = INIT_CPP) -> list[GateCheck]:
    """True when every parallelism flag still has DEBUG_ONLY (promotion not done)."""
    text = init_cpp.read_text(encoding="utf-8")
    checks: list[GateCheck] = []
    for flag in DEBUG_ONLY_FLAGS:
        span = _addarg_span(text, flag)
        if span is None:
            checks.append(
                GateCheck(
                    f"debug_only_{flag}",
                    False,
                    f"missing AddArg for -{flag}= in {init_cpp}",
                )
            )
            continue
        has_debug = "DEBUG_ONLY" in span
        checks.append(
            GateCheck(
                f"debug_only_{flag}",
                has_debug,
                (
                    f"-{flag}= still DEBUG_ONLY"
                    if has_debug
                    else f"-{flag}= no longer DEBUG_ONLY — promotion already applied?"
                ),
            )
        )
    return checks


def _variant_recorded(state: dict[str, Any], variant_id: str) -> GateCheck:
    entry = state.get("variants", {}).get(variant_id)
    ok = phase6_sweep.variant_is_recorded(entry)
    return GateCheck(
        f"recorded_{variant_id}",
        ok,
        f"{variant_id} recorded in sweep state" if ok else f"{variant_id} not recorded",
    )


def baseline_prefetch_gate(
    state: dict[str, Any],
    rollup_index: int = PREFETCH_GATE_ROLLUP_INDEX,
) -> GateCheck:
    """Require log-based rollup #120 with readers_full=0 and prefetch_hit>0.

    Unlike phase6_sweep.baseline_passes_prefetch_gate, promotion does not accept
    summary-only fallback when milestone rows are absent from sweep state.
    """
    log_path = _baseline_entry_log_path(state)
    if log_path is None:
        return GateCheck(
            "baseline_prefetch_gate",
            False,
            "baseline archive log missing",
        )
    import benchstats_parse

    series = benchstats_parse.parse_benchstats_series(log_path)
    if not series:
        return GateCheck(
            "baseline_prefetch_gate",
            False,
            "no benchstats rollups in baseline log",
        )
    target = next(
        (r for r in series if int(r["rollup_index"]) == rollup_index),
        None,
    )
    if target is None:
        max_idx = max(int(r["rollup_index"]) for r in series)
        return GateCheck(
            "baseline_prefetch_gate",
            False,
            f"rollup #{rollup_index} not present in baseline log (max #{max_idx})",
        )
    readers_full = int(target.get("readers_full", 0))
    prefetch_hit = int(target.get("prefetch_hit", 0))
    if readers_full > 0 or prefetch_hit <= 0:
        return GateCheck(
            "baseline_prefetch_gate",
            False,
            f"rollup #{rollup_index}: readers_full={readers_full}, "
            f"prefetch_hit={prefetch_hit} (need readers_full=0, prefetch_hit>0)",
        )
    return GateCheck(
        "baseline_prefetch_gate",
        True,
        f"rollup #{rollup_index} healthy (readers_full=0, prefetch_hit={prefetch_hit})",
    )


def _milestone_implied_blk(
    matrix: dict[str, Any], variant_id: str, rollup_index: int
) -> float | None:
    for row in matrix.get("rows", []):
        if (
            row.get("variant_id") == variant_id
            and int(row.get("rollup_index", 0)) == rollup_index
            and not row.get("missing_milestone")
        ):
            val = row.get("implied_blk_per_s")
            return float(val) if val is not None else None
    return None


def baseline_beats_prefetch_serial(
    state: dict[str, Any],
    profile_logs: Path | None,
    milestones: tuple[int, ...] = PROMOTION_MILESTONES,
) -> GateCheck:
    matrix = phase6_sweep.build_compare_matrix(state, profile_logs, milestones)
    failures: list[str] = []
    for idx in milestones:
        baseline = _milestone_implied_blk(matrix, "baseline", idx)
        serial = _milestone_implied_blk(matrix, "prefetch_serial", idx)
        if baseline is None:
            failures.append(f"#{idx}: baseline milestone missing")
            continue
        if serial is None:
            failures.append(f"#{idx}: prefetch_serial milestone missing")
            continue
        if baseline <= serial:
            failures.append(
                f"#{idx}: baseline implied_blk_per_s={baseline} "
                f"<= prefetch_serial={serial}"
            )
    if failures:
        return GateCheck(
            "baseline_beats_prefetch_serial",
            False,
            "; ".join(failures),
        )
    return GateCheck(
        "baseline_beats_prefetch_serial",
        True,
        f"baseline faster than prefetch_serial at milestones {list(milestones)}",
    )


def _baseline_entry_log_path(state: dict[str, Any]) -> Path | None:
    entry = state.get("variants", {}).get("baseline")
    if not phase6_sweep.variant_is_recorded(entry):
        return None
    log_path = Path(entry.get("archive_log") or "")
    if log_path.is_file():
        return log_path
    archive_dir = Path(entry.get("archive_path", ""))
    candidate = archive_dir / "debug.log"
    return candidate if candidate.is_file() else None


def baseline_ibd_depth_gate(
    state: dict[str, Any],
    min_rollup_index: int = MIN_IBD_ROLLUP_INDEX,
) -> GateCheck:
    """Require IBD past min_rollup_index with healthy prefetch at that rollup."""
    log_path = _baseline_entry_log_path(state)
    if log_path is None:
        return GateCheck(
            "baseline_ibd_depth",
            False,
            "baseline archive log missing",
        )
    import benchstats_parse

    series = benchstats_parse.parse_benchstats_series(log_path)
    if not series:
        return GateCheck("baseline_ibd_depth", False, "no benchstats rollups in baseline log")
    max_idx = max(int(r["rollup_index"]) for r in series)
    if max_idx < min_rollup_index:
        return GateCheck(
            "baseline_ibd_depth",
            False,
            f"max rollup #{max_idx} < #{min_rollup_index} ({min_rollup_index * 1000} blocks)",
        )
    target = next(
        (r for r in series if int(r["rollup_index"]) == min_rollup_index),
        None,
    )
    if target is None:
        return GateCheck(
            "baseline_ibd_depth",
            False,
            f"rollup #{min_rollup_index} not present (max #{max_idx})",
        )
    readers_full = int(target.get("readers_full", 0))
    prefetch_hit = int(target.get("prefetch_hit", 0))
    if readers_full > 0 or prefetch_hit <= 0:
        return GateCheck(
            "baseline_ibd_depth",
            False,
            f"rollup #{min_rollup_index}: readers_full={readers_full}, "
            f"prefetch_hit={prefetch_hit} (need readers_full=0, prefetch_hit>0)",
        )
    return GateCheck(
        "baseline_ibd_depth",
        True,
        f"rollup #{min_rollup_index} healthy (readers_full=0, prefetch_hit={prefetch_hit})",
    )


def operator_gate_reminders() -> list[GateCheck]:
    return [
        GateCheck(
            "operator_verify_green",
            False,
            "run `just verify` and confirm exit 0",
            automated=False,
        ),
        GateCheck(
            "operator_phase0_check",
            False,
            "run `just phase0-check` during healthy IBD and confirm exit 0",
            automated=False,
        ),
        GateCheck(
            "operator_explicit_parallel",
            False,
            "record explicit_parallel variant after baseline gate (campaign tail)",
            automated=False,
        ),
    ]


def evaluate_promotion_gates(
    profile_logs: Path | None = None,
    *,
    init_cpp: Path = INIT_CPP,
) -> list[GateCheck]:
    checks: list[GateCheck] = []
    checks.extend(debug_only_flags_present(init_cpp))
    try:
        state = phase6_sweep.load_sweep_state(profile_logs)
    except phase6_sweep.SweepStateError as exc:
        checks.append(
            GateCheck(
                "sweep_state_load",
                False,
                str(exc),
            )
        )
        return checks
    for vid in REQUIRED_RECORDED_VARIANTS:
        checks.append(_variant_recorded(state, vid))
    checks.append(baseline_prefetch_gate(state))
    checks.append(baseline_beats_prefetch_serial(state, profile_logs))
    checks.append(baseline_ibd_depth_gate(state))
    return checks


def promotion_ready(checks: list[GateCheck]) -> bool:
    return all(c.passed for c in checks if c.automated)


def print_promotion_report(checks: list[GateCheck], *, include_operator: bool = True) -> None:
    print("promotion_gates")
    print(f"status\t{'ready' if promotion_ready(checks) else 'blocked'}")
    for check in checks:
        status = "pass" if check.passed else "fail"
        kind = "auto" if check.automated else "manual"
        print(f"gate\t{check.name}\t{status}\t{kind}\t{check.detail}")
    if include_operator:
        print("operator_checklist")
        for item in operator_gate_reminders():
            print(f"  - {item.detail}")
    if not promotion_ready(checks):
        print(
            "promotion_blocked: DEBUG_ONLY flags must remain until all automated gates pass; "
            "then complete operator checklist before editing src/init.cpp",
            file=sys.stderr,
        )


def default_profile_logs() -> Path:
    home = Path.home()
    return Path(
        __import__("os").environ.get(
            "SWORDS_LOG_ARCHIVE",
            str(home / ".bitcoin-swords-profile-logs"),
        )
    )


def parse_argv(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check sweep campaign gates before DEBUG_ONLY parallelism promotion",
    )
    parser.add_argument(
        "--profile-logs",
        type=Path,
        default=None,
        help="profile-log archive root (default: SWORDS_LOG_ARCHIVE or ~/.bitcoin-swords-profile-logs)",
    )
    parser.add_argument(
        "--init-cpp",
        type=Path,
        default=INIT_CPP,
        help="path to src/init.cpp (default: repo src/init.cpp)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit machine-readable JSON on stdout",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_argv(argv)
    profile_logs = args.profile_logs or default_profile_logs()
    checks = evaluate_promotion_gates(profile_logs, init_cpp=args.init_cpp)
    if args.json:
        import json

        payload = {
            "ready": promotion_ready(checks),
            "profile_logs": str(profile_logs),
            "gates": [
                {
                    "name": c.name,
                    "passed": c.passed,
                    "automated": c.automated,
                    "detail": c.detail,
                }
                for c in checks
            ],
            "operator_checklist": [c.detail for c in operator_gate_reminders()],
        }
        print(json.dumps(payload, indent=2))
    else:
        print_promotion_report(checks)
    return 0 if promotion_ready(checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())