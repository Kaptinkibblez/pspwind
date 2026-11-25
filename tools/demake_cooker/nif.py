"""NetImmerse 4.0.0.2 (Morrowind) NIF / KF reader.

Morrowind NIFs carry no per-block sizes, so every block type that can occur
must be parsed exactly. Layouts follow niftools nif.xml evaluated for
version 4.0.0.2 / user version 0 (docs/NIF_LAYOUTS.md lists them). Two
places where nif.xml and OpenMW's reader disagree are resolved in OpenMW's
favour because they never differ on retail data (UV sets when Has UV is
false, LOZENGE bounding volumes which nif.xml omits).

Usage:
    nif = parse_nif(data)
    for root in nif.roots: ...
    nif.get(block.data)  # resolve a ref (None for -1)
"""

from __future__ import annotations

import struct
from typing import Callable, Dict, List, Optional, Sequence, Tuple

NIF_VERSION_MW = 0x04000002
HEADER_PREFIXES = (b"NetImmerse File Format, Version 4.0.0.2",)


class NifError(Exception):
    pass


class Reader:
    __slots__ = ("data", "pos")

    def __init__(self, data: bytes, pos: int = 0) -> None:
        self.data = data
        self.pos = pos

    def _take(self, fmt: str, size: int):
        if self.pos + size > len(self.data):
            raise NifError(f"unexpected end of file at 0x{self.pos:x}")
        v = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += size
        return v

    def u8(self) -> int:
        return self._take("<B", 1)[0]

    def u16(self) -> int:
        return self._take("<H", 2)[0]

    def i16(self) -> int:
        return self._take("<h", 2)[0]

    def u32(self) -> int:
        return self._take("<I", 4)[0]

    def i32(self) -> int:
        return self._take("<i", 4)[0]

    def f32(self) -> float:
        return self._take("<f", 4)[0]

    def bool(self) -> bool:  # 32-bit before 4.1.0.1
        return self.u32() != 0

    def ref(self) -> int:
        return self.i32()

    def floats(self, n: int) -> Tuple[float, ...]:
        if n == 0:
            return ()
        return self._take(f"<{n}f", 4 * n)

    def u16s(self, n: int) -> Tuple[int, ...]:
        if n == 0:
            return ()
        return self._take(f"<{n}H", 2 * n)

    def u32s(self, n: int) -> Tuple[int, ...]:
        if n == 0:
            return ()
        return self._take(f"<{n}I", 4 * n)

    def refs(self, n: int) -> Tuple[int, ...]:
        if n == 0:
            return ()
        return self._take(f"<{n}i", 4 * n)

    def ref_list(self) -> Tuple[int, ...]:
        return self.refs(self.u32())

    def vec3(self) -> Tuple[float, float, float]:
        return self.floats(3)  # type: ignore[return-value]

    def mat33(self) -> Tuple[float, ...]:
        return self.floats(9)  # row-major

    def string(self) -> str:
        n = self.u32()
        if n > 0x100000:
            raise NifError(f"implausible string length {n} at 0x{self.pos - 4:x}")
        if self.pos + n > len(self.data):
            raise NifError("string runs past end of file")
        s = self.data[self.pos:self.pos + n]
        self.pos += n
        return s.decode("latin-1")

    def raw(self, n: int) -> bytes:
        if self.pos + n > len(self.data):
            raise NifError("byte block runs past end of file")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def vec3_array(self, n: int) -> List[Tuple[float, float, float]]:
        flat = self.floats(3 * n)
        return [flat[i:i + 3] for i in range(0, 3 * n, 3)]  # type: ignore[misc]


class Block:
    """A parsed NIF record. Attributes are set by the type readers."""

    def __init__(self, type_name: str, index: int) -> None:
        self.type = type_name
        self.index = index

    def __repr__(self) -> str:
        name = getattr(self, "name", "")
        return f"<{self.type}#{self.index} {name!r}>"


class NifFile:
    def __init__(self, version: int, blocks: List[Block], roots: List[int]) -> None:
        self.version = version
        self.blocks = blocks
        self.root_refs = roots

    @property
    def roots(self) -> List[Block]:
        return [self.blocks[r] for r in self.root_refs if 0 <= r < len(self.blocks)]

    def get(self, ref: int) -> Optional[Block]:
        if ref is None or ref < 0 or ref >= len(self.blocks):
            return None
        return self.blocks[ref]


