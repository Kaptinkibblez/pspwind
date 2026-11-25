"""anim_demaker.py + actorpose.py + actor assembly in world_demaker."""

import math
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import actorpose  # noqa: E402
import anim_demaker as A  # noqa: E402
import dmcommon  # noqa: E402
import dzone  # noqa: E402
import make_testworld  # noqa: E402
import synth as S  # noqa: E402

X = (1.0, 0.0, 0.0)
Z = (0.0, 0.0, 1.0)


def simple_kf(xyz=False):
    if xyz:
        rot = S.keyframe_track_data([], [], xyz=([(0.0, 0.0), (1.0, 0.0)],
                                                  [(0.0, 0.0), (1.0, 0.0)],
                                                  [(0.0, 0.0), (1.0, math.pi / 2)]))
    else:
        rot = S.keyframe_track_data([(0.0, S.quat_axis(Z, 0.0)), (1.0, S.quat_axis(Z, math.pi / 2))], [])
    root = S.keyframe_track_data([], [(0.0, (0.0, 0.0, 0.0)), (1.0, (0.0, 0.0, 0.0)),
                                      (2.0, (0.0, 200.0, 10.0))], trans_interp=1)
    still = S.keyframe_track_data([(0.0, S.quat_axis(X, 0.3))], [(0.0, (1.0, 2.0, 3.0))])
    keys = [(0.0, "Idle: Start\r\nIdle: Loop Start"), (0.6, "Idle: Loop Stop"),
            (1.0, "Idle: Stop\r\nWalkForward: Start"), (1.5, "SoundGen: Left"),
            (2.0, "WalkForward: Stop"), (2.0, "Bogus")]
    return S.kf_file(keys, [("Bip01 Spin", rot), ("Bip01", root), ("Bip01 Still", still)])


class KfTests(unittest.TestCase):
    def test_parse_pairs_names_with_controllers(self):
        keys, tracks = A.parse_kf(simple_kf())
        self.assertEqual([n for n, _ in tracks], ["Bip01 Spin", "Bip01", "Bip01 Still"])
        groups = A.parse_groups(keys)
        self.assertEqual(set(groups), {"idle", "walkforward"})
        self.assertAlmostEqual(groups["idle"]["loop stop"], 0.6, places=5)

    def test_cook_and_sample(self):
        data, info = A.cook_kf(simple_kf(), "test", fps=10.0)
        anim = A.read_danim(data)
        self.assertEqual(anim["frames"], 21)
        self.assertEqual(info["groups"], 2)
        g = anim["groups"]["idle"]
        self.assertEqual((g["start"], g["stop"], g["loop_start"], g["loop_stop"]), (0, 10, 0, 6))
        walk = anim["groups"]["walkforward"]
        self.assertAlmostEqual(walk["speed"], 200.0, places=3)
        tracks = {t["bone_hash"]: t for t in anim["tracks"]}
        spin = tracks[dmcommon.fnv1a32("Bip01 Spin")]
        self.assertTrue(spin["flags"] & A.T_ROT_ANIMATED)
        rot, trans = A.sample(anim, spin, 5.0)  # t = 0.5 s: 45 degrees about Z
        self.assertIsNone(trans)
        self.assertAlmostEqual(rot[2], math.sin(math.pi / 8), places=3)
        self.assertAlmostEqual(rot[3], math.cos(math.pi / 8), places=3)
        still = tracks[dmcommon.fnv1a32("Bip01 Still")]
        self.assertFalse(still["flags"] & (A.T_ROT_ANIMATED | A.T_TRANS_ANIMATED))
        self.assertEqual(len(still["rot"]), 1)
        root = tracks[dmcommon.fnv1a32("Bip01")]
        _r, t15 = A.sample(anim, root, 15.0)
        self.assertAlmostEqual(t15[1], 0.0, places=3)  # horizontal travel stripped
        self.assertAlmostEqual(t15[2], 5.0, places=3)  # vertical bob kept

    def test_xyz_euler_matches_quaternion(self):
        a = A.read_danim(A.cook_kf(simple_kf(xyz=False), "q", 10.0)[0])
        b = A.read_danim(A.cook_kf(simple_kf(xyz=True), "e", 10.0)[0])
        h = dmcommon.fnv1a32("Bip01 Spin")
        ta = next(t for t in a["tracks"] if t["bone_hash"] == h)
        tb = next(t for t in b["tracks"] if t["bone_hash"] == h)
        for f in (0.0, 3.0, 10.0):
            qa, _ = A.sample(a, ta, f)
            qb, _ = A.sample(b, tb, f)
            self.assertLess(min(sum((x - y) ** 2 for x, y in zip(qa, qb)),
                                sum((x + y) ** 2 for x, y in zip(qa, qb))), 1e-5)

    def test_xyz_order_is_x_then_y_then_z(self):
        # rotate +90 about X then +90 about Z: (0, 1, 0) -> (0, 0, 1) -> stays (0, 0, 1)
        q = A.q_norm(A.q_mul(A.q_axis(2, math.pi / 2), A.q_mul(A.q_axis(1, 0.0), A.q_axis(0, math.pi / 2))))
        m = actorpose.trs((0, 0, 0), q, 1.0)
        v = [m[i][1] for i in range(3)]
        self.assertAlmostEqual(v[2], 1.0, places=6)

    def test_group_frame_loops(self):
        anim = A.read_danim(A.cook_kf(simple_kf(), "t", 10.0)[0])
        self.assertAlmostEqual(actorpose.group_frame(anim, "idle", 0.3), 3.0)
        self.assertAlmostEqual(actorpose.group_frame(anim, "idle", 0.8), 2.0)  # wrapped in 0..6


class ActorWorldTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import cook_all
        cls.tmp = tempfile.TemporaryDirectory()
        data = make_testworld.make(cls.tmp.name)
        cls.out = os.path.join(cls.tmp.name, "cooked")
        cook_all.main(["--data", data, "--out", cls.out, "--jobs", "1"])
        cls.zones = dmcommon.DPakReader(dmcommon.read_file(os.path.join(cls.out, "zones.dpak")))
        cls.meshes = dmcommon.DPakReader(dmcommon.read_file(os.path.join(cls.out, "meshes.dpak")))
        cls.anims = dmcommon.DPakReader(dmcommon.read_file(os.path.join(cls.out, "anims.dpak")))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_npc_assembly(self):
        z = dzone.read_zone(self.zones.get("zone/ext_0_0"))
        self.assertEqual(len(z["actors"]), 2)
        names = [self.meshes.find_hash(h) for h in z["meshes"]]
        self.assertEqual([names[a["skeleton"]] for a in z["actors"]], ["base_anim", "base_anim"])
        self.assertEqual(self.anims.find_hash(z["anims"][0]), "xbase_anim")
        male, female = sorted(z["actors"], key=lambda a: a["flags"])
        self.assertEqual(female["flags"], dzone.ACTOR_F_FEMALE)
        chest = lambda a: [names[m] for m, b, _ in a["parts"] if b == 0]
        self.assertEqual(chest(male), ["tw/b_folk_chest"])
        self.assertEqual(chest(female), ["tw/b_folk_chest_f"])
        hands = [(b, mir) for m, b, mir in male["parts"] if names[m] == "tw/b_folk_hand"]
        self.assertEqual(hands, [(dmcommon.fnv1a32("Right Hand"), False),
                                 (dmcommon.fnv1a32("Left Hand"), True)])
        for a in z["actors"]:
            ins = z["instances"][a["instance"]]
            self.assertEqual(ins["kind"], dzone.INST_NPC)
            self.assertTrue(ins["flags"] & dzone.IF_ACTOR)

    def test_posed_npc_stands_on_its_feet(self):
        import world_demaker as W
        z = dzone.read_zone(self.zones.get("zone/ext_0_0"))
        meshes = [W.MeshLib(self.meshes).get(self.meshes.find_hash(h)) for h in z["meshes"]]
        anim = A.read_danim(self.anims.get("xbase_anim"))
        act = z["actors"][0]
        zs = []
        for dm, b, m, _mir in actorpose.actor_batches(act, meshes, anim, 0.0):
            for v in b["vertices"]:
                p = v["pos"]
                zs.append(m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3])
        self.assertAlmostEqual(min(zs), 0.0, delta=1.0)   # feet on the ground
        self.assertAlmostEqual(max(zs), 158.0, delta=2.0)  # top of the head

    def test_creature_uses_x_variant(self):
        z = dzone.read_zone(self.zones.get("zone/int/test hamlet, cellar"))
        self.assertEqual(len(z["actors"]), 1)
        a = z["actors"][0]
        self.assertEqual(self.meshes.find_hash(z["meshes"][a["skeleton"]]), "tw/xcritter")
        self.assertEqual(self.anims.find_hash(z["anims"][a["anim"]]), "tw/xcritter")
        self.assertEqual(a["flags"], dzone.ACTOR_F_CREATURE)
        self.assertEqual(a["parts"], [])


if __name__ == "__main__":
    unittest.main()
