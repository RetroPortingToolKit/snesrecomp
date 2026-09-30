"""The scaffold's symbol-edit -> regen contract, including the native pipeline."""

import json
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "recompiler")]
from sync_symbols import BEGIN, END, sync_symbols
from v2.cfg_loader import load_bank_cfg
from v2_sync_funcs_h import collect_function_names


def write_symbols(cfg, *entries):
    text = ""
    for name, bank, addr, emit in entries:
        text += (f'[[func]]\nname = "{name}"\nbank = {bank}\n'
                 f'addr = "{addr:04x}"\nemit = {str(emit).lower()}\n')
    (cfg / "symbols.toml").write_text(text, encoding="utf-8")


def test_sync_multibank_preserves_manual_cfg_and_removes_stale_symbols(tmp_path):
    manual = "bank = c0\nfunc Manual 1000 entry_mx:0,0\n"
    path = tmp_path / "bankc0.cfg"
    path.write_text(manual + BEGIN + "\nfunc Old 0000\n" + END + "\n# after\n", encoding="utf-8")
    write_symbols(tmp_path, ("Start", 0xc0, 0, True), ("Sound", 0xc4, 0x100, False))
    sync_symbols(tmp_path)
    assert path.read_text().startswith(manual)
    assert path.read_text().endswith("# after\n")
    assert "Old" not in path.read_text()
    cfg = load_bank_cfg(str(path))
    assert [(e.name, e.start) for e in cfg.entries] == [("Manual", 0x1000), ("Start", 0)]
    sound = load_bank_cfg(str(tmp_path / "bankc4.cfg"))
    assert not sound.entries
    assert [(s.name, s.addr_24) for s in sound.symbols] == [("Sound", 0xc40100)]
    assert sound.force_lle == {0xc40100}
    assert collect_function_names(tmp_path) == [("Start", 0xc00000), ("Manual", 0xc01000)]

    snapshot = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in tmp_path.glob("*.cfg")}
    sync_symbols(tmp_path)
    assert snapshot == {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in tmp_path.glob("*.cfg")}
    write_symbols(tmp_path)
    sync_symbols(tmp_path)
    assert path.read_text(encoding="utf-8") == manual + BEGIN + "\n" + END + "\n# after\n"
    assert not load_bank_cfg(str(tmp_path / "bankc4.cfg")).force_lle


@pytest.mark.parametrize("invalid", [
    'addr = "XXXX"', 'bank = 256', 'emit = "false"',
    'name = "bad name"', 'entry_m = 2',
])
def test_invalid_symbols_do_not_modify_any_cfg(tmp_path, invalid):
    path = tmp_path / "bank00.cfg"
    path.write_text("bank = 00\nauto_vectors\n")
    fields = dict(name='"Bad"', addr='"8000"', bank='0', emit='false')
    key, value = invalid.split(" = ")
    fields[key] = value
    (tmp_path / "symbols.toml").write_text(
        '[[func]]\nname="Good"\nbank=192\naddr="0000"\nemit=true\n'
        + "[[func]]\n" + "\n".join(f"{k} = {v}" for k, v in fields.items()))
    with pytest.raises(ValueError, match="func #2"):
        sync_symbols(tmp_path)
    assert path.read_text() == "bank = 00\nauto_vectors\n"
    assert sorted(p.name for p in tmp_path.glob("*.cfg")) == ["bank00.cfg"]


def test_aliases_share_one_lle_boundary_and_conflicts_fail(tmp_path):
    write_symbols(tmp_path, ("First", 0xc2, 0x9319, False), ("Alias", 0xc2, 0x9319, False))
    sync_symbols(tmp_path)
    cfg = load_bank_cfg(str(tmp_path / "bankc2.cfg"))
    assert len(cfg.symbols) == 2
    assert cfg.force_lle == {0xc29319}
    write_symbols(tmp_path, ("First", 0xc2, 0x9319, False), ("Alias", 0xc2, 0x9319, True))
    with pytest.raises(ValueError, match="conflicting tier"):
        sync_symbols(tmp_path)


def test_malformed_managed_block_does_not_partially_sync(tmp_path):
    path = tmp_path / "bankc4.cfg"
    path.write_text("bank = c4\n" + BEGIN + "\n", encoding="utf-8")
    write_symbols(tmp_path, ("Start", 0xc0, 0, True))
    with pytest.raises(ValueError, match="malformed"):
        sync_symbols(tmp_path)
    assert not (tmp_path / "bankc0.cfg").exists()


def test_absent_toml_is_noop(tmp_path):
    path = tmp_path / "bank00.cfg"
    path.write_text("bank = 00\n")
    assert sync_symbols(tmp_path) == []
    assert path.read_text() == "bank = 00\n"


