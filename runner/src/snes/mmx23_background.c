/* X2/X3 share the retail two-layer map format. Native view uses VRAM except
 * for X1's hidden door-face policy; self-validated gutters use WRAM/ROM. */
#include "mmx23_background.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cart.h"
#include "ppu.h"
#include "snes.h"
#include "ws_shadow.h"
#include "mmx_door.h"
static Snes *s_snes;
static Ppu *s_ppu;
static uint8_t *s_ram;
/* A frame-local memo avoids decoding the same metatile on all eight raster
 * lines. Its size is independent of world height; collisions re-resolve. */
static uint32_t s_tile_keys[4096];
static uint16_t s_tile_values[4096];
static uint32_t s_door_keys[4096];
static uint8_t s_door_values[4096];
typedef struct MmxBgStream {
  uint16_t layoutBase;  /* screen-id layout window, bank $7E        */
  uint16_t screenDefs;  /* 16x16 block-id maps, 512B/screen, bank $7E */
  uint16_t mtPtrAddr;   /* WRAM address of 24-bit metatile table ptr */
  uint16_t worldXAddr;  /* stream-struct world anchor (16-bit)       */
  uint16_t worldYAddr;
  uint8_t bgsc;         /* expected BGnSC value: map base + 64x32    */
  uint16_t mapBaseWord; /* VRAM word base, for native self-validation */
} MmxBgStream;

static const MmxBgStream kBgStreams[2] = {
    {0xE800, 0x2800, 0x09C5, 0x1E5D, 0x1E60, 0x51, 0x5000}, /* BG1 */
    {0xEC00, 0xA600, 0x09C8, 0x1E9D, 0x1EA0, 0x59, 0x5800}, /* BG2 */
};

static uint16_t wram16(uint32_t addr) {
  return (uint16_t)(s_ram[addr] | (s_ram[addr + 1] << 8));
}

/* Exact tilemap entry for world pixel (px, py), via the game's own walk. */
static uint16_t world_tile(const MmxBgStream *s, uint32_t px,
                                 uint32_t py) {
  uint32_t li = (((py >> 8) & 0x1F) << 5) | ((px >> 8) & 0x1F);
  uint8_t sid = s_ram[s->layoutBase + li];
  uint16_t ba = (uint16_t)(s->screenDefs + sid * 512u +
                           ((py >> 4) & 0xF) * 0x20u + ((px >> 4) & 0xF) * 2u);
  uint16_t block = (uint16_t)(s_ram[ba] | (s_ram[(uint16_t)(ba + 1)] << 8));
  uint16_t mt_addr = wram16(s->mtPtrAddr);
  uint8_t mt_bank = s_ram[s->mtPtrAddr + 2];
  /* 16-bit wrap with a fixed bank replicates the guest's LDA [$1C] walk
   * ($00:B4EE), whose pointer arithmetic never carries into the bank. */
  uint16_t a = (uint16_t)(mt_addr + block * 8u + (((py >> 3) & 1u) << 2) +
                          (((px >> 3) & 1u) << 1));
  return (uint16_t)(cart_read(s_snes->cart, mt_bank, a) |
                    (cart_read(s_snes->cart, mt_bank, (uint16_t)(a + 1))
                     << 8));
}

/* VRAM word address the 64x32 map stores world pixel (px, py) at. */
static uint16_t vram_word(const MmxBgStream *s, uint32_t px,
                                uint32_t py) {
  uint32_t tx = px >> 3, ty = py >> 3;
  return (uint16_t)(s->mapBaseWord + ((tx >> 5) & 1u) * 0x400u +
                    (ty & 0x1F) * 0x20u + (tx & 0x1F));
}

