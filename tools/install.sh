#!/bin/sh
# Pixel VR Player installer for the Valve Steam Frame.
#
# Download-and-run (no other files needed):
#   curl -fsSL https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player/main/tools/install.sh | sh
#
# Or from an extracted release tarball:
#   ./install.sh
#
# Options:
#   --dir DIR           install directory (default: ~/PixelVRPlayer)
#   --version vX.Y.Z    release to install (default: the latest release)
#   --no-steam          do not add the Non-Steam Game shortcut
#   --restart-steam     close Steam before registering, then start it again
#   --uninstall         remove the app and its Steam shortcut
#   --force             install even if the CPU is not aarch64
#   -h, --help          show this help
#
# Settings (~/.config/pixelvr) and the thumbnail cache (~/.cache/pixelvr) live
# outside the install directory, so reinstalling or upgrading never loses them.
set -eu

# Everything this script prints is also appended to logs/install.log in the player folder (the
# desktop icons point users there). Run from a file we re-run ourselves through
# tee; when piped from curl the message helpers below write the log instead.
LOG_FILE="${PIXELVR_INSTALL_DIR:-$HOME/PixelVRPlayer}/logs/install.log"
mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null || true
# Old logs from the home folder: appended to this one (an older version may have written there again).
for old in "$HOME/pixelvr-install.log" "$HOME/.local/state/pixelvr/install.log"; do
    if [ -f "$old" ]; then cat "$old" >> "$LOG_FILE" 2>/dev/null && rm -f "$old" 2>/dev/null || true; fi
done
if [ -z "${PIXELVR_INSTALL_LOGGING:-}" ] && [ -f "$0" ] && command -v tee >/dev/null 2>&1; then
    status_file=$(mktemp 2>/dev/null) || status_file=""
    if [ -n "$status_file" ]; then
        PIXELVR_INSTALL_LOGGING=1
        export PIXELVR_INSTALL_LOGGING
        printf '\n=== %s  install.sh %s ===\n' "$(date '+%F %T')" "$*" >> "$LOG_FILE" 2>/dev/null || true
        { sh "$0" "$@"; echo $? > "$status_file"; } 2>&1 | tee -a "$LOG_FILE"
        rc=$(cat "$status_file" 2>/dev/null || echo 1)
        rm -f "$status_file"
        exit "$rc"
    fi
fi
logline() { [ -n "${PIXELVR_INSTALL_LOGGING:-}" ] || printf '%s\n' "$*" >> "$LOG_FILE" 2>/dev/null || true; }

REPO="Pixelplanet/Pixel-VR-Player"
ASSET="pixelvr-linux-arm64.tar.gz"
APP_NAME="Pixel VR Player"

INSTALL_DIR="${PIXELVR_INSTALL_DIR:-$HOME/PixelVRPlayer}"
VERSION=""
DO_STEAM=1
RESTART_STEAM=0
UNINSTALL=0
DO_UPDATE=0
FORCE=0

if [ -t 1 ]; then
    C_INFO=$(printf '\0033[36m')
    C_OK=$(printf '\0033[32m')
    C_WARN=$(printf '\0033[33m')
    C_ERR=$(printf '\0033[31m')
    C_OFF=$(printf '\0033[0m')
else
    C_INFO=""; C_OK=""; C_WARN=""; C_ERR=""; C_OFF=""
fi

info() { printf '%s==>%s %s\n' "$C_INFO" "$C_OFF" "$*"; logline "==> $*"; }
ok()   { printf '%s  ok%s %s\n' "$C_OK" "$C_OFF" "$*"; logline "  ok $*"; }
warn() { printf '%swarn%s %s\n' "$C_WARN" "$C_OFF" "$*" >&2; logline "warn $*"; }
die()  { printf '%serror%s %s\n' "$C_ERR" "$C_OFF" "$*" >&2; logline "error $*"; exit 1; }

# A terminal to ask in (opening /dev/tty fails without one, e.g. ssh -T).
has_tty() { ( exec < /dev/tty > /dev/tty ) 2>/dev/null; }

usage() {
    cat <<'USAGE'
Pixel VR Player installer for the Valve Steam Frame.

Usage:
  curl -fsSL https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player/main/tools/install.sh | sh
  ./install.sh [options]

Options:
  --dir DIR           install directory (default: ~/PixelVRPlayer)
  --version vX.Y.Z    release to install (default: the latest release)
  --update            force fetching the latest release from GitHub
  --no-steam          do not add the Non-Steam Game shortcut
  --restart-steam     close Steam before registering, then start it again
  --uninstall         remove the app and its Steam shortcut
  --force             install even if the CPU is not aarch64
  -h, --help          show this help

Settings in ~/.config/pixelvr and cached thumbnails in ~/.cache/pixelvr are
never touched by install, upgrade or uninstall.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dir)
            [ $# -ge 2 ] || die "--dir needs a value"
            case "$2" in
                "~/"*) INSTALL_DIR="$HOME/${2#\~/}" ;;
                *) INSTALL_DIR="$2" ;;
            esac
            shift ;;
        --dir=*) INSTALL_DIR="${1#--dir=}" ; case "$INSTALL_DIR" in "~/"*) INSTALL_DIR="$HOME/${INSTALL_DIR#\~/}" ;; esac ;;
        --version)
            [ $# -ge 2 ] || die "--version needs a value"
            VERSION="$2"; shift ;;
        --version=*) VERSION="${1#--version=}" ;;
        --update) DO_UPDATE=1 ;;
        --no-steam) DO_STEAM=0 ;;
        --restart-steam) RESTART_STEAM=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --force) FORCE=1 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

case "$INSTALL_DIR" in
    ""|"/"|".") die "refusing to use '$INSTALL_DIR' as install directory" ;;
esac

# ---------------------------------------------------------------------------
# Steam shortcut registration (non-Steam game in the library)
# ---------------------------------------------------------------------------
steam_running() {
    command -v pgrep >/dev/null 2>&1 && pgrep -x steam >/dev/null 2>&1
}

