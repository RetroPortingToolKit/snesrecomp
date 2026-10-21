"""`live_operand`: an immediate read from the ROM image at run time.

A host that edits an operand in the loaded ROM (a cheat raising a limit) only
reaches compiled code if the emitter did not fold the constant into C.
"""
import os
import tempfile

import pytest

from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402


def _rom(code: bytes) -> bytes:
    return code + bytes(0x8000 - len(code))


def _load(text):
    fd, path = tempfile.mkstemp(suffix='.cfg', text=True)
    with os.fdopen(fd, 'w', encoding='utf-8') as f:
        f.write(text)
    try:
        return load_bank_cfg(path)
    finally:
        os.unlink(path)


def test_live_operand_parses():
    assert _load("bank = 00\nlive_operand c236\n").live_operand == {0xC236}


@pytest.mark.parametrize('text, why', [
    ("bank = 00\nlive_operand c236\nlive_operand c236\n", 'duplicate'),
    ("bank = 00\nlive_operand\n", 'exactly one'),
])
def test_live_operand_parse_errors(text, why):
    with pytest.raises(ValueError, match=why):
        _load(text)


def test_wide_immediate_becomes_a_rom_word_read():
    rom = _rom(bytes.fromhex('C9 40 00 60'))          # CMP #$0040 / RTS
    folded = emit_function(rom, bank=0, start=0x8000, entry_m=0, entry_x=0,
                           end=0x8004, func_name='Cmp')
    live = emit_function(rom, bank=0, start=0x8000, entry_m=0, entry_x=0,
                         end=0x8004, func_name='Cmp',
                         live_operand_sites={0x008000})
    assert '= 0x40;' in folded
    assert '= 0x40;' not in live
    assert 'cpu_read16(cpu, 0x00, (uint16)(0x8001))' in live
    # The cycle charge comes from the opcode and does not move.
    charge = [l for l in folded.splitlines() if 'cpu->cycles +=' in l]
    assert charge == [l for l in live.splitlines() if 'cpu->cycles +=' in l]


def test_narrow_immediate_becomes_a_rom_byte_read():
    rom = _rom(bytes.fromhex('C9 40 60'))             # CMP #$40 / RTS at m=1
    live = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                         end=0x8003, func_name='Cmp',
                         live_operand_sites={0x008000})
    assert 'cpu_read8(cpu, 0x00, (uint16)(0x8001))' in live


def test_non_immediate_site_is_rejected():
    rom = _rom(bytes.fromhex('A5 10 60'))             # LDA $10 / RTS
    with pytest.raises(ValueError, match='not an immediate'):
        emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                      end=0x8003, func_name='Lda',
                      live_operand_sites={0x008000})


@pytest.mark.parametrize('code, mnem, why', [
    ('C2 30 60', 'REP', 'a status operand'),           # REP #$30 / RTS
    ('E2 20 60', 'SEP', 'a status operand'),           # SEP #$20 / RTS
    ('54 7E 7F 60', 'MVN', 'not a data operand'),      # MVN $7E,$7F / RTS
])
def test_status_and_control_operands_are_refused(code, mnem, why):
    rom = _rom(bytes.fromhex(code))
    with pytest.raises(ValueError, match=f'{mnem} #imm is {why}'):
        emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                      end=0x8000 + len(bytes.fromhex(code)), func_name='Ctl',
                      live_operand_sites={0x008000})


# LDA #$00 / BEQ +5 / LDA #$11 / STA $40 / RTS / LDA #$22 / STA $40 / RTS:
# with the immediate folded, Z is known and the fall-through is dead code.
_BEQ = bytes.fromhex('A9 00 F0 05 A9 11 85 40 60 A9 22 85 40 60')


def test_decoder_does_not_fold_a_live_immediate():
    from v2.decoder import decode_function, clear_decode_cache
    rom = _rom(_BEQ)
    clear_decode_cache()
    folded = decode_function(rom, 0, 0x8000, 1, 1, end=0x800E)
    live = decode_function(rom, 0, 0x8000, 1, 1, end=0x800E,
                           live_operand_sites={0x008000})
    assert [f.branch_pc24 for f in folded.const_z_folds] == [0x008002]
    assert not live.const_z_folds
    pcs = {k.pc & 0xFFFF for k in live.insns}
    assert {0x8004, 0x8009} <= pcs              # both arms kept
    assert 0x8004 not in {k.pc & 0xFFFF for k in folded.insns}


