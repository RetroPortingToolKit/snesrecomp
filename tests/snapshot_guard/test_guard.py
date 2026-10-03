"""Exercise the optional state envelope without a ROM or SDL."""
from pathlib import Path
import os
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


def test_envelope():
    cc = _compiler()
    if not cc:
        pytest.skip('a C11 compiler is required')
    with tempfile.TemporaryDirectory(prefix='snes-state-guard-') as build:
        exe = Path(build) / ('guard.exe' if os.name == 'nt' else 'guard')
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'runner/src'),
                        str(ROOT / 'runner/src/snapshot_guard.c'),
                        str(ROOT / 'runner/src/crc32.c'),
                        str(Path(__file__).with_name('guard_test.c')),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    test_envelope()
