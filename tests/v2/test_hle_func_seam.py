"""`hle_func` is the seam for reimplementing a recompiled routine in C.

Pins the contract documented in docs/HLE_FUNC.md (issue #133):

* every guest control transfer that can reach an hle_func'd PC through
  generated code reaches the forwarding stub, never the replaced ROM bytes
  (direct JSR/JSL, JMP/JML/BRL tail transfer, cfg dispatch tables, ptrcall
  and ptrtail, fall-through, a caller whose end: range covers the PC, and a
  PEI;RTS local computed goto);
* the native analyzer models the same boundary, so a caller's manifest node
  never contains the replaced bytes;
* the stub exists for all four M/X variants and owns a dispatch row;
* the stub itself neither pops the return frame nor charges cycles: the
  helper owns the architectural RTS/RTL stack effect. That ownership is
  executed here end to end against real generated callers.
"""
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

import pytest

from _helpers import make_lorom_bank0  # noqa: E402

from v2 import codegen
from v2.emit_function import emit_function


REPO = pathlib.Path(__file__).resolve().parents[2]


# ── Emission: every call shape reaches the stub ───────────────────────────

def _shapes_rom() -> bytes:
    rom = bytearray([0xFF] * 0x8000)

    def put(pc, data):
        rom[pc - 0x8000:pc - 0x8000 + len(data)] = bytes(data)

    callers = [0x8100, 0x8200, 0x8300, 0x8400, 0x8500, 0x8600, 0x8700,
               0x8800, 0x8900, 0x8A00, 0x8FFC]
    root = []
    for pc in callers:
        root += [0x20, pc & 0xFF, pc >> 8]          # JSR caller
    put(0x8000, root + [0x60])
    put(0x8100, [0x20, 0x00, 0x90, 0x60])           # JSR $9000 ; RTS
    put(0x8200, [0x4C, 0x00, 0x90])                 # JMP $9000
    put(0x8300, [0xA2, 0x00, 0xFC, 0x00, 0xA0, 0x60])  # JSR ($A000,X) table
    put(0xA000, [0x00, 0x90, 0x00, 0x90])
    put(0x8400, [0x22, 0x00, 0x91, 0x00, 0x60])     # JSL $00:9100 ; RTS
    put(0x8500, [0xA2, 0x00, 0xFC, 0x10, 0x00, 0x60])  # JSR ($0010,X) ptrcall
    put(0x8600, [0x6C, 0x10, 0x00])                 # JMP ($0010) ptrtail
    put(0x8700, [0x5C, 0x00, 0x90, 0x80])           # JML $80:9000 (mirror)
    put(0x8800, [0xA9, 0x00, 0xF0, 0x01, 0x60,      # LDA #0; BEQ +1; RTS
                 0x82, 0xF8, 0x07])                 # BRL $9000
    put(0x8900, [0x82, 0xFD, 0x06])                 # BRL $9000, inside end:
    put(0x8A00, [0xD4, 0x10, 0x60])                 # PEI ($10) ; RTS
    put(0x8FFC, [0xEA, 0xEA, 0xEA, 0xEA])           # falls into $9000
    put(0x9000, [0xA9, 0x01, 0x60])                 # replaced RTS routine
    put(0x9100, [0xA9, 0x02, 0x6B])                 # replaced RTL routine
    put(0x9201, [0xA9, 0x03, 0x60])                 # ordinary local runway
    rom[0x7FFC:0x7FFE] = bytes([0x00, 0x80])
    rom[0x7FEA:0x7FEC] = bytes([0xFF, 0xFF])
    rom[0x7FEE:0x7FF0] = bytes([0xFF, 0xFF])
    return bytes(rom)


_CALLERS = {
    "CallerJsr": 0x8100, "CallerJmp": 0x8200, "CallerTable": 0x8300,
    "CallerJsl": 0x8400, "CallerPtrcall": 0x8500, "CallerPtrtail": 0x8600,
    "CallerJml": 0x8700, "CallerBrl": 0x8800, "WideRange": 0x8900,
    "StackDispatch": 0x8A00, "FallInto": 0x8FFC,
}


