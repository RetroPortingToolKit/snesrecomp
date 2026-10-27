/*
 * Runtime half of cfg `mod_hook` (docs/MOD_HOOK.md): arming, the contract
 * checks around each callback, and the interpreter-tier registration.
 *
 * Compiled code tests `g_snes_mod_hook_armed[slot]` (one load + branch while
 * disarmed) and calls snes_mod_hook_invoke() through its program's generated
 * `snes_mod_hook_dispatch`. Interpreted code reaches the same invoke through
 * one interpreter pre-opcode registration per armed hook PC, installed on arm
 * and removed on disarm, so both tiers run the callback once per execution of
 * the hooked instruction with identical semantics.
 */
#include "mod_hook.h"

#include <stdio.h>
#include <stdlib.h>

#include "snes/interp_bridge.h"

enum { kMaxPrograms = 16, kMaxLogged = 64 };

static struct {
    const SnesModHookDecl *table;
    unsigned count;
    uint8 *armed;
} s_programs[kMaxPrograms];
static unsigned s_program_count;

#if !SNESRECOMP_MOD_HOOK_STRICT
/* Production builds log each (hook, violation) pair once. */
static struct {
    const SnesModHookDecl *decl;
    unsigned kinds;
} s_logged[kMaxLogged];
static unsigned s_logged_count;
#endif

static void mod_hook_interp(CpuState *cpu, uint32_t pc24);

bool snes_mod_hook_register_program(const SnesModHookDecl *table,
                                    unsigned count, uint8 *armed) {
    if (!table || !armed) return false;
    for (unsigned i = 0; i < s_program_count; ++i)
        if (s_programs[i].table == table) return true;
    if (s_program_count >= kMaxPrograms) {
        fprintf(stderr,
                "[mod_hook] program registry full (%u); a program's %u hooks "
                "can never be armed\n", (unsigned)kMaxPrograms, count);
        return false;
    }
    s_programs[s_program_count].table = table;
    s_programs[s_program_count].count = count;
    s_programs[s_program_count].armed = armed;
    ++s_program_count;
    return true;
}

static bool decl_has_pc(const SnesModHookDecl *d, uint32 pc24) {
    pc24 &= 0xFFFFFFu;
    for (unsigned i = 0; i < d->pc_count && i < SNES_MOD_HOOK_MAX_ALIASES; ++i)
        if (d->pcs[i] == pc24) return true;
    return false;
}

/* Is any armed hook, in any program, executed at an interpreter key equal to
 * `pc24`'s? The bridge folds bank bit 7, so the $00 and $80 aliases share one
 * registration. */
static bool interp_key_still_armed(uint32 pc24) {
    const uint32 key = pc24 & 0x7FFFFFu;
    for (unsigned p = 0; p < s_program_count; ++p) {
        for (unsigned i = 0; i < s_programs[p].count; ++i) {
            const SnesModHookDecl *d = &s_programs[p].table[i];
            if (!s_programs[p].armed[d->slot]) continue;
            for (unsigned a = 0; a < d->pc_count; ++a)
                if ((d->pcs[a] & 0x7FFFFFu) == key) return true;
        }
    }
    return false;
}

static void set_armed(unsigned p, const SnesModHookDecl *d, bool armed) {
    uint8 *flag = &s_programs[p].armed[d->slot];
    if (!!*flag == armed) return;
    *flag = armed ? 1 : 0;
    for (unsigned a = 0; a < d->pc_count && a < SNES_MOD_HOOK_MAX_ALIASES; ++a) {
        if (armed) {
            if (!interp_bridge_add_pre_opcode_hook(d->pcs[a], mod_hook_interp))
                fprintf(stderr,
                        "[mod_hook] %s: interpreter hook table full; $%06X "
                        "will not fire in interpreted code\n",
                        d->name, (unsigned)d->pcs[a]);
        } else if (!interp_key_still_armed(d->pcs[a])) {
            interp_bridge_remove_pre_opcode_hook(d->pcs[a], mod_hook_interp);
        }
    }
}

bool snes_mod_hook_arm(uint32 pc24, bool armed) {
    bool found = false;
    for (unsigned p = 0; p < s_program_count; ++p) {
        for (unsigned i = 0; i < s_programs[p].count; ++i) {
            const SnesModHookDecl *d = &s_programs[p].table[i];
            if (!decl_has_pc(d, pc24)) continue;
            found = true;
            set_armed(p, d, armed);
        }
    }
    return found;
}

void snes_mod_hook_disarm_all(void) {
    for (unsigned p = 0; p < s_program_count; ++p)
        for (unsigned i = 0; i < s_programs[p].count; ++i)
            set_armed(p, &s_programs[p].table[i], false);
}

bool snes_mod_hook_is_armed(uint32 pc24) {
    for (unsigned p = 0; p < s_program_count; ++p)
        for (unsigned i = 0; i < s_programs[p].count; ++i) {
            const SnesModHookDecl *d = &s_programs[p].table[i];
            if (decl_has_pc(d, pc24) && s_programs[p].armed[d->slot])
                return true;
        }
    return false;
}

unsigned snes_mod_hook_count(void) {
    unsigned n = 0;
    for (unsigned p = 0; p < s_program_count; ++p) n += s_programs[p].count;
    return n;
}

const SnesModHookDecl *snes_mod_hook_at(unsigned index) {
    for (unsigned p = 0; p < s_program_count; ++p) {
        if (index < s_programs[p].count) return &s_programs[p].table[index];
        index -= s_programs[p].count;
    }
    return NULL;
}

enum {
    kViolationWidths = 1u << 0,
    kViolationStack = 1u << 1,
    kViolationBank = 1u << 2,
    kViolationTarget = 1u << 3,
};

static void violation(const SnesModHookDecl *d, uint32 pc24, unsigned kind,
                      const char *what) {
#if SNESRECOMP_MOD_HOOK_STRICT
    (void)kind;
    fprintf(stderr, "[mod_hook] %s at $%06X broke its contract: %s\n",
            d->name, (unsigned)pc24, what);
    fflush(stderr);
    abort();
#else
    unsigned i = 0;
    for (; i < s_logged_count; ++i)
        if (s_logged[i].decl == d) break;
    if (i == s_logged_count) {
        if (s_logged_count >= kMaxLogged) return;
        s_logged[s_logged_count].decl = d;
        s_logged[s_logged_count].kinds = 0;
        ++s_logged_count;
    }
    if (s_logged[i].kinds & kind) return;
    s_logged[i].kinds |= kind;
    fprintf(stderr, "[mod_hook] %s at $%06X broke its contract: %s "
            "(corrected; reported once)\n", d->name, (unsigned)pc24, what);
#endif
}

uint32 snes_mod_hook_invoke(CpuState *cpu, const SnesModHookDecl *d,
                            uint32 pc24) {
    const uint8 m = cpu->m_flag, x = cpu->x_flag, e = cpu->emulation;
    const uint16 s = cpu->S;
    const uint8 pb = cpu->PB;
    uint32 r = d->fn(cpu, pc24) & 0xFFFFFFu;

    if (cpu->m_flag != m || cpu->x_flag != x || cpu->emulation != e) {
        violation(d, pc24, kViolationWidths, "changed M, X or E");
        cpu->m_flag = m; cpu->x_flag = x; cpu->emulation = e;
    }
    if (cpu->S != s) {
        violation(d, pc24, kViolationStack, "changed S");
        cpu->S = s;
    }
    if (cpu->PB != pb) {
        violation(d, pc24, kViolationBank, "changed PB");
        cpu->PB = pb;
    }
    /* Narrow index registers have a zero high byte on hardware. */
    if (cpu->x_flag) {
        cpu->X &= 0x00FFu;
        cpu->Y &= 0x00FFu;
    }
    cpu_mirrors_to_p(cpu);

    if (r == 0) return 0;
    const uint32 bank = (pc24 >> 16) & 0xFFu;
    bool bank_ok = false;
    for (unsigned a = 0; a < d->pc_count && a < SNES_MOD_HOOK_MAX_ALIASES; ++a)
        if (((d->pcs[a] >> 16) & 0xFFu) == ((r >> 16) & 0xFFu)) bank_ok = true;
    bool declared = false;
    for (unsigned t = 0; bank_ok && t < d->target_count; ++t)
        if (d->targets[t] == (uint16)(r & 0xFFFFu)) declared = true;
    if (!declared) {
        violation(d, pc24, kViolationTarget,
                  "returned a PC that is not a declared mod_hook_target");
        return 0;
    }
    return (bank << 16) | (r & 0xFFFFu);
}

/* Interpreter tier: one registration per armed hook PC. The armed flag is
 * checked again here, so a hook disarmed by another callback at this same
 * instruction does not run. */
static void mod_hook_interp(CpuState *cpu, uint32_t pc24) {
    for (unsigned p = 0; p < s_program_count; ++p) {
        for (unsigned i = 0; i < s_programs[p].count; ++i) {
            const SnesModHookDecl *d = &s_programs[p].table[i];
            if (!s_programs[p].armed[d->slot] || !decl_has_pc(d, pc24))
                continue;
            const uint32 r = snes_mod_hook_invoke(cpu, d, pc24);
            if (r) interp_bridge_pre_opcode_redirect(r);
            return;
        }
    }
}
