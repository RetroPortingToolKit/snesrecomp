"""`mod_hook`: a mutating, runtime-armed host callback before a guest PC.

Contract (docs/MOD_HOOK.md): while armed, the callback runs once per execution
of the hooked instruction in compiled AND interpreted code, after the block's
deadline check and before anything of the instruction runs or is charged. It
may change A/X/Y/D/DB/flags and memory, and returns 0 (continue) or a declared
`mod_hook_target` (redirect). Disarmed, behaviour is identical to no directive.
The analyzers derive no value fact across a hook or target PC.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

import pytest

from _helpers import make_lorom_bank0
from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.decoder import (  # noqa: E402
    classify_dispatch_helper, clear_decode_cache, decode_function,
    detect_inline_arg_bytes)
from v2.emit_function import emit_function  # noqa: E402
from v2.program_emit import (  # noqa: E402
    ModHookDecl, emit_mod_hook_table, mod_hooks_for_bank)

_REPO = pathlib.Path(__file__).resolve().parents[2]


def _load(text):
    fd, path = tempfile.mkstemp(suffix='.cfg', text=True)
    with os.fdopen(fd, 'w', encoding='utf-8') as f:
        f.write(text)
    try:
        return load_bank_cfg(path)
    finally:
        os.unlink(path)


# ── Configuration ───────────────────────────────────────────────────────────

def test_mod_hook_parses():
    cfg = _load("bank = 00\nmod_hook 9102 HRedir\n"
                "mod_hook_target 9102 9106\nmod_hook_target 9102 9104\n")
    assert cfg.mod_hook == {0x9102: 'HRedir'}
    assert cfg.mod_hook_target == {0x9102: {0x9104, 0x9106}}


_CFG_ERRORS = [
    ("bank = 00\nmod_hook 9102\n", 'needs <pc16> <c_function>'),
    ("bank = 00\nmod_hook 9102 9Bad\n", 'valid C identifier'),
    ("bank = 00\nmod_hook 9102 Bad-Name\n", 'valid C identifier'),
    ("bank = 00\nmod_hook 9102 A\nmod_hook 9102 B\n", 'duplicate pc'),
    ("bank = 00\nmod_hook 10000 A\n", '16-bit'),
    ("bank = 00\nmod_hook 9102 A\nmod_hook_target 9102\n",
     'needs <hook_pc16> <target_pc16>'),
    ("bank = 00\nmod_hook 9102 A\nmod_hook_target 9102 19106\n", '16-bit'),
    ("bank = 00\nmod_hook 9102 A\nmod_hook_target 9102 9106\n"
     "mod_hook_target 9102 9106\n", 'duplicate target'),
    ("bank = 00\nmod_hook_target 9102 9106\n", 'not a mod_hook'),
    ("bank = 00\nhle_func 9100 Hle\nmod_hook 9100 A\n",
     'replaced by hle_func'),
    ("bank = 00\nfunc Sub 9100 end:9110\nhle_func 9100 Hle\n"
     "mod_hook 9104 A\n", 'replaced by hle_func'),
    ("bank = 00\nfunc Sub 9100 end:9110\nhle_func 9100 Hle\n"
     "mod_hook 9200 A\nmod_hook_target 9200 9108\n", 'replaced by hle_func'),
    ("bank = 00\nhle_spc_upload 9100\nmod_hook 9100 A\n",
     'replaced by hle_spc_upload'),
]


@pytest.mark.parametrize('text, why', _CFG_ERRORS)
def test_python_parser_rejects(text, why):
    with pytest.raises(ValueError, match=why):
        _load(text)


def test_hook_outside_an_hle_range_is_accepted():
    cfg = _load("bank = 00\nfunc Sub 9100 end:9110\nhle_func 9100 Hle\n"
                "mod_hook 9110 A\n")
    assert cfg.mod_hook == {0x9110: 'A'}


def _native_analyze(tmp_path, rom, cfg_text):
    sys.path.insert(0, str(_REPO / 'tools'))
    from v2_analyze import build_manifest_native
    rom_path = tmp_path / 'fixture.sfc'
    rom_path.write_bytes(rom)
    cfg_dir = tmp_path / f'cfg{len(list(tmp_path.glob("cfg*")))}'
    cfg_dir.mkdir()
    (cfg_dir / 'bank00.cfg').write_text(cfg_text, encoding='utf-8')
    return build_manifest_native(rom_path=rom_path, cfg_dir=cfg_dir,
                                 all_cfg_roots=True)


@pytest.mark.parametrize('text, why', _CFG_ERRORS)
def test_native_parser_rejects(tmp_path, text, why):
    rom = make_lorom_bank0({0x8000: bytes([0x60])})
    with pytest.raises(RuntimeError, match=why):
        _native_analyze(tmp_path, rom,
                        text + 'func Root 8000 end:8001 entry_mx:1,1\n')


# ── Analysis barrier ────────────────────────────────────────────────────────

# LDA #$00 / BEQ +4 / JSR $9000 / RTS / JSR $9100 / RTS
_FOLD = bytes.fromhex('A9 00 F0 04 20 00 90 60 20 00 91 60')


def _jsr_targets(graph):
    return {di.insn.operand & 0xFFFF for di in graph.insns.values()
            if di.insn.mnem == 'JSR'}


@pytest.mark.parametrize('hooks, targets', [
    (None, None),
    ({0x008002}, None),            # hook at the branch
    ({0x008000}, {0x008002}),      # redirect target at the branch
    ({0x008000}, None),            # hook at the load: the load still runs
])
def test_constant_z_fold_does_not_cross_a_hook_or_target(hooks, targets):
    rom = make_lorom_bank0({0x8000: _FOLD})
    clear_decode_cache()
    graph = decode_function(rom, 0, 0x8000, 1, 1, end=0x800C,
                            mod_hook_sites=hooks,
                            mod_hook_target_sites=targets)
    if hooks is None or (hooks == {0x008000} and targets is None):
        assert _jsr_targets(graph) == {0x9100}
        assert [f.branch_pc24 for f in graph.const_z_folds] == [0x008002]
    else:
        assert _jsr_targets(graph) == {0x9000, 0x9100}
        assert not graph.const_z_folds


@pytest.mark.parametrize('cfg_lines, python_sets', [
    ('', (None, None)),
    ('mod_hook 8002 HBranch\n', ({0x008002}, None)),
    ('mod_hook 8000 HLoad\nmod_hook_target 8000 8002\n',
     ({0x008000}, {0x008002})),
])
def test_native_analyzer_matches_python_decoder(tmp_path, cfg_lines,
                                                python_sets):
    """Same directive, same graph: the manifest demands equal the Python
    decoder's calls, and a second analysis is byte-identical."""
    from v2.program_analysis import VariantKey
    rom = make_lorom_bank0({0x8000: _FOLD, 0x9000: bytes([0x60]),
                            0x9100: bytes([0x60])})
    text = ('bank = 00\nfunc Root 8000 end:800c entry_mx:1,1\n' + cfg_lines)
    manifest, _h, _i, _o = _native_analyze(tmp_path, rom, text)
    again, _h, _i, _o = _native_analyze(tmp_path, rom, text)
    assert manifest.to_json() == again.to_json()
    root = manifest.nodes[VariantKey(0x008000, 1, 1)]
    native = {d.target.pc24 & 0xFFFF for d in root.demands if d.target}
    clear_decode_cache()
    graph = decode_function(rom, 0, 0x8000, 1, 1, end=0x800C,
                            mod_hook_sites=python_sets[0],
                            mod_hook_target_sites=python_sets[1])
    assert native == _jsr_targets(graph)
    assert native == ({0x9100} if not cfg_lines else {0x9000, 0x9100})


