"""Opt-in instruction timing against interp816's counted-bus model.

Fetch bytes are charged without reading the ROM twice. Architectural data
accesses use the runtime paced helpers; remaining CPU cycles are internal/6.
Unsupported instructions tier to the interpreter before any charge. In
particular, indirect pointers/dispatches are not re-read to estimate timing.
This is instruction-boundary clock parity, not a bus-phase scheduler.
"""
import re

import snes65816 as d

_M = {"LDA", "STA", "ORA", "AND", "EOR", "ADC", "SBC", "CMP", "BIT", "STZ"}
_X = {"LDX", "LDY", "STX", "STY", "CPX", "CPY"}
_RMW = {"ASL", "LSR", "ROL", "ROR", "INC", "DEC", "TSB", "TRB"}
_DIRECT = {d.DP, d.DP_X, d.DP_Y, d.ABS, d.ABS_X, d.ABS_Y, d.LONG, d.LONG_X, d.STK}
_SPECIAL = {"COP", "BRK", "RTI", "MVN", "MVP", "WAI", "STP", "PEI", "XCE"}
_BUS_CALL = re.compile(r"\bcpu_(?:read|write)(8|16)\(")


def data_bytes(insn):
    """Known architectural data bytes, or None for conservative fallback."""
    name, mode = insn.mnem, insn.mode
    if name in _SPECIAL or getattr(insn, "dispatch_entries", None):
        return None
    if name in {"JSR", "JMP"} and mode not in {d.ABS, d.LONG}:
        return None
    m = 1 if insn.m_flag else 2
    x = 1 if insn.x_flag else 2
    if name in {"PHA", "PLA"}: return m
    if name in {"PHX", "PLX", "PHY", "PLY"}: return x
    if name in {"PHP", "PLP", "PHB", "PLB", "PHK"}: return 1
    if name in {"PHD", "PLD", "PEA", "PER", "JSR", "RTS"}: return 2
    if name in {"JSL", "RTL"}: return 3
    if mode in {d.IMP, d.ACC, d.IMM, d.REL, d.REL16}:
        return 0
    if name == "JMP" and mode in {d.ABS, d.LONG}:
        return 0
    if mode not in _DIRECT:
        return None
    if name in _M: return m
    if name in _X: return x
    if name in _RMW: return m * 2
    return None


def plan(insn, lines, cpu_cycles, *, live_operand=False):
    """Return fetch/internal precharge and paced body, or None.

    Checking the emitted byte-access count prevents an optimization or new
    lowering from silently invalidating a previously supported bus recipe.
    The supported bodies have one straight-line data-access sequence; complex
    dispatch/pointer/control helpers deliberately remain authoritative LLE.
    """
    from v2.region_pointers import MODES, plan as pointer_plan
    if insn.mode in MODES and not live_operand:
        return pointer_plan(insn, lines, cpu_cycles)
    count = data_bytes(insn)
    if count is None or live_operand:
        return None
    text = "\n".join(lines)
    actual = sum(int(bits) // 8 for bits in _BUS_CALL.findall(text))
    internal = cpu_cycles - insn.length - count
    if actual != count or internal < 0:
        return None
    # Opcode/operand fetch wraps within the live program bank. Do not call a
    # read accessor: materialized immediate bytes already supplied their value.
    pc = insn.addr & 0xFFFF
    costs = [f"cpu_region_speed(((uint32)cpu->PB << 16) | 0x{(pc + n) & 0xFFFF:04X}u)"
             for n in range(insn.length)]
    if internal:
        costs.append(str(internal * 6))
    paced = re.sub(r"\bcpu_(read|write)(8|16)\(", r"cpu_\1\2_paced(", text)
    # Dynamic DP/page/branch additions are native instruction time too. Credit
    # at its origin, never infer elapsed time around nested mixed-tier calls.
    paced = re.sub(r"cpu->master_cycles\s*\+=\s*([^;]+);",
                   r"cpu_add_aot_master_cycles(cpu, \1);", paced).split("\n")
    return " + ".join(costs), paced, []


def mmio_guard(insn):
    """Pure address predicates only; never perform a data/pointer read."""
    count = data_bytes(insn)
    if not count:
        return None
    mode, name, value = insn.mode, insn.mnem, insn.operand
    stack = {"PHA", "PLA", "PHX", "PLX", "PHY", "PLY", "PHP", "PLP", "PHB", "PLB",
             "PHK", "PHD", "PLD", "PEA", "PER", "JSR", "JSL", "RTS", "RTL"}
    if name in stack:
        # Conservative window also covers every preincrement/decrement byte.
        return "cpu->S >= 0x1FFBu && cpu->S < 0x6005u"
    idx = " + cpu->X" if mode in {d.DP_X, d.ABS_X, d.LONG_X} else " + cpu->Y" if mode in {d.DP_Y, d.ABS_Y} else ""
    if mode in {d.DP, d.DP_X, d.DP_Y}:
        ea = f"(uint16)(cpu->D + 0x{value & 0xFF:02X}u{idx})"
    elif mode == d.STK:
        ea = f"(uint16)(cpu->S + 0x{value & 0xFF:02X}u)"
    elif mode in {d.ABS, d.ABS_X, d.ABS_Y}:
        ea = f"((((uint32)cpu->DB << 16) + 0x{value & 0xFFFF:04X}u{idx}) & 0xFFFFFFu)"
    elif mode in {d.LONG, d.LONG_X}:
        ea = f"((0x{value & 0xFFFFFF:06X}u{idx}) & 0xFFFFFFu)"
    else:
        return None
    # RMW count includes both directions, but maximum accessed width is half.
    width = count // 2 if name in _RMW else count
    terms = [f"(({ea}) & 0xFFFFu) == 0xFFFFu"] if width > 1 else []
    for n in range(width):
        adr = f"(({ea}) + {n}u)"
        terms.append(f"((({adr} >> 16) & 0x7Fu) < 0x40u && ({adr} & 0xFFFFu) >= 0x2000u && ({adr} & 0xFFFFu) < 0x6000u)")
    return " || ".join(terms)
