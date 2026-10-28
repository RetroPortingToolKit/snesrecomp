#include "snes/raster_plan.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint32_t seed = 0x974e134bu;
static uint32_t random32(void) {
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
  return seed;
}
static bool reference_window(const Ppu *p, unsigned layer, int x,
                              const SnesRenderWindowPlan *geometry) {
  unsigned flags = (p->windowsel >> (layer * 4)) & 15;
  bool a = (x >= geometry->left[0] && x <= geometry->right[0]) != ((flags & 1) != 0);
  bool b = (x >= geometry->left[1] && x <= geometry->right[1]) != ((flags & 4) != 0);
  if (!(flags & 2)) return (flags & 8) && b;
  if (!(flags & 8)) return a;
  switch ((p->wbgobjlog >> (layer * 2)) & 3) {
    case 0: return a || b;
    case 1: return a && b;
    case 2: return a != b;
    default: return a == b;
  }
}
int main(void) {
  const unsigned widths[] = {0, 1, 17, 256, 342, 682, 1536};
  for (unsigned sample = 0; sample < 4096; ++sample) {
    unsigned width = widths[sample % 7];
    int first = -(int)(random32() % 768);
    Ppu p = {0};
    p.windowsel = random32() & 0xffffff;
    p.wbgobjlog = (uint16_t)random32();
    p.screenEnabled[0] = (uint8_t)random32(); p.screenEnabled[1] = (uint8_t)random32();
    p.screenWindowed[0] = (uint8_t)random32(); p.screenWindowed[1] = (uint8_t)random32();
    int edges[4];
    for (unsigned i = 0; i < 4; ++i) edges[i] = (int)(random32() % 1800) - 900;
    if (!(sample % 17)) edges[0] = INT_MIN;
    if (!(sample % 19)) edges[3] = INT_MAX;
    SnesRenderWindowPlan windows = SnesRenderWindows(&p, edges[0], edges[1], edges[2], edges[3]);
    SnesRenderInterval color[2];
    unsigned color_count = sample % 3;
    for (unsigned i = 0; i < color_count; ++i) {
      color[i].left = (int)(random32() % 1800) - 900;
      color[i].right = (int)(random32() % 1800) - 900;
    }
    SnesRenderComposePlan plan;
    assert(SnesRenderPlanCompose(&plan, &p, &windows, first, width, color, color_count));
    assert(plan.count <= 11);
    unsigned cursor = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
      const SnesRenderComposeSpan *span = &plan.spans[i];
      assert(span->begin == cursor && span->end > cursor && span->end <= width);
      for (unsigned sx = span->begin; sx < span->end; ++sx) {
        int x = first + (int)sx;
        for (unsigned layer = 0; layer < 4; ++layer) {
          unsigned hw = layer == 3 ? 4 : layer, bit = 1u << hw;
          bool inside = reference_window(&p, hw, x, &windows);
          for (unsigned sub = 0; sub < 2; ++sub) {
            uint16_t expected = (p.screenEnabled[sub] & bit) &&
                (!(p.screenWindowed[sub] & bit) || !inside) ? 0xffffu : 0;
            assert((sub ? span->sub[layer] : span->main[layer]) == expected);
          }
        }
        bool inside = reference_window(&p, 5, x, &windows);
        if (color_count) {
          inside = false;
          for (unsigned k = 0; k < color_count; ++k)
            inside |= x >= color[k].left && x <= color[k].right;
        }
        assert(span->color_window == inside);
        assert(span->margin == (x < 0 || x >= 256));
      }
      cursor = span->end;
    }
    assert(cursor == width);
    assert(!SnesRenderPlanCompose(NULL, &p, &windows, first, width, color, color_count));
    assert(!SnesRenderPlanCompose(&plan, &p, &windows, first, width, color, 3));
    assert(!SnesRenderPlanCompose(&plan, &p, &windows, first, 0x10001, color, 0));
  }
  puts("4096 ROW PLANS MATCH SCALAR WINDOW/VISIBILITY CONTRACTS");
  return 0;
}
