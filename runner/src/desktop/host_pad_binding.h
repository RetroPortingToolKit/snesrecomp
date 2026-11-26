#ifndef SNES_HOST_PAD_BINDING_H
#define SNES_HOST_PAD_BINDING_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <recomp_input.h>

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
  ri_device device = {0};
  device.buttons = buttons;
  if (axes) memcpy(device.axes, axes, sizeof(device.axes));
  return ri_binding_held(&device, (uint32_t)binding, 16384) != 0;
}
#endif
