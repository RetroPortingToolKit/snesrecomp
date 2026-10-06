/* Beam-aligned frame driver. See beam_frame_driver.h.
 *
 * Brought up on Doom (SuperFX 2), whose frame is a raster IRQ chain: a split
 * at line 23 ends the top forced-blank border, one at line 199 starts the
 * bottom one and DMAs the GSU's frame to VRAM through vblank, and each GSU
 * STOP raises an IRQ that starts the next GSU task. Measured against Mesen2
 * (tools/mesen_scene, irq_log=1): 2-4 IRQs per field at those lines. Each
 * rule below is a way the scaffold's earlier driver broke that chain. */
#include "beam_frame_driver.h"

#include <stdbool.h>
#include <stdint.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/cx4.h"
#include "snes/dma.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/sa1.h"
#include "snes/snes.h"
#include "snes/superfx.h"

extern CpuState g_cpu;
extern Ppu *g_ppu;

/* One NTSC field: 262 scanlines x 1364 master clocks. */
#define BFD_MASTER_CYCLES_PER_FIELD 357368ull
#define BFD_MASTER_CYCLES_PER_LINE 1364u
#define BFD_VBLANK_LINE 225u

/* Bounds the run/park loop: a field holds 262 one-line park steps plus the
 * slices and interrupts between them. Only a pathological guest reaches it. */
#define BFD_MAX_SLICES_PER_FIELD 1024

/* 0 until the first frame has booted from the reset vector. */
static uint32_t s_resume_pc;

static uint32_t read_vector(uint32_t addr) {
  /* Through the guest bus, so a mapper or coprocessor window resolves the
   * vector the way the CPU sees it. */
  uint32_t lo = snes_read(g_snes, addr);
  uint32_t hi = snes_read(g_snes, addr + 1u);
  return (hi << 8) | lo;
}

static uint32_t reset_vector(void) { return read_vector(0x00FFFCu); }
static uint32_t nmi_vector(void) { return read_vector(0x00FFEAu); }
static uint32_t irq_vector(void) { return read_vector(0x00FFEEu); }

static void update_resume_pc(void) {
  uint32_t resume = interp_bridge_lle_resume_pc();
  if (resume)
    s_resume_pc = resume;
}

/* A cartridge coprocessor holding the CPU IRQ line. The bridge yields to this
 * scheduler the moment one is pending with I clear, so the CPU half has to
 * deliver it -- Doom chains its GSU tasks off exactly this interrupt. */
static bool cart_irq_pending(void) {
  const Cart *cart = g_snes->cart;
  if (!cart)
    return false;
  if (cart->superfx && cart->superfx->irq_pending)
    return true;
  if (cart->cx4 && cx4_irq_pending(cart->cx4))
    return true;
  if (cart->sa1 && sa1_cpu_irq_pending(cart->sa1))
    return true;
  return false;
}

static bool irq_wanted(void) {
  return !g_cpu._flag_I && (g_snes->inIrq || cart_irq_pending());
}

/* Master clocks the CPU has spent that the beam has not walked yet: the walk
 * stops one clock past an IRQ latch and owes the rest. */
static uint64_t beam_debt(void) {
  return g_cpu.master_cycles > g_snes->beamMasterLast
             ? g_cpu.master_cycles - g_snes->beamMasterLast
             : 0;
}

/* Parked time: the beam, APU and coprocessors advance; the CPU does not. */
static void advance_parked(uint64_t until) {
  g_cpu.master_cycles = until;
  snes_refresh_exempt();
  snes_sync_master_clock(g_snes, g_cpu.master_cycles);
  cart_sync_coprocessors(g_snes->cart, g_cpu.master_cycles);
}

static void run_interrupt(uint32_t vector, uint64_t deadline) {
  /* The beam keeps running through the handler, as on hardware. A held beam
   * cannot satisfy a handler that polls it -- Doom's reads OPVCT to choose its
   * split, then waits for H-blank -- and the handler spins to the step cap.
   * The bridge yields at the instruction that latched the IRQ, so the handler
   * already starts where the real CPU would take it. */
  cpu_push_interrupt_frame_at(&g_cpu, s_resume_pc);
  interp_bridge_set_master_deadline(deadline);
  (void)interp_bridge_run_interrupt(&g_cpu, vector);
  /* A deadline left armed stays true for every AOT block prologue afterwards
   * and turns each compiled body into an immediate yield. */
  interp_bridge_set_master_deadline(0);
  update_resume_pc();
}

