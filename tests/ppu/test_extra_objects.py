"""Build/run the ROM-free extra-object PPU contract on Windows or Unix."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[2]


def _compiler():
    if os.name == 'nt':
        mingw = Path('C:/msys64/mingw64/bin/gcc.exe')
        return str(mingw) if mingw.is_file() else shutil.which('gcc')
    return shutil.which('cc') or shutil.which('gcc')


def test_extra_objects():
    cc = _compiler()
    if not cc:
        pytest.skip('a C11 compiler is required')
    with tempfile.TemporaryDirectory(prefix='ppu-extra-') as tmp:
        exe = Path(tmp) / ('test.exe' if os.name == 'nt' else 'test')
        subprocess.run([cc, '-std=c11', '-O2', '-DSNESRECOMP_REVERSE_DEBUG=0',
                        '-Irunner/src', '-Irunner/src/snes',
                        'tests/ppu/ppu_extra_object_test.c',
                        'runner/src/snes/ppu.c', 'runner/src/snes/ppu_legacy.c',
                        '-lm', '-o', str(exe)], cwd=ROOT, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    test_extra_objects()
