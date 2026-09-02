#!/usr/bin/env python3
"""Turn one equirectangular image into a complete Menu Studio backdrop pack.

Input: a Radiance .hdr (Poly Haven's download format, read natively here), or
any LDR image PIL opens (.png/.jpg/.tga). EXR needs `pip install imageio` plus
its EXR plugin; the script says so if you hand it one without.

The image sphere's UVs are equirect (make_voidshell.py authored them), so the
whole pipeline is texture-side: tone-map to 8-bit, resize to a sane width,
compress to BC7 with a full mip chain (the r58 lesson: a large texture without
BC + mips AVs the d3d11 draw), and write the pack manifest next to it.

Output layout (a ready mod folder, point --out at a test mod or merge it):
    <out>/textures/mtb/backdrops/<id>.dds
    <out>/SKSE/Plugins/MenuStudio/Backdrops/<id>.ini

Usage:
    python tools/hdri_to_pack.py sky.hdr --name "Autumn Field" --out dist
    python tools/hdri_to_pack.py --selftest --out dist   # synthetic test image

Tone mapping: exposure (--ev stops) then Reinhard x/(1+x) per channel, then
gamma 2.2. Flat and predictable; judge brightness in game, not here - the
sphere renders through the game's own tonemap and usually reads darker than a
desktop viewer.
"""

import argparse
import math
import os
import re
import struct
import subprocess
import sys

import numpy as np

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from make_voidshell import TEXCONV  # the one spelling of the Texconv path

MAX_WIDTH = 4096


def read_hdr(path):
    """Radiance RGBE (.hdr) reader, pure numpy. Returns float32 (H,W,3)."""
    with open(path, "rb") as f:
        magic = f.readline()
        if not magic.startswith(b"#?"):
            raise ValueError("not a Radiance .hdr file")
        while True:
            line = f.readline()
            if not line:
                raise ValueError("truncated .hdr header")
            if line.strip() == b"":
                break
        dims = f.readline().split()
        if len(dims) != 4 or dims[0] != b"-Y" or dims[2] != b"+X":
            raise ValueError("unsupported .hdr orientation %r" % (dims,))
        h, w = int(dims[1]), int(dims[3])
        data = f.read()

    rgbe = np.zeros((h, w, 4), np.uint8)
    pos = 0
    for y in range(h):
        if pos + 4 <= len(data) and data[pos] == 2 and data[pos + 1] == 2:
            # adaptive RLE scanline
            pos += 4
            for c in range(4):
                x = 0
                while x < w:
                    count = data[pos]; pos += 1
                    if count > 128:                      # run
                        rgbe[y, x:x + count - 128, c] = data[pos]
                        pos += 1
                        x += count - 128
                    else:                                # literal
                        rgbe[y, x:x + count, c] = np.frombuffer(
                            data[pos:pos + count], np.uint8)
                        pos += count
                        x += count
        else:                                            # flat scanline
            row = np.frombuffer(data[pos:pos + w * 4], np.uint8).reshape(w, 4)
            rgbe[y] = row
            pos += w * 4

    exp = rgbe[..., 3].astype(np.int32)
    scale = np.where(exp == 0, 0.0, np.ldexp(1.0, exp - 136))  # 2^(e-128)/256
    return (rgbe[..., :3].astype(np.float32) * scale[..., None]).astype(np.float32)


