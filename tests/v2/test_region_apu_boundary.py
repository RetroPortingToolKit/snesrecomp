"""ROM-free region instruction boundaries preserve APU credit ownership."""
import os
import re
import subprocess

import pytest

from _helpers import make_lorom_bank0
from test_region_pacing import REPO, _c_function, _emit
from v2.emit_function import emit_function


def test_region_boundary_runs_after_refresh_before_deadline():
    source = _emit("A9 01 85 00 D0 FA 60")
    labels = list(re.finditer(r"(?m)^  L_[^:]+:\s*$", source))
    assert labels
    for index, label in enumerate(labels):
        end = labels[index + 1].start() if index + 1 < len(labels) else len(source)
        block = source[label.end():end]
        assert block.count("cpu_aot_instruction_boundary(cpu);") == 1
        assert block.index("WatchdogCheck();") < block.index("cpu_aot_instruction_boundary(cpu);")
        assert block.index("cpu_aot_instruction_boundary(cpu);") < block.index("interp_bridge_lle_master_deadline_reached(cpu)")
    default = emit_function(make_lorom_bank0({0x8000: bytes.fromhex("EA 60")}),
                            0, 0x8000, 1, 1)
    assert "cpu_aot_instruction_boundary(cpu);" not in default
    # A final native RTS/RTL has no following native label. Both paired
    # bridge return sites must finalize refresh and observe the boundary
    # before another interpreted opcode can add its own clocks.
    bridge = (REPO / "runner/src/snes/interp_bridge.c").read_text()
    paired_calls = list(re.finditer(r"cpu_dispatch_pc_paired\([^;]+;", bridge))
    assert len(paired_calls) == 2
    for call in paired_calls:
        assert re.match(r"\s*if \(s_apu_pending_master\) \{\s*snes_refresh_charge\(\);\s*cpu_aot_instruction_boundary\(cpu\);",
                        bridge[call.end():])


def test_actual_boundary_callback_flushes_only_completed_credit(tmp_path):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip("C compiler required")
    runtime = (REPO / "runner/src/cpu_state.c").read_text()
    bridge = (REPO / "runner/src/snes/interp_bridge.c").read_text()
    timeline = _c_function((REPO / "runner/src/snes/interp_bridge.h").read_text(),
                           "interp_bridge_use_absolute_apu_timeline")
    source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
typedef struct { int portTimeValid; } FakeApu;
typedef struct { void *cart; FakeApu *apu; double apuCatchupCycles; } Snes;
static Snes *g_snes;
static int s_interp_bridge_depth, timeline_on, extended_on, sa1_on, shared_on;
static uint64 s_apu_pending_master, threshold = 32, flushed_total, relative_flushed;
static unsigned flush_calls, sync_calls, apu_sync_calls;
static uint64 synced_master, g_apu_last_sync_master;
static const double kInterpApuPerMaster = 1024000.0 / 21477272.0;
bool rtl_apu_frame_timeline_active(void) { return timeline_on; }
bool rtl_apu_extended_frame_timing(void) { return extended_on; }
bool cart_has_sa1(void *cart) { (void)cart; return sa1_on; }
int cosim_apu_shared_clock(void) { return shared_on; }
static uint64 bridge_bounce_flush_thresh(void) { return threshold; }
static void snes_sync_master_clock(Snes *snes, uint64 master) {
    assert(snes == g_snes); sync_calls++; synced_master = master;
}
static void RtlApuLock(void) {
    assert(sync_calls > flush_calls);
    flush_calls++; flushed_total += s_apu_pending_master;
}
static void RtlApuUnlock(void) {}
static void rtl_sync_apu_to_cpu_locked(void) {
    assert(g_apu_last_sync_master == synced_master); apu_sync_calls++;
}
static void snes_catchupApu(Snes *snes) {
    assert(snes == g_snes);
    assert(snes->apuCatchupCycles == (double)s_apu_pending_master * kInterpApuPerMaster);
    relative_flushed += s_apu_pending_master; snes->apuCatchupCycles = 0;
}
void (*g_cpu_aot_master_credit_hook)(CpuState *cpu, uint64 clocks);
void (*g_cpu_aot_instruction_boundary_hook)(CpuState *cpu);
#define SNES_COSIM 1
'''
    source += timeline
    source += _c_function(bridge, "bridge_apu_flush")
    source += _c_function(bridge, "bridge_aot_credit_master")
    source += _c_function(bridge, "bridge_aot_instruction_boundary")
    source += _c_function(runtime, "cpu_add_aot_master_cycles")
    source += _c_function(runtime, "cpu_aot_instruction_boundary")
    source += r'''
