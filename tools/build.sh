#!/usr/bin/env bash
# Builds the ArchiveXL patch meshes and TweakXL settings for every scope in tools/scopes.txt
# from the installed game's vanilla files, regenerates the .xl and scopes.yaml, packs the archive and
# writes the Vortex zip to dist\. Run from Git Bash: bash tools/build.sh
set -euo pipefail

GAME="${GAME:-C:/Games/steamapps/common/Cyberpunk 2077}"
WKIT="${WKIT:-D:/WolvenKit.Console-9.1.0/WolvenKit.CLI.exe}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/_build"

rm -rf "$WORK"
mkdir -p "$WORK/vanilla" "$WORK/json" "$WORK/ScopeZoomLevels" "$ROOT/archive/pc/mod" "$ROOT/dist"

SCOPES=$(sed -e 's/#.*//' "$ROOT/tools/scopes.txt" | awk 'NF{print $1}' | paste -sd'|')
echo "== extracting vanilla scopes: $SCOPES"
for dir in content ep1; do  # base game and Phantom Liberty
  "$WKIT" extract "$GAME/archive/pc/$dir" -r "($SCOPES).*\.(app|mesh)$" -o "$WORK/vanilla" >/dev/null
done
"$WKIT" convert serialize "$WORK/vanilla" -o "$WORK/json" >/dev/null

echo "== building patch meshes"
python -I "$ROOT/tools/build_scopes.py" "$ROOT/tools/scopes.txt" "$WORK/json" "$WORK/ScopeZoomLevels" \
  "$ROOT/archive/pc/mod/ScopeZoomLevels.archive.xl" "$ROOT/r6/tweaks/ScopeZoomLevels/scopes.yaml"
"$WKIT" convert deserialize "$WORK/ScopeZoomLevels" >/dev/null
find "$WORK/ScopeZoomLevels" -name "*.json" -delete

echo "== packing"
"$WKIT" pack "$WORK/ScopeZoomLevels" -o "$WORK" >/dev/null
cp "$WORK/ScopeZoomLevels.archive" "$ROOT/archive/pc/mod/ScopeZoomLevels.archive"

echo "== zipping"
rm -f "$ROOT/dist/ScopeZoomLevels.zip"
(cd "$ROOT" && powershell.exe -NoProfile -Command \
  "Compress-Archive -Path 'archive','r6' -DestinationPath 'dist/ScopeZoomLevels.zip'")
echo "built dist/ScopeZoomLevels.zip"
