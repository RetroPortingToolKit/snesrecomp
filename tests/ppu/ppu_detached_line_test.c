/* Private-picture rendering must share normal pixels without publishing a
 * second authoritative scanout. No ROM, generated code or frontend needed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/ws_shadow.h"
#include "debug_server.h"

Snes *g_snes;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
static unsigned line_calls, drawn_calls, oam_calls, window_calls;
static unsigned shadow_calls, enhancer_calls;
#ifdef SNESRECOMP_INTERP_PROFILE
static uint64_t clock_calls;
uint64_t snesrecomp_host_now_ns(void) { return ++clock_calls; }
#endif

void debug_server_on_ppu_line(int line) { (void)line; ++line_calls; }
void debug_server_on_ppu_line_drawn(int line, int renderer, unsigned mode) {
  (void)line; (void)renderer; (void)mode; ++drawn_calls;
}
void debug_server_on_oam_render(void) { ++oam_calls; }
void debug_server_on_ppu_window(int line, int layer, const int16_t *edges,
                              unsigned nr, uint8_t bits) {
  (void)line; (void)layer; (void)edges; (void)nr; (void)bits; ++window_calls;
}
void debug_server_on_oam_write(int high, uint16_t index, uint16_t value) {
  (void)high; (void)index; (void)value;
}
void debug_server_on_vram_write(uint32_t address, uint8_t value) {
  (void)address; (void)value;
}
uint16_t WsShadowTile(int layer, int x, uint32_t y, uint16_t scroll,
                     uint16_t address, uint16_t tile) {
  (void)layer; (void)x; (void)y; (void)scroll; (void)address;
  ++shadow_calls; return tile;
}
bool WsShadowLayerActive(int layer) { (void)layer; ++shadow_calls; return false; }
uint32_t WsShadowWorldX(int layer) { (void)layer; ++shadow_calls; return 0; }
uint32_t WsShadowPresentWorldY(int layer, int x) {
  (void)layer; (void)x; ++shadow_calls; return 0;
}
uint32_t WsShadowScrollY(int layer) { (void)layer; ++shadow_calls; return 0; }
void WsShadowOnVramWrite(uint16_t address, uint16_t value) {
  (void)address; (void)value;
}
static void enhancer(Ppu *ppu, uint y, bool sub, void *context) {
  (void)ppu; (void)y; (void)sub; (void)context; ++enhancer_calls;
}

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

enum { WIDTH = 272, HEIGHT = 240 };
static Snes machine;
static Ppu live, live_before, normal, detached, detached_before;
static uint32_t pixels[WIDTH * HEIGHT], expected[WIDTH * HEIGHT];
static uint32_t hd[WIDTH * HEIGHT], expected_hd[WIDTH * HEIGHT];
static uint32_t seed = 0x6f13ad75;
static uint32_t random32(void) {
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed;
}
static uint64_t digest = UINT64_C(1469598103934665603);
static void hash(const void *memory, size_t size) {
  const uint8_t *bytes = memory;
  for (size_t i = 0; i < size; ++i)
    digest = (digest ^ bytes[i]) * UINT64_C(1099511628211);
}

static void fixture(unsigned test) {
  memset(&normal, 0, sizeof normal);
  ppu_reset(&normal);
  normal.renderFlags = test & 1 ? kPpuRenderFlags_NewRenderer : 0;
  normal.inidisp = (test % 19 == 0 ? 0x80 : 0) | (test % 16);
  normal.lastBrightnessMult = 0xff;
  normal.bgmode = (test / 2) % 8;
  normal.mosaic = (uint8_t)random32();
  normal.screenEnabled[0] = (uint8_t)random32() & 31;
  normal.screenEnabled[1] = (uint8_t)random32() & 31;
  normal.screenWindowed[0] = (uint8_t)random32() & 31;
  normal.screenWindowed[1] = (uint8_t)random32() & 31;
  normal.windowsel = random32();
  normal.window1left = 16; normal.window1right = 100;
  normal.window2left = 80; normal.window2right = 230;
  normal.wbgobjlog = (uint16_t)random32();
  normal.cgadsub = (uint8_t)random32();
  normal.cgwsel = (uint8_t)random32();
  normal.fixedColor = (uint16_t)random32() & 0x7fff;
  normal.bgTileAdr = (uint16_t)random32();
  normal.obsel = (uint8_t)random32();
  normal.extraLeftRight = normal.extraLeftCur = normal.extraRightCur = 8;
  normal.wsOamMotionLastLine = 100;
  for (unsigned i = 0; i < 4; ++i) {
    normal.hScroll[i] = (uint16_t)random32();
    normal.vScroll[i] = (uint16_t)random32();
    normal.bgXsc[i] = (uint8_t)random32();
  }
  for (unsigned i = 0; i < 8; ++i) normal.m7matrix[i] = (int16_t)random32();
  normal.m7matrix[0] = normal.m7matrix[3] = 256;
  normal.m7matrix[1] = normal.m7matrix[2] = 0;
  for (unsigned i = 0; i < 0x8000; ++i) normal.vram[i] = (uint16_t)random32();
  for (unsigned i = 0; i < 256; ++i) {
    normal.cgram[i] = (uint16_t)random32() & 0x7fff;
    normal.oam[i] = (uint16_t)random32();
  }
  for (unsigned i = 0; i < 32; ++i) normal.highOam[i] = (uint8_t)random32();
  /* Prepare picture caches through the existing ordinary pre-render path.
   * Both drawings then consume the same already-latched field parity/hints. */
  ppu_runLine(&normal, 0);
  detached = normal;
  normal.renderBuffer = (uint8_t *)expected;
  detached.renderBuffer = (uint8_t *)pixels;
  normal.renderPitch = detached.renderPitch = WIDTH * 4;
  normal.mode7Hd = (PpuMode7HdSurface){expected_hd, WIDTH * 4, WIDTH, HEIGHT, 1};
  detached.mode7Hd = (PpuMode7HdSurface){hd, WIDTH * 4, WIDTH, HEIGHT, 1};
  memset(pixels, 0xa5, sizeof pixels); memset(expected, 0xa5, sizeof expected);
  memset(hd, 0x36, sizeof hd); memset(expected_hd, 0x36, sizeof expected_hd);
}

