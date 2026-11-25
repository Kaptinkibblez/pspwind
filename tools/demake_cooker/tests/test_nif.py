"""nif.py: NetImmerse 4.0.0.2 parsing."""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import nif  # noqa: E402
import nif_fixtures as F  # noqa: E402


class HeaderTests(unittest.TestCase):
    def test_rejects_other_versions(self):
        with self.assertRaises(nif.NifError):
            nif.parse_nif(b"Gamebryo File Format, Version 20.0.0.5\n" + bytes(16))
        bad = bytearray(nif.write_nif([F.node("Root", [])], [0]))
        nl = bad.index(b"\n") + 1
        bad[nl:nl + 4] = (0x04000000).to_bytes(4, "little")
        with self.assertRaises(nif.NifError):
            nif.parse_nif(bytes(bad))

    def test_unknown_block_is_reported(self):
        def junk(w):
            w.u32(0)
        data = nif.write_nif([F.node("Root", [1]), ("NiMysteryBlock", junk)], [0])
        with self.assertRaisesRegex(nif.NifError, "NiMysteryBlock"):
            nif.parse_nif(data)

    def test_truncated_file(self):
        data = F.textured_static_nif()
        with self.assertRaises(nif.NifError):
            nif.parse_nif(data[:-40])


class BlockTests(unittest.TestCase):
    def test_static_scene(self):
        data = F.textured_static_nif(root_t=(1.0, 2.0, 3.0))
        n = nif.parse_nif(data)
        self.assertEqual([b.type for b in n.blocks],
                         ["NiNode", "NiTriShape", "NiTriShapeData", "NiTexturingProperty",
                          "NiMaterialProperty", "NiSourceTexture"])
        root = n.roots[0]
        self.assertEqual(root.name, "Root")
        self.assertEqual(root.translation, (1.0, 2.0, 3.0))
        self.assertEqual(root.children, (1,))
        shape = n.get(root.children[0])
        self.assertEqual(shape.properties, (3, 4))
        d = n.get(shape.data)
        self.assertEqual(d.num_vertices, 24)
        self.assertEqual(len(d.triangles), 12)
        self.assertEqual(len(d.uv_sets), 1)
        self.assertEqual(len(d.normals), 24)
        self.assertEqual(d.match_groups, [(0, 0)])
        tex = n.blocks[3]
        self.assertEqual(tex.texture_count, 7)
        self.assertEqual(tex.textures[0]["source"], 5)
        self.assertEqual(tex.textures[0]["ps2_k"], -75)
        self.assertTrue(all(t is None for t in tex.textures[1:]))
        self.assertEqual(n.blocks[5].filename, "Textures\\Tx_Wood_01.TGA")
        self.assertAlmostEqual(n.blocks[4].diffuse[0], 0.5)

    def test_bump_slot_extras_and_bounds(self):
        verts, normals, uvs, tris = F.cube()
        blocks = [F.node("Root", [1], bound=((0, 0, 0), (5, 5, 5))),
                  F.shape("s", 2, props=[3]),
                  F.shape_data(verts, tris, normals=normals, uvs=uvs),
                  F.texturing(4, with_bump=True), F.source_texture("a.tga")]
        n = nif.parse_nif(nif.write_nif(blocks, [0]))
        self.assertEqual(n.roots[0].bound["type"], "box")
        self.assertEqual(n.roots[0].bound["extent"], (5.0, 5.0, 5.0))
        self.assertEqual(n.blocks[3].textures[5]["luma"], (1.0, 0.0))

    def test_strips(self):
        verts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0), (0, 2, 0)]
        blocks = [F.node("Root", [1]), F.shape("s", 2, kind="NiTriStrips"),
                  F.strips_data(verts, [[0, 1, 2, 3, 4], [0, 0, 1]])]
        n = nif.parse_nif(nif.write_nif(blocks, [0]))
        self.assertEqual(n.blocks[2].strips, [(0, 1, 2, 3, 4), (0, 0, 1)])
        tris = nif.strips_to_triangles(n.blocks[2].strips)
        self.assertEqual(tris, [(0, 1, 2), (2, 1, 3), (2, 3, 4)])

    def test_skin_controllers_particles_and_misc(self):
        verts, normals, uvs, tris = F.cylinder()
        blocks = [
            F.node("Root", [1, 2, 7, 11, 15], extra=16),                 # 0
            F.node("Bip01", [], ctrl=8),                                  # 1
            F.shape("Tri Body", 3, skin=4),                               # 2
            F.shape_data(verts, tris, normals=normals, uvs=uvs),          # 3
            F.skin_instance(5, 1, [1]),                                   # 4
            F.skin_data([(F.I9, (0, 0, 0), 1.0, [(0, 1.0), (1, 0.5)])]),  # 5
            F.vis_data(),                                                 # 6
            F.node("Emitter", []),                                        # 7
            F.keyframe_controller(1, 9, next_ref=10),                     # 8
            F.keyframe_data(xyz=True),                                    # 9
            F.vis_controller(1, 6),                                       # 10
            F.shape("Particles", 12, kind="NiAutoNormalParticles", ctrl=13),  # 11
            F.particles_data(4),                                          # 12
            F.particle_controller(11, 7, 14),                             # 13
            F.grow_fade(-1, 13),                                          # 14
            F.point_light(),                                              # 15
            F.string_extra("MRK", next_ref=17),                           # 16
            F.text_keys([(0.0, "idle: start"), (1.0, "idle: stop")]),     # 17
            F.keyframe_data(xyz=False),                                   # 18
        ]
        n = nif.parse_nif(nif.write_nif(blocks, [0]))
        sd = n.blocks[5]
        self.assertEqual(sd.partition, -1)
        self.assertEqual(sd.bone_data[0]["weights"], [(0, 1.0), (1, 0.5)])
        self.assertEqual(n.blocks[4].bones, (1,))
        kd = n.blocks[9]
        self.assertEqual(kd.rotation_type, 4)
        self.assertEqual(len(kd.xyz_rotations), 3)
        self.assertEqual(kd.translations["interp"], 2)
        self.assertEqual(kd.translations["keys"][1][1], (0.0, 0.0, 10.0))
        kd2 = n.blocks[18]
        self.assertEqual(kd2.rotation_type, 3)
        self.assertEqual(len(kd2.rotation_keys), 2)
        self.assertEqual(n.blocks[6].keys, [(0.0, 1), (1.0, 0)])
        pc = n.blocks[13]
        self.assertEqual(pc.emitter, 7)
        self.assertEqual(pc.particle_modifier, 14)
        self.assertAlmostEqual(pc.birth_rate, 20.0)
        self.assertEqual(len(pc.particles), 2)
        self.assertEqual(n.blocks[12].num_particles, 4)
        self.assertEqual(len(n.blocks[12].sizes), 4)
        self.assertEqual(n.blocks[15].attenuation, (1.0, 0.0, 0.0))
        self.assertEqual(n.blocks[16].string, "MRK")
        self.assertEqual(n.blocks[17].keys[1], (1.0, "idle: stop"))

    def test_embedded_texture_and_switch(self):
        rgb = bytes([255, 0, 0] * 16)
        blocks = [F.switch_node("Root", [1, 2], 1), F.node("A", []), F.node("B", []),
                  F.internal_texture(4), F.pixel_data_rgb(4, 4, rgb)]
        n = nif.parse_nif(nif.write_nif(blocks, [0]))
        self.assertEqual(n.blocks[0].switch_index, 1)
        self.assertEqual(n.blocks[3].pixel_data, 4)
        self.assertEqual(n.blocks[4].mipmaps, [(4, 4, 0)])
        self.assertEqual(n.blocks[4].pixels, rgb)


if __name__ == "__main__":
    unittest.main()
