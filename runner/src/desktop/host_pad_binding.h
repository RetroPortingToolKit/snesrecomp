#ifndef SNES_HOST_PAD_BINDING_H
#define SNES_HOST_PAD_BINDING_H
#include <stdbool.h>
#include <stdint.h>

/* recomp-ui's portable controller encoding uses standard SDL button/axis
 * indices. Zero is an unspecified default; -1 is an explicitly cleared row.
 * Both stick clicks have no SNES gameplay bits. */
#define SNES_HOST_STATE_MENU_PAD_DEFAULT (1000 + (1 << 7) + (1 << 8))

static inline bool SnesHostPadBindingValid(int binding) {
  if (binding == -1 || binding == 0) return true;
  if (binding > 0 && binding < 100) return binding <= 32;
  if (binding >= 100 && binding < 1000) return (binding - 100) / 2 < 6;
  return binding > 1000;
}

static inline bool SnesHostPadBindingDown(int binding, uint32_t buttons,
                                        const int16_t axes[6]) {
  if (!SnesHostPadBindingValid(binding) || binding <= 0) return false;
  if (binding < 100) return (buttons & (UINT32_C(1) << (binding - 1))) != 0;
  if (binding < 1000) {
    const int code = (binding - 100) / 2;
    return ((binding - 100) & 1) ? axes[code] > 16384 : axes[code] < -16384;
  }
  const uint32_t mask = (uint32_t)(binding - 1000);
  return (buttons & mask) == mask;
}
#endif
