"""Execute emitted JSL frames against the CPU's architectural stack contract."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest
from v2.codegen import _emit_return_frame_push
from v2.ir import Call
from v2.emitter_helpers import pb_save_restore_envelope


def test_jsl_frame_preserves_live_program_bank_through_rom_mirrors(tmp_path):
    compiler = os.environ.get('CC') or ('C:/msys64/mingw64/bin/gcc.exe'
        if Path('C:/msys64/mingw64/bin/gcc.exe').is_file() else shutil.which('cc'))
    if not compiler:
        pytest.skip('C compiler required')
    frame = '\n'.join(_emit_return_frame_push(Call(
        target=0x918000, long=True, source_pc24=0x0BA100)))
    source = '''
#include <assert.h>
#include <stdint.h>
typedef uint16_t uint16;
typedef struct { uint16 S; uint8_t PB, host_return_valid; } CpuState;
static uint8_t ram[65536];
static void cpu_write8(CpuState *cpu, unsigned bank, uint16 address, uint8_t value) {
  (void)cpu; assert(bank == 0); ram[address] = value;
}
static void generated(CpuState *cpu) {
''' + frame + '''
}
int main(void) {
  /* Same generated $0B:A100 body, two valid execution banks. */
  const uint8_t banks[] = {0x0b, 0x8b};
  for (unsigned i = 0; i < 2; ++i) {
    CpuState cpu = {0x1fff, banks[i], 0};
    generated(&cpu);
    assert(cpu.S == 0x1ffc && cpu.host_return_valid == 3);
    /* W65C816S JSL bus sequence: PBR, PCH, PCL; return is PC+3. */
    assert(ram[0x1fff] == banks[i]);
    assert(ram[0x1ffe] == 0xa1 && ram[0x1ffd] == 0x03);
  }
  return 0;
}
'''
    c = tmp_path / 'jsl.c'
    c.write_text(source, encoding='utf-8')
    exe = tmp_path / ('jsl.exe' if os.name == 'nt' else 'jsl')
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(c), '-o', str(exe)], check=True, capture_output=True)
    subprocess.run([str(exe)], check=True, capture_output=True)


def test_nested_jsl_unwind_preserves_live_resume_bank(tmp_path):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    inner = '\n'.join(pb_save_restore_envelope(0x82,
        ['RecompReturn _r = leaf(cpu);'], trace_pc24=0x838000))
    outer = '\n'.join(pb_save_restore_envelope(0x83,
        ['RecompReturn _r = inner(cpu);'], trace_pc24=0x8B8000))
    source = r'''
#include <stdint.h>
#include <stdio.h>
typedef uint8_t uint8;
typedef int RecompReturn;
typedef struct { uint8 PB; uint16_t S; } CpuState;
enum { RECOMP_RETURN_NORMAL=0, RECOMP_RETURN_LLE_UNWIND_BASE=0x40000000 };
#define cpu_trace_pb_change(...) ((void)0)
#define cpu_trace_event(...) ((void)0)
#define cpu_trace_mark_nlr_exit(...) ((void)0)
static int pending, mode;
int interp_bridge_lle_unwind_pending(void) { return pending; }
static RecompReturn leaf(CpuState *cpu) {
  if (cpu->PB != 0x82) return -100;
  if (mode >= 2) { if (mode == 3) cpu->PB = 0x91; pending=1; return RECOMP_RETURN_LLE_UNWIND_BASE; }
  return mode == 1 ? 2 : RECOMP_RETURN_NORMAL;
}
static RecompReturn inner(CpuState *cpu) {
''' + inner + r'''
  return RECOMP_RETURN_NORMAL;
}
static RecompReturn outer(CpuState *cpu) {
''' + outer + r'''
  return RECOMP_RETURN_NORMAL;
}
int main(void) {
  for (mode=0; mode<4; mode++) {
    CpuState cpu = {0x8b, 0x01f4}; pending=0;
    RecompReturn result=outer(&cpu);
    if (mode < 2) {
      if (result != 0 || cpu.PB != 0x8b || cpu.S != 0x01f4) return 1;
    } else {
      if (result != RECOMP_RETURN_LLE_UNWIND_BASE-2 ||
          cpu.PB != (mode == 2 ? 0x82 : 0x91) || cpu.S != 0x01f4 || !pending) {
        fprintf(stderr, "unwind restored ancestor PB: PB=%02x return=%08x\n", cpu.PB, result);
        return 2;
      }
    }
  }
  puts("normal and SKIP restore caller PB; nested LLE unwind preserves resume PB");
  return 0;
}
'''
    c_file = tmp_path / 'jsl_unwind.c'
    c_file.write_text(source)
    exe = tmp_path / ('jsl_unwind.exe' if os.name == 'nt' else 'jsl_unwind')
    build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(c_file), '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr


def test_pending_api_links_to_production_bridge_unwind_state(tmp_path):
    """Compile the actual arm/getter with the public declaration in another TU."""
    import re
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    repo = Path(__file__).resolve().parents[2]
    bridge = (repo / 'runner/src/snes/interp_bridge.c').read_text()
    arm = re.search(r'(?ms)^static void lle_unwind_arm\(.*?^\}', bridge).group()
    getter = re.search(r'(?m)^int interp_bridge_lle_unwind_pending\(void\) \{[^\n]*\}', bridge).group()
    api = tmp_path / 'bridge_api.c'
    api.write_text(r'''
#include "cpu_state.h"
static int s_lle_unwind_active, s_lle_unwind_owner_depth, s_lle_unwind_is_deadline;
static uint32 s_lle_unwind_pc24;
enum { INTERP_RESUME_KIND_UNWIND_ARM = 3 };
static void resume_ring_note(int site, int kind, uint32 old, uint32 pc, uint16 sp) {
  (void)site; (void)kind; (void)old; (void)pc; (void)sp;
}
''' + arm + '\n' + getter + r'''
void fixture_arm(void) { lle_unwind_arm(0x918123, 3, 1, 0, 0x1fe4); }
int fixture_context(void) {
  return s_lle_unwind_pc24 == 0x918123 && s_lle_unwind_owner_depth == 3 &&
         s_lle_unwind_is_deadline == 1;
}
void fixture_consume(void) { s_lle_unwind_active = 0; }
''')
    main = tmp_path / 'bridge_main.c'
    main.write_text(r'''
#include "cpu_state.h"
void fixture_arm(void);
int fixture_context(void);
void fixture_consume(void);
int main(void) {
  if (interp_bridge_lle_unwind_pending()) return 1;
  fixture_arm();
  if (!interp_bridge_lle_unwind_pending() || !fixture_context()) return 2;
  fixture_consume();
  return interp_bridge_lle_unwind_pending() ? 3 : 0;
}
''')
    exe = tmp_path / ('bridge_api.exe' if os.name == 'nt' else 'bridge_api')
    build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(repo / 'runner/src'), str(api), str(main),
                            '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    executed = subprocess.run([str(exe)], capture_output=True, text=True)
    assert executed.returncode == 0, executed.stdout + executed.stderr
