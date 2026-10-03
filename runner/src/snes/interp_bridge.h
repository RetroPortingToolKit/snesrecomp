/*
 * interp_bridge — the interp816 <-> AOT bridge (interpreter-fallback tier).
 *
 * Entered at a trap site (Phase 1b: dispatch_oob / bank-miss; the spike uses
 * a synthetic site) with a known guest PC and the live CpuState. Runs the
 * LakeSnes-derived interpreter (interp816) over guest code, SHARING the
 * caller's register state and memory:
 *   - memory goes through the AOT cpu_read8 / cpu_write8 HLE bus (one map);
 *   - register/flag state is synced CpuState <-> Interp816 at every crossing.
 *
 * When the interpreted code calls (JSR/JSL) into a guest address that has a
 * compiled body (a g_dispatch_table entry for the current (m,x)), the call is
 * BOUNCED through cpu_dispatch_pc so compiled code keeps running compiled;
 * the interpreter resumes at the return address. Honors the Option-1 cpu->S
 * return-frame model (see cpu_state.h / docs/MULTI_TIER.md).
 *
 * The bridge exits when the interpreted routine returns past its entry stack
 * depth (an RTS/RTL that leaves cpu->S above the value at entry). The caller
 * must have a return frame (or sentinel) on cpu->S so that final RTS has
 * something to pop — exactly as the Option-1 model already arranges for
 * dispatched entries.
 *
 * Anti-RECURSION_BUG contract (docs/MULTI_TIER.md §6): re-entry is bounded by
 * guest call depth (bounces RETURN, never stack a permanent interp context),
 * exit asserts a balanced stack, and there is no host recursion unmatched by
 * a guest return.
 */
#ifndef INTERP_BRIDGE_H
#define INTERP_BRIDGE_H

#include <stdint.h>
#include <stdio.h>
/* Dump the last n entries of the always-on global interp step ring
 * (pc/op/sp/frame per interpreted opcode) to `out` (NULL = stderr). */
void interp_bridge_dump_recent_steps(int n, FILE *out);

/* ── Always-on interpreter control-flow edge ring ───────────────────────
 * One record per control transfer the interpreter takes. A transfer that
 * repeats one of the last few records folds into that record's count, so a
 * spin costs a handful of records however long it runs and the path that
 * reached it stays readable afterwards. An arrival at a PC is an edge whose
 * to_pc24 is that PC: query the ring instead of arming a catch. */
enum {
    INTERP_EDGE_BRANCH = 1,  /* taken Bcc / BRA / BRL                     */
    INTERP_EDGE_JUMP,        /* JMP / JML                                 */
    INTERP_EDGE_CALL,        /* JSR / JSL executed by the interpreter     */
    INTERP_EDGE_RETURN,      /* RTS / RTL / RTI                           */
    INTERP_EDGE_VECTOR,      /* BRK / COP took its vector                 */
    INTERP_EDGE_EXTERNAL,    /* PC moved by something other than the last
                                opcode: interrupt delivery, a yield resume,
                                an unwind landing                          */
    INTERP_EDGE_AOT_CALL,    /* call bounced to a compiled body           */
    INTERP_EDGE_ENTRY,       /* compiled code / host entered the bridge   */
    INTERP_EDGE_KIND_COUNT
};
typedef struct InterpEdge {
    uint32_t from_pc24;   /* last instruction before the transfer (0: ENTRY) */
    uint32_t to_pc24;
    int32_t  first_frame;
    int32_t  last_frame;
    uint32_t count;       /* times taken while folded into this record */
    uint16_t sp;          /* guest S on arrival (first time) */
    uint8_t  kind;        /* INTERP_EDGE_* */
    uint8_t  op;          /* opcode at from_pc24 */
} InterpEdge;
uint64_t interp_bridge_edge_total(void);     /* records ever appended */
int interp_bridge_edge_capacity(void);
/* Copy record `seq` (0 = oldest ever appended); 0 if evicted / not yet. */
int interp_bridge_edge_get(uint64_t seq, InterpEdge *out);
const char *interp_bridge_edge_kind_name(int kind);
void interp_bridge_dump_recent_edges(int n, FILE *out);

/* ── Always-on LLE resume-PC ring ───────────────────────────────────────
 * Every write of the scheduler resume PC and of the pending yield-unwind PC,
 * old -> new, with the named site that made it. */
