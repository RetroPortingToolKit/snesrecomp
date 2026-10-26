"""Setting the X flag clears the high bytes of X and Y (65816).

The interpreter does this in interp816_setFlags (SEP/PLP/RTI/XCE). Generated
code only zero-extended 8-bit X/Y writes, so a register that kept its 16-bit
value across SEP #$10 still held the old high byte. An interrupt handler that
switched back to 16-bit and pushed X then stored that stale byte."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest
from v2 import codegen
from v2.ir import XCE

ROOT = Path(__file__).resolve().parents[2]


def _compiler():
    return os.environ.get('CC') or ('C:/msys64/mingw64/bin/gcc.exe'
        if Path('C:/msys64/mingw64/bin/gcc.exe').is_file() else shutil.which('cc'))


def test_x_flag_set_clears_index_high_bytes(tmp_path):
    compiler = _compiler()
    if not compiler:
        pytest.skip('C compiler required')
    xce = "\n".join("    " + ln for ln in codegen.emit_op(XCE()))
    source = f'''
#include <stdio.h>
#include "cpu_state.h"
#include "cpu_trace.h"

static int check(CpuState *cpu, unsigned x, unsigned y, const char *what) {{
    if (cpu->X != x || cpu->Y != y) {{
        printf("%s: X=%04X Y=%04X exp %04X/%04X\\n", what, cpu->X, cpu->Y, x, y);
        return 1;
    }}
    return 0;
}}

static void xce(CpuState *cpu) {{
{xce}
}}

int main(void) {{
    CpuState c = {{0}};
    int fails = 0;
    /* SEP #$10 / PLP / RTI all end in cpu_p_to_mirrors. */
    c.X = 0xF000; c.Y = 0x1234; c.P = 0x30;
    cpu_p_to_mirrors(&c);
    fails += check(&c, 0x0000, 0x0034, "x set");
    /* x clear keeps the 16-bit values. */
    c.X = 0xF000; c.Y = 0x1234; c.P = 0x20;
    cpu_p_to_mirrors(&c);
    fails += check(&c, 0xF000, 0x1234, "x clear");
    /* XCE into emulation forces x = 1. */
    c.X = 0xABCD; c.Y = 0x5678; c.emulation = 0; c._flag_C = 1; c.x_flag = 0;
    xce(&c);
    fails += check(&c, 0x00CD, 0x0078, "xce");
    return fails;
}}
'''
    c = tmp_path / 'index_high.c'
    c.write_text(source, encoding='utf-8')
    exe = tmp_path / ('index_high.exe' if os.name == 'nt' else 'index_high')
    build = subprocess.run([compiler, '-std=c11', '-I', str(ROOT / 'runner' / 'src'),
                            '-I', str(ROOT / 'runner' / 'src' / 'snes'), str(c), '-o', str(exe)],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout
