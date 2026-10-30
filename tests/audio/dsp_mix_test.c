/* Compare the build-time native mixer with the maintained scalar mixer.
 * The runner compiles dsp.c twice, renaming the scalar public symbols. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/dsp.h"

Dsp *reference_dsp_init(uint8_t *ram);
void reference_dsp_free(Dsp *dsp);
void reference_dsp_reset(Dsp *dsp);
void reference_dsp_cycle(Dsp *dsp);
void reference_dsp_write(Dsp *dsp, uint8_t adr, uint8_t value);
bool reference_dsp_set_output_voices(Dsp *dsp, uint8_t voices);
void reference_dsp_suspend_output_filter(Dsp *dsp, bool suspended);
void reference_dsp_saveload(Dsp *dsp, SaveLoadInfo *info);
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
int snes_frame_counter;

static uint32_t state = 0xd5135ea9;
static uint32_t random32(void) {
  state ^= state << 13; state ^= state >> 17; state ^= state << 5;
  return state;
}

typedef struct Snapshot {
  SaveLoadInfo info;
  unsigned char bytes[sizeof(Dsp)];
  bool loading;
} Snapshot;
static void transfer(SaveLoadInfo *info, void *data, size_t size) {
  Snapshot *snapshot = (Snapshot *)info;
  assert(size <= sizeof(snapshot->bytes));
  if (snapshot->loading) memcpy(data, snapshot->bytes, size);
  else memcpy(snapshot->bytes, data, size);
}

int main(void) {
  uint8_t ram[2][65536];
  for (unsigned i = 0; i < sizeof(ram[0]); ++i)
    ram[0][i] = ram[1][i] = (uint8_t)random32();
  Dsp *reference = reference_dsp_init(ram[0]);
  Dsp *native = dsp_init(ram[1]);
  reference_dsp_reset(reference); dsp_reset(native);
  unsigned audible = 0, echo_writes = 0, nonzero_pcm = 0;
  unsigned active_nonzero_pcm = 0, ring_wraps = 0;
  unsigned active_masks[256] = {0}, suspended_masks[256] = {0};
  /* Random live BRR, echo, FIR, KON/KOF, gain/source changes and noise.
   * All voice masks are exercised independently of canonical state. */
  for (unsigned tick = 0; tick < 65536; ++tick) {
    if (!(tick & 7u)) {
      uint8_t reg = random32() & 127u, value = random32();
      reference_dsp_write(reference, reg, value); dsp_write(native, reg, value);
    }
    if (!(tick & 31u)) {
      uint16_t address = random32(); uint8_t value = random32();
      ram[0][address] = ram[1][address] = value;
    }
    if (!(tick & 255u)) {
      uint8_t voices = (uint8_t)(tick >> 8);
      assert(reference_dsp_set_output_voices(reference, voices));
      assert(dsp_set_output_voices(native, voices));
    }
    /* Exercise every mask with the presentation mixer active AND suspended. */
    bool suspended = (tick & 128u) != 0;
    if (!(tick & 127u)) {
      reference_dsp_suspend_output_filter(reference, suspended);
      dsp_suspend_output_filter(native, suspended);
    }
    unsigned mask = tick >> 8;
    if (suspended) suspended_masks[mask]++;
    else active_masks[mask]++;
    if (!(tick & 63u)) {
      /* Include ring wrap and output suppression without losing synthesis. */
      reference->sampleRead = native->sampleRead = reference->sampleWrite;
      reference->suppressOutput = native->suppressOutput = (tick & 64u) != 0;
    }
    if (!(tick & 1023u)) {
      Snapshot a = {{transfer, NULL}, {0}, false}, b = a;
      reference_dsp_saveload(reference, &a.info); dsp_saveload(native, &b.info);
      assert(!memcmp(a.bytes, b.bytes, sizeof(a.bytes)));
      reference->channel[0].gain ^= 0x400;
      native->channel[0].gain ^= 0x400;
      reference->sampleRead++; native->sampleRead++;
      a.loading = b.loading = true;
      reference_dsp_saveload(reference, &a.info); dsp_saveload(native, &b.info);
    }
    uint32_t old_write = native->sampleWrite;
    reference_dsp_cycle(reference); dsp_cycle(native);
    if (native->sampleWrite != old_write) {
      unsigned slot = old_write & (DSP_SAMPLE_RING - 1);
      bool nonzero = native->sampleBuffer[slot * 2] != 0 ||
                     native->sampleBuffer[slot * 2 + 1] != 0;
      nonzero_pcm += nonzero;
      active_nonzero_pcm += nonzero && !suspended && mask != 255;
      ring_wraps += (native->sampleWrite & (DSP_SAMPLE_RING - 1)) == 0;
    }
    audible += native->channel[0].sampleOut != 0;
    echo_writes += native->echoWrites;
    assert(!memcmp(ram[0], ram[1], sizeof(ram[0])));
    assert(!memcmp(reference->ram, native->ram,
                   sizeof(Dsp) - offsetof(Dsp, ram)));
  }
  assert(audible > 100); assert(echo_writes > 100);
  assert(nonzero_pcm > 100); assert(active_nonzero_pcm > 100);
  assert(ring_wraps > 0);
  for (unsigned mask = 0; mask < 256; ++mask) {
    assert(active_masks[mask] == 128);
    assert(suspended_masks[mask] == 128);
  }
  printf("DSP MIX COVERAGE: masks=256 active_ticks=128 suspended_ticks=128 "
         "nonzero_pcm=%u active_masked_nonzero_pcm=%u ring_wraps=%u\n",
         nonzero_pcm, active_nonzero_pcm, ring_wraps);
  reference_dsp_free(reference); dsp_free(native);
  puts("DSP MIX CONTRACT PASSED: PCM, live RAM/BRR, registers, echo, masks, state");
  return 0;
}
