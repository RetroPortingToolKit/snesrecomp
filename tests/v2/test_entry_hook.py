"""`entry_hook`: an observational host callback before a compiled instruction.

Contract (docs/ENTRY_HOOK.md): the callback runs once per execution of the
hooked instruction by compiled code, after the block's deadline check and
before anything of that instruction runs or is charged. An execution the
compiled code hands to the interpreter is not reported by compiled code; the
interpreter executes the instruction and its own pre-opcode hook sees it.
"""
import os
import pathlib
import subprocess
import tempfile

import pytest

from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402

_REPO = pathlib.Path(__file__).resolve().parents[2]


def _load(text):
    fd, path = tempfile.mkstemp(suffix='.cfg', text=True)
    with os.fdopen(fd, 'w', encoding='utf-8') as f:
        f.write(text)
    try:
        return load_bank_cfg(path)
    finally:
        os.unlink(path)


def test_entry_hook_parses():
    cfg = _load("bank = 00\nentry_hook 9437 HostSawList\n")
    assert cfg.entry_hook == {0x9437: 'HostSawList'}


@pytest.mark.parametrize('text, why', [
    ("bank = 00\nentry_hook 9437\n", 'needs <pc16> <c_function>'),
    ("bank = 00\nentry_hook 9437 9Bad\n", 'valid C identifier'),
    ("bank = 00\nentry_hook 9437 A\nentry_hook 9437 B\n", 'duplicate'),
    ("bank = 00\nhle_func 9437 HleList\nentry_hook 9437 A\n",
     'replaced by hle_func'),
])
def test_entry_hook_parse_errors(text, why):
    with pytest.raises(ValueError, match=why):
        _load(text)


# Count  $8000: INC $40 / LDA $40 / AND #$03 / BNE $8000 / RTS
#               (the hooked entry is also the loop head)
# Rec    $8100: LDA $41 / BEQ + / DEC $41 / JSR $8100 / + RTS
# Twice  $8200: JSR $8000 / JSR $8000 / RTS
# Fall   $8300: LDA #$05 / STA $42 / [$8304, hooked] INC $42 / RTS
_CODE = {
    0x8000: 'E6 40 A5 40 29 03 D0 F8 60',
    0x8100: 'A5 41 F0 05 C6 41 20 00 81 60',
    0x8200: '20 00 80 20 00 80 60',
    0x8300: 'A9 05 85 42 E6 42 60',
}
_FUNCS = [  # (start, end, name)
    (0x8000, 0x8009, 'bank_00_8000'),
    (0x8100, 0x810A, 'bank_00_8100'),
    (0x8200, 0x8207, 'Twice'),
    (0x8300, 0x8307, 'Fall'),
]
_HOOKS = {0x008000: 'HostSaw', 0x008100: 'HostSaw', 0x008304: 'HostSaw'}


def _rom() -> bytes:
    rom = bytearray(0x8000)
    for pc, code in _CODE.items():
        data = bytes.fromhex(code)
        rom[pc - 0x8000:pc - 0x8000 + len(data)] = data
    return bytes(rom)


def _emit(name):
    start, end, func = next(f for f in _FUNCS if f[2] == name)
    return emit_function(_rom(), bank=0, start=start, entry_m=1, entry_x=1,
                         end=end, func_name=func, entry_hooks=_HOOKS)


def test_hook_is_observational_and_sits_between_deadline_and_charge():
    src = _emit('bank_00_8000')
    assert 'extern void HostSaw(const CpuState *cpu, uint32 pc24);' in src
    call = 'HostSaw(cpu, 0x008000u);'
    assert src.count(call) == 1          # one block, reached by every entry
    entry = src.index('  L_8000_M1X1:')
    # A yield at the block hands this execution to the interpreter, whose own
    # hook reports it; the compiled call must come after that exit ...
    assert src.index('interp_bridge_lle_yield_unwind', entry) < src.index(call)
    # ... and before anything of the instruction is charged or run.
    assert src.index(call) < src.index('cpu->cycles +=', entry)
    assert src.index(call) < src.index('cpu_write8', entry)


def test_a_fall_through_into_the_hooked_pc_starts_a_hooked_block():
    src = _emit('Fall')
    label = src.index('  L_8304_M1X1:')
    call = src.index('HostSaw(cpu, 0x008304u);')
    assert src.index('interp_bridge_lle_yield_unwind', label) < call
    assert call < src.index('cpu->cycles +=', label)


def test_no_hook_no_code():
    rom = bytes([0x60]) + bytes(0x7FFF)
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                        end=0x8001, func_name='Plain',
                        entry_hooks={0x009000: 'Elsewhere'})
    assert 'Elsewhere' not in src


