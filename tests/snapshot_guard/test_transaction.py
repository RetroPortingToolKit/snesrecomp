"""Production snapshot walkers and loader, exercised without ROM or SDL."""
from pathlib import Path
import shutil
import subprocess
import pytest
ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'runner/src'

def function(source, name):
    start = source.index(name)
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

def test_rejected_snapshot_preserves_machine_and_host(tmp_path):
    cc = shutil.which('gcc') or shutil.which('cc')
    if not cc:
        pytest.skip('C compiler required')
    rtl = (RUNTIME / 'common_rtl.c').read_text(encoding='utf-8')
    snes = (RUNTIME / 'snes/snes.c').read_text(encoding='utf-8')
    parts = [Path(__file__).with_name('transaction_fixture.c').read_text(encoding='utf-8')]
    for file, name in [('cpu','cpu'),('apu','apu'),('dsp','dsp'),('spc','spc'),('dma','dma'),('ppu','ppu')]:
        parts.append(function((RUNTIME / f'snes/{file}.c').read_text(encoding='utf-8'), f'{name}_saveload('))
    parts.append(snes[snes.index('static uint32_t s_saveload_version'):snes.index('void snes_reset(')])
    parts.append(rtl[rtl.index('#define RTL_SAV_MAGIC'):rtl.index('/* Host-side state that paces')])
    parts.append(rtl[rtl.index('bool RtlSaveSnapshot('):rtl.rfind('/*', 0, rtl.index('See common_rtl.h for why the guest'))])
    parts.append(Path(__file__).with_name('transaction_test.c').read_text(encoding='utf-8'))
    source = tmp_path / 'transaction.c'
    source.write_text('\n'.join(parts), encoding='utf-8')
    exe = tmp_path / 'transaction.exe'
    result = subprocess.run([cc, '-std=c11', '-O1', '-I'+str(RUNTIME), '-I'+str(RUNTIME/'snes'),
                            '-include', str(RUNTIME/'snapshot_guard.h'), str(source),
                            str(RUNTIME/'snes/joypad.c'), str(RUNTIME/'snapshot_guard.c'), str(RUNTIME/'crc32.c'), '-o', str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(exe)], cwd=tmp_path, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr

