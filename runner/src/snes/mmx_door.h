#ifndef SNESRECOMP_MMX_DOOR_H
#define SNESRECOMP_MMX_DOOR_H
#include <stdbool.h>
#include <stdint.h>

/* X1's structural door test, also used by X2/X3. Require the full mirrored
 * three-metatile body and distinct cap/middle tile IDs. Symmetric unflipped
 * walls can have similar caps and must not be mistaken for door faces. */
static inline bool MmxDoorBody(const uint16_t words[3][4]) {
  const uint16_t *t = words[0], *m = words[1], *b = words[2];
  const uint16_t h = 0x4000, v = 0x8000;
  return t[1] == (uint16_t)(t[0] ^ h) &&
      t[3] == (uint16_t)(t[2] ^ h) &&
      m[1] == (uint16_t)(m[0] ^ h) &&
      m[2] == (uint16_t)(m[0] ^ v) &&
      m[3] == (uint16_t)(m[0] ^ h ^ v) &&
      b[0] == (uint16_t)(t[2] ^ 0x8000) &&
      b[1] == (uint16_t)(t[3] ^ 0x8000) &&
      b[2] == (uint16_t)(t[0] ^ 0x8000) &&
      b[3] == (uint16_t)(t[1] ^ 0x8000) &&
      (t[0] & 1023) != (t[2] & 1023) &&
      (m[0] & 1023) != (t[0] & 1023) &&
      (m[0] & 1023) != (t[2] & 1023);
}
#endif
