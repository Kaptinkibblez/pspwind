#!/usr/bin/env python3
"""tex_demaker - Morrowind textures -> crunchy PSP paletted textures.

Pipeline per texture:
  1. read DDS/TGA/BMP from the layered Data Files view (loose > BSA)
  2. pick a power-of-two target (64x64 world, 32x32 small/detail, custom
     sizes for UI prefixes) and downscale with nearest-neighbour sampling
  3. classify alpha: opaque / cutout (1-bit, palette index 0 transparent) /
     blend (graded alpha kept in the palette)
  4. quantise to 16 colours (GU_PSM_T4) when that keeps PSNR above the
     threshold, otherwise 256 colours (GU_PSM_T8)
  5. build box-filtered mip levels mapped onto the same palette
  6. swizzle every level into GE 16x8-byte block order
  7. emit a DTEX (src/fmt.h) into textures.dpak

Inventory icons are packed into shared 256x256 T8 atlas pages (DATL index).

Usage:
  tex_demaker.py --data "/path/Morrowind/Data Files" --out build/cooked
"""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
import time
from dataclasses import asdict, dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import dmcommon  # noqa: E402
from dmcommon import LOG, BinWriter, DataConfig, DataSource, fnv1a32  # noqa: E402

# --------------------------------------------------------------------------
# constants mirrored from src/fmt.h
# --------------------------------------------------------------------------
DTEX_MAGIC = b"DTEX"
DTEX_VERSION = 1
DTEX_MAX_LEVELS = 4
DTEX_PSM_T4 = 4
DTEX_PSM_T8 = 5
DTEX_F_SWIZZLED = 1 << 0
DTEX_F_ALPHA_TEST = 1 << 1
DTEX_F_ALPHA_BLEND = 1 << 2
DTEX_F_ATLAS_PAGE = 1 << 3
DTEX_HEADER_SIZE = 96
DTEX_HEADER_FMT = "<4sHBBHHBBHII"  # 24 bytes, then 4 levels, then name_hash, reserved
DTEX_LEVEL_FMT = "<HHHHII"  # 16 bytes

DATL_MAGIC = b"DATL"
DATL_VERSION = 1

ALPHA_OPAQUE = "opaque"
ALPHA_CUTOUT = "cutout"
ALPHA_BLEND = "blend"

DEFAULT_SIZE_RULES = (
    ("splash/", 256),
    ("levelup/", 128),
    ("bookart/", 128),
    ("menu_", 128),
    ("tx_menubook", 128),
    ("birthsigns/", 128),
)


@dataclass
class TexConfig:
    size: int = 64                 # world texture max dimension
    small_size: int = 32           # for small sources
    large_threshold: int = 256     # sources >= this get `size`, smaller get `small_size`
    colors: str = "auto"           # auto | 16 | 256
    t4_psnr: float = 30.0          # dB needed to accept 16 colours in auto mode
    filter: str = "nearest"        # level-0 filter: nearest | box
    dither: bool = False
    mips: bool = True
    max_levels: int = 3
    swizzle: bool = True
    alpha_ref: int = 128
    size_rules: Tuple[Tuple[str, int], ...] = DEFAULT_SIZE_RULES


# --------------------------------------------------------------------------
# geometry helpers
# --------------------------------------------------------------------------


def pow2_floor(v: float) -> int:
    v = max(1, int(v))
    return 1 << (v.bit_length() - 1)


def min_width(psm: int) -> int:
    """Smallest width whose row is 16 bytes (swizzle block width)."""
    return 32 if psm == DTEX_PSM_T4 else 16


def target_size(w: int, h: int, max_dim: int) -> Tuple[int, int]:
    """Power-of-two size with the longest side == max_dim (or the source size
    if smaller), aspect preserved, clamped to swizzle-friendly minima."""
    max_dim = pow2_floor(max_dim)
    longest = max(w, h)
    cap = min(max_dim, pow2_floor(longest)) if longest >= 8 else 8
    scale = cap / float(longest)
    tw = pow2_floor(max(1, round(w * scale)))
    th = pow2_floor(max(1, round(h * scale)))
    tw = max(32, min(tw, 512))
    th = max(8, min(th, 512))
    return tw, th


def max_dim_for(key: str, src_w: int, src_h: int, cfg: TexConfig) -> int:
    for prefix, size in cfg.size_rules:
        if key.startswith(prefix):
            return size
    return cfg.size if max(src_w, src_h) >= cfg.large_threshold else cfg.small_size


