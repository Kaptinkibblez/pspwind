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
                                                        20, 21, 1, 0))]),
        esm.record("CLAS", [esm.sub("NAME", esm.zs("tw_elder")), esm.sub("FNAM", esm.zs("Elder")),
                            esm.sub("CLDT", struct.pack("<15i", 1, 2, 0, 9, 24, 16, 25, 8, 22, 18, 19,
                                                        20, 21, 1, 0))]),
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
                            esm.sub("SPDT", struct.pack("<iii", 0, 5, 0)),
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
                            esm.sub("FLAG", struct.pack("<i", 0))]),
    ]
    recs.append(dial("Greeting 0", 2))
    recs += infos("Greeting 0", [
        {"actor": W, "text": "Oh! A stranger. I'm the hamlet weaver. Have you heard about my lost ring?",
         "selects": [("03sX0metpc", 0)], "script": "set metpc to 1\nAddTopic \"lost ring\""},
    ])
    recs.append(dial("Greeting 1", 2))
    recs += infos("Greeting 1", [
        {"actor": E, "text": "Welcome to the hamlet, %PCName. Ask me about the latest rumors."},
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
        esm.record("RACE", [esm.sub("NAME", esm.zs("tw_folk")), esm.sub("FNAM", esm.zs("Folk")),
                            esm.sub("RADT", bytes(136) + struct.pack("<i", 1))]),
        esm.record("CREA", [esm.sub("NAME", esm.zs("tw_critter")),
                            esm.sub("MODL", esm.zs("tw\\critter.nif")),
                            esm.sub("FNAM", esm.zs("Shore Critter"))]),
    ]
    for bid, (part, female, key, _rigid) in PARTS.items():
        recs.append(esm.record("BODY", [
            esm.sub("NAME", esm.zs(bid)), esm.sub("MODL", esm.zs(key.replace("/", "\\") + ".nif")),
            esm.sub("FNAM", esm.zs("tw_folk")),
            esm.sub("BYDT", bytes((part, 0, 1 if female else 0, 0)))]))
    for nid, name, female, cls, fac, disp, script in (
            ("tw_villager", "Hamlet Elder", False, "tw_elder", "", 50, ""),
            ("tw_villager_f", "Hamlet Weaver", True, "tw_weaver", "tw_weavers", 40, "tw_weaver_script")):
        subs = [esm.sub("NAME", esm.zs(nid)), esm.sub("FNAM", esm.zs(name)),
                esm.sub("RNAM", esm.zs("tw_folk")), esm.sub("CNAM", esm.zs(cls)),
                esm.sub("BNAM", esm.zs("tw_folk_head")),
                esm.sub("NPDT", struct.pack("<hBBbxxxi", 3, disp, 5, 2 if fac else 0, 25)),
                esm.sub("FLAG", struct.pack("<i", 1 if female else 0))]
        if fac:
            subs.append(esm.sub("ANAM", esm.zs(fac)))
        if script:
            subs.append(esm.sub("SCRI", esm.zs(script)))
        subs.append(esm.sub("NPCO", struct.pack("<i", 2) + esm.zs("tw_coin").ljust(32, b"\0")))
        recs.append(esm.record("NPC_", subs))
    recs += rpg_records()
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
    cellar += ref_subs(index, "tw_critter", (-250.0, 150.0, 0.0), (0.0, 0.0, 2.2))
    index += 1
    cellar += [esm.sub("NAM0", struct.pack("<I", 1))]
    cellar += ref_subs(index, "tw_coin", (-200.0, 250.0, 0.0))
    index += 1
    cellar += ref_subs(index, "tw_ring", (150.0, -200.0, 0.0), (0.0, 0.0, 0.0), scale=0.25)
    index += 1
    cellar += ref_subs(index, "tw_book_history", (-300.0, -250.0, 0.0), (0.0, 0.0, 0.0), scale=0.3)
    index += 1
    recs.append(esm.record("CELL", cellar))
    return recs


def make(out_dir: str) -> str:
    data_dir = os.path.join(out_dir, "Data Files")
    os.makedirs(data_dir, exist_ok=True)
    files = {}
    files.update(textures())
    files.update(meshes())
    files.update(actor_files())
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
