"""Actual core RDNMI ownership with a ROM-free non-idempotent guest loop."""
import os
import subprocess

import pytest

import pathlib
REPO = pathlib.Path(__file__).resolve().parents[2]


def test_host_rdnmi_ownership_preserves_core_defaults_and_irq(tmp_path):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip("C compiler required")
    source = r'''
#define main unused_hdma_fixture_main
#define ppu_rasterFieldBoundary unused_fixture_field_boundary
#include "hdma_timing_test.c"
#undef ppu_rasterFieldBoundary
#undef main
#include <assert.h>
#include "interp816.h"

static unsigned field_boundaries;
void ppu_rasterFieldBoundary(void) { field_boundaries++; }
int interp816_opcode_hook(uint32_t pc) { (void)pc; return 0; }
static uint8_t guest[65536];
static uint8_t read_guest(void *memory, uint32_t address) {
    if ((address & 65535) == 0x4210) return snes_readReg(memory, 0x4210);
    return guest[address & 65535];
}
static void write_guest(void *memory, uint32_t address, uint8_t value) {
    (void)memory; guest[address & 65535] = value;
}
static uint16_t word(unsigned at) { return guest[at] | ((uint16_t)guest[at+1] << 8); }
static void run_guest(Interp816 *cpu, unsigned instructions) {
    while (instructions--) interp816_runOpcode(cpu);
}
static void cross_vblank(Snes *snes) {
    snes->hPos = 1360; snes->vPos = 224;
    snes_advance_master_cycles(snes, 8);
    assert(snes->hPos == 4 && snes->vPos == 225 && snes->inVblank);
}
static void guest_remap_witness(bool beam_owned) {
    /* Synthetic wait/read/remap loop, not extracted game code. Each readable
     * vblank shifts a word once. A duplicate edge demonstrably repeats an
     * action on already-processed input; no title-specific bit map is assumed. */
    static const uint8_t code[] = {
        0xad,0x10,0x42, 0x10,0xfb, 0xc2,0x20,
        0xad,0x00,0x10, 0x4a, 0x8d,0x00,0x10,
        0xee,0x02,0x10, 0xe2,0x20, 0x4c,0x00,0x80
    };
    Snes snes = {0}; Dma dma = {0}; snes.dma = &dma; snes.hdmaBeamOff = true;
    memset(guest, 0, sizeof guest); memcpy(guest+0x8000, code, sizeof code);
    guest[0x1001] = 0x80;
    Interp816 cpu = {0}; cpu.mem=&snes; cpu.read=read_guest; cpu.write=write_guest;
    cpu.pc=0x8000; cpu.sp=0x01ff; interp816_setFlags(&cpu, 0x30);
    snes_set_rdnmi_beam_enabled(beam_owned);
    snes.inNmi = true; run_guest(&cpu, 100);
    assert(word(0x1000)==0x4000 && word(0x1002)==1);
    cross_vblank(&snes); run_guest(&cpu, 100);
    assert(word(0x1000)==(beam_owned ? 0x2000 : 0x4000));
    assert(word(0x1002)==(beam_owned ? 2 : 1));
    if (!beam_owned) {
        /* The next real host frame refreshes raw input and publishes its own
         * edge. Manual mode must not suppress that original source. */
        guest[0x1000]=0; guest[0x1001]=0x80;
        snes.inNmi=true; run_guest(&cpu, 100);
        assert(word(0x1000)==0x4000 && word(0x1002)==2);
    }
}
int main(void) {
    Snes s = {0}; Dma dma = {0}; s.dma=&dma; s.hdmaBeamOff=true;
    /* Process-default remains beam-owned, independent of interrupt enable. */
    for (unsigned enabled=0; enabled<2; enabled++) {
        s.nmiEnabled=enabled; cross_vblank(&s);
        assert(s.rdnmiPending && !s.inNmi);
        assert(snes_readReg(&s,0x4210)==0x82 && snes_readReg(&s,0x4210)==2);
    }
    for (unsigned beam_owned=0; beam_owned<2; beam_owned++) {
        snes_set_rdnmi_beam_enabled(beam_owned);
        /* A stale old-state beam latch is ignored only in host-owned mode,
         * and still cleared on read. The manual host edge works in both. */
        s.rdnmiPending=true; s.inNmi=false;
        assert(snes_readReg(&s,0x4210)==(beam_owned ? 0x82 : 2));
        assert(!s.rdnmiPending && !s.inNmi);
        s.rdnmiPending=true; s.inNmi=true;
        assert(snes_readReg(&s,0x4210)==0x82);
        assert(!s.rdnmiPending && !s.inNmi && snes_readReg(&s,0x4210)==2);
        unsigned before=field_boundaries;
        cross_vblank(&s);
        assert(s.rdnmiPending==beam_owned && field_boundaries==before+1);
        (void)snes_readReg(&s,0x4210);
        /* IRQ preemption and subsequent line/field progression retain their
         * actual core behavior under either RDNMI owner. */
        s.hPos=1350; s.vPos=224; s.inIrq=false;
        s.hIrqEnabled=true; s.hTimer=340; s.vIrqEnabled=false;
        before=field_boundaries;
        snes_advance_master_cycles(&s,20);
        assert(s.inIrq && s.hPos==1361 && s.vPos==224);
        assert(field_boundaries==before && !s.rdnmiPending);
        assert(snes_readReg(&s,0x4211)==0x80 && !s.inIrq);
        s.hIrqEnabled=false; snes_advance_master_cycles(&s,8);
        assert(s.hPos==5 && s.vPos==225 && s.inVblank);
        assert(field_boundaries==before+1 && s.rdnmiPending==beam_owned);
        s.hPos=1360; s.vPos=261; snes_advance_master_cycles(&s,8);
        assert(s.hPos==4 && s.vPos==0 && !s.inVblank && !s.rdnmiPending);
    }
    guest_remap_witness(true);
    guest_remap_witness(false);
    snes_set_rdnmi_beam_enabled(true);
    puts("RDNMI ownership: core defaults, manual/stale latches, IRQ/field events, synthetic guest duplicate-remap witness PASS");
    return 0;
}
'''
    c_file = tmp_path / "rdnmi_ownership.c"
    c_file.write_text(source)
    exe = tmp_path / ("rdnmi_ownership.exe" if os.name == "nt" else "rdnmi_ownership")
    command = [compiler, "-std=c11", "-O1", "-DSNESRECOMP_REVERSE_DEBUG=0",
               "-ffunction-sections", "-fdata-sections", "-Irunner/src",
               "-Irunner/src/snes", "-Itests/dma", str(c_file),
               "runner/src/snes/snes.c", "runner/src/snes/dma.c",
               "runner/src/snes/sdd1.c", "runner/src/snes/interp816.c", "-lm", "-o", str(exe)]
    build = subprocess.run(command, cwd=REPO, capture_output=True, text=True, timeout=60)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "duplicate-remap witness PASS" in run.stdout
