#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/snes.h"
#include "program_module.h"
#include "sha256.h"
#include "snes/tier2_capture.h"

typedef struct recomp_snap_entry recomp_snap_entry;
typedef struct Ppu Ppu;
typedef struct Dma Dma;

#ifdef EXPECT_FUNC_SNAPSHOT_ON
#if SNESRECOMP_FUNC_SNAPSHOT != 1
#error "SNESRECOMP_FUNC_SNAPSHOT must be numeric 1 when opt-in diagnostics are enabled"
#endif
#endif

#ifdef EXPECT_STACK_BALANCE_DIAGNOSTICS_ON
#if SNESRECOMP_STACK_BALANCE_DIAGNOSTICS != 1
#error "SNESRECOMP_STACK_BALANCE_DIAGNOSTICS must be numeric 1 when opt-in diagnostics are enabled"
#endif
#endif

CpuState g_cpu;
int snes_frame_counter;
int g_wlog_configured;
uint8 g_ram[0x20000];
uint8 *g_sram;
int g_sram_size;
const uint8 *g_rom;
Ppu *g_ppu;
Dma *g_dma;

extern const char *g_recomp_snap_on_func;
extern int g_recomp_snap_count;
extern int g_recomp_stack_top;
extern uint16_t g_cpu_entry_s[];
extern const recomp_snap_entry *recomp_snap_lookup(int call_idx);

int wlog_scope_available(void) { return 0; }
void wlog_scope_enter(const char *tag) { (void)tag; }
void wlog_scope_exit(void) {}
uint8 *RomPtr(uint32_t addr) { (void)addr; return NULL; }
void Tier2CoverageWriteDefaultManifest(const char *rom_title) {
  (void)rom_title;
}
void tier2_capture_set_default_enabled(int enabled) { (void)enabled; }
int tier2_capture_enabled(void) { return 0; }
void msu1_init(void) {}
Snes *snes_init(uint8_t *ram) { (void)ram; return NULL; }
bool snes_loadRom(Snes *snes, const uint8_t *data, int length) {
  (void)snes; (void)data; (void)length; return false;
}
void snes_reset(Snes *snes, bool hard) { (void)snes; (void)hard; }
void cart_set_master_clock_source(Cart *cart, const uint64_t *master_clock) {
  (void)cart; (void)master_clock;
}
void ppu_reset(Ppu *ppu) { (void)ppu; }
void dma_reset(Dma *dma) { (void)dma; }
void cpu_state_init(CpuState *cpu, uint8_t *ram) {
  (void)cpu;
  (void)ram;
}
void rtl_reset_host_pacing(void) {}

/* PE linkers can retain SnesInit even with section GC. This harness exercises
 * stack diagnostics only; fail loudly if an initialization dependency runs. */
void snes_sync_master_clock(Snes *snes, uint64_t clock) {
  (void)snes; (void)clock; abort();
}
void snes_set_master_clock_charge_hook(SnesMasterClockChargeHook hook) { (void)hook; abort(); }
void snes_set_wram_write_log_hook(SnesWramWriteLogHook hook) { (void)hook; abort(); }
void wlog_addr_note_direct(uint32_t wa, uint8_t v, const char *via) {
  (void)wa; (void)v; (void)via; abort();
}
const char *cpu_dispatch_entry_reason(uint32_t pc, uint8_t mx) {
  (void)pc; (void)mx; abort();
}
void tier2_capture_set_entry_probe(const char *(*probe)(uint32_t, uint8_t)) { (void)probe; abort(); }
void tier2_capture_set_checkpoint_hook(void (*hook)(void)) { (void)hook; abort(); }
void Tier2CoverageReset(void) { abort(); }
void sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]) {
  (void)data; (void)len; (void)out; abort();
}
const SnesProgramModule *snes_program_module_active(void) { abort(); }
const SnesProgramModule *snes_program_module_find(const char *id) { (void)id; abort(); }
void tier2_capture_set_identity(const char *rom, const char *module,
                               const char *program, const char *mapper) {
  (void)rom; (void)module; (void)program; (void)mapper; abort();
}
void tier2_capture_set_build_digest(const char *digest) { (void)digest; abort(); }

static int check(int condition, const char *message) {
  if (!condition)
    fprintf(stderr, "FAIL: %s\n", message);
  return condition ? 0 : 1;
}

static int hle_entry_s_frames_do_not_resolve_returns(void) {
  const uint16_t entry_s = 0x01d0;
  int failures = 0;

  g_recomp_stack_top = 0;
  g_cpu.S = entry_s;
  g_cpu.host_return_valid = 0;

  /* Prime slot zero with a valid generated-frame entry, then reuse the same
   * slot for an HLE forwarding frame. That frame has no generated prologue. */
  RecompStackPush("previous_generated_frame");
  RecompStackPop();
  RecompStackPush("hle_forwarding_stub");
  cpu_take_tailcall_return_context(NULL, NULL);
  RecompStackPush("generated_child");

  failures += check(cpu_resolve_ancestor_skip(entry_s) == -1,
                    "HLE entry-S is excluded from ancestor return resolution");
  failures += check(cpu_resolve_post_return_skip(entry_s) == -1,
                    "HLE entry-S is excluded from post-return resolution");

  RecompStackPop();
  RecompStackPop();
  return failures;
}

