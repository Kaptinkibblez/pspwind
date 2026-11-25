"""decimate.py: quadric edge collapse."""

import math
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import decimate  # noqa: E402
import nif_fixtures as F  # noqa: E402


def edges_with_counts(tris):
    counts = {}
    for a, b, c in tris:
        for u, v in ((a, b), (b, c), (c, a)):
            k = (min(u, v), max(u, v))
            counts[k] = counts.get(k, 0) + 1
    return counts


class DecimateTests(unittest.TestCase):
    def welded_sphere(self):
        verts, _n, _uv, tris = F.uv_sphere(50.0, 16, 32)
        keys = [tuple(round(c, 4) for c in v) for v in verts]
        rep, tris = decimate.weld(keys, tris)
        return [verts[i] for i in rep], tris

    def test_sphere_halves_and_stays_closed(self):
        pos, tris = self.welded_sphere()
        out = decimate.simplify(pos, tris, len(tris) // 2)
        self.assertLessEqual(len(out), len(tris) // 2 + 1)
        self.assertGreater(len(out), len(tris) // 3)
        # still a closed 2-manifold: every edge shared by exactly two faces
        self.assertTrue(all(c == 2 for c in edges_with_counts(out).values()))
        # no face flipped: outward normals stay outward
        for a, b, c in out:
            n, area = decimate.face_normal(pos[a], pos[b], pos[c])
            centroid = [(pos[a][i] + pos[b][i] + pos[c][i]) / 3 for i in range(3)]
            self.assertGreater(area, 0.0)
            self.assertGreater(sum(n[i] * centroid[i] for i in range(3)), 0.0)
        # surviving vertices are original vertices (half-edge collapse)
        used = {v for t in out for v in t}
        for v in used:
            self.assertAlmostEqual(math.sqrt(sum(c * c for c in pos[v])), 50.0, places=3)

    def test_flat_grid_keeps_border(self):
        verts, _n, _uv, tris = F.grid(16, 100.0)
        out = decimate.simplify(verts, tris, len(tris) // 4)
        self.assertLess(len(out), len(tris) // 2)
        used = {v for t in out for v in t}
        xs = [verts[v][0] for v in used]
        ys = [verts[v][1] for v in used]
        self.assertEqual((min(xs), max(xs), min(ys), max(ys)), (0.0, 100.0, 0.0, 100.0))
        # corners survive (silhouette preserved)
        corners = {(0.0, 0.0), (100.0, 0.0), (0.0, 100.0), (100.0, 100.0)}
        self.assertTrue(corners <= {(verts[v][0], verts[v][1]) for v in used})
        # surface area preserved (flat plane, border kept)
        area = sum(decimate.face_normal(verts[a], verts[b], verts[c])[1] for a, b, c in out)
        self.assertAlmostEqual(area, 100.0 * 100.0, places=3)
        for a, b, c in out:
            n, _ = decimate.face_normal(verts[a], verts[b], verts[c])
            self.assertGreater(n[2], 0.99)

    def test_locked_vertices_survive(self):
        verts, _n, _uv, tris = F.grid(10, 10.0)
        locked = {i for i, v in enumerate(verts) if abs(v[0] - 5.0) < 1e-6}
        out = decimate.simplify(verts, tris, 10, locked=locked)
        used = {v for t in out for v in t}
        self.assertTrue(locked <= used)

    def test_error_cap_stops_early(self):
        pos, tris = self.welded_sphere()
        out = decimate.simplify(pos, tris, 10, max_error=1e-6)
        self.assertGreater(len(out), len(tris) * 0.8)

    def test_weld_and_compact(self):
        keys = [(0, 0), (1, 0), (0, 0), (0, 1)]
        rep, tris = decimate.weld(keys, [(0, 1, 3), (2, 1, 3), (0, 2, 1)])
        self.assertEqual(rep, [0, 1, 3])
        self.assertEqual(tris, [(0, 1, 2), (0, 1, 2)])  # degenerate (0,2,1) dropped
        order, ctris = decimate.compact([(5, 7, 9)], 10)
        self.assertEqual(order, [5, 7, 9])
        self.assertEqual(ctris, [(0, 1, 2)])


if __name__ == "__main__":
    unittest.main()