find_steam_userdata() {
    steam_root=""
    for candidate in "$HOME/.steam/steam" "$HOME/.local/share/Steam" "$HOME/.steam/root"; do
        if [ -d "$candidate/userdata" ]; then
            steam_root="$candidate"
            break
        fi
    done
    [ -n "$steam_root" ] || return 1

    vdf=""
    newest=0
    for dir in "$steam_root"/userdata/*; do
        case "${dir##*/}" in ''|*[!0-9]*) continue ;; esac
        file="$dir/config/shortcuts.vdf"
        [ -f "$file" ] || continue
        stamp=$(stat -c %Y "$file" 2>/dev/null || stat -f %m "$file" 2>/dev/null || echo 0)
        if [ -z "$vdf" ] || [ "$stamp" -ge "$newest" ]; then
            vdf="$file"
            newest="$stamp"
        fi
    done
    if [ -z "$vdf" ]; then
        # No shortcuts.vdf yet: create it for the most recently used account.
        for dir in "$steam_root"/userdata/*; do
            case "${dir##*/}" in ''|*[!0-9]*) continue ;; esac
            [ -d "$dir/config" ] || continue
            stamp=$(stat -c %Y "$dir/config" 2>/dev/null || stat -f %m "$dir/config" 2>/dev/null || echo 0)
            if [ -z "$vdf" ] || [ "$stamp" -ge "$newest" ]; then
                vdf="$dir/config/shortcuts.vdf"
                newest="$stamp"
            fi
        done
    fi
    [ -n "$vdf" ] || return 1
    printf '%s\n' "$vdf"
}

