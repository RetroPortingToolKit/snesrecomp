"""Put the host analyzer in a setup pack, or verify its staged SDK contract.

Setup packs contain a compiler/Python toolchain, not a Rust toolchain. Build
the analyzer on the packaging host so first-run generation needs no Cargo.
The shell packager bundles its runtime libraries before --smoke-only.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from native_analyzer_identity import stamp_path  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage-root", type=Path, required=True,
                        help="the staged snesrecomp source directory")
    parser.add_argument("--artifact", help="linux-x64, windows-x64, macos-arm64, ...")
    parser.add_argument("--smoke-only", action="store_true")
    args = parser.parse_args()
    stage = args.stage_root.resolve(strict=True)
    if not (stage / "snesrecomp_cli.py").is_file():
        parser.error("stage root must contain the setup pack's framework sources")
    binary_name = "snesrecomp-analyze.exe" if os.name == "nt" else "snesrecomp-analyze"
    destination = stage / "recompiler-rs/target/release" / binary_name
    # A developer's framework/analyzer override must not make a staged smoke
    # test accidentally validate the original source tree or another binary.
    env = {k: v for k, v in os.environ.items() if not k.startswith("SNESRECOMP_")}
    if args.smoke_only:
        if not destination.is_file():
            parser.error(f"setup pack has no native analyzer: {destination}")
        subprocess.run([sys.executable, str(stage / "tools/smoke_sdk_generate.py")],
                       cwd=stage, env=env, check=True)
        print("staged setup SDK smoke passed", flush=True)
        return 0
    systems = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}
    machines = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}
    host = f"{systems.get(platform.system())}-{machines.get(platform.machine().lower())}"
    if args.artifact != host:
        parser.error(f"cannot package a {host} analyzer for {args.artifact!r}")
    subprocess.run([sys.executable, str(ROOT / "tools/build_native_analyzer.py")],
                   cwd=ROOT, env=env, check=True)
    source = ROOT / "recompiler-rs/target/release" / binary_name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    # The source stamp travels with the binary: the staged SDK has no Cargo,
    # so without it the analyzer would be refused as unverifiable.
    shutil.copy2(stamp_path(source), stamp_path(destination))
    print(f"staged native analyzer: {destination}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
