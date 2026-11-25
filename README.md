# Morrowind PSP Demake

A native Sony PSP-3000 runtime (C99 + Allegrex VFPU assembly, PSPSDK) plus an
offline PC "demake cooker" that converts **your own legally owned** copy of
*The Elder Scrolls III: Morrowind* (plus *Tribunal* / *Bloodmoon*) into
lightweight PS1/PSP-era assets: 16/256-colour swizzled textures, decimated
low-poly meshes with rigid-limb characters, and (in later milestones) zone
data, compiled MWScript bytecode and the full quest/dialogue database.

**No game data is included in this repository** and none is ever committed.
The cooker reads `Morrowind.esm` / `.bsa` files from your installation and
writes cooked packs to a local output directory.

Status and milestone plan: see [PROGRESS.md](PROGRESS.md).

## Layout

```
src/                     PSP runtime (C99)
  common.h               types, alignment helpers, asset-name hash
  fmt.h                  cooked binary formats (shared contract with the cooker)
  mem.c/.h               strict 32 MB static arena (regions, marks, pools, buddy)
  vfpu.c/.h              VFPU math: matrices, quaternions, culling, ray tests
  selftest.c/.h          allocator + VFPU-vs-scalar test suite (host and device)
  main.c                 Milestone 1 boot harness
tests/host/              native build of the portable runtime code + test runner
tools/demake_cooker/     Python 3 + Pillow offline cooker
  tex_demaker.py         textures -> DTEX (T4/T8, swizzled) + icon atlases
  mesh_demaker.py        NIF -> DMESH (decimated, segmented skins, collision)
  nif.py                 NetImmerse 4.0.0.2 reader
  decimate.py            quadric edge-collapse decimation
  dmesh.py, dmcommon.py  writers/readers, BSA, DPAK, hashing
  dev/nifxml_dump.py     verifies NIF layouts against niftools nif.xml
docs/                    FORMATS.md, VFPU.md, NIF_LAYOUTS.md
```

## Building the PSP runtime

Install the pspdev toolchain (prebuilt release):

```bash
curl -L -o pspdev.tar.gz https://github.com/pspdev/pspdev/releases/latest/download/pspdev-ubuntu-latest-x86_64.tar.gz
```

```bash
mkdir -p "$HOME/pspdev-root" && tar -xzf pspdev.tar.gz -C "$HOME/pspdev-root"
```

```bash
export PSPDEV="$HOME/pspdev-root/pspdev" PATH="$HOME/pspdev-root/pspdev/bin:$PATH"
```

Then:

```bash
make
```

This produces `EBOOT.PBP`. Copy it to `ms0:/PSP/GAME/pspwind/EBOOT.PBP` on a
PSP-3000 running custom firmware (or open it in PPSSPP). The Milestone 1
harness runs the allocator and VFPU self-tests and reports
`ALL SELF-TESTS PASSED`.

## Tests

```bash
make test
```

`host-test` compiles the runtime's portable code natively (ASan + UBSan) and
runs the shared self-test suite; `tools-test` runs the cooker unit tests.
Neither needs the PSP toolchain.

## Cooking assets

```bash
pip install -r tools/demake_cooker/requirements.txt
```

```bash
python3 tools/demake_cooker/mesh_demaker.py --data "/path/to/Morrowind/Data Files" --out build/cooked
```

```bash
python3 tools/demake_cooker/tex_demaker.py --data "/path/to/Morrowind/Data Files" --data build/cooked/loose --out build/cooked --only-keys build/cooked/meshes_manifest.json
```

`--data` mounts a Data Files directory (its BSAs in Morrowind → Tribunal →
Bloodmoon order, loose files overriding archives). Cooking meshes first lets
the texture pass cook only textures the models use (plus embedded NIF
textures, written to `build/cooked/loose`). Useful options: `--ratio`
(triangle budget, default 0.5), `--size`/`--small-size` (64/32 px),
`--colors auto|16|256`, `--dither`, `--jobs N`, `--limit N`, `--strict`.
