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
    parser.add_argument("--cfg-dir", type=Path, default=Path("recomp"))
    parser.add_argument("--rom", type=Path)
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--analysis-backend", choices=("native", "auto"), default="native")
    for option in ("max-insns", "max-nodes", "bank-shard-threshold-kib", "bank-shard-pc-span"):
        aliases = ["--" + option]
        if option == "max-insns":
            aliases.append("--max-instructions")
        parser.add_argument(*aliases, type=lambda value: int(value, 0))
    parser.add_argument("--cfg-roots", action="store_true")
    parser.add_argument("--profile", "--profile-manifest", type=Path, action="append", default=[])
    parser.add_argument("--no-default-profiles", action="store_true")
    parser.add_argument("--strict-idempotent", action="store_true")
    parser.add_argument("--no-tests", action="store_true")
    parser.add_argument("--tests", action="store_true", help="run the engine test suite after generation")
    args = parser.parse_args(argv)
    project = args.project_root.resolve()
    cfg = args.cfg_dir if args.cfg_dir.is_absolute() else project / args.cfg_dir
    ensure_tool_paths(ROOT)
    recipe = load_recipe(cfg)
    identity = recipe.identity() if recipe else parse(project / "rom_identity.txt")
    rom = args.rom or os.environ.get("SNESRECOMP_ROM")
    if not rom:
        hint = identity.get("rom_file")
        if hint and (project / hint).is_file():
            rom = hint
        elif (project / "rom.cfg").is_file():
            lines = (project / "rom.cfg").read_text(encoding="utf-8").splitlines()
            rom = lines[0].strip() if lines else None
    if not rom:
        parser.error("no ROM configured; pass --rom or set SNESRECOMP_ROM")
    rom = Path(rom).expanduser()
    if not rom.is_absolute():
        rom = project / rom
    out_dir = args.out_dir or Path(recipe.data.get("out_dir", "src/gen") if recipe else "src/gen")
    if not out_dir.is_absolute():
        out_dir = project / out_dir
    options = dict(rom=rom.resolve(), cfg_dir=cfg, out_dir=out_dir,
                   funcs_h=cfg / "funcs.h", cfg_roots=args.cfg_roots,
                   analysis_backend=args.analysis_backend,
                   emitter_options={key: getattr(args, key) for key in
                                    ("max_insns", "max_nodes", "bank_shard_threshold_kib", "bank_shard_pc_span")
                                    if getattr(args, key) is not None},
                   use_default_profiles=not args.no_default_profiles,
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
    if args.tests and not args.no_tests:
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
