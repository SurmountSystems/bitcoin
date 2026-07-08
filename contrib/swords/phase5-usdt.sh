#!/usr/bin/env bash
# Phase 5 USDT/kernel tracing: connectblock bpftrace, utxo flush BCC.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PY="${ROOT}/contrib/swords/phase5_usdt.py"
SCRIPT="${ROOT}/contrib/swords/phase5-usdt.sh"
# shellcheck source=swords-shell-common.sh
source "${ROOT}/contrib/swords/swords-shell-common.sh"

eval "$(python3 "${PY}" shell-export)"
BITCOIND="${BITCOIND:-${ROOT}/build/bin/bitcoind}"
BITCOIN_CLI="${BITCOIN_CLI:-${ROOT}/build/bin/bitcoin-cli}"
DURATION="${DURATION:-60}"
START="${START:-}"
END="${END:-0}"
THRESHOLD_MS="${THRESHOLD_MS:-25}"

usage() {
  DATADIR="${DATADIR}" python3 "${PY}" help
}

require_cmd() {
  local cmd="$1"
  local hint="$2"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "${cmd} not found; ${hint}" >&2
    exit 1
  fi
}

require_root() {
  if [[ "$(id -u)" -ne 0 ]]; then
    echo "eBPF USDT tracing requires root (sudo)." >&2
    echo "Retry: sudo -E bash ${SCRIPT} ${SUBCMD:-help} ..." >&2
    echo "Or: sudo -E just profile-connectblock / sudo -E just profile-utxo-flush" >&2
    exit 1
  fi
}

utc_now() {
  python3 "${PY}" utc-now 2>/dev/null || date -u +%Y-%m-%dT%H:%M:%SZ
}

utc_stamp() {
  python3 "${PY}" utc-stamp 2>/dev/null || date -u +%Y%m%dT%H%M%SZ
}

resolve_bitcoind_binary() {
  local pid="${1:-}"
  local resolved=""
  if [[ -n "${pid}" && -e "/proc/${pid}/exe" ]]; then
    resolved="$(readlink -f "/proc/${pid}/exe" 2>/dev/null || true)"
    if [[ -n "${resolved}" && -f "${resolved}" ]]; then
      echo "${resolved}"
      return
    fi
  fi
  if [[ -x "${BITCOIND}" ]]; then
    readlink -f "${BITCOIND}" 2>/dev/null || echo "${BITCOIND}"
  fi
}

resolve_check_usdt_binary() {
  local pid=""
  local bitcoind_path=""
  pid="$(try_bitcoind_pid)"
  if [[ -n "${pid}" ]]; then
    bitcoind_path="$(resolve_bitcoind_binary "${pid}")"
  fi
  if [[ -z "${bitcoind_path}" || ! -f "${bitcoind_path}" ]]; then
    if [[ -x "${BITCOIND}" ]]; then
      bitcoind_path="$(readlink -f "${BITCOIND}" 2>/dev/null || echo "${BITCOIND}")"
    fi
  fi
  if [[ -z "${bitcoind_path}" || ! -f "${bitcoind_path}" ]]; then
    echo "bitcoind binary not found; start bitcoind or set BITCOIND" >&2
    exit 1
  fi
  echo "${bitcoind_path}"
}

check_usdt_binary() {
  local bitcoind_path="$1"
  python3 "${PY}" check-usdt --bitcoind "${bitcoind_path}"
}

cmd_check_usdt() {
  local bitcoind_path=""
  bitcoind_path="$(resolve_check_usdt_binary)"
  check_usdt_binary "${bitcoind_path}"
}

