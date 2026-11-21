"""Compiled resume (generate --aot-resume): every region-paced instruction is also an entry."""
import re
from pathlib import Path
import subprocess

import pytest

from _helpers import make_lorom_bank0
from snes65816 import get_rom_mapping, set_rom_mapping, ROM_MAP_LOROM
from v2.emit_function import emit_function
from v2.program_emit import emit_aot_resume_table


def _emit(code, **options):
    old = get_rom_mapping()
    try:
        set_rom_mapping(ROM_MAP_LOROM)
        rom = make_lorom_bank0({0x8000: bytes.fromhex(code)})
        return emit_function(rom, 0, 0x8000, 1, 1, func_name="Case",
                             instruction_blocks=True, region_paced=True, **options)
    finally:
        set_rom_mapping(old)


# LDA $00 / INC A / STA $02 / RTS
_CODE = "A5 00 1A 85 02 60"


def test_every_instruction_is_a_resume_entry():
    source = _emit(_CODE, aot_resume=True)
    assert "cpu_take_aot_resume(&_entry_s, &_hrv)" in source
    cases = re.findall(r"/\* aot-resume \$([0-9A-F]{6}) m1x1 \*/", source)
    assert cases == ["008000", "008002", "008003", "008005"]


def test_off_by_default():
    source = _emit(_CODE)
    assert "cpu_take_aot_resume" not in source
    assert "aot-resume $" not in source