# --------------------------------------------------------------------------
# shared sub-records
# --------------------------------------------------------------------------


def read_object_net(r: Reader, b: Block) -> None:
    b.name = r.string()
    b.extra = r.ref()
    b.controller = r.ref()


def read_bound_volume(r: Reader) -> dict:
    kind = r.u32()
    if kind == 0xFFFFFFFF:
        return {"type": "base"}
    if kind == 0:
        c = r.vec3()
        return {"type": "sphere", "center": c, "radius": r.f32()}
    if kind == 1:
        return {"type": "box", "center": r.vec3(), "axes": r.floats(9), "extent": r.vec3()}
    if kind == 2:
        return {"type": "capsule", "center": r.vec3(), "origin": r.vec3(),
                "extent": r.f32(), "radius": r.f32()}
    if kind == 3:  # LOZENGE (OpenMW layout for 4.0.0.2)
        return {"type": "lozenge", "radius": r.f32(), "center": r.vec3(),
                "axis0": r.vec3(), "axis1": r.vec3()}
    if kind == 4:
        n = r.u32()
        return {"type": "union", "children": [read_bound_volume(r) for _ in range(n)]}
    if kind == 5:
        return {"type": "halfspace", "plane": r.floats(4), "center": r.vec3()}
    raise NifError(f"unknown bounding volume type {kind}")


def read_av_object(r: Reader, b: Block) -> None:
    read_object_net(r, b)
    b.flags = r.u16()
    b.translation = r.vec3()
    b.rotation = r.mat33()
    b.scale = r.f32()
    b.velocity = r.vec3()
    b.properties = r.ref_list()
    b.bound = read_bound_volume(r) if r.bool() else None


def read_transform(r: Reader) -> dict:
    return {"rotation": r.mat33(), "translation": r.vec3(), "scale": r.f32()}


KEY_SIZES = {"float": 1, "vec3": 3, "color4": 4}


def read_key_group(r: Reader, kind: str) -> dict:
    """KeyGroup<T>: returns {'interp': int, 'keys': [(time, value, extra...)]}"""
    n = r.u32()
    if n == 0:
        return {"interp": 0, "keys": []}
    interp = r.u32()
    width = KEY_SIZES[kind]
    keys = []
    for _ in range(n):
        t = r.f32()
        v = r.floats(width)
        val = v[0] if width == 1 else v
        if interp == 2:  # quadratic: forward + backward tangents
            fwd = r.floats(width)
            back = r.floats(width)
            keys.append((t, val, fwd[0] if width == 1 else fwd, back[0] if width == 1 else back))
        elif interp == 3:  # TBC
            keys.append((t, val, r.floats(3)))
        else:
            keys.append((t, val))
    return {"interp": interp, "keys": keys}


def read_morph_key_group(r: Reader) -> dict:
    """Morph keys: interpolation is always present (even with zero keys)."""
    n = r.u32()
    interp = r.u32()
    keys = []
    for _ in range(n):
        t = r.f32()
        v = r.f32()
        if interp == 2:
            keys.append((t, v, r.f32(), r.f32()))
        elif interp == 3:
            keys.append((t, v, r.floats(3)))
        else:
            keys.append((t, v))
    return {"interp": interp, "keys": keys}


# --------------------------------------------------------------------------
# block readers
# --------------------------------------------------------------------------


def rd_node(r: Reader, b: Block) -> None:
    read_av_object(r, b)
    b.children = r.ref_list()
    b.effects = r.ref_list()


def rd_switch_node(r: Reader, b: Block) -> None:
    rd_node(r, b)
    b.switch_index = r.u32()


def rd_lod_node(r: Reader, b: Block) -> None:
    rd_switch_node(r, b)
    b.lod_center = r.vec3()
    n = r.u32()
    b.lod_levels = [(r.f32(), r.f32()) for _ in range(n)]


def rd_geometry(r: Reader, b: Block) -> None:
    read_av_object(r, b)
    b.data = r.ref()
    b.skin = r.ref()


