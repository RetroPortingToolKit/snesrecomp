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
#include <stdlib.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/cx4.h"
#include "snes/dma.h"
#include "snes/interp_bridge.h"
#include "snes/last_cycle.h"
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

/* The 65816's reset sequence: the first instruction starts at master clock
 * 186 while the beam starts at 0 (MesenCE, from power-on). SNESRECOMP_RESET_CLOCK;
 * default: on with the master-clock APU timeline (RtlApuUseMasterClock). */
#define BFD_RESET_CLOCKS 186u
static bool reset_clock_rule(void) {
  static int env = -2;
  if (env == -2) {
    const char *e = getenv("SNESRECOMP_RESET_CLOCK");
    env = e ? e[0] != '0' : -1;
  }
  return env >= 0 ? env : rtl_apu_master_clock();
}

/* Bounds the run/park loop: a field holds 262 one-line park steps plus the
 * slices and interrupts between them. Only a pathological guest reaches it. */
#define BFD_MAX_SLICES_PER_FIELD 1024

/* 0 until the first frame has booted from the reset vector. */
static uint32_t s_resume_pc;

/* The CPU executed WAI and is halted until an interrupt line asserts. */
static bool s_wai_halted;

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

static bool irq_line_asserted(void) {
  return g_snes->inIrq || cart_irq_pending();
}

