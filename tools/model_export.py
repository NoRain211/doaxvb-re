# SPDX-License-Identifier: GPL-3.0-or-later
"""Export private skinned geometry with supplied, FK or procedural clip poses.

Archive scalar channels are NOT joint rotations. --motion writes a diagnostic
sidecar unless FK or explicit solver inputs are selected. Controllers stay frozen.
See docs/wayfinder/research/model-format.md for the explicit rig input contract.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import struct
from urllib.parse import quote

from afs_extract import private_output
from model_animation import parse_clip
from model_format import companion_draws, palette_matrices, read_mesh
from model_pose import affine_local_poses, sample_fk_clip
from model_skeleton import solve_scene
from texture_rip import texture_key, texture_payloads


def translation_matrix(xyz):
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, *xyz, 1]


def reflect(xyz):
    return [xyz[0], xyz[1], -xyz[2]]


def rest_worlds(joints):
    if not joints or len(joints) > 65535:
        raise ValueError('Invalid joint count')
    worlds, active = {}, set()

    def visit(i):
        if i in active or not 0 <= i < len(joints):
            raise ValueError('Invalid or cyclic joint hierarchy')
        if i in worlds:
            return worlds[i]
        active.add(i)
        joint = joints[i]
        xyz, parent = joint['translation'], joint['parent']
        if len(xyz) != 3 or not all(math.isfinite(v) for v in xyz):
            raise ValueError('Invalid bind translation')
        origin = [0, 0, 0] if parent == -1 else visit(parent)[12:15]
        worlds[i] = translation_matrix([a + b for a, b in zip(origin, xyz)])
        active.remove(i)
        return worlds[i]

    return [visit(i) for i in range(len(joints))]


class Document:
    def __init__(self):
        self.binary = bytearray()
        self.json = {'asset': {'version': '2.0', 'generator': 'private model exporter'},
                     'scene': 0, 'scenes': [{'nodes': [0]}],
                     'nodes': [{'name': 'root', 'children': []}],
                     'meshes': [], 'skins': [], 'materials': [],
                     'images': [], 'textures': [], 'accessors': [], 'bufferViews': []}

    def accessor(self, rows, kind, component=5126, target=None, bounds=False):
        widths = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}
        width = widths[kind]
        if not rows or any(len(row) != width for row in rows):
            raise ValueError('Invalid accessor shape')
        if any(not math.isfinite(v) for row in rows for v in row):
            raise ValueError('Nonfinite accessor value')
        self.binary.extend(b'\0' * (-len(self.binary) % 4))
        start = len(self.binary)
        fmt = '<' + {5126: 'f', 5123: 'H', 5125: 'I'}[component] * width
        for row in rows:
            self.binary.extend(struct.pack(fmt, *row))
        view = {'buffer': 0, 'byteOffset': start, 'byteLength': len(self.binary) - start}
        if target:
            view['target'] = target
        index = len(self.json['accessors'])
        accessor = {'bufferView': len(self.json['bufferViews']), 'componentType': component,
                    'count': len(rows), 'type': kind}
        if bounds:
            accessor['min'] = [min(row[i] for row in rows) for i in range(width)]
            accessor['max'] = [max(row[i] for row in rows) for i in range(width)]
        self.json['bufferViews'].append(view)
        self.json['accessors'].append(accessor)
        return index


def add_animation(doc, poses, joint_count):
    """Export one explicitly supplied clip of solved local translations/quaternions."""
    times, frames = poses['times'], poses['frames']
    if (not times or len(times) != len(frames)
            or any(not math.isfinite(t) or t < 0 for t in times)
            or any(a >= b for a, b in zip(times, times[1:]))
            or any(len(frame) != joint_count for frame in frames)):
        raise ValueError('Invalid solved pose sample shape or times')
    time_index = doc.accessor([(t,) for t in times], 'SCALAR', bounds=True)
    animation = {'name': 'solved_clip', 'samplers': [], 'channels': []}
    for joint in range(joint_count):
        rotations, translations, scales = [], [], []
        for frame in frames:
            value = frame[joint]
            q, xyz = value['rotation'], value['translation']
            if len(q) != 4 or len(xyz) != 3 or abs(sum(v*v for v in q) - 1) > 1e-5:
                raise ValueError('Invalid solved pose transform')
            # Reflect the source Z axis; quaternion components are XYZW.
            q = [-q[0], -q[1], q[2], q[3]]
            if rotations and sum(a*b for a, b in zip(rotations[-1], q)) < 0:
                q = [-v for v in q]
            rotations.append(q)
            translations.append(reflect(xyz))
            if 'scale' in value:
                scale = value['scale']
                if len(scale)!=3 or any(not math.isfinite(v) or v<=0 for v in scale):
                    raise ValueError('Invalid solved pose scale')
                scales.append(scale)
        channels = [('translation', translations, 'VEC3'), ('rotation', rotations, 'VEC4')]
        if scales:
            if len(scales)!=len(frames):
                raise ValueError('Incomplete scale samples')
            channels.append(('scale', scales, 'VEC3'))
        for path, values, kind in channels:
            sampler = len(animation['samplers'])
            animation['samplers'].append({'input': time_index,
                                         'output': doc.accessor(values, kind),
                                         'interpolation': 'LINEAR'})
            animation['channels'].append({'sampler': sampler,
                                          'target': {'node': joint + 1, 'path': path}})
    doc.json['animations'] = [animation]


def sample_solved_clip(clip, rig, inputs, grid, ticks_per_second):
    if not math.isfinite(ticks_per_second) or ticks_per_second <= 0:
        raise ValueError('Ticks per second must be positive')
    frames, error = [], 0.0
    for tick in range(clip.count):
        values = [0.0 if v is None else v for v in clip.sample(tick)]
        worlds,_ = solve_scene(inputs['tables'],values,inputs['targets'],grid)
        frame,residual = affine_local_poses(worlds,rig['joints'])
        frames.append(frame)
        error = max(error,residual)
    return {'times':[i/ticks_per_second for i in range(clip.count)], 'frames':frames,
            'evaluation':'native skeleton model; frozen actor/controllers; queried scene terrain',
            'max_world_trs_projection_error':error}


def build_document(model, draws, rig, texture_uris, poses=None):
    worlds = rest_worlds(rig['joints'])
    doc = Document()
    j = doc.json
    for i, joint in enumerate(rig['joints']):
        j['nodes'].append({'name': f'bone_{i}', 'translation': reflect(joint['translation'])})
    for i, joint in enumerate(rig['joints']):
        parent = joint['parent'] + 1
        j['nodes'][parent].setdefault('children', []).append(i + 1)
    inverse_bind = [translation_matrix([-v for v in reflect(w[12:15])]) for w in worlds]
    j['skins'].append({'joints': list(range(1, len(worlds) + 1)), 'skeleton': 0,
                       'inverseBindMatrices': doc.accessor(inverse_bind, 'MAT4')})
    material_ids, objects = {}, set()
    for draw in draws:
        obj = draw['object']
        mesh = read_mesh(model, obj)
        if draw['flags'] & ~1:
            raise ValueError('Draw flags need an explicit material interpretation')
        if 'bone' in draw:
            operations = [(draw['bone'], [])]
        else:
            operations = rig['palettes'][draw['palette']]
        palette = palette_matrices(operations, rig['offsets'], worlds)
        origin = palette[0][12:15]
        if any(max(abs(a-b) for a, b in zip(origin, m[12:15])) > 1e-6 for m in palette):
            raise ValueError('Palette does not share one bind-space origin')
        # The implicit weight belongs to the final palette slot, including rigid draws.
        offset = struct.unpack_from('<I', model, 40 + obj * 4)[0]
        explicit = struct.unpack_from('<I', model, offset + 4)[0]
        if len(palette) != explicit + 1:
            raise ValueError('Palette size does not match vertex influence count')
        positions = [reflect([a+b for a, b in zip(p, origin)]) for p in mesh.positions]
        normals = []
        for n in mesh.normals:
            length = math.sqrt(sum(v*v for v in n))
            if not 0.98 <= length <= 1.02:
                raise ValueError('Unexpected normal length')
            normals.append(reflect([v / length for v in n]))
        ids = [op[0] for op in operations]
        ids += [0] * (4 - len(ids))
        attributes = {
            'POSITION': doc.accessor(positions, 'VEC3', target=34962, bounds=True),
            'NORMAL': doc.accessor(normals, 'VEC3', target=34962),
            'TEXCOORD_0': doc.accessor(mesh.uvs, 'VEC2', target=34962),
            'JOINTS_0': doc.accessor([ids] * len(positions), 'VEC4', 5123, 34962),
            'WEIGHTS_0': doc.accessor(mesh.weights, 'VEC4', target=34962)}
        primitives = []
        for texture, faces in mesh.subsets:
            if texture not in material_ids:
                key, uri = texture_uris[texture]
                material_ids[texture] = len(j['materials'])
                image_index = len(j['images'])
                j['images'].append({'uri': uri, 'name': key})
                j['textures'].append({'source': image_index})
                j['materials'].append({'name': f'texture_{texture}', 'doubleSided': True,
                    'pbrMetallicRoughness': {'baseColorTexture': {'index': image_index},
                                           'metallicFactor': 0, 'roughnessFactor': 1},
                    'extras': {'replacement_key': key, 'material_fidelity': 'preview'}})
            # Reversing winding accompanies the handedness conversion.
            indices = [(faces[i+k],) for i in range(0, len(faces), 3) for k in (0, 2, 1)]
            primitives.append({'attributes': attributes,
                               'indices': doc.accessor(indices, 'SCALAR', 5125, 34963),
                               'material': material_ids[texture]})
        node = {'name': f'object_{obj}', 'mesh': len(j['meshes']), 'skin': 0}
        j['nodes'][0]['children'].append(len(j['nodes']))
        j['nodes'].append(node)
        j['meshes'].append({'primitives': primitives})
        objects.add(obj)
    if not objects:
        raise ValueError('No exportable draws')
    if poses is not None:
        add_animation(doc, poses, len(worlds))
    j['extras'] = {'scope': 'primary bind preview with supplied rig',
                   'rig_provenance': rig.get('provenance', 'unspecified'),
                   'exported_objects': sorted(objects),
                   'animation': 'supplied solved poses' if poses else 'not solved'}
    return doc


def structural_check(doc):
    """Check this writer's buffer spans/references. Not a full glTF validator."""
    j = doc.json
    for a in j['accessors']:
        view = j['bufferViews'][a['bufferView']]
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
        size = {5123: 2, 5125: 4, 5126: 4}[a['componentType']]
        if (view['byteOffset'] % 4 or a['count'] * width * size != view['byteLength']
                or view['byteOffset'] + view['byteLength'] > len(doc.binary)):
            raise ValueError('Invalid accessor buffer span')
    for mesh in j['meshes']:
        for p in mesh['primitives']:
            count = j['accessors'][p['attributes']['POSITION']]['count']
            if any(j['accessors'][a]['count'] != count for a in p['attributes'].values()):
                raise ValueError('Mismatched vertex attributes')
            a = j['accessors'][p['indices']]
            view = j['bufferViews'][a['bufferView']]
            values = struct.unpack_from('<' + 'I' * a['count'], doc.binary, view['byteOffset'])
            if len(values) % 3 or max(values) >= count:
                raise ValueError('Invalid triangle indices')
    for animation in j.get('animations', []):
        targets = set()
        for channel in animation['channels']:
            target = channel['target']
            key = (target['node'], target['path'])
            if key in targets or not 0 <= key[0] < len(j['nodes']):
                raise ValueError('Duplicate or invalid animation target')
            targets.add(key)
            sampler = animation['samplers'][channel['sampler']]
            time = j['accessors'][sampler['input']]
            value = j['accessors'][sampler['output']]
            kind = {'translation': 'VEC3', 'rotation': 'VEC4', 'scale': 'VEC3'}.get(key[1])
            if (sampler['interpolation'] != 'LINEAR' or time['type'] != 'SCALAR'
                    or value['type'] != kind or time['count'] != value['count']
                    or time['componentType'] != 5126 or value['componentType'] != 5126):
                raise ValueError('Invalid animation accessor contract')
            view = j['bufferViews'][time['bufferView']]
            times = struct.unpack_from('<' + 'f'*time['count'], doc.binary, view['byteOffset'])
            if (any(not math.isfinite(t) or t < 0 for t in times)
                    or any(a >= b for a, b in zip(times, times[1:]))):
                raise ValueError('Invalid encoded animation times')
            view = j['bufferViews'][value['bufferView']]
            width = 4 if kind == 'VEC4' else 3
            for row in struct.iter_unpack('<' + 'f'*width, doc.binary[
                    view['byteOffset']:view['byteOffset'] + view['byteLength']]):
                if (not all(math.isfinite(v) for v in row)
                        or (key[1] == 'scale' and min(row) <= 0)
                        or (width == 4 and abs(sum(v*v for v in row)-1) > 2e-5)):
                    raise ValueError('Invalid encoded animation transform')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('model', 'companion', 'rig', 'textures'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('private/rip/gltf/model'))
    parser.add_argument('--variant', type=int, default=0)
    parser.add_argument('--pose-samples', type=Path)
    parser.add_argument('--motion', type=Path, help='Write decoded scalars; does not solve a skeleton')
    parser.add_argument('--slot', type=int, default=0)
    parser.add_argument('--fk-motion', action='store_true', help='Export explicitly incomplete FK animation')
    parser.add_argument('--ticks-per-second', type=float, default=60)
    parser.add_argument('--solver-inputs', type=Path, help='Private tables and frozen controller targets')
    parser.add_argument('--terrain-grid', type=Path, help='Private scene grid for independent height queries')
    args = parser.parse_args()
    try:
        output = private_output(args.output)
        if output.exists():
            raise ValueError('Output directory must be new')
        model = args.model.read_bytes()
        rig = json.loads(args.rig.read_text())
        draws, ignored = companion_draws(args.companion.read_bytes(), args.variant)
        uris = {}
        for ordinal, (_, texture, payload, palette, error) in enumerate(texture_payloads(model, True)):
            if error:
                continue
            key = texture_key(texture, payload, palette)
            if not re.fullmatch(r'[0-9a-f]{2}-\d+x\d+-m\d+-[0-9a-f]{64}', key):
                raise ValueError('Invalid texture key')
            png = args.textures / (key + '.png')
            if not png.is_file():
                raise ValueError(f'Missing decoded texture ordinal {ordinal}')
            uris[ordinal] = (key, quote(os.path.relpath(png.resolve(), output).replace('\\', '/')))
        poses = json.loads(args.pose_samples.read_text()) if args.pose_samples else None
        clip = parse_clip(args.motion.read_bytes(), args.slot) if args.motion else None
        if args.fk_motion:
            if clip is None or poses is not None:
                raise ValueError('--fk-motion requires --motion and excludes --pose-samples')
            poses = sample_fk_clip(clip, rig, args.ticks_per_second)
        if args.solver_inputs or args.terrain_grid:
            if not args.solver_inputs or not args.terrain_grid or clip is None or poses is not None:
                raise ValueError('Solver requires motion, inputs and terrain; excludes FK/supplied poses')
            poses = sample_solved_clip(clip, rig, json.loads(args.solver_inputs.read_text()),
                                       json.loads(args.terrain_grid.read_text()), args.ticks_per_second)
        doc = build_document(model, draws, rig, uris, poses)
        doc.json['extras']['ignored_draw_opcodes'] = ignored
        if args.fk_motion:
            doc.json['extras']['animation'] = poses['evaluation']
            doc.json['extras']['ticks_per_second'] = args.ticks_per_second
            doc.json['animations'][0]['name'] = 'fk_clip_incomplete'
        if args.solver_inputs:
            doc.json['extras'].update(animation=poses['evaluation'],
                ticks_per_second=args.ticks_per_second,
                max_world_trs_projection_error=poses['max_world_trs_projection_error'],
                solver_source_commit='8ca957b', controller_policy='frozen for the entire clip')
        structural_check(doc)
        motion = None
        if clip is not None:
            motion = {'nominal_ticks_per_second': 60, 'slot': args.slot,
                      'warning': 'Scalar channels, not joint transforms; nominal speed only',
                      'samples': [clip.sample(i) for i in range(clip.count)]}
        doc.json['buffers'] = [{'uri': 'model.bin', 'byteLength': len(doc.binary)}]
        output.mkdir(parents=True)
        (output / 'model.bin').write_bytes(doc.binary)
        (output / 'model.gltf').write_text(json.dumps(doc.json, indent=2, allow_nan=False) + '\n',
                                          encoding='utf-8', newline='\n')
        if motion:
            (output / 'motion-scalars.json').write_text(json.dumps(motion, allow_nan=False) + '\n',
                                                        encoding='utf-8', newline='\n')
        print(f'Exported {len(doc.json["meshes"])} primary draws to {output}')
        print('Structural check passed; animation:', doc.json['extras']['animation'])
    except (ValueError, OSError, KeyError, IndexError, struct.error) as error:
        parser.exit(1, f'model export: {error}\n')


if __name__ == '__main__':
    main()
