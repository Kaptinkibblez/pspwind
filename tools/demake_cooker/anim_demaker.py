#!/usr/bin/env python3
"""anim_demaker - Morrowind .kf animations -> fixed-rate DANIM clips.

Per .kf file:
  * text keys ("Idle: Start", "WalkForward: Loop Start", ... - several keys
    per time are separated by CR LF) become named frame ranges
  * each NiKeyframeController is paired with the NiStringExtraData naming
    its target bone (same order as the controller chain, OpenMW loadKf)
  * tracks are resampled at a fixed rate (default 15 fps - PS1-style
    stepped motion, cheap to evaluate: nlerp between two frames):
    quaternion keys by slerp, XYZ euler keys composed as Rz*Ry*Rx
    (x applied first), translations linearly
  * the accumulation root ("Bip01") keeps only its vertical motion; its
    horizontal travel per group becomes the group's speed (units/s), which
    the runtime uses to move actors instead of sliding the skeleton
  * constant tracks are stored once, rotations as int16 quaternions

Output: anims.dpak (entries named by kf key, e.g. "xbase_anim", "r/xrat").

    anim_demaker.py --data "/path/Morrowind/Data Files" --out build/cooked
"""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
import time
from typing import Dict, List, Optional, Sequence, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import dmcommon  # noqa: E402
import nif as niflib  # noqa: E402
from dmcommon import LOG, BinWriter, StringTable, fnv1a32  # noqa: E402

DANIM_MAGIC = b"DANM"
DANIM_VERSION = 1
T_ROT_ANIMATED = 1 << 0
T_TRANS_ANIMATED = 1 << 1
T_HAS_ROT = 1 << 2
T_HAS_TRANS = 1 << 3
HEADER_FMT = "<4sHHHHfIIIII3I"
TRACK_FMT = "<IHHII"
GROUP_FMT = "<IIHHHHfI"
SIZES = {"DAnimHeader": struct.calcsize(HEADER_FMT), "DAnimTrack": struct.calcsize(TRACK_FMT),
         "DAnimGroup": struct.calcsize(GROUP_FMT)}
assert SIZES == {"DAnimHeader": 48, "DAnimTrack": 16, "DAnimGroup": 24}, SIZES
ACCUM_ROOTS = ("bip01", "root bone")

Quat = Tuple[float, float, float, float]  # x, y, z, w


# --------------------------------------------------------------------------
# quaternion helpers (x, y, z, w)
# --------------------------------------------------------------------------


def q_mul(a: Quat, b: Quat) -> Quat:
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def q_axis(axis: int, angle: float) -> Quat:
    s, c = math.sin(angle / 2.0), math.cos(angle / 2.0)
    v = [0.0, 0.0, 0.0]
    v[axis] = s
    return (v[0], v[1], v[2], c)


def q_norm(q: Quat) -> Quat:
    ln = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / ln for c in q)  # type: ignore[return-value]


def q_slerp(a: Quat, b: Quat, t: float) -> Quat:
    d = sum(x * y for x, y in zip(a, b))
    if d < 0.0:
        b = tuple(-c for c in b)  # type: ignore[assignment]
        d = -d
    if d > 0.9995:
        return q_norm(tuple(x + (y - x) * t for x, y in zip(a, b)))  # type: ignore[arg-type]
    om = math.acos(min(1.0, d))
    so = math.sin(om)
    s0, s1 = math.sin((1 - t) * om) / so, math.sin(t * om) / so
    return tuple(x * s0 + y * s1 for x, y in zip(a, b))  # type: ignore[return-value]


# --------------------------------------------------------------------------
# key evaluation
# --------------------------------------------------------------------------


def _bracket(keys: Sequence[tuple], t: float) -> Tuple[int, int, float]:
    if t <= keys[0][0]:
        return 0, 0, 0.0
    if t >= keys[-1][0]:
        return len(keys) - 1, len(keys) - 1, 0.0
    lo, hi = 0, len(keys) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if keys[mid][0] <= t:
            lo = mid
        else:
            hi = mid
    t0, t1 = keys[lo][0], keys[hi][0]
    return lo, hi, (t - t0) / (t1 - t0) if t1 > t0 else 0.0


def eval_float(group: dict, t: float) -> Optional[float]:
    keys = group["keys"]
    if not keys:
        return None
    a, b, f = _bracket(keys, t)
    if group["interp"] == 5:  # constant
        return keys[a][1]
    return keys[a][1] + (keys[b][1] - keys[a][1]) * f


