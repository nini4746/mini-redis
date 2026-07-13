#!/usr/bin/env bash
#
# Honest side-by-side benchmark: mini-redis vs real Redis, same machine,
# same options, real Redis with persistence OFF (appendonly no, save '').
#
# Requires: redis-benchmark and redis-server on PATH. If they are missing the
# script prints instructions and exits; run bench/loadgen.py for a
# mini-redis-only indicative load instead.
#
# Usage: bash bench/run_bench.sh
set -u

MINI_PORT=6380
REAL_PORT=6379
N=100000
C=50
TESTS=ping,set,get,incr,lpush,lpop,sadd,hset
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MINI_BIN="$HERE/mini-redis"

command -v redis-benchmark >/dev/null 2>&1 || { echo "redis-benchmark not found on PATH."; exit 2; }
command -v redis-server   >/dev/null 2>&1 || { echo "redis-server not found on PATH."; exit 2; }
[ -x "$MINI_BIN" ] || { echo "build mini-redis first (make)"; exit 2; }

echo "== starting servers =="
"$MINI_BIN" >/tmp/mini-redis-bench.log 2>&1 &
MINI_PID=$!
redis-server --port "$REAL_PORT" --appendonly no --save '' --daemonize no \
    >/tmp/real-redis-bench.log 2>&1 &
REAL_PID=$!
sleep 1

cleanup() { kill "$MINI_PID" "$REAL_PID" 2>/dev/null; }
trap cleanup EXIT

# flush both before measuring
redis-cli -p "$MINI_PORT" FLUSHALL >/dev/null 2>&1 || true
redis-cli -p "$REAL_PORT" FLUSHALL >/dev/null 2>&1 || true

run() { # port label pipeline
    local port=$1 label=$2 P=$3
    echo
    echo "== $label  (-n $N -c $C -P $P) =="
    redis-benchmark -p "$port" -t "$TESTS" -n "$N" -c "$C" -P "$P" --csv
}

for P in 1 16; do
    run "$MINI_PORT" "mini-redis" "$P"
    run "$REAL_PORT" "real-redis" "$P"
done

echo
echo "Repeat 2-3x and take the median. Paste results into BENCH.md."
