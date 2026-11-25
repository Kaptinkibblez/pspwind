#!/usr/bin/env python3
"""world_demaker - Morrowind.esm worldspace -> discrete PSP zones.

* Exterior: the seamless grid is cut into N x N cell zones (default 3x3,
  24576 units square). Each zone also carries the ring of neighbouring
  cells as terrain-only "fringe" patches so the fogged horizon never shows a
  void; walking past the core cells triggers a loading-screen transition.
* Interior: one zone per interior cell.
* Terrain: LAND 65x65 heightfields are point-sampled to 17x17 vertex patches
  (512-unit quads). Each quad takes the VTEX texture covering it (the 16x16
  VTEX grid maps 1:1 onto the 16x16 quads), quads are batched per texture,
  and VCLR vertex colours x baked sun lighting become Gouraud vertex colours.
* Statics: small STAT models are pre-transformed and merged per cell and
  texture into a few big draw batches; large or animated models stay as
  instanced references (they also keep merged statics' collision).
* Interactive refs (doors, containers, lights, items, actors) stay instances;
  DODT/DNAM doors are resolved to destination zones.

Usage:
  world_demaker.py --data "/path/Morrowind/Data Files" \\
                   --meshes build/cooked/meshes.dpak --out build/cooked
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
from dataclasses import asdict, dataclass
from typing import Dict, List, Optional, Sequence, Set, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import dmcommon  # noqa: E402
import dmesh  # noqa: E402
import dzone  # noqa: E402
import esm  # noqa: E402
from dmcommon import LOG, fnv1a32  # noqa: E402

Vec3 = Tuple[float, float, float]
Mat = List[List[float]]

STOCK_MASTERS = ("morrowind.esm", "tribunal.esm", "bloodmoon.esm")
KIND_BY_TYPE = {
    "STAT": dzone.INST_STATIC, "ACTI": dzone.INST_ACTIVATOR, "DOOR": dzone.INST_DOOR,
    "CONT": dzone.INST_CONTAINER, "LIGH": dzone.INST_LIGHT, "NPC_": dzone.INST_NPC,
    "CREA": dzone.INST_CREATURE, "LEVC": dzone.INST_LEVELED, "LEVI": dzone.INST_LEVELED,
    "BODY": dzone.INST_OTHER,
}
ITEM_TYPES = ("MISC", "WEAP", "ARMO", "CLOT", "BOOK", "ALCH", "INGR", "APPA", "LOCK", "PROB",
              "REPA")
WATER_TEXTURE = "water/water00"

# BODY BYDT part (ESM::BodyPart::MeshPart) -> skeleton attachment bones; the
# second entry of a pair is the mirrored left side (OpenMW npcanimation
# part list).
PART_BONES = {
    0: ("Head",), 1: ("Head",), 2: ("Neck",), 3: ("Chest",), 4: ("Groin",),
    5: ("Right Hand", "Left Hand"), 6: ("Right Wrist", "Left Wrist"),
    7: ("Right Forearm", "Left Forearm"), 8: ("Right Upper Arm", "Left Upper Arm"),
    9: ("Right Foot", "Left Foot"), 10: ("Right Ankle", "Left Ankle"),
    11: ("Right Knee", "Left Knee"), 12: ("Right Upper Leg", "Left Upper Leg"),
    13: ("Right Clavicle", "Left Clavicle"), 14: ("Tail",),
}
BODY_SKIN = 0
BODY_F_FEMALE = 1
NPC_F_FEMALE = 1
RACE_F_BEAST = 2


@dataclass
class WorldConfig:
    zone_size: int = 3
    fringe: bool = True
    merge_max_tris: int = 400
    zone_budget: int = 7 * 1024 * 1024 + 512 * 1024  # ZONE region minus loader headroom
    ext_fog_color: Tuple[float, float, float] = (0.58, 0.60, 0.62)
    ext_fog_near: float = 1600.0
    ext_fog_far: float = 5200.0
    ext_ambient: Tuple[float, float, float] = (0.42, 0.42, 0.46)
    ext_sun_color: Tuple[float, float, float] = (0.85, 0.80, 0.72)
    sun_dir: Tuple[float, float, float] = (-0.40, -0.30, -0.866)
    int_fog_far_min: float = 1400.0
    int_fog_far_max: float = 6000.0


# --------------------------------------------------------------------------
# math
# --------------------------------------------------------------------------


def _mmul(a: Mat, b: Mat) -> Mat:
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def _rot(axis: int, angle: float) -> Mat:
    c, s = math.cos(angle), math.sin(angle)
    m = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]]
    i, j = [(1, 2), (2, 0), (0, 1)][axis]
    m[i][i], m[i][j], m[j][i], m[j][j] = c, -s, s, c
    return m


def placement_matrix(pos: Sequence[float], rot: Sequence[float], scale: float) -> Mat:
    """Morrowind reference transform: T(pos) * Rx(-rx) * Ry(-ry) * Rz(-rz) * S(scale)
    (OpenMW Misc::Convert::makeOsgQuat with OSG's apply-left-first product)."""
    r = _mmul(_mmul(_rot(0, -rot[0]), _rot(1, -rot[1])), _rot(2, -rot[2]))
    m = [[r[i][j] * scale for j in range(3)] + [pos[i]] for i in range(3)]
    m.append([0.0, 0.0, 0.0, 1.0])
    return m


def _xf(m: Mat, p: Sequence[float]) -> Vec3:
    return (m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3],
            m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3],
            m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3])


