"""Pin dispatch-table START alignment against the cfg data_region overlay.

`JSR ($base,X)` adds X to `base` and calls through the pointer it finds.
The auto-recovery has always assumed entry 0 lives AT `base` and that X is
a multiple of the entry size. That is a real ROM fact for most tables and a
silent wrong answer for the rest.

Yoshi's Island writes the exception twice — `$01:DE80` (the message-box
handler) and `$01:B58E` (a level-state dispatcher). Both name their own
`RTL` opcode as the operand so the table begins one byte later, and both
index with a state variable that steps 1, 3, 5, ...:

    JSR ($DE84,x)            ; X = r_msg_box_state, an ODD byte offset
    PLB
    RTL                      ; $01DE84 — the operand byte
    message_box_state_ptr:   ; $01DE85 — entry 0 lives HERE
    dw $DE93, $DEA9, ...

Walking from `$DE84` at stride 2 reads every pointer one byte low
($6B|$93 -> $01936B, ...). Those addresses sit in the middle of unrelated
routines; the emitted C compiles, links, runs, and the guest disappears
into them — measured as a runaway into unmapped bank $ED (beads-8wg.20.1).

The only authority that can settle the alignment is the cfg `data_region`
overlay, which states outright which bytes are the table. These tests pin
both halves of that: the START (bias) and the END (no entry may be composed
of bytes past the declared span).
"""
from _helpers import make_lorom_bank0  # noqa: E402  (sets sys.path)

from types import SimpleNamespace

from v2.codegen import _emit_indirect_dispatch  # noqa: E402
from v2.decoder import _autorecover_indirect_xtable  # noqa: E402


def _yi_shaped_rom() -> bytes:
    """A bank-0 LoROM image with the Yoshi's Island idiom at $8008.

    $8005  LDX $0D0F        AE 0F 0D
    $8008  JSR ($800C,x)    FC 0C 80
    $800B  PLB              AB
    $800C  RTL              6B          <- the operand byte, real code
    $800D  dw $9093                     <- entry 0 (data_region starts here)
    $800F  dw $90A9
    $8011  dw $90D0
    $8013  (past the table)

    The misaligned walk from $800C would read $936B / $A990 / $D090, so the
    three decoys are filled with plausible code bytes: if the walk is wrong,
    it succeeds and produces garbage rather than failing loudly. That is
    exactly why this needed a ROM fact rather than a heuristic.
    """
    code = bytes([0xAE, 0x0F, 0x0D,        # $8005 LDX $0D0F
                  0xFC, 0x0C, 0x80,        # $8008 JSR ($800C,x)
                  0xAB,                    # $800B PLB
                  0x6B])                   # $800C RTL  (operand byte)
    table = bytes([0x93, 0x90, 0xA9, 0x90, 0xD0, 0x90])   # $800D..$8012
    handler = bytes([0xEA] * 15 + [0x60])                 # NOPs + RTS
    return make_lorom_bank0({
        0x8005: code + table,
        0x9093: handler, 0x90A9: handler, 0x90D0: handler,
        # Decoys the misaligned walk would land on.
        0x936B: handler, 0xA990: handler, 0xD090: handler,
    })


def _site():
    return SimpleNamespace(operand=0x800C, mnem='JSR', length=3, opcode=0xFC)


# data_region <bank> <start> <end_exclusive>: the table is $800D..$8012.
TABLE_REGION = [(0x00, 0x800D, 0x8013)]


def test_table_after_the_operand_byte_is_found_with_its_bias():
    entries, bias = _autorecover_indirect_xtable(
        _yi_shaped_rom(), 0x00, _site(), TABLE_REGION, func_start=0x8005)
    assert bias == 1, "entry 0 is one byte past the operand"
    assert entries == [0x009093, 0x0090A9, 0x0090D0], \
        [hex(e) for e in entries]


def test_without_the_overlay_the_walk_is_misaligned():
    """Pins WHY the overlay is load-bearing rather than decorative: with no
    data_region the same ROM yields the one-byte-low pointers, every one of
    which passes the existing range/padding gates."""
    entries, bias = _autorecover_indirect_xtable(
        _yi_shaped_rom(), 0x00, _site(), None, func_start=0x8005)
    assert bias == 0
    assert 0x00936B in entries, [hex(e) for e in entries]
    assert 0x009093 not in entries


def test_walk_stops_at_the_declared_end_of_the_table():
    """Bytes past the data_region are code, so no entry may be composed of
    them however valid the resulting address looks."""
    rom = bytearray(_yi_shaped_rom())
    # $8013/$8014 would read as a perfectly plausible $90D0 entry.
    rom[0x8013 - 0x8000] = 0xD0
    rom[0x8014 - 0x8000] = 0x90
    entries, _bias = _autorecover_indirect_xtable(
        bytes(rom), 0x00, _site(), TABLE_REGION, func_start=0x8005)
    assert len(entries) == 3, [hex(e) for e in entries]


def test_ordinary_table_at_the_operand_keeps_bias_zero():
    """The common layout — operand IS the table — must be untouched, with or
    without an overlay declaring the span."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xFC, 0x00, 0x90]),                 # JSR ($9000,x)
        0x9000: bytes([0x00, 0x91, 0x10, 0x91]),           # $9100, $9110
        0x9100: bytes([0xEA] * 15 + [0x60]),
        0x9110: bytes([0xEA] * 15 + [0x60]),
    })
    insn = SimpleNamespace(operand=0x9000, mnem='JSR', length=3, opcode=0xFC)
    for regions in (None, [(0x00, 0x9000, 0x9004)]):
        entries, bias = _autorecover_indirect_xtable(
            rom, 0x00, insn, regions, func_start=0x8000)
        assert bias == 0
        assert entries == [0x009100, 0x009110], [hex(e) for e in entries]


def test_codegen_subtracts_the_bias_from_the_index_register():
    insn = SimpleNamespace(
        addr=0x008008, operand=0x800C, mnem='JSR', length=3, opcode=0xFC,
        mode=None, m_flag=1, x_flag=1,
        dispatch_entries=[0x009093, 0x0090A9, 0x0090D0],
        dispatch_kind='short', dispatch_idx_reg='X', dispatch_table_bases=(),
        dispatch_index_bias=1, dispatch_call=True)
    body = '\n'.join(_emit_indirect_dispatch(insn))
    assert '(uint16)(cpu->X - 1)' in body, body
    assert 'cpu->X & 0xFFFF) / 2' not in body, body

    insn.dispatch_index_bias = 0
    body = '\n'.join(_emit_indirect_dispatch(insn))
    assert '(cpu->X & 0xFFFF) / 2' in body, body
