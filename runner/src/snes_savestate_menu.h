#ifndef SNES_SAVESTATE_MENU_H
#define SNES_SAVESTATE_MENU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Save-state slot browser — a full-screen overlay opened with Select + R.
 *
 * Ported from psxrecomp's runtime/src/psx_savestate_menu.c, with the state
 * machine moved down here rather than left in the host. psxrecomp keeps
 * savestate_menu_open/_slot as statics in main.cpp and drives them from the
 * event pump; that is workable for one host, but the SNES side has ~25 port
 * repos generated from one scaffold, and a state machine copied 25 times
 * cannot inherit a fix. So the framework owns everything except the two
 * things only a host can do: pump events and put pixels on the screen.
 *
 * A host wires it up in five calls:
 *
 *   per frame, on seat 0's input word, before RtlRunFrame:
 *       inputs = snes_savestate_menu_filter_guest_input(inputs);
 *       snes_savestate_menu_poll_open(inputs)
 *   per frame, after the PPU has composited, to keep the thumbnail fresh:
 *       snes_savestate_menu_note_frame(fb, w, h)
 *   then, if snes_savestate_menu_is_open(), a modal loop that pumps SDL and
 *   calls, each iteration:
 *       snes_savestate_menu_poll_nav(inputs, SDL_GetTicks())
 *       snes_savestate_menu_handle_key(keycode, repeat)   [on SDL_KEYDOWN]
 *       snes_savestate_menu_overlay_image(&px, &w, &h)    [to present]
 *
 * The guest is frozen while the menu is open — the host simply stops calling
 * RtlRunFrame. That is what psxrecomp does (savestate_menu_host_pause_loop),
 * and it is the only way "save right here" means a definite point in time.
 * Audio goes quiet for the duration, which is expected of a paused game.
 *
 * This module performs the save/load itself via RtlSaveLoad, so a host cannot
 * wire the overlay up and forget the part that does the work.
 */

#define SNES_SAVESTATE_SLOTS 12

/* Panel dimensions. 2x the SNES frame, so it overlays the game texture with
 * the same destination rect and needs no aspect correction of its own. */
#define SNES_SSM_W 512
#define SNES_SSM_H 448

int  snes_savestate_menu_is_open(void);
int  snes_savestate_menu_selected(void);

/* Mask out any button that was still held when the menu closed, until it is
 * released. Call once per frame on seat 0's word and pass the result to both
 * snes_savestate_menu_poll_open() and RtlRunFrame, so the press that closed
 * the menu neither reaches the game nor re-opens the menu. */
uint32_t snes_savestate_menu_filter_guest_input(uint32_t inputs);

/* Edge-detect the Select + R open gesture on the runner's input word
 * (bit 2 = Select, bit 11 = R). Returns 1 on the frame it opens. Safe to
 * call every frame; does nothing while already open. */
int  snes_savestate_menu_poll_open(uint32_t inputs);

/* Gamepad navigation while open: Up/Down pick a slot (with key repeat),
 * A loads, X saves, B closes. ticks_ms is a free-running millisecond clock. */
void snes_savestate_menu_poll_nav(uint32_t inputs, uint32_t ticks_ms);

/* Keyboard navigation while open. key is an SDL keycode; repeat is nonzero
 * for auto-repeat events, which are ignored. */
void snes_savestate_menu_handle_key(int key, int repeat);

void snes_savestate_menu_close(void);

/* Hand over the frame the player is looking at, in the PPU's native XRGB
 * layout, so a save taken from this point carries a thumbnail of it. Cheap
 * (one downsample to 128x112); ignored while the menu is open, so the
 * thumbnail is of the game and not of the overlay. */
void snes_savestate_menu_note_frame(const uint32_t *fb, int w, int h);

/* Optional: the picture the player actually sees, when a title composes its
 * own (e.g. a co-op renderer that draws the second player over the PPU
 * frame). Called while the game is paused, as a slot is written; fills `dst`
 * (capacity `cap` pixels, XRGB, rows packed) and sets its size. Returns 0 to
 * keep the PPU frame noted above. */
typedef int (*SnesSavestateComposeFn)(uint32_t *dst, size_t cap, int *w, int *h);
void snes_savestate_menu_set_compose(SnesSavestateComposeFn compose);

/* The rasterized panel, ARGB8888, SNES_SSM_W x SNES_SSM_H. Returns 0 and
 * nulls the outputs when the menu is closed. */
int  snes_savestate_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

/* Netplay's synchronized pause menu (snes_netplay.c) drives these on every
 * peer. On a guest the menu is a read-only mirror of the host's. */
void snes_savestate_menu_netplay_show(void);
void snes_savestate_menu_netplay_hide(void);
void snes_savestate_menu_netplay_cursor(int slot);
void snes_savestate_menu_netplay_status(const char *fmt, int slot);
/* Write the current state and thumbnail to `slot` on this peer. */
int  snes_savestate_menu_netplay_write_slot(int slot);

#ifdef __cplusplus
}
#endif

#endif /* SNES_SAVESTATE_MENU_H */
