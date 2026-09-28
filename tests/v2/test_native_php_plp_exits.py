"""ROM-free reductions for issue #110, not a replay of its private fixture."""
import pytest
from _helpers import make_lorom_bank0
from test_analysis_tool import analyze, _load_cfgs
from v2.program_analysis import NodeDisposition, VariantKey


@pytest.mark.parametrize('m,x', [(0,0),(0,1),(1,0),(1,1)])
@pytest.mark.parametrize('body,child', [
    (b'\x08\xe2\x30\x28\x60', b'\x60'),
    (b'\x08\xc2\x30\x28\x60', b'\x60'),
    (b'\x08\xe2\x30\x20\x00\x90\x28\x60', b'\xc2\x30\x60'),
    (b'\x08\xc2\x30\x48\xda\x5a\x7a\xfa\x68\x28\x60', b'\x60'),
    (b'\x08\xe2\x30\x28\x4c\x00\x90', b'\x08\xc2\x30\x28\x60'),
])
def test_native_proves_balanced_status_wrappers(tmp_path, m, x, body, child):
    rom = make_lorom_bank0({0x8000:b'\x20\x00\x81\x60',0x8100:body,0x9000:child})
    (tmp_path/'bank00.cfg').write_text(
        f'bank = 00\nfunc I_RESET 8000 end:8004 entry_mx:{m},{x}\n'
        f'func Wrapper 8100 end:{0x8100+len(body):04x}\n'
        f'func Child 9000 end:{0x9000+len(child):04x}\n')
    manifest, _, _ = analyze(rom,_load_cfgs(tmp_path),all_cfg_roots=True)
    for pc in (0x8000,0x8100):
        key=VariantKey(pc,m,x)
        assert manifest.exit_modes.get(key)==(m,x), manifest.to_dict()
        assert manifest.nodes[key].disposition==NodeDisposition.AOT_ELIGIBLE
