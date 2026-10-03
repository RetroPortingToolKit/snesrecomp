#!/usr/bin/env python3
"""Audit cfg `func` boundaries against the ROM: which declared ranges run long?

A cfg entry `func <name> <start> end:<end>` asserts that [start, end) is one
function. Ingesters commonly derive `end:` from the NEXT known symbol, which is
not the same claim: everything between a function and the next symbol -- data
tables, inline call arguments, padding -- gets swallowed into the function
above it and translated as code. That is not a cosmetic error. Two shipped bugs
in SuperMetroidRecomp came from it (DEVELOPMENT.md, 2026-09-19): misaligned
bytes inside a swallowed HDMA table decoded as `BNE` into a phantom block that
corrupted a Mode 7 register, and a 31 KB message-box data region translated as
code.

This decodes each declared range from its entry point at its declared entry
width, follows local control flow, and reports ranges with a large unreachable
tail that the cfg has not already declared as data (`exclude_range`,
`data_region`). ROM and cfg are the only inputs -- no decomp listing, no
per-game data -- so it runs for any port. The cfg is read by the recompiler's
own loader (recompiler/v2/cfg_loader.py), so `end:` / `entry_mx:` anywhere on
the line, `end_at` / `entry_mx_at` overrides and comments mean exactly what
they mean to the recompiler, and the ROM mapping (LoROM, HiROM, SA-1, S-DD1)
is the one the recompiler detects.

A finding is a CANDIDATE, not a verdict: a function reached only through an
indirect dispatch, or one whose tail is a jump table the decoder handles
elsewhere, will show up here and be fine. Triage is a human's job; nothing is
auto-applied.

Usage:
  python cfg_boundary_audit.py <rom> <cfg-dir> [--min-tail 0x40] [--top N]
"""
import argparse
import pathlib
import sys
from dataclasses import dataclass
from typing import Iterable, List, Set, Tuple

_REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO / "recompiler"))
import snes65816 as s  # noqa: E402
from v2.cfg_loader import load_bank_cfg  # noqa: E402

TERMINAL = {"RTS", "RTL", "RTI", "STP"}
COND_BRANCH = {"BCC", "BCS", "BEQ", "BNE", "BMI", "BPL", "BVC", "BVS"}
UNCOND_LOCAL = {"BRA", "BRL"}
DIRECT_JMP_ABS = 0x4C   # JMP abs    -- target is the operand, same bank
DIRECT_JML = 0x5C       # JML long   -- target is the operand
# JMP (abs), JMP (abs,X) and JML [abs] name a POINTER, not a target: the
# walk stops there, as it does at any transfer it cannot resolve statically.


@dataclass(frozen=True)
class Finding:
    undeclared: int     # unreachable tail bytes not declared as data
    tail: int           # unreachable tail bytes in total
    bank: int
    name: str
    start: int
    end: int
    reached: int        # bytes reachable from the entry inside the range
    exhausted: bool     # the walk hit its step budget (coverage incomplete)


def reachable(rom: bytes, bank: int, start: int, end: int,
              entry_m: int = 1, entry_x: int = 1,
              budget: int = 20000) -> Tuple[Set[int], bool]:
    """Bytes reachable from `start` without leaving [start, end).

    Follows fall-through, conditional branches (both ways), BRA/BRL and
    direct same-bank JMP/JML, tracking REP/SEP so immediates decode at the
    right width; stops at RTS/RTL/RTI/STP, at an indirect jump, and at
    anything leaving the range or the ROM. Returns the covered addresses and
    whether the step budget ran out before the walk finished.
    """
    seen: Set[int] = set()
    work = [(start, entry_m, entry_x)]
    steps = 0
    while work:
        pc, m, x = work.pop()
        while True:
            if steps >= budget:
                return seen, True
            steps += 1
            if not (start <= pc < end) or pc in seen:
                break
            if not s.is_rom_address(bank, pc):
                break
            off = s.rom_offset(bank, pc)
            if off >= len(rom):
                break
            try:
                ins = s.decode_insn(rom, off, pc, bank, m, x)
            except IndexError:
                break
            if ins is None:
                break
            for i in range(ins.length):
                seen.add(pc + i)
            mnem = ins.mnem.upper()
            if mnem == "REP":
                if ins.operand & 0x20:
                    m = 0
                if ins.operand & 0x10:
                    x = 0
            elif mnem == "SEP":
                if ins.operand & 0x20:
                    m = 1
                if ins.operand & 0x10:
                    x = 1
            if mnem in TERMINAL:
                break
            if mnem in COND_BRANCH:
                tgt = ins.operand & 0xFFFF
                if start <= tgt < end:
                    work.append((tgt, m, x))
                pc += ins.length
                continue
            if mnem in UNCOND_LOCAL:
                tgt = ins.operand & 0xFFFF
                if start <= tgt < end:
                    pc = tgt
                    continue
                break
            if ins.opcode == DIRECT_JMP_ABS or ins.opcode == DIRECT_JML:
                tgt = ins.operand & 0xFFFF
                tbank = (ins.operand >> 16) & 0xFF
                same_bank = (ins.opcode == DIRECT_JMP_ABS or tbank == bank
                             or s.rom_bank_mirror(tbank) == bank)
                if same_bank and start <= tgt < end:
                    pc = tgt
                    continue
                break
            if mnem in ("JMP", "JML"):
                break
            pc += ins.length
    return seen, False