# Binary KeyValues editor for shortcuts.vdf. Arguments:
#   <add|remove> <vdf path> <app name> <exe> <start dir>
run_vdf_helper() {
    python3 - "$@" <<'PYEOF'
import os
import struct
import sys
import zlib

OBJ, STR, INT, U64, END = 0x00, 0x01, 0x02, 0x07, 0x08


class Unsupported(Exception):
    pass


def read_cstr(buf, pos):
    end = buf.find(b"\0", pos)
    if end < 0:
        raise ValueError("unterminated string")
    return buf[pos:end].decode("utf-8", "surrogateescape"), end + 1


def parse_obj(buf, pos):
    entries = {}
    while True:
        if pos >= len(buf):
            raise ValueError("truncated object")
        kind = buf[pos]
        if kind in (END, 0x0B):
            return entries, pos + 1
        key, pos = read_cstr(buf, pos + 1)
        if kind == OBJ:
            value, pos = parse_obj(buf, pos)
        elif kind == STR:
            value, pos = read_cstr(buf, pos)
        elif kind == INT:
            value = struct.unpack_from("<i", buf, pos)[0]
            pos += 4
        elif kind == U64:
            value = struct.unpack_from("<Q", buf, pos)[0]
            pos += 8
        else:
            raise Unsupported("unsupported type 0x%02x at key %r" % (kind, key))
        entries[key] = [kind, value]
    return entries, pos


def parse_file(buf):
    if not buf:
        return {"shortcuts": [OBJ, {}]}
    if buf[0] != OBJ:
        raise Unsupported("not a binary KeyValues file")
    key, pos = read_cstr(buf, 1)
    value, _ = parse_obj(buf, pos)
    return {key: [OBJ, value]}


def dump(entries):
    out = bytearray()
    for key, (kind, value) in entries.items():
        kb = key.encode("utf-8", "surrogateescape")
        if kind == OBJ:
            out += bytes([OBJ]) + kb + b"\0" + dump(value) + bytes([END])
        elif kind == STR:
            out += bytes([STR]) + kb + b"\0" + value.encode("utf-8", "surrogateescape") + b"\0"
        elif kind == INT:
            out += bytes([INT]) + kb + b"\0" + struct.pack("<i", value)
        elif kind == U64:
            out += bytes([U64]) + kb + b"\0" + struct.pack("<Q", value)
    return bytes(out)


def app_id(exe, name):
    # Steam derives the shortcut id from the quoted exe path plus the name.
    return (zlib.crc32((exe + name).encode("utf-8")) | 0x80000000) & 0xFFFFFFFF


def to_i32(value):
    return value - 0x100000000 if value >= 0x80000000 else value


def find(shortcuts, name, exe):
    for key in shortcuts:
        kind, entry = shortcuts[key]
        if kind != OBJ:
            continue
        stored_exe = entry.get("Exe", [STR, ""])[1].strip('"')
        stored_name = entry.get("AppName", [STR, ""])[1]
        devkit_id = entry.get("DevkitGameID", [STR, ""])[1]
        if (exe and stored_exe == exe) or stored_name == name or stored_name in ("Devkit Game: PixelVRPlayer", "Devkit Game: Pixel VR Player") or devkit_id == "PixelVRPlayer":
            return key, entry
    return None, None


def install_art(path, raw_appid, src_dir):
    import shutil
    grid_dir = os.path.join(os.path.dirname(path), "grid")  # userdata/<id>/config/grid
    os.makedirs(grid_dir, exist_ok=True)
    icon = os.path.join(src_dir, "icon.png")
    names = (("icon.png", "%d_icon.png"), ("capsule.png", "%dp.png"), ("header.png", "%d.png"),
             ("hero.png", "%d_hero.png"), ("logo.png", "%d_logo.png"))
    for asset, target in names:
        source = os.path.join(src_dir, asset)
        if not os.path.isfile(source) and asset in ("capsule.png", "header.png"):
            source = icon  # no dedicated art: fall back to the icon
        if os.path.isfile(source):
            shutil.copyfile(source, os.path.join(grid_dir, target % raw_appid))


def main():
    mode, path, name = sys.argv[1], sys.argv[2], sys.argv[3]
    exe = sys.argv[4] if len(sys.argv) > 4 else ""
    start_dir = sys.argv[5] if len(sys.argv) > 5 else ""

    buf = b""
    if os.path.exists(path):
        with open(path, "rb") as handle:
            buf = handle.read()
    root = parse_file(buf)
    root_key = next((k for k in root if k.lower() == "shortcuts"), None)
    if root_key is None:
        root_key = "shortcuts"
        root[root_key] = [OBJ, {}]
    shortcuts = root[root_key][1]

    if mode == "find":
        # Prints "appid|exe" of the shortcut called <name> (the one whose Exe is <exe> if several).
        best = None
        for k in shortcuts:
            kind, e = shortcuts[k]
            if kind != OBJ or e.get("AppName", [STR, ""])[1] != name:
                continue
            cand = (e.get("appid", [INT, 0])[1] & 0xFFFFFFFF, e.get("Exe", [STR, ""])[1].strip('"'))
            if exe and cand[1] == exe:
                best = cand
                break
            best = best or cand
        if best is None:
            return 1
        print("%d|%s" % best)
        return 0
    if mode == "art":
        install_art(path, int(name), exe)  # name carries the appid, exe the folder with the images
        return 0

    key, entry = find(shortcuts, name, exe)

    if mode == "vrflag":
        # Prints the entry's "Include in VR Library" flag (1 / 0).
        if entry is None:
            return 1
        print(1 if entry.get("OpenVR", [INT, 0])[1] else 0)
        return 0

    if mode == "remove":
        if entry is None:
            print("no matching shortcut, nothing to remove")
            return 0
        raw_appid = entry.get("appid", [INT, 0])[1] & 0xFFFFFFFF
        if raw_appid:
            steam_user_dir = os.path.dirname(os.path.dirname(path))
            grid_dir = os.path.join(steam_user_dir, "config", "grid")
            for suffix in ("", "p", "_hero", "_logo", "_icon"):
                for ext in (".png", ".jpg"):
                    gf = os.path.join(grid_dir, "%d%s%s" % (raw_appid, suffix, ext))
                    if os.path.isfile(gf):
                        try:
                            os.remove(gf)
                        except Exception:
                            pass
        del shortcuts[key]
        renumbered = {}
        ordered = sorted(shortcuts, key=lambda k: int(k) if k.isdigit() else 1 << 30)
        for index, old in enumerate(ordered):
            renumbered[str(index)] = shortcuts[old]
        root[root_key][1] = renumbered
        print("removed '%s' and grid artwork" % name)
    elif mode == "setvr":
        # Only while Steam is closed: it rewrites the file from memory.
        if entry is None:
            return 1
        entry["OpenVR"] = [INT, 1]
        print("turned on 'Include in VR Library' for '%s'" % name)
    else:
        quoted_exe = '"%s"' % exe
        created = entry is None
        if created:
            index = 0
            for existing in shortcuts:
                if existing.isdigit() and int(existing) >= index:
                    index = int(existing) + 1
            entry = {}
            shortcuts[str(index)] = [OBJ, entry]
        if "appid" not in entry:
            entry["appid"] = [INT, to_i32(app_id(quoted_exe, name))]
        icon_path = os.path.join(start_dir, "icon.png") if os.path.isfile(os.path.join(start_dir, "icon.png")) else ""
        for field, value in (
            ("AppName", [STR, name]),
            ("Exe", [STR, quoted_exe]),
            ("StartDir", [STR, '"%s"' % start_dir]),
            ("icon", [STR, icon_path]),
            ("ShortcutPath", [STR, ""]),
            ("LaunchOptions", [STR, ""]),
            ("IsHidden", [INT, 0]),
            ("AllowDesktopConfig", [INT, 1]),
            ("AllowOverlay", [INT, 1]),
            ("OpenVR", [INT, 1]),
            ("Devkit", [INT, 0]),
            ("DevkitGameID", [STR, ""]),
            ("DevkitOverrideAppID", [INT, 0]),
            ("LastPlayTime", [INT, 0]),
            ("FlatpakAppID", [STR, ""]),
        ):
            entry[field] = value
        entry.setdefault("tags", [OBJ, {}])
        if icon_path:
            try:
                install_art(path, entry["appid"][1] & 0xFFFFFFFF, start_dir)
            except Exception:
                pass
        print("%s '%s' (%d shortcut(s) in the library)" % (
            "added" if created else "updated", name, len(shortcuts)))

    data = dump(root) + bytes([END])
    tmp = path + ".tmp"
    with open(tmp, "wb") as handle:
        handle.write(data)
    if os.path.exists(path):
        os.chmod(tmp, os.stat(path).st_mode & 0o777)
    os.replace(tmp, path)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Unsupported as exc:
        sys.stderr.write("cannot edit shortcuts.vdf: %s\n" % exc)
        sys.exit(3)
    except Exception as exc:  # noqa: BLE001 - surface any failure to the shell
        sys.stderr.write("cannot edit shortcuts.vdf: %s\n" % exc)
        sys.exit(1)
PYEOF
}

write_desktop_entry() {
    exec_path="$INSTALL_DIR/run.sh"
    case "$exec_path" in *" "*) exec_path="\"$exec_path\"" ;; esac
    cat > "$INSTALL_DIR/$APP_NAME.desktop" <<DESKTOP
[Desktop Entry]
Name=$APP_NAME
Comment=VR video player for the Valve Steam Frame
Exec=$exec_path
Icon=$INSTALL_DIR/icon.png
Type=Application
Terminal=false
Categories=AudioVideo;Player;
DESKTOP
    chmod 0755 "$INSTALL_DIR/$APP_NAME.desktop"
}

