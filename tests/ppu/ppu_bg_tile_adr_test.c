/* The accessor accepts pointer/index expressions, evaluating each once. */
#include "snes/ppu.h"
#include <stdio.h>

static struct State { Ppu base; } state;
static unsigned pointer_calls, layer_calls;
static Ppu *next_ppu(void) { ++pointer_calls; return &state.base; }
static unsigned next_layer(void) { ++layer_calls; return 2; }
#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "PPU_bgTileAdr check failed at line %d\n", __LINE__); return 1; \
} } while (0)

int main(void) {
  static const unsigned expected[] = {0x3000, 0xa000, 0x5000, 0xf000};
  Ppu *ppu = &state.base;
  state.base.bgTileAdr = 0xf5a3;
  for (unsigned layer = 0; layer < 4; ++layer)
    CHECK(PPU_bgTileAdr(ppu, layer) == expected[layer]);
  CHECK(PPU_bgTileAdr(&state.base, 1) == 0xa000);
  CHECK(PPU_bgTileAdr(ppu, 1 + 1) == 0x5000);
  CHECK(PPU_bgTileAdr(next_ppu(), next_layer()) == 0x5000);
  CHECK(pointer_calls == 1 && layer_calls == 1);
  puts("PPU_bgTileAdr expression checks passed");
  return 0;
}