int main(void) {
  g_snes = &machine;
  machine.ppu = &live;
  ppu_reset(&live);
  live_before = live;
  unsigned cases = 0;
  static const int rows[] = {1, 2, 97, 112, 224, 240};
  for (unsigned test = 0; test < 128; ++test) {
    fixture(test);
    for (unsigned r = 0; r < sizeof rows / sizeof rows[0]; ++r) {
      int line = rows[r];
      normal.wsOamMotionLastLine = detached.wsOamMotionLastLine = (int16_t)line;
      normal.rangeOver = detached.rangeOver = (test & 2) != 0;
      normal.timeOver = detached.timeOver = (test & 4) != 0;
      ++snes_frame_counter;
      ppu_runLine(&normal, line);
      uint32_t frame; int16_t cursor;
      ppu_wlog_position(&frame, &cursor);
      unsigned lines = line_calls, drawn = drawn_calls, oam = oam_calls;
      unsigned windows = window_calls, shadows = shadow_calls;
#ifdef SNESRECOMP_INTERP_PROFILE
      uint64_t clocks = clock_calls;
#endif
      detached_before = detached;
      PpuDrawDetachedLine(&detached, line);
      CHECK(memcmp(pixels, expected, sizeof pixels) == 0);
      CHECK(memcmp(hd, expected_hd, sizeof hd) == 0);
      CHECK(line_calls == lines && drawn_calls == drawn && oam_calls == oam);
      CHECK(window_calls == windows && shadow_calls == shadows);
#ifdef SNESRECOMP_INTERP_PROFILE
      CHECK(clocks == clock_calls);
#endif
      uint32_t after_frame; int16_t after_cursor;
      ppu_wlog_position(&after_frame, &after_cursor);
      CHECK(frame == after_frame && cursor == after_cursor);
      CHECK(detached.rangeOver == detached_before.rangeOver);
      CHECK(detached.timeOver == detached_before.timeOver);
      CHECK(detached.evenFrame == detached_before.evenFrame);
      CHECK(detached.wsOamMotionLastLine == detached_before.wsOamMotionLastLine);
      CHECK(memcmp(detached.wsOamMotionGrace, detached_before.wsOamMotionGrace,
                   sizeof detached.wsOamMotionGrace) == 0);
      CHECK(memcmp(&live, &live_before, sizeof live) == 0);
      hash(pixels + (line - 1) * WIDTH, WIDTH * 4);
      ++cases;
    }
  }

  /* A private repeat cannot steal the once-per-frame live OAM snapshot or
   * publish debug/window callbacks. It also skips copied external enhancers. */
  fixture(31);
  detached.widescreenLineEnhancer = enhancer;
  detached.widescreenLineEnhancerContext = &machine;
  detached.rangeOver = detached.timeOver = true;
  detached.evenFrame = true;
  detached.wsOamMotionLastLine = 112;
  ++snes_frame_counter;
  unsigned oam_before = oam_calls;
  PpuDrawDetachedLine(&detached, 0);
  CHECK(detached.rangeOver && detached.timeOver && detached.evenFrame);
  CHECK(detached.wsOamMotionLastLine == 112);
  PpuDrawDetachedLine(&detached, 112);
  CHECK(enhancer_calls == 0);
  CHECK(detached.widescreenLineEnhancer == enhancer);
  CHECK(detached.widescreenLineEnhancerContext == &machine);
  CHECK(oam_calls == oam_before);
  ppu_runLine(&normal, 112);
  CHECK(oam_calls == oam_before + 1);
  ppu_runLine(&normal, 112);
  CHECK(oam_calls == oam_before + 1);

  detached_before = detached;
  PpuDrawDetachedLine(&detached, -1);
  PpuDrawDetachedLine(&detached, 241);
  PpuDrawDetachedLine(NULL, 1);
  PpuDrawDetachedLine(&live, 1);
  CHECK(memcmp(&detached, &detached_before, sizeof detached) == 0);
  CHECK(memcmp(&live, &live_before, sizeof live) == 0);
  printf("ppu_detached_line: PASS cases=%u digest=%016llx debug=%u/%u/%u/%u\n",
         cases, (unsigned long long)digest, line_calls, drawn_calls, oam_calls, window_calls);
  return 0;
}
