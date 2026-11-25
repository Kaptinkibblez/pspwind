# Progress

| Milestone | Scope | Status |
|---|---|---|
| 1 | Scaffold, Makefile, 32 MB arena (`mem.c`), VFPU math (`vfpu.c`), `tex_demaker.py`, `mesh_demaker.py` | Done |
| 2 | `world_demaker.py` (17×17 terrain, batched cells) + `zone.c` + fogged `sceGu` renderer (`gfx.c`) | Done |
| 3 | `phys.c` collision, controls, doors/zone transitions, segmented-limb animation | **Done – awaiting review** |
| 4 | `rpg_compiler.py`, MWScript VM (`vm.c`), dialogue + journal UI (`ui.c`) | Not started |
| 5 | Combat, magic, inventory/barter/loot UI, character creation, levelling (`rpg.c`) | Not started |
| 6 | AI (`ai.c`), audio mixer (`audio.c`), save/load (`save.c`), 30/60 FPS pass | Not started |

## Milestone 3 — delivered

### Cooker
* `anim_demaker.py` — `.kf` → DANIM: text keys → named groups (Start/Stop/
  Loop Start/Loop Stop, CR LF separated), controllers paired with the
  `NiStringExtraData` bone names (OpenMW `loadKf` order), resampled at
  15 fps (quaternion slerp; XYZ euler keys composed x→y→z as in OpenMW
  `getXYZRotation`), constant tracks stored once, int16 quaternions.
  `Bip01` horizontal root motion is stripped and stored as group speed.
* `mesh_demaker.py` keeps whole Biped hierarchies (`Bip01*` nodes), so bare
  skeletons such as `base_anim.nif` survive cooking.
* `world_demaker.py` assembles actors: NPC skeleton (`base_anim`,
  `base_anim_female`, `base_animkna` for beast races), race skin BODY parts
  by sex (female → male fallback), head/hair from the NPC record, rigid parts
  attached to MW attachment bones ("Head", "Right Hand", …; left side
  mirrored), skinned parts bound by bone name; creatures use the
  `x`-prefixed model + `.kf` (OpenMW `correctActorModelPath`). DZONE v2 adds
  actor and animation tables.
* `actorpose.py` — Python reference of the runtime posing; the preview tool
  now renders posed actors (`--group walkforward --time 0.25`).
* `make_testworld.py` — adds an original Biped skeleton, a race with
  skinned + rigid body parts, Idle/WalkForward clips, two villagers and a
  creature.

### Runtime
* `anim.c` — validated DANIM, binary-searched groups, looping playback,
  nlerp sampling.
* `actor.c` — bone↔track, part-node↔bone and part↔attachment bindings at
  zone load; per-frame posing of actors within 3000 units (one VFPU
  `mat4_mul` per bone); limb matrix = actor × bone × [mirror] × dequant.
* `phys.c` — static collision from every instance's DMESH collision
  (merged statics included) in a 256-unit CSR grid; swinging doors as
  dynamic triangles; terrain triangles built on demand from the 17×17
  patches; actors as cylinder obstacles. Movement: vertical cylinder vs
  triangle (clip to the slab [feet + 34, head], push out of the 2D
  footprint) so steps ≤ 34 units and slopes ≤ ~46° are walkable; gravity
  627.2, jump, step-down snap 62, ceilings, swimming with buoyancy,
  steep-slope sliding (OpenMW constants). Rays for activation.
* `game.c` — portable gameplay layer: two PSP control schemes (stick moves
  + face buttons look, or the reverse), run/walk, sneak, jump, debug fly,
  activation ray (192 units, occlusion-checked), teleport doors (lock
  check, destination zone + position), swinging doors, exterior border
  transitions, NPCs turning to face the player, debug animation cycling.
* `gfx.c` draws actors (mirrored limbs with flipped winding).
* `main.c` — pause menu (control scheme, invert look, face culling,
  overlay, quit), HUD (crosshair, activation prompt with door destination,
  messages), loading cards on zone requests.

