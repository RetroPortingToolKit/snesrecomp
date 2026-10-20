#pragma once

/*
 * host_api.h — exported API symbols for dependent projects.
 *
 * This header declares symbols that the framework leaves visible so that
 * dependent projects (forks, ports, etc.) can interact with the host from
 * outside the library.  See host_main.h for the full contract.
 */

#include <stdbool.h>
#include <stdint.h>

/* SDL window and fullscreen-related types — included via the desktop host's
 * sdl_compat.h.  Dependent projects should ensure SDL is linked. */
#include "sdl_compat.h"

/* The SDL window managed by the framework.  Removing `static` from the
 * definition in host_main.c makes this visible through the extern declaration
 * below. */
extern SDL_Window *g_window;

/* Set when the fullscreen hotkey is pressed in-game.  The shutdown path
 * writes the config based on this flag so the launcher opens on the same
 * mode next run.  Removing `static` from the definition in host_main.c makes
 * this visible through the extern declaration below. */
extern bool g_fullscreen_changed;

/* Enables or disables fullscreen/borderless mode from dependent project code.
 * Clamps mode to {0,1}: 0 = windowed, 1 = borderless
 * (SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP).
 *
 * This function is the public API entry point for toggling fullscreen at
 * runtime (e.g. from an in-game menu). */
void snesrecomp_set_fullscreen(int mode);

/* The fullscreen mode currently in effect: 0 = windowed, 1 = borderless
 * (SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP).  A menu that offers a
 * fullscreen toggle needs to know which way round it is before it can
 * toggle it. */
int snesrecomp_get_fullscreen(void);

/* ── Live-applied presentation settings ───────────────────────────────────
 *
 * Each of these writes the config value, applies it to the running session
 * and marks the config for the shutdown write, so the value survives a
 * restart and the launcher opens on what the player left.  They mirror the
 * settings ApplyLiveSettings() applies when the launcher resumes, so none of
 * them needs a restart to take effect.
 */

/* Vertical sync at present time.  mode: 0 = off, 1 = on, 2 = adaptive
 * (late-swap tearing where the driver has it).  Clamped to {0,1,2}. */
void snesrecomp_set_vsync(int mode);

/* Average each presented frame with the previous one, so alternate-frame
 * flicker reads as translucency.  enabled: 0 = off, nonzero = on. */
void snesrecomp_set_frame_blend(int enabled);

/* Frames of local input-latency hiding.  frames: 0 = off.  Negative values
 * are clamped to 0. */
void snesrecomp_set_run_ahead(int frames);

/* Window scale factor.  scale: 1..kMaxWindowScale (10).  Values outside that
 * range are clamped.  Does nothing while the window is fullscreen. */
void snesrecomp_set_window_scale(int scale);