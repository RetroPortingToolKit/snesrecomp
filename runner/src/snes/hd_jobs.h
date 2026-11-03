#ifndef SNESRECOMP_HD_JOBS_H
#define SNESRECOMP_HD_JOBS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { kSnesHdDefaultScale = 8, kSnesHdMaxWidth = 1024, kSnesHdMaxHeight = 512, kSnesHdMaxScale = 64,
       kSnesHdMaxTilemaps = 16, kSnesHdMapWidth = 1024, kSnesHdMapHeight = 512 };
enum { kSnesHdNative = 1, kSnesHdMode7 = 2, kSnesHdAffine = 3 };
typedef struct SnesHdFrame SnesHdFrame;
typedef struct SnesHdCapture SnesHdCapture;
typedef struct SnesHdSettings { bool enabled; unsigned scale; } SnesHdSettings;
static inline SnesHdSettings SnesHdDefaultSettings(void) {
  SnesHdSettings settings = {true, kSnesHdDefaultScale};
  return settings;
}
static inline bool SnesHdParseScale(const char *text, unsigned *scale) {
  if (!text || !scale || !*text) return false;
  unsigned value = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    value = value * 10 + (unsigned)(*text - '0');
    if (value > kSnesHdMaxScale) return false;
  }
  if (!value) return false;
  *scale = value; return true;
}
struct Ppu;
/* Lazy frame publisher for standard PPU hosts: no packet allocation on frames
 * without Mode7. Bound only to the live PPU; detached work never publishes. */
SnesHdCapture *SnesHdCaptureCreate(void);
bool SnesHdCaptureBind(SnesHdCapture *capture, struct Ppu *ppu, unsigned height);
void SnesHdCaptureReset(SnesHdCapture *capture);
void SnesHdCaptureDestroy(SnesHdCapture *capture);
const SnesHdFrame *SnesHdCaptureFrame(const SnesHdCapture *capture);
/* All arrays are native-sized. A published frame owns every source byte;
 * neither the GPU nor the portable consumer reads live console memory. */
typedef struct SnesHdLine {
  uint32_t native[kSnesHdMaxWidth];
  uint32_t overlay[kSnesHdMaxWidth]; /* premultiplied ARGB, explicit alpha */
  uint16_t object[kSnesHdMaxWidth], object_color[kSnesHdMaxWidth], palette[256];
  uint8_t visibility[kSnesHdMaxWidth], native_mask[kSnesHdMaxWidth];
  int32_t origin_x, origin_y, step_x, step_y, row_x, row_y; /* 1/256 texel */
  int16_t screen_left, active_left, active_right;
  uint16_t source, fixed_color;
  uint8_t kind, control, brightness, cgwsel, cgadsub;
  /* Optional prepared affine camera, in Q8 source coordinates. The first
   * four values are origin XY/step XY; the next four are their row deltas.
   * This preserves interpolated cameras without quantizing them to registers. */
  double affine[8];
  int32_t map_offset_x, map_offset_y;
  uint16_t tilemap; /* zero: hardware map; otherwise owned map index + 1 */
} SnesHdLine;
typedef struct SnesHdPicture {
  uint64_t hash;
  uint16_t vram[32768];
} SnesHdPicture;

typedef struct SnesHdTilemap {
  uint64_t hash;
  unsigned width, height; /* powers of two, in 8x8-character cells */
  uint8_t texels[kSnesHdMapWidth * kSnesHdMapHeight];
} SnesHdTilemap;
typedef struct SnesHdView {
  unsigned width, height, source_count;
  const SnesHdLine *lines;
  const SnesHdPicture *const *sources;
  size_t owned_bytes;
  unsigned tilemap_count;
  const SnesHdTilemap *const *tilemaps;
} SnesHdView;
SnesHdFrame *SnesHdFrameCreate(unsigned width, unsigned height);
void SnesHdFrameDestroy(SnesHdFrame *frame);
/* Prepared renderers publish the same job contract as the PPU capture.
 * Sources are copied/deduplicated once, then referenced by row jobs. */
int SnesHdFrameAddPicture(SnesHdFrame *frame, const uint16_t vram[32768]);
int SnesHdFrameAddTilemap(SnesHdFrame *frame, const uint8_t *tiles,
    unsigned width, unsigned height);
bool SnesHdFrameSetLine(SnesHdFrame *frame, unsigned y, const SnesHdLine *line);
/* Attach only a mutable builder; detach before sealing. Reset detaches it.
 * Once sealed, a frame is immutable until its owner destroys it. */
bool PpuSetHdFrameCapture(struct Ppu *ppu, SnesHdFrame *frame);
bool SnesHdFrameCaptureLine(SnesHdFrame *frame, unsigned destination_y,
    const struct Ppu *ppu, unsigned source_line, unsigned crop_left,
    bool promoted_overlays);
bool SnesHdFrameCopyLine(SnesHdFrame *frame, unsigned destination_y,
    const SnesHdFrame *source, unsigned source_y, unsigned crop_left);
bool SnesHdFrameCopyRows(SnesHdFrame *frame, unsigned destination_y,
    const SnesHdFrame *source);
/* Presentation foreground. Opaque black and transparent black differ in A.
 * Native pixels supplied by the host already include these overlays. */
bool SnesHdFrameSetOverlay(SnesHdFrame *frame, const uint32_t *argb, size_t pitch);
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