/* The resolver must reproduce the live native view to earn the gutters. */
static bool stream_valid(const MmxBgStream *s, int32_t wx, int32_t wy) {
  if (wx < 0 || wy < 0)
    return false;
  /* Boot/menu frames can already have the gameplay BG mode/map bases while
   * the stream-source cluster is still zero. Reject before cart_read so an
   * uninitialized pointer cannot create a false off-rails diagnostic. */
  if (wram16(s->mtPtrAddr) < 0x8000)
    return false;
  int miss = 0, modal = 0;
  uint16_t modal_tile = 0;
  uint16_t got_v[12];
  for (int i = 0; i < 12; i++) {
    uint32_t px = (uint32_t)wx + 10u + (uint32_t)i * 20u;        /* 10..230 */
    uint32_t py = (uint32_t)wy + 12u + (uint32_t)(i % 6) * 36u;  /* 12..192 */
    uint16_t want = world_tile(s, px, py);
    uint16_t got = s_ppu->vram[vram_word(s, px, py) & 0x7FFF];
    got_v[i] = got;
    if (want != got)
      miss++;
  }
  /* One mismatch tolerated: a single in-flight column upload must not
   * flicker the margins off for a frame. Two or more = not our scene. */
  if (miss > 1)
    return false;
  /* Diversity: a near-uniform native sample proves nothing — some scenes
   * repurpose BG2 as a mostly-transparent OBJECT layer (X-Hunter tower
   * lifts live in the BG2 map and move by scroll). There the provider's
   * "level data" is all blank, matches trivially, and would erase the
   * object in the gutters (owner-reported half-culled platforms). Require
   * real structure in the view; otherwise authentic map wrap serves the
   * object correctly. */
  for (int i = 0; i < 12; i++) {
    int same = 0;
    for (int j = 0; j < 12; j++)
      same += (got_v[j] == got_v[i]);
    if (same > modal) {
      modal = same;
      modal_tile = got_v[i];
    }
  }
  if (modal <= 9)
    return true;

  /*
   * A sparse level screen can legitimately make the quick sample almost
   * uniform. Slot 0's vertical fall is mostly empty until the terrain scrolls
   * into view, so the old diversity gate disabled exact gutters and exposed
   * wrapped stale rows at the widescreen edges.
   *
   * Preserve the object-layer safeguard by looking for positive evidence:
   * scan the native view for six source tiles that differ from the modal
   * entry and require each one to reproduce VRAM. A repurposed object layer
   * whose level source is blank finds no proof; one whose live objects
   * disagree with that source fails the comparisons.
   */
  int proof = 0, proof_miss = 0;
  for (uint32_t y = 4; y < 224; y += 8) {
    for (uint32_t x = 4; x < 256; x += 8) {
      uint32_t px = (uint32_t)wx + x;
      uint32_t py = (uint32_t)wy + y;
      uint16_t want = world_tile(s, px, py);
      if (want == modal_tile)
        continue;
      uint16_t got =
          s_ppu->vram[vram_word(s, px, py) & 0x7FFF];
      if (want == got) {
        if (++proof >= 6)
          return true;
      } else if (++proof_miss > 1) {
        return false;
      }
    }
  }
  return false;
}


static uint16_t source_tile(const MmxBgStream *stream, uint32_t tx, uint32_t ty) {
  uint32_t key = tx | (ty << 10) | ((stream == &kBgStreams[1]) << 20);
  unsigned cache = (key * 2654435761u) >> 20;
  if (s_tile_keys[cache] == key + 1) {
    return s_tile_values[cache];
  }
  uint32_t x = tx * 8, y = ty * 8;
  uint16_t entry = world_tile(stream, x, y);
  s_tile_keys[cache] = key + 1;
  s_tile_values[cache] = entry;
  return entry;
}

