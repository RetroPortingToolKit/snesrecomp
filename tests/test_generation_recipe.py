"""Actual SDK generation must preserve project work and apply its recipe."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.smoke_sdk_generate import write_fixture_rom


def project(tmp_path, hook, *, prepare=""):
    root = tmp_path / "game with spaces"
    cfg = root / "recomp"
    cfg.mkdir(parents=True)
    rom = root / "fixture.sfc"
    raw = write_fixture_rom(rom)
    sha = hashlib.sha256(raw).hexdigest()
    (root / "rom_identity.txt").write_text(f"expected_sha256={sha}\n")
    (cfg / "bank00.cfg").write_text("bank = 0\nauto_vectors\n")
    (root / "hook.py").write_text(hook)
    (cfg / "generate.toml").write_text(
        'schema = 1\nidentity_file = "rom_identity.txt"\n'
        'cfg_roots = true\n' + prepare +
        '[[postprocess]]\nscript = "hook.py"\nargs = ["{out_dir}"]\n')
    return root, rom


def run(root, rom):
    return subprocess.run(
        [sys.executable, str(ROOT / "snesrecomp_cli.py"), "generate",
         "--project-root", str(root), "--rom", str(rom),
         "--cfg-dir", "recomp", "--out-dir", "src/gen",
         "--funcs-h", "recomp/funcs.h", "--json-progress"],
        capture_output=True, text=True, encoding="utf-8", errors="replace")


def test_cli_applies_hook_and_keeps_jsonl_and_identity(tmp_path):
    root, rom = project(tmp_path,
        'import pathlib,sys\np=pathlib.Path(sys.argv[1])/"dispatch_v2.c"\n'
        'p.write_text(p.read_text()+"\\n/* game hook */\\n")\nprint("hook finished")\n')
    result = run(root, rom)
    assert result.returncode == 0, result.stdout + result.stderr
    events = [json.loads(line) for line in result.stdout.splitlines()]
    assert events[-1]["event"] == "result" and events[-1]["ok"]
    assert events[-1]["out_dir"] == str(root / "src/gen")
    assert events[-1]["rom"]["verified"]
    assert "/* game hook */" in (root / "src/gen/dispatch_v2.c").read_text()
    assert (root / "recomp/funcs.h").is_file()
    # Re-running uses fresh emitter output, not yesterday's transformed text.
    assert run(root, rom).returncode == 0
    assert (root / "src/gen/dispatch_v2.c").read_text().count("/* game hook */") == 1


def test_failed_hook_preserves_published_tree_and_header(tmp_path):
    root, rom = project(tmp_path,
        'import pathlib,sys\np=pathlib.Path(sys.argv[1])/"dispatch_v2.c"\n'
        'p.write_text("incomplete")\nraise SystemExit(9)\n')
    target = root / "src/gen"
    target.mkdir(parents=True)
    (target / "dispatch_v2.c").write_text("known good C")
    header = root / "recomp/funcs.h"
    header.write_text("known good header")
    result = run(root, rom)
    assert result.returncode == 1
    assert "hook.py failed (exit 9)" in result.stdout
    assert (target / "dispatch_v2.c").read_text() == "known good C"
    assert header.read_text() == "known good header"


def test_identity_failure_precedes_prepare_hook(tmp_path):
    root, rom = project(tmp_path, 'print("unused")\n', prepare=
        '[[prepare]]\nscript = "prepare.py"\n')
    (root / "prepare.py").write_text('from pathlib import Path\nPath("touched").touch()\n')
    raw = bytearray(rom.read_bytes()); raw[100] ^= 1; rom.write_bytes(raw)
    result = run(root, rom)
    assert result.returncode == 3
    assert not (root / "touched").exists()


def test_hook_source_participates_in_generation_identity(tmp_path):
    from tools.generation_recipe import load_recipe
    root, _rom = project(tmp_path, 'print("v1")\n')
    recipe = load_recipe(root / "recomp")
    before = recipe.digest()
    (root / "hook.py").write_text('print("v2")\n')
    assert recipe.digest() != before


def test_frozen_sdk_runs_hook_with_available_python(tmp_path, monkeypatch):
    from tools.generation_recipe import load_recipe
    from tools.sdk_progress import ProgressReporter
    root, rom = project(tmp_path,
        'import pathlib,sys\n(pathlib.Path(sys.argv[1])/"hook.ok").touch()\n')
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setenv("PYTHON", sys.executable)
    recipe = load_recipe(root / "recomp")
    recipe.run("postprocess", rom=rom, cfg_dir=root / "recomp",
               out_dir=root, reporter=ProgressReporter())
    assert (root / "hook.ok").is_file()


@pytest.mark.parametrize("field", ['schema = 2', 'cfg_roots = "yes"',
                                    'unknown_key = true'])
def test_bad_recipe_refused(tmp_path, field):
    from tools.generation_recipe import load_recipe
    root, _rom = project(tmp_path, 'pass\n')
    recipe = root / "recomp/generate.toml"
    recipe.write_text(("" if field.startswith("schema") else "schema = 1\n") + field)
    with pytest.raises(ValueError):
        load_recipe(root / "recomp")
