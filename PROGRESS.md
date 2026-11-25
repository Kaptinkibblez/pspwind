# Progress

| Milestone | Scope | Status |
|---|---|---|
| 1 | Scaffold, Makefile, 32 MB arena (`mem.c`), VFPU math (`vfpu.c`), `tex_demaker.py`, `mesh_demaker.py` | **Done – awaiting review** |
| 2 | `world_demaker.py` (17×17 terrain, batched cells) + `zone.c` + fogged `sceGu` renderer (`gfx.c`) | Not started |
| 3 | `phys.c` collision, controls, doors/zone transitions, segmented-limb animation | Not started |
| 4 | `rpg_compiler.py`, MWScript VM (`vm.c`), dialogue + journal UI (`ui.c`) | Not started |
| 5 | Combat, magic, inventory/barter/loot UI, character creation, levelling (`rpg.c`) | Not started |
| 6 | AI (`ai.c`), audio mixer (`audio.c`), save/load (`save.c`), 30/60 FPS pass | Not started |

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
