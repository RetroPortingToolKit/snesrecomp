"""Whole-program analysis: run the native (Rust) analyzer and load its manifest.

The analyzer lives in recompiler-rs (`snesrecomp-analyze`). It decodes each
demanded exact variant, runs the exit-mode fixed point and writes one
deterministic JSON manifest; this module is the Python side of that contract
plus the cfg loading the emitter shares with it. There is one analyzer: the
former Python implementation was retired once the native one became
authoritative, so every consumer gets the same program.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile


REPO = pathlib.Path(
    os.environ.get("SNESRECOMP_ROOT", pathlib.Path(__file__).resolve().parent.parent)
).resolve()
sys.path.insert(0, str(REPO / "recompiler"))

from snes65816 import vector_table_offset  # noqa: E402
from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.program_analysis import NodeDisposition, ProgramManifest  # noqa: E402


_BANK_CFG_RE = re.compile(r"bank([0-9a-fA-F]+)\.cfg$")


def native_analyzer_path() -> pathlib.Path:
    """Return the configured/default release native-analyzer executable."""
    configured = os.environ.get("SNESRECOMP_NATIVE_ANALYZER")
    if configured:
        return pathlib.Path(configured).expanduser().resolve()
    executable = ("snesrecomp-analyze.exe" if os.name == "nt"
                  else "snesrecomp-analyze")
    return REPO / "recompiler-rs" / "target" / "release" / executable


def ensure_native_analyzer(executable=None) -> pathlib.Path:
    """Return the analyzer binary, building the release binary if absent.

    An explicit path (argument or SNESRECOMP_NATIVE_ANALYZER) must exist: a
    caller that named a binary gets that binary or an error. The default
    in-repo binary is built on demand with the pinned toolchain, exactly as
    the title regen scripts do, so a fresh checkout generates the same code
    as any other.
    """
    explicit = executable or os.environ.get("SNESRECOMP_NATIVE_ANALYZER")
    path = (pathlib.Path(executable).expanduser().resolve() if executable
            else native_analyzer_path())
    if path.is_file():
        return path
    if explicit:
        raise FileNotFoundError(f"native analyzer not found at {path}")
    print(f"analysis: building the native analyzer ({path.name})", flush=True)
    completed = subprocess.run(
        [sys.executable, str(REPO / "tools" / "build_native_analyzer.py")],
        text=True, capture_output=True, check=False)
    if completed.returncode or not path.is_file():
        detail = (completed.stderr.strip() or completed.stdout.strip())[-2000:]
        raise RuntimeError(
            "could not build the native analyzer (needs the Rust toolchain; "
            f"run `python tools/build_native_analyzer.py`): {detail}")
    return path


def build_manifest_native(*, rom_path, cfg_dir, all_cfg_roots=False,
                          additional_roots=(), executable=None,
                          max_insns=4096, max_nodes=100_000,
                          force_lle=()):
    """Run the compiled analyzer and load its stable manifest contract."""
    executable = ensure_native_analyzer(executable)
    fd, temporary = tempfile.mkstemp(
        prefix="snesrecomp-native-analysis-", suffix=".json")
    os.close(fd)
    temporary_paths = [temporary]

    def write_arg_file(prefix, values):
        fd, path = tempfile.mkstemp(prefix=prefix, suffix=".txt")
        with os.fdopen(fd, "w", encoding="ascii", newline="\n") as handle:
            for value in values:
                handle.write(value)
                handle.write("\n")
        temporary_paths.append(path)
        return path

    command = [
        str(executable),
        "--rom", str(pathlib.Path(rom_path).resolve()),
        "--cfg-dir", str(pathlib.Path(cfg_dir).resolve()),
        "--manifest", temporary,
        "--max-insns", str(int(max_insns)),
        "--max-nodes", str(int(max_nodes)),
    ]
    if all_cfg_roots:
        command.append("--all-cfg-roots")
    root_values = [
        f"{key.pc24:06X}:{key.m}:{key.x}"
        for key in sorted(set(additional_roots))
    ]
    if root_values:
        command.extend((
            "--roots-file",
            write_arg_file("snesrecomp-native-roots-", root_values),
        ))
    force_lle_values = [
        f"{pc24 & 0xFFFFFF:06X}"
        for pc24 in sorted(set(force_lle))
    ]
    if force_lle_values:
        command.extend((
            "--force-lle-file",
            write_arg_file("snesrecomp-native-force-lle-", force_lle_values),
        ))
    try:
        completed = subprocess.run(
            command, text=True, capture_output=True, check=False)
        if completed.returncode:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise RuntimeError(
                f"native analyzer exited {completed.returncode}: {detail}")
        value = json.loads(pathlib.Path(temporary).read_text(
            encoding="utf-8"))
    finally:
        for path in temporary_paths:
            try:
                os.unlink(path)
            except OSError:
                pass
    manifest = ProgramManifest.from_dict(value)
    metadata = value.get("native_analysis", {})
    helpers = {
        int(pc24, 16): str(kind)
        for pc24, kind in metadata.get("dispatch_helpers", {}).items()
    }
    inline_args = {
        int(pc24, 16): int(count)
        for pc24, count in metadata.get("inline_args", {}).items()
    }
    return manifest, helpers, inline_args, completed.stdout.strip()


def _load_cfgs(cfg_dir: pathlib.Path):
    parsed = []
    for path in sorted(cfg_dir.glob("bank*.cfg")):
        match = _BANK_CFG_RE.fullmatch(path.name)
        if match:
            parsed.append((int(match.group(1), 16), path,
                           load_bank_cfg(str(path))))
    if not parsed:
        raise ValueError(f"no bank*.cfg under {cfg_dir}")
    return parsed


def _seed_auto_vectors(parsed, rom: bytes) -> None:
    """Mirror v2_regen's byte-derived reset/NMI/IRQ roots."""
    from v2.emit_bank import BankEntry

    vector_base = vector_table_offset(rom)
    if len(rom) < vector_base + 0x20:
        return
    for bank, _path, cfg in parsed:
        if bank != 0 or not cfg.auto_vectors:
            continue
        existing_starts = {entry.start & 0xFFFF for entry in cfg.entries}

        def vector(offset: int) -> int:
            return rom[vector_base + offset] | (rom[vector_base + offset + 1] << 8)

        for name, pc in (
                ("I_RESET", vector(0x1C)),
                ("I_NMI", vector(0x0A)),
                ("I_IRQ", vector(0x0E))):
            if pc in (0, 0xFFFF) or pc in existing_starts:
                continue
            cfg.entries.append(BankEntry(name=name, start=pc))
            existing_starts.add(pc)


