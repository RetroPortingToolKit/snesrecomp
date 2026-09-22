#include "content_variant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define variant_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define variant_mkdir(p) mkdir((p), 0755)
#endif

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "generic_frame_driver.h"
#include "rom_patch.h"
#include "sha256.h"
#include "util.h"

#if SNESRECOMP_ENABLE_MODS
#include "mod_runtime.h"
#endif

#define VARIANT_MAX_IMAGE (16u * 1024u * 1024u)

static SnesContentVariant s_variants[SNES_VARIANT_MAX];
static unsigned s_count;
static const SnesContentVariant *s_active;
static int s_active_profile = 1;

static struct { char module_id[SNES_VARIANT_ID_CAP]; const RtlGameInfo *info; }
    s_bound[SNES_PROGRAM_MODULE_MAX];
static unsigned s_bound_count;

static void copy_text(char *dst, size_t cap, const char *src) {
    if (!src) src = "";
    snprintf(dst, cap, "%s", src);
}

static void set_reason(SnesContentVariant *v, const char *fmt, const char *arg) {
    if (arg) snprintf(v->reason, sizeof(v->reason), fmt, arg);
    else snprintf(v->reason, sizeof(v->reason), "%s", fmt);
}

/* ── Registration ─────────────────────────────────────────────────────────── */

static SnesContentVariant *find_mut(const char *id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < s_count; i++)
        if (strcmp(s_variants[i].decl.id, id) == 0) return &s_variants[i];
    return NULL;
}

static SnesContentVariant *alloc_slot(const char *id) {
    SnesContentVariant *existing = find_mut(id);
    if (existing) return existing;
    if (s_count >= SNES_VARIANT_MAX) {
        fprintf(stderr, "[variant] registry full (%u); '%s' dropped\n",
                (unsigned)SNES_VARIANT_MAX, id);
        return NULL;
    }
    SnesContentVariant *v = &s_variants[s_count++];
    memset(v, 0, sizeof(*v));
    return v;
}

const SnesContentVariant *snes_variant_register_builtin(
    const char *id, const char *display_name, const char *module_id,
    const char *selector_group, int selector_order,
    int has_selector_screen, uint32_t selector_wram_addr,
    uint8_t selector_wram_value) {
    if (!id || !id[0]) return NULL;
    if (find_mut(id)) {
        fprintf(stderr, "[variant] duplicate builtin id '%s'\n", id);
        return NULL;
    }
    SnesContentVariant *v = alloc_slot(id);
    if (!v) return NULL;
    copy_text(v->decl.id, sizeof(v->decl.id), id);
    copy_text(v->decl.display_name, sizeof(v->decl.display_name),
              display_name && display_name[0] ? display_name : id);
    copy_text(v->decl.module_id, sizeof(v->decl.module_id),
              module_id && module_id[0] ? module_id : "main");
    copy_text(v->decl.save_namespace, sizeof(v->decl.save_namespace), id);
    copy_text(v->decl.selector_group, sizeof(v->decl.selector_group),
              selector_group && selector_group[0] ? selector_group : "default");
    v->decl.selector_order = selector_order;
    v->decl.has_selector_screen = has_selector_screen;
    v->decl.selector_wram_addr = selector_wram_addr;
    v->decl.selector_wram_value = selector_wram_value;
    v->builtin = 1;
    v->available = 1;
    v->reason[0] = '\0';
    if (!s_active) s_active = v;
    return v;
}

const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *decl) {
    if (!decl || !decl->id[0]) return NULL;
    SnesContentVariant *existing = find_mut(decl->id);
    if (existing && existing->builtin) {
        fprintf(stderr, "[variant] package variant '%s' collides with the "
                        "title's builtin variant; ignored\n", decl->id);
        return NULL;
    }
    SnesContentVariant *v = alloc_slot(decl->id);
    if (!v) return NULL;
    v->decl = *decl;
    if (!v->decl.display_name[0])
        copy_text(v->decl.display_name, sizeof(v->decl.display_name), decl->id);
    if (!v->decl.save_namespace[0])
        copy_text(v->decl.save_namespace, sizeof(v->decl.save_namespace), decl->id);
    if (!v->decl.selector_group[0])
        copy_text(v->decl.selector_group, sizeof(v->decl.selector_group), "default");
    if (v->decl.boot.poke_count > 8) v->decl.boot.poke_count = 8;
    v->builtin = 0;
    v->available = 0;
    set_reason(v, "not yet checked", NULL);
    v->module = NULL;
    return v;
}

