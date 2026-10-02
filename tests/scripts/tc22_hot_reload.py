#!/usr/bin/env python3
"""
tc22_hot_reload.py — TC-22: Hot-Reload (SIGUSR1)

Full automated end-to-end test for the hot-reload feature.

What this test proves:
  1. The gateway can load a new config file via SIGUSR1 with zero downtime.
  2. A NEW route becomes reachable immediately after reload.
  3. An EXISTING connection that was open BEFORE the reload is NOT dropped —
     it continues to relay data normally after the SIGUSR1.
  4. The gateway process never crashes or stalls.

Prerequisites — backends must be running BEFORE you call this script:
  python3 test_utils/echo_backend.py 7001   # backend for the first route
  python3 test_utils/echo_backend.py 7002   # backend for the second (new) route

The script starts and stops the gateway itself.

Usage:
  python3 tests/scripts/tc22_hot_reload.py
Exit: 0=PASS  1=FAIL
"""

import os
import signal
import socket
import subprocess
import sys
import time
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BINARY          = os.path.join(ROOT, "build", "sluice")
CONFIG_BEFORE   = os.path.join(ROOT, "tests", "configs", "tc_reload_before.json")
CONFIG_AFTER    = os.path.join(ROOT, "tests", "configs", "tc_reload_after.json")
# We copy CONFIG_AFTER over the live config path so the gateway re-reads it.
LIVE_CONFIG     = os.path.join(ROOT, "tests", "configs", "tc_reload_live.json")

GATEWAY_HOST    = "127.0.0.1"
PORT_OLD        = 9000   # must stay alive through reload
PORT_NEW        = 9001   # must appear after reload

PASS = True


def fail(msg):
    global PASS
    print(f"  FAIL: {msg}")
    PASS = False


def connect(port, timeout=5):
    return socket.create_connection((GATEWAY_HOST, port), timeout=timeout)


def echo_roundtrip(sock, msg):
    """Send msg, read back the echo, return the reply bytes."""
    sock.sendall(msg)
    reply = b""
    sock.settimeout(5)
    try:
        while len(reply) < len(msg):
            chunk = sock.recv(4096)
            if not chunk:
                break
            reply += chunk
    except socket.timeout:
        pass
    return reply


print("=" * 60)
print("[TC-22] Hot-Reload (SIGUSR1) test")
print("=" * 60)

# ---- Step 1: prepare the live config (copy of before) --------------------
shutil.copy(CONFIG_BEFORE, LIVE_CONFIG)

# ---- Step 2: start the gateway -------------------------------------------
print("\n[1/6] Starting gateway with initial config (1 route on :9000)...")
gw = subprocess.Popen(
    [BINARY, LIVE_CONFIG],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT
)
time.sleep(0.5)
if gw.poll() is not None:
    fail("Gateway exited immediately after start")
    sys.exit(1)
print(f"      Gateway PID: {gw.pid}")

# ---- Step 3: open a persistent connection BEFORE the reload --------------
print("\n[2/6] Opening persistent connection on :9000 (will survive reload)...")
try:
    pre_sock = connect(PORT_OLD)
    r = echo_roundtrip(pre_sock, b"pre-reload-message")
    if r == b"pre-reload-message":
        print("      Connection works — relay confirmed before reload")
    else:
        fail(f"Echo before reload returned: {r!r}")
except Exception as e:
    fail(f"Could not connect to :9000 before reload: {e}")
    gw.terminate(); gw.wait()
    sys.exit(1)

# ---- Step 4: swap config and send SIGUSR1 --------------------------------
print("\n[3/6] Swapping config file and sending SIGUSR1...")
shutil.copy(CONFIG_AFTER, LIVE_CONFIG)
os.kill(gw.pid, signal.SIGUSR1)
time.sleep(0.5)   # give gateway one epoll batch to apply the reload

if gw.poll() is not None:
    fail("Gateway crashed after SIGUSR1!")
    sys.exit(1)
print("      Gateway still running after SIGUSR1 ✓")

# ---- Step 5: verify NEW route is reachable --------------------------------
print("\n[4/6] Verifying new route :9001 is now reachable...")
try:
    new_sock = connect(PORT_NEW)
    r = echo_roundtrip(new_sock, b"post-reload-new-route")
    if r == b"post-reload-new-route":
        print("      New route :9001 is reachable and relaying ✓")
    else:
        fail(f"New route echo returned: {r!r}")
    new_sock.shutdown(socket.SHUT_WR)
    new_sock.close()
except Exception as e:
    fail(f"New route :9001 not reachable after reload: {e}")

# ---- Step 6: verify OLD persistent connection still works -----------------
print("\n[5/6] Verifying pre-reload connection on :9000 is still alive...")
try:
    r = echo_roundtrip(pre_sock, b"after-reload-still-alive")
    if r == b"after-reload-still-alive":
        print("      Pre-reload connection still relaying data ✓")
    else:
        fail(f"Pre-reload connection echo returned: {r!r}")
    pre_sock.shutdown(socket.SHUT_WR)
    pre_sock.close()
except Exception as e:
    fail(f"Pre-reload connection broke after SIGUSR1: {e}")

# ---- Cleanup -------------------------------------------------------------
print("\n[6/6] Shutting down gateway...")
gw.terminate()
gw.wait(timeout=5)
# Clean up temporary live config
try:
    os.remove(LIVE_CONFIG)
except OSError:
    pass

# ---- Result --------------------------------------------------------------
print()
print("=" * 60)
if PASS:
    print("[TC-22] PASS — hot-reload works correctly")
    sys.exit(0)
else:
    print("[TC-22] FAIL — see above for details")
    sys.exit(1)