def eval_vec3(group: dict, t: float) -> Optional[Tuple[float, float, float]]:
    keys = group["keys"]
    if not keys:
        return None
    a, b, f = _bracket(keys, t)
    va, vb = keys[a][1], keys[b][1]
    if group["interp"] == 5:
        return tuple(va)  # type: ignore[return-value]
    return tuple(va[i] + (vb[i] - va[i]) * f for i in range(3))  # type: ignore[return-value]


def eval_rotation(kd: niflib.Block, t: float) -> Optional[Quat]:
    if kd.rotation_keys:
        keys = kd.rotation_keys
        a, b, f = _bracket(keys, t)
        qa = (keys[a][1][1], keys[a][1][2], keys[a][1][3], keys[a][1][0])  # wxyz -> xyzw
        qb = (keys[b][1][1], keys[b][1][2], keys[b][1][3], keys[b][1][0])
        return q_norm(q_slerp(qa, qb, f))
    if kd.xyz_rotations is not None:
        angles = [eval_float(g, t) or 0.0 for g in kd.xyz_rotations]
        if all(not g["keys"] for g in kd.xyz_rotations):
            return None
        # x applied first, then y, then z (OpenMW getXYZRotation, XYZ order)
        return q_norm(q_mul(q_axis(2, angles[2]), q_mul(q_axis(1, angles[1]), q_axis(0, angles[0]))))
    return None


def key_span(kd: niflib.Block) -> float:
    end = 0.0
    if kd.rotation_keys:
        end = max(end, kd.rotation_keys[-1][0])
    if kd.xyz_rotations is not None:
        for g in kd.xyz_rotations:
            if g["keys"]:
                end = max(end, g["keys"][-1][0])
    if kd.translations["keys"]:
        end = max(end, kd.translations["keys"][-1][0])
    return end


# --------------------------------------------------------------------------
# KF parsing
# --------------------------------------------------------------------------


def parse_kf(data: bytes) -> Tuple[List[Tuple[float, str]], List[Tuple[str, niflib.Block]]]:
    nif = niflib.parse_nif(data)
    root = nif.roots[0] if nif.roots else None
    if root is None or root.type != "NiSequenceStreamHelper":
        raise niflib.NifError("KF root is not NiSequenceStreamHelper")
    first = nif.get(root.extra)
    if first is None or first.type != "NiTextKeyExtraData":
        raise niflib.NifError("KF has no text keys")
    text_keys = list(first.keys)
    tracks: List[Tuple[str, niflib.Block]] = []
    extra = nif.get(first.next_extra)
    ctrl = nif.get(root.controller)
    seen = set()
    while extra is not None and ctrl is not None:
        if extra.type != "NiStringExtraData":
            raise niflib.NifError(f"unexpected {extra.type} in KF name chain")
        if ctrl.type == "NiKeyframeController":
            data_block = nif.get(ctrl.data)
            name = extra.string
            if data_block is not None and name.lower() not in seen:
                seen.add(name.lower())
                tracks.append((name, data_block))
        extra = nif.get(extra.next_extra)
        ctrl = nif.get(ctrl.next_controller)
    return text_keys, tracks


def parse_groups(text_keys: Sequence[Tuple[float, str]]) -> Dict[str, dict]:
    """'Group: Start/Stop/Loop Start/Loop Stop' markers -> time ranges."""
    groups: Dict[str, dict] = {}
    for t, text in text_keys:
        for line in text.replace("\r\n", "\n").split("\n"):
            line = line.strip().lower()
            if ":" not in line:
                continue
            name, _, what = line.partition(":")
            name, what = name.strip(), what.strip()
            if what not in ("start", "stop", "loop start", "loop stop"):
                continue
            g = groups.setdefault(name, {})
            if what not in g:  # first occurrence wins
                g[what] = t
    return {n: g for n, g in groups.items() if "start" in g and "stop" in g and g["stop"] >= g["start"]}


# --------------------------------------------------------------------------
# DANIM
# --------------------------------------------------------------------------


