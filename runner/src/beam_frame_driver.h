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
