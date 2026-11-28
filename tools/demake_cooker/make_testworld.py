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
SNAPPER_CELL = (-2, -2)


def snapper_pos() -> Tuple[float, float]:
    """A dry spot in the south-west corner, far from the hamlet."""
    for r in range(0, 4000, 250):
        for k in range(8):
            x = -2 * CELL + 4096.0 + r * math.cos(k * math.pi / 4)
            y = -2 * CELL + 4096.0 + r * math.sin(k * math.pi / 4)
            if terrain_height(x, y) > 40.0:
                return x, y
    return -2 * CELL + 4096.0, -2 * CELL + 4096.0


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
        "textures/tx_tw_skin.dds": _dds(_noise_img(64, (196, 150, 120), (10, 8, 8), 13)),
        "textures/tx_tw_cloth.dds": _dds(_planks(64, (70, 90, 130), 14)),
        "textures/tx_tw_shell.dds": _dds(_noise_img(64, (150, 80, 50), (20, 12, 10), 15)),
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
    walls = b.textured("Tri Walls", box(512, 384, 300, cz=90, uv_scale=2.0), "tx_tw_bricks")  # foundation sunk 60 units
    roof = b.textured("Tri Roof", pyramid(300, 236, 240, 420), "tx_tw_roof")
    coll = b.placeholder()
    cbox = b.placeholder()
    cdata = b.add(S.shape_data(*[box(512, 384, 240, cz=120)[i] for i in (0, 3)]))
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


# --------------------------------------------------------------------------
# actors: Biped-style skeleton, race body parts, animations, a creature
# --------------------------------------------------------------------------

def _side(name: str, x: float):
    """Right side is +X (characters face +Y)."""
    return [(name.format(s="R", side="Right"), x), (name.format(s="L", side="Left"), -x)]


def skeleton_def():
    """(name, parent, local translation) in parent-first order."""
    bones = [("Bip01", None, (0.0, 0.0, 0.0)),
             ("Bip01 Pelvis", "Bip01", (0.0, 0.0, 72.0)),
             ("Groin", "Bip01 Pelvis", (0.0, 0.0, 0.0)),
             ("Bip01 Spine", "Bip01 Pelvis", (0.0, 0.0, 8.0)),
             ("Bip01 Spine1", "Bip01 Spine", (0.0, 0.0, 22.0)),
             ("Chest", "Bip01 Spine1", (0.0, 0.0, 0.0)),
             ("Bip01 Neck", "Bip01 Spine1", (0.0, 0.0, 26.0)),
             ("Bip01 Head", "Bip01 Neck", (0.0, 0.0, 8.0)),
             ("Head", "Bip01 Head", (0.0, 0.0, 0.0))]
    for s_, side in (("R", "Right"), ("L", "Left")):
        sx = 1.0 if s_ == "R" else -1.0
        bones += [(f"Bip01 {s_} UpperArm", "Bip01 Spine1", (17.0 * sx, 0.0, 22.0)),
                  (f"{side} Upper Arm", f"Bip01 {s_} UpperArm", (0.0, 0.0, 0.0)),
                  (f"Bip01 {s_} Forearm", f"Bip01 {s_} UpperArm", (0.0, 0.0, -24.0)),
                  (f"{side} Forearm", f"Bip01 {s_} Forearm", (0.0, 0.0, 0.0)),
                  (f"Bip01 {s_} Hand", f"Bip01 {s_} Forearm", (0.0, 0.0, -22.0)),
                  (f"{side} Hand", f"Bip01 {s_} Hand", (0.0, 0.0, 0.0)),
                  (f"Bip01 {s_} Thigh", "Bip01 Pelvis", (8.0 * sx, 0.0, -4.0)),
                  (f"{side} Upper Leg", f"Bip01 {s_} Thigh", (0.0, 0.0, 0.0)),
                  (f"Bip01 {s_} Calf", f"Bip01 {s_} Thigh", (0.0, 0.0, -32.0)),
                  (f"Bip01 {s_} Foot", f"Bip01 {s_} Calf", (0.0, 0.0, -32.0)),
                  (f"{side} Foot", f"Bip01 {s_} Foot", (0.0, 0.0, 0.0))]
    return bones


def skeleton_world_z(name: str) -> float:
    pos = {n: (p, t) for n, p, t in skeleton_def()}
    z = 0.0
    while name is not None:
        p, t = pos[name]
        z += t[2]
        name = p
    return z


def build_skeleton(b: "NifBuilder", extra_children: Dict[str, List[int]] = None) -> Dict[str, int]:
    """Adds all skeleton nodes; returns bone name -> block ref (root = 'Bip01')."""
    extra_children = extra_children or {}
    refs = {name: b.placeholder() for name, _p, _t in skeleton_def()}
    for name, parent, t in skeleton_def():
        kids = [refs[n] for n, p, _t in skeleton_def() if p == name] + extra_children.get(name, [])
        b.set(refs[name], S.node(name, kids, t=t))
    return refs


def mesh_skeleton() -> bytes:
    b = NifBuilder()
    root = b.placeholder()
    refs = build_skeleton(b)
    b.set(root, S.node("Skeleton", [refs["Bip01"]]))
    return b.build(root)


def mesh_folk_chest(texture: str) -> bytes:
    """Skinned torso: lower half on Bip01 Spine, upper half on Bip01 Spine1."""
    b = NifBuilder()
    root = b.placeholder()
    shape = b.placeholder()
    refs = build_skeleton(b)
    verts, normals, uvs, tris = box(34, 18, 56, cz=104)
    data = b.add(S.shape_data(verts, tris, normals=normals, uvs=uvs))
    spine_z, spine1_z = skeleton_world_z("Bip01 Spine"), skeleton_world_z("Bip01 Spine1")
    w0 = [(i, 1.0) for i, v in enumerate(verts) if v[2] < spine1_z]
    w1 = [(i, 1.0) for i, v in enumerate(verts) if v[2] >= spine1_z]
    sdata = b.add(S.skin_data([(S.I9, (0.0, 0.0, -spine_z), 1.0, w0),
                               (S.I9, (0.0, 0.0, -spine1_z), 1.0, w1)]))
    skin = b.add(S.skin_instance(sdata, refs["Bip01"], [refs["Bip01 Spine"], refs["Bip01 Spine1"]]))
    src = b.add(S.source_texture(f"Textures\\{texture}.tga"))
    tex = b.add(S.texturing(src))
    b.set(shape, S.shape("Tri Chest", data, props=[tex], skin=skin))
    b.set(root, S.node("Chest Part", [refs["Bip01"], shape]))
    return b.build(root)


def mesh_rigid_part(name: str, size, centre, texture: str) -> bytes:
    """Unskinned body part authored in its attachment bone's space."""
    b = NifBuilder()
    root = b.placeholder()
    v, n, uv, t = box(*size, cz=0.0)
    v = [(x + centre[0], y + centre[1], z + centre[2]) for x, y, z in v]
    shape = b.textured(f"Tri {name}", (v, n, uv, t), texture)
    b.set(root, S.node(name, [shape]))
    return b.build(root)


