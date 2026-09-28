"""Scaffold trace builds must leave hook ownership to debug_server.c."""
import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("trace", [None, 0, 1])
def test_scaffold_trace_hook_link_contract(tmp_path, trace):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("C compiler unavailable")
    host = tmp_path / "host.c"
    host.write_text((ROOT / "tools/new_project/templates/host_contract.c.in").read_text())
    peer = tmp_path / "peer.c"
    # A trace build supplies these definitions from the debug server. A
    # production build must satisfy the very same references from the host.
    declarations = """
void debug_on_block_enter(unsigned, unsigned, unsigned, unsigned);
void debug_on_wram_write_byte(unsigned, unsigned char, unsigned char);
void debug_on_wram_write_word(unsigned, unsigned short, unsigned short);
"""
    definitions = """
void debug_on_block_enter(unsigned a,unsigned b,unsigned c,unsigned d) {}
void debug_on_wram_write_byte(unsigned a,unsigned char b,unsigned char c) {}
void debug_on_wram_write_word(unsigned a,unsigned short b,unsigned short c) {}
"""
    peer.write_text(declarations + (definitions if trace else "") + """
int main(void) {
  debug_on_block_enter(0,0,0,0);
  debug_on_wram_write_byte(0,0,0);
  debug_on_wram_write_word(0,0,0);
  return 0;
}
""")
    exe = tmp_path / "contract.exe"
    flags = [] if trace is None else [f"-DSNESRECOMP_TRACE={trace}"]
    subprocess.run([cc, *flags, str(host), str(peer), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
