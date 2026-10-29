#ifndef SNESRECOMP_RASTER_REGISTER_JOBS_H
#define SNESRECOMP_RASTER_REGISTER_JOBS_H

#include "ppu_registers.h"

/* Pure jobs over captured SNES data. No guest writes, asset/scene knowledge,
 * allocation, backend dependency or persistent simulation state. Consumers
 * own their caches; initialize a cache before sharing it with render workers.
 *
 * Mode1 pixels: priority[15:12], layer[11:8], CGRAM index[7:0]; zero is
 * transparent. Priorities follow the SNES Mode1 ordering, including BG3-high.
 * Output colors are 0x00RRGGBB, matching the software field contract.
 */
typedef struct SnesRenderOamBinding {
  uint64_t key, slots[2];
} SnesRenderOamBinding;
typedef struct SnesRenderOamIndex {
  SnesRenderOamBinding entries[256];
} SnesRenderOamIndex;
typedef struct SnesRenderColorCache {
  uint32_t rgb[16][32768];
  bool ready[16];
} SnesRenderColorCache;

/* Resolve all six window layers from two geometry tests per pixel. Geometry
 * is explicit: a title may supply wider effect bounds without putting scene
 * knowledge in this job. Table entries are masks of BG1..BG4, OBJ, color. */
typedef struct SnesRenderWindowPlan {
  int left[2], right[2];
  uint8_t inside[4];
} SnesRenderWindowPlan;
static inline SnesRenderWindowPlan SnesRenderRegistersWindows(const SnesRenderRegisters *p,
    int left1, int right1, int left2, int right2) {
  SnesRenderWindowPlan plan = {{left1, left2}, {right1, right2}, {0}};
  for (unsigned region = 0; region < 4; ++region)
    for (unsigned layer = 0; layer < 6; ++layer) {
      unsigned flags = (p->windowsel >> (layer * 4)) & 15;
      bool a = ((region & 1) != 0) != ((flags & 1) != 0);
      bool b = ((region & 2) != 0) != ((flags & 4) != 0);
      bool inside;
      if (!(flags & 2)) inside = (flags & 8) && b;
      else if (!(flags & 8)) inside = a;
      else switch ((p->wbgobjlog >> (layer * 2)) & 3) {
        case 0: inside = a || b; break;
        case 1: inside = a && b; break;
        case 2: inside = a != b; break;
        default: inside = a == b; break;
      }
      if (inside) plan.inside[region] |= (uint8_t)(1u << layer);
    }
  return plan;
}
static inline unsigned SnesRenderWindowMask(const SnesRenderWindowPlan *plan, int x) {
  unsigned a = x >= plan->left[0] && x <= plan->right[0];
  unsigned b = x >= plan->left[1] && x <= plan->right[1];
  return plan->inside[a | (b << 1)];
}

static inline uint64_t SnesRenderOamKey(int x, int y, uint16_t attr) {
  return ((uint64_t)attr << 17) | ((unsigned)y & 255u) << 9 | ((unsigned)x & 511u);
}
static inline unsigned SnesRenderOamBucket(const SnesRenderOamIndex *index, uint64_t key) {
  unsigned hash = (unsigned)(key ^ (key >> 16) ^ (key >> 32));
  unsigned bucket = (hash * 0x9e3779b1u) >> 24;
  while ((index->entries[bucket].slots[0] | index->entries[bucket].slots[1]) &&
      index->entries[bucket].key != key)
    bucket = (bucket + 1u) & 255u;
  return bucket;
}
/* Build over any contiguous subset of the 128 hardware slots. Duplicate
 * submissions retain every matching slot, including slots in both masks. */
static inline bool SnesRenderIndexOam(SnesRenderOamIndex *index,
    const uint16_t oam[256], const uint8_t high_oam[32], unsigned first, unsigned count) {
  if (!index || !oam || !high_oam || first > 128 || count > 128 - first) return false;
  memset(index, 0, sizeof(*index));
  for (unsigned slot = first; slot < first + count; ++slot) {
    unsigned pos = oam[slot * 2];
    unsigned hi = (high_oam[slot / 4] >> (slot % 4 * 2)) & 1;
    int x = (pos & 255) | (hi << 8);
    if (x >= 256) x -= 512;
    uint64_t key = SnesRenderOamKey(x, pos >> 8, oam[slot * 2 + 1]);
    SnesRenderOamBinding *entry = &index->entries[SnesRenderOamBucket(index, key)];
    entry->key = key;
    entry->slots[slot / 64] |= UINT64_C(1) << (slot % 64);
  }
  return true;
}
static inline const SnesRenderOamBinding *SnesRenderFindOam(const SnesRenderOamIndex *index,
    int x, int y, uint16_t attr) {
  if (!index || x < -256 || x >= 256) return NULL;
  const SnesRenderOamBinding *entry =
      &index->entries[SnesRenderOamBucket(index, SnesRenderOamKey(x, y, attr))];
  return (entry->slots[0] | entry->slots[1]) ? entry : NULL;
}

