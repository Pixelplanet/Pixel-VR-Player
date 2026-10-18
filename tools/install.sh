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

REPO="Pixelplanet/Pixel-VR-Player"
ASSET="pixelvr-linux-arm64.tar.gz"
APP_NAME="Pixel VR Player"

INSTALL_DIR="${PIXELVR_INSTALL_DIR:-$HOME/PixelVRPlayer}"
VERSION=""
DO_STEAM=1
RESTART_STEAM=0
UNINSTALL=0
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

info() { printf '%s==>%s %s\n' "$C_INFO" "$C_OFF" "$*"; }
ok()   { printf '%s  ok%s %s\n' "$C_OK" "$C_OFF" "$*"; }
warn() { printf '%swarn%s %s\n' "$C_WARN" "$C_OFF" "$*" >&2; }
die()  { printf '%serror%s %s\n' "$C_ERR" "$C_OFF" "$*" >&2; exit 1; }

usage() {
    cat <<'USAGE'
Pixel VR Player installer for the Valve Steam Frame.

Usage:
  curl -fsSL https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player/main/tools/install.sh | sh
  ./install.sh [options]

Options:
  --dir DIR           install directory (default: ~/PixelVRPlayer)
  --version vX.Y.Z    release to install (default: the latest release)
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

    key, entry = find(shortcuts, name, exe)

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
        if icon_path and os.path.isfile(icon_path):
            try:
                import shutil
                steam_user_dir = os.path.dirname(os.path.dirname(path))
                grid_dir = os.path.join(steam_user_dir, "config", "grid")
                if icon_path and os.path.isfile(icon_path):
                    shutil.copyfile(icon_path, os.path.join(grid_dir, "%d_icon.png" % raw_appid))
                capsule_path = os.path.join(os.path.dirname(icon_path), "capsule.png") if icon_path else ""
                if capsule_path and os.path.isfile(capsule_path):
                    shutil.copyfile(capsule_path, os.path.join(grid_dir, "%dp.png" % raw_appid))
                elif icon_path and os.path.isfile(icon_path):
                    shutil.copyfile(icon_path, os.path.join(grid_dir, "%dp.png" % raw_appid))
                header_path = os.path.join(os.path.dirname(icon_path), "header.png") if icon_path else ""
                if header_path and os.path.isfile(header_path):
                    shutil.copyfile(header_path, os.path.join(grid_dir, "%d.png" % raw_appid))
                elif icon_path and os.path.isfile(icon_path):
                    shutil.copyfile(icon_path, os.path.join(grid_dir, "%d.png" % raw_appid))
                hero_path = os.path.join(os.path.dirname(icon_path), "hero.png") if icon_path else ""
                if hero_path and os.path.isfile(hero_path):
                    shutil.copyfile(hero_path, os.path.join(grid_dir, "%d_hero.png" % raw_appid))
                logo_path = os.path.join(os.path.dirname(icon_path), "logo.png") if icon_path else ""
                if logo_path and os.path.isfile(logo_path):
                    shutil.copyfile(logo_path, os.path.join(grid_dir, "%d_logo.png" % raw_appid))
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
        else
            warn "Steam is running - restart Steam afterwards to see '$APP_NAME' in the library"
        fi
    fi

    if [ -f "$vdf" ]; then
        backup="$vdf.bak.$(date +%Y%m%d%H%M%S)"
        cp "$vdf" "$backup"
    else
        backup=""
    fi

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
    if [ "$DO_STEAM" -eq 1 ]; then
        if unregister_steam_shortcut; then
            ok "removed the Steam shortcut"
        else
            warn "could not remove the Steam shortcut; delete it in Steam manually"
        fi
    fi
    rm -rf "$INSTALL_DIR"
    ok "removed $INSTALL_DIR"
    printf '\nSettings (%s/.config/pixelvr) and cached thumbnails were kept.\n' "$HOME"
    printf 'Delete them too with: rm -rf ~/.config/pixelvr ~/.cache/pixelvr\n'
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
cleanup() { [ -z "$tmp" ] || rm -rf "$tmp"; }
trap cleanup EXIT INT TERM

if [ -n "$script_dir" ] && [ -f "$script_dir/pixelvr" ] && [ -d "$script_dir/shaders" ]; then
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
fi

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
info "Installing to $INSTALL_DIR"
mkdir -p "$INSTALL_DIR/shaders"
rm -f "$INSTALL_DIR/pixelvr"
cp "$srcdir/pixelvr" "$INSTALL_DIR/pixelvr"
chmod 0755 "$INSTALL_DIR/pixelvr"
rm -f "$INSTALL_DIR/shaders/"*.spv
cp "$srcdir/shaders/"*.spv "$INSTALL_DIR/shaders/"
cp "$srcdir/run.sh" "$INSTALL_DIR/run.sh"
chmod 0755 "$INSTALL_DIR/run.sh"
INSTALLED_SCRIPT=0
if [ -f "$srcdir/install.sh" ]; then
    cp "$srcdir/install.sh" "$INSTALL_DIR/install.sh"
    chmod 0755 "$INSTALL_DIR/install.sh"
    INSTALLED_SCRIPT=1
elif [ -n "$script_dir" ] && [ -f "$script_dir/install.sh" ]; then
    cp "$script_dir/install.sh" "$INSTALL_DIR/install.sh"
    chmod 0755 "$INSTALL_DIR/install.sh"
    INSTALLED_SCRIPT=1
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
if [ "$DO_STEAM" -eq 1 ]; then
    info "Adding '$APP_NAME' to the Steam library"
    if register_steam_shortcut; then
        ok "Steam library entry ready"
    else
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
printf '  Logs        : ~/pixelvr-run.log\n'
printf '  Update      : run this script again (settings and cache are kept)\n'
if [ "$INSTALLED_SCRIPT" -eq 1 ]; then
    printf '  Remove      : %s/install.sh --uninstall\n\n' "$INSTALL_DIR"
else
    printf '  Remove      : rerun this script with --uninstall\n\n'
fi
