/* Real dma.c + snes.c CPU synchronization. Only unrelated peripherals are
 * stubbed by the established HDMA fixture. No field-start pricing snapshot. */
#define main hdma_peripheral_fixture_main
#include "hdma_timing_test.c"
#undef main

#ifdef SNES_TEST_OLD_BASELINE
#define snes_sync_master_clock_parked snes_sync_master_clock
#define snes_sync_master_clock_boundary snes_sync_master_clock
#endif

static unsigned failures;
static uint64_t charged;
#define CHECK(test, message) do { failures += check((test), (message)); } while (0)

/* Same clock ownership as SnesInit's real hook, including its eager sync.
 * The outer observer must suppress recursion and retain pending IRQ debt. */
static void charge_cpu(Snes *snes, uint64_t clocks) {
  charged += clocks;
  g_cpu.master_cycles += clocks;
  snes_sync_master_clock(snes, g_cpu.master_cycles);
}

static void setup(Snes *s, Dma *d) {
  memset(s, 0, sizeof(*s)); memset(d, 0, sizeof(*d));
  memset(ram, 0, sizeof(ram)); memset(ppu_regs, 0, sizeof(ppu_regs));
  memset(&g_cpu, 0, sizeof(g_cpu));
  d->snes = s; s->dma = d; s->ram = ram;
  s->vPos = 20; s->beamMasterLast = g_cpu.master_cycles = 10000;
  charged = 0; ppu_observed_machine = s; ppu_observed_writes = 0;
  snes_set_master_clock_charge_hook(charge_cpu);
}

static void channel(Dma *d, int c, uint8_t mode, uint16_t table, uint8_t target) {
  uint16_t reg = (uint16_t)(0x4300 + c * 16);
  dma_write(d, reg, mode); dma_write(d, reg + 1, target);
  dma_write(d, reg + 2, table & 255); dma_write(d, reg + 3, table >> 8);
  dma_write(d, reg + 4, 0x7e); dma_write(d, reg + 7, 0x7e);
}

static uint64_t execute(Snes *s, uint32_t clocks) {
  g_cpu.master_cycles += clocks;
  uint64_t before = g_cpu.master_cycles;
  snes_sync_master_clock(s, g_cpu.master_cycles);
  return g_cpu.master_cycles - before;
}

static uint64_t edge(Snes *s) {
  uint32_t distance = s->hPos < 1104 ? 1104u - s->hPos
                                    : 1364u - s->hPos + 1104u;
  return execute(s, distance);
}

