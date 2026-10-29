#ifndef SNESRECOMP_PPU_MODE7_LINE_H
#define SNESRECOMP_PPU_MODE7_LINE_H

#include "raster_jobs.h"

/* Image-only replacement for the legacy Mode 7 line service. The caller has
 * prepared m7startX/Y and evaluated live OAM. No device, persistent cache,
 * status, register or background-buffer writes belong to this job.
 *
 * Keep the legacy sampler's arithmetic and mosaic convention (including C's
 * truncation toward zero in negative margin columns). EXTBG shares the same
 * affine transform, but can have a different horizontal mosaic setting. */
static void ppu_mode7_row(Ppu *p, int left, int right, unsigned layer,
                           uint8_t *row) {
  const uint16_t *vram = PpuRenderVram(p);
  const int size = PPU_mosaicEnabled(p, layer) ? PPU_mosaicSize(p) : 1;
  const int dx = PPU_m7xFlip(p) ? -p->m7matrix[0] : p->m7matrix[0];
  const int dy = PPU_m7xFlip(p) ? -p->m7matrix[2] : p->m7matrix[2];
  int source = size > 1 ? left - left % size : left;
  int rx = PPU_m7xFlip(p) ? 255 - source : source;
  int ax = p->m7startX + p->m7matrix[0] * rx;
  int ay = p->m7startY + p->m7matrix[2] * rx;
  const bool large = PPU_m7largeField(p), fill = PPU_m7charFill(p);
  for (int x = left; x < right;) {
    int width = size > 1 ? (source < 0 ? source - x + 1 : size - (x - source)) : 1;
    if (width > right - x) width = right - x;
    int tx = ax >> 8, ty = ay >> 8;
    bool outside = large && (tx < 0 || tx >= 1024 || ty < 0 || ty >= 1024);
    tx &= 1023; ty &= 1023;
    unsigned tile = outside ? 0 : vram[(ty >> 3) * 128 + (tx >> 3)] & 255;
    uint8_t pixel = outside && !fill ? 0 :
        vram[tile * 64 + (ty & 7) * 8 + (tx & 7)] >> 8;
    for (int i = 0; i < width; ++i) row[x + i - left] = pixel;
    x += width;
    int next = size > 1 ? x - x % size : x;
    ax += dx * (next - source); ay += dy * (next - source);
    source = next;
  }
}

/* The legacy priority lists reduce to these three background ranks and four
 * existing sprite ranks. A transparent texel never claims a layer. */
static inline uint16_t ppu_mode7_top(unsigned enabled, unsigned bg0,
                                    unsigned bg1, uint16_t object, bool ext) {
  uint16_t top = 0x0500; /* backdrop */
  if ((enabled & 1) && bg0) top = 0x5000 | bg0;
  if (ext && (enabled & 2) && (bg1 & 127)) {
    uint16_t b = ((bg1 & 128) ? 0x9100 : 0x1100) | (bg1 & 127);
    if (b > top) top = b;
  }
  unsigned rank = object >> 12;
  if ((enabled & 16) && (object & 255) &&
      (rank == 2 || rank == 6 || rank == 10 || rank == 14) &&
      (object & 0xf000) > (top & 0xf000))
    top = (object & 0xf0ff) | ((object & 255) < 192 ? 0x0600 : 0x0400);
  return top;
}

static inline unsigned ppu_mode7_color(Ppu *p, uint16_t pixel, int x,
                                        bool direct) {
  unsigned layer = (pixel >> 8) & 15, index = pixel & 255;
  if (direct && layer == 0)
    return ((index & 7) << 2) | ((index & 56) << 4) | ((index & 192) << 7);
  if (layer == 4 || layer == 6)
    return PpuPixelColor(p, p->objBuffer.data[x + kPpuExtraLeftRight],
                         x + kPpuExtraLeftRight) & 0x7fff;
  return p->cgram[index] & 0x7fff;
}

static inline bool ppu_mode7_window_test(unsigned mode, bool inside) {
  return mode == 3 || (mode == 2 && inside) || (mode == 1 && !inside);
}

