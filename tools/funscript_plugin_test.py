#!/usr/bin/env python3
"""Drives the funscript plugin the way the player does (JSON lines on stdin /
stdout) against tools/fake_intiface.py, and checks what reaches the virtual
devices: sync timing, positions, soft start after a seek, and the safety stops.
No hardware involved. Runs on the headset (the plugin is an aarch64 binary):

    python3 tools/funscript_plugin_test.py build/plugins/funscript
"""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_intiface import FakeIntiface, now_ms  # noqa: E402

failures = 0


def check(ok, what):
    global failures
    print(("PASS  " if ok else "FAIL  ") + what, flush=True)
    if not ok:
        failures += 1


def main():
    plugin_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/plugins/funscript")
    server = FakeIntiface()
    tmp = tempfile.mkdtemp(prefix="pvrfunscript.")
    # 20 s script: alternating bottom / top every 500 ms.
    actions = [{"at": i * 500, "pos": 0 if i % 2 == 0 else 100} for i in range(41)]
    script_text = json.dumps({"version": "1.0", "inverted": False, "range": 100, "actions": actions})
    with open(os.path.join(tmp, "Test Clip.funscript"), "w") as f:
        f.write(script_text)

    env = dict(os.environ, PIXELVR_PLUGIN_API="1", PIXELVR_PLUGIN_ID="org.pixelvr.funscript",
               PIXELVR_PLUGIN_DIR=plugin_dir, PIXELVR_PLUGIN_DATA=tmp)
    proc = subprocess.Popen([os.path.join(plugin_dir, "funscript")], cwd=plugin_dir, env=env, text=True,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(os.path.join(tmp, "plugin.log"), "w"))
    out = []  # (t_ms, message) from the plugin
    out_lock = threading.Lock()

    def reader():
        for line in proc.stdout:
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            with out_lock:
                out.append((now_ms(), msg))
            if msg.get("type") == "files.read":  # serve sidecars like the host does
                path = os.path.join(tmp, msg.get("name", ""))
                if os.path.isfile(path):
                    data = {"name": msg["name"], "text": open(path).read()}
                    send({"type": "result", "id": msg["id"], "ok": True, "data": data})
                else:
                    send({"type": "result", "id": msg["id"], "ok": False, "error": "not found"})

    send_lock = threading.Lock()

    def send(obj):
        with send_lock:
            proc.stdin.write(json.dumps(obj) + "\n")
            proc.stdin.flush()

    def messages(kind):
        with out_lock:
            return [(t, m) for t, m in out if m.get("type") == kind]

    def wait_for(pred, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.02)
        return False

    threading.Thread(target=reader, daemon=True).start()
    settings = {"intiface": True, "server": "ws://127.0.0.1:%d" % server.port, "offset": 6, "bottom": 0, "top": 0,
                "speed": 0, "vibe": 0, "vibeMax": 0, "invert": False, "folders": ""}
    send({"type": "hello", "api": 1, "apiMinor": 0, "app": "test", "granted": ["playback", "files.sidecar", "ui", "network"],
          "settings": settings})
    check(wait_for(lambda: messages("hello"), 3), "plugin answers hello")
    send({"type": "screens", "screens": [{"screen": 0, "kind": "video", "selected": True, "audio": True}]})
    send({"type": "app.state", "focused": True, "worn": True, "exiting": False})
    check(wait_for(lambda: server.commands("RequestDeviceList"), 4), "connects to Intiface and asks for devices")

    # Open the video with its sidecar, paused at 0.
    send({"type": "media.open", "screen": 0, "media": "m1", "private": False, "fileName": "Test Clip.mp4", "title": "Test Clip",
          "source": "local", "duration": 20.0, "projection": "flat", "stereo": "mono", "sidecars": ["Test Clip.funscript"]})
    send({"type": "media.state", "screen": 0, "media": "m1", "playing": False, "buffering": False, "position": 0.0,
          "rate": 1.0, "clock": int(now_ms()), "reason": "open"})
    check(wait_for(lambda: messages("files.read"), 2), "asks for the sidecar through files.read")
    check(wait_for(lambda: any("Funscript loaded" in m.get("text", "") for _, m in messages("notice")), 2), "script loaded")
    check(wait_for(lambda: messages("ui.timeline"), 2) and messages("ui.timeline")[-1][1]["tracks"], "heat track sent")
    time.sleep(0.3)
    check(not server.commands("LinearCmd"), "nothing moves while paused")

    # Play from 0 with 250 ms ticks, like the host.
    clock = {"pos": 0.0, "at": now_ms(), "playing": True}

    def state(reason):
        t = now_ms()
        pos = clock["pos"] + ((t - clock["at"]) / 1000.0 if clock["playing"] else 0.0)
        send({"type": "media.state", "screen": 0, "media": "m1", "playing": clock["playing"], "buffering": False,
              "position": pos, "rate": 1.0, "clock": int(t), "reason": reason})

    def media_time(t_ms):
        return clock["pos"] * 1000.0 + (t_ms - clock["at"])

    def play(seconds, ticks=True):
        end = time.time() + seconds
        while time.time() < end:
            time.sleep(0.25)
            if ticks:
                state("tick")

    start_cmds = len(server.commands())
    clock.update(pos=0.0, at=now_ms(), playing=True)
    state("play")
    play(4.0)
    linear = [c for c in server.commands()[start_cmds:] if c[1] == "LinearCmd"]
    check(len(linear) >= 6, "stroker commands while playing (%d)" % len(linear))
    soft = linear[0] if linear else None
    check(soft is not None and soft[2]["Vectors"][0]["Duration"] == 500, "soft start glides over 500 ms")
    late = []
    wrong = 0
    for t, _, f in linear[1:]:
        m = media_time(t)
        prev = [a for a in actions if a["at"] <= m]
        nxt = [a for a in actions if a["at"] > m]
        if not prev or not nxt:
            continue
        late.append(m - prev[-1]["at"])
        if abs(f["Vectors"][0]["Position"] - nxt[0]["pos"] / 100.0) > 0.01:
            wrong += 1
    late.sort()
    med = late[len(late) // 2] if late else 999
    check(late and med < 30 and late[-1] < 80, "commands go out on time (median %.0f ms, worst %.0f ms after the point)"
          % (med, late[-1] if late else -1))
    check(wrong == 0, "each command targets the next script point")
    durs_ok = all(400 <= f["Vectors"][0]["Duration"] <= 520 for _, _, f in linear[1:])
    check(durs_ok, "durations match the time to the next point (~500 ms)")
    vib = [c for c in server.commands()[start_cmds:] if c[1] == "ScalarCmd"]
    check(vib and all(0.0 < s["Scalar"] <= 1.0 for _, _, f in vib for s in f["Scalars"]), "vibrator follows the speed")
    check(any(c[1] == "RotateCmd" for c in server.commands()[start_cmds:]), "rotator driven")
    gauges = [m["value"] for _, m in messages("ui.gauge") if m.get("value", -1) >= 0]
    check(gauges and min(gauges) < 0.1 and max(gauges) > 0.9, "gauge swings with the script (virtual device)")

    # Seek to 10 s: a new soft start.
    mark = len(server.commands())
    clock.update(pos=10.0, at=now_ms(), playing=True)
    state("seek")
    play(1.0)
    after = [c for c in server.commands()[mark:] if c[1] == "LinearCmd"]
    # (A command for the old position may still be on its way when the seek is sent.)
    check(any(c[2]["Vectors"][0]["Duration"] == 500 for c in after[:2]), "seek: soft start again")

    def stops_within(fn, ms):
        mark = len(server.commands())
        t0 = now_ms()
        fn()
        ok = wait_for(lambda: any(c[1] == "StopAllDevices" for c in server.commands()[mark:]), ms / 1000.0)
        stop_t = next((c[0] for c in server.commands()[mark:] if c[1] == "StopAllDevices"), t0)
        return ok, stop_t - t0

    def pause():
        clock.update(pos=media_time(now_ms()) / 1000.0, at=now_ms(), playing=False)
        state("pause")
    ok, dt = stops_within(pause, 300)
    check(ok, "pause stops all devices (%.0f ms)" % dt)
    time.sleep(0.4)
    check(not [c for c in server.commands() if c[1] == "LinearCmd" and c[0] > now_ms() - 300], "no movement while paused")

    clock.update(at=now_ms(), playing=True)
    state("play")
    play(1.0)
    ok, dt = stops_within(lambda: send({"type": "app.state", "focused": True, "worn": False, "exiting": False}), 300)
    check(ok, "taking the headset off stops all devices (%.0f ms)" % dt)
    send({"type": "app.state", "focused": True, "worn": True, "exiting": False})
    play(1.0)
    ok, dt = stops_within(lambda: send({"type": "app.state", "focused": False, "worn": True, "exiting": False}), 300)
    check(ok, "losing focus (SteamVR dashboard) stops all devices (%.0f ms)" % dt)
    send({"type": "app.state", "focused": True, "worn": True, "exiting": False})
    play(1.0)
    ok, dt = stops_within(lambda: None, 2500)  # the player stops ticking while "playing"
    check(ok, "a stalled player (no ticks) stops all devices (%.0f ms)" % dt)
    state("tick")
    play(1.0)
    ok, dt = stops_within(lambda: send({"type": "media.close", "screen": 0, "media": "m1", "reason": "stop"}), 300)
    check(ok, "closing the video stops all devices (%.0f ms)" % dt)

    # Lost connection while playing, then shutdown.
    send({"type": "media.open", "screen": 0, "media": "m2", "private": False, "fileName": "Test Clip.mp4", "title": "Test Clip",
          "source": "local", "duration": 20.0, "projection": "flat", "stereo": "mono", "sidecars": ["Test Clip.funscript"]})
    clock.update(pos=2.0, at=now_ms(), playing=True)
    state("open")
    play(1.0)
    dropped_at = now_ms()
    server.drop_clients()
    play(0.5)
    check(wait_for(lambda: any("connection lost" in m.get("text", "") for t, m in messages("status") if t > dropped_at), 2),
          "status reports the lost Intiface connection")
    check(wait_for(lambda: len(server.commands("RequestServerInfo")) >= 2, 6), "reconnects to Intiface by itself")
    play(1.0)
    mark = len(server.commands())
    send({"type": "shutdown"})
    try:
        proc.wait(timeout=3)
        exited = True
    except subprocess.TimeoutExpired:
        proc.kill()
        exited = False
    check(exited, "plugin exits on shutdown")
    check(any(c[1] == "StopAllDevices" for c in server.commands()[mark:]), "shutdown stops all devices")

    print("%s (%d failure%s); plugin log: %s" % ("FAILED" if failures else "ALL PASSED", failures,
                                                 "" if failures == 1 else "s", os.path.join(tmp, "plugin.log")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
