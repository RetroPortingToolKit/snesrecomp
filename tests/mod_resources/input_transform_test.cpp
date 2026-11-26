// Standalone, ROM-free test of mod input transforms (mod_runtime.h): they
// run in registration order, a duplicate registration is ignored, the
// result is masked to the 12 button bits, and each sees the seat context.
#include "mod_runtime.h"
#include "content_variant.h"
#include <cassert>
#include <cstdio>

// Like exclusive_plugin_test, isolate the runtime from program switching.
extern "C" void snes_variant_clear_declared(void) {}
extern "C" const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) { return nullptr; }

static int g_seen_player = -1;
static uint16_t SwapShoulders(uint16_t b, const SnesModInputContext *ctx) {
  g_seen_player = ctx->player;
  uint16_t l = b & 0x400, r = b & 0x800;
  return (uint16_t)((b & ~0xC00u) | l << 1 | r >> 1);
}
static uint16_t ReadWram(uint16_t b, const SnesModInputContext *ctx) {
  // A held (bit 8) when WRAM $0010 is nonzero; also tries to set high bits.
  return (uint16_t)(b | (ctx->wram[0x10] ? 0x100 : 0) | 0xF000);
}

int main() {
  uint8_t wram[0x20000] = {0};
  SnesModInputContext ctx = {1, wram, 0, 0, 0, 0};
  assert(snes_mod_runtime_transform_input_c(0x400, &ctx) == 0x400);  // none registered
  assert(snes_mod_register_input_transform(SwapShoulders));
  assert(snes_mod_register_input_transform(SwapShoulders));           // idempotent
  assert(snes_mod_register_input_transform(ReadWram));
  assert(!snes_mod_register_input_transform(nullptr));
  assert(snes_mod_runtime_transform_input_c(0x400, &ctx) == 0x800);   // swapped once, masked
  assert(g_seen_player == 1);
  wram[0x10] = 1;
  assert(snes_mod_runtime_transform_input_c(0x800, &ctx) == 0x500);
  std::puts("input_transform_test: PASS");
  return 0;
}
