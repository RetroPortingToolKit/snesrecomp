"""Finite indexed target declarations preserve original table selectors."""
import pathlib
import tempfile
from _helpers import make_lorom_bank0
from v2.cfg_loader import load_bank_cfg
from v2.decoder import decode_function, analyze_function_exit_mx_modes
from v2.emit_function import emit_function


def parse(line):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / 'bank00.cfg'
        path.write_text('bank = 00\n' + line + '\n', encoding='ascii')
        return load_bank_cfg(str(path)).indirect_dispatch[0]


def test_explicit_indexed_targets_preserve_null_slot_and_selector():
    site = parse('indirect_dispatch 8005 3 idx:Y targets:009000,000000,009100')
    assert site['idx_reg'] == 'Y'
    assert site['targets'] == [0x9000, 0, 0x9100]
    assert not site['table_bases']
    assert not site.get('pointer_match')


def test_handler_minus_one_pha_keeps_real_handlers_and_inherited_frame():
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x30, 0xB9, 0x00, 0x81, 0x48, 0xE2, 0x30, 0x60]),
        0x8100: bytes([0xFF, 0x8F, 0xFF, 0x90]),
        0x9000: bytes([0x60]), 0x9100: bytes([0x60]),
    })
    site = parse('indirect_dispatch 8005 2 idx:Y targets:009000,009100')
    dispatch = {0x8005: site}
    graph = decode_function(rom, 0, 0x8000, 1, 1, indirect_dispatch=dispatch)
    assert analyze_function_exit_mx_modes(graph) == {(1, 1)}
    src = emit_function(rom=rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                        func_name='IndexedPhaTail', indirect_dispatch=dispatch)
    assert '(cpu->Y & 0xFFFF) / 2' in src
    assert 'RTS-stack dispatch terminator' in src
    assert '0x9000u' in src and '0x9100u' in src
    assert '0x8FFFu' not in src and '0x90FFu' not in src


def test_invalid_or_ambiguous_indexed_target_authority_is_rejected():
    for line in (
        'indirect_dispatch 8005 2 idx:Y targets:009000',
        'indirect_dispatch 8005 2 idx:Y targets:',
        'indirect_dispatch 8005 1 idx:Y tables:8100 targets:009000',
    ):
        try:
            parse(line)
        except ValueError:
            continue
        raise AssertionError(line)


def test_existing_rom_table_declaration_remains_unchanged():
    site = parse('indirect_dispatch 8005 2 idx:Y tables:8100')
    assert site['table_bases'] == (0x8100,)
    assert 'targets' not in site
