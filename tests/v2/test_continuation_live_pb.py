"""Execute emitted cooperative-entry and code-clock logic through ROM mirrors."""
import os
from pathlib import Path
import re
import shutil
import subprocess

import pytest
from v2.emit_function import emit_function


def test_emitted_wait_and_deadline_continuations_keep_live_bank(tmp_path):
    compiler = os.environ.get('CC') or ('C:/msys64/mingw64/bin/gcc.exe'
        if Path('C:/msys64/mingw64/bin/gcc.exe').is_file() else shutil.which('cc'))
    if not compiler:
        pytest.skip('C compiler required')
    code = bytes.fromhex('08 E2 20 AD B8 05 CD B8 05 F0 FB 28 60')
    generated = emit_function(code + bytes(0x8000 - len(code)), bank=0,
                             start=0x8000, entry_m=0, entry_x=0, end=0x800D,
                             func_name='Wait')
    # Execute the actual entry guards; both paths return before the body.
    entry = generated.split('  RecompReturn _pending_skip')[0] + '  assert(0); return 0;\n}\n'
    # Execute an actual generated charge, including the live FastROM condition.
    charge = re.search(r'cpu->master_cycles \+= (\d+) \* ([^;]+);', generated)
    assert charge
    cycles = int(charge[1])
    source = r'''
#include <assert.h>
#include <stdint.h>
typedef uint32_t uint32;
typedef int RecompReturn;
typedef struct { uint8_t PB; uint16_t S; uint64_t master_cycles; } CpuState;
const char *g_last_recomp_func;
static unsigned g_memsel, deadline, depth;
static uint32 resume;
#define RecompStackPush(...) (++depth)
#define RecompStackPopYield() (--depth)
#define cpu_dbg_funcname(...) ((void)0)
#define cpu_trace_func_entry(...) ((void)0)
static int interp_bridge_lle_master_deadline_reached(CpuState *cpu) {
    (void)cpu; return deadline;
}
static int interp_bridge_in_lle_scheduler(void) { return 1; }
static RecompReturn interp_bridge_lle_yield_unwind(CpuState *cpu, uint32 pc) {
    (void)cpu; resume = pc; return 99;
}
''' + entry + r'''
static void charge(CpuState *cpu) {
''' + charge[0] + r'''
}
int main(void) {
    for (unsigned bank = 0; bank <= 0x80; bank += 0x80) {
        for (deadline = 0; deadline < 2; deadline++) {
            CpuState cpu = {bank, 0x1ffc, 0};
            assert(Wait_M0X0(&cpu) == 99);
            assert(resume == ((bank << 16) | 0x8000));
            assert(cpu.S == 0x1ffc && cpu.PB == bank && depth == 0);
            for (g_memsel = 0; g_memsel < 2; g_memsel++) {
                cpu.master_cycles = 0;
                charge(&cpu);
                const unsigned expected_speed = (bank == 0x80 && g_memsel) ? 6 : 8;
''' + f'                assert(cpu.master_cycles == {cycles} * expected_speed);\n' + r'''
            }
        }
    }
}
'''
    c = tmp_path / 'continuation.c'
    c.write_text(source, encoding='utf-8')
    exe = tmp_path / ('continuation.exe' if os.name == 'nt' else 'continuation')
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(c), '-o', str(exe)], check=True, capture_output=True)
    subprocess.run([str(exe)], check=True, capture_output=True)