def test_native_analyzer_does_not_fold_a_live_immediate(tmp_path):
    """The manifest must not drop the arm the original bytes made dead."""
    import pathlib
    import sys
    from _helpers import make_lorom_bank0
    repo = pathlib.Path(__file__).resolve().parents[2]
    if str(repo / 'tools') not in sys.path:
        sys.path.insert(0, str(repo / 'tools'))
    from v2_analyze import build_manifest_native
    from v2.program_analysis import VariantKey
    # LDA #$00 / BEQ +4 / JSR $9000 / RTS / JSR $9100 / RTS
    rom = make_lorom_bank0({
        0x8000: bytes.fromhex('A9 00 F0 04 20 00 90 60 20 00 91 60'),
        0x9000: bytes([0x60]), 0x9100: bytes([0x60])})
    rom_path = tmp_path / 'fixture.sfc'
    rom_path.write_bytes(rom)
    targets = {}
    for live in (False, True):
        cfg_dir = tmp_path / ('live' if live else 'folded')
        cfg_dir.mkdir()
        (cfg_dir / 'bank00.cfg').write_text(
            'bank = 00\nfunc Root 8000 end:800c entry_mx:1,1\n'
            + ('live_operand 8000\n' if live else ''), encoding='utf-8')
        manifest, _helpers, _inline, _out = build_manifest_native(
            rom_path=rom_path, cfg_dir=cfg_dir, all_cfg_roots=True)
        root = manifest.nodes[VariantKey(0x008000, 1, 1)]
        targets[live] = {d.target.pc24 for d in root.demands if d.target}
    assert targets[False] == {0x009100}          # Z=1 folds the BEQ taken
    assert targets[True] == {0x009000, 0x009100}  # both arms analysed


# Execution: the operand is edited in the loaded image between two runs of
# the same compiled routine, and the routine follows the edit both ways.
_CMP = bytes.fromhex('A9 05 C9 40 B0 04 A9 11 80 02 A9 22 85 41 60')

_MAIN = r'''
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d FAILED: %s\n", \
    __FILE__, __LINE__, #c); fails++; } } while (0)

static void call(RecompReturn (*fn)(CpuState *)) {
  CpuState cpu;
  memset(&cpu, 0, sizeof cpu);
  memset(g_ram, 0, sizeof g_ram);
  cpu.ram = g_ram;
  cpu.S = 0x01FF; cpu.m_flag = 1; cpu.x_flag = 1; cpu.P = 0x30;
  g_tc_valid = 0; g_recomp_stack_top = 0;
  cpu_push_jsr_return_frame(&cpu);
  fn(&cpu);
  CHECK(cpu.S == 0x01FF);
}

int main(void) {
  memcpy(g_rom, ROM_BEQ, sizeof ROM_BEQ);
  memcpy(g_rom + 0x100, ROM_CMP, sizeof ROM_CMP);
  g_rom[0x0001] = 0x00; call(LiveBeq_M1X1); CHECK(g_ram[0x40] == 0x22);
  g_rom[0x0001] = 0x07; call(LiveBeq_M1X1); CHECK(g_ram[0x40] == 0x11);
  g_rom[0x0001] = 0x00; call(LiveBeq_M1X1); CHECK(g_ram[0x40] == 0x22);
  g_rom[0x0103] = 0x40; call(LiveCmp_M1X1); CHECK(g_ram[0x41] == 0x11);
  g_rom[0x0103] = 0x03; call(LiveCmp_M1X1); CHECK(g_ram[0x41] == 0x22);
  g_rom[0x0103] = 0x40; call(LiveCmp_M1X1); CHECK(g_ram[0x41] == 0x11);
  if (fails) return 1;
  puts("ok");
  return 0;
}
'''


