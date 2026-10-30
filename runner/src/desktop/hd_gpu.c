#include "hd_gpu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#if defined(SNESRECOMP_HD_GPU) && SNESRECOMP_HD_GPU
#include "gl_core_3_1.h"
#include "sdl_compat.h"
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8e28
#endif
typedef void (CODEGEN_FUNCPTR *HdQueryCounter)(GLuint, GLenum);
typedef void (CODEGEN_FUNCPTR *HdQueryResult64)(GLuint, GLenum, uint64_t *);
enum { kResidentPictures = 256, kQueryFrames = 4, kHdTextures = 9 };
typedef struct HdResident {
  uint64_t hash, seen;
  uint16_t *copy;
} HdResident;
struct SnesHdGpu {
  GLuint program, affine_program, vao, textures[kHdTextures], queries[kQueryFrames][3];
  GLuint coordinate_program, coordinate_fbo;
  unsigned coordinate_width, coordinate_height;
  unsigned width, height, texture_width, texture_height, max_texture, max_layers, query_pending;
  bool failed;
  uint64_t serial;
  uint64_t query_frame[kQueryFrames];
  uint32_t *native_pixels, *overlay;
  uint16_t *objects, *palette;
  int32_t *metadata;
  double *affine;
  HdResident resident[kResidentPictures];
  SnesHdTilemap *maps[kSnesHdMaxTilemaps];
  uint64_t map_seen[kSnesHdMaxTilemaps];
  HdQueryCounter query_counter;
  HdQueryResult64 query_result;
  SnesHdGpuStats stats;
};
typedef struct HdState {
  GLint program, vao, active, textures[kHdTextures][2], viewport[4], scissor[4], unpack, row_length, pbo, draw_fbo;
  GLboolean blend, depth, cull, scissor_on, srgb, color[4];
} HdState;
static void hd_save(HdState *s) {
  glGetIntegerv(GL_CURRENT_PROGRAM, &s->program); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active); glGetIntegerv(GL_VIEWPORT, s->viewport);
  glGetIntegerv(GL_SCISSOR_BOX, s->scissor); glGetBooleanv(GL_COLOR_WRITEMASK, s->color);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack); glGetIntegerv(GL_UNPACK_ROW_LENGTH, &s->row_length);
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s->pbo);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fbo);
  s->blend = glIsEnabled(GL_BLEND); s->depth = glIsEnabled(GL_DEPTH_TEST);
  s->cull = glIsEnabled(GL_CULL_FACE); s->scissor_on = glIsEnabled(GL_SCISSOR_TEST);
  s->srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  for (unsigned i = 0; i < kHdTextures; ++i) {
    glActiveTexture(GL_TEXTURE0 + i);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->textures[i][0]);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &s->textures[i][1]);
  }
}
static void hd_enable(GLenum flag, GLboolean on) { if (on) glEnable(flag); else glDisable(flag); }
static void hd_restore(const HdState *s) {
  glUseProgram(s->program); glBindVertexArray(s->vao);
  for (unsigned i = 0; i < kHdTextures; ++i) {
    glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, s->textures[i][0]);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s->textures[i][1]);
  }
  glActiveTexture(s->active); glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
  glScissor(s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3]);
  hd_enable(GL_BLEND, s->blend); hd_enable(GL_DEPTH_TEST, s->depth); hd_enable(GL_CULL_FACE, s->cull);
  hd_enable(GL_SCISSOR_TEST, s->scissor_on); hd_enable(GL_FRAMEBUFFER_SRGB, s->srgb);
  glColorMask(s->color[0], s->color[1], s->color[2], s->color[3]);
  glPixelStorei(GL_UNPACK_ALIGNMENT, s->unpack); glPixelStorei(GL_UNPACK_ROW_LENGTH, s->row_length);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, s->pbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->draw_fbo);
}
static const char *vertex_source =
  "#version 330 core\n"
  "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
