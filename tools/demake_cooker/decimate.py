"""Edge-collapse mesh decimation for the PS1-style polygon budget.

Garland-Heckbert quadric error metrics driving *half-edge* collapses: a
vertex u is merged into a neighbour v that keeps its exact position and
attributes. No new vertices are invented, so UVs, normals and vertex colours
stay correct without interpolation - ideal for crunchy low-poly output.

Guards that keep the result watertight and sane:
  * locked vertices never move (attribute seams, shared shape borders,
    non-manifold fans) - collapsing *into* them is allowed
  * open-boundary vertices only slide along boundary edges, and boundary
    edges carry heavy perpendicular-plane quadrics to keep silhouettes
  * the link condition rejects collapses that would create non-manifold
    topology
  * a fold-over test rejects collapses that flip or degenerate any face
  * an absolute error cap stops before visible damage on small meshes
"""

from __future__ import annotations

import heapq
import math
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

Vec3 = Tuple[float, float, float]
Tri = Tuple[int, int, int]

BOUNDARY_WEIGHT = 50.0
FOLD_DOT_MIN = 0.2


def _sub(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a: Vec3, b: Vec3) -> Vec3:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a: Vec3, b: Vec3) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _norm(a: Vec3) -> float:
    return math.sqrt(_dot(a, a))


def face_normal(p0: Vec3, p1: Vec3, p2: Vec3) -> Tuple[Vec3, float]:
    """Unit normal and area (twice the area -> /2)."""
    n = _cross(_sub(p1, p0), _sub(p2, p0))
    ln = _norm(n)
    if ln <= 1e-20:
        return (0.0, 0.0, 0.0), 0.0
    return (n[0] / ln, n[1] / ln, n[2] / ln), ln * 0.5


def _plane_quadric(n: Vec3, d: float, w: float) -> List[float]:
    a, b, c = n
    return [w * a * a, w * a * b, w * a * c, w * a * d,
            w * b * b, w * b * c, w * b * d,
            w * c * c, w * c * d,
            w * d * d]


def _q_add(q: List[float], r: Sequence[float]) -> None:
    for i in range(10):
        q[i] += r[i]


def _q_eval(q: Sequence[float], p: Vec3) -> float:
    x, y, z = p
    return (q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x
            + q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y
            + q[7] * z * z + 2 * q[8] * z
            + q[9])


