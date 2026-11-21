/* The last bus cycle of an instruction (snes/last_cycle.c), from code in WRAM:
 * an internal operation 6 clocks, an access by its region's speed. */
#define main hdma_harness_main
#include "hdma_timing_test.c"
#undef main
#include <assert.h>
#include "snes/last_cycle.h"

Snes *g_snes;
uint8_t g_memsel;

static uint8_t s_ram[0x20000];

static unsigned at(const uint8_t *code, unsigned n, uint32_t next) {
  for (unsigned i = 0; i < n; i++) s_ram[0x1000 + i] = code[i];
  return snes_last_cycle_clocks(0x7E1000, next);
}

int main(void) {
  static Snes s;
  s.ram = s_ram;
  g_snes = &s;
  g_cpu.DB = 0;
  assert(at((const uint8_t[]){0xEA}, 1, 0x7E1001) == 6);                    /* NOP */
  assert(at((const uint8_t[]){0xAD, 0x12, 0x42}, 3, 0x7E1003) == 6);       /* LDA $4212 */
  assert(at((const uint8_t[]){0xAD, 0x16, 0x40}, 3, 0x7E1003) == 12);      /* LDA $4016 */
  assert(at((const uint8_t[]){0xAD, 0x00, 0x10}, 3, 0x7E1003) == 8);       /* LDA $1000 */
  assert(at((const uint8_t[]){0xD0, 0x10}, 2, 0x7E1002) == 8);             /* BNE, not taken */
  assert(at((const uint8_t[]){0xD0, 0x10}, 2, 0x7E1012) == 6);             /* BNE, taken */
  g_cpu.S = 0x1FF0;
  assert(at((const uint8_t[]){0x48}, 1, 0x7E1001) == 8);                    /* PHA */
  puts("last cycle: internal, I/O, joypad, WRAM, branches, stack PASS");
  return 0;
}
