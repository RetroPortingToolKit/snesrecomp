"""Pin v2 decoder cfg `data_region` behaviour.

`data_region <bank> <start> <end>` declares a byte range as data, not
code — a real ROM-structure fact the recompiler can't infer from
bytes alone. The dispatch-table reader and the auto-promote pass
both consult `data_regions` and refuse to treat any address inside
a region as a callable handler.

Concrete trigger: SMW's InitializeScrollSprites_05BD0E PLA/PLY
dispatcher's table overruns into a bank-internal data table at
$05:9C60. Without the directive, the auto-promoter synthesised
`auto_059C60_M1X1` and the decoder followed its bytes (which look
like `BRL $1F78` followed by garbage) into an UNRESOLVABLE_GOTO.
With `data_region 05 9C60 9C8E`, the dispatch-table reader stops
at the suspect entry and records a DispatchTargetSuppressed entry
for the build report.
"""
from _helpers import make_lorom_bank0  # noqa: E402
import pytest

from v2.decoder import (  # noqa: E402
    decode_function,
    _addr_in_data_regions,
    DispatchTargetSuppressed,
)


def test_addr_in_data_regions_basic():
    regions = [(0x00, 0x9C60, 0x9C8E)]
    assert _addr_in_data_regions(regions, 0, 0x9C60) is True
    assert _addr_in_data_regions(regions, 0, 0x9C8D) is True
    assert _addr_in_data_regions(regions, 0, 0x9C8E) is False  # exclusive end
    assert _addr_in_data_regions(regions, 0, 0x9C5F) is False
    # Wrong bank.
    assert _addr_in_data_regions(regions, 1, 0x9C70) is False


def test_addr_in_data_regions_no_regions_is_false():
    assert _addr_in_data_regions(None, 0, 0x9C60) is False
    assert _addr_in_data_regions([], 0, 0x9C60) is False


@pytest.mark.parametrize('kind,stride', [('short', 2), ('long', 3)])
def test_declared_inline_table_keeps_valid_slots_after_unused_data_target(kind, stride):
    targets = [0x9000, 0x9c70, 0x9100, 0x9200]
    # The final plausible pointer belongs to adjacent data, outside the table.
    table = b''.join(t.to_bytes(stride, 'little') for t in targets)
    rom = make_lorom_bank0({
        0x8000: b'\x22\x00\xe0\x00' + table,
        0x9000: b'\x60', 0x9100: b'\x60', 0x9200: b'\x60',
        0x9c70: bytes(range(16)),
    })
    graph = decode_function(rom, 0, 0x8000, 1, 1,
        dispatch_helpers={0x00e000: kind},
        data_regions=[(0, 0x8004, 0x8004+3*stride), (0, 0x9c60, 0x9c8e)])
    dispatch = next(di.insn for di in graph.insns.values() if di.insn.mnem == 'JSL')
    assert dispatch.dispatch_entries == targets[:3]
    assert len(graph.dispatch_targets_suppressed) == 1
    assert graph.dispatch_targets_suppressed[0].table_index == 1
    assert graph.dispatch_targets_suppressed[0].target_pc24 == 0x9c70


