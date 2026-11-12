#!/bin/sh
# Pixel VR Player uninstaller for the Valve Steam Frame.
#
#   ~/PixelVRPlayer/uninstall.sh
#
# or double-click "Uninstall Pixel VR Player" in the PixelVRPlayer folder (Desktop Mode).
# Removes the app and its Steam shortcut. Settings (~/.config/pixelvr), thumbnails and the
# vault are kept; with files in the vault it asks whether to keep it encrypted or decrypt it.
#
# Options:
#   --dir DIR     the install directory (default: ~/PixelVRPlayer)
#   --no-steam    leave the Non-Steam Game shortcut alone
#   -y, --yes     do not ask "Remove Pixel VR Player?" first
#   -h, --help    show this help
set -eu

REPO="Pixelplanet/Pixel-VR-Player"
# Always the real install, never the folder this script happens to sit in: an
# unpacked release copied to the Desktop has an uninstall.sh next to it too.
INSTALL_DIR="${PIXELVR_INSTALL_DIR:-$HOME/PixelVRPlayer}"
ASSUME_YES=0
NO_STEAM=0

if [ -t 1 ]; then
    C_INFO=$(printf '\033[36m')
    C_OK=$(printf '\033[32m')
    C_ERR=$(printf '\033[31m')
    C_OFF=$(printf '\033[0m')
else
    C_INFO=""; C_OK=""; C_ERR=""; C_OFF=""
fi

usage() { sed -n '2,14s/^# \{0,1\}//p' "$0"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --dir)
            [ $# -ge 2 ] || { printf '%serror%s --dir needs a value\n' "$C_ERR" "$C_OFF" >&2; exit 2; }
            INSTALL_DIR="$2"; shift ;;
        --dir=*) INSTALL_DIR="${1#--dir=}" ;;
        --no-steam) NO_STEAM=1 ;;
        -y|--yes) ASSUME_YES=1 ;;
        -h|--help) usage; exit 0 ;;
        *) printf '%serror%s unknown option: %s (try --help)\n' "$C_ERR" "$C_OFF" "$1" >&2; exit 2 ;;
    esac
    shift
done
case "$INSTALL_DIR" in "~/"*) INSTALL_DIR="$HOME/${INSTALL_DIR#\~/}" ;; esac

has_tty() { ( exec < /dev/tty > /dev/tty ) 2>/dev/null; }
# Started from the folder icon: the terminal window closes with the script, so wait for a key.
finish() {
    if [ -n "${PIXELVR_GUI:-}" ] && has_tty; then
        printf '\nPress Enter to close this window. ' > /dev/tty
        read -r _ < /dev/tty || true
    fi
    exit "$1"
}

# This script sits in the folder it removes: do not stand in it.
cd "$HOME" 2>/dev/null || cd /

if [ "$ASSUME_YES" -eq 0 ] && has_tty; then
    {
        printf '%s==>%s This removes Pixel VR Player (%s) and its Steam shortcut.\n' "$C_INFO" "$C_OFF" "$INSTALL_DIR"
        printf '    Your settings, your Funscripts folder and your vault are kept.\n'
        printf 'Remove Pixel VR Player? [y/N] '
    } > /dev/tty
    answer=n
    read -r answer < /dev/tty || answer=n
    case "$answer" in
        y|Y|yes|Yes|YES) ;;
        *) printf 'Nothing was removed.\n'; finish 0 ;;
    esac
fi

set -- --dir "$INSTALL_DIR" --uninstall
[ "$NO_STEAM" -eq 1 ] && set -- "$@" --no-steam

rc=0
if [ -f "$INSTALL_DIR/install.sh" ]; then
    sh "$INSTALL_DIR/install.sh" "$@" || rc=$?
elif command -v curl >/dev/null 2>&1; then
    # No installer left in the folder: the current one from the project page does the same.
    curl -fsSL "https://raw.githubusercontent.com/$REPO/main/tools/install.sh" | sh -s -- "$@" || rc=$?
else
    printf '%serror%s %s/install.sh is missing and curl is not installed.\n' "$C_ERR" "$C_OFF" "$INSTALL_DIR" >&2
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    printf '\n%s  ok%s Pixel VR Player is uninstalled.\n' "$C_OK" "$C_OFF"
else
    printf '\n%serror%s Uninstalling Pixel VR Player failed.\n' "$C_ERR" "$C_OFF" >&2
fi
finish "$rc"
