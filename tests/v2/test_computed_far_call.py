"""Execute generated stack-programmed calls with a preserved continuation."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

from _helpers import make_lorom_bank0
from test_callee_stack_cleanup import _RUNTIME
from v2.emit_function import emit_function


@pytest.mark.parametrize("far", [False, True])
def test_direct_bounce_preserves_synthetic_call_continuation(tmp_path, far):
    mingw = Path("C:/msys64/mingw64/bin/gcc.exe")
    compiler = os.environ.get("CC") or (str(mingw) if mingw.is_file() else shutil.which("cc"))
    if not compiler:
        pytest.skip("C compiler required")
    # Long: PHK/PEA creates a continuation, PHX/PHX/PLB/PHA builds
    # the far target. Short: two PEAs create continuation and target.
    body = (bytes.fromhex("4bf47f80dadaaba9009048e2206b") if far else
            bytes.fromhex("f47f80f4009060"))
    generated = emit_function(make_lorom_bank0({0x8000: body}), 0, 0x8000,
                              0, 1, end=0x8000 + len(body), func_name="ComputedCall")
    runtime = _RUNTIME.split("static int check(", 1)[0]
    runtime = runtime.replace("int interp_bridge_has_direct_paired_bounce(void) { return 0; }",
                              "int interp_bridge_has_direct_paired_bounce(void) { return 1; }")
    runtime = runtime.replace("GENERATED", generated)
    runtime += r'''
int main(void) {
  const uint16 entries[] = {0x01fd, 0x0001};
  for (unsigned i = 0; i < 2; ++i) {
    CpuState cpu = {0};
    memset(g_ram, 0, sizeof g_ram);
    cpu.ram = g_ram; cpu.S = entries[i]; cpu.X = 1;
    cpu.host_return_valid = 2; cpu.x_flag = 1; cpu.P = 0x10;
    cpu_write16(&cpu, 0, (uint16)(cpu.S + 1), 0x8123);
    other_exits = 0; ancestor_skip = -1;
    ComputedCall_M0X1(&cpu);
    if (other_exits != 1 || other_target != TARGET ||
        cpu.S != (uint16)(entries[i] - LOCALS) || g_recomp_stack_top != 0)
      return 1;
    if (cpu_read16(&cpu, 0, (uint16)(cpu.S + 1)) != 0x807f)
      return 2; /* continuation must still be on the guest stack */
    if (cpu_read16(&cpu, 0, (uint16)(entries[i] + 1)) != 0x8123)
      return 3; /* original interpreted caller is still above the continuation */
  }
  return 0;
}
'''.replace("TARGET", "0x019001" if far else "0x009001").replace("LOCALS", "3" if far else "2")
    source = tmp_path / "computed_call.c"
    source.write_text(runtime)
    exe = tmp_path / ("computed_call.exe" if os.name == "nt" else "computed_call")
    repo = Path(__file__).resolve().parents[2]
    build = subprocess.run([compiler, "-std=c11", "-O1", "-I", str(repo / "runner/src"),
                            str(source), "-o", str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
