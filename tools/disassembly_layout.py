"""Ship disassembly boundaries without ROM bytes, bound to one verified image.

A title can install disassembly-layout.json beside its bank configs. Generation
materializes the same byte-authoritative cfg overlay used in validation from
the user's verified ROM. Both launcher and command-line generation use it.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "recompiler"), str(ROOT / "tools")]
from snes65816 import detect_rom_mapping, load_rom, lorom_offset, set_rom_mapping
from ingest_disassembly_authority import ingest


SCHEMA = "snesrecomp disassembly layout v1"
FILENAME = "disassembly-layout.json"


def export_layout(authority):
    if authority.get("schema") != "snesrecomp disassembly authority v1":
        raise ValueError("unsupported authority schema")
    # Explicit allowlist: no instruction bytes, source text, or unknown source
    # spans can accidentally enter a distributable layout.
    return {
        "schema": SCHEMA,
        "rom_sha256": authority["rom_sha256"],
        "provenance": authority.get("provenance", {}),
        "probe_entry_modes": True,
        "instructions": [[item["pc24"], len(bytes.fromhex(item["bytes"]))]
                         for item in authority["instructions"]],
        "entries": authority.get("entries", []),
        "data_regions": authority.get("data_regions", []),
        "dispatches": authority.get("dispatches", []),
    }


def materialize(rom, layout):
    if layout.get("schema") != SCHEMA:
        raise ValueError("unsupported disassembly layout schema")
    if layout.get("rom_sha256") != hashlib.sha256(rom).hexdigest():
        raise ValueError("disassembly layout ROM identity mismatch")
    if type(layout.get("probe_entry_modes")) is not bool:
        raise ValueError("disassembly layout requires boolean probe_entry_modes")
    # Current source adapters were qualified for LoROM only. Do not infer
    # authority aliases for other cartridge mappings.
    if detect_rom_mapping(rom) != "lorom":
        raise ValueError("disassembly layout requires a LoROM image")
    set_rom_mapping("lorom")
    instructions = []
    for address, length in layout["instructions"]:
        address = int(address, 0) if isinstance(address, str) else address
        if (type(address) is not int or not 0 <= address <= 0xFFFFFF
                or type(length) is not int or not 1 <= length <= 4
                or (address & 0xFFFF) + length > 0x10000
                or address >> 16 in (0x7E, 0x7F)
                or address & 0xFFFF < 0x8000):
            raise ValueError("invalid disassembly layout instruction span")
        offset = lorom_offset(address >> 16, address & 0xFFFF)
        if offset < 0 or offset + length > len(rom):
            raise ValueError("disassembly layout instruction is outside the ROM")
        instructions.append({"pc24": address,
                             "bytes": rom[offset:offset + length].hex()})
    if not instructions:
        raise ValueError("disassembly layout has no instructions")
    return {"schema": "snesrecomp disassembly authority v1",
            "rom_sha256": layout["rom_sha256"], "instructions": instructions,
            "entries": layout.get("entries", []),
            "data_regions": layout.get("data_regions", []),
            "dispatches": layout.get("dispatches", [])}


@contextmanager
def configured_authority(rom_path, cfg_dir):
    cfg_dir = Path(cfg_dir).resolve()
    source = cfg_dir / FILENAME
    if not source.exists():
        yield cfg_dir, False
        return
    layout = json.loads(source.read_text(encoding="utf-8"))
    rom = load_rom(str(rom_path))
    authority = materialize(rom, layout)
    with tempfile.TemporaryDirectory(prefix="snesrecomp-authority-") as directory:
        temp = Path(directory)
        # Importer verifies the headerless image as well as every instruction.
        image = temp / "image.sfc"
        image.write_bytes(rom)
        evidence = temp / "authority.json"
        evidence.write_text(json.dumps(authority), encoding="utf-8")
        overlay = temp / "cfg"
        ingest(image, evidence, cfg_dir, overlay)
        yield overlay, layout["probe_entry_modes"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--authority", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    value = export_layout(json.loads(args.authority.read_text(encoding="utf-8")))
    # One span per line keeps the address/length metadata reviewable.
    text = json.dumps(value, indent=2) + "\n"
    text = re.sub(r'\[\s*("0x[0-9A-Fa-f]+"|\d+),\s*(\d+)\s*\]', r'[\1, \2]', text)
    with args.out.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


if __name__ == "__main__":
    main()