def read_geometry_data(r: Reader, b: Block) -> None:
    n = r.u16()
    b.num_vertices = n
    b.vertices = r.vec3_array(n) if r.bool() else []
    b.normals = r.vec3_array(n) if r.bool() else []
    b.center = r.vec3()
    b.radius = r.f32()
    if r.bool():
        flat = r.floats(4 * n)
        b.colors = [flat[i:i + 4] for i in range(0, 4 * n, 4)]
    else:
        b.colors = []
    b.data_flags = r.u16()
    num_uv = b.data_flags & 0x3F
    if not r.bool():
        num_uv = 0
    b.uv_sets = []
    for _ in range(num_uv):
        flat = r.floats(2 * n)
        b.uv_sets.append([flat[i:i + 2] for i in range(0, 2 * n, 2)])


def rd_tri_shape_data(r: Reader, b: Block) -> None:
    read_geometry_data(r, b)
    b.num_triangles = r.u16()
    num_points = r.u32()
    idx = r.u16s(num_points)
    b.triangles = [idx[i:i + 3] for i in range(0, len(idx) - len(idx) % 3, 3)]
    groups = r.u16()
    b.match_groups = [r.u16s(r.u16()) for _ in range(groups)]


def rd_tri_strips_data(r: Reader, b: Block) -> None:
    read_geometry_data(r, b)
    b.num_triangles = r.u16()
    num_strips = r.u16()
    lengths = r.u16s(num_strips)
    b.strips = [r.u16s(n) for n in lengths]


def strips_to_triangles(strips: Sequence[Sequence[int]]) -> List[Tuple[int, int, int]]:
    tris = []
    for s in strips:
        for i in range(len(s) - 2):
            a, b_, c = s[i], s[i + 1], s[i + 2]
            if a == b_ or b_ == c or a == c:
                continue
            tris.append((a, b_, c) if i % 2 == 0 else (b_, a, c))
    return tris


def rd_particles_data(r: Reader, b: Block) -> None:
    read_geometry_data(r, b)
    b.num_particles = r.u16()
    b.particle_radius = r.f32()
    b.num_active = r.u16()
    b.sizes = r.floats(b.num_vertices) if r.bool() else ()


def rd_rotating_particles_data(r: Reader, b: Block) -> None:
    rd_particles_data(r, b)
    b.rotations = r.floats(4 * b.num_vertices) if r.bool() else ()


def rd_skin_instance(r: Reader, b: Block) -> None:
    b.data = r.ref()
    b.skeleton_root = r.ref()
    b.bones = r.ref_list()


def rd_skin_data(r: Reader, b: Block) -> None:
    b.transform = read_transform(r)
    nbones = r.u32()
    b.partition = r.ref()
    b.bone_data = []
    for _ in range(nbones):
        tr = read_transform(r)
        bound = r.floats(4)
        nv = r.u16()
        weights = []
        for _ in range(nv):
            vi = r.u16()
            weights.append((vi, r.f32()))
        b.bone_data.append({"transform": tr, "bound": bound, "weights": weights})


def read_property(r: Reader, b: Block) -> None:
    read_object_net(r, b)
    b.flags = r.u16()


def rd_flag_property(r: Reader, b: Block) -> None:
    read_property(r, b)


def read_tex_desc(r: Reader) -> dict:
    return {"source": r.ref(), "clamp": r.u32(), "filter": r.u32(), "uv_set": r.u32(),
            "ps2_l": r.i16(), "ps2_k": r.i16(), "unknown": r.u16()}


def rd_texturing_property(r: Reader, b: Block) -> None:
    read_property(r, b)
    b.apply_mode = r.u32()
    count = r.u32()
    b.texture_count = count
    b.textures: List[Optional[dict]] = []
    slots = max(5, min(count, 10))
    for i in range(slots):
        if r.bool():
            desc = read_tex_desc(r)
            if i == 5:  # bump map extras
                desc["luma"] = r.floats(2)
                desc["matrix"] = r.floats(4)
            b.textures.append(desc)
        else:
            b.textures.append(None)


def rd_material_property(r: Reader, b: Block) -> None:
    read_property(r, b)
    b.ambient = r.vec3()
    b.diffuse = r.vec3()
    b.specular = r.vec3()
    b.emissive = r.vec3()
    b.glossiness = r.f32()
    b.alpha = r.f32()


def rd_alpha_property(r: Reader, b: Block) -> None:
    read_property(r, b)
    b.threshold = r.u8()


