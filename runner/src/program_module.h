#ifndef SNESRECOMP_PROGRAM_MODULE_H
#define SNESRECOMP_PROGRAM_MODULE_H

/*
 * Program modules: the registry of statically recompiled programs linked into
 * one executable.
 *
 * A generated tree (src/gen, a BS Deluxe tree, a Super Mario All-Stars tree,
 * a ROM hack's tree) is one MODULE: a dispatch table, the RAM-routine guards
 * that police its WRAM bodies, and the identity of the ROM image it was
 * generated from. v2_emit emits a `module_v2.c` per tree whose constructor
 * registers the module here before main(), so the runtime can enumerate what
 * it was linked with and switch the CPU between modules by id instead of
 * every port hand-wiring `extern const DispatchEntry deluxe_g_dispatch_table[]`
 * (which is what F-Zero did, and what this replaces).
 *
 * Selecting a module is exactly cpu_select_program() with that module's
 * tables. It must happen before guest execution enters the module -- at boot,
 * or across a full machine rebuild (snes_free + SnesInit) -- never mid-frame.
 *
 * Nothing here touches guest state; the registry is host bookkeeping.
 */

#include <stddef.h>
#include <stdint.h>

#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesProgramModule {
    /* Stable id from `v2_emit --module-id` ("smw", "smas", "bs-deluxe"). */
    const char *id;
    /* `v2_emit --module-prefix`, or "" for an unprefixed (stock) tree. */
    const char *symbol_prefix;
    const DispatchEntry *dispatch;
    unsigned dispatch_count;
    const RamRoutineGuard *guards;
    unsigned guard_count;
    /* The image this module was generated from: size in bytes (after any
     * copier header was stripped) and its SHA-256. A module must only ever run
     * against that exact image; snes_program_module_rom_matches() is the
     * check. */
    uint32_t rom_size;
    uint8_t rom_sha256[32];
} SnesProgramModule;

#define SNES_PROGRAM_MODULE_MAX 16

/* Register a module. Called from generated constructors; also usable by a
 * host for a hand-assembled module. Returns 1 on success, 0 when the id is
 * empty, already registered, or the registry is full -- and says so on stderr,
 * because a silently dropped module is a variant that can never be selected. */
int snes_program_module_register(const SnesProgramModule *module);

unsigned snes_program_module_count(void);
const SnesProgramModule *snes_program_module_at(unsigned index);
const SnesProgramModule *snes_program_module_find(const char *id);

/* Does `rom` (size bytes, header already stripped) match the image this
 * module was generated from? */
int snes_program_module_rom_matches(const SnesProgramModule *module,
                                    const uint8_t *rom, size_t size);

/* Make `module` the program the CPU dispatches into. NULL restores the
 * generated default tables (g_dispatch_table) -- the unprefixed stock module
 * when one is linked. Never call mid-frame. */
void snes_program_module_select(const SnesProgramModule *module);
const SnesProgramModule *snes_program_module_active(void);

/* Constructor plumbing shared with mod_runtime.h's SNES_MOD_CONSTRUCTOR; kept
 * separate so generated code depends on nothing but this header. */
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define SNES_PROGRAM_MODULE_CONSTRUCTOR(name)                                \
    static void __cdecl name(void);                                          \
    __declspec(allocate(".CRT$XCU"))                                         \
    static void (__cdecl *name##_constructor)(void) = name;                  \
    static void __cdecl name(void)
#elif defined(__GNUC__) || defined(__clang__)
#define SNES_PROGRAM_MODULE_CONSTRUCTOR(name)                                \
    static void name(void) __attribute__((constructor));                     \
    static void name(void)
#else
#error "program module registration needs a supported constructor mechanism"
#endif

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_PROGRAM_MODULE_H */
