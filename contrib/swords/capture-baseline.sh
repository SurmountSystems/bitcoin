#!/usr/bin/env bash
# Capture IBD read-path microbench baselines to share/swords/bench-baselines/<host>-<sha>.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export DATADIR="${DATADIR:-${HOME}/.bitcoin-swords}"

exec python3 "${ROOT}/contrib/swords/baseline_report.py" capture "$@"