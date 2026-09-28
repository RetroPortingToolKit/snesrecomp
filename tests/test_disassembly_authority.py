import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "recompiler"))
from snes65816 import set_rom_mapping
from v2.cfg_loader import load_bank_cfg
from v2.decoder import decode_function, set_instruction_authority

spec = importlib.util.spec_from_file_location("authority", ROOT / "tools/ingest_disassembly_authority.py")
authority = importlib.util.module_from_spec(spec)
spec.loader.exec_module(authority)


@pytest.fixture(autouse=True)
def clear_authority():
    set_rom_mapping("lorom")
    set_instruction_authority([])
    yield
    set_instruction_authority([])
    set_rom_mapping("lorom")


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
