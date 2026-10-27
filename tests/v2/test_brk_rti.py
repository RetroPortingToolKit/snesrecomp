"""`brk_rti`: a BRK whose native vector is a bare RTI is a two-byte no-op."""
import os
import pathlib
import subprocess
import sys
import tempfile

import pytest

from _helpers import make_lorom_bank0  # noqa: E402
from v2.brk_rti import check_brk_rti_site  # noqa: E402
from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402

_REPO = pathlib.Path(__file__).resolve().parents[2]

# 8000  LDA #$05 / BRK #$00 / STA $10 / RTS      (m=1)
CODE = bytes.fromhex('A9 05 00 00 85 10 60')


def _rom(handler: int) -> bytes:
    return make_lorom_bank0({0x8000: CODE, 0x8100: bytes([handler]),
                             0xFFE6: b'\x00\x81'})


def _load(text):
    fd, path = tempfile.mkstemp(suffix='.cfg', text=True)
    with os.fdopen(fd, 'w', encoding='utf-8') as f:
        f.write(text)
    try:
        return load_bank_cfg(path)
    finally:
        os.unlink(path)


def test_brk_rti_parses():
    assert _load("bank = 00\nbrk_rti 8002\n").brk_rti == {0x8002}


@pytest.mark.parametrize('text, why', [
    ("bank = 00\nbrk_rti 8002\nbrk_rti 8002\n", 'duplicate'),
    ("bank = 00\nbrk_rti\n", 'exactly one'),
])
def test_brk_rti_parse_errors(text, why):
    with pytest.raises(ValueError, match=why):
        _load(text)


def test_round_trip_is_compiled_and_falls_through():
    src = emit_function(_rom(0x40), bank=0, start=0x8000, entry_m=1, entry_x=1,
                        end=0x8007, func_name='Fill', brk_rti_sites={0x008002})
    assert 'BRK to a bare RTI' in src
    # the four bytes the interrupt pushes: PB, $8004 (past the signature), P
    assert 'cpu_write8(cpu, 0x00, (uint16)(_s - 1), 0x80);' in src
    assert 'cpu_write8(cpu, 0x00, (uint16)(_s - 2), 0x04);' in src
    # ... and the code after it is compiled
    assert 'cpu->D + 0x0010' in src
    assert src.index('BRK to a bare RTI') < src.index('cpu->D + 0x0010')
    # The only exit to the interpreter is emulation mode, whose BRK goes
    # through $FFFE, a vector the directive did not validate.
    guard = src.index('if (cpu->emulation) {', src.index('BRK to a bare RTI'))
    assert src.count('interp_tier_dispatch_tail') == 1
    assert guard < src.index('interp_tier_dispatch_tail') < src.index('uint16 _s = cpu->S;')


def test_without_the_directive_brk_still_ends_the_function():
    src = emit_function(_rom(0x40), bank=0, start=0x8000, entry_m=1, entry_x=1,
                        end=0x8007, func_name='Fill')
    assert 'BRK to a bare RTI' not in src
    assert 'cpu->D + 0x0010' not in src


def test_a_vector_that_is_not_a_bare_rti_is_rejected():
    with pytest.raises(ValueError, match='not a bare RTI'):
        emit_function(_rom(0xEA), bank=0, start=0x8000, entry_m=1, entry_x=1,
                      end=0x8007, func_name='Fill', brk_rti_sites={0x008002})


def test_a_site_that_is_not_brk_is_rejected():
    with pytest.raises(ValueError, match='not a BRK'):
        emit_function(_rom(0x40), bank=0, start=0x8000, entry_m=1, entry_x=1,
                      end=0x8007, func_name='Fill', brk_rti_sites={0x008000})


# ── the native analyzer applies the same rule, in the same words ───────────

def _native(tmp_path, rom, site, root_end=0x8007):
    sys.path.insert(0, str(_REPO / 'tools'))
    from v2_analyze import build_manifest_native
    rom_path = tmp_path / 'fixture.sfc'
    rom_path.write_bytes(rom)
    cfg_dir = tmp_path / 'cfg'
    cfg_dir.mkdir(exist_ok=True)
    (cfg_dir / 'bank00.cfg').write_text(
        f'bank = 00\nfunc Root 8000 end:{root_end:x} entry_mx:1,1\n'
        + (f'brk_rti {site & 0xFFFF:x}\n' if site is not None else ''),
        encoding='utf-8')
    manifest, _helpers, _inline, _out = build_manifest_native(
        rom_path=rom_path, cfg_dir=cfg_dir, all_cfg_roots=True)
    return manifest