# LDX #$02 / LDA $9000,X / TAX / LDA $9100,X / STA $FE / LDA $9101,X /
# STA $FF / JMP ($00FE)  -- guarded pointer recovery enumerates 3 targets.
_PTR = bytes([0xA2, 2, 0xBD, 0, 0x90, 0xAA, 0xBD, 0, 0x91, 0x85, 0xFE,
              0xBD, 1, 0x91, 0x85, 0xFF, 0x6C, 0xFE, 0])


def _ptr_rom():
    rom = bytearray(make_lorom_bank0({0x8000: _PTR}))
    rom[0x1000:0x1003] = bytes([0, 3, 7])
    rom[0x1100:0x1109] = bytes([0, 0xA0, 0xFF, 0x10, 0xA0, 0xFF, 0xFF,
                                0x20, 0xA0])
    for off in (0x2000, 0x2010, 0x2020):
        rom[off:off + 3] = bytes([0xEA, 0x60, 0xEA])
    return bytes(rom)


@pytest.mark.parametrize('hooks, joins, expect', [
    (None, None, [0xA000, 0xA010, 0xA020]),
    ({0x008010}, None, None),          # hook at the JMP: memory may change
    ({0x00800E}, None, None),          # hook before the high-byte store
    (None, {0x00800B}, None),          # redirect into the chain is a join
])
def test_pointer_recovery_does_not_cross_a_hook(hooks, joins, expect):
    clear_decode_cache()
    graph = decode_function(_ptr_rom(), 0, 0x8000, 1, 1,
                            mod_hook_sites=hooks, mod_hook_target_sites=joins)
    jump = next(di.insn for di in graph.insns.values()
                if di.insn.opcode == 0x6C)
    got = [e & 0xFFFF for e in (jump.dispatch_entries or ())] or None
    assert got == expect


