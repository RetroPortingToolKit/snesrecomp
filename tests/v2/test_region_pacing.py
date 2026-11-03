"""ROM-free generated execution vs actual interp816 bus callbacks."""
import os
import re
from pathlib import Path
import subprocess

import pytest

from _helpers import make_lorom_bank0
from snes65816 import get_rom_mapping, set_rom_mapping, ROM_MAP_LOROM
from v2.emit_function import emit_function

REPO = Path(__file__).resolve().parents[2]


def _emit(code, name="Case", m=1, x=1, **options):
    old = get_rom_mapping()
    try:
        set_rom_mapping(ROM_MAP_LOROM)
        rom = make_lorom_bank0({0x8000: bytes.fromhex(code)})
        return emit_function(rom, 0, 0x8000, m, x, func_name=name,
                             instruction_blocks=True, region_paced=True, **options)
    finally:
        set_rom_mapping(old)


def test_region_pacing_requires_instruction_blocks():
    with pytest.raises(ValueError, match="requires instruction-blocks"):
        emit_function(make_lorom_bank0({0x8000: b"\x60"}), 0, 0x8000, 1, 1,
                      region_paced=True)


@pytest.mark.parametrize("code", ["A5 00 60", "BD FF 00 60", "D0 01 EA 60"])
def test_all_region_master_charges_use_origin_credit_api(code):
    source = _emit(code)
    assert "cpu_add_aot_master_cycles(cpu," in source
    assert "cpu->master_cycles +=" not in source


@pytest.mark.parametrize("code", ["AD 40 21 60", "B1 10 60", "02 01 60", "54 7E 7F 60", "FB 60"])
def test_mmio_or_unmodeled_instruction_falls_back_before_charge(code):
    source = _emit(code)
    block = source.split("L_8000_M1X1:", 1)[1]
    guard = block.index("interp_tier_dispatch_tail")
    charge = block.find("cpu->cycles +=")
    assert charge < 0 or guard < charge
    assert "cpu_read16_paced(cpu, 0x00, (uint16)(cpu->D" not in block
    assert "interp_tier_dispatch_balanced" not in source


