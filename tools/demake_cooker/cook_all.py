#!/usr/bin/env python3
"""cook_all - run the whole demake pipeline in dependency order.

    meshes  (mesh_demaker)  -> meshes.dpak + embedded textures in loose/
    anims   (anim_demaker)  -> anims.dpak
    world   (world_demaker) -> zones.dpak (needs meshes.dpak for merging)
    rpg     (rpg_compiler)  -> rpg.dpak (dialogue, journal, scripts, items, AI,
                               sound records, path grids...)
    sounds  (sound_demaker) -> sounds.dpak (ADPCM effects), music.dpak (music,
                               voices)
    textures(tex_demaker)   -> textures.dpak, limited to textures that the
                               meshes and zones reference (+ UI art, icons)

The output directory is what goes to ms0:/PSP/GAME/pspwind/data/.

    python3 cook_all.py --data "/path/Morrowind/Data Files" --out build/cooked
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from typing import Optional, Sequence

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import dmcommon  # noqa: E402
import anim_demaker  # noqa: E402
import mesh_demaker  # noqa: E402
import rpg_compiler  # noqa: E402
import sound_demaker  # noqa: E402
import tex_demaker  # noqa: E402
import world_demaker  # noqa: E402
from dmcommon import LOG  # noqa: E402


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", action="append", required=True, help="Data Files directory")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--ratio", type=float, default=0.5)
    ap.add_argument("--zone-size", type=int, default=3)
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    dmcommon.setup_logging(args.verbose)
    t0 = time.time()
    data_args = [a for d in args.data for a in ("--data", d)]
    common = ["--jobs", str(args.jobs)] + (["--strict"] if args.strict else [])
    rc = mesh_demaker.main(data_args + ["--out", args.out, "--ratio", str(args.ratio)] + common)
    if rc:
        return rc
    rc = anim_demaker.main(data_args + ["--out", args.out] + common)
    if rc:
        return rc
    rc = world_demaker.main(data_args + ["--out", args.out, "--zone-size", str(args.zone_size),
                                         "--meshes", os.path.join(args.out, "meshes.dpak"),
                                         "--anims", os.path.join(args.out, "anims.dpak")])
    if rc:
        return rc
    rc = rpg_compiler.main(data_args + ["--out", args.out])
    if rc:
        return rc
    rc = sound_demaker.main(data_args + ["--out", args.out] + common)
    if rc:
        return rc
    loose = os.path.join(args.out, "loose")
    extra = ["--data", loose] if os.path.isdir(loose) else []
    rc = tex_demaker.main(data_args + extra + ["--out", args.out,
                                               "--only-keys", os.path.join(args.out, "meshes_manifest.json"),
                                               "--only-keys", os.path.join(args.out, "zones_manifest.json")]
                          + common)
    LOG.info("pipeline finished in %.1fs -> %s", time.time() - t0, args.out)
    return rc


if __name__ == "__main__":
    sys.exit(main())