def test_inline_argument_and_helper_idioms_do_not_cross_a_hook():
    # REP #$30 / LDA $01,S / CLC / ADC #$0003 / STA $01,S / RTL
    rom = make_lorom_bank0({0x8000: bytes([
        0xC2, 0x30, 0xA3, 0x01, 0x18, 0x69, 0x03, 0x00, 0x83, 0x01, 0x6B])})
    assert detect_inline_arg_bytes(rom, 0, 0x8000, 1, 1) == 3
    assert detect_inline_arg_bytes(rom, 0, 0x8000, 1, 1,
                                   barrier_sites={0x008005}) is None
    # The entry itself runs before anything the idiom tracks.
    assert detect_inline_arg_bytes(rom, 0, 0x8000, 1, 1,
                                   barrier_sites={0x008000}) == 3
    from test_inline_dispatch_table_parity import SHORT_HELPER
    rom = make_lorom_bank0({0x8000: SHORT_HELPER})
    assert classify_dispatch_helper(rom, 0, 0x8000) == 'short'
    assert classify_dispatch_helper(rom, 0, 0x8000,
                                    barrier_sites={0x00800A}) is None


# ── Compiled and interpreted execution ──────────────────────────────────────

# Fold   $9000: LDA #$00 / [hook] BEQ +2 / INC $40 / INC $41 / RTS
# Redir  $9100: LDA #$05 / [hook] STA $42 / INC $42 / RTS   targets $9104,$9106
# Loop   $9200: [hook] INC $40 / LDA $40 / AND #$03 / BNE $9200 / RTS
# Rec    $9300: [hook] LDA $41 / BEQ + / DEC $41 / JSR $9300 / + RTS
# Lead   $9400: LDA #$05 / STA $43 / falls into Sub
# Sub    $9404: [hook] INC $43 / RTS
_CODE = {
    0x9000: 'A9 00 F0 02 E6 40 E6 41 60',
    0x9100: 'A9 05 85 42 E6 42 60',
    0x9200: 'E6 40 A5 40 29 03 D0 F8 60',
    0x9300: 'A5 41 F0 05 C6 41 20 00 93 60',
    0x9400: 'A9 05 85 43 E6 43 60',
}
_FUNCS = [  # (start, end, name)
    (0x9000, 0x9009, 'Fold'),
    (0x9100, 0x9107, 'Redir'),
    (0x9200, 0x9209, 'Loop'),
    (0x9300, 0x930A, 'bank_00_9300'),
    (0x9400, 0x9407, 'Lead'),
    (0x9404, 0x9407, 'Sub'),
]
_DECLS = [
    ModHookDecl(0, 0x1002, 'HFold', (0x009002, 0x809002), ()),
    ModHookDecl(1, 0x1102, 'HRedir', (0x009102, 0x809102), (0x9104, 0x9106)),
    ModHookDecl(2, 0x1200, 'HLoop', (0x009200, 0x809200), ()),
    ModHookDecl(3, 0x1300, 'HRec', (0x009300, 0x809300), ()),
    ModHookDecl(4, 0x1404, 'HFall', (0x009404, 0x809404), ()),
]


def _rom() -> bytes:
    rom = bytearray(0x8000)
    for pc, code in _CODE.items():
        data = bytes.fromhex(code)
        rom[pc - 0x8000:pc - 0x8000 + len(data)] = data
    return bytes(rom)


def _emit(name, *, bank=0, hooked=True, prefix=''):
    start, end, func = next(f for f in _FUNCS if f[2] == name)
    return emit_function(
        _rom(), bank=bank, start=start, entry_m=1, entry_x=1, end=end,
        func_name=prefix + func,
        mod_hooks=mod_hooks_for_bank(_DECLS, bank) if hooked else None)


def test_hook_sits_between_deadline_and_charge_behind_one_armed_test():
    src = _emit('Redir')
    assert src.count('snes_mod_hook_dispatch(cpu, 1u, 0x009102u)') == 1
    label = src.index('  L_9102_M1X1:')
    test = src.index('if (g_snes_mod_hook_armed[1]) {', label)
    assert src.index('interp_bridge_lle_yield_unwind', label) < test
    assert test < src.index('cpu->cycles +=', label)
    assert 'if (_mh == 0x009104u) goto L_9104_M1X1;' in src
    assert 'if (_mh == 0x009106u) goto L_9106_M1X1;' in src
    # The fold the hook invalidates is not taken: both arms are compiled.
    fold = _emit('Fold')
    assert 'L_9004_M1X1:' in fold
    assert 'L_9004_M1X1:' not in _emit('Fold', hooked=False)


def test_no_directive_no_code():
    for _s, _e, name in _FUNCS:
        plain = emit_function(_rom(), bank=0, start=_s, entry_m=1, entry_x=1,
                              end=_e, func_name=name)
        other_bank = emit_function(
            _rom(), bank=0, start=_s, entry_m=1, entry_x=1, end=_e,
            func_name=name, mod_hooks=mod_hooks_for_bank(_DECLS, 0x05))
        assert plain == other_bank
        assert 'mod_hook' not in plain