void snes_variant_clear_declared(void) {
    unsigned kept = 0;
    for (unsigned i = 0; i < s_count; i++) {
        if (s_variants[i].builtin) {
            if (kept != i) s_variants[kept] = s_variants[i];
            kept++;
        } else if (s_active == &s_variants[i]) {
            s_active = NULL;
        }
    }
    s_count = kept;
    if (!s_active && s_count) s_active = &s_variants[0];
}

/* ── Enumeration ──────────────────────────────────────────────────────────── */

unsigned snes_variant_count(void) { return s_count; }
const SnesContentVariant *snes_variant_at(unsigned index) {
    return index < s_count ? &s_variants[index] : NULL;
}
const SnesContentVariant *snes_variant_find(const char *id) { return find_mut(id); }
const SnesContentVariant *snes_variant_active(void) { return s_active; }
int snes_variant_active_profile(void) { return s_active_profile; }

/* ── Frame drivers ────────────────────────────────────────────────────────── */

void snes_variant_bind_game_info(const char *module_id, const RtlGameInfo *info) {
    if (!module_id || !module_id[0]) return;
    for (unsigned i = 0; i < s_bound_count; i++) {
        if (strcmp(s_bound[i].module_id, module_id) == 0) {
            s_bound[i].info = info;
            return;
        }
    }
    if (s_bound_count >= SNES_PROGRAM_MODULE_MAX) return;
    copy_text(s_bound[s_bound_count].module_id,
              sizeof(s_bound[s_bound_count].module_id), module_id);
    s_bound[s_bound_count].info = info;
    s_bound_count++;
}

const RtlGameInfo *snes_variant_game_info_for(const SnesContentVariant *variant) {
    if (!variant) return NULL;
    for (unsigned i = 0; i < s_bound_count; i++)
        if (strcmp(s_bound[i].module_id, variant->decl.module_id) == 0 && s_bound[i].info)
            return s_bound[i].info;
    return snes_generic_frame_driver_game_info(variant->decl.id);
}

/* ── Image resolution ─────────────────────────────────────────────────────── */

#if SNESRECOMP_ENABLE_MODS
static int feature_on(const SnesVariantDecl *d) {
    return snes_mod_runtime_feature_enabled_c(d->package_id, d->feature_id) != 0;
}
static int resource_path(const SnesVariantDecl *d, char *out, size_t cap) {
    return snes_mod_runtime_resource_path_c(d->package_id, d->feature_id,
                                            d->source_rom_id, out, (uint32_t)cap) != 0;
}
#else
static int feature_on(const SnesVariantDecl *d) { (void)d; return 0; }
static int resource_path(const SnesVariantDecl *d, char *out, size_t cap) {
    (void)d; if (cap) out[0] = '\0'; return 0;
}
#endif

static int digest_matches(const uint8_t *data, size_t size, const uint8_t expect[32]) {
    uint8_t actual[32];
    sha256_compute(data, size, actual);
    return memcmp(actual, expect, 32) == 0;
}

/* Produce the image a declared variant runs. Returns a malloc'd buffer or
 * NULL with `reason` filled. `launched_*` is the title's own image. */
