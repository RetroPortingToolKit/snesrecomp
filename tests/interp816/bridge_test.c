/*
 * interp_bridge Phase-1 contract harness (no game / no ROM).
 *
 * Proves the interp<->AOT bridge mechanics deterministically with fakes:
 *   - cpu_read8/cpu_write8  -> a flat RAM (the bus the bridge routes through);
 *   - cpu_dispatch_pc / cpu_dispatch_has_entry -> ONE known "compiled" entry
 *     whose fake body mutates A and pops its return frame (modelling a real
 *     AOT function's RTS: pop frame, dispatch-miss on return addr, S restored).
 *
 * Scenarios:
 *   S1: interp routine that JSRs into the compiled entry -> the bounce runs the
 *       compiled body, state syncs, stack stays balanced, resume at return addr.
 *   S2: pure interp routine (no call) -> exits balanced, no bounce.
 *   S3: interp routine that JSRs a NON-compiled target -> interpreted through,
 *       its RTS returns to caller level (no premature exit), final RTS exits.
 *   S13: runtime-call fallback executing M=0 PLA; RTL consumes its inner JSR
 *        and an outer JSL, propagating SKIP_1 to the compiled caller.
 *
 * Build/run: tests/interp816/run.sh (WSL gcc). Validation only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interp_bridge.h"   /* -> cpu_state.h (types, inline frame helpers) */
#include "tier2_capture.h"
#include "snes.h"            /* Snes storage for the bridge's APU clock hook */
#include "apu.h"
#include "sa1.h"
#include "snes_cycles.h"

CpuState g_cpu;

#define MEMSZ 0x1000000u
static uint8_t *RAM;
static int      g_aot_called;
static int      g_aot_rewrites_return;
static int      g_aot_nested_rewrite;
static int      g_aot_interp_nlr;
static int      g_aot_double_rewrite;
static int      g_aot_crosses_interp_owner;
static int      g_aot_skips_interp_owner;
static int      g_aot_deadline_unwind;
static int      g_aot_nested_deadline;
static int      g_after_nested_deadline;
static int      g_aot_gap_walks_into_wait;
static int      g_owner_target_result;
static int      g_aot_tail_chain_probe;
static int      g_aot_skips_root;
static int      g_tail_chain_direct;
static int      g_nested_chain_direct;
#define FAKE_AOT 0x008100u
#define FAKE_AOT_2 0x008200u

/* ── fakes the bridge links against (cpu_state.c provides these in prod) ── */
/* The Phase-2 manifest recorder stamps the live frame counter on each
 * discovery; the bridge references it as extern. */
int snes_frame_counter = 0;
const char *rtl_game_title(void) { return "bridge_test"; }
static Snes g_test_snes;
Snes *g_snes = &g_test_snes;
uint64_t g_apu_last_sync_master;
int g_interp_apu_driving;
static bool g_frame_timeline, g_extended_frames;
static unsigned g_absolute_syncs, g_relative_syncs;
bool rtl_apu_frame_timeline_active(void) { return g_frame_timeline; }
bool rtl_apu_extended_frame_timing(void) { return g_extended_frames; }
void rtl_sync_apu_to_cpu_locked(void) { ++g_absolute_syncs; }
bool sa1_cpu_irq_pending(const Sa1 *sa1) { (void)sa1; return false; }
int g_recomp_stack_top;
uint16_t g_cpu_entry_s[64];
uint8 g_memsel;
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y) {
    (void)pc; (void)a; (void)x; (void)y;
}
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

void snes_refresh_charge(void) {}
/* Per-frame NMI/IRQ tally (ppu_dma_trace.c in the real runner). */
void ppudma_note_interrupt(int is_nmi) { (void)is_nmi; }
uint32_t cpu_region_speed(uint32_t addr24) {
    return (uint32_t)snes_region_speed(addr24, g_memsel);
}
uint8_t sdd1_read(Sdd1 *sdd1, uint16_t addr) {
    (void)sdd1;
    (void)addr;
    return 0;
}

/* Attribution-scope stubs (common_cpu_infra.c in the real runner). The test
 * doubles record what the bridge pushed so the interp scope is testable: the
 * write rings copy g_last_recomp_func / the stack at write time, and if the
 * bridge stops installing its interp@$ name, interpreted writes silently
 * re-attribute to the stale enclosing AOT frame. */
const char *g_last_recomp_func = "(none)";
static const char *g_push_log[16];
static int g_push_count = 0;
static int g_interp_push_count = 0;
static int g_push_depth = 0;
static int g_pop_underflow = 0;
/* Model the real push/pop on g_recomp_stack_top too (common_cpu_infra.c
 * seeds g_cpu_entry_s[slot] = S at push). The bridge keys its "did this
 * rewritten return cross into a compiled ancestor" decision on
 * s_interp_bounce_recomp_base = g_recomp_stack_top at bounce time; with a
 * stub that never advanced the top, that whole branch was untestable and
 * S8d below could not fail. */
static CpuState g_c;
static void redirect_to_return(CpuState *cpu, uint32_t pc) {
    (void)cpu; (void)pc;
    interp_bridge_pre_opcode_redirect(0x008500);
}
void RecompStackPush(const char *name) {
    if (g_push_count < 16) g_push_log[g_push_count] = name;
    g_push_count++;
    g_push_depth++;
    if (g_recomp_stack_top < 64) {
        g_cpu_entry_s[g_recomp_stack_top] = g_c.S;
        g_recomp_stack_top++;
    }
}
void RecompStackPushInterpreter(const char *name) {
    ++g_interp_push_count;
    RecompStackPush(name);
}
void RecompStackPop(void) {
    if (g_push_depth <= 0) g_pop_underflow = 1;
    g_push_depth--;
    if (g_recomp_stack_top > 0) g_recomp_stack_top--;
}
void snes_catchupApu(Snes *snes) { (void)snes; ++g_relative_syncs; }
void snes_sync_master_clock(Snes *snes, uint64_t master_clock) {
    (void)snes; (void)master_clock;
}
void cart_sync_coprocessors(Cart *cart, uint64_t master_clock) {
    (void)cart; (void)master_clock;
}
/* A plain LoROM cart over the flat RAM: $8000-$FFFF of every non-WRAM bank is
 * ROM. The stable-poll detector asks the cart which reads are ROM. */
static Cart g_test_cart;
uint8_t *cart_getRomPtr(Cart *cart, uint8_t bank, uint16_t adr) {
    (void)cart;
    if (bank == 0x7E || bank == 0x7F || adr < 0x8000) return NULL;
    return &RAM[((uint32_t)bank << 16) | adr];
}
/* cpu_state.c isn't linked here; the bridge's constructor installs its
 * step-ring dump into this hook, so provide the slot. */
void (*g_interp_recent_dump_hook)(int n, FILE *out) = 0;
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    (void)cpu; return RAM[(((uint32)bank << 16) | addr) & 0xFFFFFF];
}
/* Write-site attribution probe (S16): what the bridge published as the
 * executing opcode's PC when this store reached the bus. */
