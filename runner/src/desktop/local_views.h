#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "display_aspect.h"

/* Presentation surfaces, independent of the guest PPU's line buffer. A game
 * renders each seat from one completed simulation frame, then publishes both
 * together. Shared menus/cutscenes can pass NULL for right to duplicate left. */
enum {
  kSnesDesktopMaxViewWidth = 1024,
  kSnesDesktopMaxFrameWidth = 2 * kSnesDesktopMaxViewWidth,
};

/* Preserve each pane's native pixel proportions while widening its field.
 * target_aspect is per pane, including half the drawable for adaptive views. */
static inline SnesDisplayFrame SnesLocalViews_ComputePane(
    int native_width, int height, double native_aspect, double target_aspect) {
  if (native_width <= 0 || native_width > kSnesDesktopMaxViewWidth ||
      height <= 0 || height > 240 || !(native_aspect > 0))
    return (SnesDisplayFrame){0};
  int max_width = kSnesDesktopMaxViewWidth - ((kSnesDesktopMaxViewWidth - native_width) & 1);
  double maximum = native_aspect * max_width / native_width;
  if (!(target_aspect >= native_aspect)) target_aspect = native_aspect;
  if (target_aspect > maximum) target_aspect = maximum;
  int extra = (int)((native_width * target_aspect / native_aspect - native_width) / 2 + .5);
  return (SnesDisplayFrame){native_width + 2 * extra, extra, target_aspect};
}

static inline SnesDisplayViewport SnesLocalViews_Viewport(
    double pane_aspect, int drawable_width, int drawable_height) {
  return SnesDisplayAspect_FitViewport(
      2 * pane_aspect, drawable_width, drawable_height);
}

static inline bool SnesLocalViews_ComposeSideBySide(
    void *dst, size_t pitch, int pane_width, int height,
    const void *left, size_t left_pitch, const void *right, size_t right_pitch) {
  if (!dst || !left || pane_width <= 0 ||
      pane_width > kSnesDesktopMaxViewWidth || height <= 0 || height > 240)
    return false;
  size_t row_bytes = (size_t)pane_width * sizeof(uint32_t);
  if (!right) { right = left; right_pitch = left_pitch; }
  if (pitch < 2 * row_bytes || left_pitch < row_bytes || right_pitch < row_bytes)
    return false;
  for (int y = 0; y < height; ++y) {
    uint8_t *row = (uint8_t *)dst + (size_t)y * pitch;
    memcpy(row, (const uint8_t *)left + (size_t)y * left_pitch, row_bytes);
    memcpy(row + row_bytes, (const uint8_t *)right + (size_t)y * right_pitch, row_bytes);
  }
  return true;
}
