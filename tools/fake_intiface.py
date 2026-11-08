#!/usr/bin/env python3
"""Fake Intiface Central: a Buttplug (message spec v3) WebSocket server with
virtual devices that logs every command with a CLOCK_MONOTONIC timestamp.
Standard library only. Development tool for building device support blind.

    python3 tools/fake_intiface.py [port]      # run standalone and print commands
"""
import base64
import hashlib
import json
import socket
import struct
import sys
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

DEVICES = [
    {"DeviceName": "Fake Stroker", "DeviceIndex": 0,
     "DeviceMessages": {"LinearCmd": [{"StepCount": 100, "FeatureDescriptor": "Stroker", "ActuatorType": "Position"}],
                        "StopDeviceCmd": {}}},
    {"DeviceName": "Fake Vibrator", "DeviceIndex": 1,
     "DeviceMessages": {"ScalarCmd": [{"StepCount": 20, "FeatureDescriptor": "Motor", "ActuatorType": "Vibrate"}],
                        "StopDeviceCmd": {}}},
    {"DeviceName": "Fake Rotator", "DeviceIndex": 2,
     "DeviceMessages": {"RotateCmd": [{"StepCount": 20, "FeatureDescriptor": "Rotor"}], "StopDeviceCmd": {}}},
]


def now_ms():
    return time.monotonic() * 1000.0


def parse_frame(buf):
    """Returns (opcode, payload, rest) for one complete frame at the start of buf, else None."""
    if len(buf) < 2:
        return None
    n = buf[1] & 0x7F
    off = 2
    if n == 126:
        if len(buf) < 4:
            return None
        n = struct.unpack(">H", buf[2:4])[0]
        off = 4
    elif n == 127:
        if len(buf) < 10:
            return None
        n = struct.unpack(">Q", buf[2:10])[0]
        off = 10
    mask = b"\0\0\0\0"
    if buf[1] & 0x80:
        if len(buf) < off + 4:
            return None
        mask = buf[off:off + 4]
        off += 4
    if len(buf) < off + n:
        return None
    payload = bytes(b ^ mask[i % 4] for i, b in enumerate(buf[off:off + n]))
    return buf[0] & 0x0F, payload, buf[off + n:]


class FakeIntiface:
    def __init__(self, port=0, max_ping_ms=0):
        self.log = []            # (t_ms, type, fields)
        self.lock = threading.Lock()
        self.max_ping_ms = max_ping_ms
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(4)
        self.port = self.srv.getsockname()[1]
        self.clients = []
        threading.Thread(target=self._accept, daemon=True).start()

    def commands(self, kind=None):
        with self.lock:
            return [e for e in self.log if kind is None or e[1] == kind]

    def drop_clients(self):
        for c in list(self.clients):
            try:
                c.shutdown(socket.SHUT_RDWR)  # close() alone does not end a recv() in another thread
                c.close()
            except OSError:
                pass
        self.clients.clear()

    def _accept(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            self.clients.append(c)
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    def _send(self, c, obj):
        data = json.dumps(obj).encode()
        hdr = bytes([0x81])
        n = len(data)
        hdr += bytes([n]) if n < 126 else (bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n))
        c.sendall(hdr + data)

    def _serve(self, c):
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = c.recv(4096)
            if not d:
                return
            buf += d
        head, buf = buf.split(b"\r\n\r\n", 1)
        key = ""
        for line in head.decode(errors="replace").split("\r\n"):
            if line.lower().startswith("sec-websocket-key:"):
                key = line.split(":", 1)[1].strip()
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
        while True:
            frame = parse_frame(buf)
            while frame is None:
                try:
                    d = c.recv(65536)
                except OSError:
                    return
                if not d:
                    return
                buf += d
                frame = parse_frame(buf)
            op, payload, buf = frame
            if op == 0x8:
                return
            if op != 0x1:
                continue
            try:
                msgs = json.loads(payload.decode())
            except ValueError:
                continue
            for entry in msgs:
                for kind, fields in entry.items():
                    with self.lock:
                        self.log.append((now_ms(), kind, fields))
                    mid = fields.get("Id", 0)
                    try:
                        if kind == "RequestServerInfo":
                            self._send(c, [{"ServerInfo": {"Id": mid, "ServerName": "Fake Intiface", "MessageVersion": 3,
                                                           "MaxPingTime": self.max_ping_ms}}])
                        elif kind == "RequestDeviceList":
                            self._send(c, [{"DeviceList": {"Id": mid, "Devices": DEVICES}}])
                        else:
                            self._send(c, [{"Ok": {"Id": mid}}])
                    except OSError:
                        return  # client gone (or dropped by the test)


if __name__ == "__main__":
    srv = FakeIntiface(int(sys.argv[1]) if len(sys.argv) > 1 else 12345)
    print("fake Intiface on ws://127.0.0.1:%d" % srv.port, flush=True)
    seen = 0
    while True:
        time.sleep(0.1)
        cmds = srv.commands()
        for t, kind, fields in cmds[seen:]:
            print("%10.1f %-16s %s" % (t, kind, json.dumps(fields)), flush=True)
        seen = len(cmds)