# Fill in the display/session variables a running Steam has, so steam:// calls
# also work from a plain ssh shell.
steam_session_env() {
    steam_pid=$(pgrep -x steam 2>/dev/null | head -n 1)
    [ -n "$steam_pid" ] && [ -r "/proc/$steam_pid/environ" ] || return 0
    for var in DISPLAY XAUTHORITY XDG_RUNTIME_DIR WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS XDG_SESSION_TYPE; do
        eval "cur=\${$var:-}"
        [ -z "$cur" ] || continue
        val=$(tr '\0' '\n' < "/proc/$steam_pid/environ" 2>/dev/null | sed -n "s/^$var=//p" | head -n 1)
        if [ -n "$val" ]; then
            eval "$var=\$val"
            export "$var"
        fi
    done
}

# Steam loads custom artwork for non-Steam shortcuts only when it starts: true when any
# file for <appid> in the grid folder is newer than the running Steam process.
art_needs_restart() {  # <shortcuts.vdf> <appid>
    steam_pid=$(pgrep -x steam 2>/dev/null | head -n 1)
    [ -n "$steam_pid" ] || return 1
    started=$(stat -c %Y "/proc/$steam_pid" 2>/dev/null) || return 1
    newer=$(find "$(dirname "$1")/grid" -name "$2*" -newermt "@$started" 2>/dev/null | head -n 1)
    [ -n "$newer" ]
}

# Without "Include in VR Library" (OpenVR=1) SteamVR does not know the shortcut
# is a VR app: its dashboard stays up on "Resume Game" at every launch. Steam's
# own add-shortcut path leaves the flag off and only takes the change while it
# is closed, so it is set on the next Steam restart (restart_steam_now).
check_vr_flag() {  # <shortcuts.vdf>
    flag=$(run_vdf_helper vrflag "$1" "$APP_NAME" "$INSTALL_DIR/run.sh" 2>/dev/null) || flag=""
    if [ "$flag" = 0 ]; then
        VR_FLAG_OFF=1
        warn "'Include in VR Library' is off for '$APP_NAME' - Steam will show 'Resume Game' at launch until it is on"
    fi
}