def read_any(path):
    """Returns (float32 linear-ish (H,W,3), was_hdr)."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".hdr":
        return read_hdr(path), True
    if ext == ".exr":
        # cv2 first: this environment's imageio route detours into pyav and
        # dies there, while opencv reads EXR directly (BGR, hence the flip).
        try:
            os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
            import cv2
            img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
            if img is not None:
                return img[..., 2::-1].astype(np.float32), True
        except ImportError:
            pass
        try:
            import imageio.v3 as iio
            return iio.imread(path).astype(np.float32)[..., :3], True
        except Exception as e:
            raise SystemExit("EXR needs opencv or imageio (pip install "
                             "opencv-python): %s" % e)
    from PIL import Image
    img = np.asarray(Image.open(path).convert("RGB"), np.float32) / 255.0
    return (img ** 2.2).astype(np.float32), False       # undo display gamma


def apply_cc(x, cc):
    """Colour correction in display space (after the gamma encode), applied
    in this fixed order: black point -> gamma -> contrast -> saturation.
    The order matters to anyone fitting these values from a chart shot, so
    keep it stable. Identity values (0, 1, 1, 1) leave the array untouched
    and the byte-identical output proves it.

    The point of these is to pre-compensate the in-game wash: the render
    pipeline lays an additive haze veil over the sphere and bends effect
    colours through its own curve, so the bake steepens the image by the
    inverse amount. Fit the numbers from the CC Chart pack, do not guess.
    """
    black, gamma, contrast, sat = cc
    if black > 0.0:
        x = np.clip((x - black) / (1.0 - black), 0.0, 1.0)
    if gamma != 1.0:
        x = np.clip(x, 0.0, 1.0) ** gamma
    if contrast != 1.0:
        x = np.clip(0.5 + (x - 0.5) * contrast, 0.0, 1.0)
    if sat != 1.0:
        luma = (x * np.array([0.2126, 0.7152, 0.0722], np.float32)).sum(
            axis=-1, keepdims=True)
        x = np.clip(luma + (x - luma) * sat, 0.0, 1.0)
    return x


CC_IDENTITY = (0.0, 1.0, 1.0, 1.0)


def tone_map(linear, ev, cc=CC_IDENTITY):
    """exposure -> Reinhard -> gamma 2.2 -> CC -> uint8 (H,W,3)."""
    x = linear * (2.0 ** ev)
    x = x / (1.0 + x)
    x = np.clip(x, 0.0, 1.0) ** (1.0 / 2.2)
    x = apply_cc(x, cc)
    return (x * 255.0 + 0.5).astype(np.uint8)


def resize_half(img):
    """Box-filter halving (even dims assumed after the pad below)."""
    h, w = img.shape[:2]
    return img.reshape(h // 2, 2, w // 2, 2, -1).mean(axis=(1, 3))


def fit(linear):
    """Pad to even, halve until width <= MAX_WIDTH; warn when not 2:1."""
    h, w = linear.shape[:2]
    if abs(w / h - 2.0) > 0.02:
        print("  WARNING: %dx%d is not 2:1 equirect - the sphere will stretch it."
              % (w, h))
    while linear.shape[1] > MAX_WIDTH:
        h, w = linear.shape[:2]
        if h % 2 or w % 2:
            linear = linear[:h - h % 2, :w - w % 2]
        linear = resize_half(linear)
    return linear


def make_selftest(w=2048, h=1024):
    """Synthetic HDR equirect: sky gradient, a hot sun disc (HDR range), a
    horizon band, and a red meridian stripe at yaw 0 so the seam/framing is
    checkable in game. Runs the exact pipeline a real HDRI takes."""
    v = np.linspace(0.0, 1.0, h)[:, None]               # 0 top .. 1 bottom
    u = np.linspace(0.0, 1.0, w)[None, :]
    img = np.zeros((h, w, 3), np.float32)
    # sky: zenith blue to pale horizon; ground: dark warm brown
    sky = np.clip(1.0 - v * 2.0, 0.0, 1.0)
    img[..., 0] = 0.10 + 0.55 * (1.0 - sky)
    img[..., 1] = 0.18 + 0.50 * (1.0 - sky)
    img[..., 2] = 0.45 + 0.35 * (1.0 - sky)
    ground = (v[:, :, 0] if v.ndim == 3 else v) > 0.5
    ground = np.broadcast_to(ground, (h, w))
    img[ground] = [0.08, 0.055, 0.04]
    # horizon glow band
    band = np.exp(-((v - 0.5) ** 2) / (2 * 0.015 ** 2))
    img += band[..., None] * np.array([0.9, 0.65, 0.35]) * 0.8
    # sun disc at u=0.25, elevation 30 deg (v=1/3): HDR-hot
    du = (u - 0.25) * 2.0
    dv = (v - 1.0 / 3.0)
    disc = (du * du + dv * dv) < 0.0006
    img[np.broadcast_to(disc, (h, w))] = [60.0, 55.0, 45.0]
    # red meridian stripe at yaw 0 (u=0/1, the UV seam)
    stripe = (u < 0.004) | (u > 0.996)
    img[np.broadcast_to(stripe & (v < 0.5), (h, w))] = [2.5, 0.05, 0.05]
    return img


CHART_GRAYS = [0, 16, 32, 48, 64, 96, 128, 160, 192, 224, 240, 255]
CHART_COLORS = [(255, 0, 0), (0, 255, 0), (0, 0, 255),
                (0, 255, 255), (255, 0, 255), (255, 255, 0)]


def make_chart(w=2048, h=1024):
    """Calibration chart, authored directly in display space: the DDS holds
    these exact sRGB values, no tone map, no CC. Photograph it in the menu,
    read the patches off the screenshot, and the difference between encoded
    and seen IS the pipeline's wash. Fit --black/--gamma/--contrast/--sat
    from that, then re-bake the real packs.

    The tile repeats four times around the sphere so a patch band faces the
    camera at every yaw. Rows, top to bottom: gray ramp (CHART_GRAYS),
    colours at full, colours at half, anchor patches (0/255/128/64/192/118).
    """
    img = np.full((h, w, 3), 32, np.uint8)
    tile_w = w // 4
    band_t, band_b = int(h * 0.30), int(h * 0.70)
    rows = 4
    row_h = (band_b - band_t) // rows

    def patch_row(y0, y1, x0, values, tile):
        n = len(values)
        pw = (tile - 4) // n
        for i, val in enumerate(values):
            px = x0 + 2 + i * pw
            img[y0 + 2:y1 - 2, px + 1:px + pw - 1] = val

    for t in range(4):
        x0 = t * tile_w
        img[band_t:band_b, x0:x0 + tile_w] = 0            # separators stay black
        patch_row(band_t, band_t + row_h, x0, CHART_GRAYS, tile_w)
        patch_row(band_t + row_h, band_t + 2 * row_h, x0, CHART_COLORS, tile_w)
        half = [tuple(c // 2 for c in col) for col in CHART_COLORS]
        patch_row(band_t + 2 * row_h, band_t + 3 * row_h, x0, half, tile_w)
        patch_row(band_t + 3 * row_h, band_b, x0,
                  [0, 255, 128, 64, 192, 118], tile_w)
        img[band_t:band_t + 1, x0:x0 + tile_w] = 255      # thin white frame
        img[band_b - 1:band_b, x0:x0 + tile_w] = 255
    return img


def compress(png_path, dds_path):
    # ⚠ BC7_UNORM_SRGB, not BC7_UNORM. The PNG is gamma-encoded; under
    # Community Shaders' linear lighting an unflagged texture is read as
    # LINEAR data, which lifts every midtone into a pale wash that no
    # brightness scalar can undo (field 2026-08-20, three rounds to pin).
    # The sRGB flag makes the sampler decode, so the pipeline sees the
    # image the author saw.
    import shutil
    exe = TEXCONV if os.path.exists(TEXCONV) else shutil.which("texconv")
    if not exe:
        raise SystemExit("Texconv not found - run by hand:\n"
                         "  texconv -f BC7_UNORM_SRGB -m 0 -y -o %s %s"
                         % (os.path.dirname(dds_path), png_path))
    outdir = os.path.dirname(dds_path)
    os.makedirs(outdir, exist_ok=True)
    subprocess.run([exe, "-f", "BC7_UNORM_SRGB", "-m", "0", "-y", "-o", outdir,
                    png_path], check=True, capture_output=True)
    made = os.path.join(outdir, os.path.splitext(os.path.basename(png_path))[0]
                        + ".DDS")
    for candidate in (made, made[:-4] + ".dds"):
        if os.path.exists(candidate) and candidate != dds_path:
            os.replace(candidate, dds_path)
            break
    if not os.path.exists(dds_path):
        raise SystemExit("Texconv ran but %s is missing" % dds_path)


def pack_id(name):
    s = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    return s or "backdrop"


# ---- reflection cubemap ----------------------------------------------------
# Baked next to the image as <id>_cube.dds; Menu Studio hands it to Community
# Shaders' cubemap override so armor reflections agree with the backdrop.
# Face conventions mirror CS's GetSamplingVector exactly (uv = 2*(s,1-t)-1,
# D3D face order, texel = scene along MINUS the sampling vector - the capture
# stores it that way). World is Z-up; equirect u/v match the sphere mesh
# (u = lon/2pi from +X, v = lat/pi from +Z). Values are LINEAR (exposure +
# Reinhard, no gamma) so CS's float pipeline reads them as scene light.

CUBE_EDGE = 256

# The faces carry the SAME tone-mapped image the sphere shows, in the same
# gamma space - CS's sampling applies IrradianceToLinear to cubemap content,
# so gamma-encoded data is what it expects, and the patched override branch
# (absolute radiance, no normalize-by-average) reads it straight. Bright
# 0..1 data also keeps BC6H quantization noise down; the first linear bake
# sat near black and the encoder's blocks read as an oil slick.


def equirect_to_cube(linear, ev, cc=CC_IDENTITY):
    h, w = linear.shape[:2]
    x = linear * (2.0 ** ev)
    x = (np.clip(x / (1.0 + x), 0.0, 1.0) ** (1.0 / 2.2)).astype(np.float32)
    x = apply_cc(x, cc).astype(np.float32)  # faces match the corrected sphere

    e = CUBE_EDGE
    st = (np.arange(e, dtype=np.float32) + 0.5) / e
    sx, ty = np.meshgrid(st, st)
    u = 2.0 * sx - 1.0
    v = 2.0 * (1.0 - ty) - 1.0
    one = np.ones_like(u)
    faces_s = [
        np.stack([one, v, -u], -1),      # +X
        np.stack([-one, v, u], -1),      # -X
        np.stack([u, one, -v], -1),      # +Y
        np.stack([u, -one, v], -1),      # -Y
        np.stack([u, v, one], -1),       # +Z
        np.stack([-u, v, -one], -1),     # -Z
    ]
    out = np.zeros((6, e, e, 3), np.float32)
    for i, s in enumerate(faces_s):
        d = -s / np.linalg.norm(s, axis=-1, keepdims=True)   # scene along -S
        lon = np.mod(np.arctan2(d[..., 1], d[..., 0]), 2.0 * np.pi)
        lat = np.arccos(np.clip(d[..., 2], -1.0, 1.0))
        fx = lon / (2.0 * np.pi) * w - 0.5
        fy = lat / np.pi * h - 0.5
        x0 = np.floor(fx).astype(np.int64); x1 = (x0 + 1) % w
        y0 = np.clip(np.floor(fy).astype(np.int64), 0, h - 1)
        y1 = np.clip(y0 + 1, 0, h - 1)
        wx = (fx - np.floor(fx))[..., None]; wy = (fy - np.floor(fy))[..., None]
        x0 = np.mod(x0, w)
        out[i] = ((x[y0, x0] * (1 - wx) + x[y0, x1] * wx) * (1 - wy) +
                  (x[y1, x0] * (1 - wx) + x[y1, x1] * wx) * wy)
    return out


def write_cube_dds(path, faces):
    """DX10 cubemap DDS, R16G16B16A16_FLOAT, single mip, all six faces."""
    e = faces.shape[1]
    DDSD = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000
    header = struct.pack(
        "<4s7I44x8I5I",
        b"DDS ", 124, DDSD, e, e, e * 8, 0, 0,
        32, 0x4, 0x30315844, 0, 0, 0, 0, 0,   # DDPF_FOURCC "DX10"
        0x1008, 0xFE00, 0, 0, 0)              # caps TEXTURE|COMPLEX, caps2 cube+faces
    dx10 = struct.pack("<5I", 10, 3, 0x4, 1, 0)  # RGBA16F, TEXTURE2D, CUBE, array 1
    rgba = np.zeros((6, e, e, 4), np.float16)
    rgba[..., :3] = faces.astype(np.float16)
    rgba[..., 3] = 1.0
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header)
        f.write(dx10)
        f.write(np.ascontiguousarray(rgba).tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", nargs="?", help=".hdr/.exr/.png/.jpg equirect")
    ap.add_argument("--name", help="pack name shown in the panel")
    ap.add_argument("--author", default="", help="author= line (put the HDRI "
                    "source here even for CC0)")
    ap.add_argument("--ev", type=float, default=0.0, help="exposure in stops "
                    "before tone mapping (default 0)")
    ap.add_argument("--out", default="dist", help="mod-folder root to write "
                    "into (default dist)")
    ap.add_argument("--selftest", action="store_true",
                    help="ignore inputs, build the synthetic test pack")
    ap.add_argument("--chart", action="store_true",
                    help="build the CC calibration chart pack (exact sRGB "
                         "values, no tone map, no CC; local use, never ship)")
    ap.add_argument("--black", type=float, default=0.0,
                    help="CC black point 0..0.5 (default 0 = off)")
    ap.add_argument("--gamma", type=float, default=1.0,
                    help="CC midtone gamma, >1 darkens mids (default 1)")
    ap.add_argument("--contrast", type=float, default=1.0,
                    help="CC linear pivot around 0.5 (default 1)")
    ap.add_argument("--sat", type=float, default=1.0,
                    help="CC saturation about Rec.709 luma (default 1)")
    ap.add_argument("--radius", type=int, default=0,
                    help="write radius= into the manifest (game clamps "
                         "500..1200; 0 = omit, game default 800)")
    args = ap.parse_args()
    cc = (args.black, args.gamma, args.contrast, args.sat)

    if args.chart:
        name = args.name or "CC Chart"
        mapped = make_chart()
        author = args.author or "Menu Studio (calibration)"
        pid = pack_id(name)
        h, w = mapped.shape[:2]
        print("%s: %dx%d chart -> BC7 (exact values, CC bypassed)" % (pid, w, h))
        write_pack(args, pid, name, author, mapped, linear=None, cc=CC_IDENTITY)
        return

    if args.selftest:
        name = args.name or "HDRI Sample"
        linear, was_hdr = make_selftest(), True
        author = args.author or "Menu Studio (synthetic)"
    else:
        if not args.image:
            ap.error("an image is required (or --selftest)")
        name = args.name or os.path.splitext(os.path.basename(args.image))[0]
        linear, was_hdr = read_any(args.image)
        author = args.author

    pid = pack_id(name)
    linear = fit(linear)
    mapped = tone_map(linear, args.ev, cc)
    h, w = mapped.shape[:2]
    print("%s: %dx%d %s -> BC7+mips" % (pid, w, h, "HDR" if was_hdr else "LDR"))
    if cc != CC_IDENTITY:
        print("  CC: black=%g gamma=%g contrast=%g sat=%g" % cc)
    write_pack(args, pid, name, author, mapped, linear, cc)


def write_pack(args, pid, name, author, mapped, linear, cc):
    """DDS + thumb + (optionally) cube + manifest. `linear` None skips the
    reflection cube (the chart has no scene light to reflect)."""
    from PIL import Image
    tex_rel = os.path.join("mtb", "backdrops", pid + ".dds")
    dds_path = os.path.join(args.out, "textures", tex_rel)
    png_path = os.path.join(args.out, "textures", "mtb", "backdrops", pid + ".png")
    os.makedirs(os.path.dirname(png_path), exist_ok=True)
    img8 = Image.fromarray(mapped, "RGB")
    img8.save(png_path)
    compress(png_path, dds_path)
    # The card picker's thumbnail: FUCK's image loader reads PNG, not DDS.
    thumb_path = dds_path[:-4] + "_thumb.png"
    img8.resize((256, 128), Image.LANCZOS).save(thumb_path)
    os.remove(png_path)                                  # the DDS is the artifact

    cube_path = None
    if linear is not None:
        cube_path = dds_path[:-4] + "_cube.dds"
        write_cube_dds(cube_path, equirect_to_cube(linear, args.ev, cc))

    ini_path = os.path.join(args.out, "SKSE", "Plugins", "MenuStudio",
                            "Backdrops", pid + ".ini")
    os.makedirs(os.path.dirname(ini_path), exist_ok=True)
    lines = ["[Pack]", "name=" + name]
    if author:
        lines.append("author=" + author)
    lines += ["", "[Background]", "image=" + tex_rel.replace(os.sep, "\\")]
    if args.radius:
        lines.append("radius=%d" % args.radius)
    lines.append("")
    with open(ini_path, "w", newline="\r\n") as f:
        f.write("\n".join(lines))

    print("  %s (%d bytes)" % (dds_path, os.path.getsize(dds_path)))
    if cube_path:
        print("  %s (reflections)" % cube_path)
    print("  %s" % ini_path)
    print("Pick '%s' in the backdrop panel to see it." % name)


if __name__ == "__main__":
    main()
