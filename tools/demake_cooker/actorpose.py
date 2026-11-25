"""Reference actor posing (mirrors src/actor.c) - used by preview_zone.py and tests.

An actor = skeleton DMESH (bone hierarchy; creature geometry too) + optional
NPC body-part DMESHes + a DANIM clip. Posing:
  local[bone] = bind TRS, with the clip's rotation/translation substituted
               where a track names that bone (by name hash)
  world[bone] = world[parent] * local[bone]
Drawing (all relative to the instance placement matrix):
  creature batch           -> world[batch.node]
  NPC segment batch        -> world[skeleton bone with the part node's name]
  NPC rigid part batch     -> world[part bone] (* mirror X) * part node world
"""

from __future__ import annotations

import math
from typing import Dict, Iterator, List, Optional, Sequence, Tuple

import anim_demaker
import dmesh

Mat = List[List[float]]


def mmul(a: Mat, b: Mat) -> Mat:
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def trs(t: Sequence[float], q: Sequence[float], s: float) -> Mat:
    x, y, z, w = q
    return [[(1 - 2 * (y * y + z * z)) * s, 2 * (x * y - w * z) * s, 2 * (x * z + w * y) * s, t[0]],
            [2 * (x * y + w * z) * s, (1 - 2 * (x * x + z * z)) * s, 2 * (y * z - w * x) * s, t[1]],
            [2 * (x * z - w * y) * s, 2 * (y * z + w * x) * s, (1 - 2 * (x * x + y * y)) * s, t[2]],
            [0.0, 0.0, 0.0, 1.0]]


MIRROR_X: Mat = [[-1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0],
                 [0.0, 0.0, 0.0, 1.0]]


def group_frame(anim: dict, group: str, seconds: float) -> float:
    """Looping playback position (fractional frame) of a group."""
    g = anim["groups"].get(group) or anim["groups"].get("idle")
    if g is None:
        return 0.0
    f = g["start"] + seconds * anim["fps"]
    if f <= g["loop_stop"] or g["loop_stop"] <= g["loop_start"]:
        return min(f, float(g["stop"]))
    span = g["loop_stop"] - g["loop_start"]
    return g["loop_start"] + math.fmod(f - g["loop_start"], span)


def pose(skel: dict, anim: Optional[dict], frame: float) -> List[Mat]:
    tracks = {}
    if anim is not None:
        tracks = {t["bone_hash"]: t for t in anim["tracks"]}
    worlds: List[Mat] = []
    for n in skel["nodes"]:
        t, q, s = n["translation"], n["rotation"], n["scale"]
        tr = tracks.get(n["hash"])
        if tr is not None:
            rot, trans = anim_demaker.sample(anim, tr, frame)
            if rot is not None:
                q = rot
            if trans is not None:
                t = trans
        local = trs(t, q, s)
        worlds.append(local if n["parent"] < 0 else mmul(worlds[n["parent"]], local))
    return worlds


def node_worlds(dm: dict) -> List[Mat]:
    return pose(dm, None, 0.0)


def actor_batches(actor: dict, meshes: Sequence[dict], anim: Optional[dict], frame: float
                  ) -> Iterator[Tuple[dict, dict, Mat, bool]]:
    """Yield (mesh, batch, actor-space matrix, mirrored) for every drawable batch."""
    skel = meshes[actor["skeleton"]]
    worlds = pose(skel, anim, frame)
    by_hash = {n["hash"]: i for i, n in enumerate(skel["nodes"])}
    if not actor["parts"]:
        for b in skel["batches"]:
            yield skel, b, worlds[b["node"]], False
        return
    for mesh_index, bone_hash, mirrored in actor["parts"]:
        part = meshes[mesh_index]
        part_worlds = node_worlds(part)
        for b in part["batches"]:
            if b["flags"] & dmesh.BATCH_F_SEGMENT:
                idx = by_hash.get(part["nodes"][b["node"]]["hash"])
                if idx is None:
                    continue
                yield part, b, worlds[idx], False
            else:
                idx = by_hash.get(bone_hash)
                if idx is None:
                    continue
                m = worlds[idx]
                if mirrored:
                    m = mmul(m, MIRROR_X)
                yield part, b, mmul(m, part_worlds[b["node"]]), mirrored
