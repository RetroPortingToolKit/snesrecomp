#include "generic_frame_driver.h"

#include <stdio.h>
#include <string.h>

#include "common_rtl.h"          /* SimpleHdma_*, g_snesrecomp_last_hdmaen */
#include "cpu_state.h"
#include "snes/dma.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/snes.h"

/* One NTSC frame: 262 scanlines x 1364 master clocks. Bounds a productive
 * MMIO loop so it cannot run across several vblanks atomically. */
#define GFD_MASTER_CYCLES_PER_FRAME 357368ull

/* How many times a frame may re-enter the guest after it parks on a poll.
 * Each slice ends at a deterministic read-only cycle or at an IRQ; the bound
 * only stops a pathological loop from spinning the host. */
#define GFD_MAX_SLICES_PER_FRAME 64

/* 0 until the first frame has booted from the reset vector. */
static uint32_t s_resume_pc;
static SnesGenericBootPolicy s_boot;
static RtlGameInfo s_game_info;
static char s_title[64] = "variant";

void snes_generic_frame_driver_set_boot_policy(const SnesGenericBootPolicy *policy) {
    if (policy) s_boot = *policy;
    else memset(&s_boot, 0, sizeof(s_boot));
    if (s_boot.poke_count > 8) s_boot.poke_count = 8;
}

const SnesGenericBootPolicy *snes_generic_frame_driver_boot_policy(void) {
    return &s_boot;
}

static uint32_t read_vector(uint32_t addr) {
    /* Read through the guest bus so a mapper or coprocessor window resolves
     * the same way the CPU sees it. */
    uint32_t lo = snes_read(g_snes, addr);
    uint32_t hi = snes_read(g_snes, addr + 1u);
    return (hi << 8) | lo;
}

static uint32_t reset_vector(void) { return read_vector(0x00FFFCu); }
static uint32_t nmi_vector(void)   { return read_vector(0x00FFEAu); }
static uint32_t irq_vector(void)   { return read_vector(0x00FFEEu); }

/* Run one interrupt handler to its RTI, entered as hardware enters it: the
 * frame is pushed at the PC the guest was interrupted AT, so the handler's
 * terminal RTI returns into that instruction stream. */
static void run_interrupt(uint32_t vector, uint64_t frame_end) {
    cpu_push_interrupt_frame_at(&g_cpu, s_resume_pc);
    interp_bridge_set_master_deadline(frame_end);
    (void)interp_bridge_run_interrupt(&g_cpu, vector);
    /* Clearing matters: a deadline left armed stays true for every AOT block
     * prologue afterwards, which turns every compiled body into an immediate
     * yield-unwind. */
    interp_bridge_set_master_deadline(0);
    {
        uint32_t resume = interp_bridge_lle_resume_pc();
        if (resume) s_resume_pc = resume;
    }
}

static void apply_boot_policy(void) {
    for (unsigned i = 0; i < s_boot.poke_count; i++) {
        uint32_t addr = s_boot.pokes[i].addr24 & 0xFFFFFFu;
        snes_write(g_snes, addr, s_boot.pokes[i].value);
    }
    if (s_boot.entry_pc24) s_resume_pc = s_boot.entry_pc24 & 0xFFFFFFu;
    if (s_boot.poke_count || s_boot.entry_pc24)
        fprintf(stderr, "[frame-driver] boot policy: %u poke(s), entry $%06X\n",
                s_boot.poke_count, s_resume_pc);
}

