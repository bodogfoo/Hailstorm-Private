#!/usr/bin/env bash
# Hailstone 24 Ubuntu 24-Box Benchmark & Switch-Latency Verification Harness
# Non-destructive: operates purely on an isolated --data-dir and runtime socket.
set -euo pipefail

BIN_DIR="${1:-./build/hailstone}"
DATA_DIR="${HAILSTONE_DATA_DIR:-${HOME}/.local/share/hailstone24}"
ROSTER="${2:-${DATA_DIR}/hailstone24_roster.ini}"
SOCKET="${XDG_RUNTIME_DIR:-/tmp/hailstone24-$(id -u)}/hailstone24/bus.sock"
OUT_LOG="${3:-/tmp/hailstone24_bench_$(date +%Y%m%d_%H%M%S).log}"

BROKER="${BIN_DIR}/hailstone24_broker"
MANAGER="${BIN_DIR}/hailstone24_manager"

if [[ ! -x "${BROKER}" || ! -x "${MANAGER}" ]]; then
  echo "Error: build binaries not found in ${BIN_DIR} (expected hailstone24_broker & hailstone24_manager)" >&2
  exit 1
fi
if [[ ! -f "${ROSTER}" ]]; then
  echo "Error: roster file not found: ${ROSTER}" >&2
  echo "Generate one with: ${BIN_DIR}/hailstone --data-dir ${DATA_DIR} --import-mq-logins /path/to/login.db --roster-out ${ROSTER}" >&2
  exit 1
fi

echo "=== Hailstone 24 Benchmark ===" | tee "${OUT_LOG}"
echo "Data Dir : ${DATA_DIR}" | tee -a "${OUT_LOG}"
echo "Roster   : ${ROSTER}" | tee -a "${OUT_LOG}"
echo "Socket   : ${SOCKET}" | tee -a "${OUT_LOG}"
echo "Hardware : $(lscpu | awk -F: '/Model name/{gsub(/^ +/,"",$2); print $2; exit}')" | tee -a "${OUT_LOG}"
echo "Memory   : $(free -h | awk '/Mem:/{print $2 " total, " $7 " available"}')" | tee -a "${OUT_LOG}"
if command -v lspci >/dev/null 2>&1; then
  echo "GPU      : $(lspci | grep -iE 'vga|display|3d' | head -n 1)" | tee -a "${OUT_LOG}"
fi

cleanup() {
  if [[ -n "${BROKER_PID:-}" ]]; then
    kill -TERM "${BROKER_PID}" 2>/dev/null || true
    wait "${BROKER_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

"${BROKER}" \
  --data-dir "${DATA_DIR}" \
  --roster "${ROSTER}" \
  --socket "${SOCKET}" \
  --launch-all \
  --stagger-ms 400 \
  --status-interval-s 10 \
  >> "${OUT_LOG}" 2>&1 &
BROKER_PID=$!

echo "Waiting for sessions to register on IPC bus..." | tee -a "${OUT_LOG}"
sleep 12

echo "--- Initial Roster Telemetry ---" | tee -a "${OUT_LOG}"
"${MANAGER}" --socket "${SOCKET}" --status | tee -a "${OUT_LOG}"

echo "--- Switch Latency Probe (slot02 -> slot05 -> slot01) ---" | tee -a "${OUT_LOG}"
for target in slot02 slot05 slot01; do
  t0=$(date +%s%3N)
  "${MANAGER}" --socket "${SOCKET}" --switch "${target}" >> "${OUT_LOG}"
  sleep 0.35
  t1=$(date +%s%3N)
  echo "Switch -> ${target}: wall=$((t1 - t0))ms" | tee -a "${OUT_LOG}"
done

echo "--- Broadcast IPC Latency Probe (/bca //echo hailstone24_ok) ---" | tee -a "${OUT_LOG}"
"${MANAGER}" --socket "${SOCKET}" --cmd "/bcaa /echo hailstone24_bench_ping" | tee -a "${OUT_LOG}"

echo "--- Process Memory Footprint ---" | tee -a "${OUT_LOG}"
ps -eo pid,rss,vsz,comm | grep -E 'hailstone|PID' | tee -a "${OUT_LOG}"
echo "--- Final Roster Status ---" | tee -a "${OUT_LOG}"
"${MANAGER}" --socket "${SOCKET}" --status | tee -a "${OUT_LOG}"

echo "Benchmark log saved to: ${OUT_LOG}"
