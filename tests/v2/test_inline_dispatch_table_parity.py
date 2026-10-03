"""The native analyzer and the Python emission decoder must read the same
inline (JSL/JML dispatch-helper) table.

The analyzer decides which handler variants get AOT bodies; the Python
decoder emits the dispatch switch. A table one side reads longer than the
other either fabricates targets or drops real ones, so each table bound is
pinned on both sides here: a `data_region` that declares the table's extent
(an unused data-target slot inside it does not end the table, a plausible
pointer past it is not read) and the next declared function entry.
"""
import pytest

from _helpers import make_lorom_bank0
from test_analysis_tool import analyze, _load_cfgs
from v2.decoder import decode_function
from v2.program_analysis import EdgeKind, EdgeResolution, VariantKey

# ExecutePtr-shaped helpers the analyzer classifies from ROM bytes alone:
# pull the return address, scale the index (ASL, plus ADC for 24-bit
# entries), TAY, then JML through the loaded pointer.
SHORT_HELPER = bytes([
    0x84, 0x03, 0x7A, 0x84, 0x00, 0xC2, 0x30, 0x29, 0xFF, 0x00, 0x0A,
    0xA8, 0x68, 0x85, 0x02, 0xC8, 0xB7, 0x00, 0x85, 0x00, 0xE2, 0x30,
    0xA4, 0x03, 0xDC, 0x00, 0x00,
])
LONG_HELPER = bytes([
    0x84, 0x05, 0x7A, 0x84, 0x02, 0xC2, 0x30, 0x29, 0xFF, 0x00, 0x85,
    0x03, 0x0A, 0x65, 0x03, 0xA8, 0x68, 0x85, 0x03, 0xC8, 0xB7, 0x02,
    0x85, 0x00, 0xC8, 0xB7, 0x02, 0x85, 0x01, 0xE2, 0x30, 0xA4, 0x05,
    0xDC, 0x00, 0x00,
])
KINDS = [('short', 2, SHORT_HELPER), ('long', 3, LONG_HELPER)]


def _rom(stride, helper, targets, extra=None):
    table = b''.join(t.to_bytes(stride, 'little') for t in targets)
    blobs = {
        0x8000: b'\x22\x00\xe0\x00' + table,
        0x9000: b'\x60', 0x9100: b'\x60', 0x9200: b'\x60', 0x9260: b'\x60',
        0x9c70: bytes(range(16)),
        0xE000: helper,
    }
    blobs.update(extra or {})
    return make_lorom_bank0(blobs)


def _native_dispatch(tmp_path, rom, cfg_lines):
    (tmp_path / 'bank00.cfg').write_text(
        'bank = 00\nfunc Root 8000 entry_mx:1,1\n' + ''.join(
            line + '\n' for line in cfg_lines), encoding='utf-8')
    manifest, helpers, _inline = analyze(
        rom, _load_cfgs(tmp_path), all_cfg_roots=True)
    root = manifest.nodes[VariantKey(0x008000, 1, 1)]
    edges = {
        edge.target.pc24 & 0xFFFF: edge.resolution
        for edge in root.demands
        if edge.kind == EdgeKind.STATIC_DISPATCH and edge.site_pc24 == 0x008000
    }
    return helpers, edges


def _python_dispatch(rom, kind, regions, siblings=None):
    graph = decode_function(
        rom, 0, 0x8000, 1, 1, dispatch_helpers={0x00E000: kind},
        data_regions=regions, sibling_entry_pcs=siblings)
    jsl = next(di.insn for di in graph.insns.values()
               if di.insn.addr & 0xFFFF == 0x8000)
    entries = [e & 0xFFFF for e in (jsl.dispatch_entries or ())]
    return entries, graph.dispatch_targets_suppressed


@pytest.mark.parametrize('kind,stride,helper', KINDS, ids=['short', 'long'])
def test_declared_table_bound_agrees_between_analyzer_and_emitter(
        tmp_path, kind, stride, helper):
    rom = _rom(stride, helper, [0x9000, 0x9C70, 0x9100, 0x9200])
    table_end = 0x8004 + 3 * stride
    regions = [(0, 0x8004, table_end), (0, 0x9C60, 0x9C8E)]

    entries, suppressed = _python_dispatch(rom, kind, regions)
    assert entries == [0x9000, 0x9C70, 0x9100]
    assert [(s.table_index, s.target_pc24) for s in suppressed] == [(1, 0x9C70)]

    helpers, edges = _native_dispatch(tmp_path, rom, [
        f'data_region 00 8004 {table_end:04x}',
        'data_region 00 9c60 9c8e',
    ])
    assert helpers.get(0x00E000) == kind, helpers
    assert sorted(edges) == sorted(entries)
    # The unused slot stays an exact interpreter edge; it never becomes code.
    assert edges[0x9C70] == EdgeResolution.LLE_EXACT
    assert edges[0x9000] == edges[0x9100] == EdgeResolution.AOT_EXACT


@pytest.mark.parametrize('kind,stride,helper', KINDS, ids=['short', 'long'])
def test_undeclared_table_stops_at_data_target_on_both_sides(
        tmp_path, kind, stride, helper):
    rom = _rom(stride, helper, [0x9000, 0x9C70, 0x9100, 0x9200])

    entries, suppressed = _python_dispatch(rom, kind, [(0, 0x9C60, 0x9C8E)])
    assert entries == [0x9000]
    assert [(s.table_index, s.target_pc24) for s in suppressed] == [(1, 0x9C70)]

    _helpers, edges = _native_dispatch(
        tmp_path, rom, ['data_region 00 9c60 9c8e'])
    assert sorted(edges) == sorted(entries)


@pytest.mark.parametrize('kind,stride,helper', KINDS, ids=['short', 'long'])
def test_table_stops_at_next_declared_function_on_both_sides(
        tmp_path, kind, stride, helper):
    # Two real entries, then the next routine. Its opening bytes read as a
    # plausible pointer to $9260 (RTS), so only the function boundary can
    # end the table.
    sibling = 0x8004 + 2 * stride
    opening = (0x9260).to_bytes(stride, 'little')
    rom = _rom(stride, helper, [0x9000, 0x9100], {sibling: opening + b'\x60'})

    unbounded, _ = _python_dispatch(rom, kind, [])
    assert unbounded[:3] == [0x9000, 0x9100, 0x9260]
    entries, _ = _python_dispatch(rom, kind, [], siblings={sibling})
    assert entries == [0x9000, 0x9100]

    _helpers, edges = _native_dispatch(
        tmp_path, rom, [f'func Next {sibling:04x}'])
    assert sorted(edges) == sorted(entries)
