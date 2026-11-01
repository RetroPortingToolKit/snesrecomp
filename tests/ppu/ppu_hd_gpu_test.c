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
static void compare_visible(SnesHdGpu *g,const SnesHdFrame *f,unsigned scale) {
  enum { w=137,h=7 };
  SnesHdView v; CHECK(SnesHdFrameView(f,&v));
  make_target(w,h);CHECK(SnesHdGpuDraw(g,f,scale,w,h));
  uint32_t gpu[w*h];glReadPixels(0,0,w,h,GL_BGRA,GL_UNSIGNED_BYTE,gpu);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
    unsigned px=(2*x+1)*v.width*scale/(2*w),py=(2*y+1)*v.height*scale/(2*h);
    uint32_t expected;CHECK(SnesHdFrameRenderRegion(f,scale,px,py,1,1,&expected,4));
    CHECK((gpu[y*w+x]&0xffffff)==(expected&0xffffff));
  }
}
static void check_smoothing(SnesHdGpu *g) {
  enum { sw=16, sh=8, dw=37, dh=19 };
  for(unsigned affine=0;affine<2;++affine) {
  setup();
  SnesHdFrame *f=SnesHdFrameCreate(sw,sh);CHECK(f);
  int picture=SnesHdFrameAddPicture(f,p.vram);CHECK(picture>=0);
  for(unsigned y=0;y<sh;++y) {
    SnesHdLine j={0};j.kind=affine?kSnesHdAffine:kSnesHdMode7;j.source=picture;j.brightness=15;j.active_right=sw;
    j.origin_x=y<4?0:20000;j.origin_y=y*256;j.step_x=769;j.step_y=93;j.row_y=256;
    memcpy(j.palette,p.cgram,sizeof(j.palette));
    j.affine[0]=y<4?0:20000;j.affine[1]=y*256;
    j.affine[2]=769.125;j.affine[3]=93.25;j.affine[5]=256;
    for(unsigned x=0;x<sw;++x) {
      j.visibility[x]=3;j.native[x]=0x123456;j.native_mask[x]=x<2;
      if(x==2)j.overlay[x]=0xff000000;
      if(x==3){j.object[x]=0x601f;j.object_color[x]=0x3e0;j.visibility[x]|=12;}
    }
    CHECK(SnesHdFrameSetLine(f,y,&j));
  }
  CHECK(SnesHdFrameSeal(f));
  SnesHdView immutable;CHECK(SnesHdFrameView(f,&immutable));
  SnesHdLine *before=malloc(sh*sizeof(*before));CHECK(before);
  memcpy(before,immutable.lines,sh*sizeof(*before));
  const unsigned scales[]={1,2,8,32,48,64};
  for(unsigned si=0;si<countof(scales);++si) {
    unsigned scale=scales[si];SnesHdGpuImage image;
    CHECK(SnesHdGpuRenderSmoothed(g,f,scale,dw,dh,true,1024*1024,NULL,0,NULL,0,true,&image));
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,image.texture,0);
    uint32_t actual[dw*dh];glReadPixels(0,0,dw,dh,GL_BGRA,GL_UNSIGNED_BYTE,actual);
    unsigned averaged=0;
    for(unsigned y=0;y<dh;++y)for(unsigned x=0;x<dw;++x) {
      unsigned px=(2*x+1)*sw*scale/(2*dw),py=(2*y+1)*sh*scale/(2*dh);
      unsigned nx=px/scale,ny=py/scale,count=0,sum[3]={0};uint32_t point;
      CHECK(SnesHdFrameRenderRegion(f,scale,px,py,1,1,&point,4));
      if(scale>1 && nx>=4)for(unsigned sy=0;sy<2;++sy)for(unsigned sx=0;sx<2;++sx) {
        unsigned qx=(4*x+2*sx+1)*sw*scale/(4*dw),qy=(4*y+2*sy+1)*sh*scale/(4*dh);
        if(qx/scale<4 || (ny<4)!=(qy/scale<4))continue;
        uint32_t c;CHECK(SnesHdFrameRenderRegion(f,scale,qx,qy,1,1,&c,4));
        for(unsigned ch=0;ch<3;++ch)sum[ch]+=(c>>(ch*8))&255;++count;
      }
      if(count)++averaged;
      for(unsigned ch=0;ch<3;++ch) {
        int expected=count?(sum[ch]+count/2)/count:(point>>(ch*8))&255;
        int got=(actual[y*dw+x]>>(ch*8))&255;
        CHECK(abs(got-expected)<=(count?1:0));
      }
    }
    CHECK(scale==1 || averaged>0);
    SnesHdGpuStats stats;CHECK(SnesHdGpuReadStats(g,true,&stats));
    CHECK(stats.samples_per_pixel==(scale==1?1:4));
  }
  SnesHdGpuImage image;
  size_t point_budget=(size_t)dw*dh*4+(size_t)(sw+1)*dh*16;
  CHECK(SnesHdGpuRenderSmoothed(g,f,64,dw,dh,true,point_budget,NULL,0,NULL,0,true,&image));
  SnesHdGpuStats stats;CHECK(SnesHdGpuReadStats(g,true,&stats));CHECK(stats.samples_per_pixel==1);
  CHECK(!memcmp(before,immutable.lines,sh*sizeof(*before)));free(before);
  SnesHdFrameDestroy(f);
  }
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
  check_smoothing(g);
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
      compare_visible(g,f,scale);
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
      compare_visible(g,f,scale);
    }
    if (seed == 2) SnesHdGpuReset(g);
    SnesHdFrameDestroy(f);
  }
  /* Non-periodic maps exercise the Q16 fast-path boundary and steeper
   * double-precision fallback without hiding coordinate errors in repetition.
   * Cover both signs and non-power-of-two scales above32x. */
  for (unsigned seed = 0; seed < 4; ++seed) {
    setup();
    static uint8_t tiles[512 * 256];
    for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 512; ++x)
      tiles[y * 512 + x] = (uint8_t)(1 + (x * 7 + y * 11 + (x / 128) * 53 + (y / 128) * 73) % 255);
    for (unsigned tile = 0; tile < 256; ++tile) for (unsigned texel = 0; texel < 64; ++texel)
      p.vram[tile * 64 + texel] = (uint16_t)((1 + (tile * 31 + texel) % 255) << 8);
    SnesHdFrame *f = SnesHdFrameCreate(288, HEIGHT); CHECK(f);
    int picture = SnesHdFrameAddPicture(f, p.vram);
    int map = SnesHdFrameAddTilemap(f, tiles, 512, 256); CHECK(picture >= 0 && map >= 0);
    for (unsigned y = 0; y < HEIGHT; ++y) {
      SnesHdLine j = {0};
      j.kind = kSnesHdAffine; j.source = (uint16_t)picture; j.tilemap = (uint16_t)(map + 1);
      j.brightness = 15; j.active_left = -16; j.active_right = 272; j.screen_left = -16;
      memcpy(j.palette, p.cgram, sizeof(j.palette));
      memset(j.visibility, 3, sizeof(j.visibility));
      const double t[8] = {-23.125, 256.0 * y - 93.5,
        (seed & 1 ? -1 : 1) * 256.0 * (seed < 2 ? 255.875 : 1100.125),
        (seed & 1 ? 1 : -1) * 256.0 * (seed < 2 ? 199.375 : 733.375),
        37.1, 511.11, .004, -.001};
      memcpy(j.affine, t, sizeof(t)); CHECK(SnesHdFrameSetLine(f, y, &j));
    }
    CHECK(SnesHdFrameSeal(f));
    static const unsigned steep_scales[] = {1, 16, 24, 32, 33, 40, 48, 56, 64};
    for (unsigned s = 0; s < countof(steep_scales); ++s) {
      unsigned scale = steep_scales[s], w, h;
      CHECK(SnesHdGpuAdmit(g, 288, HEIGHT, scale, (size_t)1024 * 1024 * 1024, &w, &h));
      make_target(w, h); CHECK(SnesHdGpuDraw(g, f, scale, w, h));
      compare_roi(g, f, scale, 16 * scale, 0, 128, h < 16 ? h : 16);
      compare_roi(g, f, scale, w - 128, h - 4, 128, 4);
      comparisons += 2;
      compare_visible(g, f, scale);
    }
    SnesHdFrameDestroy(f);
  }
  /* Exercise the shared offscreen presenter, including exact visible centers,
   * explicit opaque-black foreground and caller GL state restoration. */
  setup(); PpuSetExtraSpace(&p,16);
  SnesHdFrame *presented = scene(288);
  enum { pw = 137, ph = 7 };
  uint32_t foreground[288 * HEIGHT];
  for (unsigned y = 0; y < HEIGHT; ++y) for (unsigned x = 0; x < 288; ++x)
    foreground[y * 288 + x] = x & 1 ? 0xff123456 : 0xff000000;
  make_target(pw, ph); glViewport(5,6,11,12);
  GLint saved_fbo, restored_fbo, vp[4]; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&saved_fbo);
  SnesHdGpuImage image;
  CHECK(!SnesHdGpuAdmit(g,288,HEIGHT,64,1,&target_w,&target_h));
  CHECK(SnesHdGpuRender(g,presented,64,pw,ph,true,128u*1024u*1024u,
      NULL,0,foreground,288*4,&image));
  CHECK(image.visible_samples && image.width==pw && image.height==ph);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&restored_fbo);CHECK(saved_fbo==restored_fbo);
  glGetIntegerv(GL_VIEWPORT,vp);CHECK(vp[0]==5 && vp[1]==6 && vp[2]==11 && vp[3]==12);
  glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,image.texture,0);
  uint32_t visible[pw*ph]; glReadPixels(0,0,pw,ph,GL_BGRA,GL_UNSIGNED_BYTE,visible);
  for (unsigned y=0;y<ph;++y) for (unsigned x=0;x<pw;++x) {
    unsigned nx=((2*x+1)*288*64/(2*pw))/64;
    CHECK((visible[y*pw+x]&0xffffff)==(foreground[nx]&0xffffff));
  }
  CHECK(SnesHdFrameView(presented,&(SnesHdView){0}));
  SnesHdFrameDestroy(presented);
  printf("GPU parity: %u ROI comparisons at1..64x (including33/40/48/56x) and shared target/foreground/state checks passed\n", comparisons);
  glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &target);
  SnesHdGpuDestroy(g); SDL_GL_DestroyContext(context); SDL_DestroyWindow(window); SDL_Quit();
  return 0;
}