def _declared_data(cfg) -> List[Tuple[int, int]]:
    """This bank's declared data ranges, [lo, hi) local addresses."""
    spans = [(lo & 0xFFFF, hi) for lo, hi in cfg.exclude_ranges]
    spans += [(lo & 0xFFFF, hi) for b, lo, hi in cfg.data_regions
              if b == cfg.bank]
    return spans


def _covered(lo: int, hi: int, spans: Iterable[Tuple[int, int]]) -> int:
    """How many bytes of [lo, hi) the spans cover (overlaps counted once)."""
    marks = bytearray(max(0, hi - lo))
    for a, b in spans:
        a, b = max(a, lo), min(b, hi)
        for i in range(a, b):
            marks[i - lo] = 1
    return sum(marks)


def audit(rom: bytes, cfg_paths: Iterable[pathlib.Path],
          min_tail: int) -> Tuple[int, List[Finding]]:
    findings: List[Finding] = []
    total = 0
    for path in cfg_paths:
        cfg = load_bank_cfg(str(path))
        if cfg.bank < 0:
            continue
        data = _declared_data(cfg)
        for e in cfg.entries:
            if e.end is None:
                continue
            start = e.start & 0xFFFF
            end = min(e.end, 0x10000)
            if end <= start or not s.is_rom_address(cfg.bank, start):
                continue
            total += 1
            cov, exhausted = reachable(rom, cfg.bank, start, end,
                                       e.entry_m, e.entry_x)
            if not cov:
                continue
            tail_lo = max(cov) + 1
            tail = end - tail_lo
            undeclared = tail - _covered(tail_lo, end, data)
            if undeclared >= min_tail:
                findings.append(Finding(undeclared, tail, cfg.bank, e.name,
                                        start, end, len(cov), exhausted))
    findings.sort(key=lambda f: (-f.undeclared, f.bank, f.start))
    return total, findings


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("cfg_dir")
    ap.add_argument("--min-tail", default="0x40",
                    help="report ranges whose undeclared unreachable tail is "
                         "at least this many bytes (default 0x40)")
    ap.add_argument("--top", type=int, default=0,
                    help="print only the N worst (0 = all)")
    args = ap.parse_args(argv)
    min_tail = int(args.min_tail, 0)
    rom = s.load_rom(args.rom)
    total, findings = audit(rom, sorted(pathlib.Path(args.cfg_dir).glob("bank*.cfg")),
                            min_tail)
    shown = findings[:args.top] if args.top else findings
    print(f"{total} func declarations checked; "
          f"{len(findings)} with an undeclared unreachable tail >= {min_tail:#x}\n")
    print(f"{'tail':>7}  {'bank':>4}  {'declared':>13}  {'reached':>7}  name")
    for f in shown:
        note = "  (walk budget exhausted)" if f.exhausted else ""
        print(f"{f.undeclared:#7x}  ${f.bank:02X}  ${f.start:04X}-${f.end:04X}  "
              f"{f.reached:7d}  {f.name}{note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
