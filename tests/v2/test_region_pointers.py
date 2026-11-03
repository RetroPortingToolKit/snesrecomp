"""Cached pointer instructions against the actual interpreter and bus counters."""
import pytest

from test_region_pacing import run_region_cases, _emit


OPCODES = {
    "dp":       {"LDA": 0xB2, "STA": 0x92, "ORA": 0x12, "AND": 0x32, "EOR": 0x52, "ADC": 0x72, "SBC": 0xF2, "CMP": 0xD2},
    "dpx":      {"LDA": 0xA1, "STA": 0x81, "ORA": 0x01, "AND": 0x21, "EOR": 0x41, "ADC": 0x61, "SBC": 0xE1, "CMP": 0xC1},
    "dpy":      {"LDA": 0xB1, "STA": 0x91, "ORA": 0x11, "AND": 0x31, "EOR": 0x51, "ADC": 0x71, "SBC": 0xF1, "CMP": 0xD1},
    "long":     {"LDA": 0xA7, "STA": 0x87, "ORA": 0x07, "AND": 0x27, "EOR": 0x47, "ADC": 0x67, "SBC": 0xE7, "CMP": 0xC7},
    "long_y":   {"LDA": 0xB7, "STA": 0x97, "ORA": 0x17, "AND": 0x37, "EOR": 0x57, "ADC": 0x77, "SBC": 0xF7, "CMP": 0xD7},
    "stack_y":  {"LDA": 0xB3, "STA": 0x93, "ORA": 0x13, "AND": 0x33, "EOR": 0x53, "ADC": 0x73, "SBC": 0xF3, "CMP": 0xD3},
}


def _imm(value, m):
    return [0xA9, value & 0xFF] + ([] if m else [(value >> 8) & 0xFF])


def pointer_program(mode, opcode, m, target):
    """Initialize the pointer by ordinary instructions on both execution paths."""
    code = []
    low = target & 0xFFFF
    slot = 1 if mode == "dpx" else 0  # X starts at 1, Y at 2 in the shared fixture.
    def store(value, offset):
        code.extend(_imm(value, m))
        if mode == "stack_y":
            address = 0x020D + offset  # entry S=01FD, instruction operand=10.
            code.extend([0x8D, address & 0xFF, address >> 8])
        else:
            code.extend([0x85, slot + offset])
    if m:
        store(low, 0); store(low >> 8, 1)
    else:
        store(low, 0)
    if mode in {"long", "long_y"}:
        store(target >> 16, 2)
    code.extend(_imm(0x1234, m))
    code.extend([opcode, 0x10 if mode == "stack_y" else 0x00, 0x60])
    return bytes(code).hex(" ")


@pytest.mark.parametrize("mode", list(OPCODES))
def test_pointer_load_store_width_page_bank_and_single_fetch(tmp_path, mode):
    cases = []
    for name in ("LDA", "STA"):
        for m, x in ((1, 1), (1, 0), (0, 1), (0, 0)):
            for dp in (0x0100, 0x0101):
                for target in (0x7E0120, 0x7E00FF, 0x7EFFFE):
                    label = f"P_{mode}_{name}_{m}{x}_{dp:04X}_{target:06X}"
                    cases.append((label, pointer_program(mode, OPCODES[mode][name], m, target), m, x, dp))
    # Read-modify-register ALU cases use the same pointer recipes, not a
    # separate expression whose duplicate pointer reads could go unnoticed.
    for name in ("ORA", "AND", "EOR", "ADC", "SBC", "CMP"):
        for m, x in ((1, 1), (1, 0), (0, 1), (0, 0)):
            label = f"P_{mode}_{name}_{m}{x}"
            cases.append((label, pointer_program(mode, OPCODES[mode][name], m, 0x7EFFFE), m, x, 0x0101))
    # The shared harness maps banks7E/7F separately, compares every RAM byte,
    # and counts actual byte-bus reads at the pointer locations. A duplicated
    # observable pointer fetch fails even if the resulting value is unchanged.
    run_region_cases(tmp_path, cases, check_read_counts=True)


@pytest.mark.parametrize("opcode", [0xB2, 0x92, 0xA1, 0x81, 0xB1, 0x91, 0xA7, 0x87, 0xB7, 0x97, 0xB3, 0x93])
def test_pointer_guard_precedes_charge_and_guest_pointer_fetch(opcode):
    source = _emit(bytes([opcode, 0x10, 0x60]).hex(" "))
    block = source.split("L_8000_M1X1:", 1)[1]
    guard = block.index("interp_tier_dispatch_tail")
    assert guard < block.index("cpu->cycles +=")
    assert guard < block.index("cpu_read16_paced")
    assert "cpu->ram[" in block  # preview is guarded plain low-WRAM only.
    assert block.count("= cpu_read16_paced(cpu, 0x00,") == 1