@pytest.mark.parametrize('handler, site', [
    (0xEA, 0x008002),     # vector is not a bare RTI
    (0x40, 0x008000),     # site is LDA, not BRK
    (0xEA, 0x009000),     # never decoded, zero-filled, vector still wrong
    (0x40, 0x009001),     # never decoded, not even ROM-checked by decode
])
def test_native_and_python_reject_the_same_sites_alike(tmp_path, handler, site):
    rom = bytearray(_rom(handler))
    if site == 0x009001:
        rom[0x1001] = 0xEA                     # NOP at $9001: not a BRK
    rom = bytes(rom)
    with pytest.raises(ValueError) as python:
        check_brk_rti_site(rom, site)
    with pytest.raises(RuntimeError) as native:
        _native(tmp_path, rom, site)
    assert str(python.value) in str(native.value)


def test_native_manifest_compiles_through_a_declared_site(tmp_path):
    from v2.program_analysis import VariantKey
    rom = _rom(0x40)
    check_brk_rti_site(rom, 0x008002)          # accepted by Python ...
    (tmp_path / 'plain').mkdir()
    (tmp_path / 'declared').mkdir()
    plain = _native(tmp_path / 'plain', rom, None)
    declared = _native(tmp_path / 'declared', rom, 0x008002)
    root = VariantKey(0x008000, 1, 1)
    assert 'brk_at_008002' in plain.nodes[root].reasons
    node = declared.nodes[root]                # ... and by the analyzer
    assert not any(r.startswith('brk_at_') for r in node.reasons)
    assert node.max_pc24 >= 0x008006           # the code past it is decoded
    assert declared.exit_modes[root] == (1, 1)


# ── compiled versus interpreted round trip ─────────────────────────────────
# 8000  BRK #$00 / STZ $40 / RTS ; native BRK vector -> $9000: RTI.
# STZ does not touch P, so P after the RTS is P as the RTI restored it.
_TRIP = bytes.fromhex('00 00 64 40 60')