### Verification run
* `make` → `EBOOT.PBP` (335 KB), zero warnings under `-Werror`.
* `make test`: 102 host checks; 72 cooker tests (adds KF parsing,
  resampling, XYZ-euler order, looping, root-motion stripping, NPC/creature
  assembly, posed NPC stands with feet at z 0 and head top at 158);
  world loader + fuzzing; **game test 40/40** — the real C gameplay code
  driven by scripted input on the test world: rests on terrain, walks north
  and stops at the door, the prompt names the cellar, R loads the cellar,
  stands on its floor, the wall stops it, a jump peaks at ~103 units and
  lands, the creature's tail animates, the cellar door leads back, the
  villager's idle head sits at 136 and its walk swings the arms with root
  motion stripped, all 13 body-part batches bind (left side mirrored),
  the villager turns to face the player and blocks movement, crossing x = 0
  loads the western zone, deep water makes the player swim at the surface.
* The tests caught and fixed two real bugs before commit: ground support
  hovering 2 units above sloped terrain, and root motion not being stripped.
* Software previews of posed actors: `docs/img/`.

### Still not verified
1. Nothing has run on a PSP or in PPSSPP (same permission block as before).
   On first run check: face culling (pause menu), sun direction, mirrored
   limbs, fps.
2. Retail data untested. Body-part mirroring for rigid left-side parts and
   the female skeleton fallback follow OpenMW's part list but could not be
   checked against the real assets.

## Milestone 2 — delivered

