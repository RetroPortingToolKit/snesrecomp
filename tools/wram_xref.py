#!/usr/bin/env python3
"""Which WRAM addresses a game's compiled routines name, and from where.

The WRAM counterpart of naming functions: walk every variant in a program
manifest with the v2 decoder, at the entry width the manifest lists, and
record each instruction whose operand names WRAM -- which routine, which
site, read or write. Diagnostic only; nothing is emitted.

    python tools/wram_xref.py --rom game.sfc --manifest out/program_manifest.json
    python tools/wram_xref.py ... --addr 7e1d08 1d08      # every site of these
    python tools/wram_xref.py ... --json xref.json        # machine-readable
    python tools/wram_xref.py ... --dp --assume-d 0000    # direct page too

An operand does not always name one address, so every reference carries its
kind, and the kind says what was assumed:

    exact     a long operand into `$7E`/`$7F`, or into a low-WRAM mirror bank
              (`$00`-`$3F`, `$80`-`$BF`) below `$2000`. Exact whatever DB and
              D hold.
    db        an absolute operand below `$2000`. It is `$7E:xxxx` while DB is
              a mirror bank or `$7E`, `$7F:xxxx` while DB is `$7F`, and not
              WRAM at all in a cartridge bank that does not mirror it
              (`$40`-`$7D`, `$C0`-`$FF`). The walk does not track DB, so it
              reports the mirror reading and says so.
    base      an indexed operand (`abs,X`, `abs,Y`, `long,X`): the address the
              index starts from, not the one accessed. `exact_base` is set when
              the base itself is exact (a long operand).
    dp        a direct-page operand, only with `--dp`: D plus the operand,
              under the D given by `--assume-d` (default `$0000`), which the
              reference records. `dp_pointer` marks the pointer of an indirect
              form -- `(dp),Y`, `[dp]` and the like -- which is read, while the
              access itself goes wherever the pointer points.

Absolute operands from `$2000` up are not reported: below `$8000` they are
I/O, and above it ROM unless DB is `$7E`/`$7F`, which the walk does not
track. Stack-relative operands, block moves and control transfers are left
out.

A site decoded the same way in more than one variant is one reference,
listing the variants that reach it. The walk follows the manifest's proven
callee exits (`exit_modes`, `exit_mode_sets`); it does not re-apply a game's
cfg dispatch tables or inline-argument maps, so a path the emitter followed
through one of those may end early here. `--cfg-dir` adds routine names and
data regions.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import pathlib
import sys
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

REPO = pathlib.Path(
    os.environ.get("SNESRECOMP_ROOT", pathlib.Path(__file__).resolve().parent.parent)
)
sys.path.insert(0, str(REPO / "recompiler"))

from snes65816 import (  # noqa: E402
    ABS, ABS_X, ABS_Y, DP, DP_INDIR, DP_X, DP_Y, INDIR_DPX, INDIR_L,
    INDIR_LY, INDIR_Y, LONG, LONG_X, MODE_STR, load_rom,
)
from v2.decoder import decode_function  # noqa: E402
from v2.program_analysis import ProgramManifest, VariantKey  # noqa: E402

READS_NOTHING = frozenset({
    "JMP", "JML", "JSR", "JSL", "PEA", "PER", "MVN", "MVP", "BRK", "COP",
})
WRITES = frozenset({"STA", "STX", "STY", "STZ"})
MODIFIES = frozenset({"INC", "DEC", "ASL", "LSR", "ROL", "ROR", "TSB", "TRB"})
DP_DIRECT = frozenset({DP, DP_X, DP_Y})
DP_POINTER = frozenset({DP_INDIR, INDIR_Y, INDIR_L, INDIR_LY, INDIR_DPX})
ABS_INDEXED = frozenset({ABS_X, ABS_Y})

KINDS = ("exact", "db", "base", "dp", "dp_pointer")


@dataclass
class Reference:
    """One instruction site naming one WRAM address."""
    address24: int
    kind: str                 # one of KINDS
    access: str               # "R", "W" or "RMW"
    site_pc24: int
    mnemonic: str
    mode: str
    exact_base: bool = False  # kind "base": the base itself is exact
    assumed_d: Optional[int] = None   # kind "dp"/"dp_pointer"
    variants: List[str] = field(default_factory=list)

    def qualification(self) -> str:
        if self.kind == "exact":
            return "exact"
        if self.kind == "db":
            return "DB-relative: $7E while DB mirrors low WRAM, $7F under DB=$7F"
        if self.kind == "base":
            return ("indexed base, exact" if self.exact_base
                    else "indexed base, DB-relative")
        d = f"${self.assumed_d:04X}" if self.assumed_d is not None else "?"
        what = "direct page" if self.kind == "dp" else "pointer in direct page"
        return f"{what}, assuming D={d}"

    def to_dict(self) -> dict:
        return {
            "address24": f"{self.address24:06X}",
            "kind": self.kind,
            "qualification": self.qualification(),
            "access": self.access,
            "site_pc24": f"{self.site_pc24:06X}",
            "mnemonic": self.mnemonic,
            "mode": self.mode,
            "exact_base": self.exact_base,
            "assumed_d": (f"{self.assumed_d:04X}"
                          if self.assumed_d is not None else None),
            "variants": sorted(self.variants),
        }


def access_of(mnem: str) -> str:
    return "W" if mnem in WRITES else "RMW" if mnem in MODIFIES else "R"


def is_low_wram_mirror_bank(bank: int) -> bool:
    return (bank & 0x7F) < 0x40


def classify(insn, *, want_dp: bool, assume_d: int) -> Optional[Tuple[int, str, bool, Optional[int]]]:
    """(address24, kind, exact_base, assumed_d) for one decoded instruction,
    or None when its operand does not name WRAM."""
    if insn.mnem in READS_NOTHING:
        return None
    mode, operand = insn.mode, insn.operand
    if mode in (LONG, LONG_X):
        bank, low = (operand >> 16) & 0xFF, operand & 0xFFFF
        if bank in (0x7E, 0x7F):
            address = operand
        elif is_low_wram_mirror_bank(bank) and low < 0x2000:
            address = 0x7E0000 | low
        else:
            return None
        if mode == LONG_X:
            return address, "base", True, None
        return address, "exact", False, None
    if mode == ABS or mode in ABS_INDEXED:
        if operand >= 0x2000:
            return None
        if mode in ABS_INDEXED:
            return 0x7E0000 | operand, "base", False, None
        return 0x7E0000 | operand, "db", False, None
    if want_dp and (mode in DP_DIRECT or mode in DP_POINTER or insn.mnem == "PEI"):
        address = 0x7E0000 | ((assume_d + operand) & 0xFFFF)
        kind = "dp_pointer" if mode in DP_POINTER and insn.mnem != "PEI" else "dp"
        return address, kind, False, assume_d
    return None


def callee_exits(manifest: ProgramManifest):
    """The manifest's proven exits in the decoder's callee_exit_mx shape."""
    exact = {(k.pc24, k.m, k.x): tuple(v) for k, v in manifest.exit_modes.items()}
    sets = {(k.pc24, k.m, k.x): frozenset(tuple(e) for e in v)
            for k, v in manifest.exit_mode_sets.items()}
    return exact, sets


def collect(rom: bytes, manifest: ProgramManifest, *, want_dp: bool = False,
            assume_d: int = 0,
            data_regions: Optional[List[Tuple[int, int, int]]] = None,
            max_insns: int = 12000) -> List[Reference]:
    """Every WRAM reference of every manifest variant, one per distinct site."""
    exact, sets = callee_exits(manifest)
    merged: Dict[tuple, Reference] = {}
    for key in sorted(manifest.nodes):
        bank, start = key.pc24 >> 16, key.pc24 & 0xFFFF
        graph = decode_function(
            rom, bank, start, key.m, key.x, max_insns=max_insns,
            data_regions=data_regions, callee_exit_mx=exact,
            callee_exit_mx_modes=sets)
        name = key.manifest_key
        for decoded in graph.insns.values():
            insn = decoded.insn
            found = classify(insn, want_dp=want_dp, assume_d=assume_d)
            if found is None:
                continue
            address, kind, exact_base, d = found
            access = access_of(insn.mnem)
            site = decoded.key.pc
            ident = (site, address, kind, access, insn.mnem, insn.mode)
            ref = merged.get(ident)
            if ref is None:
                ref = merged[ident] = Reference(
                    address24=address, kind=kind, access=access,
                    site_pc24=site, mnemonic=insn.mnem,
                    mode=MODE_STR.get(insn.mode, str(insn.mode)),
                    exact_base=exact_base, assumed_d=d)
            if name not in ref.variants:
                ref.variants.append(name)
    return sorted(merged.values(),
                  key=lambda r: (r.address24, r.site_pc24, r.kind, r.access))


def owner_of(site: int, starts: Sequence[Tuple[int, str]]) -> Optional[str]:
    """The nearest named routine at or below a site, from --cfg-dir names."""
    lo, hi, best = 0, len(starts), None
    while lo < hi:
        mid = (lo + hi) // 2
        if starts[mid][0] <= site:
            best = starts[mid][1]
            lo = mid + 1
        else:
            hi = mid
    return best


def load_names(cfg_dir: pathlib.Path):
    """(sorted [(pc24, name)], data_regions) from a game's bank cfgs."""
    from v2.cfg_loader import load_bank_cfg
    starts, regions = [], []
    for path in sorted(cfg_dir.glob("bank*.cfg")):
        cfg = load_bank_cfg(str(path))
        for entry in cfg.entries:
            if entry.name:
                starts.append(((cfg.bank << 16) | (entry.start & 0xFFFF), entry.name))
        for decl in list(cfg.names) + list(cfg.symbols):
            starts.append((decl.addr_24, decl.name))
        regions.extend(cfg.data_regions)
    return sorted(set(starts)), regions