def test_redirect_target_must_be_compiled_at_the_hook_widths():
    decls = [ModHookDecl(0, 0, 'H', (0x009102,), (0x9180,))]
    with pytest.raises(ValueError, match=r'mod_hook_target \$009180 is not '
                                         r'compiled at M1X1'):
        emit_function(_rom(), bank=0, start=0x9100, entry_m=1, entry_x=1,
                      end=0x9107, func_name='Redir',
                      mod_hooks=mod_hooks_for_bank(decls, 0))


def test_redirect_must_not_change_the_stack_depth():
    # PHA / [hook] PLA / RTS: a redirect from the PLA to the RTS skips a pull.
    rom = make_lorom_bank0({0x8000: bytes([0x48, 0x68, 0x60])})
    decls = [ModHookDecl(0, 0, 'H', (0x008001,), (0x8002,))]
    with pytest.raises(ValueError, match='stack depth'):
        emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                      end=0x8003, func_name='Push',
                      mod_hooks=mod_hooks_for_bank(decls, 0))


_CALLBACKS = r'''
static unsigned g_calls[5];
uint32 HFold(CpuState *cpu, uint32 pc24) {
  (void)pc24; g_calls[0]++;
  cpu->A = (uint16)((cpu->A & 0xFF00u) | 1u); cpu->_flag_Z = 0;
  return 0;
}
uint32 HRedir(CpuState *cpu, uint32 pc24) {
  g_calls[1]++;
  switch (GUEST(0x44)) {
  case 1: cpu->A = 9; return (pc24 & 0xFF0000u) | 0x9106u;  /* return-style */
  case 2: cpu->A = 7; cpu->X = 0x1234; GUEST(0x47) = 0x5A; return 0;
  case 3: cpu->A = 0x20; return (pc24 & 0xFF0000u) | 0x9104u;
  case 4: cpu->A = 9; return 0x009106u;      /* bank-$00 form from any alias */
  case 9: return 0x009108u;                   /* undeclared */
  case 10: cpu->m_flag = 0; return 0;         /* contract violation */
  }
  return 0;
}
uint32 HLoop(CpuState *cpu, uint32 pc24) {
  (void)cpu; (void)pc24;
  if (++g_calls[2] == 1) GUEST(0x40) = 1;
  return 0;
}
uint32 HRec(CpuState *cpu, uint32 pc24) {
  (void)cpu; (void)pc24; g_calls[3]++; GUEST(0x45) += 0x10; return 0;
}
uint32 HFall(CpuState *cpu, uint32 pc24) {
  (void)cpu; (void)pc24; g_calls[4]++; GUEST(0x43) *= 2; return 0;
}

static void report(const char *name, const CpuState *c) {
  CpuState flags = *c;            /* the _flag_* mirrors are authoritative */
  cpu_mirrors_to_p(&flags);
  printf("%s A=%04X X=%04X Y=%04X S=%04X P=%02X", name, c->A, c->X, c->Y,
         c->S, flags.P);
  for (int i = 0x40; i < 0x48; i++) printf(" %02X", GUEST(i));
  printf(" calls=%u,%u,%u,%u,%u\n", g_calls[0], g_calls[1], g_calls[2],
         g_calls[3], g_calls[4]);
}

/* Scenario table shared by both tiers: name, entry, bank, hook to arm (0 =
 * none), and the guest bytes $41/$44 to seed. */
typedef struct { const char *name; uint32 entry; uint32 arm; uint8 r41, r44; }
  Scenario;
static const Scenario kScenarios[] = {
  {"fold-disarmed", 0x009000, 0, 0, 0},
  {"fold-armed", 0x009000, 0x009002, 0, 0},
  {"redirect-rts", 0x009100, 0x009102, 0, 1},
  {"continue-mutated", 0x009100, 0x009102, 0, 2},
  {"redirect-mid", 0x009100, 0x009102, 0, 3},
  {"redirect-disarmed", 0x009100, 0, 0, 1},
  {"mirror-redirect", 0x809100, 0x809102, 0, 4},
  {"mirror-continue", 0x809100, 0x009102, 0, 2},
  {"loop-armed", 0x009200, 0x009200, 0, 0},
  {"loop-disarmed", 0x009200, 0, 0, 0},
  {"recursion", 0x009300, 0x009300, 2, 0},
  {"fall-through", 0x009400, 0x009404, 0, 0},
  {"fall-disarmed", 0x009400, 0, 0, 0},
};
'''

_COMPILED_STUBS = r'''
#define GUEST(a) g_ram[(a)]
#include <stdbool.h>
typedef void (*InterpPreOpcodeHook)(CpuState *cpu, uint32_t pc24);
static unsigned g_adds, g_removes;
bool interp_bridge_add_pre_opcode_hook(uint32_t pc24, InterpPreOpcodeHook h) {
  (void)pc24; (void)h; g_adds++; return true;
}
bool interp_bridge_remove_pre_opcode_hook(uint32_t pc24, InterpPreOpcodeHook h) {
  (void)pc24; (void)h; g_removes++; return true;
}
void interp_bridge_pre_opcode_redirect(uint32_t pc24) { (void)pc24; abort(); }
'''

