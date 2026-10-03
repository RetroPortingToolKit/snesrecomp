"""Pin tools/cfg_boundary_audit.py on ROM-free synthetic images.

Each fixture function isolates one decision the audit makes: how far the walk
reaches from the entry (branches both ways, local jumps, entry width, REP),
where it must stop (an indirect jump names a pointer, not a target), and which
unreachable tails are findings (declared data is not; a HiROM function below
$8000 is audited, not skipped).
"""
import io
import pathlib
import sys
from contextlib import redirect_stdout

import pytest

REPO = pathlib.Path(__file__).resolve().parent.parent
if str(REPO / "tools") not in sys.path:
    sys.path.insert(0, str(REPO / "tools"))

import cfg_boundary_audit as audit_tool  # noqa: E402
import snes65816 as s  # noqa: E402


@pytest.fixture(autouse=True)
def _restore_mapping():
    saved = s.get_rom_mapping()
    yield
    s.set_rom_mapping(saved)


LOROM_CODE = {
    # Long: LDA #1 / BEQ +2 / NOP / NOP / RTS -- 7 bytes of a 0x80 range.
    0x8000: bytes([0xA9, 0x01, 0xF0, 0x02, 0xEA, 0xEA, 0x60]),
    # Tight: the whole declared range is code.
    0x8080: bytes([0xEA, 0xEA, 0xEA, 0x60]),
    # Wide: LDA #$1234 / RTS, valid only at the declared entry width M=0.
    0x8100: bytes([0xA9, 0x34, 0x12, 0x60]),
    # Declared: RTS, then a tail the cfg already declares as data.
    0x8200: bytes([0x60]),
    # Jumpy: JMP $8310 over a gap, RTS there.
    0x8300: bytes([0x4C, 0x10, 0x83]),
    0x8310: bytes([0x60]),
    # Indirect: JMP ($8420) -- $8420 is a pointer, not code to walk.
    0x8400: bytes([0x6C, 0x20, 0x84]),
    0x8420: bytes([0xEA] * 0x1F + [0x60]),
    # Rep: REP #$20 widens the following immediate.
    0x8500: bytes([0xC2, 0x20, 0xA9, 0x34, 0x12, 0x60]),
}

LOROM_CFG = """\
bank = 00
func Long     8000 end:8080
func Tight    8080 end:8084
func Wide     8100 end:8140 entry_mx:0,0   # trailing comment is ignored
func Declared 8200 end:8280
exclude_range 8201 8280
func Jumpy    8300 end:8340
func Indirect 8400 end:8440
func Rep      8500 end:8540
func NoEnd    8600
"""


def _lorom_image() -> bytes:
    rom = bytearray([0xFF] * 0x8000)
    for pc, blob in LOROM_CODE.items():
        rom[pc - 0x8000:pc - 0x8000 + len(blob)] = blob
    return bytes(rom)


def _hirom_image() -> bytes:
    rom = bytearray([0xFF] * 0x10000)
    rom[0x0100:0x0102] = bytes([0xEA, 0x60])          # $C0:0100 NOP / RTS
    hdr = 0xFFC0
    rom[hdr + 0x15] = 0x21                            # HiROM map mode
    rom[hdr + 0x1C:hdr + 0x20] = bytes([0xFF, 0xFF, 0x00, 0x00])  # cmpl/sum
    rom[hdr + 0x3C:hdr + 0x3E] = bytes([0x00, 0x80])  # RESET -> $8000
    return bytes(rom)


def _run(tmp_path, rom: bytes, cfg_text: str, min_tail: int):
    cfg = tmp_path / "bank00.cfg"
    cfg.write_text(cfg_text)
    s.set_rom_mapping(s.detect_rom_mapping(rom))
    total, findings = audit_tool.audit(rom, [cfg], min_tail)
    return total, {f.name: f for f in findings}


def test_lorom_findings_are_exactly_the_undeclared_long_tails(tmp_path):
    total, found = _run(tmp_path, _lorom_image(), LOROM_CFG, 0x20)
    # NoEnd declares no boundary, so there is nothing to audit.
    assert total == 7
    assert sorted(found) == ["Indirect", "Jumpy", "Long", "Rep", "Wide"]

    long_ = found["Long"]
    assert (long_.reached, long_.tail, long_.undeclared) == (7, 0x79, 0x79)
    assert (long_.bank, long_.start, long_.end) == (0x00, 0x8000, 0x8080)
    assert not long_.exhausted

    # Decoded at M=1 the immediate would be one byte and the walk would run
    # on through the $FF padding; at the declared M=0 it stops at the RTS.
    assert found["Wide"].reached == 4
    assert found["Rep"].reached == 6
    # The JMP target is walked; the bytes it jumps over are not.
    assert found["Jumpy"].reached == 4
    assert found["Jumpy"].tail == 0x8340 - 0x8311
    # An indirect jump's operand is a pointer: nothing past it is reached.
    assert found["Indirect"].reached == 3


def test_declared_data_is_not_a_finding_but_still_counts_as_tail(tmp_path):
    _, found = _run(tmp_path, _lorom_image(), LOROM_CFG, 0)
    declared = found["Declared"]
    assert declared.tail == 0x7F
    assert declared.undeclared == 0
    assert found["Tight"].tail == 0


def test_hirom_functions_below_8000_are_audited(tmp_path):
    rom = _hirom_image()
    assert s.detect_rom_mapping(rom) == s.ROM_MAP_HIROM
    total, found = _run(tmp_path, rom, "bank = C0\nfunc Low 0100 end:0180\n", 0x40)
    assert total == 1
    assert found["Low"].reached == 2
    assert found["Low"].undeclared == 0x7E


def test_walk_budget_exhaustion_is_reported(tmp_path):
    rom = _lorom_image()
    s.set_rom_mapping(s.detect_rom_mapping(rom))
    seen, exhausted = audit_tool.reachable(rom, 0x00, 0x8000, 0x8080, budget=2)
    assert exhausted
    assert seen == set(range(0x8000, 0x8004))


def test_cli_report(tmp_path):
    rom_path = tmp_path / "fixture.sfc"
    rom_path.write_bytes(_lorom_image())
    (tmp_path / "bank00.cfg").write_text(LOROM_CFG)
    out = io.StringIO()
    with redirect_stdout(out):
        rc = audit_tool.main([str(rom_path), str(tmp_path),
                              "--min-tail", "0x30", "--top", "2"])
    lines = out.getvalue().splitlines()
    assert rc == 0
    assert lines[0] == ("7 func declarations checked; "
                        "4 with an undeclared unreachable tail >= 0x30")
    assert lines[2].split() == ["tail", "bank", "declared", "reached", "name"]
    assert lines[3].split() == ["0x79", "$00", "$8000-$8080", "7", "Long"]
    assert lines[4].split() == ["0x3d", "$00", "$8400-$8440", "3", "Indirect"]
    assert len(lines) == 5