_PRELUDE = r'''
RecompReturn bank_00_8000_M0X0(CpuState *cpu);
RecompReturn bank_00_8000_M0X1(CpuState *cpu);
RecompReturn bank_00_8000_M1X0(CpuState *cpu);
RecompReturn bank_00_8100_M0X0(CpuState *cpu);
RecompReturn bank_00_8100_M0X1(CpuState *cpu);
RecompReturn bank_00_8100_M1X0(CpuState *cpu);

/* The host's observer: records what it saw, changes nothing. */
enum { kMaxHooks = 32 };
static int g_hooks;
static uint32 g_hook_pc[kMaxHooks];
static uint16 g_hook_a[kMaxHooks], g_hook_s[kMaxHooks];
static uint8 g_hook_counter[kMaxHooks];
void HostSaw(const CpuState *cpu, uint32 pc24) {
  if (g_hooks < kMaxHooks) {
    g_hook_pc[g_hooks] = pc24;
    g_hook_a[g_hooks] = cpu->A;
    g_hook_s[g_hooks] = cpu->S;
    g_hook_counter[g_hooks] = cpu->ram[pc24 == 0x008000u ? 0x40 :
                                       pc24 == 0x008100u ? 0x41 : 0x42];
  }
  g_hooks++;
}
'''

_MAIN = r'''
static int fails;
static const char *scenario;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d %s FAILED: %s\n", \
    __FILE__, __LINE__, scenario, #c); fails++; } } while (0)

static RecompReturn wrong_width(CpuState *cpu) {
  (void)cpu; fails++; fprintf(stderr, "%s: wrong-width variant\n", scenario);
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_00_8000_M0X0(CpuState *cpu) { return wrong_width(cpu); }
RecompReturn bank_00_8000_M0X1(CpuState *cpu) { return wrong_width(cpu); }
RecompReturn bank_00_8000_M1X0(CpuState *cpu) { return wrong_width(cpu); }
RecompReturn bank_00_8100_M0X0(CpuState *cpu) { return wrong_width(cpu); }
RecompReturn bank_00_8100_M0X1(CpuState *cpu) { return wrong_width(cpu); }
RecompReturn bank_00_8100_M1X0(CpuState *cpu) { return wrong_width(cpu); }

static void prepare(CpuState *cpu, int deadline_at) {
  memset(cpu, 0, sizeof *cpu);
  memset(g_ram, 0, sizeof g_ram);
  cpu->ram = g_ram;
  cpu->S = 0x01FF; cpu->m_flag = 1; cpu->x_flag = 1; cpu->P = 0x30;
  cpu->A = 0x77;
  g_tc_valid = 0; g_recomp_stack_top = 0;
  g_other_exits = 0; g_other_target = 0;
  g_hooks = 0; g_deadline_n = 0; g_deadline_at = deadline_at;
  cpu_push_jsr_return_frame(cpu);       /* host calls the routine like a JSR */
}

int main(void) {
  CpuState cpu;

  scenario = "loop back to the hooked entry";
  prepare(&cpu, 0);
  bank_00_8000_M1X1(&cpu);
  CHECK(g_hooks == 4);                  /* $8000 executed four times */
  for (int i = 0; i < 4; i++) {
    CHECK(g_hook_pc[i] == 0x008000u);
    CHECK(g_hook_counter[i] == i);      /* before the INC of that pass */
    CHECK(g_hook_s[i] == 0x01FD);
  }
  CHECK(g_hook_a[0] == 0x77);           /* the caller's A */
  CHECK(g_hook_a[1] == 1 && g_hook_a[2] == 2 && g_hook_a[3] == 3);
  CHECK(g_ram[0x40] == 4);
  CHECK(g_other_exits == 0);

  scenario = "deadline in the prologue";
  prepare(&cpu, 1);
  bank_00_8000_M1X1(&cpu);
  CHECK(g_hooks == 0);                  /* nothing executed compiled */
  CHECK(g_other_exits == 1 && g_other_target == 0x008000u);
  CHECK(g_ram[0x40] == 0);

  scenario = "deadline at the hooked block";
  prepare(&cpu, 2);
  bank_00_8000_M1X1(&cpu);
  CHECK(g_hooks == 0);
  CHECK(g_other_exits == 1 && g_other_target == 0x008000u);
  CHECK(g_ram[0x40] == 0);

  scenario = "deadline on the back-edge to the hooked block";
  prepare(&cpu, 3);
  bank_00_8000_M1X1(&cpu);
  CHECK(g_hooks == 1);                  /* the one execution done compiled */
  CHECK(g_ram[0x40] == 1);
  CHECK(g_other_exits == 1 && g_other_target == 0x008000u);

  scenario = "two calls";
  prepare(&cpu, 0);
  Twice_M1X1(&cpu);
  CHECK(g_hooks == 8);
  for (int i = 0; i < 8 && i < kMaxHooks; i++) {
    CHECK(g_hook_pc[i] == 0x008000u);
    CHECK(g_hook_s[i] == 0x01FB);       /* host frame + JSR frame */
  }
  CHECK(g_ram[0x40] == 8);
  CHECK(g_other_exits == 0);

  scenario = "recursion";
  prepare(&cpu, 0);
  g_ram[0x41] = 2;
  bank_00_8100_M1X1(&cpu);
  CHECK(g_hooks == 3);
  CHECK(g_hook_s[0] == 0x01FD && g_hook_s[1] == 0x01FB && g_hook_s[2] == 0x01F9);
  CHECK(g_hook_counter[0] == 2 && g_hook_counter[1] == 1 &&
        g_hook_counter[2] == 0);
  CHECK(g_ram[0x41] == 0);
  CHECK(g_other_exits == 0);
  CHECK(cpu.S == 0x01FF);

  scenario = "fall-through into a hooked instruction";
  prepare(&cpu, 0);
  Fall_M1X1(&cpu);
  CHECK(g_hooks == 1);
  CHECK(g_hook_pc[0] == 0x008304u);
  CHECK(g_hook_counter[0] == 5);        /* STA done, INC not yet */
  CHECK(g_hook_a[0] == 5);
  CHECK(g_ram[0x42] == 6);
  CHECK(g_other_exits == 0);

  if (fails) return 1;
  puts("ok");
  return 0;
}
'''


