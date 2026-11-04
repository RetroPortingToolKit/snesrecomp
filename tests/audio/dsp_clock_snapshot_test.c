/* Current-build per-clock DSP save/load must resume the same simulation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/dsp.h"
int snes_frame_counter;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

typedef struct {
  SaveLoadInfo base;
  unsigned char data[sizeof(Dsp) * 2];
  size_t position;
  int loading;
} Snapshot;
static void transfer(SaveLoadInfo *info, void *data, size_t size) {
  Snapshot *s = (Snapshot *)info;
  assert(s->position + size <= sizeof(s->data));
  if (s->loading) memcpy(data, s->data + s->position, size);
  else memcpy(s->data + s->position, data, size);
  s->position += size;
}

int main(void) {
  uint8_t *ram = calloc(65536, 1);
  Dsp *dsp = dsp_init(ram);
  Snapshot *snapshot = calloc(1, sizeof(*snapshot));
  assert(ram && dsp && snapshot);
  snapshot->base.func = transfer;
  for (unsigned phase = 0; phase < 32; ++phase) {
  memset(snapshot, 0, sizeof(*snapshot)); snapshot->base.func = transfer;
  memset(ram, 0, 65536);
  ram[0x2001] = ram[0x2003] = 0x21;
  ram[0x2100] = 0x73;
  memset(ram + 0x2101, 0x77, 8);
  dsp_reset(dsp);
  dsp_write(dsp, 0x6c, 0x20); /* disable echo RAM writes */
  dsp_write(dsp, 0x00, 127); dsp_write(dsp, 0x01, 127);
  dsp_write(dsp, 0x03, 0x10); dsp_write(dsp, 0x05, 0); dsp_write(dsp, 0x07, 0x7f);
  dsp_write(dsp, 0x0c, 127); dsp_write(dsp, 0x1c, 127);
  dsp_write(dsp, 0x5d, 0x20); dsp_write(dsp, 0x5c, 0); dsp_write(dsp, 0x4c, 1);
  for (unsigned i = 0; i < 1024 + phase; ++i) {
    if (dsp_perClock()) dsp_clock(dsp); else dsp_cycle(dsp);
  }
  assert(dsp_available(dsp) > 0);
  bool audible = false;
  for (unsigned i = 0; i < dsp_available(dsp); ++i) {
    int16_t l, r; dsp_peek(dsp, i, &l, &r);
    audible |= l != 0 || r != 0;
  }
  if (!audible) {
    fprintf(stderr, "voice setup silent: per_clock=%d phase=%u samples=%u env=%d legacy_gain=%u mode=%u kon=%u kof=%u gain_reg=%u\n",
      dsp_perClock(), phase, dsp_available(dsp), dsp->clk.v[0].env,
      dsp->channel[0].gain, dsp->clk.v[0].mode, dsp->clk.kon, dsp->clk.kof, dsp->clk.regs[7]);
    return 2;
  }
  DspClock saved = dsp->clk;
  dsp_saveload(dsp, &snapshot->base);
  for (unsigned i = 0; i < 347; ++i) {
    if (dsp_perClock()) dsp_clock(dsp); else dsp_cycle(dsp);
  }
  DspClock expected = dsp->clk;
  DspOutputRing *output = malloc(sizeof(*output)); assert(output);
  dsp_output_ring_save(dsp, output);
  snapshot->position = 0; snapshot->loading = 1;
  dsp_saveload(dsp, &snapshot->base);
  if (memcmp(&saved, &dsp->clk, sizeof(saved))) {
    fprintf(stderr, "DSP clock snapshot lost simulation state: saved step=%u loaded=%u\n",
            saved.step, dsp->clk.step);
    return 1;
  }
  for (unsigned i = 0; i < 347; ++i) {
    if (dsp_perClock()) dsp_clock(dsp); else dsp_cycle(dsp);
  }
  assert(memcmp(&expected, &dsp->clk, sizeof(expected)) == 0);
  DspOutputRing *replayed = malloc(sizeof(*replayed)); assert(replayed);
  dsp_output_ring_save(dsp, replayed);
  assert(memcmp(output, replayed, sizeof(*output)) == 0);
  free(output); free(replayed);
  }
  /* An enabled native echo must also follow the selected output voices,
   * while canonical echo RAM and the synthesis pipeline remain unchanged. */
  dsp_write(dsp, 0x4d, 1); dsp_write(dsp, 0x2c, 127); dsp_write(dsp, 0x3c, 127);
  dsp_write(dsp, 0x0f, 64); dsp_write(dsp, 0x6d, 0x30); dsp_write(dsp, 0x7d, 1);
  dsp_write(dsp, 0x6c, 0);
  for (unsigned i = 0; i < 512; ++i) {
    if (dsp_perClock()) dsp_clock(dsp); else dsp_cycle(dsp);
  }
  snapshot->position = 0; snapshot->loading = 0;
  dsp_saveload(dsp, &snapshot->base);
  uint8_t *saved_ram = malloc(65536), *masked_ram = malloc(65536);
  assert(saved_ram && masked_ram); memcpy(saved_ram, ram, 65536);
  assert(dsp_set_output_voices(dsp, 0));
  dsp_trimSamples(dsp, 0);
  for (unsigned i = 0; i < 128; ++i) {
    if (dsp_perClock()) dsp_clock(dsp); else dsp_cycle(dsp);
  }
  assert(dsp_available(dsp));
  for (unsigned i = 0; i < dsp_available(dsp); ++i) {
    int16_t l, r; dsp_peek(dsp, i, &l, &r);
    if (l || r) { fprintf(stderr, "voice mask ignored: per_clock=%d PCM=%d/%d\n", dsp_perClock(), l, r); return 3; }
  }
  DspClock masked_clock = dsp->clk; memcpy(masked_ram, ram, 65536);
  snapshot->position = 0; snapshot->loading = 1;
  dsp_saveload(dsp, &snapshot->base); memcpy(ram, saved_ram, 65536);
  assert(dsp_set_output_voices(dsp, 0xff)); dsp_trimSamples(dsp, 0);
  for (unsigned i = 0; i < 128; ++i) {
    if (dsp_perClock()) dsp_clock(dsp); else dsp_cycle(dsp);
  }
  assert(!memcmp(&masked_clock, &dsp->clk, sizeof(masked_clock)));
  assert(!memcmp(masked_ram, ram, 65536));
  bool unfiltered_audible = false;
  for (unsigned i = 0; i < dsp_available(dsp); ++i) {
    int16_t l, r; dsp_peek(dsp, i, &l, &r); unfiltered_audible |= l || r;
  }
  assert(unfiltered_audible); free(saved_ram); free(masked_ram);
  dsp_free(dsp); free(ram); free(snapshot);
  puts("per-clock DSP snapshot and continuation PASS");
  return 0;
}
