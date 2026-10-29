#ifndef SNESRECOMP_RASTER_JOBS_H
#define SNESRECOMP_RASTER_JOBS_H

#include "ppu.h"
#include "raster_register_jobs.h"

/* Compatibility adapters for existing device callers. New capture consumers
 * can include raster_register_jobs.h and own only SnesRenderRegisters.
 * memcpy makes a real typed value; a Ppu is never aliased as a short object. */
static inline SnesRenderWindowPlan SnesRenderWindows(const Ppu *p,
    int left1, int right1, int left2, int right2) {
  SnesRenderRegisters registers = SnesRenderRegistersLoad(p);
  return SnesRenderRegistersWindows(&registers, left1, right1, left2, right2);
}
static inline bool SnesRenderMode1TileRow(const Ppu *p,
    const uint16_t vram[0x8000], unsigned layer, int x, int y,
    unsigned width, uint16_t *out) {
  if (!p) return false;
  SnesRenderRegisters registers = SnesRenderRegistersLoad(p);
  return SnesRenderRegistersMode1TileRow(&registers, vram, layer, x, y, width, out);
}

#endif
