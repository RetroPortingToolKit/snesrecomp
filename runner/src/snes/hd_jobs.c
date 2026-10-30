#include "hd_jobs_internal.h"
#include "mode7_hd.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>


static bool SnesHdWindowCondition(unsigned mode, bool inside) {
  return mode == 3 || (mode == 1 && !inside) || (mode == 2 && inside);
}


SnesHdFrame *SnesHdFrameCreate(unsigned width, unsigned height) {
  if (!width || width > kSnesHdMaxWidth || !height || height > kSnesHdMaxHeight)
    return NULL;
  SnesHdFrame *f = calloc(1, sizeof(*f));
  if (!f) return NULL;
  f->lines = calloc(height, sizeof(*f->lines));
  if (!f->lines) { free(f); return NULL; }
  f->width = width; f->height = height;
  return f;
}
void SnesHdFrameDestroy(SnesHdFrame *f) {
  if (!f) return;
  for (unsigned i = 0; i < f->source_count; ++i) free(f->sources[i]);
  for (unsigned i = 0; i < f->tilemap_count; ++i) free(f->tilemaps[i]);
  free(f->lines); free(f);
}
int SnesHdFrameAddPicture(SnesHdFrame *f, const uint16_t vram[32768]) {
  if (!f || f->sealed || f->failed || !vram) return -1;
  uint64_t hash = UINT64_C(14695981039346656037);
  for (unsigned i = 0; i < 32768; ++i) {
    hash ^= vram[i]; hash *= UINT64_C(1099511628211);
  }
  for (unsigned i = 0; i < f->source_count; ++i)
    if (f->sources[i]->hash == hash &&
        !memcmp(f->sources[i]->vram, vram, 65536)) return (int)i;
  if (f->source_count == kSnesHdMaxHeight) { f->failed = true; return -1; }
  SnesHdPicture *s = malloc(sizeof(*s));
  if (!s) { f->failed = true; return -1; }
  memcpy(s->vram, vram, sizeof(s->vram));
  s->hash = hash; f->sources[f->source_count] = s;
  return (int)f->source_count++;
}
int SnesHdFrameAddTilemap(SnesHdFrame *f, const uint8_t *tiles,
                         unsigned width, unsigned height) {
  if (!f || f->sealed || f->failed || !tiles || !width || !height ||
      width > kSnesHdMapWidth || height > kSnesHdMapHeight ||
      (width & (width - 1)) || (height & (height - 1))) return -1;
  uint64_t hash = UINT64_C(14695981039346656037) ^ width ^ ((uint64_t)height << 32);
  size_t bytes = (size_t)width * height;
  for (size_t i = 0; i < bytes; ++i) { hash ^= tiles[i]; hash *= UINT64_C(1099511628211); }
  for (unsigned i = 0; i < f->tilemap_count; ++i) {
    const SnesHdTilemap *m = f->tilemaps[i];
    if (m->hash == hash && m->width == width && m->height == height &&
        !memcmp(m->texels, tiles, bytes)) return (int)i;
  }
  if (f->tilemap_count == kSnesHdMaxTilemaps) return -1;
  SnesHdTilemap *m = malloc(sizeof(*m));
  if (!m) return -1;
  m->hash = hash; m->width = width; m->height = height;
  memcpy(m->texels, tiles, bytes); f->tilemaps[f->tilemap_count] = m;
  return (int)f->tilemap_count++;
}
bool SnesHdFrameSetLine(SnesHdFrame *f, unsigned y, const SnesHdLine *j) {
  if (!f || f->sealed || f->failed || !j || y >= f->height ||
      j->kind < kSnesHdNative || j->kind > kSnesHdAffine ||
      (j->kind != kSnesHdNative && j->source >= f->source_count) ||
      j->tilemap > f->tilemap_count || j->brightness > 15) return false;
  if (j->kind == kSnesHdAffine) {
    for (unsigned i = 0; i < 8; ++i)
      if (!isfinite(j->affine[i]) || fabs(j->affine[i]) > 1e9) return false;
    for (unsigned row = 0; row < 2; ++row) for (unsigned side = 0; side < 2; ++side)
      for (unsigned axis = 0; axis < 2; ++axis) {
        double x = j->screen_left + side * (double)f->width;
        double q = j->affine[axis] + row * j->affine[axis + 4] +
            x * (j->affine[axis + 2] + row * j->affine[axis + 6]);
        if (!isfinite(q) || fabs(q) > 256e6) return false;
      }
  }
  f->lines[y] = *j; return true;
}
bool SnesHdFrameSeal(SnesHdFrame *f) {
  if (!f || f->sealed || f->failed) return false;
  for (unsigned i = 0; i < f->height; ++i)
    if (!f->lines[i].kind) return false;
  f->sealed = true; return true;
}
bool SnesHdFrameView(const SnesHdFrame *f, SnesHdView *v) {
  if (!f || !v || !f->sealed || f->failed) return false;
  *v = (SnesHdView){f->width, f->height, f->source_count, f->lines,
      (const SnesHdPicture *const *)f->sources,
      sizeof(*f) + f->height * sizeof(*f->lines) +
          f->source_count * sizeof(SnesHdPicture) +
          f->tilemap_count * sizeof(SnesHdTilemap), f->tilemap_count,
      (const SnesHdTilemap *const *)f->tilemaps};
  return true;
}
bool SnesHdFrameCopyLine(SnesHdFrame *f, unsigned y, const SnesHdFrame *s,
                        unsigned sy, unsigned crop) {
  if (!f || f->sealed || f->failed || !s || !s->sealed || s->failed ||
      y >= f->height || sy >= s->height || crop > s->width ||
      f->width > s->width - crop) return false;
  const SnesHdLine *a = &s->lines[sy]; SnesHdLine *b = &f->lines[y];
  *b = *a;
  memmove(b->native, a->native + crop, f->width * sizeof(uint32_t));
  memmove(b->overlay, a->overlay + crop, f->width * sizeof(uint32_t));
  memmove(b->object, a->object + crop, f->width * sizeof(uint16_t));
  memmove(b->object_color, a->object_color + crop, f->width * sizeof(uint16_t));
  memmove(b->visibility, a->visibility + crop, f->width);
  memmove(b->native_mask, a->native_mask + crop, f->width);
  b->screen_left += (int16_t)crop;
  if (a->kind != kSnesHdNative) {
    int n = SnesHdFrameAddPicture(f, s->sources[a->source]->vram);
    if (n < 0) return false;
    b->source = (uint16_t)n;
  }
  if (a->tilemap) {
    const SnesHdTilemap *m = s->tilemaps[a->tilemap - 1];
    int n = SnesHdFrameAddTilemap(f, m->texels, m->width, m->height);
    if (n < 0) return false;
    b->tilemap = (uint16_t)(n + 1);
  }
  return true;
}
bool SnesHdFrameCopyRows(SnesHdFrame *f, unsigned y, const SnesHdFrame *s) {
  if (!f || f->sealed || f->failed || !s || !s->sealed || s->failed ||
      y > f->height || s->height > f->height - y || f->width != s->width) return false;
  uint16_t sources[kSnesHdMaxHeight], maps[kSnesHdMaxTilemaps];
  for (unsigned i = 0; i < s->source_count; ++i) {
    int n = SnesHdFrameAddPicture(f, s->sources[i]->vram);
    if (n < 0) return false;
    sources[i] = (uint16_t)n;
  }
  for (unsigned i = 0; i < s->tilemap_count; ++i) {
    const SnesHdTilemap *m = s->tilemaps[i];
    int n = SnesHdFrameAddTilemap(f, m->texels, m->width, m->height);
    if (n < 0) return false;
    maps[i] = (uint16_t)(n + 1);
  }
  for (unsigned i = 0; i < s->height; ++i) {
    SnesHdLine *j = &f->lines[y + i]; *j = s->lines[i];
    if (j->kind != kSnesHdNative) j->source = sources[j->source];
    if (j->tilemap) j->tilemap = maps[j->tilemap - 1];
  }
  return true;
}
bool SnesHdFrameSetOverlay(SnesHdFrame *f, const uint32_t *argb, size_t pitch) {
  if (!f || f->sealed || f->failed || !argb || pitch < f->width * 4) return false;
  for (unsigned y = 0; y < f->height; ++y)
    memcpy(f->lines[y].overlay, (const uint8_t *)argb + y * pitch, f->width * 4);
  return true;
}
bool SnesHdFrameSetNative(SnesHdFrame *f, const uint32_t *pixels,
                          size_t pitch, const uint8_t *mask, size_t mp) {
  if (!f || f->sealed || f->failed || !pixels || pitch < f->width * 4 ||
      (mask && mp < f->width)) return false;
  for (unsigned y = 0; y < f->height; ++y) {
    memcpy(f->lines[y].native, (const uint8_t *)pixels + y * pitch, f->width * 4);
    if (mask) memcpy(f->lines[y].native_mask, mask + y * mp, f->width);
  }
  return true;
}