# Steam only reads shortcuts.vdf at startup and rewrites it from memory, so
# while it runs the entry must be added through Steam itself (steam://).
# Returns 0 when the entry is in the library, 1 when Steam did not confirm it.
register_via_steam() {  # <shortcuts.vdf>
    vdf="$1"
    found=$(run_vdf_helper find "$vdf" "$APP_NAME" "$INSTALL_DIR/run.sh" 2>/dev/null) || found=""
    if [ -n "$found" ]; then
        found_appid="${found%%|*}"
        found_exe="${found#*|}"
        if [ "$found_exe" = "$INSTALL_DIR/run.sh" ]; then
            ok "'$APP_NAME' is already in the Steam library"
            check_vr_flag "$vdf"
            run_vdf_helper art "$vdf" "$found_appid" "$INSTALL_DIR" >/dev/null 2>&1 || true
            ! art_needs_restart "$vdf" "$found_appid" || ART_NEEDS_RESTART=1
            return 0
        fi
        # An entry that points into the folder this install was unpacked from (e.g. a
        # copy on the Desktop) or at a path that no longer exists is stale; one that
        # points at some other existing install is left alone.
        if [ -e "$found_exe" ] && [ "${found_exe#"$srcdir"/}" = "$found_exe" ]; then
            warn "Steam already lists '$APP_NAME' from $found_exe (another install) - not adding a second entry"
            STEAM_OTHER=1
            return 0
        fi
        warn "Steam has a '$APP_NAME' entry pointing at $found_exe, which is not this install; adding a new one - delete the old one in Steam"
    fi

    write_desktop_entry
    steam_session_env
    if command -v steamos-add-to-steam >/dev/null 2>&1; then
        steamos-add-to-steam "$INSTALL_DIR/$APP_NAME.desktop" >/dev/null 2>&1 || warn "steamos-add-to-steam failed"
    else
        encoded=$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$INSTALL_DIR/$APP_NAME.desktop")
        touch /tmp/addnonsteamgamefile
        steam "steam://addnonsteamgame/$encoded" >/dev/null 2>&1 || warn "could not reach Steam"
    fi

    waited=0
    while [ "$waited" -lt 15 ]; do
        found=$(run_vdf_helper find "$vdf" "$APP_NAME" "$INSTALL_DIR/run.sh" 2>/dev/null) || found=""
        if [ -n "$found" ] && [ "${found#*|}" = "$INSTALL_DIR/run.sh" ]; then
            run_vdf_helper art "$vdf" "${found%%|*}" "$INSTALL_DIR" >/dev/null 2>&1 || warn "could not install the library artwork"
            ! art_needs_restart "$vdf" "${found%%|*}" || ART_NEEDS_RESTART=1
            ok "Steam added '$APP_NAME' to the library (appid ${found%%|*})"
            check_vr_flag "$vdf"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

register_steam_shortcut() {
    vdf=$(find_steam_userdata) || {
        warn "Steam userdata not found - is Steam installed and logged in at least once?"
        return 1
    }
    if ! command -v python3 >/dev/null 2>&1; then
        warn "python3 not found - skipping Steam registration"
        return 1
    fi

    steam_was_running=0
    if steam_running && [ "$RESTART_STEAM" -eq 0 ]; then
        if register_via_steam "$vdf"; then
            STEAM_REGISTERED=1
            return 0
        fi
        STEAM_PENDING=1
        warn "Steam did not confirm the new library entry - restart Steam to finish adding '$APP_NAME'"
        return 1
    fi
    if steam_running; then
        steam_was_running=1
        if [ "$RESTART_STEAM" -eq 1 ]; then
            info "Closing Steam so the new shortcut is kept..."
            steam -shutdown >/dev/null 2>&1 || true
            waited=0
            while steam_running && [ "$waited" -lt 30 ]; do
                sleep 1
                waited=$((waited + 1))
            done
            if steam_running; then
                warn "Steam did not close in time"
            fi
        fi
    fi

    if [ -f "$vdf" ]; then
        backup="$vdf.bak.$(date +%Y%m%d%H%M%S)"
        cp "$vdf" "$backup"
    else
        backup=""
    fi

    write_desktop_entry
    if ! run_vdf_helper add "$vdf" "$APP_NAME" "$INSTALL_DIR/run.sh" "$INSTALL_DIR"; then
        [ -n "$backup" ] && [ -f "$backup" ] && cp "$backup" "$vdf"
        return 1
    fi
    [ -n "$backup" ] && ok "shortcuts.vdf backed up to ${backup}"

    if [ "$RESTART_STEAM" -eq 1 ] && [ "$steam_was_running" -eq 1 ] && ! steam_running; then
        info "Starting Steam again..."
        if command -v steam >/dev/null 2>&1; then
            nohup steam >/dev/null 2>&1 &
        fi
    fi
    STEAM_REGISTERED=1
}

unregister_steam_shortcut() {
    vdf=$(find_steam_userdata) || return 1
    [ -f "$vdf" ] || return 1
    command -v python3 >/dev/null 2>&1 || return 1
    run_vdf_helper remove "$vdf" "$APP_NAME" "" "" || return 1
}

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------
if [ "$UNINSTALL" -eq 1 ]; then
    info "Removing Pixel VR Player from $INSTALL_DIR"
    # The vault: encrypted files can only be opened with ~/.config/pixelvr/vault.key and the unlock
    # combination. Offer to decrypt it into a normal folder first (only with a terminal to answer in).
    VAULT_KEPT=0
    VAULT_DIR="$HOME/.pixelvr_vault"
    if [ -x "$INSTALL_DIR/pixelvr" ]; then
        vinfo=$(timeout 60 "$INSTALL_DIR/pixelvr" --vault-info 2>/dev/null || true)
        vget() { printf '%s\n' "$vinfo" | sed -n "s/^$1=//p" | head -n 1; }
        vfiles=$(vget files); venc=$(vget encrypted); vbytes=$(vget bytes); vloc=$(vget location); vavail=$(vget available)
        [ -n "$(vget dir)" ] && VAULT_DIR=$(vget dir)
        case "$vfiles" in ''|*[!0-9]*) vfiles=0 ;; esac
        case "$venc" in ''|*[!0-9]*) venc=0 ;; esac
        if [ "$vloc" = "sdcard" ] && [ "$vavail" != "1" ]; then
            warn "Your vault is on an SD card that is not inserted: it stays encrypted there."
            VAULT_KEPT=1
        elif [ "$vfiles" -gt 0 ]; then
            vgb=$(awk "BEGIN { printf \"%.1f\", ${vbytes:-0} / 1e9 }")
            warn "Your vault holds $vfiles file(s), $vgb GB ($venc encrypted)."
            export_dir="$HOME/PixelVR Vault"
            choice=k
            if has_tty; then
                printf '  K  keep it encrypted (install the player again later to open it)\n' > /dev/tty
                printf '  D  decrypt it into "%s" now\n' "$export_dir" > /dev/tty
                printf 'Keep or decrypt? [K/d] ' > /dev/tty
                read -r choice < /dev/tty || choice=k
            else
                info "No terminal to ask in: the vault stays encrypted."
            fi
            case "$choice" in
                d|D)
                    info "Decrypting the vault into $export_dir ..."
                    if "$INSTALL_DIR/pixelvr" --vault-export "$export_dir"; then
                        ok "vault decrypted into $export_dir"
                    else
                        warn "not everything could be decrypted: the rest stays in the vault"
                        VAULT_KEPT=1
                    fi
                    ;;
                *) VAULT_KEPT=1 ;;
            esac
        fi
    elif [ -d "$VAULT_DIR" ] && [ -n "$(ls -A "$VAULT_DIR" 2>/dev/null)" ]; then
        VAULT_KEPT=1
    fi
    # Take the player out of SteamVR's app list (it registers itself on launch).
    if [ -x "$INSTALL_DIR/pixelvr" ]; then
        timeout 20 "$INSTALL_DIR/pixelvr" --steamvr-unregister 2>/dev/null || true
    fi
    rm -f "$HOME/.config/pixelvr/pixelvr.vrmanifest" "$HOME/.config/pixelvr/vr_manifest_registered"
    if [ "$DO_STEAM" -eq 1 ]; then
        # Steam rewrites shortcuts.vdf from memory, so the entry can only be
        # removed while it is closed (a Gaming Mode session starts it again).
        steam_was_running=0
        if steam_running; then
            steam_was_running=1
            info "Closing Steam to remove the shortcut..."
            steam_session_env
            steam -shutdown >/dev/null 2>&1 || true
            waited=0
            while steam_running && [ "$waited" -lt 150 ]; do
                sleep 0.2
                waited=$((waited + 1))
            done
        fi
        if steam_running; then
            warn "Steam did not close - delete the shortcut in Steam manually"
        elif unregister_steam_shortcut; then
            ok "removed the Steam shortcut"
        else
            warn "could not remove the Steam shortcut; delete it in Steam manually"
        fi
        if [ "$steam_was_running" -eq 1 ]; then
            waited=0
            while ! steam_running && [ "$waited" -lt 5 ]; do
                sleep 1
                waited=$((waited + 1))
            done
            if ! steam_running && command -v steam >/dev/null 2>&1; then
                info "Starting Steam again..."
                nohup steam >/dev/null 2>&1 &
            fi
        fi
    fi
    keep_scripts=0
    keep_combo=0
    if [ -d "$INSTALL_DIR/Funscripts" ] && [ -n "$(ls -A "$INSTALL_DIR/Funscripts" 2>/dev/null)" ]; then
        keep_scripts=1
    fi
    # While the vault stays, the written-down combination stays too (logs/session.dat).
    if [ "$VAULT_KEPT" -eq 1 ] && [ -f "$INSTALL_DIR/logs/session.dat" ]; then
        keep_combo=1
    fi
    if [ "$keep_scripts" -eq 1 ] || [ "$keep_combo" -eq 1 ]; then
        find "$INSTALL_DIR" -mindepth 1 -maxdepth 1 ! -name Funscripts ! -name logs -exec rm -rf {} +
        [ "$keep_scripts" -eq 1 ] || rm -rf "$INSTALL_DIR/Funscripts"
        if [ "$keep_combo" -eq 1 ]; then
            find "$INSTALL_DIR/logs" -mindepth 1 -maxdepth 1 ! -name session.dat -exec rm -rf {} +
        else
            rm -rf "$INSTALL_DIR/logs"
        fi
        kept=""
        [ "$keep_scripts" -eq 1 ] && kept="your Funscripts folder"
        [ "$keep_combo" -eq 1 ] && kept="${kept:+$kept and }logs/session.dat (your vault combination)"
        ok "removed $INSTALL_DIR (kept $kept)"
    else
        rm -rf "$INSTALL_DIR"
        ok "removed $INSTALL_DIR"
    fi
    rm -f "$HOME/Desktop/Update Pixel VR Player.desktop"
    printf '\nSettings (%s/.config/pixelvr) and cached thumbnails were kept.\n' "$HOME"
    if [ "$VAULT_KEPT" -eq 1 ]; then
        printf 'Your vault stays encrypted in %s.\n' "$VAULT_DIR"
        printf 'Only %s/.config/pixelvr/vault.key and your unlock combination open it again\n' "$HOME"
        if [ "$keep_combo" -eq 1 ]; then
            printf '(the combination is written down in %s/logs/session.dat).\n' "$INSTALL_DIR"
        fi
        printf 'Keep ~/.config/pixelvr as long as you want the vault. Thumbnails: rm -rf ~/.cache/pixelvr\n'
    else
        printf 'Delete them too with: rm -rf ~/.config/pixelvr ~/.cache/pixelvr\n'
    fi
    exit 0
