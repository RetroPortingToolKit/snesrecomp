"""Exit widths around a ptrcall dispatch followed by PLP: soundness.

    8100  PHP                 ; save the caller's widths (M1X0)
          REP #$30            ; dispatch at M0X0
          LDA $80 / ASL / TAX
          PER $810C
          JMP ($8200,X)       ; indirect_dispatch ... ptrcall return:810d
    810D  PLP                 ; caller's widths back
          RTS

Whatever the handlers do, the routine returns at its entry width. Mirrors
$00:A2C5 in Metal Marines (entered M1X0, handlers entered and left at M0X0).

These tests ask only that no exit is *wrong*: a single width must be the
real one, and a set must contain it (a caller then continues through the
dynamic post-call switch). That the exit is the single saved width is #110.
"""
import pathlib
import subprocess
import sys

from _helpers import make_lorom_bank0
from test_analysis_tool import analyze, _load_cfgs
from v2.program_analysis import VariantKey

DISPATCHER = (b'\x08\xc2\x30\xa5\x80\x0a\xaa'
              b'\x62\x02\x00\x7c\x00\x82\x28\x60')
HANDLER = b'\x60'                              # RTS: leaves the widths alone
REPO = pathlib.Path(__file__).resolve().parents[2]


def _analyze(cfg_dir, rom, cfg):
    cfg_dir.mkdir(exist_ok=True)
    (cfg_dir / 'bank00.cfg').write_text(cfg)
    manifest, _, _ = analyze(rom, _load_cfgs(cfg_dir), all_cfg_roots=True)
    return manifest


def _emit(tmp_path, cfg_dir, rom):
    image = bytearray(rom)
    image[0x7FFC:0x7FFE] = b'\x00\x80'
    (tmp_path / 'r.sfc').write_bytes(bytes(image))
    subprocess.run([sys.executable, str(REPO / 'tools' / 'v2_emit.py'),
                    '--rom', str(tmp_path / 'r.sfc'), '--cfg-dir', str(cfg_dir),
                    '--out-dir', str(tmp_path / 'out'), '--cfg-roots'],
                   check=True, capture_output=True)
    return ''.join(p.read_text() for p in (tmp_path / 'out').glob('*.c'))


def _body(src, name):
    i = src.index('RecompReturn %s(CpuState *cpu) {' % name)
    return src[i:src.index('\n}\n', i)]


def _assert_sound(m, key, real):
    single, many = m.exit_modes.get(key), m.exit_mode_sets.get(key)
    assert single == real or (single is None and many and real in many), \
        (key, single, many)


def _dispatcher(tmp_path, caller):
    rom = make_lorom_bank0({0x8000: caller, 0x8100: DISPATCHER,
                            0x8200: b'\x00\x83\x00\x84',
                            0x8300: HANDLER, 0x8400: HANDLER})
    cfg = ('bank = 00\n'
           'func I_RESET 8000 end:%04x entry_mx:1,0\n' % (0x8000 + len(caller)) +
           'func Dispatcher 8100 end:%04x\n' % (0x8100 + len(DISPATCHER)) +
           'func HandlerA 8300 end:8301\nfunc HandlerB 8400 end:8401\n'
           'indirect_dispatch 810a 2 ptrcall targets:008300,008400 return:810d\n')
    return rom, cfg


def test_plp_after_the_return_restores_the_saved_width(tmp_path):
    """On e54427b the exit is a single M0X0 -- the handlers' width -- because
    the continuation key after the ptrcall starts with an empty PHP stack,
    and a PLP with nothing recorded keeps the current widths."""
    rom, cfg = _dispatcher(tmp_path, b'\x20\x00\x81\x60')    # JSR $8100 / RTS
    m = _analyze(tmp_path / 'cfg', rom, cfg)
    _assert_sound(m, VariantKey(0x8100, 1, 0), (1, 0))
    _assert_sound(m, VariantKey(0x8100, 1, 1), (1, 1))


