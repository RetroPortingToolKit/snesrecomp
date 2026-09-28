"""Analyze once, then atomically emit exact manifest-selected AOT variants."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import sys
import time


REPO = pathlib.Path(
    os.environ.get("SNESRECOMP_ROOT", pathlib.Path(__file__).resolve().parent.parent)
).resolve()
sys.path.insert(0, str(REPO / "recompiler"))
sys.path.insert(0, str(REPO / "tools"))

from snes65816 import (  # noqa: E402
    clear_reloc_regions,
    load_rom,
    register_reloc_region,
    set_rom_image_size,
)
from v2.link_closure import assert_closed  # noqa: E402
from v2.program_analysis import VariantKey  # noqa: E402
from v2.program_emit import (  # noqa: E402
    CACHE_FORMAT_VERSION,
    discover_host_roots,
    discover_authority_roots,
    discover_profile_roots,
    emit_program,
    validate_module_identity,
)
from v2_analyze import (  # noqa: E402
    _load_cfgs,
    _seed_auto_vectors,
    build_manifest_native,
    ensure_native_analyzer,
)
from disassembly_layout import configured_authority  # noqa: E402


def _tree_digest(paths) -> str:
    digest = hashlib.sha256()
    for path in sorted({pathlib.Path(p).resolve() for p in paths}):
        if path.is_dir():
            files = sorted(
                p for p in path.rglob("*")
                if p.is_file() and p.suffix in (".py", ".rs", ".toml"))
        else:
            files = [path]
        for file in files:
            try:
                identity = str(file.relative_to(REPO)).replace("\\", "/")
            except ValueError:
                identity = f"external/{file.name}"
            digest.update(identity.encode())
            digest.update(b"\0")
            digest.update(file.read_bytes())
            digest.update(b"\0")
    return digest.hexdigest()


def _config_digest(parsed) -> str:
    digest = hashlib.sha256()
    cfg_dirs = set()
    for _bank, path, _cfg in parsed:
        cfg_dirs.add(path.resolve().parent)
        digest.update(path.name.encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    for cfg_dir in sorted(cfg_dirs):
        header = cfg_dir / "funcs.h"
        if header.exists():
            digest.update(b"funcs.h\0")
            digest.update(header.read_bytes())
            digest.update(b"\0")
    return digest.hexdigest()


def _analysis_input_digest(*, rom: bytes, generator_digest: str,
                           config_digest: str, additional_roots,
                           force_lle,
                           cfg_roots: bool,
                           analysis_backend: str,
                           enable_hle: bool, max_insns: int,
                           max_nodes: int, shard_threshold_bytes: int,
                           shard_pc_span: int,
                           module_id: str = "main",
                           module_prefix: str | None = None) -> str:
    value = {
        "format": CACHE_FORMAT_VERSION,
        "rom": hashlib.sha256(rom).hexdigest(),
        "generator": generator_digest,
        "config": config_digest,
        "additional_roots": [
            (key.pc24, key.m, key.x) for key in sorted(additional_roots)
        ],
        "force_lle": [int(pc24) for pc24 in sorted(force_lle)],
        "cfg_roots": bool(cfg_roots),
        "analysis_backend": str(analysis_backend),
        "hle": bool(enable_hle),
        "module": [str(module_id), str(module_prefix or "")],
        "max_insns": int(max_insns),
        "max_nodes": int(max_nodes),
        "sharding": [int(shard_threshold_bytes), int(shard_pc_span)],
    }
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _verified_cached_stats(out_dir: pathlib.Path,
                           analysis_input_digest: str):
    """Return published-generation stats only after verifying every output.

    A matching input key alone is insufficient: a manually edited, truncated,
    or partially copied generated tree must force normal atomic regeneration.
    """
    out_dir = out_dir.resolve()
    try:
        cache = json.loads(
            (out_dir / ".snesrecomp-cache.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if (cache.get("format_version") != CACHE_FORMAT_VERSION
            or cache.get("analysis_input_digest") != analysis_input_digest):
        return None
    outputs = cache.get("outputs")
    stats = cache.get("stats")
    if not isinstance(outputs, dict) or not outputs or not isinstance(stats, dict):
        return None
    for name, expected in sorted(outputs.items()):
        path = (out_dir / name).resolve()
        if out_dir not in path.parents or not path.is_file():
            return None
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            return None
    required_stats = ("roots", "emitted_variants", "lle_variants", "banks")
    if any(not isinstance(stats.get(name), int) for name in required_stats):
        return None
    return stats


def _install_ram_routines(rom: bytes, parsed):
    """Append each `ram_routine` blob to the ROM image and register a reloc
    region redirecting its WRAM entry to the appended bytes, so the standard
    offset-based Python decoder materializes an AOT body for it (matching the
    native analyzer, which does the same against the ROM file). Returns
    (extended_rom, tuple_of_VariantKey_roots)."""
    clear_reloc_regions()
    set_rom_image_size(len(rom))
    buf = bytearray(rom)
    roots = []
    for _bank, _path, cfg in parsed:
        for rr in getattr(cfg, "ram_routines", ()):  # noqa: B009
            base = len(buf)
            buf += rr.data
            buf += b"\x00" * 8   # guard pad (outside the reloc region)
            register_reloc_region((rr.pc24 >> 16) & 0xFF, rr.pc24 & 0xFFFF,
                                  len(rr.data), base)
            roots.append(VariantKey(rr.pc24, rr.entry_m, rr.entry_x))
    return bytes(buf), tuple(roots)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="manifest-driven LLE-first v2 generation")
    parser.add_argument("--rom", required=True)
    parser.add_argument("--cfg-dir", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--source-root", action="append", default=[])
    parser.add_argument(
        "--profile-manifest", action="append", default=[],
        help="tier2 coverage manifest whose clean targets become optional "
             "AOT roots (repeatable)")
    parser.add_argument("--legacy-profile-rom-sha256", help="Explicitly associate legacy profiles with this ROM digest")
    parser.add_argument(
        "--historical-profile-manifest", action="append", default=[],
        help="Retain seeds from a separate historical checkpoint/bundle; "
             "validate each identity independently, never merge its costs")
    parser.add_argument(
        "--cfg-roots", action="store_true",
        help="treat every cfg `func` declaration as an analysis root in "
             "addition to the architectural vectors. This is the static-"
             "coverage policy: the declared surface is materialized as AOT "
             "wherever the analysis proves it; LLE remains the failsafe for "
             "anything unprovable, never the plan of record.")
    parser.add_argument(
        "--module-id", default="main",
        help="program-module id recorded in the generated module_v2.c "
             "descriptor and used by the runtime registry "
             "(runner/src/program_module.h) to select this tree "
             "(default: main)")
    parser.add_argument(
        "--module-prefix", default=None,
        help="prefix every generated symbol as <prefix>_<name> through a "
             "generated module_namespace.h so this module can be linked "
             "beside another generated module (content variants). Requires "
             "a funcs.h beside the cfg files; default: no prefix")
    parser.add_argument("--no-host-root-scan", action="store_true")
    parser.add_argument("--disassembly-entry-modes", action="store_true",
                        help="probe all M/X modes at byte-authoritative declared entries; "
                             "contradictory paths remain LLE (requires gameplay qualification)")
    parser.add_argument("--no-hle", action="store_true")
    parser.add_argument("--max-insns", type=int, default=4096)
    parser.add_argument("--max-nodes", type=int, default=100_000)
    parser.add_argument(
        "--analysis-backend", choices=("auto", "native", "python"),
        default="native",
        help="accepted for compatibility: the native analyzer is the only "
             "one; auto means native, and python is an error")
    parser.add_argument(
        "--no-link-closure-check", action="store_true",
        help="skip the post-emit check that every called <Name>_M<m>X<x> "
             "variant is also defined somewhere in the generated tree. The "
             "check exists because an unclosed tree otherwise surfaces as "
             "linker noise long after generation reported success; turn it "
             "off only to inspect a known-broken tree")
    parser.add_argument(
        "--bank-shard-threshold-kib", type=int, default=4096,
        help="shard generated banks at or above this source size into "
             "stable translation units (default: 4096 KiB; 0 shards every "
             "non-empty bank)")
    parser.add_argument(
        "--bank-shard-pc-span", type=lambda value: int(value, 0),
        default=0x0800,
        help="entry-PC range per bank translation unit (default: 0x800; "
             "0 disables sharding)")
    args = parser.parse_args()
    if args.analysis_backend == "python":
        parser.error("the Python analyzer was retired; the native analyzer "
                     "is the only one (drop --analysis-backend python)")
    try:
        with configured_authority(args.rom, args.cfg_dir) as (cfg_dir, probe_modes):
            args.disassembly_entry_modes |= probe_modes
            return _generate(args, parser, cfg_dir)
    except (ValueError, KeyError, OSError) as exc:
        parser.error(str(exc))


def _generate(args, parser, cfg_dir):
    shard_threshold_bytes = max(0, args.bank_shard_threshold_kib) * 1024
    shard_pc_span = max(0, args.bank_shard_pc_span)

    started = time.perf_counter()
    out_dir = pathlib.Path(args.out_dir).resolve()
    rom = load_rom(args.rom)
    try:
        validate_module_identity(args.module_id, args.module_prefix)
    except ValueError as exc:
        parser.error(str(exc))
    # The module descriptor names the IMAGE this tree was generated from, so
    # take the digest before ram_routine blobs are materialized into the copy
    # the analyzer sees.
    rom_sha256_hex = hashlib.sha256(rom).hexdigest()
    rom_image_size = len(rom)
    parsed = _load_cfgs(cfg_dir)
    # Materialize ram_routine blobs into the ROM image + reloc registry so
    # their WRAM entries decode as ordinary AOT bodies. The native analyzer
    # seeds the same WRAM roots from cfg; passing them as additional roots
    # too keeps the demand explicit.
    rom, ram_routine_roots = _install_ram_routines(rom, parsed)
    try:
        native_path = ensure_native_analyzer()
    except (OSError, RuntimeError) as exc:
        parser.error(str(exc))
    analysis_backend = "native"
    source_roots = [pathlib.Path(p).resolve() for p in args.source_root]
    if not source_roots and not args.no_host_root_scan:
        conventional = pathlib.Path(args.cfg_dir).resolve().parent / "src"
        if conventional.exists():
            source_roots.append(conventional)
    host_roots = () if args.no_host_root_scan else discover_host_roots(
        parsed, source_roots, excluded_roots=(out_dir,))
    declared_entry_pcs = {
        ((bank & 0xFF) << 16) | (entry.start & 0xFFFF)
        for bank, _path, cfg in parsed for entry in cfg.entries
    }
    try:
        profile_force_lle = set()
        profile_roots = discover_profile_roots(
            args.profile_manifest, declared_entry_pcs, profile_force_lle,
            expected_rom=rom_sha256_hex, expected_module=args.module_id,
            legacy_rom=args.legacy_profile_rom_sha256,
            historical_paths=args.historical_profile_manifest)
    except ValueError as exc:
        parser.error(str(exc))
    if profile_force_lle and parsed:
        # Coverage and promotion are separate contracts. Keep every observed
        # boundary in the exact LLE manifest, but only let the profile's
        # explicitly qualified targets reach AOT eligibility.
        parsed[0][2].force_lle.update(profile_force_lle)
    additional_roots = tuple(sorted(
        set(host_roots) | set(profile_roots) | set(ram_routine_roots)
        | (set(discover_authority_roots(parsed)) if args.disassembly_entry_modes else set())))

    def generator_digest_for():
        tree_digest = _tree_digest((
            REPO / "recompiler" / "v2", pathlib.Path(__file__).resolve(),
            REPO / "recompiler" / "snes65816.py",
            REPO / "tools" / "v2_analyze.py",
            REPO / "tools" / "disassembly_layout.py",
            REPO / "tools" / "ingest_disassembly_authority.py",
            REPO / "recompiler-rs" / "src",
            REPO / "recompiler-rs" / "Cargo.toml",
            REPO / "recompiler-rs" / "Cargo.lock",
            native_path))
        # This environment switch changes every emitted AOT body, so it must
        # participate in the published-output cache key.  Treat any non-empty
        # value as enabled to match emit_function.py's codegen guard.
        deny_gate = bool(os.environ.get("SNESRECOMP_EMIT_AOT_DENY_GATE"))
        return hashlib.sha256(
            f"{tree_digest}\0aot_deny_gate={int(deny_gate)}".encode()
        ).hexdigest()

    generator_digest = generator_digest_for()
    config_digest = _config_digest(parsed)
    analysis_input_digest = _analysis_input_digest(
        rom=rom,
        generator_digest=generator_digest,
        config_digest=config_digest,
        additional_roots=additional_roots,
        force_lle=profile_force_lle,
        cfg_roots=args.cfg_roots,
        analysis_backend=analysis_backend,
        enable_hle=not args.no_hle,
        max_insns=args.max_insns,
        max_nodes=args.max_nodes,
        shard_threshold_bytes=shard_threshold_bytes,
        shard_pc_span=shard_pc_span,
        module_id=args.module_id, module_prefix=args.module_prefix,
    )
    cached = _verified_cached_stats(out_dir, analysis_input_digest)
    if cached is not None and not args.no_link_closure_check:
        # Reuse re-publishes an earlier run's verdict. If that tree predates
        # this check (or was produced with it off), reusing it would silently
        # hand the compiler the same unlinkable output again.
        assert_closed(out_dir)
    if cached is not None:
        elapsed = time.perf_counter() - started
        print(
            f"v2_emit: {cached['roots']} roots, "
            f"{cached['emitted_variants']} exact AOT variants, "
            f"{cached['lle_variants']} LLE variants")
        print(
            f"v2_emit: 0 bank(s) emitted, {cached['banks']} reused "
            f"in {elapsed:.2f}s")
        print(f"v2_emit: reused verified published output {out_dir}")
        return 0

    # The emitter's cfg model needs the friendly auto_vectors entries
    # (I_RESET/I_NMI/I_IRQ); the native analyzer seeds its own copy.
    _seed_auto_vectors(parsed, rom)
    try:
        manifest, helpers, inline_args, native_output = build_manifest_native(
            rom_path=args.rom, cfg_dir=cfg_dir,
            all_cfg_roots=args.cfg_roots,
            additional_roots=additional_roots,
            force_lle=profile_force_lle,
            executable=native_path,
            max_insns=args.max_insns,
            max_nodes=args.max_nodes)
    except (OSError, RuntimeError, ValueError) as exc:
        parser.error(f"native analysis failed: {exc}")
    if native_output:
        print(native_output)
    result = emit_program(
        rom=rom,
        parsed=parsed,
        manifest=manifest,
        dispatch_helpers=helpers,
        inline_arg_map=inline_args,
        out_dir=out_dir,
        manifest_text=manifest.to_json(),
        generator_digest=generator_digest,
        config_digest=config_digest,
        analysis_input_digest=analysis_input_digest,
        callee_exit_mx={
            (key.pc24, key.m, key.x): pair
            for key, pair in manifest.exit_modes.items()
        },
        callee_exit_mx_modes={
            (key.pc24, key.m, key.x): frozenset(mode_set)
            for key, mode_set in manifest.exit_mode_sets.items()
        },
        enable_hle=not args.no_hle,
        shard_threshold_bytes=shard_threshold_bytes,
        shard_pc_span=shard_pc_span,
        check_link_closure=not args.no_link_closure_check,
        module_id=args.module_id,
        module_prefix=args.module_prefix,
        rom_sha256_hex=rom_sha256_hex,
        rom_size=rom_image_size,
    )
    elapsed = time.perf_counter() - started
    print(
        f"v2_emit: {len(manifest.roots)} roots, {result.emitted_variants} "
        f"exact AOT variants, {result.lle_variants} LLE variants")
    print(
        f"v2_emit: {result.emitted_banks} bank(s) emitted, "
        f"{result.reused_banks} reused in {elapsed:.2f}s")
    print(f"v2_emit: atomically published {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
