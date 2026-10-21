"""ROM-free regressions for issue #193, including different bank-local flow."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from snes65816 import (get_rom_mapping, set_rom_mapping, rom_bank_aliases,
                      rom_bank_mirror, rom_offset)
from v2_analyze import _load_cfgs, build_manifest_native
from v2.program_analysis import VariantKey
from v2.coverage_profile import canonical_pc, execution_pc
from disassembly_layout import materialize, export_layout
from ingest_disassembly_authority import ingest
from export_disassembly_authority import export


@pytest.fixture
def hirom(tmp_path):
    previous = get_rom_mapping()
    set_rom_mapping('hirom')
    rom = bytearray([0xFF] * 0x10000)
    rom[0xFFD5] = 0x31
    rom[0xFFDC:0xFFE0] = bytes.fromhex('cbed3412')
    rom[0xFFFC:0xFFFE] = bytes.fromhex('0090')
    rom[0x9000:0x9004] = bytes.fromhex('4c001060')  # JMP $1000; RTS
    rom[0x1000] = 0x60
    image = tmp_path / 'fixture.sfc'
    image.write_bytes(rom)
    cfg = tmp_path / 'cfg'
    cfg.mkdir()
    (cfg / 'bankc0.cfg').write_text('bank = C0\nfunc Jump 9000\nfunc Lower 1000\n')
    yield rom, image, cfg
    set_rom_mapping(previous)


def test_physical_aliases_are_symmetric_and_exclude_ram(hirom):
    for bank in (0, 0x40, 0x80, 0xC0):
        aliases = rom_bank_aliases(bank, 0x9000)
        assert aliases[0] == bank
        assert set(aliases) == {0, 0x40, 0x80, 0xC0}
        assert {rom_offset(b, 0x9000) for b in aliases} == {0x9000}
    assert set(rom_bank_aliases(0xC0, 0x1000)) == {0x40, 0xC0}
    assert rom_bank_aliases(0, 0x1000) == ()
    assert rom_bank_aliases(0x7E, 0x9000) == ()
    assert set(rom_bank_aliases(0xFE, 0x9000)) == {0x3E, 0xBE, 0xFE}
    assert rom_bank_mirror(0xC0) == 0x40
    assert rom_bank_mirror(0) == 0x80  # never substitute a full-bank body


def test_cfg_contracts_reach_aliases_without_low_half_leaks(hirom):
    _, _, cfg = hirom
    (cfg / 'bankc0.cfg').write_text('bank = C0\nfunc Upper 9000\n'
        'entry_mx_at 9000 0 1\nfunc Lower 1000\nforce_lle C09000\n'
        'authority_data 7000 9000\n')
    parsed = {bank: value for bank, _, value in _load_cfgs(cfg)}
    assert set(parsed) == {0, 0x40, 0x80, 0xC0}
    assert [(e.start, e.entry_m, e.entry_x) for e in parsed[0].entries] == [(0x9000, 0, 1)]
    assert {e.start for e in parsed[0x40].entries} == {0x1000, 0x9000}
    assert parsed[0].authority_data == [(0x8000, 0x9000)]
    assert parsed[0x40].authority_data == [(0x7000, 0x9000)]
    assert parsed[0xC0].force_lle == {0x009000, 0x409000, 0x809000, 0xC09000}
    (cfg / 'bank00.cfg').write_text('bank = 00\nfunc Explicit 9000 entry_mx:1,0\n')
    parsed = {bank: value for bank, _, value in _load_cfgs(cfg)}
    assert (parsed[0].entries[0].entry_m, parsed[0].entries[0].entry_x) == (1, 0)


def test_coverage_groups_bytes_without_promoting_another_execution_bank(hirom):
    assert {canonical_pc(b << 16 | 0x9000, 'hirom') for b in (0, 0x40, 0x80, 0xC0)} == {0xC09000}
    assert execution_pc(0x009000, 'hirom') == 0x009000
    assert execution_pc(0xC09000, 'hirom') == 0xC09000
    assert canonical_pc(0x001000, 'hirom') == 0x001000
    assert canonical_pc(0x7E9000, 'hirom') == 0x7E9000


def test_explicit_alias_exit_contract_wins(hirom):
    _, _, cfg = hirom
    (cfg / 'bankc0.cfg').write_text('bank = C0\nfunc Jump 9000\nexit_mx_at C09000 0 0\n')
    (cfg / 'bank00.cfg').write_text('bank = 00\nexit_mx_at 009000 1 1\n')
    parsed = _load_cfgs(cfg)
    exits = {(b, pc): (m, x) for _, _, c in parsed for b, pc, m, x in c.exit_mx_at}
    assert exits[0, 0x9000] == (1, 1)
    assert exits[0xC0, 0x9000] == (0, 0)


def test_source_export_reads_lower_hirom_bank(hirom, tmp_path):
    _, image, _ = hirom
    source = tmp_path / 'source'
    source.mkdir()
    asm = source / 'bankc0.asm'
    asm.write_text('|C01000| RTS\n|C09000| JMP $1000\n|C09003| RTS\n')
    args = SimpleNamespace(rom=image, assembled_rom=image, source=source,
        format='usdasm', symbols=None, source_commit='synthetic', assembler=asm,
        classifier=asm, out=tmp_path / 'export.json')
    # This adapter test supplies already-classified synthetic source rows.
    classifier = SimpleNamespace(strip_label=lambda s: s.strip(),
        strip_comment=lambda s: s, statement_kind=lambda *_: 'code')
    export(args, classifier)
    rows = json.loads(args.out.read_text())['instructions']
    assert {int(r['pc24'], 0): r['bytes'] for r in rows} == {
        0xC01000: '60', 0xC09000: '4c0010', 0xC09003: '60'}


def test_layout_and_ingestion_use_physical_hirom_spans(hirom, tmp_path):
    rom, image, cfg = hirom
    authority = {'schema': 'snesrecomp disassembly authority v1',
        'rom_sha256': hashlib.sha256(rom).hexdigest(),
        'instructions': [{'pc24': 0x009000, 'bytes': '4c0010'},
                         {'pc24': 0xC09000, 'bytes': '4c0010'},
                         {'pc24': 0xC01000, 'bytes': '60'}]}
    assert materialize(rom, export_layout(authority))['instructions'] == authority['instructions']
    source = tmp_path / 'authority.json'
    source.write_text(json.dumps(authority))
    result = ingest(image, source, cfg, tmp_path / 'overlay')
    assert result['instructions'] == 2
    assert not (tmp_path / 'overlay/bank00.cfg').exists()
    assert 'authority_insn 9000 4c0010' in (tmp_path / 'overlay/bankc0.cfg').read_text()
    authority['instructions'].append({'pc24': 0x809001, 'bytes': '0010'})
    source.write_text(json.dumps(authority))
    with pytest.raises(ValueError, match='overlapping instruction'):
        ingest(image, source, cfg, tmp_path / 'conflict')
    authority['instructions'].pop()
    authority['data_regions'] = [{'start_pc24': 0x409000, 'end_pc24': 0x409003}]
    source.write_text(json.dumps(authority))
    with pytest.raises(ValueError, match='data region overlaps'):
        ingest(image, source, cfg, tmp_path / 'data-conflict')
    authority.pop('data_regions')
    authority['entries'] = [{'pc24': 0x009000, 'm': 1, 'x': 1},
                            {'pc24': 0xC09000, 'm': 0, 'x': 0}]
    source.write_text(json.dumps(authority))
    ingest(image, source, cfg, tmp_path / 'contexts')
    parsed = {bank: c for bank, _, c in _load_cfgs(tmp_path / 'contexts')}
    for bank, modes in ((0, (1, 1)), (0xC0, (0, 0))):
        entry = next(e for e in parsed[bank].entries if e.start == 0x9000)
        assert (entry.entry_m, entry.entry_x) == modes


def test_native_analyzes_short_jump_in_its_live_bank(hirom):
    _, image, cfg = hirom
    manifest, _, _, _ = build_manifest_native(rom_path=image, cfg_dir=cfg,
        additional_roots=(VariantKey(0xC09000, 1, 1),), max_insns=64)
    low = manifest.nodes[VariantKey(0x009000, 1, 1)]
    full = manifest.nodes[VariantKey(0xC09000, 1, 1)]
    assert low.disposition.value == 'aot_eligible'
    assert {(e.target.pc24, e.resolution.value) for e in low.demands} == {(0x001000, 'lle_exact')}
    assert {(e.target.pc24, e.resolution.value) for e in full.demands} == {(0xC01000, 'aot_exact')}
    assert full.disposition.value == 'aot_eligible'
    (cfg / 'bankc0.cfg').write_text('bank = C0\nfunc Jump 9000\nforce_lle C09000\n')
    manifest, _, _, _ = build_manifest_native(rom_path=image, cfg_dir=cfg, max_insns=64)
    assert VariantKey(0x009000, 1, 1) not in manifest.nodes  # forced roots stay in LLE


def test_emitter_and_native_agree_on_hirom_alias_contracts(hirom, tmp_path):
    rom, image, cfg = hirom
    rom[0x9000:0x9002] = bytes.fromhex('ea60')
    image.write_bytes(rom)
    (cfg / 'bank00.cfg').write_text('bank = 00\nauto_vectors\n')
    (cfg / 'bankc0.cfg').write_text('bank = C0\nfunc Return 9000\n')
    output = tmp_path / 'generated'
    result = subprocess.run([sys.executable, str(ROOT / 'tools/v2_emit.py'),
        '--rom', str(image), '--cfg-dir', str(cfg), '--out-dir', str(output),
        '--no-host-root-scan'], text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    manifest = json.loads((output / 'program_manifest.json').read_text())
    assert manifest['nodes']['009000:M1X1']['disposition'] == 'aot_eligible'
    (cfg / 'bankc0.cfg').write_text('bank = C0\nfunc Return 9000\nforce_lle C09000\n')
    result = subprocess.run(result.args, text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    manifest = json.loads((output / 'program_manifest.json').read_text())
    assert manifest['nodes'] == {}  # reset alias is explicitly interpreter-only
