"""ROM-free continuation tests against the actual shared MSU mixer."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('fixture', ['msu_playback_state_test', 'msu_track_resolver_test'])
def test_msu_playback(tmp_path, fixture):
    compiler = os.environ.get('CC') or shutil.which('gcc') or shutil.which('cc')
    if not compiler:
        pytest.skip('C compiler required')
    exe = tmp_path / (fixture + ('.exe' if os.name == 'nt' else ''))
    subprocess.run([compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-UNDEBUG',
                    '-I' + str(ROOT / 'runner/src'), str(ROOT / 'tests/audio' / (fixture + '.c')),
                    str(ROOT / 'runner/src/snes/msu1.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=tmp_path, check=True, timeout=30)
