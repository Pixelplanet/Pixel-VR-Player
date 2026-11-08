#!/usr/bin/env python3
"""Checks the DeoVR-compatible sync plugin the way MultiFunPlayer uses DeoVR's
remote protocol (TCP 23554, 4-byte little-endian length + JSON, 1 s packets).

    python3 tools/deovr_plugin_test.py build/plugins/deovr-remote
"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

failures = 0


def check(ok, what):
    global failures
    print(("PASS  " if ok else "FAIL  ") + what, flush=True)
    if not ok:
        failures += 1


def now_ms():
    return time.monotonic() * 1000.0


def main():
    plugin_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/plugins/deovr-remote")
    tmp = tempfile.mkdtemp(prefix="pvrdeovr.")
    proc = subprocess.Popen([os.path.join(plugin_dir, "deovr-remote")], cwd=plugin_dir, text=True,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(os.path.join(tmp, "plugin.log"), "w"))
    out = []
    lock = threading.Lock()

    def reader():
        for line in proc.stdout:
            try:
                with lock:
                    out.append(json.loads(line))
            except ValueError:
                pass

    def send(obj):
        proc.stdin.write(json.dumps(obj) + "\n")
        proc.stdin.flush()

    def plugin_said(kind):
        with lock:
            return [m for m in out if m.get("type") == kind]

    threading.Thread(target=reader, daemon=True).start()
    send({"type": "hello", "api": 1, "apiMinor": 0, "app": "test", "granted": ["playback", "control", "network"],
          "settings": {"control": True, "port": 0}})
    time.sleep(0.5)
    check(bool(plugin_said("hello")), "plugin answers hello")
    statuses = [m["text"] for m in plugin_said("status")]
    check(any("port 23554" in s for s in statuses), "listens on 23554 and says where")

    sock = socket.create_connection(("127.0.0.1", 23554), timeout=3)
    sock.settimeout(0.2)
    buf = b""
    packets = []

    def pump(seconds):
        nonlocal buf
        end = time.time() + seconds
        last_ping = 0
        while time.time() < end:
            if time.time() - last_ping > 0.9:  # clients must ping at least once a second
                sock.sendall(struct.pack("<I", 0))
                last_ping = time.time()
            try:
                d = sock.recv(65536)
                if d:
                    buf += d
            except socket.timeout:
                pass
            while len(buf) >= 4:
                n = struct.unpack("<I", buf[:4])[0]
                if len(buf) < 4 + n:
                    break
                packets.append((now_ms(), json.loads(buf[4:4 + n]) if n else None))
                buf = buf[4 + n:]

    send({"type": "screens", "screens": [{"screen": 0, "kind": "video", "selected": True, "audio": True}]})
    pump(1.5)
    check(packets and all(p is None for _, p in packets), "pings only while nothing plays")
    send({"type": "media.open", "screen": 0, "media": "m1", "private": False, "fileName": "Test Clip.mp4",
          "title": "Test Clip", "source": "local", "duration": 120.0, "sidecars": []})
    t0 = now_ms()
    send({"type": "media.state", "screen": 0, "media": "m1", "playing": True, "buffering": False, "position": 10.0,
          "rate": 1.0, "clock": int(t0), "reason": "play"})
    mark = len(packets)
    pump(2.5)
    data = [(t, p) for t, p in packets[mark:] if p]
    check(len(data) >= 2, "reports at least once a second (%d packets)" % len(data))
    check(data and data[-1][1].get("path") == "Test Clip.mp4" and data[-1][1].get("playerState") == 0, "file name and playing state")
    err = max((abs(p["currentTime"] - (10.0 + (t - t0) / 1000.0)) for t, p in data), default=9)
    check(err < 0.1, "currentTime matches the player clock (worst %.3f s)" % err)
    check(all(abs(p.get("duration", 0) - 120.0) < 0.01 for _, p in data), "duration reported")

    def payload(obj):
        b = json.dumps(obj).encode()
        return struct.pack("<I", len(b)) + b

    sock.sendall(payload({"playerState": 1}))
    sock.sendall(payload({"currentTime": 50.0, "playerState": 0}))
    pump(0.6)
    controls = [(m["do"], m.get("position", 0)) for m in plugin_said("control")]
    check(("pause", 0.0) in [(d, round(p)) for d, p in controls], "client pause reaches the player")
    check(any(d == "seek" and abs(p - 50.0) < 0.01 for d, p in controls), "client seek reaches the player")

    send({"type": "media.close", "screen": 0, "media": "m1", "reason": "stop"})
    send({"type": "media.open", "screen": 0, "media": "m2", "private": True})
    mark = len(packets)
    pump(1.5)
    check(packets[mark:] and all(p is None for _, p in packets[mark:]), "a private (vault) video is never reported")

    send({"type": "shutdown"})
    try:
        proc.wait(timeout=3)
        check(True, "exits on shutdown")
    except subprocess.TimeoutExpired:
        proc.kill()
        check(False, "exits on shutdown")
    print("%s (%d failure%s)" % ("FAILED" if failures else "ALL PASSED", failures, "" if failures == 1 else "s"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