def anim_xbase() -> bytes:
    """Idle (0-2 s, breathing) and WalkForward (2-3 s, limb swing + 120 u/s root motion)."""
    X = (1.0, 0.0, 0.0)
    def swing(sign):
        return [(0.0, S.quat_axis(X, 0.0)), (2.0, S.quat_axis(X, 0.0)),
                (2.25, S.quat_axis(X, 0.5 * sign)), (2.75, S.quat_axis(X, -0.5 * sign)),
                (3.0, S.quat_axis(X, 0.0))]
    tracks = [
        ("Bip01", S.keyframe_track_data([], [(0.0, (0.0, 0.0, 0.0)), (2.0, (0.0, 0.0, 0.0)),
                                             (2.5, (0.0, 60.0, 2.0)), (3.0, (0.0, 120.0, 0.0))])),
        ("Bip01 Spine1", S.keyframe_track_data([(0.0, S.quat_axis(X, 0.0)), (1.0, S.quat_axis(X, 0.06)),
                                                (2.0, S.quat_axis(X, 0.0))], [])),
        ("Bip01 R UpperArm", S.keyframe_track_data(swing(1.0), [])),
        ("Bip01 L UpperArm", S.keyframe_track_data(swing(-1.0), [])),
        ("Bip01 R Thigh", S.keyframe_track_data(swing(-1.0), [])),
        ("Bip01 L Thigh", S.keyframe_track_data(swing(1.0), [])),
    ]
    keys = [(0.0, "Idle: Start"), (2.0, "Idle: Stop\r\nWalkForward: Start"),
            (2.25, "SoundGen: Left"), (3.0, "WalkForward: Stop")]
    return S.kf_file(keys, tracks)


def mesh_critter() -> bytes:
    """Skinned creature: body on 'Bip01 Body', tail on 'Bip01 Tail'."""
    b = NifBuilder()
    root = b.placeholder()
    shape = b.placeholder()
    tail = b.add(S.node("Bip01 Tail", [], t=(0.0, -30.0, 0.0)))
    body = b.add(S.node("Bip01 Body", [tail], t=(0.0, 0.0, 20.0)))
    bip = b.add(S.node("Bip01", [body]))
    v1, n1, uv1, t1 = box(40, 50, 24, cz=20)
    v2, n2, uv2, t2 = box(8, 30, 8, cz=20)
    v2 = [(x, y - 40.0, z) for x, y, z in v2]
    verts, normals, uvs = v1 + v2, n1 + n2, uv1 + uv2
    tris = t1 + [(a + len(v1), b_ + len(v1), c + len(v1)) for a, b_, c in t2]
    data = b.add(S.shape_data(verts, tris, normals=normals, uvs=uvs))
    sdata = b.add(S.skin_data([(S.I9, (0.0, 0.0, -20.0), 1.0, [(i, 1.0) for i in range(len(v1))]),
                               (S.I9, (0.0, 30.0, -20.0), 1.0,
                                [(i + len(v1), 1.0) for i in range(len(v2))])]))
    skin = b.add(S.skin_instance(sdata, bip, [body, tail]))
    src = b.add(S.source_texture("Textures\\tx_tw_shell.tga"))
    tex = b.add(S.texturing(src))
    b.set(shape, S.shape("Tri Critter", data, props=[tex], skin=skin))
    b.set(root, S.node("Critter", [bip, shape]))
    return b.build(root)


def anim_critter() -> bytes:
    Z = (0.0, 0.0, 1.0)
    tail = [(0.0, S.quat_axis(Z, 0.0)), (0.4, S.quat_axis(Z, 0.6)), (1.2, S.quat_axis(Z, -0.6)),
            (1.6, S.quat_axis(Z, 0.0))]
    return S.kf_file([(0.0, "Idle: Start"), (1.6, "Idle: Stop")],
                     [("Bip01 Tail", S.keyframe_track_data(tail, []))])


PARTS = {  # BODY id -> (BYDT part, female, mesh key, rigid spec or None)
    "tw_folk_chest": (3, False, "tw/b_folk_chest", None),
    "tw_folk_chest_f": (3, True, "tw/b_folk_chest_f", None),
    "tw_folk_head": (0, False, "tw/b_folk_head", ((18, 20, 22), (0.0, 1.0, 11.0), "tx_tw_skin")),
    "tw_folk_groin": (4, False, "tw/b_folk_groin", ((26, 16, 14), (0.0, 0.0, -4.0), "tx_tw_cloth")),
    "tw_folk_upperarm": (8, False, "tw/b_folk_upperarm", ((8, 8, 24), (0.0, 0.0, -12.0), "tx_tw_cloth")),
    "tw_folk_forearm": (7, False, "tw/b_folk_forearm", ((7, 7, 22), (0.0, 0.0, -11.0), "tx_tw_skin")),
    "tw_folk_hand": (5, False, "tw/b_folk_hand", ((6, 9, 12), (0.0, 0.0, -6.0), "tx_tw_skin")),
    "tw_folk_upperleg": (12, False, "tw/b_folk_upperleg", ((12, 12, 32), (0.0, 0.0, -16.0), "tx_tw_cloth")),
    "tw_folk_foot": (9, False, "tw/b_folk_foot", ((10, 20, 8), (0.0, -4.0, 0.0), "tx_tw_planks")),
}


# --------------------------------------------------------------- sounds (M6)
def _wav(samples: List[int], rate: int, channels: int = 1, bits: int = 16) -> bytes:
    if bits == 8:
        data = bytes(max(0, min(255, (v >> 8) + 128)) for v in samples)
    else:
        data = struct.pack(f"<{len(samples)}h", *samples)
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * bits // 8, channels * bits // 8, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def _synth(seconds: float, rate: int, fn) -> List[int]:
    n = int(seconds * rate)
    return [max(-32768, min(32767, int(fn(i / rate, i / n)))) for i in range(n)]


def _silent_mp3(frames: int) -> bytes:
    """MPEG-1 layer III, 64 kbit/s, 44.1 kHz, mono frames whose side info and
    main data are zero: a valid stream that decodes to silence."""
    header = struct.pack(">I", 0xFFFB50C4)  # sync, MPEG-1, layer III, no CRC, 64k, 44100, mono
    size = 144 * 64000 // 44100
    return b"ID3" + bytes([3, 0, 0, 0, 0, 0, 0]) + (header + bytes(size - 4)) * frames


