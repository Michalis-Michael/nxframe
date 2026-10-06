#!/usr/bin/env python3
"""Sequential, repeated HEVC experiments; preserves the supplied preset on disk."""
import argparse
import json
import os
from pathlib import Path
import random
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='./build/bench_encoder_x265')
    parser.add_argument('--preset', default='-', help='- uses the built-in superfast 422/10-bit baseline')
    parser.add_argument('--frames', type=int, default=500)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--groups', default='asm,threads,unpack,conversion')
    parser.add_argument('--results', default='hevc_benchmark.jsonl')
    args = parser.parse_args()
    if not 10 <= args.frames <= 10000 or not 1 <= args.repeats <= 20:
        parser.error('frames must be 10..10000; repeats must be 1..20')
    groups = set(args.groups.split(','))
    if not groups <= {'asm', 'threads', 'unpack', 'conversion'}:
        parser.error('unknown benchmark group')
    available = len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else os.cpu_count() or 1
    cases = [('baseline', [], None)]
    if 'asm' in groups:
        cases += [('asm_' + mode, ['--asm=' + mode], None) for mode in ('avx2', 'avx512')]
    if 'threads' in groups:
        for pool in sorted({max(1, available // 2), max(1, available - 2)}):
            for frames in (2, 4):
                cases.append((f'frame{frames}_pool{pool}',
                              [f'--frame-threads={frames}', f'--pools={pool}'], None))
    if 'unpack' in groups:
        # Each subprocess constructs a fresh pool so the environment takes effect.
        # These cases measure unpack with concurrent x265 work, not an isolated kernel.
        for mode in ('avx2', 'avx512'):
            for helpers in (0, 1, 2):
                cases.append((f'unpack_{mode}_helpers{helpers}', ['--unpack=' + mode], helpers))
    if 'conversion' in groups:
        cases += [('output_' + mode, ['--output=' + mode], None) for mode in ('42010', '4208')]
    results = Path(args.results)
    errors = 0
    with results.open('w') as output, Path(str(results) + '.log').open('w') as log:
        for repeat in range(args.repeats):
            order = cases.copy()
            random.Random(265 + repeat).shuffle(order)
            for name, options, helpers in order:
                env = os.environ.copy()
                if helpers is not None:
                    env['NXFRAME_V210_THREADS'] = str(helpers)
                command = [args.binary, args.preset, str(args.frames), '--realtime'] + options
                print(f'[{repeat + 1}/{args.repeats}] {name}', flush=True)
                try:
                    run = subprocess.run(command, env=env, capture_output=True, text=True,
                                         timeout=max(60, args.frames / 50 * 5 + 30))
                    log.write(f'\n{name} repeat={repeat + 1}\n{run.stderr}\n{run.stdout}')
                    log.flush()
                    payloads = [line[7:] for line in run.stdout.splitlines() if line.startswith('RESULT ')]
                    record = json.loads(payloads[-1]) if payloads else {}
                    unsupported = ('unavailable on this CPU/OS/build' in run.stderr or
                                   'unsupported unpack path:' in run.stderr)
                    status = 'ok' if run.returncode == 0 and payloads else 'unsupported' if unsupported else 'error'
                    record.update(case=name, repeat=repeat + 1, status=status, returncode=run.returncode,
                                  options=options, unpack_helpers=helpers, affinity_cpus=available)
                except (OSError, subprocess.TimeoutExpired, ValueError) as error:
                    record = dict(case=name, repeat=repeat + 1, status='error', error=str(error))
                if record['status'] == 'error':
                    errors += 1
                output.write(json.dumps(record) + '\n')
                output.flush()
                if record['status'] == 'ok':
                    print(f"  call p95={record['encode_call_p95_ms']:.3f}ms "
                          f"p99={record['encode_call_p99_ms']:.3f}ms "
                          f"input-to-packet p95={record.get('input_to_packet_p95_ms', 0):.1f}ms "
                          f"late inputs={record['late_inputs_gt1ms']}", flush=True)
                else:
                    print('  ' + record['status'], flush=True)
    print(f'Results: {results}; codec logs: {results}.log')
    return 2 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
