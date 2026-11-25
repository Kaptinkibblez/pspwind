"""Synthetic NetImmerse 4.0.0.2 block builders and geometry generators.

Used by the unit tests and by make_testworld.py. Each block helper returns
(block type name, writer fn) for nif.write_nif(). Field order follows
nif.xml for version 4.0.0.2 (see docs/NIF_LAYOUTS.md).
"""

from __future__ import annotations

import math
import os
import sys
from typing import List, Optional, Sequence, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import nif  # noqa: E402

I9 = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)


def rot_z(deg: float) -> Tuple[float, ...]:
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return (c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0)  # row-major


def _net(w, name, extra=-1, ctrl=-1):
    w.string(name)
    w.i32(extra)
    w.i32(ctrl)


def _av(w, name, t=(0, 0, 0), r=I9, s=1.0, props=(), flags=0, extra=-1, ctrl=-1,
        bound=None):
    _net(w, name, extra, ctrl)
    w.u16(flags)
    w.floats(t)
    w.floats(r)
    w.f32(s)
    w.floats((0.0, 0.0, 0.0))
    w.ref_list(list(props))
    if bound is None:
        w.bool(False)
    else:
        w.bool(True)
        w.u32(1)  # BOX_BV
        w.floats(bound[0])
        w.floats(I9)
        w.floats(bound[1])


def node(name, children, kind="NiNode", effects=(), **kw):
    def f(w):
        _av(w, name, **kw)
        w.ref_list(list(children))
        w.ref_list(list(effects))
    return (kind, f)


def switch_node(name, children, index, **kw):
    def f(w):
        _av(w, name, **kw)
        w.ref_list(list(children))
        w.ref_list([])
        w.u32(index)
    return ("NiSwitchNode", f)


def shape(name, data, props=(), skin=-1, kind="NiTriShape", **kw):
    def f(w):
        _av(w, name, props=props, **kw)
        w.i32(data)
        w.i32(skin)
    return (kind, f)


def _geom_data(w, verts, normals, colors, uvs):
    n = len(verts)
    w.u16(n)
    w.bool(True)
    for v in verts:
        w.floats(v)
    w.bool(normals is not None)
    for v in normals or ():
        w.floats(v)
    w.floats((0.0, 0.0, 0.0))
    w.f32(1.0)
    w.bool(colors is not None)
    for c in colors or ():
        w.floats(c)
    w.u16(1 if uvs is not None else 0)
    w.bool(uvs is not None)
    for u in uvs or ():
        w.floats(u)


def shape_data(verts, tris, normals=None, uvs=None, colors=None):
    def f(w):
        _geom_data(w, verts, normals, colors, uvs)
        w.u16(len(tris))
        w.u32(len(tris) * 3)
        for t in tris:
            for i in t:
                w.u16(i)
        w.u16(1)  # one match group
        w.u16(2)
        w.u16(0)
        w.u16(0)
    return ("NiTriShapeData", f)


def strips_data(verts, strips, normals=None, uvs=None):
    def f(w):
        _geom_data(w, verts, normals, None, uvs)
        w.u16(sum(max(0, len(s) - 2) for s in strips))
        w.u16(len(strips))
        for s in strips:
            w.u16(len(s))
        for s in strips:
            for i in s:
                w.u16(i)
    return ("NiTriStripsData", f)


def texturing(src, clamp=3, uv_set=0, with_bump=False, count=7):
    def f(w):
        _net(w, "")
        w.u16(0)
        w.u32(2)  # apply mode
        w.u32(count)
        for slot in range(max(5, min(count, 10))):
            used = slot == 0 or (slot == 5 and with_bump)
            w.bool(used)
            if used:
                w.i32(src)
                w.u32(clamp)
                w.u32(2)
                w.u32(uv_set)
                w.i16(0)
                w.i16(-75)
                w.u16(0)
                if slot == 5:
                    w.floats((1.0, 0.0))
                    w.floats((1.0, 0.0, 0.0, 1.0))
    return ("NiTexturingProperty", f)


