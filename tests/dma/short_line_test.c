/* The short scanline: with SNESRECOMP_SHORT_LINE=1, line 240 of every other
 * non-interlaced field lasts 1360 master clocks; with 0, every line 1364.
 * argv[1]: "on" or "off". Real beam/register implementation; reuse the
 * peripheral-only stubs of the HDMA harness. */
#define main hdma_harness_main
#include "hdma_timing_test.c"
#undef main
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static Ppu s_ppu;

/* Walk to the next V=225 the way a frame driver does; returns the clocks. */
static uint32_t to_vblank(Snes *s) {
  uint32_t d = snes_master_clocks_until_line(s, 225);
  if (!d) d = snes_master_clocks_until_line(s, 224) + 1364u; /* a whole field */
  snes_sync_master_clock(s, s->beamMasterLast + d);
  assert(s->vPos == 225 && s->hPos == 0);
  return d;
}

int main(int argc, char **argv) {
  const bool on = argc > 1 && strcmp(argv[1], "on") == 0;
  setenv("SNESRECOMP_SHORT_LINE", on ? "1" : "0", 1);
  Snes s = {0};
  Dma dma = {0};
  s.dma = &dma;
  s.ppu = &s_ppu;
  s.hdmaBeamOff = true;
  g_interp_apu_driving = 1;

  /* From reset: line 240 of the first field is long, then short and long by
   * turns (MesenCE's V=225 edges from power-on). */
  assert(to_vblank(&s) == 225u * 1364u && !s.fieldParity);
  for (int field = 0; field < 6; field++) {
    const uint32_t want = on && (field & 1) ? 357364u : 357368u;
    assert(to_vblank(&s) == want);
    assert(s.fieldParity == ((field & 1) == 0));
  }

  /* The short line itself: H wraps at 1360 on line 240 of a field with the
   * parity set, the long line follows. */
  s.vPos = 240; s.hPos = 1350; s.fieldParity = true; s.beamMasterLast = 0;
  snes_sync_master_clock(&s, 12);
  assert(on ? (s.vPos == 241 && s.hPos == 2) : (s.vPos == 240 && s.hPos == 1362));
  s.vPos = 240; s.hPos = 1350; s.fieldParity = false; s.beamMasterLast = 0;
  snes_sync_master_clock(&s, 12);
  assert(s.vPos == 240 && s.hPos == 1362);

  /* Clocks to a V IRQ and to a line across the short line. */
  const uint32_t lost = on ? 4u : 0u;
  s.vPos = 239; s.hPos = 100; s.fieldParity = true; s.beamMasterLast = 0;
  s.vIrqEnabled = true; s.hIrqEnabled = false; s.vTimer = 242; s.inIrq = false;
  assert(snes_master_clocks_until_irq(&s) == 3u * 1364u - 100u - lost);
  assert(snes_master_clocks_until_line(&s, 242) == 3u * 1364u - 100u - lost);
  uint64_t at = 0;
  assert(snes_next_irq_master(&s, 1000, &at) && at == 1000u + 3u * 1364u - 100u - lost);
  /* From past line 240 the next short line is a field later, of the other
   * parity: none when this field's line was the short one. */
  s.vPos = 241; s.hPos = 0; s.vTimer = 240;
  assert(snes_master_clocks_until_irq(&s) == 261u * 1364u);
  s.fieldParity = false;
  assert(snes_master_clocks_until_irq(&s) == 261u * 1364u);
  s.vPos = 239; s.vTimer = 241;
  assert(snes_master_clocks_until_irq(&s) == 2u * 1364u);
  s.vIrqEnabled = false;

  /* Interlace keeps every line long. */
  s_ppu.setini = 1;
  s.vPos = 240; s.hPos = 1350; s.fieldParity = true; s.beamMasterLast = 0;
  snes_sync_master_clock(&s, 12);
  assert(s.vPos == 240 && s.hPos == 1362);
  s_ppu.setini = 0;

  printf("short line (%s): field lengths from reset, line 240 wrap, IRQ and line distances, interlace PASS\n",
         on ? "on" : "off");
  return 0;
}
