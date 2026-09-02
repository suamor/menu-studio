#!/usr/bin/env bash
# Build the Menu Studio FOMOD installer zip (the release artifact).
#
# Menu Studio is one universal DLL for Skyrim SE 1.5.97 and Anniversary Edition
# (1.6.1130+), so there is no SE/AE file choice and nothing any answer adds or
# withholds. The FOMOD asks TWO questions, the scene and the background, and
# both are starting settings the in-game panel owns a second later.
#
# Package layout (zip root): fomod/{info.xml,ModuleConfig.xml}, Images/ (the
# banner + one screenshot per option), core/ (game files, installed to Data),
# settings/<scene>-<background>/ (the starting INI, one per combination),
# + LICENSE and a short README.txt at the root (NOT installed). The download
# carries only LICENSE (GPL) + a README.txt pointer, no
# CHANGELOG/KNOWN-ISSUES/THIRD-PARTY.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="$(sed -n 's/^project(MenuStudio VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
DLL="$ROOT/build/release/MenuStudio.dll"
STAGE="$ROOT/release/fomod-stage"
ZIP="$ROOT/release/MenuStudio-$VER.zip"
# A zip built with the banner standing in for missing installer pictures is a
# test artifact and is named as one, so it can never be uploaded by mistake as
# the release. Same rule and same spelling as the sibling repo's packager.
if [ "${MS_ALLOW_MISSING_SHOTS:-0}" = "1" ]; then
    ZIP="$ROOT/release/MenuStudio-$VER-TEST-no-shots.zip"
fi

[ -f "$DLL" ] || { echo "no DLL at $DLL - build first"; exit 1; }

# ⚠⚠ A DIAGNOSTIC BUILD CAN NEVER WEAR THE RELEASE NAME. It forces logging on
# for every player who installs it, and the one way that reaches Nexus is a
# packaging run against a build tree somebody left configured with
# MENUSTUDIO_DIAG=ON. The DLL is asked directly rather than the cache, because
# the cache is not what ships. Same rule and same spelling as the missing-shots
# refusal above: rename the artifact, do not refuse the build.
if grep -qa "1\.1\.5-diag" "$DLL" 2>/dev/null ||
   grep -qa "DIAGNOSTIC BUILD" "$DLL" 2>/dev/null; then
    ZIP="${ZIP%.zip}-DIAG-do-not-upload.zip"
    echo "*** the DLL is a DIAGNOSTIC build: naming the archive $(basename "$ZIP")"
fi

# The installer pictures, checked BEFORE the old zip is removed. One in-game
# screenshot per option lives in fomod/images/ under the name
# fomod/ModuleConfig.xml asks for; the shot list is docs/release/fomod-shots.md.
#
# ⚠ THE LIST IS READ OUT OF THE XML RATHER THAN KEPT HERE, so adding an option
# with a new picture cannot forget the file. A mod manager shows a broken image
# or nothing at all for a missing path and says so nowhere a release check would
# read, which is how every option on the scene step showed the banner for a
# month. The refusal is ahead of the rm below, so a refused run leaves the
# previous zip where it was.
STAND_IN=()
missing=0
while IFS= read -r img; do
    # if/then rather than `[ ] && continue`: under set -e a false test at the
    # end of an && list aborts the run, and this loop must reach its report.
    if [ -f "$ROOT/fomod/images/$img" ]; then continue; fi
    if [ "${MS_ALLOW_MISSING_SHOTS:-0}" = "1" ]; then
        echo "WARNING: fomod/images/$img is missing, shipping the banner in its place (test zip only)"
        STAND_IN+=("$img")
    else
        echo "missing installer picture: fomod/images/$img (named by fomod/ModuleConfig.xml)"
        missing=1
    fi
done < <(grep -o 'Images\\\\[^"]*' "$ROOT/fomod/ModuleConfig.xml" |
         sed 's|Images\\\\||' | sort -u)
[ "$missing" = 0 ] || {
    echo "The shot list is docs/release/fomod-shots.md. Take the pictures, drop them"
    echo "in fomod/images/, and package again. MS_ALLOW_MISSING_SHOTS=1 builds a"
    echo "test zip with the banner standing in."
    exit 1
}

