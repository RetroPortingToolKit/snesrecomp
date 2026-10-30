/* Independent native/CPU-HD scenes are reused as a black-box GPU oracle. */
#define main old_ppu_hd_test_main
#include "ppu_mode7_hd_test.c"
#undef main
#include "desktop/hd_gpu.h"
#include "gl_core_3_1.h"
#include "desktop/sdl_compat.h"
#include <time.h>

static GLuint target, framebuffer;
static unsigned target_w, target_h;
static void make_target(unsigned w, unsigned h) {
  if (w == target_w && h == target_h) return;
  if (target) glDeleteTextures(1, &target);
  glGenTextures(1, &target); glBindTexture(GL_TEXTURE_2D, target);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
  CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  CHECK(glGetError() == GL_NO_ERROR); target_w = w; target_h = h;
}
static SnesHdFrame *scene(unsigned width) {
  SnesHdFrame *f = SnesHdFrameCreate(width, HEIGHT); CHECK(f);
  CHECK(PpuSetHdFrameCapture(&p, f));
  for (unsigned y = 1; y <= HEIGHT; ++y) ppu_runLine(&p, y);
  CHECK(PpuSetHdFrameCapture(&p, NULL)); CHECK(SnesHdFrameSeal(f)); return f;
}
static void compare_roi(SnesHdGpu *g, const SnesHdFrame *f, unsigned scale,
                         unsigned x, unsigned y, unsigned width, unsigned height) {
  uint32_t cpu[4096], gpu[4096]; CHECK(width * height <= 4096);
  CHECK(SnesHdFrameRenderRegion(f, scale, x, y, width, height, cpu, width * 4));
  glReadPixels(x, y, width, height, GL_BGRA, GL_UNSIGNED_BYTE, gpu);
  for (unsigned i = 0; i < width * height; ++i)
    if ((cpu[i] & 0xffffff) != (gpu[i] & 0xffffff)) {
      fprintf(stderr, "GPU mismatch scale%u pixel%u,%u cpu%06x gpu%06x\n",
          scale, x + i % width, y + i / width, cpu[i] & 0xffffff, gpu[i] & 0xffffff); exit(1);
    }
  (void)g;
}
int main(void) {
  SDL_SetMainReady();
  CHECK(SDL_Init(SDL_INIT_VIDEO));
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_Window *window = SDL_CreateWindow("SNES HD GPU automated test", 64, 64,
      SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN); CHECK(window);
  SDL_GLContext context = SDL_GL_CreateContext(window); CHECK(context);
  CHECK(ogl_LoadFunctions());
  SnesHdGpu *g = SnesHdGpuCreate(); CHECK(g);
  CHECK(SnesHdGpuSupportsAffine(g));
  glGenFramebuffers(1, &framebuffer);
  unsigned comparisons = 0;
  static const unsigned scales[] = {1, 2, 3, 4, 16, 32, 64};
  for (unsigned seed = 0; seed < 32; ++seed) {
    setup(); PpuSetExtraSpace(&p, 16); sprite((seed >> 1) & 3);
    p.inidisp = 1 + seed % 15; p.screenEnabled[0] = 17; p.screenEnabled[1] = seed & 1 ? 17 : 1;
    p.cgwsel = (seed & 3) << 6 | ((seed >> 2) & 3) << 4 | (seed & 4 ? 2 : 0);
    p.cgadsub = 0x31 | (seed & 8 ? 64 : 0) | (seed & 16 ? 128 : 0);
    p.windowsel = 0xa000a; p.window1left = 2; p.window1right = 14;
    p.screenWindowed[0] = seed & 2 ? 17 : 0;
    p.m7matrix[0] = seed & 1 ? -257 : 257;
    p.m7matrix[1] = 37; p.m7matrix[2] = -129; p.m7matrix[3] = 511;
    p.m7matrix[4] = -1; p.m7matrix[5] = 1023; p.m7sel = seed % 4 | (seed & 8 ? 0x80 : 0);
    SnesHdFrame *f = scene(288);
    for (unsigned s = 0; s < countof(scales); ++s) {
      unsigned scale = scales[s], w, h;
      CHECK(SnesHdGpuAdmit(g, 288, HEIGHT, scale, (size_t)1024 * 1024 * 1024, &w, &h));
      make_target(w, h); CHECK(SnesHdGpuDraw(g, f, scale, w, h));
      if (w == 288 * scale && h == HEIGHT * scale) {
        compare_roi(g, f, scale, 16 * scale, 0, 64, h < 16 ? h : 16);
        compare_roi(g, f, scale, w - 64, h > 8 ? h - 8 : 0, 64, h < 8 ? h : 8);
        comparisons += 2;
      }
      SnesHdGpuStats stats; CHECK(SnesHdGpuReadStats(g, true, &stats));
      CHECK(stats.gpu_timestamps && stats.draw_gpu_ns);
    }
    SnesHdFrameDestroy(f);
  }
  /* Prepared cameras and large owned maps take the same composition path.
   * Negative coordinates, fractional transforms, row interpolation, source
   * replacement and reset must agree with the portable consumer. */
  for (unsigned seed = 0; seed < 4; ++seed) {
    setup();
    uint8_t tiles[32 * 16];
    for (unsigned i = 0; i < sizeof(tiles); ++i) tiles[i] = (uint8_t)(i * 71 + seed);
    SnesHdFrame *f = SnesHdFrameCreate(288, HEIGHT); CHECK(f);
    int picture = SnesHdFrameAddPicture(f, p.vram);
    int map = SnesHdFrameAddTilemap(f, tiles, 32, 16); CHECK(picture >= 0 && map >= 0);
    for (unsigned y = 0; y < HEIGHT; ++y) {
      SnesHdLine j = {0};
      j.kind = kSnesHdAffine; j.source = (uint16_t)picture; j.tilemap = (uint16_t)(map + 1);
      j.brightness = 15; j.active_left = -16; j.active_right = 272; j.screen_left = -16;
      j.map_offset_x = -513; j.map_offset_y = 4099; j.control = seed == 3 ? 0xc0 : 0;
      memcpy(j.palette, p.cgram, sizeof(j.palette));
      for (unsigned x = 0; x < 288; ++x) {
        j.visibility[x] = 3;
        j.overlay[x] = x % 3 == 0 ? 0xff000000 : x % 3 == 1 ? 0x80402010 : 0;
      }
      const double t[8] = {-23.125, 256.0 * y - 93.5, 257.03, -129.99, 37.1, 511.11, .004, -.001};
      memcpy(j.affine, t, sizeof(t)); CHECK(SnesHdFrameSetLine(f, y, &j));
    }
    CHECK(SnesHdFrameSeal(f));
    for (unsigned s = 0; s < countof(scales); ++s) {
      unsigned scale = scales[s], w, h;
      CHECK(SnesHdGpuAdmit(g, 288, HEIGHT, scale, (size_t)1024 * 1024 * 1024, &w, &h));
      make_target(w, h); CHECK(SnesHdGpuDraw(g, f, scale, w, h));
      compare_roi(g, f, scale, 16 * scale, 0, 64, h < 16 ? h : 16);
      compare_roi(g, f, scale, w - 64, h > 8 ? h - 8 : 0, 64, h < 8 ? h : 8);
      comparisons += 2;
    }
    if (seed == 2) SnesHdGpuReset(g);
    SnesHdFrameDestroy(f);
  }
  printf("GPU parity: %u ROI comparisons at1/2/3/4/16/32/64x passed\n", comparisons);
  glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &target);
  SnesHdGpuDestroy(g); SDL_GL_DestroyContext(context); SDL_DestroyWindow(window); SDL_Quit();
  return 0;
}