def _emit_shapes(tmp_path, declare_func):
    (tmp_path / "game.sfc").write_bytes(_shapes_rom())
    cfg_dir = tmp_path / "recomp"
    cfg_dir.mkdir()
    lines = [
        "bank = 00",
        "func I_RESET 8000 end:8022 entry_mx:1,1",
        "func CallerJsr 8100 end:8104 entry_mx:1,1",
        "func CallerJmp 8200 end:8203 entry_mx:1,1",
        "func CallerTable 8300 end:8306 entry_mx:1,1",
        "func CallerJsl 8400 end:8405 entry_mx:1,1",
        "func CallerPtrcall 8500 end:8506 entry_mx:1,1",
        "func CallerPtrtail 8600 end:8603 entry_mx:1,1",
        "func CallerJml 8700 end:8704 entry_mx:1,1",
        "func CallerBrl 8800 end:8808 entry_mx:1,1",
        # Explicit range covering the replaced routine.
        "func WideRange 8900 end:9100 entry_mx:1,1",
        "func StackDispatch 8a00 end:8a03 entry_mx:1,1",
        # No end: the decoder follows the fall-through edge.
        "func FallInto 8ffc entry_mx:1,1",
        "indirect_dispatch 8302 2 idx:X tables:a000",
        "indirect_dispatch 8502 1 ptrcall targets:9000",
        "indirect_dispatch 8600 1 ptrtail targets:9000",
        "indirect_dispatch 8a00 2 rtsstack targets:9000,9201",
        "hle_func 9000 HleShort",
        "hle_func 9100 HleLong",
    ]
    if declare_func:
        lines += ["func Short 9000 end:9003 entry_mx:1,1",
                  "func Long 9100 end:9103 entry_mx:1,1"]
    (cfg_dir / "bank00.cfg").write_text("\n".join(lines) + "\n",
                                        encoding="utf-8")
    out_dir = tmp_path / "gen"
    result = subprocess.run([
        sys.executable, str(REPO / "tools" / "v2_emit.py"),
        "--rom", str(tmp_path / "game.sfc"), "--cfg-dir", str(cfg_dir),
        "--out-dir", str(out_dir), "--no-host-root-scan",
    ], text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return out_dir


def _function_body(source, name):
    match = re.search(
        r"^RecompReturn " + re.escape(name) + r"\(CpuState \*cpu\) \{\n"
        r"(.*?)^\}\n", source, re.S | re.M)
    assert match, f"{name} not emitted"
    return match.group(1)


@pytest.mark.parametrize("declare_func", [True, False],
                         ids=["with_func_line", "hle_func_only"])
def test_every_generated_transfer_reaches_the_hle_stub(tmp_path, declare_func):
    out_dir = _emit_shapes(tmp_path, declare_func)
    bank00 = (out_dir / "bank00_v2.c").read_text(encoding="utf-8")
    bank80 = (out_dir / "bank80_v2.c").read_text(encoding="utf-8")
    dispatch = (out_dir / "dispatch_v2.c").read_text(encoding="utf-8")
    short = "Short" if declare_func else "bank_00_9000"
    long_ = "Long" if declare_func else "bank_00_9100"

    # The replaced ROM bytes are compiled nowhere: no body carries a label
    # for either replaced routine, and their RTS/RTL sites never appear.
    assert not re.search(r"^\s*L_9000_M[01]X[01]:", bank00, re.M)
    assert not re.search(r"^\s*L_9100_M[01]X[01]:", bank00, re.M)
    assert "0x009002u" not in bank00 and "0x009102u" not in bank00

    expected = {
        "CallerJsr": short, "CallerJmp": short, "CallerTable": short,
        "CallerJsl": long_, "CallerPtrcall": short, "CallerPtrtail": short,
        "CallerJml": "bank_80_9000", "CallerBrl": short,
        "WideRange": short, "StackDispatch": short, "FallInto": short,
    }
    for caller, stub in expected.items():
        body = _function_body(bank00, f"{caller}_M1X1")
        assert re.search(rf"\b{stub}_M1X1\(cpu\)", body), (caller, body)

    # Tail-shaped arrivals hand the caller's return context to the stub.
    for caller in ("CallerJmp", "CallerBrl", "WideRange", "FallInto",
                   "StackDispatch", "CallerPtrtail", "CallerJml"):
        body = _function_body(bank00, f"{caller}_M1X1")
        assert "cpu_tailcall_inherit_return_context(_entry_s, _hrv);" in body
    # Call-shaped arrivals push the hardware frame the helper must pop.
    jsr = _function_body(bank00, "CallerJsr_M1X1")
    assert "cpu->host_return_valid = 2;  /* paired host caller, JSR frame */" in jsr
    jsl = _function_body(bank00, "CallerJsl_M1X1")
    assert "cpu->host_return_valid = 3;  /* paired host caller, JSL frame */" in jsl

    # All four M/X variants are stubs, in both LoROM mirrors, each with a
    # complete dispatch row so runtime lookups also land on the helper.
    for source, base, helper in ((bank00, short, "HleShort"),
                                 (bank00, long_, "HleLong"),
                                 (bank80, "bank_80_9000", "HleShort")):
        for m in (0, 1):
            for x in (0, 1):
                body = _function_body(source, f"{base}_M{m}X{x}")
                assert f"RecompReturn _r = {helper}(cpu);" in body
                assert "cpu->S" not in body          # no frame pop
                assert "cycles" not in body          # no cycle charge
                assert "cpu_take_tailcall_return_context(NULL, NULL);" in body
        slots = ", ".join(f"{base}_M{m}X{x}" for m in (0, 1) for x in (0, 1))
        assert slots in dispatch


@pytest.mark.parametrize("declare_func", [True, False],
                         ids=["with_func_line", "hle_func_only"])
def test_analyzer_models_hle_pc_as_a_boundary(tmp_path, declare_func):
    """The manifest must agree with emission: a caller node never contains
    the replaced bytes, and every arrival is a demand edge onto the HLE PC."""
    out_dir = _emit_shapes(tmp_path, declare_func)
    manifest = json.loads(
        (out_dir / "program_manifest.json").read_text(encoding="utf-8"))
    nodes = manifest["nodes"]
    for caller, pc in _CALLERS.items():
        node = nodes[f"{pc:06X}:M1X1"]
        assert node["disposition"] == "aot_eligible", (caller, node)
        assert not (0x9000 <= node["max_pc24"] < 0x9200), (caller, node)
    for caller in ("CallerJmp", "CallerBrl", "WideRange", "FallInto",
                   "StackDispatch"):
        demands = nodes[f"{_CALLERS[caller]:06X}:M1X1"]["demands"]
        assert any(d.get("target") and d["target"]["pc24"] == 0x009000
                   and d["kind"] == "direct_tail_call"
                   for d in demands), (caller, demands)


# ── Execution: the helper owns the return frame ────────────────────────────

def _compiler():
    cc = os.environ.get("CC")
    if cc:
        return cc
    mingw = pathlib.Path("C:/msys64/mingw64/bin/gcc.exe")
    if mingw.is_file():
        return str(mingw)
    return shutil.which("cc") or shutil.which("gcc")


_HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "cpu_trace.h"
#include "common_cpu_infra.h"

uint8 g_ram[0x20000];
uint8 g_memsel;
const char *g_last_recomp_func = "";
int g_recomp_stack_top;
uint16_t g_cpu_entry_s[64];

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
  (void)cpu; (void)bank; return g_ram[addr];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
  return (uint16)(cpu_read8(cpu, bank, addr) |
                  (cpu_read8(cpu, bank, (uint16)(addr + 1)) << 8));
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
  (void)cpu; (void)bank; g_ram[addr] = v;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
  cpu_write8(cpu, bank, addr, (uint8)v);
  cpu_write8(cpu, bank, (uint16)(addr + 1), (uint8)(v >> 8));
}

