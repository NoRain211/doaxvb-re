# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture matched pose inputs/results from an owned Windows debugger run.

Requires a runner built with matching PDB symbols. Guest snapshots and receipts
are private. Breakpoints observe natural execution; no guest state is forced.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import time

from afs_extract import private_output


RAM_SIZE = 0x4000000


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def debugger_scripts(output, actor, frame):
    """CDB file commands avoid nested quoting inside breakpoint commands."""
    output = Path(output).absolute()
    if any(c in str(output) for c in '\n\r";'):
        raise ValueError('Output path contains debugger command delimiters')
    if not 0 <= actor < 4 or not 0 <= frame <= 100000:
        raise ValueError('Invalid actor or target frame')
    module = 'recomp_program_runner!'
    regs = module + 'recomp_runtime'
    counter = module + 'frame_state+0xc'
    bank = 0x4d3650 + actor * 0x800

    def path(name):
        return (output / name).as_posix()

    def read(name, address, length):
        return f'.writemem "{path(name)}" {address} L?{length:x}'

    def run(name):
        return f'$><{path(name)}'

    arm_pose = (
            f'bp0 {module}sub_000B01E0 ".if (dwo({regs}) == {actor:x}) {{ '
            f'.if (dwo({counter}) >= {frame:x}) {{ {run("pose.cdb")} }} '
            f'.else {{ gc }} }} .else {{ gc }}"')
    return {
        'start.cdb': '\n'.join([
            '.reload /f recomp_program_runner.exe', arm_pose,
            'g']),
        'pose.cdb': '\n'.join([
            'bc 0', f'r @$t0 = poi({module}direct_ram)',
            f'r @$t3 = dwo({counter})',
            '.printf "MODEL_POSE frame=%u ram=%p\\n", @$t3, @$t0',
            read('pose-registers.bin', regs, 32),
            read('before.bin', '@$t0', RAM_SIZE),
            f'bp1 /1 poi(@rsp) "{run("pose-return.cdb")}"', 'g']),
        'pose-return.cdb': '\n'.join([
            read('after.bin', '@$t0', RAM_SIZE),
            f'bp2 {module}sub_000636D0 "'
            f'.if (dwo(@$t0+dwo({regs}+0x1c)+4) == {bank:x}) {{ '
            f'.if (dwo({counter}) == @$t3) {{ {run("palette.cdb")} }} '
            f'.else {{ {run("retry.cdb")} }} }} .else {{ gc }}"', 'g']),
        'retry.cdb': '\n'.join([
            '.printf "MODEL_RETRY pose=%u draw=%u\\n", @$t3, '
            f'dwo({counter})', 'bc 2', arm_pose, 'g']),
        'palette.cdb': '\n'.join([
            'bc 2', read('palette-registers.bin', regs, 32),
            read('draw-bank.bin', f'@$t0+{bank:x}', 0x800),
            f'.printf "MODEL_PALETTE frame=%u recipe=%u offsets=%x\\n", '
            f'dwo({counter}), dwo({regs}), dwo({regs}+0x14)',
            f'bp3 /1 poi(@rsp) "{run("palette-return.cdb")}"', 'g']),
        'palette-return.cdb': '\n'.join([
            read('palette.bin', '@$t0+0xb25840', 0x100),
            '.echo MODEL_CAPTURE_COMPLETE', 'q'])}


def snapshot_summary(before, after, actor, palette_registers, palette, draw_bank=None):
    if len(before) != RAM_SIZE or len(after) != RAM_SIZE:
        raise ValueError('Expected complete guest RAM snapshots')

    def read(address, fmt, data=before):
        if address & 0xf0000000 == 0x80000000:
            address &= 0x0fffffff
        if address < 0 or address + struct.calcsize('<' + fmt) > len(data):
            raise ValueError('Guest span outside snapshot')
        return struct.unpack_from('<' + fmt, data, address)

    pose = read(0x5e06a0 + actor * 0x5c, 'I')[0]
    context = 0x5e5fe8 + actor * 0x180
    rig = read(context + 0x10, 'I')[0]
    layers = []
    for i in range(2):
        base = 0x5e0800 + actor * 0x100 + i * 0x80
        layers.append({'flags': read(base + 0x60, 'H')[0],
                       'clip': read(base + 0x62, 'H')[0],
                       'cursor': read(base + 0x48, 'f')[0],
                       'bank_character': read(base + 0x6e, 'B')[0],
                       'mirrored': read(base + 0x79, 'B')[0]})
        layer = layers[-1]
        layer['slot'] = layer['clip'] & 0xfff
        layer['shared_bank'] = bool((layer['clip'] | layer['flags']) & 0x2000)
        bank_pointer = (0x5dd874 if layer['shared_bank'] else
                        0x5dd844 + layer['bank_character'] * 12)
        layer['bank_address'] = read(bank_pointer, 'I')[0]
        layer['clip_address'] = read(layer['bank_address'] + layer['slot']*4, 'I')[0]
        layer['record_header'] = read(base + 0x5c, 'I')[0]
    bank = 0x4d3650 + actor * 0x800
    preg = struct.unpack('<8I', palette_registers)
    actor_base = 0x4d6ff0 + actor * 0xbc
    return {'actor': actor, 'pose_address': pose, 'rig_address': rig,
            'mode': read(0x4d56da, 'B')[0],
            'root_heading_preserved': read(context + 0x17e, 'B')[0],
            'root_heading_radians_before': read(context + 12, 'f')[0],
            'angle_unit_radians': read(0x281288, 'f')[0],
            'character': read(actor_base + 1, 'B')[0],
            'actor_position': read(actor_base + 8, '3f'),
            'actor_heading_units': read(actor_base + 0x14, 'I')[0],
            'pose_header': read(pose, '3I'), 'channels': read(pose + 12, '60f'),
            'after_channels': read(pose + 12, '60f', after), 'layers': layers,
            'offsets': [read(rig + i*16, '3f') for i in range(24)],
            'actor_state_before': before[context:context+0x180].hex(),
            'actor_state_after': after[context:context+0x180].hex(),
            'limbs': [list(read(read(context+0x44+i*0x4c, 'I')[0], '24B')) for i in range(4)],
            'alternate_offsets': [read(read(0x2d0c98, 'I')[0]+i*16, '3f') for i in range(24)],
            'terrain_disabled': read(0x4d96d8+actor, 'B')[0],
            'derived_enabled': read(0x4cb648+actor, 'B')[0],
            'look_target': read(0x416630, '3f'),
            'bone_worlds': [read(bank + i*64, '16f', after) for i in range(32)],
            'draw_bone_worlds': (list(struct.iter_unpack('<16f', draw_bank))
                                 if draw_bank is not None else None),
            'palette_recipe': preg[0], 'palette_offsets_address': preg[5],
            'palette_offsets': [read(preg[5]+i*16, '3f') for i in range(24)],
            'palette': list(struct.iter_unpack('<16f', palette)),
            'cursor_note': 'Layer cursor may already be advanced; pose header/scalars are captured inputs'}