def test_dispatch_table_stops_at_data_region_entry():
    """Dispatch table whose 3rd entry points inside a cfg data_region
    must be truncated to 2 entries; the suppression is recorded.
    """
    rom = bytearray(0x8000)
    # JSL helper.
    rom[0x8000 - 0x8000:0x8004 - 0x8000] = b'\x22\x00\xE0\x00'
    # table[0]=$9000, table[1]=$9100, table[2]=$9C70 (inside data_region)
    rom[0x8004 - 0x8000:0x800A - 0x8000] = b'\x00\x90\x00\x91\x70\x9C'
    # Real handlers (bytes don't matter for dispatch reader; only the
    # entries' addr-range / data_region / padding tests fire).
    rom[0x9000 - 0x8000] = 0x60
    rom[0x9100 - 0x8000] = 0x60
    # $9C60-$9C8E filled with "code-looking" bytes — must NOT trigger
    # the padding-uniformity gate, only the data_region gate.
    rom[0x9C60 - 0x8000:0x9C8E - 0x8000] = b'\x82\x15\x83\x15\x59\x1A\x00\x0B' * 4 + b'\x82\x15\x83\x15\x59\x1A'
    rom[0x800A - 0x8000] = 0x60   # RTS after table
    rom_bytes = bytes(rom)
    graph = decode_function(
        rom_bytes, bank=0, start=0x8000, entry_m=1, entry_x=1,
        dispatch_helpers={0x00E000: 'short'},
        data_regions=[(0x00, 0x9C60, 0x9C8E)],
    )
    jsl_di = next(di for di in graph.insns.values() if di.insn.mnem == 'JSL')
    assert jsl_di.insn.dispatch_entries == [0x9000, 0x9100]
    # Suppression record present.
    assert len(graph.dispatch_targets_suppressed) == 1
    rec = graph.dispatch_targets_suppressed[0]
    assert isinstance(rec, DispatchTargetSuppressed)
    assert rec.target_pc24 == 0x009C70
    assert rec.reason == 'data_region'
    assert rec.table_index == 2


def test_dispatch_table_with_no_data_region_overlap_keeps_all_entries():
    """Sanity: a real-target table is unaffected by an unrelated
    data_region declaration."""
    rom = bytearray(0x8000)
    rom[0x8000 - 0x8000:0x8004 - 0x8000] = b'\x22\x00\xE0\x00'
    rom[0x8004 - 0x8000:0x800A - 0x8000] = b'\x00\x90\x00\x91\x00\x92'
    rom[0x800A - 0x8000] = 0x60
    rom[0x9000 - 0x8000] = 0x60
    rom[0x9100 - 0x8000] = 0x60
    rom[0x9200 - 0x8000] = 0x60
    rom_bytes = bytes(rom)
    graph = decode_function(
        rom_bytes, bank=0, start=0x8000, entry_m=1, entry_x=1,
        dispatch_helpers={0x00E000: 'short'},
        data_regions=[(0x00, 0xC000, 0xC100)],   # unrelated range
    )
    jsl_di = next(di for di in graph.insns.values() if di.insn.mnem == 'JSL')
    # table terminator is the byte after table[2] which is $0060 (RTS
    # opcode + 0x00); $0060 < $8000 so the addr-range stop fires.
    assert jsl_di.insn.dispatch_entries == [0x9000, 0x9100, 0x9200]
    assert len(graph.dispatch_targets_suppressed) == 0


def test_long_dispatch_table_data_region_gate():
    """Same gate works for `long` dispatch helpers."""
    rom = bytearray(0x8000)
    # JSL helper at $E000 — long-kind dispatch.
    rom[0x8000 - 0x8000:0x8004 - 0x8000] = b'\x22\x00\xE0\x00'
    # 3-byte entries: bank=00 then addr16. table[0]=$009000,
    # table[1]=$00C100 (inside data_region).
    rom[0x8004 - 0x8000:0x800A - 0x8000] = b'\x00\x90\x00\x00\xC1\x00'
    rom[0x800A - 0x8000] = 0x60
    rom[0x9000 - 0x8000] = 0x60
    rom[0xC100 - 0x8000:0xC180 - 0x8000] = b'\x97\x15\x91\x15\x59\x1A' * 16 + b'\x97\x15'
    rom_bytes = bytes(rom)
    graph = decode_function(
        rom_bytes, bank=0, start=0x8000, entry_m=1, entry_x=1,
        dispatch_helpers={0x00E000: 'long'},
        data_regions=[(0x00, 0xC100, 0xC180)],
    )
    jsl_di = next(di for di in graph.insns.values() if di.insn.mnem == 'JSL')
    assert jsl_di.insn.dispatch_entries == [0x009000]
    assert len(graph.dispatch_targets_suppressed) == 1
    assert graph.dispatch_targets_suppressed[0].target_pc24 == 0x00C100
