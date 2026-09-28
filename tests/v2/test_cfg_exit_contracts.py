import pytest
from v2.cfg_loader import load_bank_cfg
from v2.program_analysis import VariantKey
from test_analysis_tool import analyze, _load_cfgs
from _helpers import make_lorom_bank0
from v2.exit_mx_autoroute import detect_and_route


def test_inline_and_per_variant_exits_reach_native_manifest(tmp_path):
    cfg = tmp_path / 'bank00.cfg'
    cfg.write_text('bank = 00\nfunc Root 8000 end:8001 exit_mx:1,1\n'
                   'exit_mx_at 008000 0 0\n'
                   'exit_mx_variant 008000 1 0 0 1\n')
    parsed = load_bank_cfg(str(cfg))
    assert parsed.exit_mx_at == [(0,0x8000,1,1),(0,0x8000,0,0)]
    assert parsed.exit_mx_at_per_variant == [(0,0x8000,1,0,0,1)]
    rom = make_lorom_bank0({0x8000: b'\x60'})
    manifest, _, _ = analyze(rom, _load_cfgs(tmp_path), all_cfg_roots=True)
    assert manifest.exit_modes[VariantKey(0x8000,1,0)] == (0,1)
    assert manifest.exit_modes[VariantKey(0x8000,0,0)] == (0,0)
    # Inline contracts alone must reach the native executable, too.
    cfg.write_text('bank = 00\nfunc Root 8000 end:8001 exit_mx:1,0\n')
    manifest, _, _ = analyze(rom, _load_cfgs(tmp_path), all_cfg_roots=True)
    assert manifest.exit_modes[VariantKey(0x8000,1,1)] == (1,0)


@pytest.mark.parametrize('directive', [
    'exit_mx_variant 008000 1 0 0',
    'exit_mx_variant 008000 1 0 0 2',
    'exit_mx_variant 1000000 1 0 0 1',
    'exit_mx_variant 008000 1 0 0 1\nexit_mx_variant 008000 1 0 1 1',
])
def test_invalid_variant_contract_is_rejected(tmp_path, directive):
    cfg = tmp_path / 'bank00.cfg'
    cfg.write_text('bank = 00\n' + directive + '\n')
    with pytest.raises(ValueError):
        load_bank_cfg(str(cfg))


def test_declared_variant_survives_auto_exit_refresh(tmp_path):
    cfg = tmp_path / 'bank00.cfg'
    cfg.write_text('bank = 00\nfunc Root 8000 end:8001\n'
                   'exit_mx_variant 008000 1 0 0 1\n')
    parsed = _load_cfgs(tmp_path)
    rom = make_lorom_bank0({0x8000: b'\x60'})
    for _ in range(2):
        config = parsed[0][2]
        config.exit_mx_at_per_variant[:] = config.declared_exit_mx_at_per_variant
        detect_and_route(parsed, rom)
        assert config.exit_mx_at_per_variant == [(0,0x8000,1,0,0,1)]