/* Minimal recomp-stack / tail-context runtime with production semantics. */
static int g_tc_valid; static uint16 g_tc_s; static uint8 g_tc_hrv;
void RecompStackPush(const char *name) { (void)name; g_recomp_stack_top++; }
void RecompStackPop(void) { g_recomp_stack_top--; }
void RecompStackPopYield(void) { g_recomp_stack_top--; }
void WatchdogCheck(void) {}
void cpu_dbg_funcname(const char *name) { (void)name; }
void cpu_tailcall_inherit_return_context(uint16_t s, uint8_t hrv) {
  g_tc_s = s; g_tc_hrv = hrv; g_tc_valid = 1;
}
int cpu_take_tailcall_return_context(uint16_t *s, uint8_t *hrv) {
  if (!g_tc_valid) return 0;
  if (s) *s = g_tc_s;
  if (hrv) *hrv = g_tc_hrv;
  g_tc_valid = 0;
  return 1;
}
int cpu_resolve_ancestor_skip(uint16_t ret_s) { (void)ret_s; return -1; }
int interp_bridge_lle_master_deadline_reached(const CpuState *cpu) {
  (void)cpu; return 0;
}
int interp_bridge_return_targets_owner(uint16 a, uint16 b) {
  (void)a; (void)b; return 0;
}
int interp_bridge_has_direct_paired_bounce(void) { return 0; }
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) {
  (void)cpu; (void)pc24; return 0;
}

/* Every non-host-return exit is recorded: the contract under test is that
 * none of them is taken when the helper honours the return frame. */
