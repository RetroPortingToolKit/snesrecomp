#!/usr/bin/env python3
"""Regenerate a ROM-bound project using the same recipe as its launcher."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.sdk_generate import ensure_tool_paths, generate
from tools.generation_recipe import load_recipe
from tools.rom_identity import parse


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, default=Path.cwd())
    parser.add_argument("--rom", type=Path)
    parser.add_argument("--cfg-roots", action="store_true")
    parser.add_argument("--profile", type=Path, action="append", default=[])
    parser.add_argument("--strict-idempotent", action="store_true")
    parser.add_argument("--no-tests", action="store_true")
    args = parser.parse_args(argv)
    project = args.project_root.resolve()
    cfg = project / "recomp"
    ensure_tool_paths(ROOT)
    recipe = load_recipe(cfg)
    identity = recipe.identity() if recipe else parse(project / "rom_identity.txt")
    rom = args.rom or os.environ.get("SNESRECOMP_ROM") or identity.get("rom_file")
    if not rom:
        parser.error("no ROM configured; pass --rom or set SNESRECOMP_ROM")
    rom = Path(rom).expanduser()
    if not rom.is_absolute():
        rom = project / rom
    options = dict(rom=rom.resolve(), cfg_dir=cfg, out_dir=project / "src/gen",
                   funcs_h=cfg / "funcs.h", cfg_roots=args.cfg_roots,
                   profile_manifests=[p if p.is_absolute() else project / p
                                      for p in args.profile],
                   expected_crc32=identity.get("expected_crc32") or None,
                   expected_sha256=identity.get("expected_sha256") or None)
    generate(**options)
    if args.strict_idempotent:
        from tools.v2_compare_output import compare_output
        with tempfile.TemporaryDirectory(prefix="snesrecomp-regen-compare-") as temp:
            comparison = Path(temp) / "generated"
            generate(**dict(options, out_dir=comparison, funcs_h=None))
            differences = compare_output(options["out_dir"], comparison)
            if differences:
                raise RuntimeError("regeneration differs:\n" + "\n".join(differences))
            print("Complete generated trees are byte-identical")
    if not args.no_tests:
        subprocess.run([sys.executable, str(ROOT / "tests/run_tests.py")],
                       cwd=ROOT, check=True,
                       # CREATE_NO_WINDOW has no console handles to inherit.
                       # Forward the caller's redirected streams explicitly.
                       stdout=sys.stdout, stderr=sys.stderr,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"regen_project: {exc}", file=sys.stderr)
        raise SystemExit(1)