int main(void) {
  Snes s; Dma d;
  setup(&s, &d);
  ram[0x100] = 0x82; ram[0x101] = 0x11; ram[0x102] = 0x22;
  channel(&d, 0, 0, 0x100, 0x26);
  dma_startDma(&d, 1, true); dma_initHdma(&d);
  CHECK(execute(&s, 1103) == 0 && !ppu_observed_writes,
        "no transfer or CPU charge before the H=1104 DMA request");
  CHECK(execute(&s, 1) == 34, "CPU observer pays one-byte live transfer work");
  CHECK(ppu_regs[0x26] == 0x11 && d.channel[0].tableAdr == 0x102,
        "live channel transferred once, with no recursive second consume");
  CHECK(ppu_observed_line == 20 && ppu_observed_h == 1104,
        "PPU transfer observes its actual beam line and edge");
  CHECK(s.hPos == 1138 && s.beamMasterLast == g_cpu.master_cycles,
        "CPU and beam both include the stall before the next instruction");
  CHECK(!s.cpuClockSyncing, "CPU observer recursion guard unwound");

  /* A CPU rewrites both channel shape and its still-live source table. The
   * next event must see four bytes and price four, not yesterday's one. */
  dma_write(&d, 0x4300, 4); dma_write(&d, 0x430a, 0x81);
  ram[0x102] = 0x55; ram[0x103] = 0x66;
  ram[0x104] = 0x77; ram[0x105] = 0x88; ram[0x106] = 0;
  CHECK(edge(&s) == 58, "mid-field channel mode changes actual stall work");
  CHECK(ppu_regs[0x26] == 0x55 && ppu_regs[0x27] == 0x66 &&
        ppu_regs[0x28] == 0x77 && ppu_regs[0x29] == 0x88 &&
        d.channel[0].tableAdr == 0x107 && d.channel[0].terminated,
        "mid-field table writes and terminal descriptor are consumed live");
  CHECK(edge(&s) == 0, "terminated channels do not continue taxing the CPU");

  /* Late enable: the first slot fetches an indirect descriptor only. Then a
   * mid-field pointer rewrite changes the next reload and following payload. */
  channel(&d, 1, 0x40, 0x200, 0x2a);
  ram[0x200] = 0x81; ram[0x201] = 0; ram[0x202] = 4;
  ram[0x203] = 0x81; ram[0x204] = 0; ram[0x205] = 5; ram[0x206] = 0;
  ram[0x400] = 0xa1; ram[0x600] = 0xb2;
  dma_startDma(&d, 3, true);
  unsigned writes = ppu_observed_writes;
  CHECK(edge(&s) == 42 && ppu_observed_writes == writes,
        "late indirect enable prices descriptor/address reads without payload");
  ram[0x205] = 6;
  CHECK(edge(&s) == 50 && ppu_regs[0x2a] == 0xa1 && d.channel[1].size == 0x600,
        "indirect reload prices and reads the changed live pointer");
  CHECK(edge(&s) == 34 && ppu_regs[0x2a] == 0xb2 && d.channel[1].terminated,
        "next line transfers from the changed indirect source");

  setup(&s, &d);
  ram[0x100] = 2; ram[0x101] = 0x33; ram[0x102] = 0;
  channel(&d, 0, 0, 0x100, 0x26);
  dma_startDma(&d, 1, true); dma_initHdma(&d);
  CHECK(edge(&s) == 34, "non-repeat table transfers its first line");
  writes = ppu_observed_writes;
  CHECK(edge(&s) == 26 && ppu_observed_writes == writes,
        "non-repeat hold line pays channel slot but transfers no bytes");

  setup(&s, &d);
  ram[0x100] = 0x81; ram[0x101] = 0x44; ram[0x102] = 0;
  channel(&d, 0, 0, 0x100, 0x26);
  dma_startDma(&d, 1, true); dma_initHdma(&d);
  s.hPos = 1090;
  g_cpu.master_cycles += 14;
  uint64_t parked = g_cpu.master_cycles;
  snes_sync_master_clock_parked(&s, parked);
  CHECK(ppu_regs[0x26] == 0x44 && charged == 0 && g_cpu.master_cycles == parked && s.hPos == 1104,
        "WAI/idle physical time executes HDMA without charging waiting twice");

  setup(&s, &d);
  ram[0x100] = 0x81; ram[0x101] = 0x44; ram[0x102] = 0;
  channel(&d, 0, 0, 0x100, 0x26);
  dma_startDma(&d, 1, true); dma_initHdma(&d);
  s.hPos = 1090; s.hIrqEnabled = true; s.hTimer = 278; /* H=1112 */
  CHECK(execute(&s, 14) == 34 && s.inIrq && s.hPos == 1113,
        "IRQ during HDMA stall latches without dropping CPU clocks or debt");
  CHECK(g_cpu.master_cycles - s.beamMasterLast == 25 && charged == 34,
        "IRQ preemption retains the remaining physical-time debt");
  s.hIrqEnabled = false; s.inIrq = false;
  snes_sync_master_clock(&s, g_cpu.master_cycles);
  CHECK(s.hPos == 1138 && s.beamMasterLast == g_cpu.master_cycles && charged == 34 &&
        ppu_observed_writes == 1, "resume pays debt without another transfer or CPU charge");

  setup(&s, &d);
  ram[0x100] = 0x81; ram[0x101] = 0x44; ram[0x102] = 0;
  channel(&d, 0, 0, 0x100, 0x26);
  dma_startDma(&d, 1, true); dma_initHdma(&d);
  s.hPos = 1090; g_cpu.master_cycles += 14;
  uint64_t boundary = g_cpu.master_cycles;
  snes_sync_master_clock_boundary(&s, boundary);
  CHECK(charged == 34 && s.hPos == 1104 && g_cpu.master_cycles == boundary + 34,
        "field-boundary settlement charges work without walking past the boundary");
  snes_sync_master_clock(&s, g_cpu.master_cycles);
  CHECK(s.hPos == 1138 && charged == 34 && ppu_observed_writes == 1,
        "next CPU observer consumes carried field debt exactly once");

  setup(&s, &d);
  s.vPos = 261; s.hPos = 1360;
  ram[0x100] = 0x81; ram[0x101] = 0x44; ram[0x102] = 0;
  channel(&d, 0, 0, 0x100, 0x26); dma_startDma(&d, 1, true);
  CHECK(execute(&s, 4) == 26 && s.vPos == 0 && s.hPos == 26 && d.channel[0].tableAdr == 0x101,
        "field initialization charges the actual descriptor read");
  snes_set_master_clock_charge_hook(NULL);
  if (failures) return 1;
  puts("HDMA CPU observer: PASS live modes/tables/indirect reload, termination, WAI, IRQ debt and init");
  return 0;
}
