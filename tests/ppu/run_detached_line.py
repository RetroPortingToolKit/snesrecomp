#!/usr/bin/env python3
"""Run the ROM-free detached-line contract, including real debug callbacks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='snes-detached-line-') as folder:
        binary = Path(folder) / 'ppu_detached_line.exe'
        command = [args.cc, '-std=c11', '-O2', '-Wall', '-Wextra',
                   '-DSNESRECOMP_TRACE=1', '-DSNESRECOMP_REVERSE_DEBUG=0',
                   '-DSNESRECOMP_INTERP_PROFILE=1',
                   '-I' + str(root / 'runner/src'),
                   '-I' + str(root / 'runner/src/snes'),
                   str(root / 'tests/ppu/ppu_detached_line_test.c'),
                   str(root / 'runner/src/snes/ppu.c'),
                   str(root / 'runner/src/snes/ppu_legacy.c'),
                   '-o', str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
