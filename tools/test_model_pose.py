# SPDX-License-Identifier: GPL-3.0-or-later
import math
import unittest

from model_pose import (compare_worlds, euler, fk_worlds, identity, inverse_rigid,
                        local_poses, multiply, quaternion, rotation, translation,
                        affine_local_poses, inverse_affine, pose_matrix)


class PoseTests(unittest.TestCase):
    def assertMatrixClose(self, actual, expected):
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b, places=12)

    def test_rigid_inverse_includes_rotated_translation(self):
        m = multiply(euler([0.3, 0.4, 0.8]), translation([7, -3, 2]))
        self.assertMatrixClose(multiply(m, inverse_rigid(m)), identity())
        self.assertMatrixClose(multiply(inverse_rigid(m), m), identity())

    def test_quaternion_rotation_and_half_turn_branches(self):
        for axis in range(3):
            for angle in [0.5, math.pi, -math.pi/2]:
                q = quaternion(rotation(axis, angle))
                wanted = [0, 0, 0, math.cos(angle/2)]
                wanted[axis] = math.sin(angle/2)
                if sum(a*b for a, b in zip(q, wanted)) < 0:
                    wanted = [-v for v in wanted]
                self.assertMatrixClose(q, wanted)

    def test_local_pose_removes_parent_motion(self):
        parent = multiply(rotation(2, 0.4), translation([4, 2, 1]))
        child = multiply(translation([0, 2, 0]), parent)
        poses = local_poses([parent, child], [{'parent': -1}, {'parent': 0}])
        self.assertMatrixClose(poses[1]['translation'], [0, 2, 0])
        self.assertMatrixClose(poses[1]['rotation'], [0, 0, 0, 1])

    def test_affine_local_scale_preserved_and_shear_residual_reported(self):
        parent = pose_matrix({'translation':[2,3,4], 'rotation':[0,0,0,1], 'scale':[2,3,4]})
        child = multiply(multiply(rotation(1,0.5),translation([0,2,0])),parent)
        poses,error = affine_local_poses([parent,child],[{'parent':-1},{'parent':0}])
        self.assertLess(error,1e-12)
        self.assertMatrixClose(poses[0]['scale'],[2,3,4])
        self.assertMatrixClose(poses[1]['translation'],[0,2,0])
        self.assertMatrixClose(multiply(parent,inverse_affine(parent)),identity())
        child[1] += 0.1
        _,error = affine_local_poses([parent,child],[{'parent':-1},{'parent':0}])
        self.assertGreater(error,0.01)

    def test_fk_primary_chain_and_exact_difference_list(self):
        joints = [{'parent': -1, 'translation': [0, 0, 0]} for _ in range(32)]
        joints[23] = {'parent': 2, 'translation': [0, 0.2, 0]}
        joints[0] = {'parent': 23, 'translation': [0, 0.3, 0]}
        channels = [0] * 60
        channels[1] = 1
        worlds = fk_worlds(channels, {'joints': joints}, [5, 0, 0])
        self.assertMatrixClose(worlds[0][12:15], [5, 1.5, 0])
        expected = [m.copy() for m in worlds]
        expected[7][12] += 0.01
        self.assertEqual(compare_worlds(worlds, expected)['differing_joints'], [7])
        expected[8][0] = float('nan')
        with self.assertRaises(ValueError):
            compare_worlds(worlds, expected)


if __name__ == '__main__':
    unittest.main()
