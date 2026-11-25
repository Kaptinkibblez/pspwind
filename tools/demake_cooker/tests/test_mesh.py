"""mesh_demaker: NIF -> DMESH conversion end to end."""

import json
import math
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import dmcommon  # noqa: E402
import dmesh  # noqa: E402
import mesh_demaker as M  # noqa: E402
import nif  # noqa: E402
import nif_fixtures as F  # noqa: E402
import tex_demaker  # noqa: E402


def cook(blocks, cfg=None, key="test/mesh"):
    data = nif.write_nif(blocks, [0])
    res = M.demake_nif(data, key, cfg or M.MeshConfig())
    return res, dmesh.read_dmesh(res.data)


def node_worlds(d):
    out = []
    for n in d["nodes"]:
        local = M.compose(n["translation"], n["rotation"], n["scale"])
        out.append(local if n["parent"] < 0 else M.m_mul(out[n["parent"]], local))
    return out


def model_space_vertices(d, batch):
    w = node_worlds(d)[batch["node"]]
    return [M.m_point(w, v["pos"]) for v in batch["vertices"]]


def close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


class StaticMeshTests(unittest.TestCase):
    def test_cube_roundtrip(self):
        res, d = cook([F.node("Root", [1], t=(100.0, 0.0, 0.0)),
                       F.shape("Tri Box", 2, props=[3, 4]),
                       F.shape_data(*self.cube_args()),
                       F.texturing(5, clamp=0), F.material(diffuse=(0.5, 0.25, 1.0)),
                       F.source_texture("Textures\\Tx_Crate.TGA")])
        self.assertEqual(len(d["nodes"]), 1)
        self.assertEqual(d["nodes"][0]["translation"], (100.0, 0.0, 0.0))
        self.assertEqual(len(d["batches"]), 1)
        b = d["batches"][0]
        self.assertEqual(len(b["indices"]), 36)  # 12 tris: below min_tris, untouched
        self.assertEqual(len(b["vertices"]), 24)
        self.assertEqual(d["textures"][0]["name"], "tx_crate")
        self.assertEqual(d["textures"][0]["hash"], dmcommon.fnv1a32("tx_crate"))
        self.assertTrue(b["flags"] & dmesh.BATCH_F_CLAMP_U and b["flags"] & dmesh.BATCH_F_CLAMP_V)
        pts = model_space_vertices(d, b)
        for p in pts:
            self.assertTrue(close([abs(p[0] - 100.0), abs(p[1]), abs(p[2])], [5, 5, 5], 0.01))
        self.assertTrue(close(d["bbox_min"], (95, -5, -5), 1e-3))
        self.assertTrue(close(d["bbox_max"], (105, 5, 5), 1e-3))
        self.assertAlmostEqual(d["bound_radius"], math.sqrt(75), places=3)
        # baked vertex colour = material diffuse
        self.assertTrue(close(b["vertices"][0]["color"], (0.5, 0.25, 1.0, 1.0), 0.01))
        # uv dequantisation precision
        uvs = sorted({tuple(round(c, 3) for c in v["uv"]) for v in b["vertices"]})
        self.assertEqual(uvs, [(0.0, 0.0), (0.0, 1.0), (1.0, 0.0), (1.0, 1.0)])
        # normals survive 8-bit quantisation
        for v in b["vertices"]:
            self.assertAlmostEqual(sum(c * c for c in v["normal"]), 1.0, delta=0.02)
        # collision falls back to the render geometry in model space
        coll = d["collision"]
        self.assertEqual(len(coll["triangles"]), 12)
        self.assertTrue(all(abs(abs(p[0] - 100.0) - 5.0) < 1e-4 or abs(abs(p[1]) - 5) < 1e-4
                            or abs(abs(p[2]) - 5) < 1e-4 for p in coll["positions"]))
        self.assertTrue(d["flags"] & dmesh.DMESH_F_COLLISION)

    @staticmethod
    def cube_args():
        verts, normals, uvs, tris = F.cube()
        return verts, tris, normals, uvs

    def test_nested_transforms_are_baked(self):
        verts, normals, uvs, tris = F.cube(2.0)
        res, d = cook([F.node("Root", [1]),
                       F.node("Child", [2], t=(0.0, 10.0, 0.0), r=F.rot_z(90), s=2.0),
                       F.shape("Tri", 3, t=(5.0, 0.0, 0.0)),
                       F.shape_data(verts, tris, normals=normals)])
        self.assertEqual(len(d["nodes"]), 1)  # static hierarchy collapses into the root
        pts = model_space_vertices(d, d["batches"][0])
        # cube centre: child(R90 * 2 * (5,0,0)) + (0,10,0) = (0, 20, 0); half extent 2
        cx = sum(p[0] for p in pts) / len(pts)
        cy = sum(p[1] for p in pts) / len(pts)
        self.assertAlmostEqual(cx, 0.0, places=2)
        self.assertAlmostEqual(cy, 20.0, places=2)
        self.assertAlmostEqual(max(p[2] for p in pts), 2.0, places=2)
        # normals were rotated with the geometry
        n = d["batches"][0]["vertices"][0]["normal"]
        self.assertAlmostEqual(math.sqrt(sum(c * c for c in n)), 1.0, delta=0.02)

    def test_decimation_hits_budget(self):
        verts, normals, uvs, tris = F.uv_sphere(50.0, 16, 32)
        res, d = cook([F.node("Root", [1]), F.shape("Tri", 2, props=[3]),
                       F.shape_data(verts, tris, normals=normals, uvs=uvs),
                       F.texturing(4), F.source_texture("tx_ball.dds")])
        info = res.info
        self.assertEqual(info["src_triangles"], len(tris))
        self.assertLessEqual(info["triangles"], len(tris) * 0.55)
        self.assertGreaterEqual(info["triangles"], len(tris) * 0.3)
        for p in model_space_vertices(d, d["batches"][0]):
            self.assertAlmostEqual(math.sqrt(sum(c * c for c in p)), 50.0, delta=0.05)
        res_full, d_full = cook([F.node("Root", [1]), F.shape("Tri", 2),
                                 F.shape_data(verts, tris, normals=normals)],
                                M.MeshConfig(ratio=1.0))
        self.assertEqual(res_full.info["triangles"], len(tris))

    def test_strips_switch_hidden_and_markers(self):
        verts = [(0, 0, 0), (10, 0, 0), (0, 10, 0), (10, 10, 0)]
        res, d = cook([F.node("Root", [1, 3, 5, 7], extra=9),
                       F.shape("Tri Strip", 2, kind="NiTriStrips"),           # 1
                       F.strips_data(verts, [[0, 1, 2, 3]]),                  # 2
                       F.shape("Tri Hidden", 4, flags=1),                     # 3
                       F.shape_data(*self.cube_args()),                       # 4
                       F.shape("Tri EditorMarker", 6),                        # 5
                       F.shape_data(*self.cube_args()),                       # 6
                       F.switch_node("Switch", [8, 4], 1),                    # 7 (child 4 is data!)
                       F.shape("Tri Off", 4),                                 # 8
                       F.string_extra("MRK")])                                # 9
        self.assertEqual(res.info["src_triangles"], 2)  # strip only; rest hidden/inactive
        self.assertEqual(d["total_triangles"], 2)

    def test_materials(self):
        verts, normals, uvs, tris = F.cube()
        additive = 1 | (6 << 1) | (0 << 5)   # blend, src SRCALPHA, dst ONE
        res, d = cook([F.node("Root", [1, 2, 3]),
                       F.shape("Glow", 4, props=[5, 6]),
                       F.shape("Leaves", 4, props=[7, 8]),
                       F.shape("Lit", 9, props=[10]),
                       F.shape_data(verts, tris, normals=normals),                    # 4
                       F.alpha_prop(additive),                                        # 5
                       F.material(emissive=(1.0, 0.5, 0.0)),                          # 6
                       F.alpha_prop(1 << 9, threshold=133),                           # 7
                       F.stencil(3),                                                  # 8
                       F.shape_data(verts, tris, normals=normals,
                                    colors=[(0.5, 0.5, 0.5, 0.25)] * len(verts)),     # 9
                       F.material(diffuse=(1.0, 0.0, 0.5), alpha=0.5)])               # 10
        by_flags = {b["flags"]: b for b in d["batches"]}
        glow = next(b for b in d["batches"] if b["flags"] & dmesh.BATCH_F_ADDITIVE)
        self.assertTrue(glow["flags"] & dmesh.BATCH_F_ALPHA_BLEND)
        self.assertTrue(glow["flags"] & dmesh.BATCH_F_EMISSIVE)
        self.assertEqual(glow["blend"], 6)
        self.assertTrue(close(glow["emissive"][:3], (1.0, 0.5, 0.0), 0.01))
        leaves = next(b for b in d["batches"] if b["flags"] & dmesh.BATCH_F_ALPHA_TEST)
        self.assertEqual(leaves["alpha_ref"], 133)
        self.assertTrue(leaves["flags"] & dmesh.BATCH_F_DOUBLE_SIDED)
        lit = next(b for b in d["batches"] if b["flags"] & dmesh.BATCH_F_VERTEX_ALPHA)
        # vertex colour * diffuse, alpha * material alpha
        self.assertTrue(close(lit["vertices"][0]["color"], (0.5, 0.0, 0.25, 0.125), 0.01))
        # draw order: opaque, alpha test, alpha blend
        order = [0 if not b["flags"] & 3 else (1 if b["flags"] & 2 and not b["flags"] & 1 else 2)
                 for b in d["batches"]]
        self.assertEqual(order, sorted(order))
        self.assertEqual(len(by_flags), 3)