def sound_files() -> Dict[str, bytes]:
    rng = random.Random(77)
    noise = [rng.uniform(-1.0, 1.0) for _ in range(44100)]
    sine = math.sin
    out = {
        # footstep: short decaying noise thump
        "sound/fx/tw/step.wav": _wav(_synth(0.12, 22050, lambda t, u: noise[int(t * 22050)] * 9000 * (1 - u) ** 3), 22050),
        # sword swish: noise with a falling envelope, 11 kHz 8-bit (conversion path)
        "sound/fx/tw/swish.wav": _wav(_synth(0.25, 11025, lambda t, u: noise[int(t * 11025)] * 14000 * sine(math.pi * u)),
                                      11025, bits=8),
        # hit: low thud, stereo 44.1 kHz (downmix + resample path)
        "sound/fx/tw/hit.wav": _wav([v for v in _synth(0.2, 44100, lambda t, u: 16000 * sine(2 * math.pi * 90 * t) * (1 - u) ** 2)
                                     for _ in (0, 1)], 44100, channels=2),
        "sound/fx/tw/door.wav": _wav(_synth(0.4, 22050, lambda t, u: 9000 * sine(2 * math.pi * (180 - 80 * u) * t) * (1 - u)), 22050),
        "sound/fx/tw/cast.wav": _wav(_synth(0.5, 22050, lambda t, u: 8000 * sine(2 * math.pi * (400 + 900 * u) * t) * (1 - u)), 22050),
        "sound/fx/tw/click.wav": _wav(_synth(0.03, 22050, lambda t, u: 12000 * sine(2 * math.pi * 2000 * t) * (1 - u)), 22050),
        # loops: shore waves and lantern fire
        "sound/fx/tw/waves.wav": _wav(_synth(2.0, 22050, lambda t, u: noise[int(t * 22050) % 44100] * 3000 * (0.6 + 0.4 * sine(2 * math.pi * t * 0.5))), 22050),
        "sound/fx/tw/fire.wav": _wav(_synth(1.0, 22050, lambda t, u: noise[int(t * 22050)] * (5000 if noise[int(t * 2205)] > 0.7 else 900)), 22050),
        "sound/cr/tw/moan.wav": _wav(_synth(0.6, 22050, lambda t, u: 7000 * sine(2 * math.pi * (120 + 30 * sine(9 * t)) * t) * sine(math.pi * u)), 22050),
        "sound/cr/tw/roar.wav": _wav(_synth(0.6, 22050, lambda t, u: 9000 * sine(2 * math.pi * 70 * t) * noise[int(t * 22050)] * (1 - u)), 22050),
        # a voice line: silent MP3 frames (MP3 pass-through path)
        "sound/vo/tw/hello.mp3": _silent_mp3(60),
        "sound/vo/tw/attack.mp3": _silent_mp3(30),
        "sound/vo/tw/hit.mp3": _silent_mp3(20),
        "sound/vo/tw/welcome.mp3": _silent_mp3(80),
    }
    # four-second stereo theme for the explore playlist
    notes = [262, 330, 392, 330, 294, 349, 440, 349]
    theme = _synth(4.0, 22050, lambda t, u: 6000 * sine(2 * math.pi * notes[int(t * 2) % 8] * t) * (0.5 + 0.5 * sine(math.pi * ((t * 2) % 1))))
    stereo = []
    for i, v in enumerate(theme):
        stereo += [v, (v * 3) // 4]
    out["music/explore/tw_theme.wav"] = _wav(stereo, 22050, channels=2)
    out["music/battle/tw_battle.wav"] = _wav(_synth(2.0, 22050, lambda t, u: 7000 * sine(2 * math.pi * 110 * t) * (1 if (t * 4) % 1 < 0.5 else 0.3)), 22050)
    return out


SOUNDS = [  # SOUN id, file (relative to Sound/), volume, min, max
    ("FootBareLeft", "Fx\\tw\\step.wav", 200, 0, 0), ("FootBareRight", "Fx\\tw\\step.wav", 200, 0, 0),
    ("FootLightLeft", "Fx\\tw\\step.wav", 220, 0, 0), ("FootLightRight", "Fx\\tw\\step.wav", 220, 0, 0),
    ("FootMedLeft", "Fx\\tw\\step.wav", 230, 0, 0), ("FootMedRight", "Fx\\tw\\step.wav", 230, 0, 0),
    ("DefaultLand", "Fx\\tw\\step.wav", 255, 0, 0),
    ("Weapon Swish", "Fx\\tw\\swish.wav", 255, 0, 0), ("Health Damage", "Fx\\tw\\hit.wav", 255, 0, 0),
    ("Hand To Hand Hit", "Fx\\tw\\hit.wav", 255, 0, 0), ("Light Armor Hit", "Fx\\tw\\hit.wav", 255, 0, 0),
    ("Door Wood Open", "Fx\\tw\\door.wav", 255, 0, 0), ("Door Wood Close", "Fx\\tw\\door.wav", 255, 0, 0),
    ("destruction cast", "Fx\\tw\\cast.wav", 255, 0, 0), ("destruction hit", "Fx\\tw\\hit.wav", 255, 0, 0),
    ("restoration cast", "Fx\\tw\\cast.wav", 255, 0, 0), ("conjuration cast", "Fx\\tw\\cast.wav", 255, 0, 0),
    ("Menu Click", "Fx\\tw\\click.wav", 200, 0, 0), ("Item Misc Up", "Fx\\tw\\click.wav", 255, 0, 0),
    ("Item Gold Up", "Fx\\tw\\click.wav", 255, 0, 0), ("Open Lock", "Fx\\tw\\click.wav", 255, 0, 0),
    ("tw waves", "Fx\\tw\\waves.wav", 160, 10, 40), ("Fire", "Fx\\tw\\fire.wav", 180, 1, 10),
    ("tw critter moan", "Cr\\tw\\moan.wav", 255, 2, 20), ("tw critter roar", "Cr\\tw\\roar.wav", 255, 2, 25),
]


def sound_records() -> List[bytes]:
    recs = [esm.record("SOUN", [esm.sub("NAME", esm.zs(sid)), esm.sub("FNAM", esm.zs(fn)),
                                esm.sub("DATA", bytes((vol, mn, mx)))]) for sid, fn, vol, mn, mx in SOUNDS]
    for i, (gtype, snd) in enumerate(((0, "FootBareLeft"), (1, "FootBareRight"), (4, "tw critter moan"),
                                      (5, "tw critter roar"), (6, "tw critter roar"))):
        recs.append(esm.record("SNDG", [esm.sub("NAME", esm.zs(f"tw_critter{i:04d}")),
                                        esm.sub("DATA", struct.pack("<i", gtype)),
                                        esm.sub("CNAM", esm.zs("tw_critter")),
                                        esm.sub("SNAM", esm.zs(snd))]))
    return recs


def ai_wander(distance: int, idles=(40, 20, 10, 0, 0, 0, 0, 0)) -> bytes:
    return esm.sub("AI_W", struct.pack("<HHB8BB", distance, 0, 0, *idles, 1))


def pathgrid(name: str, gx: int, gy: int, points, edges) -> bytes:
    """points in cell-local (exterior) or interior coordinates; edges = pairs."""
    conn = [[] for _ in points]
    for a, b in edges:
        conn[a].append(b)
        conn[b].append(a)
    pgrp = b"".join(struct.pack("<iiiBBH", int(x), int(y), int(z), 1, len(conn[i]), 0)
                    for i, (x, y, z) in enumerate(points))
    pgrc = b"".join(struct.pack("<I", c) for cl in conn for c in cl)
    return esm.record("PGRD", [esm.sub("DATA", struct.pack("<iiHH", gx, gy, 128, len(points))),
                               esm.sub("NAME", esm.zs(name)), esm.sub("PGRP", pgrp),
                               esm.sub("PGRC", pgrc)])


def hamlet_pathgrid(house) -> bytes:
    pts, edges = [], []
    for k in range(10):
        a = 2 * math.pi * k / 10
        x, y = house[0] + 720.0 * math.cos(a), house[1] + 720.0 * math.sin(a)
        pts.append((x, y, terrain_height(x, y)))
        edges.append((k, (k + 1) % 10))
    # a spoke out to the guard post north of the house
    x, y = house[0], house[1] + 1300.0
    pts.append((x, y, terrain_height(x, y)))
    edges.append((len(pts) - 1, 2))
    edges.append((len(pts) - 1, 3))
    return pathgrid("Test Hamlet", 0, 0, pts, edges)


def cellar_pathgrid() -> bytes:
    pts = [(x, y, 0.0) for y in (-300.0, 0.0, 300.0) for x in (-300.0, 0.0, 300.0)]
    edges = []
    for r in range(3):
        for c in range(3):
            i = r * 3 + c
            if c < 2:
                edges.append((i, i + 1))
            if r < 2:
                edges.append((i, i + 3))
    return pathgrid(CELLAR, 0, 0, pts, edges)


def m6_records() -> List[bytes]:
    recs = sound_records()
    recs.append(esm.record("CLAS", [esm.sub("NAME", esm.zs("Guard")), esm.sub("FNAM", esm.zs("Guard")),
                                    esm.sub("CLDT", struct.pack("<15i", 0, 5, 0, 5, 3, 0, 4, 8, 2, 6, 7,
                                                                1, 18, 0, 0))]))
    recs.append(obj("MISC", "tw_goblet", "tw\\crate.nif", "Silver Goblet",
                    extra=[esm.sub("MCDT", struct.pack("<fii", 1.0, 60, 0))]))
    recs.append(spell_record("tw_summon", "Call Critter", 0, 10, 1, [effect(102, dur=20)]))
    recs.append(esm.record("GMST", [esm.sub("NAME", esm.zs("sMagicScampID")), esm.sub("STRV", b"tw_critter")]))
    recs.append(esm.record("CREA", [esm.sub("NAME", esm.zs("tw_snapper")),
                                    esm.sub("MODL", esm.zs("tw\\critter.nif")),
                                    esm.sub("FNAM", esm.zs("Shore Snapper")),
                                    esm.sub("NPDT", struct.pack("<24i", 0, 3, 40, 20, 20, 40, 40, 40, 20, 30,
                                                                40, 0, 50, 25, 60, 0, 30, 3, 8, 3, 8, 3, 8, 0)),
                                    esm.sub("FLAG", struct.pack("<i", 0x8)),
                                    esm.sub("AIDT", bytes([0, 0, 100, 20, 0, 0, 0, 0]) + struct.pack("<I", 0)),
                                    ai_wander(300)]))
    recs += m7_records()
    return recs


def m7_records() -> List[bytes]:
    """Milestone 7: voice lines, sleep creatures, a bow and arrows."""
    recs = []
    recs.append(dial("Hello", 1))
    recs += infos("Hello", [{"actor": "tw_guard", "text": "Citizen.", "sound": "Vo\\tw\\hello.mp3"}])
    recs.append(dial("Attack", 1))
    recs += infos("Attack", [{"text": "Die!", "sound": "Vo\\tw\\attack.mp3"}])
    recs.append(dial("Hit", 1))
    recs += infos("Hit", [{"text": "Argh!", "sound": "Vo\\tw\\hit.mp3"}])
    recs.append(esm.record("LEVC", [esm.sub("NAME", esm.zs("tw_sleep_creatures")),
                                    esm.sub("DATA", struct.pack("<i", 1)), esm.sub("NNAM", bytes([0])),
                                    esm.sub("INDX", struct.pack("<i", 2)),
                                    esm.sub("CNAM", esm.zs("tw_critter")), esm.sub("INTV", struct.pack("<h", 1)),
                                    esm.sub("CNAM", esm.zs("tw_snapper")), esm.sub("INTV", struct.pack("<h", 20))]))
    recs.append(obj("WEAP", "tw_bow", "tw\\crate.nif", "Shore Bow",
                    extra=[esm.sub("WPDT", struct.pack("<fihHffH6Bi", 6.0, 80, 9, 300, 1.0, 1.0, 0,
                                                       2, 10, 0, 0, 0, 0, 0))]))
    recs.append(obj("WEAP", "tw_arrow", "tw\\crate.nif", "Reed Arrow",
                    extra=[esm.sub("WPDT", struct.pack("<fihHffH6Bi", 0.1, 1, 12, 0, 1.0, 1.0, 0,
                                                       3, 8, 0, 0, 0, 0, 0))]))
    return recs


def actor_files() -> Dict[str, bytes]:
    files = {"meshes/base_anim.nif": mesh_skeleton(),
             "meshes/xbase_anim.kf": anim_xbase(),
             "meshes/tw/critter.nif": mesh_critter(),
             "meshes/tw/xcritter.nif": mesh_critter(),
             "meshes/tw/xcritter.kf": anim_critter()}
    for _bid, (part, _female, key, rigid) in PARTS.items():
        if rigid is None:
            tex = "tx_tw_cloth" if key.endswith("_f") else "tx_tw_planks"
            files[f"meshes/{key}.nif"] = mesh_folk_chest(tex)
        else:
            size, centre, tex = rigid
            files[f"meshes/{key}.nif"] = mesh_rigid_part(key.rsplit("/", 1)[1], size, centre, tex)
    return files


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
             dest_cell=None, owner=None) -> List[bytes]:
    subs = [esm.sub("FRMR", struct.pack("<I", index)), esm.sub("NAME", esm.zs(oid))]
    if owner:
        subs.append(esm.sub("ANAM", esm.zs(owner)))
    if scale is not None:
        subs.append(esm.sub("XSCL", struct.pack("<f", scale)))
    if door is not None:
        subs.append(esm.sub("DODT", struct.pack("<6f", *door)))
    if dest_cell:
        subs.append(esm.sub("DNAM", esm.zs(dest_cell)))
    subs.append(esm.sub("DATA", struct.pack("<6f", *pos, *rot)))
    return subs


