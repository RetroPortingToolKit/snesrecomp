/* Native APU time service. Included by apu.c after its private queue helper.
 *
 * The SPC core already executes an instruction atomically at its first clock.
 * Between instruction starts, DSP sample edges and queued CPU port writes,
 * timer advancement is the only work. Advance that interval as a native job.
 * Preserve the reference order at an edge: ports, SPC, DSP, then timers.
 * All state stays in Apu; no cached host pointer or unfinished job is saved.
 */
static void apu_native_timer(Timer *timer, uint32_t clocks, unsigned period,
                             int first_timer) {
  if (clocks <= timer->cycles) {
    timer->cycles -= (uint8_t)clocks;
    return;
  }
  clocks -= timer->cycles;
  do {
    if (timer->enabled) {
      timer->divider++;
      if (timer->divider == timer->target) {
        timer->divider = 0;
        timer->counter = (timer->counter + 1u) & 15u;
        if (first_timer) {
          extern uint64_t g_apu_timer0_total_ticks;
          g_apu_timer0_total_ticks++;
        }
      }
    }
    if (clocks <= period) {
      timer->cycles = (uint8_t)(period - clocks);
      return;
    }
    clocks -= period;
  } while (1);
}

static void apu_runCycles_native(Apu *apu, uint32_t clocks) {
  while (clocks) {
    apu_drainPortQueue(apu);
    if (!apu->cpuCyclesLeft) {
#if SNESRECOMP_SPC_DIAGNOSTICS
      g_spc_pc_histogram[apu->spc->pc]++;
      if (apu->spc->pc > g_spc_pc_max_seen)
        g_spc_pc_max_seen = apu->spc->pc;
#endif
      apu->cpuCyclesLeft = spc_runOpcode(apu->spc);
    }
    unsigned sample_phase = apu->cycles & 31u;
    if (!sample_phase) dsp_cycle(apu->dsp);
    uint32_t span = apu->cpuCyclesLeft;
    unsigned to_sample = 32u - sample_phase;
    if (span > to_sample) span = to_sample;
    if (span > clocks) span = clocks;
    if (apu->portQHead != apu->portQTail) {
      const ApuPortWrite *write =
          &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)];
      uint64_t distance = write->target_cycle - apu->portClock;
      if (span > distance) span = (uint32_t)distance;
    }
    apu_native_timer(&apu->timer[0], span, 128u, 1);
    apu_native_timer(&apu->timer[1], span, 128u, 0);
    apu_native_timer(&apu->timer[2], span, 16u, 0);
    apu->cpuCyclesLeft -= (uint8_t)span;
    apu->cycles += span;
    apu->portClock += span;
    clocks -= span;
  }
}
