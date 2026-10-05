# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic model/animation fixtures; no executable or archive data."""
import math
import struct
import unittest

from model_animation import decode_tangent, decode_value, parse_clip, parse_track
from model_export import add_animation, build_document, rest_worlds, structural_check
from model_format import companion_draws, palette_matrices, palette_operations, read_mesh


def triangle_model():
    data = bytearray(600)
    data[20:24] = b'MDL\0'
    struct.pack_into('<4I', data, 24, 1, 1, 0, 1)
    struct.pack_into('<I', data, 40, 48)
    data[48:52] = b'OBJ\0'
    struct.pack_into('<II', data, 52, 1, 4)
    struct.pack_into('<4I', data, 80, 3, 400, 3, 360)
    struct.pack_into('<II', data, 208, 0x80000000, 144)
    struct.pack_into('<I', data, 308, 1)
    struct.pack_into('<4I', data, 336, 32, 0, 1, 2)
    struct.pack_into('<3H', data, 360, 0, 1, 2)
    for i, (x, y) in enumerate([(0, 0), (1, 0), (0, 1)]):
        struct.pack_into('<9f', data, 400 + 36*i, x, y, 0, 0.25, 0, 0, 1, x, y)
    return data


def synthetic_rig():
    return {'joints': [{'parent': -1, 'translation': [0, 2, 0]},
                       {'parent': 0, 'translation': [1, 0, 0]}],
            'offsets': [[1, 0, 0]], 'palettes': [[(0, [(0, 1)]), (1, [])]]}


class AnimationTests(unittest.TestCase):
    def test_custom_values_and_tangents(self):
        self.assertEqual(decode_value(0, 32768), 0.5)
        self.assertEqual(decode_value(0x8000, 32768), -0.5)
        self.assertEqual(decode_value(0x1000, 0), 1)
        self.assertEqual(decode_value(0xa000, 32768), -3)
        self.assertEqual(decode_tangent(0x780000), 1)
        self.assertEqual(decode_tangent(0xf80000), -1)
        self.assertEqual(decode_tangent(0), 0)
        self.assertEqual(decode_tangent(0x010000), 2**-21)
        self.assertEqual(decode_tangent(1), (1 + 1/65536) * 2**-22)

    def test_hermite_endpoints_midpoint_and_linear(self):
        # d=2, p0=0, p1=1, m0=1, m1=0 => midpoint 0.75.
        data = struct.pack('<5H2H', 8, 0, 0, 0, 0x78, 0x1000, 0)
        track = parse_track(data, 0, len(data))
        self.assertEqual(track[0].sample(0), 0)
        self.assertEqual(track[0].sample(1), 0.75)
        self.assertEqual(track[0].sample(2), 1)
        self.assertEqual(track[1].sample(100), 1)
        data = struct.pack('<4H', 9, 0, 0x1000, 0)
        self.assertEqual(parse_track(data, 0, len(data))[0].sample(1), 0.5)

    def test_clip_aliases_absence_and_invalid_spans(self):
        data = bytearray(4004 + 252)
        struct.pack_into('<2I', data, 0, 4004, 4004)
        struct.pack_into('<HHI', data, 4004, 3, 60, 244)
        struct.pack_into('<4H', data, 4248, 9, 0, 0x1000, 0)
        for slot in (0, 1):
            clip = parse_clip(data, slot)
            self.assertEqual(clip.sample(1)[0], 0.5)
            self.assertIsNone(clip.sample(1)[1])
            with self.assertRaises(ValueError):
                clip.sample(0.5)
        with self.assertRaises(ValueError):
            parse_clip(data, 2)
        struct.pack_into('<I', data, 4008, 100)
        with self.assertRaises(ValueError):
            parse_clip(data, 0)
        for bad in (b'', struct.pack('<HH', 8, 0), struct.pack('<HH', 6, 0)):
            with self.assertRaises(ValueError):
                parse_track(bad, 0, len(bad))


