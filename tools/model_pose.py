# SPDX-License-Identifier: GPL-3.0-or-later
"""Primary FK evaluation and row-vector transform conversion.

Limb targets, terrain, look-at and derived-joint solvers are deliberately absent
from fk_worlds. Comparisons must report their residuals, not treat FK as solved.
"""
import math


def identity():
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def multiply(a, b):
    return [sum(a[r*4+k]*b[k*4+c] for k in range(4)) for r in range(4) for c in range(4)]


def translation(xyz):
    matrix = identity()
    matrix[12:15] = xyz
    return matrix


def rotation(axis, angle):
    c, s = math.cos(angle), math.sin(angle)
    matrix = identity()
    a, b = ((1, 2), (2, 0), (0, 1))[axis]
    matrix[a*4+a] = matrix[b*4+b] = c
    matrix[a*4+b], matrix[b*4+a] = s, -s
    return matrix


def euler(xyz):
    # Local Z, Y, X operations pre-multiply the current row-vector matrix.
    return multiply(multiply(rotation(0, xyz[0]), rotation(1, xyz[1])), rotation(2, xyz[2]))


def inverse_rigid(matrix):
    result = identity()
    for r in range(3):
        for c in range(3):
            result[r*4+c] = matrix[c*4+r]
    for c in range(3):
        result[12+c] = -sum(matrix[12+k]*result[k*4+c] for k in range(3))
    return result


def quaternion(matrix):
    """Unit XYZW quaternion from a row-vector rotation matrix."""
    # Standard column-vector extraction on the transpose.
    m = [[matrix[c*4+r] for c in range(3)] for r in range(3)]
    trace = sum(m[i][i] for i in range(3))
    if trace > 0:
        s = 2 * math.sqrt(trace + 1)
        q = [(m[2][1]-m[1][2])/s, (m[0][2]-m[2][0])/s,
             (m[1][0]-m[0][1])/s, s/4]
    else:
        i = max(range(3), key=lambda k: m[k][k])
        j, k = (i+1) % 3, (i+2) % 3
        s = 2 * math.sqrt(max(0, 1 + m[i][i] - m[j][j] - m[k][k]))
        if s < 1e-12:
            raise ValueError('Degenerate joint rotation')
        q = [0.0] * 4
        q[i], q[j], q[k] = s/4, (m[j][i]+m[i][j])/s, (m[k][i]+m[i][k])/s
        q[3] = (m[k][j]-m[j][k])/s
    norm = math.sqrt(sum(v*v for v in q))
    return [v/norm for v in q]


def local_poses(worlds, joints):
    if len(worlds) != len(joints):
        raise ValueError('Pose and hierarchy sizes differ')
    result = []
    for i, joint in enumerate(joints):
        parent = joint['parent']
        matrix = worlds[i] if parent == -1 else multiply(worlds[i], inverse_rigid(worlds[parent]))
        result.append({'translation': matrix[12:15], 'rotation': quaternion(matrix)})
    return result


def fk_worlds(channels, rig, position=(0, 0, 0), heading=0.0):
    if len(channels) != 60 or any(not math.isfinite(v) for v in channels):
        raise ValueError('Expected sixty finite pose channels')
    joints = rig['joints']
    if len(joints) != 32:
        raise ValueError('Primary FK requires the 32-slot rig')
    keyed = {2: channels[3:6], 23: channels[44:47], 0: channels[6:9],
             19: channels[47:50], 1: channels[9:12],
             21: [channels[56], channels[57], 0],
             17: [channels[58], channels[59], 0]}
    root = multiply(rotation(1, heading), translation(position))
    worlds, active = {}, set()

    def visit(i):
        if i in worlds:
            return worlds[i]
        if i in active or not 0 <= i < len(joints):
            raise ValueError('Invalid FK hierarchy')
        active.add(i)
        joint = joints[i]
        offset = joint['translation']
        parent = identity() if joint['parent'] == -1 else visit(joint['parent'])
        if i == 2:
            offset, parent = [0, channels[1], 0], root
        matrix = multiply(translation(offset), parent)
        if i in keyed:
            matrix = multiply(euler(keyed[i]), matrix)
        if i in (20, 16):
            channel = 50 if i == 20 else 52
            matrix = multiply(rotation(1, channels[channel+1]), matrix)
            matrix = multiply(rotation(2, channels[channel]), matrix)
        if i == 15:
            matrix = root
        worlds[i] = matrix
        active.remove(i)
        return matrix

    return [visit(i) for i in range(32)]


def compare_worlds(actual, expected, tolerance=2e-5):
    if len(actual) != len(expected):
        raise ValueError('Matrix bank lengths differ')
    result = []
    for i, (a, b) in enumerate(zip(actual, expected)):
        if len(a) != 16 or len(b) != 16 or not all(math.isfinite(v) for v in (*a, *b)):
            raise ValueError('Invalid comparison matrix')
        error = max(abs(x-y) for x, y in zip(a, b))
        result.append({'joint': i, 'max_abs_error': error, 'matches': error <= tolerance,
                       'translation_error': math.sqrt(sum((a[k]-b[k])**2 for k in (12, 13, 14)))})
    return {'tolerance': tolerance, 'joints': result,
            'differing_joints': [r['joint'] for r in result if not r['matches']]}


def sample_fk_clip(clip, rig, ticks_per_second=60.0):
    if not math.isfinite(ticks_per_second) or ticks_per_second <= 0:
        raise ValueError('Ticks per second must be positive')
    frames = []
    for tick in range(clip.count):
        channels = [0.0 if v is None else v for v in clip.sample(tick)]
        frames.append(local_poses(fk_worlds(channels, rig), rig['joints']))
    return {'times': [i/ticks_per_second for i in range(clip.count)], 'frames': frames,
            'evaluation': 'FK only; IK, terrain, look-at and derived joints unsolved',
            'missing_channel_policy': 'zero; verify the absent-track default for the input revision'}