static uint8_t *resolve_image(const SnesVariantDecl *d,
                              const uint8_t *launched_rom, size_t launched_size,
                              size_t *out_size, char *reason, size_t reason_cap) {
    const uint8_t *source = launched_rom;
    size_t source_size = launched_size;
    uint8_t *owned_source = NULL;

    if (d->source_rom_id[0]) {
        char path[SNES_VARIANT_PATH_CAP];
        if (!resource_path(d, path, sizeof(path)) || !path[0]) {
            snprintf(reason, reason_cap, "select the required ROM in Mods");
            return NULL;
        }
        size_t n = 0;
        owned_source = ReadWholeFile(path, &n);
        if (!owned_source) {
            snprintf(reason, reason_cap, "ROM file not found");
            return NULL;
        }
        source = owned_source;
        source_size = n;
        if (source_size % 1024 == 512) {
            source += 512;
            source_size -= 512;
        }
    }
    if (!source || source_size == 0) {
        snprintf(reason, reason_cap, "no source ROM");
        free(owned_source);
        return NULL;
    }
    if (d->has_source_sha256 && !digest_matches(source, source_size, d->source_sha256)) {
        snprintf(reason, reason_cap, "ROM does not match the required image");
        free(owned_source);
        return NULL;
    }

    uint8_t *image = NULL;
    size_t image_size = 0;
    if (d->patch_path[0]) {
        size_t patch_size = 0;
        uint8_t *patch = ReadWholeFile(d->patch_path, &patch_size);
        if (!patch) {
            snprintf(reason, reason_cap, "patch file missing");
            free(owned_source);
            return NULL;
        }
        RomPatchStatus st = snes_rom_patch_apply(source, source_size, patch, patch_size,
                                            VARIANT_MAX_IMAGE, &image, &image_size);
        free(patch);
        if (st != kRomPatch_Ok) {
            snprintf(reason, reason_cap, "%s", snes_rom_patch_status_text(st));
            free(owned_source);
            return NULL;
        }
        if (d->has_patch_target_sha256 &&
            !digest_matches(image, image_size, d->patch_target_sha256)) {
            snprintf(reason, reason_cap, "patched ROM does not match its declared digest");
            free(image);
            free(owned_source);
            return NULL;
        }
    } else {
        image = malloc(source_size);
        if (!image) {
            snprintf(reason, reason_cap, "out of memory");
            free(owned_source);
            return NULL;
        }
        memcpy(image, source, source_size);
        image_size = source_size;
    }
    free(owned_source);
    *out_size = image_size;
    reason[0] = '\0';
    return image;
}

static int check_declared(SnesContentVariant *v, const uint8_t *launched_rom,
                          size_t launched_size, uint8_t **image_out,
                          size_t *image_size_out) {
    const SnesVariantDecl *d = &v->decl;
    v->module = snes_program_module_find(d->module_id);
    if (image_out) *image_out = NULL;
#if !SNESRECOMP_ENABLE_MODS
    set_reason(v, "this build has no mod support", NULL);
    return 0;
#endif
    if (!feature_on(d)) {
        set_reason(v, "enable it in Mods", NULL);
        return 0;
    }
    if (!v->module) {
        set_reason(v, "module '%s' is not part of this build", d->module_id);
        return 0;
    }
    char reason[160];
    size_t image_size = 0;
    uint8_t *image = resolve_image(d, launched_rom, launched_size, &image_size,
                                   reason, sizeof(reason));
    if (!image) {
        set_reason(v, "%s", reason);
        return 0;
    }
    if (!snes_program_module_rom_matches(v->module, image, image_size)) {
        set_reason(v, "image does not match module '%s'", d->module_id);
        free(image);
        return 0;
    }
    v->reason[0] = '\0';
    if (image_out) {
        *image_out = image;
        *image_size_out = image_size;
    } else {
        free(image);
    }
    return 1;
}

void snes_variant_refresh(const uint8_t *launched_rom, size_t launched_size) {
    for (unsigned i = 0; i < s_count; i++) {
        SnesContentVariant *v = &s_variants[i];
        if (v->builtin) {
            v->module = snes_program_module_find(v->decl.module_id);
            v->available = 1;
            v->reason[0] = '\0';
            continue;
        }
        v->available = check_declared(v, launched_rom, launched_size, NULL, NULL);
    }
}

/* ── Saves ────────────────────────────────────────────────────────────────── */

static void mkdir_p(const char *path) {
    char buf[SNES_VARIANT_PATH_CAP];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            variant_mkdir(buf);
            *p = c;
        }
    }
    variant_mkdir(buf);
}

void snes_variant_save_root(const SnesContentVariant *variant, int profile,
                            const char *base_root, char *out, size_t cap) {
    if (!base_root || !base_root[0]) base_root = "saves";
    if (profile < 1) profile = 1;
    if (profile > SNES_VARIANT_PROFILES) profile = SNES_VARIANT_PROFILES;
    if (!variant || (variant->builtin && profile == 1)) {
        snprintf(out, cap, "%s", base_root);
        return;
    }
    snprintf(out, cap, "%s/variants/%s/p%d", base_root,
             variant->decl.save_namespace, profile);
}

