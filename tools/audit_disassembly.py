"""Generate a fresh program and audit the actual emitter decode against authority.

This observes the decoder used by emit_function, after native analysis has
selected exact variants and exit-mode facts. It does not run the retired Python
whole-program analyzer. Unknown addresses and HLE bodies are reported separately;
neither is evidence of instruction conformance or execution correctness.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "recompiler"), str(ROOT / "tools")]
from snes65816 import detect_rom_mapping, lorom_offset, set_rom_mapping


class Authority:
    def __init__(self, rom: bytes, value: dict):
        if value.get("schema") != "snesrecomp disassembly authority v1":
            raise ValueError("unsupported authority schema")
        if value["rom_sha256"] != hashlib.sha256(rom).hexdigest():
            raise ValueError("authority ROM identity mismatch")
        set_rom_mapping(detect_rom_mapping(rom))
        self.rom = rom
        self.starts = {}
        self.owners = {}
        self.data = set()
        for item in value["instructions"]:
            pc = int(item["pc24"], 0) if isinstance(item["pc24"], str) else item["pc24"]
            raw = bytes.fromhex(item["bytes"])
            offset = self.offset(pc)
            if not 1 <= len(raw) <= 4 or (pc & 0xFFFF) + len(raw) > 0x10000:
                raise ValueError(f"invalid authority span at {pc:06X}")
            if offset is None or rom[offset:offset+len(raw)] != raw:
                raise ValueError(f"authority bytes mismatch at {pc:06X}")
            if offset in self.starts and self.starts[offset][0] == raw:
                continue  # physical ROM mirror, not a second instruction
            for n in range(len(raw)):
                if offset+n in self.owners:
                    raise ValueError(f"overlapping authority at {pc+n:06X}")
                self.owners[offset+n] = offset
            self.starts[offset] = (raw, pc, item.get("source"))
        for item in value.get("data_regions", []):
            start, end = (int(item[k], 0) if isinstance(item[k], str) else item[k]
                          for k in ("start_pc24", "end_pc24"))
            if end <= start or start >> 16 != (end-1) >> 16:
                raise ValueError("invalid data region")
            off = self.offset(start)
            if off is None or off+end-start > len(rom):
                raise ValueError("unmapped data region")
            self.data.update(range(off, off+end-start))
        if self.data.intersection(self.owners):
            raise ValueError("authority code/data overlap")

    def offset(self, pc):
        # ROM equivalence is for auditing bytes only. Execution keeps raw PB.
        if pc >> 16 in (0x7E, 0x7F):
            return None
        try:
            off = lorom_offset(pc >> 16, pc & 0xFFFF)
        except (AssertionError, ValueError):
            return None
        return off if 0 <= off < len(self.rom) else None

    def classify(self, pc, length):
        off = self.offset(pc)
        if off in self.data:
            return "data", None
        owner = self.owners.get(off)
        if owner is None:
            return "unknown", None
        raw, start, source = self.starts[owner]
        detail = {"authority_pc24": f"0x{start:06X}",
                  "authority_length": len(raw), "source": source}
        if owner != off:
            return "mid_instruction", detail
        if len(raw) != length:
            return "length_mismatch", detail
        return "match", detail


class Audit:
    def __init__(self, authority):
        self.authority = authority
        self.variants = {}
        self.covered = set()

    def observe(self, bank, start, m, x, graph):
        key = f"{bank:02X}{start:04X}:M{m}X{x}"
        counts = Counter()
        examples = {}
        for di in sorted(graph.insns.values(), key=lambda di: (di.key.pc, di.key.m, di.key.x)):
            kind, detail = self.authority.classify(di.key.pc, di.insn.length)
            counts[kind] += 1
            if kind == "match":
                self.covered.add(self.authority.offset(di.key.pc))
            elif kind not in examples:
                examples[kind] = dict(detail or {}, pc24=f"0x{di.key.pc:06X}",
                                      m=di.key.m, x=di.key.x,
                                      mnemonic=di.insn.mnem, length=di.insn.length)
        self.variants[key] = {"counts": dict(counts), "first_examples_by_class": examples}

    def report(self):
        counts = Counter()
        classes = Counter()
        for value in self.variants.values():
            counts.update(value["counts"])
            classes.update(value["first_examples_by_class"].keys())
        bad = {"data", "mid_instruction", "length_mismatch"}
        invalid = sum(bool(bad.intersection(v["counts"])) for v in self.variants.values())
        unknown = sum("unknown" in v["counts"] for v in self.variants.values())
        return {"schema": "snesrecomp disassembly audit v1",
                "scope": "emitter decode conformance; not execution equivalence",
                "observed_variants": len(self.variants), "invalid_variants": invalid,
                "variants_with_unknown_instructions": unknown,
                "instruction_instances_by_class": dict(counts),
                "variants_by_failure_class": dict(classes),
                "authority_instructions": len(self.authority.starts),
                "covered_authority_instructions": len(self.covered),
                "variants": self.variants}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("rom", "authority", "cfg-dir", "out-dir", "report"):
        ap.add_argument("--"+name, required=True, type=Path)
    ap.add_argument("--source-root", type=Path)
    ap.add_argument("--disassembly-entry-modes", action="store_true")
    args = ap.parse_args()
    if args.out_dir.exists():
        ap.error("out-dir must not exist: every emitted body must be observed, never cached")
    authority_bytes = args.authority.read_bytes()
    authority = Authority(args.rom.read_bytes(), json.loads(authority_bytes))
    audit = Audit(authority)
    emitter = importlib.import_module("v2.emit_function")
    original = emitter.decode_function

    def observe(rom, bank, start, entry_m, entry_x, **kwargs):
        graph = original(rom, bank, start, entry_m, entry_x, **kwargs)
        audit.observe(bank, start, entry_m, entry_x, graph)
        return graph

    emitter.decode_function = observe
    argv = sys.argv
    sys.argv = ["v2_emit", "--rom", str(args.rom), "--cfg-dir", str(args.cfg_dir),
                "--out-dir", str(args.out_dir), "--cfg-roots"]
    if args.source_root:
        sys.argv += ["--source-root", str(args.source_root)]
    if args.disassembly_entry_modes:
        sys.argv += ["--disassembly-entry-modes"]
    try:
        import v2_emit
        result = v2_emit.main()
    finally:
        sys.argv = argv
        emitter.decode_function = original
    if result:
        raise RuntimeError(f"generation failed: {result}")
    report = audit.report()
    report["rom_sha256"] = hashlib.sha256(args.rom.read_bytes()).hexdigest()
    manifest = args.out_dir / "program_manifest.json"
    report["program_manifest_sha256"] = hashlib.sha256(manifest.read_bytes()).hexdigest()
    report["authority_sha256"] = hashlib.sha256(authority_bytes).hexdigest()
    if args.authority.read_bytes() != authority_bytes:
        raise ValueError("authority changed during audit; refusing mixed evidence")
    report["unobserved_manifest_variants"] = sorted(
        k for k, v in json.loads(manifest.read_text())["nodes"].items()
        if v["disposition"] in ("aot_eligible", "hle_overlay") and k not in audit.variants)
    if not audit.variants:
        raise ValueError("no emitted decode observed; refusing an empty audit")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({k:v for k,v in report.items() if k != "variants"}, indent=2))
    return 1 if report["invalid_variants"] else 2 if report["variants_with_unknown_instructions"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
