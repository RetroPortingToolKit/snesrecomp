/* Unit tests for runner/src/content_variant.{c,h} and variant_selector.{c,h}.
 *
 * ROM-free. The runtime seams the registry touches (save root, game
 * registration, module selection, the generic frame driver) are stubbed so
 * the test observes exactly what a prepare/switch hands them. Files are
 * written under a scratch directory in the current working directory. */
#include "content_variant.h"
#include "variant_selector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "generic_frame_driver.h"
#include "sha256.h"

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

/* ── stubs ─────────────────────────────────────────────────────────────── */
static char s_root[96] = "cv_test_saves";
const char *RtlSaveRoot(void) { return s_root; }
void RtlSetSaveRoot(const char *root) { snprintf(s_root, sizeof(s_root), "%s", root ? root : "saves"); }
void RtlEnsureSaveDir(void) {}
static const RtlGameInfo *s_registered;
void RtlRegisterGame(const RtlGameInfo *info) { s_registered = info; }
static const DispatchEntry *s_dispatch;
static unsigned s_dispatch_count;
void cpu_select_program(const DispatchEntry *dispatch, unsigned count,
                        const RamRoutineGuard *guards, unsigned guard_count) {
  (void)guards; (void)guard_count;
  s_dispatch = dispatch; s_dispatch_count = count;
}
static SnesGenericBootPolicy s_policy;
static int s_policy_set, s_driver_reset;
void snes_generic_frame_driver_set_boot_policy(const SnesGenericBootPolicy *p) {
  s_policy_set++;
  if (p) s_policy = *p; else memset(&s_policy, 0, sizeof(s_policy));
}
void snes_generic_frame_driver_reset(void) { s_driver_reset++; }
static RtlGameInfo s_generic_info;
const RtlGameInfo *snes_generic_frame_driver_game_info(const char *title) {
  s_generic_info.title = title;
  return &s_generic_info;
}
uint8 *ReadWholeFile(const char *name, size_t *length) {
  FILE *f = fopen(name, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8 *buf = malloc((size_t)n + 1);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
  fclose(f);
  *length = (size_t)n;
  return buf;
}
/* Mods are compiled OUT in this test: declared variants resolve through the
 * "no mod support" path, which is exactly the shipping shape of a build
 * without the loader. */

static const DispatchEntry kStockDispatch[1] = {{0x008000u, {NULL, NULL, NULL, NULL}, 0}};
static const DispatchEntry kOtherDispatch[1] = {{0x008000u, {NULL, NULL, NULL, NULL}, 0}};

int main(void) {
  static uint8_t rom[0x8000];
  for (size_t i = 0; i < sizeof(rom); i++) rom[i] = (uint8_t)(i * 3u);

  SnesProgramModule stock = {0}, other = {0};
  stock.id = "main"; stock.symbol_prefix = ""; stock.dispatch = kStockDispatch; stock.dispatch_count = 1;
  stock.rom_size = sizeof(rom); sha256_compute(rom, sizeof(rom), stock.rom_sha256);
  other.id = "testb"; other.symbol_prefix = "testb"; other.dispatch = kOtherDispatch; other.dispatch_count = 1;
  other.rom_size = sizeof(rom); memcpy(other.rom_sha256, stock.rom_sha256, 32);
  CHECK(snes_program_module_register(&stock));
  CHECK(snes_program_module_register(&other));

  static const RtlGameInfo smw_info = { .title = "smw" };
  snes_variant_bind_game_info("main", &smw_info);

  /* Builtin registration: first one is active; duplicate refused. */
  const SnesContentVariant *smw = snes_variant_register_builtin(
      "smw", "Super Mario World", "main", "mario", 0, 1, 0x0100, 0x08);
  CHECK(smw && snes_variant_active() == smw);
  CHECK(snes_variant_register_builtin("smw", "dup", "main", "mario", 0, 0, 0, 0) == NULL);
  CHECK(snes_variant_count() == 1);

  /* Declared variants: a save-namespace-only twin of the stock module and a
   * second-module variant. Neither can be available without mod support. */
  SnesVariantDecl a; memset(&a, 0, sizeof(a));
  snprintf(a.id, sizeof(a.id), "test.a");
  snprintf(a.display_name, sizeof(a.display_name), "Test A");
  snprintf(a.module_id, sizeof(a.module_id), "main");
  snprintf(a.selector_group, sizeof(a.selector_group), "mario");
  a.selector_order = 10; a.has_selector_screen = 1; a.selector_wram_addr = 0x0100; a.selector_wram_value = 0x08;
  snprintf(a.package_id, sizeof(a.package_id), "pkg"); snprintf(a.feature_id, sizeof(a.feature_id), "test");
  SnesVariantDecl b = a;
  snprintf(b.id, sizeof(b.id), "test.b"); snprintf(b.display_name, sizeof(b.display_name), "Test B");
  snprintf(b.module_id, sizeof(b.module_id), "testb"); b.selector_order = 20;
  b.boot.poke_count = 1; b.boot.pokes[0].addr24 = 0x7FFF00; b.boot.pokes[0].value = 2; b.boot.entry_pc24 = 0x008123;
  SnesVariantDecl elsewhere = a;
  snprintf(elsewhere.id, sizeof(elsewhere.id), "zelda.x");
  snprintf(elsewhere.selector_group, sizeof(elsewhere.selector_group), "other");
  CHECK(snes_variant_register_declared(&a) != NULL);
  CHECK(snes_variant_register_declared(&b) != NULL);
  CHECK(snes_variant_register_declared(&elsewhere) != NULL);
  CHECK(snes_variant_count() == 4);
  /* A package may not shadow the builtin. */
  SnesVariantDecl shadow = a; snprintf(shadow.id, sizeof(shadow.id), "smw");
  CHECK(snes_variant_register_declared(&shadow) == NULL);
  /* Re-declaring replaces in place. */
  snprintf(a.display_name, sizeof(a.display_name), "Test A2");
  CHECK(snes_variant_register_declared(&a) == snes_variant_find("test.a"));
  CHECK(strcmp(snes_variant_find("test.a")->decl.display_name, "Test A2") == 0);
  CHECK(snes_variant_count() == 4);
  CHECK(strcmp(snes_variant_find("test.b")->decl.save_namespace, "test.b") == 0);

  snes_variant_refresh(rom, sizeof(rom));
  CHECK(snes_variant_find("smw")->available == 1);
  CHECK(snes_variant_find("test.a")->available == 0);
  CHECK(snes_variant_find("test.a")->reason[0] != '\0');

  /* Save roots: stock profile 1 is the historical root; everything else is
   * namespaced; profiles clamp. */
  char root[96];
  snes_variant_save_root(smw, 1, "saves", root, sizeof(root));
  CHECK(strcmp(root, "saves") == 0);
  snes_variant_save_root(smw, 2, "saves", root, sizeof(root));
  CHECK(strcmp(root, "saves/variants/smw/p2") == 0);
  snes_variant_save_root(snes_variant_find("test.a"), 1, "saves", root, sizeof(root));
  CHECK(strcmp(root, "saves/variants/test.a/p1") == 0);
  snes_variant_save_root(snes_variant_find("test.b"), 9, "saves", root, sizeof(root));
  CHECK(strcmp(root, "saves/variants/test.b/p3") == 0);
  snes_variant_save_root(smw, 0, NULL, root, sizeof(root));
  CHECK(strcmp(root, "saves") == 0);

  /* Prepare the builtin: module selected, port's game info registered, save
   * root moved to the profile, image is the launched ROM unowned. */
  const uint8_t *image = NULL; size_t image_size = 0; int owned = -1;
  char err[160];
  CHECK(snes_variant_prepare(smw, 2, rom, sizeof(rom), &image, &image_size, &owned, err, sizeof(err)) == 1);
  CHECK(image == rom && image_size == sizeof(rom) && owned == 0);
  CHECK(s_dispatch == kStockDispatch && s_dispatch_count == 1);
  CHECK(s_registered == &smw_info);
  CHECK(strcmp(RtlSaveRoot(), "cv_test_saves/variants/smw/p2") == 0);
  CHECK(snes_variant_active() == smw && snes_variant_active_profile() == 2);
  CHECK(s_policy_set == 1 && s_policy.poke_count == 0 && s_driver_reset == 1);

  /* Last selection round-trips through the base root. */
  char last_id[64] = ""; int last_profile = 0;
  CHECK(snes_variant_load_last_selection("cv_test_saves", last_id, sizeof(last_id), &last_profile) == 1);
  CHECK(strcmp(last_id, "smw") == 0 && last_profile == 2);
  CHECK(snes_variant_store_last_selection("cv_test_saves", "ghost", 1) == 1);
  CHECK(snes_variant_load_last_selection("cv_test_saves", last_id, sizeof(last_id), &last_profile) == 0);

  /* Preparing an unavailable variant fails, changes nothing. */
  CHECK(snes_variant_prepare(snes_variant_find("test.b"), 1, rom, sizeof(rom), &image, &image_size, &owned, err, sizeof(err)) == 0);
  CHECK(err[0] != '\0');
  CHECK(snes_variant_active() == smw && s_registered == &smw_info);
  CHECK(strcmp(RtlSaveRoot(), "cv_test_saves/variants/smw/p2") == 0);

  /* ── selector ─────────────────────────────────────────────────────────── */
  static uint8_t wram[0x20000];
  snes_variant_selector_sync();
  /* Off the file select: invisible, nothing masked. */
  wram[0x0100] = 0x07;
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_RIGHT | SNES_SEL_BTN_UP, wram)
        == (SNES_SEL_BTN_RIGHT | SNES_SEL_BTN_UP));
  CHECK(!snes_variant_selector_visible());
  /* On the file select: visible; the group is smw, test.a, test.b (zelda.x
   * is another group). Right moves to test.a and is consumed. */
  wram[0x0100] = 0x08;
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  CHECK(snes_variant_selector_visible());
  CHECK(snes_variant_selector_highlight_index() == 0);
  uint32_t out = snes_variant_selector_filter_input(SNES_SEL_BTN_RIGHT, wram);
  CHECK(out == 0);
  CHECK(snes_variant_selector_highlight_index() == 1);
  CHECK(snes_variant_selector_highlight_profile() == 1);
  /* Held Right stays consumed; Up/Down now move the profile and are masked
   * (guest cursor must not move while browsing). */
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_RIGHT, wram) == 0);
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_DOWN, wram) == 0);
  CHECK(snes_variant_selector_highlight_profile() == 2);
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_UP, wram) == 0);
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_UP, wram) == 0);
  CHECK(snes_variant_selector_highlight_profile() == SNES_VARIANT_PROFILES);
  /* Unrelated seat-1 bits and non-nav seat-0 bits pass through while away. */
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_SELECT | (0xfffu << 12), wram)
        == (SNES_SEL_BTN_SELECT | (0xfffu << 12)));
  /* Confirm on an UNAVAILABLE variant does nothing but consume. */
  const SnesContentVariant *sw = NULL; int swp = 0;
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_A, wram) == 0);
  CHECK(snes_variant_selector_take_switch(&sw, &swp) == 0);
  /* Cancel returns home; everything passes again, including Up/Down. */
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_B, wram) == 0);
  CHECK(snes_variant_selector_highlight_index() == 0);
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_UP | SNES_SEL_BTN_A, wram)
        == (SNES_SEL_BTN_UP | SNES_SEL_BTN_A));
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  /* Left wraps to the last of the group. Make it available and confirm. */
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_LEFT, wram) == 0);
  CHECK(snes_variant_selector_highlight_index() == 2);
  ((SnesContentVariant *)snes_variant_find("test.b"))->available = 1;
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_DOWN, wram) == 0);
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_START, wram) == 0);
  CHECK(snes_variant_selector_take_switch(&sw, &swp) == 1);
  CHECK(sw == snes_variant_find("test.b") && swp == 2);
  CHECK(snes_variant_selector_take_switch(&sw, &swp) == 0);
  /* Leaving the screen snaps the highlight home so the guest is never
   * blocked when it returns. */
  wram[0x0100] = 0x0E;
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  wram[0x0100] = 0x08;
  CHECK(snes_variant_selector_filter_input(0, wram) == 0);
  CHECK(snes_variant_selector_highlight_index() == 0);

  /* With a single-variant group the selector is inert on the same screen. */
  snes_variant_clear_declared();
  CHECK(snes_variant_count() == 1 && snes_variant_active() == smw);
  snes_variant_selector_sync();
  CHECK(snes_variant_selector_filter_input(SNES_SEL_BTN_LEFT | SNES_SEL_BTN_RIGHT, wram)
        == (SNES_SEL_BTN_LEFT | SNES_SEL_BTN_RIGHT));
  CHECK(!snes_variant_selector_visible());

  /* Drawing into a frame while invisible leaves it untouched. */
  static uint8_t fb[256 * 224 * 4];
  memset(fb, 0x11, sizeof(fb));
  snes_variant_selector_draw(fb, 256 * 4, 256, 224);
  CHECK(fb[0] == 0x11 && fb[sizeof(fb) - 1] == 0x11);

  remove("cv_test_saves/variants/last-selection.txt");
  if (g_failures) {
    printf("%d failure(s)\n", g_failures);
    return 1;
  }
  printf("content_variant_test: all checks passed\n");
  return 0;
}
