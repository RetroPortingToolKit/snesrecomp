#include "hd_gpu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(SNESRECOMP_HD_GPU) && SNESRECOMP_HD_GPU
#include "gl_core_3_1.h"
#include "sdl_compat.h"
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8e28
#endif
typedef void (CODEGEN_FUNCPTR *HdQueryCounter)(GLuint, GLenum);
typedef void (CODEGEN_FUNCPTR *HdQueryResult64)(GLuint, GLenum, uint64_t *);
enum { kResidentPictures = 256, kQueryFrames = 4 };
typedef struct HdResident {
  uint64_t hash, seen;
  uint16_t *copy;
} HdResident;
struct SnesHdGpu {
  GLuint program, vao, textures[5], queries[kQueryFrames][3];
  unsigned width, height, texture_width, texture_height, max_texture, max_layers, query_pending;
  bool failed;
  uint64_t serial;
  uint32_t *native_pixels;
  uint16_t *objects, *palette;
  int32_t *metadata;
  HdResident resident[kResidentPictures];
  HdQueryCounter query_counter;
  HdQueryResult64 query_result;
  SnesHdGpuStats stats;
};
typedef struct HdState {
  GLint program, vao, active, textures[5][2], viewport[4], scissor[4], unpack, row_length, pbo;
  GLboolean blend, depth, cull, scissor_on, srgb, color[4];
} HdState;
static void hd_save(HdState *s) {
  glGetIntegerv(GL_CURRENT_PROGRAM, &s->program); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active); glGetIntegerv(GL_VIEWPORT, s->viewport);
  glGetIntegerv(GL_SCISSOR_BOX, s->scissor); glGetBooleanv(GL_COLOR_WRITEMASK, s->color);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack); glGetIntegerv(GL_UNPACK_ROW_LENGTH, &s->row_length);
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s->pbo);
  s->blend = glIsEnabled(GL_BLEND); s->depth = glIsEnabled(GL_DEPTH_TEST);
  s->cull = glIsEnabled(GL_CULL_FACE); s->scissor_on = glIsEnabled(GL_SCISSOR_TEST);
  s->srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  for (unsigned i = 0; i < 5; ++i) {
    glActiveTexture(GL_TEXTURE0 + i);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->textures[i][0]);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &s->textures[i][1]);
  }
}
static void hd_enable(GLenum flag, GLboolean on) { if (on) glEnable(flag); else glDisable(flag); }
static void hd_restore(const HdState *s) {
  glUseProgram(s->program); glBindVertexArray(s->vao);
  for (unsigned i = 0; i < 5; ++i) {
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
}
static const char *vertex_source =
  "#version 330 core\n"
  "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
static const char *fragment_source =
  "#version 330 core\n"
  "uniform usampler2D native_plane,object_plane,palette_plane;\n"
  "uniform isampler2D metadata;uniform usampler2DArray picture;\n"
  "uniform ivec2 requested,output_size;uniform int scale;out vec4 result;\n"
  "int fd(int n,int d){return n>=0?int(uint(n)/uint(d)):-1-int(uint(-(n+1))/uint(d));}\n"
  "int coord(int o,int step,int row,int x,int sx,int sy){int q=o+x*step;int b=fd(q,256);"
  "return b+fd((q-b*256)*scale+sx*step+sy*row,256*scale);}\n"
  "bool cond(int m,bool inside){return m==3||(m==1&&!inside)||(m==2&&inside);}\n"
  "uint word(int address,int layer){return texelFetch(picture,ivec3(address&255,(address>>8)&127,layer),0).r;}\n"
  "uint fetch_index(ivec2 t,int control,int layer){bool outside=any(lessThan(t,ivec2(0)))||any(greaterThanEqual(t,ivec2(1024)));"
  "if(outside&&(control&128)!=0&&(control&64)==0)return 0u;ivec2 p=t&1023;"
  "int tile=outside&&(control&128)!=0?0:int(word((p.y/8)*128+p.x/8,layer)&255u);"
  "return word(tile*64+(p.y&7)*8+(p.x&7),layer)>>8;}\n"
  "uint color(uint p,uvec4 object,int y){uint layer=(p>>8)&15u;return layer==4u||layer==6u?object.g:texelFetch(palette_plane,ivec2(int(p&255u),y),0).r;}\n"
  "void rgb(uint c){result=vec4(vec3((c>>16)&255u,(c>>8)&255u,c&255u)/255.0,1.0);}\n"
  "void main(){ivec2 p=ivec2(gl_FragCoord.xy);p=p*(requested/output_size)+p*(requested%output_size)/output_size;"
  "ivec2 n=p/scale;uvec4 object=texelFetch(object_plane,n,0);ivec4 cfg=texelFetch(metadata,ivec2(0,n.y),0);"
  "if(scale==1||cfg.x!=2||object.a!=0u){rgb(texelFetch(native_plane,n,0).r);return;}"
  "ivec4 a=texelFetch(metadata,ivec2(1,n.y),0),b=texelFetch(metadata,ivec2(2,n.y),0),c=texelFetch(metadata,ivec2(3,n.y),0);"
  "int x=b.z+n.x;if(x<b.w||x>=c.x){rgb(0u);return;}int sx=p.x%scale,sy=p.y%scale;"
  "uint visible=object.b;uint index=0u;"
  "if((visible&3u)!=0u){ivec2 t=ivec2(coord(a.x,a.z,b.x,x,sx,sy),coord(a.y,a.w,b.y,x,sx,sy));index=fetch_index(t,cfg.z,cfg.y);}"
  "uint bg=index!=0u?0x5000u|index:0x500u;uint mainp=(visible&1u)!=0u?bg:0x500u,sub=(visible&2u)!=0u?bg:0x500u;"
  "if((visible&4u)!=0u&&object.r>mainp)mainp=object.r;if((visible&8u)!=0u&&object.r>sub)sub=object.r;"
  "bool inside=(visible&16u)!=0u;bool clip=cond((c.z>>6)&3,inside);"
  "bool math=!cond((c.z>>4)&3,inside)&&(uint(c.w)&(1u<<((mainp>>8)&15u)))!=0u;"
  "uint v=color(mainp,object,n.y),other=uint(c.y);bool half=math&&(c.w&64)!=0;"
  "if(math&&(c.z&2)!=0){if((sub&255u)!=0u)other=color(sub,object,n.y);else half=false;}"
  "uvec3 outc=uvec3(0);for(int channel=0;channel<3;++channel){int q=clip?0:int((v>>(5*channel))&31u);"
  "if(math){int s=int((other>>(5*channel))&31u);q=max(0,q+((c.w&128)!=0?-s:s));if(half)q/=2;q=min(q,31);}"
  "outc[channel]=uint(((q<<3)|(q>>2))*cfg.w/15);}result=vec4(vec3(outc)/255.0,1.0);}\n";
static GLuint hd_shader(GLenum kind, const char *source) {
  GLuint shader = glCreateShader(kind); glShaderSource(shader, 1, &source, NULL); glCompileShader(shader);
  GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[2048]; glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    fprintf(stderr, "SNES HD GPU shader: %s\n", log); glDeleteShader(shader); return 0; }
  return shader;
}
static void hd_bind(SnesHdGpu *g, unsigned unit) {
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(unit == 4 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D, g->textures[unit]);
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
  if (max_texture < 512 || layers < 1 || units < 5) { free(g); return NULL; }
  g->max_texture = (unsigned)max_texture;
  g->max_layers = layers < kResidentPictures ? (unsigned)layers : kResidentPictures;
  GLuint vs = hd_shader(GL_VERTEX_SHADER, vertex_source), fs = hd_shader(GL_FRAGMENT_SHADER, fragment_source);
  if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); free(g); return NULL; }
  g->program = glCreateProgram(); glAttachShader(g->program, vs); glAttachShader(g->program, fs);
  glLinkProgram(g->program); glDeleteShader(vs); glDeleteShader(fs);
  GLint ok = 0; glGetProgramiv(g->program, GL_LINK_STATUS, &ok);
  if (!ok) { glDeleteProgram(g->program); free(g); return NULL; }
  HdState saved; hd_save(&saved); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glGenTextures(5, g->textures); glGenVertexArrays(1, &g->vao); hd_bind(g, 4);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R16UI, 256, 128, g->max_layers, 0,
                GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
  g->query_counter = (HdQueryCounter)SDL_GL_GetProcAddress("glQueryCounter");
  g->query_result = (HdQueryResult64)SDL_GL_GetProcAddress("glGetQueryObjectui64v");
  if (g->query_counter && g->query_result) glGenQueries(kQueryFrames * 3, &g->queries[0][0]);
  g->stats.gpu_timestamps = g->query_counter && g->query_result;
  g->stats.source_gpu_bytes = (size_t)g->max_layers * 65536;
  ok = glGetError() == GL_NO_ERROR; hd_restore(&saved);
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
}
void SnesHdGpuDestroy(SnesHdGpu *g) {
  if (!g) return;
  SnesHdGpuReset(g); glDeleteTextures(5, g->textures); glDeleteVertexArrays(1, &g->vao);
  if (g->program) glDeleteProgram(g->program);
  if (g->stats.gpu_timestamps) glDeleteQueries(kQueryFrames * 3, &g->queries[0][0]);
  free(g->native_pixels); free(g->objects); free(g->palette); free(g->metadata); free(g);
}
bool SnesHdGpuAdmit(SnesHdGpu *g, unsigned w, unsigned h, unsigned scale,
                    size_t budget, unsigned *out_w, unsigned *out_h) {
  if (!g || !out_w || !out_h || !w || w > kSnesHdMaxWidth || !h ||
      h > kSnesHdMaxHeight || !scale || scale > kSnesHdMaxScale || budget < 4) return false;
  unsigned rw = w * scale, rh = h * scale, aw = rw, ah = rh;
  while (aw > g->max_texture || ah > g->max_texture || (uint64_t)aw * ah * 4 > budget) {
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
  if (g->width != v.width || g->height != v.height) {
    uint32_t *native = malloc(v.width * v.height * 4);
    uint16_t *objects = calloc((size_t)v.width * v.height * 4, 2);
    uint16_t *palette = malloc(v.height * 512);
    int32_t *meta = malloc(v.height * 64);
    if (!native || !objects || !palette || !meta) {
      free(native); free(objects); free(palette); free(meta); return false;
    }
    free(g->native_pixels); free(g->objects); free(g->palette); free(g->metadata);
    g->native_pixels = native; g->objects = objects; g->palette = palette; g->metadata = meta;
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
  for (unsigned y = 0; y < v.height; ++y) {
    const SnesHdLine *j = &v.lines[y];
    memcpy(g->native_pixels + y * v.width, j->native, v.width * 4);
    memcpy(g->palette + y * 256, j->palette, 512);
    for (unsigned x = 0; x < v.width; ++x) {
      uint16_t *p = g->objects + (y * v.width + x) * 4;
      p[0] = j->object[x]; p[1] = j->object_color[x];
      p[2] = j->visibility[x]; p[3] = j->native_mask[x];
    }
    int32_t *m = g->metadata + y * 16;
    m[0] = j->kind; m[1] = j->kind == 2 ? (int)source_layers[j->source] : 0;
    m[2] = j->control; m[3] = j->brightness;
    m[4] = j->origin_x; m[5] = j->origin_y; m[6] = j->step_x; m[7] = j->step_y;
    m[8] = j->row_x; m[9] = j->row_y; m[10] = j->screen_left; m[11] = j->active_left;
    m[12] = j->active_right; m[13] = j->fixed_color; m[14] = j->cgwsel; m[15] = j->cgadsub;
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
  /* All staging/upload dimensions are native-sized and independent of scale. */
  hd_image(g, 0, GL_R32UI, v.width, v.height, GL_RED_INTEGER, GL_UNSIGNED_INT, g->native_pixels);
  hd_image(g, 1, GL_RGBA16UI, v.width, v.height, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, g->objects);
  hd_image(g, 2, GL_R16UI, 256, v.height, GL_RED_INTEGER, GL_UNSIGNED_SHORT, g->palette);
  hd_image(g, 3, GL_RGBA32I, 4, v.height, GL_RGBA_INTEGER, GL_INT, g->metadata);
  g->texture_width = g->width; g->texture_height = g->height;
  uploaded += (uint64_t)v.width * v.height * 12 + v.height * (512 + 64);
  if (g->stats.gpu_timestamps) g->query_counter(g->queries[query][1], GL_TIMESTAMP);
  glUseProgram(g->program); glBindVertexArray(g->vao);
  const char *names[5] = {"native_plane", "object_plane", "palette_plane", "metadata", "picture"};
  for (unsigned i = 0; i < 5; ++i) { hd_bind(g, i); glUniform1i(glGetUniformLocation(g->program, names[i]), i); }
  glUniform2i(glGetUniformLocation(g->program, "requested"), v.width * scale, v.height * scale);
  glUniform2i(glGetUniformLocation(g->program, "output_size"), width, height);
  glUniform1i(glGetUniformLocation(g->program, "scale"), scale);
  glViewport(0, 0, width, height); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_FRAMEBUFFER_SRGB);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDrawArrays(GL_TRIANGLES, 0, 3);
  if (g->stats.gpu_timestamps) { g->query_counter(g->queries[query][2], GL_TIMESTAMP); g->query_pending = query + 1; }
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
