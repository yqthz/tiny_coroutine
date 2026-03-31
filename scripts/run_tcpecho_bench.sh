#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
SERVER_BIN="${SERVER_BIN:-$BUILD_DIR/tcpechoserver}"
STRESS_BIN="${STRESS_BIN:-$BUILD_DIR/tcpecho_stress}"

HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-12345}"
SERVER_WORKERS="${SERVER_WORKERS:-8}"
SERVER_BUFFER="${SERVER_BUFFER:-4096}"
SERVER_REPORT="${SERVER_REPORT:-1}"

CONNECTIONS="${CONNECTIONS:-512}"
THREADS="${THREADS:-8}"
PAYLOAD="${PAYLOAD:-64}"
WARMUP="${WARMUP:-3}"
DURATION="${DURATION:-20}"
STRESS_REPORT="${STRESS_REPORT:-1}"
SAMPLE_RATE="${SAMPLE_RATE:-100}"

CONFIGURE="${CONFIGURE:-1}"
BUILD="${BUILD:-1}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

OUT_DIR="${OUT_DIR:-$ROOT_DIR/build/tcpecho_bench_logs}"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
SERVER_LOG="$OUT_DIR/server_${TIMESTAMP}.log"
STRESS_LOG="$OUT_DIR/stress_${TIMESTAMP}.log"

usage() {
  cat <<USAGE
Usage: $(basename "$0") [--quick] [--no-build] [--help]

Env overrides:
  HOST PORT SERVER_WORKERS SERVER_BUFFER SERVER_REPORT
  CONNECTIONS THREADS PAYLOAD WARMUP DURATION STRESS_REPORT SAMPLE_RATE
  BUILD_DIR SERVER_BIN STRESS_BIN CONFIGURE BUILD BUILD_TYPE OUT_DIR

Example:
  CONNECTIONS=1024 THREADS=16 DURATION=30 $0
USAGE
}

while (($# > 0)); do
  case "$1" in
    --quick)
      CONNECTIONS=128
      THREADS=4
      PAYLOAD=64
      WARMUP=1
      DURATION=3
      ;;
    --no-build)
      CONFIGURE=0
      BUILD=0
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage
      exit 1
      ;;
  esac
  shift
done

mkdir -p "$OUT_DIR"

if [[ "$CONFIGURE" == "1" ]]; then
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

if [[ "$BUILD" == "1" ]]; then
  cmake --build "$BUILD_DIR" -j --target tcpechoserver tcpecho_stress
fi

if [[ ! -x "$SERVER_BIN" ]]; then
  echo "server binary not found: $SERVER_BIN" >&2
  exit 1
fi
if [[ ! -x "$STRESS_BIN" ]]; then
  echo "stress binary not found: $STRESS_BIN" >&2
  exit 1
fi

SERVER_PID=""
cleanup() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -INT "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

if command -v stdbuf >/dev/null 2>&1; then
  SERVER_LAUNCH=(stdbuf -oL -eL "$SERVER_BIN")
  STRESS_LAUNCH=(stdbuf -oL -eL "$STRESS_BIN")
else
  SERVER_LAUNCH=("$SERVER_BIN")
  STRESS_LAUNCH=("$STRESS_BIN")
fi

echo "[bench] starting server: $SERVER_BIN"
"${SERVER_LAUNCH[@]}" \
  --host "$HOST" \
  --port "$PORT" \
  --workers "$SERVER_WORKERS" \
  --buffer "$SERVER_BUFFER" \
  --report "$SERVER_REPORT" \
  >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

sleep 0.5

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "server exited unexpectedly, see $SERVER_LOG" >&2
  exit 1
fi

echo "[bench] running stress: $STRESS_BIN"
"${STRESS_LAUNCH[@]}" \
  --host "$HOST" \
  --port "$PORT" \
  --connections "$CONNECTIONS" \
  --threads "$THREADS" \
  --payload "$PAYLOAD" \
  --warmup "$WARMUP" \
  --duration "$DURATION" \
  --report "$STRESS_REPORT" \
  --sample-rate "$SAMPLE_RATE" \
  >"$STRESS_LOG" 2>&1

echo "[bench] completed"
echo "[bench] stress log: $STRESS_LOG"
echo "[bench] server log: $SERVER_LOG"
echo

echo "----- stress summary (tail) -----"
tail -n 20 "$STRESS_LOG" || true
echo "----- server summary (tail) -----"
tail -n 20 "$SERVER_LOG" || true
