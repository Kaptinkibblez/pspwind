"""DMESH writer / reader - mirrors the structs in src/fmt.h byte for byte."""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from typing import List, Optional, Sequence, Tuple

from dmcommon import BinWriter, StringTable, fnv1a32

DMESH_MAGIC = b"DMSH"
DMESH_VERSION = 1
DMESH_NO_TEXTURE = 0xFFFF
DMESH_NO_PARENT = -1

DMESH_F_SKELETAL = 1 << 0
DMESH_F_COLLISION = 1 << 1
DMESH_F_ANIMATED = 1 << 2
DMESH_F_PARTICLES = 1 << 3

NODE_F_BONE = 1 << 0
NODE_F_BILLBOARD = 1 << 1
NODE_F_ANIMATED = 1 << 2
NODE_F_HIDDEN = 1 << 3
NODE_F_ATTACH = 1 << 4
NODE_F_EMITTER = 1 << 5

BATCH_F_ALPHA_BLEND = 1 << 0
BATCH_F_ALPHA_TEST = 1 << 1
BATCH_F_DOUBLE_SIDED = 1 << 2
BATCH_F_VERTEX_ALPHA = 1 << 3
BATCH_F_EMISSIVE = 1 << 4
BATCH_F_ADDITIVE = 1 << 5
BATCH_F_CLAMP_U = 1 << 6
BATCH_F_CLAMP_V = 1 << 7
BATCH_F_SEGMENT = 1 << 8

COLL_F_INDEX32 = 1 << 0

HEADER_FMT = "<4sHHIHHHHIIIIIIII4f3f3fI"
NODE_FMT = "<hHIIf4f3fI"
BATCH_FMT = "<HHHBBIIII3f3f2f2fII4f"
TEXTURE_FMT = "<II"
EMITTER_FMT = "<HHHHI10f3f2f"
COLL_FMT = "<IIIIHH3f3fI"
VERTEX_FMT = "<HHIbbbbhhhh"

SIZES = {
    "header": struct.calcsize(HEADER_FMT),
    "node": struct.calcsize(NODE_FMT),
    "batch": struct.calcsize(BATCH_FMT),
    "texture": struct.calcsize(TEXTURE_FMT),
    "emitter": struct.calcsize(EMITTER_FMT),
    "collision": struct.calcsize(COLL_FMT),
    "vertex": struct.calcsize(VERTEX_FMT),
}
assert SIZES == {"header": 96, "node": 48, "batch": 88, "texture": 8, "emitter": 72,
                 "collision": 48, "vertex": 20}, SIZES

Vec3 = Tuple[float, float, float]


@dataclass
class OutNode:
    name: str
    parent: int
    flags: int = 0
    translation: Vec3 = (0.0, 0.0, 0.0)
    rotation: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)  # x y z w
    scale: float = 1.0


@dataclass
class OutBatch:
    node: int
    texture: int  # index into OutMesh.textures or DMESH_NO_TEXTURE
    flags: int
    alpha_ref: int
    blend: int
    diffuse: Tuple[float, float, float, float]
    emissive: Vec3
    positions: List[Vec3]
    normals: List[Vec3]
    uvs: List[Tuple[float, float]]
    colors: List[Tuple[float, float, float, float]]
    indices: List[int]


@dataclass
class OutEmitter:
    node: int
    texture: int
    max_particles: int
    flags: int
    color: Tuple[float, float, float, float]
    size: float
    emit_rate: float
    lifetime: float
    lifetime_random: float
    speed: float
    speed_random: float
    vertical_dir: float
    vertical_angle: float
    horizontal_dir: float
    horizontal_angle: float
    gravity: Vec3
    grow_time: float
    fade_time: float


