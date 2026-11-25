#!/usr/bin/env python3
"""mesh_demaker - Morrowind NIF models -> PS1/PSP style .dmesh

Per model:
  1. parse the NetImmerse 4.0.0.2 scene graph (nif.py)
  2. strip the PC look: multi-texture stages (dark/detail/gloss/glow/bump/
     decal), specular, dithering and z-buffer tricks are dropped; only the
     base texture, vertex colour, material diffuse/emissive and alpha mode
     survive, baked straight into the vertex colours
  3. flatten the node hierarchy: static meshes collapse into the root node;
     animated, billboard, attach-point and emitter nodes are kept with their
     ancestors so runtime animation still works
  4. skinned geometry becomes a segmented rigid-limb character: every
     triangle is assigned to its dominant bone and stored in that bone's
     local space (zero per-vertex skinning at runtime)
  5. merge geometry per (node, material) into draw batches, weld vertices and
     decimate with quadric edge collapse to the triangle budget
  6. quantise to the 20-byte GE vertex (16-bit UV / 8888 colour / 8-bit
     normal / 16-bit position) as indexed triangle lists
  7. export collision (RootCollisionNode, else render geometry) and
     particle emitters

Usage:
  mesh_demaker.py --data "/path/Morrowind/Data Files" --out build/cooked
"""

from __future__ import annotations

import argparse
import io
import json
import math
import os
import sys
import time
from dataclasses import asdict, dataclass, field
from typing import Dict, List, Optional, Sequence, Set, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import decimate  # noqa: E402
import dmcommon  # noqa: E402
import dmesh  # noqa: E402
import nif as niflib  # noqa: E402
from dmcommon import LOG, DataConfig, DataSource  # noqa: E402

Vec3 = Tuple[float, float, float]
Mat = List[List[float]]  # 4x4 row-major, column-vector convention: p' = M p

TRANSFORM_CONTROLLERS = frozenset(("NiKeyframeController", "NiPathController",
                                   "NiLookAtController", "NiRollController", "NiVisController"))
PARTICLE_CONTROLLERS = frozenset(("NiParticleSystemController", "NiBSPArrayController"))


@dataclass
class MeshConfig:
    ratio: float = 0.5            # keep this fraction of triangles
    min_tris: int = 24            # batches smaller than this are not decimated
    error_tol: float = 0.02       # max collapse error as a fraction of model size
    collision_ratio: float = 0.5
    weld_eps: float = 1e-3        # position weld grid (game units)
    keep_hidden: bool = False


# --------------------------------------------------------------------------
# matrix helpers
# --------------------------------------------------------------------------

IDENTITY: Mat = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0],
                 [0.0, 0.0, 0.0, 1.0]]


def m_from_nif(t: Sequence[float], r: Sequence[float], s: float) -> Mat:
    """NiTransform: p' = R * (s * p) + t, R stored row-major."""
    return [[r[0] * s, r[1] * s, r[2] * s, t[0]],
            [r[3] * s, r[4] * s, r[5] * s, t[1]],
            [r[6] * s, r[7] * s, r[8] * s, t[2]],
            [0.0, 0.0, 0.0, 1.0]]


def m_mul(a: Mat, b: Mat) -> Mat:
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def m_inv(m: Mat) -> Mat:
    """Inverse of an affine matrix (general 3x3 part)."""
    a, b, c = m[0][0], m[0][1], m[0][2]
    d, e, f = m[1][0], m[1][1], m[1][2]
    g, h, i = m[2][0], m[2][1], m[2][2]
    co = [[e * i - f * h, -(d * i - f * g), d * h - e * g],
          [-(b * i - c * h), a * i - c * g, -(a * h - b * g)],
          [b * f - c * e, -(a * f - c * d), a * e - b * d]]
    det = a * co[0][0] + b * co[0][1] + c * co[0][2]
    if abs(det) < 1e-20:
        return [row[:] for row in IDENTITY]
    inv3 = [[co[j][i] / det for j in range(3)] for i in range(3)]
    t = [m[0][3], m[1][3], m[2][3]]
    it = [-(inv3[r][0] * t[0] + inv3[r][1] * t[1] + inv3[r][2] * t[2]) for r in range(3)]
    return [[inv3[0][0], inv3[0][1], inv3[0][2], it[0]],
            [inv3[1][0], inv3[1][1], inv3[1][2], it[1]],
            [inv3[2][0], inv3[2][1], inv3[2][2], it[2]],
            [0.0, 0.0, 0.0, 1.0]]


def m_point(m: Mat, p: Sequence[float]) -> Vec3:
    return (m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3],
            m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3],
            m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3])


def normal_matrix(m: Mat) -> Mat:
    """Inverse-transpose of the 3x3 part (handles non-uniform scale)."""
    inv = m_inv(m)
    return [[inv[j][i] for j in range(3)] + [0.0] for i in range(3)] + [[0.0, 0.0, 0.0, 1.0]]


def m_normal(nm: Mat, n: Sequence[float]) -> Vec3:
    x = nm[0][0] * n[0] + nm[0][1] * n[1] + nm[0][2] * n[2]
    y = nm[1][0] * n[0] + nm[1][1] * n[1] + nm[1][2] * n[2]
    z = nm[2][0] * n[0] + nm[2][1] * n[1] + nm[2][2] * n[2]
    ln = math.sqrt(x * x + y * y + z * z)
    if ln < 1e-12:
        return (0.0, 0.0, 1.0)
    return (x / ln, y / ln, z / ln)