def _atomic_write(path: pathlib.Path, content: str) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.tmp-", dir=str(path.parent), text=True)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser(
        description="build a compact LLE-first program-analysis manifest")
    parser.add_argument("--rom", required=True)
    parser.add_argument("--cfg-dir", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--max-insns", type=int, default=4096)
    parser.add_argument("--max-nodes", type=int, default=100_000)
    parser.add_argument(
        "--all-cfg-roots", action="store_true",
        help="migration diagnostic: treat every func declaration as a root; "
             "the default correctly treats func as a boundary only")
    args = parser.parse_args()

    manifest, helpers, inline_args, output = build_manifest_native(
        rom_path=args.rom, cfg_dir=pathlib.Path(args.cfg_dir),
        all_cfg_roots=args.all_cfg_roots,
        max_insns=args.max_insns, max_nodes=args.max_nodes)
    if output:
        print(output)
    _atomic_write(pathlib.Path(args.manifest), manifest.to_json())

    counts = {disposition: 0 for disposition in NodeDisposition}
    edge_count = 0
    for node in manifest.nodes.values():
        counts[node.disposition] += 1
        edge_count += len(node.demands)
    print(
        f"analysis: {len(manifest.roots)} roots -> {len(manifest.nodes)} "
        f"exact variants, {edge_count} edges")
    print(
        f"analysis: {counts[NodeDisposition.AOT_ELIGIBLE]} AOT-eligible, "
        f"{counts[NodeDisposition.LLE_ONLY]} LLE-only; "
        f"discovered {len(helpers)} dispatch helpers and "
        f"{len(inline_args)} inline-argument routines")
    print(f"analysis: wrote {pathlib.Path(args.manifest).resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
