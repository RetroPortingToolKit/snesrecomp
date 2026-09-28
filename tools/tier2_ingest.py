#!/usr/bin/env python3
"""Audit/merge coverage captures; observations do not authorize AOT execution."""
import argparse
import hashlib
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "recompiler"))
from v2.ram_coverage import describe_snapshot
from v2.coverage_profile import load_profiles, promotion_reason, canonical_pc, pc


def audit(profile, declared=(), program=None):
    nodes = (program or {}).get("nodes", {})
    rows = []
    for observed in profile.discoveries:
        row = dict(observed)
        target = canonical_pc(row["target_pc24"], profile.identity.get("mapper"))
        key = f"{target:06X}:{row.get('entry_mx', 'unknown')}"
        node = nodes.get(key) or nodes.get(
            f"{pc(row['target_pc24']):06X}:{row.get('entry_mx', 'unknown')}")
        if node:
            row["analysis_disposition"] = node.get("disposition")
            row["analysis_reasons"] = node.get("reasons", [])
            row["explanation"] = (
                "Analyzed AOT entry: check runtime policy, byte guards, and linked build"
                if node.get("disposition") == "aot_eligible"
                else "Analysis rejected this exact variant")
        else:
            row["explanation"] = "Exact variant absent from supplied analysis" if program else "No program manifest supplied"
        row["candidate_status"] = promotion_reason(row, declared, profile.identity)
        row["variant"] = key
        rows.append(row)
    return {
        "identity": profile.identity,
        "capture_count": len(profile.captures),
        "warnings": sorted(set(profile.warnings)),
        "interpreted_instructions": sum(r["interpreted_instructions"] for r in profile.costs),
        "interpreted_guest_cycles": sum(r["guest_cycles"] for r in profile.costs),
        "processors": {processor: {
            "instructions": sum(r["interpreted_instructions"] for r in profile.costs if r.get("processor", "snes_cpu") == processor),
            "guest_cycles": sum(r["guest_cycles"] for r in profile.costs if r.get("processor", "snes_cpu") == processor),
        } for processor in sorted({r.get("processor", "snes_cpu") for r in profile.costs})},
        "cost_attribution": "exclusive instruction PC, entry M/X and E; never inferred function ownership",
        "discoveries": rows,
        "hot_instructions": sorted(profile.costs, key=lambda r: -r["guest_cycles"]),
        "ram_routines": [describe_snapshot(r) for r in profile.ram_routines],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", nargs="+", help="v2 JSON checkpoints/JSONL journals or legacy v1 JSON")
    parser.add_argument("--cfg-dir", default="recomp")
    parser.add_argument("--program-manifest", type=pathlib.Path)
    parser.add_argument("--rom-sha256")
    parser.add_argument("--legacy-profile-rom-sha256")
    parser.add_argument("--min-hits", type=int, default=1)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    declared = set()
    for path in pathlib.Path(args.cfg_dir).glob("bank*.cfg"):
        bank = re.fullmatch(r"bank([0-9a-fA-F]{2})", path.stem)
        if not bank:
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            match = re.match(r"\s*func\s+\S+\s+([0-9a-fA-F]+)", line)
            if match:
                declared.add((int(bank[1], 16) << 16) | int(match[1], 16))
    try:
        profile = load_profiles(args.manifest, expected_rom=args.rom_sha256,
                                legacy_rom=args.legacy_profile_rom_sha256)
        program = None
        if args.program_manifest:
            raw = args.program_manifest.read_bytes()
            expected = profile.identity.get("program_digest")
            if expected and hashlib.sha256(raw).hexdigest() != expected:
                raise ValueError("program manifest differs from captured build; supply the original generated manifest")
            program = json.loads(raw)
        report = audit(profile, declared, program)
    except (ValueError, OSError, KeyError) as exc:
        parser.error(str(exc))
    if args.json:
        print(json.dumps(report, indent=2))
        return 0
    print(f"Coverage: {report['capture_count']} capture(s), {len(report['discoveries'])} transfer tuples")
    print(f"Interpreted work: {report['interpreted_instructions']:,} instructions, "
          f"{report['interpreted_guest_cycles']:,} guest cycles")
    for warning in report["warnings"]:
        print(f"Warning: {warning}")
    for row in report["discoveries"]:
        hits = sum(row.get(k, 0) for k in ("observed_hits", "completed_hits", "yielded_hits", "bail_hits"))
        if hits < args.min_hits:
            continue
        print(f"{row['variant']} E={row.get('emulation', '?')} "
              f"from 0x{pc(row.get('site_pc24', 0)):06X}: {row['candidate_status']}")
        print(f"  observed={row['observed_hits']} completed={row['completed_hits']} "
              f"yielded={row['yielded_hits']} bailed={row['bail_hits']} pending={row['pending_hits']}")
        print(f"  {row['explanation']}; reasons={','.join(row.get('analysis_reasons', [])) or 'unknown'}")
    print("Hottest interpreted instructions (exclusive guest cycles):")
    for row in report["hot_instructions"][:20]:
        print(f"  {row.get('processor', 'snes_cpu')} 0x{pc(row['target_pc24']):06X} {row['entry_mx']} E={row.get('emulation')} "
              f"{row['guest_cycles']:,} cycles / {row['interpreted_instructions']:,} instructions")
    print("Candidates seed --profile-manifest analysis; qualify generated changes with replay before release.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
