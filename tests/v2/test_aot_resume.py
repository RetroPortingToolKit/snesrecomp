"""Compiled resume (generate --aot-resume): every region-paced instruction is also an entry."""
import re

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
