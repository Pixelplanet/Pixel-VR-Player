#!/bin/sh
# Packs a built plugin folder (build/plugins/<name>: plugin.json + executable)
# into <out>/<id>-<version>.pvrplugin, a gzip tar with that one folder. Open the
# file in Pixel VR Player's file browser to install it.
#
#   tools/package-plugin.sh build/plugins/funscript dist/
set -eu
src=${1:?usage: package-plugin.sh <build>/plugins/<name> [out-dir]}
out=${2:-.}
manifest="$src/plugin.json"
[ -f "$manifest" ] || { echo "no plugin.json in $src" >&2; exit 1; }
field() { sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$manifest" | head -n 1; }
id=$(field id)
version=$(field version)
[ -n "$id" ] && [ -n "$version" ] || { echo "plugin.json needs \"id\" and \"version\"" >&2; exit 1; }
name=$(basename "$src")
mkdir -p "$out"
file="$out/$id-$version.pvrplugin"
# Pack a stripped copy (build trees carry debug info).
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp -r "$src" "$stage/$name"
if command -v strip >/dev/null 2>&1; then
    for f in "$stage/$name"/*; do
        if [ -f "$f" ] && [ -x "$f" ]; then strip "$f" 2>/dev/null || true; fi
    done
fi
tar -czf "$file" --owner=0 --group=0 -C "$stage" "$name"
echo "$file"
