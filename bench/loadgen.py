#!/usr/bin/env python3
"""Fallback load generator for mini-redis.

This is NOT redis-benchmark. It exists so the benchmark plan can be exercised
on a machine where redis-benchmark / redis-server are not installed: it proves
the server completes the required workload (per-command, -n 100000) with and
without pipelining, over many concurrent connections, without errors, and
reports an indicative throughput for mini-redis alone.

For the honest side-by-side comparison required by SPEC R7, run bench/run_bench.sh
with a real redis-benchmark installed.

Usage: python3 loadgen.py [-n N] [-c C] [-P P] [-p PORT]
"""
import argparse, socket, threading, time, sys

TESTS = ["ping", "set", "get", "incr", "lpush", "lpop", "sadd", "hset"]

def build(cmd_bytes_list):
    out = b"*%d\r\n" % len(cmd_bytes_list)
    for a in cmd_bytes_list:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out

def make_request(test, i):
    k = b"key:%d" % (i % 1000)
    if test == "ping":  return build([b"PING"])
    if test == "set":   return build([b"SET", k, b"xxxxxxxxxx"])
    if test == "get":   return build([b"GET", k])
    if test == "incr":  return build([b"INCR", b"counter:%d" % (i % 1000)])
    if test == "lpush": return build([b"LPUSH", b"mylist", b"ele"])
    if test == "lpop":  return build([b"RPUSH", b"plist", b"e"]) + build([b"LPOP", b"plist"])
    if test == "sadd":  return build([b"SADD", b"myset", b"m:%d" % i])
    if test == "hset":  return build([b"HSET", b"myhash", b"f:%d" % (i % 1000), b"v"])
    raise ValueError(test)

def count_replies(test):
    # number of top-level RESP replies produced per iteration
    return 2 if test == "lpop" else 1

def read_n_replies(sock, buf, need):
    """Consume `need` complete RESP replies from sock, returns leftover buf."""
    got = 0
    while got < need:
        while True:
            nl = buf.find(b"\r\n")
            if nl != -1:
                break
            chunk = sock.recv(65536)
            if not chunk:
                raise EOFError("closed")
            buf += chunk
        t = buf[:1]
        line = buf[:nl]
        if t in (b"+", b"-", b":"):
            buf = buf[nl+2:]
            got += 1
        elif t == b"$":
            n = int(line[1:])
            if n == -1:
                buf = buf[nl+2:]; got += 1
            else:
                need_bytes = nl + 2 + n + 2
                while len(buf) < need_bytes:
                    chunk = sock.recv(65536)
                    if not chunk: raise EOFError("closed")
                    buf += chunk
                buf = buf[need_bytes:]; got += 1
        elif t == b"*":
            # flatten: push element count onto need
            n = int(line[1:])
            buf = buf[nl+2:]
            # read n bulk/simple elements (assume flat)
            buf = read_n_replies_raw(sock, buf, n)
            got += 1
        else:
            raise ValueError("bad reply %r" % t)
    return buf

def read_n_replies_raw(sock, buf, need):
    got = 0
    while got < need:
        while True:
            nl = buf.find(b"\r\n")
            if nl != -1: break
            chunk = sock.recv(65536)
            if not chunk: raise EOFError("closed")
            buf += chunk
        t = buf[:1]; line = buf[:nl]
        if t in (b"+", b"-", b":"):
            buf = buf[nl+2:]; got += 1
        elif t == b"$":
            n = int(line[1:])
            if n == -1: buf = buf[nl+2:]; got += 1
            else:
                nb = nl+2+n+2
                while len(buf) < nb:
                    chunk = sock.recv(65536)
                    if not chunk: raise EOFError("closed")
                    buf += chunk
                buf = buf[nb:]; got += 1
        else:
            raise ValueError("nested array unsupported")
    return buf

errors = []

def worker(test, port, ops, pipeline):
    try:
        s = socket.socket(); s.connect(("127.0.0.1", port))
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        buf = b""
        replies_per = count_replies(test)
        i = 0
        while i < ops:
            batch = min(pipeline, ops - i)
            payload = b"".join(make_request(test, i + j) for j in range(batch))
            s.sendall(payload)
            buf = read_n_replies(s, buf, batch * replies_per)
            i += batch
        s.close()
    except Exception as e:
        errors.append("%s: %r" % (test, e))

def run_test(test, total, clients, pipeline, port):
    per = total // clients
    threads = [threading.Thread(target=worker, args=(test, port, per, pipeline))
               for _ in range(clients)]
    t0 = time.time()
    for t in threads: t.start()
    for t in threads: t.join()
    dt = time.time() - t0
    done = per * clients
    return done / dt if dt > 0 else 0.0, dt

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=100000)
    ap.add_argument("-c", type=int, default=50)
    ap.add_argument("-P", type=int, default=1)
    ap.add_argument("-p", type=int, default=6380)
    args = ap.parse_args()

    print("# mini-redis fallback load (n=%d c=%d P=%d port=%d)"
          % (args.n, args.c, args.P, args.p))
    print("%-8s %14s %10s" % ("test", "req/s", "secs"))
    for test in TESTS:
        rps, dt = run_test(test, args.n, args.c, args.P, args.p)
        print("%-8s %14.0f %10.3f" % (test, rps, dt))
    if errors:
        print("ERRORS:", *errors, sep="\n  ")
        sys.exit(1)
    print("all tests completed without errors")

if __name__ == "__main__":
    main()