_MAIN = r'''
#include "snes/interp816.h"

int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }

static uint8 g_mem_ram[0x10000];
static uint8_t bus_read(void *m, uint32_t adr) {
  (void)m; adr &= 0xFFFF;
  return adr >= 0x8000 ? g_rom[adr - 0x8000] : g_mem_ram[adr];
}
static void bus_write(void *m, uint32_t adr, uint8_t v) {
  (void)m; adr &= 0xFFFF;
  if (adr < 0x8000) g_mem_ram[adr] = v;
}

static int fails;
static const char *scenario;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d %s FAILED: %s\n", \
    __FILE__, __LINE__, scenario, #c); fails++; } } while (0)

typedef struct {
  uint8 p, pb; uint16 s; uint8 stack[4]; uint8 zp[2]; uint64 cycles;
} Outcome;

static void compiled(RecompReturn (*fn)(CpuState *), uint8 p, Outcome *o) {
  CpuState cpu;
  memset(&cpu, 0, sizeof cpu);
  memset(g_ram, 0, sizeof g_ram);
  g_ram[0x40] = g_ram[0x41] = 0xFF;
  cpu.ram = g_ram;
  cpu.S = 0x01FF; cpu.P = p; cpu_p_to_mirrors(&cpu);
  g_tc_valid = 0; g_recomp_stack_top = 0; g_other_exits = 0;
  cpu_push_jsr_return_frame(&cpu);            /* S = $01FD at the BRK */
  fn(&cpu);
  cpu_mirrors_to_p(&cpu);
  o->p = cpu.P; o->pb = cpu.PB; o->s = cpu.S;
  memcpy(o->stack, &g_ram[0x01FA], 4);
  memcpy(o->zp, &g_ram[0x40], 2);
  o->cycles = cpu.cycles;
  CHECK(cpu.master_cycles == cpu.cycles * 8);  /* all of it in bank $00 */
  CHECK(g_other_exits == 0);
}

static void interpreted(uint8 p, Outcome *o) {
  Interp816 *cpu = interp816_init(NULL, bus_read, bus_write);
  interp816_set_brk_hook_enabled(cpu, false);
  memset(g_mem_ram, 0, sizeof g_mem_ram);
  g_mem_ram[0x40] = g_mem_ram[0x41] = 0xFF;
  g_mem_ram[0x01FE] = 0xFF; g_mem_ram[0x01FF] = 0x7F;   /* a return frame */
  cpu->e = false; cpu->sp = 0x01FD; cpu->k = 0; cpu->pc = 0x8000;
  cpu->dp = 0; cpu->db = 0;
  interp816_setFlags(cpu, p);
  uint64 cycles = 0;
  cycles += (uint64)interp816_runOpcode(cpu);         /* BRK */
  CHECK(cpu->k == 0 && cpu->pc == 0x9000);
  CHECK(cpu->sp == 0x01F9);
  CHECK(cpu->i && !cpu->d);
  cycles += (uint64)interp816_runOpcode(cpu);         /* RTI */
  CHECK(cpu->k == 0 && cpu->pc == 0x8002);            /* past the signature */
  CHECK(cpu->sp == 0x01FD);
  CHECK(interp816_getFlags(cpu) == p);
  cycles += (uint64)interp816_runOpcode(cpu);         /* STZ $40 */
  cycles += (uint64)interp816_runOpcode(cpu);         /* RTS */
  o->p = interp816_getFlags(cpu); o->pb = cpu->k; o->s = cpu->sp;
  memcpy(o->stack, &g_mem_ram[0x01FA], 4);
  memcpy(o->zp, &g_mem_ram[0x40], 2);
  o->cycles = cycles;
  interp816_free(cpu);
}

static void same(RecompReturn (*fn)(CpuState *), uint8 p) {
  Outcome c, i;
  compiled(fn, p, &c);
  interpreted(p, &i);
  CHECK(c.p == p && i.p == p);
  CHECK(c.pb == 0 && i.pb == 0);
  CHECK(c.s == 0x01FF && i.s == 0x01FF);
  CHECK(memcmp(c.stack, i.stack, 4) == 0);
  CHECK(c.stack[3] == 0x00 && c.stack[2] == 0x80 && c.stack[1] == 0x02 &&
        c.stack[0] == p);                     /* PB, $8002, P */
  CHECK(memcmp(c.zp, i.zp, 2) == 0);          /* STZ ran at the continuation */
  CHECK(c.cycles == i.cycles);
  if (c.cycles != i.cycles)
    fprintf(stderr, "  compiled %llu, interpreted %llu cycles\n",
            (unsigned long long)c.cycles, (unsigned long long)i.cycles);
}

int main(void) {
  memcpy(g_rom, ROM_TRIP, sizeof ROM_TRIP);
  g_rom[0x1000] = 0x40;                       /* $9000: RTI */
  g_rom[0x7FE6] = 0x00; g_rom[0x7FE7] = 0x90; /* native BRK vector */

  scenario = "8-bit, D/V/C set";
  same(Trip_M1X1, 0x30 | 0x08 | 0x40 | 0x01);
  scenario = "16-bit, D/N/C set";
  same(Trip_M0X0, 0x08 | 0x80 | 0x01);
  scenario = "16-bit A, 8-bit X, Z set";
  same(Trip_M0X1, 0x10 | 0x02);

  /* Emulation mode vectors through $FFFE: compiled code hands the BRK, with
   * nothing pushed yet, to the interpreter. */
  scenario = "emulation mode";
  {
    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);
    memset(g_ram, 0, sizeof g_ram);
    cpu.ram = g_ram;
    cpu.S = 0x01FF; cpu.P = 0x30; cpu_p_to_mirrors(&cpu); cpu.emulation = 1;
    g_tc_valid = 0; g_recomp_stack_top = 0; g_other_exits = 0;
    cpu_push_jsr_return_frame(&cpu);
    Trip_M1X1(&cpu);
    CHECK(g_other_exits == 1 && g_other_target == 0x008000u);
    CHECK(g_ram[0x01FA] == 0 && g_ram[0x01FB] == 0 && g_ram[0x01FC] == 0 &&
          g_ram[0x01FD] == 0);
  }

  if (fails) return 1;
  puts("ok");
  return 0;
}
'''


def test_compiled_round_trip_matches_the_interpreter(tmp_path):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    rom = bytearray(make_lorom_bank0({0x8000: _TRIP, 0x9000: bytes([0x40]),
                                      0xFFE6: b'\x00\x90'}))
    rom = bytes(rom)
    generated = ''.join(
        emit_function(rom, bank=0, start=0x8000, entry_m=m, entry_x=x,
                      end=0x8005, func_name='Trip', brk_rti_sites={0x008000})
        for m, x in ((1, 1), (0, 0), (0, 1)))
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    read8 = ('uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {\n'
             '  (void)cpu; (void)bank; return g_ram[addr];\n}')
    assert read8 in harness
    harness = harness.replace(read8, (
        'uint8 g_rom[0x8000];\n'
        'uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {\n'
        '  (void)cpu; (void)bank;\n'
        '  return addr >= 0x8000 ? g_rom[addr - 0x8000] : g_ram[addr];\n}'))
    table = ('static const uint8 ROM_TRIP[] = {'
             + ', '.join(f'0x{b:02X}' for b in _TRIP) + '};\n')
    c_file = tmp_path / 'brk_rti.c'
    c_file.write_text(harness + table + generated + _MAIN, encoding='utf-8')
    exe = tmp_path / ('brk_rti.exe' if os.name == 'nt' else 'brk_rti')
    build = subprocess.run(
        [compiler, '-std=c11', '-O1', '-I', str(_REPO / 'runner' / 'src'),
         '-I', str(_REPO / 'runner' / 'src' / 'snes'), str(c_file),
         str(_REPO / 'runner' / 'src' / 'snes' / 'interp816.c'),
         '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.strip() == 'ok'
