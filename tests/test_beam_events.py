import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_beam_events_and_midspan_sync(tmp_path):
    cc = shutil.which('gcc') or shutil.which('cc')
    if cc is None:
        pytest.skip('C compiler unavailable')
    exe = tmp_path / 'beam_events.exe'
    subprocess.run([cc, '-std=c11', '-O1', '-DSNESRECOMP_REVERSE_DEBUG=0',
        '-ffunction-sections', '-fdata-sections', '-Irunner/src',
        '-Irunner/src/snes', '-Itests/dma', 'tests/dma/beam_events_test.c',
        'runner/src/snes/snes.c', 'runner/src/snes/dma.c',
        'runner/src/snes/sdd1.c', '-lm', '-o', str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], check=True)



@pytest.mark.parametrize('mode', ['on', 'off'])
def test_short_line(tmp_path, mode):
    cc = shutil.which('gcc') or shutil.which('cc')
    if cc is None:
        pytest.skip('C compiler unavailable')
    exe = tmp_path / 'short_line.exe'
    subprocess.run([cc, '-std=c11', '-O1', '-DSNESRECOMP_REVERSE_DEBUG=0',
        '-ffunction-sections', '-fdata-sections', '-Irunner/src',
        '-Irunner/src/snes', '-Itests/dma', 'tests/dma/short_line_test.c',
        'runner/src/snes/snes.c', 'runner/src/snes/dma.c',
        'runner/src/snes/sdd1.c', '-lm', '-o', str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe), mode], check=True)


def test_render_hdma_first_slot(tmp_path):
    cc=shutil.which('gcc') or shutil.which('cc')
    if cc is None:pytest.skip('C compiler unavailable')
    source=(ROOT/'runner/src/beam_frame_driver.c').read_text(encoding='utf-8')
    parts=['#include "beam_frame_driver.h"\n#include "snes/snes.h"\n#include "snes/ppu.h"\n#include "snes/dma.h"\nextern Snes *g_snes;\nextern Ppu *g_ppu;\n']
    for name in ('void snes_beam_frame_driver_draw_ppu_frame(',
                 'void snes_beam_frame_driver_draw_ppu_frame_observed('):
        start=source.index(name);brace=source.index('{',start);depth=1;end=brace+1
        while depth:
            depth+=(source[end]=='{')-(source[end]=='}');end+=1
        parts.append(source[start:end])
    driver=tmp_path/'render.c';driver.write_text('\n'.join(parts),encoding='utf-8')
    exe=tmp_path/'frame-render.exe'
    subprocess.run([cc,'-std=c11','-O1','-DSNESRECOMP_REVERSE_DEBUG=0',
        '-Irunner/src','-Irunner/src/snes','-Itests/dma',
        'tests/dma/frame_render_hdma_test.c',str(driver),'runner/src/snes/snes.c',
        'runner/src/snes/dma.c','runner/src/snes/sdd1.c','-lm','-o',str(exe)],cwd=ROOT,check=True)
    subprocess.run([str(exe)],check=True)


@pytest.mark.parametrize('test,extra', [
    ('dma/hvbjoy_refresh_test.c', []),
    ('dma/last_cycle_test.c', ['runner/src/snes/last_cycle.c']),
    ('frame_driver/exact_rules_test.c', []),
])
def test_exact_rules(tmp_path, test, extra):
    cc = shutil.which('gcc') or shutil.which('cc')
    if cc is None:
        pytest.skip('C compiler unavailable')
    exe = tmp_path / 'exact.exe'
    subprocess.run([cc, '-std=c11', '-O1', '-DSNESRECOMP_REVERSE_DEBUG=0',
        '-ffunction-sections', '-fdata-sections', '-Irunner/src',
        '-Irunner/src/snes', '-Itests/dma', 'tests/' + test, *extra,
        'runner/src/snes/snes.c', 'runner/src/snes/dma.c',
        'runner/src/snes/sdd1.c', '-lm', '-o', str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], check=True)
