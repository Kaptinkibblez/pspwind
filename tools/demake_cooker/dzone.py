"""DZONE / DWORLD writer + reader - mirrors src/fmt.h byte for byte."""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

import dmesh
from dmcommon import BinWriter, StringTable, fnv1a32

DZONE_MAGIC = b"DZON"
DZONE_VERSION = 2
DWORLD_MAGIC = b"DWLD"
DWORLD_VERSION = 1
GRID = 17
NONE16 = 0xFFFF
NO_ZONE = 0xFFFFFFFF

KIND_EXTERIOR = 0
KIND_INTERIOR = 1
F_WATER = 1 << 0
F_TERRAIN = 1 << 1
F_QUASI_EXTERIOR = 1 << 2
CELL_F_FRINGE = 1 << 0
CELL_F_WATER = 1 << 1

INST_STATIC, INST_ACTIVATOR, INST_DOOR, INST_CONTAINER, INST_LIGHT, INST_ITEM, \
    INST_NPC, INST_CREATURE, INST_LEVELED, INST_OTHER = range(10)
IF_MERGED = 1 << 0
IF_PERSISTENT = 1 << 1
IF_NO_MESH = 1 << 2
IF_ACTOR = 1 << 3
ACTOR_MAX_PARTS = 16
ACTOR_F_FEMALE = 1 << 0
ACTOR_F_BEAST = 1 << 1
ACTOR_F_CREATURE = 1 << 2

HEADER_FMT = "<4sHBBIII4hfIffII3fHHHHIIII9I3f3fHHII"
CELL_FMT = "<hhHHIIIIIIIf2I"
TERRAIN_FMT = "<hhHH2f2fIIIIII"
INSTANCE_FMT = "<HBBHHII3f3ff4fI"
DOOR_FMT = "<II3f3fIiII"
LIGHT_FMT = "<3ffIIII"
ACTOR_FMT = "<IHHHH16HII3I"
WORLD_HEADER_FMT = "<4sHHIIIIIIII2I"
WORLD_ZONE_FMT = "<IIIBBH4hII"
WORLD_EXT_FMT = "<hhI"
WORLD_INT_FMT = "<II"

SIZES = {name: struct.calcsize(fmt) for name, fmt in (
    ("DZoneHeader", HEADER_FMT), ("DZoneCell", CELL_FMT), ("DZoneTerrain", TERRAIN_FMT),
    ("DZoneInstance", INSTANCE_FMT), ("DZoneDoor", DOOR_FMT), ("DZoneLight", LIGHT_FMT),
    ("DZoneActor", ACTOR_FMT),
    ("DWorldHeader", WORLD_HEADER_FMT), ("DWorldZone", WORLD_ZONE_FMT),
    ("DWorldExt", WORLD_EXT_FMT), ("DWorldInt", WORLD_INT_FMT))}
assert SIZES == {"DZoneHeader": 160, "DZoneCell": 48, "DZoneTerrain": 48, "DZoneInstance": 64,
                 "DZoneDoor": 48, "DZoneLight": 32, "DZoneActor": 64, "DWorldHeader": 48, "DWorldZone": 32,
                 "DWorldExt": 8, "DWorldInt": 8}, SIZES

Vec3 = Tuple[float, float, float]


@dataclass
class ZCell:
    x: int
    y: int
    flags: int
    name: str
    region: str
    water: float
    terrain: int = NONE16
    batches: List[dmesh.OutBatch] = field(default_factory=list)
    instances: List["ZInstance"] = field(default_factory=list)


@dataclass
class ZTerrain:
    x: int
    y: int
    flags: int
    origin: Tuple[float, float]
    heights: List[float]  # 17*17
    colors: List[int] = field(default_factory=list)  # 17*17 ABGR8888
    quad_tex: List[int] = field(default_factory=list)  # 16*16 zone texture index / NONE16
    batches: List[dmesh.OutBatch] = field(default_factory=list)  # its static batches


@dataclass
class ZInstance:
    mesh: int
    kind: int
    flags: int
    door: Optional["ZDoor"]
    object_id: str
    refnum: int
    pos: Vec3
    rot: Vec3
    scale: float
    bound: Tuple[float, float, float, float]
    light: Optional["ZLight"] = None
    cell_slot: int = 0


@dataclass
class ZActor:
    instance: "ZInstance"
    skeleton: int
    anim: int
    flags: int
    parts: List[Tuple[int, int, bool]] = field(default_factory=list)  # mesh, bone hash, mirror


@dataclass
class ZDoor:
    dest_zone: int
    dest_pos: Vec3
    dest_rot: Vec3
    dest_cell: str
    lock_level: int
    key: str
    trap: str


@dataclass
class ZLight:
    pos: Vec3
    radius: float
    color: int
    flags: int


@dataclass
class Zone:
    entry: str
    name: str
    kind: int
    flags: int
    cell_range: Tuple[int, int, int, int]
    water_height: float
    fog_color: Tuple[float, float, float]
    fog_near: float
    fog_far: float
    ambient: Tuple[float, float, float]
    sun_color: Tuple[float, float, float]
    sun_dir: Vec3
    cells: List[ZCell] = field(default_factory=list)
    meshes: List[str] = field(default_factory=list)
    textures: List[str] = field(default_factory=list)
    terrain: List[ZTerrain] = field(default_factory=list)
    anims: List[str] = field(default_factory=list)
    actors: List[ZActor] = field(default_factory=list)


def _rgb(c: Sequence[float]) -> int:
    return dmesh.pack_color((c[0], c[1], c[2], 1.0))