def resize(img: Image.Image, w: int, h: int, flt: str) -> Image.Image:
    if img.size == (w, h):
        return img.copy()
    method = Image.NEAREST if flt == "nearest" else Image.BOX
    return img.resize((w, h), method)


# --------------------------------------------------------------------------
# alpha + quantisation
# --------------------------------------------------------------------------


def analyze_alpha(img: Image.Image, alpha_ref: int = 128) -> str:
    alpha = img.getchannel("A")
    lo, hi = alpha.getextrema()
    if lo >= 250:
        return ALPHA_OPAQUE
    hist = alpha.histogram()
    total = img.size[0] * img.size[1]
    mid = sum(hist[9:247])
    # mostly binary alpha (foliage, railings) -> PS1 style cutout
    if mid <= total * 0.08:
        return ALPHA_CUTOUT
    return ALPHA_BLEND


Color = Tuple[int, int, int, int]


def _mean_opaque_rgb(img: Image.Image, alpha_ref: int) -> Tuple[int, int, int]:
    px = dmcommon.image_pixels(img)
    r = g = b = n = 0
    for (pr, pg, pb, pa) in px:
        if pa >= alpha_ref:
            r += pr
            g += pg
            b += pb
            n += 1
    if n == 0:
        return (0, 0, 0)
    return (r // n, g // n, b // n)


def _palette_from(pimg: Image.Image, rawmode: str, count: int) -> List[Color]:
    flat = pimg.getpalette(rawmode) or []
    step = len(rawmode)
    pal: List[Color] = []
    for i in range(0, min(len(flat), count * step), step):
        c = flat[i:i + step]
        pal.append((c[0], c[1], c[2], c[3] if step == 4 else 255))
    return pal


def quantize(img: Image.Image, ncolors: int, mode: str, alpha_ref: int
             ) -> Tuple[bytes, List[Color]]:
    """Return (index per pixel, palette) with len(palette) <= ncolors."""
    if mode == ALPHA_OPAQUE:
        rgb = img.convert("RGB")
        pimg = rgb.quantize(colors=ncolors, method=Image.Quantize.MEDIANCUT,
                            dither=Image.Dither.NONE)
        pal = _palette_from(pimg, "RGB", ncolors)
        return pimg.tobytes(), pal
    if mode == ALPHA_CUTOUT:
        fill = _mean_opaque_rgb(img, alpha_ref)
        alpha = dmcommon.image_pixels(img.getchannel("A"))
        solid = Image.new("RGB", img.size, fill)
        solid.paste(img.convert("RGB"), mask=img.getchannel("A").point(
            lambda a: 255 if a >= alpha_ref else 0))
        pimg = solid.quantize(colors=ncolors - 1, method=Image.Quantize.MEDIANCUT,
                              dither=Image.Dither.NONE)
        pal = [(fill[0], fill[1], fill[2], 0)] + _palette_from(pimg, "RGB", ncolors - 1)
        idx = bytes(0 if a < alpha_ref else i + 1
                    for i, a in zip(pimg.tobytes(), alpha))
        return idx, pal
    pimg = img.quantize(colors=ncolors, method=Image.Quantize.FASTOCTREE,
                        dither=Image.Dither.NONE)
    pal = _palette_from(pimg, "RGBA", ncolors)
    return pimg.tobytes(), pal


def remap(img: Image.Image, pal: Sequence[Color], mode: str, alpha_ref: int,
          dither: bool) -> bytes:
    """Map pixels onto an existing palette (nearest colour, optional
    Floyd-Steinberg error diffusion on RGB)."""
    w, h = img.size
    px = dmcommon.image_pixels(img)
    use_alpha = mode == ALPHA_BLEND
    first = 1 if mode == ALPHA_CUTOUT else 0
    cand = list(range(first, len(pal)))
    cache: Dict[Tuple[int, ...], int] = {}

    def nearest(r: int, g: int, b: int, a: int) -> int:
        key = (r, g, b, a) if use_alpha else (r, g, b)
        hit = cache.get(key)
        if hit is not None:
            return hit
        best, bd = cand[0] if cand else 0, 1 << 62
        for i in cand:
            pr, pg, pb, pa = pal[i]
            d = (pr - r) ** 2 + (pg - g) ** 2 + (pb - b) ** 2
            if use_alpha:
                d += (pa - a) ** 2
            if d < bd:
                bd, best = d, i
        cache[key] = best
        return best

    out = bytearray(w * h)
    if not dither:
        for i, (r, g, b, a) in enumerate(px):
            if mode == ALPHA_CUTOUT and a < alpha_ref:
                out[i] = 0
            else:
                out[i] = nearest(r, g, b, a)
        return bytes(out)

    err = [[0.0, 0.0, 0.0] for _ in range(w * h)]
    for y in range(h):
        for x in range(w):
            i = y * w + x
            r, g, b, a = px[i]
            if mode == ALPHA_CUTOUT and a < alpha_ref:
                out[i] = 0
                continue
            e = err[i]
            rr = max(0, min(255, int(round(r + e[0]))))
            gg = max(0, min(255, int(round(g + e[1]))))
            bb = max(0, min(255, int(round(b + e[2]))))
            c = nearest(rr, gg, bb, a)
            out[i] = c
            pr, pg, pb, _ = pal[c]
            dr, dg, db = rr - pr, gg - pg, bb - pb
            for dx, dy, wgt in ((1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and ny < h:
                    t = err[ny * w + nx]
                    t[0] += dr * wgt
                    t[1] += dg * wgt
                    t[2] += db * wgt
    return bytes(out)


def psnr(img: Image.Image, idx: bytes, pal: Sequence[Color], mode: str) -> float:
    """PSNR (dB) of the paletted reconstruction against the source."""
    px = dmcommon.image_pixels(img)
    se = 0
    n = 0
    for (r, g, b, a), i in zip(px, idx):
        pr, pg, pb, pa = pal[i] if i < len(pal) else (0, 0, 0, 255)
        if mode == ALPHA_CUTOUT and a < 128:
            continue
        se += (pr - r) ** 2 + (pg - g) ** 2 + (pb - b) ** 2
        if mode == ALPHA_BLEND:
            se += (pa - a) ** 2
        n += 4 if mode == ALPHA_BLEND else 3
    if n == 0 or se == 0:
        return 99.0
    mse = se / n
    return 10.0 * math.log10(255.0 * 255.0 / mse)


# --------------------------------------------------------------------------
# packing + swizzle
# --------------------------------------------------------------------------


def pack_indices(idx: bytes, psm: int) -> bytes:
    if psm == DTEX_PSM_T8:
        return bytes(idx)
    if len(idx) % 2:
        raise ValueError("T4 pixel count must be even")
    # first (left) pixel in the low nibble
    return bytes((idx[i] & 0xF) | ((idx[i + 1] & 0xF) << 4) for i in range(0, len(idx), 2))


def unpack_indices(data: bytes, psm: int, count: int) -> bytes:
    if psm == DTEX_PSM_T8:
        return bytes(data[:count])
    out = bytearray()
    for b in data:
        out.append(b & 0xF)
        out.append(b >> 4)
    return bytes(out[:count])


def swizzle(data: bytes, width_bytes: int, height: int) -> bytes:
    """Linear -> GE swizzled (16-byte x 8-row blocks, row-major blocks)."""
    if width_bytes % 16 or height % 8:
        raise ValueError(f"cannot swizzle {width_bytes}x{height}")
    out = bytearray(len(data))
    o = 0
    for by in range(0, height, 8):
        for bx in range(0, width_bytes, 16):
            for row in range(8):
                s = (by + row) * width_bytes + bx
                out[o:o + 16] = data[s:s + 16]
                o += 16
    return bytes(out)


def unswizzle(data: bytes, width_bytes: int, height: int) -> bytes:
    out = bytearray(len(data))
    o = 0
    for by in range(0, height, 8):
        for bx in range(0, width_bytes, 16):
            for row in range(8):
                d = (by + row) * width_bytes + bx
                out[d:d + 16] = data[o:o + 16]
                o += 16
    return bytes(out)


def bytes_per_row(width: int, psm: int) -> int:
    return width // 2 if psm == DTEX_PSM_T4 else width


@dataclass
class Level:
    width: int
    height: int
    indices: bytes  # one byte per pixel, linear


def build_dtex(levels: Sequence[Level], pal: Sequence[Color], psm: int, flags: int,
               alpha_ref: int, name_hash: int, do_swizzle: bool) -> bytes:
    if not 1 <= len(levels) <= DTEX_MAX_LEVELS:
        raise ValueError("bad level count")
    entries = 16 if psm == DTEX_PSM_T4 else 256
    if len(pal) > entries:
        raise ValueError("palette too large for PSM")
    w = BinWriter()
    w.reserve(DTEX_HEADER_SIZE)
    clut_ofs = w.align(16)
    for i in range(entries):
        r, g, b, a = pal[i] if i < len(pal) else (0, 0, 0, 0)
        w.pack("BBBB", r, g, b, a)  # ABGR8888 little endian: R in low byte
    level_recs = []
    for lv in levels:
        packed = pack_indices(lv.indices, psm)
        if do_swizzle:
            packed = swizzle(packed, bytes_per_row(lv.width, psm), lv.height)
        ofs = w.align(16)
        w.bytes(packed)
        level_recs.append((lv.width, lv.height, lv.width, 0, ofs, len(packed)))
    if do_swizzle:
        flags |= DTEX_F_SWIZZLED
    w.align(16)
    w.patch(0, DTEX_HEADER_FMT[1:], DTEX_MAGIC, DTEX_VERSION, psm, flags,
            levels[0].width, levels[0].height, len(levels), alpha_ref, entries,
            clut_ofs, len(w))
    for i in range(DTEX_MAX_LEVELS):
        rec = level_recs[i] if i < len(level_recs) else (0, 0, 0, 0, 0, 0)
        w.patch(24 + 16 * i, DTEX_LEVEL_FMT[1:], *rec)
    w.patch(88, "II", name_hash, 0)
    return w.getvalue()


def parse_dtex(data: bytes) -> dict:
    (magic, version, psm, flags, width, height, nlevels, alpha_ref, entries, clut_ofs,
     file_size) = struct.unpack_from(DTEX_HEADER_FMT, data, 0)
    if magic != DTEX_MAGIC or version != DTEX_VERSION:
        raise ValueError("not a DTEX v1")
    if file_size != len(data):
        raise ValueError("DTEX size mismatch")
    pal = [tuple(data[clut_ofs + 4 * i: clut_ofs + 4 * i + 4]) for i in range(entries)]
    levels = []
    for i in range(nlevels):
        lw, lh, bw, _r, ofs, size = struct.unpack_from(DTEX_LEVEL_FMT, data, 24 + 16 * i)
        raw = data[ofs:ofs + size]
        if flags & DTEX_F_SWIZZLED:
            raw = unswizzle(raw, bytes_per_row(bw, psm), lh)
        levels.append(Level(lw, lh, unpack_indices(raw, psm, lw * lh)))
    (name_hash,) = struct.unpack_from("<I", data, 88)
    return {"psm": psm, "flags": flags, "width": width, "height": height,
            "alpha_ref": alpha_ref, "palette": pal, "levels": levels,
            "clut_ofs": clut_ofs, "name_hash": name_hash}


def dtex_to_image(data: bytes, level: int = 0) -> Image.Image:
    info = parse_dtex(data)
    lv = info["levels"][level]
    pal = info["palette"]
    img = Image.new("RGBA", (lv.width, lv.height))
    img.putdata([tuple(pal[i]) for i in lv.indices])
    return img


# --------------------------------------------------------------------------
# texture demake
# --------------------------------------------------------------------------


@dataclass
class TexResult:
    key: str
    ok: bool
    data: bytes = b""
    error: str = ""
    info: dict = field(default_factory=dict)


def demake_image(src: Image.Image, key: str, cfg: TexConfig, force_psm: Optional[int] = None,
                 max_dim: Optional[int] = None, extra_flags: int = 0,
                 allow_mips: bool = True) -> Tuple[bytes, dict]:
    src = src.convert("RGBA")
    if max_dim is None:
        max_dim = max_dim_for(key, src.size[0], src.size[1], cfg)
    tw, th = target_size(src.size[0], src.size[1], max_dim)
    base = resize(src, tw, th, cfg.filter)
    mode = analyze_alpha(base, cfg.alpha_ref)

    candidates: List[int]
    if force_psm is not None:
        candidates = [force_psm]
    elif cfg.colors == "16":
        candidates = [DTEX_PSM_T4]
    elif cfg.colors == "256":
        candidates = [DTEX_PSM_T8]
    else:
        candidates = [DTEX_PSM_T4, DTEX_PSM_T8]

    chosen = None
    for psm in candidates:
        n = 16 if psm == DTEX_PSM_T4 else 256
        idx, pal = quantize(base, n, mode, cfg.alpha_ref)
        if cfg.dither and mode != ALPHA_BLEND:
            idx = remap(base, pal, mode, cfg.alpha_ref, dither=True)
        q = psnr(base, idx, pal, mode)
        chosen = (psm, idx, pal, q)
        if psm == DTEX_PSM_T4 and len(candidates) > 1 and q < cfg.t4_psnr:
            continue
        break
    assert chosen is not None
    psm, idx, pal, q = chosen

    levels = [Level(tw, th, idx)]
    if cfg.mips and allow_mips:
        lw, lh = tw // 2, th // 2
        while (len(levels) < min(cfg.max_levels, DTEX_MAX_LEVELS)
               and lw >= min_width(psm) and lh >= 8):
            lvl_img = src.resize((lw, lh), Image.BOX)
            levels.append(Level(lw, lh, remap(lvl_img, pal, mode, cfg.alpha_ref, False)))
            lw, lh = lw // 2, lh // 2

    flags = extra_flags
    if mode == ALPHA_CUTOUT:
        flags |= DTEX_F_ALPHA_TEST
    elif mode == ALPHA_BLEND:
        flags |= DTEX_F_ALPHA_BLEND
    data = build_dtex(levels, pal, psm, flags, cfg.alpha_ref, fnv1a32(key), cfg.swizzle)
    info = {"key": key, "src_size": list(src.size), "size": [tw, th],
            "psm": "T4" if psm == DTEX_PSM_T4 else "T8", "alpha": mode,
            "levels": len(levels), "colors": len(pal), "psnr": round(q, 2),
            "bytes": len(data)}
    return data, info


def _worker_ctx(arg):
    data_cfg, cfg = arg
    return (DataSource(data_cfg), cfg)


def _cook_one(ctx, item) -> TexResult:
    source, cfg = ctx
    key, path = item
    try:
        img = dmcommon.read_image_bytes(source.read(path))
        data, info = demake_image(img, key, cfg)
        info["src"] = path
        return TexResult(key, True, data, info=info)
    except Exception as exc:  # report and keep cooking the rest
        return TexResult(key, False, error=f"{type(exc).__name__}: {exc}")


# --------------------------------------------------------------------------
# atlas
# --------------------------------------------------------------------------


@dataclass
class AtlasEntry:
    key: str
    page: int
    x: int
    y: int
    w: int
    h: int


def pack_atlas(images: Sequence[Tuple[str, Image.Image]], page_w: int, page_h: int
               ) -> Tuple[List[Image.Image], List[AtlasEntry]]:
    """Shelf packer: tallest first, left to right, new shelf / page on overflow."""
    order = sorted(images, key=lambda kv: (-kv[1].size[1], -kv[1].size[0], kv[0]))
    pages: List[Image.Image] = []
    entries: List[AtlasEntry] = []
    x = y = shelf_h = 0
    page = None
    for key, img in order:
        w, h = img.size
        if w > page_w or h > page_h:
            raise ValueError(f"atlas image {key} ({w}x{h}) larger than page")
        if page is None or x + w > page_w:
            x, y, shelf_h = 0, y + shelf_h, 0
        if page is None or y + h > page_h:
            page = Image.new("RGBA", (page_w, page_h), (0, 0, 0, 0))
            pages.append(page)
            x = y = shelf_h = 0
        page.paste(img, (x, y))
        entries.append(AtlasEntry(key, len(pages) - 1, x, y, w, h))
        x += w
        shelf_h = max(shelf_h, h)
    return pages, entries


def build_datl(page_names: Sequence[str], entries: Sequence[AtlasEntry], page_w: int,
               page_h: int) -> bytes:
    w = BinWriter()
    w.reserve(32)
    pages_ofs = w.tell()
    for name in page_names:
        w.pack("I", fnv1a32(name))
    entries_ofs = w.align(16)
    seen = {}
    for e in sorted(entries, key=lambda e: fnv1a32(e.key)):
        h = fnv1a32(e.key)
        if h in seen:
            raise ValueError(f"atlas hash collision {e.key} / {seen[h]}")
        seen[h] = e.key
        w.pack("IHHHHHH", h, e.page, e.x, e.y, e.w, e.h, 0)
    w.patch(0, "4sHHIHHII8x", DATL_MAGIC, DATL_VERSION, len(page_names), len(entries),
            page_w, page_h, pages_ofs, entries_ofs)
    return w.getvalue()


def parse_datl(data: bytes) -> dict:
    magic, version, npages, nentries, pw, ph, pages_ofs, entries_ofs = struct.unpack_from(
        "<4sHHIHHII", data, 0)
    if magic != DATL_MAGIC or version != DATL_VERSION:
        raise ValueError("not a DATL v1")
    pages = list(struct.unpack_from(f"<{npages}I", data, pages_ofs))
    entries = [struct.unpack_from("<IHHHHHH", data, entries_ofs + 16 * i)[:6]
               for i in range(nentries)]
    return {"page_size": (pw, ph), "pages": pages, "entries": entries}


def cook_atlas(name: str, images: Sequence[Tuple[str, Image.Image]], cfg: TexConfig,
               page_size: int, icon_max: int) -> Tuple[Dict[str, bytes], dict]:
    prepared = []
    for key, img in images:
        img = img.convert("RGBA")
        w, h = img.size
        if max(w, h) > icon_max:
            s = icon_max / float(max(w, h))
            img = img.resize((max(1, int(w * s)), max(1, int(h * s))), Image.BOX)
        prepared.append((key, img))
    pages, entries = pack_atlas(prepared, page_size, page_size)
    out: Dict[str, bytes] = {}
    page_names = []
    page_cfg = TexConfig(**{**asdict(cfg), "mips": False})
    for i, page in enumerate(pages):
        pname = f"atlas/{name}/{i}"
        data, _info = demake_image(page, pname, page_cfg, force_psm=DTEX_PSM_T8,
                                   max_dim=page_size, extra_flags=DTEX_F_ATLAS_PAGE,
                                   allow_mips=False)
        out[pname] = data
        page_names.append(pname)
    out[f"atlas/{name}.datl"] = build_datl(page_names, entries, page_size, page_size)
    return out, {"name": name, "pages": len(pages), "icons": len(entries)}


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def collect_sources(source: DataSource, atlas_prefixes: Sequence[str],
                    only_keys: Optional[set], always_prefixes: Sequence[str] = ()
                    ) -> Tuple[List[Tuple[str, str]], List[Tuple[str, str]]]:
    """Return (texture items, atlas items) as (key, path), preferring .dds
    when several files share a key. With only_keys, keys starting with one of
    always_prefixes (UI art no mesh references) are still cooked."""
    rank = {".dds": 0, ".tga": 1, ".bmp": 2, ".png": 3}
    best: Dict[str, Tuple[int, str]] = {}
    for prefix in ("textures/", "bookart/") + tuple(atlas_prefixes):
        for path in source.list(prefix, dmcommon.TEXTURE_EXTS):
            key = dmcommon.texture_key(path)
            r = rank.get(os.path.splitext(path)[1], 9)
            if key not in best or r < best[key][0]:
                best[key] = (r, path)
    tex, atlas = [], []
    for key in sorted(best):
        path = best[key][1]
        if path.startswith(tuple(atlas_prefixes)):
            atlas.append((key, path))
        elif only_keys is None or key in only_keys or key.startswith(tuple(always_prefixes)):
            tex.append((key, path))
    return tex, atlas


def load_only_keys(paths: Optional[Sequence[str]]) -> Optional[set]:
    """Union of texture keys from JSON lists or cooker manifests ("textures")."""
    if not paths:
        return None
    out: set = set()
    for path in paths:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
        keys = doc.get("textures", []) if isinstance(doc, dict) else doc
        out.update(dmcommon.texture_key(k) for k in keys)
    return out


def parse_size_rules(rules: Sequence[str]) -> Tuple[Tuple[str, int], ...]:
    out = []
    for r in rules:
        prefix, _, size = r.partition("=")
        out.append((dmcommon.normalize_path(prefix), int(size)))
    return tuple(out)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", action="append", default=[],
                    help="Data Files directory (repeatable; BSAs inside are mounted)")
    ap.add_argument("--bsa", action="append", default=[], help="extra BSA archive")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--size", type=int, default=64)
    ap.add_argument("--small-size", type=int, default=32)
    ap.add_argument("--large-threshold", type=int, default=256)
    ap.add_argument("--colors", choices=("auto", "16", "256"), default="auto")
    ap.add_argument("--t4-psnr", type=float, default=30.0)
    ap.add_argument("--filter", choices=("nearest", "box"), default="nearest")
    ap.add_argument("--dither", action="store_true")
    ap.add_argument("--no-mips", action="store_true")
    ap.add_argument("--no-swizzle", action="store_true")
    ap.add_argument("--size-rule", action="append", default=[],
                    help="PREFIX=SIZE override (replaces built-in UI rules if given)")
    ap.add_argument("--atlas-prefix", action="append", default=None,
                    help="path prefix packed into the icon atlas (default icons/)")
    ap.add_argument("--atlas-page", type=int, default=256)
    ap.add_argument("--icon-max", type=int, default=32)
    ap.add_argument("--only-keys", action="append", default=[],
                    help="JSON list or cooker manifest of texture keys to cook (repeatable)")
    ap.add_argument("--limit", type=int, default=0, help="cook at most N textures (testing)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--strict", action="store_true", help="exit non-zero on any failure")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    dmcommon.setup_logging(args.verbose)

    if not args.data and not args.bsa:
        ap.error("give at least one --data directory or --bsa archive")
    cfg = TexConfig(size=args.size, small_size=args.small_size,
                    large_threshold=args.large_threshold, colors=args.colors,
                    t4_psnr=args.t4_psnr, filter=args.filter, dither=args.dither,
                    mips=not args.no_mips, swizzle=not args.no_swizzle,
                    size_rules=parse_size_rules(args.size_rule) if args.size_rule
                    else DEFAULT_SIZE_RULES)
    data_cfg = DataConfig(tuple(args.data), tuple(args.bsa))
    source = DataSource(data_cfg)
    atlas_prefixes = tuple(dmcommon.normalize_path(p) for p in (args.atlas_prefix or ["icons/"]))
    tex_items, atlas_items = collect_sources(source, atlas_prefixes,
                                             load_only_keys(args.only_keys),
                                             tuple(p for p, _ in cfg.size_rules))
    if args.limit:
        tex_items = tex_items[:args.limit]
        atlas_items = atlas_items[:args.limit]
    LOG.info("textures: %d world/UI, %d atlas icons", len(tex_items), len(atlas_items))

    t0 = time.time()
    pack = dmcommon.DPakWriter(dmcommon.DPAK_KIND_TEXTURE)
    manifest = {"config": asdict(cfg), "textures": [], "failures": [], "atlases": []}
    stats = {"T4": 0, "T8": 0, "opaque": 0, "cutout": 0, "blend": 0}
    for res in dmcommon.parallel_map(_cook_one, tex_items, _worker_ctx, (data_cfg, cfg),
                                     args.jobs, dmcommon.progress_printer("textures")):
        if res.ok:
            pack.add(res.key, res.data)
            manifest["textures"].append(res.info)
            stats[res.info["psm"]] += 1
            stats[res.info["alpha"]] += 1
        else:
            LOG.warning("texture %s failed: %s", res.key, res.error)
            manifest["failures"].append({"key": res.key, "error": res.error})

    if atlas_items:
        images = []
        for key, path in atlas_items:
            try:
                images.append((key, dmcommon.read_image_bytes(source.read(path))))
            except Exception as exc:
                LOG.warning("icon %s failed: %s", key, exc)
                manifest["failures"].append({"key": key, "error": str(exc)})
        files, info = cook_atlas("icons", images, cfg, args.atlas_page, args.icon_max)
        for name, data in files.items():
            pack.add(name, data)
        manifest["atlases"].append(info)

    os.makedirs(args.out, exist_ok=True)
    size = pack.write(os.path.join(args.out, "textures.dpak"))
    manifest["stats"] = stats
    manifest["pack_bytes"] = size
    with open(os.path.join(args.out, "textures_manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    LOG.info("wrote textures.dpak: %d entries, %.1f KB in %.1fs (T4 %d, T8 %d, "
             "cutout %d, blend %d, failed %d)", len(pack.items), size / 1024.0,
             time.time() - t0, stats["T4"], stats["T8"], stats["cutout"], stats["blend"],
             len(manifest["failures"]))
    return 1 if (args.strict and manifest["failures"]) else 0


if __name__ == "__main__":
    sys.exit(main())
