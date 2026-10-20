# SPDX-License-Identifier: GPL-3.0-or-later
"""Model geometry and draw-palette readers. Executable rig tables stay private."""
from dataclasses import dataclass
import math

from model_animation import unpack


@dataclass(frozen=True)
class Mesh:
    positions: tuple
    normals: tuple
    uvs: tuple
    weights: tuple
    subsets: tuple


def read_mesh(data, index):
    if data[20:24] != b'MDL\0':
        raise ValueError('Missing model user record')
    count, textures = unpack(data, 24, 'II')
    if not 0 <= index < count:
        raise ValueError('Object ordinal outside model')
    offset = unpack(data, 40 + index * 4, 'I')[0]
    if data[offset:offset + 4] != b'OBJ\0':
        raise ValueError('Missing object record')
    weights, blocks = unpack(data, offset + 4, 'II')
    if weights > 3 or blocks != 4:
        raise ValueError('Unsupported vertex layout')
    vertices, vertex_data, indices, index_data = unpack(data, offset + 32, '4I')
    stride = 32 + 4 * weights
    if not vertices or vertices * stride > len(data) - vertex_data:
        raise ValueError('Invalid vertex span')
    all_indices = unpack(data, index_data, f'{indices}H')
    if any(i >= vertices for i in all_indices):
        raise ValueError('Vertex index outside buffer')
    positions, normals, uvs, influences = [], [], [], []
    for i in range(vertices):
        values = unpack(data, vertex_data + i * stride, f'{8 + weights}f')
        if not all(math.isfinite(v) for v in values):
            raise ValueError('Nonfinite vertex')
        w = values[3:3 + weights]
        remainder = 1 - sum(w)
        # glTF cannot encode negative influences. Never silently repair them.
        if any(v < 0 or v > 1 for v in w) or remainder < 0:
            raise ValueError('Weights cannot be represented by glTF without repair')
        positions.append(values[:3])
        normals.append(values[3 + weights:6 + weights])
        uvs.append(values[-2:])
        influences.append((*w, remainder, *(0.0 for _ in range(3 - weights))))
    subsets = []
    cursor = offset + 32 + blocks * 32
    while True:
        tag = unpack(data, cursor, 'I')[0]
        if tag == 0:
            break
        size = unpack(data, cursor + 4, 'I')[0]
        if size != 144 or tag & 7 not in (0, 1, 3, 4, 5):
            raise ValueError('Unsupported material command')
        stages = unpack(data, cursor + 100, 'I')[0]
        if stages != 1:
            raise ValueError('Only one-stage material commands are supported')
        texture = unpack(data, cursor + 112, 'I')[0]
        declaration, start, triangles, topology = unpack(data, cursor + 128, '4I')
        if declaration != 32 or topology != 2:
            raise ValueError('Unsupported subset vertex declaration or topology')
        if texture >= textures or start + triangles + 2 > indices:
            raise ValueError('Invalid subset texture or index span')
        strip = all_indices[start:start + triangles + 2]
        faces = []
        for i in range(len(strip) - 2):
            a, b, c = strip[i:i + 3]
            if i & 1:
                a, b = b, a
            if len({a, b, c}) == 3:
                faces.extend((a, b, c))
        if faces:
            subsets.append((texture, tuple(faces)))
        cursor += size
    if not subsets:
        raise ValueError('Object has no triangles')
    return Mesh(tuple(positions), tuple(normals), tuple(uvs),
                tuple(influences), tuple(subsets))


def companion_draws(data, variant=0):
    """Read the primary companion draw list; return draws and ignored opcodes.

    Secondary geometry and material mutations are reported, not simulated.
    Offsets are relative to the first companion section after header decoding.
    """
    header = list(unpack(data, 0, '5I'))
    salt = ((header[0] // 43) * 2537) // 67
    for i in range(1, 5):
        header[i] = (header[i] - salt) & 0xffffffff
        salt = salt * 59 // 67
    base = header[1]
    if not 20 <= base < len(data):
        raise ValueError('Invalid companion section')
    cursor, scripts = base, []
    while True:
        kind, relative = unpack(data, cursor, 'II')
        if not kind:
            break
        if kind == 1:
            sub = base + relative
            while True:
                pass_id, script = unpack(data, sub, 'II')
                if not pass_id:
                    break
                scripts.append(base + script)
                sub += 8
        cursor += 8
    if not scripts:
        raise ValueError('Missing primary draw scripts')
    draws, ignored = [], set()
    sizes = {1: 2, 2: 2, 3: 6, 4: 6, 5: 2, 6: 2, 7: 6,
             8: 0, 9: 0, 10: 1, 11: 0, 12: 2, 13: 2}
    for cursor in scripts:
        enabled = True
        while True:
            op = unpack(data, cursor, 'B')[0]
            cursor += 1
            if not op:
                break
            if op not in sizes:
                raise ValueError(f'Unknown draw opcode {op}')
            args = unpack(data, cursor, f'{sizes[op]}B')
            cursor += sizes[op]
            if op == 10:
                enabled = args[0] == variant
            elif op == 11:
                enabled = True
            elif enabled and op in (1, 2, 3, 4):
                selector, obj = args[:2]
                flags = int.from_bytes(bytes(args[2:]), 'little') if op in (3, 4) else 0
                if op in (2, 4):
                    draws.append({'object': obj, 'palette': selector, 'flags': flags | 1})
                else:
                    bone = selector if selector < 128 else selector - 104
                    draws.append({'object': obj, 'bone': bone, 'flags': flags})
            elif enabled and op not in (8, 9):
                ignored.add(op)
    return draws, sorted(ignored)


def palette_operations(record, used_count=None):
    """Decode one 16-byte palette recipe into (bone slot, offset operations)."""
    if len(record) != 16 or not 1 <= record[0] <= 4:
        raise ValueError('Invalid palette recipe')
    count = record[0] if used_count is None else used_count
    if not 1 <= count <= record[0]:
        raise ValueError('Invalid consumed palette count')
    cursor, result = 1, []
    for _ in range(count):
        bone, offsets = None, []
        while cursor < len(record) and record[cursor] != 255:
            op = record[cursor]
            cursor += 1
            if op < 64:
                bone, offsets = op, []
            elif op < 128:
                offsets.append((op & 63, -1))
            else:
                offsets.append((op & 63, 1))
        if bone is None or cursor == len(record):
            raise ValueError('Unterminated palette or offset before bone selection')
        cursor += 1
        result.append((bone, offsets))
    return result


def palette_matrices(operations, offsets, worlds):
    """Apply local translations to row-vector world matrices (WORLD0 first)."""
    result = []
    for bone, translations in operations:
        if not 0 <= bone < len(worlds):
            raise ValueError('Palette bone outside matrix bank')
        matrix = list(worlds[bone])
        if len(matrix) != 16 or not all(math.isfinite(v) for v in matrix):
            raise ValueError('Invalid world matrix')
        for index, sign in translations:
            if not 0 <= index < len(offsets):
                raise ValueError('Palette offset outside rig')
            xyz = offsets[index]
            for column in range(4):
                matrix[12 + column] += sign * sum(xyz[k] * matrix[4*k + column]
                                                 for k in range(3))
        result.append(matrix)
    return result
