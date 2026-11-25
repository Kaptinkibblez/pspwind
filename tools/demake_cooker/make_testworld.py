#!/usr/bin/env python3
"""make_testworld - generate a small original Morrowind-format test world.

Writes a "Data Files" folder (TestWorld.esm + TestWorld.bsa) containing
only procedurally generated content: rolling terrain with three land
textures and a shoreline, a hamlet of box houses, rocks and trees, a lantern,
and a cellar interior linked to the exterior by doors. It exercises every
path of the cooker pipeline (LAND/LTEX/CELL/refs/doors/lights, NIF skins are
covered by unit tests) and gives the PSP viewer something to render without
retail data.

    python3 make_testworld.py --out build/testworld
    python3 cook_all.py --data "build/testworld/Data Files" --out build/testworld/cooked
"""

from __future__ import annotations

import argparse
import io
import math
import os
import random
import struct
import sys
from typing import Dict, List, Sequence, Tuple

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import dmcommon  # noqa: E402
import esm  # noqa: E402
import nif  # noqa: E402
import synth as S  # noqa: E402

CELL = esm.CELL_SIZE
EXTERIOR = [(x, y) for x in range(-3, 3) for y in range(-3, 3)]  # 6x6 cells
CELLAR = "Test Hamlet, Cellar"


# --------------------------------------------------------------------------
# procedural textures (original art: noise, planks, bricks)
# --------------------------------------------------------------------------


def _noise_img(size, base, var, seed, alpha=None):
    rng = random.Random(seed)
    img = Image.new("RGBA", (size, size))
    px = []
    for y in range(size):
        for x in range(size):
            n = rng.random() * 2 - 1
            c = tuple(max(0, min(255, int(base[i] + var[i] * n))) for i in range(3))
            a = 255 if alpha is None else alpha(x, y, rng)
            px.append(c + (a,))
    img.putdata(px)
    return img


