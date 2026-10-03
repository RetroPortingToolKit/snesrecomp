/* ROM-free, analytic scenes exercising real scanout and the HD side surface. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/mode7_hd.h"

Snes *g_snes;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
uint16_t WsShadowTile(int layer, int x, uint32_t y, uint16_t tile) {
  (void)layer; (void)x; (void)y; return tile;
}
bool WsShadowLayerActive(int layer) { (void)layer; return false; }
uint32_t WsShadowWorldX(int layer) { (void)layer; return 0; }
uint32_t WsShadowPresentWorldY(int layer, int x) { (void)layer; (void)x; return 0; }
uint32_t WsShadowScrollY(int layer) { (void)layer; return 0; }
void WsShadowOnVramWrite(uint16_t address, uint16_t value) { (void)address; (void)value; }

#define CHECK(test) do { if (!(test)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)

enum { HEIGHT = 4, SCALE = 4, PITCH = kPpuBufWidth * SCALE + 8 };
static uint32_t native[kPpuBufWidth * HEIGHT];
static uint32_t hd[1 + PITCH * HEIGHT * SCALE + 1];
static Ppu p;
static uint32_t rgb(unsigned index) {
  unsigned colour = p.cgram[index];
  unsigned r = colour & 31, g = (colour >> 5) & 31, b = (colour >> 10) & 31;
  return ((r << 3) | (r >> 2)) << 16 |
         ((g << 3) | (g >> 2)) << 8 | ((b << 3) | (b >> 2));
}
static void setup(void) {
  memset(&p, 0, sizeof(p));
  ppu_reset(&p);
  p.inidisp = 15;
  p.bgmode = 7;
  p.screenEnabled[0] = 1;
  p.m7matrix[0] = p.m7matrix[3] = 512;
  for (int i = 0; i < 128; ++i) p.oam[2 * i] = 0xf000;
  for (int i = 0; i < 0x4000; ++i) p.vram[i] = 1;
  for (unsigned i = 0; i < 64; ++i) p.vram[64 + i] |= (i + 1) << 8;
  for (unsigned i = 0; i < 256; ++i) p.cgram[i] = (uint16_t)((i & 31) | ((i >> 5) << 5));
  for (unsigned i = 0; i < countof(hd); ++i) hd[i] = 0xdeadbeef;
  memset(native, 0, sizeof(native));
  PpuBeginDrawing(&p, (uint8_t *)native, kPpuBufWidth * 4, kPpuRenderFlags_NewRenderer);
  ppu_runLine(&p, 0);
}
static void bind(unsigned width, unsigned scale) {
  CHECK(PpuBindMode7HdSurface(&p, hd + 1, sizeof(hd) - 8, PITCH * 4, width, HEIGHT, scale));
}
static void check_guards(unsigned width, unsigned scale, unsigned rows) {
  CHECK(hd[0] == 0xdeadbeef && hd[countof(hd) - 1] == 0xdeadbeef);
  for (unsigned y = 0; y < rows * scale; ++y)
    for (unsigned x = width * scale; x < PITCH; ++x)
      CHECK(hd[1 + y * PITCH + x] == 0xdeadbeef);
  for (unsigned i = 1 + rows * scale * PITCH; i + 1 < countof(hd); ++i)
    CHECK(hd[i] == 0xdeadbeef);
}
static void test_detail_and_isolation(void) {
  setup();
  ppu_runLine(&p, 1);
  uint32_t before[kPpuBufWidth];
  memcpy(before, native, sizeof(before));
  Ppu state = p;
  bind(256, 2);
  ppu_runLine(&p, 1);
  CHECK(!memcmp(before, native, sizeof(before)));
  CHECK(!memcmp(&p.inidisp, &state.inidisp, PPU_SAVESTATE_REGS_SIZE));
  CHECK(!memcmp(&p.cgram, &state.cgram, PPU_SAVESTATE_MEM_SIZE));
  CHECK(!memcmp(p.bgBuffers, state.bgBuffers, sizeof(p.bgBuffers)));
  CHECK(!memcmp(&p.objBuffer, &state.objBuffer, sizeof(p.objBuffer)));
  CHECK(p.timeOver == state.timeOver && p.rangeOver == state.rangeOver);
  /* Matrix 2x2 samples even texels natively; 2x HD recovers odd columns/rows. */
  CHECK(hd[1] == rgb(17)); CHECK(hd[2] == rgb(18));
  CHECK(hd[1 + PITCH] == rgb(25)); CHECK(hd[2 + PITCH] == rgb(26));
  CHECK(hd[1] != hd[2] && hd[1] != hd[1 + PITCH]);
  check_guards(256, 2, 1);
  CHECK(PpuBindMode7HdSurface(&p, NULL, 0, 0, 0, 0, 0));
  hd[1] = 0x12345678;
  ppu_runLine(&p, 1);
  CHECK(hd[1] == 0x12345678 && !memcmp(before, native, sizeof(before)));
}
static void test_fallbacks_and_capacity(void) {
  for (unsigned kind = 0; kind < 9; ++kind) {
    setup(); bind(256, kind == 0 ? 1 : 3);
    if (kind == 1) p.bgmode = 1;
    if (kind == 2) p.inidisp |= 128;
    if (kind == 3) { p.mosaic = 0x11; ppu_runLine(&p, 0); }
    if (kind == 4) p.setini = 0x40;
    if (kind == 5) p.cgwsel = 1;
    if (kind == 6) p.setini = 1;
    if (kind == 7) p.setini = 8;
    if (kind == 8) p.inidisp = 0;
    ppu_runLine(&p, 1);
    unsigned scale = kind == 0 ? 1 : 3;
    for (unsigned y = 0; y < scale; ++y)
      for (unsigned x = 0; x < 256 * scale; ++x)
        CHECK(hd[1 + y * PITCH + x] == native[x / scale]);
    check_guards(256, scale, 1);
  }
  setup(); bind(256, 2);
  PpuMode7HdSurface binding = p.mode7Hd;
  CHECK(!PpuBindMode7HdSurface(&p, hd + 1, 32, PITCH * 4, 256, HEIGHT, 2));
  CHECK(!PpuBindMode7HdSurface(&p, hd + 1, sizeof(hd), PITCH * 4, 255, HEIGHT, 2));
  CHECK(!PpuBindMode7HdSurface(&p, hd + 1, sizeof(hd), SIZE_MAX - 3, 256, HEIGHT, 2));
  CHECK(!PpuBindMode7HdSurface(&p, hd + 1, sizeof(hd), PITCH * 4, 256, HEIGHT, 5));
  CHECK(!memcmp(&binding, &p.mode7Hd, sizeof(binding)));
  ppu_reset(&p);
  CHECK(!memcmp(&binding, &p.mode7Hd, sizeof(binding)));
  /* Overscan lines outside the explicitly bound surface cannot write. */
  ppu_runLine(&p, 0);
  PpuSetExtraSpace(&p, 16);
  ppu_runLine(&p, 1);
  for (unsigned y = 0; y < 2; ++y)
    for (unsigned x = 0; x < 512; ++x) CHECK(hd[1 + y * PITCH + x] == 0);
  check_guards(256, 2, 1);
}
static void sprite(unsigned priority) {
  p.obsel = 3; /* Sprite tile data at word $6000, outside the Mode 7 map. */
  p.oam[0] = 0;
  p.oam[1] = (uint16_t)(priority << 12);
  for (unsigned y = 0; y < 8; ++y) p.vram[0x6000 + y] = 0x00ff;
  p.cgram[129] = 0x7c00;
}
static void test_transparency_and_sprites(void) {
  setup(); bind(256, 2); sprite(0);
  p.screenEnabled[0] = 17;
  p.vram[64 + 17] &= 255; /* Texel x=1,y=2, between native samples. */
  ppu_runLine(&p, 1);
  CHECK(native[0] == rgb(17));
  CHECK(hd[1] == rgb(17) && hd[2] == rgb(129));
  sprite(1);
  ppu_runLine(&p, 1);
  CHECK(hd[1] == rgb(129) && hd[2] == rgb(129));
}
static void test_composition_and_raster_state(void) {
  /* At integer sample locations, this integral matrix matches the hardware.
   * Exercise main/sub selection, all colour windows, math modes, priorities,
   * flips and overflow with the existing compositor as an independent oracle. */
  for (unsigned seed = 0; seed < 512; ++seed) {
    setup(); PpuSetExtraSpace(&p, 16); bind(288, 2);
    p.inidisp = (uint8_t)(1 + seed % 15);
    sprite((seed >> 4) & 3);
    p.screenEnabled[0] = (seed & 1) ? 17 : 1;
    p.screenEnabled[1] = (seed & 2) ? 17 : 1;
    p.cgwsel = (uint8_t)((seed & 3) << 6 | ((seed >> 2) & 3) << 4 | (seed & 4 ? 2 : 0));
    p.cgadsub = (uint8_t)(0x31 | (seed & 8 ? 64 : 0) | (seed & 16 ? 128 : 0));
    p.fixedColor = 0x4a3b;
    p.windowsel = (2u << 20) | (2u << 16) | 2;
    p.window1left = 2; p.window1right = 120;
    p.screenWindowed[0] = seed & 32 ? 17 : 0;
    p.screenWindowed[1] = seed & 64 ? 17 : 0;
    p.m7sel = (uint8_t)((seed & 3) | (seed & 128 ? 0x80 : 0) | (seed & 256 ? 0x40 : 0));
    ppu_runLine(&p, 1);
    for (unsigned x = 0; x < 288; ++x) {
      if (hd[1 + 2 * x] != native[x]) {
        fprintf(stderr, "composition seed=%u x=%u hd=%06x native=%06x\n",
                seed, x, hd[1 + 2 * x], native[x]);
        exit(1);
      }
    }
  }
  setup(); bind(256, 2);
  ppu_runLine(&p, 1);
  uint32_t first = hd[1];
  p.m7matrix[6] = 3;
  p.cgram[39] = 0x03e0;
  ppu_runLine(&p, 2);
  CHECK(hd[1] == first && hd[1 + 2 * PITCH] == rgb(39));
  /* Alternate picture memory is part of presentation, too. */
  static uint16_t picture[0x8000];
  memcpy(picture, p.vram, sizeof(picture));
  picture[64 + 38] = (uint16_t)(1 | (7 << 8));
  p.renderVram = picture;
  ppu_runLine(&p, 2);
  CHECK(hd[1 + 2 * PITCH] == rgb(7));
}
static void test_sampler(void) {
  setup();
  SnesMode7HdTransform t = SnesMode7HdMakeTransform(p.m7matrix, 0, 1);
  CHECK(SnesMode7HdSample(&t, p.vram, -0.5, 0) == 24);
  t.control = 0x80;
  CHECK(SnesMode7HdSample(&t, p.vram, -0.5, 0) == 0);
  p.vram[23] |= 99 << 8;
  t.control = 0xc0;
  CHECK(SnesMode7HdSample(&t, p.vram, -0.5, 0) == 99);
  t.origin_x = NAN;
  CHECK(SnesMode7HdSample(&t, p.vram, 0, 0) == 0);
  t.origin_x = 1e100; t.control = 0;
  CHECK(SnesMode7HdSample(&t, p.vram, 0, 0) > 0);
  /* Fractional products survive instead of losing six bits at each term. */
  p.m7matrix[0] = 257; p.m7matrix[1] = 33; p.m7matrix[6] = 1;
  t = SnesMode7HdMakeTransform(p.m7matrix, 0, 1);
  CHECK(fabs(t.origin_x - 290.0 / 256) < 1e-12);
  CHECK(SnesMode7HdSign13(0x1fff) == -1);
}
static void test_bounded_sampler(void) {
  setup();
  for (unsigned i = 0; i < 0x8000; ++i) p.vram[i] = (uint16_t)(i * 251u + i / 17u);
  static const double coordinates[] = {-2147483646.75, -1073741824.25,
      -1024.125, -1.5, -1, -0.125, -0.0, 0.125, 7.999, 1023.999, 1024, 2147483646.75};
  for (unsigned i = 0; i < countof(coordinates); ++i)
    for (unsigned j = 0; j < countof(coordinates); ++j) {
      double x = coordinates[i], y = coordinates[j];
      CHECK(SnesMode7HdSpanFits(x, y, x, y, INT_MAX));
      int tx = SnesMode7HdFloorInt(x), ty = SnesMode7HdFloorInt(y);
      CHECK(tx == floor(x) && ty == floor(y));
      SnesMode7HdTexel texel = {floor(x), floor(y)};
      for (unsigned control = 0; control < 256; control += 64)
        for (int tile = -1; tile <= 257; tile += 129)
          CHECK(SnesMode7HdFetchInt((uint8_t)control, p.vram, tx, ty, tile) ==
                SnesMode7HdFetch((uint8_t)control, p.vram, texel, tile));
    }
  CHECK(!SnesMode7HdSpanFits(0, 0, INT_MAX, 0, INT_MAX));
  CHECK(!SnesMode7HdSpanFits(INT_MIN, 0, 0, 0, INT_MAX));
  CHECK(!SnesMode7HdSpanFits(0, 0, 0, 0, 0));
  CHECK(!SnesMode7HdSpanFits(0, NAN, 1, 1, INT_MAX));
  CHECK(!SnesMode7HdSpanFits(0, 0, INFINITY, 1, INT_MAX));
  CHECK(!SnesMode7HdSpanFits(-1e100, 0, 1, 1, INT_MAX));
  CHECK(!SnesMode7HdSpanFits(0, 0, 1024, 1, 1024));
  /* Custom course tiles obey the same outside-map rules as the native map. */
  CHECK(SnesMode7HdFetchInt(0x80, p.vram, -1, 0, 37) == 0);
  CHECK(SnesMode7HdFetchInt(0xc0, p.vram, -1, 0, 37) == (p.vram[7] >> 8));
  CHECK(SnesMode7HdFetchInt(0, p.vram, -1, 0, 37) == (p.vram[37*64+7] >> 8));
}
static void test_fractional_scanout(void) {
  /* Compare the optimized scanout to the general scalar sampler at every
   * fractional sample, including repeated texels across native columns,
   * negative/large coordinates, flips and all three outside-map behaviors. */
  static const unsigned controls[] = {0, 1, 2, 3, 0x80, 0xc0};
  static const int slopes[] = {64, 257, -257, 32767};
  for (unsigned control = 0; control < countof(controls); ++control)
    for (unsigned slope = 0; slope < countof(slopes); ++slope)
      for (unsigned scale = 2; scale <= 4; ++scale) {
        setup(); PpuSetExtraSpace(&p, 16); bind(288, scale);
        for (unsigned i = 0; i < 0x8000; ++i) p.vram[i] = (uint16_t)(i * 251u + i / 17u);
        p.m7sel = (uint8_t)controls[control];
        p.m7matrix[0] = (int16_t)slopes[slope]; p.m7matrix[2] = -129;
        p.m7matrix[1] = 37; p.m7matrix[3] = 511;
        p.m7matrix[4] = 0x1fff; p.m7matrix[5] = 1023;
        for (unsigned line = 1; line <= HEIGHT; ++line) {
          ppu_runLine(&p, (int)line);
          SnesMode7HdTransform t = SnesMode7HdMakeTransform(p.m7matrix, p.m7sel, line);
          for (unsigned sy = 0; sy < scale; ++sy)
            for (unsigned x = 0; x < 288 * scale; ++x) {
              double position = (int)(x / scale) - 16 + (double)(x % scale) / scale;
              unsigned index = SnesMode7HdSample(&t, p.vram, position, (double)sy / scale);
              CHECK(hd[1 + ((line-1)*scale+sy)*PITCH+x] == rgb(index));
            }
        }
        check_guards(288, scale, HEIGHT);
      }
}
int main(void) {
  test_detail_and_isolation();
  test_fallbacks_and_capacity();
  test_transparency_and_sprites();
  test_composition_and_raster_state();
  test_sampler();
  test_bounded_sampler();
  test_fractional_scanout();
  puts("HD Mode 7: detail, isolation, bounds, fallbacks, sprites, raster state, fractional scanout and 512 composition cases passed");
  return 0;
}