static void selection_path(const char *base_root, char *out, size_t cap) {
    snprintf(out, cap, "%s/variants/last-selection.txt",
             base_root && base_root[0] ? base_root : "saves");
}

int snes_variant_load_last_selection(const char *base_root,
                                     char *id, size_t id_cap, int *profile) {
    char path[SNES_VARIANT_PATH_CAP];
    selection_path(base_root, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[128];
    int ok = 0;
    if (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n) {
            snprintf(id, id_cap, "%s", line);
            int p = 1;
            if (fgets(line, sizeof(line), f)) p = atoi(line);
            if (p < 1 || p > SNES_VARIANT_PROFILES) p = 1;
            if (profile) *profile = p;
            ok = snes_variant_find(id) != NULL;
        }
    }
    fclose(f);
    return ok;
}

int snes_variant_store_last_selection(const char *base_root,
                                      const char *id, int profile) {
    char path[SNES_VARIANT_PATH_CAP], dir[SNES_VARIANT_PATH_CAP];
    snprintf(dir, sizeof(dir), "%s/variants",
             base_root && base_root[0] ? base_root : "saves");
    mkdir_p(dir);
    selection_path(base_root, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "%s\n%d\n", id ? id : "", profile);
    fclose(f);
    return 1;
}

/* ── Activation ───────────────────────────────────────────────────────────── */

static char s_base_root[96];

static const char *base_root(void) {
    if (!s_base_root[0]) snprintf(s_base_root, sizeof(s_base_root), "%s", RtlSaveRoot());
    return s_base_root;
}

int snes_variant_prepare(const SnesContentVariant *variant, int profile,
                         const uint8_t *launched_rom, size_t launched_size,
                         const uint8_t **rom, size_t *rom_size,
                         int *rom_is_owned,
                         char *error, size_t error_cap) {
    if (error && error_cap) error[0] = '\0';
    if (!variant || !rom || !rom_size || !rom_is_owned) {
        if (error) snprintf(error, error_cap, "invalid variant request");
        return 0;
    }
    SnesContentVariant *v = find_mut(variant->decl.id);
    if (!v) {
        if (error) snprintf(error, error_cap, "unknown variant");
        return 0;
    }
    if (profile < 1) profile = 1;
    if (profile > SNES_VARIANT_PROFILES) profile = SNES_VARIANT_PROFILES;

    const uint8_t *image = launched_rom;
    size_t image_size = launched_size;
    int owned = 0;
    if (v->builtin) {
        v->module = snes_program_module_find(v->decl.module_id);
        if (!image || !image_size) {
            if (error) snprintf(error, error_cap, "no launched ROM");
            return 0;
        }
    } else {
        uint8_t *resolved = NULL;
        if (!check_declared(v, launched_rom, launched_size, &resolved, &image_size)) {
            v->available = 0;
            if (error) snprintf(error, error_cap, "%s", v->reason);
            return 0;
        }
        image = resolved;
        owned = 1;
    }
    v->available = 1;

    /* Everything verified: commit host state. Order matters only in that the
     * module must be selected before any guest code can dispatch, which is
     * before SnesInit -- and the caller holds SnesInit until we return. */
    snes_program_module_select(v->module);   /* NULL => generated default */
    snes_generic_frame_driver_set_boot_policy(v->builtin ? NULL : &v->decl.boot);
    snes_generic_frame_driver_reset();
    RtlRegisterGame(snes_variant_game_info_for(v));

    char root[96];
    snes_variant_save_root(v, profile, base_root(), root, sizeof(root));
    mkdir_p(root);
    RtlSetSaveRoot(root);
    RtlEnsureSaveDir();

    s_active = v;
    s_active_profile = profile;
    snes_variant_store_last_selection(base_root(), v->decl.id, profile);
    fprintf(stderr, "[variant] active '%s' (module %s) profile %d saves=%s\n",
            v->decl.id, v->module ? v->module->id : "default", profile, root);

    *rom = image;
    *rom_size = image_size;
    *rom_is_owned = owned;
    return 1;
}
