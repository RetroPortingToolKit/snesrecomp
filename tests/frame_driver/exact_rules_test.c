/* The beam frame driver's exact rules (SNESRECOMP_BEAM_EXACT=1) against the
 * real beam (snes.c), with a scripted CPU in place of the bridge: every
 * instruction lasts 8 master clocks and its last cycle 8 (an 8-clock access).
 * DRAM refresh is off here (SNESRECOMP_REFRESH_AT_BEAM=0, the charge is
 * snesrecomp's runtime, not linked); the short line too, so a field is 357,368.
 *
 *  - the field's NMI follows the first instruction whose last cycle starts at
 *    V=225 H=6 or later, and the interrupt sequence costs 60 clocks;
 *  - an NMI enabled inside vblank, RDNMI still set, fires after the next
 *    instruction, in that field. */
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
bool ppu_checkOverscan(Ppu *ppu) { (void)ppu; return false; }
void ppu_handleVblank(Ppu *ppu) { (void)ppu; }
void ppu_runLine(Ppu *ppu, int line) { (void)ppu; (void)line; }
void ppu_rasterBegin(Ppu *ppu) { (void)ppu; }
void ppu_rasterRenderBegin(Ppu *ppu) { (void)ppu; }
void ppu_rasterRenderEnd(Ppu *ppu) { (void)ppu; }
int ppu_rasterTakeHdmaen(uint8_t *out) { (void)out; return 0; }
void ppu_rasterApplyLine(Ppu *ppu, int line) { (void)ppu; (void)line; }
void snes_refresh_charge(void) {}
void snes_refresh_exempt(void) {}
unsigned snes_last_cycle_clocks(uint32_t pc24, uint32_t next_pc24) { (void)pc24; (void)next_pc24; return 8; }

/* The scripted CPU: main-line PCs count up from $808000; the NMI vector reads
 * as 0 (cart_read), and its handler returns at once. */
static uint64_t s_bridge_deadline, s_nmi_at, s_enable_from;
static uint32_t s_pc = 0x808000;
static int s_nmis;
void interp_bridge_set_master_deadline(uint64_t master_clock) { s_bridge_deadline = master_clock; }
int interp_bridge_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24) {
  if (entry_pc24 == 0) return 0; /* the NMI handler */
  s_pc = entry_pc24;
  while (!s_bridge_deadline || cpu->master_cycles < s_bridge_deadline) {
    if (g_interp_bridge_pc_hook) g_interp_bridge_pc_hook(s_pc, 1, 1);
    /* An instruction that enables NMI ($4200) from s_enable_from on. */
    if (s_enable_from && cpu->master_cycles >= s_enable_from) { g_snes->nmiEnabled = true; s_enable_from = 0; }
    cpu->master_cycles += 8;
    s_pc++;
    if (!s_bridge_deadline) break;
  }
  return 0;
}
int interp_bridge_run_interrupt(CpuState *cpu, uint32_t entry_pc24) { (void)cpu; (void)entry_pc24; return 0; }
uint32_t interp_bridge_lle_resume_pc(void) { return s_pc; }
int interp_bridge_lle_took_wai(void) { return 0; }

static void on_nmi(void) { s_nmi_at = g_cpu.master_cycles; s_nmis++; }
static const SnesBeamFrameHooks s_test_hooks = { .nmi_taken = on_nmi };

int main(void) {
  setenv("SNESRECOMP_BEAM_EXACT", "1", 1);
  setenv("SNESRECOMP_REFRESH_AT_BEAM", "0", 1);
  setenv("SNESRECOMP_SHORT_LINE", "0", 1);
  static Snes snes;
  static Dma dma;
  static Ppu ppu;
  snes.dma = &dma;
  snes.ppu = &ppu;
  g_snes = &snes;
  g_ppu = &ppu;
  g_interp_apu_driving = 1;
  snes_beam_frame_driver_set_hooks(&s_test_hooks);
  snes_beam_frame_driver_set_position(0x808000, false);

  /* Field 1 from V=225 H=0 at clock 1,000,000, NMI on. */
  const uint64_t f0 = 1000000;
  snes.vPos = 225; snes.hPos = 0; snes.beamMasterLast = f0; g_cpu.master_cycles = f0;
  snes.nmiEnabled = true;
  snes_beam_frame_driver_run_frame();
  assert(s_nmis == 1 && s_nmi_at == f0 + 60);
  const uint64_t f1 = f0 + 357368;
  /* The CPU ran past V=225 to the instruction NMI follows; the next field
   * takes it there, 60 clocks later. Instructions start at f0 + 60 + 8k. */
  snes_beam_frame_driver_run_frame();
  assert(s_nmis == 2);
  const uint64_t end = s_nmi_at - 60, last_start = end - 8;
  assert(last_start >= f1 + 6 && last_start - 8 < f1 + 6);
  assert((end - (f0 + 60)) % 8 == 0);

  /* NMI off: enabled by an instruction 300 clocks into vblank, it fires after
   * the next instruction, in that field. */
  const uint64_t f2 = f1 + 357368, f3 = f2 + 357368;
  snes_beam_frame_driver_run_frame();   /* field 2: NMI taken at its start, still on */
  assert(s_nmis == 3);
  snes.nmiEnabled = false;              /* the handler turned it off */
  s_enable_from = f3 + 300;
  snes_beam_frame_driver_run_frame();   /* field 3 starts at f3, NMI off: the late NMI in it */
  assert(s_nmis == 4 && s_nmi_at > f3 + 300 && s_nmi_at < f3 + 300 + 8 + 8 + 60 + 8);
  /* the enabling instruction, one more, then the sequence */
  assert((s_nmi_at - 60) - (f3 + 300) <= 2 * 8);

  puts("exact rules: NMI after the instruction with a cycle from H=6, 60-clock sequence, late NMI in its field PASS");
  return 0;
}