class HierarchyTests(unittest.TestCase):
    def test_animated_node_is_kept(self):
        verts, normals, uvs, tris = F.cube(4.0)
        res, d = cook([F.node("Root", [1, 5]),
                       F.node("Wheel", [2], t=(0.0, 0.0, 30.0), r=F.rot_z(45), ctrl=3),  # 1
                       F.shape("Tri Wheel", 6),                                        # 2
                       F.keyframe_controller(1, 4),                                    # 3
                       F.keyframe_data(),                                              # 4
                       F.shape("Tri Base", 6),                                         # 5
                       F.shape_data(verts, tris, normals=normals)])                    # 6
        self.assertTrue(d["flags"] & dmesh.DMESH_F_ANIMATED)
        names = [n["name"] for n in d["nodes"]]
        self.assertEqual(names, ["Root", "Wheel"])
        self.assertTrue(d["nodes"][1]["flags"] & dmesh.NODE_F_ANIMATED)
        self.assertEqual(d["nodes"][1]["parent"], 0)
        wheel = next(b for b in d["batches"] if b["node"] == 1)
        base = next(b for b in d["batches"] if b["node"] == 0)
        # wheel vertices are node-local: a unit cube around the origin
        self.assertTrue(all(close([abs(c) for c in v["pos"]], [2, 2, 2], 0.01)
                            for v in wheel["vertices"]))
        world = model_space_vertices(d, wheel)
        self.assertAlmostEqual(sum(p[2] for p in world) / len(world), 30.0, places=2)
        self.assertAlmostEqual(sum(p[2] for p in model_space_vertices(d, base)) / 24, 0.0, places=2)

    def test_collision_node_and_no_collision_flag(self):
        sphere = F.uv_sphere(20.0, 8, 16)
        verts, normals, uvs, tris = F.cube(100.0)
        res, d = cook([F.node("Root", [1, 3]),
                       F.shape("Tri Sphere", 2),
                       F.shape_data(sphere[0], sphere[3], normals=sphere[1]),
                       F.node("Collision", [4], kind="RootCollisionNode"),
                       F.shape("Tri Coll", 5, flags=1),
                       F.shape_data(verts, tris)])
        self.assertEqual(len(d["collision"]["triangles"]), 12)
        span = [max(p[i] for p in d["collision"]["positions"]) for i in range(3)]
        self.assertTrue(close(span, (50, 50, 50), 1e-3))
        self.assertTrue(all(math.sqrt(sum(c * c for c in p)) < 20.1
                            for p in model_space_vertices(d, d["batches"][0])))
        self.assertNotIn("Collision", [n["name"] for n in d["nodes"]])

        res, d = cook([F.node("Root", [1], extra=3), F.shape("Tri", 2),
                       F.shape_data(verts, tris), F.string_extra("NCO")])
        self.assertIsNone(d["collision"])
        self.assertFalse(d["flags"] & dmesh.DMESH_F_COLLISION)

    def test_particles(self):
        res, d = cook([F.node("Root", [1, 2], props=[7]),
                       F.node("Emitter", [], t=(0.0, 0.0, 12.0)),                 # 1
                       F.shape("Particles", 3, kind="NiAutoNormalParticles", ctrl=4),  # 2
                       F.particles_data(16),                                         # 3
                       F.particle_controller(2, 1, 5),                               # 4
                       F.gravity(6, 4),                                              # 5
                       F.grow_fade(-1, 4),                                           # 6
                       F.texturing(8),                                               # 7
                       F.source_texture("Textures\\tx_spark.tga")])                  # 8
        self.assertEqual(len(d["emitters"]), 1)
        e = d["emitters"][0]
        self.assertEqual(d["nodes"][e["node"]]["name"], "Emitter")
        self.assertTrue(d["nodes"][e["node"]]["flags"] & dmesh.NODE_F_EMITTER)
        self.assertEqual(e["max_particles"], 16)
        self.assertAlmostEqual(e["emit_rate"], 20.0)
        self.assertAlmostEqual(e["lifetime"], 1.5)
        self.assertTrue(close(e["gravity"], (0.0, 0.0, -4.0), 1e-5))
        self.assertAlmostEqual(e["grow_time"], 0.2, places=5)
        self.assertAlmostEqual(e["fade_time"], 0.4, places=5)
        self.assertEqual(d["textures"][e["texture"]]["name"], "tx_spark")
        self.assertTrue(close(e["color"], (1.0, 0.5, 0.25, 1.0), 0.01))
        self.assertTrue(d["flags"] & dmesh.DMESH_F_PARTICLES)

    def test_embedded_texture(self):
        verts, normals, uvs, tris = F.cube()
        res, d = cook([F.node("Root", [1]), F.shape("Tri", 2, props=[3]),
                       F.shape_data(verts, tris, normals=normals, uvs=uvs),
                       F.texturing(4), F.internal_texture(5),
                       F.pixel_data_rgb(2, 2, bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9]))],
                      key="x/embedded")
        name = d["textures"][0]["name"]
        self.assertEqual(name, "embedded/x_embedded_4")
        self.assertIn(name, res.embedded)
        self.assertTrue(res.embedded[name].startswith(b"\x89PNG"))