def write_zone(z: Zone) -> bytes:
    strings = StringTable()
    name_ofs = strings.add(z.name)
    w = BinWriter()
    w.reserve(SIZES["DZoneHeader"])

    # flatten instances / batches in cell order; doors and lights are owned by
    # instances and get their final indices here
    instances: List[ZInstance] = []
    batches: List[dmesh.OutBatch] = []
    cell_recs = []
    for slot, c in enumerate(z.cells):
        first_b, first_i = len(batches), len(instances)
        for b in c.batches:
            b.node = slot
            batches.append(b)
        for ins in c.instances:
            ins.cell_slot = slot
            instances.append(ins)
        cell_recs.append((c.x, c.y, c.flags, c.terrain, fnv1a32(c.name), strings.add(c.name),
                          fnv1a32(c.region) if c.region else 0, first_b, len(c.batches), first_i,
                          len(c.instances), c.water, 0, 0))

    cells_ofs = w.align(16)
    for slot, rec in enumerate(cell_recs):
        w.pack(CELL_FMT[1:], *rec)
    meshes_ofs = w.align(16)
    for m in z.meshes:
        w.pack("I", fnv1a32(m))
    textures_ofs = w.align(16)
    for t in z.textures:
        w.pack("II", fnv1a32(t), strings.add(t))
    terrain_ofs = w.align(16)
    terrain_recs = w.reserve(SIZES["DZoneTerrain"] * len(z.terrain))
    doors = [(i, ins.door) for i, ins in enumerate(instances) if ins.door is not None]
    lights = [(i, ins.light) for i, ins in enumerate(instances) if ins.light is not None]
    door_index = {i: k for k, (i, _d) in enumerate(doors)}
    instances_ofs = w.align(16)
    for i, ins in enumerate(instances):
        w.pack(INSTANCE_FMT[1:], ins.mesh, ins.kind, ins.flags, ins.cell_slot,
               door_index.get(i, NONE16), fnv1a32(ins.object_id), ins.refnum, *ins.pos,
               *ins.rot, ins.scale, *ins.bound, 0)
    doors_ofs = w.align(16)
    for i, d in doors:
        w.pack(DOOR_FMT[1:], i, d.dest_zone, *d.dest_pos, *d.dest_rot,
               fnv1a32(d.dest_cell) if d.dest_cell else 0, d.lock_level,
               fnv1a32(d.key) if d.key else 0, fnv1a32(d.trap) if d.trap else 0)
    lights_ofs = w.align(16)
    for i, l in lights:
        w.pack(LIGHT_FMT[1:], *l.pos, l.radius, l.color, l.flags, i, 0)
    inst_index = {id(ins): i for i, ins in enumerate(instances)}
    anims_ofs = w.align(16)
    for a in z.anims:
        w.pack("I", fnv1a32(a))
    actors_ofs = w.align(16)
    actor_recs = w.reserve(SIZES["DZoneActor"] * len(z.actors))
    for k, act in enumerate(z.actors):
        if len(act.parts) > ACTOR_MAX_PARTS:
            raise ValueError("too many actor parts")
        bones_ofs = w.align(4)
        for _m, bone, _mir in act.parts:
            w.pack("I", bone)
        meshes = [m for m, _b, _mir in act.parts] + [NONE16] * (ACTOR_MAX_PARTS - len(act.parts))
        mirror = sum(1 << i for i, (_m, _b, mir) in enumerate(act.parts) if mir)
        w.patch(actor_recs + k * SIZES["DZoneActor"], ACTOR_FMT[1:], inst_index[id(act.instance)],
                act.skeleton, act.anim, act.flags, len(act.parts), *meshes, bones_ofs, mirror,
                0, 0, 0)
    batches_ofs = w.align(16)
    batch_recs = w.reserve(dmesh.SIZES["batch"] * len(batches))

    batch_id = {id(b): i for i, b in enumerate(batches)}
    for i, t in enumerate(z.terrain):
        hofs = w.align(16)
        w.pack(f"{GRID * GRID}f", *t.heights)
        cofs = w.align(16)
        colors = t.colors or [0xFFFFFFFF] * (GRID * GRID)
        w.pack(f"{GRID * GRID}I", *colors)
        qofs = w.align(16)
        quads = t.quad_tex or [NONE16] * ((GRID - 1) * (GRID - 1))
        w.pack(f"{len(quads)}H", *quads)
        ids = sorted(batch_id[id(b)] for b in t.batches)
        if ids and ids != list(range(ids[0], ids[0] + len(ids))):
            raise ValueError("terrain batches must be contiguous")
        w.patch(terrain_recs + i * SIZES["DZoneTerrain"], TERRAIN_FMT[1:], t.x, t.y, t.flags, 0,
                *t.origin, min(t.heights), max(t.heights), hofs, cofs, qofs,
                ids[0] if ids else 0, len(ids), 0)
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    for i, b in enumerate(batches):
        rec = dmesh.emit_batch(w, b)
        w.buf[batch_recs + i * 88:batch_recs + (i + 1) * 88] = rec
        for p in b.positions:
            for k in range(3):
                lo[k] = min(lo[k], p[k])
                hi[k] = max(hi[k], p[k])
    for ins in instances:
        for k in range(3):
            lo[k] = min(lo[k], ins.bound[k] - ins.bound[3])
            hi[k] = max(hi[k], ins.bound[k] + ins.bound[3])
    if lo[0] == float("inf"):
        lo, hi = [0.0] * 3, [0.0] * 3

    strings_ofs = w.align(16)
    w.bytes(bytes(strings.data))
    w.align(16)
    x0, y0, x1, y1 = z.cell_range
    w.patch(0, HEADER_FMT[1:], DZONE_MAGIC, DZONE_VERSION, z.kind, z.flags, len(w),
            fnv1a32(z.entry), name_ofs,
            x0, y0, x1, y1, z.water_height, _rgb(z.fog_color), z.fog_near, z.fog_far,
            _rgb(z.ambient), _rgb(z.sun_color), *z.sun_dir, len(z.cells), len(z.meshes),
            len(z.textures), len(z.terrain), len(batches), len(instances), len(doors),
            len(lights), cells_ofs, meshes_ofs, textures_ofs, terrain_ofs, batches_ofs,
            instances_ofs, doors_ofs, lights_ofs, strings_ofs, *lo, *hi, len(z.actors),
            len(z.anims), actors_ofs, anims_ofs)
    return w.getvalue()


