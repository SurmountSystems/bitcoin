#!/usr/bin/env bash
# Capture IBD read-path microbench baselines to share/swords/bench-baselines/<host>-<sha>.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/build"
BENCH="${BUILD}/bin/bench_bitcoin"
OUT_DIR="${ROOT}/share/swords/bench-baselines"
DATADIR="${DATADIR:-${HOME}/.bitcoin-swords}"

if [[ ! -x "${BENCH}" ]]; then
  echo "bench_bitcoin not found; run: just configure-bench && just build-bench" >&2
  exit 1
fi

HOST="$(hostname -s 2>/dev/null || hostname)"
SHA="$(git -C "${ROOT}" rev-parse --short HEAD)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="${OUT_DIR}/${HOST}-${SHA}-${STAMP}.json"
mkdir -p "${OUT_DIR}"

TMP="$(mktemp)"
"${BENCH}" -priority-level=high -output-json="${TMP}" >/dev/null

SEGMENT_JSON="null"
if [[ -f "${DATADIR}/debug.log" ]]; then
  SEGMENT_JSON="$(python3 "${ROOT}/contrib/swords/parse-reindex-log.py" "${DATADIR}" --json 2>/dev/null || echo "null")"
fi

python3 - "${TMP}" "${OUT}" "${HOST}" "${SHA}" "${STAMP}" "${SEGMENT_JSON}" <<'PY'
import json, sys
bench_json, out_path, host, sha, stamp, segment_raw = sys.argv[1:7]
with open(bench_json) as f:
    data = json.load(f)
medians = {}
for entry in data.get("benchmarks", []):
    name = entry.get("name")
    if not name:
        continue
    medians[name] = entry.get("median", entry.get("real_time", 0))
doc = {
    "host": host,
    "git_sha": sha,
    "captured_at": stamp,
    "medians_ns": medians,
}
segment = json.loads(segment_raw)
if segment:
    doc["segment_replay"] = segment
with open(out_path, "w") as f:
    json.dump(doc, f, indent=2, sort_keys=True)
print(out_path)
PY

rm -f "${TMP}"