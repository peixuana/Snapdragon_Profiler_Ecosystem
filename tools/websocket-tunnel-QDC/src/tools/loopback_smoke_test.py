#!/usr/bin/env python3
# ============================================================================================================
#
#                  Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                              SPDX-License-Identifier: BSD-3-Clause
#
# ============================================================================================================
"""
Loopback smoke test for qdc-tunnel-target + qdc-tunnel-host.

Starts a simple echo server, then launches both tunnel binaries and verifies
that data flows end-to-end through the WebSocket multiplexed tunnel.

Usage:
    python loopback_smoke_test.py <path/to/qdc-tunnel-target> <path/to/qdc-tunnel-host>

Port layout (all localhost):
    TCP_PORT  6500  – device-local TCP port (profiler clients, i.e. our test client)
    WS_PORT   9000  – WebSocket port (device server ↔ host client)
    ECHO_PORT 6600  – local echo server (the "forward target" on the host side)

Test 1: 5 concurrent streams each sending 1 KB of data, verify echo.
Test 2: single stream sending 4 MB of data, verify round-trip integrity.
"""

import os
import socket
import subprocess
import sys
import threading
import time

TCP_PORT  = 6500
WS_PORT   = 9000
ECHO_PORT = 6600

ECHO_BACKLOG = 16
CONNECT_TIMEOUT = 5.0
TRANSFER_TIMEOUT = 30.0


# ── Echo server ─────────────────────────────────────────────────────────────

def _echo_client_thread(conn: socket.socket) -> None:
    try:
        conn.settimeout(TRANSFER_TIMEOUT)
        while True:
            data = conn.recv(65536)
            if not data:
                break
            conn.sendall(data)
    except Exception:
        pass
    finally:
        conn.close()


def _echo_server_thread(srv: socket.socket) -> None:
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=_echo_client_thread, args=(conn,), daemon=True).start()


def start_echo_server() -> socket.socket:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", ECHO_PORT))
    srv.listen(ECHO_BACKLOG)
    threading.Thread(target=_echo_server_thread, args=(srv,), daemon=True).start()
    return srv


# ── Tunnel process helpers ───────────────────────────────────────────────────

def start_target(target_bin: str) -> subprocess.Popen:
    return subprocess.Popen(
        [target_bin,
         "--port-map", f"{TCP_PORT}:{WS_PORT}",
         "--bind-host", "127.0.0.1"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )


def start_host(host_bin: str) -> subprocess.Popen:
    return subprocess.Popen(
        [host_bin,
         "--remote-host", "127.0.0.1",
         "--port-map", f"{WS_PORT}:{ECHO_PORT}",
         "--forward-host", "127.0.0.1"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )


def wait_for_port(host: str, port: int, timeout: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            s = socket.create_connection((host, port), timeout=0.2)
            s.close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


# ── Test helpers ─────────────────────────────────────────────────────────────

def run_echo_stream(size: int, idx: int, results: list, lock: threading.Lock) -> None:
    try:
        s = socket.create_connection(("127.0.0.1", TCP_PORT), timeout=CONNECT_TIMEOUT)
        s.settimeout(TRANSFER_TIMEOUT)
        payload = bytes(range(256)) * (size // 256) + bytes(range(size % 256))

        sent = 0
        while sent < len(payload):
            n = s.send(payload[sent:sent + 65536])
            if n == 0:
                raise RuntimeError("connection closed during send")
            sent += n

        received = b""
        while len(received) < len(payload):
            chunk = s.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed before all data received")
            received += chunk

        s.close()
        ok = received == payload
        with lock:
            results.append(("ok" if ok else "mismatch", idx, size))
    except Exception as exc:
        with lock:
            results.append(("error", idx, str(exc)))


# ── Main ─────────────────────────────────────────────────────────────────────

def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <qdc-tunnel-target> <qdc-tunnel-host>", file=sys.stderr)
        return 2

    target_bin = sys.argv[1]
    host_bin = sys.argv[2]

    for p in (target_bin, host_bin):
        if not os.path.isfile(p):
            print(f"ERROR: binary not found: {p}", file=sys.stderr)
            return 1

    print("Starting echo server …")
    echo_srv = start_echo_server()

    print("Starting qdc-tunnel-target (device) …")
    target_proc = start_target(target_bin)

    print("Starting qdc-tunnel-host …")
    host_proc = start_host(host_bin)

    try:
        print(f"Waiting for TCP port {TCP_PORT} to open …")
        if not wait_for_port("127.0.0.1", TCP_PORT, timeout=15.0):
            print(f"ERROR: TCP port {TCP_PORT} did not open within 15 s", file=sys.stderr)
            return 1

        # Give the host a moment to connect its WebSocket.
        time.sleep(1.0)

        # ── Test 1: concurrent streams ──────────────────────────────────────
        print("\n=== Test 1: 5 concurrent 1 KB streams ===")
        results: list = []
        lock = threading.Lock()
        threads = []
        for i in range(5):
            t = threading.Thread(
                target=run_echo_stream, args=(1024, i, results, lock), daemon=True
            )
            threads.append(t)
            t.start()
        for t in threads:
            t.join(timeout=TRANSFER_TIMEOUT + 5)

        failures = [r for r in results if r[0] != "ok"]
        if failures:
            print(f"FAIL: {len(failures)} stream(s) failed: {failures}")
            return 1
        print(f"PASS: all {len(results)} streams matched")

        # ── Test 2: large payload ───────────────────────────────────────────
        print("\n=== Test 2: 4 MB single-stream round-trip ===")
        results = []
        run_echo_stream(4 * 1024 * 1024, 0, results, lock)
        if results[0][0] != "ok":
            print(f"FAIL: {results[0]}")
            return 1
        print("PASS: 4 MB round-trip matched")

    finally:
        target_proc.terminate()
        host_proc.terminate()
        echo_srv.close()
        target_proc.wait(timeout=5)
        host_proc.wait(timeout=5)

    print("\nAll tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