static int g_other_exits; static uint32 g_other_target;
RecompReturn interp_bridge_lle_yield_unwind(CpuState *cpu, uint32 pc24) {
  (void)cpu; g_other_exits++; g_other_target = pc24; return 0;
}
RecompReturn interp_tier_dispatch_rewritten_return(CpuState *cpu,
    uint32 pc24, uint32 site) {
  (void)cpu; (void)site; g_other_exits++; g_other_target = pc24; return 0;
}
RecompReturn interp_tier_dispatch_popped_return(CpuState *cpu, uint32 pc24,
    uint32 site, uint16 restore) {
  (void)site; g_other_exits++; g_other_target = pc24; cpu->S = restore;
  return 0;
}
RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 pc24, uint16 restore,
    uint32 site) {
  (void)site; g_other_exits++; g_other_target = pc24; cpu->S = restore;
  return 0;
}
RecompReturn interp_tier_run_call_frame(CpuState *cpu, uint32 pc24,
    uint32 site, uint8 frame, uint32 *ret) {
  (void)cpu; (void)site; (void)frame; (void)ret;
  g_other_exits++; g_other_target = pc24; return 0;
}
RecompReturn interp_tier_dispatch_tail(CpuState *cpu, uint32 pc24,
    uint32 site, uint16 entry_s, uint8 hrv) {
  (void)cpu; (void)site; (void)entry_s; (void)hrv;
  g_other_exits++; g_other_target = pc24; return 0;
}

/* The C helpers. mode 0 honours the contract (pop the RTS/RTL frame),
 * mode 1 forgets the pop, mode 2 returns to the caller's caller. */
static int g_mode, g_helper_calls;
static uint16 g_helper_entry_s; static uint8 g_helper_entry_hrv;
static RecompReturn helper(CpuState *cpu, unsigned frame) {
  g_helper_calls++;
  g_helper_entry_s = cpu->S;
  g_helper_entry_hrv = cpu->host_return_valid;
  if (g_mode == 1) return RECOMP_RETURN_NORMAL;
  if (g_mode == 2) {                    /* PLA;PLA;RTS-style exit */
    cpu->S = (uint16)(cpu->S + frame + 2);
    return RECOMP_RETURN_SKIP_1;
  }
  cpu->S = (uint16)(cpu->S + frame);
  return RECOMP_RETURN_NORMAL;
}
RecompReturn HleShort(CpuState *cpu) { return helper(cpu, 2); }
RecompReturn HleLong(CpuState *cpu) { return helper(cpu, 3); }

GENERATED

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d %s FAILED: %s\n", \
    __FILE__, __LINE__, scenario, #c); fails++; } } while (0)

static RecompReturn run(CpuState *cpu, RecompReturn (*fn)(CpuState *),
                        int mode) {
  memset(cpu, 0, sizeof *cpu);
  memset(g_ram, 0, sizeof g_ram);
  cpu->ram = g_ram;
  cpu->S = 0x01FF; cpu->m_flag = 1; cpu->x_flag = 1; cpu->P = 0x30;
  g_mode = mode; g_helper_calls = 0; g_other_exits = 0; g_other_target = 0;
  g_tc_valid = 0; g_recomp_stack_top = 0;
  cpu_push_jsr_return_frame(cpu);       /* host calls the caller like a JSR */
  return fn(cpu);
}