def test_setup_host_defines_empty_resume_table(tmp_path):
    from test_hle_func_seam import _compiler
    cc = _compiler()
    if not cc:
        pytest.skip("C compiler required")
    repo = Path(__file__).resolve().parents[2]
    main = tmp_path / "setup.c"
    main.write_text('#include "cpu_state.h"\n'
                    'int main(void) { return g_aot_resume_count || '
                    'g_aot_resume_table[0].fn != 0; }\n')
    exe = tmp_path / "setup.exe"
    subprocess.run([cc, '-std=c11', '-DSNESRECOMP_SETUP_HOST=1',
                    '-I', str(repo / 'runner/src'), str(main),
                    str(repo / 'runner/src/setup_host_dispatch.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def test_mirrored_handback_uses_the_dispatch_identity_provider(tmp_path):
    from test_hle_func_seam import _compiler
    from test_region_pacing import _c_function
    cc = _compiler()
    if not cc:
        pytest.skip('C compiler required')
    repo = Path(__file__).resolve().parents[2]
    bridge = (repo / 'runner/src/snes/interp_bridge.c').read_text(encoding='utf-8')
    source = r'''
#include <stdint.h>
#include <assert.h>
static uint32_t s_aot_resume_skip = 0xffffffffu, target, site;
static int enabled = 1, same, queried;
static int aot_resume_on(void) { return enabled; }
int cpu_dispatch_pc_matches(uint32_t a, uint32_t b) {
  assert(a == target && b == site); queried++; return same;
}
'''
    source += _c_function(bridge, 'aot_resume_note_handback')
    source += r'''
int main(void) {
  target=0x808100; site=0x008100; same=1;
  aot_resume_note_handback(target,site);
  assert(s_aot_resume_skip==target && queried==1);
  s_aot_resume_skip=0xffffffffu; target=0x7e8100; site=0xfe8100; same=0;
  aot_resume_note_handback(target,site);
  assert(s_aot_resume_skip==0xffffffffu && queried==2);
  target=site=0x008100; same=1; enabled=0;
  aot_resume_note_handback(target,site);
  assert(s_aot_resume_skip==0xffffffffu && queried==2);
  return 0;
}
'''
    main = tmp_path / 'handback.c'; main.write_text(source)
    exe = tmp_path / 'handback.exe'
    subprocess.run([cc, '-std=c11', str(main), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def test_needs_region_pacing():
    old = get_rom_mapping()
    try:
        set_rom_mapping(ROM_MAP_LOROM)
        source = emit_function(make_lorom_bank0({0x8000: bytes.fromhex(_CODE)}), 0, 0x8000, 1, 1,
                               func_name="Case", instruction_blocks=True, aot_resume=True)
    finally:
        set_rom_mapping(old)
    assert "cpu_take_aot_resume" not in source


def test_whole_variant_fallback_hands_back_the_resume_point():
    # The prologue's stack-window guard hands the variant to the interpreter;
    # a resumed frame has run past the entry and goes on where it stopped.
    source = _emit(_CODE, aot_resume=True)
    guard = source.index("native-mode pacing only")
    take = source.index("cpu_take_aot_resume")
    resume = source.index("if (_aot_resume) return interp_tier_dispatch_tail(cpu, _aot_resume, _aot_resume,")
    assert take < resume < guard


def test_compiled_instructions_report_themselves():
    source = _emit(_CODE, aot_resume=True)
    for pc16 in ("8000", "8002", "8003", "8005"):
        assert f"cpu_aot_insn_begin(cpu, 0x{pc16}u);" in source


def test_resume_table_keeps_the_longest_variant(tmp_path):
    a = tmp_path / "a.c"
    a.write_text("RecompReturn Short(CpuState *cpu) {\n"
                 "      case 1u: goto L;  /* aot-resume $008002 m1x1 */\n")
    b = tmp_path / "b.c"
    b.write_text("RecompReturn Long(CpuState *cpu) {\n"
                 "      case 1u: goto L;  /* aot-resume $008000 m1x1 */\n"
                 "      case 2u: goto L;  /* aot-resume $008002 m1x1 */\n")
    table = emit_aot_resume_table([a, b])
    assert "{ 0x008000u, 3, Long }," in table
    assert "{ 0x008002u, 3, Long }," in table
    assert "Short }" not in table


@pytest.mark.parametrize("flags,message", [
    (["--aot-resume"], "--aot-resume requires --region-paced"),
])
def test_v2_emit_refuses_resume_without_region_pacing(flags, message, capsys):
    import sys
    from tools import v2_emit
    old = sys.argv
    sys.argv = ["v2_emit", "--rom", "x", "--cfg-dir", "y", "--out-dir", "z", *flags]
    try:
        with pytest.raises(SystemExit):
            v2_emit.main()
    finally:
        sys.argv = old
    assert message in capsys.readouterr().err


def test_generated_resume_after_deadline_and_current_cpu_restore(tmp_path):
    """Execute emitted instructions and the real arm/take/paired APIs.

    The bridge's automatic abandoned-frame discovery is a separate gate;
    this pins the generated prologue/continuation contract it calls.
    """
    from test_region_pacing import _c_function, run_region_cases
    repo = Path(__file__).resolve().parents[2]
    infra = (repo / "runner/src/common_cpu_infra.c").read_text(encoding="utf-8")
    runtime = (repo / "runner/src/cpu_state.c").read_text(encoding="utf-8")
    helpers = r'''
static uint32_t g_aot_resume_pc;
static uint16_t g_aot_resume_entry_s;
static uint8_t g_aot_resume_hrv, g_cpu_entry_hrv[64];
'''
    helpers += _c_function(infra, "cpu_arm_aot_resume")
    helpers += _c_function(infra, "cpu_take_aot_resume")
    helpers += _c_function(runtime, "cpu_dispatch_resume_paired")
    helpers += r'''
void cpu_aot_resume_miss(CpuState *cpu, uint32 key) {
  (void)cpu; (void)key; abort();
}
'''
    helpers += _emit(_CODE, aot_resume=True).replace("Case_M1X1", "ResumeCase_M1X1")
    def instrument(source):
        source = "static unsigned long long resume_deadline;\n" + source
        old = "(void)cpu; return 0;\n}\nint interp_bridge_lle_unwind_pending"
        assert old in source
        return source.replace(old, "return resume_deadline && cpu->master_cycles >= resume_deadline;\n}\nint interp_bridge_lle_unwind_pending")
    main = r'''
  {
    CpuState cpu, expected, suspended;
    static uint8_t expected_memory[sizeof g_ram], suspended_memory[sizeof g_ram];
    const uint8_t code[] = {0xa5,0x00,0x1a,0x85,0x02,0x60};
    prepare(&cpu, code, sizeof code, 1, 1, 0); g_ram[0] = 0x2a;
    ResumeCase_M1X1(&cpu); cpu_mirrors_to_p(&cpu); expected = cpu;
    memcpy(expected_memory, g_ram, sizeof g_ram);
    prepare(&cpu, code, sizeof code, 1, 1, 0); g_ram[0] = 0x2a;
    resume_deadline = 1;
    ResumeCase_M1X1(&cpu);
    if (g_other_exits != 1 || g_other_target != 0x008002 ||
        cpu.A != 0x122a || cpu.S != 0x1fd || g_ram[2] != 0) fails++;
    suspended = cpu; memcpy(suspended_memory, g_ram, sizeof g_ram);
    resume_deadline = 0;
    for (int restored = 0; restored < 2; restored++) {
      cpu = suspended; memcpy(g_ram, suspended_memory, sizeof g_ram);
      g_other_exits = 0;
      cpu_dispatch_resume_paired(&cpu, 0x008002, cpu.S, 2, ResumeCase_M1X1);
      cpu_mirrors_to_p(&cpu);
      if (g_other_exits || cpu.A != expected.A || cpu.S != expected.S ||
          cpu.P != expected.P || cpu.X != expected.X || cpu.Y != expected.Y ||
          cpu.cycles != expected.cycles || cpu.master_cycles != expected.master_cycles ||
          cpu.open_bus != expected.open_bus || memcmp(expected_memory, g_ram, sizeof g_ram)) fails++;
    }
  }
'''
    run_region_cases(tmp_path, cases=[], extra_emitted=helpers, extra_main=main,
                     harness_transform=instrument)