def by_address(refs: Iterable[Reference]) -> "collections.OrderedDict[tuple, List[Reference]]":
    """References grouped per (address, kind), in address order."""
    groups: Dict[tuple, List[Reference]] = collections.OrderedDict()
    for ref in sorted(refs, key=lambda r: (r.address24, KINDS.index(r.kind))):
        groups.setdefault((ref.address24, ref.kind), []).append(ref)
    return groups


def parse_address(text: str) -> int:
    value = int(text.replace("$", "").replace(":", ""), 16)
    return value if value > 0xFFFF else 0x7E0000 | value


def wram(address24: int) -> str:
    return f"${address24 >> 16:02X}:{address24 & 0xFFFF:04X}"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rom", required=True)
    ap.add_argument("--manifest", required=True,
                    help="program_manifest.json from v2_analyze / v2_regen")
    ap.add_argument("--cfg-dir", help="bank cfgs, for names and data regions")
    ap.add_argument("--addr", nargs="+", default=[],
                    help="list every site of these addresses (7e1d08, 1d08, $7F:0040)")
    ap.add_argument("--dp", action="store_true",
                    help="report direct-page operands too, under --assume-d")
    ap.add_argument("--assume-d", default="0000",
                    help="the D register value direct-page readings assume (hex)")
    ap.add_argument("--kinds", default=",".join(KINDS),
                    help="which kinds to report, comma-separated")
    ap.add_argument("--json", help="write the references as JSON here ('-' for stdout)")
    args = ap.parse_args(argv)

    rom = load_rom(args.rom)
    manifest = ProgramManifest.from_dict(json.loads(pathlib.Path(args.manifest).read_text()))
    starts, regions = load_names(pathlib.Path(args.cfg_dir)) if args.cfg_dir else ([], None)
    refs = collect(rom, manifest, want_dp=args.dp,
                   assume_d=int(args.assume_d, 16), data_regions=regions)
    kinds = {k.strip() for k in args.kinds.split(",") if k.strip()}
    refs = [r for r in refs if r.kind in kinds]
    if args.addr:
        wanted = {parse_address(a) for a in args.addr}
        refs = [r for r in refs if r.address24 in wanted]

    if args.json:
        payload = {
            "format_version": 1,
            "rom": os.path.basename(args.rom),
            "manifest": os.path.basename(args.manifest),
            "variants": len(manifest.nodes),
            "assumptions": {"d": f"{int(args.assume_d, 16):04X}" if args.dp else None,
                            "db": "abs operands below $2000 read as the low-WRAM mirror"},
            "references": [dict(r.to_dict(),
                                routine=owner_of(r.site_pc24, starts) if starts else None)
                           for r in refs],
        }
        text = json.dumps(payload, indent=2) + "\n"
        if args.json == "-":
            sys.stdout.write(text)
        else:
            pathlib.Path(args.json).write_text(text)
        return 0

    groups = by_address(refs)
    print(f"wram_xref: {len(groups)} address/kind pairs from "
          f"{len(manifest.nodes)} variants, {len(refs)} sites")
    for (address, kind), group in groups.items():
        rw = "".join(sorted({c for r in group for c in r.access}))
        print(f"{wram(address)}  {kind:<10} {rw:<3} {len(group):3} site(s)  "
              f"{group[0].qualification()}")
        if args.addr:
            for r in group:
                who = owner_of(r.site_pc24, starts) if starts else None
                print(f"    {r.site_pc24 >> 16:02X}:{r.site_pc24 & 0xFFFF:04X}  "
                      f"{r.mnemonic} {r.mode:<8} {r.access:<3} "
                      f"{who or ''}  [{', '.join(sorted(r.variants))}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