def simplify(positions: Sequence[Vec3], triangles: Sequence[Tri], target: int,
             locked: Optional[Set[int]] = None, max_error: float = float("inf")
             ) -> List[Tri]:
    """Return a reduced triangle list (indices into `positions`).

    target    - desired triangle count (stops earlier if no valid collapse)
    locked    - vertex indices that must not move
    max_error - largest accepted quadric error (squared distance units)
    """
    locked = set(locked or ())
    faces: List[Optional[Tri]] = []
    for t in triangles:
        if t[0] != t[1] and t[1] != t[2] and t[0] != t[2]:
            faces.append(tuple(t))  # type: ignore[arg-type]
    live = len(faces)
    if live <= target:
        return [f for f in faces if f is not None]

    nverts = len(positions)
    vfaces: List[Set[int]] = [set() for _ in range(nverts)]
    for fi, f in enumerate(faces):
        for v in f:  # type: ignore[union-attr]
            vfaces[v].add(fi)

    # edge -> face count; non-manifold edges lock their endpoints
    edge_faces: Dict[Tuple[int, int], int] = {}
    for f in faces:
        a, b, c = f  # type: ignore[misc]
        for e in ((a, b), (b, c), (c, a)):
            k = (e[0], e[1]) if e[0] < e[1] else (e[1], e[0])
            edge_faces[k] = edge_faces.get(k, 0) + 1
    for (a, b), n in edge_faces.items():
        if n > 2:
            locked.add(a)
            locked.add(b)

    quad = [[0.0] * 10 for _ in range(nverts)]
    for f in faces:
        a, b, c = f  # type: ignore[misc]
        n, area = face_normal(positions[a], positions[b], positions[c])
        if area <= 0.0:
            continue
        d = -_dot(n, positions[a])
        q = _plane_quadric(n, d, 1.0)
        for v in (a, b, c):
            _q_add(quad[v], q)

    boundary_vert = [False] * nverts
    for f in faces:
        a, b, c = f  # type: ignore[misc]
        fn, area = face_normal(positions[a], positions[b], positions[c])
        for u, v in ((a, b), (b, c), (c, a)):
            k = (u, v) if u < v else (v, u)
            if edge_faces.get(k) != 1:
                continue
            boundary_vert[u] = boundary_vert[v] = True
            if area <= 0.0:
                continue
            e = _sub(positions[v], positions[u])
            pn = _cross(e, fn)
            ln = _norm(pn)
            if ln <= 1e-20:
                continue
            pn = (pn[0] / ln, pn[1] / ln, pn[2] / ln)
            d = -_dot(pn, positions[u])
            q = _plane_quadric(pn, d, BOUNDARY_WEIGHT * _dot(e, e))
            _q_add(quad[u], q)
            _q_add(quad[v], q)

    removed = [False] * nverts
    version = [0] * nverts
    heap: List[Tuple[float, int, int, int, int]] = []

    def neighbours(v: int) -> Set[int]:
        out: Set[int] = set()
        for fi in vfaces[v]:
            out.update(faces[fi])  # type: ignore[arg-type]
        out.discard(v)
        return out

    def push(u: int, v: int) -> None:
        if u in locked:
            return
        q = [quad[u][i] + quad[v][i] for i in range(10)]
        cost = _q_eval(q, positions[v])
        if cost < 0.0:
            cost = 0.0
        if cost <= max_error:
            heapq.heappush(heap, (cost, u, v, version[u], version[v]))

    for (a, b) in edge_faces:
        push(a, b)
        push(b, a)

    def shared_faces(u: int, v: int) -> Set[int]:
        return vfaces[u] & vfaces[v]

    def can_collapse(u: int, v: int) -> bool:
        shared = shared_faces(u, v)
        if not shared:
            return False
        if boundary_vert[u] and len(shared) != 1:
            return False  # boundary vertices slide only along the boundary
        if boundary_vert[u] and not boundary_vert[v]:
            return False
        common = neighbours(u) & neighbours(v)
        if len(common) != len(shared):
            return False  # link condition
        pv = positions[v]
        for fi in vfaces[u]:
            if fi in shared:
                continue
            f = faces[fi]
            pts_old = [positions[x] for x in f]  # type: ignore[union-attr]
            pts_new = [pv if x == u else positions[x] for x in f]  # type: ignore[union-attr]
            n_old, a_old = face_normal(*pts_old)
            n_new, a_new = face_normal(*pts_new)
            if a_new <= a_old * 1e-4 or a_new <= 1e-12:
                return False
            if _dot(n_old, n_new) < FOLD_DOT_MIN:
                return False
        return True

    while live > target and heap:
        cost, u, v, vu, vv = heapq.heappop(heap)
        if removed[u] or removed[v] or version[u] != vu or version[v] != vv:
            continue
        if not can_collapse(u, v):
            continue
        shared = shared_faces(u, v)
        for fi in shared:
            f = faces[fi]
            for x in f:  # type: ignore[union-attr]
                vfaces[x].discard(fi)
            faces[fi] = None
            live -= 1
        for fi in list(vfaces[u]):
            f = faces[fi]
            faces[fi] = tuple(v if x == u else x for x in f)  # type: ignore[union-attr]
            vfaces[v].add(fi)
        vfaces[u].clear()
        removed[u] = True
        # Only costs of collapses touching v changed (its quadric grew); the
        # version bump invalidates their stale heap entries. Geometric
        # validity of other edges is re-checked when they are popped.
        _q_add(quad[v], quad[u])
        version[v] += 1
        for w in neighbours(v):
            push(v, w)
            push(w, v)
    return [f for f in faces if f is not None]


def compact(triangles: Sequence[Tri], nverts: int) -> Tuple[List[int], List[Tri]]:
    """Drop unreferenced vertices: returns (old index per new vertex, remapped tris)."""
    remap: Dict[int, int] = {}
    order: List[int] = []
    out: List[Tri] = []
    for t in triangles:
        nt = []
        for v in t:
            if v not in remap:
                remap[v] = len(order)
                order.append(v)
            nt.append(remap[v])
        out.append((nt[0], nt[1], nt[2]))
    return order, out


def weld(keys: Sequence[tuple], triangles: Iterable[Tri]) -> Tuple[List[int], List[Tri]]:
    """Merge vertices with identical (already quantised) keys.
    Returns (representative old index per new vertex, remapped non-degenerate tris)."""
    first: Dict[tuple, int] = {}
    rep: List[int] = []
    mapping: List[int] = []
    for i, k in enumerate(keys):
        j = first.get(k)
        if j is None:
            j = len(rep)
            first[k] = j
            rep.append(i)
        mapping.append(j)
    tris = []
    for a, b, c in triangles:
        ta, tb, tc = mapping[a], mapping[b], mapping[c]
        if ta != tb and tb != tc and ta != tc:
            tris.append((ta, tb, tc))
    return rep, tris