def source_texture(filename):
    def f(w):
        _net(w, "")
        w.u8(1)
        w.string(filename)
        w.u32(5)
        w.u32(2)
        w.u32(3)
        w.u8(1)
    return ("NiSourceTexture", f)


def internal_texture(pixel_ref):
    def f(w):
        _net(w, "")
        w.u8(0)
        w.u8(1)
        w.i32(pixel_ref)
        w.u32(5)
        w.u32(2)
        w.u32(3)
        w.u8(1)
    return ("NiSourceTexture", f)


def pixel_data_rgb(width, height, rgb_bytes):
    def f(w):
        w.u32(0)  # PX_FMT_RGB8
        for m in (0xFF, 0xFF00, 0xFF0000, 0):
            w.u32(m)
        w.u32(24)
        w.buf += bytes(8)
        w.i32(-1)
        w.u32(1)
        w.u32(3)
        w.u32(width)
        w.u32(height)
        w.u32(0)
        w.u32(len(rgb_bytes))
        w.buf += rgb_bytes
    return ("NiPixelData", f)


def material(diffuse=(1, 1, 1), alpha=1.0, emissive=(0, 0, 0)):
    def f(w):
        _net(w, "")
        w.u16(0)
        w.floats((1.0, 1.0, 1.0))
        w.floats(diffuse)
        w.floats((0.0, 0.0, 0.0))
        w.floats(emissive)
        w.f32(10.0)
        w.f32(alpha)
    return ("NiMaterialProperty", f)


def alpha_prop(flags, threshold=0):
    def f(w):
        _net(w, "")
        w.u16(flags)
        w.u8(threshold)
    return ("NiAlphaProperty", f)


def stencil(draw_mode):
    def f(w):
        _net(w, "")
        w.u16(0)
        w.u8(0)
        for v in (0, 0, 0xFFFFFFFF, 0, 0, 0, draw_mode):
            w.u32(v)
    return ("NiStencilProperty", f)


def vertex_color_prop(vertex_mode, lighting_mode=1):
    def f(w):
        _net(w, "")
        w.u16(0)
        w.u32(vertex_mode)
        w.u32(lighting_mode)
    return ("NiVertexColorProperty", f)


def zbuffer():
    def f(w):
        _net(w, "")
        w.u16(3)
    return ("NiZBufferProperty", f)


def string_extra(s, next_ref=-1):
    def f(w):
        w.i32(next_ref)
        w.u32(4 + len(s))
        w.string(s)
    return ("NiStringExtraData", f)


def text_keys(keys):
    def f(w):
        w.i32(-1)
        w.u32(0)
        w.u32(len(keys))
        for t, s in keys:
            w.f32(t)
            w.string(s)
    return ("NiTextKeyExtraData", f)


def skin_instance(data_ref, root_ref, bones):
    def f(w):
        w.i32(data_ref)
        w.i32(root_ref)
        w.ref_list(list(bones))
    return ("NiSkinInstance", f)


def skin_data(bones):
    """bones: list of (rotation9, translation3, scale, [(vertex, weight), ...])"""
    def f(w):
        w.floats(I9)
        w.floats((0.0, 0.0, 0.0))
        w.f32(1.0)
        w.u32(len(bones))
        w.i32(-1)  # skin partition (always -1 in Morrowind)
        for rot, trans, scale, weights in bones:
            w.floats(rot)
            w.floats(trans)
            w.f32(scale)
            w.floats((0.0, 0.0, 0.0, 10.0))
            w.u16(len(weights))
            for vi, wt in weights:
                w.u16(vi)
                w.f32(wt)
    return ("NiSkinData", f)


def _controller(w, next_ref, target, flags=8):
    w.i32(next_ref)
    w.u16(flags)
    w.f32(1.0)
    w.f32(0.0)
    w.f32(0.0)
    w.f32(2.0)
    w.i32(target)


def keyframe_controller(target, data_ref, next_ref=-1):
    def f(w):
        _controller(w, next_ref, target)
        w.i32(data_ref)
    return ("NiKeyframeController", f)


