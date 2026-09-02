#!/usr/bin/env python3
"""Harvest every sky dome in an MO2 load order into one backdrop pack.

Walks the active profile's modlist in priority order plus the vanilla
archives, looks inside every mod's loose meshes AND its BSAs (a filesystem
walk alone is blind to archives), and keeps the NIFs that look like sky
domes. The result is a single generated manifest with one [BackgroundN]
section per dome, grouped under its own accordion in the picker.

    python harvest_backdrops.py                  # find MO2, write into the mod
    python harvest_backdrops.py --dry-run        # just list what it found
    python harvest_backdrops.py --mo2-ini "D:\\MO2\\ModOrganizer.ini"
    python harvest_backdrops.py --out "D:\\MO2\\mods\\My Sky Pack"

Re-run it after the load order changes; it overwrites its own file only
(harvested_skies.ini). Menu Studio picks it up at the next launch.

It needs Python 3 and reads everything else out of ModOrganizer.ini: which
profile is selected, where the mods live and where the game is. Nothing about
this machine is written down here.

Two optional pieces, and it says which one is missing rather than failing:
  - Pillow (`pip install pillow`) draws the card pictures. Without it every
    card shows the sky's name instead, which is what the picker did before
    pictures existed.
  - texconv (Microsoft's DirectXTex tool, on PATH) decodes a compressed sky
    texture into something Pillow can crop. Without it the cards fall back to
    the generic tile.

What counts as a sky dome (deliberately conservative - a false positive is a
broken-looking card, a false negative is one missing sky):
  - any .nif whose NAME contains skydome/skybox/skysphere/perkskydome
  - meshes\\sky\\*.nif whose name contains dome/atmosphere/stars/skyquad
Cloud layer meshes, sun/moon quads and precipitation are skipped by name.
"""

import argparse
import configparser
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from bsa_list import list_bsa
from bsa_extract import read_bsa

# ⚠⚠ NOTHING ABOUT ONE MACHINE IS WRITTEN DOWN HERE ANY MORE. This file used to
# open with an instance path, a game path and an import reaching into a sibling
# repo's tools folder, which was fine while it was a developer's script and is
# the whole difficulty the moment it ships. Everything below is read out of
# ModOrganizer.ini at run time, and bsa_extract.py is vendored beside this file.
GAME_DATA = None  # set by resolve_mo2()
TEXCONV = None    # set by find_texconv(), None when there is none


def unbyte(value):
    """MO2 stores paths as @ByteArray(...) with doubled backslashes."""
    if value is None:
        return ""
    m = re.match(r"@ByteArray\((.*)\)$", value.strip())
    if m:
        value = m.group(1)
    return value.replace("\\\\", "\\").strip()


def find_mo2_ini(explicit):
    """The ini, in the order somebody would look for it themselves."""
    if explicit:
        if not os.path.isfile(explicit):
            sys.exit("no ModOrganizer.ini at %s" % explicit)
        return explicit
    tried = []
    # A portable instance keeps its ini beside ModOrganizer.exe, and this
    # script is usually sitting in the mod folder under that same MO2 tree.
    here = TOOLS
    for _ in range(6):
        parent = os.path.dirname(here)
        if parent == here:
            break  # the drive root, and walking further just repeats it
        here = parent
        cand = os.path.join(here, "ModOrganizer.ini")
        tried.append(cand)
        if os.path.isfile(cand):
            return cand
    # An instanced install keeps it under the local app data.
    local = os.environ.get("LOCALAPPDATA", "")
    root = os.path.join(local, "ModOrganizer")
    if os.path.isdir(root):
        for name in sorted(os.listdir(root)):
            cand = os.path.join(root, name, "ModOrganizer.ini")
            tried.append(cand)
            if os.path.isfile(cand):
                return cand
    sys.exit("could not find ModOrganizer.ini. Looked in:\n  %s\n"
             "Pass it yourself:\n"
             "  python harvest_backdrops.py --mo2-ini \"D:\\path\\ModOrganizer.ini\""
             % "\n  ".join(tried))


def find_texconv(explicit):
    """texconv, or None. Optional on purpose: it only decodes card pictures."""
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    return shutil.which("texconv")

NAME_PAT = re.compile(r"(skydome|skybox|skysphere|perkskydome)", re.I)
SKIP_PAT = re.compile(r"(cloud|sunglare|\bsun\b|^sun\.|secunda|masser|rain|snow|"
                      r"precip|lightning|_lod)", re.I)


