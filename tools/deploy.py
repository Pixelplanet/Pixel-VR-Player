#!/usr/bin/env python3
"""Deploy Pixel VR Player to a Steam Frame that is in Developer Mode.

This copies the aarch64 binary, the compiled SPIR-V shaders and the direct-run
helper into ``~/devkit-game/PixelVRPlayer`` on the headset and registers it as a
Non-Steam "Devkit Game" that runs *directly on the host* (no Steam Linux Runtime
container), which is how a native SteamOS/ARM64 OpenXR app must launch so the
SteamVR runtime and compositor are wired up.

Prerequisites on the headset:
  * Developer Mode enabled and paired with the SteamOS Devkit Client
    (this installs ``~/devkit-utils`` and authorises the ``devkit_rsa`` key).

Examples:
  python3 tools/deploy.py                       # copy + register only
  python3 tools/deploy.py --launch              # copy, register and start it
  python3 tools/deploy.py --launch -- /home/steamos/Videos/clip_360_tb.mp4
  python3 tools/deploy.py --host steamos@192.168.1.50 --launch

Environment overrides: FRAME_HOST, FRAME_SSH_KEY.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

GAME = "PixelVRPlayer"
REMOTE_DIR = f"devkit-game/{GAME}"  # relative to the remote $HOME
PROJECT_ROOT = Path(__file__).resolve().parent.parent


def default_devkit_key() -> str | None:
    """Locate the SteamOS Devkit Client SSH key for the current OS."""
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
        candidates = [base / "steamos-devkit" / "steamos-devkit" / "devkit_rsa"]
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
        candidates = [base / "steamos-devkit" / "devkit_rsa"]
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
        candidates = [base / "steamos-devkit" / "devkit_rsa"]
    for key in candidates:
        if key.is_file():
            return str(key)
    return None


def find_build_outputs() -> tuple[Path, Path]:
    """Return (binary, shader_dir), preferring the aarch64 build."""
    for build in ("build-aarch64", "build"):
        binary = PROJECT_ROOT / build / "bin" / "pixelvr"
        if binary.is_file():
            return binary, PROJECT_ROOT / build / "shaders"
    raise FileNotFoundError(
        "No pixelvr binary found. Build first, e.g.: ./tools/build.sh --aarch64"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Deploy Pixel VR Player to a Steam Frame.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--host",
        default=os.environ.get("FRAME_HOST", "steamos@frame"),
        help="SSH target (default: steamos@frame or $FRAME_HOST).",
    )
    parser.add_argument(
        "--key",
        default=os.environ.get("FRAME_SSH_KEY") or default_devkit_key(),
        help="SSH private key (default: SteamOS Devkit devkit_rsa).",
    )
    parser.add_argument(
        "--launch",
        action="store_true",
        help="Start the app on the headset after deploying.",
    )
    args, player_args = parser.parse_known_args()
    if player_args and player_args[0] == "--":
        player_args = player_args[1:]

    binary, shader_dir = find_build_outputs()
    shaders = sorted(shader_dir.glob("*.spv"))
    if not shaders:
        print(f"warning: no compiled shaders in {shader_dir}", file=sys.stderr)

    host = args.host if "@" in args.host else f"steamos@{args.host}"
    ssh_opts = ["-o", "ConnectTimeout=10", "-o", "BatchMode=yes"]
    if args.key:
        ssh_opts += ["-i", str(Path(args.key).expanduser())]

    def ssh(command: str) -> str:
        result = subprocess.run(
            ["ssh", *ssh_opts, host, command],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
            timeout=180,
        )
        return result.stdout.strip()

    def scp(sources: list[str], dest: str) -> None:
        subprocess.run(
            ["scp", *ssh_opts, *sources, f"{host}:{dest}"],
            check=True,
            timeout=300,
        )

    # 1. Verify Developer Mode, ensure the target dir, refuse to clobber a run.
    print(f"Connecting to {host} ...")
    remote_dir = ssh(
        "set -eu; "
        'test -x "$HOME/devkit-utils/steam-client-create-shortcut" || '
        "{ echo 'error: ~/devkit-utils missing - enable Developer Mode and pair "
        "the SteamOS Devkit Client first.' >&2; exit 1; }; "
        f'target="$HOME/{REMOTE_DIR}"; '
        'if command -v fuser >/dev/null 2>&1 && fuser "$target/pixelvr" '
        ">/dev/null 2>&1; then echo 'error: pixelvr is running - stop it in Steam "
        "before deploying.' >&2; exit 1; fi; "
        'mkdir -p "$target/shaders"; printf %s "$target"'
    )

    # 2. Copy binary, shaders, launcher, and icon.
    print("Copying binary ...")
    scp([str(binary)], f"{REMOTE_DIR}/pixelvr")
    if shaders:
        print(f"Copying {len(shaders)} shader(s) ...")
        scp([str(p) for p in shaders], f"{REMOTE_DIR}/shaders/")
    launch_helper = PROJECT_ROOT / "tools" / "launch.sh"
    if launch_helper.is_file():
        scp([str(launch_helper)], f"{REMOTE_DIR}/launch.sh")
    for asset_name in ("icon.png", "hero.png", "capsule.png", "header.png", "logo.png"):
        asset_file = PROJECT_ROOT / "assets" / asset_name
        if asset_file.is_file():
            scp([str(asset_file)], f"{REMOTE_DIR}/{asset_name}")

    # 3. Register the Non-Steam shortcut (runs on the host, not in Sniper).
    request = {
        "gameid": GAME,
        "directory": remote_dir,
        "argv": ["./pixelvr", *player_args],
        "settings": {"steam_play": "0", "compat_tool": ""},
    }
    print("Registering the Non-Steam shortcut ...")
    raw = ssh(
        f"chmod u+x {shlex.quote(f'{REMOTE_DIR}/pixelvr')} "
        f"{shlex.quote(f'{REMOTE_DIR}/launch.sh')} 2>/dev/null; "
        + shlex.join(
            [
                "python3",
                "-B",
                "devkit-utils/steam-client-create-shortcut",
                "--parms",
                json.dumps(request),
            ]
        )
    )
    try:
        response = json.loads(raw) if raw else {}
    except json.JSONDecodeError:
        response = {"raw": raw}
    if isinstance(response, dict) and response.get("error"):
        raise RuntimeError(f"shortcut registration failed: {response['error']}")

    # 3b. Sanitize shortcuts.vdf to remove the 'Devkit Game: ' prefix and set icon.
    sanitize_script = (
        "import glob, os, struct, shutil\n"
        "OBJ, STR, INT, U64, END = 0, 1, 2, 7, 8\n"
        "def read_cstr(buf, pos):\n"
        "    end = buf.find(b'\\0', pos)\n"
        "    return buf[pos:end].decode('utf-8', 'surrogateescape'), end + 1\n"
        "def parse_obj(buf, pos):\n"
        "    entries = {}\n"
        "    while pos < len(buf):\n"
        "        kind = buf[pos]\n"
        "        if kind in (END, 0x0B): return entries, pos + 1\n"
        "        key, pos = read_cstr(buf, pos + 1)\n"
        "        if kind == OBJ: value, pos = parse_obj(buf, pos)\n"
        "        elif kind == STR: value, pos = read_cstr(buf, pos)\n"
        "        elif kind == INT: value = struct.unpack_from('<i', buf, pos)[0]; pos += 4\n"
        "        elif kind == U64: value = struct.unpack_from('<Q', buf, pos)[0]; pos += 8\n"
        "        else: break\n"
        "        entries[key] = [kind, value]\n"
        "    return entries, pos\n"
        "def dump(entries):\n"
        "    out = bytearray()\n"
        "    for key, (kind, value) in entries.items():\n"
        "        kb = key.encode('utf-8', 'surrogateescape')\n"
        "        if kind == OBJ: out += bytes([OBJ]) + kb + b'\\0' + dump(value) + bytes([END])\n"
        "        elif kind == STR: out += bytes([STR]) + kb + b'\\0' + value.encode('utf-8', 'surrogateescape') + b'\\0'\n"
        "        elif kind == INT: out += bytes([INT]) + kb + b'\\0' + struct.pack('<i', value)\n"
        "        elif kind == U64: out += bytes([U64]) + kb + b'\\0' + struct.pack('<Q', value)\n"
        "    return bytes(out)\n"
        "for vdf in glob.glob(os.path.expanduser('~/.steam/steam/userdata/*/config/shortcuts.vdf')):\n"
        "    try:\n"
        "        with open(vdf, 'rb') as f: data = f.read()\n"
        "        if not data or data[0] != OBJ: continue\n"
        "        key, pos = read_cstr(data, 1)\n"
        "        s_dict, _ = parse_obj(data, pos)\n"
        "        mod = False\n"
        "        for k, v in s_dict.items():\n"
        "            if isinstance(v, list) and v[0] == OBJ:\n"
        "                e = v[1]\n"
        "                name = e.get('AppName', [STR, ''])[1]\n"
        "                if 'Pixel' in name:\n"
        "                    e['AppName'] = [STR, 'Pixel VR Player']\n"
        "                    e['Devkit'] = [INT, 0]\n"
        "                    e['DevkitGameID'] = [STR, '']\n"
        "                    icon_p = os.path.expanduser('~/" + REMOTE_DIR + "/icon.png')\n"
        "                    hero_p = os.path.expanduser('~/" + REMOTE_DIR + "/hero.png')\n"
        "                    capsule_p = os.path.expanduser('~/" + REMOTE_DIR + "/capsule.png')\n"
        "                    header_p = os.path.expanduser('~/" + REMOTE_DIR + "/header.png')\n"
        "                    logo_p = os.path.expanduser('~/" + REMOTE_DIR + "/logo.png')\n"
        "                    grid = os.path.join(os.path.dirname(os.path.dirname(vdf)), 'config', 'grid')\n"
        "                    os.makedirs(grid, exist_ok=True)\n"
        "                    raw_id = e.get('appid', [INT, 0])[1] & 0xFFFFFFFF\n"
        "                    if os.path.isfile(icon_p):\n"
        "                        e['icon'] = [STR, icon_p]\n"
        "                        shutil.copyfile(icon_p, os.path.join(grid, f'{raw_id}_icon.png'))\n"
        "                    if os.path.isfile(capsule_p):\n"
        "                        shutil.copyfile(capsule_p, os.path.join(grid, f'{raw_id}p.png'))\n"
        "                    elif os.path.isfile(icon_p):\n"
        "                        shutil.copyfile(icon_p, os.path.join(grid, f'{raw_id}p.png'))\n"
        "                    if os.path.isfile(header_p):\n"
        "                        shutil.copyfile(header_p, os.path.join(grid, f'{raw_id}.png'))\n"
        "                    elif os.path.isfile(icon_p):\n"
        "                        shutil.copyfile(icon_p, os.path.join(grid, f'{raw_id}.png'))\n"
        "                    if os.path.isfile(hero_p):\n"
        "                        shutil.copyfile(hero_p, os.path.join(grid, f'{raw_id}_hero.png'))\n"
        "                    if os.path.isfile(logo_p):\n"
        "                        shutil.copyfile(logo_p, os.path.join(grid, f'{raw_id}_logo.png'))\n"
        "                    mod = True\n"
        "        if mod:\n"
        "            tmp = vdf + '.tmp'\n"
        "            with open(tmp, 'wb') as f: f.write(dump({key: [OBJ, s_dict]}) + bytes([END]))\n"
        "            os.replace(tmp, vdf)\n"
        "    except Exception:\n"
        "        pass\n"
    )
    ssh(f"python3 -c {shlex.quote(sanitize_script)}")

    # 4. Optionally launch it in the headset.
    if args.launch:
        print("Launching in the headset ...")
        ssh(
            shlex.join(
                [
                    "python3",
                    "-B",
                    "devkit-utils/steam-devkit-rpc",
                    "run-game",
                    f"gameid={GAME}",
                ]
            )
        )

    print()
    print(f"Deployed to {host}:~/{REMOTE_DIR}")
    if player_args:
        print("  argv:", " ".join(["./pixelvr", *player_args]))
    print("  Launch from Steam > Library > Non-Steam > 'Pixel VR Player'")
    print(f"  Or over SSH:  ssh {host} '{REMOTE_DIR}/launch.sh <video>'")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as exc:
        print(f"error: command failed ({exc.returncode}): {exc}", file=sys.stderr)
        sys.exit(1)
    except (FileNotFoundError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
