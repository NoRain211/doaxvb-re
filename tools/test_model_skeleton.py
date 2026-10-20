# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic solver fixtures; no archive, executable table or captured pose."""
import copy
import math
import unittest

from model_skeleton import f32, height_at, sine_cosine, solve_scene, solve_skeleton


def fixture():
    tables = {'offsets':[[0,0,0] for _ in range(24)],
              'alternate_offsets':[[0,0,0] for _ in range(24)], 'limbs':[]}
    targets = dict(position=[0,0,0], heading=0, ground_height=0,
                   hip_height=0, neck_height=0, limb_height=[0]*4, lengths=[],
                   target_offset=[0]*3, target_weight=0, upper_morph=0, lower_morph=0,
                   look_target=[0]*3, look_weight=0, eyes=[0]*4, target_mask=0,
                   minimum_bend=False, terrain_disabled=True, derived_blend=False)
    for i, (end, proximal, middle, tip) in enumerate(((3,5,4,6),(7,8,9,18),(12,11,10,13),(22,14,15,255))):
        tables['limbs'].append([end,2,proximal,middle,tip,12,13,14,0,0,0,15,16,0,
                               0,0,0,0,0,0,0,1,i%2,0])
        targets['lengths'].append(dict(proximal=1,distal=1,squared_difference=0,
                                      alternate_proximal=1.1,alternate_distal=1.1,
                                      alternate_squared_difference=0))
    channels = [0]*60
    channels[14], channels[15] = 1.5,-1.0
    return tables,channels,targets


class SkeletonTests(unittest.TestCase):
    def test_approximate_trig_retains_float_stores(self):
        sine,cosine = sine_cosine(f32(math.tau))
        self.assertEqual(sine,0)
        self.assertAlmostEqual(cosine,0.99984318,places=7)
        self.assertNotEqual(cosine,1)

    def test_reach_translation_repeatability_and_immutable_inputs(self):
        t,p,s = fixture()
        saved = copy.deepcopy((t,p,s))
        a = solve_skeleton(t,p,s)
        self.assertEqual((t,p,s),saved)
        self.assertEqual(a,solve_skeleton(t,p,s))
        self.assertAlmostEqual(a[3][14],1.5,delta=0.001)
        self.assertAlmostEqual(a[3][12],0,delta=0.001)
        s['position'][0] = 10
        b = solve_skeleton(t,p,s)
        self.assertAlmostEqual(b[3][12]-a[3][12],10,delta=1e-5)

    def test_active_frozen_branches_are_finite_and_change_pose(self):
        t,p,s = fixture()
        a = solve_skeleton(t,p,s)
        s.update(look_weight=1,look_target=[2,1,3],upper_morph=0.5,derived_blend=True,
                 terrain_disabled=False,neck_height=0.15)
        t['offsets'][19][2] = 0.3
        b = solve_skeleton(t,p,s)
        self.assertNotEqual(a,b)
        self.assertGreater(abs(b[2][9]),0.01)
        self.assertTrue(all(math.isfinite(v) for row in b for v in row))

    def test_invalid_descriptor_and_singular_limb_rejected(self):
        t,p,s = fixture()
        t['limbs'][0][7] = 60
        with self.assertRaises(ValueError):
            solve_skeleton(t,p,s)

        t,p,s = fixture()
        p[14] = 0
        with self.assertRaisesRegex(ValueError,'Singular'):
            solve_skeleton(t,p,s)
        p[14] = float('nan')
        with self.assertRaises(ValueError):
            solve_skeleton(t,p,s)

    def test_nonfinite_look_target_disables_correction(self):
        t,p,s = fixture()
        expected = solve_skeleton(t,p,s)
        s.update(look_weight=1,look_target=[float('nan'),0,0])
        self.assertEqual(solve_skeleton(t,p,s),expected)

    def test_bilinear_grid_flags_bounds_and_scene_query(self):
        grid = dict(xmin=0,xmax=1,zmin=0,zmax=1,cell=1,base=10,width=2,stride=2,
                    heights=[0xf000,1024,2048,3072])
        self.assertEqual(height_at(grid,0.5,0.5),11.5)
        self.assertEqual(height_at(grid,-1,0),10)
        with self.assertRaises(ValueError):
            height_at(grid,1,1)
        t,p,s = fixture()
        grid.update(xmin=-10,xmax=10,zmin=-10,zmax=10,cell=20,heights=[0]*4,base=0)
        before = copy.deepcopy(s)
        a,queried = solve_scene(t,p,s,grid)
        self.assertEqual(s,before)
        self.assertEqual(queried['limb_height'],[0]*4)
        self.assertEqual(a,solve_skeleton(t,p,s))


if __name__ == '__main__':
    unittest.main()