def is_sky_dome(rel):
    """rel: data-relative mesh path, backslashes, lowercase-insensitive."""
    low = rel.lower().replace("/", "\\")
    if not low.endswith(".nif") or not low.startswith("meshes\\"):
        return False
    name = low.rsplit("\\", 1)[-1]
    if SKIP_PAT.search(name):
        return False
    if NAME_PAT.search(name):
        return True
    # Inside meshes\sky\ everything that is not a cloud layer or a celestial
    # quad IS a sky: the vanilla set is atmosphere, stars, the auroras,
    # sovngardesky, soulcairnsky, bloodmoon.
    return low.startswith("meshes\\sky\\")


def resolve_mo2(ini):
    """profile dir, mods dir and the game's Data folder, all from the ini.

    ⚠ base_directory IS OPTIONAL IN MO2's OWN FILE. A portable instance that
    keeps everything beside ModOrganizer.exe leaves the key out entirely, so an
    absent one means "next to the ini" rather than "broken install".
    """
    global GAME_DATA
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.read(ini, encoding="utf-8-sig")

    base = unbyte(cp.get("Settings", "base_directory", fallback=""))
    if not base:
        base = os.path.dirname(ini)
    prof = unbyte(cp.get("General", "selected_profile", fallback=""))
    if not prof:
        sys.exit("%s names no selected profile; open MO2 once and try again" % ini)

    game = unbyte(cp.get("General", "gamePath", fallback=""))
    if not game:
        sys.exit("%s names no gamePath; pass --game-data yourself" % ini)
    GAME_DATA = os.path.join(game, "Data")

    profile_dir = os.path.join(base, "profiles", prof)
    mods_dir = os.path.join(base, "mods")
    for label, path in (("profile", profile_dir), ("mods", mods_dir),
                        ("game data", GAME_DATA)):
        if not os.path.isdir(path):
            sys.exit("the %s folder %s does not exist (read from %s)"
                     % (label, path, ini))
    return profile_dir, mods_dir


def enabled_mods(profile_dir):
    """Modlist bottom-to-top = ascending priority; return ascending."""
    mods = []
    with io.open(os.path.join(profile_dir, "modlist.txt"), encoding="utf-8-sig") as f:
        for line in f:
            line = line.strip()
            if line.startswith("+"):
                mods.append(line[1:])
    return list(reversed(mods))


def scan_mod_dir(root):
    """Yields (data-relative path, provenance) for loose sky NIFs + BSA hits."""
    meshes = os.path.join(root, "meshes")
    if os.path.isdir(meshes):
        for dirpath, _dirs, files in os.walk(meshes):
            for fn in files:
                rel = os.path.relpath(os.path.join(dirpath, fn), root)
                if is_sky_dome(rel):
                    yield rel.replace("/", "\\"), "loose"
    try:
        entries = os.listdir(root)
    except OSError:
        return
    for fn in entries:
        if fn.lower().endswith(".bsa"):
            try:
                for full in list_bsa(os.path.join(root, fn)):
                    if is_sky_dome(full):
                        yield full.replace("/", "\\"), fn
            except Exception as e:
                print("  WARNING: %s unreadable (%s)" % (fn, e))


# ---- card thumbnails -------------------------------------------------------
# A dome's own texture is its card picture. The NIF's texture paths are plain
# ASCII strings in both stream formats, so a regex over the raw bytes finds
# them without a NIF parser; normal/glow/mask suffixes are skipped and a _d
# diffuse wins when present.

TEX_PAT = re.compile(rb"textures\\[\x20-\x7e]{3,200}?\.dds", re.I)
TEX_SKIP = re.compile(r"(_n|_msn|_g|_sk|_m|_p|_e|_em)\.dds$", re.I)


def nif_first_texture(nif_bytes):
    cands, raw = [], []
    for m in TEX_PAT.finditer(nif_bytes):
        t = m.group(0).decode("cp1252", "replace")
        raw.append(t)
        if not TEX_SKIP.search(t):
            cands.append(t)
    for t in cands:
        if re.search(r"_d\.dds$", t, re.I):
            return t
    if cands:
        return cands[0]
    # A dome whose only sheets wear the skip suffixes (glow-map skies) still
    # has a usable picture, and a glow map of a sky beats a nameless tile.
    return raw[0] if raw else None


def have_pillow():
    try:
        import PIL  # noqa: F401
        return True
    except ImportError:
        return False


