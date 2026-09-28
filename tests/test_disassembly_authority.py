import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "recompiler"))
from snes65816 import set_rom_mapping, clear_reloc_regions
from v2.cfg_loader import load_bank_cfg
from v2.decoder import decode_function, set_instruction_authority

spec = importlib.util.spec_from_file_location("authority", ROOT / "tools/ingest_disassembly_authority.py")
authority = importlib.util.module_from_spec(spec)
spec.loader.exec_module(authority)


@pytest.fixture(autouse=True)
def clear_authority():
    set_rom_mapping("lorom")
    clear_reloc_regions()
    set_instruction_authority([])
    clear_reloc_regions()
    yield
    set_instruction_authority([])
    set_rom_mapping("lorom")


def test_overlay_preserves_existing_mirror_execution_contracts(tmp_path):
    rom = bytes.fromhex("60") + bytes(0x8000 - 1)
    image = tmp_path / "rom.sfc"
    image.write_bytes(rom)
    cfg_dir = tmp_path / "cfg"
    cfg_dir.mkdir()
    (cfg_dir / "bank00.cfg").write_text("bank = 00\nfunc Upload 8000\nhle_spc_upload 8000\n")
    source = tmp_path / "authority.json"
    source.write_text(json.dumps({
        "schema": "snesrecomp disassembly authority v1",
        "rom_sha256": hashlib.sha256(rom).hexdigest(),
        "instructions": [{"pc24": "0x808000", "bytes": "60"}],
        "data_regions": [{"start_pc24": "0x808001", "end_pc24": "0x808002"}]}))
    output = tmp_path / "out"
    authority.ingest(image, source, cfg_dir, output)
    assert not (output / "bank80.cfg").exists()
    text = (output / "bank00.cfg").read_text()
    assert "hle_spc_upload 8000" in text
    assert "authority_insn 8000 60" in text
    assert "data_region 00 8001 8002" in text


def test_byte_verified_overlay_rejects_wrong_width_and_operand_entry(tmp_path):
    rom = bytes.fromhex("A9 60 00 60") + bytes(0x8000 - 4)
    image = tmp_path / "test.sfc"
    image.write_bytes(rom)
    cfg_dir = tmp_path / "cfg"
    cfg_dir.mkdir()
    (cfg_dir / "bank00.cfg").write_text("bank = 00\n")
    evidence = {"schema": "snesrecomp disassembly authority v1",
                "rom_sha256": hashlib.sha256(rom).hexdigest(),
                "instructions": [{"pc24": "0x008000", "bytes": "a96000"},
                                 {"pc24": "0x008003", "bytes": "60"}],
                "entries": [{"pc24": "0x008000", "m": 0, "x": 1}]}
    source = tmp_path / "evidence.json"
    source.write_text(json.dumps(evidence))
    output = tmp_path / "overlay"
    authority.ingest(image, source, cfg_dir, output)
    path = output / "bank00.cfg"
    cfg = load_bank_cfg(str(path))
    assert cfg.entries[0].entry_m == 0
    set_instruction_authority([(0, path, cfg)])
    assert not decode_function(rom, 0, 0x8000, 0, 1).authority_conflict
    assert decode_function(rom, 0, 0x8000, 1, 1).authority_conflict
    assert decode_function(rom, 0, 0x8001, 1, 1).authority_conflict
    assert not decode_function(rom, 0x80, 0x8000, 0, 1).authority_conflict
    assert decode_function(rom, 0x80, 0x8000, 1, 1).authority_conflict
    assert decode_function(rom, 0x80, 0x8001, 1, 1).authority_conflict
    evidence["instructions"][0]["bytes"] = "a96100"
    source.write_text(json.dumps(evidence))
    with pytest.raises(ValueError, match="bytes differ"):
        authority.ingest(image, source, cfg_dir, tmp_path / "bad")
    assert not (tmp_path / "bad").exists()


def test_sa1_mapping_keeps_low_high_windows_distinct():
    from snes65816 import detect_rom_mapping, rom_offset, is_rom_address
    rom = bytearray(0x8000)
    rom[0x7FD5:0x7FD7] = bytes([0x23, 0x35])
    assert detect_rom_mapping(rom) == "sa1"
    set_rom_mapping("sa1")
    assert rom_offset(0, 0x8000) == rom_offset(0xC0, 0) == 0
    assert rom_offset(0x80, 0x8000) == rom_offset(0xE0, 0) == 0x200000
    assert rom_offset(0xC3, 0x1234) == 0x31234
    assert is_rom_address(0xC0, 0x100)
    assert not is_rom_address(0x40, 0x8000)


def native_manifest(tmp_path, rom, cfgs, roots=()):
    sys.path.insert(0, str(ROOT / "tools"))
    from v2_analyze import build_manifest_native
    for name, content in cfgs.items():
        (tmp_path / name).write_text(content, encoding="utf-8")
    image = tmp_path / "fixture.sfc"
    image.write_bytes(rom)
    return build_manifest_native(rom_path=image, cfg_dir=tmp_path,
                                 all_cfg_roots=True, additional_roots=roots)[0]


