#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp1.h"
#include "saveload.h"

enum {
  kDrc = 0x04,
  kDrs = 0x10,
  kRqm = 0x80,
};

typedef struct MemoryState {
  SaveLoadInfo base;
  uint8_t data[8192];
  size_t position;
  int loading;
  int failed;
} MemoryState;

static void memory_state_transfer(SaveLoadInfo *sli, void *data, size_t size) {
  MemoryState *state = (MemoryState *)sli;
  if (state->position + size > sizeof(state->data)) {
    state->failed = 1;
    return;
  }
  if (state->loading)
    memcpy(data, state->data + state->position, size);
  else
    memcpy(state->data + state->position, data, size);
  state->position += size;
}

static int check(int condition, const char *message) {
  if (condition) return 0;
  fprintf(stderr, "FAIL: %s\n", message);
  return 1;
}

static uint64_t master_clock;

static uint8_t status(Dsp1 *d) { return dsp1_read(d, 1); }

static int wait_rqm(Dsp1 *d) {
  for (unsigned i = 0; i < 100000; i++) {
    if (status(d) & kRqm) return 1;
    master_clock += 16;
    dsp1_sync(d, master_clock);
  }
  return 0;
}

static int write_command(Dsp1 *d, uint8_t command) {
  if (!wait_rqm(d)) return 0;
  uint8_t sr = status(d);
  if ((sr & (kRqm | kDrc)) != (kRqm | kDrc)) return 0;
  dsp1_write(d, 0, command);
  return 1;
}

static int write_word(Dsp1 *d, uint16_t value) {
  if (!wait_rqm(d)) return 0;
  uint8_t sr = status(d);
  if (!(sr & kRqm) || (sr & kDrc)) return 0;
  dsp1_write(d, 0, (uint8_t)value);
  if (!(status(d) & kDrs)) return 0;
  dsp1_write(d, 0, (uint8_t)(value >> 8));
  return !(status(d) & kDrs);
}

static int read_word(Dsp1 *d, uint16_t *value) {
  if (!wait_rqm(d)) return 0;
  uint8_t sr = status(d);
  if (!(sr & kRqm) || (sr & kDrc)) return 0;
  uint8_t low = dsp1_read(d, 0);
  if (!(status(d) & kDrs)) return 0;
  uint8_t high = dsp1_read(d, 0);
  if (status(d) & kDrs) return 0;
  *value = (uint16_t)(low | ((uint16_t)high << 8));
  return 1;
}

static int activate_hle(Dsp1 *d) {
  return !dsp1_load_firmware(d, "missing.sfc") &&
         !dsp1_firmware_loaded(d) && dsp1_hle_active(d);
}

int main(void) {
  int fails = 0;
  Dsp1 *source = dsp1_create();
  Dsp1 *restored = dsp1_create();
  fails += check(source && restored, "create DSP-1 instances");
  if (!source || !restored) return 1;
  fails += check(activate_hle(source), "HLE build ignores firmware inputs");
  fails += check(activate_hle(restored), "restored instance activates HLE");

  fails += check(write_command(source, 0x00), "submit multiply command");
  fails += check(write_word(source, 0x4000), "submit first multiply word");
  fails += check(wait_rqm(source), "wait for second multiply parameter");
  dsp1_write(source, 0, 0x00);
  fails += check((status(source) & kDrs) != 0,
                 "partial parameter word sets byte phase");

  MemoryState memory = {{memory_state_transfer, NULL}, {0}, 0, 0, 0};
  dsp1_saveload(source, &memory.base);
  fails += check(!memory.failed, "save in-flight HLE command");
  memory.position = 0;
  memory.loading = 1;
  dsp1_saveload(restored, &memory.base);
  fails += check(!memory.failed, "restore in-flight HLE command");

  dsp1_write(restored, 0, 0x40);
  fails += check(!(status(restored) & kRqm),
                 "HLE compute keeps RQM low until its delay expires");
  uint16_t result = 0;
  fails += check(read_word(restored, &result) && result == 0x2000,
                 "restored multiply completes exactly");
  fails += check(wait_rqm(restored) &&
                     (status(restored) & (kRqm | kDrc)) ==
                         (kRqm | kDrc),
                 "ordinary output returns to command mode");

  fails += check(write_command(restored, 0x00),
                 "submit same-timestamp multiply command");
  dsp1_write(restored, 0, 0x00);
  dsp1_write(restored, 0, 0x40);
  dsp1_write(restored, 0, 0x00);
  dsp1_write(restored, 0, 0x40);
  fails += check(!(status(restored) & kRqm),
                 "same-timestamp multiply still schedules compute delay");
  uint8_t direct_low = dsp1_read(restored, 0);
  uint8_t direct_high = dsp1_read(restored, 0);
  fails += check((uint16_t)(direct_low | ((uint16_t)direct_high << 8)) ==
                     0x2000,
                 "AOT access spacing makes direct result reads coherent");
  fails += check(wait_rqm(restored), "finish direct multiply output");

  const uint16_t projection[7] = {
      0x0880, 0x27a0, 0x0000, 0x0040, 0x0100, 0x0000, 0x3400};
  const uint16_t projection_expected[4] = {
      0x0000, 0xffb2, 0x0880, 0x27a3};
  fails += check(write_command(restored, 0x02),
                 "submit projection command");
  for (unsigned i = 0; i < 7; i++)
    fails += check(write_word(restored, projection[i]),
                   "submit projection parameter");
  for (unsigned i = 0; i < 4; i++) {
    result = 0;
    fails += check(read_word(restored, &result) &&
                       result == projection_expected[i],
                   "read projection result");
  }

  const uint16_t raster_expected[4] = {
      0x05ff, 0x0000, 0x0000, 0x14aa};
  fails += check(write_command(restored, 0x0a),
                 "submit continuous raster command");
  fails += check(write_word(restored, 0xffb6), "submit raster start");
  for (unsigned i = 0; i < 4; i++) {
    result = 0;
    fails += check(read_word(restored, &result) &&
                       result == raster_expected[i],
                   "read first raster matrix");
  }
  for (unsigned byte = 0; byte < 8; byte++) {
    if (!(byte & 1u))
      fails += check(wait_rqm(restored), "wait to discard raster word");
    dsp1_write(restored, 0, 0x80);
  }
  fails += check(wait_rqm(restored) &&
                     (status(restored) & (kRqm | kDrc)) ==
                         (kRqm | kDrc),
                 "eight writes terminate continuous raster output");
  fails += check(dsp1_command_count(restored, 0x0a) == 1 &&
                     dsp1_command_count(restored, 0x80) == 0,
                 "discard writes are not counted as commands");
  fails += check(write_command(restored, 0x80),
                 "stream terminator is accepted in command mode");
  fails += check(dsp1_command_count(restored, 0x80) == 1,
                 "command-mode stream terminator is counted");

  fails += check(write_command(restored, 0x7f),
                 "unsupported command reaches fail-loud boundary");
  fails += check(dsp1_hle_failed(restored) &&
                     dsp1_hle_failed_command(restored) == 0x7f &&
                     !(status(restored) & kRqm),
                 "unsupported command stops HLE without fabricated output");

  dsp1_destroy(source);
  dsp1_destroy(restored);
  if (fails) return 1;
  puts("dsp1_hle_host_test: PASS");
  return 0;
}
