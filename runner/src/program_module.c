#include "program_module.h"

#include <stdio.h>
#include <string.h>

#include "sha256.h"

static const SnesProgramModule *s_modules[SNES_PROGRAM_MODULE_MAX];
static unsigned s_module_count;
static const SnesProgramModule *s_active;

int snes_program_module_register(const SnesProgramModule *module) {
    if (!module || !module->id || !module->id[0]) {
        fprintf(stderr, "[program-module] refusing a module with no id\n");
        return 0;
    }
    if (snes_program_module_find(module->id)) {
        fprintf(stderr,
                "[program-module] duplicate module id '%s' -- two generated "
                "trees claim the same identity; the second is ignored\n",
                module->id);
        return 0;
    }
    if (s_module_count >= SNES_PROGRAM_MODULE_MAX) {
        fprintf(stderr,
                "[program-module] registry full (%u); module '%s' dropped\n",
                (unsigned)SNES_PROGRAM_MODULE_MAX, module->id);
        return 0;
    }
    s_modules[s_module_count++] = module;
    return 1;
}

unsigned snes_program_module_count(void) { return s_module_count; }

const SnesProgramModule *snes_program_module_at(unsigned index) {
    return index < s_module_count ? s_modules[index] : NULL;
}

const SnesProgramModule *snes_program_module_find(const char *id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < s_module_count; i++)
        if (strcmp(s_modules[i]->id, id) == 0) return s_modules[i];
    return NULL;
}

int snes_program_module_rom_matches(const SnesProgramModule *module,
                                    const uint8_t *rom, size_t size) {
    uint8_t digest[32];
    if (!module || !rom || size == 0) return 0;
    if (module->rom_size != 0 && (size_t)module->rom_size != size) return 0;
    sha256_compute(rom, size, digest);
    return memcmp(digest, module->rom_sha256, sizeof(digest)) == 0;
}

void snes_program_module_select(const SnesProgramModule *module) {
    s_active = module;
    if (!module) {
        cpu_select_program(NULL, 0, NULL, 0);
        return;
    }
    cpu_select_program(module->dispatch, module->dispatch_count,
                       module->guards, module->guard_count);
}

const SnesProgramModule *snes_program_module_active(void) { return s_active; }