def test_authoritative_data_rejects_explicit_function_through_bank_end(tmp_path):
    from v2.program_analysis import VariantKey, NodeDisposition
    rom = bytearray(0x8000)
    rom[-2:] = b'\xEA\x60'
    text = ("bank = 00\nfunc DataRoot FFFE\n"
            "authority_data FFFE 10000\ndata_region 00 FFFE 10000\n")
    manifest = native_manifest(tmp_path, rom, {"bank00.cfg": text},
                               [VariantKey(0x80FFFE, 1, 1)])
    for bank in (0, 0x80):
        node = manifest.nodes[VariantKey((bank << 16) | 0xFFFE, 1, 1)]
        assert node.disposition == NodeDisposition.LLE_ONLY
        assert "authority_conflict" in node.reasons
    path = tmp_path / "bank00.cfg"
    set_instruction_authority([(0, path, load_bank_cfg(str(path)))])
    assert decode_function(rom, 0x80, 0xFFFE, 1, 1).authority_conflict


def test_native_authority_checks_both_mirrors_and_probe_keeps_only_valid_width(tmp_path):
    from v2.program_analysis import VariantKey, NodeDisposition
    from v2.program_emit import discover_authority_roots
    rom = bytes.fromhex("A9 60 00 60") + bytes(0x8000-4)
    text = "bank = 00\nfunc WrongDefault 8000\nauthority_insn 8000 a96000\nauthority_insn 8003 60\n"
    path = tmp_path / "bank00.cfg"
    path.write_text(text)
    roots = discover_authority_roots([(0, path, load_bank_cfg(str(path)))])
    assert len(roots) == 4
    manifest = native_manifest(tmp_path, rom, {"bank00.cfg": text},
                               roots + (VariantKey(0x808000, 1, 1), VariantKey(0x808001, 1, 1)))
    assert manifest.nodes[VariantKey(0x008000, 0, 1)].disposition == NodeDisposition.AOT_ELIGIBLE
    for key in (VariantKey(0x008000, 1, 1), VariantKey(0x808000, 1, 1), VariantKey(0x808001, 1, 1)):
        assert "authority_conflict" in manifest.nodes[key].reasons


def test_materialized_ram_blob_is_not_a_new_cartridge_bank(tmp_path):
    sys.path.insert(0, str(ROOT / "tools"))
    from v2_emit import _install_ram_routines
    from v2.program_analysis import VariantKey, NodeDisposition
    text = "bank = 00\nram_routine 7F8000 M1X1 EA60\n"
    rom = bytes(0x8000)
    key = VariantKey(0x018000, 1, 1)  # would alias the appended capture
    manifest = native_manifest(tmp_path, rom, {"bank00.cfg": text}, [key])
    assert manifest.nodes[key].disposition == NodeDisposition.LLE_ONLY
    assert manifest.nodes[VariantKey(0x7F8000, 1, 1)].disposition == NodeDisposition.AOT_ELIGIBLE
    path = tmp_path / "bank00.cfg"
    extended, _roots = _install_ram_routines(rom, [(0, path, load_bank_cfg(str(path)))])
    assert not decode_function(extended, 1, 0x8000, 1, 1).insns
    assert len(decode_function(extended, 0x7F, 0x8000, 1, 1).insns) == 2


def test_mirrored_multi_exit_fact_does_not_mutate_previous_solver_round(tmp_path):
    from v2.program_analysis import VariantKey
    rom = b'\x60' + bytes(0x8000-1)
    manifest = native_manifest(tmp_path, rom, {
        "bank00.cfg": "bank = 00\nfunc Declared 8000\nexit_mx_set 008000 M1X1 M0X0,M1X1\n",
        "bank80.cfg": "bank = 80\nfunc Inferred 8000\n"})
    assert manifest.exit_mode_sets[VariantKey(0x808000, 1, 1)] == frozenset({(0, 0), (1, 1)})


def test_audit_distinguishes_mirrored_code_data_width_and_unknown(tmp_path):
    sys.path.insert(0, str(ROOT / "tools"))
    from audit_disassembly import Authority
    rom = bytes.fromhex("A9 60 00 60") + bytes(0x8000-4)
    value = {"schema": "snesrecomp disassembly authority v1",
             "rom_sha256": hashlib.sha256(rom).hexdigest(),
             "instructions": [{"pc24": "0x008000", "bytes": "a96000"}],
             "data_regions": [{"start_pc24": "0x008003", "end_pc24": "0x008004"}]}
    a = Authority(rom, value)
    assert a.classify(0x808000, 3)[0] == "match"
    assert a.classify(0x808000, 2)[0] == "length_mismatch"
    assert a.classify(0x808001, 2)[0] == "mid_instruction"
    assert a.classify(0x808003, 1)[0] == "data"
    assert a.classify(0x7F8000, 1)[0] == "unknown"
