#!/usr/bin/env bash
# Pixel VR Player - Remote installer from Mac / Linux to Steam Frame

set -euo pipefail

echo "==============================================================="
echo "       Pixel VR Player - Steam Frame Remote Installer"
echo "==============================================================="
echo " This tool installs Pixel VR Player on your Valve Steam Frame"
echo " over your local Wi-Fi network directly from your Mac or PC."
echo "==============================================================="
echo ""

read -rp "Enter Steam Frame IP address [press Enter for steamframe.local]: " INPUT_IP
FRAME_IP="${INPUT_IP:-steamframe.local}"

echo ""
echo "Connecting to steamos@${FRAME_IP}..."
echo "(If prompted for a password, enter your SteamOS desktop password)"
echo ""

EXTRA_ARGS="${*:-}"
if [ -n "${EXTRA_ARGS}" ]; then
    REMOTE_CMD="curl -fsSL https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player/main/tools/install.sh | sh -s -- ${EXTRA_ARGS}"
else
    REMOTE_CMD="curl -fsSL https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player/main/tools/install.sh | sh"
fi

if ssh -o StrictHostKeyChecking=no "steamos@${FRAME_IP}" "${REMOTE_CMD}"; then
    echo ""
    echo "==============================================================="
    if [[ "${EXTRA_ARGS}" == *"--uninstall"* ]]; then
        echo " [SUCCESS] Pixel VR Player removed from your Steam Frame!"
    else
        echo " [SUCCESS] Pixel VR Player installed on your Steam Frame!"
        echo " It is now in your Steam Library under Non-Steam Games."
    fi
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
