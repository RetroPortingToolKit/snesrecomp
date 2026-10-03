"""Bounded pointer candidates from already decoded instructions.

The result is a target universe for a *runtime pointer comparison*, never an
index-to-function substitution. Unsupported effects/joins terminate recovery.
This intentionally keeps uncertainty on the interpreter path.
"""
from snes65816 import IMM, ABS_X, ABS_Y, LONG_X, DP, ACC, lorom_offset


def recover_pointer_targets(rom, graph, site_key, pointer, long_pointer=False,
                            target_is_code=lambda pc: True):
    chain = []
    current = site_key
    visited = set()
    # Consume only a unique fall-through chain. Branch joins, backward edges,
    # calls and unknown effects cannot donate a pointer fact.
    while len(chain) < 256:
        preds = [di for di in graph.insns.values() if current in di.successors]
        if not preds:
            break
        if len(preds) != 1:
            return None
        pred = preds[0]
        insn = pred.insn
        if pred.key in visited or (pred.key.pc & 0xFFFF) + insn.length != (current.pc & 0xFFFF):
            return None
        if len(pred.successors) != 1 or insn.mnem in ("JSR", "JSL"):
            break
        visited.add(pred.key)
        chain.append(insn)
        current = pred.key
    chain.reverse()
    if not chain:
        return None
    # A small immediate index is an upper bound for candidate enumeration.
    # Including lower slots is safe only because emission compares the actual
    # loaded pointer and retains an interpreter default arm.
    bounds = [i.operand for i in chain if i.mnem in ("LDX", "LDY") and i.mode == IMM]
    bound = min(bounds[-1], 255) if bounds else (255 if site_key.x else -1)
    if bound < 0 or (bounds and bounds[-1] > 255):
        return None
    bank = site_key.pc >> 16
    targets = set()
    for seed in range(bound + 1):
        regs = {"A": None, "X": seed, "Y": seed}
        slots = {}
        valid = True
        for insn in chain:
            name, mode, value = insn.mnem, insn.mode, insn.operand
            amask = 0xFF if insn.m_flag else 0xFFFF
            xmask = 0xFF if insn.x_flag else 0xFFFF
            if name in ("REP", "SEP", "NOP", "PHP", "PLP"):
                continue  # widths come from the decoder's exact instruction
            if name in ("LDX", "LDY") and mode == IMM:
                regs[name[-1]] = seed if value <= 255 else None
            elif name == "LDA" and mode == IMM:
                regs["A"] = value & amask
            elif name == "LDA" and mode in (ABS_X, ABS_Y, LONG_X):
                index = regs["Y" if mode == ABS_Y else "X"]
                if index is None:
                    valid = False; break
                address = value + index
                read_bank = (address >> 16) if mode == LONG_X else bank
                address &= 0xFFFF
                try:
                    offset = lorom_offset(read_bank, address)
                except (AssertionError, ValueError):
                    valid = False; break
                size = 1 if insn.m_flag else 2
                if offset < 0 or offset + size > len(rom) or address + size > 0x10000:
                    valid = False; break
                regs["A"] = int.from_bytes(rom[offset:offset + size], "little")
            elif name in ("TAX", "TAY"):
                regs[name[-1]] = None if regs["A"] is None else regs["A"] & xmask
            elif name in ("TXA", "TYA"):
                regs["A"] = None if regs[name[1]] is None else regs[name[1]] & amask
            elif name in ("INX", "DEX", "INY", "DEY"):
                reg = name[-1]
                if regs[reg] is not None:
                    regs[reg] = (regs[reg] + (1 if name.startswith("IN") else -1)) & xmask
            elif name in ("AND", "ORA", "EOR") and mode == IMM:
                if regs["A"] is not None:
                    regs["A"] = {"AND": lambda a: a & value,
                                 "ORA": lambda a: a | value,
                                 "EOR": lambda a: a ^ value}[name](regs["A"]) & amask
            elif name in ("ASL", "LSR") and mode == ACC:
                if regs["A"] is not None:
                    regs["A"] = ((regs["A"] << 1) if name == "ASL" else (regs["A"] >> 1)) & amask
            elif name in ("STA", "STX", "STY", "STZ") and mode == DP:
                reg = {"STA": "A", "STX": "X", "STY": "Y"}.get(name)
                val = regs[reg] if reg else 0
                width = insn.x_flag if name in ("STX", "STY") else insn.m_flag
                for n in range(1 if width else 2):
                    slots[(value + n) & 0xFFFF] = None if val is None else (val >> (8*n)) & 255
            else:
                # Includes unknown writes, bank/DP changes, stack tricks,
                # calls and accumulator clobbers; never retain stale facts.
                valid = False; break
        need = 3 if long_pointer else 2
        values = [slots.get(pointer + n) for n in range(need)]
        if not valid or any(v is None for v in values):
            return None
        target = sum(v << (8*n) for n, v in enumerate(values))
        if not long_pointer:
            target |= bank << 16
        if not target_is_code(target):
            continue
        targets.add(target)
    return tuple(sorted(targets)) or None
