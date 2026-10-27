#ifndef SNESRECOMP_MOD_HOOK_H
#define SNESRECOMP_MOD_HOOK_H

/*
 * Mutating, runtime-armable guest-PC hooks (cfg `mod_hook`). Contract and
 * placement: docs/MOD_HOOK.md.
 *
 * A port declares `mod_hook <pc16> <Fn>` (and any `mod_hook_target
 * <pc16> <target16>`) in a bank cfg and provides
 *
 *     uint32 Fn(CpuState *cpu, uint32 pc24);
 *
 * The generator emits a per-program table (mod_hooks_v2.c) that registers
 * itself here before main(). Every hook starts disarmed. While armed, Fn runs
 * immediately before the instruction at the hooked PC executes, whether a
 * compiled body or the interpreter executes it. Fn may change A, X, Y, D, DB,
 * the N/V/Z/C/I/D flag mirrors (`cpu->_flag_*`) and guest memory. It must not
 * change M, X, E, S or PB. It returns 0 to continue at the hooked instruction
 * or a declared `mod_hook_target` PC (any bank that aliases the hook's bank)
 * to continue there instead.
 *
 * Checks (SNESRECOMP_MOD_HOOK_STRICT, default on unless NDEBUG): a changed
 * M/X/E/S/PB or an undeclared return aborts with a message naming the hook.
 * Without strict checks, M/X/E/S/PB are restored, an undeclared return is
 * treated as 0, and each kind of violation is logged once per hook.
 */

#include <stdbool.h>
#include <stdint.h>

#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SNESRECOMP_MOD_HOOK_STRICT
#ifdef NDEBUG
#define SNESRECOMP_MOD_HOOK_STRICT 0
#else
#define SNESRECOMP_MOD_HOOK_STRICT 1
#endif
#endif

typedef uint32 (*SnesModHookFn)(CpuState *cpu, uint32 pc24);

enum { SNES_MOD_HOOK_MAX_ALIASES = 4 };

/* One generated table row. */
typedef struct SnesModHookDecl {
    SnesModHookFn fn;
    const char *name;
    /* Every execution PC that aliases the hooked ROM byte, declaring bank
     * first (LoROM $00/$80; HiROM $00/$40/$80/$C0 views). */
    uint32 pcs[SNES_MOD_HOOK_MAX_ALIASES];
    unsigned pc_count;
    /* Declared redirect targets, 16-bit PCs in the hook's bank. */
    const uint16 *targets;
    unsigned target_count;
    unsigned slot;
} SnesModHookDecl;

/* Called by the generated constructor; `armed` has `count` bytes owned by the
 * generated tree. Returns false (and says so on stderr) when the registry is
 * full. */
bool snes_mod_hook_register_program(const SnesModHookDecl *table,
                                    unsigned count, uint8 *armed);

/* Arm or disarm every hook declared at pc24 or at a PC that aliases it, in
 * every registered program. Returns false when no declared hook matches. Arming
 * also installs the interpreter pre-opcode hook at every alias; disarming
 * removes exactly that registration (other owners' hooks are untouched). */
bool snes_mod_hook_arm(uint32 pc24, bool armed);
void snes_mod_hook_disarm_all(void);
bool snes_mod_hook_is_armed(uint32 pc24);

/* Number of hooks declared by all registered programs, and the i-th one. */
unsigned snes_mod_hook_count(void);
const SnesModHookDecl *snes_mod_hook_at(unsigned index);

/* Run one hook: call it, enforce the contract, and return 0 or the redirect
 * target normalised to pc24's bank. Called by generated code (through the
 * per-program `snes_mod_hook_dispatch`) and by the interpreter wrapper. */
uint32 snes_mod_hook_invoke(CpuState *cpu, const SnesModHookDecl *decl,
                            uint32 pc24);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_MOD_HOOK_H */
