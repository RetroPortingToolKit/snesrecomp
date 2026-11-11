/* $4212 at the access's own clock (SNESRECOMP_HBLANK_EXACT, _HVBJOY_EXACT)
 * and the DRAM refresh points (snes_next_refresh), on the real beam. The
 * access lead is the harness's fallback, 20 clocks. */
#define rtl_cpu_master_now harness_rtl_cpu_master_now
#define main hdma_harness_main
#include "hdma_timing_test.c"
#undef main
#undef rtl_cpu_master_now
#include <assert.h>
#include <stdlib.h>

static uint64_t s_now;
uint64_t rtl_cpu_master_now(void) { return s_now; }

static uint8_t read_at(Snes *s, uint64_t now) { s_now = now; return snes_readReg(s, 0x4212); }

int main(void) {
  setenv("SNESRECOMP_HBLANK_EXACT", "1", 1);
  setenv("SNESRECOMP_HVBJOY_EXACT", "1", 1);
  Snes s = {0};
  Dma dma = {0};
  s.dma = &dma;
  s.hdmaBeamOff = true;
  g_interp_apu_driving = 1;

  /* Bit 7 at the read's clock, 20 clocks past the instruction's start, while
   * the beam itself is still on line 224. */
  s.vPos = 224; s.hPos = 1300; s.beamMasterLast = 1000;
  assert((read_at(&s, 1000) & 0x80) == 0);       /* H 1320 of line 224 */
  assert((read_at(&s, 1050) & 0x80) == 0x80);    /* H 6 of line 225 */
  assert(s.vPos == 224 && s.hPos == 1300);       /* the beam did not move */

  /* Bit 0: the automatic read from the first clock 128 past a multiple of 256
   * and at least 3 after line 225 began, for 4352 clocks. */
  s.autoJoyRead = true;
  s.vPos = 225; s.hPos = 0; s.beamMasterLast = 256000;
  assert((read_at(&s, 256128 - 20 - 1) & 1) == 0);
  assert((read_at(&s, 256128 - 20) & 1) == 1);
  assert((read_at(&s, 256128 + 4351 - 20) & 1) == 1);
  assert((read_at(&s, 256128 + 4352 - 20) & 1) == 0);
  s.beamMasterLast = 256126;                     /* line 225 at 128 - 2: the next 128 */
  assert((read_at(&s, 256128 - 20) & 1) == 0);
  assert((read_at(&s, 256384 - 20) & 1) == 1);

  /* Refresh points: H = 538 - (the line's start & 7). */
  s.vPos = 10; s.hPos = 0; s.beamMasterLast = 8000;          /* start & 7 = 0 */
  assert(snes_next_refresh(&s, 8000) == 8538);
  assert(snes_next_refresh(&s, 8538) == 8000 + 1364 + 538 - 4);
  s.beamMasterLast = 8004;                                   /* start & 7 = 4 */
  assert(snes_next_refresh(&s, 8004) == 8004 + 534);

  puts("$4212 at the access: vblank and auto-joypad edges; refresh points PASS");
  return 0;
}