### Cooker
* `esm.py` — TES3 reader with engine-accurate load-order merging: object
  overrides/deletes, per-cell reference merge keyed by (content file, ref
  index) using the FRMR master byte, `MVRF` moved refs, `NAM0` persistence,
  per-file `LTEX` indices, `VHGT` delta decoding and `VTEX` de-swizzling
  (rules checked against OpenMW's `loadland.cpp`/`loadcell.cpp`/`cellref.cpp`).
* `world_demaker.py` — cuts the exterior into 3×3-cell zones (`--zone-size`)
  with a terrain-only fringe ring, one zone per interior. LAND → 17×17
  patches (point samples of the 65×65 grid), per-quad VTEX textures batched
  per texture, VCLR × baked sun lighting as vertex colour. Small statics are
  merged per cell/texture into world-space batches (falls back to instancing
  if a zone exceeds its budget); everything else stays an instance with a
  world bound. Doors resolve DNAM/DODT to destination zones; LIGH refs become
  zone lights; interior AMBI drives fog/ambient/sun.
* `dzone.py` — DZONE/DWORLD writer/reader mirrored in `fmt.h`.
* `cook_all.py` — meshes → zones → (referenced) textures in one command.
* `make_testworld.py` — original Morrowind-format test world (ESM + BSA:
  hills, shore, hamlet, trees, lantern, door into a cellar interior).
* `preview_zone.py` — software rasteriser for cooked zones (near-plane
  clipping, back-face culling, fog) to inspect cooked data on a PC.

### Runtime
* `pak.c` — DPAK directory in the DB region, binary-search lookups, chunked
  Memory Stick reads.
* `texcache.c` — buddy allocator over the 8 MB TEXTURE region, DTEX
  validation, generation-based eviction across zone loads, magenta fallback.
* `zone.c` — world index, zone + mesh loading into the ZONE region with full
  bounds validation of every offset/count/index, precomputed placement and
  node×dequant matrices, terrain height queries, spawn points, D-cache
  write-back for the GE.
* `gfx.c` — 5650 colour + 16-bit depth, GL-style projection, per-zone fog,
  sun + 3 nearest point lights for instances, baked lighting for terrain,
  VFPU frustum + fog-distance culling, opaque → alpha-test → water → sorted
  alpha-blend passes, nearest-filtered mip-mapped CLUT textures with per-batch
  `sceGuTexScale/Offset` dequantisation. Terrain within 1400 units of the
  camera and the water plane are re-tessellated every frame into the
  display list (4×/2× subdivision), so large triangles never cross the GE's
  guard band.
* `main.c` — boot self-tests, then a free-fly viewer: automatic loading-card
  transitions at exterior zone borders, X through doors (debug teleport),
  △/□ zone cycling, overlay with fps/draws/triangles/memory, SELECT cycles
  face culling.

### Verification run
* `make` → `EBOOT.PBP` (305 KB), zero warnings under `-Werror`.
* `make test`:
  * 48/48 allocator + 54/54 math checks (ASan/UBSan)
  * 64/64 cooker tests (adds ESM merge semantics, VHGT/VTEX codecs, rotation
    convention, terrain sampling, door resolution, merge-budget fallback)
  * world test: cooks the test world and loads all 5 zones through the real C
    loader; the C placement matrices reproduce all 127 instance bounds the
    Python cooker computed (cross-language rotation check); both doors
    resolve; 0 texture misses; 4,000 corrupted zone/mesh blobs are rejected
    or accepted without a single out-of-bounds read
* Independent checks against PPSSPP's sources: VFPU `vmmul`/`vtfm`/`vcrsp`/
  `vqmul` semantics, GE 16-bit texcoord/position decoding, fog = (view z +
  far) × slope, light positions in world space.
* `make package` → `build/psp/pspwind/` (EBOOT + cooked test world).
* Software previews of the cooked zones: `docs/img/`.

### Still not verified (needs a PSP or an emulator run)
1. Nothing has executed on PSP hardware or in PPSSPP yet: building the
   emulator was refused twice by this session's permission policy, even after
   you asked for it. To let me run it here, allow `cmake`/`ninja` for the
   PPSSPP headless build in the session's permission settings; otherwise run
   `build/psp/pspwind/EBOOT.PBP` in PPSSPP yourself.
2. On-screen facts to confirm in that run: face culling direction (SELECT
   cycles CW / CCW / off if geometry appears inside-out), sun direction sign,
   fps.
3. Retail data still untested (no game files here).

## Milestone 1 — delivered

### Environment
* PSPSDK: pspdev prebuilt toolchain (`psp-gcc` 15.2.0), `psp-config --pspsdk-path` OK.
* Python 3.13.18 + Pillow 13.0.0.

### PSP runtime (`src/`)
* `Makefile` — PRX/EBOOT build via `build.mak`, FW 6.61, `-std=c99 -Wall -Wextra -Werror`;
  `make test` runs host + cooker tests without the PSP toolchain.
* `mem.c` — one 32 MB static `.bss` block carved into DB 6 MB / ZONE 8 MB /
  TEXTURE 8 MB / SCRIPT 4 MB / AUDIO_UI 6 MB. Double-ended bump allocation
  with generation-checked marks, fixed-block pools, and a buddy allocator for
  the swizzled texture cache (handles non-power-of-two spans, coalesces fully).
  OOM and misuse halt with a message. newlib heap capped at 512 KB; nothing
  calls malloc.
* `vfpu.c` — inline VFPU assembly for mat4 multiply/transpose/rotate/translate/
  scale, orthonormal inverse, vector/point transforms, batched point
  transforms, dot/cross/normalize, quaternion product, sin/cos/sqrt/rsqrt,
  6-plane sphere/AABB frustum culling (two `vtfm4.q` passes), ray–sphere and
  ray–triangle (Möller–Trumbore). Plus perspective/ortho/look-at, general
  inverse, TRS from quaternion, slerp/nlerp, closest point on triangle.
  Register conventions were taken from disassembling the SDK's
  `libpspgum_vfpu.a` (see `docs/VFPU.md`).
* `selftest.c` — 48 allocator checks and 54 math checks shared by host and
  device; on PSP every VFPU routine is compared against its scalar `ref_*`
  oracle, plus VFPU-vs-scalar micro-benchmarks.
* `main.c` — boot harness: arena init, self-tests, benchmarks, arena report.
* `fmt.h` — binary contract for DPAK/DTEX/DATL/DMESH with static size asserts
  (also cross-checked by the Python tests). `docs/FORMATS.md` describes it.

### Demake cooker (`tools/demake_cooker/`)
* `dmcommon.py` — TES3 BSA reader/writer, layered Data Files view (loose >
  Bloodmoon > Tribunal > Morrowind), DPAK packs, FNV-1a keys identical to the
  runtime, multiprocessing.
* `tex_demaker.py` — DDS/TGA/BMP → power-of-two 64×64 (world) / 32×32 (small)
  nearest-neighbour downscale (UI prefixes get larger sizes), opaque / cutout /
  blend alpha classification, 16-colour T4 when PSNR ≥ 30 dB else 256-colour
  T8, optional Floyd–Steinberg dither, box-filtered mips on the same palette,
  GE swizzle, icon atlas pages (256×256 T8) with a hash index.
  `--only-keys meshes_manifest.json` cooks just the textures models use.
* `nif.py` — NetImmerse 4.0.0.2 reader covering all 72 block types Morrowind
  uses (scene graph, geometry, skinning, properties, controllers, keyframes,
  particles, lights, effects). Each layout was checked against niftools
  `nif.xml`; `dev/nifxml_dump.py` regenerates `docs/NIF_LAYOUTS.md`.
* `decimate.py` — quadric-error half-edge collapse (keeps original vertices so
  UVs/colours stay exact), locked seams, boundary-preserving quadrics, link
  condition, fold-over rejection, error cap.
* `mesh_demaker.py` — strips multi-texture/specular/bump state and bakes
  diffuse × vertex colour into the vertices; collapses static hierarchies
  into one node and keeps only animated/billboard/attach/emitter/bone nodes;
  converts skinned meshes into rigid-limb segments (each triangle goes to its
  dominant bone, stored in that bone's space); batches per node+material,
  welds, decimates (default 50% triangle budget, `--ratio`), quantises to the
  20-byte GE vertex as u16-indexed triangle lists; exports collision
  (RootCollisionNode or render geometry, honouring `NC`/`MRK`), particle
  emitters, and embedded NiPixelData textures.

### Verification run
* `make` → `EBOOT.PBP` (252 KB) with zero warnings under `-Werror`; image
  is 34.6 MB including the 32 MB arena, which fits the PSP-3000's extended
  user memory.
* `make host-test` → 48/48 allocator + 54/54 math checks under ASan/UBSan.
* Cooker tests → 55/55 (BSA, DPAK, layout contract with `fmt.h`, swizzle
  layout, T4/T8 selection, alpha modes, atlas packing, NIF parsing of every
  fixture block family, decimation invariants, geometry round trips for
  static, animated-node and segmented-skin models, CLI runs from a BSA).
* A 16k-triangle model cooks in about 1 s.

### Not yet verified (needs your hardware or data)
1. **VFPU code has not run on a PSP or emulator.** Building PPSSPP's headless
   runner here was blocked by the session's permission policy. Run
   `EBOOT.PBP` on the PSP-3000 or in PPSSPP: it must print
   `ALL SELF-TESTS PASSED` before Milestone 2 builds on it.
2. **The cookers have not seen retail Morrowind files**; there is no game data
   in this environment. NIF layouts match the spec and synthetic fixtures,
   but run `mesh_demaker.py` and `tex_demaker.py` against your
   `Data Files` and send the failure list from `*_manifest.json`. Failed
   files are logged and skipped; they don't stop the run.
3. The 16-bit texcoord decode (`u16 / 32768`, scaled by
   `sceGuTexScale/TexOffset`) matches the GE vertex format as I understand
   it; the Milestone 2 renderer is where that gets confirmed on screen.

### Decisions to confirm
* "~30–50% triangle budget" is read as *keep 50%* by default (`--ratio 0.5`).
* World textures are nearest-neighbour downscaled as specified; `--filter box`
  is available if 512→64 aliasing is too noisy for you.