_COMPILED_PRELUDE = r'''
/* The recursive JSR dispatches on the live widths; only M1X1 runs here. */
static RecompReturn wrong_width(CpuState *cpu) { (void)cpu; abort(); }
RecompReturn bank_00_9300_M0X0(CpuState *cpu) { return wrong_width(cpu); }
RecompReturn bank_00_9300_M0X1(CpuState *cpu) { return wrong_width(cpu); }
RecompReturn bank_00_9300_M1X0(CpuState *cpu) { return wrong_width(cpu); }
'''

_COMPILED_MAIN = r'''
typedef RecompReturn (*Body)(CpuState *);
static Body body_for(uint32 entry, int plain) {
  switch (entry) {
  case 0x009000: return plain ? Plain_Fold_M1X1 : Fold_M1X1;
  case 0x009100: return plain ? Plain_Redir_M1X1 : Redir_M1X1;
  case 0x809100: return Mirror_Redir_M1X1;
  case 0x009200: return plain ? Plain_Loop_M1X1 : Loop_M1X1;
  case 0x009300: return plain ? Plain_bank_00_9300_M1X1 : bank_00_9300_M1X1;
  case 0x009400: return plain ? Plain_Lead_M1X1 : Lead_M1X1;
  }
  abort();
}
static void prepare(CpuState *cpu, const Scenario *s, int deadline_at) {
  memset(cpu, 0, sizeof *cpu);
  memset(g_ram, 0, sizeof g_ram);
  memset(g_calls, 0, sizeof g_calls);
  cpu->ram = g_ram;
  cpu->S = 0x01FF; cpu->m_flag = 1; cpu->x_flag = 1;
  cpu->PB = (uint8)(s->entry >> 16);
  cpu_mirrors_to_p(cpu);
  GUEST(0x41) = s->r41; GUEST(0x44) = s->r44;
  g_tc_valid = 0; g_recomp_stack_top = 0;
  g_other_exits = 0; g_other_target = 0;
  g_deadline_n = 0; g_deadline_at = deadline_at;
  snes_mod_hook_disarm_all();
  if (s->arm && !snes_mod_hook_arm(s->arm, true)) abort();
  cpu_push_jsr_return_frame(cpu);
}

int main(int argc, char **argv) {
  CpuState cpu;
  if (argc > 1) {                 /* contract-violation probes */
    Scenario s = {"bad", 0x009100, 0x009102, 0, (uint8)atoi(argv[1])};
    for (int pass = 0; pass < 2; pass++) {
      prepare(&cpu, &s, 0);
      Redir_M1X1(&cpu);
      report(pass ? "bad-2" : "bad-1", &cpu);
    }
    return 0;
  }
  for (unsigned i = 0; i < sizeof kScenarios / sizeof kScenarios[0]; i++) {
    const Scenario *s = &kScenarios[i];
    prepare(&cpu, s, 0);
    body_for(s->entry, 0)(&cpu);
    if (g_other_exits) printf("%s other-exit %06X\n", s->name, g_other_target);
    report(s->name, &cpu);
    if (!s->arm && (s->entry >> 16) == 0) {      /* no directive at all */
      prepare(&cpu, s, 0);
      body_for(s->entry, 1)(&cpu);
      report(s->name, &cpu);
    }
  }
  /* Deadlines: the yield precedes the hook, so the interpreter, which
   * resumes at the hooked PC, runs it instead. */
  static const char *names[] = {"deadline-prologue", "deadline-hooked-block",
                                "deadline-back-edge"};
  for (int at = 1; at <= 3; at++) {
    const Scenario s = {names[at - 1], 0x009200, 0x009200, 0, 0};
    prepare(&cpu, &s, at);
    Loop_M1X1(&cpu);
    printf("%s yield=%06X exits=%d ", s.name, g_other_target, g_other_exits);
    report("", &cpu);
  }
  /* Arming API: aliases, registration and removal. */
  snes_mod_hook_disarm_all();
  g_adds = g_removes = 0;
  const int via_alias = snes_mod_hook_arm(0x809102, true);
  const int armed_here = snes_mod_hook_is_armed(0x009102);
  const int unknown = snes_mod_hook_arm(0x009999, true);
  printf("api %d %d %d", via_alias, armed_here, unknown);
  printf(" adds=%u", g_adds);
  snes_mod_hook_arm(0x009102, true);              /* idempotent */
  printf(" adds=%u", g_adds);
  snes_mod_hook_disarm_all();
  printf(" armed=%d removes=%u count=%u\n", snes_mod_hook_is_armed(0x809102),
         g_removes, snes_mod_hook_count());
  return 0;
}
'''


