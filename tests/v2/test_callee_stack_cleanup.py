"""Execute generated returns that relocate the caller frame above arguments."""

import os
import pathlib
import shutil
import subprocess

import pytest

from _helpers import make_lorom_bank0
from v2.emit_function import emit_function


_RUNTIME = r'''
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "cpu_trace.h"
#include "common_cpu_infra.h"

uint8 g_ram[0x20000], g_memsel;
const char *g_last_recomp_func;
int g_recomp_stack_top;
uint16_t g_cpu_entry_s[64];
static int other_exits, ancestor_skip;
static uint32 other_target;
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
  (void)cpu; (void)bank; return g_ram[addr];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
  return cpu_read8(cpu, bank, addr) |
         ((uint16)cpu_read8(cpu, bank, (uint16)(addr + 1)) << 8);
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
  (void)cpu; (void)bank; g_ram[addr] = v;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
  cpu_write8(cpu, bank, addr, (uint8)v);
  cpu_write8(cpu, bank, (uint16)(addr + 1), (uint8)(v >> 8));
}
void RecompStackPush(const char *s) { (void)s; g_recomp_stack_top++; }
void RecompStackPop(void) { g_recomp_stack_top--; }
void RecompStackPopYield(void) { g_recomp_stack_top--; }
void WatchdogCheck(void) {}
void cpu_dbg_funcname(const char *s) { (void)s; }
int cpu_take_tailcall_return_context(uint16_t *s, uint8_t *h) {
  (void)s; (void)h; return 0;
}
int cpu_resolve_ancestor_skip(uint16_t s) { (void)s; return ancestor_skip; }
int interp_bridge_lle_master_deadline_reached(const CpuState *c) {
  (void)c; return 0;
}
int interp_bridge_return_targets_owner(uint16 a, uint16 b) {
  (void)a; (void)b; return 0;
}
int interp_bridge_has_direct_paired_bounce(void) { return 0; }
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc) {
  (void)cpu; (void)pc; return 0;
}
RecompReturn interp_bridge_lle_yield_unwind(CpuState *cpu, uint32 pc) {
  (void)cpu; other_exits++; other_target = pc; return 0;
}
RecompReturn interp_tier_dispatch_rewritten_return(CpuState *cpu,
    uint32 pc, uint32 site) {
  (void)cpu; (void)site; other_exits++; other_target = pc; return 0;
}
RecompReturn interp_tier_dispatch_popped_return(CpuState *cpu,
    uint32 pc, uint32 site, uint16 restore) {
  (void)site; other_exits++; other_target = pc; cpu->S = restore; return 0;
}
RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 pc,
    uint16 restore, uint32 site) {
  (void)site; other_exits++; other_target = pc; cpu->S = restore; return 0;
}

GENERATED

static int check(RecompReturn (*fn)(CpuState *), unsigned frame,
                 unsigned cleanup, uint16 entry, int skip, int rewrite) {
  CpuState cpu = {0};
  memset(g_ram, 0, sizeof g_ram);
  cpu.ram = g_ram; cpu.S = entry; cpu.host_return_valid = frame;
  cpu_write16(&cpu, 0, (uint16)(entry + 1), 0x1234);
  cpu_write16(&cpu, 0, (uint16)(entry + frame + 1), 0xabcd);
  other_exits = 0; ancestor_skip = skip;
  RecompReturn result = fn(&cpu);
  if (skip >= 0) {
    if (result != skip || other_exits) return 1;
  } else if (rewrite) {
    if (other_exits != 1 || other_target != 0x001236) return 2;
  } else {
    if (result || other_exits || cpu.S != (uint16)(entry + frame + cleanup)) {
      fprintf(stderr, "frame=%u S=%04X expected=%04X exits=%d\n", frame,
              cpu.S, (uint16)(entry + frame + cleanup), other_exits);
      return 3;
    }
  }
  return g_recomp_stack_top != 0 ? 4 : 0;
}
int main(void) {
  /* RTL moves its three-byte frame over two words; RTS moves over one word.
   * The last pair also exercises 16-bit stack wrap. */
  if (check(CleanLong_M0X0, 3, 4, 0x1fe7, -1, 0)) return 1;
  if (check(CleanShort_M0X0, 2, 2, 0x1fec, -1, 0)) return 2;
  if (check(CleanLong_M0X0, 3, 4, 0xfff9, -1, 0)) return 3;
  if (check(CleanShort_M0X0, 2, 2, 0xfffd, -1, 0)) return 4;
  /* A matching PC cannot swallow a return through a known ancestor. */
  if (check(CleanLong_M0X0, 3, 4, 0x1fe7, 2, 0)) return 5;
  if (check(CleanShort_M0X0, 2, 2, 0x1fec, 1, 0)) return 6;
  /* Callee cleanup must not hide an intentionally rewritten return PC. */
  if (check(RewriteLong_M0X0, 3, 4, 0x1fe7, -1, 1)) return 7;
  puts("ok");
  return 0;
}
'''


def test_callee_cleaned_return_preserves_architectural_stack(tmp_path):
    mingw = pathlib.Path("C:/msys64/mingw64/bin/gcc.exe")
    compiler = (os.environ.get("CC") or
                (str(mingw) if mingw.is_file() else shutil.which("cc")))
    if not compiler:
        pytest.skip("C compiler required")
    long = bytes([0xa3, 2, 0x83, 6,  # LDA 2,S; STA 6,S
                  0xa3, 1, 0x83, 5,  # LDA 1,S; STA 5,S
                  0x68, 0x68, 0x6b])  # PLA; PLA; RTL
    short = bytes([0xa3, 1, 0x83, 3, 0x68, 0x60])
    rewrite = long[:6] + bytes([0x1a]) + long[6:]  # INC return address
    rom = make_lorom_bank0({0x8000: long, 0x8100: short, 0x8200: rewrite})
    parts = [emit_function(rom, 0, start, 0, 0, end=start + len(body),
                           func_name=name)
             for start, body, name in ((0x8000, long, "CleanLong"),
                                       (0x8100, short, "CleanShort"),
                                       (0x8200, rewrite, "RewriteLong"))]
    source = tmp_path / "callee_cleanup.c"
    source.write_text(_RUNTIME.replace("GENERATED", "\n".join(parts)))
    repo = pathlib.Path(__file__).resolve().parents[2]
    exe = tmp_path / ("callee_cleanup.exe" if os.name == "nt" else "callee_cleanup")
    build = subprocess.run([compiler, "-std=c11", "-O1", "-I",
                            str(repo / "runner/src"), str(source), "-o", str(exe)],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.strip() == "ok"
