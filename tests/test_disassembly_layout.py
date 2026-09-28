import hashlib
import json
from pathlib import Path
import sys
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "recompiler"), str(ROOT / "tools")]
from disassembly_layout import (FILENAME, configured_authority, export_layout,
                                materialize)
from ingest_disassembly_authority import ingest


@pytest.fixture
def sample(tmp_path):
    rom = bytes.fromhex("a9600060") + bytes(0x8000 - 4)
    image = tmp_path / "game.sfc"
    image.write_bytes(rom)
    cfg = tmp_path / "recomp"
    cfg.mkdir()
    (cfg / "bank00.cfg").write_text("bank = 00\nfunc Start 8000\n", encoding="utf-8")
    authority = {
        "schema": "snesrecomp disassembly authority v1",
        "rom_sha256": hashlib.sha256(rom).hexdigest(),
        "instructions": [{"pc24": "0x808000", "bytes": "a96000"},
                         {"pc24": "0x808003", "bytes": "60"}],
        "data_regions": [{"start_pc24": "0x808004", "end_pc24": "0x808010"}],
    }
    layout = export_layout(authority)
    (cfg / FILENAME).write_text(json.dumps(layout), encoding="utf-8")
    return rom, image, cfg, authority, layout


def test_layout_reproduces_validated_overlay_and_accepts_copier_header(sample, tmp_path):
    rom, image, cfg, authority, layout = sample
    evidence = tmp_path / "authority.json"
    evidence.write_text(json.dumps(authority), encoding="utf-8")
    expected = tmp_path / "expected"
    ingest(image, evidence, cfg, expected)
    assert all(isinstance(row, list) and len(row) == 2 for row in layout["instructions"])
    assert '"bytes"' not in json.dumps(layout)
    image.write_bytes(bytes(512) + rom)
    original = (cfg / "bank00.cfg").read_bytes()
    with configured_authority(image, cfg) as (overlay, probe):
        assert probe
        assert (overlay / "bank00.cfg").read_bytes() == (expected / "bank00.cfg").read_bytes()
        assert not (overlay / "bank80.cfg").exists()
    assert not overlay.exists()
    assert (cfg / "bank00.cfg").read_bytes() == original


def test_layout_rejects_wrong_rom_before_changing_output(sample):
    rom, image, cfg, _, _ = sample
    image.write_bytes(b"\xea" + rom[1:])
    with pytest.raises(ValueError, match="ROM identity mismatch"):
        with configured_authority(image, cfg):
            pytest.fail("wrong ROM must never reach generation")


@pytest.mark.parametrize("span", [
    ["0x808000", 0], ["0x808000", 5], ["0x80FFFF", 2],
    ["0x7E8000", 1], ["0x808000", True], ["0x018000", 1],
])
def test_layout_rejects_invalid_instruction_spans(sample, span):
    rom, _, _, _, layout = sample
    layout["instructions"] = [span]
    with pytest.raises(ValueError, match="span|outside"):
        materialize(rom, layout)


def test_layout_rejects_overlapping_spans(sample):
    _, image, cfg, _, layout = sample
    layout["instructions"].append(["0x808001", 1])
    (cfg / FILENAME).write_text(json.dumps(layout), encoding="utf-8")
    with pytest.raises(ValueError, match="overlapping"):
        with configured_authority(image, cfg):
            pytest.fail("overlapping authority must not reach generation")


def test_sdk_generation_uses_layout_without_extra_flags(sample, tmp_path):
    _, image, cfg, _, _ = sample
    output = tmp_path / "gen"
    command = [sys.executable, str(ROOT / "snesrecomp_cli.py"), "generate",
               "--rom", str(image), "--cfg-dir", str(cfg), "--out-dir", str(output),
               "--cfg-roots"]
    subprocess.run(command, check=True, capture_output=True, text=True)
    manifest = json.loads((output / "program_manifest.json").read_text())
    assert manifest["nodes"]["008000:M0X1"]["disposition"] == "aot_eligible"
    assert manifest["nodes"]["008000:M1X1"]["disposition"] == "lle_only"
    assert "authority_conflict" in manifest["nodes"]["008000:M1X1"]["reasons"]
    before = {path.name: path.read_bytes() for path in output.iterdir() if path.is_file()}
    subprocess.run(command, check=True, capture_output=True, text=True)
    assert {path.name: path.read_bytes() for path in output.iterdir() if path.is_file()} == before
