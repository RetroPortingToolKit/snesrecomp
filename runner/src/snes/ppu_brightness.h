#ifndef SNESRECOMP_PPU_BRIGHTNESS_H
#define SNESRECOMP_PPU_BRIGHTNESS_H

#include <stdint.h>

/* One 5-bit colour component at INIDISP brightness l (0-15), as 8 bits.
 * Hardware scales in the 5-bit domain with rounding, then expands to 8 bits
 * (bsnes lightTable: round(c * l / 15), then (c << 3) | (c >> 2)). Expanding
 * first and truncating left fades a few levels off. The GLSL path in
 * desktop/hd_gpu.c computes the same expression.
 *
 * g_ppu_brightness_truncate: scale with truncation instead, as MesenCE does
 * (SnesPpu::ApplyBrightness: c * l / 15), so a parity run against it can tell
 * the brightness rule from any other difference in the picture. Set it before
 * the first frame (the optimized renderer caches its table per brightness);
 * the GLSL path does not follow it. */
extern int g_ppu_brightness_truncate;
static inline uint8_t ppu_brightness_level(unsigned c, unsigned l) {
  unsigned v = g_ppu_brightness_truncate ? c * l / 15u : (2u * c * l + 15u) / 30u;
  return (uint8_t)((v << 3) | (v >> 2));
}

#endif