def test_caller_is_not_compiled_at_the_wrong_width(tmp_path):
    """The consequence: a caller decodes what follows the call at the exit it
    is given. LDA #$12 / NOP / STA $10 / RTS at M1X0 is A9 12 EA 85 10 60; at
    M0X0 it is LDA #$EA12 / STA $10 (a word) / RTS -- valid code, so the
    caller compiles, and on e54427b it does, at M0X0 only."""
    rom, cfg = _dispatcher(tmp_path, b'\x20\x00\x81\xa9\x12\xea\x85\x10\x60')
    m = _analyze(tmp_path / 'cfg', rom, cfg)
    _assert_sound(m, VariantKey(0x8000, 1, 0), (1, 0))
    body = _body(_emit(tmp_path, tmp_path / 'cfg', rom), 'I_RESET_M1X0')
    assert 'L_8003_M1X0' in body, \
        'no M1X0 continuation after the call: ' + ', '.join(
            l.strip() for l in body.splitlines() if 'L_8003' in l or 'ea12' in l)


def test_continuation_after_a_handler_that_changes_width(tmp_path):
    """The continuation is decoded at the dispatch site's widths whatever the
    handlers return at -- there is no dynamic post-call switch after a
    ptrcall, as there is after a call. Dispatch at M1X0, the one handler
    returns at M0X0 (REP #$20 / RTS), and the continuation reads an
    immediate before its PLP:

        8100  PHP / SEP #$20 / LDA $80 / ASL / TAX / PER $810C / JMP ($8200,X)
        810D  LDA #$12 / NOP / STA $10 / PLP / RTS

    At run time A is 16-bit there: A9 12 EA is LDA #$EA12. On e54427b the
    dispatcher is AOT-eligible and continues at L_810D_M1X0 with LDA #$12."""
    disp = (b'\x08\xe2\x20\xa5\x80\x0a\xaa\x62\x02\x00\x7c\x00\x82'
            b'\xa9\x12\xea\x85\x10\x28\x60')
    rom = make_lorom_bank0({0x8000: b'\x20\x00\x81\x60', 0x8100: disp,
                            0x8200: b'\x00\x83\x00\x83',
                            0x8300: b'\xc2\x20\x60'})
    cfg = ('bank = 00\nfunc I_RESET 8000 end:8004 entry_mx:1,0\n'
           'func Dispatcher 8100 end:%04x\n' % (0x8100 + len(disp)) +
           'func Handler 8300 end:8303\n'
           'indirect_dispatch 810a 2 ptrcall targets:008300,008300 return:810d\n')
    m = _analyze(tmp_path / 'cfg', rom, cfg)
    assert m.nodes[VariantKey(0x8100, 1, 0)].disposition.name == 'AOT_ELIGIBLE'
    body = _body(_emit(tmp_path, tmp_path / 'cfg', rom), 'Dispatcher_M1X0')
    assert 'L_810D_M1X0' not in body and '= 0x12;' not in body, \
        'continues at the dispatch width: ' + ', '.join(
            l.strip() for l in body.splitlines()
            if 'L_810D' in l or '0x12;' in l)
    assert 'L_810D_M0X0' in body and '= 0xea12;' in body


def test_mixed_handler_exits_switch_the_continuation_on_live_widths(tmp_path):
    disp = (b'\x08\xe2\x20\xa5\x80\x0a\xaa\x62\x02\x00\x7c\x00\x82'
            b'\xa9\x12\xea\x85\x10\x28\x60')
    rom = make_lorom_bank0({0x8000: b'\x20\x00\x81\x60', 0x8100: disp,
                            0x8200: b'\x00\x83\x00\x84',
                            0x8300: b'\xc2\x20\x60', 0x8400: b'\x60'})
    cfg = ('bank = 00\nfunc I_RESET 8000 end:8004 entry_mx:1,0\n'
           'func Dispatcher 8100 end:%04x\n' % (0x8100 + len(disp)) +
           'func WideHandler 8300 end:8303\nfunc NarrowHandler 8400 end:8401\n'
           'indirect_dispatch 810a 2 ptrcall targets:008300,008400 return:810d\n')
    manifest = _analyze(tmp_path / 'cfg', rom, cfg)
    key = VariantKey(0x8100, 1, 0)
    assert manifest.nodes[key].disposition.name == 'AOT_ELIGIBLE'
    assert manifest.exit_modes[key] == (1, 0)
    body = _body(_emit(tmp_path, tmp_path / 'cfg', rom), 'Dispatcher_M1X0')
    assert 'dynamic post-call M0X0' in body and 'dynamic post-call M1X0' in body
    assert 'L_810D_M0X0' in body and 'L_810D_M1X0' in body
    assert '= 0xea12;' in body and '= 0x12;' in body
