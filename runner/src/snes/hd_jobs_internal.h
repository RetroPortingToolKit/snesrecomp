#ifndef SNESRECOMP_HD_JOBS_INTERNAL_H
#define SNESRECOMP_HD_JOBS_INTERNAL_H
#include "hd_jobs.h"
struct SnesHdFrame {
  unsigned width, height, source_count;
  bool sealed, failed;
  SnesHdLine *lines;
  SnesHdPicture *sources[kSnesHdMaxHeight];
  unsigned tilemap_count;
  SnesHdTilemap *tilemaps[kSnesHdMaxTilemaps];
  const uint16_t *last_vram;
  uint32_t last_generation;
  unsigned last_source;
};
#endif
