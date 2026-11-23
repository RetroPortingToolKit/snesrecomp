#ifndef SNESRECOMP_RASTER_INSPECTION_H
#define SNESRECOMP_RASTER_INSPECTION_H

#include "../raster_snapshot.h"
#include "raster_image.h"

enum { kSnesInspectionLines = 224, kSnesInspectionFrames = 2 };

/* Keep the last two completed hardware fields plus one being recorded.
 * Reuse equal memory at adjacent lines. Even a different full VRAM image
 * on each line is bounded by the number of lines and these three stores. */
typedef struct SnesRasterInspectionFrame {
  SnesRasterImage image;
  SnesRasterSnapshotStore vram, palette, oam, high_oam;
  uint8_t registers[kSnesInspectionLines][PPU_SAVESTATE_REGS_SIZE];
  SnesRasterImageLine lines[kSnesInspectionLines];
  uint32_t number;
  unsigned count;
} SnesRasterInspectionFrame;

typedef struct SnesRasterInspection {
  SnesRasterInspectionFrame frames[kSnesInspectionFrames + 1];
  SnesRasterInspectionFrame *recording;
  SnesRasterInspectionFrame *history[kSnesInspectionFrames];
  unsigned next;
} SnesRasterInspection;

typedef struct SnesRasterInspectionObject {
  int x;
  unsigned y, tile, priority, size_select, obj_selection, palette_base;
  bool flip_x, flip_y;
  uint16_t palette[16];
} SnesRasterInspectionObject;

typedef struct SnesRasterInspectionView {
  SnesRenderRegisters registers;
  uint16_t palette[256];
  SnesRasterInspectionObject sprite;
} SnesRasterInspectionView;

static inline SnesRasterInspection *SnesRasterInspectionCreate(void) {
  SnesRasterInspection *inspection = calloc(1, sizeof(*inspection));
  if (inspection) {
    inspection->recording = &inspection->frames[0];
    for (unsigned i = 0; i < kSnesInspectionFrames; ++i)
      inspection->history[i] = &inspection->frames[i + 1];
  }
  return inspection;
}

static inline void SnesRasterInspectionDestroy(SnesRasterInspection *inspection) {
  if (!inspection) return;
  for (unsigned i = 0; i < kSnesInspectionFrames + 1; ++i) {
    SnesRasterInspectionFrame *frame = &inspection->frames[i];
    SnesRasterSnapshotDestroy(&frame->vram);
    SnesRasterSnapshotDestroy(&frame->palette);
    SnesRasterSnapshotDestroy(&frame->oam);
    SnesRasterSnapshotDestroy(&frame->high_oam);
  }
  free(inspection);
}

static inline bool SnesRasterInspectionCapture(SnesRasterInspection *inspection,
    const Ppu *ppu, uint32_t number, unsigned line) {
  if (!inspection || !ppu || line < 1 || line > kSnesInspectionLines) return false;
  SnesRasterInspectionFrame *frame = inspection->recording;
  if (!SnesRasterImageCanDefer(ppu) || (ppu->setini & 4)) {
    frame->count = 0;
    SnesRasterImageReset(&frame->image);
    return false;
  }
  if (line == 1) {
    frame->number = number;
    frame->count = 0;
    SnesRasterImageReset(&frame->image);
    if (!SnesRasterSnapshotBegin(&frame->vram, sizeof(ppu->vram), kSnesInspectionLines) ||
        !SnesRasterSnapshotBegin(&frame->palette, sizeof(ppu->cgram), kSnesInspectionLines) ||
        !SnesRasterSnapshotBegin(&frame->oam, sizeof(ppu->oam), kSnesInspectionLines) ||
        !SnesRasterSnapshotBegin(&frame->high_oam, sizeof(ppu->highOam), kSnesInspectionLines))
      return false;
    SnesRasterImageBegin(&frame->image, ppu);
    frame->image.inspection = true;
    frame->image.origin.renderBuffer = NULL;
  }
  if (!frame->image.valid || frame->number != number || frame->count + 1 != line)
    return false;

  uint8_t *registers = frame->registers[line - 1];
  memcpy(registers, ppu, PPU_SAVESTATE_REGS_SIZE);
  SnesRasterImageLine captured = {
    .registers = registers,
    .high_oam = SnesRasterSnapshotCapture(&frame->high_oam, ppu->highOam, true),
    .palette = SnesRasterSnapshotCapture(&frame->palette, ppu->cgram, true),
    .oam = SnesRasterSnapshotCapture(&frame->oam, ppu->oam, true),
    .vram = SnesRasterSnapshotCapture(&frame->vram, ppu->vram, true),
  };
  if (!captured.high_oam || !captured.palette || !captured.oam || !captured.vram) {
    SnesRasterImageReset(&frame->image);
    return false;
  }
  frame->lines[frame->count++] = captured;
  return frame->count == kSnesInspectionLines;
}

