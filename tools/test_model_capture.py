# SPDX-License-Identifier: GPL-3.0-or-later
import struct
import unittest
from pathlib import Path

from model_capture import RAM_SIZE, debugger_scripts, snapshot_summary


class CaptureTests(unittest.TestCase):
    def test_breakpoints_pair_actor_and_frame_without_guest_writes(self):
        scripts = debugger_scripts(Path('private/test-capture'), 2, 900)
        self.assertIn('>= 384', scripts['start.cdb'])
        self.assertIn('== 2)', scripts['start.cdb'])
        self.assertNotIn('&&', scripts['start.cdb'])
        self.assertNotIn('\\', scripts['start.cdb'])
        self.assertIn('== @$t3', scripts['pose-return.cdb'])
        self.assertIn('== 4d4650', scripts['pose-return.cdb'])
        self.assertIn('poi(@rsp)', scripts['pose.cdb'])
        self.assertIn('L?4000000', scripts['pose.cdb'])
        self.assertTrue(scripts['palette-return.cdb'].endswith('\nq'))
        with self.assertRaises(ValueError):
            debugger_scripts(Path('private/bad;command'), 0, 1)
        with self.assertRaises(ValueError):
            debugger_scripts(Path('private/test'), 4, 1)

    def test_summary_uses_selected_actor_and_keeps_before_after_separate(self):
        before = bytearray(RAM_SIZE)
        actor = 1
        pose, rig = 0x1000, 0x2000
        struct.pack_into('<I', before, 0x5e06a0 + actor*0x5c, pose)
        struct.pack_into('<I', before, 0x5e5ff8 + actor*0x180, rig)
        struct.pack_into('<f', before, pose + 12, 0.5)
        struct.pack_into('<f', before, rig, 0.25)
        struct.pack_into('<H', before, 0x5e0800 + actor*0x100 + 0x62, 17)
        after = before.copy()
        struct.pack_into('<f', after, pose + 12, 0.75)
        struct.pack_into('<f', after, 0x4d3650 + actor*0x800, 1)
        registers = struct.pack('<8I', 3, 0, 0, 0, 0, rig, 0, 0)
        result = snapshot_summary(before, after, actor, registers, bytes(256))
        self.assertEqual(result['channels'][0], 0.5)
        self.assertEqual(result['after_channels'][0], 0.75)
        self.assertEqual(result['bone_worlds'][0][0], 1)
        self.assertEqual(result['layers'][0]['clip'], 17)
        self.assertEqual(result['offsets'][0][0], 0.25)
        self.assertEqual(result['palette_recipe'], 3)
        with self.assertRaises(ValueError):
            snapshot_summary(b'', after, actor, registers, bytes(256))


if __name__ == '__main__':
    unittest.main()