def rd_stencil_property(r: Reader, b: Block) -> None:
    read_property(r, b)
    b.enabled = r.u8()
    b.func = r.u32()
    b.ref_value = r.u32()
    b.mask = r.u32()
    b.fail_action = r.u32()
    b.zfail_action = r.u32()
    b.pass_action = r.u32()
    b.draw_mode = r.u32()


def rd_vertex_color_property(r: Reader, b: Block) -> None:
    read_property(r, b)
    b.vertex_mode = r.u32()
    b.lighting_mode = r.u32()


def rd_fog_property(r: Reader, b: Block) -> None:
    read_property(r, b)
    b.fog_depth = r.f32()
    b.fog_color = r.vec3()


def rd_source_texture(r: Reader, b: Block) -> None:
    read_object_net(r, b)
    b.external = r.u8()
    b.filename = ""
    b.pixel_data = -1
    if b.external:
        b.filename = r.string()
    else:
        b.use_internal = r.u8()
        if b.use_internal:
            b.pixel_data = r.ref()
    b.pixel_layout = r.u32()
    b.use_mipmaps = r.u32()
    b.alpha_format = r.u32()
    b.is_static = r.u8()


def rd_pixel_data(r: Reader, b: Block) -> None:
    b.pixel_format = r.u32()
    b.masks = r.u32s(4)
    b.bpp = r.u32()
    b.fast_compare = r.raw(8)
    b.palette = r.ref()
    nmips = r.u32()
    b.bytes_per_pixel = r.u32()
    b.mipmaps = [(r.u32(), r.u32(), r.u32()) for _ in range(nmips)]
    b.pixels = r.raw(r.u32())


def rd_palette(r: Reader, b: Block) -> None:
    b.has_alpha = r.u8()
    n = r.u32()
    count = 16 if n == 16 else 256
    b.colors = [r.raw(4) for _ in range(count)]


def read_extra(r: Reader, b: Block) -> None:
    b.next_extra = r.ref()
    b.num_bytes = r.u32()


def rd_extra_data(r: Reader, b: Block) -> None:
    read_extra(r, b)
    b.payload = r.raw(b.num_bytes)


def rd_string_extra(r: Reader, b: Block) -> None:
    read_extra(r, b)
    b.string = r.string()


def rd_text_key_extra(r: Reader, b: Block) -> None:
    read_extra(r, b)
    n = r.u32()
    b.keys = [(r.f32(), r.string()) for _ in range(n)]


def rd_vert_weights_extra(r: Reader, b: Block) -> None:
    read_extra(r, b)
    b.weights = r.floats(r.u16())


def read_controller(r: Reader, b: Block) -> None:
    b.next_controller = r.ref()
    b.flags = r.u16()
    b.frequency = r.f32()
    b.phase = r.f32()
    b.start_time = r.f32()
    b.stop_time = r.f32()
    b.target = r.ref()


def rd_data_controller(r: Reader, b: Block) -> None:
    read_controller(r, b)
    b.data = r.ref()


def rd_uv_controller(r: Reader, b: Block) -> None:
    read_controller(r, b)
    b.uv_set = r.u16()
    b.data = r.ref()


def rd_path_controller(r: Reader, b: Block) -> None:
    read_controller(r, b)
    b.bank_dir = r.i32()
    b.max_bank_angle = r.f32()
    b.smoothing = r.f32()
    b.follow_axis = r.i16()
    b.path_data = r.ref()
    b.percent_data = r.ref()


def rd_look_at_controller(r: Reader, b: Block) -> None:
    read_controller(r, b)
    b.look_at = r.ref()


def rd_geom_morpher_controller(r: Reader, b: Block) -> None:
    read_controller(r, b)
    b.data = r.ref()
    b.always_update = r.u8()


def rd_flip_controller(r: Reader, b: Block) -> None:
    read_controller(r, b)
    b.texture_slot = r.u32()
    b.accum_time = r.f32()
    b.delta = r.f32()
    b.sources = r.ref_list()


