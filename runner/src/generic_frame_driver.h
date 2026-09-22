#ifndef SNESRECOMP_GENERIC_FRAME_DRIVER_H
#define SNESRECOMP_GENERIC_FRAME_DRIVER_H

/*
 * Game-agnostic frame driver for a recompiled program the host has no
 * hand-written frame model for.
 *
 * A port's own RtlGameInfo (SMW's RunOneFrameOfGame, MMX's task scheduler)
 * encodes what "one frame" means for THAT title. A content variant that runs
 * a different program -- Super Mario All-Stars inside the SMW executable, a
 * ROM hack whose main loop moved -- cannot use it. This driver is the shape
 * every scaffolded port starts from (tools/new_project/templates/game_rtl.c.in),
 * lifted into the engine so a variant needs no C at all:
 *
 *      boot from the reset vector read through the guest bus
 *      -> deliver NMI at the vblank edge, gated on NMITIMEN
 *      -> run the guest in slices until it parks or the frame's clock is out
 *      -> service a raster IRQ whenever the comparator asserts
 *      -> rasterize the field with HDMA per line
 *
 * Execution is LLE-first with AOT bounce through the ACTIVE program module's
 * dispatch table, so the same driver runs whichever module is selected.
 *
 * The driver holds one piece of host state, the resume PC. A variant switch
 * rebuilds the machine, so snes_generic_frame_driver_reset() must run before
 * the new session's first frame (RtlGameInfo.session_reset points at it).
 */

#include <stdint.h>

#include "common_cpu_infra.h"
#include "snes_boot_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boot policy (snes_boot_policy.h) applied on the first frame after reset. */
void snes_generic_frame_driver_set_boot_policy(const SnesGenericBootPolicy *policy);
const SnesGenericBootPolicy *snes_generic_frame_driver_boot_policy(void);

void snes_generic_frame_driver_run_frame(void);
void snes_generic_frame_driver_draw_ppu_frame(void);
void snes_generic_frame_driver_reset(void);

/* An RtlGameInfo wired to the functions above. `title` is used for save-slot
 * naming only; pass the variant id. The returned pointer stays valid for the
 * process; the struct is reconfigured in place on each call. */
const RtlGameInfo *snes_generic_frame_driver_game_info(const char *title);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_GENERIC_FRAME_DRIVER_H */
