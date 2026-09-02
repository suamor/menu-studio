#!/usr/bin/env python3
"""Bake card pictures for the SHIPPED backgrounds (the star domes).

The picker draws an image card for anything with a thumb= and a named tile
otherwise. The three vanilla perk skydomes shipped as named tiles until this
existed; blank and custom stay named tiles on purpose (blank has no texture,
and custom is whatever image the user last armed).

Writes into dist (the release payload) AND the deployed mod, because a
release must carry the same cards the dev install shows.

    python tools/bake_shipped_thumbs.py
"""

import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
sys.path.insert(0, r"C:\Studios\Mod Studio\Fitting Room\tools")

from harvest_backdrops import (DEFAULT_OUT, bake_thumb, enabled_mods,
                               mo2_profile_dir, nif_first_texture,
                               read_data_file)

DIST = os.path.join(os.path.dirname(TOOLS), "dist")

# Mesh paths verbatim from kBuiltinBackgrounds in src/BackdropPacks.cpp
# (BackgroundPreset.mesh is meshes-relative; prefix restored here because
# read_data_file wants the full data-relative path). The slug is the preset
# NAME, which is exactly what the C++ thumb path spells: the two sides meet
# at "mtb\\backdrops\\shipped\\<name>.png" and nowhere else, so a rename on
# either side must visit both.
#
# The texture is CURATED, not guessed: the perk domes layer a dozen sheets
# and the first non-skip hit is the shared star tile, which baked the
# constellation and vampire cards identical. Three fixed domes are worth
# three hand-picked panoramas; None falls back to the harvester's pick.
SHIPPED = [
    ("constellation", "meshes\\interface\\intperkskydome.nif",
     "textures\\interface\\INTfullNebulaPanarama.dds"),
    ("vampire", "meshes\\dlc01\\interface\\intvampireperkskydome.nif",
     "textures\\dlc01\\interface\\INTVampSkyLevel1.dds"),
    ("aurora", "meshes\\interface\\teatperkskydome.nif", None),
]


def synth_cards():
    """The two shipped entries with nothing to photograph still get cards
    (user 2026-08-21): blank is a flat void-dark tile, custom is a muted
    spectrum band that reads as "your own picture goes here"."""
    import colorsys

    from PIL import Image

    blank = Image.new("RGB", (256, 128), (23, 23, 27))

    custom = Image.new("RGB", (256, 128))
    px = custom.load()
    for x in range(256):
        r, g, b = colorsys.hsv_to_rgb(x / 256.0, 0.45, 0.42)
        for y in range(128):
            px[x, y] = (int(r * 255), int(g * 255), int(b * 255))

    return [("blank", blank), ("custom", custom)]


def main():
    profile_dir, mods_dir = mo2_profile_dir()
    enabled = enabled_mods(profile_dir)
    for slug, img in synth_cards():
        rel = os.path.join("textures", "mtb", "backdrops", "shipped",
                           slug + ".png")
        for root in (DIST, DEFAULT_OUT):
            out = os.path.join(root, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            img.save(out)
            print("drew %s" % out)
    for slug, mesh, want in SHIPPED:
        nif = read_data_file(mesh, None, mods_dir, enabled)
        tex = want or (nif_first_texture(nif) if nif else None)
        dds = read_data_file(tex, None, mods_dir, enabled) if tex else None
        if not dds:
            print("no picture for %s (mesh %s, texture %s)"
                  % (slug, mesh, tex or "not found"))
            continue
        rel = os.path.join("textures", "mtb", "backdrops", "shipped",
                           slug + ".png")
        for root in (DIST, DEFAULT_OUT):
            out = os.path.join(root, rel)
            if bake_thumb(dds, out):
                print("baked %s" % out)
            else:
                print("  WARNING: could not bake %s into %s" % (slug, root))


if __name__ == "__main__":
    main()
