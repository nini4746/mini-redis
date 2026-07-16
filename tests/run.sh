#!/usr/bin/env bash
# Direct-execution test suite: build mini-redis, spawn the REAL server on a
# test port in an isolated dir, drive it with the official redis-cli, assert
# outputs, then shut it down. Covers SPEC R1-R4 acceptance criteria.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/mini-redis"
PORT="${MINIREDIS_TEST_PORT:-6399}"
CLI=(redis-cli -p "$PORT")
WORK="$(mktemp -d)"
PASS=0
FAIL=0

cleanup() {
  [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
  [ -n "${SRV_PID:-}" ] && wait "$SRV_PID" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

command -v redis-cli >/dev/null || { echo "FATAL: redis-cli not found"; exit 2; }

echo "== build =="
make -C "$ROOT" >/dev/null || { echo "FATAL: build failed"; exit 2; }

echo "== start server on :$PORT (cwd=$WORK) =="
( cd "$WORK" && exec "$BIN" --port "$PORT" ) >"$WORK/server.log" 2>"$WORK/server.err" &
SRV_PID=$!

# Wait for bind (startup race): retry PING until PONG or timeout.
ready=0
for _ in $(seq 1 50); do
  if [ "$(redis-cli -p "$PORT" PING 2>/dev/null)" = "PONG" ]; then ready=1; break; fi
  sleep 0.1
done
[ "$ready" = 1 ] || { echo "FATAL: server never became ready"; cat "$WORK/server.err"; exit 2; }

# check <desc> <expected> <actual>
check() {
  local desc="$1" exp="$2" act="$3"
  if [ "$act" = "$exp" ]; then
    PASS=$((PASS+1)); printf '  ok   - %s\n' "$desc"
  else
    FAIL=$((FAIL+1)); printf '  FAIL - %s\n         expected [%s] got [%s]\n' "$desc" "$exp" "$act"
  fi
}

echo "== R1 protocol =="
check "PING -> PONG"              "PONG"  "$("${CLI[@]}" PING)"
check "PING msg echoes"           "hi"    "$("${CLI[@]}" PING hi)"
check "ECHO hello"                "hello" "$("${CLI[@]}" ECHO hello)"
check "unknown cmd -> error, conn stays open" "PONG" \
      "$("${CLI[@]}" NOSUCHCMD x >/dev/null 2>&1; "${CLI[@]}" PING)"
# inline (telnet-style) command over a raw socket
inline=$(printf 'PING\r\n' | nc -w1 127.0.0.1 "$PORT" | tr -d '\r')
check "inline PING -> +PONG"      "+PONG" "$inline"

echo "== R2 strings =="
check "SET k v -> OK"             "OK"    "$("${CLI[@]}" SET k v)"
check "GET k -> v"                "v"     "$("${CLI[@]}" GET k)"
check "EXISTS k -> 1"             "1"     "$("${CLI[@]}" EXISTS k)"
check "DEL k -> 1"                "1"     "$("${CLI[@]}" DEL k)"
check "GET missing -> (nil/empty)" ""     "$("${CLI[@]}" GET k)"
"${CLI[@]}" SET n 10 >/dev/null
check "INCR n -> 11"              "11"    "$("${CLI[@]}" INCR n)"
"${CLI[@]}" SET bad notanint >/dev/null
check "INCR non-int -> error"     "1" \
      "$("${CLI[@]}" INCR bad 2>&1 | grep -c -i 'not an integer')"
# 1 MB bulk round-trip (STRLEN not implemented -> measure the returned value).
# Feed the value via -x/stdin: a 1 MB argv would blow past ARG_MAX.
head -c 1048576 /dev/zero | tr '\0' 'x' > "$WORK/big.txt"
"${CLI[@]}" -x SET big < "$WORK/big.txt" >/dev/null
got_len=$(printf %s "$("${CLI[@]}" GET big)" | wc -c | tr -d ' ')
check "1MB bulk round-trip len"   "1048576" "$got_len"

echo "== R3 collections =="
check "RPUSH l a b c -> 3"        "3"     "$("${CLI[@]}" RPUSH l a b c)"
check "LLEN l -> 3"               "3"     "$("${CLI[@]}" LLEN l)"
check "LRANGE l 0 -1"             $'a\nb\nc' "$("${CLI[@]}" LRANGE l 0 -1)"
check "LPOP l -> a"               "a"     "$("${CLI[@]}" LPOP l)"
check "WRONGTYPE: GET on list"    "1" \
      "$("${CLI[@]}" GET l 2>&1 | grep -c -i 'wrongtype')"
check "HSET h f1 v1 -> 1"         "1"     "$("${CLI[@]}" HSET h f1 v1)"
check "HGET h f1 -> v1"           "v1"    "$("${CLI[@]}" HGET h f1)"
"${CLI[@]}" HSET h f2 v2 >/dev/null
# HGETALL field order is unspecified (as in real Redis) -> compare sorted.
check "HGETALL h fields (sorted)" $'f1\nf2\nv1\nv2' "$("${CLI[@]}" HGETALL h | sort)"
check "SADD s x y y -> 2 (dedup)" "2"     "$("${CLI[@]}" SADD s x y y)"
check "SISMEMBER s x -> 1"        "1"     "$("${CLI[@]}" SISMEMBER s x)"
check "SCARD s -> 2"              "2"     "$("${CLI[@]}" SCARD s)"

echo "== R4 ttl =="
"${CLI[@]}" SET t1 v >/dev/null
check "TTL no-expiry -> -1"       "-1"    "$("${CLI[@]}" TTL t1)"
check "TTL missing -> -2"         "-2"    "$("${CLI[@]}" TTL nope)"
"${CLI[@]}" EXPIRE t1 100 >/dev/null
persisted=$("${CLI[@]}" PERSIST t1)
check "PERSIST -> 1"              "1"     "$persisted"
check "TTL after PERSIST -> -1"   "-1"    "$("${CLI[@]}" TTL t1)"
"${CLI[@]}" SET t2 v >/dev/null
"${CLI[@]}" EXPIRE t2 1 >/dev/null
sleep 1.2
check "lazy expire: GET after TTL -> nil" "" "$("${CLI[@]}" GET t2)"
check "EXPIRE <=0 deletes now -> 1" "1"   "$("${CLI[@]}" SET t3 v >/dev/null; "${CLI[@]}" EXPIRE t3 0)"
check "t3 gone after EXPIRE 0"     "0"    "$("${CLI[@]}" EXISTS t3)"

echo "== concurrency + pipelining =="
# 20 concurrent clients each INCR the same counter -> final 20.
# wait only on the INCR jobs, NOT the still-running server ($SRV_PID).
"${CLI[@]}" SET ctr 0 >/dev/null
pids=()
for _ in $(seq 1 20); do "${CLI[@]}" INCR ctr >/dev/null & pids+=($!); done
wait "${pids[@]}"
check "20 concurrent INCR -> 20"  "20"    "$("${CLI[@]}" GET ctr)"
# pipeline: 3 commands in one connection, order preserved
pipe=$(printf 'SET p 1\r\nINCR p\r\nGET p\r\n' | nc -w1 127.0.0.1 "$PORT" | tr -d '\r' | grep -c -E '^(OK|:2|\$1|2)$')
check "pipelined SET/INCR/GET replies present" "3" "$pipe"

echo
echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
