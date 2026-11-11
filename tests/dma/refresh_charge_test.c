/* snes_refresh_charge (common_cpu_infra.c, cut out by
 * tests/test_beam_events.py) with DRAM refresh at the beam
 * (SNESRECOMP_REFRESH_AT_BEAM=1): one 40-clock stall per refresh point the
 * CPU crossed, wherever the charge comes; argv[1] "off": 40 per 1364 clocks
 * of CPU time, as before. */
#define main hdma_harness_main
#include "hdma_timing_test.c"
#undef main
#include <assert.h>
#include <stdlib.h>
#include <string.h>

Snes *g_snes;
void snes_refresh_charge(void);
extern uint64_t g_refresh_charged_upto;

static uint64_t run(uint64_t to) { g_cpu.master_cycles = to; snes_refresh_charge(); return g_cpu.master_cycles; }

int main(int argc, char **argv) {
  const bool on = !(argc > 1 && strcmp(argv[1], "off") == 0);
  setenv("SNESRECOMP_REFRESH_AT_BEAM", on ? "1" : "0", 1);
  static Snes s;
  static Dma dma;
  s.dma = &dma;
  s.hdmaBeamOff = true;
  g_snes = &s;
  g_interp_apu_driving = 1;
  s.vPos = 10; s.hPos = 0; s.beamMasterLast = 8000;   /* line start & 7 = 0: its point at H=538 */
  assert(run(8000) == 8000 && g_refresh_charged_upto == 8000);
  if (on) {
    assert(run(8537) == 8537);                    /* short of the point: nothing */
    assert(run(8538) == 8578);                    /* on it: the stall */
    assert(run(8578 + 600) == 9178);              /* the next point is 9898 (H=534) */
    assert(run(9898 + 10) == 9948);
    assert(run(9948 + 2 * 1364) == 9948 + 2 * 1364 + 80); /* two points in one charge */
    assert(run(50000000) == 50000000);            /* a teleport is not charged */
  } else {
    assert(run(8538) == 8538);
    assert(run(8000 + 1364) == 8000 + 1364 + 40); /* per 1364 clocks of CPU time */
  }
  printf("refresh charge (%s) PASS\n", on ? "at the beam" : "per CPU time");
  return 0;
}
