#ifndef SNESRECOMP_RASTER_PLAN_H
#define SNESRECOMP_RASTER_PLAN_H

#include "raster_jobs.h"
#include "raster_register_plan.h"

/* Retain the established Ppu caller interface without requiring pure jobs
 * or new consumers to allocate the complete mutable device. */
static inline bool SnesRenderPlanCompose(SnesRenderComposePlan *out,
    const Ppu *p, const SnesRenderWindowPlan *windows, int first, unsigned width,
    const SnesRenderInterval *color_windows, unsigned color_count) {
  if (!p) return false;
  SnesRenderRegisters registers = SnesRenderRegistersLoad(p);
  return SnesRenderRegistersPlanCompose(out, &registers, windows, first, width,
      color_windows, color_count);
}

#endif
