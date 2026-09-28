"""Exercise a packaged CLI with a synthetic, redistributable SNES ROM."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    args = parser.parse_args()

    executable = pathlib.Path(args.executable).resolve()
    if not executable.is_file():
        parser.error(f"CLI executable not found: {executable}")

    with tempfile.TemporaryDirectory(prefix="snesrecomp-cli-smoke-") as directory:
        root = pathlib.Path(directory)
        rom = bytearray([0xFF] * 0x8000)
        rom[0] = 0x60  # RTS at $00:8000
        rom[0x7FC0 + 0x15] = 0x20  # standard LoROM mapping byte
        rom[0x7FC0 + 0x1C:0x7FC0 + 0x20] = bytes([0xFF, 0xFF, 0, 0])
        for offset in (0x0A, 0x0E, 0x1C):
            rom[0x7FE0 + offset:0x7FE0 + offset + 2] = bytes([0x00, 0x80])
        rom_path = root / "fixture.sfc"
        rom_path.write_bytes(rom)
        output = root / "project"

        completed = subprocess.run([
            str(executable), "build",
            "--rom", str(rom_path),
            "--output", str(output),
            "--name", "CI Fixture",
        ], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(completed.stdout, end="")
        completed.check_returncode()
        if "Expected build result: generated-code static library only" not in completed.stdout:
            raise RuntimeError(
                "packaged CLI omitted static-library guidance from its output")
        required = (
            output / "CMakeLists.txt",
            output / "build.ps1",
            output / "build.sh",
            output / "README.md",
            output / "config" / "bank00.cfg",
            output / "generated" / "dispatch_v2.c",
            output / "generated" / "program_manifest.json",
            output / "snesrecomp" / "LICENSE",
            output / "snesrecomp" / "THIRD_PARTY_ATTRIBUTION.md",
            output / "snesrecomp" / "runner" / "runner.cmake",
        )
        missing = [str(path) for path in required if not path.is_file()]
        if missing:
            raise RuntimeError(f"packaged CLI omitted expected output: {missing}")

        build_notice = "No playable executable was produced"
        for path in (output / "build.ps1", output / "build.sh"):
            if build_notice not in path.read_text(encoding="utf-8"):
                raise RuntimeError(
                    f"generated build helper omitted artifact guidance: {path}")

        readme = (output / "README.md").read_text(encoding="utf-8")
        if ("Expected build result" not in readme or
                "playable executable" not in readme):
            raise RuntimeError(
                "generated README omitted static-library artifact guidance")

        # The launcher drives `generate --json-progress`, not `build`. Prove the
        # packaged binary answers that contract: a package that ships the CLI
        # without the SDK modules it dispatches through, or without
        # v2_sync_funcs_h, fails here instead of in a player's rebuild.
        # Qualified titles also ship byte-free disassembly layouts. Exercise
        # their automatic materialization in the frozen CLI, not just Python.
        (output / "config" / "disassembly-layout.json").write_text(json.dumps({
            "schema": "snesrecomp disassembly layout v1",
            "rom_sha256": hashlib.sha256(rom).hexdigest(),
            "probe_entry_modes": True,
            "instructions": [["0x008000", 1]],
        }), encoding="utf-8")
        generate = subprocess.run([
            str(executable), "generate",
            "--rom", str(rom_path),
            "--cfg-dir", str(output / "config"),
            "--out-dir", str(output / "generated"),
            "--funcs-h", str(output / "config" / "funcs.h"),
            "--no-host-root-scan",
            "--json-progress",
        ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        print(generate.stderr, end="")
        if generate.returncode != 0:
            raise RuntimeError(
                f"packaged generate failed: rc={generate.returncode}\n"
                f"stdout:\n{generate.stdout}\nstderr:\n{generate.stderr}")
        events = []
        for line in generate.stdout.splitlines():
            if not line.strip():
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise RuntimeError(
                    "packaged generate stdout is not clean JSONL: "
                    f"{line!r} ({exc})") from exc
        phases = [e.get("phase") for e in events if e.get("event") == "phase"]
        for phase in ("verify", "emit", "sync_funcs_h", "done"):
            if phase not in phases:
                raise RuntimeError(
                    f"packaged generate missing phase {phase!r}: {phases}")
        if not any(e.get("event") == "result" and e.get("ok") for e in events):
            raise RuntimeError(f"packaged generate missing result event: {events}")
    print("packaged CLI smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
