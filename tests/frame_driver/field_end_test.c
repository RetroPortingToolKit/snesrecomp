/* The beam frame driver's field end with the short line
 * (SNESRECOMP_SHORT_LINE=1), against the real beam (snes.c) and a scripted
 * CPU of 8-clock instructions, for run_frame ("plain") and run_frame_exact
 * ("exact", SNESRECOMP_BEAM_EXACT=1). DRAM refresh is off here, as in
 * exact_rules_test.c. In "exact-wai" the CPU halts in WAI, the way a game
 * waits for vblank: the exact driver then parks it to the field end and the
 * beam stops exactly on V=225 H=0, as in run_frame. Without WAI the exact
 * driver runs the CPU past V=225, so only its first field starts there.
 *
 * A field that starts with the beam exactly on V=225 H=0 ends exactly on the
 * next V=225 H=0: 357,364 clocks when its line 240 is the short one, 357,368
 * otherwise. Both drivers used to take a whole 357,368-clock field there,
 * which left the beam 4 clocks into line 225 and the NMI 4 clocks late. */
#define main hdma_harness_main
#include "../dma/hdma_timing_test.c"
#undef main
#include "../../runner/src/beam_frame_driver.c"
#include <assert.h>
#include <stdlib.h>

Snes *g_snes;
void (*g_interp_bridge_pc_hook)(uint32_t pc24, int m_flag, int x_flag);

void cart_sync_coprocessors(Cart *cart, uint64_t master_clock) { (void)cart; (void)master_clock; }
int cx4_irq_pending(const Cx4 *cx4) { (void)cx4; return 0; }
bool sa1_cpu_irq_pending(const Sa1 *sa1) { (void)sa1; return false; }
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) { (void)cpu; (void)bank; (void)addr; (void)v; }
void ppu_runLine(Ppu *ppu, int line) { (void)ppu; (void)line; }
void ppu_rasterBegin(Ppu *ppu) { (void)ppu; }
void ppu_rasterBeginPicture(Ppu *ppu) { (void)ppu; }
bool ppu_rasterReplayReady(void) { return true; }
void ppu_renderRasterLine(Ppu *ppu, int line) { (void)ppu; (void)line; }
void ppu_rasterRenderBegin(Ppu *ppu) { (void)ppu; }
void ppu_rasterRenderEnd(Ppu *ppu) { (void)ppu; }
int ppu_rasterTakeHdmaen(uint8_t *out) { (void)out; return 0; }
void ppu_rasterApplyLine(Ppu *ppu, int line) { (void)ppu; (void)line; }
void snes_refresh_charge(void) {}
void snes_refresh_exempt(void) {}
unsigned snes_last_cycle_clocks(uint32_t pc24, uint32_t next_pc24) { (void)pc24; (void)next_pc24; return 8; }

/* The scripted CPU: main-line instructions of 8 clocks up to the deadline,
 * or a single WAI in "exact-wai". */
static uint64_t s_bridge_deadline;
static uint32_t s_pc = 0x808000;
static bool s_wai;
void interp_bridge_set_master_deadline(uint64_t master_clock) { s_bridge_deadline = master_clock; }
void interp_bridge_set_irq_deadline(uint64_t (*hook)(void)) { (void)hook; }
int interp_bridge_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24) {
  if (entry_pc24 == 0) return 0;
  s_pc = entry_pc24;
  if (s_wai) {               /* the instruction is WAI: the CPU halts after it */
    cpu->master_cycles += 8;
    s_pc++;
    return 0;
  }
  while (!s_bridge_deadline || cpu->master_cycles < s_bridge_deadline) {
    if (g_interp_bridge_pc_hook) g_interp_bridge_pc_hook(s_pc, 1, 1);
    cpu->master_cycles += 8;
    s_pc++;
    if (!s_bridge_deadline) break;
  }
  return 0;
}
int interp_bridge_run_interrupt(CpuState *cpu, uint32_t entry_pc24) { (void)cpu; (void)entry_pc24; return 0; }
uint32_t interp_bridge_lle_resume_pc(void) { return s_pc; }
int interp_bridge_lle_took_wai(void) { return s_wai; }

/* run_frame_exact reports the field end it computed. */
static uint64_t s_frame_end;
static void on_frame_start(uint64_t frame_end, bool booting) { (void)booting; s_frame_end = frame_end; }
static const SnesBeamFrameHooks s_test_hooks = { .frame_start = on_frame_start };

static void check_field(const char *mode, bool exact, uint64_t start, uint64_t end) {
  printf("%s field end: +%llu, beam V=%u H=%u\n", mode,
         (unsigned long long)((exact ? s_frame_end : g_snes->beamMasterLast) - start),
         (unsigned)g_snes->vPos, (unsigned)g_snes->hPos);
  if (exact) assert(s_frame_end == end);
  if (exact && !s_wai) {
    /* The exact driver ran the CPU on to the instruction the NMI follows. */
    assert(g_snes->vPos == 225 && g_snes->beamMasterLast - end == g_snes->hPos);
  } else {
    assert(g_snes->vPos == 225 && g_snes->hPos == 0 && g_snes->beamMasterLast == end);
  }
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "plain";
  const bool exact = strncmp(mode, "exact", 5) == 0;
  s_wai = strcmp(mode, "exact-wai") == 0;
  setenv("SNESRECOMP_BEAM_EXACT", exact ? "1" : "0", 1);
  setenv("SNESRECOMP_REFRESH_AT_BEAM", "0", 1);
  setenv("SNESRECOMP_SHORT_LINE", "1", 1);
  static Snes snes;
  static Dma dma;
  static Ppu ppu;
  snes.dma = &dma;
  snes.ppu = &ppu;
  g_snes = &snes;
  g_ppu = &ppu;
  g_interp_apu_driving = 1;
  assert(snes_beam_frame_driver_exact() == exact);
  assert(snes_line_clocks(&snes, 240, true) == 1360u);
  assert(snes_line_clocks(&snes, 240, false) == 1364u);

  /* A field from V=225 H=0 whose line 240 is the short one, NMI off. */
  const uint64_t f0 = 1000000;
  snes.vPos = 225; snes.hPos = 0; snes.fieldParity = true;
  snes.beamMasterLast = f0; g_cpu.master_cycles = f0;
  snes.nmiEnabled = false;
  snes_beam_frame_driver_set_hooks(&s_test_hooks);
  snes_beam_frame_driver_set_position(0x808000, false);
  snes_beam_frame_driver_run_frame();
  check_field(mode, exact, f0, f0 + 357364u);
  /* The next field has no short line: a whole 357,368 clocks; the one after
   * it has the short line again. */
  snes_beam_frame_driver_run_frame();
  check_field(mode, exact, f0, f0 + 357364u + 357368u);
  snes_beam_frame_driver_run_frame();
  check_field(mode, exact, f0, f0 + 357364u + 357368u + 357364u);

  printf("field end (%s): fields alternate 357,364 and 357,368 clocks PASS\n", mode);
  return 0;
}
