"""Self-contained ROM-to-source front end for snesrecomp."""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import sys


def resource_root() -> pathlib.Path:
    frozen = getattr(sys, "_MEIPASS", None)
    return pathlib.Path(frozen).resolve() if frozen else pathlib.Path(__file__).resolve().parent


ROOT = resource_root()
os.environ["SNESRECOMP_ROOT"] = str(ROOT)
for path in (ROOT, ROOT / "recompiler", ROOT / "tools"):
    value = str(path)
    if value not in sys.path:
        sys.path.insert(0, value)

from snes65816 import detect_rom_mapping, load_rom  # noqa: E402
from tools import v2_emit  # noqa: E402
# generate / verify-rom are the documented headless contract
# (docs/LOCAL_CODEGEN_SDK.md): one JSON object per line on stdout, exit 3 on a
# ROM digest mismatch. tools/sdk_generate.py is that contract's only
# implementation -- this front end must not grow a second one, because the copy
# that lived here shipped without --json-progress and every launcher-driven
# rebuild died with "unrecognized arguments: --json-progress" (exit 2).
from tools.sdk_generate import (  # noqa: E402
    EXIT_ERROR,
    add_generate_parser,
    add_verify_parser,
)
from tools.sdk_progress import ProgressReporter  # noqa: E402


def safe_name(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9]+", "_", value).strip("_")
    if not cleaned:
        return "SNESGameRecomp"
    if cleaned[0].isdigit():
        cleaned = "Game_" + cleaned
    return cleaned + "Recomp"


