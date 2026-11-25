#ifndef SNES_RUNNER_INPUT_H
#define SNES_RUNNER_INPUT_H
#include <stdint.h>

/* The packed simulation word retains pads in bits 0..11 / 12..23 and
 * presence in 30..31. Three title actions per seat occupy 24..26 / 27..29.
 * Network samples carry those actions in bits 12..14 of their uint16 seat.
 * Guest joypads still receive only the original twelve buttons. */
#define SNES_INPUT_ACTION_COUNT 3
#define SNES_INPUT_SEAT_MASK 0x7fffu
/* Startup profiles are namespaced by this canonical layout, independently of
 * game version, build diagnostics or the initial machine-state digest. */
#define SNES_INPUT_SCHEMA_ID 0x534e4101u
static inline uint32_t SnesInputQualification(uint32_t qualification) {
  if (!qualification) return SNES_INPUT_SCHEMA_ID;
  uint32_t result=(qualification ^ SNES_INPUT_SCHEMA_ID) * 0x01000193u;
  return result ? result : SNES_INPUT_SCHEMA_ID;
}
static inline uint16_t SnesInputSeat(uint32_t word, unsigned seat) {
  if (seat > 1) return 0;
  return (uint16_t)(((word >> (seat * 12)) & 0xfffu) |
      (((word >> (24 + seat * 3)) & 7u) << 12));
}
static inline uint32_t SnesInputPackSeat(uint16_t word, unsigned seat) {
  if (seat > 1) return 0;
  return ((uint32_t)(word & 0xfffu) << (seat * 12)) |
      ((uint32_t)((word >> 12) & 7u) << (24 + seat * 3));
}
#endif