def rd_particle_system_controller(r: Reader, b: Block) -> None:
    read_controller(r, b)
    b.speed = r.f32()
    b.speed_variation = r.f32()
    b.declination = r.f32()
    b.declination_variation = r.f32()
    b.planar_angle = r.f32()
    b.planar_angle_variation = r.f32()
    b.initial_normal = r.vec3()
    b.initial_color = r.floats(4)
    b.initial_size = r.f32()
    b.emit_start_time = r.f32()
    b.emit_stop_time = r.f32()
    b.reset_particle_system = r.u8()
    b.birth_rate = r.f32()
    b.lifetime = r.f32()
    b.lifetime_variation = r.f32()
    b.use_birth_rate = r.u8()
    b.spawn_on_death = r.u8()
    b.emitter_dimensions = r.vec3()
    b.emitter = r.ref()
    b.num_spawn_generations = r.u16()
    b.percentage_spawned = r.f32()
    b.spawn_multiplier = r.u16()
    b.spawn_speed_chaos = r.f32()
    b.spawn_dir_chaos = r.f32()
    n = r.u16()
    b.num_valid = r.u16()
    b.particles = []
    for _ in range(n):
        vel = r.vec3()
        axis = r.vec3()
        age, life, last = r.floats(3)
        gen, code = r.u16(), r.u16()
        b.particles.append((vel, axis, age, life, last, gen, code))
    b.emitter_modifier = r.ref()
    b.particle_modifier = r.ref()
    b.particle_collider = r.ref()
    b.static_target_bound = r.u8()


def read_particle_modifier(r: Reader, b: Block) -> None:
    b.next_modifier = r.ref()
    b.controller = r.ref()


def rd_gravity(r: Reader, b: Block) -> None:
    read_particle_modifier(r, b)
    b.decay = r.f32()
    b.force = r.f32()
    b.field_type = r.u32()
    b.position = r.vec3()
    b.direction = r.vec3()


def rd_grow_fade(r: Reader, b: Block) -> None:
    read_particle_modifier(r, b)
    b.grow = r.f32()
    b.fade = r.f32()


def rd_color_modifier(r: Reader, b: Block) -> None:
    read_particle_modifier(r, b)
    b.color_data = r.ref()


def rd_particle_rotation(r: Reader, b: Block) -> None:
    read_particle_modifier(r, b)
    b.random_initial_axis = r.u8()
    b.initial_axis = r.vec3()
    b.rotation_speed = r.f32()


def rd_planar_collider(r: Reader, b: Block) -> None:
    read_particle_modifier(r, b)
    b.bounce = r.f32()
    b.height = r.f32()
    b.width = r.f32()
    b.position = r.vec3()
    b.x_vector = r.vec3()
    b.y_vector = r.vec3()
    b.plane = r.floats(4)


def rd_spherical_collider(r: Reader, b: Block) -> None:
    read_particle_modifier(r, b)
    b.bounce = r.f32()
    b.radius = r.f32()
    b.position = r.vec3()


def rd_particle_bomb(r: Reader, b: Block) -> None:
    read_particle_modifier(r, b)
    b.decay = r.f32()
    b.duration = r.f32()
    b.delta_v = r.f32()
    b.start = r.f32()
    b.decay_type = r.u32()
    b.position = r.vec3()
    b.direction = r.vec3()


def rd_keyframe_data(r: Reader, b: Block) -> None:
    n = r.u32()
    b.rotation_type = 0
    b.rotation_keys = []
    b.xyz_rotations = None
    if n:
        b.rotation_type = r.u32()
        if b.rotation_type != 4:
            for _ in range(n):
                t = r.f32()
                w, x, y, z = r.floats(4)
                if b.rotation_type == 3:
                    b.rotation_keys.append((t, (w, x, y, z), r.floats(3)))
                else:
                    b.rotation_keys.append((t, (w, x, y, z)))
        else:
            b.axis_order = r.f32()
            b.xyz_rotations = [read_key_group(r, "float") for _ in range(3)]
    b.translations = read_key_group(r, "vec3")
    b.scales = read_key_group(r, "float")


def rd_pos_data(r: Reader, b: Block) -> None:
    b.keys = read_key_group(r, "vec3")


def rd_float_data(r: Reader, b: Block) -> None:
    b.keys = read_key_group(r, "float")


def rd_color_data(r: Reader, b: Block) -> None:
    b.keys = read_key_group(r, "color4")


def rd_vis_data(r: Reader, b: Block) -> None:
    n = r.u32()
    b.keys = [(r.f32(), r.u8()) for _ in range(n)]


def rd_uv_data(r: Reader, b: Block) -> None:
    b.groups = [read_key_group(r, "float") for _ in range(4)]


