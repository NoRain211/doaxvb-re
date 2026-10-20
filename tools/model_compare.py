# SPDX-License-Identifier: GPL-3.0-or-later
"""Report every FK joint/palette residual against a paired private capture."""
import argparse
import copy
import json
import math
from pathlib import Path

from afs_extract import private_output
from model_animation import parse_clip
from model_format import palette_matrices
from model_pose import compare_worlds, fk_worlds
from model_skeleton import solve_scene


def compare_capture(capture, rig, tolerance=2e-5, channels=None, solver=None, grid=None):
    rig = copy.deepcopy(rig)
    # Offsets are captured actor inputs; no output matrices enter FK evaluation.
    rig['offsets'] = capture['offsets']
    for i in range(24):
        if i != 15:
            rig['joints'][i]['translation'] = capture['offsets'][i]
    heading = capture['root_heading_radians_before']
    if capture['mode'] != 5 and not capture['root_heading_preserved']:
        units = (capture['actor_heading_units'] + 16384) & 0xffffffff
        if units >= 2**31:
            units -= 2**32
        heading = units * math.tau / 65536
    values = capture['channels'] if channels is None else channels
    worlds = (solve_scene(solver['tables'], values, solver['targets'], grid)[0] if solver else
              fk_worlds(values, rig, capture['actor_position'], heading))
    report = compare_worlds(worlds, capture['bone_worlds'], tolerance)
    report['evaluation'] = ('FK from captured scalars' if channels is None else
                            'FK from archive scalars, with captured actor/rig inputs')
    report['scalar_max_abs_error'] = max(abs(a-b) for a, b in zip(values, capture['channels']))
    if solver:
        report['evaluation'] = 'Native model port; explicit frozen controllers; independently queried terrain'
        report['scalar_source'] = 'archive' if channels is not None else 'captured input'
    report['actor'] = capture['actor']
    report['frame'] = capture['frame']
    operations = rig['palettes'][capture['palette_recipe']]
    palettes = palette_matrices(operations, capture['palette_offsets'], worlds)
    report['draw_palette'] = compare_worlds(palettes, capture['palette'][:len(palettes)], tolerance)
    report['draw_palette']['slots'] = [op[0] for op in operations]
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', required=True, type=Path)
    parser.add_argument('--rig', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--tolerance', type=float, default=2e-5)
    parser.add_argument('--motion', type=Path)
    parser.add_argument('--slot', type=int, default=0)
    parser.add_argument('--tick', type=int, default=0)
    parser.add_argument('--solver-inputs', type=Path)
    parser.add_argument('--terrain-grid', type=Path)
    args = parser.parse_args()
    try:
        if not math.isfinite(args.tolerance) or args.tolerance < 0:
            raise ValueError('Invalid comparison tolerance')
        output = private_output(args.output)
        channels = None
        if args.motion:
            clip = parse_clip(args.motion.read_bytes(), args.slot)
            channels = [0.0 if v is None else v for v in clip.sample(args.tick)]
        if bool(args.solver_inputs) != bool(args.terrain_grid):
            raise ValueError('Solver inputs and terrain grid must be supplied together')
        report = compare_capture(json.loads(args.capture.read_text()),
                                 json.loads(args.rig.read_text()), args.tolerance, channels,
                                 json.loads(args.solver_inputs.read_text()) if args.solver_inputs else None,
                                 json.loads(args.terrain_grid.read_text()) if args.terrain_grid else None)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(report, stream, indent=2, allow_nan=False)
            stream.write('\n')
        print('Differing joints:', report['differing_joints'])
        print('Differing palette entries:', report['draw_palette']['differing_joints'])
    except (ValueError, OSError, KeyError, IndexError) as error:
        parser.exit(1, f'model comparison: {error}\n')


if __name__ == '__main__':
    main()