/* The host publishes the presented picture. Frame-model hosts advance
 * their simulation index before the raster pass, while the cycle model
 * draws inside the simulation. Both use the completed index for display. */
static inline void SnesRasterInspectionPublish(SnesRasterInspection *inspection, uint32_t number) {
  if (!inspection) return;
  SnesRasterInspectionFrame *frame = inspection->recording;
  if (frame->count != kSnesInspectionLines ||
      (frame->number != number && frame->number != number + 1)) return;
  const SnesRasterInspectionFrame *latest = inspection->history[
      (inspection->next + kSnesInspectionFrames - 1) % kSnesInspectionFrames];
  if (latest->count == kSnesInspectionLines) {
    if (number == latest->number) {
      frame->count = 0;
      return;
    }
    if (number < latest->number) {
      for (unsigned i = 0; i < kSnesInspectionFrames; ++i)
        inspection->history[i]->count = 0;
      inspection->next = 0;
    }
  }
  unsigned slot = inspection->next;
  frame->number = number;
  inspection->recording = inspection->history[slot];
  inspection->recording->count = 0;
  inspection->history[slot] = frame;
  inspection->next = (slot + 1) % kSnesInspectionFrames;
}

static inline SnesRasterInspectionFrame *SnesRasterInspectionFind(
    const SnesRasterInspection *inspection, uint32_t number) {
  if (inspection)
    for (unsigned i = 0; i < kSnesInspectionFrames; ++i) {
      SnesRasterInspectionFrame *frame = inspection->history[i];
      if (frame->count == kSnesInspectionLines && frame->number == number) return frame;
    }
  return NULL;
}

static inline bool SnesRasterInspectionReadLine(const void *context, unsigned y,
    SnesRasterImageLine *result) {
  const SnesRasterInspectionFrame *frame = context;
  if (y >= frame->count) return false;
  *result = frame->lines[y];
  return true;
}

static inline bool SnesRasterInspectionReadObject(const SnesRasterInspectionFrame *frame,
    unsigned y, unsigned slot, SnesRasterInspectionObject *result) {
  SnesRasterImageLine line;
  if (!frame || slot >= 128 || !SnesRasterInspectionReadLine(frame, y, &line)) return false;
  SnesRenderRegisters registers;
  memcpy(&registers, line.registers, sizeof(registers));
  uint16_t pos = line.oam[slot * 2], attributes = line.oam[slot * 2 + 1];
  unsigned high = (line.high_oam[slot / 4] >> ((slot % 4) * 2)) & 3;
  result->x = (int)(pos & 255) - ((high & 1) ? 256 : 0);
  result->y = pos >> 8;
  result->tile = attributes & 0x1ff;
  result->priority = (attributes >> 12) & 3;
  result->size_select = high >> 1;
  result->obj_selection = registers.obsel;
  result->flip_x = (attributes & 0x4000) != 0;
  result->flip_y = (attributes & 0x8000) != 0;
  result->palette_base = PpuObjPaletteBase(attributes);
  memcpy(result->palette, line.palette + result->palette_base, sizeof(result->palette));
  return true;
}

static inline bool SnesRasterInspectionDraw(SnesRasterInspectionFrame *frame,
    uint8_t layer_mask, uint32_t pixels[256 * kSnesInspectionLines]) {
  if (!frame || frame->count != kSnesInspectionLines || !pixels) return false;
  frame->image.layer_mask = layer_mask;
  for (unsigned line = 1; line <= kSnesInspectionLines; ++line)
    if (!SnesRasterImageClaim(&frame->image, line)) return false;
  return SnesRasterImageMaterialize(&frame->image, pixels, SnesRasterInspectionReadLine, frame);
}

#endif
