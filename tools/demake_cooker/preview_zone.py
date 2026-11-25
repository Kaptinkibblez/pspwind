#!/usr/bin/env python3
"""preview_zone - software preview of a cooked zone (no PSP needed).

Rasterises a DZONE from zones.dpak plus its DMESH instances with a simple
painter's algorithm (flat-shaded triangles: vertex colour x the average
colour of the DTEX palette, distance fog from the zone header) using the
same camera conventions as the runtime (yaw 0 = north/+Y, Z up, 60 degree
vertical FOV, 480x272). Useful to eyeball cooked content - terrain, object
placement and rotation, fog colour - on a PC.

    preview_zone.py --data build/cooked --zone zone/ext_0_0 --out shot.png
    preview_zone.py --data build/cooked --zone "zone/int/test hamlet, cellar" --top
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from typing import Dict, List, Optional, Sequence, Tuple

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import dmcommon  # noqa: E402
import dmesh  # noqa: E402
import dzone  # noqa: E402
import tex_demaker  # noqa: E402
import world_demaker  # noqa: E402

W, H = 480, 272


class Library:
    def __init__(self, data_dir: str) -> None:
        self.zones = dmcommon.DPakReader(dmcommon.read_file(os.path.join(data_dir, "zones.dpak")))
        self.meshes = dmcommon.DPakReader(dmcommon.read_file(os.path.join(data_dir, "meshes.dpak")))
        self.textures = dmcommon.DPakReader(dmcommon.read_file(os.path.join(data_dir, "textures.dpak")))
        self.tex_color: Dict[int, Tuple[float, float, float]] = {}
        self.mesh_lib = world_demaker.MeshLib(self.meshes)

    def texture_color(self, h: int) -> Tuple[float, float, float]:
        if h not in self.tex_color:
            name = self.textures.find_hash(h)
            if name is None:
                self.tex_color[h] = (1.0, 0.0, 1.0)
            else:
                info = tex_demaker.parse_dtex(self.textures.get(name))
                lv = info["levels"][-1]
                pal = info["palette"]
                r = g = b = 0.0
                n = 0
                for i in lv.indices:
                    c = pal[i]
                    if c[3] < 128:
                        continue
                    r, g, b, n = r + c[0], g + c[1], b + c[2], n + 1
                self.tex_color[h] = (r / max(n, 1) / 255, g / max(n, 1) / 255, b / max(n, 1) / 255)
        return self.tex_color[h]


def clip_near(poly, near):
    """Sutherland-Hodgman clip of a view-space polygon against z >= near."""
    out = []
    for i, cur in enumerate(poly):
        prev = poly[i - 1]
        cin, pin = cur[2] >= near, prev[2] >= near
        if cin != pin:
            t = (near - prev[2]) / (cur[2] - prev[2])
            out.append(tuple(prev[k] + (cur[k] - prev[k]) * t for k in range(3)))
        if cin:
            out.append(cur)
    return out


def look_at(eye, yaw, pitch):
    f = (math.sin(yaw) * math.cos(pitch), math.cos(yaw) * math.cos(pitch), math.sin(pitch))
    r = (f[1], -f[0], 0.0)
    rl = math.hypot(r[0], r[1]) or 1.0
    r = (r[0] / rl, r[1] / rl, 0.0)
    u = (r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0])
    return eye, f, r, u


def render(lib: Library, zone_entry: str, out: str, top: bool, eye=None, yaw=None,
           pitch=None) -> dict:
    z = dzone.read_zone(lib.zones.get(zone_entry))
    fog = z["fog_color"][:3]
    fog_near, fog_far = z["fog_near"], z["fog_far"]
    tris: List[Tuple[bool, List[Tuple[float, float, float]], Tuple[float, float, float]]] = []

    # gather world-space triangles with colours
    def add_batch(b, tex_hash, xf):
        base = tex_hash is not None and lib.texture_color(tex_hash) or (0.8, 0.8, 0.8)
        two_sided = bool(b["flags"] & dmesh.BATCH_F_DOUBLE_SIDED)
        vs = b["vertices"]
        idx = b["indices"]
        for k in range(0, len(idx), 3):
            pts = [xf(vs[idx[k + m]]["pos"]) for m in range(3)]
            col = [sum(vs[idx[k + m]]["color"][c] for m in range(3)) / 3 for c in range(3)]
            tris.append((two_sided, pts, tuple(base[c] * col[c] for c in range(3))))

    for b in z["batches"]:
        th = z["textures"][b["texture"]]["hash"] if b["texture"] != dmesh.DMESH_NO_TEXTURE else None
        add_batch(b, th, lambda p: p)
    for ins in z["instances"]:
        if ins["mesh"] == dzone.NONE16 or ins["flags"] & dzone.IF_MERGED:
            continue
        name = lib.meshes.find_hash(z["meshes"][ins["mesh"]])
        dm = lib.mesh_lib.get(name) if name else None
        if dm is None:
            continue
        place = world_demaker.placement_matrix(ins["pos"], ins["rot"], ins["scale"])
        for b in dm["batches"]:
            m = world_demaker._mmul(place, dm["node_worlds"][b["node"]])
            th = dm["textures"][b["texture"]]["hash"] if b["texture"] != dmesh.DMESH_NO_TEXTURE else None
            add_batch(b, th, lambda p, m=m: world_demaker._xf(m, p))

    lo, hi = z["bbox_min"], z["bbox_max"]
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    img = Image.new("RGB", (W, H), tuple(int(c * 255) for c in fog))
    draw = ImageDraw.Draw(img)
    if top:
        span = max(hi[0] - lo[0], (hi[1] - lo[1]) * W / H, 1.0) * 1.05
        s = W / span

        def proj(p):
            return (W / 2 + (p[0] - cx) * s, H / 2 - (p[1] - cy) * s, p[2])

        items = []
        for _, pts, col in tris:
            q = [proj(p) for p in pts]
            items.append((sum(p[2] for p in q) / 3, q, col))
        items.sort(key=lambda t: t[0])  # low first, high drawn last
        for _, q, col in items:
            draw.polygon([(p[0], p[1]) for p in q], fill=tuple(int(min(1, c) * 255) for c in col))
    else:
        if eye is None:
            eye = (cx, lo[1] - 300.0, (lo[2] + hi[2]) / 2 + 600.0)
            yaw, pitch = 0.0, -0.25
        e, f, r, u = look_at(eye, yaw, pitch)
        fy = 1.0 / math.tan(math.radians(30))
        fx = fy * H / W

        def proj(p):
            d = (p[0] - e[0], p[1] - e[1], p[2] - e[2])
            zc = sum(d[i] * f[i] for i in range(3))
            xc = sum(d[i] * r[i] for i in range(3))
            yc = sum(d[i] * u[i] for i in range(3))
            return xc, yc, zc

        items = []
        culled = 0
        for two_sided, pts, col in tris:
            q = clip_near([proj(p) for p in pts], 12.0)
            if len(q) < 3:
                continue
            if not two_sided:
                # cooked data is counter-clockwise front facing: in view space
                # (x right, y up) a front face has positive signed area
                a = [(p[0] / p[2], p[1] / p[2]) for p in q[:3]]
                area = (a[1][0] - a[0][0]) * (a[2][1] - a[0][1]) - (a[2][0] - a[0][0]) * (a[1][1] - a[0][1])
                if area <= 0.0:
                    culled += 1
                    continue
            depth = sum(p[2] for p in q) / len(q)
            if depth > fog_far + 256:
                continue
            fogf = max(0.0, min(1.0, (depth - fog_near) / max(fog_far - fog_near, 1.0)))
            c = tuple(col[i] * (1 - fogf) + fog[i] * fogf for i in range(3))
            scr = [(W / 2 + p[0] / p[2] * fx * W / 2, H / 2 - p[1] / p[2] * fy * H / 2) for p in q]
            items.append((depth, scr, c))
        items.sort(key=lambda t: -t[0])
        for _, scr, c in items:
            draw.polygon(scr, fill=tuple(int(min(1, x) * 255) for x in c))
    img.save(out)
    return {"triangles": len(tris), "zone": z["name"], "backfaces": locals().get("culled", 0)}


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, help="directory with the three .dpak files")
    ap.add_argument("--zone", required=True, help="pack entry, e.g. zone/ext_0_0")
    ap.add_argument("--out", required=True)
    ap.add_argument("--top", action="store_true", help="orthographic top-down map")
    ap.add_argument("--eye", type=float, nargs=3)
    ap.add_argument("--yaw", type=float, default=0.0, help="degrees, 0 = north")
    ap.add_argument("--pitch", type=float, default=-15.0, help="degrees")
    args = ap.parse_args(argv)
    lib = Library(args.data)
    info = render(lib, args.zone, args.out, args.top, tuple(args.eye) if args.eye else None,
                  math.radians(args.yaw), math.radians(args.pitch))
    print(f"{info['zone']}: {info['triangles']} triangles, {info['backfaces']} back faces culled "
          f"-> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
