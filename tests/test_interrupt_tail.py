"""Subroutine fallback inside an interrupt must preserve the return owner."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('fixture', ['interrupt_tail_test.c', 'bridge_test.c'])
def test_interrupt_tail_ownership(tmp_path, fixture):
    cc = shutil.which('gcc') or shutil.which('cc')
    if cc is None:
        pytest.skip('C11 compiler unavailable')
    exe = tmp_path / 'bridge.exe'
    subprocess.run([
        cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1',
        '-DSNESRECOMP_TIER2_TEST=1', '-Irunner/src', '-Irunner/src/snes',
        f'tests/interp816/{fixture}', 'runner/src/snes/interp816.c',
        'runner/src/snes/interp_bridge.c', 'runner/src/snes/tier2_capture.c',
        'runner/src/sha256.c', 'runner/src/snes/cx4.c', '-lm', '-o', str(exe),
    ], cwd=ROOT, check=True, capture_output=True, text=True)
    run = subprocess.run([str(exe)], cwd=tmp_path, capture_output=True,
                         text=True, timeout=60)
    assert run.returncode == 0, run.stdout + run.stderr