# --------------------------------------------------------------- RPG layer
QUEST = "tw_lostring"

WEAVER_SCRIPT = """Begin tw_weaver_script
; local state read by the weaver's greetings and result scripts
short metpc
short asked
float idle
set idle to idle + GetSecondsPassed
End tw_weaver_script
"""

RING_SCRIPT = """Begin tw_ring_script
if ( OnActivate == 1 )
    if ( tw_ringfound == 0 )
        set tw_ringfound to 1
        Journal "tw_lostring" 20
        MessageBox "The ring glints in the dust."
    endif
    Activate
endif
End tw_ring_script
"""

MAIN_SCRIPT = """Begin tw_hamlet_main
short done
if ( done == 0 )
    set done to 1
    MessageBox "Welcome to the Test Hamlet, %s." "Outlander" "Thanks"
endif
if ( GetButtonPressed == 0 )
    set tw_welcomed to 1
endif
End tw_hamlet_main
"""


def script_record(name: str, source: str) -> bytes:
    loc_counts = {"s": 0, "l": 0, "f": 0}
    for line in source.splitlines():
        w = line.split()
        if len(w) >= 2 and w[0].lower() in ("short", "long", "float"):
            loc_counts[w[0][0].lower()] += 1
    schd = name.encode("latin-1").ljust(32, b"\0") + struct.pack(
        "<5I", loc_counts["s"], loc_counts["l"], loc_counts["f"], 0, 0)
    return esm.record("SCPT", [esm.sub("SCHD", schd), esm.sub("SCTX", source.encode("latin-1"))])


def dial(name: str, dtype: int) -> bytes:
    return esm.record("DIAL", [esm.sub("NAME", esm.zs(name)), esm.sub("DATA", bytes([dtype]))])


def infos(topic: str, entries) -> List[bytes]:
    """entries: dicts with text and optional actor/disp/selects/script/quest."""
    out = []
    ids = [f"{topic.replace(' ', '_')}_{i}" for i in range(len(entries))]
    for i, e in enumerate(entries):
        subs = [esm.sub("INAM", esm.zs(ids[i])),
                esm.sub("PNAM", esm.zs(ids[i - 1] if i else "")),
                esm.sub("NNAM", esm.zs(ids[i + 1] if i + 1 < len(entries) else "")),
                esm.sub("DATA", struct.pack("<iibbbb", 0, e.get("disp", 0), e.get("rank", -1),
                                            e.get("gender", -1), e.get("pcrank", -1), 0))]
        for key, tag in (("actor", "ONAM"), ("race", "RNAM"), ("class", "CNAM"),
                         ("faction", "FNAM"), ("cell", "ANAM"), ("pcfaction", "DNAM")):
            if e.get(key):
                subs.append(esm.sub(tag, esm.zs(e[key])))
        if e.get("sound"):
            subs.append(esm.sub("SNAM", esm.zs(e["sound"])))
        subs.append(esm.sub("NAME", e["text"].encode("latin-1")))
        for rule, value in e.get("selects", ()):
            subs.append(esm.sub("SCVR", rule.encode("latin-1")))
            if isinstance(value, float):
                subs.append(esm.sub("FLTV", struct.pack("<f", value)))
            else:
                subs.append(esm.sub("INTV", struct.pack("<i", value)))
        if e.get("script"):
            subs.append(esm.sub("BNAM", e["script"].encode("latin-1")))
        for flag in e.get("quest", ()):
            subs.append(esm.sub(flag, b"\x01"))
        out.append(esm.record("INFO", subs))
    return out


