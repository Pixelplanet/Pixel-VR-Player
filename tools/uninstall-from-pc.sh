#!/usr/bin/env bash
# Pixel VR Player - Remote uninstaller from Mac / Linux to Steam Frame

set -euo pipefail

echo "==============================================================="
echo "       Pixel VR Player - Steam Frame Remote Uninstaller"
echo "==============================================================="
echo " This tool removes Pixel VR Player and its Steam shortcut from"
echo " your Valve Steam Frame over your local Wi-Fi network."
echo "==============================================================="
echo ""

read -rp "Enter Steam Frame IP address [press Enter for steamframe.local]: " INPUT_IP
FRAME_IP="${INPUT_IP:-steamframe.local}"

echo ""
echo "Connecting to steamos@${FRAME_IP}..."
echo ""

if ssh -o StrictHostKeyChecking=no "steamos@${FRAME_IP}" "curl -fsSL https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player/main/tools/install.sh | sh -s -- --uninstall"; then
    echo ""
    echo "==============================================================="
    echo " [SUCCESS] Pixel VR Player removed from your Steam Frame!"
    echo "==============================================================="
else
    echo ""
    echo "==============================================================="
    echo " [NOTE] If connection failed, please ensure:"
    echo "  1. Both devices are on the same Wi-Fi network."
    echo "  2. SSH is enabled on the headset."
    echo "==============================================================="
fi
