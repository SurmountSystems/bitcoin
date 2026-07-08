# Shared shell helpers for Swords profiling scripts.
# Requires ROOT and DATADIR; optional BITCOIN_CLI.

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
  if [[ -n "${BITCOIN_CLI:-}" && -x "${BITCOIN_CLI}" ]]; then
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