"""Conservative cached-pointer recipes for instruction-paced emission.

Only low WRAM may supply a pointer. A pure host-memory preview refuses MMIO
destinations before any guest mutation; the accepted instruction then performs
its architectural pointer fetch once through the paced CPU bus. Dispatch and
software-interrupt pointers are separate contracts and remain interpreted.
"""
import re
import snes65816 as d

MODES = {d.DP_INDIR, d.INDIR_DPX, d.INDIR_Y, d.INDIR_L, d.INDIR_LY, d.STK_IY}
NAMES = {"LDA", "STA", "ORA", "AND", "EOR", "ADC", "SBC", "CMP"}


def plan(insn, lines, cpu_cycles):
    if insn.mode not in MODES or insn.mnem not in NAMES:
        return None
    from v2.codegen import _emit_read, _emit_write
    from v2.ir import Read, Write, Value
    from v2.lowering import _segref_for
    from snes_cycles import xcross_add

    width = 1 if insn.m_flag else 2
    long_pointer = insn.mode in {d.INDIR_L, d.INDIR_LY}
    pointer_bytes = 3 if long_pointer else 2
    internal = cpu_cycles - insn.length - pointer_bytes - width
    if internal < 0:
        return None
    segment = _segref_for(insn)
    text = "\n".join(lines)
    name = f"_rp_{insn.addr:06X}_{insn.m_flag}{insn.x_flag}"
    # Match the current code generator's complete architectural access, not
    # a nested address expression. Indexed-indirect lowering may cache the
    # pointer and split a word crossing into a conditional sequence. The
    # precharge guards below refuse that crossing, so accepted instructions
    # replace the entire sequence with one guarded, cached paced access.
    # Reusing the emitted Read/Write shape supports both the original and
    # bank-carry-corrected lowering without reading the guest pointer twice.
    matches = []
    for vid in sorted(set(int(v) for v in re.findall(r"\b_v(\d+)\b", text))):
        value = Value(vid)
        if insn.mnem == "STA":
            original = "\n".join(_emit_write(Write(segment, value, width)))
            replacement = (f"cpu_write{width * 8}_paced(cpu, "
                f"(uint8)({name}_ea >> 16), (uint16){name}_ea, _v{vid});")
        else:
            original = "\n".join(_emit_read(Read(segment, width, value)))
            replacement = (f"uint{width * 8} _v{vid} = cpu_read{width * 8}_paced(cpu, "
                f"(uint8)({name}_ea >> 16), (uint16){name}_ea);")
        if text.count(original) == 1:
            matches.append((original, replacement))
    if len(matches) != 1:
        return None
    text = text.replace(*matches[0])
    if re.search(r"\bcpu_(?:read|write)(?:8|16)\(", text):
        return None

    offset = insn.operand & 0xFF
    base = "cpu->S" if insn.mode == d.STK_IY else "cpu->D"
    preindex = " + cpu->X" if insn.mode == d.INDIR_DPX else ""
    postindex = " + cpu->Y" if insn.mode in {d.INDIR_Y, d.INDIR_LY, d.STK_IY} else ""

    def effective(pointer):
        start = pointer if long_pointer else f"(((uint32)cpu->DB << 16) + {pointer})"
        return f"(({start}{postindex}) & 0xFFFFFFu)"

    # Preview is not a CPU bus read: the range guard proves plain low WRAM,
    # excludes devices/bank wrap, and leaves open_bus and every clock untouched.
    preview = f"((uint32)cpu->ram[{name}_at] | ((uint32)cpu->ram[{name}_at + 1u] << 8))"
    if long_pointer:
        preview += f" | ((uint32)cpu->ram[{name}_at + 2u] << 16)"
    prefix = [
        f"uint16 {name}_at = (uint16)({base} + 0x{offset:02X}u{preindex});",
        f"if ({name}_at > 0x{0x2000 - pointer_bytes:04X}u) {{ @FALLBACK@ }}",
        f"uint32 {name}_preview = {preview};",
        f"uint32 {name}_target = {effective(name + '_preview')};",
    ]
    refused = []
    for n in range(width):
        adr = f"({name}_target + {n}u)"
        refused.append(f"((({adr} >> 16) & 0x7Fu) < 0x40u && ({adr} & 0xFFFFu) >= 0x2000u && ({adr} & 0xFFFFu) < 0x6000u)")
    if width == 2:
        refused.append(f"({name}_target & 0xFFFFu) == 0xFFFFu")
    prefix.append(f"if ({' || '.join(refused)}) {{ @FALLBACK@ }}")

    body = [f"uint32 {name}_pointer = cpu_read16_paced(cpu, 0x00, {name}_at);"]
    if long_pointer:
        body.append(f"{name}_pointer |= (uint32)cpu_read8_paced(cpu, 0x00, (uint16)({name}_at + 2u)) << 16;")
    body.append(f"uint32 {name}_ea = {effective(name + '_pointer')};")
    # X=0's read penalty is already part of the static cost. X=1 requires the
    # actual pointer's page comparison; stores already include indexing cost.
    if insn.mode == d.INDIR_Y and insn.x_flag and xcross_add(insn.opcode):
        body.append(f"if (({name}_pointer >> 8) != (({name}_pointer + cpu->Y) >> 8)) {{ cpu->cycles += 1; cpu_add_aot_master_cycles(cpu, 6); }}")
    text = re.sub(r"cpu->master_cycles\s*\+=\s*([^;]+);",
                  r"cpu_add_aot_master_cycles(cpu, \1);", text)
    body.extend(text.split("\n"))
    pc = insn.addr & 0xFFFF
    costs = [f"cpu_region_speed(((uint32)cpu->PB << 16) | 0x{(pc + n) & 0xFFFF:04X}u)" for n in range(insn.length)]
    if internal:
        costs.append(str(internal * 6))
    return " + ".join(costs), body, prefix
