/* Independent native/CPU-HD scenes are reused as a black-box GPU oracle. */
#define main old_ppu_hd_test_main
#include "ppu_mode7_hd_test.c"
#undef main
#define main reconstruction_cpu_test_main
#include "ppu_mode7_reconstruct_test.c"
#undef main
#include "desktop/hd_gpu.h"
#include "desktop/present_shader.h"
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

/* Exact nearest resolve must agree with the visible-sample pixel centers.
 * Interpolated floating UVs can choose the adjacent texel at exact integer
 * boundaries, especially when the window is not a divisor of the HD raster. */
static void test_nearest_presentation(void) {
  static const char *vertex = "#version 330 core\n"
      "out vec2 TexCoord; void main() {"
      "vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));"
      "gl_Position=vec4(p[gl_VertexID],0,1);"
      "TexCoord=vec2((p[gl_VertexID].x+1)*.5,(1-p[gl_VertexID].y)*.5); }";
  const char *fragment=SnesGlPresentFragmentShader();
  GLuint vs=glCreateShader(GL_VERTEX_SHADER),fs=glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(vs,1,&vertex,NULL);glCompileShader(vs);
  glShaderSource(fs,1,&fragment,NULL);glCompileShader(fs);
  GLint ok;glGetShaderiv(vs,GL_COMPILE_STATUS,&ok);CHECK(ok);
  glGetShaderiv(fs,GL_COMPILE_STATUS,&ok);CHECK(ok);
  GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);
  glLinkProgram(program);glGetProgramiv(program,GL_LINK_STATUS,&ok);CHECK(ok);
  GLuint vao,source;glGenVertexArrays(1,&vao);glGenTextures(1,&source);
  static const unsigned cases[][6]={{2048,1792,768,576,5,7},{2728,1792,1280,720,0,0}};
  for(unsigned n=0;n<countof(cases);++n) {
    unsigned sw=cases[n][0],sh=cases[n][1],w=cases[n][2],h=cases[n][3],ox=cases[n][4],oy=cases[n][5];
    uint32_t *input=malloc((size_t)sw*sh*4),*actual=malloc((size_t)w*h*4);CHECK(input && actual);
    for(unsigned y=0;y<sh;++y)for(unsigned x=0;x<sw;++x)
      input[(size_t)y*sw+x]=0xff000000u|((x*1103515245u ^ y*2654435761u)&0xffffffu);
    make_target(w+ox,h+oy);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,source);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,sw,sh,0,GL_BGRA,GL_UNSIGNED_BYTE,input);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glViewport(ox,oy,w,h);glUseProgram(program);
    glUniform1i(glGetUniformLocation(program,"texture1"),0);
    glUniform1i(glGetUniformLocation(program,"exactNearest"),1);
    glUniform4i(glGetUniformLocation(program,"nearestViewport"),ox,oy,w,h);
    glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,3);
    glReadPixels(ox,oy,w,h,GL_BGRA,GL_UNSIGNED_BYTE,actual);
    unsigned differences=0;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
      unsigned sx=(2*x+1)*sw/(2*w),sy=(2*(h-1-y)+1)*sh/(2*h);
      differences+=(actual[(size_t)y*w+x]&0xffffff)!=(input[(size_t)sy*sw+sx]&0xffffff);
    }
    fprintf(stderr,"nearest presentation %ux%u ->%ux%u: %u mismatches\n",sw,sh,w,h,differences);
    CHECK(!differences);free(input);free(actual);
  }
  glBindVertexArray(0);glUseProgram(0);glDeleteVertexArrays(1,&vao);
  glDeleteTextures(1,&source);glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);
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
  test_nearest_presentation();
  unsigned comparisons = 0;
  test_clipped_math();
  for (unsigned flags = 0; flags < 4; ++flags) {
    SnesHdFrame *f = clipped_math_fixture(flags);
    make_target(4, 2); CHECK(SnesHdGpuDraw(g, f, 2, 4, 2));
    compare_roi(g, f, 2, 0, 0, 4, 2);
    SnesHdFrameDestroy(f);
  }
  test_projection_geometry(); test_nonperspective();
  for (unsigned flags = 0; flags < 16; ++flags) {
    SnesHdFrame *f = projection_fixture(flags, true, true);
    static const unsigned projection_scales[] = {2, 3, 8, 33, 64};
    for (unsigned s = 0; s < countof(projection_scales); ++s) {
      unsigned scale = projection_scales[s];
      make_target(288*scale, 64*scale);
      CHECK(SnesHdGpuDraw(g, f, scale, 288*scale, 64*scale));
      compare_roi(g, f, scale, 3*scale, 20*scale, 64, 32);
      compare_roi(g, f, scale, 280*scale, 32*scale-1, 16, 32);
      compare_visible(g, f, scale); comparisons += 2;
    }
    SnesHdFrameDestroy(f);
  }
  for (unsigned samples=2;samples<=4;++samples) for(unsigned flags=0;flags<16;flags+=5) {
    SnesHdFrame *f=projection_fixture_sampling(flags,true,true,samples);
    static const unsigned aa_scales[]={1,3,8};
    for(unsigned s=0;s<countof(aa_scales);++s) {
      unsigned scale=aa_scales[s];make_target(288*scale,64*scale);
      CHECK(SnesHdGpuDraw(g,f,scale,288*scale,64*scale));
      compare_roi(g,f,scale,20*scale,20*scale,32,32);
      compare_visible(g,f,scale);comparisons++;
    }
    compare_visible(g,f,64);
    SnesHdFrameDestroy(f);
  }
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
  /* The resolve scratch must obey the same memory cap and restore caller GL
   * state on both refusal and a reduced visible target. */
  SnesHdFrame *sampled=projection_fixture_sampling(0,true,true,4);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,saved_fbo);glViewport(5,6,11,12);
  CHECK(!SnesHdGpuRender(g,sampled,64,pw,ph,true,1,NULL,0,NULL,0,&image));
  const size_t sample_budget=128u*1024u;
  CHECK(SnesHdGpuRender(g,sampled,64,pw,ph,true,sample_budget,NULL,0,NULL,0,&image));
  CHECK(image.width<pw && image.height<ph);
  CHECK((uint64_t)image.width*image.height*4*17+
      (uint64_t)289*image.height*4*16<=sample_budget);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&restored_fbo);CHECK(saved_fbo==restored_fbo);
  glGetIntegerv(GL_VIEWPORT,vp);CHECK(vp[0]==5 && vp[1]==6 && vp[2]==11 && vp[3]==12);
  glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,image.texture,0);
  glReadPixels(0,0,image.width,image.height,GL_BGRA,GL_UNSIGNED_BYTE,visible);
  for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x) {
    uint32_t expected;unsigned px=(2*x+1)*288*64/(2*image.width);
    unsigned py=(2*y+1)*64*64/(2*image.height);
    CHECK(SnesHdFrameRenderRegion(sampled,64,px,py,1,1,&expected,4));
    CHECK((visible[y*image.width+x]&0xffffff)==(expected&0xffffff));
  }
  SnesHdFrameDestroy(sampled);
  printf("GPU parity: %u ROI comparisons at1..64x (including33/40/48/56x) and shared target/foreground/state checks passed\n", comparisons);
  glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &target);
  SnesHdGpuDestroy(g); SDL_GL_DestroyContext(context); SDL_DestroyWindow(window); SDL_Quit();
  return 0;
}