def write_text(path: pathlib.Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8", newline="\n")


def run_tool(tool, arguments: list[str]) -> int:
    original = sys.argv
    try:
        sys.argv = [tool.__file__, *arguments]
        try:
            result = tool.main()
        except SystemExit as exc:
            result = exc.code
        return int(result or 0)
    finally:
        sys.argv = original


ROM_SUFFIXES = (".sfc", ".smc")


def read_rom(path: pathlib.Path) -> bytes:
    """Validate ROM shape and return its bytes, or raise ValueError."""
    if not path.is_file():
        raise ValueError(f"ROM not found: {path}")
    if path.suffix.lower() not in ROM_SUFFIXES:
        raise ValueError("ROM must be an .sfc or .smc file")
    raw = path.read_bytes()
    if len(raw) < 32 * 1024 or len(raw) > 16 * 1024 * 1024:
        raise ValueError("ROM size is outside the supported 32 KiB to 16 MiB range")
    if len(raw) % 1024 not in (0, 512):
        raise ValueError("ROM size is not a standard SNES image size")
    return raw


def resolve_analyzer(backend: str) -> str:
    """Point the emitter at the native analyzer, the only analyzer there is.

    Packaged builds ship the binary beside this file, so it is named here
    rather than left to the emitter's repo-relative lookup. In a source
    checkout without a built binary the emitter builds it. `auto` and
    `native` both mean native; `python` names the retired analyzer.
    """
    if backend == "python":
        raise RuntimeError(
            "the Python analyzer was retired; the native analyzer is the "
            "only one (drop --analysis-backend python)")
    analyzer = ROOT / "recompiler-rs" / "target" / "release" / (
        "snesrecomp-analyze.exe" if os.name == "nt" else "snesrecomp-analyze")
    if analyzer.is_file():
        os.environ["SNESRECOMP_NATIVE_ANALYZER"] = str(analyzer)
    return "native"


def run_emit(rom: pathlib.Path, cfg_dir: pathlib.Path, out_dir: pathlib.Path,
             *, backend: str = "auto", cfg_roots: bool = False,
             no_host_root_scan: bool = False,
             source_roots: list[str] | None = None,
             profile_manifests: list[str] | None = None) -> None:
    """Generate C from a ROM plus its bank configs. Raises on failure."""
    resolved = resolve_analyzer(backend)
    arguments = [
        "--rom", str(rom),
        "--cfg-dir", str(cfg_dir),
        "--out-dir", str(out_dir),
        "--analysis-backend", resolved,
    ]
    if cfg_roots:
        arguments.append("--cfg-roots")
    if no_host_root_scan:
        arguments.append("--no-host-root-scan")
    for root in source_roots or []:
        arguments.extend(["--source-root", root])
    # Runtime profiles select OPTIONAL ahead-of-time work: a manifest of
    # observed tier-2 coverage seeds extra AOT roots, so it changes which
    # functions are compiled versus interpreted. v2_emit has always taken
    # these; this front end did not forward them, which meant a project that
    # used one could not be regenerated through the modern entry point at
    # all -- SuperMetroidRecomp's tools/regen.sh had to call v2_emit
    # directly, and a template sync that pointed it here silently dropped
    # its profile and changed the emitted C.
    for manifest in profile_manifests or []:
        arguments.extend(["--profile-manifest", manifest])
    if run_tool(v2_emit, arguments):
        raise RuntimeError("source generation failed")


def build_project(args: argparse.Namespace) -> int:
    rom_path = pathlib.Path(args.rom).expanduser().resolve()
    output = pathlib.Path(args.output).expanduser().resolve()
    raw = read_rom(rom_path)
    if output.exists():
        if not output.is_dir():
            raise ValueError(f"output path is not a directory: {output}")
        if any(output.iterdir()):
            raise ValueError(f"output directory is not empty: {output}")

    title = args.name or rom_path.stem
    project_name = safe_name(title)
    normalized_rom = load_rom(str(rom_path))
    mapping = detect_rom_mapping(normalized_rom)

    config_dir = output / "config"
    generated_dir = output / "generated"
    config_dir.mkdir(parents=True, exist_ok=True)
    write_text(config_dir / "bank00.cfg", "bank = 0\nauto_vectors\n")
    write_text(config_dir / "funcs.h", """/* Starter declarations for generated C. */
#pragma once
#include "cpu_state.h"
""")

    print("[1/4] Created the starter bank configuration.")

    print("[2/4] Analyzing the ROM and generating C source...")
    run_emit(rom_path, config_dir, generated_dir,
             backend="native", no_host_root_scan=True)

    print("[3/4] Copying the integration framework...")
    runner_source = ROOT / "framework" / "runner"
    if not runner_source.is_dir():
        runner_source = ROOT / "runner"
    if not (runner_source / "runner.cmake").is_file():
        raise RuntimeError("the packaged runner framework is missing")
    framework_output = output / "snesrecomp"
    shutil.copytree(runner_source, framework_output / "runner")
    framework_root = ROOT / "framework"
    if not (framework_root / "LICENSE").is_file():
        framework_root = ROOT
    # Both notices, each with the same packaged-vs-source fallback. This was
    # two straight-line copy2 calls with an orphaned `if not source.is_file()`
    # fragment dangling off the second one -- the remains of this loop, left
    # by a conflict resolution. It is a SyntaxError, so the whole module fails
    # to import and `snesrecomp_cli.py generate` cannot run at all: every
    # port's tools/regen.sh dies with IndentationError before it reaches the
    # ROM. Nothing caught it because no test imports this file.
    for notice in ("LICENSE", "THIRD_PARTY_ATTRIBUTION.md"):
        source = framework_root / notice
        if not source.is_file():
            source = ROOT / "framework" / notice
        shutil.copy2(source, framework_output)

    cmake = f"""cmake_minimum_required(VERSION 3.20)
project({project_name} C)
set(CMAKE_C_STANDARD 11)

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS
  "${{CMAKE_CURRENT_SOURCE_DIR}}/generated/*.c")
add_library(snesrecomp_game STATIC ${{GENERATED_SOURCES}})
target_include_directories(snesrecomp_game PRIVATE
  "${{CMAKE_CURRENT_SOURCE_DIR}}/config"
  "${{CMAKE_CURRENT_SOURCE_DIR}}/snesrecomp/runner/src"
  "${{CMAKE_CURRENT_SOURCE_DIR}}/snesrecomp/runner/src/snes")
if(NOT MSVC)
  target_compile_options(snesrecomp_game PRIVATE
    -w -Wno-implicit-function-declaration)
endif()
"""
    write_text(output / "CMakeLists.txt", cmake)
    write_text(output / "build.ps1", """$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
cmake -S $Root -B (Join-Path $Root 'build') -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build (Join-Path $Root 'build') --config Release --parallel
if ($LASTEXITCODE -eq 0) {
    Write-Host 'No playable executable was produced; this build creates the generated-code static library only.'
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host ''
Write-Host 'Built the generated-code static library.'
Write-Host 'No playable executable was produced; see README.md under "Continue the port".'
exit 0
""")
    write_text(output / "build.sh", """#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --config Release --parallel
printf '\n%s\n' 'Built the generated-code static library.'
printf '%s\n' 'No playable executable was produced; see README.md under "Continue the port".'
echo "No playable executable was produced; this build creates the generated-code static library only."
""")
    write_text(output / ".gitignore", "build/\ngenerated/\n")
    write_text(output / "project.txt", (
        f"name={title}\n"
        f"rom_file={rom_path.name}\n"
        f"rom_sha256={hashlib.sha256(raw).hexdigest()}\n"
        f"normalized_size={len(normalized_rom)}\n"
        f"mapping={mapping}\n"
    ))
    write_text(output / "README.md", f"""# {title} recompilation project

Generated locally from your ROM by snesrecomp.

## Build the generated source

Install CMake, Ninja, and a C compiler. On Windows, run:

```powershell
.\\build.ps1
```

On macOS or Linux, run `sh build.sh`.

**Expected build result:** a static library named `snesrecomp_game`, not a
playable executable. The library contains the automatically discovered
recompiled code. The original ROM is not copied into this project.

Expected build result: generated-code static library only. No playable executable is produced by this starter project.

## Continue the port

An arbitrary SNES game still needs game-specific function boundaries,
indirect-dispatch configuration, and a host application before it is a
playable native port. Add those declarations under `config/`, regenerate the
source, and integrate the library with the runner under `snesrecomp/runner`.

`generated/` is derived from copyrighted ROM data. Do not redistribute it
unless you have permission.
""")
    print("[4/4] Wrote project files.")
    print(f"\nReady: {output}")
    print("Expected build result: generated-code static library only")
    print(f"Build with: {output / ('build.ps1' if os.name == 'nt' else 'build.sh')}")
    print("Expected build result: generated-code static library only.")
    print("A playable executable requires game-specific host integration; "
          "see the generated README.md.")
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="snesrecomp",
        description="Turn a SNES ROM into a recompilation source project.")
    commands = result.add_subparsers(dest="command", required=True)
    build = commands.add_parser(
        "build", help="generate C source and build scripts from a ROM")
    build.add_argument("--rom", required=True, help="path to a .sfc or .smc ROM")
    build.add_argument("--output", "-o", required=True, help="new output directory")
    build.add_argument("--name", help="project title (defaults to the ROM filename)")
    build.set_defaults(handler=build_project)

    add_generate_parser(commands)
    add_verify_parser(commands)

    return result


def main() -> int:
    arguments = parser().parse_args()
    handler = arguments.handler

    # The SDK commands own their own ProgressReporter and exit codes (0/1/2/3,
    # docs/LOCAL_CODEGEN_SDK.md); build is the interactive scaffolder.
    if arguments.command in ("generate", "verify-rom"):
        progress = ProgressReporter(
            json_progress=bool(getattr(arguments, "json_progress", False)),
        )
        if arguments.command == "generate":
            # Packaged builds ship the native analyzer beside this file, so the
            # backend is resolved here rather than left to the emitter's own
            # repo-relative lookup.
            try:
                arguments.analysis_backend = resolve_analyzer(
                    arguments.analysis_backend)
            except RuntimeError as exc:
                progress.error(str(exc), code=EXIT_ERROR)
                return EXIT_ERROR
        return handler(arguments, progress)

    try:
        return handler(arguments)
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"snesrecomp: error: {exc}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    raise SystemExit(main())
