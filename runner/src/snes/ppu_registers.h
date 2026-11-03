#ifndef SNESRECOMP_PPU_REGISTERS_H
#define SNESRECOMP_PPU_REGISTERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Pure raster jobs consume captured register values, not a live device.
 * Palette/VRAM/OAM are separate immutable inputs. No pointers, buffers,
 * latches, renderer caches or simulation state belong in this object. */
typedef struct SnesRenderRegisters {
#define SNESRECOMP_PPU_REGISTER(type, name, array) type name array;
#include "ppu_register_fields.inc"
#undef SNESRECOMP_PPU_REGISTER
} SnesRenderRegisters;

#ifdef __cplusplus
static_assert(sizeof(SnesRenderRegisters) == 0x40, "captured PPU register layout");
#else
_Static_assert(sizeof(SnesRenderRegisters) == 0x40, "captured PPU register layout");
#endif

/* source supplies 0x40 readable bytes, possibly unaligned. Copy into a real
 * typed object; never reinterpret a short capture buffer as a full Ppu. */
static inline SnesRenderRegisters SnesRenderRegistersLoad(const void *source) {
  SnesRenderRegisters registers;
  memcpy(&registers, source, sizeof(registers));
  return registers;
}

#endif
