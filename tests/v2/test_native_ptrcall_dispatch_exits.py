"""ROM-free reductions of the shapes behind #110's remaining losses.

Each mirrors a dispatcher in Metal Marines with synthetic bytes: a status
save, a `PER return-1 / JMP (table,X)` call into one of several handlers
declared with `indirect_dispatch ... ptrcall`, and a `PLP` after the return
that restores the caller's widths whatever the handler left. The dispatcher
therefore returns at its entry width (here M1X0) in every case.
"""
import pytest
from _helpers import make_lorom_bank0
from test_analysis_tool import analyze, _load_cfgs
from v2.program_analysis import NodeDisposition, VariantKey

# 8000  JSR $8100 / RTS
CALLER = b'\x20\x00\x81\x60'
# 8100  PHP / REP #$30 / LDA $80 / ASL / TAX / SEP #$20 /
# 8109  PER $810E / JMP ($8200,X) / 810F PLP / RTS
DISPATCHER = (b'\x08\xc2\x30\xa5\x80\x0a\xaa\xe2\x20'
              b'\x62\x02\x00\x7c\x00\x82\x28\x60')
DISPATCH_CFG = 'indirect_dispatch 810c 2 ptrcall targets:{targets} return:810f\n'
REP30 = b'\xc2\x30\x60'                 # REP #$30 / RTS: returns M0X0
SEP30 = b'\xe2\x30\x60'                 # SEP #$30 / RTS: returns M1X1


def _run(tmp_path, chunks, funcs, targets):
    rom = make_lorom_bank0(chunks)
    cfg = 'bank = 00\nfunc I_RESET 8000 end:8004 entry_mx:1,0\n'
    cfg += 'func Dispatcher 8100 end:%04x\n' % (0x8100 + len(DISPATCHER))
    for name, pc, size in funcs:
        cfg += 'func %s %04x end:%04x\n' % (name, pc, pc + size)
    cfg += DISPATCH_CFG.format(targets=','.join('00%04x' % t for t in targets))
    (tmp_path / 'bank00.cfg').write_text(cfg)
    manifest, _, _ = analyze(rom, _load_cfgs(tmp_path), all_cfg_roots=True)
    return manifest


def _assert_proven(manifest):
    for pc in (0x8100, 0x8000):
        key = VariantKey(pc, 1, 0)
        assert manifest.nodes[key].disposition == NodeDisposition.AOT_ELIGIBLE, \
            (key, manifest.nodes[key].reasons)
        assert manifest.exit_modes.get(key) == (1, 0), \
            (key, manifest.exit_modes.get(key), manifest.exit_mode_sets.get(key))


@pytest.mark.parametrize('a,b', [(REP30, REP30), (SEP30, SEP30), (REP30, SEP30)],
                         ids=['both-M0X0', 'both-M1X1', 'disagree'])
def test_ptrcall_handlers_that_change_width_behind_a_plp(tmp_path, a, b):
    """$00:8665 ScreenMode_Dispatch. Fails on a980fe3: the dispatcher's exit
    is the set {handler exit, saved width} instead of the saved width, even
    when both handlers agree. Handlers that leave the widths alone pass."""
    m = _run(tmp_path,
             {0x8000: CALLER, 0x8100: DISPATCHER, 0x8200: b'\x00\x83\x00\x84',
              0x8300: a, 0x8400: b},
             [('HandlerA', 0x8300, len(a)), ('HandlerB', 0x8400, len(b))],
             [0x8300, 0x8400])
    _assert_proven(m)


def test_control_ptrcall_handler_that_reenters_the_caller(tmp_path):
    """Control, passes on a980fe3: one handler is a loop that never returns
    and calls the dispatcher again (Metal Marines' mode 25 jumps into the
    middle of its main loop at $00:80E6). The cycle alone is not the loss."""
    loop = b'\x08\x20\x00\x81\x28\x80\xf9'   # PHP / JSR $8100 / PLP / BRA $8400
    m = _run(tmp_path,
             {0x8000: CALLER, 0x8100: DISPATCHER, 0x8200: b'\x00\x83\x00\x84',
              0x8300: b'\x60', 0x8400: loop},
             [('HandlerA', 0x8300, 1), ('MainLoopTail', 0x8400, len(loop))],
             [0x8300, 0x8400])
    _assert_proven(m)


@pytest.mark.parametrize('declare_target', [True, False])
def test_ptrcall_target_known_only_through_the_table(tmp_path, declare_target):
    """$00:B74A Mode07_SubModeDispatch. With the handler declared as a func
    this passes; known only as a dispatch target it fails on a980fe3 -- the
    dispatcher stays lle_only with unproven_call_at_008503_to_008600_m0x0
    although the leaf's M0X0 variant exists and is proven (with or without
    an entry_mx on the leaf)."""
    c = b'\x08\xc2\x30\x20\x00\x86\x28\x60'              # PHP/REP #$30/JSR $8600/PLP/RTS
    leaf = b'\x08\xe2\x20\xa9\x0a\x8d\x43\x21\x28\x60'   # PHP/SEP #$20/LDA #$0A/STA $2143/PLP/RTS
    funcs = [('HandlerA', 0x8300, 1), ('Leaf', 0x8600, len(leaf))]
    if declare_target:
        funcs.append(('HandlerC', 0x8500, len(c)))
    m = _run(tmp_path,
             {0x8000: CALLER, 0x8100: DISPATCHER, 0x8200: b'\x00\x83\x00\x85',
              0x8300: b'\x60', 0x8500: c, 0x8600: leaf},
             funcs, [0x8300, 0x8500])
    _assert_proven(m)