def _compiled_source():
    import test_hle_func_seam as seam
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    deadline = ('int interp_bridge_lle_master_deadline_reached(const CpuState *cpu) {\n'
                '  (void)cpu; return 0;\n}')
    assert deadline in harness
    harness = harness.replace(deadline, (
        'static int g_deadline_n, g_deadline_at;\n'
        'int interp_bridge_lle_master_deadline_reached(const CpuState *cpu) {\n'
        '  (void)cpu; return ++g_deadline_n == g_deadline_at;\n}'))
    generated = (
        ''.join(_emit(name) for *_r, name in _FUNCS)
        + ''.join(_emit(name, hooked=False, prefix='Plain_')
                  for *_r, name in _FUNCS)
        + _emit('Redir', bank=0x80, prefix='Mirror_'))
    return (harness + _COMPILED_STUBS + _COMPILED_PRELUDE
            + emit_mod_hook_table(_DECLS) + _CALLBACKS + generated
            + _COMPILED_MAIN)


def _build(tmp_path, name, sources, extra_flags=()):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    exe = tmp_path / (name + ('.exe' if os.name == 'nt' else ''))
    build = subprocess.run(
        [compiler, '-std=c11', '-O1', '-D_POSIX_C_SOURCE=200809L',
         '-I', str(_REPO / 'runner' / 'src'),
         '-I', str(_REPO / 'runner' / 'src' / 'snes'), *extra_flags,
         *map(str, sources), '-lm', '-o', str(exe)],
        capture_output=True, text=True, cwd=_REPO)
    assert build.returncode == 0, build.stdout + build.stderr
    return exe


def _run_compiled(tmp_path, *args, strict=True):
    c_file = tmp_path / 'mod_hook_compiled.c'
    c_file.write_text(_compiled_source(), encoding='utf-8')
    exe = _build(tmp_path, 'mod_hook_compiled' + ('' if strict else '_prod'),
                 [c_file, _REPO / 'runner' / 'src' / 'mod_hook.c'],
                 () if strict else ('-DSNESRECOMP_MOD_HOOK_STRICT=0',))
    return subprocess.run([str(exe), *args], capture_output=True, text=True,
                          cwd=tmp_path)


def _lines(stdout):
    rows = {}
    for line in stdout.splitlines():
        name = line.split(' ', 1)[0]
        rows.setdefault(name, []).append(line)
    return rows


# What each scenario must end with (A, guest $40-$47, hook call counts).
_EXPECT = {
    'fold-disarmed': ('A=0000', '00 01 00 00 00 00 00 00', '0,0,0,0,0'),
    'fold-armed': ('A=0001', '01 01 00 00 00 00 00 00', '1,0,0,0,0'),
    'redirect-rts': ('A=0009', '00 00 00 00 01 00 00 00', '0,1,0,0,0'),
    'continue-mutated': ('A=0007', '00 00 08 00 02 00 00 5A', '0,1,0,0,0'),
    'redirect-mid': ('A=0020', '00 00 01 00 03 00 00 00', '0,1,0,0,0'),
    'redirect-disarmed': ('A=0005', '00 00 06 00 01 00 00 00', '0,0,0,0,0'),
    'mirror-redirect': ('A=0009', '00 00 00 00 04 00 00 00', '0,1,0,0,0'),
    'mirror-continue': ('A=0007', '00 00 08 00 02 00 00 5A', '0,1,0,0,0'),
    'loop-armed': ('A=0000', '04 00 00 00 00 00 00 00', '0,0,3,0,0'),
    'loop-disarmed': ('A=0000', '04 00 00 00 00 00 00 00', '0,0,0,0,0'),
    'recursion': ('A=0000', '00 00 00 00 00 30 00 00', '0,0,0,3,0'),
    'fall-through': ('A=0005', '00 00 00 0B 00 00 00 00', '0,0,0,0,1'),
    'fall-disarmed': ('A=0005', '00 00 00 06 00 00 00 00', '0,0,0,0,0'),
}


def _check_expected(rows):
    for name, (a, ram, calls) in _EXPECT.items():
        assert name in rows, name
        for line in rows[name]:
            assert f' {a} ' in line, line
            assert 'S=01FF' in line, line
            assert f' {ram} calls={calls}' in line, line


def test_compiled_hooks_mutate_redirect_and_disarm(tmp_path):
    run = _run_compiled(tmp_path)
    assert run.returncode == 0, run.stdout + run.stderr
    rows = _lines(run.stdout)
    assert not any('other-exit' in line for line in run.stdout.splitlines())
    _check_expected(rows)
    # Disarmed equals no directive: both lines of each disarmed scenario agree.
    for name in ('fold-disarmed', 'redirect-disarmed', 'loop-disarmed',
                 'fall-disarmed'):
        assert len(rows[name]) == 2 and rows[name][0] == rows[name][1]
    # Narrow X: the callback's 0x1234 keeps the hardware zero high byte.
    assert 'X=0034' in rows['continue-mutated'][0]
    dl = {line.split(' ', 1)[0]: line for line in run.stdout.splitlines()
          if line.startswith('deadline-')}
    assert 'yield=009200 exits=1' in dl['deadline-prologue']
    assert dl['deadline-prologue'].endswith('calls=0,0,0,0,0')
    assert 'yield=009200 exits=1' in dl['deadline-hooked-block']
    assert dl['deadline-hooked-block'].endswith('calls=0,0,0,0,0')
    assert 'yield=009200 exits=1' in dl['deadline-back-edge']
    assert dl['deadline-back-edge'].endswith('calls=0,0,1,0,0')
    # The one pass done compiled saw the hook's write of $40 = 1, then INC.
    assert ' 02 00 00 00 00 00 00 00 calls' in dl['deadline-back-edge']
    api = next(line for line in run.stdout.splitlines()
               if line.startswith('api'))
    # arm via the $80 alias, armed at $00, unknown PC refused; one interpreter
    # registration per alias, none added twice, both removed on disarm.
    assert api == 'api 1 1 0 adds=2 adds=2 armed=0 removes=2 count=5'