static bool irq_wanted(void) {
  return !g_cpu._flag_I && irq_line_asserted();
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

/* ── MesenCE's interrupt and field rules (beam_frame_driver.h) ───────────── */

bool snes_beam_frame_driver_exact(void) {
  static int env = -2;
  if (env == -2) {
    const char *e = getenv("SNESRECOMP_BEAM_EXACT");
    env = e ? e[0] != '0' : -1;
  }
  return env >= 0 ? env : rtl_apu_master_clock();
}

static const SnesBeamFrameHooks *s_hooks;
void snes_beam_frame_driver_set_hooks(const SnesBeamFrameHooks *hooks) { s_hooks = hooks; }

/* The interrupt sequence in native mode: 2 internal cycles (6 clocks each),
 * PB, PCH, PCL and P pushed to the WRAM stack and the vector read from bank
 * $00 ROM (8 each). MesenCE takes each field's NMI 76 clocks after V=225: the
 * instruction the beam crossed it in, then these 60. */
#define BFD_INTERRUPT_CYCLES 8u
#define BFD_INTERRUPT_CLOCKS (2u * 6u + 4u * 8u + 2u * 8u)
/* The NMI line rises at H=6 of line 225 and the CPU sees it at the start of
 * its next cycle: the NMI follows the first instruction with a cycle starting
 * there or later. The CPU half runs on to H=12 (a 6-clock last cycle starting
 * at 6), and one instruction more when the last one's last cycle started
 * before 6. */
#define BFD_NMI_LINE_CLOCKS 6u
#define BFD_NMI_SEEN_CLOCKS 12u
/* Single-instruction slices while a late NMI enable is possible: a vblank
 * holds ~37 lines of them. */
#define BFD_MAX_LATE_NMI_STEPS 200000

static uint64_t s_deadline, s_hard_deadline;
static void set_slice_deadline(uint64_t clock, uint64_t hard) {
  s_deadline = clock;
  s_hard_deadline = hard;
  interp_bridge_set_master_deadline(clock);
}
void snes_beam_frame_driver_deadlines(uint64_t *deadline, uint64_t *hard) {
  if (deadline) *deadline = s_deadline;
  if (hard) *hard = s_hard_deadline;
}

/* The instructions a slice starts: the last one decides the field's NMI. */
static void (*s_prev_pc_hook)(uint32_t pc24, int m_flag, int x_flag);
static uint32_t s_last_pc;
static uint64_t s_last_dma; /* g_snes_dma_charged as the last instruction started */
uint32_t snes_beam_frame_driver_last_pc(void) { return s_last_pc; }
static void record_pc(uint32_t pc24, int m_flag, int x_flag) {
  if (s_prev_pc_hook) s_prev_pc_hook(pc24, m_flag, x_flag);
  s_last_pc = pc24;
  s_last_dma = g_snes_dma_charged;
}

/* One CPU slice to `deadline`; `hard` is the first stop that is not a DRAM
 * refresh. True when the host's slice_end hook took its time back. */
static bool run_slice(uint64_t deadline, uint64_t hard) {
  s_prev_pc_hook = g_interp_bridge_pc_hook;
  g_interp_bridge_pc_hook = record_pc;
  s_last_pc = 0;
  set_slice_deadline(deadline, hard);
  (void)interp_bridge_run_until_quiescent(&g_cpu, s_resume_pc);
  set_slice_deadline(0, 0);
  g_interp_bridge_pc_hook = s_prev_pc_hook;
  snes_refresh_charge();
  const bool taken_back = s_hooks && s_hooks->slice_end && s_hooks->slice_end();
  update_resume_pc();
  if (interp_bridge_lle_took_wai())
    s_wai_halted = true;
  /* The beam where the CPU is: RDNMI and $4212 for the next decision. */
  snes_sync_master_clock(g_snes, g_cpu.master_cycles);
  return taken_back;
}

static uint64_t next_refresh_stop(uint64_t now) {
  return snes_refresh_at_beam() ? snes_next_refresh(g_snes, now) + 1u : UINT64_MAX;
}

static void take_interrupt(uint32_t vector, uint64_t frame_end, bool nmi) {
  if (s_hooks && s_hooks->interrupt_entry) s_hooks->interrupt_entry(nmi);
  cpu_push_interrupt_frame_at(&g_cpu, s_resume_pc);
  g_cpu.cycles += BFD_INTERRUPT_CYCLES;
  g_cpu.master_cycles += BFD_INTERRUPT_CLOCKS;
  snes_refresh_charge(); /* a refresh point inside the sequence stalls it */
  if (nmi && s_hooks && s_hooks->nmi_taken) s_hooks->nmi_taken();
  if (nmi) {
    /* The NMI handler is an ordinary slice from its vector: it stops at the
     * next refresh point like every other one and goes on in the field's
     * slices, rather than running whole to its RTI with its refresh charged
     * afterwards. */
    const uint64_t refresh = next_refresh_stop(g_cpu.master_cycles);
    const uint64_t stop = refresh < frame_end ? refresh : frame_end;
    set_slice_deadline(stop, stop);
    (void)interp_bridge_run_until_quiescent(&g_cpu, vector);
  } else {
    set_slice_deadline(frame_end, frame_end);
    (void)interp_bridge_run_interrupt(&g_cpu, vector);
  }
  set_slice_deadline(0, 0);
  snes_refresh_charge();
  update_resume_pc();
}

static void take_nmi(uint64_t frame_end) {
  g_snes->inNmi = true;
  s_wai_halted = false;
  take_interrupt(nmi_vector(), frame_end, true);
  g_snes->inNmi = false;
}

/* After the slice that reached V=225 H=12: one instruction more when the last
 * one's last cycle started before H=6. A DMA that instruction started ($420B)
 * runs in the next instruction's first cycle on hardware: its clocks, charged
 * to the write here, are not a cycle of this instruction. */
static void finish_nmi_approach(uint64_t frame_end) {
  if (!s_last_pc || g_cpu.master_cycles < frame_end + BFD_NMI_SEEN_CLOCKS) return;
  const uint64_t dma = g_snes_dma_charged - s_last_dma;
  const unsigned last = snes_last_cycle_clocks(s_last_pc, s_resume_pc);
  if (g_cpu.master_cycles - dma - last >= frame_end + BFD_NMI_LINE_CLOCKS) return;
  (void)run_slice(g_cpu.master_cycles + 1u, g_cpu.master_cycles + 1u);
}

/* NMI can still fire late in this field: it is off and RDNMI is still set. */
static bool late_nmi_possible(void) { return !g_snes->nmiEnabled && g_snes->rdnmiPending; }

static void run_frame_exact(void) {
  const bool booting = s_resume_pc == 0;
  int slices = 0, late_steps = 0;
  uint64_t frame_end;
  {
    uint32_t to_vblank = snes_master_clocks_until_line(g_snes, BFD_VBLANK_LINE);
    frame_end = g_snes->beamMasterLast + (to_vblank ? to_vblank : BFD_MASTER_CYCLES_PER_FIELD);
  }
  if (s_hooks && s_hooks->frame_start) s_hooks->frame_start(frame_end, booting);
  if (booting) {
    s_resume_pc = reset_vector();
    if (reset_clock_rule() && g_cpu.master_cycles < BFD_RESET_CLOCKS)
      advance_parked(BFD_RESET_CLOCKS);
  } else {
    ppu_checkOverscan(g_ppu);
    ppu_handleVblank(g_ppu);
  }
  snes_set_hdma_beam_enabled(g_snes, false);
  snes_set_raster_irq_beam_enabled(true);
  ppu_rasterBegin(g_ppu);
  if (!booting && g_snes->nmiEnabled &&
      !(s_hooks && s_hooks->field_nmi && s_hooks->field_nmi(frame_end)))
    take_nmi(frame_end);

  const uint64_t cpu_end = frame_end + BFD_NMI_SEEN_CLOCKS;
  while (g_cpu.master_cycles < cpu_end && slices++ < BFD_MAX_SLICES_PER_FIELD) {
    if (s_wai_halted && irq_line_asserted())
      s_wai_halted = false;
    if (!s_wai_halted && !irq_wanted()) {
      /* While a late NMI is possible, one instruction per slice. */
      const bool watch = !booting && late_nmi_possible() && late_steps < BFD_MAX_LATE_NMI_STEPS;
      const uint64_t before = g_cpu.master_cycles;
      uint64_t deadline = watch ? before + 1u : next_refresh_stop(before);
      if (deadline > cpu_end) deadline = cpu_end;
      /* A refresh stop before the field end: only the field end is hard. */
      if (run_slice(deadline, watch || deadline >= frame_end ? deadline : frame_end))
        slices--;
      if (!s_wai_halted) finish_nmi_approach(frame_end);
      if (watch && g_cpu.master_cycles > before && g_cpu.master_cycles < frame_end) {
        late_steps++;
        slices--;
        if (g_snes->nmiEnabled && g_snes->rdnmiPending) {
          /* MesenCE ($4200 write, SnesCpu::DetectNmiSignalEdge): the NMI comes
           * after the next instruction. */
          (void)run_slice(g_cpu.master_cycles + 1u, g_cpu.master_cycles + 1u);
          take_nmi(frame_end);
        }
        continue;
      }
      if (!watch && deadline < cpu_end && g_cpu.master_cycles > before && g_cpu.master_cycles < cpu_end) {
        slices--; /* a refresh stop, not a park */
        continue;
      }
    }
    if (g_cpu.master_cycles >= frame_end)
      break;
    if (irq_wanted()) {
      s_wai_halted = false;
      take_interrupt(irq_vector(), frame_end, false);
      continue;
    }
    if (beam_debt()) {
      snes_sync_master_clock(g_snes, g_cpu.master_cycles);
      cart_sync_coprocessors(g_snes->cart, g_cpu.master_cycles);
      continue;
    }
    uint32_t to_irq = snes_master_clocks_until_irq(g_snes);
    if (to_irq)
      to_irq += 1;
    if (to_irq == 0 || to_irq > BFD_MASTER_CYCLES_PER_LINE)
      to_irq = BFD_MASTER_CYCLES_PER_LINE;
    uint64_t next = g_cpu.master_cycles + to_irq;
    if (next > frame_end)
      next = frame_end;
    advance_parked(next);
  }

  {
    int walk = 0;
    if (g_cpu.master_cycles < frame_end)
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

void snes_beam_frame_driver_run_frame(void) {
  if (snes_beam_frame_driver_exact()) {
    run_frame_exact();
    return;
  }
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
    if (reset_clock_rule() && g_cpu.master_cycles < BFD_RESET_CLOCKS)
      advance_parked(BFD_RESET_CLOCKS);
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
    s_wai_halted = false;
    run_interrupt(nmi_vector(), frame_end);
    g_snes->inNmi = false;
  }

  /* Run out the field, taking each interrupt at the beam position it latches
   * at -- not from the render walk afterwards, where the beam is parked at
   * V=225 and a handler that reads OPVCT sees the wrong line. */
  while (g_cpu.master_cycles < frame_end && slices++ < BFD_MAX_SLICES_PER_FIELD) {
    uint64_t next;
    uint32_t to_irq;

    /* WAI halts the CPU until an interrupt line asserts -- NMI (next field),
     * a raster match or a coprocessor IRQ. With I set the IRQ wakes it
     * without taking the vector. Resuming it after a park step instead made
     * every WAI last one scanline: Doom times its title hold as a countdown
     * of WAIs (`WAI / DEX / BPL`), which then ran out within the first field
     * and skipped the title. */
    if (s_wai_halted && irq_line_asserted())
      s_wai_halted = false;
    if (!s_wai_halted && !irq_wanted()) {
      interp_bridge_set_master_deadline(frame_end);
      (void)interp_bridge_run_until_quiescent(&g_cpu, s_resume_pc);
      interp_bridge_set_master_deadline(0);
      update_resume_pc();
      if (interp_bridge_lle_took_wai())
        s_wai_halted = true;
    }
    /* Past the boundary, an interrupt belongs to the next frame. Delivering
     * one with the deadline already spent made the handler yield on its first
     * instruction, and the next delivery stacked a second interrupt frame on
     * top: Doom's bottom split DMAs from line 199 across V=225 (the forced-
     * blank borders exist to give it that time), so this was every field. */
    if (g_cpu.master_cycles >= frame_end)
      break;
    if (irq_wanted()) {
      s_wai_halted = false;
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
    /* Parked on a poll or halted on WAI with field time left: hardware time
     * still passes.
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
  snes_beam_frame_driver_draw_ppu_frame_observed(NULL, NULL);
}

void snes_beam_frame_driver_draw_ppu_frame_observed(
    SnesBeamRasterObserver *observer, void *context) {
  int line;

  /* The field's register state as of its start, then each line's journal
   * entries and HDMA in beam order. HDMA here is the real unit (dma.c), so
   * the beam must not also step it from inside guest register writes. */
  snes_set_hdma_beam_enabled(g_snes, false);
  ppu_rasterRenderBegin(g_ppu);
  dma_initHdma(g_snes->dma);
  /* H-blank on V=0 performs and consumes the first transfer before visible
   * line 1. A preview leaves the address/count behind and repeats that slot. */
  dma_doHdma(g_snes->dma);
  for (line = 1; line <= 224; line++) {
    uint8_t hdmaen;
    /* A split is written in H-blank after its line is drawn, so the previous
     * line's entries take effect on this one. */
    ppu_rasterApplyLine(g_ppu, line - 1);
    if (ppu_rasterTakeHdmaen(&hdmaen))
      dma_startDma(g_snes->dma, hdmaen, true);
    ppu_runLine(g_ppu, line);
    if (observer)
      observer(g_ppu, (unsigned)line, context);
    /* HDMA runs in the H-blank after this line; it lands on the next. */
    dma_doHdma(g_snes->dma);
  }
  ppu_rasterRenderEnd(g_ppu);
}

void snes_beam_frame_driver_reset(void) {
  s_resume_pc = 0;
  s_wai_halted = false;
}

void snes_beam_frame_driver_get_position(uint32_t *pc, bool *wai) {
  *pc = s_resume_pc;
  *wai = s_wai_halted;
}

void snes_beam_frame_driver_set_position(uint32_t pc, bool wai) {
  s_resume_pc = pc;
  s_wai_halted = wai;
}