enum {
    INTERP_RESUME_SITE_IRQ_PENDING,     /* auto-quiescent: IRQ line pending   */
    INTERP_RESUME_SITE_DEADLINE,        /* auto-quiescent: master deadline    */
    INTERP_RESUME_SITE_D9_IRQ,          /* $C0:84B2 wait specialization       */
    INTERP_RESUME_SITE_D9_DEADLINE,
    INTERP_RESUME_SITE_QUIESCENT,       /* stable CPU/memory state cycle      */
    INTERP_RESUME_SITE_STABLE_POLL,     /* stable-state poll (interrupt wait) */
    INTERP_RESUME_SITE_JOYPAD_WAIT,     /* auto-joypad $4212 wait             */
    INTERP_RESUME_SITE_NESTED_HANDOFF,  /* nested frame on scheduler's wait   */
    INTERP_RESUME_SITE_YIELD_FLAG,      /* scheduler yield flag matched       */
    INTERP_RESUME_SITE_WAI,
    INTERP_RESUME_SITE_DEADLINE_UNWIND, /* scheduler publishes deadline unwind */
    INTERP_RESUME_SITE_YIELD_UNWIND,    /* bounce owner consumes yield unwind */
    INTERP_RESUME_SITE_YIELD_PRIMITIVE, /* compiled yield primitive's sentinel */
    INTERP_RESUME_SITE_STEP_CAP,        /* step-cap bail                      */
    INTERP_RESUME_SITE_EXTERNAL,        /* interp_bridge_set_lle_resume_pc    */
    INTERP_RESUME_SITE_ROLLBACK,        /* rollback state load                */
    INTERP_RESUME_SITE_COUNT
};
enum {
    INTERP_RESUME_KIND_SET,             /* s_lle_resume_pc24 written          */
    INTERP_RESUME_KIND_UNWIND_ARM,      /* unwind armed for an outer frame    */
    INTERP_RESUME_KIND_UNWIND_CONSUME,  /* unwind landed: interp resumes there */
    INTERP_RESUME_KIND_RESTORE,         /* rollback restored the resume PC    */
};
typedef struct InterpResumeEvent {
    uint32_t old_pc24;
    uint32_t new_pc24;
    int32_t  frame;
    uint16_t sp;
    uint8_t  site;          /* INTERP_RESUME_SITE_* */
    uint8_t  kind;          /* INTERP_RESUME_KIND_* */
    int8_t   bridge_depth;  /* interp bridge nesting at the write */
    int8_t   sched_depth;   /* LLE scheduler nesting at the write */
    uint16_t pad;
} InterpResumeEvent;
uint64_t interp_bridge_resume_total(void);
int interp_bridge_resume_capacity(void);
int interp_bridge_resume_get(uint64_t seq, InterpResumeEvent *out);
const char *interp_bridge_resume_site_name(int site);
const char *interp_bridge_resume_kind_name(int kind);
void interp_bridge_dump_resume_ring(int n, FILE *out);

/* Steps, edges and resume writes together: what a halt or trap prints. */
void interp_bridge_dump_recent(int n, FILE *out);
#include "cpu_state.h"

/* Launch-time main-scheduler AOT policy: -1 default/environment, 0 floor,
 * 1 accelerated. Native interrupt helpers retain their ordinary policy. */
void interp_bridge_set_scheduler_aot_policy(int enabled);

/* Once port time is mapped, the frame timeline and interpreter catch-up
 * describe the same elapsed time. Extended-frame hosts use absolute sync;
 * legacy hosts and unmapped boot (e.g. Mega Man X's IPL polling) keep relative
 * catch-up. SA-1 already uses absolute frame time. */
static inline bool interp_bridge_use_absolute_apu_timeline(
    bool frame_timeline_active, bool is_sa1, bool mapped_extended_frame) {
  return frame_timeline_active && (is_sa1 || mapped_extended_frame);
}

/* Optional game policy invoked immediately before one interpreted opcode.
 * The bridge compares the live PC first, so ordinary interpreted instructions
 * pay only the address check. At a match it synchronizes registers into
 * CpuState, invokes the callback, then copies any changes back.
 *
 * Up to 192 registrations may be armed (LoROM FastROM bit7 is masked). Calling with a
 * non-NULL hook adds/replaces that PC; hook=NULL clears all slots. */