fi

# ---------------------------------------------------------------------------
# Platform checks
# ---------------------------------------------------------------------------
machine=$(uname -m)
case "$machine" in
    aarch64|arm64) ok "CPU: $machine" ;;
    *)
        if [ "$FORCE" -eq 0 ]; then
            die "this release is built for the Steam Frame (aarch64), not '$machine'.
       Build from source for other machines, or pass --force."
        fi
        warn "installing an aarch64 build on '$machine' (--force)" ;;
esac

# ---------------------------------------------------------------------------
# Fetch (reused by the local and download paths)
# ---------------------------------------------------------------------------
fetch() {  # <url> <dest>
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$2" "$1"
    else
        die "need curl or wget to download the release"
    fi
}

script_dir=""
if [ -f "$0" ]; then
    script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd) || script_dir=""
fi

tmp=""
cleanup() {
    [ -z "$tmp" ] || rm -rf "$tmp"
    # Handed over from an older installer (below): its download folder.
    [ -z "${PIXELVR_HANDOFF_TMP:-}" ] || rm -rf "$PIXELVR_HANDOFF_TMP"
}
trap cleanup EXIT INT TERM

if [ "$DO_UPDATE" -eq 0 ] && [ -n "$script_dir" ] && [ -f "$script_dir/pixelvr" ] && [ -d "$script_dir/shaders" ]; then
    info "Installing from $script_dir"
    srcdir="$script_dir"
    if [ -f "$script_dir/VERSION" ]; then
        VERSION="$(cat "$script_dir/VERSION")"
    fi
    if [ -n "$VERSION" ]; then
        version_label="$VERSION"
    else
        version_label="local build"
    fi
else
    case "$VERSION" in
        "") base="https://github.com/$REPO/releases/latest/download"
            version_label="latest" ;;
        v*) base="https://github.com/$REPO/releases/download/$VERSION"
            version_label="$VERSION" ;;
        *)  VERSION="v$VERSION"
            base="https://github.com/$REPO/releases/download/$VERSION"
            version_label="$VERSION" ;;
    esac

    tmp=$(mktemp -d) || die "cannot create a temporary directory"
    info "Downloading $version_label for aarch64"
    fetch "$base/$ASSET" "$tmp/$ASSET" || die "download failed: $base/$ASSET"

    if fetch "$base/$ASSET.sha256" "$tmp/$ASSET.sha256" 2>/dev/null; then
        expected=$(awk 'NR == 1 { print $1 }' "$tmp/$ASSET.sha256")
        if command -v sha256sum >/dev/null 2>&1; then
            actual=$(sha256sum "$tmp/$ASSET" | awk '{ print $1 }')
        elif command -v shasum >/dev/null 2>&1; then
            actual=$(shasum -a 256 "$tmp/$ASSET" | awk '{ print $1 }')
        else
            actual=""
        fi
        if [ -n "$actual" ]; then
            [ "$actual" = "$expected" ] || die "checksum mismatch for $ASSET"
            ok "checksum verified"
        fi
    else
        warn "no checksum published for this release"
    fi

    mkdir -p "$tmp/unpacked"
    tar -xzf "$tmp/$ASSET" -C "$tmp/unpacked" || die "cannot unpack $ASSET"
    srcdir="$tmp/unpacked/PixelVRPlayer"
    [ -x "$srcdir/pixelvr" ] || [ -f "$srcdir/pixelvr" ] || die "unexpected archive layout"

    # An update runs the installed copy of this script, which can be older than
    # the release: let the release's own installer do the rest, so its newer
    # steps (e.g. the Steam VR flag) apply on this update already.
    if [ -z "${PIXELVR_HANDOFF:-}" ] && [ -f "$srcdir/install.sh" ] && ! cmp -s "$0" "$srcdir/install.sh" 2>/dev/null; then
        info "Continuing with the installer from $version_label"
        set -- --dir "$INSTALL_DIR"
        [ "$DO_STEAM" -eq 1 ] || set -- "$@" --no-steam
        [ "$RESTART_STEAM" -eq 0 ] || set -- "$@" --restart-steam
        [ "$FORCE" -eq 0 ] || set -- "$@" --force
        trap - EXIT INT TERM
        PIXELVR_HANDOFF=1 PIXELVR_HANDOFF_TMP="$tmp" exec sh "$srcdir/install.sh" "$@"
    fi