void snes_beam_frame_driver_run_frame(void) {
  const bool booting = s_resume_pc == 0;
  int slices = 0;
  uint64_t frame_end;

  /* The field ends at the beam's next V=225, measured from the BEAM: the
   * walk below leaves it exactly there, and the CPU clock may already be past
   * it (a long instruction, or clocks owed after a latch). A boundary kept on
   * a CPU-time grid drifted from the field, so NMI arrived wherever the
   * previous frame happened to stop: Doom brackets each VRAM upload with
   * INIDISP=$80 ... $0F in vblank, the $80 landed before a raster walk and the
   * $0F after it, and two of every seven gameplay frames drew black. */
  {
    uint32_t to_vblank = snes_master_clocks_until_line(g_snes, BFD_VBLANK_LINE);
    frame_end = g_snes->beamMasterLast +
                (to_vblank ? to_vblank : BFD_MASTER_CYCLES_PER_FIELD);
  }

  if (booting) {
    s_resume_pc = reset_vector();
  } else {
    /* Vblank-edge PPU work a frame-model host owns: OAMADDR re-latches. */
    ppu_checkOverscan(g_ppu);
    ppu_handleVblank(g_ppu);
  }
  /* The render walk owns HDMA (see draw) and the CPU half owns the H/V
   * comparator. Re-asserted per frame: a save-state load restores Snes. */
  snes_set_hdma_beam_enabled(g_snes, false);
  snes_set_raster_irq_beam_enabled(true);
  ppu_rasterBegin(g_ppu);

  /* NMITIMEN gates NMI; nothing is delivered before reset has run, since
   * there is no instruction stream to interrupt yet. */
  if (!booting && g_snes->nmiEnabled) {
    g_snes->inNmi = true;
    run_interrupt(nmi_vector(), frame_end);
    g_snes->inNmi = false;
  }

  /* Run out the field, taking each interrupt at the beam position it latches
   * at -- not from the render walk afterwards, where the beam is parked at
   * V=225 and a handler that reads OPVCT sees the wrong line. */
  while (g_cpu.master_cycles < frame_end && slices++ < BFD_MAX_SLICES_PER_FIELD) {
    uint64_t next;
    uint32_t to_irq;

    if (!irq_wanted()) {
      interp_bridge_set_master_deadline(frame_end);
      (void)interp_bridge_run_until_quiescent(&g_cpu, s_resume_pc);
      interp_bridge_set_master_deadline(0);
      update_resume_pc();
      (void)interp_bridge_lle_took_wai(); /* sticky: consume it */
    }
    /* Past the boundary, an interrupt belongs to the next frame. Delivering
     * one with the deadline already spent made the handler yield on its first
     * instruction, and the next delivery stacked a second interrupt frame on
     * top: Doom's bottom split DMAs from line 199 across V=225 (the forced-
     * blank borders exist to give it that time), so this was every field. */
    if (g_cpu.master_cycles >= frame_end)
      break;
    if (irq_wanted()) {
      run_interrupt(irq_vector(), frame_end);
      continue;
    }
    /* Pay the beam what it owes first: those clocks can latch the next
     * split. */
    if (beam_debt()) {
      snes_sync_master_clock(g_snes, g_cpu.master_cycles);
      cart_sync_coprocessors(g_snes->cart, g_cpu.master_cycles);
      continue;
    }
    /* Parked on a poll with field time left: hardware time still passes.
     * Ending the CPU half here instead also took that time from the
     * coprocessors, which run on the same master clock. Step to the next H/V
     * match when it is nearer than a line (+1: the comparator window excludes
     * its end), a line otherwise, so a raster IRQ or a GSU STOP is seen
     * within a line of hardware raising it. */
    to_irq = snes_master_clocks_until_irq(g_snes);
    if (to_irq)
      to_irq += 1;
    if (to_irq == 0 || to_irq > BFD_MASTER_CYCLES_PER_LINE)
      to_irq = BFD_MASTER_CYCLES_PER_LINE;
    next = g_cpu.master_cycles + to_irq;
    if (next > frame_end)
      next = frame_end;
    advance_parked(next);
  }

  /* Walk the beam to the boundary -- not past it: clocks the CPU spent
   * beyond it belong to the next field. Reaching V=225 closes this field's
   * raster journal (ppu_rasterFieldBoundary). */
  {
    int walk = 0;
    if (g_cpu.master_cycles < frame_end) /* slices exhausted */
      advance_parked(frame_end);
    while (g_snes->beamMasterLast < frame_end && walk++ < 1200) {
      uint64_t before = g_snes->beamMasterLast;
      uint64_t want = frame_end - before;
      snes_advance_master_cycles(g_snes, want > UINT32_MAX ? UINT32_MAX : (uint32_t)want);
      if (g_snes->beamMasterLast == before)
        break;
    }
    cart_sync_coprocessors(g_snes->cart, g_cpu.master_cycles);
  }
}

void snes_beam_frame_driver_draw_ppu_frame(void) {
  int line;

  /* The field's register state as of its start, then each line's journal
   * entries and HDMA in beam order. HDMA here is the real unit (dma.c), so
   * the beam must not also step it from inside guest register writes. */
  snes_set_hdma_beam_enabled(g_snes, false);
  ppu_rasterRenderBegin(g_ppu);
  dma_initHdma(g_snes->dma);
  /* Hardware's V=0 init also performs each channel's first transfer before
   * any visible pixel. */
  dma_primeHdmaFirstLine(g_snes->dma);
  for (line = 1; line <= 224; line++) {
    uint8_t hdmaen;
    /* A split is written in H-blank after its line is drawn, so the previous
     * line's entries take effect on this one. */
    ppu_rasterApplyLine(g_ppu, line - 1);
    if (ppu_rasterTakeHdmaen(&hdmaen))
      dma_startDma(g_snes->dma, hdmaen, true);
    ppu_runLine(g_ppu, line);
    /* HDMA runs in the H-blank after this line; it lands on the next. */
    dma_doHdma(g_snes->dma);
  }
  ppu_rasterRenderEnd(g_ppu);
}

void snes_beam_frame_driver_reset(void) {
  s_resume_pc = 0;
}