@pytest.mark.parametrize('mode, what', [
    (9, 'returned a PC that is not a declared mod_hook_target'),
    (10, 'changed M, X or E'),
])
def test_contract_violation_traps_in_dev_and_is_corrected_in_production(
        tmp_path, mode, what):
    strict = _run_compiled(tmp_path, str(mode))
    assert strict.returncode != 0
    assert f'HRedir at $009102 broke its contract: {what}' in strict.stderr
    prod = _run_compiled(tmp_path, str(mode), strict=False)
    assert prod.returncode == 0, prod.stdout + prod.stderr
    assert prod.stderr.count('reported once') == 1     # two calls, one log
    rows = _lines(prod.stdout)
    for line in rows['bad-1'] + rows['bad-2']:
        # Undeclared return = continue; restored widths = ordinary run.
        assert ' A=0005 ' in line and 'S=01FF' in line
        assert ' 00 00 06 00 ' in line          # STA #5, INC: $42 = 6


_INTERP_MAIN = r'''
#define GUEST(a) RAM[(a)]
''' + '{CALLBACKS}' + r'''
static void load_rom(uint32 bank) {
{LOADS}
}
int main(void) {
  RAM = malloc(MEMSZ);
  g_test_cart.type = CART_LOROM;
  g_test_snes.cart = &g_test_cart;
  for (unsigned i = 0; i < sizeof kScenarios / sizeof kScenarios[0]; i++) {
    const Scenario *s = &kScenarios[i];
    memset(RAM, 0, MEMSZ);
    load_rom(0x00); load_rom(0x80);
    memset(&g_c, 0, sizeof g_c);
    memset(g_calls, 0, sizeof g_calls);
    g_c.S = 0x01FF; g_c.m_flag = 1; g_c.x_flag = 1; g_c.ram = RAM;
    g_c.PB = (uint8)(s->entry >> 16);
    cpu_mirrors_to_p(&g_c);
    GUEST(0x41) = s->r41; GUEST(0x44) = s->r44;
    snes_mod_hook_disarm_all();
    if (s->arm && !snes_mod_hook_arm(s->arm, true)) abort();
    cpu_push_jsr_return_frame(&g_c);
    int rc = interp_bridge_run(&g_c, s->entry);
    if (rc != 1) printf("%s rc=%d\n", s->name, rc);
    report(s->name, &g_c);
  }
  snes_mod_hook_disarm_all();
  return 0;
}
'''


def test_interpreter_tier_matches_compiled_tier(tmp_path):
    """The same scenarios through interp_bridge (pre-opcode hook installed by
    arming) end in exactly the state the compiled bodies reach."""
    if os.name == 'nt' and not pathlib.Path(
            'C:/msys64/mingw64/bin/gcc.exe').is_file():
        pytest.skip('MinGW GCC is not installed')
    bridge = (_REPO / 'tests' / 'interp816' / 'bridge_test.c').read_text(
        encoding='utf-8')
    prelude = bridge.split('static int g_fail = 0, g_check = 0;')[0]
    def c_bytes(code):
        return ''.join('\\x%02X' % b for b in bytes.fromhex(code))
    loads = '\n'.join(
        f'  memcpy(&RAM[(bank << 16) | 0x{pc:04X}u], "{c_bytes(code)}", '
        f'{len(bytes.fromhex(code))});'
        for pc, code in _CODE.items())
    main = (_INTERP_MAIN.replace('{CALLBACKS}', _CALLBACKS)
            .replace('{LOADS}', loads))
    c_file = tmp_path / 'mod_hook_interp.c'
    c_file.write_text(prelude + '#include "mod_hook.h"\n'
                      + emit_mod_hook_table(_DECLS) + main, encoding='utf-8')
    src = _REPO / 'runner' / 'src'
    exe = _build(tmp_path, 'mod_hook_interp', [
        c_file, src / 'snes' / 'interp816.c', src / 'snes' / 'interp_bridge.c',
        src / 'snes' / 'tier2_capture.c', src / 'sha256.c',
        src / 'snes' / 'cx4.c', src / 'mod_hook.c'],
        ('-DSNESRECOMP_TIER2_TEST=1',))
    env = dict(os.environ, SNESRECOMP_TIER2_JOURNAL=str(tmp_path / 'j.jsonl'))
    interp = subprocess.run([str(exe)], capture_output=True, text=True,
                            cwd=tmp_path, env=env)
    assert interp.returncode == 0, interp.stdout + interp.stderr
    interp_rows = _lines(interp.stdout)
    _check_expected(interp_rows)
    compiled = _run_compiled(tmp_path)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    compiled_rows = _lines(compiled.stdout)
    for name in _EXPECT:
        assert interp_rows[name][0] == compiled_rows[name][0], name