@dataclass
class OutMesh:
    flags: int = 0
    nodes: List[OutNode] = field(default_factory=list)
    batches: List[OutBatch] = field(default_factory=list)
    textures: List[str] = field(default_factory=list)
    emitters: List[OutEmitter] = field(default_factory=list)
    collision_positions: Optional[List[Vec3]] = None
    collision_triangles: Optional[List[Tuple[int, int, int]]] = None
    bound_center: Vec3 = (0.0, 0.0, 0.0)
    bound_radius: float = 0.0
    bbox_min: Vec3 = (0.0, 0.0, 0.0)
    bbox_max: Vec3 = (0.0, 0.0, 0.0)


def pack_color(c: Sequence[float]) -> int:
    r, g, b = (max(0, min(255, int(round(x * 255.0)))) for x in c[:3])
    a = max(0, min(255, int(round((c[3] if len(c) > 3 else 1.0) * 255.0))))
    return r | (g << 8) | (b << 16) | (a << 24)


def unpack_color(v: int) -> Tuple[float, float, float, float]:
    return ((v & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0, ((v >> 16) & 0xFF) / 255.0,
            ((v >> 24) & 0xFF) / 255.0)


def quant_params(values: Sequence[Vec3]) -> Tuple[Vec3, Vec3]:
    """Offset/scale so that (p - off) / scale * 32768 fits int16."""
    if not values:
        return (0.0, 0.0, 0.0), (1.0, 1.0, 1.0)
    lo = [min(v[i] for v in values) for i in range(3)]
    hi = [max(v[i] for v in values) for i in range(3)]
    off = tuple((lo[i] + hi[i]) * 0.5 for i in range(3))
    scale = tuple(max((hi[i] - lo[i]) * 0.5, 1e-4) * (32768.0 / 32767.0) for i in range(3))
    return off, scale  # type: ignore[return-value]


def uv_params(uvs: Sequence[Tuple[float, float]]) -> Tuple[Tuple[float, float], Tuple[float, float]]:
    if not uvs:
        return (0.0, 0.0), (1.0, 1.0)
    lo = [min(u[i] for u in uvs) for i in range(2)]
    hi = [max(u[i] for u in uvs) for i in range(2)]
    return (lo[0], lo[1]), (max(hi[0] - lo[0], 1e-6), max(hi[1] - lo[1], 1e-6))


def _q16s(v: float) -> int:
    return max(-32768, min(32767, int(round(v * 32768.0))))


def _q16u(v: float) -> int:
    return max(0, min(32768, int(round(v * 32768.0))))


def _q8n(v: float) -> int:
    return max(-127, min(127, int(round(v * 127.0))))


def bound_sphere(points: Sequence[Vec3]) -> Tuple[Vec3, float]:
    if not points:
        return (0.0, 0.0, 0.0), 0.0
    lo = [min(p[i] for p in points) for i in range(3)]
    hi = [max(p[i] for p in points) for i in range(3)]
    c = ((lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5)
    r = max(math.sqrt((p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 + (p[2] - c[2]) ** 2)
            for p in points)
    return c, r


def _batch_order(b: OutBatch) -> int:
    if b.flags & BATCH_F_ALPHA_BLEND:
        return 2
    if b.flags & BATCH_F_ALPHA_TEST:
        return 1
    return 0


def write_dmesh(mesh: OutMesh) -> bytes:
    if len(mesh.nodes) > 0x7FFF:
        raise ValueError("too many nodes")
    for i, n in enumerate(mesh.nodes):
        if n.parent >= i:
            raise ValueError("nodes must be ordered parents first")
    batches = sorted(mesh.batches, key=_batch_order)
    strings = StringTable()
    w = BinWriter()
    w.reserve(SIZES["header"])

    nodes_ofs = w.align(16)
    for n in mesh.nodes:
        w.pack(NODE_FMT[1:], n.parent, n.flags, fnv1a32(n.name), strings.add(n.name),
               n.scale, *n.rotation, *n.translation, 0)
    batches_ofs = w.align(16)
    batch_recs = w.reserve(SIZES["batch"] * len(batches))
    textures_ofs = w.align(16)
    for t in mesh.textures:
        w.pack(TEXTURE_FMT[1:], fnv1a32(t), strings.add(t))
    emitters_ofs = w.align(16)
    for e in mesh.emitters:
        w.pack(EMITTER_FMT[1:], e.node, e.texture, e.max_particles, e.flags,
               pack_color(e.color), e.size, e.emit_rate, e.lifetime, e.lifetime_random,
               e.speed, e.speed_random, e.vertical_dir, e.vertical_angle, e.horizontal_dir,
               e.horizontal_angle, *e.gravity, e.grow_time, e.fade_time)
    coll_rec = 0
    flags = mesh.flags
    if mesh.collision_positions and mesh.collision_triangles:
        coll_rec = w.align(16)
        w.reserve(SIZES["collision"])
        flags |= DMESH_F_COLLISION
    else:
        flags &= ~DMESH_F_COLLISION

    total_v = total_t = 0
    for i, b in enumerate(batches):
        nv = len(b.positions)
        if nv > 65535:
            raise ValueError("batch exceeds 65535 vertices (split before writing)")
        if len(b.indices) % 3:
            raise ValueError("index count not a multiple of 3")
        off, scale = quant_params(b.positions)
        uv_off, uv_scale = uv_params(b.uvs)
        vofs = w.align(16)
        for k in range(nv):
            p = b.positions[k]
            n = b.normals[k] if b.normals else (0.0, 0.0, 1.0)
            uv = b.uvs[k] if b.uvs else (0.0, 0.0)
            col = b.colors[k] if b.colors else (1.0, 1.0, 1.0, 1.0)
            w.pack(VERTEX_FMT[1:],
                   _q16u((uv[0] - uv_off[0]) / uv_scale[0]),
                   _q16u((uv[1] - uv_off[1]) / uv_scale[1]),
                   pack_color(col), _q8n(n[0]), _q8n(n[1]), _q8n(n[2]), 0,
                   _q16s((p[0] - off[0]) / scale[0]), _q16s((p[1] - off[1]) / scale[1]),
                   _q16s((p[2] - off[2]) / scale[2]), 0)
        iofs = w.align(16)
        if b.indices:
            w.bytes(struct.pack(f"<{len(b.indices)}H", *b.indices))
        c, r = bound_sphere(b.positions)
        w.patch(batch_recs + i * SIZES["batch"], BATCH_FMT[1:], b.node, b.texture, b.flags,
                b.alpha_ref, b.blend, nv, len(b.indices), vofs, iofs, *off, *scale,
                *uv_off, *uv_scale, pack_color(b.diffuse), pack_color((*b.emissive, 1.0)),
                *c, r)
        total_v += nv
        total_t += len(b.indices) // 3

    if coll_rec:
        cp = mesh.collision_positions or []
        ct = mesh.collision_triangles or []
        cflags = COLL_F_INDEX32 if len(cp) > 65535 else 0
        vofs = w.align(16)
        for p in cp:
            w.pack("3f", *p)
        iofs = w.align(16)
        flat = [i for t in ct for i in t]
        if flat:
            w.bytes(struct.pack(f"<{len(flat)}{'I' if cflags else 'H'}", *flat))
        lo = tuple(min(p[i] for p in cp) for i in range(3))
        hi = tuple(max(p[i] for p in cp) for i in range(3))
        w.patch(coll_rec, COLL_FMT[1:], len(cp), len(ct), vofs, iofs, cflags, 0, *lo, *hi, 0)

    strings_ofs = w.align(16)
    w.bytes(bytes(strings.data))
    w.align(16)
    w.patch(0, HEADER_FMT[1:], DMESH_MAGIC, DMESH_VERSION, flags, len(w), len(mesh.nodes),
            len(batches), len(mesh.textures), len(mesh.emitters), nodes_ofs, batches_ofs,
            textures_ofs, emitters_ofs, coll_rec, strings_ofs, total_v, total_t,
            *mesh.bound_center, mesh.bound_radius, *mesh.bbox_min, *mesh.bbox_max, 0)
    return w.getvalue()


def _cstr(data: bytes, ofs: int) -> str:
    return data[ofs:data.index(b"\0", ofs)].decode("latin-1")


def read_dmesh(data: bytes) -> dict:
    """Parse a DMESH and dequantise vertices (tests / inspection)."""
    h = struct.unpack_from(HEADER_FMT, data, 0)
    (magic, version, flags, file_size, n_nodes, n_batches, n_tex, n_emit, nodes_ofs,
     batches_ofs, tex_ofs, emit_ofs, coll_ofs, str_ofs, total_v, total_t) = h[:16]
    if magic != DMESH_MAGIC or version != DMESH_VERSION:
        raise ValueError("not a DMESH v1")
    if file_size != len(data):
        raise ValueError("DMESH size mismatch")
    out = {"flags": flags, "total_vertices": total_v, "total_triangles": total_t,
           "bound_center": h[16:19], "bound_radius": h[19], "bbox_min": h[20:23],
           "bbox_max": h[23:26], "nodes": [], "batches": [], "textures": [], "emitters": []}
    for i in range(n_nodes):
        r = struct.unpack_from(NODE_FMT, data, nodes_ofs + i * SIZES["node"])
        out["nodes"].append({"parent": r[0], "flags": r[1], "hash": r[2],
                             "name": _cstr(data, str_ofs + r[3]), "scale": r[4],
                             "rotation": r[5:9], "translation": r[9:12]})
    for i in range(n_tex):
        hsh, nofs = struct.unpack_from(TEXTURE_FMT, data, tex_ofs + i * 8)
        out["textures"].append({"hash": hsh, "name": _cstr(data, str_ofs + nofs)})
    for i in range(n_emit):
        r = struct.unpack_from(EMITTER_FMT, data, emit_ofs + i * SIZES["emitter"])
        out["emitters"].append({"node": r[0], "texture": r[1], "max_particles": r[2],
                                "flags": r[3], "color": unpack_color(r[4]), "size": r[5],
                                "emit_rate": r[6], "lifetime": r[7], "gravity": r[15:18],
                                "grow_time": r[18], "fade_time": r[19]})
    for i in range(n_batches):
        r = struct.unpack_from(BATCH_FMT, data, batches_ofs + i * SIZES["batch"])
        node, tex, bflags, aref, blend, nv, ni, vofs, iofs = r[:9]
        off, scale, uv_off, uv_scale = r[9:12], r[12:15], r[15:17], r[17:19]
        if vofs % 16 or iofs % 16:
            raise ValueError("batch data misaligned")
        verts = []
        for k in range(nv):
            u, v, col, nx, ny, nz, _p0, x, y, z, _p1 = struct.unpack_from(
                VERTEX_FMT, data, vofs + k * SIZES["vertex"])
            verts.append({
                "pos": tuple(q / 32768.0 * scale[j] + off[j] for j, q in enumerate((x, y, z))),
                "uv": (u / 32768.0 * uv_scale[0] + uv_off[0], v / 32768.0 * uv_scale[1] + uv_off[1]),
                "normal": (nx / 127.0, ny / 127.0, nz / 127.0),
                "color": unpack_color(col)})
        idx = list(struct.unpack_from(f"<{ni}H", data, iofs)) if ni else []
        out["batches"].append({"node": node, "texture": tex, "flags": bflags, "alpha_ref": aref,
                               "blend": blend, "vertices": verts, "indices": idx,
                               "diffuse": unpack_color(r[19]), "emissive": unpack_color(r[20]),
                               "bound": r[21:25]})
    out["collision"] = None
    if coll_ofs:
        nv, nt, vofs, iofs, cflags, _r0 = struct.unpack_from(COLL_FMT, data, coll_ofs)[:6]
        pts = [struct.unpack_from("<3f", data, vofs + 12 * k) for k in range(nv)]
        fmt = "I" if cflags & COLL_F_INDEX32 else "H"
        flat = struct.unpack_from(f"<{nt * 3}{fmt}", data, iofs) if nt else ()
        out["collision"] = {"positions": pts,
                            "triangles": [flat[k:k + 3] for k in range(0, len(flat), 3)]}
    return out