def test_executed_pointer_guards_are_side_effect_free(tmp_path):
    generated = []
    main = ["s_interp_bounce_owner_depth = 1;"]
    for mode, operations in OPCODES.items():
        post = mode in {"dpy", "long_y", "stack_y"}
        for operation in ("LDA", "STA"):
            opcode = operations[operation]
            name = f"Refuse_{mode}_{operation}"
            generated.append(_emit(f"{opcode:02x} 10 60", name))
            # Both system-bank mirrors must fall back before pointer bus
            # reads, MMIO access, clock credit, register or stack mutation.
            for bank in (0, 0x80):
                loc = "cpu.S + 0x10" if mode == "stack_y" else "cpu.D + 0x10" + (" + cpu.X" if mode == "dpx" else "")
                target = 0x2140 - (2 if post else 0)
                main += [f'''{{ CpuState cpu; const uint8 code[] = {{0x{opcode:02x},0x10,0x60}};
  prepare(&cpu, code, sizeof code, 1, 1, 0); cpu.D=0x0100; cpu.DB=0x{bank:02x};
  uint16 at = (uint16)({loc}); g_ram[at]=0x{target & 255:02x}; g_ram[at+1]=0x{target >> 8:02x}; g_ram[at+2]=0x{bank:02x};
  {name}_M1X1(&cpu);
  unsigned reads=0; for (unsigned i=0;i<0x40;i++) reads+=g_probe_reads[i];
  if (cpu.cycles || cpu.master_cycles || credited_master || g_mmio_reads || reads ||
      cpu.A!=0x1234 || cpu.X!=1 || cpu.Y!=2 || cpu.S!=0x01FD ||
      cpu.P!=0x30 || cpu.D!=0x0100 || !cpu.m_flag || !cpu.x_flag ||
      g_balanced_calls || g_other_exits!=1 || g_other_target!=0x008000) {{
    fprintf(stderr,"{name} MMIO guard failed\\n"); fails++;
  }} }}''']
            # Pointer storage itself must remain in low WRAM. Preparing host
            # RAM bytes does not authorize a CPU read from a device window.
            loc = "cpu.S + 0x10" if mode == "stack_y" else "cpu.D + 0x10" + (" + cpu.X" if mode == "dpx" else "")
            setup = "cpu.S=0x1FF0;" if mode == "stack_y" else "cpu.D=0x1FF0;"
            main += [f'''{{ CpuState cpu; const uint8 code[] = {{0x{opcode:02x},0x10,0x60}};
  prepare(&cpu, code, sizeof code, 1, 1, 0); {setup}
  uint16 before=cpu.S; {name}_M1X1(&cpu);
  if (cpu.cycles || cpu.master_cycles || credited_master || g_mmio_reads ||
      cpu.S!=before || g_other_exits!=1) {{ fprintf(stderr,"{name} pointer-window guard failed\\n"); fails++; }}
}}''']
            # A 16-bit data access at xx:FFFF remains interpreter-owned even
            # when the cached pointer and its bank are themselves ordinary RAM.
            wide_name = name + "Wide"
            generated.append(_emit(f"{opcode:02x} 10 60", wide_name, m=0))
            target = 0xFFFF - (2 if post else 0)
            main += [f'''{{ CpuState cpu; const uint8 code[] = {{0x{opcode:02x},0x10,0x60}};
  prepare(&cpu, code, sizeof code, 0, 1, 0); cpu.D=0x0100;
  uint16 at=(uint16)({loc}); g_ram[at]=0x{target & 255:02x}; g_ram[at+1]=0x{target >> 8:02x}; g_ram[at+2]=0x7E;
  {wide_name}_M0X1(&cpu);
  if (cpu.cycles || cpu.master_cycles || credited_master || g_mmio_reads ||
      cpu.S!=0x01FD || g_other_exits!=1) {{ fprintf(stderr,"{name} word-boundary guard failed\\n"); fails++; }}
}}''']
    main.append("s_interp_bounce_owner_depth = 0;")
    # One ordinary case keeps the shared driver active; guard bodies above
    # are executed explicitly and are never passed off as native equivalence.
    run_region_cases(tmp_path, [("GuardControl", "EA 60", 1, 1, 0)],
                     extra_emitted="\n".join(generated), extra_main="\n".join(main))
