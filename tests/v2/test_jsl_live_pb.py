"""Execute emitted JSL frames against the CPU's architectural stack contract."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest
from v2.codegen import _emit_return_frame_push
from v2.ir import Call


def test_jsl_frame_preserves_live_program_bank_through_rom_mirrors(tmp_path):
    compiler = os.environ.get('CC') or ('C:/msys64/mingw64/bin/gcc.exe'
        if Path('C:/msys64/mingw64/bin/gcc.exe').is_file() else shutil.which('cc'))
    if not compiler:
        pytest.skip('C compiler required')
    frame = '\n'.join(_emit_return_frame_push(Call(
        target=0x918000, long=True, source_pc24=0x0BA100)))
    source = '''
#include <assert.h>
#include <stdint.h>
typedef uint16_t uint16;
typedef struct { uint16 S; uint8_t PB, host_return_valid; } CpuState;
static uint8_t ram[65536];
static void cpu_write8(CpuState *cpu, unsigned bank, uint16 address, uint8_t value) {
  (void)cpu; assert(bank == 0); ram[address] = value;
}
static void generated(CpuState *cpu) {
''' + frame + '''
}
int main(void) {
  /* Same generated $0B:A100 body, two valid execution banks. */
  const uint8_t banks[] = {0x0b, 0x8b};
  for (unsigned i = 0; i < 2; ++i) {
    CpuState cpu = {0x1fff, banks[i], 0};
    generated(&cpu);
    assert(cpu.S == 0x1ffc && cpu.host_return_valid == 3);
    /* W65C816S JSL bus sequence: PBR, PCH, PCL; return is PC+3. */
    assert(ram[0x1fff] == banks[i]);
    assert(ram[0x1ffe] == 0xa1 && ram[0x1ffd] == 0x03);
  }
  return 0;
}
'''
    c = tmp_path / 'jsl.c'
    c.write_text(source, encoding='utf-8')
    exe = tmp_path / ('jsl.exe' if os.name == 'nt' else 'jsl')
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(c), '-o', str(exe)], check=True, capture_output=True)
    subprocess.run([str(exe)], check=True, capture_output=True)
