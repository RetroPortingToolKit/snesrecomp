/* Real framebuffer readback: scaling must never mix opposite image edges.
 * Optional shader/HD services are stubbed; the shared plain presenter runs
 * unchanged on a real hidden GL context. Windows also compares D3D11 output.
 */
#define SDL_MAIN_HANDLED
#include "sdl_compat.h"
#include <stdio.h>
#include <stdlib.h>
static void capture_swap(SDL_Window *window);
#define SDL_GL_SwapWindow capture_swap
#include "../../src/desktop/opengl.c"
#undef SDL_GL_SwapWindow

Config g_config;
static unsigned char captured[4][3];
void Die(const char *why) { fprintf(stderr,"%s\n",why); exit(2); }
void MkDir(const char *path) { (void)path; }
GlslShader *GlslShader_CreateFromFile(const char *name) { (void)name; return NULL; }
void GlslShader_Destroy(GlslShader *shader) { (void)shader; }
void GlslShader_Render(GlslShader *s,GlTextureWithSize *t,int x,int y,int w,int h) {
  (void)s;(void)t;(void)x;(void)y;(void)w;(void)h;
}
SnesHdGpu *SnesHdGpuCreate(void) { return NULL; }
void SnesHdGpuDestroy(SnesHdGpu *gpu) { (void)gpu; }
bool SnesHdGpuRender(SnesHdGpu *g,const SnesHdFrame *f,unsigned s,unsigned w,unsigned h,
    bool visible,size_t budget,const uint32_t *n,size_t np,const uint32_t *o,size_t op,SnesHdGpuImage *i) {
  (void)g;(void)f;(void)s;(void)w;(void)h;(void)visible;(void)budget;
  (void)n;(void)np;(void)o;(void)op;(void)i; return false;
}
static void capture_swap(SDL_Window *window) {
  int w,h; snesrecomp_sdl_get_drawable_size(window,&w,&h);
  glFinish(); glPixelStorei(GL_PACK_ALIGNMENT,1);
  glReadPixels(w/2,h-1,1,1,GL_RGB,GL_UNSIGNED_BYTE,captured[0]);
  glReadPixels(w/2,0,1,1,GL_RGB,GL_UNSIGNED_BYTE,captured[1]);
  glReadPixels(0,h/2,1,1,GL_RGB,GL_UNSIGNED_BYTE,captured[2]);
  glReadPixels(w-1,h/2,1,1,GL_RGB,GL_UNSIGNED_BYTE,captured[3]);
}
static const unsigned char expected[4][3]={{0,0,255},{255,0,0},{255,255,0},{255,0,255}};
static int check(const char *name,int linear,int w,int h) {
  int failures=0;
  for(unsigned edge=0;edge<4;++edge) {
    int bad=0;
    for(unsigned c=0;c<3;++c) if(abs((int)captured[edge][c]-expected[edge][c])>2)bad=1;
    if(bad) { ++failures; fprintf(stderr,"%s filter=%d %dx%d edge=%u RGB=%u,%u,%u expected=%u,%u,%u\n",
        name,linear,w,h,edge,captured[edge][0],captured[edge][1],captured[edge][2],
        expected[edge][0],expected[edge][1],expected[edge][2]); }
  }
  return failures;
}
static void pattern(uint32_t *pixels) {
  for(int y=0;y<4;++y)for(int x=0;x<4;++x)
    pixels[y*4+x]=x==0?0xffffff00u:x==3?0xffff00ffu:y==0?0xff0000ffu:y==3?0xffff0000u:0xff00ff00u;
}
int main(void) {
  SDL_SetMainReady();
  if(!SDL_Init(SDL_INIT_VIDEO))Die(SDL_GetError());
  struct RendererFuncs funcs; OpenGLRenderer_Create(&funcs);
  SDL_Window *window=SDL_CreateWindow("Presenter edge regression",80,80,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
  if(!window || !funcs.Initialize(window))Die(SDL_GetError());
  g_config.ignore_aspect_ratio=true; snesrecomp_opengl_set_vsync(0);
  int failures=0;
  for(int size=0;size<2;++size)for(int linear=0;linear<2;++linear) {
    int w=size?83:80,h=size?71:80; SDL_SetWindowSize(window,w,h); SDL_SyncWindow(window);
    g_config.linear_filtering=linear;
    uint8 *pixels;int pitch;funcs.BeginDraw(4,4,&pixels,&pitch);pattern((uint32_t *)pixels);
    /* A shader may have left repeat addressing on this same texture. */
    glBindTexture(GL_TEXTURE_2D,g_texture.gl_texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    funcs.EndDraw(); failures+=check("OpenGL",linear,w,h);
  }
  funcs.Destroy(); SDL_DestroyWindow(window);
#ifdef _WIN32
  window=SDL_CreateWindow("D3D11 edge reference",80,80,SDL_WINDOW_HIDDEN);
  SDL_Renderer *renderer=window?SDL_CreateRenderer(window,"direct3d11"):NULL;
  if(!renderer)Die(SDL_GetError());
  SDL_Texture *texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STATIC,4,4);
  uint32_t pixels[16];pattern(pixels);SDL_UpdateTexture(texture,NULL,pixels,16);SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE);
  for(int size=0;size<2;++size)for(int linear=0;linear<2;++linear) {
    int w=size?83:80,h=size?71:80;SDL_SetWindowSize(window,w,h);SDL_SyncWindow(window);
    SDL_SetTextureScaleMode(texture,linear?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);
    SDL_RenderClear(renderer);SDL_RenderTexture(renderer,texture,NULL,NULL);
    SDL_Surface *surface=SDL_RenderReadPixels(renderer,NULL);if(!surface)Die(SDL_GetError());
    int xy[4][2]={{surface->w/2,0},{surface->w/2,surface->h-1},{0,surface->h/2},{surface->w-1,surface->h/2}};
    for(unsigned i=0;i<4;++i){Uint8 a;SDL_ReadSurfacePixel(surface,xy[i][0],xy[i][1],&captured[i][0],&captured[i][1],&captured[i][2],&a);}
    failures+=check("D3D11",linear,w,h);SDL_DestroySurface(surface);
  }
  SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);
#endif
  SDL_Quit(); if(failures)return 1;
  puts("PASS: nearest/linear, integer/fractional scaling and prior repeat state preserve all four source edges");return 0;
}