PLAYER_ITEMS = [("Gold_001", 100), ("tw_dagger", 1), ("tw_hammer", 1), ("tw_mortar", 1), ("tw_pick", 1),
                ("tw_soulgem", 1), ("tw_reed", 2), ("tw_root", 2), ("tw_potion_heal", 1),
                ("tw_scroll", 1), ("tw_skillbook", 1), ("tw_cuirass", 1), ("tw_boots", 1), ("tw_shield", 1)]

# magic effects used by the test content: index -> (school, base cost)
TEST_MGEFS = {102: (1, 10.0), 3: (0, 2.0), 7: (0, 1.0), 8: (0, 1.0), 9: (0, 3.0), 13: (0, 6.0), 14: (2, 5.0),
              16: (2, 5.0), 18: (2, 4.0), 23: (2, 8.0), 27: (2, 9.0), 39: (3, 40.0), 41: (3, 0.3),
              44: (3, 5.0), 45: (3, 40.0), 57: (4, 5.0), 58: (4, 2.0), 60: (4, 18.0), 61: (4, 18.0),
              62: (4, 15.0), 72: (5, 15.0), 75: (5, 5.0), 77: (5, 1.0), 79: (5, 1.0), 83: (5, 1.0),
              84: (5, 1.0), 86: (4, 8.0), 90: (5, 2.0), 117: (5, 1.0), 120: (1, 2.0)}


def effect(eff, rng=0, area=0, dur=0, mn=1, mx=1, skill=-1, attr=-1):
    return struct.pack("<hbbiiiii", eff, skill, attr, rng, area, dur, mn, mx)


def race_record(rid, name, bonuses, attrs, flags, powers):
    radt = b"".join(struct.pack("<ii", s, b) for s, b in bonuses)
    radt += b"".join(struct.pack("<ii", -1, 0) for _ in range(7 - len(bonuses)))
    radt += b"".join(struct.pack("<ii", a, a) for a in attrs)
    radt += struct.pack("<4fi", 1.0, 0.95, 1.0, 0.9, flags)
    subs = [esm.sub("NAME", esm.zs(rid)), esm.sub("FNAM", esm.zs(name)), esm.sub("RADT", radt)]
    subs += [esm.sub("NPCS", p.encode("latin-1").ljust(32, b"\0")) for p in powers]
    subs.append(esm.sub("DESC", esm.zs(f"The {name} of the test shore.")))
    return esm.record("RACE", subs)


def class_record(cid, name, attrs, spec, pairs, playable):
    vals = [attrs[0], attrs[1], spec] + [v for pr in pairs for v in pr] + [playable, 0]
    return esm.record("CLAS", [esm.sub("NAME", esm.zs(cid)), esm.sub("FNAM", esm.zs(name)),
                               esm.sub("CLDT", struct.pack("<15i", *vals))])


def spell_record(sid, name, stype, cost, flags, effects):
    return esm.record("SPEL", [esm.sub("NAME", esm.zs(sid)), esm.sub("FNAM", esm.zs(name)),
                               esm.sub("SPDT", struct.pack("<iii", stype, cost, flags))]
                      + [esm.sub("ENAM", e) for e in effects])


def rules_records() -> List[bytes]:
    import mwdata
    recs = []
    for i, (name, _g, attr, spec, use) in enumerate(mwdata.SKILLS):
        recs.append(esm.record("SKIL", [esm.sub("INDX", struct.pack("<i", i)),
                                        esm.sub("SKDT", struct.pack("<ii4f", attr, spec, *use)),
                                        esm.sub("DESC", esm.zs(f"The art of {name.lower()}."))]))
    for idx, (school, cost) in sorted(TEST_MGEFS.items()):
        recs.append(esm.record("MGEF", [esm.sub("INDX", struct.pack("<i", idx)),
                                        esm.sub("MEDT", struct.pack("<ifiiiifff", school, cost, 0x600,
                                                                    255, 128, 64, 1.0, 1.0, 1.0))]))
    recs += [
        class_record("tw_warrior", "Shore Warrior", (0, 5), 0, [(8, 5), (19, 3), (20, 0), (26, 7), (22, 2)], 1),
        class_record("tw_mage", "Shore Mage", (1, 2), 1, [(5, 10), (22, 11), (8, 15), (19, 14), (20, 16)], 1),
        esm.record("BSGN", [esm.sub("NAME", esm.zs("tw_sign_blade")), esm.sub("FNAM", esm.zs("The Blade")),
                            esm.sub("TNAM", esm.zs("birthsigns\\tw_blade.tga")),
                            esm.sub("DESC", esm.zs("Born under the Blade.")),
                            esm.sub("NPCS", b"tw_ability_attack".ljust(32, b"\0"))]),
        esm.record("BSGN", [esm.sub("NAME", esm.zs("tw_sign_star")), esm.sub("FNAM", esm.zs("The Star")),
                            esm.sub("DESC", esm.zs("Born under the Star.")),
                            esm.sub("NPCS", b"tw_ability_magicka".ljust(32, b"\0"))]),
        spell_record("tw_ability_attack", "Blade's Edge", 1, 0, 0, [effect(117, mn=10, mx=10)]),
        spell_record("tw_ability_magicka", "Star's Gift", 1, 0, 0, [effect(84, mn=5, mx=5)]),
        spell_record("tw_power", "Folk Strength", 5, 0, 0, [effect(79, dur=30, mn=20, mx=20, attr=0)]),
        spell_record("tw_heal", "Mend", 0, 5, 1 | 2, [effect(75, dur=5, mn=2, mx=2)]),
        spell_record("tw_bolt", "Frost Bolt", 0, 9, 1, [effect(16, rng=2, area=5, dur=1, mn=8, mx=12)]),
        spell_record("tw_trap", "Soul Snare", 0, 6, 1, [effect(58, rng=1, dur=30)]),
        spell_record("tw_disease", "Shore Cough", 3, 0, 0, [effect(77, mn=0, mx=0)]),
        obj("WEAP", "tw_sword", "tw\\crate.nif", "Hamlet Sword",
            extra=[esm.sub("WPDT", struct.pack("<fihHffH6Bi", 15.0, 50, 1, 400, 1.25, 1.0, 100,
                                               4, 12, 3, 10, 2, 8, 0))]),
        obj("WEAP", "bound_dagger", "tw\\crate.nif", "Bound Dagger",
            extra=[esm.sub("WPDT", struct.pack("<fihHffH6Bi", 0.0, 0, 0, 500, 2.5, 1.0, 0,
                                               10, 20, 10, 20, 10, 20, 1))]),
        obj("ARMO", "tw_cuirass", "tw\\crate.nif", "Reed Cuirass",
            extra=[esm.sub("AODT", struct.pack("<ifiiii", 1, 25.0, 80, 300, 50, 10))]),
        obj("ARMO", "tw_boots", "tw\\crate.nif", "Reed Boots",
            extra=[esm.sub("AODT", struct.pack("<ifiiii", 5, 6.0, 20, 100, 20, 4))]),
        obj("ARMO", "tw_shield", "tw\\crate.nif", "Reed Shield",
            extra=[esm.sub("AODT", struct.pack("<ifiiii", 8, 8.0, 30, 150, 30, 6))]),
        obj("ALCH", "tw_potion_heal", "tw\\crate.nif", "Potion of Mending",
            extra=[esm.sub("ALDT", struct.pack("<fii", 0.5, 15, 0)), esm.sub("ENAM", effect(75, dur=3, mn=10, mx=10))]),
        obj("INGR", "tw_reed", "tw\\crate.nif", "Marsh Reed",
            extra=[esm.sub("IRDT", struct.pack("<fi4i4i4i", 0.1, 2, 77, 79, -1, -1, -1, -1, -1, -1, -1, 3, -1, -1))]),
        obj("INGR", "tw_root", "tw\\crate.nif", "Bitter Root",
            extra=[esm.sub("IRDT", struct.pack("<fi4i4i4i", 0.1, 3, 77, 23, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1))]),
        obj("APPA", "tw_mortar", "tw\\crate.nif", "Apprentice Mortar",
            extra=[esm.sub("AADT", struct.pack("<iffi", 0, 0.5, 2.0, 6))]),
        obj("REPA", "tw_hammer", "tw\\crate.nif", "Repair Hammer",
            extra=[esm.sub("RIDT", struct.pack("<fiif", 2.0, 6, 10, 1.0))]),
        obj("LOCK", "tw_pick", "tw\\crate.nif", "Apprentice Lockpick",
            extra=[esm.sub("LKDT", struct.pack("<fifi", 0.2, 5, 1.0, 5))]),
        obj("MISC", "tw_soulgem", "tw\\crate.nif", "Petty Soul Gem",
            extra=[esm.sub("MCDT", struct.pack("<fii", 0.5, 10, 0))]),
        obj("BOOK", "tw_scroll", "tw\\crate.nif", "Scroll of Sparks",
            extra=[esm.sub("BKDT", struct.pack("<fiiii", 0.2, 20, 1, -1, 0)), esm.sub("ENAM", esm.zs("tw_scroll_en")),
                   esm.sub("TEXT", b"Sparks fly.")]),
        obj("BOOK", "tw_skillbook", "tw\\crate.nif", "On Reeds and Roots",
            extra=[esm.sub("BKDT", struct.pack("<fiiii", 1.0, 30, 0, 16, 0)),
                   esm.sub("TEXT", b"How to brew.<BR>Mix reeds and roots.")]),
        esm.record("ENCH", [esm.sub("NAME", esm.zs("tw_scroll_en")),
                            esm.sub("ENDT", struct.pack("<iiii", 0, 5, 5, 0)),
                            esm.sub("ENAM", effect(14, rng=2, dur=1, mn=10, mx=10))]),
        esm.record("STAT", [esm.sub("NAME", esm.zs("divinemarker"))]),
        esm.record("STAT", [esm.sub("NAME", esm.zs("prisonmarker"))]),
    ]
    return recs


