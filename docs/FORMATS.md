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
| instances | placed references: mesh, kind, flags (merged / persistent / no mesh), cell, door, object id hash, refnum, pos, Morrowind euler rot, scale, world bound sphere, owner (hash of the owning NPC or faction id from the reference's ANAM / CNAM, 0 = nobody; taking owned things is theft) |
| doors | destination zone, position + rotation, destination cell hash, lock level, key, trap |
| lights | LIGH references: position, radius, colour, flags |

Placement follows OpenMW: `world = T(pos) · Rx(−rx) · Ry(−ry) · Rz(−rz) · S(scale)`
(Morrowind's Z rotation is clockwise seen from above). Static models up to
`--merge-max-tris` triangles are pre-transformed into the cell batches and
keep an instance flagged `DZONE_IF_MERGED` (collision only). If merging
pushes a zone over its memory budget the cooker rebuilds it instanced.

## DANIM — animation clip (`anims.dpak`, entry = `.kf` key)

Header (fps, frame count), tracks `{bone name hash, flags, rotation
offset, translation offset}` and groups `{name hash, start, stop, loop
start, loop stop, root speed}` sorted by hash. Rotations are int16
quaternions (x, y, z, w ÷ 32767), translations floats; a track stores one
value per frame when animated, otherwise a single constant. Sampling is a
frame lookup plus nlerp/lerp between neighbouring frames. The accumulation
root (`Bip01`) keeps only its vertical motion; its horizontal travel per
group is stored as the group speed (units/s).

## Actors in DZONE (version 2)

`DZoneActor` records reference an instance (flagged `DZONE_IF_ACTOR`), a
skeleton mesh, an animation clip and up to 16 body-part meshes with an
attachment-bone hash each (0 for skinned parts, whose rigid-limb segments
bind to skeleton bones by node name) and a mirror bit for left-side parts.
NPCs use `base_anim` (`base_anim_female`, `base_animkna` for beast races)
with the `x`-prefixed `.kf`; race skin parts are chosen by race and sex
(female NPCs fall back to male parts), head and hair come from the NPC
record. Creatures use the `x`-prefixed variant of their model and `.kf`
when present.

## DRPG — RPG database (`rpg.dpak`, version 3)

| Entry | Contents |
|---|---|
| `rpg/core` | resident tables (below), loaded into the DB region |
| `text/<n>` | raw DEFLATE block (~32 KB inflated) of NUL-terminated texts and result-script blobs |
| `script/<id>` | compiled script: `code[code_size]` + string pool |
| `pgrd/<cell>` | path grid of one cell, loaded with the zone (below); `<cell>` is the lower-case interior name or `<x>,<y>` |

A *blob id* is `block << 16 | offset` into an inflated text block.

`rpg/core` starts with `DRpgHeader` (`{ofs, count}` per table, all 16-byte
aligned) followed by:

| Table | Notes |
|---|---|
| objects | `DRpgObject` sorted by id hash: type, name, script, flags (unique, female, key, scroll…), icon/model hashes, weight, value, three type-specific links, two list ranges, six type-specific stats (see `fmt.h`) |
| actors | `DRpgActor`: level, dynamics, gold, soul, attributes, creature attacks, skills, disposition, reputation, rank, AI settings, services |
| lists | `{id hash, count}`: inventories, spell lists, leveled-list entries |
| effects | `DRpgEffect`: effect, skill, attribute, range, area, duration, magnitude |
| spells | `DRpgSpell` (SPEL and ENCH) sorted by hash |
| factions / reactions | rank names, favoured attributes/skills, rank requirements, reactions |
| globals | `{hash, type, initial value}` sorted by hash — the bytecode global index |
| gmsts | `{hash, type, value}` (string values are name-pool offsets) |
| scripts / vars | locals counts, flags (`OnActivate` used…), local variable table sorted by hash (`{hash, type, slot}`) |
| starts | start-script indices (SSCR) |
| topics | `DRpgTopic` sorted by name (Greeting order); a second uint32 index sorted by hash |
| infos | `DRpgInfo` per topic in merged load order: text/script blob ids, voice hash, condition range, quest flags, disposition or journal index |
| conds | `DRpgCond {func, comp, is_float, key, value}` — header filters (100–109) first, then SCVR functions (0–73) and variable kinds (74–84) |
| blocks | `{pak entry hash, inflated size, packed size}` |
| skills | `DRpgSkill[27]` in skill index order: name (GMST `sSkill…`), governing attribute, specialisation, the four use gains (SKIL record, else Morrowind's values), description blob |
| mgefs | `DRpgMgef[143]` in effect index order: name (GMST `sEffect…`), flags (record flags merged with the engine's hard-coded ones: target skill/attribute, no duration/magnitude, harmful, spellmaking, enchanting, applied once…), base cost, speed, school, `present`, resisted-by / weakened-by effect |
| races | `DRpgRace` sorted by hash: 7 skill bonuses, male/female attributes, heights/weights, playable/beast flags, description blob; powers are the RACE object's `list2` |
| travel | `DRpgTravel {pos[3], rot_z, cell hash}`; an NPC's range is `stat[1]` (first) and `stat[2]` (count) |
| markers | `DRpgMarker {pos[3], rot_z, cell hash, kind}`: kind 0 Divine Intervention (`divinemarker`), 1 Almsivi (`templemarker`), 2 jail release (`prisonmarker`); exteriors have cell 0 |
| aipkgs | `DRpgAiPackage {pos[3], target, cell, distance, duration, type, hour, idle[8]}` in record order (AI_W, AI_T, AI_E, AI_F + CNDT, AI_A); an NPC's / creature's range is `stat[3]` (first) and `stat[4]` (count), the first package is the active one |
| sounds | `DRpgSound {hash, file, volume, min, max}` sorted by SOUN id hash; `file` = `pw_hash_name("sound/" + path without extension)` |
| soundgens | `DRpgSoundGen {creature, sound, type}` sorted by (creature, type); creature 0 rows are the defaults |
| names | NUL-terminated name pool (offset 0 = "") |

Version 3 additions elsewhere: `DRpgMgef` gains `sound[4]` (cast, bolt,
hit, area; the school's "destruction cast" style default when the MGEF
names none); DOOR `stat[0]`/`stat[1]` = open/close sounds; LIGH `link[2]` =
looping sound; REGN `list2` = ambient sounds `{sound, chance %}`; CREA
`link[0]` = hash of the "x" variant model key (`r/rat` → `r/xrat`), the
animation and preferred skeleton when a creature is spawned at run time.

Path grids: `DPgrdHeader {magic "DPGR", version 1, count, edge_count}`,
then `float[3]` points in world coordinates (exterior cells are offset by
their cell origin), `uint16 edge_first[count + 1]`, `uint16 edges[]`. The
runtime merges the grids of a zone's cells and links each point to the
nearest point of neighbouring cells within 700 units.

Version 2 additions elsewhere: `DRPG_OT_BSGN` objects (birthsigns: `link[1]`
description blob, `list2` spells, `icon` texture); CLAS `stat[4]` services
are OR'd into the NPC's actor services; NPCs with the autocalc flag (12-byte
NPDT) get level-scaled attributes, skills and health/magicka/fatigue
computed by the cooker with Morrowind's autocalc rules, so the runtime never
recalculates record stats (their spell lists stay as authored; autocalculated
spell selection is done for the player at character creation). `rpgdb_open` rejects a core without
the 27 skill and 143 effect rows.

Dialogue result scripts are `DRpgSnippet {flags, code_size, strings_size}`
+ code + strings inside a text block.

### MWScript bytecode

Typed stack machine (int32/float slots, types resolved at compile time).
Locals are slots `[shorts…, longs…, floats…]`; strings are pool offsets.

| Op | Operands | Effect |
|---|---|---|
| `PUSHI8/16/32`, `PUSHF`, `PUSHS` | literal / pool offset | push |
| `LOADL`, `STOREL_S/L/F` | u16 slot | local (short stores truncate to 16 bits) |
| `LOADG`, `STOREG_S/L/F` | u16 global index | global |
| `LOADM`, `STOREM` | u32 ref hash, u16 slot, u8 flags | `ref.var` (bit 0: global script, bits 1–2: type) |
| `LOADD`, `STORED` | u32 name hash | speaker local by name (result scripts), as float |
| `I2F`, `F2I`, `NEG*`, `ADD/SUB/MUL/DIV` I/F, `CMPI/CMPF`+cc | — | arithmetic, comparisons push 0/1 |
| `JMP`, `JZ` | s16 relative | control flow |
| `CALL`, `CALLX` | u16 verb, u8 argc [, u32 ref hash] | instruction/function (functions push a result) |
| `MSGBOX` | u16 fmt, u8 nargs, u8 nbuttons, types[], u16 buttons[] | message box |
| `POP`, `RET` | — | |

Verb ids are indices into `mwsig.py`'s alphabetical table (`src/mwfuncs.h`).

## DSND — sound (`sounds.dpak`, `music.dpak`)

Entries are named by `dmcommon.sound_key()` (`sound/fx/foot/walk`,
`music/explore/mx_1`, `sound/vo/d/m/hlo_dm019`) and start with
`DSndHeader {magic "DSND", version 1, codec, channels, rate, samples,
block_bytes, block_samples, data_size}` (32 bytes).

* codec 0, IMA ADPCM: blocks of `block_samples` frames (505); per channel a
  4-byte header (`int16` first sample, `uint8` step index, 0) and
  `(block_samples - 1) / 2` bytes of nibbles, low nibble first; stereo
  stores the left block, then the right one.
* codec 1, MP3: the MPEG audio frames with ID3 tags removed; `samples` is
  counted from the frame headers.

`sounds.dpak` holds every `Sound/**/*.wav` as mono ADPCM at up to 22 050 Hz
(the mixer's cache); `music.dpak` holds `Music/**` and the MP3 voices, plus
`music/index`: one `<folder> <key>` line per track (explore / battle
playlists).

## DSAV — saved game (`saves/slot<N>.sav`)

`DSaveHeader` (magic "DSAV", version, size, CRC-32 of the rest, rpg/core
fingerprint, struct-size check, and a summary: name, cell, level, game
date and hour, play time, real time), followed by `{tag, size}` sections:
`VMGL` globals, `VMTI` time + randomness + player name/race/class, `VMIN`
script instances and the locals pool, `VMPR` used stat-store slots with
their slot index, `VMIT` used inventory slots with their index (equipment
refers to stacks by index), `VMJR` quest state, journal, known topics,
heard infos, faction ranks, `RPGS` the mechanics state, `GAME` zone and
player, `AIST` stolen goods, spawn serial and every actor of the current
zone (position, state, package; spawned creatures are re-created). Files
are written as `slot<N>.tmp` and renamed; loading checks everything before
it changes the game.
