"""Execute direct and recovered indirect short calls through both ROM mirrors."""
import os
from pathlib import Path
import shutil
import subprocess
from types import SimpleNamespace

import pytest
from v2 import codegen
from v2.ir import Call


@pytest.mark.parametrize('form', ['direct', 'index', 'pointer'])
def test_short_call_aot_and_fallback_keep_live_bank(tmp_path, form):
    compiler = os.environ.get('CC') or ('C:/msys64/mingw64/bin/gcc.exe'
        if Path('C:/msys64/mingw64/bin/gcc.exe').is_file() else shutil.which('cc'))
    if not compiler:
        pytest.skip('C compiler required')
    variants, authority = codegen._VALID_VARIANTS, codegen._VALID_VARIANTS_AUTHORITATIVE
    names = dict(codegen._NAME_RESOLVER)
    try:
        codegen.set_valid_variants({0x009000: frozenset({(1, 1)})}, authoritative=True)
        codegen.set_name_resolver({0x009000: 'Target'})
        if form == 'direct':
            lines = codegen.emit_op(Call(target=0x009000, long=False, source_pc24=0x008000))
        else:
            insn = SimpleNamespace(addr=0x008000, operand=0x8100, mnem='JSR',
                mode=codegen.INDIR_X, dispatch_entries=(0x009000,),
                dispatch_kind='short', dispatch_call=form == 'pointer',
                dispatch_table_bases=(0x8100,) if form == 'pointer' else (),
                m_flag=1, x_flag=1)
            lines = codegen._emit_indirect_dispatch(insn)
    finally:
        codegen.set_valid_variants(variants, authoritative=authority)
        codegen.set_name_resolver(names)
    source = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int RecompReturn;
#define RECOMP_RETURN_NORMAL 0
#define cpu_trace_event(...) ((void)0)
#define cpu_trace_mark_nlr_exit(...) ((void)0)
typedef struct { uint16 S, X; uint8 PB, m_flag, x_flag, host_return_valid; } CpuState;
static uint32 seen;
static int aot_calls, lle_calls;
static uint8 ram[65536];
static void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 value) {
    (void)cpu; assert(bank == 0); ram[addr] = value;
}
static uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    assert(bank == cpu->PB && addr == 0x8100); return 0x9000;
}
static int cpu_trace_dispatch_oob(CpuState *cpu, uint32 pc, uint16 target) {
    (void)cpu; (void)pc; (void)target; assert(0); return 0;
}
static RecompReturn Target_M1X1(CpuState *cpu) {
    aot_calls++; seen = ((uint32)cpu->PB << 16) | 0x9000;
    cpu->S += 2; return 0;
}
static RecompReturn interp_tier_run_call_frame(CpuState *cpu, uint32 target,
        uint32 source, uint8 size, void *unused) {
    (void)unused; assert(source == 0x008000 && size == 2);
    lle_calls++; seen = target; cpu->S += 2; return 0;
}
static RecompReturn generated(CpuState *cpu) {
''' + '\n'.join(lines) + r'''
    return 0;
}
int main(void) {
    for (unsigned bank = 0; bank <= 0x80; bank += 0x80) {
        for (unsigned compiled = 0; compiled < 2; compiled++) {
            CpuState cpu = {0x1fff, 0, bank, compiled, 1, 0};
            assert(generated(&cpu) == 0);
            assert(seen == ((bank << 16) | 0x9000));
            assert(cpu.PB == bank && cpu.S == 0x1fff);
            assert(ram[0x1fff] == 0x80 && ram[0x1ffe] == 0x02);
        }
    }
    assert(aot_calls == 2 && lle_calls == 2);
}
'''
    c = tmp_path / 'short_call.c'
    c.write_text(source, encoding='utf-8')
    exe = tmp_path / ('short_call.exe' if os.name == 'nt' else 'short_call')
    subprocess.run([compiler, '-std=c11', str(c), '-o', str(exe)],
                   check=True, capture_output=True)
    subprocess.run([str(exe)], check=True, capture_output=True)
