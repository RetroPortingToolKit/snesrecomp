"""Execute emitted tail wrappers with the production ancestor resolvers.

Host tail frames share a guest return frame: neither raw host depth nor an
extra decrement in the wrapper represents the SKIP contract correctly.
"""
from pathlib import Path
import re
import shutil
import subprocess

import pytest
from _helpers import make_lorom_bank0
from v2 import codegen
from v2.emit_function import emit_function

ROOT = Path(__file__).resolve().parents[2]


def _production_function(source, name):
    match = re.search(rf'^(?:static )?(?:int|void) {name}\([^\n]*\) \{{.*?^\}}',
                      source, re.MULTILINE | re.DOTALL)
    assert match, name
    return match.group()


def _harness():
    saved = dict(codegen._NAME_RESOLVER)
    try:
        codegen.set_name_resolver({0x8001: 'TailTarget'})
        emitted = emit_function(
            make_lorom_bank0({0x8000: bytes([0xEA, 0x60])}),
            bank=0, start=0x8000, end=0x8001, entry_m=1, entry_x=1,
            func_name='TailOwner')
    finally:
        codegen.set_name_resolver(saved)
    statement = next(line for line in emitted.splitlines()
                     if 'tail-call past end:' in line)
    infra = (ROOT / 'runner/src/common_cpu_infra.c').read_text(encoding='utf-8')
    context = infra[infra.index('uint16_t g_cpu_entry_s['):
                    infra.index('/* ── Unresolved-site')]
    resolvers = '\n'.join(_production_function(infra, name) for name in (
        'cpu_resolve_unwind_depth', 'recomp_dump_frame_array', 'cpu_resolve_ancestor_skip',
        'cpu_resolve_post_return_skip'))
    fixture = Path(__file__).with_name('tail_unwind_fixture.c').read_text()
    return (fixture.replace('/* PRODUCTION_CONTEXT */', context)
            .replace('/* PRODUCTION_RESOLVERS */', resolvers)
            .replace('/* GENERATED_TAIL */', statement))


def test_tail_chains_preserve_guest_unwind_depth(tmp_path):
    cc = shutil.which('gcc') or shutil.which('cc')
    if cc is None:
        pytest.skip('C11 compiler unavailable')
    source = tmp_path / 'tail_unwind.c'
    source.write_text(_harness(), encoding='utf-8')
    exe = tmp_path / 'tail_unwind.exe'
    subprocess.run([cc, '-std=c11', '-O1', '-Wall', '-Wextra',
                    f'-I{ROOT / "runner/src"}', str(source), '-o', str(exe)],
                   check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