static void ppu_draw_mode7_line_native(Ppu *p, int line) {
  const int left = -p->extraLeftCur, right = 256 + p->extraRightCur;
  uint8_t *dst = p->renderBuffer + (line - 1) * p->renderPitch +
      (p->extraLeftRight + left) * 4;
  if (PPU_forcedBlank(p)) {
    memset(dst, 0, (right - left) * 4);
    return;
  }
  const bool ext = PPU_m7extBg(p), direct = PPU_directColor(p);
  uint8_t rows[2][kPpuBufWidth];
  unsigned screens = (p->screenEnabled[0] | p->screenEnabled[1]) & g_snes_ppu_dbg_layer_mask;
  if (screens & 1) ppu_mode7_row(p, left, right, 0, rows[0]);
  if (ext && (screens & 2)) {
    if (!!PPU_mosaicEnabled(p, 0) == !!PPU_mosaicEnabled(p, 1) && (screens & 1))
      memcpy(rows[1], rows[0], right - left);
    else ppu_mode7_row(p, left, right, 1, rows[1]);
  }
  uint8_t brightness[32];
  for (unsigned c = 0; c < 32; ++c)
    brightness[c] = ((c << 3) | (c >> 2)) * PPU_brightness(p) / 15;
  SnesRenderWindowPlan windows = SnesRenderWindows(p, p->window1left,
      p->window1right, p->window2left, p->window2right);
  /* Window geometry has at most four internal boundaries. Resolve all six
   * layer masks and color decisions once per constant span, not per pixel. */
  int edges[6] = {left, right}, count = 2;
  for (unsigned i = 0; i < 2; ++i) {
    int boundary[2] = {windows.left[i], windows.right[i] + 1};
    for (unsigned j = 0; j < 2; ++j) {
      int b = boundary[j];
      if (b <= left || b >= right) continue;
      int at = 0;
      while (at < count && edges[at] < b) ++at;
      if (at < count && edges[at] == b) continue;
      for (int k = count; k > at; --k) edges[k] = edges[k - 1];
      edges[at] = b; ++count;
    }
  }
  const bool add = PPU_addSubscreen(p), subtract = PPU_subtractColor(p);
  const bool half = PPU_halfColor(p), pseudo = PPU_pseudoHires(p);
  const unsigned fixed = PPU_fixedColorR(p) | (PPU_fixedColorG(p) << 5) |
                         (PPU_fixedColorB(p) << 10);
  for (int span = 0; span + 1 < count; ++span) {
    unsigned mask = SnesRenderWindowMask(&windows, edges[span]);
    unsigned main = p->screenEnabled[0] & g_snes_ppu_dbg_layer_mask & ~(mask & p->screenWindowed[0]);
    unsigned sub = p->screenEnabled[1] & g_snes_ppu_dbg_layer_mask & ~(mask & p->screenWindowed[1]);
    bool inside = (mask & 32) != 0;
    bool clip = ppu_mode7_window_test(PPU_clipMode(p), inside);
    unsigned math = ppu_mode7_window_test(PPU_preventMathMode(p), inside) ?
        0 : PPU_mathEnabled(p);
    for (int x = edges[span]; x < edges[span + 1]; ++x) {
      unsigned bg0 = (screens & 1) ? rows[0][x - left] : 0;
      unsigned bg1 = (ext && (screens & 2)) ? rows[1][x - left] : 0;
      uint16_t object = p->objBuffer.data[x + kPpuExtraLeftRight];
      uint16_t top = ppu_mode7_top(main, bg0, bg1, object, ext);
      unsigned layer = (top >> 8) & 15;
      unsigned color = clip ? 0 : ppu_mode7_color(p, top, x, direct);
      bool enabled = layer < 6 && (math & (1u << layer));
      if (enabled) {
        uint16_t second = (add || pseudo) ? ppu_mode7_top(sub, bg0, bg1, object, ext) : 0x0500;
        bool backdrop = ((second >> 8) & 15) == 5;
        unsigned other = add && !backdrop ? ppu_mode7_color(p, second, x, direct) : fixed;
        unsigned result = 0;
        for (unsigned component = 0; component < 3; ++component) {
          int a = (color >> (component * 5)) & 31;
          int b = (other >> (component * 5)) & 31;
          int value = subtract ? a - b : a + b;
          if (half && (!backdrop || !add)) value >>= 1;
          if (value < 0) value = 0;
          if (value > 31) value = 31;
          result |= (unsigned)value << (component * 5);
        }
        color = result;
      }
      /* The legacy caller promises B,G,R,0 bytes, independent of host byte
       * order and without an alignment requirement on the supplied buffer. */
      dst[0] = brightness[(color >> 10) & 31];
      dst[1] = brightness[(color >> 5) & 31];
      dst[2] = brightness[color & 31];
      dst[3] = 0;
      dst += 4;
    }
  }
}
#endif