typedef void (*InterpPreOpcodeHook)(CpuState *cpu, uint32_t pc24);
void interp_bridge_set_pre_opcode_hook(uint32_t pc24,
                                       InterpPreOpcodeHook hook);
/* Compose without replacing an existing owner's callback. Duplicate pairs are
 * idempotent. Returns false on invalid input/capacity exhaustion. Callbacks run
 * in registration order; a redirect completes handling for that original PC. */
bool interp_bridge_add_pre_opcode_hook(uint32_t pc24, InterpPreOpcodeHook hook);
void interp_bridge_pre_opcode_redirect(uint32_t pc24);

/*
 * Run the interpreter over guest code at entry_pc24, in the context of `cpu`.
 * `cpu` is updated in place. Returns:
 *   1 = the routine returned cleanly (balanced past entry);
 *   0 = the bridge bailed (iteration cap / contained failure).
 */
int interp_bridge_run(CpuState *cpu, uint32_t entry_pc24);

/* Faithful LLE of an infinite cooperative-scheduler loop (e.g. MMX's $8099 task
 * scheduler): run the real ROM scheduler under interp816 from entry_pc24 and
 * yield after one frame's slot walk — when it reaches yield_pc (its vblank-wait
 * spin) with the flag byte at flag_addr cleared. Tasks it dispatches bounce to
 * compiled bodies via the paired ABI. Returns 1 on clean yield, 0 on cap bail. */
int interp_bridge_run_scheduler(CpuState *cpu, uint32_t entry_pc24,
                                uint32_t yield_pc, uint16_t flag_addr);

/* General infinite-loop driver.  This is the scheduler helper with an
 * explicit byte value for games whose vblank wait flag is asserted while
 * waiting (Super Metroid), rather than cleared after a slot walk (MMX). */
int interp_bridge_run_loop(CpuState *cpu, uint32_t entry_pc24,
                           uint32_t yield_pc, uint16_t flag_addr,
                           uint8_t flag_value);

/* Run a whole-program LLE continuation until it reaches a deterministic
 * read-only cycle: the complete architectural state repeats without any bus
 * write.  Such a cycle can only make progress through asynchronous hardware
 * (NMI/IRQ/coprocessor), so it is a general frame boundary rather than a
 * game-address hint. */
int interp_bridge_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24);
uint32_t interp_bridge_lle_resume_pc(void);
/* Host override of the resume PC (recorded in the resume ring as external). */
void interp_bridge_set_lle_resume_pc(uint32_t pc);

/*
 * Rollback support: the bridge carries execution state across frames that no
 * guest snapshot can hold — the LLE resume cursor, the batched APU debt, and
 * the progress/livelock heuristic's per-address value cache and epochs. A
 * rewind that restores WRAM and the CPU but leaves these on the discarded
 * timeline makes the interpreter break out of a spin after a different number
 * of iterations, so the replayed frame consumes a different number of master
 * cycles: measured as hPos, apuCatchupCycles and autoJoyTimer diverging on
 * the FIRST replayed frame in ~92% of probes, with guest memory identical
 * (see snes_rb_probe.c). Opaque blob by design — its contents are the
 * bridge's business, and the caller only has to store and hand it back.
 */
size_t interp_bridge_rb_state_size(void);
void   interp_bridge_rb_state_save(void *out);
void   interp_bridge_rb_state_load(const void *in);

/* True if the most recent auto-quiescent yield was a 65816 WAI. The host
 * should deliver NMI/IRQ before resuming at interp_bridge_lle_resume_pc().
 * Sticky until read (then cleared). */
int interp_bridge_lle_took_wai(void);

/* True if the most recent auto-quiescent yield was a read-only spin (stable
 * CPU/memory state, no WAI) — e.g. a game polling $4210 for the NMI flag.
 * The host should deliver NMI to such a blocked game when NMI is enabled.
 * Sticky until read (then cleared). */
int interp_bridge_lle_took_quiescent(void);

/* Optional whole-program LLE deadline.  When nonzero, the auto-quiescent
 * bridge yields at the first architectural instruction boundary whose master
 * clock reaches this value.  Event-driven game schedulers use this to prevent
 * a productive CPU/MMIO loop from running across multiple vblanks atomically. */
