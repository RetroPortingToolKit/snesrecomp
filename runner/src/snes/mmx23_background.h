#ifndef SNES_MMX23_BACKGROUND_H
#define SNES_MMX23_BACKGROUND_H
#include <stdbool.h>
#include <stdint.h>
struct Snes;
struct Ppu;
void Mmx23ConfigureWsBgMargins(struct Snes *snes, struct Ppu *ppu,
                              uint8_t *ram, bool enabled);
#endif
