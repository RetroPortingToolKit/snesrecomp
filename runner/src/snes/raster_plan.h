#ifndef SNESRECOMP_RASTER_PLAN_H
#define SNESRECOMP_RASTER_PLAN_H

#include "raster_jobs.h"

/* A Mode-1 row's visibility decisions. Inputs belong to one immutable captured
 * raster line and one view. No live PPU, guest memory or persistent state is
 * modified. Ranges are inclusive, as are the SNES window registers.
 *
 * Two hardware windows, two optional replacement color windows and the native
 * view edges yield at most eleven spans. Empty/inverted windows remain empty.
 */
typedef struct SnesRenderInterval { int left, right; } SnesRenderInterval;
typedef struct SnesRenderComposeSpan {
  unsigned begin, end;
  uint16_t main[4], sub[4]; /* all bits or zero: BG1, BG2, BG3, OBJ */
  bool color_window, margin;
} SnesRenderComposeSpan;
typedef struct SnesRenderComposePlan {
  SnesRenderComposeSpan spans[11];
  unsigned count;
} SnesRenderComposePlan;

static inline void SnesRenderPlanCut(int cuts[12], unsigned *count,
                                     int x, int first, int end) {
  if (x <= first || x >= end) return;
  unsigned i = 0;
  while (i < *count && cuts[i] < x) ++i;
  if (i < *count && cuts[i] == x) return;
  for (unsigned j = *count; j > i; --j) cuts[j] = cuts[j - 1];
  cuts[i] = x;
  ++*count;
}

static inline bool SnesRenderPlanCompose(SnesRenderComposePlan *out,
    const Ppu *p, const SnesRenderWindowPlan *windows, int first, unsigned width,
    const SnesRenderInterval *color_windows, unsigned color_count) {
  if (!out || !p || !windows || width > 0x10000 || color_count > 2 ||
      (color_count && !color_windows) || first < -0x10000 || first > 0x10000)
    return false;
  int end = first + (int)width;
  int cuts[12] = {first, end};
  unsigned count = width ? 2 : 1;
  for (unsigned i = 0; i < 2; ++i) {
    SnesRenderPlanCut(cuts, &count, windows->left[i], first, end);
    /* Explicit range bounds avoid overflowing on an arbitrary caller's input. */
    if (windows->right[i] >= first && windows->right[i] < end)
      SnesRenderPlanCut(cuts, &count, windows->right[i] + 1, first, end);
  }
  for (unsigned i = 0; i < color_count; ++i) {
    SnesRenderPlanCut(cuts, &count, color_windows[i].left, first, end);
    if (color_windows[i].right >= first && color_windows[i].right < end)
      SnesRenderPlanCut(cuts, &count, color_windows[i].right + 1, first, end);
  }
  SnesRenderPlanCut(cuts, &count, 0, first, end);
  SnesRenderPlanCut(cuts, &count, 256, first, end);
  out->count = 0;
  for (unsigned i = 0; i + 1 < count; ++i) {
    int x = cuts[i];
    unsigned mask = SnesRenderWindowMask(windows, x);
    SnesRenderComposeSpan *span = &out->spans[out->count++];
    span->begin = (unsigned)(x - first);
    span->end = (unsigned)(cuts[i + 1] - first);
    for (unsigned layer = 0; layer < 4; ++layer) {
      unsigned bit = 1u << (layer == 3 ? 4 : layer);
      span->main[layer] = (p->screenEnabled[0] & bit) &&
          !(p->screenWindowed[0] & mask & bit) ? 0xffffu : 0;
      span->sub[layer] = (p->screenEnabled[1] & bit) &&
          !(p->screenWindowed[1] & mask & bit) ? 0xffffu : 0;
    }
    span->color_window = (mask & 32) != 0;
    if (color_count) {
      span->color_window = false;
      for (unsigned k = 0; k < color_count; ++k)
        span->color_window |= x >= color_windows[k].left && x <= color_windows[k].right;
    }
    span->margin = x < 0 || x >= 256;
  }
  return true;
}

#endif
