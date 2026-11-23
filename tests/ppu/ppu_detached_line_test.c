/* Private-picture rendering must share normal pixels without publishing a
 * second authoritative scanout. No ROM, generated code or frontend needed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/ppu.h"
#include "snes/raster_inspection.h"
#include "snes/snes.h"
#include "snes/dma.h"
#include "snes/ws_shadow.h"
#include "debug_server.h"

Snes *g_snes;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
static unsigned line_calls, drawn_calls, oam_calls, window_calls;
static unsigned shadow_calls, enhancer_calls;
static SnesRasterInspection *inspection_capture;
#ifdef SNESRECOMP_INTERP_PROFILE
static uint64_t clock_calls;
uint64_t snesrecomp_host_now_ns(void) { return ++clock_calls; }
#endif

void debug_server_on_ppu_line(int line) {
  ++line_calls;
  if (inspection_capture)
    SnesRasterInspectionCapture(inspection_capture, g_snes->ppu, snes_frame_counter, (unsigned)line);
}
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
static SnesHdFrame *private_hd;
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
  /* Match the physical framebuffer budget as well as the visible margins.
   * The legacy renderer offsets negative screen coordinates by this budget. */
  PpuSetExtraSpace(&normal, 8);
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
  SnesHdFrameDestroy(private_hd);
  private_hd = SnesHdFrameCreate(WIDTH, 1); CHECK(private_hd);
  detached.hdFrame = private_hd; normal.hdFrame = NULL;
  memset(pixels, 0xa5, sizeof pixels); memset(expected, 0xa5, sizeof expected);
}

static uint32_t inspection_original[2][256 * kSnesInspectionLines];
static uint32_t inspection_pixels[256 * kSnesInspectionLines];
static uint32_t inspection_partial[256 * kSnesInspectionLines];
static Snes inspection_machine_before;
static Dma inspection_dma, inspection_dma_before;

static void write_palette(Ppu *ppu, uint8_t index, uint16_t rgb) {
  ppu_write(ppu, 0x21, index);
  ppu_write(ppu, 0x22, (uint8_t)rgb);
  ppu_write(ppu, 0x22, (uint8_t)(rgb >> 8));
}