fi

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
info "Installing to $INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
SAME_DIR=0
if [ "$(cd "$srcdir" && pwd -P)" = "$(cd "$INSTALL_DIR" && pwd -P)" ]; then
    SAME_DIR=1
fi
INSTALLED_SCRIPT=0
if [ "$SAME_DIR" -eq 1 ]; then
    info "the files are already in $INSTALL_DIR"
    [ -f "$INSTALL_DIR/install.sh" ] && INSTALLED_SCRIPT=1
else
mkdir -p "$INSTALL_DIR/shaders"
rm -f "$INSTALL_DIR/pixelvr"
cp "$srcdir/pixelvr" "$INSTALL_DIR/pixelvr"
chmod 0755 "$INSTALL_DIR/pixelvr"
rm -f "$INSTALL_DIR/shaders/"*.spv
cp "$srcdir/shaders/"*.spv "$INSTALL_DIR/shaders/"
# Bundled UI fonts (optional: the app falls back to system fonts).
if [ -d "$srcdir/fonts" ]; then
    mkdir -p "$INSTALL_DIR/fonts"
    cp "$srcdir/fonts/"* "$INSTALL_DIR/fonts/"
fi
# Scripts are replaced via a new file + rename, never overwritten in place: an update runs
# the installed install.sh/update.sh, and the shell keeps reading the running script from
# its old byte offset, so overwriting it mid-run executes garbage ("syntax error near '<'").
put_script() {
    cp "$1" "$2.new.$$" && chmod 0755 "$2.new.$$" && mv -f "$2.new.$$" "$2"
}
put_script "$srcdir/run.sh" "$INSTALL_DIR/run.sh"
if [ -f "$srcdir/install.sh" ]; then
    put_script "$srcdir/install.sh" "$INSTALL_DIR/install.sh"
    INSTALLED_SCRIPT=1
elif [ -n "$script_dir" ] && [ -f "$script_dir/install.sh" ]; then
    put_script "$script_dir/install.sh" "$INSTALL_DIR/install.sh"
    INSTALLED_SCRIPT=1
fi
# Copy updater script
if [ -f "$srcdir/update.sh" ]; then
    put_script "$srcdir/update.sh" "$INSTALL_DIR/update.sh"
elif [ -n "$script_dir" ] && [ -f "$script_dir/update.sh" ]; then
    put_script "$script_dir/update.sh" "$INSTALL_DIR/update.sh"
elif [ -f "tools/update.sh" ]; then
    put_script "tools/update.sh" "$INSTALL_DIR/update.sh"
fi

if [ -f "$srcdir/VERSION" ]; then
    cp "$srcdir/VERSION" "$INSTALL_DIR/VERSION"
elif [ -n "$VERSION" ]; then
    printf '%s\n' "$VERSION" > "$INSTALL_DIR/VERSION"
fi
for asset in icon.png hero.png capsule.png header.png logo.png; do
    if [ -f "$srcdir/$asset" ]; then
        cp "$srcdir/$asset" "$INSTALL_DIR/$asset"
    elif [ -n "$script_dir" ] && [ -f "$script_dir/$asset" ]; then
        cp "$script_dir/$asset" "$INSTALL_DIR/$asset"
    elif [ -f "assets/$asset" ]; then
        cp "assets/$asset" "$INSTALL_DIR/$asset"
    fi
done
fi
write_update_icon() {
    [ -d "$HOME/Desktop" ] || return 0
    cat > "$HOME/Desktop/Update Pixel VR Player.desktop" <<'UPDATEICON'
[Desktop Entry]
Name=Update Pixel VR Player
Comment=Update Pixel VR Player to the latest version on Valve Steam Frame
Exec=bash -c 'export PIXELVR_GUI=1; if [ -f "$HOME/PixelVRPlayer/update.sh" ]; then bash "$HOME/PixelVRPlayer/update.sh"; else curl -fsSL https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player/main/tools/install.sh | bash -s -- --update; fi; rc=$?; if [ $rc -ne 0 ]; then command -v kdialog >/dev/null && kdialog --title "Pixel VR Player" --detailederror "Update failed. Details:" "$(tail -n 15 "$HOME/PixelVRPlayer/logs/install.log" 2>/dev/null)"; fi'
Icon=steam
Terminal=true
Type=Application
Categories=AudioVideo;Player;
UPDATEICON
    chmod 0755 "$HOME/Desktop/Update Pixel VR Player.desktop"
    ok "Installed the update icon on the Desktop"
}
write_update_icon
ok "$(ls -1 "$INSTALL_DIR/shaders" | wc -l | tr -d ' ') shaders, $(du -h "$INSTALL_DIR/pixelvr" 2>/dev/null | cut -f1) binary"

missing_libs=""
if command -v ldd >/dev/null 2>&1; then
    missing_libs=$(ldd "$INSTALL_DIR/pixelvr" 2>/dev/null | awk '/not found/ { print $1 }' \
        | sort -u | tr '\n' ' ' || true)
fi
if [ -n "$missing_libs" ]; then
    warn "missing shared libraries: $missing_libs"
    warn "if these are libav* libraries, the system FFmpeg is too old"
fi

if [ ! -f /opt/steamvr/steamxr_linuxarm64.json ]; then
    warn "SteamVR not found - install it from Steam before launching"
fi

