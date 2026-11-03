/* Exercise the actual production implementations, including MMIO observers. */
#define SNESRECOMP_SA1_TIMER_TEST 1
#ifndef SNESRECOMP_SA1_TIMER_NATIVE
#define SNESRECOMP_SA1_TIMER_NATIVE 1
#endif
#include "../../runner/src/snes/sa1.c"
#include <stdio.h>

static uint32_t seed = 0x5a108166u;
static uint64_t cases;
typedef struct Snapshot {
  SaveLoadInfo io;
  unsigned char bytes[16384];
  size_t position;
  bool load;
} Snapshot;
static void snapshot_data(SaveLoadInfo *io, void *data, size_t size) {
  Snapshot *snapshot = (Snapshot *)io;
  if (size > sizeof(snapshot->bytes) - snapshot->position) exit(11);
  if (snapshot->load) memcpy(data, snapshot->bytes + snapshot->position, size);
  else memcpy(snapshot->bytes + snapshot->position, data, size);
  snapshot->position += size;
}
static uint32_t random32(void) {
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
  return seed;
}

static void compare(const Sa1 *initial, uint32_t clocks) {
  Sa1 reference = *initial, native = *initial;
  timer_advance_reference(&reference, clocks);
  timer_advance_native(&native, clocks);
  ++cases;
  if (memcmp(&reference, &native, sizeof(reference))) {
    fprintf(stderr, "timer mismatch case=%llu linear=%d enables=%d%d "
        "h=%u v=%u target=%u,%u clocks=%u expected=%u,%u,%d got=%u,%u,%d\n",
        (unsigned long long)cases, initial->io.timer_linear,
        initial->io.timer_h_enable, initial->io.timer_v_enable,
        initial->h_counter, initial->v_counter,
        initial->io.h_count_target, initial->io.v_count_target, clocks,
        reference.h_counter, reference.v_counter, reference.io.timer_irq_flag,
        native.h_counter, native.v_counter, native.io.timer_irq_flag);
    exit(1);
  }
  /* $2302 latches both counters, and subsequent reads keep the latched copy. */
  for (unsigned address = 0x2301; address <= 0x2305; ++address)
    if (io_read_sa1(&reference, address) != io_read_sa1(&native, address)) exit(2);
}

static void interrupt_boundary(void) {
  uint8_t rom[0x10000] = {0}, bwram[0x2000] = {0};
  rom[0] = rom[1] = rom[0x10] = 0xea; /* NOP and IRQ handler. */
  Sa1 *chip = sa1_create(rom, sizeof(rom), bwram, sizeof(bwram));
  if (!chip) exit(5);
  sa1_cpu_write(chip, 0, 0x2203, 0x00);
  sa1_cpu_write(chip, 0, 0x2204, 0x80);
  sa1_cpu_write(chip, 0, 0x2207, 0x10);
  sa1_cpu_write(chip, 0, 0x2208, 0x80);
  sa1_cpu_write(chip, 0, 0x2200, 0);
  chip->cpu->i = false;
  io_write_sa1(chip, 0x2210, 1); /* H-only, target four master clocks. */
  io_write_sa1(chip, 0x2212, 1);
  sa1_sync(chip, 1);
  if (chip->cpu->pc != 0x8001 || !chip->io.timer_irq_flag ||
      chip->cpu->irqWanted || chip->master_clock != 4) exit(6);
  Snapshot snapshot = {.io = {.func = snapshot_data}};
  sa1_saveload(chip, &snapshot.io);
  chip->h_counter = 999;
  chip->io.timer_irq_flag = false;
  snapshot.position = 0;
  snapshot.load = true;
  sa1_saveload(chip, &snapshot.io);
  if (chip->h_counter != 4 || !chip->io.timer_irq_flag ||
      chip->cpu->pc != 0x8001 || chip->master_clock != 4) exit(12);
  /* Disabled delivery never suppresses the pending latch. Enabling it is
   * observed at the next instruction, not during the preceding charge. */
  io_write_sa1(chip, 0x220a, 0x40);
  sa1_sync(chip, 5);
  if (chip->cpu->pc != 0x8010 || !chip->cpu->i ||
      !chip->io.timer_irq_flag) exit(7);
  io_write_sa1(chip, 0x220b, 0x40);
  if (io_read_sa1(chip, 0x2301) & 0x40) exit(8);
  io_write_sa1(chip, 0x2211, 0);
  if (io_read_sa1(chip, 0x2302) || io_read_sa1(chip, 0x2304)) exit(9);
  chip->io.sa1_ready = true;
  uint64_t clock = chip->master_clock;
  sa1_sync(chip, clock + 4);
  if (chip->h_counter != 4 || !chip->io.timer_irq_flag) exit(10);
  sa1_destroy(chip);
}

