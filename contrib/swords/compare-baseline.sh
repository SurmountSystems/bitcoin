#!/usr/bin/env bash
# Compare current HIGH-priority bench medians against a baseline JSON.
# Exits non-zero when any benchmark regresses more than 5%.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/build"
BENCH="${BUILD}/bin/bench_bitcoin"
BASELINE="${1:-}"

if [[ -z "${BASELINE}" ]]; then
  BASELINE="$(ls -t "${ROOT}"/share/swords/bench-baselines/*.json 2>/dev/null | head -1 || true)"
fi
if [[ -z "${BASELINE}" || ! -f "${BASELINE}" ]]; then
  echo "Usage: $0 [baseline.json]" >&2
  exit 1
fi
if [[ ! -x "${BENCH}" ]]; then
  echo "bench_bitcoin not found; run: just configure-bench && just build-bench" >&2
  exit 1
fi

TMP="$(mktemp)"
"${BENCH}" -priority-level=high -output-json="${TMP}" >/dev/null

DATADIR="${DATADIR:-${HOME}/.bitcoin-swords}"
CUR_SEGMENT="{}"
if [[ -f "${DATADIR}/debug.log" ]]; then
  CUR_SEGMENT="$(python3 "${ROOT}/contrib/swords/parse-reindex-log.py" "${DATADIR}" --json 2>/dev/null || echo "{}")"
fi

python3 - "${BASELINE}" "${TMP}" "${CUR_SEGMENT}" <<'PY'
import json, sys
baseline_path, current_path, cur_segment_raw = sys.argv[1:4]
with open(baseline_path) as f:
    base = json.load(f)
with open(current_path) as f:
    cur = json.load(f)
cur_map = {e["name"]: e.get("median", e.get("real_time", 0)) for e in cur.get("benchmarks", [])}
failed = False
warned = False
for name, base_ns in sorted(base.get("medians_ns", {}).items()):
    cur_ns = cur_map.get(name)
    if cur_ns is None:
        print(f"WARN missing bench: {name}")
        continue
    if base_ns <= 0:
        continue
    delta = (cur_ns - base_ns) / base_ns
    pct = delta * 100
    mark = "OK"
    if delta > 0.05:
        mark = "FAIL"
        failed = True
    elif abs(delta) > 0.10:
        mark = "WARN"
        warned = True
    print(f"{mark} {name}: baseline={base_ns:.0f}ns current={cur_ns:.0f}ns ({pct:+.1f}%)")

base_seg = base.get("segment_replay") or {}
cur_seg = json.loads(cur_segment_raw) if cur_segment_raw else {}
if base_seg:
    print("segment_replay:")
    for field in ("blocks_per_hr", "p50_load_ms", "p50_connect_ms"):
        base_val = base_seg.get(field)
        if base_val is None or base_val <= 0:
            continue
        cur_val = cur_seg.get(field)
        if cur_val is None:
            print(f"WARN segment.{field}: missing in current capture")
            warned = True
            continue
        delta = (cur_val - base_val) / base_val
        pct = delta * 100
        mark = "OK"
        if abs(delta) > 0.10:
            mark = "WARN"
            warned = True
        print(f"{mark} segment.{field}: baseline={base_val} current={cur_val} ({pct:+.1f}%)")

if failed:
    sys.exit(2)
if warned:
    sys.exit(1)
PY

rm -f "${TMP}"