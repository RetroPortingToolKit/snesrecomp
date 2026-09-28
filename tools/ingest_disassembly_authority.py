#!/usr/bin/env python3
"""Validate a byte-matched disassembly and produce an isolated cfg overlay.

Input schema: snesrecomp disassembly authority v1. Requires rom_sha256 and
instructions [{pc24, bytes}], with optional entries [{pc24, m, x}],
data_regions [{start_pc24, end_pc24}] and dispatches
[{site_pc24, kind: ptrtail|ptrcall, targets: [pc24,...]}]. Addresses use 0x.
Generate with v2_emit --cfg-dir <output> --cfg-roots. Evidence constrains
decoding; it never bypasses structural analysis or runtime pointer checks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "recompiler"))
from snes65816 import lorom_offset, detect_rom_mapping, set_rom_mapping
from v2.coverage_profile import pc
from v2.cfg_loader import load_bank_cfg


def ingest(rom_path, authority_path, cfg_dir, out_dir):
    rom = Path(rom_path).read_bytes()
    authority = json.loads(Path(authority_path).read_text(encoding="utf-8"))
    if authority.get("schema") != "snesrecomp disassembly authority v1":
        raise ValueError("unsupported disassembly authority schema")
    if authority.get("rom_sha256") != hashlib.sha256(rom).hexdigest():
        raise ValueError("disassembly ROM identity mismatch")
    set_rom_mapping(detect_rom_mapping(rom))
    lines, occupied, starts = {}, {}, set()
    def add(address, line):
        lines.setdefault(address >> 16, []).append(line)
    for insn in authority.get("instructions", []):
        address, raw = pc(insn["pc24"]), bytes.fromhex(insn["bytes"])
        if not 1 <= len(raw) <= 4 or (address & 0xFFFF) + len(raw) > 0x10000:
            raise ValueError(f"invalid instruction span at {address:06X}")
        offset = lorom_offset(address >> 16, address & 0xFFFF)
        if rom[offset:offset + len(raw)] != raw:
            raise ValueError(f"disassembly bytes differ at {address:06X}")
        for n in range(len(raw)):
            if address + n in occupied:
                raise ValueError(f"overlapping instruction spans at {address+n:06X}")
            occupied[address+n] = address
        starts.add(address)
        add(address, f"authority_insn {address & 0xFFFF:04X} {raw.hex()}")
    cfg_dir, out_dir = Path(cfg_dir).resolve(), Path(out_dir).resolve()
    if out_dir == cfg_dir or cfg_dir in out_dir.parents or out_dir.exists():
        raise ValueError("output must be a new directory outside the source cfg directory")
    known = {}
    for path in cfg_dir.glob("bank*.cfg"):
        cfg = load_bank_cfg(str(path))
        known[cfg.bank] = {e.start for e in cfg.entries}
    variants = set()
    for entry in authority.get("entries", []):
        address, m, x = pc(entry["pc24"]), entry["m"], entry["x"]
        if address not in starts or m not in (0, 1) or x not in (0, 1):
            raise ValueError("entry requires an authoritative instruction and exact M/X")
        if address in variants:
            raise ValueError("multiple entry modes require separate authority inputs")
        variants.add(address)
        if address & 0xFFFF not in known.get(address >> 16, set()):
            add(address, f"func authority_{address:06X} {address & 0xFFFF:04X} entry_mx:{m},{x}")
        add(address, f"entry_mx_at {address & 0xFFFF:04X} {m} {x}")
    for region in authority.get("data_regions", []):
        start, end = pc(region["start_pc24"]), pc(region["end_pc24"])
        if end <= start or start >> 16 != (end - 1) >> 16 or any(start <= a < end for a in occupied):
            raise ValueError("data region overlaps code or crosses a bank")
        add(start, f"data_region {start >> 16:02X} {start & 0xFFFF:04X} {end-start+(start & 0xFFFF):04X}")
    for dispatch in authority.get("dispatches", []):
        site, kind = pc(dispatch["site_pc24"]), dispatch["kind"]
        targets = sorted({pc(v) for v in dispatch["targets"]})
        if site not in starts or kind not in ("ptrtail", "ptrcall") or not targets or any(t not in starts for t in targets):
            raise ValueError("dispatch requires authoritative site and target instructions")
        add(site, f"indirect_dispatch {site & 0xFFFF:04X} {len(targets)} {kind} targets:" + ",".join(f"{t:06X}" for t in targets))
    # Validation is complete before making the reviewable overlay.
    shutil.copytree(cfg_dir, out_dir)
    for bank, directives in sorted(lines.items()):
        path = out_dir / f"bank{bank:02x}.cfg"
        existing = next((p for p in out_dir.glob("bank*.cfg") if p.stem.lower() == path.stem), None)
        path = existing or path
        prefix = "" if path.exists() else f"bank = {bank:02X}\n"
        with path.open("a", encoding="utf-8") as f:
            f.write("\n" + prefix + "# Byte-verified disassembly authority\n" + "\n".join(directives) + "\n")
    return {"rom_sha256": authority["rom_sha256"], "instructions": len(starts), "entries": len(variants), "output": str(out_dir)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("rom", "authority", "cfg-dir", "out-dir"):
        ap.add_argument("--" + name, required=True, type=Path)
    args = ap.parse_args()
    try:
        print(json.dumps(ingest(args.rom, args.authority, args.cfg_dir, args.out_dir), indent=2))
    except (ValueError, KeyError, OSError, AssertionError) as exc:
        ap.error(str(exc))


if __name__ == "__main__":
    main()
