#ifndef SNESRECOMP_HD_GPU_H
#define SNESRECOMP_HD_GPU_H
#include "snes/hd_jobs.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SnesHdGpu SnesHdGpu;
typedef struct SnesHdGpuStats {
  uint64_t frames, uploaded_bytes, upload_gpu_ns, draw_gpu_ns;
  unsigned requested_width, requested_height, output_width, output_height;
  size_t source_gpu_bytes, target_bytes;
  bool gpu_timestamps;
  uint64_t timed_frame; /* frame associated with the most recent ready timestamps */
} SnesHdGpuStats;
/* Requires a current desktop OpenGL3.3 context. Missing GPU support returns
 * NULL; all platforms retain the portable frame/ROI consumer. */
SnesHdGpu *SnesHdGpuCreate(void);
/* Interpolated double-precision camera rows need GL4; register jobs use3.3. */
bool SnesHdGpuSupportsAffine(const SnesHdGpu *gpu);
void SnesHdGpuDestroy(SnesHdGpu *gpu);
void SnesHdGpuReset(SnesHdGpu *gpu);
/* No full-resolution staging image. Allocation belongs to the host; this
 * helper chooses bounded dimensions before the host creates its target. */
bool SnesHdGpuAdmit(SnesHdGpu *gpu, unsigned native_width,
    unsigned native_height, unsigned scale, size_t target_budget,
    unsigned *width, unsigned *height);
/* Draw into the caller's current framebuffer. OpenGL state is restored;
 * logical image row0 occupies texture row0. Context ownership stays local.
 * A window-sized target evaluates exactly the visible texel centers of the
 * requested raster, equivalent to a full raster followed by nearest sampling.
 * Filters/presets that inspect neighboring texels should use the full target. */
bool SnesHdGpuDraw(SnesHdGpu *gpu, const SnesHdFrame *frame,
    unsigned scale, unsigned output_width, unsigned output_height);
/* Nonblocking timestamp collection in normal use. wait=true is for bounded
 * benchmark/QA only, never the interactive presentation loop. */
bool SnesHdGpuReadStats(SnesHdGpu *gpu, bool wait, SnesHdGpuStats *stats);
#ifdef __cplusplus
}
#endif
#endif