def _c_function(source, name):
    start = re.search(r"(?m)^(?:static\s+)?(?:inline\s+)?[A-Za-z_]\w*\s+\**" + re.escape(name) + r"\(", source).start()
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def run_region_cases(tmp_path, cases=None, *, extra_main="", extra_emitted="", check_read_counts=False):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip("C compiler required")
    runtime = (REPO / "runner/src/cpu_state.c").read_text(encoding="utf-8")
    paced = "static inline bool snes_set_cpu_read_clocked(Snes *snes, bool clocked);\n"
    paced += _c_function(runtime, "is_hw_reg")
    paced += "void (*g_cpu_aot_master_credit_hook)(CpuState *cpu, uint64 clocks);\n"
    paced += "void (*g_cpu_aot_instruction_boundary_hook)(CpuState *cpu);\n"
    paced += "\n".join(_c_function(runtime, name) for name in (
        "cpu_aot_instruction_boundary", "cpu_add_aot_master_cycles", "cpu_region_speed", "cpu_read8_paced", "cpu_read16_paced",
        "cpu_write8_paced", "cpu_write16_paced"))
    harness = seam._HARNESS.split("/* The C helpers.")[0]
    harness = harness.replace("return g_ram[addr];", "return cpu->open_bus = g_ram[addr];")
    harness = harness.replace("g_ram[addr] = v;", "g_ram[addr] = v; cpu->open_bus = v;")
    harness = harness.replace("g_ram[addr]", "g_ram[(bank == 0x7f ? 0x10000u : 0u) + addr]")
    harness = "static unsigned g_mmio_reads, g_probe_reads[0x40];\n" + harness
    harness = harness.replace(_c_function(harness, "interp_tier_dispatch_tail"), "")
    harness = harness.replace("(void)cpu; (void)bank; return cpu->open_bus",
        "if (bank == 0 && addr >= 0x100 && addr < 0x120) g_probe_reads[addr-0x100]++; "
        "if (bank == 0 && addr >= 0x200 && addr < 0x220) g_probe_reads[addr-0x200+0x20]++; "
        "if ((bank & 0x7f) < 0x40 && addr >= 0x2000 && addr < 0x6000) g_mmio_reads++; return cpu->open_bus")
    extra = r'''
#include "snes/interp816.h"
int snes_frame_counter;
static int g_balanced_calls, s_interp_bounce_owner_depth;
static uint64 credited_master;
static void record_credit(CpuState *cpu, uint64 clocks) { (void)cpu; credited_master += clocks; }
int cpu_interrupt_context_active(void) { return 0; }
RecompReturn interp_tier_dispatch_interrupt(CpuState *cpu, uint32 pc) {
  (void)cpu; (void)pc; return 0;
}
int interp816_opcode_hook(uint32_t address) { (void)address; return 0; }
RecompReturn interp_tier_dispatch_balanced(CpuState *cpu, uint32 pc,
  uint32 site, uint16 entry, uint8 hrv, bool tail) {
  (void)cpu; (void)site; (void)entry; (void)hrv; (void)tail;
  g_other_exits++; g_other_target = pc; g_balanced_calls++; return 0;
}
static unsigned bus_count; static uint64_t bus_master;
static uint8_t reference_read(void *opaque, uint32_t address) {
  bus_count++; bus_master += cpu_region_speed(address);
  return cpu_read8(opaque, address >> 16, (uint16)address);
}
static void reference_write(void *opaque, uint32_t address, uint8_t value) {
  bus_count++; bus_master += cpu_region_speed(address);
  cpu_write8(opaque, address >> 16, (uint16)address, value);
}
static uint8 expected_ram[sizeof g_ram];
static unsigned expected_reads[0x40];
static void prepare(CpuState *cpu, const uint8_t *code, unsigned length,
                    int m, int x, int fast) {
  memset(cpu, 0, sizeof *cpu); memset(g_ram, 0, sizeof g_ram);
  memcpy(g_ram + 0x8000, code, length);
  g_ram[0x0100] = 0x56; g_ram[0x0101] = 0x78;
  g_ram[0x10000] = 0xAB; g_ram[0x10001] = 0xCD; g_ram[0x10002] = 0xEF;
  cpu->ram = g_ram; cpu->S = 0x01FF; cpu->A = 0x1234;
  cpu->X = 1; cpu->Y = 2; cpu->D = 0; cpu->DB = 0x7E;
  cpu->P = (m ? 0x20 : 0) | (x ? 0x10 : 0); cpu_p_to_mirrors(cpu);
  cpu->PB = fast ? 0x80 : 0; g_memsel = fast;
  g_other_exits = 0; g_tc_valid = 0; g_recomp_stack_top = 0; g_mmio_reads = 0; g_balanced_calls = 0;
  credited_master = 0; g_cpu_aot_master_credit_hook = record_credit;
  memset(g_probe_reads, 0, sizeof g_probe_reads);
  cpu_push_jsr_return_frame(cpu);
}
static int check(const char *name, RecompReturn (*fn)(CpuState *),
                 const uint8_t *code, unsigned len, int m, int x, int dp, int fast) {
  CpuState native, memory;
  prepare(&native, code, len, m, x, fast); native.D = dp;
  fn(&native); cpu_mirrors_to_p(&native); memcpy(expected_ram, g_ram, sizeof g_ram);
  memcpy(expected_reads, g_probe_reads, sizeof g_probe_reads);
  if (credited_master != native.master_cycles) {
    fprintf(stderr, "%s native credit missing/doubled\n", name); return 1;
  }
  if (g_other_exits) { fprintf(stderr, "%s unexpectedly fell back at %06x\n", name, g_other_target); return 1; }
  prepare(&memory, code, len, m, x, fast);
  Interp816 ref; memset(&ref, 0, sizeof ref);
  ref.mem = &memory; ref.read = reference_read; ref.write = reference_write;
  ref.a = memory.A; ref.x = memory.X; ref.y = memory.Y; ref.sp = memory.S;
  ref.pc = 0x8000; ref.k = memory.PB; ref.db = memory.DB; ref.dp = dp;
  interp816_setFlags(&ref, memory.P); ref.e = false;
  uint64_t cycles = 0, master = 0;
  for (unsigned n = 0; n < 256 && ref.pc != 1; n++) {
    bus_count = 0; bus_master = 0;
    unsigned c = interp816_runOpcode(&ref);
    cycles += c; master += bus_master + (c > bus_count ? c - bus_count : 0) * 6u;
  }
  if (ref.pc != 1 || native.cycles != cycles || native.master_cycles != master ||
      native.A != ref.a || native.X != ref.x || native.Y != ref.y ||
      native.S != ref.sp || native.D != ref.dp || native.DB != ref.db ||
      native.P != interp816_getFlags(&ref) || native.open_bus != memory.open_bus ||
      credited_master || memcmp(expected_ram, g_ram, sizeof g_ram) ||
      (CHECK_READ_COUNTS && memcmp(expected_reads, g_probe_reads, sizeof g_probe_reads))) {
    fprintf(stderr, "%s failed: cycles %llu/%llu master %llu/%llu S %04x/%04x P %02x/%02x bus %02x/%02x\n", name,
      (unsigned long long)native.cycles, (unsigned long long)cycles,
      (unsigned long long)native.master_cycles, (unsigned long long)master,
      native.S, ref.sp, native.P, interp816_getFlags(&ref), native.open_bus, memory.open_bus);
    return 1;
  }
  return 0;
}
'''
    cases = cases if cases is not None else [
        ("Immediate", "A9 05 EA 60", 1, 1, 0),
        ("Wide", "A9 34 12 EA 60", 0, 0, 0),
        ("Dp", "A5 00 85 04 60", 1, 1, 0x100),
        ("DpPenalty", "A5 00 85 04 60", 1, 1, 0x101),
        ("WideDp", "A5 00 85 04 60", 0, 0, 0x100),
        ("PageRead", "BD FF 00 60", 1, 1, 0),
        ("WideIndex", "BD FF 00 60", 1, 0, 0),
        ("PageStore", "9D FF 00 60", 1, 1, 0),
        ("Rmw", "EE 00 01 60", 1, 1, 0),
        ("WideRmw", "EE 00 01 60", 0, 0, 0),
        ("Stack", "48 68 DA FA 5A 7A 08 28 8B AB 0B 2B 60", 1, 1, 0),
        ("WideStack", "48 68 DA FA 5A 7A 0B 2B 60", 0, 0, 0),
        ("Effective", "F4 34 12 68 68 60", 1, 1, 0),
        ("BranchTaken", "A9 01 D0 01 EA 60", 1, 1, 0),
        ("BranchNotTaken", "A9 00 D0 01 EA 60", 1, 1, 0),
        ("DirectCall", "20 10 80 60 " + "EA " * 12 + "EA 60", 1, 1, 0),
        ("LongCall", "22 20 80 00 60 " + "EA " * 27 + "EA 6B", 1, 1, 0),
    ]
    exits = {(pc, m, x): (m, x) for pc in (0x008010, 0x008020) for m in (0, 1) for x in (0, 1)}
    emitted = "".join(f"RecompReturn bank_00_{pc:04X}_M{m}X{x}(CpuState *cpu);\n"
                      for pc in (0x8010, 0x8020) for m in (0, 1) for x in (0, 1))
    emitted += "".join(_emit(code, name, m, x, callee_exit_mx=exits) for name, code, m, x, dp in cases)
    for name, code, m, x, dp in (case for case in cases if case[0] in {"DirectCall", "LongCall"}):
        rom = make_lorom_bank0({0x8000: bytes.fromhex(code)})
        for m in (0, 1):
            for x in (0, 1):
                pc = 0x8010 if name == "DirectCall" else 0x8020
                emitted += emit_function(rom, 0, pc, m, x, instruction_blocks=True, region_paced=True)
    emitted += _emit("AD 40 21 60", "Mmio") + _emit("A5 00 60", "DpMmio")
    emitted += extra_emitted
    bridge = (REPO / "runner/src/snes/interp_bridge.c").read_text(encoding="utf-8")
    # Compile the actual owner-aware tail helper, rather than a stub that
    # would conceal accidentally starting a new nested interpreter frame.
    tail_helpers = (_c_function(bridge, "interp_tier_dispatch_tail_ex") +
                    _c_function(bridge, "interp_tier_dispatch_tail"))
    timeline = _c_function((REPO / "runner/src/snes/interp_bridge.h").read_text(encoding="utf-8"),
                           "interp_bridge_use_absolute_apu_timeline")
    credit_fixture = r'''
typedef struct { int portTimeValid; } FakeApu;
struct Snes { void *cart; FakeApu *apu; bool cpuReadClocked; };
Snes *g_snes;
static int s_interp_bridge_depth;
static uint64 s_apu_pending_master;
static int timeline_on, extended_on, sa1_on, shared_on;
bool rtl_apu_frame_timeline_active(void) { return timeline_on; }
bool rtl_apu_extended_frame_timing(void) { return extended_on; }
bool cart_has_sa1(void *cart) { (void)cart; return sa1_on; }
int cosim_apu_shared_clock(void) { return shared_on; }
#define SNES_COSIM 1
'''
    credit_fixture += _c_function((REPO / "runner/src/snes/snes.h").read_text(encoding="utf-8"),
                                  "snes_set_cpu_read_clocked")
    credit_fixture += timeline + _c_function(bridge, "bridge_aot_credit_master")
    credit_fixture += "\n#undef SNES_COSIM\n"
    main = ["int main(void) { int fails = 0;"]
    for name, code, m, x, dp in cases:
        values = ",".join(f"0x{b:02x}" for b in bytes.fromhex(code))
        main += [f"{{ const uint8 code[] = {{{values}}};",
                 f' for (int fast=0; fast<2; fast++) fails += check("{name}", {name}_M{m}X{x}, code, sizeof code, {m}, {x}, {dp}, fast); }}']
    main += [r'''
  { CpuState cpu; const uint8 code[] = {0xad,0x40,0x21,0x60};
    s_interp_bounce_owner_depth = 1;
    prepare(&cpu, code, sizeof code, 1, 1, 0); cpu.DB = 0;
    Mmio_M1X1(&cpu);
    if (cpu.cycles || cpu.master_cycles || cpu.S != 0x01FD ||
        credited_master || g_mmio_reads || g_balanced_calls || g_other_exits != 1 || g_other_target != 0x008000) fails++;
    prepare(&cpu, code, sizeof code, 1, 1, 0); cpu.D = 0x2140;
    DpMmio_M1X1(&cpu);
    if (cpu.cycles || cpu.master_cycles || g_mmio_reads || g_balanced_calls || g_other_exits != 1) fails++;
    prepare(&cpu, code, sizeof code, 1, 1, 0); cpu.emulation = 1;
    Mmio_M1X1(&cpu);
    if (cpu.cycles || cpu.master_cycles || g_mmio_reads || g_balanced_calls || g_other_exits != 1) fails++;
    s_interp_bounce_owner_depth = 0;
    prepare(&cpu, code, sizeof code, 1, 1, 0); cpu.DB = 0;
    Mmio_M1X1(&cpu);
    if (cpu.cycles || cpu.master_cycles || g_mmio_reads || g_balanced_calls != 1 || g_other_exits != 1) fails++;
  }
  { CpuState cpu; memset(&cpu, 0, sizeof cpu);
    FakeApu apu = {0}; Snes snes = {0, &apu}; g_snes = &snes;
    g_cpu_aot_master_credit_hook = bridge_aot_credit_master;
    s_interp_bridge_depth = 1; s_apu_pending_master = 0;
    cpu_add_aot_master_cycles(&cpu, 11);       /* outer AOT instruction */
    cpu.master_cycles += 7; s_apu_pending_master += 7; /* nested LLE opcode */
    s_interp_bridge_depth = 2;
    cpu_add_aot_master_cycles(&cpu, 13);       /* nested AOT instruction */
    s_interp_bridge_depth = 1;
    if (cpu.master_cycles != 31 || s_apu_pending_master != 31) fails++;
    s_interp_bridge_depth = 0; cpu_add_aot_master_cycles(&cpu, 5);
    if (s_apu_pending_master != 31) fails++;
    s_interp_bridge_depth = 1; g_snes = NULL; cpu_add_aot_master_cycles(&cpu, 5);
    if (s_apu_pending_master != 31) fails++;
    g_snes = &snes; timeline_on = 1; sa1_on = 1; s_apu_pending_master = 0;
    cpu_add_aot_master_cycles(&cpu, 6); /* existing SA-1 absolute clock */
    if (s_apu_pending_master) fails++;
    sa1_on = 0; extended_on = 1; apu.portTimeValid = 1;
    cpu_add_aot_master_cycles(&cpu, 6); /* absolute mode still schedules sync */
    if (s_apu_pending_master != 6) fails++;
    timeline_on = 0; cpu_add_aot_master_cycles(&cpu, 7);
    if (s_apu_pending_master != 13) fails++;
    shared_on = 1; cpu_add_aot_master_cycles(&cpu, 8);
    if (s_apu_pending_master != 13) fails++;
    g_cpu_aot_master_credit_hook = NULL;
    /* Real #247 scope helper must restore both unclocked and nested reads. */
    snes.cpuReadClocked = false;
    cpu_read8_paced(&cpu, 0, 0x4212);
    if (snes.cpuReadClocked) fails++;
    snes.cpuReadClocked = true;
    cpu_read16_paced(&cpu, 0, 0x4212);
    if (!snes.cpuReadClocked) fails++;
    uint64 before = cpu.master_cycles; cpu_add_aot_master_cycles(&cpu, 9);
    if (cpu.master_cycles != before + 9 || s_apu_pending_master != 13) fails++;
  }
''']
    main += [extra_main]
    main += [f'if (!fails) puts("{len(cases) * 2} synthetic native/interpreter timing cases passed"); return fails != 0; }}']
    c_file = tmp_path / "region.c"
    c_file.write_text(f"#define CHECK_READ_COUNTS {int(check_read_counts)}\n" + harness + paced + extra + tail_helpers + credit_fixture + emitted + "\n".join(main))
    exe = tmp_path / ("region.exe" if os.name == "nt" else "region")
    build = subprocess.run([compiler, "-std=c11", "-O1", "-I", str(REPO / "runner/src"),
                            str(c_file), str(REPO / "runner/src/snes/interp816.c"), "-o", str(exe)],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert f"{len(cases) * 2} synthetic" in run.stdout


def test_executed_native_pacing_matches_interpreter_bus_costs(tmp_path):
    run_region_cases(tmp_path)