void snes_generic_frame_driver_run_frame(void) {
    const uint64_t frame_end = g_cpu.master_cycles + GFD_MASTER_CYCLES_PER_FRAME;
    const int booting = (s_resume_pc == 0);
    int slice;

    if (booting) {
        s_resume_pc = reset_vector();
        apply_boot_policy();
    }

    /* Vblank edge. NMITIMEN gates it: delivering before the guest has enabled
     * NMI would land an interrupt frame in the middle of its SEI boot
     * sequence. Nothing is delivered on the very first frame either -- reset
     * has not run yet, so there is no instruction stream to interrupt. */
    if (!booting && g_snes->nmiEnabled) {
        g_snes->inNmi = true;
        run_interrupt(nmi_vector(), frame_end);
        g_snes->inNmi = false;
    }

    /* Run the guest until it parks on a read-only poll (its vblank wait) or
     * the frame's clock runs out. The guest typically parks several times per
     * frame -- on HVBJOY, on a DMA-complete flag, on its own state machine --
     * and each park needs either an interrupt or simply more time. */
    for (slice = 0; slice < GFD_MAX_SLICES_PER_FRAME; slice++) {
        if (g_cpu.master_cycles >= frame_end) break;
        interp_bridge_set_master_deadline(frame_end);
        interp_bridge_run_until_quiescent(&g_cpu, s_resume_pc);
        interp_bridge_set_master_deadline(0);
        {
            uint32_t resume = interp_bridge_lle_resume_pc();
            if (resume) s_resume_pc = resume;
        }
        /* A raster IRQ asserted while the guest ran: service it before
         * continuing, exactly as the CPU samples it between instructions. */
        if (g_snes->inIrq && !g_cpu._flag_I) {
            run_interrupt(irq_vector(), frame_end);
            continue;
        }
        /* Parked with no interrupt pending and clock left over: the guest is
         * waiting for the next vblank. Nothing more happens this frame. */
        if (interp_bridge_lle_took_wai()) break;
    }
}

void snes_generic_frame_driver_draw_ppu_frame(void) {
    SimpleHdma hdma_chans[8];
    Dma *dma = g_snes->dma;
    int trigger, line, ch;

    /* Re-arm HDMA from the last $420C (HDMAEN) the guest wrote -- typically
     * during the NMI just run. The framework records it for exactly this. */
    dma_startDma(dma, g_snesrecomp_last_hdmaen, true);
    for (ch = 0; ch < 8; ch++)
        SimpleHdma_Init(&hdma_chans[ch], &dma->channel[ch]);

    /* Mid-frame raster split, if the guest programmed the V comparator. */
    trigger = g_snes->vIrqEnabled ? (int)g_snes->vTimer : -1;

    /* From line 0: starting at 1 leaves the top scanline holding the previous
     * frame's state, a stripe of stale tilemap above a HUD. */
    for (line = 0; line <= 224; line++) {
        /* HDMA runs in the H-blank BEFORE each visible line, and the raster
         * IRQ then selects the register set that line is drawn with -- so
         * both must precede ppu_runLine for this line, not follow it. */
        for (ch = 0; ch < 8; ch++)
            SimpleHdma_DoLine(&hdma_chans[ch]);
        if (line == trigger) {
            g_snes->inIrq = true;
            cpu_push_interrupt_frame_at(&g_cpu, s_resume_pc);
            (void)interp_bridge_run_interrupt(&g_cpu, irq_vector());
            g_snes->inIrq = false;
            {
                uint32_t resume = interp_bridge_lle_resume_pc();
                if (resume) s_resume_pc = resume;
            }
            trigger = g_snes->vIrqEnabled ? (int)g_snes->vTimer : -1;
        }
        ppu_runLine(g_ppu, line);
    }
}

void snes_generic_frame_driver_reset(void) {
    /* A new session must boot again: the resume PC is the only sticky state
     * this driver owns, and it must not survive snes_free/SnesInit. */
    s_resume_pc = 0;
}

const RtlGameInfo *snes_generic_frame_driver_game_info(const char *title) {
    memset(&s_game_info, 0, sizeof(s_game_info));
    if (title && title[0]) snprintf(s_title, sizeof(s_title), "%s", title);
    s_game_info.title = s_title;
    s_game_info.run_frame = &snes_generic_frame_driver_run_frame;
    s_game_info.draw_ppu_frame = &snes_generic_frame_driver_draw_ppu_frame;
    s_game_info.save_name_prefix = "save";
    s_game_info.session_reset = &snes_generic_frame_driver_reset;
    return &s_game_info;
}