def _planks(size, base, seed):
    img = _noise_img(size, base, (18, 14, 10), seed)
    for y in range(size):
        if y % (size // 4) == 0:
            for x in range(size):
                img.putpixel((x, y), (40, 28, 18, 255))
    return img


def _bricks(size, base, seed):
    img = _noise_img(size, base, (20, 20, 20), seed)
    rows = 8
    h = size // rows
    for r in range(rows):
        off = (size // 8) if r % 2 else 0
        for x in range(size):
            img.putpixel((x, r * h), (60, 55, 50, 255))
        for k in range(4):
            x = (k * size // 4 + off) % size
            for y in range(r * h, r * h + h):
                img.putpixel((x, y), (60, 55, 50, 255))
    return img


def _leaves(size, seed):
    def a(x, y, rng):
        cx, cy = size / 2, size / 2
        d = math.hypot(x - cx, y - cy) / (size / 2)
        return 255 if d < 0.95 and rng.random() > d * 0.6 else 0
    return _noise_img(size, (52, 104, 40), (20, 30, 15), seed, a)


def _dds(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    img.save(buf, "DDS")
    return buf.getvalue()


def textures() -> Dict[str, bytes]:
    return {
        "textures/_land_default.dds": _dds(_noise_img(128, (98, 84, 64), (16, 14, 10), 1)),
        "textures/tx_tw_grass.dds": _dds(_noise_img(256, (88, 112, 56), (24, 26, 14), 2)),
        "textures/tx_tw_dirt.dds": _dds(_noise_img(256, (112, 92, 66), (20, 16, 12), 3)),
        "textures/tx_tw_rock.dds": _dds(_noise_img(256, (120, 116, 110), (30, 30, 30), 4)),
        "textures/tx_tw_planks.dds": _dds(_planks(256, (128, 92, 58), 5)),
        "textures/tx_tw_bricks.dds": _dds(_bricks(256, (150, 138, 120), 6)),
        "textures/tx_tw_roof.dds": _dds(_planks(256, (120, 48, 36), 7)),
        "textures/tx_tw_bark.dds": _dds(_noise_img(128, (84, 64, 44), (20, 14, 10), 8)),
        "textures/tx_tw_leaves.dds": _dds(_leaves(128, 9)),
        "textures/tx_tw_glow.dds": _dds(_noise_img(64, (250, 210, 120), (5, 10, 10), 10)),
        "textures/water/water00.dds": _dds(_noise_img(128, (40, 80, 110), (8, 12, 14), 11)),
        "icons/tw/tx_coin.dds": _dds(_noise_img(32, (220, 180, 40), (20, 20, 10), 12)),
    }


# --------------------------------------------------------------------------
# meshes
# --------------------------------------------------------------------------


def box(sx, sy, sz, cz=0.0, inward=False, uv_scale=1.0):
    """Axis aligned box centred on (0, 0, cz); inward flips faces (rooms)."""
    verts, normals, uvs, tris = S.cube(1.0)
    v = [(x * sx, y * sy, z * sz + cz) for x, y, z in verts]
    n = [(-a, -b, -c) if inward else (a, b, c) for a, b, c in normals]
    u = [(a * uv_scale, b * uv_scale) for a, b in uvs]
    t = [(a, c, b) if inward else (a, b, c) for a, b, c in tris]
    return v, n, u, t


def pyramid(sx, sy, base_z, apex_z):
    corners = [(-sx, -sy), (sx, -sy), (sx, sy), (-sx, sy)]
    verts, normals, uvs, tris = [], [], [], []
    for i in range(4):
        (ax, ay), (bx, by) = corners[i], corners[(i + 1) % 4]
        p0, p1, p2 = (ax, ay, base_z), (bx, by, base_z), (0.0, 0.0, apex_z)
        e1 = [p1[k] - p0[k] for k in range(3)]
        e2 = [p2[k] - p0[k] for k in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        ln = math.sqrt(sum(c * c for c in n))
        base = len(verts)
        verts += [p0, p1, p2]
        normals += [tuple(c / ln for c in n)] * 3
        uvs += [(0.0, 0.0), (2.0, 0.0), (1.0, 2.0)]
        tris.append((base, base + 1, base + 2))
    return verts, normals, uvs, tris


def cross_quads(size, z0, z1):
    verts, normals, uvs, tris = [], [], [], []
    for ang in (0.0, math.pi / 2):
        c, s = math.cos(ang) * size, math.sin(ang) * size
        base = len(verts)
        verts += [(-c, -s, z0), (c, s, z0), (c, s, z1), (-c, -s, z1)]
        n = (-math.sin(ang), math.cos(ang), 0.0)
        normals += [n] * 4
        uvs += [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
        tris += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    return verts, normals, uvs, tris


class NifBuilder:
    """Accumulates blocks; returns refs so meshes can be composed readably."""

    def __init__(self) -> None:
        self.blocks: List[tuple] = []

    def add(self, block) -> int:
        self.blocks.append(block)
        return len(self.blocks) - 1

    def placeholder(self) -> int:
        self.blocks.append(None)
        return len(self.blocks) - 1

    def set(self, ref: int, block) -> None:
        self.blocks[ref] = block

    def textured(self, name, geom, texture, extra_props=(), **shape_kw) -> int:
        verts, normals, uvs, tris = geom
        shape = self.placeholder()
        data = self.add(S.shape_data(verts, tris, normals=normals, uvs=uvs))
        src = self.add(S.source_texture(f"Textures\\{texture}.tga"))
        tex = self.add(S.texturing(src))
        props = [tex] + [self.add(p) for p in extra_props]
        self.set(shape, S.shape(name, data, props=props, **shape_kw))
        return shape

    def build(self, root: int) -> bytes:
        return nif.write_nif(self.blocks, [root])


def mesh_house() -> bytes:
    b = NifBuilder()
    root = b.placeholder()
    walls = b.textured("Tri Walls", box(256, 192, 160, cz=160, uv_scale=2.0), "tx_tw_bricks")
    roof = b.textured("Tri Roof", pyramid(300, 236, 320, 520), "tx_tw_roof")
    coll = b.placeholder()
    cbox = b.placeholder()
    cdata = b.add(S.shape_data(*[box(256, 192, 260, cz=260)[i] for i in (0, 3)]))
    b.set(cbox, S.shape("Tri Collision", cdata, flags=1))
    b.set(coll, S.node("Collision", [cbox], kind="RootCollisionNode"))
    b.set(root, S.node("House", [walls, roof, coll]))
    return b.build(root)


def mesh_rock() -> bytes:
    b = NifBuilder()
    root = b.placeholder()
    v, n, uv, t = S.uv_sphere(60.0, 6, 10)
    v = [(x * 1.4, y, z * 0.6 + 10) for x, y, z in v]
    shape = b.textured("Tri Rock", (v, n, uv, t), "tx_tw_rock")
    b.set(root, S.node("Rock", [shape]))
    return b.build(root)


def mesh_tree() -> bytes:
    b = NifBuilder()
    root = b.placeholder()
    v, n, uv, t = S.cylinder(14.0, 260.0, rings=2, segments=6)
    trunk = b.textured("Tri Trunk", (v, n, uv, t), "tx_tw_bark")
    leaves = b.textured("Tri Leaves", cross_quads(170.0, 160.0, 460.0), "tx_tw_leaves",
                        extra_props=(S.alpha_prop(1 << 9, threshold=128), S.stencil(3)))
    b.set(root, S.node("Tree", [trunk, leaves]))
    return b.build(root)


def mesh_crate() -> bytes:
    b = NifBuilder()
    root = b.placeholder()
    shape = b.textured("Tri Crate", box(40, 40, 40, cz=40), "tx_tw_planks")
    b.set(root, S.node("Crate", [shape]))
    return b.build(root)


def mesh_door() -> bytes:
    b = NifBuilder()
    root = b.placeholder()
    shape = b.textured("Tri Door", box(64, 6, 110, cz=110), "tx_tw_planks")
    b.set(root, S.node("Door", [shape]))
    return b.build(root)


def mesh_lantern() -> bytes:
    b = NifBuilder()
    root = b.placeholder()
    glow = b.textured("Tri Glow", box(10, 10, 16, cz=16), "tx_tw_glow",
                      extra_props=(S.material(emissive=(1.0, 0.8, 0.4)),))
    attach = b.add(S.node("AttachLight", [], t=(0.0, 0.0, 16.0)))
    b.set(root, S.node("Lantern", [glow, attach]))
    return b.build(root)


def mesh_cellar_room() -> bytes:
    b = NifBuilder()
    root = b.placeholder()
    walls = b.textured("Tri Room", box(1200, 1200, 400, cz=200, inward=True, uv_scale=4.0),
                       "tx_tw_bricks")
    b.set(root, S.node("Cellar", [walls]))
    return b.build(root)


def meshes() -> Dict[str, bytes]:
    return {
        "meshes/tw/house.nif": mesh_house(),
        "meshes/tw/rock.nif": mesh_rock(),
        "meshes/tw/tree.nif": mesh_tree(),
        "meshes/tw/crate.nif": mesh_crate(),
        "meshes/tw/door.nif": mesh_door(),
        "meshes/tw/lantern.nif": mesh_lantern(),
        "meshes/tw/cellar_room.nif": mesh_cellar_room(),
    }


# --------------------------------------------------------------------------
# ESM
# --------------------------------------------------------------------------


def terrain_height(wx: float, wy: float) -> float:
    """Rolling hills with a sea to the south-west (z < 0 is under water)."""
    h = 260.0 * math.sin(wx / 3100.0) * math.cos(wy / 2700.0)
    h += 120.0 * math.sin((wx + wy) / 1300.0)
    h += 0.045 * (wx + wy) + 200.0
    return h


def land_record(cx: int, cy: int) -> bytes:
    n = esm.LAND_SIZE
    step = CELL / (n - 1)
    heights = [terrain_height(cx * CELL + i * step, cy * CELL + j * step)
               for j in range(n) for i in range(n)]
    normals = bytearray()
    colors = bytearray()
    for j in range(n):
        for i in range(n):
            hl = heights[j * n + max(i - 1, 0)]
            hr = heights[j * n + min(i + 1, n - 1)]
            hd = heights[max(j - 1, 0) * n + i]
            hu = heights[min(j + 1, n - 1) * n + i]
            nx, ny, nz = hl - hr, hd - hu, 2.0 * step
            ln = math.sqrt(nx * nx + ny * ny + nz * nz)
            normals += struct.pack("<bbb", *(int(round(c / ln * 127)) for c in (nx, ny, nz)))
            shade = 200 + int(40 * math.sin(i * 0.3) * math.cos(j * 0.2))
            colors += bytes((shade, shade, max(0, shade - 20)))
    tex = []
    for j in range(16):
        for i in range(16):
            wx = cx * CELL + (i + 0.5) * CELL / 16
            wy = cy * CELL + (j + 0.5) * CELL / 16
            h = terrain_height(wx, wy)
            tex.append(3 if h > 820 else (1 if h > 40 else 2))  # rock / grass / dirt (+1)
    vtex = struct.pack("<256H", *esm.transpose_vtex(tex))
    return esm.record("LAND", [
        esm.sub("INTV", struct.pack("<ii", cx, cy)),
        esm.sub("DATA", struct.pack("<I", 0x7)),
        esm.sub("VNML", bytes(normals)),
        esm.sub("VHGT", esm.encode_heights(heights)),
        esm.sub("WNAM", bytes(81)),
        esm.sub("VCLR", bytes(colors)),
        esm.sub("VTEX", vtex),
    ])


def obj(rtype: str, oid: str, model: str, name: str = "", extra=()) -> bytes:
    subs = [esm.sub("NAME", esm.zs(oid)), esm.sub("MODL", esm.zs(model))]
    if name:
        subs.append(esm.sub("FNAM", esm.zs(name)))
    subs += list(extra)
    return esm.record(rtype, subs)


def ref_subs(index: int, oid: str, pos, rot=(0.0, 0.0, 0.0), scale=None, door=None,
             dest_cell=None) -> List[bytes]:
    subs = [esm.sub("FRMR", struct.pack("<I", index)), esm.sub("NAME", esm.zs(oid))]
    if scale is not None:
        subs.append(esm.sub("XSCL", struct.pack("<f", scale)))
    if door is not None:
        subs.append(esm.sub("DODT", struct.pack("<6f", *door)))
    if dest_cell:
        subs.append(esm.sub("DNAM", esm.zs(dest_cell)))
    subs.append(esm.sub("DATA", struct.pack("<6f", *pos, *rot)))
    return subs


def build_esm() -> List[bytes]:
    rng = random.Random(1234)
    recs = [
        esm.record("LTEX", [esm.sub("NAME", esm.zs("tw_grass")), esm.sub("INTV", struct.pack("<I", 0)),
                            esm.sub("DATA", esm.zs("tx_tw_grass.tga"))]),
        esm.record("LTEX", [esm.sub("NAME", esm.zs("tw_dirt")), esm.sub("INTV", struct.pack("<I", 1)),
                            esm.sub("DATA", esm.zs("tx_tw_dirt.tga"))]),
        esm.record("LTEX", [esm.sub("NAME", esm.zs("tw_rock")), esm.sub("INTV", struct.pack("<I", 2)),
                            esm.sub("DATA", esm.zs("tx_tw_rock.tga"))]),
        esm.record("REGN", [esm.sub("NAME", esm.zs("tw region")),
                            esm.sub("FNAM", esm.zs("Test Coast Region")),
                            esm.sub("CNAM", struct.pack("<I", 0x00406080))]),
        obj("STAT", "tw_house", "tw\\house.nif"),
        obj("STAT", "tw_rock", "tw\\rock.nif"),
        obj("STAT", "tw_tree", "tw\\tree.nif"),
        obj("STAT", "tw_cellar_room", "tw\\cellar_room.nif"),
        obj("CONT", "tw_crate", "tw\\crate.nif", "Crate"),
        obj("DOOR", "tw_door", "tw\\door.nif", "Door"),
        obj("LIGH", "tw_lantern", "tw\\lantern.nif", "Lantern",
            extra=[esm.sub("LHDT", struct.pack("<fiiIII", 1.0, 5, -1, 384, 0x0060C0FF, 0x0008))]),
        obj("MISC", "tw_coin", "tw\\crate.nif", "Coin"),
    ]
    house_pos = (1200.0, 900.0, terrain_height(1200.0, 900.0))
    door_out = (house_pos[0], house_pos[1] - 200.0, house_pos[2])
    index = 1
    for (cx, cy) in EXTERIOR:
        subs = [esm.sub("NAME", esm.zs("Test Hamlet" if (cx, cy) == (0, 0) else "")),
                esm.sub("DATA", struct.pack("<iii", 0, cx, cy)),
                esm.sub("RGNN", esm.zs("tw region"))]
        for _ in range(6):
            x = cx * CELL + rng.uniform(300, CELL - 300)
            y = cy * CELL + rng.uniform(300, CELL - 300)
            z = terrain_height(x, y)
            if z < 20:
                continue
            kind = "tw_tree" if rng.random() < 0.6 else "tw_rock"
            subs += ref_subs(index, kind, (x, y, z), (0.0, 0.0, rng.uniform(0, 6.28)),
                             scale=rng.uniform(0.8, 1.4))
            index += 1
        if (cx, cy) == (0, 0):
            subs += ref_subs(index, "tw_house", house_pos, (0.0, 0.0, 0.0))
            index += 1
            subs += ref_subs(index, "tw_door", (door_out[0], door_out[1] + 8, door_out[2]),
                             door=(0.0, -420.0, 16.0, 0.0, 0.0, 0.0), dest_cell=CELLAR)
            index += 1
            subs += ref_subs(index, "tw_lantern", (house_pos[0] + 300, house_pos[1] - 260,
                                                   house_pos[2] + 120))
            index += 1
            subs += ref_subs(index, "tw_crate", (house_pos[0] - 360, house_pos[1] - 240,
                                                 terrain_height(house_pos[0] - 360, house_pos[1] - 240)))
            index += 1
        recs.append(esm.record("CELL", subs))
        recs.append(land_record(cx, cy))
    cellar = [esm.sub("NAME", esm.zs(CELLAR)),
              esm.sub("DATA", struct.pack("<iii", esm.CELL_INTERIOR | esm.CELL_HAS_WATER, 0, 0)),
              esm.sub("WHGT", struct.pack("<f", -150.0)),
              esm.sub("AMBI", struct.pack("<IIIf", 0x00283040, 0x00406080, 0x00101418, 0.6))]
    cellar += ref_subs(index, "tw_cellar_room", (0.0, 0.0, 0.0))
    index += 1
    cellar += ref_subs(index, "tw_door", (0.0, -580.0, 0.0),
                       door=(door_out[0], door_out[1] - 120.0, door_out[2], 0.0, 0.0, math.pi))
    index += 1
    cellar += ref_subs(index, "tw_crate", (300.0, 300.0, 0.0), (0.0, 0.0, 0.4))
    index += 1
    cellar += ref_subs(index, "tw_lantern", (0.0, 0.0, 300.0))
    index += 1
    cellar += [esm.sub("NAM0", struct.pack("<I", 1))]
    cellar += ref_subs(index, "tw_coin", (-200.0, 250.0, 0.0))
    index += 1
    recs.append(esm.record("CELL", cellar))
    return recs


def make(out_dir: str) -> str:
    data_dir = os.path.join(out_dir, "Data Files")
    os.makedirs(data_dir, exist_ok=True)
    files = {}
    files.update(textures())
    files.update(meshes())
    dmcommon.write_bsa(os.path.join(data_dir, "TestWorld.bsa"), files)
    esm.write_esm(os.path.join(data_dir, "TestWorld.esm"), build_esm())
    return data_dir


def main(argv: Sequence[str] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    print(make(args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
