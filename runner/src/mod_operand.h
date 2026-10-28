/*
 * cfg `mod_operand` (docs/MOD_OPERAND.md): a mod-owned value for the immediate
 * operand of one declared instruction, switchable at run time.
 *
 * The loaded ROM image is never written. Compiled code asks for the operand
 * through snes_mod_operand() (declared in cpu_state.h; the stock value is
 * folded into the call), and the interpreter substitutes the same value when
 * it fetches that instruction's immediate, so both tiers see one value.
 *
 * Only sites declared with `mod_operand` in a bank cfg can be given a value:
 * elsewhere the compiled code has the constant folded in, and an override
 * would split the tiers. snes_mod_operand_set() refuses undeclared sites.
 */
#ifndef SNESRECOMP_MOD_OPERAND_H
#define SNESRECOMP_MOD_OPERAND_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the generated mod_operands_v2.c: ROM offsets of the opcode byte
 * of every declared site. Storage is sized from the declarations. Registration
 * is transactional; false means allocation failed or the input was invalid. */
bool snes_mod_operand_register_sites(const uint32 *rom_offsets, unsigned count);

/* Give the declared site at pc24 (any bank alias) the operand `value`; the
 * instruction's own width decides how many bytes of it are used. Returns
 * false when pc24 is not a declared cartridge-ROM site. */
bool snes_mod_operand_set(uint32 pc24, uint16 value);
/* Back to the ROM's operand. */
void snes_mod_operand_clear(uint32 pc24);
void snes_mod_operand_clear_all(void);

/* The value given to the site at pc24, if any. */
bool snes_mod_operand_get(uint32 pc24, uint16 *value);
unsigned snes_mod_operand_active_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_MOD_OPERAND_H */