def test_compiled_routine_follows_the_edited_operand_both_ways(tmp_path):
    import pathlib
    import subprocess
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    rom = bytearray(_rom(_BEQ))
    rom[0x100:0x100 + len(_CMP)] = _CMP
    rom = bytes(rom)
    generated = (
        emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                      end=0x800E, func_name='LiveBeq',
                      live_operand_sites={0x008000})
        + emit_function(rom, bank=0, start=0x8100, entry_m=1, entry_x=1,
                        end=0x810F, func_name='LiveCmp',
                        live_operand_sites={0x008102}))
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    read8 = ('uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {\n'
             '  (void)cpu; (void)bank; return g_ram[addr];\n}')
    assert read8 in harness
    harness = harness.replace(read8, (
        'uint8 g_rom[0x8000];\n'
        'uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {\n'
        '  (void)cpu; (void)bank;\n'
        '  return addr >= 0x8000 ? g_rom[addr - 0x8000] : g_ram[addr];\n}'))
    def table(name, data):
        return (f'static const uint8 {name}[] = {{'
                + ', '.join(f'0x{b:02X}' for b in data) + '};\n')
    source = (harness + table('ROM_BEQ', _BEQ) + table('ROM_CMP', _CMP)
              + generated + _MAIN)
    c_file = tmp_path / 'live_operand.c'
    c_file.write_text(source, encoding='utf-8')
    exe = tmp_path / ('live_operand.exe' if os.name == 'nt' else 'live_operand')
    repo = pathlib.Path(__file__).resolve().parents[2]
    build = subprocess.run(
        [compiler, '-std=c11', '-O1', '-I', str(repo / 'runner' / 'src'),
         str(c_file), '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.strip() == 'ok'


@pytest.mark.parametrize("site", ["-1", "10000", "038B27", "838B27"])
def test_live_operand_rejects_addresses_outside_the_cfg_bank(site):
    # A typo must not silently select the low 16 bits of another bank's PC.
    with pytest.raises(ValueError, match="16-bit"):
        _load(f"bank = 03\nlive_operand {site}\n")


@pytest.mark.parametrize('bank', [0x03, 0x83])
@pytest.mark.parametrize('reg, opcode', [('A', 0xA9), ('X', 0xA2), ('Y', 0xA0)])
@pytest.mark.parametrize('narrow', [0, 1])
def test_compiled_live_operand_and_observer_use_program_bank_and_register_width(
        tmp_path, bank, reg, opcode, narrow):
    """Catch DB-relative fetches, X-width using M, and observers firing late."""
    import pathlib
    import subprocess
    import test_hle_func_seam as seam
    from snes65816 import get_rom_mapping, set_rom_mapping
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    m, x = (narrow, 1 - narrow) if reg == 'A' else (1 - narrow, narrow)
    code = bytes([0xE6, 0x40, opcode, 0x00] + ([] if narrow else [0x00]) + [0x60])
    rom = bytearray(0x20000)
    rom[0x18000:0x18000 + len(code)] = code
    old_mapping = get_rom_mapping()
    try:
        set_rom_mapping('lorom')
        generated = emit_function(
            bytes(rom), bank=bank, start=0x8000, entry_m=m, entry_x=x,
            end=0x8000 + len(code), func_name='Patched',
            live_operand_sites={(bank << 16) | 0x8002},
            entry_hooks={(bank << 16) | 0x8002: 'BeforeOperand'})
    finally:
        set_rom_mapping(old_mapping)
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    read8 = ('uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {\n'
             '  (void)cpu; (void)bank; return g_ram[addr];\n}')
    assert read8 in harness
    harness = harness.replace(read8, f'''
static unsigned reads, hooks, bad_bank;
static uint16 patched;
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {{
  (void)cpu;
  if (addr >= 0x8000) {{
    reads++;
    if (bank != 0x{bank:02X}) bad_bank++;
    return addr == 0x8003 ? (uint8)patched : (uint8)(patched >> 8);
  }}
  return g_ram[addr];
}}
void BeforeOperand(const CpuState *cpu, uint32 pc24) {{
  if (pc24 != 0x{bank:02X}8002 || cpu->{reg} != 0x35 ||
      g_ram[0x40] != 1 || reads != 0) abort();
  hooks++;
}}
''')
    main = f'''
int main(void) {{
  /* Both runs execute the same compiled function against different ROM data. */
  for (unsigned pass = 0; pass < 2; pass++) {{
    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);
    memset(g_ram, 0, sizeof g_ram);
    cpu.ram = g_ram;
    cpu.S = 0x01FF; cpu.m_flag = {m}; cpu.x_flag = {x};
    cpu.P = {(m << 5) | (x << 4)}; cpu.DB = 0x7E;
    cpu.{reg} = 0x35;
    reads = hooks = bad_bank = 0;
    patched = pass == 0 ? 0x1242 : 0x2363;
    cpu_push_jsr_return_frame(&cpu);
    Patched_M{m}X{x}(&cpu);
    unsigned want = {'(patched & 0xFF)' if narrow else 'patched'};
    if (cpu.{reg} != want || reads != {1 if narrow else 2} || bad_bank ||
        hooks != 1 || cpu.S != 0x01FF || g_other_exits) return 1;
  }}
  puts("ok");
  return 0;
}}
'''
    c_file = tmp_path / 'live_operand_bank.c'
    c_file.write_text(harness + generated + main, encoding='utf-8')
    exe = tmp_path / ('live_operand_bank.exe' if os.name == 'nt' else 'live_operand_bank')
    repo = pathlib.Path(__file__).resolve().parents[2]
    build = subprocess.run(
        [compiler, '-std=c11', '-O1', '-I', str(repo / 'runner' / 'src'),
         str(c_file), '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.strip() == 'ok'