def rd_morph_data(r: Reader, b: Block) -> None:
    nmorphs = r.u32()
    nverts = r.u32()
    b.relative_targets = r.u8()
    b.morphs = []
    for _ in range(nmorphs):
        keys = read_morph_key_group(r)
        b.morphs.append({"keys": keys, "vectors": r.vec3_array(nverts)})


def read_dynamic_effect(r: Reader, b: Block) -> None:
    read_av_object(r, b)
    b.affected_nodes = r.u32s(r.u32())


def rd_light(r: Reader, b: Block) -> None:
    read_dynamic_effect(r, b)
    b.dimmer = r.f32()
    b.ambient = r.vec3()
    b.diffuse = r.vec3()
    b.specular = r.vec3()


def rd_point_light(r: Reader, b: Block) -> None:
    rd_light(r, b)
    b.attenuation = r.floats(3)


def rd_spot_light(r: Reader, b: Block) -> None:
    rd_point_light(r, b)
    b.cutoff = r.f32()
    b.exponent = r.f32()


def rd_texture_effect(r: Reader, b: Block) -> None:
    read_dynamic_effect(r, b)
    b.projection_matrix = r.mat33()
    b.projection_translation = r.vec3()
    b.texture_filtering = r.u32()
    b.texture_clamping = r.u32()
    b.texture_type = r.u32()
    b.coord_gen_type = r.u32()
    b.source_texture = r.ref()
    b.enable_plane = r.u8()
    b.plane = r.floats(4)
    b.ps2 = (r.i16(), r.i16())
    b.unknown = r.u16()


def rd_camera(r: Reader, b: Block) -> None:
    read_av_object(r, b)
    b.frustum = r.floats(6)
    b.viewport = r.floats(4)
    b.lod_adjust = r.f32()
    b.scene = r.ref()
    b.screen_polygons = r.ref_list()


def rd_object_net(r: Reader, b: Block) -> None:
    read_object_net(r, b)


READERS: Dict[str, Callable[[Reader, Block], None]] = {
    # scene graph
    "NiNode": rd_node,
    "RootCollisionNode": rd_node,
    "NiBSAnimationNode": rd_node,
    "NiBSParticleNode": rd_node,
    "AvoidNode": rd_node,
    "NiBillboardNode": rd_node,
    "NiCollisionSwitch": rd_node,
    "NiSwitchNode": rd_switch_node,
    "NiLODNode": rd_lod_node,
    "NiTriShape": rd_geometry,
    "NiTriStrips": rd_geometry,
    "NiAutoNormalParticles": rd_geometry,
    "NiRotatingParticles": rd_geometry,
    "NiParticles": rd_geometry,
    "NiCamera": rd_camera,
    # geometry data
    "NiTriShapeData": rd_tri_shape_data,
    "NiTriStripsData": rd_tri_strips_data,
    "NiParticlesData": rd_particles_data,
    "NiAutoNormalParticlesData": rd_particles_data,
    "NiRotatingParticlesData": rd_rotating_particles_data,
    "NiSkinInstance": rd_skin_instance,
    "NiSkinData": rd_skin_data,
    # properties
    "NiTexturingProperty": rd_texturing_property,
    "NiMaterialProperty": rd_material_property,
    "NiAlphaProperty": rd_alpha_property,
    "NiStencilProperty": rd_stencil_property,
    "NiVertexColorProperty": rd_vertex_color_property,
    "NiFogProperty": rd_fog_property,
    "NiZBufferProperty": rd_flag_property,
    "NiWireframeProperty": rd_flag_property,
    "NiDitherProperty": rd_flag_property,
    "NiShadeProperty": rd_flag_property,
    "NiSpecularProperty": rd_flag_property,
    # textures
    "NiSourceTexture": rd_source_texture,
    "NiPixelData": rd_pixel_data,
    "NiPalette": rd_palette,
    # extra data
    "NiExtraData": rd_extra_data,
    "NiStringExtraData": rd_string_extra,
    "NiTextKeyExtraData": rd_text_key_extra,
    "NiVertWeightsExtraData": rd_vert_weights_extra,
    # controllers
    "NiKeyframeController": rd_data_controller,
    "NiVisController": rd_data_controller,
    "NiAlphaController": rd_data_controller,
    "NiRollController": rd_data_controller,
    "NiMaterialColorController": rd_data_controller,
    "NiUVController": rd_uv_controller,
    "NiPathController": rd_path_controller,
    "NiLookAtController": rd_look_at_controller,
    "NiGeomMorpherController": rd_geom_morpher_controller,
    "NiFlipController": rd_flip_controller,
    "NiParticleSystemController": rd_particle_system_controller,
    "NiBSPArrayController": rd_particle_system_controller,
    # particle modifiers
    "NiGravity": rd_gravity,
    "NiParticleGrowFade": rd_grow_fade,
    "NiParticleColorModifier": rd_color_modifier,
    "NiParticleRotation": rd_particle_rotation,
    "NiPlanarCollider": rd_planar_collider,
    "NiSphericalCollider": rd_spherical_collider,
    "NiParticleBomb": rd_particle_bomb,
    # controller data
    "NiKeyframeData": rd_keyframe_data,
    "NiPosData": rd_pos_data,
    "NiFloatData": rd_float_data,
    "NiColorData": rd_color_data,
    "NiVisData": rd_vis_data,
    "NiUVData": rd_uv_data,
    "NiMorphData": rd_morph_data,
    # effects
    "NiTextureEffect": rd_texture_effect,
    "NiAmbientLight": rd_light,
    "NiDirectionalLight": rd_light,
    "NiPointLight": rd_point_light,
    "NiSpotLight": rd_spot_light,
    # misc
    "NiSequenceStreamHelper": rd_object_net,
}

