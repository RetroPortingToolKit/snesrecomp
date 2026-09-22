/* Unit tests for runner/src/program_module.{c,h}: the registry of linked
 * recompiled programs. ROM-free; cpu_select_program is stubbed so the test
 * observes exactly what a selection hands the CPU. */
#include "program_module.h"

#include <stdio.h>
#include <string.h>

#include "sha256.h"

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

static const DispatchEntry *g_selected_dispatch;
static unsigned g_selected_count;
static const RamRoutineGuard *g_selected_guards;
static unsigned g_selected_guard_count;
static int g_select_calls;

void cpu_select_program(const DispatchEntry *dispatch, unsigned count,
                        const RamRoutineGuard *guards, unsigned guard_count) {
  g_selected_dispatch = dispatch;
  g_selected_count = count;
  g_selected_guards = guards;
  g_selected_guard_count = guard_count;
  g_select_calls++;
}

static const DispatchEntry kDispatchA[1] = {{0x008000u, {NULL, NULL, NULL, NULL}, 0}};
static const DispatchEntry kDispatchB[2] = {
    {0x008000u, {NULL, NULL, NULL, NULL}, 0},
    {0x018000u, {NULL, NULL, NULL, NULL}, 0}};
static const RamRoutineGuard kGuardsB[1] = {{0x7E8000u, 16u, 0x12345678u}};

/* Registration from a constructor, the way module_v2.c does it. */
static SnesProgramModule g_ctor_module;
SNES_PROGRAM_MODULE_CONSTRUCTOR(register_ctor_module) {
  g_ctor_module.id = "ctor";
  g_ctor_module.symbol_prefix = "ctor";
  g_ctor_module.dispatch = kDispatchA;
  g_ctor_module.dispatch_count = 1;
  snes_program_module_register(&g_ctor_module);
}

int main(void) {
  uint8_t rom[0x8000];
  for (size_t i = 0; i < sizeof(rom); i++) rom[i] = (uint8_t)(i * 7u);

  /* The constructor ran before main. */
  CHECK(snes_program_module_count() == 1);
  CHECK(snes_program_module_find("ctor") == &g_ctor_module);
  CHECK(snes_program_module_at(0) == &g_ctor_module);
  CHECK(snes_program_module_at(1) == NULL);

  SnesProgramModule a = {0}, b = {0}, dup = {0}, noid = {0};
  a.id = "smw"; a.symbol_prefix = ""; a.dispatch = kDispatchA; a.dispatch_count = 1;
  a.rom_size = (uint32_t)sizeof(rom);
  sha256_compute(rom, sizeof(rom), a.rom_sha256);
  b.id = "smas"; b.symbol_prefix = "smas"; b.dispatch = kDispatchB; b.dispatch_count = 2;
  b.guards = kGuardsB; b.guard_count = 1;
  b.rom_size = 0x200000u;
  dup.id = "smw";
  noid.id = "";

  CHECK(snes_program_module_register(&a) == 1);
  CHECK(snes_program_module_register(&b) == 1);
  CHECK(snes_program_module_register(&dup) == 0);   /* duplicate id */
  CHECK(snes_program_module_register(&noid) == 0);  /* empty id */
  CHECK(snes_program_module_register(NULL) == 0);
  CHECK(snes_program_module_count() == 3);
  CHECK(snes_program_module_find("smw") == &a);
  CHECK(snes_program_module_find("smas") == &b);
  CHECK(snes_program_module_find("nope") == NULL);
  CHECK(snes_program_module_find(NULL) == NULL);

  /* Identity: exact size and digest, header already stripped. */
  CHECK(snes_program_module_rom_matches(&a, rom, sizeof(rom)) == 1);
  rom[100] ^= 1;
  CHECK(snes_program_module_rom_matches(&a, rom, sizeof(rom)) == 0);
  rom[100] ^= 1;
  CHECK(snes_program_module_rom_matches(&a, rom, sizeof(rom) - 1) == 0);
  CHECK(snes_program_module_rom_matches(&b, rom, sizeof(rom)) == 0); /* size */
  CHECK(snes_program_module_rom_matches(&a, NULL, sizeof(rom)) == 0);

  /* Selection routes exactly the module's tables to the CPU. */
  CHECK(snes_program_module_active() == NULL);
  snes_program_module_select(&b);
  CHECK(snes_program_module_active() == &b);
  CHECK(g_selected_dispatch == kDispatchB && g_selected_count == 2);
  CHECK(g_selected_guards == kGuardsB && g_selected_guard_count == 1);
  snes_program_module_select(&a);
  CHECK(g_selected_dispatch == kDispatchA && g_selected_count == 1);
  CHECK(g_selected_guards == NULL && g_selected_guard_count == 0);
  snes_program_module_select(NULL);   /* generated default */
  CHECK(snes_program_module_active() == NULL);
  CHECK(g_selected_dispatch == NULL && g_selected_count == 0);
  CHECK(g_select_calls == 3);

  /* Registry capacity is loud, not silent: fill it, then one more fails. */
  static SnesProgramModule filler[SNES_PROGRAM_MODULE_MAX];
  static char ids[SNES_PROGRAM_MODULE_MAX][8];
  unsigned accepted = 0;
  for (unsigned i = 0; i < SNES_PROGRAM_MODULE_MAX; i++) {
    snprintf(ids[i], sizeof(ids[i]), "f%u", i);
    filler[i].id = ids[i];
    accepted += (unsigned)snes_program_module_register(&filler[i]);
  }
  CHECK(accepted == SNES_PROGRAM_MODULE_MAX - 3);
  CHECK(snes_program_module_count() == SNES_PROGRAM_MODULE_MAX);

  if (g_failures) {
    printf("%d failure(s)\n", g_failures);
    return 1;
  }
  printf("program_module_test: all checks passed\n");
  return 0;
}
