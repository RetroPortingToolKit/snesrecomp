#ifndef SNESRECOMP_CONTENT_VARIANT_H
#define SNESRECOMP_CONTENT_VARIANT_H

/*
 * Content variants: selectable programs a title can run, each with its own
 * ROM source, recompiled module, boot policy and save namespace.
 *
 *   variant  = "what the player picks" (Super Mario World, Super Mario Bros.,
 *              The Lost Levels, a ROM hack)
 *   module   = the recompiled program that runs it (program_module.h); one
 *              module can back several variants (one All-Stars tree serves
 *              four games) and one variant can reuse the stock module (a
 *              save-namespace-only variant)
 *   source   = which ROM image: the title's own launched ROM, or a
 *              user-supplied external ROM declared by a mod package, optionally
 *              run through an in-memory IPS/BPS patch
 *
 * Where variants come from:
 *   - the port registers its stock variant with snes_variant_register_builtin
 *     (always present, never removable: it is what the executable IS);
 *   - mod packages declare `[[variant]]` rows (mod_runtime.cpp parses them and
 *     pushes them here through snes_variant_register_declared) -- a row is
 *     AVAILABLE only when its feature is enabled in the committed plan, its
 *     module is linked into this executable, its source ROM is selected and
 *     hashes as declared, and any patch applies and hashes as declared.
 *
 * Saves: every (variant, profile) pair owns a save root. The stock variant's
 * profile 1 is the title's historical root ("saves") so upgrading changes
 * nothing for an existing player; everything else lives under
 * saves/variants/<save_namespace>/p<N>. The runtime never opens one
 * namespace's files while another is active.
 *
 * Switching (host-driven, see variant_selector.h and the port's session
 * reboot): RtlWriteSram on the old root -> snes_free -> snes_variant_prepare
 * (verify + patch + select module + register frame driver + save root) ->
 * SnesInit(image) -> RtlReadSram. Nothing here executes guest code.
 */

#include <stddef.h>
#include <stdint.h>

#include "program_module.h"
#include "snes_boot_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* common_cpu_infra.h is C-only; mod_runtime.cpp includes this header, so the
 * frame-driver type is referenced opaquely here. */
struct RtlGameInfo;

#define SNES_VARIANT_MAX 32
#define SNES_VARIANT_PROFILES 3
#define SNES_VARIANT_ID_CAP 64
#define SNES_VARIANT_NAME_CAP 64
#define SNES_VARIANT_PATH_CAP 1024

typedef struct SnesVariantDecl {
    char id[SNES_VARIANT_ID_CAP];             /* "smas.smb1" */
    char display_name[SNES_VARIANT_NAME_CAP]; /* "Super Mario Bros." */
    char module_id[SNES_VARIANT_ID_CAP];      /* program module id */
    char save_namespace[SNES_VARIANT_ID_CAP]; /* defaults to id */
    char selector_group[SNES_VARIANT_ID_CAP]; /* variants sharing a picker */
    int selector_order;
    /* The screen on which this variant shows the picker while active:
     * wram[addr] == value. Zero has_selector_screen = picker never shows
     * while this variant runs (a hack with no known safe screen). */
    int has_selector_screen;
    uint32_t selector_wram_addr;   /* offset into WRAM ($7E0000-based) */
    uint8_t selector_wram_value;
    /* Where the image comes from. */
    char package_id[SNES_VARIANT_ID_CAP];     /* declaring package ("" = builtin) */
    char feature_id[SNES_VARIANT_ID_CAP];     /* feature that must be enabled */
    char source_rom_id[SNES_VARIANT_ID_CAP];  /* [[external_rom]] id, "" = the launched ROM */
    uint8_t source_sha256[32];                /* required digest of the source image */
    int has_source_sha256;
    /* Optional patch applied in memory to the verified source. */
    char patch_path[SNES_VARIANT_PATH_CAP];   /* absolute path, "" = none */
    uint8_t patch_target_sha256[32];          /* required digest of the patched image */
    int has_patch_target_sha256;
    /* Boot policy handed to the generic frame driver. */
    SnesGenericBootPolicy boot;
} SnesVariantDecl;

