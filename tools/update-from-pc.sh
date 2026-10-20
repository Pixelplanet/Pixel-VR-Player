#!/usr/bin/env bash
# Pixel VR Player - Remote updater from Mac / Linux to Steam Frame

set -euo pipefail

echo "==============================================================="
echo "       Pixel VR Player - Steam Frame Remote Updater"
echo "==============================================================="
echo " This tool updates Pixel VR Player on your Valve Steam Frame"
echo " to the latest release over Wi-Fi directly from your Mac or PC."
echo " (All your settings, bookmarks, and shares are kept safe)."
echo "==============================================================="
echo ""

read -rp "Enter Steam Frame IP address [press Enter for steamframe.local]: " INPUT_IP
FRAME_IP="${INPUT_IP:-steamframe.local}"

echo ""
echo "Connecting to steamos@${FRAME_IP}..."
echo "(If prompted for a password, enter your SteamOS desktop password)"
echo ""

REMOTE_CMD="curl -fsSL https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player/main/tools/install.sh | sh -s -- --update"

if ssh -o StrictHostKeyChecking=no "steamos@${FRAME_IP}" "${REMOTE_CMD}"; then
    echo ""
    echo "==============================================================="
    echo " [SUCCESS] Pixel VR Player has been updated on your Steam Frame!"
    echo "==============================================================="
else
    echo ""
    echo "==============================================================="
    echo " [NOTE] If connection failed, please ensure:"
    echo "  1. Both devices are on the same Wi-Fi network."
    echo "  2. SSH is enabled on the headset."
    echo "     (In Desktop Mode, open Konsole and run 'passwd',"
    echo "      then 'sudo systemctl start sshd', or enable Dev Mode)."
    echo "==============================================================="
fi
