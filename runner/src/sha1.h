#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compute SHA-1 (RFC 3174) of `len` bytes at `data`. Writes 20 bytes to
 * `out`. One-shot reference implementation; used to name ROM dumps against
 * No-Intro catalogs, which publish SHA-1. */
void snes_sha1_compute(const uint8_t *data, size_t len, uint8_t out[20]);

#ifdef __cplusplus
}
#endif
