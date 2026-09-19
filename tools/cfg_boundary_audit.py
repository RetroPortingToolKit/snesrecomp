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

This decodes each declared range from its entry point, follows local control
flow, and reports ranges with a large unreachable tail. ROM and cfg are the
only inputs -- no decomp listing, no per-game data -- so it runs for any port.

A finding is a CANDIDATE, not a verdict: a function reached only through an
indirect dispatch, or one whose tail is a jump table the decoder handles
elsewhere, will show up here and be fine. Triage is a human's job; nothing is
auto-applied.

Usage:
  python cfg_boundary_audit.py <rom> <cfg-dir> [--min-tail 0x40] [--top N]
"""
import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "recompiler"))
import snes65816 as s  # noqa: E402

TERMINAL = {"RTS", "RTL", "RTI", "STP"}
COND_BRANCH = {"BCC", "BCS", "BEQ", "BNE", "BMI", "BPL", "BVC", "BVS"}
UNCOND_LOCAL = {"BRA", "BRL"}
FUNC_RE = re.compile(
    r"^\s*func\s+(\S+)\s+([0-9a-fA-F]+)\s+end:([0-9a-fA-F]+)", re.I)
BANK_RE = re.compile(r"^\s*bank\s*=\s*([0-9a-fA-F]+)", re.I)


def reachable(rom, bank, start, end, entry_m=1, entry_x=1, budget=20000):
    """Bytes reachable from `start` without leaving [start, end).

    Follows fall-through, conditional branches (both ways) and local
    unconditional jumps; stops at RTS/RTL/RTI and at anything leaving the
    range. Returns the set of covered addresses and the highest one seen.
    """
    seen = set()
    work = [(start, entry_m, entry_x)]
    steps = 0
    while work and steps < budget:
        pc, m, x = work.pop()
        while steps < budget:
            steps += 1
            if not (start <= pc < end) or pc in seen:
                break
            ins = s.decode_insn(rom, s.lorom_offset(bank, pc), pc, bank, m, x)
            if ins is None:
                break
            for i in range(ins.length):
                seen.add(pc + i)
            mnem = ins.mnem.upper()
            if mnem == "REP":
                if ins.operand & 0x20: m = 0
                if ins.operand & 0x10: x = 0
            elif mnem == "SEP":
                if ins.operand & 0x20: m = 1
                if ins.operand & 0x10: x = 1
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
            if mnem in ("JMP", "JML"):
                tgt = ins.operand & 0xFFFF
                same_bank = (ins.operand >> 16) in (0, bank) or ins.length < 4
                if same_bank and start <= tgt < end:
                    pc = tgt
                    continue
                break
            pc += ins.length
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("cfg_dir")
    ap.add_argument("--min-tail", default="0x40",
                    help="report ranges whose unreachable tail is at least "
                         "this many bytes (default 0x40)")
    ap.add_argument("--top", type=int, default=0,
                    help="print only the N worst (0 = all)")
    args = ap.parse_args()
    min_tail = int(args.min_tail, 0)
    rom = s.load_rom(args.rom)

    findings = []
    total = 0
    for cfg in sorted(pathlib.Path(args.cfg_dir).glob("bank*.cfg")):
        bank = None
        for line in cfg.read_text(errors="replace").splitlines():
            b = BANK_RE.match(line)
            if b:
                bank = int(b.group(1), 16) | 0x80
                continue
            m = FUNC_RE.match(line)
            if not m or bank is None:
                continue
            name, start, end = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
            if end <= start or start < 0x8000:
                continue
            total += 1
            cov = reachable(rom, bank, start, min(end, 0x10000))
            if not cov:
                continue
            tail = min(end, 0x10000) - (max(cov) + 1)
            if tail >= min_tail:
                findings.append((tail, bank, name, start, end, len(cov)))

    findings.sort(reverse=True)
    shown = findings[:args.top] if args.top else findings
    print(f"{total} func declarations checked; "
          f"{len(findings)} with an unreachable tail >= {min_tail:#x}\n")
    print(f"{'tail':>7}  {'bank':>4}  {'declared':>13}  {'reached':>7}  name")
    for tail, bank, name, start, end, cov in shown:
        print(f"{tail:#7x}  ${bank:02X}  ${start:04X}-${end:04X}  "
              f"{cov:7d}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