def keyframe_data(xyz=False):
    def f(w):
        if xyz:
            w.u32(1)
            w.u32(4)
            w.f32(0.0)  # axis order
            for _ in range(3):
                w.u32(2)
                w.u32(1)
                w.f32(0.0)
                w.f32(0.0)
                w.f32(1.0)
                w.f32(1.5)
        else:
            w.u32(2)
            w.u32(3)  # TBC quaternion keys
            for t in (0.0, 1.0):
                w.f32(t)
                w.floats((1.0, 0.0, 0.0, 0.0))
                w.floats((0.0, 0.0, 0.0))
        w.u32(2)
        w.u32(2)  # quadratic translations
        for t in (0.0, 1.0):
            w.f32(t)
            w.floats((0.0, 0.0, t * 10.0))
            w.floats((0.0, 0.0, 1.0))
            w.floats((0.0, 0.0, 1.0))
        w.u32(1)
        w.u32(1)
        w.f32(0.0)
        w.f32(1.0)
    return ("NiKeyframeData", f)


def vis_controller(target, data_ref, next_ref=-1):
    def f(w):
        _controller(w, next_ref, target)
        w.i32(data_ref)
    return ("NiVisController", f)


def vis_data():
    def f(w):
        w.u32(2)
        w.f32(0.0)
        w.u8(1)
        w.f32(1.0)
        w.u8(0)
    return ("NiVisData", f)


def uv_controller(target, data_ref):
    def f(w):
        _controller(w, -1, target)
        w.u16(0)
        w.i32(data_ref)
    return ("NiUVController", f)


def uv_data():
    def f(w):
        for g in range(4):
            if g == 0:
                w.u32(2)
                w.u32(1)
                w.f32(0.0)
                w.f32(0.0)
                w.f32(1.0)
                w.f32(1.0)
            else:
                w.u32(0)
    return ("NiUVData", f)


def particle_controller(target, emitter, modifier):
    def f(w):
        _controller(w, -1, target)
        for v in (30.0, 5.0, 0.0, 0.3, 0.0, 6.28):
            w.f32(v)
        w.floats((0.0, 0.0, 1.0))
        w.floats((1.0, 0.5, 0.25, 1.0))
        w.f32(8.0)  # size
        w.f32(0.0)
        w.f32(100.0)
        w.u8(0)
        w.f32(20.0)  # birth rate
        w.f32(1.5)  # lifetime
        w.f32(0.5)
        w.u8(1)  # use birth rate
        w.u8(0)
        w.floats((1.0, 1.0, 1.0))
        w.i32(emitter)
        w.u16(0)
        w.f32(0.0)
        w.u16(0)
        w.f32(0.0)
        w.f32(0.0)
        w.u16(2)  # particles
        w.u16(2)
        for _ in range(2):
            w.floats((0.0, 0.0, 1.0))
            w.floats((0.0, 0.0, 0.0))
            w.floats((0.0, 1.0, 0.0))
            w.u16(0)
            w.u16(0)
        w.i32(-1)
        w.i32(modifier)
        w.i32(-1)
        w.u8(0)
    return ("NiParticleSystemController", f)


def gravity(next_ref, controller, force=-4.0):
    def f(w):
        w.i32(next_ref)
        w.i32(controller)
        w.f32(0.0)
        w.f32(force)
        w.u32(0)
        w.floats((0.0, 0.0, 0.0))
        w.floats((0.0, 0.0, 1.0))
    return ("NiGravity", f)


def grow_fade(next_ref, controller, grow=0.2, fade=0.4):
    def f(w):
        w.i32(next_ref)
        w.i32(controller)
        w.f32(grow)
        w.f32(fade)
    return ("NiParticleGrowFade", f)


def particles_data(count):
    def f(w):
        _geom_data(w, [(0.0, 0.0, 0.0)] * count, None, None, None)
        w.u16(count)
        w.f32(4.0)
        w.u16(count)
        w.bool(True)
        w.floats([1.0] * count)
    return ("NiAutoNormalParticlesData", f)


