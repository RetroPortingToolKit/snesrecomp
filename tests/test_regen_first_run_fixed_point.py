"""A fresh checkout's first regeneration must equal every later one.

The emitter reads host aliases (`void Name(CpuState *cpu);  /* $BB:PPPP alias */`)
from the funcs.h beside the cfg files, and generation writes that same funcs.h
from the cfg. Ports keep funcs.h out of git, so a fresh checkout's first run
used to see no aliases and emitted no alias wrappers; the second run read the
header the first one wrote and differed in every bank (Yoshi's Island: 17
banks, `--strict-idempotent` failing on a fresh clone). Generation now hands
the emitter the funcs.h it is about to publish.
"""
import filecmp
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.smoke_sdk_generate import write_fixture_rom  # noqa: E402

CFG = "bank = 0\nauto_vectors\nfunc ResetEntry 8000 entry_mx:1,1\n"


def _project(tmp_path, *, recipe):
    root = tmp_path / ("recipe" if recipe else "plain") / "game"
    cfg = root / "recomp"
    cfg.mkdir(parents=True)
    rom = root / "fixture.sfc"
    raw = write_fixture_rom(rom)
    (cfg / "bank00.cfg").write_text(CFG)
    if recipe:
        (root / "rom_identity.txt").write_text(
            f"expected_sha256={hashlib.sha256(raw).hexdigest()}\n")
        (cfg / "generate.toml").write_text(
            'schema = 1\nidentity_file = "rom_identity.txt"\n')
    return root, rom


def _generate(root, rom):
    result = subprocess.run(
        [sys.executable, str(ROOT / "snesrecomp_cli.py"), "generate",
         "--project-root", str(root), "--rom", str(rom),
         "--cfg-dir", "recomp", "--out-dir", "src/gen",
         "--funcs-h", "recomp/funcs.h", "--no-host-root-scan"],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    assert result.returncode == 0, result.stdout + result.stderr


def _tree(path):
    return {p.relative_to(path).as_posix(): p for p in sorted(path.rglob("*"))
            if p.is_file() and p.name != ".snesrecomp-cache.json"}


@pytest.mark.parametrize("recipe", [False, True], ids=["plain", "recipe"])
def test_first_regeneration_is_the_fixed_point(tmp_path, recipe):
    root, rom = _project(tmp_path, recipe=recipe)
    assert not (root / "recomp/funcs.h").exists()
    _generate(root, rom)
    first = root.parent / "first"
    shutil.copytree(root / "src/gen", first)
    header = (root / "recomp/funcs.h").read_text(encoding="utf-8")
    assert "void ResetEntry(CpuState *cpu);  /* $00:8000 alias */" in header

    _generate(root, rom)
    second = _tree(root / "src/gen")
    assert set(_tree(first)) == set(second)
    drift = [name for name, path in _tree(first).items()
             if not filecmp.cmp(path, second[name], shallow=False)]
    assert drift == []
    assert (root / "recomp/funcs.h").read_text(encoding="utf-8") == header
    # The wrapper that only a second run used to emit is there the first time.
    assert any("void ResetEntry(CpuState *cpu) {" in p.read_text(encoding="utf-8")
               for p in _tree(first).values() if p.suffix == ".c")