def _cstr(data: bytes, ofs: int) -> str:
    return data[ofs:data.index(b"\0", ofs)].decode("latin-1")


def read_zone(data: bytes) -> dict:
    h = struct.unpack_from(HEADER_FMT, data, 0)
    (magic, version, kind, flags, file_size, name_hash, name_ofs, x0, y0, x1, y1, water,
     fog, fog_near, fog_far, ambient, sun) = h[:17]
    sun_dir = h[17:20]
    (ncells, nmeshes, ntex, nterrain, nbatches, ninst, ndoors, nlights, cells_ofs, meshes_ofs,
     tex_ofs, terrain_ofs, batches_ofs, inst_ofs, doors_ofs, lights_ofs, str_ofs) = h[20:37]
    if magic != DZONE_MAGIC or version != DZONE_VERSION:
        raise ValueError("not a DZONE v1")
    if file_size != len(data):
        raise ValueError("DZONE size mismatch")
    out = {"kind": kind, "flags": flags, "name_hash": name_hash,
           "name": _cstr(data, str_ofs + name_ofs), "cell_range": (x0, y0, x1, y1),
           "water_height": water, "fog_color": dmesh.unpack_color(fog), "fog_near": fog_near,
           "fog_far": fog_far, "ambient": dmesh.unpack_color(ambient),
           "sun_color": dmesh.unpack_color(sun), "sun_dir": sun_dir,
           "bbox_min": h[37:40], "bbox_max": h[40:43]}
    nactors, nanims, actors_ofs, anims_ofs = h[43:47]
    out["cells"] = []
    for i in range(ncells):
        r = struct.unpack_from(CELL_FMT, data, cells_ofs + i * 48)
        out["cells"].append({"x": r[0], "y": r[1], "flags": r[2], "terrain": r[3],
                             "name": _cstr(data, str_ofs + r[5]), "region_hash": r[6],
                             "first_batch": r[7], "batch_count": r[8], "first_instance": r[9],
                             "instance_count": r[10], "water": r[11]})
    out["meshes"] = list(struct.unpack_from(f"<{nmeshes}I", data, meshes_ofs)) if nmeshes else []
    out["textures"] = []
    for i in range(ntex):
        hsh, nofs = struct.unpack_from("<II", data, tex_ofs + 8 * i)
        out["textures"].append({"hash": hsh, "name": _cstr(data, str_ofs + nofs)})
    out["terrain"] = []
    for i in range(nterrain):
        r = struct.unpack_from(TERRAIN_FMT, data, terrain_ofs + 48 * i)
        heights = struct.unpack_from(f"<{GRID * GRID}f", data, r[8])
        colors = struct.unpack_from(f"<{GRID * GRID}I", data, r[9])
        quads = struct.unpack_from(f"<{(GRID - 1) ** 2}H", data, r[10])
        out["terrain"].append({"x": r[0], "y": r[1], "flags": r[2], "origin": r[4:6],
                               "min_h": r[6], "max_h": r[7], "heights": heights,
                               "colors": colors, "quad_tex": quads, "first_batch": r[11],
                               "batch_count": r[12]})
    out["batches"] = [dmesh.parse_batch(data, batches_ofs + 88 * i) for i in range(nbatches)]
    out["instances"] = []
    for i in range(ninst):
        r = struct.unpack_from(INSTANCE_FMT, data, inst_ofs + 64 * i)
        out["instances"].append({"mesh": r[0], "kind": r[1], "flags": r[2], "cell": r[3],
                                 "door": r[4], "object_hash": r[5], "refnum": r[6],
                                 "pos": r[7:10], "rot": r[10:13], "scale": r[13],
                                 "bound": r[14:18]})
    out["doors"] = []
    for i in range(ndoors):
        r = struct.unpack_from(DOOR_FMT, data, doors_ofs + 48 * i)
        out["doors"].append({"instance": r[0], "dest_zone": r[1], "dest_pos": r[2:5],
                             "dest_rot": r[5:8], "dest_cell_hash": r[8], "lock_level": r[9],
                             "key_hash": r[10], "trap_hash": r[11]})
    out["anims"] = list(struct.unpack_from(f"<{nanims}I", data, anims_ofs)) if nanims else []
    out["actors"] = []
    for i in range(nactors):
        r = struct.unpack_from(ACTOR_FMT, data, actors_ofs + 64 * i)
        count = r[4]
        bones = struct.unpack_from(f"<{count}I", data, r[21]) if count else ()
        out["actors"].append({"instance": r[0], "skeleton": r[1], "anim": r[2], "flags": r[3],
                              "parts": [(r[5 + k], bones[k], bool(r[22] >> k & 1))
                                        for k in range(count)]})
    out["lights"] = []
    for i in range(nlights):
        r = struct.unpack_from(LIGHT_FMT, data, lights_ofs + 32 * i)
        out["lights"].append({"pos": r[0:3], "radius": r[3], "color": dmesh.unpack_color(r[4]),
                              "flags": r[5], "instance": r[6]})
    return out