/* Tile decode only: the caller owns windows, mosaic, color math and scene
 * reconstruction. p supplies captured registers; vram is a separate immutable
 * snapshot. out must hold width pixels and must not overlap either input. */
static inline bool SnesRenderRegistersMode1TileRow(const SnesRenderRegisters *p, const uint16_t vram[0x8000],
    unsigned layer, int x, int y, unsigned width, uint16_t *out) {
  if (!p || !vram || !out || layer >= 3 || (p->bgmode & 7) != 1) return false;
  static const unsigned low[] = {8, 7, 1}, high[] = {12, 11, 3};
  unsigned bpp = layer == 2 ? 2 : 4;
  unsigned size = (p->bgmode & (16u << layer)) ? 16 : 8;
  unsigned py = ((unsigned)y + p->vScroll[layer]) & 1023;
  unsigned ty = py / size, sc = p->bgXsc[layer];
  unsigned row_map = (sc & 0xfc) * 256 + (ty & 31) * 32;
  if ((sc & 2) && (ty & 32)) row_map += (sc & 1) ? 2048 : 1024;
  unsigned base = ((p->bgTileAdr >> (layer * 4) & 15u) << 12);
  for (unsigned dest = 0; dest < width;) {
    unsigned px = ((unsigned)x + dest + p->hScroll[layer]) & 1023, tx = px / size;
    unsigned map = row_map + (tx & 31);
    if ((sc & 1) && (tx & 32)) map += 1024;
    unsigned tile = vram[map & 0x7fff];
    unsigned cx = px & (size - 1), cy = py & (size - 1);
    unsigned count = 8 - (px & 7);
    if (count > width - dest) count = width - dest;
    if (tile & 0x4000) cx = size - 1 - cx;
    if (tile & 0x8000) cy = size - 1 - cy;
    unsigned number = ((tile & 1023) + cx / 8 + cy / 8 * 16) & 1023;
    unsigned address = (base + number * bpp * 4 + (cy & 7)) & 0x7fff;
    unsigned bits01 = vram[address];
    unsigned bits23 = bpp == 4 ? vram[(address + 8) & 0x7fff] : 0;
    unsigned priority = tile & 0x2000 ?
        (layer == 2 && (p->bgmode & 8) ? 15 : high[layer]) : low[layer];
    unsigned binding = (priority << 12) | (layer << 8) | (((tile >> 10) & 7) << bpp);
    for (unsigned i = 0; i < count; ++i) {
      unsigned column = tile & 0x4000 ? cx - i : cx + i;
      unsigned shift = 7 - (column & 7);
      unsigned a = bits01 >> shift, b = bits23 >> shift;
      unsigned pixel = (a & 1) | ((a >> 7) & 2) | ((b & 1) << 2) | ((b >> 5) & 8);
      out[dest + i] = pixel ? (uint16_t)(binding | pixel) : 0;
    }
    dest += count;
  }
  return true;
}

/* cache must be initially zeroed. Returned tables remain valid for its life;
 * different renderer contexts can own independent caches without data races. */
static inline const uint32_t *SnesRenderBrightness(SnesRenderColorCache *cache, unsigned level) {
  if (!cache || level >= 16) return NULL;
  if (!cache->ready[level]) {
    for (unsigned color = 0; color < 32768; ++color) {
      uint32_t pixel = 0;
      for (unsigned component = 0; component < 3; ++component) {
        unsigned c = (color >> (component * 5)) & 31;
        pixel |= (((c << 3) | (c >> 2)) * level / 15) << (16 - component * 8);
      }
      cache->rgb[level][color] = pixel;
    }
    cache->ready[level] = true;
  }
  return cache->rgb[level];
}

#endif

