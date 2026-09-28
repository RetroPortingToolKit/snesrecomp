#ifndef SNESRECOMP_SNES_BOOT_POLICY_H
#define SNESRECOMP_SNES_BOOT_POLICY_H

/*
 * Boot policy for a content variant run by the generic frame driver: applied
 * on the first frame, right after the reset vector is read and before the
 * guest executes -- WRAM/bus pokes and an entry PC override. This is how a
 * variant enters a sub-program of a multi-game image directly (Super Mario
 * All-Stars: poke the game index at $7F:FF00 and enter that game's init
 * instead of the menu). All-zero = plain reset.
 *
 * Kept in its own header so C++ (mod_runtime.cpp, which fills it from a
 * manifest) can include it without dragging in the C-only runtime headers.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNES_BOOT_POLICY_MAX_POKES 8

typedef struct SnesGenericBootPolicy {
    struct { uint32_t addr24; uint8_t value; } pokes[SNES_BOOT_POLICY_MAX_POKES];
    unsigned poke_count;
    uint32_t entry_pc24;   /* 0 = the reset vector */
} SnesGenericBootPolicy;

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_SNES_BOOT_POLICY_H */
