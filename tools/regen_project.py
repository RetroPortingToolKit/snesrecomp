#!/usr/bin/env python3
"""Recipe adapter for this maintained emitter line, preserving its runtime."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'recompiler'), str(ROOT / 'tools')]
from tools.generation_recipe import load_recipe
from tools.sdk_progress import ProgressReporter
from tools.sdk_rom import verify_rom
from v2.atomic_output import AtomicOutputDir, write_if_changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-root', type=Path, default=Path.cwd())
    parser.add_argument('--rom', type=Path)
    parser.add_argument('--out-dir', type=Path)
    parser.add_argument('--cfg-roots', action='store_true')
    parser.add_argument('--analysis-backend', choices=('native', 'auto'), default='native')
    parser.add_argument('--strict-idempotent', action='store_true')
    parser.add_argument('--no-tests', action='store_true')
    parser.add_argument('--tests', action='store_true')
    args = parser.parse_args()
    project = args.project_root.resolve()
    cfg = project / 'recomp'
    recipe = load_recipe(cfg)
    if recipe is None:
        parser.error('this maintained generation line requires recomp/generate.toml')
    supported = {'schema', 'identity_file', 'cfg_roots', 'no_host_root_scan', 'prepare', 'postprocess'}
    if recipe.data.keys() - supported or not recipe.data.get('no_host_root_scan'):
        parser.error('recipe requires capabilities unavailable on this maintained emitter line')
    identity = recipe.identity()
    rom = args.rom or os.environ.get('SNESRECOMP_ROM') or identity.get('rom_file')
    if not rom and (project / 'rom.cfg').is_file():
        lines = (project / 'rom.cfg').read_text(encoding='utf-8').splitlines()
        rom = lines[0] if lines else None
    if not rom:
        parser.error('pass --rom, set SNESRECOMP_ROM, or record the original path in rom.cfg')
    rom = Path(rom).expanduser()
    if not rom.is_absolute():
        rom = project / rom
    rom = rom.resolve()
    verify_rom(rom, expected_sha256=identity.get('expected_sha256'),
               expected_crc32=identity.get('expected_crc32'))
    output = args.out_dir or project / 'src/gen'
    if not output.is_absolute():
        output = project / output
    env = dict(os.environ, SNESRECOMP_ROOT=str(ROOT))
    reporter = ProgressReporter()
    header = cfg / 'funcs.h'
    old_header = header.read_bytes() if header.is_file() else None
    def command(argv):
        subprocess.run([sys.executable, *[str(a) for a in argv]], cwd=project, env=env, check=True)
    if not env.get('SNESRECOMP_NATIVE_ANALYZER'):
        command([ROOT / 'tools/build_native_analyzer.py'])
    def generate(target):
        transaction = AtomicOutputDir(target, seed_existing=False)
        try:
            emit = [ROOT / 'tools/v2_emit.py', '--rom', rom,
                    '--cfg-dir', cfg, '--out-dir', transaction.staging,
                    '--no-host-root-scan', '--analysis-backend', args.analysis_backend]
            if args.cfg_roots or recipe.data.get('cfg_roots'):
                emit.append('--cfg-roots')
            command(emit)
            recipe.run('postprocess', rom=rom, cfg_dir=cfg,
                       out_dir=transaction.staging, reporter=reporter)
            transaction.publish()
        finally:
            transaction.cleanup()
    try:
        recipe.run('prepare', rom=rom, cfg_dir=cfg, out_dir=output, reporter=reporter)
        # The old emitter hashes funcs.h as an input even with host discovery
        # disabled. Synchronize once before both runs for clean-checkout parity.
        command([ROOT / 'tools/v2_sync_funcs_h.py', '--cfg-dir', cfg, '--out', header])
        generate(output)
        if args.strict_idempotent:
            from tools.v2_compare_output import compare_output
            with tempfile.TemporaryDirectory(prefix='snesrecomp-regen-compare-') as temp:
                comparison = Path(temp) / 'generated'
                generate(comparison)
                differences = compare_output(output, comparison)
                if differences:
                    raise RuntimeError('regeneration differs:\n' + '\n'.join(differences))
                print('Complete generated trees are byte-identical')
    except BaseException:
        if old_header is None:
            header.unlink(missing_ok=True)
        else:
            write_if_changed(header, old_header.decode('utf-8'))
        raise
    if args.tests and not args.no_tests:
        command([ROOT / 'tests/run_tests.py'])


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f'regen_project: {exc}', file=sys.stderr)
        raise SystemExit(1)
