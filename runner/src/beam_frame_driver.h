#ifndef SNESRECOMP_BEAM_FRAME_DRIVER_H
#define SNESRECOMP_BEAM_FRAME_DRIVER_H

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
/* Forget the booted machine: the next frame starts from the reset vector. */
void snes_beam_frame_driver_reset(void);

#endif /* SNESRECOMP_BEAM_FRAME_DRIVER_H */