static const char *fragment_source =
  "uniform usampler2D native_plane,object_plane,palette_plane;\n"
  "uniform isampler2D metadata;uniform usampler2DArray picture,tilemaps;\n"
  "uniform usampler2D affine_plane,overlay_plane;uniform isampler2D coordinate_plane;\n"
  "uniform ivec2 requested,output_size;uniform int scale;out vec4 result;\n"
  "int fd(int n,int d){return n>=0?int(uint(n)/uint(d)):-1-int(uint(-(n+1))/uint(d));}\n"
  "int coord(int o,int step,int row,int x,int sx,int sy){int q=o+x*step;int b=fd(q,256);"
  "return b+fd((q-b*256)*scale+sx*step+sy*row,256*scale);}\n"
  "bool cond(int m,bool inside){return m==3||(m==1&&!inside)||(m==2&&inside);}\n"
  "uint word(int address,int layer){return texelFetch(picture,ivec3(address&255,(address>>8)&127,layer),0).r;}\n"
  "uint fetch_index(ivec2 t,int control,int layer,ivec4 m,ivec4 o){bool outside=any(lessThan(t,ivec2(0)))||any(greaterThanEqual(t,ivec2(1024)));"
  "if(outside&&(control&128)!=0&&(control&64)==0)return 0u;ivec2 p=t&1023;"
  "int tile=outside&&(control&128)!=0?0:int(word((p.y/8)*128+p.x/8,layer)&255u);"
  "if(m.x>0&&!(outside&&(control&128)!=0)){ivec2 cell=((t+o.xy)>>3)&(m.yz-1);tile=int(texelFetch(tilemaps,ivec3(cell,m.x-1),0).r);}"
  "return word(tile*64+(p.y&7)*8+(p.x&7),layer)>>8;}\n"
  "uint color(uint p,uvec4 object,int y){uint layer=(p>>8)&15u;return layer==4u||layer==6u?object.g:texelFetch(palette_plane,ivec2(int(p&255u),y),0).r;}\n"
  "void rgb(uint c){result=vec4(vec3((c>>16)&255u,(c>>8)&255u,c&255u)/255.0,1.0);}\n"
  "void main(){ivec2 pixel=ivec2(gl_FragCoord.xy);ivec2 p=ivec2((uvec2(pixel)*2u+1u)*uvec2(requested)/(uvec2(output_size)*2u));"
  "ivec2 n=p/scale;uvec4 object=texelFetch(object_plane,n,0);ivec4 cfg=texelFetch(metadata,ivec2(0,n.y),0);"
  "if(scale==1||cfg.x==1||object.a!=0u){rgb(texelFetch(native_plane,n,0).r);return;}"
  "ivec4 a=texelFetch(metadata,ivec2(1,n.y),0),b=texelFetch(metadata,ivec2(2,n.y),0),c=texelFetch(metadata,ivec2(3,n.y),0);"
  "int x=b.z+n.x;if(x<b.w||x>=c.x){rgb(0u);return;}int sx=p.x%scale,sy=p.y%scale;"
  "uint visible=object.b;uint index=0u;"
  "if((visible&3u)!=0u){ivec2 t=ivec2(coord(a.x,a.z,b.x,x,sx,sy),coord(a.y,a.w,b.y,x,sx,sy));\n"
  "#ifdef HD_AFFINE\n"
  "if(cfg.x==3){ivec4 map=texelFetch(metadata,ivec2(4,n.y),0);bvec2 uncertain=bvec2(true);"
  "if((map.w&1)!=0){ivec4 lo=texelFetch(coordinate_plane,ivec2(n.x,pixel.y),0),hi=texelFetch(coordinate_plane,ivec2(n.x+1,pixel.y),0);"
  "ivec2 frac=lo.zw&65535,delta=(hi.xy-lo.xy)*65536+(hi.zw&65535)-frac;"
  "ivec2 numerator=frac*scale+sx*delta;int den=scale*65536;ivec2 q=ivec2(fd(numerator.x,den),fd(numerator.y,den));"
  "ivec2 rem=numerator-q*den;t=lo.xy+q;uncertain=bvec2(rem.x<=2*scale||rem.x>=den-2*scale,rem.y<=2*scale||rem.y>=den-2*scale);"
  "if((map.w&2)!=0)uncertain=bvec2(false);"
  "if((lo.z&65536)!=0){t.x=lo.x;uncertain.x=false;}if((lo.w&65536)!=0){t.y=lo.y;uncertain.y=false;}}"
  "if(any(uncertain)){uvec4 d0=texelFetch(affine_plane,ivec2(0,n.y),0),d1=texelFetch(affine_plane,ivec2(1,n.y),0),d2=texelFetch(affine_plane,ivec2(2,n.y),0),d3=texelFetch(affine_plane,ivec2(3,n.y),0);"
  "precise double fy=double(sy)/double(scale),px=double(x)+double(sx)/double(scale);"
  "precise double ox=packDouble2x32(d0.xy)+packDouble2x32(d2.xy)*fy,oy=packDouble2x32(d0.zw)+packDouble2x32(d2.zw)*fy;"
  "precise double dx=packDouble2x32(d1.xy)+packDouble2x32(d3.xy)*fy,dy=packDouble2x32(d1.zw)+packDouble2x32(d3.zw)*fy;"
  "precise double tx=(ox+px*dx)/256.0LF,ty=(oy+px*dy)/256.0LF;"
  "if(uncertain.x)t.x=int(floor(tx));if(uncertain.y)t.y=int(floor(ty));}}\n"
  "#endif\n"
  "index=fetch_index(t,cfg.z,cfg.y,texelFetch(metadata,ivec2(4,n.y),0),texelFetch(metadata,ivec2(5,n.y),0));}"
  "uint bg=index!=0u?0x5000u|index:0x500u;uint mainp=(visible&1u)!=0u?bg:0x500u,sub=(visible&2u)!=0u?bg:0x500u;"
  "if((visible&4u)!=0u&&object.r>mainp)mainp=object.r;if((visible&8u)!=0u&&object.r>sub)sub=object.r;"
  "bool inside=(visible&16u)!=0u;bool clip=cond((c.z>>6)&3,inside);"
  "bool math=!cond((c.z>>4)&3,inside)&&(uint(c.w)&(1u<<((mainp>>8)&15u)))!=0u;"
  "uint v=color(mainp,object,n.y),other=uint(c.y);bool half=math&&(c.w&64)!=0;"
  "if(math&&(c.z&2)!=0){if((sub&255u)!=0u)other=color(sub,object,n.y);else half=false;}"
  "uvec3 outc=uvec3(0);for(int channel=0;channel<3;++channel){int q=clip?0:int((v>>(5*channel))&31u);"
  "if(math){int s=int((other>>(5*channel))&31u);q=max(0,q+((c.w&128)!=0?-s:s));if(half)q/=2;q=min(q,31);}"
  "outc[channel]=uint(((q<<3)|(q>>2))*cfg.w/15);}"
  "uint over=texelFetch(overlay_plane,n,0).r,alpha=over>>24;"
  "uvec3 fg=uvec3((over>>16)&255u,(over>>8)&255u,over&255u);"
  "outc=min(uvec3(255),(outc*(255u-alpha)+fg*255u+127u)/255u);"
  "result=vec4(vec3(outc)/255.0,1.0);}\n";
