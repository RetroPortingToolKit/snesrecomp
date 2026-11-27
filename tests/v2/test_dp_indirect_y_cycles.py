"""Execute default emitted (dp),Y accesses against the real 65816 core.

interp816_adrIdy adds a cycle to a (dp),Y READ when the index is 16-bit or the
pointer plus Y crosses a page. The default emitter charges the 16-bit case
statically and the crossing at run time, previewing the pointer from low WRAM.
"""
from pathlib import Path
import os
import subprocess

import pytest
from _helpers import make_lorom_bank0
from snes65816 import ROM_MAP_LOROM, get_rom_mapping, set_rom_mapping
from v2.emit_function import emit_function


def test_default_dp_indirect_y_cpu_cycles_match_interpreter(tmp_path):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    # (name, code, m, x, pointer at $10, Y). All bytes are hand-authored.
    cases = [
        ('ReadCross', 'B1 10 60', 1, 1, 0x02F0, 0x20),
        ('ReadSamePage', 'B1 10 60', 1, 1, 0x0200, 0x10),
        ('StoreCross', '91 10 60', 1, 1, 0x02F0, 0x20),
        ('WideIndexRead', 'B1 10 60', 1, 0, 0x0200, 0x10),
    ]
    generated = []
    previous = get_rom_mapping()
    try:
        set_rom_mapping(ROM_MAP_LOROM)
        for name, code, m, x, _pointer, _y in cases:
            generated.append(emit_function(
                make_lorom_bank0({0x8000: bytes.fromhex(code)}),
                bank=0, start=0x8000, entry_m=m, entry_x=x, func_name=name))
    finally:
        set_rom_mapping(previous)
    repo = Path(__file__).resolve().parents[2]
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    harness += r'''
#include "snes/interp816.h"
int interp816_opcode_hook(uint32_t pc) { (void)pc; return 0; }
static uint8_t read_reference(void *memory, uint32_t address) {
  return cpu_read8(memory, (uint8)(address >> 16), (uint16)address);
}
static void write_reference(void *memory, uint32_t address, uint8_t value) {
  cpu_write8(memory, (uint8)(address >> 16), (uint16)address, value);
}
static void prepare(CpuState *cpu, const uint8 *code, unsigned length,
                    unsigned m, unsigned x, unsigned pointer, unsigned y) {
  memset(cpu, 0, sizeof *cpu); memset(g_ram, 0, sizeof g_ram);
  memcpy(g_ram + 0x8000, code, length);
  g_ram[0x10] = (uint8)pointer; g_ram[0x11] = (uint8)(pointer >> 8);
  cpu->ram = g_ram; cpu->S = 0x01ff; cpu->Y = (uint16)y;
  cpu->P = (m << 5) | (x << 4); cpu_p_to_mirrors(cpu);
  cpu_push_jsr_return_frame(cpu);
  g_other_exits = 0; g_tc_valid = 0; g_recomp_stack_top = 0;
}
static int check(const char *name, RecompReturn (*fn)(CpuState *),
                 const uint8 *code, unsigned length, unsigned m, unsigned x,
                 unsigned pointer, unsigned y) {
  CpuState native, memory;
  prepare(&native, code, length, m, x, pointer, y);
  fn(&native); cpu_mirrors_to_p(&native);
  if (g_other_exits || g_recomp_stack_top) return 1;
  prepare(&memory, code, length, m, x, pointer, y);
  Interp816 reference = {0}; reference.mem = &memory;
  reference.read = read_reference; reference.write = write_reference;
  reference.sp = memory.S; reference.pc = 0x8000; reference.y = memory.Y;
  interp816_setFlags(&reference, memory.P); reference.e = false;
  uint64 cycles = 0;
  for (unsigned n = 0; n < 32 && reference.pc != 1; ++n)
    cycles += interp816_runOpcode(&reference);
  if (reference.pc != 1 || native.cycles != cycles || native.S != reference.sp ||
      native.A != reference.a || native.Y != reference.y) {
    fprintf(stderr, "%s CPU cycles native=%llu interpreter=%llu\n", name,
            (unsigned long long)native.cycles, (unsigned long long)cycles);
    return 1;
  }
  return 0;
}
'''
    main = ['int main(void) { int failures = 0;']
    for name, code, m, x, pointer, y in cases:
        data = ','.join(f'0x{value:02x}' for value in bytes.fromhex(code))
        main.append(f'{{ const uint8 code[] = {{{data}}}; failures += check("{name}", '
                    f'{name}_M{m}X{x}, code, sizeof code, {m}, {x}, 0x{pointer:04x}, 0x{y:04x}); }}')
    main.append('return failures != 0; }')
    source = tmp_path / 'dp_indirect_y.c'
    source.write_text(harness + '\n'.join(generated) + '\n'.join(main))
    exe = tmp_path / ('dp_indirect_y.exe' if os.name == 'nt' else 'dp_indirect_y')
    build = subprocess.run([compiler, '-std=c11', '-O1', '-I', str(repo / 'runner/src'),
                            str(source), str(repo / 'runner/src/snes/interp816.c'),
                            '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    executed = subprocess.run([str(exe)], capture_output=True, text=True)
    assert executed.returncode == 0, executed.stdout + executed.stderr
