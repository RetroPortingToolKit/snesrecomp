#ifndef SNESRECOMP_BEAM_FRAME_DRIVER_H
#define SNESRECOMP_BEAM_FRAME_DRIVER_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Beam-aligned frame driver for whole-program LLE ports.
 *
 * One host frame is one PPU field, cut where hardware raises NMI: the beam's
 * vblank edge (V=225). The CPU half runs the guest against the real beam,
 * taking every interrupt where it latches -- raster H/V matches and a
 * coprocessor holding the IRQ line (SuperFX, Cx4, SA-1) -- and lets hardware
 * time pass while the guest is parked, so the beam, APU and coprocessors get
 * the whole field. The render half rasterizes that field from the raster
 * journal (ppu.c), stepping the real HDMA unit per line.
 *
 * new_project scaffolds call these from src/game_rtl.c; a title replaces
 * them only when it needs something they do not do (see
 * docs/FRAME_MODEL_HOSTS.md). */

void snes_beam_frame_driver_run_frame(void);
void snes_beam_frame_driver_draw_ppu_frame(void);
/* Observe the display state that actually produced each visible line, after
 * raster journal writes and before H-blank HDMA. The PPU is read-only and the
 * callback must not advance the machine or change guest memory/registers.
 * `line` is 1..224, corresponding to output row line-1. No observer is kept
 * between calls; the regular draw entry point is identical to passing NULL. */
struct Ppu;
typedef void SnesBeamRasterObserver(const struct Ppu *ppu, unsigned line, void *context);
void snes_beam_frame_driver_draw_ppu_frame_observed(
    SnesBeamRasterObserver *observer, void *context);
/* MesenCE's interrupt and field rules (SNESRECOMP_BEAM_EXACT; default: on
 * with the master-clock APU timeline, RtlApuUseMasterClock). With them
 * snes_beam_frame_driver_run_frame
 *  - charges the interrupt sequence: 8 cycles, 60 master clocks, and a DRAM
 *    refresh point inside it stalls it;
 *  - takes the field's NMI after the first instruction that starts a cycle
 *    from V=225 H=6 on (snes/last_cycle.h), not after the first to end past
 *    V=225;
 *  - fires an NMI that is enabled inside vblank (the RDNMI flag still set)
 *    after the next instruction, in that field, not a field later;
 *  - runs the CPU in slices that end at each DRAM refresh point
 *    (snes_refresh_at_beam), so each stall falls between the right
 *    instructions, and keeps the beam where the CPU is after each slice. */
bool snes_beam_frame_driver_exact(void);

/* Host hooks for the exact rules, all optional: instrumentation and host
 * features, never needed for correctness. */
typedef struct SnesBeamFrameHooks {
  /* A field begins; `frame_end` is its V=225 (master clock). */
  void (*frame_start)(uint64_t frame_end, bool booting);
  /* An interrupt is about to be taken: its return frame is not pushed yet. */
  void (*interrupt_entry)(bool nmi);
  /* An NMI was taken: frame pushed, sequence charged, handler not yet run. */
  void (*nmi_taken)(void);
  /* The field's NMI is due at its start. True: the host took it itself (a
   * machine loaded inside its NMI handler resumes that handler instead). */
  bool (*field_nmi)(uint64_t frame_end);
  /* A CPU slice ended (refresh charged). True: the host took the slice's time
   * back, so it does not count against the field's slice budget. */
  bool (*slice_end)(void);
} SnesBeamFrameHooks;
void snes_beam_frame_driver_set_hooks(const SnesBeamFrameHooks *hooks);
/* The slice's deadline and its first stop that is not a DRAM refresh (0: no
 * slice runs), for host code that must not run past either. */
void snes_beam_frame_driver_deadlines(uint64_t *deadline, uint64_t *hard);
/* The last instruction the current slice started (0: none yet). */
uint32_t snes_beam_frame_driver_last_pc(void);

/* Forget the booted machine: the next frame starts from the reset vector. */
void snes_beam_frame_driver_reset(void);

/* Execution position is host state, separate from the Snes device blob. */
void snes_beam_frame_driver_get_position(uint32_t *pc, bool *wai);
void snes_beam_frame_driver_set_position(uint32_t pc, bool wai);
struct SaveLoadInfo;
/* Wire all of these hooks into a whole-program host's RtlGameInfo. The
 * guarded, build-specific chunk includes CPU registers/clocks and bridge
 * scheduling context; older device-only snapshots are safely rejected. */
void snes_beam_frame_driver_state_save(struct SaveLoadInfo *sli);
void snes_beam_frame_driver_state_load(struct SaveLoadInfo *sli, uint32_t version);
void snes_beam_frame_driver_state_loaded(uint32_t version);
const char *snes_beam_frame_driver_state_identity(void);
bool snes_beam_frame_driver_state_preflight(const void *data, size_t size);

#endif /* SNESRECOMP_BEAM_FRAME_DRIVER_H */