class ModelTests(unittest.TestCase):
    def test_mesh_weights_subsets_and_bounds(self):
        data = triangle_model()
        mesh = read_mesh(data, 0)
        self.assertEqual(mesh.weights[0], (0.25, 0.75, 0, 0))
        self.assertEqual(mesh.subsets, ((0, (0, 1, 2)),))
        struct.pack_into('<f', data, 412, 1.1)
        with self.assertRaises(ValueError):
            read_mesh(data, 0)
        data = triangle_model()
        struct.pack_into('<H', data, 360, 3)
        with self.assertRaises(ValueError):
            read_mesh(data, 0)
        with self.assertRaises(ValueError):
            read_mesh(triangle_model()[:500], 0)

    def test_palette_translation_uses_rotated_basis(self):
        record = bytes([2, 0, 128, 255, 1, 64, 255] + [0]*9)
        operations = palette_operations(record)
        worlds = [[0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 4, 5, 6, 1]] * 2
        matrices = palette_matrices(operations, [[2, 0, 0]], worlds)
        self.assertEqual(matrices[0][12:15], [4, 7, 6])
        self.assertEqual(matrices[1][12:15], [4, 3, 6])
        # A draw may consume fewer entries than a table declares.
        record = bytes([2, 0, 255] + [0]*13)
        with self.assertRaises(ValueError):
            palette_operations(record)
        self.assertEqual(palette_operations(record, 1), [(0, [])])

    def test_companion_conditionals_and_relocation(self):
        data = bytearray(128)
        struct.pack_into('<5I', data, 0, 0, 20, 0, 0, 0)
        struct.pack_into('<4I', data, 20, 1, 20, 0, 0)
        struct.pack_into('<4I', data, 40, 1, 40, 0, 0)
        data[60:74] = bytes([10, 1, 1, 0, 0, 10, 0, 2, 0, 1, 6, 0, 2, 0])
        draws, ignored = companion_draws(data)
        self.assertEqual(draws, [{'object': 1, 'palette': 0, 'flags': 1}])
        self.assertEqual(ignored, [6])
        # The header obfuscation is arithmetic, not payload compression.
        salt = 2537 // 67
        struct.pack_into('<2I', data, 0, 43, 20 + salt)
        for i in range(2, 5):
            salt = salt * 59 // 67
            struct.pack_into('<I', data, i*4, salt)
        self.assertEqual(companion_draws(data)[0], draws)

    def test_skin_writer_and_real_pose_sample_animation(self):
        rig = synthetic_rig()
        doc = build_document(triangle_model(), [{'object': 0, 'palette': 0, 'flags': 1}],
                             rig, {0: ('synthetic-key', 'synthetic.png')})
        structural_check(doc)
        j = doc.json
        positions = j['accessors'][j['meshes'][0]['primitives'][0]['attributes']['POSITION']]
        self.assertEqual(positions['min'], [1, 2, 0])
        self.assertEqual(positions['max'], [2, 3, 0])
        self.assertEqual(j['skins'][0]['joints'], [1, 2])
        self.assertEqual(j['nodes'][1]['children'], [2])
        self.assertNotIn('animations', j)
        frames = [[{'translation': v['translation'], 'rotation': [0, 0, 0, 1]}
                   for v in rig['joints']] for _ in range(2)]
        frames[1][1]['rotation'] = [0, math.sin(0.5), 0, math.cos(0.5)]
        add_animation(doc, {'times': [0, 1], 'frames': frames}, 2)
        structural_check(doc)
        self.assertEqual(len(j['animations'][0]['channels']), 4)
        with self.assertRaises(ValueError):
            add_animation(doc, {'times': [1, 0], 'frames': frames}, 2)
        rig['joints'][0]['parent'] = 1
        with self.assertRaises(ValueError):
            rest_worlds(rig['joints'])

    def test_refuse_noncoincident_palette_bind_origins(self):
        rig = synthetic_rig()
        rig['palettes'][0][0] = (0, [])
        with self.assertRaisesRegex(ValueError, 'bind-space origin'):
            build_document(triangle_model(), [{'object': 0, 'palette': 0, 'flags': 1}],
                           rig, {0: ('synthetic', 'synthetic.png')})

    def test_exported_inverse_bind_matches_rotating_draw_palette(self):
        rig = synthetic_rig()
        doc = build_document(triangle_model(), [{'object': 0, 'palette': 0, 'flags': 1}],
                             rig, {0: ('synthetic', 'synthetic.png')})

        def rows(index):
            a = doc.json['accessors'][index]
            view = doc.json['bufferViews'][a['bufferView']]
            size = {'VEC3': 3, 'MAT4': 16}[a['type']]
            return list(struct.iter_unpack('<' + 'f'*size, doc.binary[
                view['byteOffset']:view['byteOffset'] + view['byteLength']]))

        def transform(point, matrix):
            return [sum(point[k]*matrix[k*4+c] for k in range(3)) + matrix[12+c]
                    for c in range(3)]

        worlds = [[0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 4, 5, 0, 1],
                  [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 7, 8, 0, 1]]
        palette = palette_matrices(rig['palettes'][0], rig['offsets'], worlds)
        ibm = rows(doc.json['skins'][0]['inverseBindMatrices'])
        positions = rows(doc.json['meshes'][0]['primitives'][0]['attributes']['POSITION'])
        # Vertex 1 has source position (1,0,0), baked position (2,2,0).
        game = [transform([1, 0, 0], m) for m in palette]
        gltf = [transform(transform(positions[1], inverse), world)
                for inverse, world in zip(ibm, worlds)]
        self.assertEqual(game, gltf)
        self.assertEqual([0.25*a + 0.75*b for a, b in zip(*gltf)], [7, 7.75, 0])


if __name__ == '__main__':
    unittest.main()
