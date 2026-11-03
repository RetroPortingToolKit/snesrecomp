"""Build both maintained legacy Mode7 implementations and compare their contract.

Usage: python tests/ppu/run_legacy_mode7.py --cc /path/to/gcc
The temporary build is independent of the product's CMake cache and removed
after the focused test. No ROM, generated title source or GPU is required.
"""
import argparse
from pathlib import Path
import os
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc'))
    args = parser.parse_args()
    if not args.cc:
        parser.error('provide a C11 compiler with --cc or CC')
    root = Path(__file__).resolve().parents[2]
    flags = ['-std=c11', '-O3', '-DNDEBUG', '-DSNESRECOMP_REVERSE_DEBUG=0',
             '-I'+str(root/'runner/src'), '-I'+str(root/'runner/src/snes')]
    with tempfile.TemporaryDirectory(prefix='snesrecomp-mode7-') as directory:
        build = Path(directory)
        source = root/'runner/src/snes/ppu_legacy.c'
        subprocess.run([args.cc, *flags, '-DSNESRECOMP_LEGACY_MODE7_NATIVE=0',
                        '-Dppu_draw_whole_line_legacy=ppu_draw_whole_line_reference',
                        '-c', str(source), '-o', str(build/'reference.o')], check=True)
        subprocess.run([args.cc, *flags, '-DSNESRECOMP_LEGACY_MODE7_NATIVE=1',
                        '-c', str(source), '-o', str(build/'native.o')], check=True)
        executable = build/('contract.exe' if os.name == 'nt' else 'contract')
        subprocess.run([args.cc, *flags, str(root/'tests/ppu/ppu_legacy_mode7_test.c'),
                        str(build/'reference.o'), str(build/'native.o'), '-lm',
                        '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
