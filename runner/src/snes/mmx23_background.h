#ifndef SNES_MMX23_BACKGROUND_H
#define SNES_MMX23_BACKGROUND_H
#include <stdbool.h>
#include <stdint.h>
struct Snes;
struct Ppu;
void Mmx23ConfigureWsBgMargins(struct Snes *snes, struct Ppu *ppu,
                              uint8_t *ram, bool enabled);
/* Optional authored right boundary: repeat the preceding 256px of BG1 into
 * the right gutter only. Zero disables this room-specific presentation.
 * bg2SourceCanvas selects a known loaded object canvas: native validation
 * still applies, but an empty native view does not reject offscreen art. */
void Mmx23ConfigureWsBgMarginsForRoom(struct Snes *snes, struct Ppu *ppu,
                                    uint8_t *ram, bool enabled,
                                    uint16_t bg1RepeatRight,
                                    bool bg2SourceCanvas);
#endif