def cook_kf(data: bytes, key: str, fps: float = 15.0) -> Tuple[bytes, dict]:
    text_keys, tracks = parse_kf(data)
    groups = parse_groups(text_keys)
    end = max([key_span(kd) for _, kd in tracks] + [t for t, _ in text_keys] + [0.0])
    frames = int(math.floor(end * fps + 1e-4)) + 1
    if frames > 65535:
        raise ValueError(f"{key}: {frames} frames exceed the 16-bit frame index")
    times = [i / fps for i in range(frames)]

    track_data = []
    accum = None
    for name, kd in tracks:
        rots = [eval_rotation(kd, t) for t in times]
        trans = [eval_vec3(kd.translations, t) for t in times]
        has_rot = rots[0] is not None
        has_trans = trans[0] is not None
        if has_rot:  # keep hemisphere continuity for nlerp at runtime
            for i in range(1, frames):
                if sum(a * b for a, b in zip(rots[i], rots[i - 1])) < 0.0:
                    rots[i] = tuple(-c for c in rots[i])  # type: ignore[assignment]
        entry = {"name": name, "rots": rots if has_rot else None,
                 "trans": [list(v) for v in trans] if has_trans else None}
        if name.lower() in ACCUM_ROOTS and has_trans and accum is None:
            accum = entry
        track_data.append(entry)

    out_groups = []
    for gname, g in sorted(groups.items()):
        start = int(round(g["start"] * fps))
        stop = min(int(round(g["stop"] * fps)), frames - 1)
        ls = int(round(g.get("loop start", g["start"]) * fps))
        le = min(int(round(g.get("loop stop", g["stop"]) * fps)), frames - 1)
        speed = 0.0
        if accum is not None and stop > start:
            a, b = accum["trans"][start], accum["trans"][stop]
            dist = math.hypot(b[0] - a[0], b[1] - a[1])
            speed = dist / ((stop - start) / fps)
        out_groups.append((gname, start, stop, max(start, min(ls, stop)), max(start, min(le, stop)), speed))
    if accum is not None:
        # strip horizontal root motion: within each group the root keeps the
        # x, y it has at the group's first frame (vertical bob survives); the
        # travel is reported as the group's speed instead
        tr = accum["trans"]
        base = [list(v) for v in tr]
        for gname, start, stop, *_ in out_groups:
            for f in range(start, stop + 1):
                tr[f] = [base[start][0], base[start][1], base[f][2]]

    w = BinWriter()
    strings = StringTable()
    w.reserve(SIZES["DAnimHeader"])
    tracks_ofs = w.align(16)
    trec = w.reserve(16 * len(track_data))
    groups_ofs = w.align(16)
    seen = {}
    for gname, start, stop, ls, le, speed in sorted(out_groups, key=lambda g: fnv1a32(g[0])):
        h = fnv1a32(gname)
        if h in seen:
            raise ValueError(f"group hash collision {gname} / {seen[h]}")
        seen[h] = gname
        w.pack(GROUP_FMT[1:], h, strings.add(gname), start, stop, ls, le, speed, 0)
    for i, tdat in enumerate(track_data):
        flags = 0
        rofs = tofs = 0
        if tdat["rots"] is not None:
            flags |= T_HAS_ROT
            rots = tdat["rots"]
            animated = any(max(abs(a - b) for a, b in zip(r, rots[0])) > 1e-4 for r in rots)
            if animated:
                flags |= T_ROT_ANIMATED
            rofs = w.align(16)
            for r in (rots if animated else rots[:1]):
                w.pack("4h", *(max(-32767, min(32767, int(round(c * 32767.0)))) for c in r))
        if tdat["trans"] is not None:
            flags |= T_HAS_TRANS
            tr = tdat["trans"]
            animated = any(max(abs(a - b) for a, b in zip(v, tr[0])) > 1e-3 for v in tr)
            if animated:
                flags |= T_TRANS_ANIMATED
            tofs = w.align(16)
            for v in (tr if animated else tr[:1]):
                w.pack("3f", *v)
        w.patch(trec + 16 * i, TRACK_FMT[1:], fnv1a32(tdat["name"]), flags, 0, rofs, tofs)
    strings_ofs = w.align(16)
    w.bytes(bytes(strings.data))
    w.align(16)
    w.patch(0, HEADER_FMT[1:], DANIM_MAGIC, DANIM_VERSION, len(track_data), len(out_groups), 0,
            fps, frames, tracks_ofs, groups_ofs, strings_ofs, len(w), 0, 0, 0)
    info = {"key": key, "frames": frames, "tracks": len(track_data), "groups": len(out_groups),
            "seconds": round(end, 2), "bytes": len(w)}
    return w.getvalue(), info