int main(void) {
    CpuState cpu; memset(&cpu, 0, sizeof cpu);
    FakeApu apu = {0}; Snes snes = {0, &apu}; g_snes = &snes;
    s_interp_bridge_depth = 1;
    g_cpu_aot_master_credit_hook = bridge_aot_credit_master;

    /* A nullable boundary has no state or clock effect. */
    cpu_add_aot_master_cycles(&cpu, 24);
    cpu_aot_instruction_boundary(&cpu);
    assert(cpu.master_cycles == 24 && s_apu_pending_master == 24 && !flush_calls && !sync_calls);
    g_cpu_aot_instruction_boundary_hook = bridge_aot_instruction_boundary;
    cpu_aot_instruction_boundary(&cpu); /* complete but subthreshold */
    assert(cpu.master_cycles == 24 && s_apu_pending_master == 24 && !flush_calls && !sync_calls);

    /* Phase credit may cross the threshold, but cannot flush mid-opcode. */
    cpu_add_aot_master_cycles(&cpu, 6);
    cpu_add_aot_master_cycles(&cpu, 2);
    assert(cpu.master_cycles == 32 && s_apu_pending_master == 32 && !flush_calls);
    CpuState before = cpu;
    cpu_aot_instruction_boundary(&cpu);
    assert(!memcmp(&before, &cpu, sizeof cpu));
    assert(flush_calls == 1 && sync_calls == 1 && flushed_total == 32 && !s_apu_pending_master);
    cpu_aot_instruction_boundary(&cpu); /* nested caller label: no duplicate */
    assert(flush_calls == 1 && sync_calls == 1 && flushed_total == 32);

    /* Existing nested LLE credit is never inferred from an AOT span. */
    cpu_add_aot_master_cycles(&cpu, 11);
    cpu.master_cycles += 7; s_apu_pending_master += 7;
    s_interp_bridge_depth = 2;
    cpu_add_aot_master_cycles(&cpu, 13);
    s_interp_bridge_depth = 1;
    cpu_aot_instruction_boundary(&cpu);
    assert(cpu.master_cycles == 63 && s_apu_pending_master == 31 && flushed_total == 32);
    cpu_add_aot_master_cycles(&cpu, 1);
    cpu_aot_instruction_boundary(&cpu);
    assert(cpu.master_cycles == 64 && !s_apu_pending_master && flushed_total == 64 && flush_calls == 2);

    /* Threshold zero means one flush per nonempty completed batch. */
    threshold = 0;
    cpu_aot_instruction_boundary(&cpu);
    assert(flush_calls == 2 && sync_calls == 2);
    cpu_add_aot_master_cycles(&cpu, 8);
    assert(flush_calls == 2);
    cpu_aot_instruction_boundary(&cpu);
    cpu_aot_instruction_boundary(&cpu);
    assert(flush_calls == 3 && sync_calls == 3 && flushed_total == 72);

    /* Missing bridge/SNES cannot synchronize or consume another owner. */
    s_apu_pending_master = 9; s_interp_bridge_depth = 0;
    cpu_aot_instruction_boundary(&cpu);
    assert(flush_calls == 3 && sync_calls == 3 && s_apu_pending_master == 9);
    s_interp_bridge_depth = 1; g_snes = NULL;
    cpu_aot_instruction_boundary(&cpu);
    assert(flush_calls == 3 && sync_calls == 3 && s_apu_pending_master == 9);
    g_snes = &snes; s_apu_pending_master = 0;

    /* SA-1 legacy absolute ownership needs no relative marker. Extended
     * mode DOES queue a scheduling marker, just like interpreted opcodes;
     * the actual bridge flush must sync without relative double-credit. */
    timeline_on = 1; sa1_on = 1;
    cpu_add_aot_master_cycles(&cpu, 6); cpu_aot_instruction_boundary(&cpu);
    assert(!s_apu_pending_master && relative_flushed == 72 && flush_calls == 3);
    sa1_on = 0; extended_on = 1; apu.portTimeValid = 1;
    cpu_add_aot_master_cycles(&cpu, 6);
    assert(s_apu_pending_master == 6 && flush_calls == 3);
    cpu_aot_instruction_boundary(&cpu);
    assert(!s_apu_pending_master && flushed_total == 78 && relative_flushed == 72 && flush_calls == 4);
    timeline_on = 0; shared_on = 1;
    cpu_add_aot_master_cycles(&cpu, 6); cpu_aot_instruction_boundary(&cpu);
    assert(cpu.master_cycles == 90 && !s_apu_pending_master && flushed_total == 78 && flush_calls == 4);
    assert(apu_sync_calls == 4 && !snes.apuCatchupCycles);
    puts("boundary credit ownership and flush ordering passed");
    return 0;
}
'''
    c_file = tmp_path / "boundary.c"
    c_file.write_text(source)
    exe = tmp_path / ("boundary.exe" if os.name == "nt" else "boundary")
    build = subprocess.run([compiler, "-std=c11", "-O1", "-I", str(REPO / "runner/src"),
                            str(c_file), "-o", str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "boundary credit ownership" in run.stdout
