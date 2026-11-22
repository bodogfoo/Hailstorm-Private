#!/usr/bin/env bash
# Hailstone Cortex miniature CPU inference server.
#
# Dedicated llama-server on 127.0.0.1:8100, CPU-only, bounded thread budget so
# it never starves Hailstone's own boxes or the Hindsight endpoint.
# Port 8096 belongs to Hindsight - this script refuses to touch anything that
# is not a llama-server explicitly bound to port 8100.
#
# Usage:
#   cortex-inference-server.sh                 # start (or restart) with default model
#   cortex-inference-server.sh <model.gguf>    # start with a specific GGUF
#   cortex-inference-server.sh --stop          # stop the server
#   cortex-inference-server.sh --status        # health + slot summary
#
# Tunables (env): CORTEX_THREADS, CORTEX_CONTEXT, CORTEX_SLOTS, CORTEX_PORT,
#                 CORTEX_INFERENCE_LOG, CORTEX_INFERENCE_PID
set -euo pipefail

PORT="${CORTEX_PORT:-8100}"
HOST=127.0.0.1
MODELS_DIR=/home/john/models/cortex
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLAMA_SERVER=/home/john/llama.cpp/build/bin/llama-server
LOG_FILE="${CORTEX_INFERENCE_LOG:-$HOME/.hermes/cache/scratch/cortex-inference-server.log}"
PID_FILE="${CORTEX_INFERENCE_PID:-$HOME/.hermes/cache/scratch/cortex-inference-server.pid}"

# Bounded GPU budget: the Intel BMG Vulkan device is available for llama.cpp.
# Keep one slot while validating latency and avoid queueing six simultaneous requests.
THREADS="${CORTEX_THREADS:-6}"
# 6 slots x ~1365 tokens = 8192 total. Dedicated context per the spec:
# enough for a ~400-token observation prompt plus the 64-token JSON reply
# in every slot, with room to spare; KV cache stays well under 1 GiB.
CONTEXT="${CORTEX_CONTEXT:-4096}"
SLOTS="${CORTEX_SLOTS:-1}"

DEFAULT_MODEL="$MODELS_DIR/Qwen3-1.7B-Q4_K_M.gguf"

# Only ever kill a llama-server that is itself bound to $PORT (never 8096).
pid_on_port() {
  ss -ltnpH "sport = :$PORT" 2>/dev/null \
    | grep -oE 'pid=[0-9]+' | head -n1 | cut -d= -f2
}

is_ours() {
  local pid="$1"
  [[ -n "$pid" ]] || return 1
  tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null | grep -q -- "--port $PORT "
}

stop_server() {
  local pid=""
  if [[ -f "$PID_FILE" ]]; then
    local recorded
    recorded="$(cat "$PID_FILE" 2>/dev/null || true)"
    if [[ -n "$recorded" ]] && is_ours "$recorded"; then
      pid="$recorded"
    fi
  fi
  [[ -n "$pid" ]] || pid="$(pid_on_port)"
  if [[ -n "$pid" ]] && is_ours "$pid"; then
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 20); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.5
    done
    kill -9 "$pid" 2>/dev/null || true
    echo "stopped pid $pid"
  else
    echo "no llama-server on port $PORT to stop"
  fi
  rm -f "$PID_FILE"
}

case "${1:-}" in
  --stop) stop_server; exit 0 ;;
  --status)
    if curl -sf "http://$HOST:$PORT/health" >/dev/null 2>&1; then
      echo "healthy: http://$HOST:$PORT/health"
      curl -s "http://$HOST:$PORT/slots" | python3 -c '
import json, sys
s = json.load(sys.stdin)
busy = sum(1 for x in s if x.get("is_processing"))
print("slots: %d total, %d busy, %d tokens each" % (len(s), busy, s[0]["n_ctx"]))'
      ps -o pid,pcpu,pmem,rss,args -p "$(pid_on_port)" | tail -n +2
    else
      echo "not healthy on $PORT"
      exit 1
    fi
    exit 0 ;;
esac

MODEL="${1:-$DEFAULT_MODEL}"
if [[ ! -f "$MODEL" ]]; then
  echo "model not found: $MODEL" >&2
  exit 1
fi
if [[ ! -x "$LLAMA_SERVER" ]]; then
  echo "llama-server not found: $LLAMA_SERVER" >&2
  exit 1
fi

# Restart cleanly: never a second listener on $PORT.
stop_server >/dev/null 2>&1 || true

mkdir -p "$(dirname "$LOG_FILE")"
echo "starting llama-server on $HOST:$PORT model=$MODEL threads=$THREADS ctx=$CONTEXT slots=$SLOTS"
echo "log: $LOG_FILE"

"$LLAMA_SERVER" \
  --host "$HOST" \
  --port "$PORT" \
  --model "$MODEL" \
  --alias cortex-mini \
  --device Vulkan0 \
  --n-gpu-layers 99 \
  --threads "$THREADS" \
  --threads-batch "$THREADS" \
  --ctx-size "$CONTEXT" \
  --parallel "$SLOTS" \
  --jinja \
  >>"$LOG_FILE" 2>&1 &

SERVER_PID=$!
echo "$SERVER_PID" > "$PID_FILE"
echo "pid $SERVER_PID"

# Wait for /health before returning.
for _ in $(seq 1 60); do
  if curl -sf "http://$HOST:$PORT/health" >/dev/null 2>&1; then
    echo "healthy: http://$HOST:$PORT/health"
    exit 0
  fi
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "server died during startup; last log lines:" >&2
    tail -n 40 "$LOG_FILE" >&2
    rm -f "$PID_FILE"
    exit 1
  fi
  sleep 1
done
echo "server did not become healthy within 60s; last log lines:" >&2
tail -n 40 "$LOG_FILE" >&2
exit 1
