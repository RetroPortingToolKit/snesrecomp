"""Exercise claimed ROM reads and unclaimed device reads on the real CPU bus."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import pytest


def test_plain_rom_read_bus_contract():
    repo = Path(__file__).resolve().parents[2]
    compiler = 'C:/msys64/mingw64/bin/gcc.exe' if os.name == 'nt' else shutil.which('cc')
    if not compiler or not Path(compiler).is_file():
        pytest.skip('C compiler unavailable')
    with tempfile.TemporaryDirectory(prefix='snes-plain-rom-') as directory:
        exe = Path(directory) / ('contract.exe' if os.name == 'nt' else 'contract')
        flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter',
                 '-ffunction-sections', '-fdata-sections', '-I', 'runner/src',
                 '-I', 'runner/src/snes']
        flags += ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']
        subprocess.run([compiler, *flags,
                        'tests/runtime_dispatch/cart_cpu_bus_latch_test.c',
                        'runner/src/cpu_state.c', '-o', str(exe)],
                       cwd=repo, check=True, timeout=120)
        subprocess.run([str(exe)], cwd=directory, check=True, timeout=30)
