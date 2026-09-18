#ifndef SNESRECOMP_PPU_DMA_TRACE_H
#define SNESRECOMP_PPU_DMA_TRACE_H

#include <stdint.h>
#include <stdio.h>

/* ── Always-on PPU + DMA observability ring ───────────────────────────────
 *
 * Compiled into EVERY build (Release included). Records, continuously:
 *   - every A->B (and B->A) DMA transfer triggered via $420B — the
 *     VRAM/CGRAM/OAM graphics uploads, captured with their source
 *     bank:addr, B-bus destination register, and size; and
 *   - a per-frame snapshot of the live PPU state (forced-blank/brightness,
 *     main/sub screen-enable, BG mode, non-zero CGRAM/VRAM counts).
 *
 * This exists so "is forced-blank stuck on / is the palette black / is VRAM
 * being populated / where is a malfunctioning DMA sourcing from" is answered
 * from RECORDED HISTORY, not by arming a trace at probe time. The rings
 * always record; env vars only control optional streaming verbosity, and a
 * targeted dump (post-mortem / on exit) pulls the slice of interest.
 *
 *   SNESRECOMP_PPU_HEARTBEAT=<N>  stream a per-frame PPU summary to stderr
 *                                 every N frames (0 = off, the default).
 *   SNESRECOMP_DMA_LOG=1          stream every recorded DMA to stderr.
 *   SNESRECOMP_HEARTBEAT_WRAM=<a1,a2,...>
 *                                 up to PPUDMA_WRAM_PROBE_MAX hex WRAM
 *                                 offsets; each is read as a 16-bit word
 *                                 into EVERY per-frame snapshot from frame
 *                                 0, appended to the heartbeat line as
 *                                 [addr]=value, and included in the ring
 *                                 dump. The probe set is configuration;
 *                                 the capture is always-on history.
 */

#define PPUDMA_WRAM_PROBE_MAX 8

/* Record one channel's config at $420B (MDMAEN) trigger time, captured
 * BEFORE the transfer consumes aAdr/size. fromB != 0 is a B->A transfer;
 * 0 is the common A->B (memory -> PPU register) case. */
void ppudma_record_dma(int channel, int fromB, uint8_t aBank, uint16_t aAdr,
                       uint8_t bAdr, uint16_t size);

/* Count one architectural interrupt delivery into the CURRENT frame's tally.
 * `is_nmi` separates the vblank interrupt from raster IRQs.
 *
 * A guest whose per-frame work is gated by its interrupt handlers runs that
 * work once per interrupt, not once per host frame. When a host frame loop
 * delivers two of them, or none, the guest silently runs two of its own
 * frames or zero -- which shows up to a player as flicker and judder and to
 * an instrument, until now, as nothing at all. Per-frame counts make the
 * ratio a measurement. */
void ppudma_note_interrupt(int is_nmi);

/* Which half of its frame the host is in: 0 = the CPU half (run_frame), 1 =
 * the raster walk (draw_ppu_frame). A frame-model host can deliver a raster
 * IRQ from either half, and when both halves deliver one for the same
 * comparator the guest runs its per-frame work twice. Bucketing the tally by
 * phase is what turns "the IRQ count per frame is unstable" into "this half
 * is delivering the extra one". */
void ppudma_set_frame_phase(int in_raster_walk);

/* Snapshot the live PPU (reads g_ppu) once per frame. `frame` is the host
 * frame counter. Resets the per-frame DMA tally. */
void ppudma_frame_snapshot(int frame);

/* Serialize both rings as JSON object members (trailing comma, no enclosing
 * braces) for the post-mortem report. */
void ppudma_dump_json(FILE *f);

/* One retained per-frame snapshot, `back` frames before the newest (0 = the
 * most recent). Returns 0 past the end of the retained window.
 *
 * The ring always recorded this; reading it used to require the process to
 * die first, which is the wrong shape for "which PPU register oscillates
 * while the game runs". A per-frame flicker is a per-frame REGISTER history
 * question, and answering it from a couple of screenshots is guesswork. */
typedef struct {
  int      frame;
  uint8_t  inidisp;
  uint8_t  tm;         /* $212C main-screen designation  */
  uint8_t  ts;         /* $212D sub-screen designation   */
  uint8_t  bgmode;     /* $2105                          */
  uint16_t cgram_nz;
  uint32_t vram_nz;
  uint16_t dma_a2b;
  uint16_t s_reg;
  uint8_t  game_mode;
  uint16_t nmi_count;   /* vblank interrupts delivered during this frame */
  uint16_t irq_count;   /* raster interrupts delivered during this frame */
  uint16_t irq_cpu;     /* ...of which, during the host's CPU half */
  uint16_t irq_raster;  /* ...of which, during the host's raster walk */
} PpuFrameInfo;

int      ppudma_frame_at(uint64_t back, PpuFrameInfo *out);
uint64_t ppudma_frame_count(void);

#endif /* SNESRECOMP_PPU_DMA_TRACE_H */
