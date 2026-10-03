#ifndef SNESRECOMP_VARIANT_SELECTOR_H
#define SNESRECOMP_VARIANT_SELECTOR_H

/*
 * In-game content-variant picker: Left/Right across variants, Up/Down across
 * save profiles, drawn as a host overlay on a screen the active variant
 * declares safe (SMW: the file select, GameMode $0100 == $08).
 *
 * Follows snes_savestate_menu's division of labour -- the framework owns the
 * state machine and the pixels; the host pumps input and presents -- so the
 * ~25 scaffolded ports inherit one picker rather than copying one each.
 *
 * A host wires it up in three calls:
 *
 *   per frame, on the pad word, before RtlRunFrame:
 *       inputs = snes_variant_selector_filter_input(inputs, g_ram);
 *   after RtlRunFrame:
 *       if (snes_variant_selector_take_switch(&variant, &profile)) { reboot }
 *   after the PPU has composited, into the presented ARGB frame:
 *       snes_variant_selector_draw(pixels, pitch_bytes, width, height);
 *
 * Behaviour:
 *   - Nothing happens (no masking, no drawing) unless more than one variant
 *     of the active variant's selector_group is REGISTERED, and the active
 *     variant's selector-screen predicate holds. A title with no variants
 *     installed is bit-identical to a build without this module.
 *   - Left/Right move the highlight across the group (available or not;
 *     an unavailable one shows its reason and cannot be confirmed).
 *   - While the highlight is not the active variant, Up/Down choose that
 *     variant's profile and Up/Down/Left/Right are MASKED from the guest so
 *     the game's own cursor stays put. While the highlight is the active
 *     variant every bit passes through untouched.
 *   - A / Start on a highlighted, available, non-active variant requests a
 *     switch (the host performs it). B / X / Y return the highlight home.
 */

#include <stddef.h>
#include <stdint.h>

#include "content_variant.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Seat-0 button bits in the host pad word (12 bits per seat). */
#define SNES_SEL_BTN_B      (1u << 0)
#define SNES_SEL_BTN_Y      (1u << 1)
#define SNES_SEL_BTN_SELECT (1u << 2)
#define SNES_SEL_BTN_START  (1u << 3)
#define SNES_SEL_BTN_UP     (1u << 4)
#define SNES_SEL_BTN_DOWN   (1u << 5)
#define SNES_SEL_BTN_LEFT   (1u << 6)
#define SNES_SEL_BTN_RIGHT  (1u << 7)
#define SNES_SEL_BTN_A      (1u << 8)
#define SNES_SEL_BTN_X      (1u << 9)

/* Is the picker showing this frame? (predicate true and >1 variant) */
int snes_variant_selector_visible(void);

/* Filter seat 0's word. `wram` is the 128 KiB guest WRAM the predicate reads. */
uint32_t snes_variant_selector_filter_input(uint32_t inputs, const uint8_t *wram);

/* A confirmed switch request, consumed by the call. */
int snes_variant_selector_take_switch(const SnesContentVariant **variant, int *profile);

/* Draw the strip into a little-endian ARGB8888 frame of width x height
 * pixels at `pitch_bytes`. No-op while not visible. */
void snes_variant_selector_draw(uint8_t *pixels, int pitch_bytes, int width, int height);

/* After a switch (or at boot) align the highlight with the active variant. */
void snes_variant_selector_sync(void);

/* Test seams. */
int snes_variant_selector_highlight_index(void);
int snes_variant_selector_highlight_profile(void);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_VARIANT_SELECTOR_H */
