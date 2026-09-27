/* GSU PC hooks: they fire on the instruction actually executing (including a
 * branch target entered after its delay slot), see the registers as the
 * program left them, can redirect, never run inside a replay clone, and a
 * save never changes what the core does next. No game ROM required. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes/saveload.h"
#include "snes/superfx.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
  __FILE__, __LINE__, #c); failures++; } } while (0)

enum { kRomSize = 65536, kRamSize = 65536 };

/*  $00 D1       inc r1
 *  $01 05 02    bra $05          (delay slot $03 executes, $04 is skipped)
 *  $03 D2       inc r2
 *  $04 D3       inc r3
 *  $05 D4       inc r4           <- hook A (the branch target)
 *  $06 D5       inc r5           <- hook B redirects to $08
 *  $07 D6       inc r6
 *  $08 D7       inc r7
 *  $09 00       stop
 *  $0A 01       nop */
static const uint8_t program[] = {0xd1, 0x05, 0x02, 0xd2, 0xd3, 0xd4,
                                  0xd5, 0xd6, 0xd7, 0x00, 0x01};

static unsigned fired_a, fired_b;
static uint16_t seen_r1, seen_r2, seen_r3;

static void hook_a(SuperFx *fx, uint32_t pc24, void *context) {
  (void)context;
  CHECK(pc24 == 0x000005u);
  ++fired_a;
  seen_r1 = superfx_reg(fx, 1);
  seen_r2 = superfx_reg(fx, 2);
  seen_r3 = superfx_reg(fx, 3);
}

static void hook_b(SuperFx *fx, uint32_t pc24, void *context) {
  CHECK(pc24 == 0x000006u);
  CHECK(context == (void *)&fired_b);
  ++fired_b;
  superfx_set_reg(fx, 9, 0x1234);
  superfx_hook_redirect(fx, 0x0008);
}

static void run(SuperFx *fx) {
  for (unsigned n = 0; n < 16; ++n) fx->r[n].data = 0;
  superfx_cpu_write_io(fx, 0x301e, 0);
  superfx_cpu_write_io(fx, 0x301f, 0);
  superfx_sync(fx, fx->master_clock + 100000);
}

typedef struct Sink { SaveLoadInfo base; uint8_t bytes[1024]; int load; } Sink;
static void transfer(SaveLoadInfo *sli, void *data, size_t size) {
  Sink *s = (Sink *)sli;
  if (size > sizeof(s->bytes)) abort();
  if (s->load) memcpy(data, s->bytes, size); else memcpy(s->bytes, data, size);
}

int main(void) {
  uint8_t *rom = calloc(kRomSize, 1), *ram = calloc(kRamSize, 1);
  if (!rom || !ram) return 2;
  memcpy(rom, program, sizeof(program));
  SuperFx *fx = superfx_create(rom, kRomSize, ram, kRamSize);
  if (!fx) return 2;

  /* No hooks: the program as written. */
  run(fx);
  CHECK(!superfx_is_running(fx));
  CHECK(fx->r[1].data == 1 && fx->r[2].data == 1 && fx->r[3].data == 0);
  CHECK(fx->r[4].data == 1 && fx->r[5].data == 1 && fx->r[6].data == 1 &&
        fx->r[7].data == 1);

  /* Hooks armed: A sees the branch target after its delay slot; B skips $06
   * and $07 by redirecting to $08. */
  CHECK(superfx_set_pc_hook(fx, 0x000005u, hook_a, NULL));
  CHECK(superfx_set_pc_hook(fx, 0x000006u, hook_b, &fired_b));
  run(fx);
  CHECK(fired_a == 1 && fired_b == 1);
  CHECK(seen_r1 == 1 && seen_r2 == 1 && seen_r3 == 0);
  CHECK(fx->r[4].data == 1);                       /* A did not skip $05 */
  CHECK(fx->r[5].data == 0 && fx->r[6].data == 0); /* B skipped $06, $07 */
  CHECK(fx->r[7].data == 1 && fx->r[9].data == 0x1234);
  CHECK(!superfx_is_running(fx));

  /* Re-arming a PC replaces it; NULL disarms it. */
  CHECK(superfx_set_pc_hook(fx, 0x000006u, NULL, NULL));
  fired_a = fired_b = 0;
  run(fx);
  CHECK(fired_a == 1 && fired_b == 0 && fx->r[5].data == 1);

  /* A reset keeps host policy. */
  superfx_reset(fx);
  fired_a = 0;
  run(fx);
  CHECK(fired_a == 1);

  /* A save changes nothing; a load of a mid-job state rebuilds the pipeline
   * address so the next hooked instruction still fires. */
  Sink sink;
  memset(&sink, 0, sizeof(sink));
  sink.base.func = transfer;
  for (unsigned n = 0; n < 16; ++n) fx->r[n].data = 0;
  superfx_cpu_write_io(fx, 0x301e, 0);
  superfx_cpu_write_io(fx, 0x301f, 0);
  fired_a = 0;
  /* Step until $04's fetch: the pipeline holds $05's opcode, fetched from
   * R15-1 in straight-line code after the branch completed. */
  while (!(fx->pipeline_pc == 0x000005u) && superfx_is_running(fx))
    superfx_sync(fx, fx->master_clock + 1);
  CHECK(fx->pipeline_pc == 0x000005u);
  uint32_t before = fx->pipeline_pc;
  superfx_saveload(fx, &sink.base);
  CHECK(fx->pipeline_pc == before);
  fx->pipeline_pc = UINT32_MAX;          /* as if restored into a fresh core */
  fx->r[1].data ^= 0xffff;               /* make the load visibly replace state */
  sink.load = 1;
  superfx_saveload(fx, &sink.base);
  CHECK(fx->pipeline_pc == 0x000005u);
  superfx_sync(fx, fx->master_clock + 100000);
  CHECK(fired_a == 1);

  superfx_clear_pc_hooks(fx);
  fired_a = 0;
  run(fx);
  CHECK(fired_a == 0);

  superfx_destroy(fx);
  free(rom);
  free(ram);
  if (failures) return 1;
  puts("superfx pc hooks: target-after-delay-slot, redirect, rearm, reset, save/load passed");
  return 0;
}