void interp_bridge_set_master_deadline(uint64_t master_clock);
void interp_bridge_reset_dynamic_cache(void);

/* True only while a paired AOT bounce is executing inside an auto-quiescent
 * scheduler whose current frame deadline has been reached. Long,
 * architecturally interruptible instructions use this at their legal byte
 * boundaries before unwinding to the owning interpreter. */
int interp_bridge_lle_master_deadline_reached(const CpuState *cpu);

/* Execute an architectural interrupt handler through its terminal RTI. The
 * caller has already materialized the hardware interrupt frame. */
int interp_bridge_run_interrupt(CpuState *cpu, uint32_t entry_pc24);

/* Save-state task resume: interpret a suspended cooperative task from its
 * recorded yield return address (an arbitrary mid-function guest PC; the
 * caller restores the task's CpuState first). Calls bounce to compiled bodies
 * via the paired ABI — including yield HLEs, which suspend the hosting fiber
 * exactly like the compiled path — so after one interpreted function frame the
 * task runs mostly compiled again. Returns 1 when the task's top-level RTS
 * unwinds past task_base_s (task finished), 0 on a step-cap wedge bail. The
 * step cap resets on every successful bounce (it bounds interp-side wedges,
 * not the resumed task's lifetime). */
int interp_bridge_resume_task(CpuState *cpu, uint32_t resume_pc24,
                              uint16_t task_base_s,
                              const uint32_t *stop_pcs, int n_stop);

/* Production tier-down entry, called from generated indirect-dispatch defaults
 * (an absolute-indirect JMP/JML whose loaded target isn't in the static case
 * list). Interprets the target instead of silently dropping the transfer;
 * always returns RECOMP_RETURN_NORMAL. Declared in cpu_state.h too (so
 * generated code sees it without including this header). */

/* Count of tier-downs taken this run (observability / tests / Phase-2
 * manifest). */
/* Optional host coverage hooks; NULL by default. pc_hook fires once per
 * INTERPRETED opcode (exact). bounce_hook fires once per compiled body
 * ENTERED -- an entry, not an extent, since a compiled body runs an unknown
 * number of opcodes without reporting them. Do not treat a bounce as coverage
 * of the body interior. */
extern void (*g_interp_bridge_pc_hook)(uint32_t pc24, int m_flag, int x_flag);
extern void (*g_interp_bridge_bounce_hook)(uint32_t pc24, int m_flag, int x_flag);

long interp_tier_hit_count(void);
void interp_tier2_stats(int *sites, unsigned long long *clean,
                        unsigned long long *bail);

/* ── Phase-2 gap manifest (always-on coverage worklist) ────────────────────
 * Every tier-down is recorded into a growable in-memory table keyed by
 * (site, target, m/x width, kind), tracking clean-return vs contained-bail counts
 * and the frame span. This is the WORKLIST the offline ingest tool
 * (tools/tier2_ingest.py, Phase 3) folds back into the cfg so the next regen
 * makes the discovered entries Tier-1 AOT. Recording is cheap and lives in
 * every config (Production included) — it is NOT gated behind SNESRECOMP_TRACE.
 *
 * Each tuple's first sighting is immediately appended and flushed to a JSONL
 * journal. A unique final manifest is written on normal exit; the journal is
 * the recovery source if the process cannot run its exit handlers.
 *
 * Tier2CoverageDumpJson embeds the table into the unified post-mortem report
 * (build/last_run_report.json), with a trailing comma like the other
 * dump_*_json sections. Tier2CoverageWriteManifest writes the slim standalone
 * manifest (schema "snesrecomp tier2 coverage v1") that the ingest tool reads. */
void Tier2CoverageDumpJson(FILE *f);
void Tier2CoverageReset(void);
void Tier2CoverageTick(int frame);
void Tier2CoverageWriteManifest(const char *path, const char *rom_title);
void Tier2CoverageWriteDefaultManifest(const char *rom_title);
#ifdef SNESRECOMP_TIER2_TEST
void Tier2CoverageTestRecord(uint32_t site, uint32_t target, uint8_t mx,
                             uint8_t kind, int clean);
#endif

#endif /* INTERP_BRIDGE_H */
