#!/usr/bin/env python3
"""Package the HDRI backdrop packs as their own download.

The base Menu Studio archive is under two megabytes. The HDRIs are ninety eight,
which is a fifty fold increase paid by every person who only wanted the void, so
they ship beside it instead of inside it (user, 2026-08-27).

    python tools/make_backdrops_zip.py
    python tools/make_backdrops_zip.py --dry-run

Where the payload comes from, and why it is not in the repo: every file here is
DERIVED. tools/hdri_to_pack.py takes a Poly Haven .hdr, tone maps it, compresses
it to BC7 with a mip chain and writes the .dds, its reflection cube, a card
thumbnail and the pack manifest straight into a mod folder. Committing ninety
eight megabytes of bake output to git would be storing the same thing twice, so
this reads the mod folder hdri_to_pack.py writes to, exactly as
harvest_backdrops.py does, and says so loudly if it is not there.

⛔ harvested_skies.ini IS NEVER PACKAGED. tools/harvest_backdrops.py generates it
from whatever MO2 profile the machine has, so it names meshes from THIS load
order: shipping it would offer a player Unslaad and Vigilant skies they do not
own. It is skipped by name.
"""

import argparse
import configparser
import os
import re
import sys
import zipfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
INSTANCE = r"C:\Games\Nolvus\Instances\Nolvus Awakening"
SOURCE = os.path.join(INSTANCE, "MODS", "mods", "Menu Studio")

PACK_DIR = os.path.join("SKSE", "Plugins", "MenuStudio", "Backdrops")
NEVER = {"harvested_skies.ini"}


def version():
    with open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"^project\(MenuStudio VERSION ([0-9.]+)", f.read(), re.M)
    if not m:
        sys.exit("could not read the version out of CMakeLists.txt")
    return m.group(1)


def referenced_textures(ini_path):
    """Every texture a pack manifest names, plus the siblings that ride with it.

    A manifest names one image. The reflection cube and the card thumbnail are
    named by convention rather than by a key, so a packager that only followed
    image= would ship a backdrop with no reflections and a blank card, which is
    exactly the shape of the bug that started this.
    """
    cfg = configparser.ConfigParser(strict=False)
    cfg.optionxform = str
    cfg.read(ini_path, encoding="utf-8-sig")

    wanted, missing = [], []
    for section in cfg.sections():
        for key in ("image", "thumb"):
            rel = cfg[section].get(key)
            if not rel:
                continue
            rel = rel.strip().replace("/", "\\")
            wanted.append(os.path.join("textures", rel))
            if key == "image":
                stem = os.path.splitext(rel)[0]
                for sibling in (stem + "_cube.dds", stem + "_thumb.png"):
                    p = os.path.join(SOURCE, "textures", sibling)
                    if os.path.isfile(p):
                        wanted.append(os.path.join("textures", sibling))
    for rel in wanted:
        if not os.path.isfile(os.path.join(SOURCE, rel)):
            missing.append(rel)
    return wanted, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    src_packs = os.path.join(SOURCE, PACK_DIR)
    if not os.path.isdir(src_packs):
        sys.exit("no pack folder at %s\n"
                 "The payload is bake output, not repo content: run\n"
                 "  python tools/hdri_to_pack.py <sky.hdr> --name \"...\"\n"
                 "for each pack first." % src_packs)

    inis = sorted(f for f in os.listdir(src_packs)
                  if f.lower().endswith(".ini") and f.lower() not in NEVER)
    skipped = sorted(f for f in os.listdir(src_packs)
                     if f.lower() in NEVER)
    if not inis:
        sys.exit("no shippable pack manifests in %s" % src_packs)

    entries, total_missing = [], []
    for ini in inis:
        ini_path = os.path.join(src_packs, ini)
        wanted, missing = referenced_textures(ini_path)
        total_missing += ["%s -> %s" % (ini, m) for m in missing]
        entries.append((ini, os.path.join(PACK_DIR, ini), wanted))

    if total_missing:
        for m in total_missing:
            print("MISSING:", m)
        sys.exit("a manifest names a texture that is not there; nothing packaged")

    # One file list, deduped: several packs can share a thumbnail.
    files = {}
    for ini, rel_ini, wanted in entries:
        files[rel_ini] = os.path.join(src_packs, ini)
        for rel in wanted:
            files[rel] = os.path.join(SOURCE, rel)

    out = os.path.join(ROOT, "release",
                       "MenuStudioBackdrops-%s.zip" % version())
    total = sum(os.path.getsize(p) for p in files.values())
    print("%d pack(s), %d file(s), %.1f MB" % (len(entries), len(files),
                                               total / 1048576.0))
    for ini, _rel, wanted in entries:
        print("   %-26s %d texture(s)" % (ini, len(wanted)))
    if skipped:
        print("   skipped, per load order and not shippable: %s" % ", ".join(skipped))
    if args.dry_run:
        return

    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for rel, path in sorted(files.items()):
            z.write(path, rel)
        z.writestr("README.txt",
                   "Menu Studio: HDRI backdrops\r\n"
                   "\r\n"
                   "Install with your mod manager, after Menu Studio itself.\r\n"
                   "The backgrounds appear in the picker in Menu Studio's\r\n"
                   "settings panel. Menu Studio works without this.\r\n"
                   "\r\n"
                   "The images come from Poly Haven and are CC0.\r\n"
                   "\r\n"
                   "https://github.com/maartenharms/menu-studio\r\n")

    # Read it back. A zip that was written is not a zip that holds what the
    # manifests ask for, and the picker draws blank on the difference.
    with zipfile.ZipFile(out) as z:
        got = {n.replace("\\", "/") for n in z.namelist()}
    for rel in files:
        assert rel.replace("\\", "/") in got, "%s did not reach the zip" % rel
    print("\n%s  %.1f MB" % (out, os.path.getsize(out) / 1048576.0))


if __name__ == "__main__":
    main()