/* One double-precision calculation per native column and enhanced row.
 * Main sampling interpolates Q16 endpoint bounds with 32-bit integers.
 * Within two Q16 units of a texel edge it repeats the exact expression;
 * quantization therefore cannot choose a different source texel. */
static const char *coordinate_source =
  "uniform usampler2D affine_plane;uniform isampler2D metadata;"
  "uniform int requested_height,output_height,scale;out ivec4 coordinates;"
  "void main(){ivec2 p=ivec2(gl_FragCoord.xy);int py=int((uint(p.y)*2u+1u)*uint(requested_height)/(uint(output_height)*2u));"
  "int y=py/scale;ivec4 b=texelFetch(metadata,ivec2(2,y),0);"
  "uvec4 d0=texelFetch(affine_plane,ivec2(0,y),0),d1=texelFetch(affine_plane,ivec2(1,y),0),d2=texelFetch(affine_plane,ivec2(2,y),0),d3=texelFetch(affine_plane,ivec2(3,y),0);"
  "precise double fy=double(py%scale)/double(scale),x=double(b.z+p.x);"
  "precise double ox=packDouble2x32(d0.xy)+packDouble2x32(d2.xy)*fy,oy=packDouble2x32(d0.zw)+packDouble2x32(d2.zw)*fy;"
  "precise double dx=packDouble2x32(d1.xy)+packDouble2x32(d3.xy)*fy,dy=packDouble2x32(d1.zw)+packDouble2x32(d3.zw)*fy;"
  "precise double tx=(ox+x*dx)/256.0LF,ty=(oy+x*dy)/256.0LF;"
  "int wx=int(floor(tx)),wy=int(floor(ty));"
  "int fx=int(floor((tx-double(wx))*65536.0LF)),fyq=int(floor((ty-double(wy))*65536.0LF));"
  "coordinates=ivec4(wx,wy,fx|(dx==0.0LF?65536:0),fyq|(dy==0.0LF?65536:0));}\n";