# ⚠⚠ THE INSTALLER COPY IS UNISEX. The player's character can be anyone, so no
# string in ModuleConfig.xml refers to them as "her" or "she". One sentence about
# interface mods carried it in all three installers at once (user, 2026-08-28);
# two were fixed by hand and the third was missed, so it is checked here instead
# of remembered. Ahead of the rm below, so a refusal leaves the old zip alone.
#
# ⚠ ONLY ModuleConfig.xml. nexus/DESCRIPTION.bbcode legitimately thanks a real
# person as "her" and must not be swept up in this.
if grep -qiE '\b(her|hers|she|herself)\b' "$ROOT/fomod/ModuleConfig.xml"; then
    echo "fomod/ModuleConfig.xml calls the player's character 'her'. The installer copy is unisex:"
    grep -niE '\b(her|hers|she|herself)\b' "$ROOT/fomod/ModuleConfig.xml"
    echo "Use 'your character', or they/them."
    exit 1
fi

rm -rf "$STAGE" "$ZIP"

# FOMOD metadata + banner (ModuleConfig references Images\menustudio.png).
mkdir -p "$STAGE/fomod" "$STAGE/Images"
cp "$ROOT/fomod/info.xml" "$ROOT/fomod/ModuleConfig.xml" "$STAGE/fomod/"
cp "$ROOT/fomod/banner.png" "$STAGE/Images/menustudio.png"
# The per-option pictures. The check ran above, before anything was deleted;
# this is only the copy.
if ls "$ROOT/fomod/images/"*.png >/dev/null 2>&1; then
    cp "$ROOT/fomod/images/"*.png "$STAGE/Images/"
fi
# ${arr[@]+"${arr[@]}"}: an empty array under set -u is "unbound" on older
# bashes, and this array is empty on every release build.
for img in ${STAND_IN[@]+"${STAND_IN[@]}"}; do
    cp "$ROOT/fomod/banner.png" "$STAGE/Images/$img"
done

# core: the game files that install to Data. DLL + the settings INI + the void
# meshes/textures (the .png in dist/textures is the dev source for the DDS, so
# leave it out). No docs in core - see below.
mkdir -p "$STAGE/core/SKSE/Plugins"
cp "$DLL" "$STAGE/core/SKSE/Plugins/"

# ---- the starting settings, and why the INI left core ----------------------
#
# It used to install with the DLL, one copy, no choice. It is a CHOICE now, so
# it moves to settings/<flavour>/ and the ModuleConfig installs exactly one.
#
# ⚠⚠ THE FLAVOURS ARE STARTING SETTINGS AND NOTHING ELSE. Every one of them is
# a key the player can change in game a second later, and the files are
# otherwise byte-identical. That is the rule this repo's sibling learned the
# hard way: an install choice must not gate a runtime setting, or the installer
# quietly decides something the panel appears to own. Nothing is added to or
# withheld from the install by any answer on these pages.
#
# ⚠ ONE TEMPLATE, SED PER FLAVOUR, GUARD PER SED. A hand-maintained second copy
# forks its comments the first time either is edited, and a substitution that
# stops biting (a renamed key, a reformatted line) ships a file labelled one
# thing and holding another, in silence. Every flip below is verified.
#
# ⚠⚠ AND EVERY sed HERE ENDS BY REWRITING THE LINE ENDING, WHICH IS NOT
# COSMETIC. The template is CRLF and MSYS sed reads it in text mode, so it hands
# back LF and every generated file silently changes all 673 lines. That is
# invisible in the INI and very visible to the guards: the old two-flavour block
# built one file with `cp` (CRLF) and one with `sed` (LF), so its "these differ
# by exactly two lines" check could never have passed, which is why no shipped
# zip has ever carried a settings folder. Strip then re-add, in that order, so
# the pair is idempotent and gives one CR on a machine whose sed keeps them as
# well as on one whose sed does not. It goes LAST in every expression list, so a
# flip written with a `$` anchor one day still sees the line it expects.
CRLF=(-e 's/\r$//' -e 's/$/\r/')

BASE="$STAGE/settings-base.ini"
sed -e 's/^bVerboseLog=1/bVerboseLog=0/' "${CRLF[@]}" \
    "$ROOT/dist/SKSE/Plugins/MenuStudio.ini" > "$BASE"
