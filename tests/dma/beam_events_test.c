/* Real beam/register implementation; reuse the peripheral-only HDMA stubs. */
#define main hdma_harness_main
#include "hdma_timing_test.c"
#undef main
#include <assert.h>

int main(void) {
  Snes s = {0};
  Dma dma = {0};
  s.dma = &dma;
  s.hdmaBeamOff = true;
  g_interp_apu_driving = 1;
  /* Every possible intermediate master-clock read remains observable, even
   * though a frame-model host owns the HDMA transfer at HBlank. */
  for (unsigned h = 0; h < 1364; ++h) {
    s.hPos = 0; s.vPos = 12; s.beamMasterLast = 100;
    snes_sync_master_clock(&s, 100 + h);
    assert(s.hPos == h && s.vPos == 12);
    assert((snes_readReg(&s, 0x4212) & 0xc0) == (h >= 1024 ? 0x40 : 0));
    s.hPos = h; s.vPos = 12; s.beamMasterLast = 100;
    snes_sync_master_clock(&s, 100 + 1364 - h + 37);
    assert(s.hPos == 37 && s.vPos == 13);
    assert(s.beamMasterLast == 100 + 1364 - h + 37);
  }
  /* A comparator on either side of HBlank still preempts one clock after
   * the match. The pending master time resumes after the IRQ is disabled. */
  for (unsigned timer = 0; timer < 341; ++timer) {
    s.hPos = 0; s.vPos = 12; s.beamMasterLast = 100;
    s.hIrqEnabled = true; s.hTimer = timer; s.inIrq = false;
    snes_sync_master_clock(&s, 100 + 1400);
    assert(s.hPos == 4 * timer + 1 && s.vPos == 12 && s.inIrq);
    assert(s.beamMasterLast == 100 + 4 * timer + 1);
    assert(snes_readReg(&s, 0x4211) == 0x80 && !s.inIrq);
    s.hIrqEnabled = false;
    snes_sync_master_clock(&s, 100 + 1400);
    assert(s.hPos == 36 && s.vPos == 13);
  }
  /* Keep the existing timer charge when an IRQ consumes a partial span.
   * At h=100 the original HBlank span is 924, not the full 1264 clocks. */
  s.hPos = 100; s.vPos = 12; s.beamMasterLast = 0;
  s.hIrqEnabled = true; s.hTimer = 100; s.autoJoyTimer = 4000;
  snes_advance_master_cycles(&s, 2000);
  assert(s.hPos == 401 && s.vPos == 12 && s.autoJoyTimer == 3076);
  s.hIrqEnabled = false; s.autoJoyTimer = 0;
  s.hPos = 1360; s.vPos = 224; s.autoJoyRead = true;
  snes_advance_master_cycles(&s, 12);
  assert(s.hPos == 8 && s.vPos == 225 && s.inVblank);
  assert(s.rdnmiPending && s.autoJoyTimer == 4216);
  assert((snes_readReg(&s, 0x4212) & 0x81) == 0x81);
  s.autoJoyRead = false;
  s.hPos = 1360; s.vPos = 261;
  snes_advance_master_cycles(&s, 12);
  assert(s.hPos == 8 && s.vPos == 0 && !s.inVblank && !s.rdnmiPending);
  puts("beam events: mid-span clocks, HBlank-side IRQs/resume, joypad timer and field latches PASS");
  return 0;
}