cmd_connectblock() {
  require_root
  require_cmd bpftrace "install: sudo apt install bpftrace  # or pacman -S bpftrace"

  local pid=""
  local stamp=""
  local finished=""
  local run_dir=""
  local out_path=""
  local meta_path=""
  local height=""
  local bitcoind_path=""
  local bt_script=""
  local start_height=""
  local bpf_ver=""
  local interrupted="false"
  local connect_status=0

  pid="$(find_bitcoind_pid)"
  stamp="$(utc_stamp)"
  height="$(get_block_height)"
  bitcoind_path="$(resolve_bitcoind_binary "${pid}")"
  if [[ -z "${bitcoind_path}" || ! -f "${bitcoind_path}" ]]; then
    echo "bitcoind binary not found (pid=${pid})" >&2
    exit 1
  fi
  check_usdt_binary "${bitcoind_path}"

  if [[ -n "${START}" ]]; then
    start_height="${START}"
  elif [[ -n "${height}" && "${height}" =~ ^[0-9]+$ ]]; then
    start_height="$(python3 "${PY}" default-start-height --height "${height}")"
  else
    start_height="0"
  fi

  run_dir="$(python3 "${PY}" run-dir --stamp "${stamp}" --profile-logs "${PROFILE_LOGS}")"
  out_path="${run_dir}/connectblock.txt"
  meta_path="${run_dir}/meta.txt"
  bt_script="${run_dir}/connectblock_benchmark.bt"
  bpf_ver="$(bpftrace --version 2>/dev/null | head -1 || echo unknown)"

  mkdir -p "${run_dir}"
  python3 "${PY}" render-bt --bitcoind "${bitcoind_path}" --output "${bt_script}" >/dev/null

  trap 'interrupted=true; echo "Connectblock tracing interrupted" >&2' INT TERM

  echo "Connectblock USDT trace pid=${pid} bitcoind=${bitcoind_path}"
  echo "Window: start=${start_height} end=${END} threshold_ms=${THRESHOLD_MS}"
  echo "Output: ${out_path}"
  echo "Rendered script: ${bt_script}"

  set +e
  bpftrace "${bt_script}" "${start_height}" "${END}" "${THRESHOLD_MS}" \
    | tee "${out_path}"
  connect_status=${PIPESTATUS[0]}
  set -e
  finished="$(utc_stamp)"

  if [[ "${interrupted}" == "true" || "${connect_status}" -ne 0 ]]; then
    interrupted="true"
  fi

  python3 "${PY}" write-connectblock-summary \
    --input "${out_path}" --output "${run_dir}/connectblock-summary.json"

  python3 - "${ROOT}" "${stamp}" "${finished}" "${height:-unknown}" "${DATADIR}" "${pid}" \
    "${PROFILE_LOGS}" "${out_path}" "${bitcoind_path}" "${bt_script}" \
    "${start_height}" "${END}" "${THRESHOLD_MS}" "${bpf_ver}" "${interrupted}" <<'PY' \
    | python3 "${PY}" write-meta --meta-path "${meta_path}"
import json, sys
from pathlib import Path

ROOT = Path(sys.argv[1])
sys.path.insert(0, str(ROOT / "contrib" / "swords"))
from phase5_usdt import detect_usdt

(
    captured,
    finished,
    height,
    datadir,
    pid,
    profile_logs,
    out_path,
    bitcoind_path,
    bt_script,
    start_height,
    end_height,
    threshold_ms,
    bpf_ver,
    interrupted,
) = sys.argv[2:16]

result = detect_usdt(Path(bitcoind_path))
print(json.dumps({
    "captured_at_utc": captured,
    "run_finished_at_utc": finished,
    "subcommand": "connectblock",
    "block_height": height,
    "datadir": datadir,
    "pid": pid,
    "profile_logs": profile_logs,
    "connectblock_output": out_path,
    "bitcoind_path": bitcoind_path,
    "connectblock_bt_script": bt_script,
    "start_height": start_height,
    "end_height": end_height,
    "threshold_ms": threshold_ms,
    "usdt_tracepoints": ",".join(result.tracepoints),
    "usdt_detection_method": result.method,
    "bpftrace_version": bpf_ver,
    "interrupted": interrupted,
}))
PY

  echo "Wrote ${out_path}"
  echo "Wrote ${meta_path}"
  if [[ "${interrupted}" == "true" ]]; then
    exit "${connect_status:-1}"
  fi
}

