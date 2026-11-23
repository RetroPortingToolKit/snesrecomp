#ifndef SNESRECOMP_RASTER_INSPECTION_H
#define SNESRECOMP_RASTER_INSPECTION_H

#include "../raster_snapshot.h"
#include "raster_image.h"

enum {
  kSnesInspectionLines = 224,
  /* The original retail X1 camouflage occupies 283 frames including its
   * subscreen transition. Leave room to inspect the surrounding picture. */
  kSnesInspectionFrames = 320,
  kSnesInspectionBytes = 96 * 1024 * 1024,
};

/* Retain completed fields and one staging frame. Each frame owns its immutable
 * snapshots, reusing equal images between adjacent lines. */
typedef struct SnesRasterInspectionFrame {
  /* The first immutable line already owns the four PPU memory arrays. */
  uint8_t origin[offsetof(Ppu, cgram)];
  SnesRasterSnapshotStore vram, palette, oam, high_oam;
  uint8_t registers[kSnesInspectionLines][PPU_SAVESTATE_REGS_SIZE];
  SnesRasterImageLine lines[kSnesInspectionLines];
  uint32_t number;
  unsigned count;
  bool valid;
} SnesRasterInspectionFrame;

typedef struct SnesRasterInspection {
  SnesRasterInspectionFrame *recording;
  unsigned head, count, capacity;
  size_t bytes, peak_bytes, byte_limit;
  SnesRasterImage drawing;
  SnesRasterInspectionFrame *history[];
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

static inline SnesRasterInspection *SnesRasterInspectionCreateBounded(
    unsigned capacity, size_t byte_limit) {
  if (!capacity || capacity > kSnesInspectionFrames) return NULL;
  const size_t bytes = sizeof(SnesRasterInspection) + capacity * sizeof(SnesRasterInspectionFrame *);
  if (bytes > byte_limit) return NULL;
  SnesRasterInspection *inspection = calloc(1, bytes);
  if (inspection) {
    inspection->capacity = capacity;
    inspection->byte_limit = byte_limit;
    inspection->bytes = inspection->peak_bytes = bytes;
  }
  return inspection;
}

static inline SnesRasterInspection *SnesRasterInspectionCreate(void) {
  return SnesRasterInspectionCreateBounded(kSnesInspectionFrames, kSnesInspectionBytes);
}

static inline size_t SnesRasterInspectionStoreBytes(const SnesRasterSnapshotStore *store) {
  return store->images ? store->limit * sizeof(void *) + store->allocated * store->image_bytes : 0;
}

static inline void SnesRasterInspectionFreeFrame(SnesRasterInspection *inspection,
    SnesRasterInspectionFrame *frame) {
  if (!frame) return;
  inspection->bytes -= sizeof(*frame) + SnesRasterInspectionStoreBytes(&frame->vram) +
      SnesRasterInspectionStoreBytes(&frame->palette) + SnesRasterInspectionStoreBytes(&frame->oam) +
      SnesRasterInspectionStoreBytes(&frame->high_oam);
  SnesRasterSnapshotDestroy(&frame->vram);
  SnesRasterSnapshotDestroy(&frame->palette);
  SnesRasterSnapshotDestroy(&frame->oam);
  SnesRasterSnapshotDestroy(&frame->high_oam);
  free(frame);
}

static inline SnesRasterInspectionFrame *SnesRasterInspectionTakeOldest(
    SnesRasterInspection *inspection) {
  SnesRasterInspectionFrame *frame = inspection->history[inspection->head];
  inspection->head = (inspection->head + 1) % inspection->capacity;
  --inspection->count;
  return frame;
}

static inline void SnesRasterInspectionDestroy(SnesRasterInspection *inspection) {
  if (!inspection) return;
  while (inspection->count)
    SnesRasterInspectionFreeFrame(inspection, SnesRasterInspectionTakeOldest(inspection));
  SnesRasterInspectionFreeFrame(inspection, inspection->recording);
  free(inspection);
}

static inline bool SnesRasterInspectionMakeRoom(SnesRasterInspection *inspection, size_t bytes) {
  while (bytes > inspection->byte_limit - inspection->bytes && inspection->count)
    SnesRasterInspectionFreeFrame(inspection, SnesRasterInspectionTakeOldest(inspection));
  return bytes <= inspection->byte_limit - inspection->bytes;
}

static inline void SnesRasterInspectionCharge(SnesRasterInspection *inspection, size_t bytes) {
  inspection->bytes += bytes;
  if (inspection->peak_bytes < inspection->bytes) inspection->peak_bytes = inspection->bytes;
}

static inline bool SnesRasterInspectionBeginStore(SnesRasterInspection *inspection,
    SnesRasterSnapshotStore *store, size_t image_bytes) {
  const size_t growth = store->images ? 0 : kSnesInspectionLines * sizeof(void *);
  if (!SnesRasterInspectionMakeRoom(inspection, growth) ||
      !SnesRasterSnapshotBegin(store, image_bytes, kSnesInspectionLines)) return false;
  SnesRasterInspectionCharge(inspection, growth);
  return true;
}

static inline const void *SnesRasterInspectionStore(SnesRasterInspection *inspection,
    SnesRasterSnapshotStore *store, const void *source) {
  unsigned allocated = store->allocated;
  size_t available = inspection->byte_limit - inspection->bytes;
  const void *image = SnesRasterSnapshotCaptureBounded(store, source, true, available);
  /* Unchanged lines and reusable blocks never evict older frames. */
  if (!image && store->count == allocated && store->count < store->limit &&
      store->image_bytes > available &&
      SnesRasterInspectionMakeRoom(inspection, store->image_bytes))
    image = SnesRasterSnapshotCapture(store, source, true);
  if (store->allocated > allocated) SnesRasterInspectionCharge(inspection, store->image_bytes);
  return image;
}

static inline bool SnesRasterInspectionCapture(SnesRasterInspection *inspection,
    const Ppu *ppu, uint32_t number, unsigned line) {
  if (!inspection || !ppu || line < 1 || line > kSnesInspectionLines) return false;
  SnesRasterInspectionFrame *frame = inspection->recording;
  if (!SnesRasterImageCanDefer(ppu) || (ppu->setini & 4)) {
    if (frame) {
      frame->count = 0;
      frame->valid = false;
    }
    return false;
  }
  if (line == 1) {
    if (!frame) {
      if (!SnesRasterInspectionMakeRoom(inspection, sizeof(*frame))) return false;
      frame = inspection->recording = calloc(1, sizeof(*frame));
      if (!frame) return false;
      SnesRasterInspectionCharge(inspection, sizeof(*frame));
    }
    frame->number = number;
    frame->count = 0;
    frame->valid = false;
    if (!SnesRasterInspectionBeginStore(inspection, &frame->vram, sizeof(ppu->vram)) ||
        !SnesRasterInspectionBeginStore(inspection, &frame->palette, sizeof(ppu->cgram)) ||
        !SnesRasterInspectionBeginStore(inspection, &frame->oam, sizeof(ppu->oam)) ||
        !SnesRasterInspectionBeginStore(inspection, &frame->high_oam, sizeof(ppu->highOam)))
      return false;
    memcpy(frame->origin, ppu, sizeof(frame->origin));
    uint8_t *no_output = NULL;
    memcpy(frame->origin + offsetof(Ppu, renderBuffer), &no_output, sizeof(no_output));
    frame->valid = true;
  }
  if (!frame || !frame->valid || frame->number != number || frame->count + 1 != line)
    return false;

  uint8_t *registers = frame->registers[line - 1];
  memcpy(registers, ppu, PPU_SAVESTATE_REGS_SIZE);
  SnesRasterImageLine captured = {
    .registers = registers,
    .high_oam = SnesRasterInspectionStore(inspection, &frame->high_oam, ppu->highOam),
    .palette = SnesRasterInspectionStore(inspection, &frame->palette, ppu->cgram),
    .oam = SnesRasterInspectionStore(inspection, &frame->oam, ppu->oam),
    .vram = SnesRasterInspectionStore(inspection, &frame->vram, ppu->vram),
  };
  if (!captured.high_oam || !captured.palette || !captured.oam || !captured.vram) {
    frame->valid = false;
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
  if (!frame || frame->count != kSnesInspectionLines ||
      (frame->number != number && frame->number != number + 1)) return;
  if (inspection->count) {
    const SnesRasterInspectionFrame *latest = inspection->history[
        (inspection->head + inspection->count - 1) % inspection->capacity];
    if (number == latest->number) {
      frame->count = 0;
      return;
    }
    if (number < latest->number)
      while (inspection->count)
        SnesRasterInspectionFreeFrame(inspection, SnesRasterInspectionTakeOldest(inspection));
  }
  inspection->recording = inspection->count == inspection->capacity ?
      SnesRasterInspectionTakeOldest(inspection) : NULL;
  if (inspection->recording) {
    inspection->recording->count = 0;
    inspection->recording->valid = false;
  }
  const unsigned slot = (inspection->head + inspection->count) % inspection->capacity;
  frame->number = number;
  inspection->history[slot] = frame;
  ++inspection->count;
}

static inline const SnesRasterInspectionFrame *SnesRasterInspectionFind(
    const SnesRasterInspection *inspection, uint32_t number) {
  if (inspection)
    for (unsigned i = 0; i < inspection->count; ++i) {
      const SnesRasterInspectionFrame *frame =
          inspection->history[(inspection->head + i) % inspection->capacity];
      if (frame->number == number) return frame;
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

static inline bool SnesRasterInspectionDraw(SnesRasterInspection *inspection,
    const SnesRasterInspectionFrame *frame,
    uint8_t layer_mask, uint32_t pixels[256 * kSnesInspectionLines]) {
  if (!inspection || !frame || frame->count != kSnesInspectionLines || !pixels) return false;
  SnesRasterImage *drawing = &inspection->drawing;
  const SnesRasterImageLine *first = &frame->lines[0];
  SnesRasterImageReset(drawing);
  memcpy(&drawing->origin, frame->origin, sizeof(frame->origin));
  memcpy(drawing->origin.cgram, first->palette, sizeof(drawing->origin.cgram));
  memcpy(drawing->origin.oam, first->oam, sizeof(drawing->origin.oam));
  memcpy(drawing->origin.highOam, first->high_oam, sizeof(drawing->origin.highOam));
  memcpy(drawing->origin.vram, first->vram, sizeof(drawing->origin.vram));
  drawing->valid = true;
  drawing->inspection = true;
  drawing->layer_mask = layer_mask;
  for (unsigned line = 1; line <= kSnesInspectionLines; ++line)
    if (!SnesRasterImageClaim(drawing, line)) return false;
  return SnesRasterImageMaterialize(drawing, pixels, SnesRasterInspectionReadLine, frame);
}

#endif
