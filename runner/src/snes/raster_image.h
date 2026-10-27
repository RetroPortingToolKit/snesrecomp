#ifndef SNESRECOMP_RASTER_IMAGE_H
#define SNESRECOMP_RASTER_IMAGE_H

#include "ppu.h"
#include <string.h>

/* A compositor may own the final pixels while the live PPU still executes
 * ppu_runLineState for every claimed line. Keep an independent stock-image
 * readback without drawing the same picture on the hot path. The caller owns
 * immutable line data, this job and the output. No guest pointers or state
 * from this host-only job belong in execution snapshots or netplay digests.
 * Begin only for a fresh captured frame; retained/runahead frames keep theirs.
 */
typedef struct SnesRasterImageLine {
  const uint8_t *registers, *high_oam;
  const uint16_t *palette, *oam, *vram;
} SnesRasterImageLine;
typedef bool SnesRasterImageReadLine(const void *context, unsigned y,
    SnesRasterImageLine *line);
typedef struct SnesRasterImage {
  Ppu origin, work;
  bool valid, missing[224];
  unsigned count;
} SnesRasterImage;

static inline void SnesRasterImageReset(SnesRasterImage *image) {
  image->valid = false;
  image->count = 0;
  memset(image->missing, 0, sizeof(image->missing));
}
static inline void SnesRasterImageBegin(SnesRasterImage *image, const Ppu *p) {
  SnesRasterImageReset(image);
  if (p) { image->origin = *p; image->valid = true; }
}
/* Extra drawing callbacks/surfaces require their own capture contract. */
static inline bool SnesRasterImageCanDefer(const Ppu *p) {
  if (!p || p->extraLeftCur || p->extraRightCur || p->extraBottomCur ||
      p->renderVram || p->extraObjectCount || p->mode7Hd.pixels ||
      p->widescreenLineEnhancer || (p->setini & 3)) return false;
  for (unsigned i = 0; i < kPpuOverlaySource_Count; ++i)
    if (p->overlayRenderBuffer[i]) return false;
  return true;
}
static inline bool SnesRasterImageClaim(SnesRasterImage *image, unsigned line) {
  if (!image->valid || line < 1 || line > 224) return false;
  if (!image->missing[line - 1]) { image->missing[line - 1] = true; ++image->count; }
  return true;
}
/* Materialization is idempotent. Visit every line for private sprite/mosaic
 * state, but draw only omitted rows. Existing stock rows remain untouched. */
static inline bool SnesRasterImageMaterialize(SnesRasterImage *image,
    uint32_t stock[256 * 224], SnesRasterImageReadLine *read, const void *context) {
  bool missing = false;
  for (unsigned y = 0; y < 224; ++y) missing |= image->missing[y];
  if (!missing) return true;
  if (!image->valid || !stock || !read) return false;
  /* Validate the entire capture before changing any output. */
  for (unsigned y = 0; y < 224; ++y) {
    SnesRasterImageLine line;
    if (!read(context, y, &line) || !line.registers || !line.palette ||
        !line.oam || !line.high_oam || !line.vram) return false;
  }
  Ppu *p = &image->work;
  *p = image->origin;
  p->renderBuffer = (uint8_t *)stock;
  p->renderPitch = 256 * 4;
  p->renderVram = NULL;
  p->extraObjects = NULL; p->extraObjectCount = 0;
  p->mode7Hd.pixels = NULL;
  p->widescreenLineEnhancer = NULL; p->widescreenLineEnhancerContext = NULL;
  memset(p->overlayRenderBuffer, 0, sizeof(p->overlayRenderBuffer));
  ppu_drawCapturedLine(p, 0, false);
  for (unsigned y = 0; y < 224; ++y) {
    SnesRasterImageLine line;
    if (!read(context, y, &line)) return false;
    memcpy(p, line.registers, PPU_SAVESTATE_REGS_SIZE);
    memcpy(p->cgram, line.palette, sizeof(p->cgram));
    memcpy(p->oam, line.oam, sizeof(p->oam));
    memcpy(p->highOam, line.high_oam, sizeof(p->highOam));
    memcpy(p->vram, line.vram, sizeof(p->vram));
    ppu_drawCapturedLine(p, (int)y + 1, image->missing[y]);
  }
  memset(image->missing, 0, sizeof(image->missing));
  return true;
}
#endif
