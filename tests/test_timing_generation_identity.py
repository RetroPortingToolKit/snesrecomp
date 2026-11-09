"""Explicit timing selection reaches emission and invalidates every cache tier."""
import argparse
import hashlib
import pathlib
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import sdk_generate
from tools.generation_recipe import load_recipe
from tools.smoke_sdk_generate import write_fixture_rom


def test_cli_defaults_and_explicit_timing():
    parser = argparse.ArgumentParser()
    sdk_generate.add_generate_parser(parser.add_subparsers(dest="command"))
    required = ["generate", "--rom", "fixture", "--cfg-dir", "cfg", "--out-dir", "gen"]
    defaults = parser.parse_args(required)
    assert not defaults.instruction_blocks and not defaults.region_paced
    explicit = parser.parse_args(required + ["--instruction-blocks", "--region-paced"])
    assert explicit.instruction_blocks and explicit.region_paced


def test_sdk_forwards_timing_to_emitter(tmp_path, monkeypatch):
    cfg = tmp_path / "recomp"
    cfg.mkdir()
    (cfg / "bank00.cfg").write_text("bank = 0\n")
    rom = tmp_path / "fixture.sfc"
    write_fixture_rom(rom)
    calls = []
    monkeypatch.setattr(sdk_generate, "run_tool", lambda tool, args: calls.append(args) or 0)
    result = sdk_generate._generate(rom=rom, cfg_dir=cfg, out_dir=tmp_path / "gen",
                                   instruction_blocks=True, region_paced=True)
    assert "--instruction-blocks" in calls[0] and result["instruction_blocks"]
    assert "--region-paced" in calls[0] and result["region_paced"]


def test_effective_recipe_options_change_identity(tmp_path, monkeypatch):
    cfg = tmp_path / "recomp"
    cfg.mkdir()
    (cfg / "bank00.cfg").write_text("bank = 0\n")
    recipe_path = cfg / "generate.toml"
    recipe_path.write_text("schema = 1\n")
    rom = tmp_path / "fixture.sfc"
    write_fixture_rom(rom)
    calls = []
    def fake_generate(**options):
        calls.append(options.copy())
        (options["out_dir"] / "synthetic.c").write_text("/* synthetic */\n")
        return {"ok": True}
    monkeypatch.setattr(sdk_generate, "_generate", fake_generate)
    for block, paced in [(False, False), (True, False), (True, True), (False, False)]:
        sdk_generate.generate(rom=rom, cfg_dir=cfg, out_dir=tmp_path / "gen",
                              instruction_blocks=block, region_paced=paced)
    digests = [call["generation_recipe_digest"] for call in calls]
    assert len(set(digests[:3])) == 3 and digests[0] == digests[3]
    expected = hashlib.sha256((load_recipe(cfg).digest() +
        "\0instruction_blocks=0\0region_paced=0").encode("ascii")).hexdigest()
    assert digests[0] == expected
    recipe_path.write_text("schema = 1\ninstruction_blocks = true\nregion_paced = true\n")
    sdk_generate.generate(rom=rom, cfg_dir=cfg, out_dir=tmp_path / "gen")
    assert calls[-1]["instruction_blocks"] and calls[-1]["region_paced"]


def test_refuse_invalid_timing_before_rom_or_output_access(tmp_path):
    with pytest.raises(ValueError, match="requires instruction-blocks"):
        sdk_generate._generate(rom=tmp_path / "absent", cfg_dir=tmp_path,
                               out_dir=tmp_path / "gen", region_paced=True)
    assert not (tmp_path / "gen").exists()


def test_emitter_output_and_bank_cache_invalidate_on_timing_change(tmp_path):
    from test_program_emit import _fixture, _emit
    rom, cfg, out = _fixture(tmp_path, target_opcode=0xEA)
    common = ["--rom", str(rom), "--cfg-dir", str(cfg), "--out-dir", str(out),
              "--no-host-root-scan"]
    for flags in [[], ["--instruction-blocks"], ["--instruction-blocks", "--region-paced"], []]:
        first = _emit(*(common + flags))
        assert first.returncode == 0, first.stdout + first.stderr
        assert "1 bank(s) emitted, 0 reused" in first.stdout
        text = (out / "bank00_v2.c").read_text()
        assert ("cpu_add_aot_master_cycles(cpu," in text) == ("--region-paced" in flags)
        repeated = _emit(*(common + flags))
        assert repeated.returncode == 0, repeated.stdout + repeated.stderr
        assert "0 bank(s) emitted, 1 reused" in repeated.stdout
