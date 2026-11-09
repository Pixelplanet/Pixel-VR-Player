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
        # What the virtual devices are doing (for the live page): the stroker moves from lin_from to
        # lin_to over lin_dur ms from lin_start; the vibrator and rotator run at vib / rot (0..1).
        self.state = {"clients": 0, "lin_from": 0.0, "lin_to": 0.0, "lin_start": now_ms(), "lin_dur": 0,
                      "vib": 0.0, "rot": 0.0, "count": 0, "last": ""}
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

    def stroker_position(self, t):
        st = self.state
        if st["lin_dur"] <= 0:
            return st["lin_to"]
        f = min(1.0, max(0.0, (t - st["lin_start"]) / st["lin_dur"]))
        return st["lin_from"] + (st["lin_to"] - st["lin_from"]) * f

    def _apply(self, kind, fields):
        """Updates the virtual devices from one command (caller holds the lock)."""
        st, t = self.state, now_ms()
        if kind == "LinearCmd" and fields.get("Vectors"):
            v = fields["Vectors"][0]
            st.update(lin_from=self.stroker_position(t), lin_to=float(v.get("Position", 0.0)), lin_start=t,
                      lin_dur=int(v.get("Duration", 0)))
        elif kind == "ScalarCmd" and fields.get("Scalars"):
            st["vib"] = float(fields["Scalars"][0].get("Scalar", 0.0))
        elif kind == "RotateCmd" and fields.get("Rotations"):
            st["rot"] = float(fields["Rotations"][0].get("Speed", 0.0))
        elif kind in ("StopAllDevices", "StopDeviceCmd"):
            p = self.stroker_position(t)
            st.update(lin_from=p, lin_to=p, lin_dur=0, vib=0.0, rot=0.0)
        if kind not in ("RequestServerInfo", "RequestDeviceList", "Ping"):
            st["count"] += 1
            st["last"] = kind

    def serve_page(self, port):
        """A live page of the virtual devices on http://<headset>:<port>/ (open it in a browser screen)."""
        import http.server
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                if self.path.startswith("/state"):
                    with owner.lock:
                        body = json.dumps(dict(owner.state, now=now_ms())).encode()
                    ctype = "application/json"
                else:
                    body, ctype = PAGE.encode(), "text/html; charset=utf-8"
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        srv = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        return srv

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
        with self.lock:
            self.state["clients"] += 1
        try:
            self._serve_frames(c, buf)
        finally:
            with self.lock:
                self.state["clients"] -= 1
                p = self.stroker_position(now_ms())  # Intiface stops the devices when the client goes
                self.state.update(lin_from=p, lin_to=p, lin_dur=0, vib=0.0, rot=0.0)

    def _serve_frames(self, c, buf):
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
                        self._apply(kind, fields)
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


PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>Virtual devices</title><style>
body{margin:0;background:#0d1117;color:#e6edf3;font:22px/1.3 system-ui,sans-serif;display:flex;flex-direction:column;
align-items:center;justify-content:center;height:100vh}
h1{font-size:34px;margin:0 0 6px}#status{font-size:22px;margin-bottom:26px;color:#8b949e}
.row{display:flex;gap:70px;align-items:flex-end}.dev{display:flex;flex-direction:column;align-items:center;gap:14px}
.track{width:90px;height:340px;border-radius:45px;background:#21262d;position:relative;overflow:hidden}
#slider{position:absolute;left:8px;right:8px;height:74px;border-radius:37px;background:#58a6ff}
.vib{width:200px;height:200px;border-radius:50%;background:#21262d;display:flex;align-items:center;justify-content:center}
#vibdot{width:120px;height:120px;border-radius:50%;background:#f778ba}
.rot{width:200px;height:200px;border-radius:50%;background:#21262d;display:flex;align-items:center;justify-content:center}
#rotor{width:150px;height:22px;border-radius:11px;background:#3fb950}
.val{font-size:26px;font-weight:600}.label{color:#8b949e}
</style></head><body><h1>Virtual devices</h1><div id="status">Waiting for the funscript plugin...</div>
<div class="row"><div class="dev"><div class="track"><div id="slider"></div></div><div class="val" id="pos">0%</div>
<div class="label">Stroker</div></div><div class="dev"><div class="vib"><div id="vibdot"></div></div>
<div class="val" id="vib">0%</div><div class="label">Vibrator</div></div><div class="dev"><div class="rot">
<div id="rotor"></div></div><div class="val" id="rot">0%</div><div class="label">Rotator</div></div></div>
<script>
let s=null,skew=0,angle=0,last=performance.now();
async function poll(){try{const r=await fetch('/state');const j=await r.json();skew=j.now-performance.now();s=j;}catch(e){}
setTimeout(poll,100)}poll();
function frame(t){requestAnimationFrame(frame);if(!s)return;const now=t+skew,dt=(t-last)/1000;last=t;
let p=s.lin_to;if(s.lin_dur>0){const f=Math.min(1,Math.max(0,(now-s.lin_start)/s.lin_dur));p=s.lin_from+(s.lin_to-s.lin_from)*f;}
document.getElementById('slider').style.bottom=(8+p*(340-74-16))+'px';
document.getElementById('pos').textContent=Math.round(p*100)+'%';
const v=s.vib,sh=v*7*Math.sin(t/18);document.getElementById('vibdot').style.transform=
'translate('+sh+'px,'+(-sh)+'px) scale('+(0.75+0.35*v)+')';document.getElementById('vib').textContent=Math.round(v*100)+'%';
angle+=s.rot*720*dt;document.getElementById('rotor').style.transform='rotate('+angle+'deg)';
document.getElementById('rot').textContent=Math.round(s.rot*100)+'%';
document.getElementById('status').textContent=s.clients>0?('Player connected  |  '+s.count+' commands, last: '+s.last):
'Waiting for the funscript plugin...';}
requestAnimationFrame(frame);
</script></body></html>"""


if __name__ == "__main__":
    # fake_intiface.py [ws-port] [--web page-port]: a live page of the devices on http://<host>:<page-port>/
    args = [a for a in sys.argv[1:]]
    web = None
    if "--web" in args:
        i = args.index("--web")
        web = int(args[i + 1]) if i + 1 < len(args) else 12346
        del args[i:i + 2]
    srv = FakeIntiface(int(args[0]) if args else 12345)
    print("fake Intiface on ws://127.0.0.1:%d" % srv.port, flush=True)
    if web:
        srv.serve_page(web)
        print("virtual devices page on http://127.0.0.1:%d/" % web, flush=True)
    seen = 0
    while True:
        time.sleep(0.1)
        cmds = srv.commands()
        for t, kind, fields in cmds[seen:]:
            print("%10.1f %-16s %s" % (t, kind, json.dumps(fields)), flush=True)
        seen = len(cmds)