def draw_placeholder(out_png):
    """A generic night-sky tile for domes with nothing to photograph
    (vertex-coloured or procedural: both Unslaad domes and NAT's atmosphere
    name no texture at all). Deterministic so re-runs are byte-stable."""
    import random

    from PIL import Image

    img = Image.new("RGB", (256, 128))
    px = img.load()
    for y in range(128):
        t = y / 127.0
        px_row = (int(10 + 14 * t), int(11 + 18 * t), int(22 + 30 * t))
        for x in range(256):
            px[x, y] = px_row
    rng = random.Random(58)
    for _ in range(70):
        x, y = rng.randrange(256), rng.randrange(128)
        v = rng.randrange(90, 200)
        px[x, y] = (v, v, min(255, v + 20))
    os.makedirs(os.path.dirname(out_png), exist_ok=True)
    img.save(out_png)


def read_data_file(rel, provider_root, mods_dir, enabled):
    """Bytes of a data-relative file: the provider mod first (loose, then its
    BSAs), then vanilla BSAs, then any mod's loose copy, newest priority
    first. None when nowhere."""
    def loose(root):
        p = os.path.join(root, rel)
        if os.path.isfile(p):
            with open(p, "rb") as f:
                return f.read()
        return None

    def in_bsas(root):
        try:
            names = [n for n in os.listdir(root) if n.lower().endswith(".bsa")]
        except OSError:
            return None
        for n in names:
            try:
                hits = read_bsa(os.path.join(root, n), rel.lower())
                if hits:
                    return hits[0][3]
            except Exception:
                pass
        return None

    roots = []
    if provider_root:
        roots.append(provider_root)
    for r in roots:
        data = loose(r) or in_bsas(r)
        if data is not None:
            return data
    data = in_bsas(GAME_DATA)
    if data is not None:
        return data
    for mod in reversed(enabled):
        data = loose(os.path.join(mods_dir, mod))
        if data is not None:
            return data
    return loose(GAME_DATA)