def rpg_records() -> List[bytes]:
    W = "tw_villager_f"
    E = "tw_villager"
    recs = [
        esm.record("GLOB", [esm.sub("NAME", esm.zs("tw_ringfound")), esm.sub("FNAM", b"s"),
                            esm.sub("FLTV", struct.pack("<f", 0.0))]),
        esm.record("GLOB", [esm.sub("NAME", esm.zs("tw_welcomed")), esm.sub("FNAM", b"l"),
                            esm.sub("FLTV", struct.pack("<f", 0.0))]),
        esm.record("GLOB", [esm.sub("NAME", esm.zs("GameHour")), esm.sub("FNAM", b"f"),
                            esm.sub("FLTV", struct.pack("<f", 9.0))]),
        esm.record("GMST", [esm.sub("NAME", esm.zs("sJournalEntry")),
                            esm.sub("STRV", b"Your journal has been updated.")]),
        esm.record("GMST", [esm.sub("NAME", esm.zs("iTestValue")), esm.sub("INTV", struct.pack("<i", 7))]),
        esm.record("GMST", [esm.sub("NAME", esm.zs("fTestValue")), esm.sub("FLTV", struct.pack("<f", 2.5))]),
        esm.record("CLAS", [esm.sub("NAME", esm.zs("tw_weaver")), esm.sub("FNAM", esm.zs("Weaver")),
                            esm.sub("CLDT", struct.pack("<15i", 4, 6, 2, 9, 24, 16, 25, 8, 22, 18, 19,
                                                        20, 21, 0, 0))]),  # NPC-only class
        esm.record("CLAS", [esm.sub("NAME", esm.zs("tw_elder")), esm.sub("FNAM", esm.zs("Elder")),
                            esm.sub("CLDT", struct.pack("<15i", 1, 2, 0, 9, 24, 16, 25, 8, 22, 18, 19,
                                                        20, 21, 0, 0))]),  # NPC-only class
        esm.record("FACT", [esm.sub("NAME", esm.zs("tw_weavers")),
                            esm.sub("FNAM", esm.zs("Weavers' Circle"))]
                   + [esm.sub("RNAM", r.encode("latin-1").ljust(32, b"\0"))
                      for r in ("Novice", "Spinner", "Weaver", "Master Weaver")]
                   + [esm.sub("FADT", struct.pack("<2i50i7ii", 4, 6, *([30, 30, 10, 5, 0] * 10),
                                                  9, 24, 16, -1, -1, -1, -1, 0)),
                      esm.sub("ANAM", esm.zs("tw_weavers")), esm.sub("INTV", struct.pack("<i", 2))]),
        obj("MISC", "tw_ring", "tw\\crate.nif", "Weaver's Ring",
            extra=[esm.sub("MCDT", struct.pack("<fii", 0.1, 120, 0)), esm.sub("SCRI", esm.zs("tw_ring_script")),
                   esm.sub("ITEX", esm.zs("m\\tw_ring.tga"))]),
        obj("MISC", "Gold_001", "tw\\crate.nif", "Gold", extra=[esm.sub("MCDT", struct.pack("<fii", 0.0, 1, 0))]),
        obj("BOOK", "tw_book_history", "tw\\crate.nif", "A History of the Hamlet",
            extra=[esm.sub("BKDT", struct.pack("<fiiii", 1.0, 15, 0, -1, 0)),
                   esm.sub("TEXT", b'<DIV ALIGN="CENTER">A History of the Hamlet<BR>\r\n<BR>\r\n'
                                   b'<DIV ALIGN="LEFT">The hamlet was founded by weavers.<BR>\r\n'
                                   b'They came for the reeds &amp; the quiet.<P>The end.')]),
        obj("WEAP", "tw_dagger", "tw\\crate.nif", "Hamlet Dagger",
            extra=[esm.sub("WPDT", struct.pack("<fihHffH6Bi", 3.0, 10, 0, 300, 2.5, 1.0, 50,
                                               2, 6, 3, 7, 4, 8, 0)),
                   esm.sub("ENAM", esm.zs("tw_spark_en"))]),
        esm.record("SPEL", [esm.sub("NAME", esm.zs("tw_spark")), esm.sub("FNAM", esm.zs("Spark")),
                            esm.sub("SPDT", struct.pack("<iii", 0, 5, 1 | 2)),
                            esm.sub("ENAM", struct.pack("<hbbiiiii", 14, -1, -1, 2, 0, 1, 5, 10))]),
        esm.record("ENCH", [esm.sub("NAME", esm.zs("tw_spark_en")),
                            esm.sub("ENDT", struct.pack("<iiii", 2, 10, 100, 0)),
                            esm.sub("ENAM", struct.pack("<hbbiiiii", 14, -1, -1, 1, 0, 1, 2, 4))]),
        esm.record("SSCR", [esm.sub("DATA", b"0000000001"), esm.sub("NAME", esm.zs("tw_hamlet_main"))]),
        script_record("tw_weaver_script", WEAVER_SCRIPT),
        script_record("tw_ring_script", RING_SCRIPT),
        script_record("tw_hamlet_main", MAIN_SCRIPT),
        esm.record("NPC_", [esm.sub("NAME", esm.zs("player")), esm.sub("FNAM", esm.zs("player")),
                            esm.sub("RNAM", esm.zs("tw_folk")), esm.sub("CNAM", esm.zs("tw_elder")),
                            esm.sub("NPDT", struct.pack("<h8B27BBHHHBBbBi", 1, *([40] * 8), *([15] * 27),
                                                        0, 50, 40, 60, 50, 0, -1, 0, 0)),
                            esm.sub("FLAG", struct.pack("<i", 0))]
                   + [esm.sub("NPCO", struct.pack("<i", n) + i.encode("latin-1").ljust(32, b"\0"))
                      for i, n in PLAYER_ITEMS]),
    ]
    recs += rules_records()
    G = "tw_guard"
    recs.append(dial("Greeting 0", 2))
    recs += infos("Greeting 0", [
        # a guard confronting a criminal: the choice answers come first
        {"actor": G, "selects": [("01500", 1)], "text": "Good. Stay out of trouble.",
         "script": "PayFine\nGoodbye"},
        {"actor": G, "selects": [("01500", 2)], "text": "Come with me, then.", "script": "GoToJail"},
        {"actor": G, "selects": [("01500", 3)], "text": "Then pay with your blood!",
         "script": "StartCombat player\nGoodbye"},
        {"actor": G, "selects": [("01433", 1)],
         "text": "Stop right there! You have committed crimes against the hamlet. Pay your fine or go to jail.",
         "script": "Choice \"Pay the fine\" 1 \"Go to jail\" 2 \"Resist arrest\" 3"},
        {"actor": W, "text": "Oh! A stranger. I'm the hamlet weaver. Have you heard about my lost ring?",
         "selects": [("03sX0metpc", 0)], "script": "set metpc to 1\nAddTopic \"lost ring\""},
    ])
    recs.append(dial("Greeting 1", 2))
    recs += infos("Greeting 1", [
        {"actor": E, "text": "Welcome to the hamlet, %PCName. Ask me about the latest rumors.",
         "sound": "Vo\\tw\\welcome.mp3"},
        {"actor": G, "text": "Keep the peace, citizen."},
        {"actor": W, "text": "Hello again, %PCName. The %Class of the %Faction greets you."},
    ])
    recs.append(dial("latest rumors", 0))
    recs += infos("latest rumors", [
        {"actor": E, "selects": [("04JX3" + QUEST, 100)],
         "text": "The weaver is happy again. You did well."},
        {"text": "They say the weaver lost something precious. Ask her about her lost ring."},
    ])
    recs.append(dial("lost ring", 0))
    recs += infos("lost ring", [
        {"actor": W, "selects": [("01500", 1)], "text": "Thank you, %PCRace. Search the cellar.",
         "script": "Journal \"tw_lostring\" 10\nset asked to 1"},
        {"actor": W, "selects": [("01500", 2)], "text": "Pity. Leave me be, then.",
         "script": "Goodbye"},
        {"actor": W, "selects": [("04JX4" + QUEST, 10)],
         "text": "My ring! I dropped it somewhere in the cellar. Will you find it for me?",
         "script": "Choice \"I will find it.\" 1 \"Not now.\" 2"},
        {"actor": W, "selects": [("04JX4" + QUEST, 100), ("05IX0tw_ring", 0)],
         "text": "Have you found my ring yet?"},
        {"actor": W, "selects": [("04JX4" + QUEST, 100), ("05IX3tw_ring", 1)],
         "text": "You found it! Here, take this gold.",
         "script": "player->RemoveItem \"tw_ring\" 1\nplayer->AddItem \"Gold_001\" 50\n"
                   "Journal \"tw_lostring\" 100\nset metpc to 2\nModDisposition 20"},
        {"actor": W, "text": "Thank you again for finding my ring."},
    ])
    recs.append(dial("little secret", 0))
    recs += infos("little secret", [
        {"actor": W, "disp": 55, "text": "The reeds by the shore make the finest thread. Tell no one."},
    ])
    recs.append(dial("Info Refusal", 0))
    recs += infos("Info Refusal", [
        {"text": "I don't know you well enough to talk about that."},
    ])
    # Persuasion-type topics: answers to the persuasion menu and the
    # refusal shown when someone dislikes the player too much to trade
    for kind, ok, bad in (("Admire", "You are too kind, %PCName.", "Flattery will get you nowhere."),
                          ("Intimidate", "All right, all right!", "You don't scare me."),
                          ("Taunt", "How dare you!", "Is that the best you can do?"),
                          ("Bribe", "Most generous of you.", "Keep your coins.")):
        recs.append(dial(kind + " Success", 3))
        recs += infos(kind + " Success", [{"text": ok}])
        recs.append(dial(kind + " Fail", 3))
        recs += infos(kind + " Fail", [{"text": bad}])
    recs.append(dial("Service Refusal", 3))
    recs += infos("Service Refusal", [{"disp": 20, "text": "I will not deal with the likes of you."}])
    recs.append(dial(QUEST, 4))
    recs += infos(QUEST, [
        {"text": "The Weaver's Lost Ring", "quest": ("QSTN",)},
        {"disp": 10, "text": "The Hamlet Weaver lost her ring somewhere in the cellar. I promised to find it."},
        {"disp": 20, "text": "I found a ring in the cellar. It must be the weaver's."},
        {"disp": 100, "text": "I returned the ring to the Hamlet Weaver. She rewarded me with 50 gold.",
         "quest": ("QSTF",)},
    ])
    recs += [script_record("tw_unused_broken", "Begin tw_unused_broken\nset nothing to 1\nif ( 1 == 1 )\nEnd")]
    return recs


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
                            esm.sub("CNAM", struct.pack("<I", 0x00406080)),
                            esm.sub("SNAM", esm.zs("tw waves").ljust(32, b"\0") + bytes([30])),
                            esm.sub("BNAM", esm.zs("tw_sleep_creatures"))]),
        obj("STAT", "tw_house", "tw\\house.nif"),
        obj("STAT", "tw_rock", "tw\\rock.nif"),
        obj("STAT", "tw_tree", "tw\\tree.nif"),
        obj("STAT", "tw_cellar_room", "tw\\cellar_room.nif"),
        obj("CONT", "tw_crate", "tw\\crate.nif", "Crate",
            extra=[esm.sub("CNDT", struct.pack("<f", 100.0)), esm.sub("FLAG", struct.pack("<i", 8)),
                   esm.sub("CNTO", struct.pack("<i", 2) + esm.zs("tw_potion_heal").ljust(32, b"\0")),
                   esm.sub("CNTO", struct.pack("<i", 15) + esm.zs("Gold_001").ljust(32, b"\0"))]),
        obj("DOOR", "tw_door", "tw\\door.nif", "Door",
            extra=[esm.sub("SNAM", esm.zs("Door Wood Open")), esm.sub("ANAM", esm.zs("Door Wood Close"))]),
        obj("LIGH", "tw_lantern", "tw\\lantern.nif", "Lantern",
            extra=[esm.sub("LHDT", struct.pack("<fiiIII", 1.0, 5, -1, 384, 0x0060C0FF, 0x0008)),
                   esm.sub("SNAM", esm.zs("Fire"))]),
        obj("MISC", "tw_coin", "tw\\crate.nif", "Coin"),
        race_record("tw_folk", "Folk", [(5, 10), (19, 5)], [40, 40, 40, 40, 40, 40, 40, 40], 1,
                    ["tw_power"]),
        race_record("tw_beast", "Shore-Kin", [(20, 15), (8, 10)], [40, 35, 35, 50, 50, 40, 30, 40], 3, []),
        esm.record("CREA", [esm.sub("NAME", esm.zs("tw_critter")),
                            esm.sub("MODL", esm.zs("tw\\critter.nif")),
                            esm.sub("FNAM", esm.zs("Shore Critter")),
                            esm.sub("NPDT", struct.pack("<24i", 0, 2, 30, 20, 20, 40, 40, 30, 20, 30,
                                                        30, 0, 40, 25, 30, 0, 20, 2, 6, 1, 4, 1, 3, 0)),
                            esm.sub("FLAG", struct.pack("<i", 0x8)),
                            esm.sub("AIDT", bytes([0, 0, 40, 30, 0, 0, 0, 0]) + struct.pack("<I", 0)),
                            esm.sub("NPCO", struct.pack("<i", 1) + esm.zs("tw_root").ljust(32, b"\0"))]),
    ]
    for bid, (part, female, key, _rigid) in PARTS.items():
        recs.append(esm.record("BODY", [
            esm.sub("NAME", esm.zs(bid)), esm.sub("MODL", esm.zs(key.replace("/", "\\") + ".nif")),
            esm.sub("FNAM", esm.zs("tw_folk")),
            esm.sub("BYDT", bytes((part, 0, 1 if female else 0, 0)))]))
    for nid, name, female, cls, fac, disp, script, services, gold in (
            ("tw_villager", "Hamlet Elder", False, "tw_elder", "", 50, "",
             0x1 | 0x2 | 0x800 | 0x4000 | 0x8000 | 0x10000 | 0x20000, 300),
            ("tw_villager_f", "Hamlet Weaver", True, "tw_weaver", "tw_weavers", 40, "tw_weaver_script",
             0x4 | 0x10 | 0x100 | 0x400 | 0x2000, 200),
            ("tw_guard", "Hamlet Guard", False, "Guard", "", 50, "", 0, 0)):
        subs = [esm.sub("NAME", esm.zs(nid)), esm.sub("FNAM", esm.zs(name)),
                esm.sub("RNAM", esm.zs("tw_folk")), esm.sub("CNAM", esm.zs(cls)),
                esm.sub("BNAM", esm.zs("tw_folk_head")),
                esm.sub("NPDT", struct.pack("<hBBbxxxi", 3, disp, 5, 2 if fac else 0, gold)),
                esm.sub("FLAG", struct.pack("<i", (1 if female else 0) | 0x10)),
                esm.sub("AIDT", bytes([30, 0, 20 if nid != "tw_guard" else 30, 30,
                                       100 if nid == "tw_guard" else 0, 0, 0, 0]) + struct.pack("<I", services)),
                ai_wander(500 if nid == "tw_guard" else 0)]
        if nid == "tw_guard":
            subs.append(esm.sub("NPCO", struct.pack("<i", 1) + esm.zs("tw_sword").ljust(32, b"\0")))
        if nid == "tw_villager":
            for sp in ("tw_spark", "tw_heal", "tw_bolt"):
                subs.append(esm.sub("NPCS", sp.encode("latin-1").ljust(32, b"\0")))
            subs.append(esm.sub("DODT", struct.pack("<6f", 0.0, -300.0, 0.0, 0.0, 0.0, 0.0)))
            subs.append(esm.sub("DNAM", esm.zs(CELLAR)))
        if fac:
            subs.append(esm.sub("ANAM", esm.zs(fac)))
        if script:
            subs.append(esm.sub("SCRI", esm.zs(script)))
        subs.append(esm.sub("NPCO", struct.pack("<i", 2) + esm.zs("tw_coin").ljust(32, b"\0")))
        recs.append(esm.record("NPC_", subs))
    recs += rpg_records()
    recs += m6_records()
    house_pos = (1200.0, 900.0, terrain_height(1200.0, 900.0))
    door_out = (house_pos[0], house_pos[1] - 206.0, house_pos[2])  # door sits at wall y - 6
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
            subs += ref_subs(index, "divinemarker", (house_pos[0] - 600.0, house_pos[1] - 700.0,
                                                    terrain_height(house_pos[0] - 600.0, house_pos[1] - 700.0)))
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
            for nid, dx in (("tw_villager", 220.0), ("tw_villager_f", -220.0)):
                px, py = house_pos[0] + dx, house_pos[1] - 520.0
                subs += ref_subs(index, nid, (px, py, terrain_height(px, py)), (0.0, 0.0, math.pi))
                index += 1
            # M6: the elder's goblet (stealing), the guard north of the house
            gx, gy = house_pos[0] + 420.0, house_pos[1] - 560.0
            subs += ref_subs(index, "tw_goblet", (gx, gy, terrain_height(gx, gy)), scale=0.2,
                             owner="tw_villager")
            index += 1
            gx, gy = house_pos[0], house_pos[1] + 1300.0
            subs += ref_subs(index, "tw_guard", (gx, gy, terrain_height(gx, gy)), (0.0, 0.0, math.pi))
            index += 1
        if (cx, cy) == SNAPPER_CELL:
            sx, sy = snapper_pos()
            subs += ref_subs(index, "tw_snapper", (sx, sy, terrain_height(sx, sy)))
            index += 1
        recs.append(esm.record("CELL", subs))
        recs.append(land_record(cx, cy))
        if (cx, cy) == (0, 0):
            recs.append(hamlet_pathgrid(house_pos))
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
    cellar += ref_subs(index, "tw_critter", (-250.0, 150.0, 0.0), (0.0, 0.0, 2.2))
    index += 1
    cellar += [esm.sub("NAM0", struct.pack("<I", 1))]
    cellar += ref_subs(index, "tw_coin", (-200.0, 250.0, 0.0))
    index += 1
    cellar += ref_subs(index, "tw_ring", (150.0, -200.0, 0.0), (0.0, 0.0, 0.0), scale=0.25)
    index += 1
    cellar += ref_subs(index, "tw_book_history", (-300.0, -250.0, 0.0), (0.0, 0.0, 0.0), scale=0.3)
    index += 1
    cellar += ref_subs(index, "prisonmarker", (0.0, 250.0, 0.0))
    index += 1
    recs.append(esm.record("CELL", cellar))
    recs.append(cellar_pathgrid())
    return recs


def make(out_dir: str) -> str:
    data_dir = os.path.join(out_dir, "Data Files")
    os.makedirs(data_dir, exist_ok=True)
    files = {}
    files.update(textures())
    files.update(meshes())
    files.update(actor_files())
    files.update(sound_files())
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
