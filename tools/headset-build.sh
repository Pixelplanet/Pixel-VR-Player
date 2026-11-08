#!/bin/sh
# Sync working-tree changes (vs HEAD) to the headset, build build-embed there
# (our FFmpeg 8 embedded), optionally install into ~/PixelVRPlayer.
# Usage: tools/headset-build.sh [deploy]   (from Git Bash; ~/pixelvr-src must already match HEAD)
set -e
cd "$(dirname "$0")/.."
KEY=~/AppData/Local/steamos-devkit/steamos-devkit/devkit_rsa
H=steamos@192.168.178.61
SSH="ssh -n -i $KEY -o StrictHostKeyChecking=no $H"
git status --porcelain --untracked-files=all -- src browser shaders cmake CMakeLists.txt tests third_party | while read -r st f; do
  case "$st" in
    D) $SSH "rm -f ~/pixelvr-src/$f" ;;
    *) $SSH "mkdir -p ~/pixelvr-src/$(dirname "$f")"; tr -d '\r' < "$f" | ssh -i $KEY -o StrictHostKeyChecking=no $H "cat > ~/pixelvr-src/$f" ;;
  esac
done
ssh -i $KEY -o StrictHostKeyChecking=no $H 'cd ~/pixelvr-src && cmake --build build-embed --parallel 2>&1 | grep -E "error|warning:" | head -40; exit ${PIPESTATUS[0]}' || { echo BUILD-FAILED; exit 1; }
echo BUILD-OK
if [ "$1" = deploy ]; then
  $SSH 'set -e; cd ~/pixelvr-src/build-embed; d=~/PixelVRPlayer; cp bin/pixelvr "$d/pixelvr.new" && chmod 755 "$d/pixelvr.new" && mv -f "$d/pixelvr.new" "$d/pixelvr"; mkdir -p $d/shaders; cp shaders/*.spv $d/shaders/; sha256sum bin/pixelvr $d/pixelvr | cut -c1-16; echo DEPLOYED'
fi