def test_compiled_hook_reports_each_execution_exactly_once(tmp_path):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    deadline = ('int interp_bridge_lle_master_deadline_reached(const CpuState *cpu) {\n'
                '  (void)cpu; return 0;\n}')
    assert deadline in harness
    harness = harness.replace(deadline, (
        '/* Scripted deadline: the Nth check of the run reports expiry. */\n'
        'static int g_deadline_n, g_deadline_at;\n'
        'int interp_bridge_lle_master_deadline_reached(const CpuState *cpu) {\n'
        '  (void)cpu; return ++g_deadline_n == g_deadline_at;\n}'))
    generated = ''.join(_emit(name) for _, _, name in _FUNCS)
    source = harness + _PRELUDE + generated + _MAIN
    c_file = tmp_path / 'entry_hook.c'
    c_file.write_text(source, encoding='utf-8')
    exe = tmp_path / ('entry_hook.exe' if os.name == 'nt' else 'entry_hook')
    build = subprocess.run(
        [compiler, '-std=c11', '-O1', '-I', str(_REPO / 'runner' / 'src'),
         str(c_file), '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.strip() == 'ok'


def test_program_emission_hooks_every_body_and_mirror(tmp_path):
    """Through v2_emit: the routine's own body, a body that falls into the
    hooked instruction, and the LoROM mirror bodies all carry the call."""
    import re
    import sys
    from _helpers import make_lorom_bank0
    # Root  $8000: JSR $8100 / JSR $80F0 / JML $80:8100
    # Sub   $8100: INC $40 / RTS             (hooked)
    # Lead  $80F0: LDA #$01 / STA $40 / falls into Sub
    rom = bytearray(make_lorom_bank0({
        0x8000: bytes.fromhex('20 00 81 20 F0 80 5C 00 81 80'),
        0x80F0: bytes.fromhex('A9 01 85 40') + bytes([0xEA] * 12),
        0x8100: bytes.fromhex('E6 40 60'),
    }))
    rom[0x7FFC:0x7FFE] = bytes([0x00, 0x80])
    (tmp_path / 'game.sfc').write_bytes(bytes(rom))
    cfg_dir = tmp_path / 'recomp'
    cfg_dir.mkdir()
    (cfg_dir / 'bank00.cfg').write_text('\n'.join([
        'bank = 00',
        'func I_RESET 8000 end:800a entry_mx:1,1',
        'func Lead 80f0 end:8103 entry_mx:1,1',
        'func Sub 8100 end:8103 entry_mx:1,1',
        'entry_hook 8100 HostSaw',
    ]) + '\n', encoding='utf-8')
    out_dir = tmp_path / 'gen'
    result = subprocess.run([
        sys.executable, str(_REPO / 'tools' / 'v2_emit.py'),
        '--rom', str(tmp_path / 'game.sfc'), '--cfg-dir', str(cfg_dir),
        '--out-dir', str(out_dir), '--no-host-root-scan',
    ], text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    hooked = {}
    for path in sorted(out_dir.glob('bank*_v2*.c')):
        bank = int(path.name[4:6], 16)
        text = path.read_text(encoding='utf-8')
        for body in re.split(r'^RecompReturn ', text, flags=re.M)[1:]:
            name = body.split('(', 1)[0]
            blocks = re.split(r'^  (L_[0-9A-F]{4}_M[01]X[01]):', body,
                              flags=re.M)
            for label, block in zip(blocks[1::2], blocks[2::2]):
                if label.startswith('L_8100_'):
                    call = f'HostSaw(cpu, 0x{bank:02X}8100u);'
                    assert block.count(call) == 1, (path.name, name, label)
                    hooked.setdefault(bank, set()).add(name.split('_M')[0])
    assert {'Sub', 'Lead'} <= hooked[0x00]
    assert 0x80 in hooked                  # the JML $80:8100 mirror body


@pytest.mark.parametrize("site", ["-1", "10000", "038B27", "838B27"])
def test_entry_hook_rejects_addresses_outside_the_cfg_bank(site):
    with pytest.raises(ValueError, match="16-bit"):
        _load(f"bank = 03\nentry_hook {site} HostSaw\n")
