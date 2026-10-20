#!/bin/sh
# Pixel VR Player updater for Valve Steam Frame
# Downloads and installs the latest release in-place.
set -eu

REPO="Pixelplanet/Pixel-VR-Player"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd)
INSTALL_DIR="${PIXELVR_INSTALL_DIR:-$HOME/PixelVRPlayer}"

if [ -n "$SCRIPT_DIR" ] && [ -f "$SCRIPT_DIR/pixelvr" ]; then
    INSTALL_DIR="$SCRIPT_DIR"
fi

if [ -t 1 ]; then
    C_INFO=$(printf '\0033[36m')
    C_OK=$(printf '\0033[32m')
    C_ERR=$(printf '\0033[31m')
    C_OFF=$(printf '\0033[0m')
else
    C_INFO=""; C_OK=""; C_ERR=""; C_OFF=""
fi

printf '%s==>%s Checking for Pixel VR Player updates for %s...\n' "$C_INFO" "$C_OFF" "$INSTALL_DIR"

# Execute in-place update using install.sh with --update flag
if [ -f "$INSTALL_DIR/install.sh" ]; then
    "$INSTALL_DIR/install.sh" --dir "$INSTALL_DIR" --update "$@"
else
    curl -fsSL https://raw.githubusercontent.com/"$REPO"/main/tools/install.sh | sh -s -- --dir "$INSTALL_DIR" --update "$@"
fi

if [ $? -eq 0 ]; then
    printf '\n%s  ok%s Pixel VR Player update complete!\n' "$C_OK" "$C_OFF"
    if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
        if command -v notify-send >/dev/null 2>&1; then
            notify-send "Pixel VR Player" "Updated to the latest version successfully!" || true
        fi
    fi
else
    printf '\n%serror%s Pixel VR Player update failed.\n' "$C_ERR" "$C_OFF" >&2
    exit 1
fi
