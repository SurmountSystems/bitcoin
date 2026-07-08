#!/usr/bin/env bash
# Compare current HIGH-priority bench medians against a baseline JSON.
# Exits non-zero when any benchmark regresses more than 5%.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASELINE="${1:-}"
export DATADIR="${DATADIR:-${HOME}/.bitcoin-swords}"

ARGS=(compare)
if [[ -n "${BASELINE}" ]]; then
  ARGS+=("${BASELINE}")
fi

exec python3 "${ROOT}/contrib/swords/baseline_report.py" "${ARGS[@]}"