# ---------------------------------------------------------------------------
# Steam library entry
# ---------------------------------------------------------------------------
STEAM_REGISTERED=0
STEAM_PENDING=0
STEAM_OTHER=0
ART_NEEDS_RESTART=0
VR_FLAG_OFF=0
if [ "$DO_STEAM" -eq 1 ]; then
    info "Adding '$APP_NAME' to the Steam library"
    if register_steam_shortcut; then
        ok "Steam library entry ready"
    elif [ "$STEAM_PENDING" -eq 0 ]; then
        warn "add $INSTALL_DIR/run.sh as a Non-Steam Game manually"
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n%s%s installed%s\n' "$C_OK" "$APP_NAME" "$C_OFF"
printf '  Version     : %s\n' "$version_label"
printf '  Install dir : %s\n' "$INSTALL_DIR"
if [ "$DO_STEAM" -eq 1 ] && [ "$STEAM_REGISTERED" -eq 1 ]; then
    printf '  Steam entry : Non-Steam Game "%s"\n' "$APP_NAME"
fi
printf '\n  Launch now  : %s/run.sh\n' "$INSTALL_DIR"
printf '  From Steam  : Library > Non-Steam > %s\n' "$APP_NAME"
printf '  Play a clip : %s/run.sh /path/to/clip.mp4\n' "$INSTALL_DIR"
printf '  Logs        : %s/logs\n' "$INSTALL_DIR"
printf '  1-Click Upd : %s/update.sh (or "Update Pixel VR Player" icon on Desktop)\n' "$INSTALL_DIR"
if [ "$INSTALLED_SCRIPT" -eq 1 ]; then
    printf '  Remove      : %s/install.sh --uninstall\n\n' "$INSTALL_DIR"
else
    printf '  Remove      : rerun this script with --uninstall\n\n'
fi
# One line for the desktop icons' message box (they show the text after "RESULT: ").
if [ "$DO_STEAM" -eq 0 ]; then
    result="Pixel VR Player $version_label is installed in $INSTALL_DIR."
elif [ "$STEAM_OTHER" -eq 1 ]; then
    result="Pixel VR Player $version_label is installed in $INSTALL_DIR.<br><br>Steam already lists Pixel VR Player from another folder, so no second entry was added. Delete that entry in Steam and run the installer again to use this copy."
elif [ "$STEAM_REGISTERED" -eq 1 ]; then
    result="Pixel VR Player $version_label is installed and in your Steam library (Non-Steam games).<br><br>Switch to Gaming Mode to start it."
elif [ "$STEAM_PENDING" -eq 1 ]; then
    result="Pixel VR Player $version_label is installed, but Steam has not confirmed the library entry yet.<br><br>Restart Steam (or the headset) to finish, then look under Non-Steam games."
else
    result="Pixel VR Player $version_label is installed, but it could not be added to Steam automatically.<br><br>Add $INSTALL_DIR/run.sh as a Non-Steam Game."
fi
printf 'RESULT: %s\n' "$result"
if [ "$ART_NEEDS_RESTART" -eq 1 ] || [ "$VR_FLAG_OFF" -eq 1 ]; then
    printf 'RESTART_STEAM: 1\n'
    [ "$ART_NEEDS_RESTART" -eq 0 ] || info "Steam shows the new icon and artwork after it restarts"
fi
if [ "$VR_FLAG_OFF" -eq 1 ] && [ "${PIXELVR_GUI:-}" != 1 ] && ! has_tty; then
    info "To open straight into the player: in Steam, Properties of '$APP_NAME' > turn on 'Include in VR Library'"
    info "(or run this installer again with --restart-steam, which sets it while Steam is closed)"
fi

restart_steam_now() {
    info "Restarting Steam..."
    steam -shutdown >/dev/null 2>&1 || true
    waited=0
    while steam_running && [ "$waited" -lt 30 ]; do
        sleep 1
        waited=$((waited + 1))
    done
    # Now that Steam has written its shortcuts, mark ours as a VR app (before
    # a Gaming Mode session starts Steam again).
    if [ "$VR_FLAG_OFF" -eq 1 ] && ! steam_running; then
        vr_vdf=$(find_steam_userdata) && run_vdf_helper setvr "$vr_vdf" "$APP_NAME" "$INSTALL_DIR/run.sh" || \
            warn "could not turn on 'Include in VR Library' - turn it on in the shortcut's Steam properties"
    fi
    # In a Gaming Mode session the session brings Steam back by itself.
    waited=0
    while ! steam_running && [ "$waited" -lt 5 ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if ! steam_running && command -v steam >/dev/null 2>&1; then
        nohup steam >/dev/null 2>&1 &
    fi
}

# From a terminal (also "curl ... | sh": the answer is read from the terminal,
# not the pipe): offer the Steam restart there.
if [ "${PIXELVR_GUI:-}" != 1 ] && { [ "$ART_NEEDS_RESTART" -eq 1 ] || [ "$VR_FLAG_OFF" -eq 1 ]; } && \
   has_tty; then
    if [ "$VR_FLAG_OFF" -eq 1 ]; then
        why="so Pixel VR Player opens straight into the player (marks it as a VR app)"
    else
        why="so its icon and artwork appear"
    fi
    printf '\nRestart Steam now %s? It takes a few seconds. [y/N] ' "$why" > /dev/tty
    answer=""
    read -r answer < /dev/tty || answer=""
    case "$answer" in
        y|Y|yes|Yes|YES) restart_steam_now ;;
        *) info "Not restarting Steam - restart it (or the headset) later to finish" ;;
    esac
fi

# Started from the desktop icons (they set PIXELVR_GUI=1): the final message box.
if [ "${PIXELVR_GUI:-}" = 1 ] && command -v kdialog >/dev/null 2>&1; then
    if [ "$ART_NEEDS_RESTART" -eq 1 ] || [ "$VR_FLAG_OFF" -eq 1 ]; then
        why="so its icon and artwork appear"
        [ "$VR_FLAG_OFF" -eq 0 ] || why="so it opens straight into the player (marks it as a VR app)"
        if kdialog --title "Pixel VR Player" --yesno "$result<br><br>Restart Steam now $why? (Takes a few seconds; you can also just reboot later.)"; then
            restart_steam_now
        fi
    else
        kdialog --title "Pixel VR Player" --msgbox "$result" || true
    fi
fi