def test_older_python_uses_tomli_fallback(tmp_path, monkeypatch):
    try:
        import tomllib
    except ModuleNotFoundError:
        import tomli as tomllib
    monkeypatch.setitem(sys.modules, "tomllib", None)
    monkeypatch.setitem(sys.modules, "tomli", tomllib)
    write_symbols(tmp_path, ("Start", 0xc0, 0, True))
    sync_symbols(tmp_path)
    assert "func Start 0000" in (tmp_path / "bankc0.cfg").read_text()


def test_missing_toml_reader_only_errors_when_symbols_exist(tmp_path, monkeypatch):
    monkeypatch.setitem(sys.modules, "tomllib", None)
    monkeypatch.setitem(sys.modules, "tomli", None)
    assert sync_symbols(tmp_path) == []
    write_symbols(tmp_path, ("Start", 0xc0, 0, True))
    with pytest.raises(ValueError, match="python -m pip install tomli"):
        sync_symbols(tmp_path)
    assert not list(tmp_path.glob("*.cfg"))


def test_existing_uppercase_cfg_and_manual_tier_override_are_preserved(tmp_path):
    path = tmp_path / "bankC0.cfg"
    manual = "bank = c0\nfunc Existing 0100 entry_mx:0,1\nforce_lle c00200\n"
    path.write_text(manual)
    write_symbols(tmp_path, ("Existing", 0xc0, 0x100, True), ("Slow", 0xc0, 0x200, False))
    sync_symbols(tmp_path)
    assert [p.name for p in tmp_path.glob("*.cfg")] == ["bankC0.cfg"]
    cfg = load_bank_cfg(str(path))
    assert len(cfg.entries) == 1
    assert (cfg.entries[0].entry_m, cfg.entries[0].entry_x) == (0, 1)
    assert cfg.force_lle == {0xc00200}
    assert path.read_text().startswith(manual)


def test_cli_generates_unreachable_hirom_symbol_and_honors_tier_toggle(tmp_path):
    # A synthetic HiROM: reset returns immediately, while the symbol at
    # $C0:0100 is unreachable from any vector. No copyrighted game data.
    rom = bytearray([0xff] * 0x10000)
    rom[0x100] = rom[0x8000] = 0x60
    rom[0xffd5] = 0x21
    rom[0xffdc:0xffe0] = bytes([0xff, 0xff, 0, 0])
    rom[0xfffc:0xfffe] = bytes([0, 0x80])
    image = tmp_path / "fixture.sfc"
    image.write_bytes(rom)
    cfg = tmp_path / "recomp"
    cfg.mkdir()
    (cfg / "bank00.cfg").write_text("bank = 00\nauto_vectors\n")
    out = tmp_path / "gen"
    command = [sys.executable, str(ROOT / "snesrecomp_cli.py"), "generate",
               "--rom", str(image), "--cfg-dir", str(cfg), "--out-dir", str(out),
               "--funcs-h", str(cfg / "funcs.h"), "--no-host-root-scan", "--json-progress"]

    def run():
        result = subprocess.run(command, text=True, capture_output=True)
        assert result.returncode == 0, result.stdout + result.stderr
        events = [json.loads(line) for line in result.stdout.splitlines()]
        assert any(e.get("event") == "result" and e.get("ok") for e in events)
        return json.loads((out / "program_manifest.json").read_text())

    write_symbols(cfg, ("Optional", 0xc0, 0x100, True))
    manifest = run()
    assert manifest["nodes"]["C00100:M1X1"]["disposition"] == "aot_eligible"
    assert "Optional_M1X1" in (out / "bankc0_v2.c").read_text()
    assert "void Optional(CpuState *cpu)" in (cfg / "funcs.h").read_text()
    # A manual entry mode override must control the explicit symbol root too.
    with (cfg / "bankc0.cfg").open("a", encoding="utf-8") as stream:
        stream.write("entry_mx_at 0100 0 1\n")
    manifest = run()
    assert "C00100:M0X1" in manifest["nodes"]
    write_symbols(cfg, ("Interpreted", 0xc0, 0x100, False))
    run()
    assert "force_lle c00100" in (cfg / "bankc0.cfg").read_text()
    assert "Optional" not in (cfg / "funcs.h").read_text()
    assert "Interpreted" not in (cfg / "funcs.h").read_text()
    assert not any("Optional_M1X1" in p.read_text() for p in out.glob("*.c"))

    # Also prove emit=false overrides an otherwise reachable hardware root.
    write_symbols(cfg, ("StayInterpreted", 0, 0x8000, False))
    manifest = run()
    assert not any(node["disposition"] == "aot_eligible"
                   for node in manifest["nodes"].values())
    assert "008000:M1X1" not in manifest["roots"]
