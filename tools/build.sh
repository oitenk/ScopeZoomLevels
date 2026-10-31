#!/usr/bin/env bash
# Builds the ArchiveXL patch meshes and TweakXL settings for every scope in tools/scopes.txt
# from the installed game's vanilla files, regenerates the .xl and scopes.yaml, packs the archive,
# builds the native part (red4ext\plugins\ScopeZoomLevels, needs Visual Studio with CMake) and
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

echo "== building the native part (red4ext plugin)"
VCVARS="${VCVARS:-C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat}"
NATIVE="$(cygpath -w "$ROOT/native")"
# A small batch file: cmd can't take this quoted command line straight from Git Bash.
cat > "$WORK/native.cmd" <<EOF
@call "$(cygpath -w "$VCVARS")" >nul || exit /b 1
cmake -S "$NATIVE" -B "$NATIVE\\build" -G Ninja -DCMAKE_BUILD_TYPE=Release >nul || exit /b 1
cmake --build "$NATIVE\\build" || exit /b 1
EOF
cmd //c "$(cygpath -w "$WORK/native.cmd")"
test -f "$ROOT/red4ext/plugins/ScopeZoomLevels/ScopeZoomLevels.dll"

echo "== zipping"
rm -f "$ROOT/dist/ScopeZoomLevels.zip"
rm -rf "$WORK/zip" && mkdir -p "$WORK/zip/red4ext/plugins"
cp -r "$ROOT/archive" "$ROOT/r6" "$WORK/zip/"
cp -r "$ROOT/red4ext/plugins/ScopeZoomLevels" "$WORK/zip/red4ext/plugins/"
(cd "$WORK/zip" && powershell.exe -NoProfile -Command \
  "Compress-Archive -Path 'archive','r6','red4ext' -DestinationPath '$(cygpath -w "$ROOT/dist/ScopeZoomLevels.zip")'")
echo "built dist/ScopeZoomLevels.zip"
