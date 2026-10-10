#ifndef SNESRECOMP_SNAPSHOT_IDENTITY_H
#define SNESRECOMP_SNAPSHOT_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/dsp1.h"

/* Keep the existing header/payload for every non-DSP-1 title. DSP-1 states
 * use distinct magic values so both disk and memory loaders reject legacy
 * untagged or cross-implementation states before mutating guest state. */
static inline uint32_t rtl_snapshot_magic(bool has_dsp1) {
  if (!has_dsp1) return 0x52544c53u; /* historical RTLS */
  return SNESRECOMP_DSP1_HLE ? 0x31504853u : 0x31504c53u; /* SHP1 / SLP1 */
}

#endif