int main(void) {
  CpuState cpu; RecompReturn r; const char *scenario;

  scenario = "JSR, helper pops 2";
  r = run(&cpu, CallerJsr_M1X1, 0);
  CHECK(r == RECOMP_RETURN_NORMAL);
  CHECK(g_helper_calls == 1);
  CHECK(g_helper_entry_hrv == 2);
  CHECK(g_helper_entry_s == 0x01FB);     /* host frame + JSR frame */
  CHECK(g_ram[0x40] == 0x55);            /* continuation after the JSR ran */
  CHECK(g_other_exits == 0);             /* caller's RTS host-returned */
  CHECK(cpu.S == 0x01FF);
  CHECK(g_recomp_stack_top == 0);

  scenario = "JSL, helper pops 3";
  r = run(&cpu, CallerJsl_M1X1, 0);
  CHECK(r == RECOMP_RETURN_NORMAL);
  CHECK(g_helper_calls == 1);
  CHECK(g_helper_entry_hrv == 3);
  CHECK(g_helper_entry_s == 0x01FA);
  CHECK(g_ram[0x41] == 0x66);
  CHECK(g_other_exits == 0);
  CHECK(cpu.S == 0x01FF);
  CHECK(cpu.PB == 0x00);

  scenario = "JMP tail, helper pops the inherited frame";
  r = run(&cpu, CallerJmp_M1X1, 0);
  CHECK(r == RECOMP_RETURN_NORMAL);
  CHECK(g_helper_calls == 1);
  CHECK(g_helper_entry_hrv == 2);        /* the jumper's own hrv */
  CHECK(g_helper_entry_s == 0x01FD);     /* no frame pushed by a JMP */
  CHECK(g_other_exits == 0);
  CHECK(cpu.S == 0x01FF);
  CHECK(!g_tc_valid);                    /* stub consumed the tail context */

  scenario = "SKIP_1 return to the caller's caller";
  r = run(&cpu, CallerJsr_M1X1, 2);
  CHECK(r == RECOMP_RETURN_NORMAL);      /* caller decremented SKIP_1 */
  CHECK(g_ram[0x40] == 0x00);            /* continuation skipped */
  CHECK(g_other_exits == 0);
  CHECK(cpu.S == 0x01FF);

  scenario = "contract violated: helper leaves the JSR frame";
  r = run(&cpu, CallerJsr_M1X1, 1);
  CHECK(g_helper_calls == 1);
  /* The caller's RTS now pops its own stale JSR frame instead of the host
   * frame: it is treated as a computed jump back to $8003 and leaves the
   * host-return path. On hardware this would double-execute the caller's
   * continuation; it is the failure the helper contract prevents. */
  CHECK(g_other_exits == 1);
  CHECK(g_other_target == 0x008003);

  if (fails) return 1;
  puts("ok");
  return 0;
}
'''


def test_helper_owns_the_return_frame_end_to_end(tmp_path):
    compiler = _compiler()
    if not compiler:
        pytest.skip("C compiler required")
    rom = make_lorom_bank0({
        # CallerJsr: JSR $9000 ; LDA #$55 ; STA $40 ; RTS
        0x8000: bytes([0x20, 0x00, 0x90, 0xA9, 0x55, 0x85, 0x40, 0x60]),
        # CallerJmp: JMP $9000
        0x8100: bytes([0x4C, 0x00, 0x90]),
        # CallerJsl: JSL $00:9100 ; LDA #$66 ; STA $41 ; RTS
        0x8200: bytes([0x22, 0x00, 0x91, 0x00, 0xA9, 0x66, 0x85, 0x41,
                       0x60]),
        0x9000: bytes([0x60]),   # replaced RTS routine
        0x9100: bytes([0x6B]),   # replaced RTL routine
    })
    names = {0x008000: "CallerJsr", 0x008100: "CallerJmp",
             0x008200: "CallerJsl", 0x009000: "Short", 0x009100: "Long"}
    all_mx = frozenset({(0, 0), (0, 1), (1, 0), (1, 1)})
    hle = {0x9000: "HleShort", 0x9100: "HleLong"}
    saved = (codegen._VALID_VARIANTS, codegen._VALID_VARIANTS_AUTHORITATIVE,
             dict(codegen._NAME_RESOLVER))
    try:
        codegen.set_name_resolver(names)
        codegen.set_valid_variants({
            0x008000: frozenset({(1, 1)}), 0x008100: frozenset({(1, 1)}),
            0x008200: frozenset({(1, 1)}), 0x009000: all_mx,
            0x009100: all_mx}, authoritative=True)
        siblings = {0x8000, 0x8100, 0x8200, 0x9000, 0x9100}
        parts = []
        for start, end, name in ((0x8000, 0x8008, "CallerJsr"),
                                 (0x8100, 0x8103, "CallerJmp"),
                                 (0x8200, 0x8209, "CallerJsl")):
            parts.append(emit_function(
                rom, 0, start, 1, 1, end=end, func_name=name, hle_func=hle,
                sibling_entry_pcs=siblings - {start}))
        for pc, name in ((0x9000, "Short"), (0x9100, "Long")):
            for m in (0, 1):
                for x in (0, 1):
                    parts.append(emit_function(rom, 0, pc, m, x,
                                               func_name=name, hle_func=hle))
    finally:
        codegen.set_valid_variants(saved[0], authoritative=saved[1])
        codegen.set_name_resolver(saved[2])
    generated = "\n".join(parts)
    assert "HleShort(cpu)" in generated and "HleLong(cpu)" in generated
    protos = "".join(f"RecompReturn {n}_M{m}X{x}(CpuState *cpu);\n"
                     for n in ("Short", "Long") for m in (0, 1) for x in (0, 1))
    source = _HARNESS.replace("GENERATED", protos + generated)
    c_file = tmp_path / "hle_seam.c"
    c_file.write_text(source, encoding="utf-8")
    exe = tmp_path / ("hle_seam.exe" if os.name == "nt" else "hle_seam")
    build = subprocess.run(
        [compiler, "-std=c11", "-O1", "-I", str(REPO / "runner" / "src"),
         str(c_file), "-o", str(exe)],
        capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.strip() == "ok"
