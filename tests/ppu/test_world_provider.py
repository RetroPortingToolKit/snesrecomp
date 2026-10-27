"""Compile the ROM-free production margin lookup on Windows and Unix."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import pytest

ROOT = Path(__file__).resolve().parents[2]

@pytest.mark.parametrize('source', ['ws_world_provider_test.c', 'mmx23_stream_test.c'])
def test_world_provider(source):
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
    if not cc and os.name == 'nt':
        mingw = Path('C:/msys64/mingw64/bin/gcc.exe')
        if mingw.is_file():
            cc = str(mingw)
    if not cc:
        pytest.skip('C11 compiler required')
    with tempfile.TemporaryDirectory(prefix='ws-provider-') as tmp:
        exe = Path(tmp) / ('test.exe' if os.name == 'nt' else 'test')
        subprocess.run([cc, '-std=c11', '-O2', '-Irunner/src',
                        'tests/ppu/'+source,
                        'runner/src/snes/ws_shadow.c', '-o', str(exe)],
                       cwd=ROOT, check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    for source in ['ws_world_provider_test.c', 'mmx23_stream_test.c']:
        test_world_provider(source)