NODE_TYPES = frozenset(k for k, v in READERS.items() if v in (rd_node, rd_switch_node, rd_lod_node))
GEOMETRY_TYPES = frozenset(("NiTriShape", "NiTriStrips"))
PARTICLE_TYPES = frozenset(("NiAutoNormalParticles", "NiRotatingParticles", "NiParticles"))


def parse_nif(data: bytes) -> NifFile:
    nl = data.find(b"\n", 0, 128)
    if nl < 0:
        raise NifError("missing NIF header line")
    header = data[:nl]
    if not header.startswith(HEADER_PREFIXES):
        raise NifError(f"unsupported NIF header {header[:60]!r} (need 4.0.0.2)")
    r = Reader(data, nl + 1)
    version = r.u32()
    if version != NIF_VERSION_MW:
        raise NifError(f"unsupported NIF version 0x{version:08x}")
    count = r.u32()
    if count > 200000:
        raise NifError(f"implausible block count {count}")
    blocks: List[Block] = []
    for i in range(count):
        start = r.pos
        type_name = r.string()
        reader = READERS.get(type_name)
        if reader is None:
            raise NifError(f"unsupported block type {type_name!r} (#{i} at 0x{start:x})")
        b = Block(type_name, i)
        try:
            reader(r, b)
        except NifError as exc:
            raise NifError(f"{type_name} #{i}: {exc}") from None
        blocks.append(b)
    # Trailing bytes after the root list (exporter padding) are ignored.
    roots = list(r.ref_list())
    return NifFile(version, blocks, roots)


# --------------------------------------------------------------------------
# writer (round-trip tests and synthetic fixtures)
# --------------------------------------------------------------------------


class Writer:
    """Minimal 4.0.0.2 writer mirroring the readers above for the block types
    the cooker cares about. Used by the unit tests to build fixtures."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def u8(self, v): self.buf += struct.pack("<B", v)
    def u16(self, v): self.buf += struct.pack("<H", v)
    def i16(self, v): self.buf += struct.pack("<h", v)
    def u32(self, v): self.buf += struct.pack("<I", v)
    def i32(self, v): self.buf += struct.pack("<i", v)
    def f32(self, v): self.buf += struct.pack("<f", v)
    def floats(self, vs): self.buf += struct.pack(f"<{len(vs)}f", *vs)
    def bool(self, v): self.u32(1 if v else 0)

    def string(self, s: str) -> None:
        b = s.encode("latin-1")
        self.u32(len(b))
        self.buf += b

    def ref_list(self, refs) -> None:
        self.u32(len(refs))
        for x in refs:
            self.i32(x)


def write_nif(blocks: Sequence[Tuple[str, Callable[[Writer], None]]], roots: Sequence[int]) -> bytes:
    w = Writer()
    w.buf += b"NetImmerse File Format, Version 4.0.0.2\n"
    w.u32(NIF_VERSION_MW)
    w.u32(len(blocks))
    for type_name, fn in blocks:
        w.string(type_name)
        fn(w)
    w.ref_list(list(roots))
    return bytes(w.buf)