static int interpreter_scope_does_not_resolve_returns(void) {
  int failures = 0;
  g_recomp_stack_top = 0;
  g_cpu.host_return_valid = 2;
  g_cpu.S = 0x1fef;
  RecompStackPush("compiled_tail_root");
  /* PHB + PEA precede a fallback. A later PLA; RTS returns to the manual
   * PEA continuation while leaving PHB on the stack for its real PLB. */
  g_cpu.S = 0x1fec;
  RecompStackPushInterpreter("interp@$A6CBE5");
  g_cpu.S = 0x1fea;
  RecompStackPush("compiled_nonlocal_return");
  failures += check(cpu_resolve_ancestor_skip(0x1fec) == -1,
                    "interpreter attribution is not a compiled ancestor");
  failures += check(cpu_resolve_post_return_skip(0x1fee) == -1,
                    "interpreter attribution cannot consume a post-return skip");
  RecompStackPop();
  RecompStackPop();
  /* Reusing the observer's slot for a real compiled frame must restore it. */
  g_cpu.S = 0x1fec;
  RecompStackPush("compiled_parent");
  g_cpu.S = 0x1fea;
  RecompStackPush("compiled_child");
  failures += check(cpu_resolve_ancestor_skip(0x1fec) == 1,
                    "reused interpreter slot accepts a real compiled ancestor");
  RecompStackPop();
  RecompStackPop();
  RecompStackPop();
  return failures;
}

static int generated_entry_s_frames_still_resolve_returns(void) {
  const uint16_t parent_entry_s = 0x01d0;
  int failures = 0;

  g_recomp_stack_top = 0;
  g_cpu.host_return_valid = 0;
  g_cpu.S = parent_entry_s;
  RecompStackPush("generated_parent");
  g_cpu.S = 0x01ce;
  RecompStackPush("generated_child");

  failures += check(cpu_resolve_ancestor_skip(parent_entry_s) == 1,
                    "generated entry-S remains eligible for ancestor returns");
  failures += check(cpu_resolve_post_return_skip(parent_entry_s) == 1,
                    "generated entry-S remains eligible for post-return resolution");

  RecompStackPop();
  RecompStackPop();
  return failures;
}

static int json_matches_stack_balance_mode(void) {
  FILE *f = tmpfile();
  char buf[256];
  size_t n;
  int ok;

  if (!f)
    return 0;
  RecompStackBalDumpJson(f);
  rewind(f);
  n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = 0;
  fclose(f);

#if SNESRECOMP_STACK_BALANCE_DIAGNOSTICS
  ok = strstr(buf, "\"stack_balance\": [") != NULL &&
       strstr(buf, "diagnostic_gates_test") != NULL &&
       strstr(buf, "\"stack_balance_disabled\"") == NULL;
#else
  ok = strstr(buf, "\"stack_balance\": []") != NULL &&
       strstr(buf, "\"stack_balance_disabled\": true") != NULL;
#endif
  return ok;
}

int main(void) {
  int failures = 0;

  memset(&g_cpu, 0, sizeof(g_cpu));
  g_recomp_stack_top = 0;
  g_cpu.host_return_valid = 2;
  g_cpu.S = 0x01fd;
  RecompStackPush("diagnostic_gates_test");
  g_cpu_entry_s[g_recomp_stack_top - 1] = 0x01fd;
  g_cpu.S = 0x01fe;
  RecompStackPop();
  failures += check(g_recomp_stack_top == 0,
                    "functional recomp stack still pushes and pops");
  failures += hle_entry_s_frames_do_not_resolve_returns();
  failures += generated_entry_s_frames_still_resolve_returns();
  failures += check(json_matches_stack_balance_mode(),
                    "stack-balance dump matches diagnostic mode");
  failures += interpreter_scope_does_not_resolve_returns();

  g_recomp_snap_on_func = "diagnostic_gates_test";
  g_recomp_snap_count = 7;
  RecompStackPush("diagnostic_gates_test");
  RecompStackPop();
#if SNESRECOMP_FUNC_SNAPSHOT
  failures += check(g_recomp_snap_count == 8 && recomp_snap_lookup(8) != NULL,
                    "function snapshots capture when enabled");
#else
  failures += check(g_recomp_snap_count == 7 && recomp_snap_lookup(1) == NULL,
                    "function snapshots stay inert by default");
#endif

  if (failures)
    return 1;
  puts("diagnostic_gates_test: PASS");
  return 0;
}