cmd_utxo_flush() {
  require_root
  require_cmd python3 "install python3"
  require_cmd timeout "install: coreutils timeout"

  if ! python3 -c "import bcc" 2>/dev/null; then
    echo "python-bcc not installed; install BCC (python bindings)." >&2
    echo "  Ubuntu: sudo apt install bpfcc-tools python3-bpfcc" >&2
    echo "  Arch: sudo pacman -S bcc python-bcc" >&2
    exit 1
  fi

  local pid=""
  local stamp=""
  local finished=""
  local run_dir=""
  local out_path=""
  local meta_path=""
  local height=""
  local bitcoind_path=""
  local flush_script=""
  local interrupted="false"
  local flush_status=0
  local dur="${DURATION}"

  pid="$(find_bitcoind_pid)"
  stamp="$(utc_stamp)"
  height="$(get_block_height)"
  bitcoind_path="$(resolve_bitcoind_binary "${pid}")"
  if [[ -z "${bitcoind_path}" || ! -f "${bitcoind_path}" ]]; then
    echo "bitcoind binary not found (pid=${pid})" >&2
    exit 1
  fi
  check_usdt_binary "${bitcoind_path}"

  run_dir="$(python3 "${PY}" run-dir --stamp "${stamp}" --profile-logs "${PROFILE_LOGS}")"
  out_path="${run_dir}/utxo-flush.txt"
  meta_path="${run_dir}/meta.txt"
  flush_script="${ROOT}/contrib/tracing/log_utxocache_flush.py"

  mkdir -p "${run_dir}"

  trap 'interrupted=true; echo "Utxo flush tracing interrupted" >&2' INT TERM

  echo "Utxo flush USDT trace pid=${pid} duration=${dur}s"
  echo "Output: ${out_path}"

  set +e
  timeout --preserve-status "${dur}" python3 "${flush_script}" "${pid}" \
    | tee "${out_path}"
  flush_status=${PIPESTATUS[0]}
  set -e
  finished="$(utc_stamp)"

  if [[ "${flush_status}" -eq 124 ]]; then
    echo "Utxo flush duration elapsed (${dur}s)."
    flush_status=0
  elif [[ "${interrupted}" == "true" || "${flush_status}" -ne 0 ]]; then
    interrupted="true"
  fi

  python3 "${PY}" write-flush-summary \
    --input "${out_path}" --output "${run_dir}/utxo-flush-summary.json"

  python3 - "${ROOT}" "${stamp}" "${finished}" "${height:-unknown}" "${DATADIR}" "${pid}" \
    "${dur}" "${PROFILE_LOGS}" "${out_path}" "${bitcoind_path}" \
    "${flush_script}" "${interrupted}" <<'PY' \
    | python3 "${PY}" write-meta --meta-path "${meta_path}"
import json, sys
from pathlib import Path

ROOT = Path(sys.argv[1])
sys.path.insert(0, str(ROOT / "contrib" / "swords"))
from phase5_usdt import detect_usdt

(
    captured,
    finished,
    height,
    datadir,
    pid,
    dur,
    profile_logs,
    out_path,
    bitcoind_path,
    flush_script,
    interrupted,
) = sys.argv[2:13]

result = detect_usdt(Path(bitcoind_path))
print(json.dumps({
    "captured_at_utc": captured,
    "run_finished_at_utc": finished,
    "subcommand": "utxo-flush",
    "block_height": height,
    "datadir": datadir,
    "pid": pid,
    "duration_seconds": dur,
    "profile_logs": profile_logs,
    "utxo_flush_output": out_path,
    "bitcoind_path": bitcoind_path,
    "utxo_flush_script": flush_script,
    "usdt_tracepoints": ",".join(result.tracepoints),
    "usdt_detection_method": result.method,
    "bcc_available": "true",
    "interrupted": interrupted,
}))
PY

  echo "Wrote ${out_path}"
  echo "Wrote ${meta_path}"
  if [[ "${interrupted}" == "true" ]]; then
    exit "${flush_status:-1}"
  fi
}

SUBCMD="${1:-help}"
shift || true

case "${SUBCMD}" in
  check-usdt) cmd_check_usdt "$@" ;;
  connectblock) cmd_connectblock "$@" ;;
  utxo-flush) cmd_utxo_flush "$@" ;;
  phase5) usage ;;
  help|-h|--help) usage ;;
  *)
    echo "Unknown subcommand: ${SUBCMD}" >&2
    usage >&2
    exit 1
    ;;
esac