@dataclass
class WorldZoneEntry:
    entry: str
    name: str
    kind: int
    flags: int
    cell_hash: int
    cell_count: int
    cell_range: Tuple[int, int, int, int]
    bytes: int = 0


def write_world(zone_size: int, zones: Sequence[WorldZoneEntry],
                ext: Dict[Tuple[int, int], int], interiors: Dict[str, int]) -> bytes:
    strings = StringTable()
    w = BinWriter()
    w.reserve(SIZES["DWorldHeader"])
    zones_ofs = w.align(16)
    for z in zones:
        w.pack(WORLD_ZONE_FMT[1:], fnv1a32(z.entry), z.cell_hash, strings.add(z.name), z.kind,
               z.flags, z.cell_count, *z.cell_range, z.bytes, 0)
    ext_ofs = w.align(16)
    for (x, y) in sorted(ext):
        w.pack(WORLD_EXT_FMT[1:], x, y, ext[(x, y)])
    int_ofs = w.align(16)
    int_sorted = sorted(((fnv1a32(name), zi, name) for name, zi in interiors.items()))
    seen = {}
    for hsh, zi, name in int_sorted:
        if hsh in seen:
            raise ValueError(f"interior hash collision: {name!r} / {seen[hsh]!r}")
        seen[hsh] = name
        w.pack(WORLD_INT_FMT[1:], hsh, zi)
    strings_ofs = w.align(16)
    w.bytes(bytes(strings.data))
    w.align(16)
    w.patch(0, WORLD_HEADER_FMT[1:], DWORLD_MAGIC, DWORLD_VERSION, zone_size, len(zones),
            len(ext), len(int_sorted), zones_ofs, ext_ofs, int_ofs, strings_ofs, len(w), 0, 0)
    return w.getvalue()


def read_world(data: bytes) -> dict:
    (magic, version, zone_size, nz, next_, nint, zones_ofs, ext_ofs, int_ofs, str_ofs,
     size, _r0, _r1) = struct.unpack_from(WORLD_HEADER_FMT, data, 0)
    if magic != DWORLD_MAGIC or version != DWORLD_VERSION or size != len(data):
        raise ValueError("not a DWORLD v1")
    zones = []
    for i in range(nz):
        r = struct.unpack_from(WORLD_ZONE_FMT, data, zones_ofs + 32 * i)
        zones.append({"entry_hash": r[0], "cell_hash": r[1], "name": _cstr(data, str_ofs + r[2]),
                      "kind": r[3], "flags": r[4], "cell_count": r[5], "range": r[6:10],
                      "bytes": r[10]})
    ext = [struct.unpack_from(WORLD_EXT_FMT, data, ext_ofs + 8 * i) for i in range(next_)]
    ints = [struct.unpack_from(WORLD_INT_FMT, data, int_ofs + 8 * i) for i in range(nint)]
    return {"zone_size": zone_size, "zones": zones, "ext": ext, "int": ints}