# ── Program emission ────────────────────────────────────────────────────────

def _emit_program(tmp_path, cfg_lines):
    # Root  $8000: JSR $8100 / JSR $80F0 / JML $80:8100
    # Lead  $80F0: LDA #$01 / STA $40 / falls into Sub
    # Sub   $8100: INC $40 / LDA #$00 / [hook] BEQ + / INC $41 / + RTS
    rom = bytearray(make_lorom_bank0({
        0x8000: bytes.fromhex('20 00 81 20 F0 80 5C 00 81 80'),
        0x80F0: bytes.fromhex('A9 01 85 40') + bytes([0xEA] * 12),
        0x8100: bytes.fromhex('E6 40 A9 00 F0 02 E6 41 60'),
    }))
    rom[0x7FFC:0x7FFE] = bytes([0x00, 0x80])
    (tmp_path / 'game.sfc').write_bytes(bytes(rom))
    cfg_dir = tmp_path / 'recomp'
    cfg_dir.mkdir(exist_ok=True)
    (cfg_dir / 'bank00.cfg').write_text('\n'.join([
        'bank = 00',
        'func I_RESET 8000 end:800a entry_mx:1,1',
        'func Lead 80f0 end:8109 entry_mx:1,1',
        'func Sub 8100 end:8109 entry_mx:1,1',
    ] + cfg_lines) + '\n', encoding='utf-8')
    out_dir = tmp_path / 'gen'
    result = subprocess.run([
        sys.executable, str(_REPO / 'tools' / 'v2_emit.py'),
        '--rom', str(tmp_path / 'game.sfc'), '--cfg-dir', str(cfg_dir),
        '--out-dir', str(out_dir), '--no-host-root-scan',
    ], text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return out_dir


def _hooked_blocks(out_dir, pc16):
    hooked = {}
    for path in sorted(out_dir.glob('bank*_v2*.c')):
        bank = int(path.name[4:6], 16)
        text = path.read_text(encoding='utf-8')
        for body in re.split(r'^RecompReturn ', text, flags=re.M)[1:]:
            name = body.split('(', 1)[0]
            blocks = re.split(r'^  (L_[0-9A-F]{4}_M[01]X[01]):', body,
                              flags=re.M)
            for label, block in zip(blocks[1::2], blocks[2::2]):
                if label.startswith(f'L_{pc16:04X}_'):
                    call = f'snes_mod_hook_dispatch(cpu, 0u, 0x{bank:02X}{pc16:04X}u)'
                    assert block.count(call) == 1, (path.name, name, label)
                    hooked.setdefault(bank, set()).add(name.split('_M')[0])
    return hooked


def test_program_emission_hooks_every_body_mirror_and_table(tmp_path):
    out_dir = _emit_program(tmp_path, ['mod_hook 8104 HostMod',
                                       'mod_hook_target 8104 8108'])
    hooked = _hooked_blocks(out_dir, 0x8104)
    assert {'Sub', 'Lead'} <= hooked[0x00]
    assert 0x80 in hooked                  # the JML $80:8100 mirror body
    table = (out_dir / 'mod_hooks_v2.c').read_text(encoding='utf-8')
    assert ('{ HostMod, "HostMod", {0x008104u, 0x808104u}, 2u, '
            'k_mod_hook_targets_0, 1u, 0u },') in table
    assert 'static const uint16 k_mod_hook_targets_0[] = {0x8108u};' in table
    for path in out_dir.glob('bank*_v2*.c'):
        text = path.read_text(encoding='utf-8')
        for bank in (0x00, 0x80):
            if f'0x{bank:02X}8104u)' in text:
                assert f'if (_mh == 0x{bank:02X}8108u) goto L_8108_M1X1;' in text


def test_program_without_the_directive_emits_nothing_new(tmp_path):
    out_dir = _emit_program(tmp_path, [])
    assert not (out_dir / 'mod_hooks_v2.c').exists()
    for path in out_dir.iterdir():
        if path.is_file():
            assert 'mod_hook' not in path.read_text(encoding='utf-8',
                                                    errors='replace')
    # Adding then removing the directive leaves no stale table behind.
    _emit_program(tmp_path, ['mod_hook 8104 HostMod'])
    assert (out_dir / 'mod_hooks_v2.c').exists()
    _emit_program(tmp_path, [])
    assert not (out_dir / 'mod_hooks_v2.c').exists()