static GLuint hd_shader(GLenum kind, const char *source, bool affine) {
  const char *parts[2] = {affine ? "#version 400 core\n#define HD_AFFINE 1\n" :
      "#version 330 core\n", source};
  GLuint shader = glCreateShader(kind);
  glShaderSource(shader, kind == GL_VERTEX_SHADER ? 1 : 2,
      kind == GL_VERTEX_SHADER ? &source : parts, NULL); glCompileShader(shader);
  GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[2048]; glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    fprintf(stderr, "SNES HD GPU shader: %s\n", log); glDeleteShader(shader); return 0; }
  return shader;
}
static GLuint hd_program_source(const char *source, bool affine) {
  GLuint vs = hd_shader(GL_VERTEX_SHADER, vertex_source, false);
  GLuint fs = hd_shader(GL_FRAGMENT_SHADER, source, affine);
  if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return 0; }
  GLuint program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs);
  glLinkProgram(program); glDeleteShader(vs); glDeleteShader(fs);
  GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) { glDeleteProgram(program); return 0; }
  return program;
}
static void hd_bind(SnesHdGpu *g, unsigned unit) {
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(unit == 4 || unit == 5 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D, g->textures[unit]);
}
static void hd_image(SnesHdGpu *g, unsigned unit, GLint format, unsigned w,
                     unsigned h, GLenum external, GLenum type, const void *data) {
  hd_bind(g, unit); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (g->texture_width != g->width || g->texture_height != g->height)
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, external, type, data);
  else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, external, type, data);
}
SnesHdGpu *SnesHdGpuCreate(void) {
  if (!ogl_LoadFunctions() || !ogl_IsVersionGEQ(3, 3)) return NULL;
  SnesHdGpu *g = calloc(1, sizeof(*g)); if (!g) return NULL;
  GLint max_texture = 0, layers = 0, units = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture); glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &layers);
  glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
  if (max_texture < 1024 || layers < kSnesHdMaxTilemaps || units < kHdTextures) { free(g); return NULL; }
  g->max_texture = (unsigned)max_texture;
  g->max_layers = layers < kResidentPictures ? (unsigned)layers : kResidentPictures;
  g->program = hd_program_source(fragment_source, false);
  if (!g->program) { free(g); return NULL; }
  /* Prepared double-precision camera jobs are optional. Register jobs retain
   * the GL3.3 integer path; lacking GL4 keeps the portable consumer usable. */
  if (ogl_IsVersionGEQ(4, 0)) {
    g->affine_program = hd_program_source(fragment_source, true);
    if (g->affine_program) g->coordinate_program = hd_program_source(coordinate_source, true);
  }
  HdState saved; hd_save(&saved); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glGenTextures(kHdTextures, g->textures); glGenVertexArrays(1, &g->vao); hd_bind(g, 4);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R16UI, 256, 128, g->max_layers, 0,
                GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
  hd_bind(g, 5);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8UI, kSnesHdMapWidth,
      kSnesHdMapHeight, kSnesHdMaxTilemaps, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);
  g->query_counter = (HdQueryCounter)SDL_GL_GetProcAddress("glQueryCounter");
  g->query_result = (HdQueryResult64)SDL_GL_GetProcAddress("glGetQueryObjectui64v");
  if (g->query_counter && g->query_result) glGenQueries(kQueryFrames * 3, &g->queries[0][0]);
  g->stats.gpu_timestamps = g->query_counter && g->query_result;
  g->stats.source_gpu_bytes = (size_t)g->max_layers * 65536 +
      (size_t)kSnesHdMapWidth * kSnesHdMapHeight * kSnesHdMaxTilemaps;
  bool ok = glGetError() == GL_NO_ERROR; hd_restore(&saved);
  if (!ok) { SnesHdGpuDestroy(g); return NULL; }
  fprintf(stderr, "SNES HD GPU available: %s, max texture%u, source layers%u\n",
            glGetString(GL_RENDERER), g->max_texture, g->max_layers);
  return g;
}
void SnesHdGpuReset(SnesHdGpu *g) {
  if (!g) return;
  for (unsigned i = 0; i < g->max_layers; ++i) {
    free(g->resident[i].copy); memset(&g->resident[i], 0, sizeof(g->resident[i]));
  }
  for (unsigned i = 0; i < kSnesHdMaxTilemaps; ++i) {
    free(g->maps[i]); g->maps[i] = NULL; g->map_seen[i] = 0;
  }
}
bool SnesHdGpuSupportsAffine(const SnesHdGpu *g) { return g && g->affine_program; }
void SnesHdGpuDestroy(SnesHdGpu *g) {
  if (!g) return;
  SnesHdGpuReset(g); glDeleteTextures(kHdTextures, g->textures); glDeleteVertexArrays(1, &g->vao);
  if (g->program) glDeleteProgram(g->program);
  if (g->affine_program) glDeleteProgram(g->affine_program);
  if (g->coordinate_program) glDeleteProgram(g->coordinate_program);
  if (g->coordinate_fbo) glDeleteFramebuffers(1, &g->coordinate_fbo);
  if (g->stats.gpu_timestamps) glDeleteQueries(kQueryFrames * 3, &g->queries[0][0]);
  free(g->native_pixels); free(g->overlay); free(g->objects); free(g->palette); free(g->metadata); free(g->affine); free(g);
}
bool SnesHdGpuAdmit(SnesHdGpu *g, unsigned w, unsigned h, unsigned scale,
                    size_t budget, unsigned *out_w, unsigned *out_h) {
  if (!g || !out_w || !out_h || !w || w > kSnesHdMaxWidth || !h ||
      h > kSnesHdMaxHeight || !scale || scale > kSnesHdMaxScale || budget < 4) return false;
  unsigned rw = w * scale, rh = h * scale, aw = rw, ah = rh;
  while (aw > g->max_texture || ah > g->max_texture ||
      (uint64_t)aw * ah * 4 + (uint64_t)(w + 1) * ah * 16 > budget) {
    aw = aw > 1 ? aw / 2 : 1; ah = ah > 1 ? ah / 2 : 1;
  }
  *out_w = aw; *out_h = ah; return true;
}
bool SnesHdGpuReadStats(SnesHdGpu *g, bool wait, SnesHdGpuStats *stats) {
  if (!g || !stats) return false;
  if (g->stats.gpu_timestamps && g->query_pending) {
    unsigned slot = (g->query_pending - 1) % kQueryFrames;
    GLint ready = 0; glGetQueryObjectiv(g->queries[slot][2], GL_QUERY_RESULT_AVAILABLE, &ready);
    if (ready || wait) {
      uint64_t t[3];
      for (unsigned i = 0; i < 3; ++i) g->query_result(g->queries[slot][i], GL_QUERY_RESULT, &t[i]);
      g->stats.upload_gpu_ns = t[1] - t[0]; g->stats.draw_gpu_ns = t[2] - t[1];
      g->stats.timed_frame = g->query_frame[slot];
      g->query_pending = 0;
    }
  }
  *stats = g->stats; return true;
}
bool SnesHdGpuDraw(SnesHdGpu *g, const SnesHdFrame *frame, unsigned scale,
                   unsigned width, unsigned height) {
  SnesHdView v;
  if (!g || g->failed || !SnesHdFrameView(frame, &v) || !scale || scale > 64 ||
      !width || !height || width > g->max_texture || height > g->max_texture ||
      v.source_count > g->max_layers) return false;
  bool affine = false;
  for (unsigned y = 0; y < v.height; ++y) affine |= v.lines[y].kind == kSnesHdAffine;
  if (affine && !g->affine_program) return false;
  if (g->width != v.width || g->height != v.height) {
    uint32_t *native = malloc(v.width * v.height * 4);
    uint32_t *overlay = malloc(v.width * v.height * 4);
    uint16_t *objects = calloc((size_t)v.width * v.height * 4, 2);
    uint16_t *palette = malloc(v.height * 512);
    int32_t *meta = calloc(v.height, 96);
    double *transforms = malloc(v.height * 64);
    if (!native || !overlay || !objects || !palette || !meta || !transforms) {
      free(native); free(overlay); free(objects); free(palette); free(meta); free(transforms); return false;
    }
    free(g->native_pixels); free(g->overlay); free(g->objects); free(g->palette); free(g->metadata); free(g->affine);
    g->native_pixels = native; g->objects = objects; g->palette = palette; g->metadata = meta;
    g->affine = transforms;
    g->overlay = overlay;
    g->width = v.width; g->height = v.height;
    /* Texture storage is resized below while preserving the caller's state. */
  }
  SnesHdGpuStats previous; SnesHdGpuReadStats(g, false, &previous);
  unsigned source_layers[kSnesHdMaxHeight]; ++g->serial;
  /* Pin every referenced resident source before choosing eviction victims. */
  for (unsigned s = 0; s < v.source_count; ++s) {
    unsigned i;
    for (i = 0; i < g->max_layers; ++i)
      if (g->resident[i].copy && g->resident[i].hash == v.sources[s]->hash &&
          !memcmp(g->resident[i].copy, v.sources[s]->vram, 65536)) break;
    source_layers[s] = i;
    if (i < g->max_layers) g->resident[i].seen = g->serial;
  }
  for (unsigned s = 0; s < v.source_count; ++s) if (source_layers[s] == g->max_layers) {
    unsigned i;
    for (i = 0; i < g->max_layers; ++i) if (g->resident[i].seen != g->serial) break;
    if (i == g->max_layers) return false;
    source_layers[s] = i; g->resident[i].seen = g->serial;
  }
  unsigned map_layers[kSnesHdMaxTilemaps];
  for (unsigned s = 0; s < v.tilemap_count; ++s) {
    const SnesHdTilemap *m = v.tilemaps[s]; unsigned i;
    for (i = 0; i < kSnesHdMaxTilemaps; ++i) {
      const SnesHdTilemap *p = g->maps[i];
      if (p && p->hash == m->hash && p->width == m->width && p->height == m->height &&
          !memcmp(p->texels, m->texels, (size_t)m->width * m->height)) break;
    }
    map_layers[s] = i;
    if (i < kSnesHdMaxTilemaps) g->map_seen[i] = g->serial;
  }
  for (unsigned s = 0; s < v.tilemap_count; ++s) if (map_layers[s] == kSnesHdMaxTilemaps) {
    unsigned i;
    for (i = 0; i < kSnesHdMaxTilemaps; ++i) if (g->map_seen[i] != g->serial) break;
    if (i == kSnesHdMaxTilemaps) return false;
    map_layers[s] = i; g->map_seen[i] = g->serial;
  }
  for (unsigned y = 0; y < v.height; ++y) {
    const SnesHdLine *j = &v.lines[y];
    memcpy(g->native_pixels + y * v.width, j->native, v.width * 4);
    memcpy(g->overlay + y * v.width, j->overlay, v.width * 4);
    memcpy(g->palette + y * 256, j->palette, 512);
    for (unsigned x = 0; x < v.width; ++x) {
      uint16_t *p = g->objects + (y * v.width + x) * 4;
      p[0] = j->object[x]; p[1] = j->object_color[x];
      p[2] = j->visibility[x]; p[3] = j->native_mask[x];
    }
    int32_t *m = g->metadata + y * 24;
    m[0] = j->kind; m[1] = j->kind != kSnesHdNative ? (int)source_layers[j->source] : 0;
    m[2] = j->control; m[3] = j->brightness;
    m[4] = j->origin_x; m[5] = j->origin_y; m[6] = j->step_x; m[7] = j->step_y;
    m[8] = j->row_x; m[9] = j->row_y; m[10] = j->screen_left; m[11] = j->active_left;
    m[12] = j->active_right; m[13] = j->fixed_color; m[14] = j->cgwsel; m[15] = j->cgadsub;
    const SnesHdTilemap *map = j->tilemap ? v.tilemaps[j->tilemap - 1] : NULL;
    m[16] = map ? (int)map_layers[j->tilemap - 1] + 1 : 0;
    m[17] = map ? (int)map->width : 0; m[18] = map ? (int)map->height : 0; m[19] = 0;
    if (g->coordinate_program && j->kind == kSnesHdAffine &&
        fabs(j->affine[2]) + fabs(j->affine[6]) < 65536 &&
        fabs(j->affine[3]) + fabs(j->affine[7]) < 65536) {
      m[19] = 1;
      bool integral = true;
      for (unsigned i = 0; i < 8; ++i) integral &= floor(j->affine[i]) == j->affine[i];
      if (integral && !(scale & (scale - 1))) m[19] |= 2;
    }
    m[20] = j->map_offset_x; m[21] = j->map_offset_y; m[22] = m[23] = 0;
    memcpy(g->affine + y * 8, j->affine, 64);
  }
  HdState saved; hd_save(&saved); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  unsigned query = (unsigned)g->stats.frames % kQueryFrames;
  if (g->stats.gpu_timestamps) g->query_counter(g->queries[query][0], GL_TIMESTAMP);
  uint64_t uploaded = 0;
  hd_bind(g, 4);
  for (unsigned s = 0; s < v.source_count; ++s) {
    HdResident *p = &g->resident[source_layers[s]];
    if (p->copy && p->hash == v.sources[s]->hash && !memcmp(p->copy, v.sources[s]->vram, 65536)) continue;
    if (!p->copy) p->copy = malloc(65536);
    if (!p->copy) { hd_restore(&saved); return false; }
    memcpy(p->copy, v.sources[s]->vram, 65536); p->hash = v.sources[s]->hash;
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, source_layers[s], 256, 128, 1,
                     GL_RED_INTEGER, GL_UNSIGNED_SHORT, p->copy); uploaded += 65536;
  }
  hd_bind(g, 5);
  for (unsigned s = 0; s < v.tilemap_count; ++s) {
    const SnesHdTilemap *m = v.tilemaps[s]; SnesHdTilemap **p = &g->maps[map_layers[s]];
    if (*p && (*p)->hash == m->hash && (*p)->width == m->width && (*p)->height == m->height &&
        !memcmp((*p)->texels, m->texels, (size_t)m->width * m->height)) continue;
    if (!*p) *p = malloc(sizeof(**p));
    if (!*p) { hd_restore(&saved); return false; }
    (*p)->hash = m->hash; (*p)->width = m->width; (*p)->height = m->height;
    size_t bytes = (size_t)m->width * m->height;
    memcpy((*p)->texels, m->texels, bytes);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, map_layers[s], m->width, m->height, 1,
        GL_RED_INTEGER, GL_UNSIGNED_BYTE, m->texels); uploaded += bytes;
  }
  /* All staging/upload dimensions are native-sized and independent of scale. */
  hd_image(g, 0, GL_R32UI, v.width, v.height, GL_RED_INTEGER, GL_UNSIGNED_INT, g->native_pixels);
  hd_image(g, 1, GL_RGBA16UI, v.width, v.height, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, g->objects);
  hd_image(g, 2, GL_R16UI, 256, v.height, GL_RED_INTEGER, GL_UNSIGNED_SHORT, g->palette);
  hd_image(g, 3, GL_RGBA32I, 6, v.height, GL_RGBA_INTEGER, GL_INT, g->metadata);
  hd_image(g, 6, GL_RGBA32UI, 4, v.height, GL_RGBA_INTEGER, GL_UNSIGNED_INT, g->affine);
  hd_image(g, 7, GL_R32UI, v.width, v.height, GL_RED_INTEGER, GL_UNSIGNED_INT, g->overlay);
  g->texture_width = g->width; g->texture_height = g->height;
  uploaded += (uint64_t)v.width * v.height * 16 + v.height * (512 + 96 + 64);
  if (g->stats.gpu_timestamps) g->query_counter(g->queries[query][1], GL_TIMESTAMP);
  glBindVertexArray(g->vao); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_FRAMEBUFFER_SRGB);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  if (affine && g->coordinate_program) {
    hd_bind(g, 8);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (g->coordinate_width != v.width + 1 || g->coordinate_height != height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, v.width + 1, height, 0, GL_RGBA_INTEGER, GL_INT, NULL);
      g->coordinate_width = v.width + 1; g->coordinate_height = height;
    }
    if (!g->coordinate_fbo) glGenFramebuffers(1, &g->coordinate_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->coordinate_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->textures[8], 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { hd_restore(&saved); return false; }
    glUseProgram(g->coordinate_program);
    hd_bind(g, 3); hd_bind(g, 6);
    glUniform1i(glGetUniformLocation(g->coordinate_program,"metadata"),3);
    glUniform1i(glGetUniformLocation(g->coordinate_program,"affine_plane"),6);
    glUniform1i(glGetUniformLocation(g->coordinate_program,"requested_height"),v.height*scale);
    glUniform1i(glGetUniformLocation(g->coordinate_program,"output_height"),height);
    glUniform1i(glGetUniformLocation(g->coordinate_program,"scale"),scale);
    glViewport(0,0,v.width+1,height); glDrawArrays(GL_TRIANGLES,0,3);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,saved.draw_fbo);
  }
  GLuint program = affine ? g->affine_program : g->program;
  glUseProgram(program); glBindVertexArray(g->vao);
  const char *names[kHdTextures] = {"native_plane", "object_plane", "palette_plane", "metadata", "picture", "tilemaps", "affine_plane", "overlay_plane", "coordinate_plane"};
  for (unsigned i = 0; i < kHdTextures; ++i) { hd_bind(g, i); glUniform1i(glGetUniformLocation(program, names[i]), i); }
  glUniform2i(glGetUniformLocation(program, "requested"), v.width * scale, v.height * scale);
  glUniform2i(glGetUniformLocation(program, "output_size"), width, height);
  glUniform1i(glGetUniformLocation(program, "scale"), scale);
  glViewport(0, 0, width, height); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_FRAMEBUFFER_SRGB);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDrawArrays(GL_TRIANGLES, 0, 3);
  if (g->stats.gpu_timestamps) {
    g->query_counter(g->queries[query][2], GL_TIMESTAMP); g->query_pending = query + 1;
    g->query_frame[query] = g->stats.frames + 1;
  }
  GLenum error = glGetError(); hd_restore(&saved);
  if (error != GL_NO_ERROR) { g->failed = true; fprintf(stderr, "SNES HD GPU error0x%x; native fallback\n", error); return false; }
  g->stats.frames++; g->stats.uploaded_bytes = uploaded;
  g->stats.requested_width = v.width * scale; g->stats.requested_height = v.height * scale;
  g->stats.output_width = width; g->stats.output_height = height; g->stats.target_bytes = (size_t)width * height * 4;
  return true;
}
#else
struct SnesHdGpu { int unused; };
SnesHdGpu *SnesHdGpuCreate(void) { return NULL; }
bool SnesHdGpuSupportsAffine(const SnesHdGpu *g) { (void)g; return false; }
void SnesHdGpuDestroy(SnesHdGpu *g) { (void)g; }
void SnesHdGpuReset(SnesHdGpu *g) { (void)g; }
bool SnesHdGpuAdmit(SnesHdGpu *g,unsigned w,unsigned h,unsigned s,size_t b,unsigned *x,unsigned *y) {
  (void)g;(void)w;(void)h;(void)s;(void)b;(void)x;(void)y;return false;
}
bool SnesHdGpuDraw(SnesHdGpu *g,const SnesHdFrame *f,unsigned s,unsigned w,unsigned h) {
  (void)g;(void)f;(void)s;(void)w;(void)h;return false;
}
bool SnesHdGpuReadStats(SnesHdGpu *g,bool wait,SnesHdGpuStats *stats) {
  (void)g;(void)wait;(void)stats;return false;
}
#endif