class SkinnedTests(unittest.TestCase):
    def build(self, cfg=None):
        verts, normals, uvs, tris = F.cylinder(5.0, 40.0, rings=8, segments=8)
        w0 = [(i, 1.0) for i, v in enumerate(verts) if v[2] < 20.0 - 1e-6]
        w1 = [(i, 1.0) for i, v in enumerate(verts) if v[2] > 20.0 + 1e-6]
        mid = [i for i, v in enumerate(verts) if abs(v[2] - 20.0) < 1e-6]
        w0 += [(i, 0.5) for i in mid]
        w1 += [(i, 0.5) for i in mid]
        blocks = [
            F.node("Root", [1, 3]),                                      # 0
            F.node("Bip01 Thigh", [2]),                                  # 1
            F.node("Bip01 Calf", [], t=(0.0, 0.0, 20.0)),                # 2
            F.shape("Tri Leg", 4, props=[7, 8], skin=5),                 # 3
            F.shape_data(verts, tris, normals=normals, uvs=uvs),         # 4
            F.skin_instance(6, 1, [1, 2]),                               # 5
            F.skin_data([(F.I9, (0.0, 0.0, 0.0), 1.0, w0),
                         (F.I9, (0.0, 0.0, -20.0), 1.0, w1)]),          # 6
            F.texturing(9),                                              # 7
            F.material(),                                                # 8
            F.source_texture("Textures\\tx_skin.dds"),                   # 9
        ]
        return cook(blocks, cfg, key="b/b_n_test_m_skins"), tris

    def test_rigid_limb_segments(self):
        (res, d), tris = self.build(M.MeshConfig(ratio=1.0))
        self.assertTrue(d["flags"] & dmesh.DMESH_F_SKELETAL)
        self.assertIsNone(d["collision"])
        names = [n["name"] for n in d["nodes"]]
        self.assertEqual(names, ["Root", "Bip01 Thigh", "Bip01 Calf"])
        self.assertTrue(d["nodes"][1]["flags"] & dmesh.NODE_F_BONE)
        self.assertTrue(d["nodes"][2]["flags"] & dmesh.NODE_F_BONE)
        segs = {b["node"]: b for b in d["batches"]}
        self.assertEqual(set(segs), {1, 2})
        for b in d["batches"]:
            self.assertTrue(b["flags"] & dmesh.BATCH_F_SEGMENT)
        self.assertEqual(sum(len(b["indices"]) // 3 for b in d["batches"]), len(tris))
        # calf segment is stored in calf space (z from 0..20 instead of 20..40)
        calf_z = [v["pos"][2] for v in segs[2]["vertices"]]
        self.assertGreaterEqual(min(calf_z), -0.01)
        self.assertLessEqual(max(calf_z), 20.01)
        # rebuilt with bind pose bone matrices the limb is exactly the source tube
        for b in d["batches"]:
            for p in model_space_vertices(d, b):
                self.assertAlmostEqual(math.hypot(p[0], p[1]), 5.0, delta=0.01)
                self.assertTrue(-0.01 <= p[2] <= 40.01)
        thigh_world = model_space_vertices(d, segs[1])
        self.assertLessEqual(max(p[2] for p in thigh_world), 25.01)
        self.assertEqual(d["textures"][0]["name"], "tx_skin")

    def test_segments_are_decimated(self):
        (res, d), tris = self.build()
        self.assertLess(res.info["triangles"], len(tris))
        for b in d["batches"]:
            for p in model_space_vertices(d, b):
                self.assertAlmostEqual(math.hypot(p[0], p[1]), 5.0, delta=0.01)


class SplitTests(unittest.TestCase):
    def test_split_chunks_respects_limit(self):
        src = list(range(30))
        tris = [(i, i + 1, i + 2) for i in range(28)]
        chunks = list(M.split_chunks(src, tris, 7))
        self.assertGreater(len(chunks), 1)
        self.assertEqual(sum(len(t) for _s, t in chunks), 28)
        for csrc, ctris in chunks:
            self.assertLessEqual(len(csrc), 7)
            for t in ctris:
                self.assertTrue(all(0 <= i < len(csrc) for i in t))

    def test_matrix_helpers(self):
        m = M.m_from_nif((1.0, 2.0, 3.0), F.rot_z(30), 2.0)
        inv = M.m_inv(m)
        ident = M.m_mul(m, inv)
        for i in range(4):
            for j in range(4):
                self.assertAlmostEqual(ident[i][j], 1.0 if i == j else 0.0, places=9)
        t, q, s = M.decompose(m)
        back = M.compose(t, q, s)
        for i in range(3):
            for j in range(4):
                self.assertAlmostEqual(back[i][j], m[i][j], places=9)
        self.assertAlmostEqual(s, 2.0)


class CliTests(unittest.TestCase):
    def test_cook_meshes_then_used_textures(self):
        from PIL import Image
        import io
        with tempfile.TemporaryDirectory() as d:
            buf = io.BytesIO()
            Image.new("RGB", (128, 128), (120, 60, 30)).save(buf, "DDS")
            dmcommon.write_bsa(os.path.join(d, "Morrowind.bsa"), {
                "meshes\\f\\furn_crate.nif": F.textured_static_nif("Textures\\Tx_Crate.TGA"),
                "meshes\\x\\broken.nif": b"NetImmerse File Format, Version 4.0.0.2\n\x02\x00\x00\x04",
                "textures\\tx_crate.dds": buf.getvalue(),
                "textures\\tx_unused.dds": buf.getvalue(),
            })
            out = os.path.join(d, "out")
            rc = M.main(["--data", d, "--out", out, "--jobs", "2"])
            self.assertEqual(rc, 0)
            pack = dmcommon.DPakReader(dmcommon.read_file(os.path.join(out, "meshes.dpak")))
            self.assertEqual(set(pack.entries), {"f/furn_crate"})
            dm = dmesh.read_dmesh(pack.get("f/furn_crate"))
            self.assertEqual(dm["textures"][0]["name"], "tx_crate")
            with open(os.path.join(out, "meshes_manifest.json"), encoding="utf-8") as f:
                manifest = json.load(f)
            self.assertEqual(manifest["textures"], ["tx_crate"])
            self.assertEqual([x["key"] for x in manifest["failures"]], ["x/broken"])
            self.assertEqual(M.main(["--data", d, "--out", out, "--jobs", "1", "--strict"]), 1)
            # the texture cooker can be limited to what the meshes reference
            rc = tex_demaker.main(["--data", d, "--out", out, "--jobs", "1", "--only-keys",
                                   os.path.join(out, "meshes_manifest.json")])
            self.assertEqual(rc, 0)
            tpack = dmcommon.DPakReader(dmcommon.read_file(os.path.join(out, "textures.dpak")))
            self.assertEqual(set(tpack.entries), {"tx_crate"})


if __name__ == "__main__":
    unittest.main()