def _xn(m: Mat, n: Sequence[float]) -> Vec3:
    x = m[0][0] * n[0] + m[0][1] * n[1] + m[0][2] * n[2]
    y = m[1][0] * n[0] + m[1][1] * n[1] + m[1][2] * n[2]
    z = m[2][0] * n[0] + m[2][1] * n[1] + m[2][2] * n[2]
    ln = math.sqrt(x * x + y * y + z * z) or 1.0
    return (x / ln, y / ln, z / ln)


def _norm(v: Sequence[float]) -> Vec3:
    ln = math.sqrt(sum(c * c for c in v)) or 1.0
    return (v[0] / ln, v[1] / ln, v[2] / ln)


def _rgba_u32(v: int) -> Tuple[float, float, float]:
    return ((v & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0, ((v >> 16) & 0xFF) / 255.0)


# --------------------------------------------------------------------------
# cooked mesh library
# --------------------------------------------------------------------------


class MeshLib:
    def __init__(self, pack: Optional[dmcommon.DPakReader]) -> None:
        self.pack = pack
        self.cache: Dict[str, Optional[dict]] = {}

    def has(self, key: str) -> bool:
        return self.pack is not None and key in self.pack.entries

    def size(self, key: str) -> int:
        return self.pack.entries[key][2] if self.has(key) else 0

    def get(self, key: str) -> Optional[dict]:
        if key not in self.cache:
            if not self.has(key):
                self.cache[key] = None
            else:
                dm = dmesh.read_dmesh(self.pack.get(key))
                worlds: List[Mat] = []
                for n in dm["nodes"]:
                    t, q, s = n["translation"], n["rotation"], n["scale"]
                    x, y, z, w = q
                    local = [
                        [(1 - 2 * (y * y + z * z)) * s, 2 * (x * y - w * z) * s, 2 * (x * z + w * y) * s, t[0]],
                        [2 * (x * y + w * z) * s, (1 - 2 * (x * x + z * z)) * s, 2 * (y * z - w * x) * s, t[1]],
                        [2 * (x * z - w * y) * s, 2 * (y * z + w * x) * s, (1 - 2 * (x * x + y * y)) * s, t[2]],
                        [0.0, 0.0, 0.0, 1.0]]
                    worlds.append(local if n["parent"] < 0 else _mmul(worlds[n["parent"]], local))
                dm["node_worlds"] = worlds
                special = dmesh.NODE_F_BILLBOARD | dmesh.NODE_F_ANIMATED | dmesh.NODE_F_EMITTER
                dm["mergeable"] = (not dm["flags"] & (dmesh.DMESH_F_SKELETAL | dmesh.DMESH_F_ANIMATED
                                                      | dmesh.DMESH_F_PARTICLES)
                                   and not any(n["flags"] & special for n in dm["nodes"]))
                self.cache[key] = dm
        return self.cache[key]


# --------------------------------------------------------------------------
# builder
# --------------------------------------------------------------------------


class MergeSet:
    """Merged static batches of one cell, keyed by render state."""

    def __init__(self) -> None:
        self.open: Dict[tuple, dmesh.OutBatch] = {}
        self.batches: List[dmesh.OutBatch] = []


class WorldBuilder:
    def __init__(self, world: esm.World, meshes: MeshLib, cfg: WorldConfig,
                 anims: Optional[dmcommon.DPakReader] = None) -> None:
        self.world = world
        self.meshes = meshes
        self.cfg = cfg
        self.anims = anims
        self.body_by_race: Dict[str, List[esm.ObjectDef]] = {}
        for obj in world.objects.values():
            if obj.type == "BODY" and obj.body is not None and obj.body[3] == BODY_SKIN:
                self.body_by_race.setdefault(obj.race.lower(), []).append(obj)
        self.warnings: List[str] = []
        self.zone_entries: List[dzone.WorldZoneEntry] = []
        self.ext_zone: Dict[Tuple[int, int], int] = {}  # zone grid -> zone index
        self.int_zone: Dict[str, int] = {}  # lower-case cell name -> zone index
        self.textures_used: Set[str] = set()
        self.missing_models: Set[str] = set()

    # ------------------------------------------------------------- zoning
    def zone_of_cell(self, x: int, y: int) -> Tuple[int, int]:
        n = self.cfg.zone_size
        return (x // n, y // n)

    def plan(self) -> None:
        grids: Set[Tuple[int, int]] = set()
        for c in self.world.exterior_cells():
            if c.refs or (c.x, c.y) in self.world.lands:
                grids.add(self.zone_of_cell(c.x, c.y))
        for (x, y) in self.world.lands:
            grids.add(self.zone_of_cell(x, y))
        n = self.cfg.zone_size
        for g in sorted(grids):
            self.ext_zone[g] = len(self.zone_entries)
            rng = (g[0] * n, g[1] * n, g[0] * n + n - 1, g[1] * n + n - 1)
            self.zone_entries.append(dzone.WorldZoneEntry(
                f"zone/ext_{g[0]}_{g[1]}", self.exterior_zone_name(rng), dzone.KIND_EXTERIOR,
                0, 0, 0, rng))
        for c in sorted(self.world.interior_cells(), key=lambda c: c.name.lower()):
            name = c.name.lower()
            self.int_zone[name] = len(self.zone_entries)
            self.zone_entries.append(dzone.WorldZoneEntry(
                f"zone/int/{name}", c.name, dzone.KIND_INTERIOR, 0, fnv1a32(name), 1,
                (0, 0, 0, 0)))

    def exterior_zone_name(self, rng: Tuple[int, int, int, int]) -> str:
        names = []
        for x in range(rng[0], rng[2] + 1):
            for y in range(rng[1], rng[3] + 1):
                c = self.world.cells.get(("ext", x, y))
                if c is not None and c.name and c.name not in names:
                    names.append(c.name)
        if names:
            return ", ".join(names[:2])
        for x in range(rng[0], rng[2] + 1):
            for y in range(rng[1], rng[3] + 1):
                c = self.world.cells.get(("ext", x, y))
                if c is not None and c.region:
                    reg = self.world.regions.get(c.region.lower())
                    return reg["name"] if reg and reg["name"] else c.region
        return f"Wilderness ({rng[0]}, {rng[1]})"

    def dest_zone(self, ref: esm.CellRef) -> int:
        if ref.dest_cell:
            return self.int_zone.get(ref.dest_cell.lower(), dzone.NO_ZONE)
        if ref.door_dest is not None:
            cx = int(math.floor(ref.door_dest[0] / esm.CELL_SIZE))
            cy = int(math.floor(ref.door_dest[1] / esm.CELL_SIZE))
            return self.ext_zone.get(self.zone_of_cell(cx, cy), dzone.NO_ZONE)
        return dzone.NO_ZONE

    # ------------------------------------------------------------ terrain
    def terrain_patch(self, land: esm.Land, zone: dzone.Zone, cell: dzone.ZCell,
                      fringe: bool) -> None:
        if land.heights is None:
            return
        step = (esm.LAND_SIZE - 1) // (dzone.GRID - 1)  # 4
        spacing = esm.CELL_SIZE / (dzone.GRID - 1)  # 512
        ox, oy = land.x * esm.CELL_SIZE, land.y * esm.CELL_SIZE
        G = dzone.GRID
        heights, positions, normals, colors = [], [], [], []
        sun = _norm(self.cfg.sun_dir)
        amb = zone.ambient
        sun_c = zone.sun_color
        for j in range(G):
            for i in range(G):
                k = (j * step) * esm.LAND_SIZE + i * step
                h = land.heights[k]
                heights.append(h)
                positions.append((ox + i * spacing, oy + j * spacing, h))
                if land.normals is not None:
                    raw = land.normals[k * 3:k * 3 + 3]
                    n = _norm([b - 256 if b > 127 else b for b in raw])
                else:
                    n = None
                normals.append(n)
                if land.colors is not None:
                    c = land.colors[k * 3:k * 3 + 3]
                    colors.append((c[0] / 255.0, c[1] / 255.0, c[2] / 255.0))
                else:
                    colors.append((1.0, 1.0, 1.0))
        for j in range(G):  # heightfield normals where VNML is missing
            for i in range(G):
                if normals[j * G + i] is None:
                    hl = heights[j * G + max(i - 1, 0)]
                    hr = heights[j * G + min(i + 1, G - 1)]
                    hd = heights[max(j - 1, 0) * G + i]
                    hu = heights[min(j + 1, G - 1) * G + i]
                    normals[j * G + i] = _norm((hl - hr, hd - hu, 2.0 * spacing))
        lit = []
        for n, c in zip(normals, colors):
            ndl = max(0.0, -(n[0] * sun[0] + n[1] * sun[1] + n[2] * sun[2]))
            lit.append(tuple(min(1.0, c[k] * (amb[k] + sun_c[k] * ndl)) for k in range(3)) + (1.0,))

        flags = dzone.CELL_F_FRINGE if fringe else 0
        cell.terrain = len(zone.terrain)
        patch = dzone.ZTerrain(land.x, land.y, flags, (ox, oy), heights,
                               colors=[dmesh.pack_color(c) for c in lit],
                               quad_tex=[dzone.NONE16] * ((G - 1) * (G - 1)))
        zone.terrain.append(patch)

        by_tex: Dict[Optional[str], List[Tuple[int, int]]] = {}
        for j in range(G - 1):
            for i in range(G - 1):
                tex = None
                if land.textures is not None:
                    path = self.world.ltex_path(land, land.textures[j * 16 + i])
                    tex = dmcommon.texture_key(path) if path else None
                by_tex.setdefault(tex, []).append((i, j))
        for tex, quads in sorted(by_tex.items(), key=lambda kv: kv[0] or ""):
            remap: Dict[int, int] = {}
            b = dmesh.OutBatch(node=0, texture=dmesh.DMESH_NO_TEXTURE, flags=0, alpha_ref=0,
                               blend=0, diffuse=(1.0, 1.0, 1.0, 1.0), emissive=(0.0, 0.0, 0.0),
                               positions=[], normals=[], uvs=[], colors=[], indices=[])

            def vid(i: int, j: int) -> int:
                k = j * G + i
                if k not in remap:
                    remap[k] = len(b.positions)
                    b.positions.append(positions[k])
                    b.normals.append(normals[k])
                    b.uvs.append((float(i), float(j)))
                    b.colors.append(lit[k])
                return remap[k]

            for i, j in quads:
                a, c1, c2, d = vid(i, j), vid(i + 1, j), vid(i + 1, j + 1), vid(i, j + 1)
                b.indices += [a, c1, c2, a, c2, d]  # counter-clockwise seen from +Z
            if tex is not None:
                b.texture = self.texture_index(zone, tex)
                for i, j in quads:
                    patch.quad_tex[j * (G - 1) + i] = b.texture
            cell.batches.append(b)
            patch.batches.append(b)

    def texture_index(self, zone: dzone.Zone, key: str) -> int:
        self.textures_used.add(key)
        if key not in zone.textures:
            zone.textures.append(key)
        return zone.textures.index(key)

    # ------------------------------------------------------------- refs
    def add_refs(self, zone: dzone.Zone, cell: dzone.ZCell, src: esm.Cell, merge: bool,
                 merged: MergeSet) -> None:
        for (content, index), ref in sorted(src.refs.items()):
            obj = self.world.objects.get(ref.id.lower())
            if obj is None:
                self.warnings.append(f"{src.name or (src.x, src.y)}: unknown object {ref.id!r}")
                continue
            if obj.type in ("NPC_", "CREA"):
                self.add_actor(zone, cell, ref, obj, content, index)
                continue
            kind = KIND_BY_TYPE.get(obj.type, dzone.INST_ITEM if obj.type in ITEM_TYPES
                                    else dzone.INST_OTHER)
            flags = dzone.IF_PERSISTENT if ref.persistent else 0
            mkey = dmcommon.mesh_key(obj.model) if obj.model else ""
            dm = self.meshes.get(mkey) if mkey else None
            mesh_index = dzone.NONE16
            m = placement_matrix(ref.pos, ref.rot, ref.scale)
            if dm is None:
                flags |= dzone.IF_NO_MESH
                if mkey and obj.type not in ("NPC_", "LEVC", "LEVI"):
                    self.missing_models.add(mkey)
                bound = (ref.pos[0], ref.pos[1], ref.pos[2] + 64.0, 96.0 * ref.scale)
            else:
                if mkey not in zone.meshes:
                    zone.meshes.append(mkey)
                mesh_index = zone.meshes.index(mkey)
                c = _xf(m, dm["bound_center"])
                bound = (c[0], c[1], c[2], dm["bound_radius"] * ref.scale)
                if (merge and obj.type == "STAT" and dm["mergeable"]
                        and dm["total_triangles"] <= self.cfg.merge_max_tris):
                    self.merge_instance(zone, m, dm, merged)
                    flags |= dzone.IF_MERGED
            ins = dzone.ZInstance(mesh_index, kind, flags, None, ref.id,
                                  ((content & 0xFF) << 24) | (index & 0xFFFFFF),
                                  ref.pos, ref.rot, ref.scale, bound)
            if obj.type == "DOOR" and (ref.door_dest is not None or ref.dest_cell):
                dd = ref.door_dest or (0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
                ins.door = dzone.ZDoor(self.dest_zone(ref), tuple(dd[:3]), tuple(dd[3:]),
                                       ref.dest_cell, ref.lock_level, ref.key, ref.trap)
            if obj.type == "LIGH" and obj.light is not None:
                ins.light = dzone.ZLight(tuple(ref.pos), float(obj.light["radius"]),
                                         obj.light["color"], obj.light["flags"])
            cell.instances.append(ins)

    # ------------------------------------------------------------ actors
    def zone_mesh(self, zone: dzone.Zone, key: str) -> Optional[int]:
        if not key or self.meshes.get(key) is None:
            if key:
                self.missing_models.add(key)
            return None
        if key not in zone.meshes:
            zone.meshes.append(key)
        return zone.meshes.index(key)

    def zone_anim(self, zone: dzone.Zone, key: str) -> int:
        if self.anims is None or key not in self.anims.entries:
            return dzone.NONE16
        if key not in zone.anims:
            zone.anims.append(key)
        return zone.anims.index(key)

    @staticmethod
    def x_variant(key: str) -> str:
        """'r/rat' -> 'r/xrat' (actor model / .kf naming, OpenMW correctActorModelPath)."""
        head, _, base = key.rpartition("/")
        return f"{head}/x{base}" if head else f"x{base}"

    def body_parts(self, npc: esm.ObjectDef, female: bool) -> List[esm.ObjectDef]:
        """Race skin parts for the NPC's sex (female NPCs fall back to the
        male part of a slot, male NPCs never get female parts), plus the
        NPC's own head and hair."""
        best: Dict[int, Tuple[int, esm.ObjectDef]] = {}
        for bp in self.body_by_race.get(npc.race.lower(), []):
            part, vampire, flags, _type = bp.body  # type: ignore[misc]
            if vampire or part in (0, 1):
                continue
            is_female = bool(flags & BODY_F_FEMALE)
            if is_female and not female:
                continue
            score = 1 if is_female == female else 0
            if part not in best or score > best[part][0]:
                best[part] = (score, bp)
        chosen = {part: bp for part, (_s, bp) in best.items()}
        for oid in (npc.head, npc.hair):
            bp = self.world.objects.get(oid.lower()) if oid else None
            if bp is not None and bp.body is not None:
                chosen[bp.body[0]] = bp
        return [chosen[k] for k in sorted(chosen)]

    def add_actor(self, zone: dzone.Zone, cell: dzone.ZCell, ref: esm.CellRef,
                  obj: esm.ObjectDef, content: int, index: int) -> None:
        flags = 0
        parts: List[Tuple[int, int, bool]] = []
        anim_key = ""
        if obj.type == "CREA":
            flags |= dzone.ACTOR_F_CREATURE
            base = dmcommon.mesh_key(obj.model) if obj.model else ""
            xkey = self.x_variant(base) if base else ""
            skel_key = xkey if xkey and self.meshes.get(xkey) is not None else base
            anim_key = xkey
        else:
            race = self.world.races.get(obj.race.lower(), {})
            beast = bool(race.get("flags", 0) & RACE_F_BEAST)
            female = bool(obj.npc_flags & NPC_F_FEMALE)
            if beast:
                flags |= dzone.ACTOR_F_BEAST
            if female:
                flags |= dzone.ACTOR_F_FEMALE
            if obj.model:
                skel_key = dmcommon.mesh_key(obj.model)
                anim_key = self.x_variant(skel_key)
            else:
                candidates = (["base_animkna"] if beast else
                              (["base_anim_female", "base_anim"] if female else ["base_anim"]))
                skel_key = next((k for k in candidates if self.meshes.get(k) is not None),
                                candidates[-1])
                anim_key = self.x_variant(skel_key)
            seen_skinned: Set[int] = set()
            for bp in self.body_parts(obj, female):
                mi = self.zone_mesh(zone, dmcommon.mesh_key(bp.model) if bp.model else "")
                if mi is None:
                    continue
                dm = self.meshes.get(zone.meshes[mi])
                skinned = any(b["flags"] & dmesh.BATCH_F_SEGMENT for b in dm["batches"])
                if skinned:
                    if mi not in seen_skinned:  # segments bind to bones by name
                        seen_skinned.add(mi)
                        parts.append((mi, 0, False))
                    continue
                bones = PART_BONES.get(bp.body[0], ())
                for side, bone in enumerate(bones):
                    parts.append((mi, fnv1a32(bone), side == 1))
            if len(parts) > dzone.ACTOR_MAX_PARTS:
                self.warnings.append(f"{obj.id}: {len(parts)} body parts, keeping 16")
                parts = parts[:dzone.ACTOR_MAX_PARTS]
        skel = self.zone_mesh(zone, skel_key)
        kind = dzone.INST_CREATURE if obj.type == "CREA" else dzone.INST_NPC
        iflags = dzone.IF_ACTOR | (dzone.IF_PERSISTENT if ref.persistent else 0)
        if skel is None:
            iflags |= dzone.IF_NO_MESH
        bound = (ref.pos[0], ref.pos[1], ref.pos[2] + 64.0 * ref.scale, 96.0 * ref.scale)
        if skel is not None and obj.type == "CREA":
            dm = self.meshes.get(zone.meshes[skel])
            m = placement_matrix(ref.pos, ref.rot, ref.scale)
            c = _xf(m, dm["bound_center"])
            if dm["bound_radius"] > 1.0:
                bound = (c[0], c[1], c[2], dm["bound_radius"] * ref.scale)
        ins = dzone.ZInstance(skel if skel is not None else dzone.NONE16, kind, iflags, None,
                              ref.id, ((content & 0xFF) << 24) | (index & 0xFFFFFF), ref.pos,
                              ref.rot, ref.scale, bound)
        cell.instances.append(ins)
        if skel is not None:
            zone.actors.append(dzone.ZActor(ins, skel, self.zone_anim(zone, anim_key), flags, parts))

    def merge_instance(self, zone: dzone.Zone, m: Mat, dm: dict,
                       merged: MergeSet) -> None:
        for b in dm["batches"]:
            tex = dm["textures"][b["texture"]]["name"] if b["texture"] != dmesh.DMESH_NO_TEXTURE \
                else None
            key = (tex, b["flags"], b["alpha_ref"], b["blend"])
            out = merged.open.get(key)
            if out is None or len(out.positions) + len(b["vertices"]) > 65535:
                out = dmesh.OutBatch(
                    node=0, texture=self.texture_index(zone, tex) if tex else dmesh.DMESH_NO_TEXTURE,
                    flags=b["flags"], alpha_ref=b["alpha_ref"], blend=b["blend"],
                    diffuse=b["diffuse"], emissive=b["emissive"][:3], positions=[], normals=[],
                    uvs=[], colors=[], indices=[])
                merged.open[key] = out
                merged.batches.append(out)
            full = _mmul(m, dm["node_worlds"][b["node"]])
            base = len(out.positions)
            for v in b["vertices"]:
                out.positions.append(_xf(full, v["pos"]))
                out.normals.append(_xn(full, v["normal"]))
                out.uvs.append(v["uv"])
                out.colors.append(v["color"])
            out.indices.extend(base + i for i in b["indices"])

    # ------------------------------------------------------------ zones
    def build_exterior(self, zi: int, merge: bool) -> dzone.Zone:
        ent = self.zone_entries[zi]
        x0, y0, x1, y1 = ent.cell_range
        cfg = self.cfg
        zone = dzone.Zone(ent.entry, ent.name, dzone.KIND_EXTERIOR,
                          dzone.F_WATER | dzone.F_TERRAIN, ent.cell_range, 0.0,
                          cfg.ext_fog_color, cfg.ext_fog_near, cfg.ext_fog_far, cfg.ext_ambient,
                          cfg.ext_sun_color, _norm(cfg.sun_dir))
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                src = self.world.cells.get(("ext", x, y))
                land = self.world.lands.get((x, y))
                if src is None and land is None:
                    continue
                cell = dzone.ZCell(x, y, dzone.CELL_F_WATER, src.name if src else "",
                                   src.region if src else "", 0.0)
                if land is not None:
                    self.terrain_patch(land, zone, cell, False)
                if src is not None:
                    merged = MergeSet()
                    self.add_refs(zone, cell, src, merge, merged)
                    cell.batches.extend(merged.batches)
                zone.cells.append(cell)
        if cfg.fringe:
            for x in range(x0 - 1, x1 + 2):
                for y in range(y0 - 1, y1 + 2):
                    if x0 <= x <= x1 and y0 <= y <= y1:
                        continue
                    land = self.world.lands.get((x, y))
                    if land is None or land.heights is None:
                        continue
                    cell = dzone.ZCell(x, y, dzone.CELL_F_FRINGE | dzone.CELL_F_WATER, "", "", 0.0)
                    self.terrain_patch(land, zone, cell, True)
                    zone.cells.append(cell)
        if zone.terrain:
            self.texture_index(zone, WATER_TEXTURE)
        else:
            zone.flags &= ~dzone.F_TERRAIN
        return zone

    def build_interior(self, zi: int, merge: bool) -> dzone.Zone:
        ent = self.zone_entries[zi]
        src = self.world.cells[("int", ent.name.lower())]
        cfg = self.cfg
        fog_color, ambient, sun = (0.05, 0.05, 0.06), (0.35, 0.33, 0.30), (0.5, 0.45, 0.40)
        fog_far = cfg.int_fog_far_max
        if src.ambient is not None:
            amb, sunl, fog, density = src.ambient
            ambient, sun, fog_color = _rgba_u32(amb), _rgba_u32(sunl), _rgba_u32(fog)
            density = max(0.0, min(1.0, density))
            fog_far = cfg.int_fog_far_max + (cfg.int_fog_far_min - cfg.int_fog_far_max) * density
        flags = 0
        water = 0.0
        if src.flags & esm.CELL_HAS_WATER:
            flags |= dzone.F_WATER
            water = src.water if src.water is not None else 0.0
        if src.flags & esm.CELL_QUASI_EX:
            flags |= dzone.F_QUASI_EXTERIOR
        zone = dzone.Zone(ent.entry, ent.name, dzone.KIND_INTERIOR, flags, (0, 0, 0, 0), water,
                          fog_color, fog_far * 0.3, fog_far, ambient, sun, _norm(cfg.sun_dir))
        cell = dzone.ZCell(0, 0, dzone.CELL_F_WATER if flags & dzone.F_WATER else 0, src.name,
                           "", water)
        merged = MergeSet()
        self.add_refs(zone, cell, src, merge, merged)
        cell.batches.extend(merged.batches)
        zone.cells.append(cell)
        if flags & dzone.F_WATER:
            self.texture_index(zone, WATER_TEXTURE)
        return zone

    def zone_cost(self, data: bytes, zone: dzone.Zone) -> int:
        return len(data) + sum(self.meshes.size(m) for m in zone.meshes)

    def build_zone(self, zi: int) -> Tuple[bytes, dict]:
        ent = self.zone_entries[zi]
        build = self.build_exterior if ent.kind == dzone.KIND_EXTERIOR else self.build_interior
        zone = build(zi, True)
        data = dzone.write_zone(zone)
        merged = True
        cost = self.zone_cost(data, zone)
        if cost > self.cfg.zone_budget:
            zone = build(zi, False)  # merging duplicates geometry: fall back to instancing
            data = dzone.write_zone(zone)
            merged = False
            cost = self.zone_cost(data, zone)
            if cost > self.cfg.zone_budget:
                self.warnings.append(f"{ent.entry}: {cost} bytes exceeds zone budget")
        ent.bytes = len(data)
        ent.flags = zone.flags
        ent.cell_count = sum(1 for c in zone.cells if not c.flags & dzone.CELL_F_FRINGE)
        info = {"entry": ent.entry, "name": ent.name, "bytes": len(data), "mesh_bytes": cost - len(data),
                "actors": len(zone.actors), "anims": list(zone.anims),
                "cells": len(zone.cells), "instances": sum(len(c.instances) for c in zone.cells),
                "batches": sum(len(c.batches) for c in zone.cells), "meshes": len(zone.meshes),
                "textures": len(zone.textures), "merged": merged}
        return data, info


def discover_content(data_dirs: Sequence[str], explicit: Sequence[str]) -> List[str]:
    if explicit:
        return list(explicit)
    found: Dict[str, str] = {}
    for d in data_dirs:
        for f in os.listdir(d):
            if f.lower().endswith((".esm", ".esp")):
                found[f.lower()] = os.path.join(d, f)
    order = [found.pop(m) for m in STOCK_MASTERS if m in found]
    order += [found[k] for k in sorted(found) if k.endswith(".esm")]
    order += [found[k] for k in sorted(found) if k.endswith(".esp")]
    return order


def cook_world(content: Sequence[str], meshes_path: Optional[str], out_dir: str,
               cfg: WorldConfig, anims_path: Optional[str] = None) -> dict:
    t0 = time.time()
    world = esm.World().load(content)
    LOG.info("world: %d objects, %d cells, %d lands from %d files", len(world.objects),
             len(world.cells), len(world.lands), len(content))
    pack = dmcommon.DPakReader(dmcommon.read_file(meshes_path)) if meshes_path else None
    anims = dmcommon.DPakReader(dmcommon.read_file(anims_path)) if anims_path and \
        os.path.exists(anims_path) else None
    builder = WorldBuilder(world, MeshLib(pack), cfg, anims)
    builder.plan()
    out = dmcommon.DPakWriter(dmcommon.DPAK_KIND_GENERIC)
    zones_info = []
    report = dmcommon.progress_printer("zones")
    for zi, ent in enumerate(builder.zone_entries):
        data, info = builder.build_zone(zi)
        out.add(ent.entry, data)
        zones_info.append(info)
        report(zi + 1, len(builder.zone_entries))
    ext_map: Dict[Tuple[int, int], int] = {}
    for zi, ent in enumerate(builder.zone_entries):
        if ent.kind == dzone.KIND_EXTERIOR:
            x0, y0, x1, y1 = ent.cell_range
            for x in range(x0, x1 + 1):
                for y in range(y0, y1 + 1):
                    if ("ext", x, y) in world.cells or (x, y) in world.lands:
                        ext_map[(x, y)] = zi
    out.add("world", dzone.write_world(cfg.zone_size, builder.zone_entries, ext_map,
                                       builder.int_zone))
    os.makedirs(out_dir, exist_ok=True)
    size = out.write(os.path.join(out_dir, "zones.dpak"))
    manifest = {"config": asdict(cfg), "content": [os.path.basename(c) for c in content],
                "zones": zones_info, "textures": sorted(builder.textures_used),
                "missing_models": sorted(builder.missing_models),
                "warnings": world.warnings + builder.warnings, "pack_bytes": size}
    with open(os.path.join(out_dir, "zones_manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    LOG.info("wrote zones.dpak: %d zones, %.1f KB, %d warnings, %.1fs", len(zones_info),
             size / 1024.0, len(manifest["warnings"]), time.time() - t0)
    return manifest


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", action="append", default=[], help="Data Files directory")
    ap.add_argument("--esm", action="append", default=[],
                    help="content file in load order (default: masters found in --data)")
    ap.add_argument("--meshes", help="meshes.dpak from mesh_demaker (enables merging/bounds)")
    ap.add_argument("--anims", help="anims.dpak from anim_demaker (actor animations)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--zone-size", type=int, default=3)
    ap.add_argument("--no-fringe", action="store_true")
    ap.add_argument("--merge-max-tris", type=int, default=400)
    ap.add_argument("--fog-far", type=float, default=5200.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    dmcommon.setup_logging(args.verbose)
    content = discover_content(args.data, args.esm)
    if not content:
        ap.error("no .esm/.esp found; pass --data or --esm")
    if args.zone_size < 1:
        ap.error("--zone-size must be >= 1")
    cfg = WorldConfig(zone_size=args.zone_size, fringe=not args.no_fringe,
                      merge_max_tris=args.merge_max_tris, ext_fog_far=args.fog_far,
                      ext_fog_near=args.fog_far * 0.3)
    cook_world(content, args.meshes, args.out, cfg, args.anims)
    return 0


if __name__ == "__main__":
    sys.exit(main())
