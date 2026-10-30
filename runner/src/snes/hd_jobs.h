#ifndef SNESRECOMP_HD_JOBS_H
#define SNESRECOMP_HD_JOBS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { kSnesHdMaxWidth = 448, kSnesHdMaxHeight = 512, kSnesHdMaxScale = 64 };
typedef struct SnesHdFrame SnesHdFrame;
struct Ppu;
/* All arrays are native-sized. A published frame owns every source byte;
 * neither the GPU nor the portable consumer reads live console memory. */
typedef struct SnesHdLine {
  uint32_t native[kSnesHdMaxWidth];
  uint16_t object[kSnesHdMaxWidth], object_color[kSnesHdMaxWidth], palette[256];
  uint8_t visibility[kSnesHdMaxWidth], native_mask[kSnesHdMaxWidth];
  int32_t origin_x, origin_y, step_x, step_y, row_x, row_y; /* 1/256 texel */
  int16_t screen_left, active_left, active_right;
  uint16_t source, fixed_color;
  uint8_t kind, control, brightness, cgwsel, cgadsub;
} SnesHdLine;
typedef struct SnesHdPicture {
  uint64_t hash;
  uint16_t vram[32768];
} SnesHdPicture;
typedef struct SnesHdView {
  unsigned width, height, source_count;
  const SnesHdLine *lines;
  const SnesHdPicture *const *sources;
  size_t owned_bytes;
} SnesHdView;
SnesHdFrame *SnesHdFrameCreate(unsigned width, unsigned height);
void SnesHdFrameDestroy(SnesHdFrame *frame);
/* Attach only a mutable builder; detach before sealing. Reset detaches it.
 * Once sealed, a frame is immutable until its owner destroys it. */
bool PpuSetHdFrameCapture(struct Ppu *ppu, SnesHdFrame *frame);
bool SnesHdFrameCaptureLine(SnesHdFrame *frame, unsigned destination_y,
    const struct Ppu *ppu, unsigned source_line, unsigned crop_left,
    bool promoted_overlays);
bool SnesHdFrameCopyLine(SnesHdFrame *frame, unsigned destination_y,
    const SnesHdFrame *source, unsigned source_y, unsigned crop_left);
/* Hosts may rehost pixel-art HUDs after capturing the world. Coverage is
 * explicit opacity, never inferred from RGB (opaque black remains opaque). */
bool SnesHdFrameSetNative(SnesHdFrame *frame, const uint32_t *pixels,
    size_t pitch, const uint8_t *coverage, size_t coverage_pitch);
bool SnesHdFrameSeal(SnesHdFrame *frame);
bool SnesHdFrameView(const SnesHdFrame *frame, SnesHdView *view);
/* Portable bounded ROI consumer; no scale-sized allocation. Scale1 always
 * copies the exact native image. High-scale readbacks may be tiled by caller. */
bool SnesHdFrameRenderRegion(const SnesHdFrame *frame, unsigned scale,
    unsigned x, unsigned y, unsigned width, unsigned height,
    uint32_t *pixels, size_t pitch);
#ifdef __cplusplus
}
#endif
#endif