def decompose(m: Mat) -> Tuple[Vec3, Tuple[float, float, float, float], float]:
    """-> translation, quaternion (x, y, z, w), uniform scale."""
    cols = [(m[0][j], m[1][j], m[2][j]) for j in range(3)]
    lens = [math.sqrt(c[0] ** 2 + c[1] ** 2 + c[2] ** 2) for c in cols]
    s = sum(lens) / 3.0
    if s < 1e-12:
        return (m[0][3], m[1][3], m[2][3]), (0.0, 0.0, 0.0, 1.0), 0.0
    r = [[m[i][j] / s for j in range(3)] for i in range(3)]
    tr = r[0][0] + r[1][1] + r[2][2]
    if tr > 0.0:
        k = math.sqrt(tr + 1.0) * 2.0
        w = 0.25 * k
        x = (r[2][1] - r[1][2]) / k
        y = (r[0][2] - r[2][0]) / k
        z = (r[1][0] - r[0][1]) / k
    elif r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        k = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0
        w = (r[2][1] - r[1][2]) / k
        x = 0.25 * k
        y = (r[0][1] + r[1][0]) / k
        z = (r[0][2] + r[2][0]) / k
    elif r[1][1] > r[2][2]:
        k = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0
        w = (r[0][2] - r[2][0]) / k
        x = (r[0][1] + r[1][0]) / k
        y = 0.25 * k
        z = (r[1][2] + r[2][1]) / k
    else:
        k = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0
        w = (r[1][0] - r[0][1]) / k
        x = (r[0][2] + r[2][0]) / k
        y = (r[1][2] + r[2][1]) / k
        z = 0.25 * k
    ln = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    q = (x / ln, y / ln, z / ln, w / ln)
    if q[3] < 0.0:
        q = (-q[0], -q[1], -q[2], -q[3])
    return (m[0][3], m[1][3], m[2][3]), q, s


def compose(t: Sequence[float], q: Sequence[float], s: float) -> Mat:
    x, y, z, w = q
    return [[(1 - 2 * (y * y + z * z)) * s, 2 * (x * y - w * z) * s, 2 * (x * z + w * y) * s, t[0]],
            [2 * (x * y + w * z) * s, (1 - 2 * (x * x + z * z)) * s, 2 * (y * z - w * x) * s, t[1]],
            [2 * (x * z - w * y) * s, 2 * (y * z + w * x) * s, (1 - 2 * (x * x + y * y)) * s, t[2]],
            [0.0, 0.0, 0.0, 1.0]]


# --------------------------------------------------------------------------
# materials
# --------------------------------------------------------------------------


@dataclass
class Material:
    texture: Optional[str] = None
    uv_set: int = 0
    flags: int = 0
    alpha_ref: int = 0
    blend: int = 0
    diffuse: Tuple[float, float, float] = (1.0, 1.0, 1.0)
    alpha: float = 1.0
    emissive: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    vertex_mode: int = 2  # NiVertexColorProperty default: ambient+diffuse

    def key(self) -> tuple:
        return (self.texture, self.flags & ~dmesh.BATCH_F_VERTEX_ALPHA, self.alpha_ref,
                self.blend, tuple(round(c, 2) for c in self.emissive))


# --------------------------------------------------------------------------
# intermediate geometry
# --------------------------------------------------------------------------


@dataclass
class Prim:
    """Triangle soup attached to an output node (positions in node space)."""
    node: int
    material: Material
    segment: bool
    positions: List[Vec3] = field(default_factory=list)
    normals: List[Vec3] = field(default_factory=list)
    uvs: List[Tuple[float, float]] = field(default_factory=list)
    colors: List[Tuple[float, float, float, float]] = field(default_factory=list)
    triangles: List[Tuple[int, int, int]] = field(default_factory=list)


@dataclass
class GeomInstance:
    block: niflib.Block
    world: Mat
    props: Dict[str, niflib.Block]
    in_collision: bool
    hidden: bool
    parent_chain: List[int]  # node block indices root..parent


@dataclass
class MeshResult:
    key: str
    ok: bool
    data: bytes = b""
    error: str = ""
    info: dict = field(default_factory=dict)
    textures: List[str] = field(default_factory=list)
    embedded: Dict[str, bytes] = field(default_factory=dict)  # texture key -> PNG


