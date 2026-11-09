"""Synthetic proof of aggregate-block overshoot and opt-in instruction yields."""
import os
from pathlib import Path
import subprocess

import pytest

from _helpers import make_lorom_bank0
from snes65816 import get_rom_mapping, set_rom_mapping, ROM_MAP_LOROM
from v2.emit_function import emit_function


def emit(name, instruction_blocks):
    previous = get_rom_mapping()
    try:
        set_rom_mapping(ROM_MAP_LOROM)
        # LDA #$01 (2 cycles), NOP (2), RTS (6).
        rom = make_lorom_bank0({0x8000: bytes([0xA9, 0x01, 0xEA, 0x60])})
        return emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                             func_name=name, instruction_blocks=instruction_blocks)
    finally:
        set_rom_mapping(previous)


def test_opt_in_places_deadline_checks_before_each_instruction():
    aggregate = emit("Aggregate", False)
    split = emit("Split", True)
    assert "cpu->cycles += 10;" in aggregate
    assert "L_8002_M1X1:" not in aggregate
    for pc in ("8000", "8002", "8003"):
        block = split.split(f"L_{pc}_M1X1:", 1)[1]
        assert block.index("interp_bridge_lle_master_deadline_reached") < block.index("cpu->cycles +=")
    assert split.count("cpu->cycles += 2;") == 2
    assert "cpu->cycles += 6;" in split


def test_executed_deadline_preserves_guest_frame_before_rts(tmp_path):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip("C compiler required")
    harness = seam._HARNESS.split("/* The C helpers.")[0]
    old = ('int interp_bridge_lle_master_deadline_reached(const CpuState *cpu) {\n'
           '  (void)cpu; return 0;\n}')
    assert old in harness
    harness = harness.replace(old,
        'int interp_bridge_lle_master_deadline_reached(const CpuState *cpu) {\n'
        '  return cpu->master_cycles >= 16;\n}')
    main = r'''
static void prepare(CpuState *cpu) {
  memset(cpu, 0, sizeof *cpu); memset(g_ram, 0, sizeof g_ram);
  cpu->ram = g_ram; cpu->S = 0x01FF;
  cpu->m_flag = 1; cpu->x_flag = 1; cpu->P = 0x30;
  g_other_exits = 0; g_other_target = 0; g_tc_valid = 0; g_recomp_stack_top = 0;
  cpu_push_jsr_return_frame(cpu);
}
int main(void) {
  CpuState cpu;
  prepare(&cpu); Aggregate_M1X1(&cpu);
  /* The default aggregate body runs its RTS after crossing the deadline. */
  if (cpu.S != 0x01FF || cpu.cycles != 10 || cpu.master_cycles != 80 || g_other_exits) return 1;
  prepare(&cpu); Split_M1X1(&cpu);
  /* At the same deadline the split body suspends BEFORE NOP/RTS, retaining
     the guest call frame. This fixture isolates scheduling, not bus timing. */
  if (cpu.S != 0x01FD || cpu.cycles != 2 || cpu.master_cycles != 16 ||
      cpu.A != 1 || g_other_exits != 1 || g_other_target != 0x008002) return 2;
  puts("aggregate S=01ff cycles=10; instruction S=01fd cycles=2 resume=008002");
  return 0;
}
'''
    source = harness + emit("Aggregate", False) + emit("Split", True) + main
    c_file = tmp_path / "deadline.c"
    c_file.write_text(source)
    exe = tmp_path / ("deadline.exe" if os.name == "nt" else "deadline")
    runner = Path(__file__).resolve().parents[2] / "runner" / "src"
    build = subprocess.run([compiler, "-std=c11", "-O1", "-I", str(runner),
                            str(c_file), "-o", str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "resume=008002" in run.stdout


def test_instruction_blocks_keep_folded_taken_cycle(tmp_path):
    from test_folded_branch_cycles import run_folded_branch_cases
    run_folded_branch_cases(tmp_path, instruction_blocks=True)
