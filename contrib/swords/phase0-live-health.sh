#!/usr/bin/env bash
# Phase 0 live health profiling: pidstat watch, perf profile, one-shot snapshot.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PY="${ROOT}/contrib/swords/phase0_live_health.py"

# Resolve defaults from env via Python (single source of truth).
eval "$(python3 "${PY}" shell-export)"
BITCOIN_CLI="${BITCOIN_CLI:-${ROOT}/build/bin/bitcoin-cli}"
DURATION="${DURATION:-}"
OUT="${OUT:-}"

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

pgrep_datadir_pattern() {
  python3 -c "
import sys
sys.path.insert(0, '${ROOT}/contrib/swords')
from phase0_live_health import pgrep_datadir_pattern
print(pgrep_datadir_pattern(sys.argv[1]))
" "${DATADIR}"
}

find_bitcoind_pid() {
  local pid=""
  local pattern=""
  pattern="$(pgrep_datadir_pattern)"
  pid="$(pgrep -f "${pattern}" 2>/dev/null | head -1 || true)"
  if [[ -z "${pid}" ]]; then
    echo "bitcoind not running for datadir=${DATADIR}" >&2
    echo "Hint: start with: just start" >&2
    exit 1
  fi
  echo "${pid}"
}

try_bitcoind_pid() {
  local pattern=""
  pattern="$(pgrep_datadir_pattern)"
  pgrep -f "${pattern}" 2>/dev/null | head -1 || true
}

resolve_bitcoin_cli() {
  if [[ -n "${BITCOIN_CLI}" && -x "${BITCOIN_CLI}" ]]; then
    echo "${BITCOIN_CLI}"
    return
  fi
  if command -v bitcoin-cli >/dev/null 2>&1; then
    command -v bitcoin-cli
    return
  fi
  echo ""
}

get_block_height() {
  local cli=""
  cli="$(resolve_bitcoin_cli)"
  if [[ -z "${cli}" ]]; then
    echo ""
    return
  fi
  "${cli}" -datadir="${DATADIR}" getblockcount 2>/dev/null || echo ""
}

utc_now() {
  python3 -c "from phase0_live_health import utc_now_iso; print(utc_now_iso())" 2>/dev/null \
    || date -u +%Y-%m-%dT%H:%M:%SZ
}

utc_stamp() {
  python3 -c "from phase0_live_health import utc_stamp; print(utc_stamp())" 2>/dev/null \
    || date -u +%Y%m%dT%H%M%SZ
}

py_path() {
  python3 - "${ROOT}" "$@" <<'PY'
import sys
from pathlib import Path

ROOT = Path(sys.argv[1])
sys.path.insert(0, str(ROOT / "contrib" / "swords"))
from phase0_live_health import (
    profile_perf_data,
    profile_run_dir,
    snapshot_path,
    watch_csv_path,
)

argv = sys.argv[2:]
kind = argv[0]
profile_logs = Path(argv[1])
stamp = argv[2]
out = argv[3] if len(argv) > 3 and argv[3] else None
out_path = Path(out) if out else None

if kind == "watch":
    print(watch_csv_path(profile_logs, stamp, out_path))
elif kind == "profile_dir":
    print(profile_run_dir(profile_logs, stamp))
elif kind == "perf_data":
    print(profile_perf_data(profile_logs, stamp))
elif kind == "snapshot":
    print(snapshot_path(profile_logs, stamp, out_path))
PY
}

write_profile_meta() {
  local meta_path="$1"
  python3 - "${ROOT}" "${meta_path}" <<'PY'
import json, sys
from pathlib import Path

ROOT = Path(sys.argv[1])
meta_path = Path(sys.argv[2])
sys.path.insert(0, str(ROOT / "contrib" / "swords"))
from phase0_live_health import write_profile_meta

fields = json.load(sys.stdin)
write_profile_meta(meta_path, {k: str(v) for k, v in fields.items()})
print(meta_path)
PY
}

perf_record_hint() {
  echo "perf record failed. Common fixes:" >&2
  echo "  echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid" >&2
  echo "  sudo sysctl -w kernel.perf_event_paranoid=-1" >&2
  echo "  retry with sudo, or run: perf record --user-regs=lr" >&2
}

watch_row() {
  local pid="$1"
  local height=""
  local ts=""

  ts="$(utc_now)"
  height="$(get_block_height)"

  python3 - "${ROOT}" "${pid}" "${ts}" "${height}" <<'PY'
import subprocess, sys
from pathlib import Path

ROOT = Path(sys.argv[1])
pid = sys.argv[2]
ts = sys.argv[3]
height = sys.argv[4] or "unknown"

sys.path.insert(0, str(ROOT / "contrib" / "swords"))
from phase0_live_health import (
    format_watch_row,
    parse_pidstat_cpu_output,
    parse_pidstat_mem_output,
)

def run_pidstat(args):
    try:
        proc = subprocess.run(
            args,
            check=False,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError:
        return ""
    return proc.stdout if proc.returncode == 0 else ""

cpu_out = run_pidstat(["pidstat", "-u", "-p", pid, "1", "1"])
mem_out = run_pidstat(["pidstat", "-r", "-p", pid, "1", "1"])
usr, sys_pct, cpu_pct = parse_pidstat_cpu_output(cpu_out, pid)
vsz, rss = parse_pidstat_mem_output(mem_out, pid)

print(
    format_watch_row(
        ts,
        height,
        cpu_pct or "0",
        usr or "0",
        sys_pct or "0",
        vsz or "0",
        rss or "0",
    )
)
PY
}

cmd_watch() {
  require_cmd pidstat "install: sudo apt install sysstat  # or pacman -S sysstat"
  local pid=""
  local stamp=""
  local out_path=""
  local end_at=""
  local watch_stop_reason=""

  pid="$(find_bitcoind_pid)"
  stamp="$(utc_stamp)"
  out_path="$(OUT="${OUT}" py_path watch "${PROFILE_LOGS}" "${stamp}" "${OUT}")"

  mkdir -p "$(dirname "${out_path}")"
  python3 - "${ROOT}" "${out_path}" <<'PY'
import sys
from pathlib import Path

ROOT = Path(sys.argv[1])
sys.path.insert(0, str(ROOT / "contrib" / "swords"))
from phase0_live_health import WATCH_CSV_HEADER

path = Path(sys.argv[2])
path.write_text(WATCH_CSV_HEADER + "\n", encoding="utf-8")
print(path)
PY

  trap 'watch_stop_reason="signal"; echo "Watch stopped (signal)" >&2' INT TERM

  echo "Watching bitcoind pid=${pid} datadir=${DATADIR}"
  echo "CSV: ${out_path}"
  echo "Interval: ${INTERVAL}s (Ctrl+C to stop)"
  echo "Per-thread CPU hint: top -H -p ${pid}"

  if [[ -n "${DURATION}" ]]; then
    end_at=$((SECONDS + DURATION))
    echo "Duration: ${DURATION}s"
  fi

  while true; do
    if [[ -n "${watch_stop_reason}" ]]; then
      break
    fi

    local new_pid=""
    new_pid="$(try_bitcoind_pid)"
    if [[ -z "${new_pid}" ]]; then
      watch_stop_reason="node_stopped"
      echo "bitcoind no longer running for datadir=${DATADIR}" >&2
      break
    fi
    if [[ "${new_pid}" != "${pid}" ]]; then
      echo "PID changed: ${pid} -> ${new_pid}" >&2
      pid="${new_pid}"
    fi

    watch_row "${pid}" >> "${out_path}"
    if [[ -n "${watch_stop_reason}" ]]; then
      break
    fi
    if [[ -n "${DURATION}" && "${SECONDS}" -ge "${end_at}" ]]; then
      watch_stop_reason="duration"
      echo "Done (${DURATION}s elapsed)."
      break
    fi
    sleep "${INTERVAL}" || true
    if [[ -n "${watch_stop_reason}" ]]; then
      break
    fi
  done

  if [[ -n "${watch_stop_reason}" ]]; then
    echo "# watch_stopped=${watch_stop_reason} at $(utc_now)" >> "${out_path}"
  fi
}

cmd_profile() {
  require_cmd perf "install: sudo apt install linux-perf  # or pacman -S perf"
  local pid=""
  local stamp=""
  local run_dir=""
  local perf_data=""
  local meta_path=""
  local height=""
  local perf_ver=""
  local dur="${DURATION:-60}"
  local profile_interrupted="false"
  local perf_status=0

  pid="$(find_bitcoind_pid)"
  stamp="$(utc_stamp)"
  run_dir="$(py_path profile_dir "${PROFILE_LOGS}" "${stamp}")"
  perf_data="$(py_path perf_data "${PROFILE_LOGS}" "${stamp}")"
  meta_path="${run_dir}/meta.txt"
  height="$(get_block_height)"
  perf_ver="$(perf --version 2>/dev/null | head -1 || echo unknown)"

  mkdir -p "${run_dir}"

  write_minimal_meta() {
    local interrupted="$1"
    local finished="$2"
    python3 - "${stamp}" "${finished}" "${height:-unknown}" "${DATADIR}" \
      "${pid}" "${dur}" "${perf_ver}" "${PROFILE_LOGS}" "${perf_data}" \
      "${interrupted}" <<'PY' | write_profile_meta "${meta_path}"
import json, sys

(
    _stamp,
    finished,
    height,
    datadir,
    pid,
    dur,
    perf_ver,
    profile_logs,
    perf_data,
    interrupted,
) = sys.argv[1:11]

print(json.dumps({
    "captured_at_utc": _stamp,
    "run_finished_at_utc": finished,
    "subcommand": "profile",
    "block_height": height,
    "datadir": datadir,
    "pid": pid,
    "duration_seconds": dur,
    "perf_version": perf_ver,
    "profile_logs": profile_logs,
    "perf_data": perf_data,
    "interrupted": interrupted,
}))
PY
  }

  profile_cleanup() {
    if [[ "${profile_interrupted}" == "true" ]]; then
      write_minimal_meta "true" "$(utc_stamp)"
      echo "Wrote interrupted meta: ${meta_path}" >&2
    fi
  }
  trap 'profile_interrupted=true; echo "Profile interrupted" >&2' INT TERM
  trap profile_cleanup EXIT

  echo "Profiling bitcoind pid=${pid} for ${dur}s -> ${perf_data}"
  echo "Per-thread CPU hint: top -H -p ${pid}"

  set +e
  perf record -g -p "${pid}" -o "${perf_data}" -- sleep "${dur}"
  perf_status=$?
  set -e

  local interrupted="false"
  if [[ "${profile_interrupted}" == "true" || "${perf_status}" -ne 0 ]]; then
    interrupted="true"
  fi

  trap - INT TERM
  trap - EXIT

  if [[ "${interrupted}" == "true" ]]; then
    if [[ "${perf_status}" -ne 0 ]]; then
      perf_record_hint
    fi
    write_minimal_meta "true" "$(utc_stamp)"
    echo "Wrote meta (interrupted=${interrupted}): ${meta_path}" >&2
    exit "${perf_status:-1}"
  fi

  write_minimal_meta "false" "$(utc_stamp)"

  echo "Wrote ${perf_data}"
  echo "Wrote ${meta_path}"
  echo "Inspect: perf report -i ${perf_data}"
}

cmd_check() {
  local log="${DATADIR}/debug.log"
  local txindex_warn_ms="${TXINDEX_WARN_MS:-500}"
  local prefetch_rollups="${PREFETCH_ROLLUPS:-3}"
  local prefetch_min_prevouts="${PREFETCH_MIN_PREVOUTS:-64}"
  echo "Checking benchstats health: ${log}"
  python3 "${PY}" check \
    --datadir "${DATADIR}" \
    --txindex-warn-ms "${txindex_warn_ms}" \
    --prefetch-rollups "${prefetch_rollups}" \
    --prefetch-min-prevouts "${prefetch_min_prevouts}"
}

cmd_snapshot() {
  require_cmd pidstat "install: sudo apt install sysstat  # or pacman -S sysstat"
  local pid=""
  local stamp=""
  local out_path=""
  local height=""

  pid="$(find_bitcoind_pid)"
  stamp="$(utc_stamp)"
  height="$(get_block_height)"
  out_path="$(OUT="${OUT}" py_path snapshot "${PROFILE_LOGS}" "${stamp}" "${OUT}")"

  mkdir -p "$(dirname "${out_path}")"
  {
    echo "=== Phase 0 snapshot ${stamp} ==="
    echo "captured_at_utc=$(utc_now)"
    echo "datadir=${DATADIR}"
    echo "pid=${pid}"
    echo "block_height=${height:-unknown}"
    echo ""
    echo "=== Per-thread CPU (optional) ==="
    echo "top -H -p ${pid}"
    echo ""
    echo "=== pidstat -u -r -p ${pid} 1 1 ==="
    pidstat -u -r -p "${pid}" 1 1 2>/dev/null || echo "(pidstat failed)"
    echo ""
    echo "=== mpstat 1 1 ==="
    if command -v mpstat >/dev/null 2>&1; then
      mpstat 1 1 2>/dev/null || echo "(mpstat failed)"
    else
      echo "mpstat not found; install: sudo apt install sysstat"
    fi
    echo ""
    echo "=== iostat -x 1 1 ==="
    if command -v iostat >/dev/null 2>&1; then
      iostat -x 1 1 2>/dev/null || echo "(iostat failed)"
    else
      echo "iostat not found; install: sudo apt install sysstat"
    fi
  } > "${out_path}"

  echo "${out_path}"
}

SUBCMD="${1:-help}"
shift || true

case "${SUBCMD}" in
  watch) cmd_watch "$@" ;;
  profile) cmd_profile "$@" ;;
  snapshot) cmd_snapshot "$@" ;;
  check) cmd_check "$@" ;;
  help|-h|--help) usage ;;
  *)
    echo "Unknown subcommand: ${SUBCMD}" >&2
    usage >&2
    exit 1
    ;;
esac