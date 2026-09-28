import pathlib
import shutil
import subprocess
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_rdnmi_latch_and_snapshot_contract(tmp_path):
    cc = shutil.which('gcc') or shutil.which('cc')
    if cc is None:
        pytest.skip('C compiler unavailable')
    exe = tmp_path / 'rdnmi.exe'
    subprocess.run([cc, '-std=c11', '-O1', '-DSNESRECOMP_REVERSE_DEBUG=0',
        '-ffunction-sections', '-fdata-sections', '-Irunner/src',
        '-Irunner/src/snes', '-Itests/dma', 'tests/dma/rdnmi_test.c',
        'runner/src/snes/snes.c', 'runner/src/snes/dma.c',
        'runner/src/snes/sdd1.c', '-lm', '-o', str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], check=True)