static int SnesHdFloorDiv(int n, int d) { return n / d - (n < 0 && n % d != 0); }
static int SnesHdCoordinate(int origin, int step, int row, int x,
                             unsigned sx, unsigned sy, unsigned scale) {
  int q = origin + x * step, whole = SnesHdFloorDiv(q, 256);
  /* Split the whole texel before multiplying the remainder by scale. All
   * register-derived intermediates fit32 bits, including64x and negative X. */
  return whole + SnesHdFloorDiv((q - whole * 256) * (int)scale +
      (int)sx * step + (int)sy * row, (int)(256 * scale));
}
static uint16_t SnesHdPixelColor(const SnesHdLine *j, unsigned x, uint16_t p) {
  unsigned layer = (p >> 8) & 15;
  return layer == 4 || layer == 6 ? j->object_color[x] : j->palette[p & 255];
}
static uint32_t SnesHdSample(const SnesHdFrame *f, unsigned scale,
                              unsigned px, unsigned py) {
  unsigned x = px / scale, y = py / scale;
  const SnesHdLine *j = &f->lines[y];
  if (scale == 1 || j->kind == kSnesHdNative || j->native_mask[x]) return j->native[x];
  int screen_x = j->screen_left + (int)x;
  if (screen_x < j->active_left || screen_x >= j->active_right) return 0;
  int tx = SnesHdCoordinate(j->origin_x, j->step_x, j->row_x, screen_x,
                            px % scale, py % scale, scale);
  int ty = SnesHdCoordinate(j->origin_y, j->step_y, j->row_y, screen_x,
                            px % scale, py % scale, scale);
  if (j->kind == kSnesHdAffine) {
    double fraction = (double)(py % scale) / scale;
    double position = screen_x + (double)(px % scale) / scale;
    double ox = j->affine[0] + j->affine[4] * fraction;
    double oy = j->affine[1] + j->affine[5] * fraction;
    double dx = j->affine[2] + j->affine[6] * fraction;
    double dy = j->affine[3] + j->affine[7] * fraction;
    tx = (int)floor((ox + position * dx) / 256);
    ty = (int)floor((oy + position * dy) / 256);
  }
  int tile = -1;
  if (j->tilemap) {
    const SnesHdTilemap *m = f->tilemaps[j->tilemap - 1];
    unsigned mx = ((uint32_t)tx + (uint32_t)j->map_offset_x) >> 3;
    unsigned my = ((uint32_t)ty + (uint32_t)j->map_offset_y) >> 3;
    tile = m->texels[(my & (m->height - 1)) * m->width + (mx & (m->width - 1))];
  }
  unsigned index = SnesMode7HdFetchInt(j->control, f->sources[j->source]->vram,
                                      tx, ty, tile);
  uint16_t bg = index ? (uint16_t)(0x5000 | index) : 0x0500;
  unsigned visible = j->visibility[x];
  uint16_t main = visible & 1 ? bg : 0x0500, sub = visible & 2 ? bg : 0x0500;
  if ((visible & 4) && j->object[x] > main) main = j->object[x];
  if ((visible & 8) && j->object[x] > sub) sub = j->object[x];
  bool inside = (visible & 16) != 0;
  bool clip = SnesHdWindowCondition(j->cgwsel >> 6, inside);
  unsigned layer = (main >> 8) & 15;
  bool math = !SnesHdWindowCondition((j->cgwsel >> 4) & 3, inside) &&
      (j->cgadsub & (1u << layer));
  unsigned color = SnesHdPixelColor(j, x, main), other = j->fixed_color;
  bool half = math && (j->cgadsub & 64);
  if (math && (j->cgwsel & 2)) {
    if (sub & 255) other = SnesHdPixelColor(j, x, sub); else half = false;
  }
  uint32_t rgb = 0;
  for (unsigned c = 0; c < 3; ++c) {
    int value = clip ? 0 : (color >> (5 * c)) & 31;
    if (math) {
      int second = (other >> (5 * c)) & 31;
      value += j->cgadsub & 128 ? -second : second;
      if (value < 0) value = 0;
      if (half) value /= 2;
      if (value > 31) value = 31;
    }
    rgb |= (uint32_t)(((value << 3) | (value >> 2)) * j->brightness / 15) << (16 - 8 * c);
  }
  uint32_t overlay = j->overlay[x], mixed = 0;
  unsigned opacity = overlay >> 24;
  if (opacity) {
    for (unsigned shift = 0; shift < 24; shift += 8) {
      unsigned channel = ((rgb >> shift) & 255) * (255 - opacity) +
          ((overlay >> shift) & 255) * 255;
      mixed |= ((channel + 127) / 255) << shift;
    }
    rgb = mixed;
  }
  return rgb;
}
bool SnesHdFrameRenderRegion(const SnesHdFrame *f, unsigned scale,
    unsigned x, unsigned y, unsigned w, unsigned h, uint32_t *pixels, size_t pitch) {
  if (!f || !f->sealed || f->failed || !scale || scale > kSnesHdMaxScale ||
      !pixels || !w || !h || x > f->width * scale || y > f->height * scale ||
      w > f->width * scale - x || h > f->height * scale - y ||
      pitch < (size_t)w * 4 || pitch > SIZE_MAX / h) return false;
  for (unsigned row = 0; row < h; ++row) {
    uint32_t *out = (uint32_t *)((uint8_t *)pixels + row * pitch);
    for (unsigned col = 0; col < w; ++col) out[col] = SnesHdSample(f, scale, x + col, y + row);
  }
  return true;
}

