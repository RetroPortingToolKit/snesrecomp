"""Build-fixed renderer qualification; timed runs require --bench explicitly."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess


def run(argv, **kwargs):
    return subprocess.run([str(a) for a in argv], check=True, text=True,
                          capture_output=True, **kwargs).stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', default=shutil.which('gcc'))
    ap.add_argument('--git', default=shutil.which('git'))
    ap.add_argument('--baseline', default='88a9f7f')
    ap.add_argument('--bench', action='store_true')
    ap.add_argument('--loops', type=int, default=2000)
    ap.add_argument('--affinity-mask', type=lambda s: int(s, 0),
                    help='Windows affinity mask inherited by benchmark children')
    args = ap.parse_args()
    if not args.cc or not args.git:
        ap.error('provide --cc and --git')
    root = Path(__file__).resolve().parents[2]
    build = root / 'build-ppu-hle'
    build.mkdir(exist_ok=True)
    baseline = build / 'ppu_baseline.c'
    baseline.write_text(run([args.git, '-C', root, 'show',
                             f'{args.baseline}:runner/src/snes/ppu.c']), encoding='utf-8')
    executables = {}
    digests = {}
    for arm in ('baseline', 'LLE', 'HLE'):
        exe = build / (arm.lower() + '.exe')
        defs = [f'-DSNESRECOMP_PPU_8BPP_HLE={int(arm == "HLE")}']
        if arm == 'baseline':
            defs.append(f'-DPPU_COMPOSITION_PPU_INCLUDE="{baseline.as_posix()}"')
        run([args.cc, '-std=c11', '-O3', '-DSNESRECOMP_REVERSE_DEBUG=0', *defs,
             '-I' + str(root / 'runner/src'), '-I' + str(root / 'runner/src/snes'),
             root / 'tests/ppu/ppu_8bpp_contract_test.c',
             root / 'runner/src/snes/ppu_legacy.c', '-o', exe],
            creationflags=getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0))
        executables[arm] = exe
        digests[arm] = run([exe])
    if len(set(digests.values())) != 1:
        raise RuntimeError(f'contract mismatch: {digests}')
    report = {
        'scope': 'synthetic 8bpp scanline component, excludes gameplay and other host work',
        'baseline_revision': run([args.git, '-C', root, 'rev-parse', args.baseline]),
        'compiler': run([args.cc, '--version']).splitlines()[0],
        'digests': digests,
        'binary_sha256': {a: hashlib.sha256(p.read_bytes()).hexdigest()
                          for a, p in executables.items()},
    }
    if args.bench:
        if args.affinity_mask is not None:
            if os.name != 'nt' or args.affinity_mask <= 0:
                ap.error('--affinity-mask requires Windows and a positive mask')
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.GetCurrentProcess.restype = ctypes.c_void_p
            kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            if not kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(), args.affinity_mask):
                raise ctypes.WinError(ctypes.get_last_error())
        report['affinity_mask'] = args.affinity_mask
        # One warmup per build, then balanced sequential arms. No output
        # hashing or fixture generation is inside the reported timing span.
        for a in ('LLE', 'HLE'):
            run([executables[a], '--bench', args.loops])
        pairs = []
        for n in range(6):
            arms = ('LLE', 'HLE') if n % 2 == 0 else ('HLE', 'LLE')
            pair = {a: json.loads(run([executables[a], '--bench', args.loops])) for a in arms}
            if pair['LLE']['hash'] != pair['HLE']['hash']:
                raise RuntimeError(f'benchmark output mismatch: {pair}')
            pair['time_reduction_pct'] = 100 * (1 - pair['HLE']['ms'] / pair['LLE']['ms'])
            pairs.append(pair)
        report['pairs'] = pairs
        report['median_time_reduction_pct'] = statistics.median(
            p['time_reduction_pct'] for p in pairs)
    path = build / ('measurement.json' if args.bench else 'contract.json')
    path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
