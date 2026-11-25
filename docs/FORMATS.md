# Cooked asset formats

`src/fmt.h` is the authoritative definition; this page explains the intent.
The Python writers mirror the structs byte for byte, and
`tools/demake_cooker/tests/test_common.py` fails if any size in `fmt.h`
drifts from the Python `struct` formats.

Common rules: little endian, offsets relative to the start of the file or
pack entry, all GE/VFPU-facing data 16-byte aligned, names stored as
`pw_hash_name()` FNV-1a hashes of normalised keys (lower case, `/`
separators, no extension, no leading `textures/` or `meshes/`).

## DPAK — asset pack

`textures.dpak`, `meshes.dpak`. A 32-byte header, a directory of
`{hash, offset, size, name_ofs}` entries sorted by hash (binary search at
runtime), a string block, then entry payloads on 64-byte boundaries so a
payload can be read from the Memory Stick straight into an arena buffer.

## DTEX — paletted PSP texture

* `psm` is `GU_PSM_T4` (16 colours, 4 bpp) or `GU_PSM_T8` (256 colours).
* CLUT is ABGR8888 (`GU_PSM_8888` CLUT) at `clut_ofs`, 16 or 256 entries,
  ready for `sceGuClutLoad(entries / 8, ptr)`.
* Up to 4 levels (`level[i]`: size, GE buffer width, offset, byte size).
  Level 0 is nearest-neighbour downscaled (crunchy), mips are box filtered
  and mapped to the same palette.
* `DTEX_F_SWIZZLED`: every level is in GE swizzle order (16-byte × 8-row
  blocks) for `sceGuTexMode(psm, levels-1, 0, GU_TRUE)`.
* `DTEX_F_ALPHA_TEST`: palette index 0 is transparent; draw with alpha test
  at `alpha_ref`. `DTEX_F_ALPHA_BLEND`: palette alpha is graded.
* T4 pixels: left pixel in the low nibble.

## DATL — UI atlas index

Icons are shelf-packed into 256×256 T8 pages stored as DTEX entries named
`atlas/<atlas>/<page>`. `atlas/<atlas>.datl` holds the page hashes and
`{icon hash, page, x, y, w, h}` entries sorted by hash.

## DMESH — demade model

| Section | Contents |
|---|---|
| header | counts, offsets, model-space bounding sphere + box |
| nodes | parent-first hierarchy, local TRS bind transform, flags |
| batches | one draw call each: node, texture, flags, quantisation params |
| textures | `{hash, name}` → `textures.dpak` entry |
| emitters | particle emitter parameters (billboard sprites at runtime) |
| collision | model-space float positions + u16/u32 triangle indices |
| strings | node / texture names |

Vertex (20 bytes, `GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_NORMAL_8BIT |
GU_VERTEX_16BIT`):

```
u16 u, v      uv  = u16 / 32768 * uv_scale + uv_offset   (sceGuTexScale/Offset)
u32 color     ABGR8888, material diffuse + vertex colour baked in
s8  nx,ny,nz  pad
s16 x, y, z   pos = s16 / 32768 * pos_scale + pos_offset (folded into model matrix)
s16 pad
```

Batches are sorted opaque → alpha-test → alpha-blend. Indices are u16
triangle lists (counter-clockwise front faces).

Node/space conventions:
* static meshes collapse into node 0; animated, billboard, `Attach*`,
  emitter and bone nodes are kept with their ancestors
* batch vertices are local to `batch.node`; world = placement × node world
* `DMESH_BATCH_F_SEGMENT` batches belong to a skinned mesh converted to
  rigid limbs: each triangle was assigned to its dominant bone and stored in
  that bone's space, so drawing = one matrix per limb, no skinning

## DWORLD — zone directory (`zones.dpak` entry `world`)

Zone table (`entry_hash` of the zone's pack entry, display name, kind,
exterior cell range, byte size), a sorted exterior map `(x, y) → zone` and a
sorted interior map `hash(lower-case cell name) → zone`. Both maps are binary
searched at runtime (door destinations, walking across zone borders).

## DZONE — one loadable zone

Exterior zones cover `zone_size × zone_size` cells (default 3×3) plus a
fringe ring of neighbour cells as terrain only; interior zones are one cell.
Everything is world space (Morrowind units, +Z up).

| Section | Contents |
|---|---|
| header | kind, flags (water / terrain), core cell range, water height, fog colour + near/far, ambient, sun colour + direction, bbox |
| cells | grid position, fringe flag, terrain patch, batch range, instance range |
| meshes | hashes of the DMESH models the instances use |
| textures | `{hash, name}` used by terrain and merged batches |
| terrain | 17×17 heights (`float`), 17×17 baked colours, 16×16 per-quad texture ids, range of its static batches |
| batches | `DMeshBatch` records in world space; `node` = cell slot, `texture` = zone texture |
| instances | placed references: mesh, kind, flags (merged / persistent / no mesh), cell, door, object id hash, refnum, pos, Morrowind euler rot, scale, world bound sphere |
| doors | destination zone, position + rotation, destination cell hash, lock level, key, trap |
| lights | LIGH references: position, radius, colour, flags |

Placement follows OpenMW: `world = T(pos) · Rx(−rx) · Ry(−ry) · Rz(−rz) · S(scale)`
(Morrowind's Z rotation is clockwise seen from above). Static models up to
`--merge-max-tris` triangles are pre-transformed into the cell batches and
keep an instance flagged `DZONE_IF_MERGED` (collision only). If merging
pushes a zone over its memory budget the cooker rebuilds it instanced.