static bool door_body(int x, int y) {
  if (x < 0 || x >= 8192 || y < 0 || y >= 8192) return false;
  unsigned tx = (unsigned)x / 16, ty = (unsigned)y / 16;
  uint32_t key = tx | (ty << 9);
  unsigned cache = (key * 2654435761u) >> 20;
  if (s_door_keys[cache] == key + 1) return s_door_values[cache];
  bool found = false;
  for (int row = 0; row < 3 && !found; ++row) {
    int top = (int)ty - row;
    if (top < 0 || top + 2 >= 512) continue;
    uint16_t words[3][4];
    for (int r = 0; r < 3; ++r)
      for (int q = 0; q < 4; ++q)
        words[r][q] = source_tile(&kBgStreams[0], tx * 2 + (q & 1),
                                  (top + r) * 2 + (q >> 1));
    found = MmxDoorBody(words);
  }
  s_door_keys[cache] = key + 1;
  s_door_values[cache] = found;
  return found;
}

static WsShadowSource world_provider(void *context, uint32_t tx, uint32_t ty,
                                     uint16_t *entry) {
  const MmxBgStream *stream = context;
  if (tx >= 1024 || ty >= 1024) return kWsShadowMiss;
  int x = (int)tx * 8, y = (int)ty * 8;
  if (stream == &kBgStreams[0] && door_body(x, y)) {
    bool left = door_body(x - 16, y), right = door_body(x + 16, y);
    if (left || right) {
      int boundary = (x & ~15) + (right ? 16 : 0);
      bool view_left = wram16(stream->worldXAddr) + 128 < boundary;
      /* Retain the face looking into the current room. The outward-facing
       * face samples the wall beyond it, exactly as in X1's renderer. */
      if (view_left && left) tx += 2;
      else if (!view_left && right) tx -= 2;
      else { *entry = source_tile(stream, tx, ty); return kWsShadowExact; }
      if (tx >= 1024) return kWsShadowMiss;
      *entry = source_tile(stream, tx, ty);
      return kWsShadowOverride;
    }
  }
  *entry = source_tile(stream, tx, ty);
  return kWsShadowExact;
}

void Mmx23ConfigureWsBgMargins(Snes *snes, Ppu *ppu, uint8_t *ram, bool enabled) {
  if (!ppu) return;
  static int on = -1, debug = -1;
  if (on < 0) {
    const char *e=getenv("SNESRECOMP_WS_BG_MARGINS"); on=!e || e[0]!='0';
    e=getenv("SNESRECOMP_WS_BG_MARGINS_DEBUG"); debug=e && e[0]!='0';
  }
  s_snes=snes; s_ppu=ppu; s_ram=ram;
  bool scene = on && enabled && snes && ppu && ram &&
      (ppu->bgmode & 0x37)==1 && ppu->bgXsc[0]==0x51 && ppu->bgXsc[1]==0x59;
  bool any=false;
  memset(s_tile_keys,0,sizeof(s_tile_keys));
  memset(s_door_keys,0,sizeof(s_door_keys));
  if (scene) for (unsigned layer=0; layer<2; ++layer) {
    const MmxBgStream *stream=&kBgStreams[layer];
    int32_t wx=wram16(stream->worldXAddr), wy=wram16(stream->worldYAddr);
    int32_t dx=(ppu->hScroll[layer]-wx)&1023, dy=(ppu->vScroll[layer]-wy)&1023;
    if (dx>=512) dx-=1024;
    if (dy>=512) dy-=1024;
    wx+=dx; wy+=dy;
    if (!stream_valid(stream,wx,wy)) continue;
    WsShadowSetWorld(layer,(uint32_t)wx,(uint32_t)wy);
    WsShadowSetWorldProvider(layer,world_provider,(void *)stream,layer==0);
    WsShadowSetBlankTile(layer,-1);
    WsShadowSetRespectGameWrites(layer,60);
    any=true;
  }
  static bool was_active;
  if (!any && was_active) WsShadowReset();
  was_active=any;
  WsShadowFrame(ppu);
  if (debug) {
    static unsigned calls;
    if (!(calls++%120)) fprintf(stderr,"[mmx23_bg] scene=%u layers=%u/%u\n",
        scene,WsShadowLayerActive(0),WsShadowLayerActive(1));
  }
}
