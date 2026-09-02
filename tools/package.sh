#!/usr/bin/env bash
# Stage + zip the release (SPEC §3.5). Layout mirrors Apparel Preview:
# flat LICENSE/README/CHANGELOG + SKSE/Plugins/{dll,ini}. No PDB.
# The staged INI flips bVerboseLog to 0 (field installs keep it on via dist/).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="$(sed -n 's/^project(MenuStudio VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
DLL="$ROOT/build/release/MenuStudio.dll"
STAGE="$ROOT/release/stage"
# Display name and internal name are both "Menu Studio" / MenuStudio now.
ZIP="$ROOT/release/MenuStudioSE-$VER.zip"

[ -f "$DLL" ] || { echo "no DLL at $DLL - build first"; exit 1; }

rm -rf "$STAGE" "$ZIP"
mkdir -p "$STAGE/SKSE/Plugins"

cp "$DLL" "$STAGE/SKSE/Plugins/"
sed 's/^bVerboseLog=1/bVerboseLog=0/' \
    "$ROOT/dist/SKSE/Plugins/MenuStudio.ini" > "$STAGE/SKSE/Plugins/MenuStudio.ini"
# Backdrop assets: the void shell/image/colour meshes + their textures. The .png
# in dist/textures is the dev source for the DDS - leave it out of the release.
cp -r "$ROOT/dist/meshes"   "$STAGE/meshes"
cp -r "$ROOT/dist/textures" "$STAGE/textures"
find "$STAGE/textures" -iname "*.png" -delete
# ⚠⚠ THE CORNER ART, AND IT HAS NEVER BEEN IN A RELEASE ZIP. FrameArt loads
# frame.png and frame_fill.png from here; without them it returns 0 and every
# carved surface silently falls back to the straight bevel, which is a different
# shape from the scooped art corner and is the whole look people install this
# for. It was survivable while iFrameStyle defaulted to auto, because a player
# not running Vel'dun drew plain and never asked for the art. Carved is the
# default now, so a zip without these renders wrong for everyone.
#
# ⚠ COPIED AFTER THE PNG SWEEP ABOVE ON PURPOSE. That -delete strips dev-source
# PNGs out of textures/; these are the real payload and must survive it.
mkdir -p "$STAGE/SKSE/Plugins/MenuStudio"
cp -r "$ROOT/dist/SKSE/Plugins/MenuStudio/icons" "$STAGE/SKSE/Plugins/MenuStudio/icons"
cp "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/CHANGELOG.md" "$STAGE/"

(cd "$STAGE" && powershell -NoProfile -Command \
    "Compress-Archive -Path * -DestinationPath '$(cygpath -w "$ZIP")' -Force")

echo "packaged: $ZIP"
unzip -l "$ZIP"