def read_danim(data: bytes) -> dict:
    h = struct.unpack_from(HEADER_FMT, data, 0)
    magic, version, ntracks, ngroups, _r, fps, frames, tracks_ofs, groups_ofs, str_ofs, size = h[:11]
    if magic != DANIM_MAGIC or version != DANIM_VERSION or size != len(data):
        raise ValueError("not a DANIM v1")
    tracks = []
    for i in range(ntracks):
        bone, flags, _r2, rofs, tofs = struct.unpack_from(TRACK_FMT, data, tracks_ofs + 16 * i)
        tr = {"bone_hash": bone, "flags": flags, "rot": None, "trans": None}
        if flags & T_HAS_ROT:
            n = frames if flags & T_ROT_ANIMATED else 1
            tr["rot"] = [tuple(c / 32767.0 for c in struct.unpack_from("<4h", data, rofs + 8 * k))
                         for k in range(n)]
        if flags & T_HAS_TRANS:
            n = frames if flags & T_TRANS_ANIMATED else 1
            tr["trans"] = [struct.unpack_from("<3f", data, tofs + 12 * k) for k in range(n)]
        tracks.append(tr)
    groups = {}
    for i in range(ngroups):
        hsh, nofs, start, stop, ls, le, speed, _r3 = struct.unpack_from(GROUP_FMT, data, groups_ofs + 24 * i)
        name = data[str_ofs + nofs:data.index(b"\0", str_ofs + nofs)].decode("latin-1")
        groups[name] = {"hash": hsh, "start": start, "stop": stop, "loop_start": ls,
                        "loop_stop": le, "speed": speed}
    return {"fps": fps, "frames": frames, "tracks": tracks, "groups": groups}


def sample(anim: dict, track: dict, frame: float):
    """Pose of one track at a fractional frame (nlerp), as the runtime does."""
    def pick(seq, f):
        if seq is None:
            return None
        if len(seq) == 1:
            return seq[0]
        i = int(math.floor(f))
        i = max(0, min(i, len(seq) - 1))
        j = min(i + 1, len(seq) - 1)
        t = f - i
        return tuple(a + (b - a) * t for a, b in zip(seq[i], seq[j]))
    rot = pick(track["rot"], frame)
    if rot is not None:
        rot = q_norm(rot)
    return rot, pick(track["trans"], frame)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def _worker_ctx(arg):
    data_cfg, fps = arg
    return (dmcommon.DataSource(data_cfg), fps)


def _cook_one(ctx, item):
    source, fps = ctx
    key, path = item
    try:
        data, info = cook_kf(source.read(path), key, fps)
        return key, data, info, None
    except Exception as exc:
        return key, None, None, f"{type(exc).__name__}: {exc}"


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", action="append", default=[])
    ap.add_argument("--bsa", action="append", default=[])
    ap.add_argument("--out", required=True)
    ap.add_argument("--fps", type=float, default=15.0)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    dmcommon.setup_logging(args.verbose)
    if not args.data and not args.bsa:
        ap.error("give at least one --data directory or --bsa archive")
    data_cfg = dmcommon.DataConfig(tuple(args.data), tuple(args.bsa))
    source = dmcommon.DataSource(data_cfg)
    items = [(dmcommon.mesh_key(p), p) for p in source.list("meshes/", (".kf",))]
    LOG.info("animations: %d KF files", len(items))
    t0 = time.time()
    pack = dmcommon.DPakWriter(dmcommon.DPAK_KIND_GENERIC)
    manifest = {"fps": args.fps, "anims": [], "failures": []}
    for key, data, info, err in dmcommon.parallel_map(_cook_one, items, _worker_ctx,
                                                      (data_cfg, args.fps), args.jobs):
        if err:
            LOG.warning("anim %s failed: %s", key, err)
            manifest["failures"].append({"key": key, "error": err})
            continue
        pack.add(key, data)
        manifest["anims"].append(info)
    os.makedirs(args.out, exist_ok=True)
    size = pack.write(os.path.join(args.out, "anims.dpak"))
    manifest["pack_bytes"] = size
    with open(os.path.join(args.out, "anims_manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    LOG.info("wrote anims.dpak: %d clips, %.1f KB, %d failed, %.1fs", len(pack.items),
             size / 1024.0, len(manifest["failures"]), time.time() - t0)
    return 1 if (args.strict and manifest["failures"]) else 0


if __name__ == "__main__":
    sys.exit(main())