grep -q '^bVerboseLog=0' "$BASE" ||
    { echo "settings INI: bVerboseLog substitution failed"; exit 1; }
grep -q '^iDeclutterMode=' "$BASE" ||
    { echo "settings INI: iDeclutterMode missing - the INI looks wrong"; exit 1; }
# ⚠⚠ TWO OF THE THREE AXES ARE GONE AND THE GUARDS STAY (user, 2026-08-27). The
# installer no longer asks which dome hangs behind the character or whether to
# hide the floating item preview. Both are panel settings, both now ship at the
# answer most people want, and an installer page that only writes a setting the
# panel owns is a page nobody needed. These two guards are what makes that a
# fact rather than a hope: if the template ever drifts off the shipped default,
# packaging stops here instead of shipping an INI nobody chose.
grep -q '^bDisableItemPreview3D=0' "$BASE" ||
    { echo "settings INI: template no longer ships bDisableItemPreview3D=0 (the preview is visible by default)"; exit 1; }
# ⚠⚠ AND THE SAME GUARD FOR THE FRAMING MEMORY, WHICH SHIPS OFF (user,
# 2026-08-31, after living with it on). This one is worth a guard more than the
# two above it, because Save() writes the key: the first install to receive it
# ON keeps it on for good, and no later change to the shipped default can reach
# that install. The template is the only chance to get it right.
grep -q '^bRememberFraming=0' "$BASE" ||
    { echo "settings INI: template no longer ships bRememberFraming=0 (menus open on their own framing by default)"; exit 1; }
# ⚠⚠ THIS GUARD ASSERTED SOMETHING THAT WAS NOT TRUE UNTIL 2026-08-28. The
# empty value matched no preset, so Settings::Load fell through to its
# constellation fallback and "no dome by default" described nothing that ever
# happened. Settings.cpp resolves the empty value to blank now. The guard stays
# on the BASE, which is the one file that still carries the empty value; every
# generated flavour below writes an explicit name over it and is checked for it.
grep -q '^sBackground=$' "$BASE" ||
    { echo "settings INI: template no longer ships an empty sBackground (the base every flavour writes over)"; exit 1; }

# ⚠ The scene axis flips this key, so the template has to ship the value the
# "void" flavour is the do-nothing case of. A template that already read 0 would
# make four scene flavours out of three.
grep -q '^iDeclutterMode=2' "$BASE" ||
    { echo "settings INI: template no longer ships iDeclutterMode=2"; exit 1; }