def point_light():
    def f(w):
        _av(w, "Light")
        w.u32(0)
        w.f32(1.0)
        w.floats((0.1, 0.1, 0.1))
        w.floats((1.0, 0.8, 0.5))
        w.floats((0.0, 0.0, 0.0))
        w.floats((1.0, 0.0, 0.0))
    return ("NiPointLight", f)


# --------------------------------------------------------------------------
# geometry generators
# --------------------------------------------------------------------------


def cube(size=10.0):
    h = size / 2.0
    verts, normals, uvs, tris = [], [], [], []
    faces = [((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, -1, 0), (0, 0, 1)),
             ((0, 1, 0), (-1, 0, 0), (0, 0, 1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    for n, u, v in faces:
        base = len(verts)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            verts.append(tuple(h * (n[i] + su * u[i] + sv * v[i]) for i in range(3)))
            normals.append(n)
            uvs.append(((su + 1) / 2.0, (sv + 1) / 2.0))
        tris += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    return verts, normals, uvs, tris


def uv_sphere(radius=50.0, rings=16, segments=32):
    """Closed sphere: duplicated seam column so UVs are continuous."""
    verts, normals, uvs, tris = [], [], [], []
    for i in range(rings + 1):
        th = math.pi * i / rings
        for j in range(segments + 1):
            ph = 2 * math.pi * j / segments
            n = (math.sin(th) * math.cos(ph), math.sin(th) * math.sin(ph), math.cos(th))
            verts.append(tuple(radius * c for c in n))
            normals.append(n)
            uvs.append((j / segments, i / rings))
    row = segments + 1
    for i in range(rings):
        for j in range(segments):
            a, b = i * row + j, i * row + j + 1
            c, d = a + row, b + row
            if i > 0:
                tris.append((a, c, b))
            if i < rings - 1:
                tris.append((b, c, d))
    return verts, normals, uvs, tris


def grid(n=20, size=100.0):
    """Flat open n x n quad grid in the XY plane (normal +Z)."""
    verts, normals, uvs, tris = [], [], [], []
    for y in range(n + 1):
        for x in range(n + 1):
            verts.append((size * x / n, size * y / n, 0.0))
            normals.append((0.0, 0.0, 1.0))
            uvs.append((x / n, y / n))
    for y in range(n):
        for x in range(n):
            a = y * (n + 1) + x
            tris += [(a, a + 1, a + n + 2), (a, a + n + 2, a + n + 1)]
    return verts, normals, uvs, tris


def cylinder(radius=5.0, height=40.0, rings=8, segments=8):
    """Open tube along +Z from 0 to height (for limb skinning tests)."""
    verts, normals, uvs, tris = [], [], [], []
    for i in range(rings + 1):
        z = height * i / rings
        for j in range(segments):
            ph = 2 * math.pi * j / segments
            n = (math.cos(ph), math.sin(ph), 0.0)
            verts.append((radius * n[0], radius * n[1], z))
            normals.append(n)
            uvs.append((j / segments, i / rings))
    for i in range(rings):
        for j in range(segments):
            a = i * segments + j
            b = i * segments + (j + 1) % segments
            tris += [(a, b, b + segments), (a, b + segments, a + segments)]
    return verts, normals, uvs, tris


def textured_static_nif(texture="Textures\\Tx_Wood_01.TGA", geometry=None,
                        root_t=(0.0, 0.0, 0.0), extra_props=()) -> bytes:
    verts, normals, uvs, tris = geometry or cube()
    props = [3, 4] + list(range(6, 6 + len(extra_props)))
    blocks = [
        node("Root", [1], t=root_t),
        shape("Tri Box", 2, props=props),
        shape_data(verts, tris, normals=normals, uvs=uvs),
        texturing(5),
        material(diffuse=(0.5, 0.5, 0.5)),
        source_texture(texture),
    ] + list(extra_props)
    return nif.write_nif(blocks, [0])
