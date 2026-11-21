/* A clipping window hides output; it must not move the source mosaic grid.
 * The reference is the unwindowed non-mosaic source at each block's origin.
 * This keeps the first sample observable even when that origin is hidden. */
#define main composition_fixture_main
#include "ppu_composition_regression_test.c"
#undef main

static void setup(Ppu *p, int layer, int bits, int flip, int scroll) {
  ppu_reset(p);
  memset(p->vram, 0, sizeof(p->vram));
  p->bgmode = 1;
  p->screenEnabled[0] = p->screenEnabled[1] = (uint8_t)(1u << layer);
  p->bgXsc[layer] = 1; /* two horizontally adjacent tile maps */
  p->bgTileAdr = (uint16_t)(2u << (4 * layer));
  p->hScroll[layer] = (uint16_t)scroll;
  for (unsigned i = 0; i < 2048; ++i)
    p->vram[i] = (uint16_t)(((i * 7 + 3) % 64) | ((i % 8) << 10) |
                           ((flip & 1) ? 0x4000 : 0) |
                           ((flip & 2) ? 0x8000 : 0) |
                           ((i & 1) ? 0x2000 : 0));
  for (unsigned tile = 0; tile < 64; ++tile) {
    for (unsigned row = 0; row < 8; ++row) {
      for (unsigned x = 0; x < 8; ++x) {
        unsigned color = 1 + ((tile + x * 3) % ((1u << bits) - 1));
        for (int plane = 0; plane < bits; ++plane) {
          unsigned word = 0x2000 + tile * bits * 4 + row + (plane / 2) * 8;
          if (color & (1u << plane))
            p->vram[word] |= (uint16_t)(1u << (7 - x + (plane & 1) * 8));
        }
      }
    }
  }
}

static void draw(Ppu *p, PpuPixelPrioBufs *out, int layer, int sub, int mosaic) {
  memset(out, 0, sizeof(*out));
  if (layer == 0) {
    if (mosaic) PpuDrawBackground_4bpp_mosaic(p, out, 1, sub, layer, 0xc000, 0x8000);
    else PpuDrawBackground_4bpp(p, out, 1, sub, layer, 0xc000, 0x8000);
  } else {
    if (mosaic) PpuDrawBackground_2bpp_mosaic(p, out, 1, sub, layer, 0xc000, 0x8000);
    else PpuDrawBackground_2bpp(p, out, 1, sub, layer, 0xc000, 0x8000);
  }
}

int main(void) {
  static const unsigned windows[][2] = {
    {0, 0}, {0, 2}, {5, 15}, {14, 34}, {127, 128}, {255, 255}, {0, 255}
  };
  static const unsigned scrolls[] = {0, 7, 255};
  Ppu *p = ppu_init();
  PpuPixelPrioBufs reference, result;
  unsigned cases = 0, failures = 0;
  if (!p) return 2;
  test_shadow_active = false;
  for (int layer = 0; layer <= 2; layer += 2)
  for (int sub = 0; sub < 2; ++sub)
  for (int size = 1; size <= 16; ++size)
  for (int wide = 0; wide < 2; ++wide)
  for (int flip = 0; flip < 4; ++flip)
  for (unsigned scroll = 0; scroll < sizeof(scrolls) / sizeof(scrolls[0]); ++scroll)
  for (unsigned window = 0; window < sizeof(windows) / sizeof(windows[0]); ++window)
  for (int invert = 0; invert < 2; ++invert) {
    int extra = wide ? 32 : 0;
    setup(p, layer, layer == 0 ? 4 : 2, flip, scrolls[scroll]);
    /* The wider source also covers a mosaic origin to the left of the
     * candidate's first visible margin column. */
    PpuSetExtraSpaceCentered(p, 64);
    draw(p, &reference, layer, sub, 0);
    PpuSetExtraSpaceCentered(p, extra);
    p->mosaic = (uint8_t)(((size - 1) << 4) | (1u << layer));
    for (int i = 0; i < 256; ++i) p->mosaicModulo[i] = (uint8_t)(i - i % size);
    p->screenWindowed[sub] = (uint8_t)(1u << layer);
    p->windowsel = (uint32_t)(2 + invert) << (layer * 4);
    p->window1left = (uint8_t)windows[window][0];
    p->window1right = (uint8_t)windows[window][1];
    PpuWindows win;
    PpuWindows_Calc(&win, p, layer, 1);
    draw(p, &result, layer, sub, 1);
    ++cases;
    for (size_t span = 0; span < win.nr; ++span) {
      for (int x = win.edges[span]; x < win.edges[span + 1]; ++x) {
        int remainder = x % size;
        if (remainder < 0) remainder += size;
        int origin = x - remainder;
        PpuZbufType expected = (win.bits & (1u << span)) ? 0 :
            reference.data[kPpuLineBorder + origin];
        PpuZbufType actual = result.data[kPpuLineBorder + x];
        if (actual != expected) {
          if (failures < 8)
            fprintf(stderr, "mosaic window mismatch layer=%d sub=%d size=%d "
                    "wide=%d flip=%d scroll=%u window=%u invert=%d x=%d "
                    "source=%d actual=%04x expected=%04x\n", layer, sub, size,
                    wide, flip, scrolls[scroll], window, invert, x, origin,
                    (unsigned)actual, (unsigned)expected);
          ++failures;
          break;
        }
      }
    }
  }
  ppu_free(p);
  if (failures) {
    fprintf(stderr, "ppu_mosaic_window_test: %u failures across %u cases\n", failures, cases);
    return 1;
  }
  printf("ppu_mosaic_window_test: PASS (%u cases)\n", cases);
  return 0;
}
