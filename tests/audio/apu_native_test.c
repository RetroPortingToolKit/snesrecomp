/* Exercise the native job against the maintained clock-by-clock executor.
 * Include the production translation unit so both run the real private port
 * scheduler. Test memory contains SPC programs, never retail audio data.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#ifndef SNESRECOMP_APU_NATIVE
#define SNESRECOMP_APU_NATIVE 1
#endif
#include "../../runner/src/snes/apu.c"

uint64_t g_apu_timer0_total_ticks;
int snes_frame_counter;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static uint32_t rng = 0x98ac6311u;
static uint32_t random32(void) {
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return rng;
}

static void equal(const void *a, const void *b, size_t n, const char *what) {
  if (!memcmp(a, b, n)) return;
  size_t i = 0;
  while (i < n && ((const uint8_t *)a)[i] == ((const uint8_t *)b)[i]) i++;
  fprintf(stderr, "%s differs at byte %zu: %u != %u\n", what, i,
      ((const uint8_t *)a)[i], ((const uint8_t *)b)[i]);
  exit(1);
}

static void same(Apu *reference, Apu *native) {
  equal(reference->ram, native->ram,
      offsetof(Apu, pad) + 6u - offsetof(Apu, ram), "APU RAM/ports/timers");
  equal(&reference->spc->a, &native->spc->a,
      sizeof(Spc) - offsetof(Spc, a), "SPC execution");
  equal(reference->dsp->ram, native->dsp->ram,
      sizeof(Dsp) - offsetof(Dsp, ram), "DSP state and PCM");
  equal(reference->portQueue, native->portQueue,
      sizeof(Apu) - offsetof(Apu, portQueue), "port queue and clock");
}

static void init(Apu *apu, unsigned mode) {
  apu_reset(apu);
  if (mode == 0) return; /* Real built-in IPL, waiting for CPU transfer. */
  /* A live program reads all timers, exchanges ports and changes timer
   * targets. Conditional branch adds variable instruction durations.
   * mov a,$fd; mov $f4,a; inc a; mov $fa,a; mov a,$f4;
   * cmp a,#$55; bne back; bra back. */
  static const uint8_t code[] = {
    0xe4,0xfd, 0xc4,0xf4, 0xbc, 0xc4,0xfa, 0xe4,0xf4,
    0x68,0x55, 0xd0,0xf3, 0x2f,0xf1
  };
  memcpy(apu->ram + 0x200, code, sizeof(code));
  apu->romReadable = false;
  apu->spc->pc = 0x200;
  apu->cpuCyclesLeft = 0;
  apu_cpuWrite(apu, 0xf1, 7);
  apu_cpuWrite(apu, 0xfa, 0); /* target 0 means divider wrap at 256 */
  apu_cpuWrite(apu, 0xfb, 1);
  apu_cpuWrite(apu, 0xfc, 255);
  dsp_write(apu->dsp, 0x6c, 0x20);
  dsp_write(apu->dsp, 0x0c, 100);
  dsp_write(apu->dsp, 0x1c, 100);
  dsp_write(apu->dsp, 0x00, 100);
  dsp_write(apu->dsp, 0x01, 100);
  dsp_write(apu->dsp, 0x07, 127);
  dsp_write(apu->dsp, 0x3d, 1); /* audible noise voice */
  dsp_write(apu->dsp, 0x4c, 1);
}

int main(int argc, char **argv) {
  Apu *reference = apu_init(), *native = apu_init();
  for (unsigned mode = 0; mode < 3; mode++) {
    init(reference, mode); init(native, mode);
    if (mode == 2) {
      reference->cycles = native->cycles = UINT32_MAX - 100u;
      reference->timer[0].divider = native->timer[0].divider = 253;
    }
    for (unsigned step = 0; step < 4096; step++) {
      if (!(step & 7u)) {
        unsigned port = random32() & 3u, value = random32() & 255u;
        uint64_t when = reference->portClock + (random32() & 127u);
        assert(apu_schedulePortWrite(reference, port, value, when));
        assert(apu_schedulePortWrite(native, port, value, when));
      }
      if (!(step & 15u)) {
        unsigned reg = 0xfa + random32() % 3, val = random32() & 255u;
        apu_cpuWrite(reference, reg, val); apu_cpuWrite(native, reg, val);
      }
      uint32_t clocks = random32() % 257u;
      uint64_t ticks = g_apu_timer0_total_ticks;
      for (uint32_t i = 0; i < clocks; i++) apu_cycle(reference);
      uint64_t expected_ticks = g_apu_timer0_total_ticks;
      g_apu_timer0_total_ticks = ticks;
      apu_runCycles(native, clocks);
      assert(g_apu_timer0_total_ticks == expected_ticks);
      same(reference, native);
    }
  }
  if (argc > 1) {
    const uint32_t clocks = 32040000;
    init(reference, 1); init(native, 1);
    clock_t begin = clock();
    for (uint32_t i = 0; i < clocks; i++) apu_cycle(reference);
    double scalar = (double)(clock()-begin) / CLOCKS_PER_SEC;
    begin = clock(); apu_runCycles(native, clocks);
    double batch = (double)(clock()-begin) / CLOCKS_PER_SEC;
    same(reference, native);
    printf("APU component: scalar %.6f native %.6f ratio %.4f\n", scalar, batch, scalar/batch);
  }
  apu_free(reference); apu_free(native);
  puts("APU NATIVE CONTRACT PASSED");
  return 0;
}