def bake_thumb(dds_bytes, out_png):
    """BC-decode via Texconv, center-crop to 2:1, save a 256x128 PNG.

    False when either optional piece is absent, which the caller already treats
    as "this dome gets the generic tile" rather than as a failure.
    """
    if not TEXCONV or not have_pillow():
        return False
    from PIL import Image
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "t.dds")
        with open(src, "wb") as f:
            f.write(dds_bytes)
        r = subprocess.run([TEXCONV, "-ft", "png", "-y", "-o", td, src],
                           capture_output=True)
        png = os.path.join(td, "t.png")
        if r.returncode != 0 or not os.path.exists(png):
            return False
        img = Image.open(png)
        if img.mode in ("RGBA", "LA"):
            # Sky textures are often additive alpha wisps; flattened onto
            # white they vanish. Black is what they draw over in the sky.
            base = Image.new("RGBA", img.size, (0, 0, 0, 255))
            img = Image.alpha_composite(base, img.convert("RGBA"))
        img = img.convert("RGB")
        w, h = img.size
        if w >= 2 * h:
            box = ((w - 2 * h) // 2, 0, (w - 2 * h) // 2 + 2 * h, h)
        else:
            box = (0, (h - w // 2) // 2, w, (h - w // 2) // 2 + w // 2)
        os.makedirs(os.path.dirname(out_png), exist_ok=True)
        img.crop(box).resize((256, 128), Image.LANCZOS).save(out_png)
        return True


def pretty_name(rel, seen_names):
    stem = rel.rsplit("\\", 1)[-1][:-4]
    stem = re.sub(r"[_\-]+", " ", stem).strip()
    stem = re.sub(r"\b(nif|msn|sm)\b", "", stem, flags=re.I).strip()
    name = " ".join(w.capitalize() for w in stem.split()) or "Sky"
    base, n = name, 2
    while name.lower() in seen_names:
        name = "%s %d" % (base, n)
        n += 1
    seen_names.add(name.lower())
    return name


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mo2-ini", help="ModOrganizer.ini, if it is not found")
    ap.add_argument("--out", help="mod-folder root to write into "
                    "(default: the Menu Studio mod in your MO2 mods folder)")
    ap.add_argument("--texconv", help="texconv.exe, if it is not on PATH")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    global TEXCONV
    ini = find_mo2_ini(args.mo2_ini)
    profile_dir, mods_dir = resolve_mo2(ini)
    TEXCONV = find_texconv(args.texconv)

    # ⚠ THE DEFAULT IS DERIVED, NOT REMEMBERED. Writing into whichever mod
    # folder MO2 says Menu Studio lives in is the only answer that is right on
    # a machine this script has never seen.
    if not args.out:
        args.out = os.path.join(mods_dir, "Menu Studio")
        if not os.path.isdir(args.out):
            sys.exit("no 'Menu Studio' mod folder at %s\n"
                     "Install Menu Studio first, or name a folder with --out."
                     % args.out)

    print("ini      : %s" % ini)
    print("profile  : %s" % profile_dir)
    print("game data: %s" % GAME_DATA)
    print("writing  : %s" % args.out)
    if not have_pillow():
        print("NOTE: Pillow is not installed, so the cards will show names "
              "rather than pictures. `pip install pillow` and re-run to get them.")
    elif not TEXCONV:
        print("NOTE: texconv is not on PATH, so the cards fall back to the "
              "generic tile. Put DirectXTex's texconv.exe on PATH, or pass "
              "--texconv, and re-run to get each sky's own picture.")

    enabled = enabled_mods(profile_dir)
    found = {}  # rel path (lower) -> (rel, provenance, provider root)
    for rel, prov in scan_mod_dir(GAME_DATA):
        found[rel.lower()] = (rel, "vanilla " + prov, GAME_DATA)
    for mod in enabled:
        root = os.path.join(mods_dir, mod)
        if os.path.isdir(root):
            for rel, prov in scan_mod_dir(root):
                found[rel.lower()] = (rel, "%s (%s)" % (mod, prov), root)

    domes = sorted(found.values(), key=lambda t: t[0].lower())
    # The four domes the shipped presets already are add nothing as cards.
    shipped = {"meshes\\interface\\intperkskydome.nif",
               "meshes\\dlc01\\interface\\intvampireperkskydome.nif",
               "meshes\\interface\\teatperkskydome.nif",
               "meshes\\mtb\\voidimage.nif", "meshes\\mtb\\voidcolor.nif",
               "meshes\\mtb\\voidshell.nif"}
    domes = [d for d in domes if d[0].lower() not in shipped]

    print("%d sky dome(s):" % len(domes))
    for rel, prov, _root in domes:
        print("  %-58s <- %s" % (rel, prov))
    if args.dry_run:
        return

    seen = set()
    lines = ["; GENERATED by tools/harvest_backdrops.py - re-run it after the",
             "; load order changes. Hand edits are overwritten.",
             "",
             "[Pack]",
             "name=Load order skies",
             "author=your load order",
             "group=Load order skies",
             ""]
    for i, (rel, prov, root) in enumerate(domes):
        sec = "Background" if i == 0 else "Background%d" % i
        dome = rel[len("meshes\\"):] if rel.lower().startswith("meshes\\") else rel
        name = pretty_name(rel, seen)
        lines += ["[%s]" % sec,
                  "name=%s" % name,
                  "; from %s" % prov,
                  "dome=%s" % dome]
        # The dome's own texture becomes the card picture.
        thumb_rel = None
        nif = read_data_file(rel, root, mods_dir, enabled)
        tex = nif_first_texture(nif) if nif else None
        if tex:
            dds = read_data_file(tex, root, mods_dir, enabled)
            if dds:
                slug = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
                out_png = os.path.join(args.out, "textures", "mtb", "backdrops",
                                       "skies", slug + ".png")
                if bake_thumb(dds, out_png):
                    # ⚠ ESCAPED, NOT PRETTY. This exact literal shipped once as
                    # "mtb\backdrops\..." and Python read \b as a BACKSPACE:
                    # every thumb= line carried an invisible 0x08, no path ever
                    # resolved, and all 16 baked thumbnails sat unused while
                    # the picker drew named tiles. The INI is bytes, not a
                    # picture of itself; od -c is what caught it.
                    thumb_rel = "mtb\\backdrops\\skies\\" + slug + ".png"
        if not thumb_rel:
            # Every card gets a picture (user 2026-08-21): a shared generic
            # night-sky tile stands in when the dome offers nothing.
            thumb_rel = "mtb\\backdrops\\skies\\placeholder.png"
            print("  (placeholder card for %s: texture %s)"
                  % (name, tex or "not found"))
        lines.append("thumb=%s" % thumb_rel)
        lines.append("")
    # ⚠ WITHOUT PILLOW THERE IS NO TILE TO DRAW, and a thumb= line pointing at
    # a picture nobody wrote is worse than no line: the card would draw blank
    # instead of falling back to its name. So the lines come out again.
    if have_pillow():
        draw_placeholder(os.path.join(args.out, "textures", "mtb", "backdrops",
                                      "skies", "placeholder.png"))
    else:
        lines = [l for l in lines if not l.startswith("thumb=")]
    ini_path = os.path.join(args.out, "SKSE", "Plugins", "MenuStudio",
                            "Backdrops", "harvested_skies.ini")
    os.makedirs(os.path.dirname(ini_path), exist_ok=True)
    with io.open(ini_path, "w", newline="\r\n") as f:
        f.write("\n".join(lines))
    print("wrote %s (%d backgrounds)" % (ini_path, len(domes)))


if __name__ == "__main__":
    main()