typedef struct SnesContentVariant {
    SnesVariantDecl decl;
    int builtin;            /* registered by the port, not a package */
    int available;          /* refreshed by snes_variant_refresh */
    char reason[160];       /* why unavailable, for the picker/overlay */
    const SnesProgramModule *module;  /* resolved, or NULL */
} SnesContentVariant;

/* ── Registration ─────────────────────────────────────────────────────────── */

/* The stock variant. `module_id` names the unprefixed generated module
 * ("main" unless the port regenerated with --module-id). `selector_wram_addr`
 * / `_value` give the screen where the picker may appear (SMW: $0100 == 8).
 * Returns the variant or NULL (duplicate id / registry full). */
const SnesContentVariant *snes_variant_register_builtin(
    const char *id, const char *display_name, const char *module_id,
    const char *selector_group, int selector_order,
    int has_selector_screen, uint32_t selector_wram_addr,
    uint8_t selector_wram_value);

/* A package-declared variant (called by mod_runtime for each `[[variant]]`;
 * a host may also call it for a hand-assembled one). Re-registering an id
 * replaces the declaration. */
const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *decl);

/* Forget every package-declared variant (the mod catalog is being
 * re-initialized). Builtins stay. */
void snes_variant_clear_declared(void);

/* ── Enumeration ──────────────────────────────────────────────────────────── */
unsigned snes_variant_count(void);
const SnesContentVariant *snes_variant_at(unsigned index);
const SnesContentVariant *snes_variant_find(const char *id);
const SnesContentVariant *snes_variant_active(void);
int snes_variant_active_profile(void);   /* 1..SNES_VARIANT_PROFILES */

/* Recompute `available`/`reason` for every variant against the current mod
 * plan and files on disk (hashes the source ROM; cheap relative to boot).
 * `launched_rom`/`launched_size` is the title's own verified image, used by
 * builtins and by patch-only variants with no external source. */
void snes_variant_refresh(const uint8_t *launched_rom, size_t launched_size);

/* ── Activation ───────────────────────────────────────────────────────────── */

/* Resolve the image for `variant`, select its module and frame driver, and
 * set the save root for `profile`. On success (*rom, *rom_size) hold the image
 * to hand to SnesInit: for a builtin this is `launched_rom` itself (not
 * copied; caller keeps ownership); otherwise a malloc'd image the caller owns.
 * On failure nothing changes and `error` explains why. The host must have
 * torn down the previous machine (snes_free) before calling this and must
 * follow it with SnesInit + RtlReadSram. */
int snes_variant_prepare(const SnesContentVariant *variant, int profile,
                         const uint8_t *launched_rom, size_t launched_size,
                         const uint8_t **rom, size_t *rom_size,
                         int *rom_is_owned,
                         char *error, size_t error_cap);

/* The frame driver a module runs with. A port binds its own RtlGameInfo to
 * its stock module; anything unbound uses the generic driver. */
void snes_variant_bind_game_info(const char *module_id, const struct RtlGameInfo *info);
const struct RtlGameInfo *snes_variant_game_info_for(const SnesContentVariant *variant);

/* ── Saves ────────────────────────────────────────────────────────────────── */

/* Save root for a (variant, profile). `base_root` is the title's historical
 * root ("saves"); the stock variant's profile 1 IS that root. */
void snes_variant_save_root(const SnesContentVariant *variant, int profile,
                            const char *base_root, char *out, size_t cap);

/* Last selection, persisted beside the saves so a relaunch resumes the same
 * variant/profile. Returns 0 when nothing was stored or it no longer exists. */
int snes_variant_load_last_selection(const char *base_root,
                                     char *id, size_t id_cap, int *profile);
int snes_variant_store_last_selection(const char *base_root,
                                      const char *id, int profile);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_CONTENT_VARIANT_H */