class MeshBuilder:
    def __init__(self, nif: niflib.NifFile, key: str, cfg: MeshConfig) -> None:
        self.nif = nif
        self.key = key
        self.cfg = cfg
        self.world: Dict[int, Mat] = {}
        self.parent: Dict[int, int] = {}
        self.node_blocks: List[niflib.Block] = []
        self.geoms: List[GeomInstance] = []
        self.particles: List[GeomInstance] = []
        self.collision_nodes = False
        self.no_collision = False
        self.marker_flag = False
        self.warnings: List[str] = []
        self.embedded: Dict[str, bytes] = {}

    # ---------------------------------------------------------------- walk
    def controllers(self, block: niflib.Block) -> List[niflib.Block]:
        out = []
        ref = getattr(block, "controller", -1)
        seen = set()
        while ref >= 0 and ref not in seen:
            seen.add(ref)
            c = self.nif.get(ref)
            if c is None:
                break
            out.append(c)
            ref = getattr(c, "next_controller", -1)
        return out

    def extra_strings(self, block: niflib.Block) -> List[str]:
        out = []
        ref = getattr(block, "extra", -1)
        seen = set()
        while ref >= 0 and ref not in seen:
            seen.add(ref)
            e = self.nif.get(ref)
            if e is None:
                break
            if e.type == "NiStringExtraData":
                out.append(e.string)
            ref = getattr(e, "next_extra", -1)
        return out

    def walk(self, block: niflib.Block, parent_world: Mat, props: Dict[str, niflib.Block],
             in_coll: bool, hidden: bool, chain: List[int], visited: Set[int]) -> None:
        if block.index in visited:
            self.warnings.append(f"cycle at block {block.index}")
            return
        visited.add(block.index)
        local = m_from_nif(block.translation, block.rotation, block.scale)
        world = m_mul(parent_world, local)
        self.world[block.index] = world
        if chain:
            self.parent[block.index] = chain[-1]
        props = dict(props)
        for ref in block.properties:
            p = self.nif.get(ref)
            if p is not None:
                props[p.type] = p
        hidden = hidden or bool(block.flags & 1)
        name = block.name.lower()
        if name.startswith("shadow") or name.startswith("tri shadow"):
            hidden = True
        if self.marker_flag and name.startswith("tri editormarker"):
            hidden = True
        if block.type in niflib.NODE_TYPES:
            self.node_blocks.append(block)
            if block.type == "RootCollisionNode":
                in_coll = True
                self.collision_nodes = True
            children = list(block.children)
            if block.type == "NiSwitchNode" and children:
                idx = block.switch_index if block.switch_index < len(children) else 0
                children = [children[idx]]
            elif block.type == "NiLODNode" and children:
                children = [children[0]]
            for ref in children:
                child = self.nif.get(ref)
                if child is None or not hasattr(child, "translation"):
                    continue
                self.walk(child, world, props, in_coll, hidden, chain + [block.index], visited)
        elif block.type in niflib.GEOMETRY_TYPES:
            self.geoms.append(GeomInstance(block, world, props, in_coll, hidden,
                                           chain + []))
        elif block.type in niflib.PARTICLE_TYPES:
            self.particles.append(GeomInstance(block, world, props, in_coll, hidden,
                                               chain + []))

    # ---------------------------------------------------------- materials
    def texture_key_for(self, src: niflib.Block) -> Optional[str]:
        if src.external:
            return dmcommon.texture_key(src.filename) if src.filename else None
        pix = self.nif.get(src.pixel_data)
        if pix is None:
            return None
        key = "embedded/" + self.key.replace("/", "_") + f"_{src.index}"
        if key not in self.embedded:
            png = decode_pixel_data(self.nif, pix)
            if png is None:
                self.warnings.append(f"unsupported embedded pixel format {pix.pixel_format}")
                return None
            self.embedded[key] = png
        return key

    def material(self, props: Dict[str, niflib.Block], has_vertex_alpha: bool) -> Material:
        m = Material()
        tex = props.get("NiTexturingProperty")
        if tex is not None and tex.textures and tex.textures[0] is not None:
            desc = tex.textures[0]
            src = self.nif.get(desc["source"])
            if src is not None and src.type == "NiSourceTexture":
                m.texture = self.texture_key_for(src)
                m.uv_set = desc["uv_set"]
                clamp = desc["clamp"]
                if clamp in (0, 1):
                    m.flags |= dmesh.BATCH_F_CLAMP_U
                if clamp in (0, 2):
                    m.flags |= dmesh.BATCH_F_CLAMP_V
        mat = props.get("NiMaterialProperty")
        if mat is not None:
            m.diffuse = tuple(max(0.0, min(1.0, c)) for c in mat.diffuse)  # type: ignore
            m.alpha = max(0.0, min(1.0, mat.alpha))
            m.emissive = tuple(max(0.0, min(1.0, c)) for c in mat.emissive)  # type: ignore
        alpha = props.get("NiAlphaProperty")
        if alpha is not None:
            f = alpha.flags
            if f & 1:
                m.flags |= dmesh.BATCH_F_ALPHA_BLEND
                src_mode = (f >> 1) & 0xF
                dst_mode = (f >> 5) & 0xF
                m.blend = src_mode | (dst_mode << 4)
                if dst_mode == 0:  # GL_ONE destination -> additive glow
                    m.flags |= dmesh.BATCH_F_ADDITIVE
            if f & (1 << 9):
                m.flags |= dmesh.BATCH_F_ALPHA_TEST
                m.alpha_ref = alpha.threshold
        stencil = props.get("NiStencilProperty")
        if stencil is not None and stencil.draw_mode == 3:
            m.flags |= dmesh.BATCH_F_DOUBLE_SIDED
        vcp = props.get("NiVertexColorProperty")
        if vcp is not None:
            m.vertex_mode = vcp.vertex_mode
        if max(m.emissive) > 0.25 or (vcp is not None and vcp.vertex_mode == 1):
            m.flags |= dmesh.BATCH_F_EMISSIVE
        if has_vertex_alpha or m.alpha < 0.999:
            m.flags |= dmesh.BATCH_F_VERTEX_ALPHA
        return m

    @staticmethod
    def bake_color(m: Material, vc: Optional[Sequence[float]]) -> Tuple[float, float, float, float]:
        if vc is not None and m.vertex_mode == 2:
            return (vc[0] * m.diffuse[0], vc[1] * m.diffuse[1], vc[2] * m.diffuse[2],
                    vc[3] * m.alpha)
        if vc is not None and m.vertex_mode == 1:
            return (vc[0], vc[1], vc[2], vc[3] * m.alpha)
        return (m.diffuse[0], m.diffuse[1], m.diffuse[2], m.alpha)

    # -------------------------------------------------------------- build
    def build(self) -> Tuple[dmesh.OutMesh, dict]:
        roots = self.nif.roots
        if not roots:
            raise niflib.NifError("NIF has no root")
        for s in self.extra_strings(roots[0]):
            up = s.upper()
            if up.startswith("NC"):
                self.no_collision = True
            if up.startswith("MRK"):
                self.marker_flag = True
        for root in roots:
            if hasattr(root, "translation"):
                self.walk(root, IDENTITY, {}, False, False, [], set())
        if not self.world:
            raise niflib.NifError("NIF has no scene graph root")

        render_geoms = [g for g in self.geoms if not g.in_collision and
                        (self.cfg.keep_hidden or not g.hidden)]
        # skinned geometry, or a bare Biped skeleton (base_anim.nif) that
        # animations and body parts bind to by bone name
        skeletal = any(g.block.skin >= 0 for g in render_geoms) or any(
            b.name.lower().startswith("bip01") for b in self.node_blocks)
        bone_blocks: Set[int] = set()
        for g in render_geoms:
            skin = self.nif.get(g.block.skin)
            if skin is not None:
                bone_blocks.update(r for r in skin.bones if r >= 0)

        # ---- choose which scene nodes survive
        kept: Set[int] = set()
        root_index = roots[0].index
        kept.add(root_index)
        emitter_nodes: Set[int] = set()
        animated: Set[int] = set()
        for b in self.node_blocks + [g.block for g in self.geoms]:
            if any(c.type in TRANSFORM_CONTROLLERS for c in self.controllers(b)):
                animated.add(b.index)
        for p in self.particles:
            for c in self.controllers(p.block):
                if c.type in PARTICLE_CONTROLLERS and c.emitter >= 0 and c.emitter in self.world:
                    emitter_nodes.add(c.emitter)
        for b in self.node_blocks:
            name = b.name.lower()
            if b.type == "RootCollisionNode" or self.in_collision_subtree(b.index):
                continue
            if skeletal or b.index in animated or b.type == "NiBillboardNode" \
                    or name.startswith("attach") or b.index in emitter_nodes \
                    or b.index in bone_blocks:
                kept.add(b.index)
        for g in render_geoms:
            if g.block.index in animated:
                kept.add(g.block.index)
        for k in list(kept):
            p = self.parent.get(k)
            while p is not None:
                kept.add(p)
                p = self.parent.get(p)

        # ---- node table in parent-first order
        out_index: Dict[int, int] = {}
        nodes: List[dmesh.OutNode] = []
        order = sorted(kept, key=self.depth_first_key())
        for bi in order:
            blk = self.nif.blocks[bi]
            p = self.parent.get(bi)
            while p is not None and p not in out_index:
                p = self.parent.get(p)
            parent_out = out_index[p] if p is not None else -1
            local = self.world[bi] if p is None else m_mul(m_inv(self.world[p]), self.world[bi])
            t, q, s = decompose(local)
            flags = 0
            if bi in bone_blocks:
                flags |= dmesh.NODE_F_BONE
            if blk.type == "NiBillboardNode":
                flags |= dmesh.NODE_F_BILLBOARD
            if bi in animated:
                flags |= dmesh.NODE_F_ANIMATED
            if blk.flags & 1:
                flags |= dmesh.NODE_F_HIDDEN
            if blk.name.lower().startswith("attach"):
                flags |= dmesh.NODE_F_ATTACH
            if bi in emitter_nodes:
                flags |= dmesh.NODE_F_EMITTER
            out_index[bi] = len(nodes)
            nodes.append(dmesh.OutNode(blk.name or f"node{bi}", parent_out, flags, t, q, s))

        def owner_of(g: GeomInstance) -> int:
            if g.block.index in out_index:
                return g.block.index
            for p in reversed(g.parent_chain):
                if p in out_index:
                    return p
            return root_index

        # ---- geometry -> prims
        prims: Dict[tuple, Prim] = {}
        src_tris = 0
        for g in render_geoms:
            data = self.nif.get(g.block.data)
            if data is None or not data.vertices:
                continue
            tris = data.triangles if data.type == "NiTriShapeData" else \
                niflib.strips_to_triangles(data.strips)
            tris = [t for t in tris if max(t) < data.num_vertices]
            if not tris:
                continue
            src_tris += len(tris)
            has_va = bool(data.colors) and any(c[3] < 0.999 for c in data.colors)
            mat = self.material(g.props, has_va)
            uvs = None
            if mat.texture is not None:
                if data.uv_sets:
                    uvs = data.uv_sets[mat.uv_set if mat.uv_set < len(data.uv_sets) else 0]
                else:
                    mat.texture = None
            skin = self.nif.get(g.block.skin)
            if skin is not None:
                self.add_skinned(g, data, tris, uvs, mat, skin, out_index, owner_of(g), prims)
            else:
                owner = owner_of(g)
                to_node = m_mul(m_inv(self.world[owner]), g.world)
                self.add_rigid(data, tris, uvs, mat, out_index[owner], to_node, False,
                               range(len(tris)), prims)

        # ---- prims -> batches
        textures: List[str] = []
        tex_index: Dict[str, int] = {}
        batches: List[dmesh.OutBatch] = []
        model_points: List[Vec3] = []
        node_world_out = self.output_node_worlds(nodes)
        out_tris = 0
        diag = self.model_diagonal(render_geoms)
        max_err = (self.cfg.error_tol * diag) ** 2 if diag > 0 else float("inf")
        for (node, _mkey, segment), prim in prims.items():
            for b in self.prim_to_batches(prim, max_err):
                if prim.material.texture is not None:
                    if prim.material.texture not in tex_index:
                        tex_index[prim.material.texture] = len(textures)
                        textures.append(prim.material.texture)
                    b.texture = tex_index[prim.material.texture]
                batches.append(b)
                out_tris += len(b.indices) // 3
                wm = node_world_out[node]
                model_points.extend(m_point(wm, p) for p in b.positions)

        # ---- particle emitters
        emitters = []
        for p in self.particles:
            em = self.emitter(p, out_index, owner_of(p), tex_index, textures)
            if em is not None:
                emitters.append(em)

        mesh = dmesh.OutMesh(nodes=nodes, batches=batches, textures=textures,
                             emitters=emitters)
        if skeletal:
            mesh.flags |= dmesh.DMESH_F_SKELETAL
        if animated:
            mesh.flags |= dmesh.DMESH_F_ANIMATED
        if emitters:
            mesh.flags |= dmesh.DMESH_F_PARTICLES

        # ---- collision (model space)
        coll_tris = 0
        if not mesh.flags & dmesh.DMESH_F_SKELETAL and not self.no_collision:
            cpos, ctri = self.collision_geometry(render_geoms)
            if ctri:
                mesh.collision_positions = cpos
                mesh.collision_triangles = ctri
                coll_tris = len(ctri)

        if model_points:
            lo = tuple(min(p[i] for p in model_points) for i in range(3))
            hi = tuple(max(p[i] for p in model_points) for i in range(3))
            mesh.bbox_min, mesh.bbox_max = lo, hi  # type: ignore[assignment]
            mesh.bound_center, mesh.bound_radius = dmesh.bound_sphere(model_points)

        info = {"key": self.key, "nodes": len(nodes), "batches": len(batches),
                "src_triangles": src_tris, "triangles": out_tris,
                "collision_triangles": coll_tris, "emitters": len(emitters),
                "skeletal": skeletal, "animated": bool(animated), "textures": textures,
                "warnings": self.warnings}
        return mesh, info

    # ---------------------------------------------------------- helpers
    def in_collision_subtree(self, idx: int) -> bool:
        p = self.parent.get(idx)
        while p is not None:
            if self.nif.blocks[p].type == "RootCollisionNode":
                return True
            p = self.parent.get(p)
        return False

    def depth_first_key(self):
        order: Dict[int, int] = {}
        counter = [0]

        def visit(b: niflib.Block, seen: Set[int]) -> None:
            if b.index in seen:
                return
            seen.add(b.index)
            order[b.index] = counter[0]
            counter[0] += 1
            for ref in getattr(b, "children", ()):
                c = self.nif.get(ref)
                if c is not None and hasattr(c, "translation"):
                    visit(c, seen)

        seen: Set[int] = set()
        for r in self.nif.roots:
            if hasattr(r, "translation"):
                visit(r, seen)
        return lambda bi: order.get(bi, 1 << 30)

    def output_node_worlds(self, nodes: Sequence[dmesh.OutNode]) -> List[Mat]:
        out: List[Mat] = []
        for n in nodes:
            local = compose(n.translation, n.rotation, n.scale)
            out.append(local if n.parent < 0 else m_mul(out[n.parent], local))
        return out

    def model_diagonal(self, geoms: Sequence[GeomInstance]) -> float:
        lo = [float("inf")] * 3
        hi = [float("-inf")] * 3
        for g in geoms:
            data = self.nif.get(g.block.data)
            if data is None:
                continue
            for v in data.vertices:
                p = m_point(g.world, v)
                for i in range(3):
                    lo[i] = min(lo[i], p[i])
                    hi[i] = max(hi[i], p[i])
        if lo[0] == float("inf"):
            return 0.0
        return math.sqrt(sum((hi[i] - lo[i]) ** 2 for i in range(3)))

    def add_rigid(self, data, tris, uvs, mat: Material, node_out: int, to_node: Mat,
                  segment: bool, tri_ids, prims: Dict[tuple, Prim]) -> None:
        key = (node_out, mat.key(), segment)
        prim = prims.get(key)
        if prim is None:
            prim = prims[key] = Prim(node_out, mat, segment)
        nm = normal_matrix(to_node)
        remap: Dict[int, int] = {}
        for ti in tri_ids:
            t = tris[ti]
            idx = []
            for v in t:
                j = remap.get(v)
                if j is None:
                    j = len(prim.positions)
                    remap[v] = j
                    prim.positions.append(m_point(to_node, data.vertices[v]))
                    prim.normals.append(m_normal(nm, data.normals[v]) if data.normals else None)
                    prim.uvs.append(tuple(uvs[v]) if uvs is not None else (0.0, 0.0))
                    vc = data.colors[v] if data.colors else None
                    prim.colors.append(self.bake_color(mat, vc))
                idx.append(j)
            prim.triangles.append((idx[0], idx[1], idx[2]))

    def add_skinned(self, g: GeomInstance, data, tris, uvs, mat: Material, skin,
                    out_index: Dict[int, int], fallback_owner: int,
                    prims: Dict[tuple, Prim]) -> None:
        sdata = self.nif.get(skin.data)
        if sdata is None or len(sdata.bone_data) != len(skin.bones):
            self.warnings.append(f"{g.block.name}: broken skin, drawn rigid")
            to_node = m_mul(m_inv(self.world[fallback_owner]), g.world)
            self.add_rigid(data, tris, uvs, mat, out_index[fallback_owner], to_node, False,
                           range(len(tris)), prims)
            return
        weights: List[Dict[int, float]] = [dict() for _ in range(data.num_vertices)]
        for bi, bd in enumerate(sdata.bone_data):
            for vi, w in bd["weights"]:
                if vi < data.num_vertices:
                    weights[vi][bi] = weights[vi].get(bi, 0.0) + w
        per_bone: Dict[int, List[int]] = {}
        unweighted: List[int] = []
        for ti, t in enumerate(tris):
            score: Dict[int, float] = {}
            for v in t:
                for bi, w in weights[v].items():
                    score[bi] = score.get(bi, 0.0) + w
            if not score:
                unweighted.append(ti)
                continue
            best = max(score.items(), key=lambda kv: (kv[1], -kv[0]))[0]
            per_bone.setdefault(best, []).append(ti)
        for bi, tri_ids in per_bone.items():
            bone_ref = skin.bones[bi]
            if bone_ref not in out_index:
                self.warnings.append(f"bone {bone_ref} not in skeleton; drawn rigid")
                to_node = m_mul(m_inv(self.world[fallback_owner]), g.world)
                self.add_rigid(data, tris, uvs, mat, out_index[fallback_owner], to_node,
                               False, tri_ids, prims)
                continue
            bt = sdata.bone_data[bi]["transform"]
            inv_bind = m_from_nif(bt["translation"], bt["rotation"], bt["scale"])
            self.add_rigid(data, tris, uvs, mat, out_index[bone_ref], inv_bind, True,
                           tri_ids, prims)
        if unweighted:
            to_node = m_mul(m_inv(self.world[fallback_owner]), g.world)
            self.add_rigid(data, tris, uvs, mat, out_index[fallback_owner], to_node, False,
                           unweighted, prims)

    def prim_to_batches(self, prim: Prim, max_err: float) -> List[dmesh.OutBatch]:
        # fill missing normals with area-weighted face normals
        if any(n is None for n in prim.normals):
            acc = [[0.0, 0.0, 0.0] for _ in prim.positions]
            for a, b, c in prim.triangles:
                pa, pb, pc = prim.positions[a], prim.positions[b], prim.positions[c]
                n = decimate._cross(decimate._sub(pb, pa), decimate._sub(pc, pa))
                for v in (a, b, c):
                    acc[v][0] += n[0]
                    acc[v][1] += n[1]
                    acc[v][2] += n[2]
            for i, n in enumerate(prim.normals):
                if n is None:
                    ln = math.sqrt(sum(x * x for x in acc[i])) or 1.0
                    prim.normals[i] = (acc[i][0] / ln, acc[i][1] / ln, acc[i][2] / ln)

        eps = self.cfg.weld_eps

        def qpos(p):
            return (round(p[0] / eps), round(p[1] / eps), round(p[2] / eps))

        keys = [qpos(p) + (round(n[0], 3), round(n[1], 3), round(n[2], 3),
                           round(uv[0], 5), round(uv[1], 5),
                           round(c[0], 3), round(c[1], 3), round(c[2], 3), round(c[3], 3))
                for p, n, uv, c in zip(prim.positions, prim.normals, prim.uvs, prim.colors)]
        rep, tris = decimate.weld(keys, prim.triangles)
        positions = [prim.positions[i] for i in rep]
        # vertices sharing a position with different attributes sit on seams
        groups: Dict[tuple, int] = {}
        for p in positions:
            k = qpos(p)
            groups[k] = groups.get(k, 0) + 1
        locked = {i for i, p in enumerate(positions) if groups[qpos(p)] > 1}
        if len(tris) >= self.cfg.min_tris and self.cfg.ratio < 1.0:
            target = max(self.cfg.min_tris // 2, int(len(tris) * self.cfg.ratio))
            tris = decimate.simplify(positions, tris, target, locked, max_err)
        order, tris = decimate.compact(tris, len(rep))
        src = [rep[i] for i in order]

        m = prim.material
        flags = m.flags
        if prim.segment:
            flags |= dmesh.BATCH_F_SEGMENT
        batches = []
        for chunk_src, chunk_tris in split_chunks(src, tris, 65535):
            b = dmesh.OutBatch(
                node=prim.node, texture=dmesh.DMESH_NO_TEXTURE, flags=flags,
                alpha_ref=m.alpha_ref, blend=m.blend,
                diffuse=(m.diffuse[0], m.diffuse[1], m.diffuse[2], m.alpha),
                emissive=m.emissive,
                positions=[prim.positions[i] for i in chunk_src],
                normals=[prim.normals[i] for i in chunk_src],
                uvs=[prim.uvs[i] for i in chunk_src],
                colors=[prim.colors[i] for i in chunk_src],
                indices=[i for t in chunk_tris for i in t])
            batches.append(b)
        return batches

    def collision_geometry(self, render_geoms: Sequence[GeomInstance]
                           ) -> Tuple[List[Vec3], List[Tuple[int, int, int]]]:
        if self.collision_nodes:
            sources = [g for g in self.geoms if g.in_collision]
        else:
            sources = list(render_geoms)
        positions: List[Vec3] = []
        tris: List[Tuple[int, int, int]] = []
        for g in sources:
            if g.block.skin >= 0:
                continue
            data = self.nif.get(g.block.data)
            if data is None or not data.vertices:
                continue
            t = data.triangles if data.type == "NiTriShapeData" else \
                niflib.strips_to_triangles(data.strips)
            base = len(positions)
            positions.extend(m_point(g.world, v) for v in data.vertices)
            tris.extend((a + base, b + base, c + base) for a, b, c in t
                        if max(a, b, c) < data.num_vertices)
        if not tris:
            return [], []
        eps = max(self.cfg.weld_eps, 0.01)
        keys = [(round(p[0] / eps), round(p[1] / eps), round(p[2] / eps)) for p in positions]
        rep, tris = decimate.weld(keys, tris)
        pos = [positions[i] for i in rep]
        if len(tris) >= self.cfg.min_tris and self.cfg.collision_ratio < 1.0:
            lo = [min(p[i] for p in pos) for i in range(3)]
            hi = [max(p[i] for p in pos) for i in range(3)]
            diag = math.sqrt(sum((hi[i] - lo[i]) ** 2 for i in range(3)))
            tris = decimate.simplify(pos, tris, int(len(tris) * self.cfg.collision_ratio),
                                     set(), (self.cfg.error_tol * diag) ** 2)
        order, tris = decimate.compact(tris, len(pos))
        return [pos[i] for i in order], tris

    def emitter(self, p: GeomInstance, out_index: Dict[int, int], owner: int,
                tex_index: Dict[str, int], textures: List[str]) -> Optional[dmesh.OutEmitter]:
        ctrl = next((c for c in self.controllers(p.block) if c.type in PARTICLE_CONTROLLERS),
                    None)
        if ctrl is None:
            return None
        node_block = ctrl.emitter if ctrl.emitter in out_index else owner
        mat = self.material(p.props, True)
        tex = dmesh.DMESH_NO_TEXTURE
        if mat.texture is not None:
            if mat.texture not in tex_index:
                tex_index[mat.texture] = len(textures)
                textures.append(mat.texture)
            tex = tex_index[mat.texture]
        gravity = (0.0, 0.0, 0.0)
        grow = fade = 0.0
        color = tuple(ctrl.initial_color)
        ref = ctrl.particle_modifier
        seen = set()
        while ref >= 0 and ref not in seen:
            seen.add(ref)
            mod = self.nif.get(ref)
            if mod is None:
                break
            if mod.type == "NiGravity":
                d = mod.direction
                ln = math.sqrt(sum(x * x for x in d)) or 1.0
                gravity = tuple(x / ln * mod.force for x in d)  # type: ignore[assignment]
            elif mod.type == "NiParticleGrowFade":
                grow, fade = mod.grow, mod.fade
            elif mod.type == "NiParticleColorModifier":
                cd = self.nif.get(mod.color_data)
                if cd is not None and cd.keys["keys"]:
                    color = tuple(cd.keys["keys"][0][1])
            ref = getattr(mod, "next_modifier", -1)
        data = self.nif.get(p.block.data)
        max_particles = getattr(data, "num_particles", 0) or len(ctrl.particles) or 32
        rate = ctrl.birth_rate if ctrl.use_birth_rate else \
            (max_particles / max(ctrl.lifetime, 1e-3))
        flags = mat.flags & (dmesh.BATCH_F_ALPHA_BLEND | dmesh.BATCH_F_ALPHA_TEST |
                             dmesh.BATCH_F_ADDITIVE)
        return dmesh.OutEmitter(
            node=out_index[node_block], texture=tex, max_particles=min(max_particles, 0xFFFF),
            flags=flags, color=(color[0], color[1], color[2], color[3] if len(color) > 3 else 1.0),
            size=ctrl.initial_size, emit_rate=rate, lifetime=ctrl.lifetime,
            lifetime_random=ctrl.lifetime_variation, speed=ctrl.speed,
            speed_random=ctrl.speed_variation, vertical_dir=ctrl.declination,
            vertical_angle=ctrl.declination_variation, horizontal_dir=ctrl.planar_angle,
            horizontal_angle=ctrl.planar_angle_variation, gravity=gravity,
            grow_time=grow, fade_time=fade)


def split_chunks(src: List[int], tris: List[Tuple[int, int, int]], limit: int):
    """Split an indexed list into chunks with at most `limit` vertices each."""
    if len(src) <= limit:
        yield src, tris
        return
    remap: Dict[int, int] = {}
    chunk_src: List[int] = []
    chunk_tris: List[Tuple[int, int, int]] = []
    for t in tris:
        new = [v for v in t if v not in remap]
        if len(chunk_src) + len(set(new)) > limit:
            yield chunk_src, chunk_tris
            remap, chunk_src, chunk_tris = {}, [], []
        idx = []
        for v in t:
            if v not in remap:
                remap[v] = len(chunk_src)
                chunk_src.append(src[v])
            idx.append(remap[v])
        chunk_tris.append((idx[0], idx[1], idx[2]))
    if chunk_tris:
        yield chunk_src, chunk_tris


def decode_pixel_data(nif: niflib.NifFile, pix: niflib.Block) -> Optional[bytes]:
    """Embedded NiPixelData (RGB8 / RGBA8 / PAL8) -> PNG bytes."""
    from PIL import Image

    if not pix.mipmaps:
        return None
    w, h, ofs = pix.mipmaps[0]
    fmt = pix.pixel_format
    if fmt == 0:
        img = Image.frombytes("RGB", (w, h), pix.pixels[ofs:ofs + w * h * 3])
    elif fmt == 1:
        img = Image.frombytes("RGBA", (w, h), pix.pixels[ofs:ofs + w * h * 4])
    elif fmt == 2:
        pal = nif.get(pix.palette)
        if pal is None:
            return None
        idx = pix.pixels[ofs:ofs + w * h]
        img = Image.new("RGBA", (w, h))
        img.putdata([tuple(pal.colors[i][:3]) + (pal.colors[i][3] if pal.has_alpha else 255,)
                     for i in idx])
    else:
        return None
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


def demake_nif(data: bytes, key: str, cfg: MeshConfig) -> MeshResult:
    nif = niflib.parse_nif(data)
    builder = MeshBuilder(nif, key, cfg)
    mesh, info = builder.build()
    out = dmesh.write_dmesh(mesh)
    info["bytes"] = len(out)
    return MeshResult(key, True, out, info=info, textures=list(mesh.textures),
                      embedded=builder.embedded)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def _worker_ctx(arg):
    data_cfg, cfg = arg
    return (DataSource(data_cfg), cfg)


def _cook_one(ctx, item) -> MeshResult:
    source, cfg = ctx
    key, path = item
    try:
        res = demake_nif(source.read(path), key, cfg)
        res.info["src"] = path
        return res
    except Exception as exc:
        return MeshResult(key, False, error=f"{type(exc).__name__}: {exc}")


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", action="append", default=[],
                    help="Data Files directory (repeatable; BSAs inside are mounted)")
    ap.add_argument("--bsa", action="append", default=[], help="extra BSA archive")
    ap.add_argument("--out", required=True)
    ap.add_argument("--ratio", type=float, default=0.5,
                    help="fraction of triangles kept by decimation (default 0.5)")
    ap.add_argument("--min-tris", type=int, default=24)
    ap.add_argument("--error-tol", type=float, default=0.02)
    ap.add_argument("--collision-ratio", type=float, default=0.5)
    ap.add_argument("--keep-hidden", action="store_true")
    ap.add_argument("--prefix", default="meshes/", help="only cook NIFs under this path")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    dmcommon.setup_logging(args.verbose)
    if not args.data and not args.bsa:
        ap.error("give at least one --data directory or --bsa archive")
    if not 0.05 <= args.ratio <= 1.0:
        ap.error("--ratio must be within [0.05, 1.0]")

    cfg = MeshConfig(ratio=args.ratio, min_tris=args.min_tris, error_tol=args.error_tol,
                     collision_ratio=args.collision_ratio, keep_hidden=args.keep_hidden)
    data_cfg = DataConfig(tuple(args.data), tuple(args.bsa))
    source = DataSource(data_cfg)
    items = [(dmcommon.mesh_key(p), p) for p in source.list(args.prefix, (".nif",))]
    if args.limit:
        items = items[:args.limit]
    LOG.info("meshes: %d NIF files", len(items))

    t0 = time.time()
    pack = dmcommon.DPakWriter(dmcommon.DPAK_KIND_MESH)
    manifest = {"config": asdict(cfg), "meshes": [], "failures": [], "textures": []}
    used_textures: Set[str] = set()
    totals = {"src": 0, "out": 0, "skeletal": 0}
    loose_dir = os.path.join(args.out, "loose", "textures")
    for res in dmcommon.parallel_map(_cook_one, items, _worker_ctx, (data_cfg, cfg), args.jobs,
                                     dmcommon.progress_printer("meshes")):
        if not res.ok:
            LOG.warning("mesh %s failed: %s", res.key, res.error)
            manifest["failures"].append({"key": res.key, "error": res.error})
            continue
        pack.add(res.key, res.data)
        manifest["meshes"].append(res.info)
        used_textures.update(res.textures)
        totals["src"] += res.info["src_triangles"]
        totals["out"] += res.info["triangles"]
        totals["skeletal"] += 1 if res.info["skeletal"] else 0
        for tkey, png in res.embedded.items():
            path = os.path.join(loose_dir, tkey + ".png")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(png)

    os.makedirs(args.out, exist_ok=True)
    size = pack.write(os.path.join(args.out, "meshes.dpak"))
    manifest["textures"] = sorted(used_textures)
    manifest["totals"] = totals
    manifest["pack_bytes"] = size
    with open(os.path.join(args.out, "meshes_manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    keep = (100.0 * totals["out"] / totals["src"]) if totals["src"] else 0.0
    LOG.info("wrote meshes.dpak: %d meshes (%d skeletal), %.1f KB, triangles %d -> %d "
             "(%.0f%%), %d failed, %.1fs", len(pack.items), totals["skeletal"], size / 1024.0,
             totals["src"], totals["out"], keep, len(manifest["failures"]), time.time() - t0)
    return 1 if (args.strict and manifest["failures"]) else 0


if __name__ == "__main__":
    sys.exit(main())