static void test_raster_inspection(void) {
  SnesRasterInspection *inspection = SnesRasterInspectionCreateBounded(2, kSnesInspectionBytes);
  CHECK(inspection);
  inspection_capture = inspection;
  ppu_reset(&live);
  machine.dma = &inspection_dma;
  ppu_write(&live, 0x00, 0x0f);
  ppu_write(&live, 0x05, 1);
  ppu_write(&live, 0x07, 4);
  ppu_write(&live, 0x2c, 1);
  /* Program OAM slot 7 and its high-table select bits through the PPU. */
  ppu_write(&live, 0x01, 0x20);
  ppu_write(&live, 0x02, 14);
  ppu_write(&live, 0x03, 0);
  ppu_write(&live, 0x04, 250);
  ppu_write(&live, 0x04, 108);
  ppu_write(&live, 0x04, 0x2a);
  ppu_write(&live, 0x04, 0xfb);
  ppu_write(&live, 0x02, 0);
  ppu_write(&live, 0x03, 1);
  ppu_write(&live, 0x04, 0);
  ppu_write(&live, 0x04, 0xc0);
  for (unsigned y = 0; y < 8; ++y) live.vram[y] = 0x00ff;
  unsigned first = ++snes_frame_counter;
  for (unsigned frame = 0; frame < 2; ++frame) {
    /* Exercise both the cycle and desktop frame-model indices. */
    snes_frame_counter = first + frame + (frame != 0);
    ppu_write(&live, 0x31, 0);
    write_palette(&live, 1, frame ? 0x03e0 : 0x001f);
    write_palette(&live, 0xd1, frame ? 0x4210 : 0x1234);
    PpuBeginDrawing(&live, (uint8_t *)inspection_original[frame], 256 * 4,
                    kPpuRenderFlags_NewRenderer);
    ppu_runLine(&live, 0);
    for (unsigned line = 1; line <= kSnesInspectionLines; ++line) {
      if (line == 113) {
        write_palette(&live, 1, 0x7c00);
        write_palette(&live, 0xd1, 0x2468);
        ppu_write(&live, 0x31, 1);
        ppu_write(&live, 0x32, 0x5f);
      }
      ppu_runLine(&live, (int)line);
    }
    CHECK(!SnesRasterInspectionFind(inspection, first + frame));
    SnesRasterInspectionPublish(inspection, first + frame);
  }
  /* Opening the next frame does not evict the previous completed field. */
  PpuBeginDrawing(&live, (uint8_t *)inspection_partial, 256 * 4,
                  kPpuRenderFlags_NewRenderer);
  ++snes_frame_counter;
  ppu_runLine(&live, 1);
  SnesRasterInspectionPublish(inspection, snes_frame_counter);
  CHECK(!SnesRasterInspectionFind(inspection, snes_frame_counter));
  const SnesRasterInspectionFrame *past = SnesRasterInspectionFind(inspection, first);
  const SnesRasterInspectionFrame *latest = SnesRasterInspectionFind(inspection, first + 1);
  CHECK(past && latest);
  CHECK(past->lines[111].palette[1] == 0x001f);
  CHECK(past->lines[112].palette[1] == 0x7c00);
  CHECK(inspection_original[0][0] != inspection_original[0][112 * 256]);
  CHECK(inspection_original[0][0] != inspection_original[1][0]);

  live_before = live;
  inspection_machine_before = machine;
  inspection_dma_before = inspection_dma;
  unsigned lines = line_calls, drawn = drawn_calls, windows = window_calls;
  unsigned oam = oam_calls, shadows = shadow_calls;
  int paused_frame = snes_frame_counter;
  CHECK(SnesRasterInspectionDraw(inspection, latest, 0x1f, inspection_pixels));
  CHECK(memcmp(inspection_pixels, inspection_original[1], sizeof inspection_pixels) == 0);
  CHECK(SnesRasterInspectionDraw(inspection, past, 0x1f, inspection_pixels));
  CHECK(memcmp(inspection_pixels, inspection_original[0], sizeof inspection_pixels) == 0);
  CHECK(SnesRasterInspectionDraw(inspection, past, 0, inspection_pixels));
  CHECK(inspection_pixels[0] != inspection_original[0][0]);
  CHECK(SnesRasterInspectionDraw(inspection, latest, 0x1f, inspection_pixels));
  CHECK(memcmp(inspection_pixels, inspection_original[1], sizeof inspection_pixels) == 0);
  CHECK(memcmp(&live, &live_before, sizeof live) == 0);
  CHECK(memcmp(&machine, &inspection_machine_before, sizeof machine) == 0);
  CHECK(memcmp(&inspection_dma, &inspection_dma_before, sizeof inspection_dma) == 0);
  CHECK(paused_frame == snes_frame_counter);
  CHECK(lines == line_calls && drawn == drawn_calls && windows == window_calls);
  CHECK(oam == oam_calls && shadows == shadow_calls);

  SnesRasterInspectionObject sprite;
  CHECK(SnesRasterInspectionReadObject(past, 111, 7, &sprite));
  CHECK(sprite.x == -6 && sprite.y == 108 && sprite.tile == 0x12a);
  CHECK(sprite.priority == 3 && sprite.size_select == 1 && sprite.obj_selection == 0x20);
  CHECK(sprite.flip_x && sprite.flip_y && sprite.palette_base == 0xd0);
  CHECK(sprite.palette[1] == 0x1234);
  CHECK(SnesRasterInspectionReadObject(past, 112, 7, &sprite));
  CHECK(sprite.palette[1] == 0x2468);
  CHECK(SnesRasterInspectionReadObject(latest, 111, 7, &sprite));
  CHECK(sprite.palette[1] == 0x4210);
  CHECK(!SnesRasterInspectionReadObject(latest, 224, 7, &sprite));
  CHECK(!SnesRasterInspectionReadObject(latest, 111, 128, &sprite));
  CHECK(memcmp(&live, &live_before, sizeof live) == 0);
  CHECK(memcmp(&machine, &inspection_machine_before, sizeof machine) == 0);
  CHECK(memcmp(&inspection_dma, &inspection_dma_before, sizeof inspection_dma) == 0);

  for (unsigned line = 2; line <= kSnesInspectionLines; ++line)
    ppu_runLine(&live, (int)line);
  SnesRasterInspectionPublish(inspection, paused_frame);
  CHECK(!SnesRasterInspectionFind(inspection, first));
  CHECK(SnesRasterInspectionFind(inspection, first + 1));
  CHECK(SnesRasterInspectionFind(inspection, paused_frame));
  for (unsigned line = 1; line <= kSnesInspectionLines; ++line)
    ppu_runLine(&live, (int)line);
  SnesRasterInspectionPublish(inspection, paused_frame);
  CHECK(SnesRasterInspectionFind(inspection, first + 1));
  snes_frame_counter = first;
  for (unsigned line = 1; line <= kSnesInspectionLines; ++line)
    ppu_runLine(&live, (int)line);
  SnesRasterInspectionPublish(inspection, first);
  CHECK(SnesRasterInspectionFind(inspection, first));
  CHECK(!SnesRasterInspectionFind(inspection, first + 1));
  CHECK(!SnesRasterInspectionFind(inspection, paused_frame));

  inspection_capture = NULL;
  SnesRasterInspectionDestroy(inspection);
  puts("ppu_raster_inspection: PASS mid-frame palette and color math; paused state unchanged");
}

static void test_raster_inspection_budget(void) {
  const unsigned capacity = 2;
  const size_t control = sizeof(SnesRasterInspection) + capacity * sizeof(SnesRasterInspectionFrame *);
  const size_t stable_frame = sizeof(SnesRasterInspectionFrame) +
      4 * kSnesInspectionLines * sizeof(void *) + PPU_SAVESTATE_MEM_SIZE;
  const size_t budget = control + 2 * stable_frame - 1;
  SnesRasterInspection *inspection = SnesRasterInspectionCreateBounded(capacity, budget);
  CHECK(inspection);
  ppu_reset(&live);
  for (unsigned frame = 1; frame <= 4; ++frame) {
    write_palette(&live, 1, (uint16_t)frame);
    bool complete = false;
    for (unsigned line = 1; line <= kSnesInspectionLines; ++line)
      complete = SnesRasterInspectionCapture(inspection, &live, frame, line);
    CHECK(complete);
    SnesRasterInspectionPublish(inspection, frame);
    CHECK(inspection->bytes == control + stable_frame && inspection->peak_bytes <= budget);
    CHECK(inspection->count == 1 && SnesRasterInspectionFind(inspection, frame));
    CHECK(!SnesRasterInspectionFind(inspection, frame - 1));
  }
  SnesRasterInspectionDestroy(inspection);
  puts("ppu_raster_inspection: PASS byte budget evicts complete frames before allocation");
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
      CHECK(!SnesHdFrameSeal(private_hd)); /* detached work never publishes */
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
  test_raster_inspection();
  test_raster_inspection_budget();
  SnesHdFrameDestroy(private_hd);
  return 0;
}
