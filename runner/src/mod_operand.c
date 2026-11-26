/*
 * Runtime half of cfg `mod_operand` (docs/MOD_OPERAND.md). Sites are keyed by
 * the ROM offset of their opcode byte, so every bank alias of a site (LoROM
 * $00/$80, HiROM $00/$40/$80/$C0) shares one value without a mapping table.
 */
#include "mod_operand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/interp816.h"
#include "snes/snes.h"

extern Snes *g_snes;

typedef struct {
    uint32 offset;
    uint16 value;
    bool active;
} OperandSite;
/* Sized from declarations before main(), never allocated during execution. */
static OperandSite *s_sites;
static unsigned s_site_count;

/* Read by snes_mod_operand() in compiled code: zero means "all stock". */
unsigned g_snes_mod_operand_active;

static bool rom_offset_of(uint32 pc24, uint32 *offset) {
    if (pc24 > 0xFFFFFFu || !g_snes || !g_snes->cart || !g_snes->cart->rom) return false;
    const uint8 *p = cart_getRomPtr(g_snes->cart, (uint8)(pc24 >> 16), (uint16)pc24);
    if (!p) return false;
    *offset = (uint32)(p - g_snes->cart->rom);
    return true;
}

static OperandSite *find_site(uint32 offset) {
    unsigned lo = 0, hi = s_site_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (s_sites[mid].offset == offset) return &s_sites[mid];
        if (s_sites[mid].offset < offset) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static bool interp_override(uint32 pc24, uint16 *value);

static int compare_sites(const void *a, const void *b) {
    uint32 x = ((const OperandSite *)a)->offset;
    uint32 y = ((const OperandSite *)b)->offset;
    return (x > y) - (x < y);
}

bool snes_mod_operand_register_sites(const uint32 *rom_offsets, unsigned count) {
    if (!count) return true;
    if (!rom_offsets || count > UINT_MAX - s_site_count) return false;
#if SIZE_MAX <= UINT_MAX
    if (count + s_site_count > SIZE_MAX / sizeof(OperandSite)) return false;
#endif
    OperandSite *sites = calloc(count + s_site_count, sizeof(*sites));
    if (!sites) return false;
    if (s_site_count) memcpy(sites, s_sites, s_site_count * sizeof(*sites));
    for (unsigned i = 0; i < count; ++i) {
        sites[s_site_count + i].offset = rom_offsets[i];
    }
    unsigned total = count + s_site_count, unique = 0;
    qsort(sites, total, sizeof(*sites), compare_sites);
    for (unsigned i = 0; i < total; ++i) {
        if (unique && sites[unique - 1].offset == sites[i].offset) {
            if (sites[i].active) sites[unique - 1] = sites[i];
        } else sites[unique++] = sites[i];
    }
    free(s_sites);
    s_sites = sites;
    s_site_count = unique;
    interp816_set_operand_override(interp_override);
    return true;
}

bool snes_mod_operand_set(uint32 pc24, uint16 value) {
    uint32 offset;
    OperandSite *site = rom_offset_of(pc24, &offset) ? find_site(offset) : NULL;
    if (!site) {
        fprintf(stderr, "[mod_operand] $%06X is not a declared mod_operand "
                "site; the operand stays stock\n", (unsigned)pc24);
        return false;
    }
    if (!site->active) ++g_snes_mod_operand_active;
    site->active = true;
    site->value = value;
    return true;
}

void snes_mod_operand_clear(uint32 pc24) {
    uint32 offset;
    if (!rom_offset_of(pc24, &offset)) return;
    OperandSite *site = find_site(offset);
    if (!site || !site->active) return;
    site->active = false;
    --g_snes_mod_operand_active;
}

void snes_mod_operand_clear_all(void) {
    for (unsigned i = 0; i < s_site_count; ++i) s_sites[i].active = false;
    g_snes_mod_operand_active = 0;
}

bool snes_mod_operand_get(uint32 pc24, uint16 *value) {
    uint32 offset;
    if (!value || !g_snes_mod_operand_active || !rom_offset_of(pc24, &offset)) return false;
    const OperandSite *site = find_site(offset);
    if (!site || !site->active) return false;
    *value = site->value;
    return true;
}

unsigned snes_mod_operand_active_count(void) { return g_snes_mod_operand_active; }

uint16 snes_mod_operand_lookup(uint32 pc24, uint16 stock) {
    uint16 value;
    return snes_mod_operand_get(pc24, &value) ? value : stock;
}

static bool interp_override(uint32 pc24, uint16 *value) {
    return snes_mod_operand_get(pc24, value);
}