def running_runners():
    command = ('Get-Process recomp_program_runner -ErrorAction SilentlyContinue | '
               'ForEach-Object { $_.Id }; exit 0')
    result = subprocess.run(['powershell', '-NoProfile', '-Command', command],
                            capture_output=True, text=True, check=True)
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', required=True, type=Path)
    parser.add_argument('--xbe', required=True, type=Path)
    parser.add_argument('--cdb', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--frame', type=int, default=1200)
    parser.add_argument('--actor', type=int, default=0)
    parser.add_argument('--timeout', type=int, default=55)
    args = parser.parse_args()
    try:
        if os.name != 'nt' or not 1 <= args.timeout <= 60:
            raise ValueError('Windows capture requires a timeout of at most 60 seconds')
        output = private_output(args.output)
        if output.exists():
            raise ValueError('Capture output must be new')
        for path in (args.runner, args.xbe, args.cdb, args.runner.with_suffix('.pdb')):
            if not path.is_file():
                raise ValueError(f'Missing capture input: {path}')
        scripts = debugger_scripts(output, args.actor, args.frame)
        output.mkdir(parents=True)
        for name, script in scripts.items():
            # $>< folds newlines into semicolons; a trailing one is invalid after q.
            (output / name).write_text(script, encoding='utf-8', newline='\n')
        deadline = time.monotonic() + 120 * 60
        while running_runners():
            if time.monotonic() >= deadline:
                raise ValueError('Shared runner remained busy for 120 minutes')
            print('Shared runner busy; waiting 60 seconds', flush=True)
            time.sleep(60)
        env = os.environ.copy()
        # Keep renderer options explicit; no host-input handover in this demo.
        env.update(RECOMP_D3D_SCALE='4.5', RECOMP_D3D_MSAA='8', RECOMP_D3D_SMAA='1')
        argv = [str(args.cdb.resolve()), '-y', str(args.runner.resolve().parent),
                '-cf', str(output / 'start.cdb'), str(args.runner.resolve()),
                '--xbe', str(args.xbe.resolve()), '--vsync',
                '--input-start-pulse-at', '6', '--input-start-pulse-at', '8']
        receipt = {'runner_sha256': digest(args.runner), 'xbe_sha256': digest(args.xbe),
                   'argv': argv, 'requested_frame': args.frame, 'actor': args.actor,
                   'host_input': False, 'timeout_seconds': args.timeout}
        started = time.monotonic()
        with (output / 'debugger.log').open('wb') as log:
            process = subprocess.Popen(argv, cwd=args.xbe.resolve().parent, env=env,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                receipt['exit_code'] = process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                receipt['timeout'] = True
                # Only terminate the debugger tree launched by this invocation.
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                               stdout=log, stderr=subprocess.STDOUT, check=False)
                process.wait(timeout=10)
        receipt['elapsed_seconds'] = time.monotonic() - started
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n',
                                             encoding='utf-8', newline='\n')
        if not (output / 'palette.bin').is_file():
            raise ValueError('Capture did not reach the palette return; inspect private debugger log')
        summary = snapshot_summary((output / 'before.bin').read_bytes(),
                                   (output / 'after.bin').read_bytes(), args.actor,
                                   (output / 'palette-registers.bin').read_bytes(),
                                   (output / 'palette.bin').read_bytes(),
                                   (output / 'draw-bank.bin').read_bytes())
        summary['snapshot_sha256'] = {name: digest(output / name)
                                     for name in ('before.bin', 'after.bin', 'palette.bin')}
        markers = re.findall(r'MODEL_POSE frame=(\d+)',
                           (output / 'debugger.log').read_text(errors='replace'))
        if not markers:
            raise ValueError('Missing captured frame identity')
        summary['frame'] = int(markers[-1])
        (output / 'capture.json').write_text(json.dumps(summary, indent=2, allow_nan=False) + '\n',
                                             encoding='utf-8', newline='\n')
        print(f'Matched pose and palette capture saved to {output}')
    except (ValueError, OSError, subprocess.SubprocessError, struct.error) as error:
        parser.exit(1, f'model capture: {error}\n')


if __name__ == '__main__':
    main()