# ---- two axes, so twenty flavours -----------------------------------------
#
# ⚠⚠ NEITHER AXIS CAN BE A FOLDER INSTALL. Both write MenuStudio.ini, which
# the DLL also writes, so the flavours are generated here and the ModuleConfig
# picks exactly one through conditionalFileInstalls.
#
# ⚠⚠ THE BACKGROUND AXIS WENT ON 2026-08-27 AND CAME BACK ON 2026-08-28, and
# both calls were the user's. It went with the item-preview axis on one
# argument: an installer page that only writes a setting the panel owns makes a
# permanent-looking decision out of a tick. That still holds for the item
# preview, which is why it is still gone and still guarded below. It does not
# hold for the background, because the five backgrounds are the mod's subject
# and there is now a picture of each: a page a player can look at is worth more
# than the tick it saves them.
#
# ⚠ FORTY FILES IS WHERE THIS WAS BEFORE, AND TWENTY IS WHERE IT IS NOW,
# because the third axis stayed dead. A cross product grows fast enough that
# every axis has to earn its place; the item preview never did.
#
# ⚠ EVERY FLAVOUR GOES THROUGH sed, INCLUDING THE ONE THAT CHANGES NOTHING.
# A file copied with `cp` beside files written by `sed` is the line ending trap:
# they come out CRLF and LF respectively, at which point the guard below reports
# every line as different and nobody can tell a real mistake from a newline. The
# no-op expression exists to keep sed the only tool that writes these files.
SCENES="off solo void stage"
# ⚠ THESE FIVE NAMES ARE THE BUILTINS IN src/BackdropPacks.cpp AND THE FLAG
# VALUES IN fomod/ModuleConfig.xml, and all three lists have to agree. A name
# here that no preset answers to would generate an INI whose sBackground
# resolves to nothing, and Settings::Load would drop that player on
# constellation without either side saying so.
BACKGROUNDS="blank constellation vampire aurora custom"
for scene in $SCENES; do
 for bg in $BACKGROUNDS; do
  dir="$STAGE/settings/$scene-$bg"
  mkdir -p "$dir"
  out="$dir/MenuStudio.ini"
  flips=0
  seds=(-e 's/^/&/')
  case "$scene" in
    off)   mode=0 ;;
    solo)  mode=1 ;;
    void)  mode=2 ;;
    stage) mode=3 ;;
  esac
  if [ "$mode" != "2" ]; then
    seds+=(-e "s/^iDeclutterMode=2/iDeclutterMode=$mode/")
    flips=$((flips + 1))
  fi
  # ⚠ blank IS WRITTEN OUT LIKE THE OTHER FOUR rather than left as the base's
  # empty value. Both mean the same thing to the DLL since 2026-08-28, and only
  # one of them says so to somebody reading the file they were shipped.
  seds+=(-e "s/^sBackground=$/sBackground=$bg/")
  flips=$((flips + 1))
  seds+=("${CRLF[@]}")
  sed "${seds[@]}" "$BASE" > "$out"

  # The flip verified in the file it was meant to land in. A substitution that
  # stops biting, because a key was renamed or a line reformatted, ships an INI
  # labelled one thing and holding another, in silence.
  grep -q "^iDeclutterMode=$mode" "$out" ||
      { echo "$scene-$bg: iDeclutterMode is not $mode"; exit 1; }
  grep -q "^sBackground=${bg}\$" "$out" ||
      { echo "$scene-$bg: sBackground is not $bg"; exit 1; }
  # ⚠ THE KEY THE INSTALLER NO LONGER ASKS ABOUT IS STILL CHECKED, in every
  # flavour, because "we stopped asking" and "it ships at the right value" are
  # different claims and only the second one is worth anything to a player.
  grep -q "^bDisableItemPreview3D=0" "$out" ||
      { echo "$scene-$bg: the floating item preview is not visible"; exit 1; }
  # ⚠ AND NOTHING ELSE MOVED. Two lines per flip, one from each side of the
  # diff. A sed that bit somewhere it should not is invisible without this.
  want_lines=$((flips * 2))
  got="$(diff "$BASE" "$out" | grep -c '^[<>]' || true)"
  [ "$got" = "$want_lines" ] ||
      { echo "$scene-$bg differs from the base by $got lines, expected $want_lines"; exit 1; }
 done
done
# ⚠ THE COUNT IS DERIVED, NOT TYPED. A ModuleConfig pattern with no folder
# behind it installs no INI and ignores the player's answers in silence, and
# the axes are two lists that can be edited independently.
made="$(find "$STAGE/settings" -name MenuStudio.ini | wc -l)"
wanted=$(( $(echo $SCENES | wc -w) * $(echo $BACKGROUNDS | wc -w) ))
[ "$made" = "$wanted" ] ||
    { echo "generated $made settings flavours, expected $wanted"; exit 1; }
# And every one of them has a pattern in the XML that names it.
for scene in $SCENES; do
  for bg in $BACKGROUNDS; do
    grep -q "settings\\\\$scene-$bg\\\\MenuStudio.ini" "$ROOT/fomod/ModuleConfig.xml" ||
        { echo "fomod/ModuleConfig.xml has no pattern installing settings/$scene-$bg"; exit 1; }
  done
done
rm -f "$BASE"
# The example backdrop pack (Backdrops/Example.ini) the "make your own backdrop"
# guide tells users to copy from.
cp -r "$ROOT/dist/SKSE/Plugins/MenuStudio" "$STAGE/core/SKSE/Plugins/MenuStudio"
cp -r "$ROOT/dist/meshes"   "$STAGE/core/meshes"
cp -r "$ROOT/dist/textures" "$STAGE/core/textures"
# ⚠⚠ A .png BESIDE A .dds OF THE SAME NAME IS A DEV SOURCE. ANYTHING ELSE IS
# SHIPPED ART. This used to be `-iname "*.png" -delete`, written when the only
# PNG in here was voidshell_g.png, the source the shipped voidshell_g.dds is
# baked from. The background picker then put its card thumbnails under
# textures/mtb/backdrops/shipped/ as PNGs and this line ate all five of them, in
# every zip, from the day the cards landed: the dev deploy copies dist/ whole so
# the cards were there while testing, and only a FOMOD install had a picker with
# no pictures on it. Field 2026-08-27: "i don't see cards for backgrounds".
dropped=0
while IFS= read -r png; do
    if [ -f "${png%.png}.dds" ]; then
        rm -f "$png"
        dropped=$((dropped + 1))
    fi
