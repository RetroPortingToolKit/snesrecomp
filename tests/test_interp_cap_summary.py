import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('cap_summary',
    Path(__file__).resolve().parents[1] / 'tools/summarize_interp_caps.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_issue_80_logs_preserve_registers_and_counts():
    line = r'[interp_cap] entry=\$000000 last=C2B6DF op=E4 m=1 x=0 db=\$00 sp=0AF5 a=FF73 flag=\$00'
    result = module.summarize(line + '\n' + line + '\n[interp_cap] head: $000000/5C')
    assert result['cap_count'] == result['zero_entry_count'] == 2
    assert result['records'] == [dict(fields=dict(entry=0,last=0xc2b6df,op=0xe4,
        m=1,x=0,db=0,sp=0xaf5,a=0xff73,flag=0),count=2)]


def test_nonzero_entry_is_not_conflated_with_zero():
    result=module.summarize('[interp_cap] entry=$808000 last=$808010 op=EA m=1 x=1 sp=01FF')
    assert result['cap_count']==1 and result['zero_entry_count']==0


def test_malformed_caps_are_visible():
    result=module.summarize('[interp_cap] entry=$zz last=$8000\n[diag] PC=000000')
    assert result['cap_count']==0 and result['unparsed_cap_count']==1
    assert len(result['unparsed_examples'])==1


def test_unrelated_log_is_not_a_successful_gameplay_diagnosis():
    assert module.summarize('hello')['cap_count']==0
