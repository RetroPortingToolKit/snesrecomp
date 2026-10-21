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
from bisect import bisect_left
import hashlib
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "recompiler"))
from snes65816 import rom_offset, detect_rom_mapping, set_rom_mapping, rom_bank_aliases, is_rom_address
from v2.coverage_profile import pc
from v2.cfg_loader import load_bank_cfg


def ingest(rom_path, authority_path, cfg_dir, out_dir):
    rom = Path(rom_path).read_bytes()
    authority = json.loads(Path(authority_path).read_text(encoding="utf-8"))
    if authority.get("schema") != "snesrecomp disassembly authority v1":
        raise ValueError("unsupported disassembly authority schema")
    if authority.get("rom_sha256") != hashlib.sha256(rom).hexdigest():
        raise ValueError("disassembly ROM identity mismatch")
    mapping = detect_rom_mapping(rom)
    set_rom_mapping(mapping)
    cfg_dir, out_dir = Path(cfg_dir).resolve(), Path(out_dir).resolve()
    if out_dir == cfg_dir or cfg_dir in out_dir.parents or out_dir.exists():
        raise ValueError("output must be a new directory outside the source cfg directory")
    known = {}
    for path in cfg_dir.glob("bank*.cfg"):
        cfg = load_bank_cfg(str(path))
        known[cfg.bank] = {e.start for e in cfg.entries}

    def owner_bank(address):
        bank = address >> 16
        # Keep evidence beside the existing execution contracts. Creating an
        # otherwise empty mirror cfg would shadow its HLE hooks/boundaries.
        if bank not in known:
            for alias in rom_bank_aliases(bank, address & 0xFFFF):
                if alias in known:
                    return alias
        return bank

    lines, occupied, starts = {}, {}, {}
    def add(address, line):
        lines.setdefault(owner_bank(address), []).append(line)
    for insn in authority.get("instructions", []):
        address, raw = pc(insn["pc24"]), bytes.fromhex(insn["bytes"])
        if not 1 <= len(raw) <= 4 or (address & 0xFFFF) + len(raw) > 0x10000:
            raise ValueError(f"invalid instruction span at {address:06X}")
        offset = rom_offset(address >> 16, address & 0xFFFF)
        if rom[offset:offset + len(raw)] != raw:
            raise ValueError(f"disassembly bytes differ at {address:06X}")
        if offset in starts and starts[offset] == raw:
            continue  # identical physical evidence, including another CPU alias
        for n in range(len(raw)):
            if offset + n in occupied:
                raise ValueError(f"overlapping instruction spans at {address+n:06X} (ROM {offset+n:06X})")
            occupied[offset+n] = offset
        starts[offset] = raw
        add(address, f"authority_insn {address & 0xFFFF:04X} {raw.hex()}")
    variants = set()
    for entry in authority.get("entries", []):
        address, m, x = pc(entry["pc24"]), entry["m"], entry["x"]
        if rom_offset(address >> 16, address & 0xFFFF) not in starts or m not in (0, 1) or x not in (0, 1):
            raise ValueError("entry requires an authoritative instruction and exact M/X")
        if address in variants:
            raise ValueError("multiple entry modes require separate authority inputs")
        variants.add(address)
        # HiROM entry widths describe an execution context, even when its
        # instruction bytes are shared. Preserve a local override instead of
        # overwriting another alias's mode through the physical evidence owner.
        entry_bank = address >> 16 if mapping == "hirom" else owner_bank(address)
        entries = lines.setdefault(entry_bank, [])
        if address & 0xFFFF not in known.get(entry_bank, set()):
            entries.append(f"func authority_{address:06X} {address & 0xFFFF:04X} entry_mx:{m},{x}")
        entries.append(f"entry_mx_at {address & 0xFFFF:04X} {m} {x}")
    occupied_addresses = sorted(occupied)
    for region in authority.get("data_regions", []):
        start, end = pc(region["start_pc24"]), pc(region["end_pc24"])
        if (end <= start or start >> 16 != (end - 1) >> 16
                or not is_rom_address(start >> 16, start & 0xFFFF)):
            raise ValueError("data region overlaps code or crosses a bank")
        offset = rom_offset(start >> 16, start & 0xFFFF)
        limit = offset + end - start
        index = bisect_left(occupied_addresses, offset)
        overlaps = index < len(occupied_addresses) and occupied_addresses[index] < limit
        if limit > len(rom) or overlaps:
            raise ValueError("data region overlaps code or crosses a bank")
        add(start, f"data_region {owner_bank(start):02X} {start & 0xFFFF:04X} {end-start+(start & 0xFFFF):04X}")
        add(start, f"authority_data {start & 0xFFFF:04X} {end-start+(start & 0xFFFF):04X}")
    for dispatch in authority.get("dispatches", []):
        site, kind = pc(dispatch["site_pc24"]), dispatch["kind"]
        targets = sorted({pc(v) for v in dispatch["targets"]})
        if (rom_offset(site >> 16, site & 0xFFFF) not in starts
                or kind not in ("ptrtail", "ptrcall") or not targets
                or any(rom_offset(t >> 16, t & 0xFFFF) not in starts for t in targets)):
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