done < <(find "$STAGE/core/textures" -iname "*.png")
echo "textures: dropped $dropped dev source png(s), kept $(find "$STAGE/core/textures" -iname "*.png" | wc -l) shipped one(s)"
# ⚠ AND THE CARDS ARE COUNTED, because "the rule is right" and "the pictures
# are in the zip" are different claims and only the second one draws a picker.
shipped_cards="$(find "$STAGE/core/textures/mtb/backdrops" -iname "*.png" 2>/dev/null | wc -l)"
[ "$shipped_cards" -ge 1 ] ||
    { echo "no background card pictures reached the package - the picker would draw blank"; exit 1; }
# Docs at the ARCHIVE ROOT (not installed to Data). Single-source-of-truth is
# Ship LICENSE (GPL requires it in the download) + a short README.txt pointer
# only. Full release information lives on the Nexus mod page; source stays on
# GitHub.
cp "$ROOT/LICENSE" "$STAGE/"
# ⚠⚠ THE SKY HARVESTER RIDES AT THE ARCHIVE ROOT, NOT IN core/. It is a
# script, not a game file: under Data it would do nothing and would collide by
# name with any other mod shipping one. Beside LICENSE is where a player can
# find it and no mod manager will install it.
#
# It reads ModOrganizer.ini for the selected profile, the mods folder and the
# game path, so it runs on a machine it has never seen; bsa_extract.py is
# vendored next to it because a cross-repo import cannot travel.
mkdir -p "$STAGE/tools"
cp "$ROOT/tools/harvest_backdrops.py" "$STAGE/tools/"
cp "$ROOT/tools/bsa_extract.py"       "$STAGE/tools/"
cp "$ROOT/tools/bsa_list.py"          "$STAGE/tools/"
for f in harvest_backdrops.py bsa_extract.py bsa_list.py; do
    [ -f "$STAGE/tools/$f" ] || { echo "tools/$f did not reach the package"; exit 1; }
done
# ⚠ AND IT CARRIES NO PATH OFF THIS MACHINE. The whole reason it can ship is
# that it stopped naming one, so the packaging step is where that gets checked
# rather than trusted.
if grep -rlE "C:.(Games.Nolvus|Studios.Mod Studio)" "$STAGE/tools/" >/dev/null 2>&1; then
    grep -rnE "C:.(Games.Nolvus|Studios.Mod Studio)" "$STAGE/tools/"
    echo "a shipped tool still names a path on this machine"
    exit 1
fi

cat > "$STAGE/README.txt" <<'EOF'
Menu Studio
Pause the world when you open a menu and keep your character live and
posed, in a clean studio of your choosing. One download for Skyrim SE
1.5.97 and Anniversary Edition.

Your load order's own skies, in the background picker
  tools/harvest_backdrops.py finds every sky dome your mods install,
  vanilla and modded, loose and inside BSAs, and writes them into Menu
  Studio as a pack of backgrounds. Needs Python 3. Run it once, and
  again whenever your load order changes:

    python tools/harvest_backdrops.py

  It reads ModOrganizer.ini for the rest. Instructions and what to do
  when it cannot find MO2 are on the mod page.

Mod page, documentation, changelog and support:
  https://www.nexusmods.com/skyrimspecialedition/mods/185362

Source code:
  https://github.com/maartenharms/menu-studio

Licensed under GPL-3.0 (see LICENSE).
EOF

(cd "$STAGE" && powershell -NoProfile -Command \
    "Compress-Archive -Path * -DestinationPath '$(cygpath -w "$ZIP")' -Force")

echo "packaged FOMOD: $ZIP"
unzip -l "$ZIP"