extern uint32_t g_interp_wlog_pc24;
static uint32_t g_write_site_pc24 = 0xFFFFFFFFu;
static uint32_t g_aot_saw_site_pc24 = 0xFFFFFFFFu;
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    (void)cpu; RAM[(((uint32)bank << 16) | addr) & 0xFFFFFF] = v;
    if (bank == 0 && addr == 0x0420) g_write_site_pc24 = g_interp_wlog_pc24;
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    uint8 lo = cpu_read8(cpu, bank, addr);
    uint8 hi = cpu_read8(cpu, bank, (uint16)(addr + 1));
    return (uint16)(lo | ((uint16)hi << 8));
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    cpu_write8(cpu, bank, addr, (uint8)v);
    cpu_write8(cpu, bank, (uint16)(addr + 1), (uint8)(v >> 8));
}
void wlog_scope_enter(const char *tag) { (void)tag; }
void wlog_scope_exit(void) {}
int cpu_take_tailcall_return_context(uint16_t *entry_s, uint8_t *hrv) {
    (void)entry_s; (void)hrv; return 0;
}
void cpu_interrupt_context_enter(void) {}
void cpu_interrupt_context_leave(void) {}
int cpu_interrupt_context_active(void) { return 0; }
uint8 cpu_dispatch_inline_arg_bytes(uint32 pc24) {
    (void)pc24; return 0;
}
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) {
    (void)cpu;
    pc24 &= 0xFFFFFF;
    return pc24 == FAKE_AOT ||
           ((g_aot_double_rewrite || g_aot_crosses_interp_owner || g_aot_nested_deadline) &&
            pc24 == FAKE_AOT_2);
}
static int g_abandon_called;
static int g_post_return_skip;
int cpu_resolve_post_return_skip(uint16_t post_s) {
    (void)post_s; return g_post_return_skip;
}
RecompReturn cpu_unresolved_abandon_balanced(CpuState *cpu, uint32 site_pc24,
                                             uint16 entry_s, uint8 hrv) {
    (void)site_pc24; g_abandon_called++;
    cpu->S = (uint16)(entry_s + hrv);
    return RECOMP_RETURN_NORMAL;
}
RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24, uint16 miss_restore) {
    if ((pc24 & 0xFFFFFF) == FAKE_AOT) {
        g_aot_called++;
        g_aot_saw_site_pc24 = g_interp_wlog_pc24;
        cpu->A = (uint16)(cpu->A + 0x0100);     /* observable "compiled" work */
        cpu->S = (uint16)(cpu->S + 2);          /* models RTS popping its frame */
        return RECOMP_RETURN_NORMAL;
    }
    cpu->S = miss_restore;
    return RECOMP_RETURN_NORMAL;
}
RecompReturn cpu_dispatch_pc_paired(CpuState *cpu, uint32 pc24,
                                    uint8 frame_size) {
    cpu->host_return_valid = frame_size;
    if (g_aot_nested_deadline && pc24 == FAKE_AOT) {
        RecompReturn result = interp_tier_dispatch_balanced(
            cpu, 0x008300, FAKE_AOT, cpu->S, frame_size, false);
        if (result != RECOMP_RETURN_NORMAL) return result;
        /* This continuation must never run after the inner deadline. */
        ++g_after_nested_deadline;
        cpu->A = 0xdead;
        if (interp_bridge_lle_master_deadline_reached(cpu))
            return interp_bridge_lle_yield_unwind(cpu, 0x008104);
        return result;
    }
    if (g_aot_nested_deadline && pc24 == FAKE_AOT_2) {
        cpu->master_cycles = 200;
        if (interp_bridge_lle_master_deadline_reached(cpu))
            return interp_bridge_lle_yield_unwind(cpu, FAKE_AOT_2);
    }
    if (g_aot_skips_interp_owner && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Model interpreter outer -> interpreter inner -> compiled root ->
         * compiled child. The child manually discards the generated child and
         * root frames, then its RTS consumes the interpreted inner frame and
         * lands at the outer continuation. No generated ancestor owns that
         * PC, so the active interpreter must resume it directly. */
        g_aot_called++;
        const int base = g_recomp_stack_top;
        g_recomp_stack_top += 2;
        g_cpu_entry_s[base] = cpu->S;
        cpu->S = (uint16)(cpu->S - 2);       /* root JSRs compiled child */
        g_cpu_entry_s[base + 1] = cpu->S;
        const uint16 ret_s = (uint16)(cpu->S + 4); /* expose inner's frame */
        cpu->S = (uint16)(ret_s + 2);        /* final RTS pops inner frame */
        g_owner_target_result =
            interp_bridge_return_targets_owner(ret_s, cpu->S);
        g_recomp_stack_top = base;
        if (g_owner_target_result)
            return interp_bridge_lle_yield_unwind(cpu, 0x008003);
        return RECOMP_RETURN_NORMAL;
    }
    if (g_aot_gap_walks_into_wait && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* An unresolved dispatch inside the compiled body opens a NESTED gap
         * frame (yield_pc == 0), and the interpreted routine it lands in walks
         * into the program's cooperative wait primitive. */
        g_aot_called++;
        return interp_tier_dispatch_balanced(cpu, 0x008300, 0x008000,
                                             cpu->S, frame_size, false);
    }
    if (g_aot_deadline_unwind && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        g_aot_called++;
        cpu->master_cycles = 200;
        if (interp_bridge_lle_master_deadline_reached(cpu))
            return interp_bridge_lle_yield_unwind(cpu, 0x008100);
        cpu->A = 0x0100;
        cpu->S = (uint16)(cpu->S + frame_size);
        return RECOMP_RETURN_NORMAL;
    }
    if (g_aot_skips_root && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Model a compiled root whose nested helper consumes the root's guest
         * JSL frame and returns SKIP_1 through the root host frame. The owning
         * interpreter must consume that one skip and continue after its JSL. */
        g_aot_called++;
        cpu->S = (uint16)(cpu->S + frame_size);
        return RECOMP_RETURN_SKIP_1;
    }
    if (g_aot_double_rewrite && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Computed-RTS dispatcher shape: a synthetic handler address is
         * pushed and immediately popped, so the interpreted caller's real
         * JSR frame remains on the guest stack.  Continue at the selected
         * handler through the rewritten-return bridge. */
        g_aot_called++;
        return interp_tier_dispatch_rewritten_return(cpu, 0x008300, 0x0081FE);
    }
    if (g_aot_double_rewrite && (pc24 & 0xFFFFFF) == FAKE_AOT_2) {
        /* Later in that interpreted handler, a second AOT helper performs
         * PLA; PLA; RTS: consume its own JSR frame plus the dispatcher's
         * preserved caller frame, then resume the interpreted grandparent. */
        g_aot_called++;
        cpu->S = (uint16)(cpu->S + 4);
        return interp_tier_dispatch_rewritten_return(cpu, 0x008003, 0x0082FE);
    }
    if (g_aot_tail_chain_probe && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        const int base = g_recomp_stack_top;
        g_recomp_stack_top += 3;
        g_cpu_entry_s[base] = 0x01FA;
        g_cpu_entry_s[base + 1] = 0x01FA;
        g_cpu_entry_s[base + 2] = 0x01FA;
        g_tail_chain_direct = interp_bridge_has_direct_paired_bounce();
        g_cpu_entry_s[base + 1] = 0x01F8; /* materialized nested JSR */
        g_nested_chain_direct = interp_bridge_has_direct_paired_bounce();
        g_recomp_stack_top = base;
        cpu->S = (uint16)(cpu->S + frame_size);
        return RECOMP_RETURN_NORMAL;
    }
    if (g_aot_interp_nlr && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Model PLA; PLA; RTS in a direct AOT bounce: consume the AOT JSR
         * frame plus its interpreted caller's JSR frame, then continue in
         * the interpreted grandparent at $8003. */
        g_aot_called++;
        cpu->S = (uint16)(cpu->S + 4);
        return interp_tier_dispatch_rewritten_return(cpu, 0x008003, 0x0081FE);
    }
    if (g_aot_nested_rewrite && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Model bridge -> compiled root -> compiled parent -> rewritten-return
         * callee.  The rewritten landing is the parent's PLB/PLP/RTL epilogue;
         * interpreting it consumes only that parent's guest frame.  SKIP_1
         * then removes the matching host parent and the compiled root resumes,
         * eventually returning normally to the bridge. */
        g_aot_called++;
        g_recomp_stack_top++;                 /* paired AOT root */
        g_cpu_entry_s[g_recomp_stack_top - 1] = cpu->S;

        cpu_write8(cpu, 0, cpu->S, 0x00); cpu->S--; /* parent JSL bank */
        cpu_write8(cpu, 0, cpu->S, 0x81); cpu->S--; /* return high */
        cpu_write8(cpu, 0, cpu->S, 0x7F); cpu->S--; /* return low ($8180) */
        cpu_write8(cpu, 0, cpu->S, cpu->P); cpu->S--;  /* parent PHP */
        cpu_write8(cpu, 0, cpu->S, cpu->DB); cpu->S--; /* parent PHB */
        g_recomp_stack_top += 2;              /* parent + rewrite callee */
        g_cpu_entry_s[g_recomp_stack_top - 2] = (uint16)(cpu->S + 2);
        g_cpu_entry_s[g_recomp_stack_top - 1] = cpu->S;

        RecompReturn r = interp_tier_dispatch_rewritten_return(
            cpu, 0x008200, 0x0081FE);
        g_recomp_stack_top -= 2;
        if (r != RECOMP_RETURN_SKIP_1) {
            g_recomp_stack_top--;
            return r;
        }

        cpu->A = (uint16)(cpu->A + 0x0100);  /* root continued after parent */
        cpu->S = (uint16)(cpu->S + frame_size); /* root returns to bridge */
        g_recomp_stack_top--;
        return RECOMP_RETURN_NORMAL;
    }
    if (g_aot_crosses_interp_owner &&
        (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Compiled root calls an untranslated routine, creating a nested
         * interpreter below the root's live host frame. */
        g_aot_called++;
        g_recomp_stack_top++;
        g_cpu_entry_s[g_recomp_stack_top - 1] = cpu->S;
        cpu_write8(cpu, 0, cpu->S, 0x81); cpu->S--;
        cpu_write8(cpu, 0, cpu->S, 0x7F); cpu->S--;
        RecompReturn r = interp_tier_run_call_frame(
            cpu, 0x008400, 0x0081F0, 2, NULL);
        g_recomp_stack_top--;
        if (r == RECOMP_RETURN_SKIP_1)
            return RECOMP_RETURN_NORMAL;
        return r;
    }
    if (g_aot_crosses_interp_owner &&
        (pc24 & 0xFFFFFF) == FAKE_AOT_2) {
        /* Consume this call frame and the nested interpreter owner's frame,
         * leaving the rewritten continuation in the compiled root. */
        g_aot_called++;
        g_recomp_stack_top++;
        g_cpu_entry_s[g_recomp_stack_top - 1] = cpu->S;
        cpu->S = (uint16)(cpu->S + frame_size + 2);
        RecompReturn r = interp_tier_dispatch_rewritten_return(
            cpu, 0x008200, 0x0082FE);
        g_recomp_stack_top--;
        return r;
    }
    if (g_aot_rewrites_return && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        g_aot_called++;
        g_recomp_stack_top++;
        cpu->S = (uint16)(cpu->S + frame_size); /* rewritten RTS/RTL popped it */
        RecompReturn r = interp_tier_dispatch_rewritten_return(
            cpu, 0x008200, 0x0081FE);
        g_recomp_stack_top--;
        return r;
    }
    return cpu_dispatch_pc(cpu, pc24, cpu->S);
}

static int g_fail = 0, g_check = 0;
#define CHECK(cond, ...) do { g_check++; if (!(cond)) { \
    g_fail++; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void init_cpu(void) {
    memset(&g_c, 0, sizeof g_c);
    g_c.S = 0x01FF; g_c.emulation = 1; g_c.m_flag = 1; g_c.x_flag = 1;
    g_c._flag_I = 1; g_c.ram = RAM; cpu_mirrors_to_p(&g_c);
}
static void load(uint32 pc24, const uint8_t *code, int len) {
    memcpy(&RAM[pc24 & 0xFFFFFF], code, (size_t)len);
}

static unsigned hook_order,hook_a,hook_b,hook_c,hook_dest;
static void observe_a(CpuState *cpu,uint32_t pc) {
    (void)pc;++hook_a;hook_order=hook_order*10+1;cpu->X=0x23;
}
static void redirect_b(CpuState *cpu,uint32_t pc) {
    (void)cpu;(void)pc;++hook_b;hook_order=hook_order*10+2;
    interp_bridge_pre_opcode_redirect(0x008010);
}
static void forbidden_c(CpuState *cpu,uint32_t pc) {(void)cpu;(void)pc;++hook_c;}
/* S17: a "device" that answers on the 10th time the loop head is reached. */
static unsigned g_poll_visits;
static uint32_t g_poll_answer_addr;
static uint8_t g_poll_answer_value;
static void poll_device(CpuState *cpu, uint32_t pc) {
    (void)cpu; (void)pc;
    if (++g_poll_visits == 10)
        RAM[g_poll_answer_addr & 0xFFFFFF] = g_poll_answer_value;
}
/* Run `code` at $8000 as a cooperative-scheduler task whose RTS lands in the
 * scheduler's own wait at $8100 (LDA $20; BNE self, flag value 0). */
static int run_poll_task(const uint8_t *code, int len) {
    static const uint8_t scheduler_wait[] = {0xAD,0x20,0x00, 0xD0,0xFB};
    load(0x8000, code, len);
    load(0x8100, scheduler_wait, sizeof scheduler_wait);
    cpu_write8(&g_c, 0, g_c.S, 0x80); g_c.S--;
    cpu_write8(&g_c, 0, g_c.S, 0xFF); g_c.S--;
    return interp_bridge_run_loop(&g_c, 0x008000, 0x008100, 0x0020, 0);
}
static int resume_ring_has(uint64_t since, int site, int kind) {
    InterpResumeEvent e;
    for (uint64_t q = since; q < interp_bridge_resume_total(); q++)
        if (interp_bridge_resume_get(q, &e) && e.site == site && e.kind == kind)
            return 1;
    return 0;
}
static void observe_dest(CpuState *cpu,uint32_t pc) {(void)cpu;(void)pc;++hook_dest;}

int main(void) {
    const char *journal = "tier2_bridge_test.jsonl";
    remove(journal);
#ifdef _WIN32
    _putenv_s("SNESRECOMP_TIER2_JOURNAL", journal);
#else
    setenv("SNESRECOMP_TIER2_JOURNAL", journal, 1);
#endif
    RAM = malloc(MEMSZ);
    g_test_cart.type = CART_LOROM;
    g_test_snes.cart = &g_test_cart;

    printf("S0 APU timeline policy remains cartridge-scoped\n");
    CHECK(!interp_bridge_use_absolute_apu_timeline(false, false, false),
          "inactive non-SA1 timeline must use legacy catch-up");
    CHECK(!interp_bridge_use_absolute_apu_timeline(true, false, false),
          "active non-SA1 timeline must use legacy catch-up");
    CHECK(!interp_bridge_use_absolute_apu_timeline(false, true, false),
          "inactive SA1 timeline must use legacy catch-up");
    CHECK(interp_bridge_use_absolute_apu_timeline(true, true, false),
          "active SA1 timeline must suppress duplicate catch-up");
    CHECK(interp_bridge_use_absolute_apu_timeline(true, false, true),
          "mapped extended frames must suppress duplicate catch-up");
    CHECK(!interp_bridge_use_absolute_apu_timeline(false, false, true),
          "mapped time before the frame loop must retain bootstrap catch-up");

    /* No APU port touches: long interpreted work must periodically sync the
     * absolute clock, while unmapped boot still uses relative catch-up. */
    { Apu apu = {0};
      g_test_snes.apu = &apu;
      g_frame_timeline = true;
      for (unsigned mode = 0; mode < 3; ++mode) {
        memset(RAM, 0, MEMSZ); init_cpu();
        uint8_t c[] = {0xA2,0xFF,0xCA,0xD0,0xFD,0x60};
        load(0x8000, c, sizeof c);
        cpu_push_jsr_return_frame(&g_c);
        g_extended_frames = mode != 0;
        apu.portTimeValid = mode == 2;
        g_absolute_syncs = g_relative_syncs = 0;
        CHECK(interp_bridge_run(&g_c, 0x008000) == 1, "timed loop returns");
        if (mode == 2)
          CHECK(g_absolute_syncs > 1 && g_relative_syncs == 0,
                "mapped work syncs repeatedly without double-driving SPC");
        else
          CHECK(g_relative_syncs > 1 && g_absolute_syncs >= g_relative_syncs,
                "legacy and unmapped boot retain relative progress");
      }
      g_test_snes.apu = NULL;
      g_frame_timeline = g_extended_frames = false;
    }

    /* S1: LDA #$01 ; JSR $8100 (compiled) ; RTS */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x01, 0x20,0x00,0x81, 0x60};
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);          /* sentinel return frame */
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S1 JSR-into-compiled bounce\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK(g_c.A == 0x0101, "A=%04X exp 0101 (01 from LDA + 0100 from AOT)", g_c.A);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (balanced)", g_c.S); }

    /* S2: LDA #$09 ; RTS  (no call) */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x09, 0x60};
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);
      g_push_count = 0; g_push_depth = 0; g_pop_underflow = 0;
      g_interp_push_count = 0;
      const char *func_before = g_last_recomp_func;
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S2 pure interp routine\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 0, "aot_called=%d exp 0", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x09, "A.lo=%02X exp 09", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF", g_c.S);
      /* Attribution scope: the run pushed its interp@$ entry name, popped it
       * on exit, and restored g_last_recomp_func. */
      CHECK(g_push_count >= 1, "push_count=%d exp >=1 (interp scope pushed)",
            g_push_count);
      CHECK(g_interp_push_count == g_push_count,
            "interpreter attribution must use observer scopes");
      CHECK(g_push_count >= 1 && g_push_log[0] &&
            strcmp(g_push_log[0], "interp@$008000") == 0,
            "pushed name '%s' exp 'interp@$008000'",
            g_push_count >= 1 && g_push_log[0] ? g_push_log[0] : "(null)");
      CHECK(g_push_depth == 0, "push_depth=%d exp 0 (balanced)", g_push_depth);
      CHECK(!g_pop_underflow, "pop underflow");
      CHECK(g_last_recomp_func == func_before,
            "g_last_recomp_func not restored after run"); }

    /* S3: JSR $8200 (NOT compiled) ; RTS  /  $8200: LDA #$33 ; RTS */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t caller[] = {0x20,0x00,0x82, 0x60};
      uint8_t callee[] = {0xA9,0x33, 0x60};
      load(0x8000, caller, sizeof caller);
      load(0x8200, callee, sizeof callee);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S3 interpret-through non-compiled call\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 0, "aot_called=%d exp 0 (no compiled body)", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x33, "A.lo=%02X exp 33", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (balanced through nested RTS)", g_c.S); }

    /* S4: interp_tier_dispatch (the production tier-down entry, tail-dispatch
     * shape): a caller frame is on the stack (as after a JSR into the
     * dispatcher); the dispatched target runs and RTSes past entry. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x07, 0x60};      /* $8000: LDA #$07 ; RTS */
      load(0x8000, c, sizeof c);
      long hits0 = interp_tier_hit_count();
      cpu_push_jsr_return_frame(&g_c);       /* the (inherited) caller frame */
      RecompReturn r = interp_tier_dispatch(&g_c, 0x008000);
      printf("S4 interp_tier_dispatch (tail-dispatch entry)\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL(0)", (int)r);
      CHECK((g_c.A & 0xFF) == 0x07, "A.lo=%02X exp 07", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (caller frame consumed)", g_c.S);
      CHECK(interp_tier_hit_count() == hits0 + 1, "hit_count delta exp 1"); }

    /* S5: interp_tier_dispatch_balanced (SM abandon-site upgrade). A clean
     * routine interprets to completion -> NORMAL, balanced, abandon NOT used. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0; g_abandon_called = 0;
      /* Even if an unrelated ancestor would match this post-S, the bridge
       * must not consult it when the tail consumed exactly its own frame. */
      g_post_return_skip = 1;
      uint8_t c[] = {0xA9,0x0C, 0x60};      /* $8000: LDA #$0C ; RTS */
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);       /* inherited caller frame (hrv=2) */
      uint16 entry_s = g_c.S;                /* function entry S = after caller's push */
      RecompReturn r = interp_tier_dispatch_balanced(&g_c, 0x008000, 0x00C0DE,
                                                     entry_s, 2, false);
      printf("S5 interp_tier_dispatch_balanced (clean -> interpret, no abandon)\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL", (int)r);
      CHECK((g_c.A & 0xFF) == 0x0C, "A.lo=%02X exp 0C (interpreted)", g_c.A & 0xFF);
      CHECK(g_abandon_called == 0, "abandon_called=%d exp 0 (clean interp)", g_abandon_called);
      CHECK(g_c.S == (uint16)(entry_s + 2), "S=%04X exp %04X (frame popped)", g_c.S, (uint16)(entry_s + 2)); }

    /* S5b: a shared suffix interpreted by the balanced tail tier performs a
     * guest non-local return (PLA; PLA; RTS).  It consumes the current return
     * frame and then returns through a compiled ancestor, so the bridge must
     * propagate the resolver's SKIP_N instead of resuming that ancestor. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_abandon_called = 0;
      g_post_return_skip = 1;
      uint8_t c[] = {0x68,0x68,0x60};        /* PLA ; PLA ; RTS */
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);       /* ancestor's return frame */
      cpu_push_jsr_return_frame(&g_c);       /* current function's frame */
      uint16 entry_s = g_c.S;
      RecompReturn r = interp_tier_dispatch_balanced(&g_c, 0x008000, 0x00C0DE,
                                                     entry_s, 2, false);
      printf("S5b balanced tail propagates interpreted non-local return\n");
      CHECK(r == RECOMP_RETURN_SKIP_1, "r=%d exp SKIP_1", (int)r);
      CHECK(g_abandon_called == 0, "abandon_called=%d exp 0 (clean interp)", g_abandon_called);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (both frames consumed)", g_c.S); }

    /* S6: rewritten return enters the caller internally. The interpreter
     * consumes that caller's frame, so the bridge must propagate SKIP_1
     * instead of letting the host resume and execute its epilogue twice. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_post_return_skip = 1;
      uint8_t c[] = {0x60};                    /* internal caller PC: RTS */
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);
      RecompReturn r = interp_tier_dispatch_rewritten_return(
          &g_c, 0x008000, 0x00C0DF);
      printf("S6 rewritten return skips consumed host caller\n");
      CHECK(r == RECOMP_RETURN_SKIP_1, "r=%d exp SKIP_1", (int)r);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (caller frame consumed once)", g_c.S); }

    /* S6b: a direct AOT bounce non-locally returns through an interpreted
     * caller. The owning ordinary (non-scheduler) interpreter must resume at
     * the grandparent's real continuation and never execute the skipped
     * inner continuation. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_interp_nlr = 1;
      uint8_t outer[] = {0x20,0x00,0x82, 0xA9,0x5A, 0x60};
      uint8_t inner[] = {0x20,0x00,0x81, 0xA9,0xEE, 0x60};
      load(0x8000, outer, sizeof outer);
      load(0x8200, inner, sizeof inner);
      cpu_push_jsr_return_frame(&g_c);       /* outer host sentinel */
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S6b AOT NLR resumes owning ordinary interpreter\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (inner continuation skipped)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (all frames balanced)", g_c.S);
      g_aot_interp_nlr = 0; }

    /* S6bb: an AOT child non-locally returns through its compiled parent and
     * an interpreted inner caller, landing in the interpreted outer caller.
     * The generated-only ancestor table cannot see either interpreted frame;
     * the mixed-tier resolver must unwind the generated bounce and resume the
     * real popped continuation in the existing interpreter. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_skips_interp_owner = 1; g_owner_target_result = 0;
      uint8_t outer[] = {0x20,0x00,0x82, 0xA9,0x5A, 0x60};
      uint8_t inner[] = {0x20,0x00,0x81, 0xA9,0xEE, 0x60};
      load(0x8000, outer, sizeof outer);
      load(0x8200, inner, sizeof inner);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S6bb nested AOT NLR resumes interpreted grandparent\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK(g_owner_target_result == 1,
            "owner_target=%d exp 1", g_owner_target_result);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (inner continuation skipped)",
            g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (all frames balanced)", g_c.S);
      g_aot_skips_interp_owner = 0; }

    /* S6c: LttP's sprite dispatch shape performs two rewritten AOT returns in
     * one interpreted call chain.  The first computed RTS preserves the
     * caller frame and selects an interpreted handler; a later AOT helper
     * consumes its own frame plus that preserved frame.  Both continuations
     * belong to the same owning interpreter.  The wrapper epilogue must run
     * once, never once in a nested bridge and again in its owner. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_double_rewrite = 1;
      uint8_t outer[] = {0x20,0x00,0x81, 0xA9,0x5A, 0x60};
      uint8_t handler[] = {0x20,0x00,0x82, 0xA9,0xEE, 0x60};
      load(0x8000, outer, sizeof outer);
      load(0x8300, handler, sizeof handler);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S6c consecutive rewritten AOT returns stay in one interpreter\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 2, "aot_called=%d exp 2", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (handler continuation skipped)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (all frames balanced)", g_c.S);
      g_aot_double_rewrite = 0; }

    /* S7: an interrupt-owned stable-value poll must cooperatively yield at
     * CMP while the sampled WRAM byte is unchanged, then resume and return
     * normally after the next frame changes it. This is the canonical shape
     * used by Super Metroid's message-box setup during ship entry. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      uint8_t c[] = {
          0xAD,0x10,0x00,                    /* LDA $0010 */
          0xCD,0x10,0x00,                    /* CMP $0010 */
          0xF0,0xFB,                         /* BEQ CMP */
          0x60                               /* RTS */
      };
      load(0x8000, c, sizeof c); RAM[0x10] = 0x34;
      cpu_push_jsr_return_frame(&g_c);
      int rc1 = interp_bridge_run_loop(&g_c, 0x008000, 0x008003, 0x0020, 0xFF);
      printf("S7 interrupt-owned stable-value poll yields and resumes\n");
      CHECK(rc1 == 1, "first rc=%d exp 1 (clean cooperative yield)", rc1);
      CHECK((g_c.A & 0xFF) == 0x34, "A.lo=%02X exp 34 (sample retained)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FD, "yield S=%04X exp 01FD (caller frame retained)", g_c.S);
      CHECK(interp_bridge_lle_resume_pc() == 0x008003,
            "resume=$%06X exp $008003 (CMP)",
            (unsigned)interp_bridge_lle_resume_pc());
      RAM[0x10] = 0x35;                      /* models the next frame's NMI */
      int rc2 = interp_bridge_run_loop(&g_c, interp_bridge_lle_resume_pc(),
                                       0x008003, 0x0020, 0xFF);
      CHECK(rc2 == 1, "second rc=%d exp 1 (poll exits)", rc2);
      CHECK(g_c.S == 0x01FF, "return S=%04X exp 01FF (balanced)", g_c.S); }

    /* S7b: host depth alone must not hide interpreter ownership across a
     * pure architectural tail chain. Every tail callee inherits the paired
     * root's entry-S watermark; a real nested JSR introduces a lower one. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      g_aot_tail_chain_probe = 1;
      g_tail_chain_direct = g_nested_chain_direct = -1;
      uint8_t caller[] = {0x20,0x00,0x81, 0x60}; /* JSR fake AOT; RTS */
      load(0x8000, caller, sizeof caller);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_tail_chain_direct == 1,
            "tail-chain direct=%d exp 1", g_tail_chain_direct);
      CHECK(g_nested_chain_direct == 0,
            "nested-chain direct=%d exp 0", g_nested_chain_direct);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF", g_c.S);
      g_aot_tail_chain_probe = 0; }

    /* S8: when an AOT callee bounced from an LLE interpreter frame rewrites
     * its return address, the rewritten continuation belongs to that active
     * interpreter. It must not be run in a nested tier frame and converted to
     * SKIP_1 (there is no compiled guest parent to skip). */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_rewrites_return = 1;
      uint8_t caller[] = {0x20,0x00,0x81};       /* JSR fake AOT */
      uint8_t continuation[] = {
          0xA9,0x5A,                             /* rewritten landing: LDA #$5A */
          0xAD,0x20,0x00, 0xD0,0xFB             /* scheduler yield loop */
      };
      load(0x8000, caller, sizeof caller);
      load(0x8200, continuation, sizeof continuation);
      RAM[0x20] = 0;
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008202, 0x0020, 0);
      printf("S8 LLE bounce resumes a rewritten return in its interpreter\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (rewritten continuation executed)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (bounce frame consumed once)", g_c.S);
      g_aot_rewrites_return = 0; }

    /* S8d: the scheduler frame resumes DEEP -- inside a wait routine's
     * epilogue (WaitForNMI shape), i.e. below the S its caller runs at. The
     * caller returns to, then JSLs an AOT callee that rewrites its return
     * (inline arguments) from a SHALLOWER S than the frame's entry S. That
     * entry S is a resume point, not a compiled-ancestor boundary, so the
     * rewritten continuation must still come back to this interpreter.
     *
     * Without interp_owner_crossed()'s scheduler exemption it is classified
     * as "crossed into a compiled ancestor", run in a nested tier frame, and
     * surfaces as SKIP_1 that abandons the live frame. Super Metroid, Start
     * at the title: FileSelectMenu_0_FadeOutConfigGfx -> (JSR) WaitForNMI ->
     * LoadInitialMenuTiles -> JSL SetupDmaTransfer(+8 inline bytes), then
     * garbage into InvalidInterrupt_Crash on the next frame.
     *
     * This test exists because the fix was lost once in a rebase and nothing
     * failed (recomp-ai-rules/PRINCIPLES.md, "Enforce the Rule in the
     * Artifact"). If it starts passing with the exemption removed, the
     * harness has stopped modelling g_recomp_stack_top -- check that first. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_rewrites_return = 1;
      uint8_t wait_epilogue[] = {0x60};          /* $8300: RTS (frame entry) */
      uint8_t caller[] = {
          0x20,0x00,0x83,                        /* $8000: JSR $8300 (in progress) */
          0x22,0x00,0x81,0x00,                   /* $8003: JSL fake AOT (rewrites) */
          0xA9,0xEE                              /* $8007: must NOT execute */
      };
      uint8_t continuation[] = {
          0xA9,0x5A,                             /* $8200: rewritten landing */
          0xAD,0x20,0x00, 0xD0,0xFB             /* $8202: scheduler yield loop */
      };
      load(0x8000, caller, sizeof caller);
      load(0x8200, continuation, sizeof continuation);
      load(0x8300, wait_epilogue, sizeof wait_epilogue);
      RAM[0x20] = 0;
      /* The previous frame yielded inside $8300's callee frame: JSR $8300's
       * return address ($8002) is on the stack and S is below the caller. */
      RAM[0x1FF] = 0x80; RAM[0x1FE] = 0x02; g_c.S = 0x01FD;
      int rc = interp_bridge_run_loop(&g_c, 0x008300, 0x008202, 0x0020, 0);
      printf("S8d scheduler frame resumed below its caller keeps a rewritten return\n");
      CHECK(rc == 1, "rc=%d exp 1 (frame yields, not bail)", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (rewritten continuation ran in its owner)",
            g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (JSL frame consumed once)", g_c.S);
      CHECK(interp_bridge_lle_resume_pc() == 0x008202,
            "resume=$%06X exp $008202 (yield loop)",
            (unsigned)interp_bridge_lle_resume_pc());
      g_aot_rewrites_return = 0; }

    /* S8e: a NESTED gap frame (yield_pc == 0) that walks into the SCHEDULER's
     * wait primitive must hand the block outward, not interpret it. Only the
     * scheduler frame's contract can be satisfied here: the handshake flag is
     * cleared by the host between scheduler frames, and the host cannot run
     * while a frame below the scheduler is still on the stack. Interpreting
     * the loop here spins to the step cap, and that cap is a BAIL --
     * interp_tier_dispatch_balanced then abandons the site with its handler's
     * side effects skipped, which is silent corruption rather than a stall.
     *
     * Super Metroid, Ceres entrance, frame 2619 (deterministic from boot, no
     * input): StartGameplay_Async $80:A07B and InitAndLoadGameData_Async
     * $82:8000 each reach an unresolved dispatch that opens a nested frame;
     * inside it a bounce into the WaitForNMI HLE armed the LLE yield unwind,
     * this frame consumed it, and it resumed interpreting $80:8338 -- whose
     * loop at $80:8343 it could never satisfy. Two abandons, then ppu_read's
     * assert(0) on the state they left behind. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0; g_abandon_called = 0;
      g_aot_gap_walks_into_wait = 1;
      uint8_t scheduler[] = {
          0x22,0x00,0x81,0x00,                 /* $8000: JSL fake compiled root */
          0xA9,0x5A,                           /* $8004: must not execute */
          0xAD,0x20,0x00, 0xD0,0xFB            /* $8006: cooperative wait loop */
      };
      uint8_t gap_routine[] = {
          0xEA,                                /* $8300: NOP */
          0x4C,0x06,0x80                       /* $8301: JMP $8006 (the wait) */
      };
      load(0x8000, scheduler, sizeof scheduler);
      load(0x8300, gap_routine, sizeof gap_routine);
      RAM[0x20] = 0;
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008006, 0x0020, 0);
      printf("S8e nested gap frame hands the scheduler's wait outward\n");
      CHECK(rc == 1, "rc=%d exp 1 (frame yields, not bail)", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK(g_abandon_called == 0,
            "abandon_called=%d exp 0 (a wait is not an unresolved site)",
            g_abandon_called);
      CHECK((g_c.A & 0xFF) == 0x00,
            "A.lo=%02X exp 00 (scheduler continuation not executed)",
            g_c.A & 0xFF);
      CHECK(interp_bridge_lle_resume_pc() == 0x008006,
            "resume=$%06X exp $008006 (wait loop owns the block point)",
            (unsigned)interp_bridge_lle_resume_pc());
      g_aot_gap_walks_into_wait = 0; }

    /* S8f: a secondary counter wait inside nested fallback must unwind just
     * like the primary scheduler wait. Model SM's PHP; SEP; LDA counter;
     * CMP counter; BEQ; PLP, with both operand widths. Resume after a host
     * counter update and prove the real epilogue and caller execute once. */
    for (int wide = 0; wide < 2; ++wide) {
      memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0; g_abandon_called = 0;
      g_c.emulation = 0;
      g_aot_gap_walks_into_wait = 1;
      uint8_t scheduler[] = {
          0x22,0x00,0x81,0x00,                 /* JSL fake compiled root */
          0xA9,0x5A, 0x85,0x21,               /* observable continuation */
          0xAD,0x20,0x00, 0xD0,0xFB            /* primary wait */
      };
      uint8_t gap[] = {
          0x08, 0xE2,0x20,                    /* PHP; SEP #$20 */
          0xAD,0x10,0x00,                    /* LDA counter */
          0xCD,0x10,0x00, 0xF0,0xFB,         /* CMP counter; BEQ CMP */
          0x28, 0x6B                         /* PLP; RTL */
      };
      if (wide) gap[1] = 0xC2;                /* REP #$20 */
      load(0x8000, scheduler, sizeof scheduler);
      load(0x8300, gap, sizeof gap);
      RAM[0x10] = 0x34; RAM[0x11] = 0x12;
      const uint8_t original_p = g_c.P;
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008008, 0x20, 0);
      printf("S8f nested %d-bit counter wait preserves guest continuation\n", wide ? 16 : 8);
      CHECK(rc == 1 && !g_abandon_called, "counter wait must yield without abandon");
      CHECK(interp_bridge_lle_resume_pc() == 0x008306,
            "resume=$%06X exp $008306", (unsigned)interp_bridge_lle_resume_pc());
      CHECK(g_c.S == 0x01FB && RAM[0x21] == 0,
            "guest JSL/PHP retained, caller not yet executed: S=%04X", g_c.S);
      CHECK(RAM[0x01FC] == original_p, "PHP must retain original status");
      RAM[wide ? 0x11 : 0x10]++;              /* asynchronous counter change */
      rc = interp_bridge_run_loop(&g_c, interp_bridge_lle_resume_pc(),
                                   0x008008, 0x20, 0);
      CHECK(rc == 1 && g_c.S == 0x01FF && RAM[0x21] == 0x5A,
            "counter change must resume real PLP/RTL and caller: S=%04X", g_c.S);
      CHECK((g_c.P & 0x30) == (original_p & 0x30), "PLP restores widths");
      CHECK(g_aot_called == 1 && g_abandon_called == 0,
            "nested AOT body must not be re-entered or abandoned");
      g_aot_gap_walks_into_wait = 0;
    }

    /* S8b: an AOT root reached from the LLE scheduler can non-locally return
     * through its own compiled host frame while still landing normally in the
     * interpreted scheduler. SKIP_1 is consumed at that mixed-tier boundary;
     * abandoning the scheduler here leaves its current task permanently
     * marked running. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_skips_root = 1;
      uint8_t scheduler[] = {
          0x22,0x00,0x81,0x00,                 /* JSL fake compiled root */
          0xA9,0x5A,                           /* scheduler continuation */
          0xAD,0x20,0x00, 0xD0,0xFB            /* cooperative wait loop */
      };
      load(0x8000, scheduler, sizeof scheduler);
      RAM[0x20] = 0;
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008006, 0x0020, 0);
      printf("S8b scheduler consumes AOT-root SKIP_1\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (scheduler continuation executed)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (JSL frame consumed once)", g_c.S);
      g_aot_skips_root = 0; }

    /* S8c: a compiled callee that reaches the host's master deadline while
     * bounced from scheduler mode must return to the host. It must not be
     * treated like a cooperative yield primitive, because that would continue
     * interpreting past the host's expired bound. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_deadline_unwind = 1;
      interp_bridge_set_master_deadline(100);
      uint8_t scheduler[] = {
          0x22,0x00,0x81,0x00,                 /* JSL fake compiled root */
          0xA9,0x5A,                           /* must not execute */
          0xAD,0x20,0x00, 0xD0,0xFB            /* cooperative wait loop */
      };
      load(0x8000, scheduler, sizeof scheduler);
      RAM[0x20] = 0;
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008006, 0x0020, 0);
      printf("S8c scheduler deadline unwind returns to host\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x00,
            "A.lo=%02X exp 00 (scheduler continuation not executed)",
            g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FC,
            "S=%04X exp 01FC (compiled JSL frame retained)", g_c.S);
      CHECK(interp_bridge_lle_resume_pc() == 0x008100,
            "resume=$%06X exp $008100 (compiled deadline resume)",
            (unsigned)interp_bridge_lle_resume_pc());
      interp_bridge_set_master_deadline(0);
      g_aot_deadline_unwind = 0; }

    /* A deadline inside scheduler -> AOT -> gap -> AOT belongs to the host,
     * not the nested gap's immediate bounce owner. Retain PHP/JSL and resume
     * the exact inner PC before any outer compiled continuation executes. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      g_aot_nested_deadline = 1; g_after_nested_deadline = 0;
      uint8_t scheduler[] = {0x22,0x00,0x81,0x00,0xa9,0x5a,0xad,0x20,0x00,0xd0,0xfb};
      uint8_t nested[] = {0x08,0x22,0x00,0x82,0x00,0x28,0x6b};
      load(0x8000,scheduler,sizeof scheduler);load(0x8300,nested,sizeof nested);
      RAM[FAKE_AOT_2]=0x6b;
      interp_bridge_set_master_deadline(100);
      int rc=interp_bridge_run_loop(&g_c,0x8000,0x8006,0x20,0);
      uint32_t resume=interp_bridge_lle_resume_pc();
      printf("S8g nested deadline preserves inner PC and guest stack\n");
      CHECK(rc==1 && resume==FAKE_AOT_2,"rc=%d resume=%06X exp 1/008200",rc,resume);
      CHECK(g_after_nested_deadline==0 && g_c.A==0,"outer continuation ran after deadline");
      CHECK(g_c.S==0x1f8,"S=%04X exp 01F8 (both JSLs and PHP retained)",g_c.S);
      interp_bridge_set_master_deadline(0);g_aot_nested_deadline=0;
      if(resume==FAKE_AOT_2) {
        rc=interp_bridge_run_loop(&g_c,resume,0x8006,0x20,0);
        CHECK(rc==1 && g_c.S==0x1ff && (g_c.A&255)==0x5a,
              "resumed RTL/PLP/RTL must restore stack and reach caller");
      }
    }

    /* S9: the same rewrite below the paired AOT root belongs to a compiled
     * ancestor, not directly to the scheduler interpreter.  Finish that
     * ancestor's epilogue in the nested tier, propagate SKIP_1 through its
     * host frame, then let the root return normally to the bridge. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_nested_rewrite = 1;
      uint8_t caller[] = {
          0x22,0x00,0x81,0x00,                 /* JSL fake compiled root */
          0xAD,0x20,0x00, 0xD0,0xFB            /* scheduler yield loop */
      };
      uint8_t parent_epilogue[] = {0xAB,0x28,0x6B}; /* PLB; PLP; RTL */
      load(0x8000, caller, sizeof caller);
      load(0x8200, parent_epilogue, sizeof parent_epilogue);
      RAM[0x20] = 0;
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008004, 0x0020, 0);
      printf("S9 nested AOT rewritten return resumes compiled ancestor\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK(g_c.A == 0x0100,
            "A=%04X exp 0100 (compiled root resumed after parent)", g_c.A);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (all guest frames balanced)", g_c.S);
      CHECK(g_recomp_stack_top == 0, "recomp depth=%d exp 0", g_recomp_stack_top);
      g_aot_nested_rewrite = 0; }

    /* S9b: a direct nested bounce can rewrite past the interpreter owner's
     * stack boundary into a compiled ancestor. That ancestor epilogue must
     * execute in a nested tier and propagate SKIP_1, rather than being resumed
     * by both the interpreter and its compiled host frame. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_crosses_interp_owner = 1;
      g_post_return_skip = 1;
      uint8_t caller[] = {0x20,0x00,0x81, 0xA9,0x5A, 0x60};
      uint8_t ancestor_epilogue[] = {0x60};
      uint8_t nested[] = {0x20,0x00,0x82, 0xA9,0xEE, 0x60};
      load(0x8000, caller, sizeof caller);
      load(0x8200, ancestor_epilogue, sizeof ancestor_epilogue);
      load(0x8400, nested, sizeof nested);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S9b rewritten return crossing interpreter owner skips ancestor\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 2, "aot_called=%d exp 2", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (compiled root consumed once)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (ancestor consumed once)", g_c.S);
      CHECK(g_recomp_stack_top == 0, "recomp depth=%d exp 0", g_recomp_stack_top);
      g_aot_crosses_interp_owner = 0; }

    /* S10: a synchronous message box waits for fresh automatic-joypad data
     * by polling $4218/$4219. With no input it must yield to the host and
     * resume at the start of the hardware poll; once a button appears, it
     * exits through the original RTS with the caller frame balanced. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      uint8_t c[] = {
          0xAD,0x12,0x42,                    /* LDA $4212 */
          0x89,0x01,                         /* BIT #$01 */
          0xD0,0xF9,                         /* BNE start */
          0xAD,0x18,0x42,                    /* LDA $4218 */
          0xD0,0x05,                         /* BNE done */
          0xAD,0x19,0x42,                    /* LDA $4219 */
          0xF0,0xEF,                         /* BEQ start */
          0x60                               /* done: RTS */
      };
      uint8_t scheduler_wait[] = {
          0xAD,0x20,0x00, 0xD0,0xFB          /* LDA $20; BNE self */
      };
      load(0x8000, c, sizeof c);
      load(0x8100, scheduler_wait, sizeof scheduler_wait);
      RAM[0x4212] = 0; RAM[0x4218] = 0; RAM[0x4219] = 0;
      /* Real JSR frame returning to the scheduler wait at $8100. */
      cpu_write8(&g_c, 0, g_c.S, 0x80); g_c.S--;
      cpu_write8(&g_c, 0, g_c.S, 0xFF); g_c.S--;
      int rc1 = interp_bridge_run_loop(&g_c, 0x008000,
                                       0x008100, 0x0020, 0);
      printf("S10 automatic-joypad wait yields and resumes\n");
      CHECK(rc1 == 1, "first rc=%d exp 1 (input wait yielded)", rc1);
      CHECK(g_c.S == 0x01FD, "yield S=%04X exp 01FD (caller retained)", g_c.S);
      CHECK(interp_bridge_lle_resume_pc() == 0x008000,
            "resume=$%06X exp $008000 (re-read joypad)",
            (unsigned)interp_bridge_lle_resume_pc());
      RAM[0x4218] = 0x80;                    /* host supplies a button */
      int rc2 = interp_bridge_run_loop(&g_c, interp_bridge_lle_resume_pc(),
                                       0x008100, 0x0020, 0);
      CHECK(rc2 == 1, "second rc=%d exp 1 (input observed)", rc2);
      CHECK(g_c.S == 0x01FF, "return S=%04X exp 01FF (balanced)", g_c.S); }

    /* S11: a dispatch-table row with no exact AOT M/X body is a known LLE
     * entry, not a mid-caller continuation.  cpu_dispatch_pc_from invokes
     * this bridge after the prior RTS already popped, so the current S is the
     * target's unwind watermark and the inherited return frame is consumed. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      uint8_t c[] = {0xA9,0x44, 0x60};       /* LDA #$44 ; RTS */
      load(0x8300, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);       /* inherited target return frame */
      RecompReturn r = interp_tier_dispatch_popped_return(
          &g_c, 0x008300, 0x0082FE, 0x01FF);
      printf("S11 known non-AOT dispatch row executes exact LLE\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL", (int)r);
      CHECK((g_c.A & 0xFF) == 0x44, "A.lo=%02X exp 44", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (inherited frame consumed)", g_c.S); }

    /* S12: the production feedback set must grow beyond the historical 4096
     * ceiling without dropping tuples. Kind is part of the tuple key, too. */
    { int before = 0, after = 0;
      interp_tier2_stats(&before, NULL, NULL);
      for (unsigned i = 0; i < 4352; ++i)
          Tier2CoverageTestRecord(0xC00000u + i, 0xC10000u + i,
                                  (uint8_t)(i & 3), 3, 1);
      Tier2CoverageTestRecord(0xD00000u, 0xD10000u, 3, 3, 1);
      Tier2CoverageTestRecord(0xD00000u, 0xD10000u, 3, 4, 1);
      interp_tier2_stats(&after, NULL, NULL);
      printf("S12 growable, kind-exact coverage set\n");
      CHECK(after == before + 4354, "sites=%d exp %d", after, before + 4354); }

    /* S13: a runtime-pointer JSR falls back to the interpreter at a state
     * handler whose 16-bit PLA consumes that inner JSR frame, then RTL
     * consumes the compiled caller's outer JSL frame. This is a clean guest
     * non-local return, not a balanced call return, so propagate at least
     * SKIP_1 even when the synthetic resolver has no matching ancestor. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_post_return_skip = 0;
      g_c.emulation = 0; g_c.m_flag = 0; g_c.x_flag = 0;
      cpu_mirrors_to_p(&g_c);
      uint8_t c[] = {0x68,0x6B};              /* PLA (16-bit) ; RTL */
      load(0x8400, c, sizeof c);
      cpu_push_jsl_return_frame(&g_c);        /* compiled caller's outer frame */
      cpu_push_jsr_return_frame(&g_c);        /* runtime dispatch's call frame */
      RecompReturn r = interp_tier_run_call_frame(
          &g_c, 0x008400, 0x0083FC, 2, NULL);
      printf("S13 runtime call propagates interpreted PLA; RTL NLR\n");
      CHECK(r == RECOMP_RETURN_SKIP_1, "r=%d exp SKIP_1", (int)r);
      CHECK(g_c.S == 0x01FF,
            "S=%04X exp 01FF (inner JSR and outer JSL consumed)", g_c.S); }

    /* S14: redirecting an ordinary instruction or a call directly to RTS
     * must decode RTS anew and consume exactly the inherited return frame. */
    for (unsigned i = 0; i < 2; ++i) {
      memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {i ? 0x20 : 0xA9, 0x00, 0x81, 0x60};
      load(0x8400, c, sizeof c); RAM[0x8500] = 0x60;
      cpu_push_jsr_return_frame(&g_c);
      interp_bridge_set_pre_opcode_hook(0x008400, redirect_to_return);
      int rc = interp_bridge_run(&g_c, 0x008400);
      interp_bridge_set_pre_opcode_hook(0, NULL);
      printf("S14 hook redirects %s to terminal RTS\n", i ? "JSR" : "LDA");
      CHECK(rc == 1 && g_c.S == 0x01FF, "rc=%d S=%04X", rc, g_c.S);
      CHECK(g_aot_called == 0, "abandoned call dispatched %d times", g_aot_called);
    }
    /* Explicit capture semantics survive a generated low-bank mirror and
     * distinguish an ordinary continuation whose entry equals its site. */
    for (unsigned tail = 0; tail < 2; ++tail) {
      memset(RAM, 0, MEMSZ); init_cpu(); g_post_return_skip = 0;
      g_c.emulation = 0; g_c.PB = 0xA6; cpu_mirrors_to_p(&g_c);
      uint8_t jump[] = {0xDC,0x00,0x10}; /* JML [$1000] */
      uint8_t body[] = {0xA9,0x07,0x60}; /* LDA #7 ; RTS */
      load(0xA69000, jump, sizeof jump); load(0xA69100, body, sizeof body);
      RAM[0x1000] = 0x00; RAM[0x1001] = 0x91; RAM[0x1002] = 0xA6;
      cpu_push_jsr_return_frame(&g_c);
      RecompReturn r = tail
          ? interp_tier_dispatch_tail_ex(&g_c, 0xA69000, 0x269000, g_c.S, 2, true)
          : interp_tier_dispatch_balanced(&g_c, 0xA69000, 0x269000, g_c.S, 2, true);
      CHECK(r == RECOMP_RETURN_NORMAL && g_c.S == 0x01FF && g_c.A == 7,
            "mirrored indirect tail=%u r=%d S=%04X A=%04X", tail, r, g_c.S, g_c.A);
    }
    { memset(RAM, 0, MEMSZ); init_cpu(); g_post_return_skip = 0;
      g_c.emulation = 0; g_c.PB = 0xA6; cpu_mirrors_to_p(&g_c);
      uint8_t body[] = {0xA9,0x07,0x60};
      load(0xA69200, body, sizeof body); cpu_push_jsr_return_frame(&g_c);
      RecompReturn r = interp_tier_dispatch_balanced(
          &g_c, 0xA69200, 0xA69200, g_c.S, 2, false);
      CHECK(r == RECOMP_RETURN_NORMAL && g_c.S == 0x01FF && g_c.A == 7,
            "ordinary continuation r=%d S=%04X A=%04X", r, g_c.S, g_c.A);
    }
    tier2_capture_flush();
    { FILE *f = fopen(journal, "rb"); char line[8192];
      int jump = 0, continuation = 0, false_target = 0;
      while (f && fgets(line, sizeof line, f)) {
        if (strstr(line, "\"site_pc24\":\"0xA69000\"") &&
            strstr(line, "\"target_pc24\":\"0xA69100\"") &&
            strstr(line, "\"site_kind\":\"indirect_goto\"") &&
            strstr(line, "\"completed_hits\":1")) jump = 1;
        if (strstr(line, "\"site_pc24\":\"0xA69200\"") &&
            strstr(line, "\"target_pc24\":\"0xA69200\"") &&
            strstr(line, "\"site_kind\":\"indirect_dispatch\"") &&
            strstr(line, "\"completed_hits\":1")) continuation = 1;
        if (strstr(line, "\"target_pc24\":\"0xA69202\"")) false_target = 1;
      }
      if (f) fclose(f);
      CHECK(jump, "journal must retain live jump PC and resolved destination");
      CHECK(continuation, "journal must retain ordinary continuation entry");
      CHECK(!false_target, "first instruction fall-through is not a discovered entry");
    }
    /* S15: composition is ordered/idempotent; redirection stops old-PC hooks
     * and classifies the destination opcode, including its return boundary. */
    { memset(RAM,0,MEMSZ);init_cpu();g_aot_called=0;
      interp_bridge_set_pre_opcode_hook(0,NULL);
      RAM[0x8000]=0xea;                    /* old PC is NOP */
      uint8_t c[]={0x20,0x00,0x81,0xa9,0x55,0x60}; /* destination JSR AOT; LDA #55; RTS */
      load(0x8010,c,sizeof(c));
      CHECK(interp_bridge_add_pre_opcode_hook(0x008000,observe_a),"first observer registered");
      CHECK(interp_bridge_add_pre_opcode_hook(0x808000,observe_a),"same observer/mirrored PC idempotent");
      CHECK(interp_bridge_add_pre_opcode_hook(0x008000,redirect_b),"second observer composed");
      CHECK(interp_bridge_add_pre_opcode_hook(0x008000,forbidden_c),"third observer registered");
      CHECK(interp_bridge_add_pre_opcode_hook(0x008010,observe_dest),"destination observer registered");
      cpu_push_jsr_return_frame(&g_c);
      int rc=interp_bridge_run(&g_c,0x008000);
      CHECK(rc==1 && g_c.S==0x01ff,"redirected routine returns balanced");
      CHECK(hook_order==12 && hook_a==1 && hook_b==1 && hook_c==0,"ordered composition and terminal redirect");
      CHECK(hook_dest==1 && g_aot_called==1,"destination hooks and JSR classification run");
      CHECK((g_c.A&0xff)==0x55 && g_c.X==0x23,"hook changes survive execution");
      init_cpu();RAM[0x8010]=0x60;          /* NOP -> RTS must recognize immediate return */
      cpu_push_jsr_return_frame(&g_c);
      rc=interp_bridge_run(&g_c,0x008000);
      CHECK(rc==1 && g_c.S==0x01ff,"redirected RTS is classified as return");
      interp_bridge_set_pre_opcode_hook(0,NULL);
    }
    /* S16: the always-on observability rings. One run of
     *   $8400 LDX #$05 / loop: DEX / BNE loop / STA $0420 /
     *         JSR $8200 (interpreted) / JSR $8100 (compiled) / RTS
     *   $8200 LDA #$33 / RTS
     * must leave exactly five control-transfer records -- the four taken
     * BNEs fold into ONE record, which is what keeps a spin's path in
     * readable -- and attribute the store to its exact instruction while the
     * compiled body sees no interpreter PC at all. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA2,0x05, 0xCA, 0xD0,0xFD, 0x8D,0x20,0x04,
                     0x20,0x00,0x82, 0x20,0x00,0x81, 0x60};
      uint8_t callee[] = {0xA9,0x33, 0x60};
      load(0x8400, c, sizeof c);
      load(0x8200, callee, sizeof callee);
      cpu_push_jsr_return_frame(&g_c);
      g_write_site_pc24 = g_aot_saw_site_pc24 = 0xFFFFFFFFu;
      const uint64_t e0 = interp_bridge_edge_total();
      int rc = interp_bridge_run(&g_c, 0x008400);
      printf("S16 edge ring folds a loop; write site and resume writes recorded\n");
      CHECK(rc == 1 && g_c.S == 0x01FF, "rc=%d S=%04X exp balanced return", rc, g_c.S);
      const uint64_t e1 = interp_bridge_edge_total();
      CHECK(e1 - e0 == 5, "edges recorded=%llu exp 5",
            (unsigned long long)(e1 - e0));
      static const struct { uint32_t from, to; int kind; uint32_t count; } want[5] = {
          {0x000000, 0x008400, INTERP_EDGE_ENTRY,    1},
          {0x008403, 0x008402, INTERP_EDGE_BRANCH,   4},
          {0x008408, 0x008200, INTERP_EDGE_CALL,     1},
          {0x008202, 0x00840B, INTERP_EDGE_RETURN,   1},
          {0x00840B, 0x008100, INTERP_EDGE_AOT_CALL, 1},
      };
      for (int i = 0; i < 5 && e1 - e0 == 5; i++) {
        InterpEdge e;
        CHECK(interp_bridge_edge_get(e0 + (uint64_t)i, &e), "edge %d readable", i);
        CHECK(e.from_pc24 == want[i].from && e.to_pc24 == want[i].to &&
              e.kind == want[i].kind && e.count == want[i].count,
              "edge %d = $%06X->$%06X %s x%u, exp $%06X->$%06X %s x%u", i,
              (unsigned)e.from_pc24, (unsigned)e.to_pc24,
              interp_bridge_edge_kind_name(e.kind), (unsigned)e.count,
              (unsigned)want[i].from, (unsigned)want[i].to,
              interp_bridge_edge_kind_name(want[i].kind),
              (unsigned)want[i].count);
      }
      InterpEdge gone;
      CHECK(!interp_bridge_edge_get(e1, &gone), "a not-yet-written record is refused");
      CHECK(g_write_site_pc24 == 0x008405,
            "store attributed to $%06X exp $008405", (unsigned)g_write_site_pc24);
      CHECK(g_aot_saw_site_pc24 == 0,
            "compiled body saw interp PC $%06X exp 0", (unsigned)g_aot_saw_site_pc24);
      CHECK(g_interp_wlog_pc24 == 0,
            "interp PC $%06X still published after the bridge returned",
            (unsigned)g_interp_wlog_pc24);

      const uint64_t r0 = interp_bridge_resume_total();
      const uint32_t before = interp_bridge_lle_resume_pc();
      interp_bridge_set_lle_resume_pc(0x00ABCD);
      InterpResumeEvent re;
      memset(&re, 0, sizeof re);
      CHECK(interp_bridge_resume_total() == r0 + 1 &&
            interp_bridge_resume_get(r0, &re) &&
            re.site == INTERP_RESUME_SITE_EXTERNAL &&
            re.kind == INTERP_RESUME_KIND_SET &&
            re.old_pc24 == (before & 0xFFFFFFu) && re.new_pc24 == 0x00ABCD,
            "external resume write recorded old->new with its site");
      CHECK(!strcmp(interp_bridge_resume_site_name(re.site), "external"),
            "site name '%s'", interp_bridge_resume_site_name(re.site));
      interp_bridge_set_lle_resume_pc(before);
    }
    /* S17: the stable-poll detector is defined by state, not by opcodes.
     * A loop that returns to its head with identical registers, having
     * written nothing and read only WRAM/ROM, can only be released by an
     * interrupt or DMA: it must yield (or, nested, hand outward) at that head
     * whatever its instructions are. A loop that makes progress, writes, or
     * reads a device must keep running. */
    for (int wide = 0; wide < 2; ++wide) {
      /* (a) nested frame, a shape no byte matcher knew: LDA/AND/BNE */
      memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0; g_abandon_called = 0;
      g_c.emulation = 0;
      g_aot_gap_walks_into_wait = 1;
      uint8_t scheduler[] = {
          0x22,0x00,0x81,0x00,                 /* JSL fake compiled root */
          0xA9,0x5A, 0x85,0x21,                /* observable continuation */
          0xAD,0x20,0x00, 0xD0,0xFB            /* primary wait */
      };
      uint8_t gap[] = {
          0x08, 0xE2,0x20,                     /* PHP; SEP #$20 */
          0xAD,0x10,0x00,                      /* loop: LDA flag */
          0x29,0x01,                           /*       AND #$01 */
          0xD0,0xF9,                           /*       BNE loop */
          0x28, 0x6B                           /* PLP; RTL */
      };
      if (wide) gap[1] = 0xC2;                 /* REP #$20: AND #$0001 */
      uint8_t gap16[] = {
          0x08, 0xC2,0x20, 0xAD,0x10,0x00, 0x29,0x01,0x00, 0xD0,0xF8,
          0x28, 0x6B
      };
      load(0x8000, scheduler, sizeof scheduler);
      if (wide) load(0x8300, gap16, sizeof gap16);
      else load(0x8300, gap, sizeof gap);
      RAM[0x10] = 0x01;
      const uint64_t r0 = interp_bridge_resume_total();
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008008, 0x20, 0);
      printf("S17a nested %d-bit LDA/AND/BNE wait hands outward\n", wide ? 16 : 8);
      CHECK(rc == 1 && !g_abandon_called, "nested wait must yield without abandon");
      CHECK(interp_bridge_lle_resume_pc() == 0x008303,
            "resume=$%06X exp $008303 (loop head)",
            (unsigned)interp_bridge_lle_resume_pc());
      CHECK(resume_ring_has(r0, INTERP_RESUME_SITE_STABLE_POLL,
                            INTERP_RESUME_KIND_UNWIND_ARM),
            "nested frame must hand the wait outward (stable_poll unwind_arm)");
      CHECK(g_c.S == 0x01FB && RAM[0x21] == 0, "frames retained, caller not run");
      RAM[0x10] = 0x00;                        /* interrupt clears the flag */
      rc = interp_bridge_run_loop(&g_c, interp_bridge_lle_resume_pc(),
                                  0x008008, 0x20, 0);
      CHECK(rc == 1 && g_c.S == 0x01FF && RAM[0x21] == 0x5A,
            "release resumes PLP/RTL and the caller once: S=%04X", g_c.S);
      CHECK(g_aot_called == 1 && g_abandon_called == 0,
            "nested body not re-entered or abandoned");
      g_aot_gap_walks_into_wait = 0;
    }
    /* (b) scheduler frame, long-indexed WRAM read: LDX; LDA long,X; BMI */
    { memset(RAM, 0, MEMSZ); init_cpu();
      const uint8_t c[] = {0xA2,0x02, 0xBF,0x0E,0x00,0x7E, 0x30,0xFA, 0x60};
      RAM[0x7E0010] = 0x80;
      int rc = run_poll_task(c, sizeof c);
      printf("S17b long-indexed WRAM wait yields at its head\n");
      CHECK(rc == 1 && interp_bridge_lle_resume_pc() == 0x008002,
            "rc=%d resume=$%06X exp $008002", rc,
            (unsigned)interp_bridge_lle_resume_pc());
      RAM[0x7E0010] = 0x00;
      rc = interp_bridge_run_loop(&g_c, interp_bridge_lle_resume_pc(),
                                  0x008100, 0x0020, 0);
      CHECK(rc == 1 && interp_bridge_lle_resume_pc() == 0x008100,
            "released loop must RTS to the scheduler wait: resume=$%06X",
            (unsigned)interp_bridge_lle_resume_pc()); }
    /* (c) negatives: each loop is released by the "device" on visit 10 and
     * must still be running then -- never parked early. */
    { static const struct { const char *what; uint8_t code[12]; int len;
                            uint32_t head, addr; uint8_t value; } neg[3] = {
        {"MMIO read ($4212)", {0xAD,0x12,0x42, 0x10,0xFB, 0x60}, 6,
         0x008000, 0x004212, 0x80},
        {"loop that writes", {0xAD,0x10,0x00, 0x8D,0x11,0x00, 0x10,0xF8, 0x60}, 9,
         0x008000, 0x000010, 0x80},
        {"loop that counts", {0xE8, 0xAD,0x10,0x00, 0x10,0xFA, 0x60}, 7,
         0x008000, 0x000010, 0x80},
      };
      for (int k = 0; k < 3; k++) {
        memset(RAM, 0, MEMSZ); init_cpu();
        g_poll_visits = 0;
        g_poll_answer_addr = neg[k].addr;
        g_poll_answer_value = neg[k].value;
        interp_bridge_set_pre_opcode_hook(neg[k].head, poll_device);
        int rc = run_poll_task(neg[k].code, neg[k].len);
        interp_bridge_set_pre_opcode_hook(0, NULL);
        printf("S17c %s is not a stable poll\n", neg[k].what);
        CHECK(rc == 1 && g_poll_visits == 10 &&
              interp_bridge_lle_resume_pc() == 0x008100,
              "rc=%d visits=%u resume=$%06X exp 10 visits then scheduler wait",
              rc, g_poll_visits, (unsigned)interp_bridge_lle_resume_pc());
      }
    }
    printf("\n==== interp_bridge Phase-1: %d/%d checks passed ====\n", g_check - g_fail, g_check);
    if (g_fail) { printf("RESULT: FAIL (%d)\n", g_fail); return 1; }
    tier2_capture_close();
    remove(journal);
    printf("RESULT: PASS\n");
    return 0;
}