int main(void) {
  static const uint32_t hs[] = {0,1,3,4,1359,1360,1363,1364,1365,
      2043,2044,2047,2048,65535,UINT32_MAX-1,UINT32_MAX};
  static const uint16_t vs[] = {0,1,260,261,262,263,510,511,512,65534,65535};
  static const uint16_t ht[] = {0,1,2,339,340,341,342,511,512,65535};
  static const uint16_t vt[] = {0,1,260,261,262,511,512,65535};
  static const uint32_t cs[] = {0,1,2,3,4,8,17,1363,1364,2048,4097};
  Sa1 state = {0};
  for (unsigned mode = 0; mode < 2; ++mode)
    for (unsigned enables = 0; enables < 4; ++enables)
      for (unsigned h = 0; h < sizeof(hs)/sizeof(*hs); ++h)
        for (unsigned v = 0; v < sizeof(vs)/sizeof(*vs); ++v)
          for (unsigned target = 0; target < sizeof(ht)/sizeof(*ht); ++target) {
            state.io.timer_linear = mode;
            state.io.timer_h_enable = enables & 1;
            state.io.timer_v_enable = enables & 2;
            state.h_counter = hs[h]; state.v_counter = vs[v];
            state.io.h_count_target = ht[target];
            state.io.v_count_target = vt[(h+v+target)%8];
            state.io.timer_irq_flag = (target & 1) != 0;
            state.io.timer_irq_enable = (target & 2) != 0;
            for (unsigned c = 0; c < sizeof(cs)/sizeof(*cs); ++c)
              compare(&state, cs[c]);
          }

  for (unsigned i = 0; i < 200000; ++i) {
    state.io.timer_linear = random32() & 1;
    state.io.timer_h_enable = random32() & 1;
    state.io.timer_v_enable = random32() & 1;
    state.io.timer_irq_flag = (random32() & 7) == 0;
    state.io.timer_irq_enable = random32() & 1;
    uint32_t period = state.io.timer_linear ? 2048 : 1364;
    state.h_counter = random32() % 4096;
    state.v_counter = (uint16_t)random32();
    state.io.h_count_target = (uint16_t)(random32() % 1024);
    state.io.v_count_target = (uint16_t)(random32() % 1024);
    if (!(i & 3)) {
      state.io.h_count_target = (uint16_t)((state.h_counter + 4) / 4);
      state.io.v_count_target = state.v_counter;
    }
    compare(&state, random32() % 96);
    if (!(i % 512)) compare(&state, period * (600 + random32() % 512));
  }

  /* Carry state across actual mode/target/clear/reset writes. */
  Sa1 reference = {0}, native = {0};
  for (unsigned i = 0; i < 50000; ++i) {
    uint16_t address = 0x2210 + random32() % 6;
    uint8_t value = (uint8_t)random32();
    io_write_sa1(&reference, address, value);
    io_write_sa1(&native, address, value);
    if (!(i & 15)) {
      io_write_sa1(&reference, 0x220b, 0x40);
      io_write_sa1(&native, 0x220b, 0x40);
    }
    uint32_t clocks = random32() % 5000;
    timer_advance_reference(&reference, clocks);
    timer_advance_native(&native, clocks);
    ++cases;
    if (memcmp(&reference, &native, sizeof(reference))) return 3;
    if (io_read_sa1(&reference, 0x2302) != io_read_sa1(&native, 0x2302)) return 4;
  }
  printf("SA-1 timer span differential: %llu cases passed\n",
         (unsigned long long)cases);
  interrupt_boundary();
  puts("SA-1 timer instruction-boundary IRQ/MMIO/ready checks passed");
  return 0;
}
