/* Native APU time service. Included by apu.c after its private queue helper.
 *
 * The SPC core already executes an instruction atomically at its first clock.
 * Between instruction starts and queued CPU port writes, advance the DSP
 * and timers over the same interval. Preserve their order at each SPC access.
 * All state stays in Apu; no cached host pointer or unfinished job is saved.
 */
static uint32_t apu_native_remaining(uint8_t *ahead, uint32_t clocks) {
  /* The SPC bus access may already have advanced this device into the span. */
  const uint32_t advanced = *ahead < clocks ? *ahead : clocks;
  *ahead -= (uint8_t)advanced;
  return clocks - advanced;
}

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
  const bool per_clock = dsp_perClock();
  while (clocks) {
    apu_drainPortQueue(apu);
    if (!apu->cpuCyclesLeft) {
      if (apu_holdOpcode(apu)) { apu->held = true; return; }
#if SNESRECOMP_SPC_DIAGNOSTICS
      g_spc_pc_histogram[apu->spc->pc]++;
      if (apu->spc->pc > g_spc_pc_max_seen)
        g_spc_pc_max_seen = apu->spc->pc;
#endif
      apu->cpuCyclesLeft = spc_runOpcode(apu->spc);
    }
    uint32_t span = apu->cpuCyclesLeft;
    if (!per_clock) {
      const unsigned sample_phase = apu->cycles & 31u;
      if (!sample_phase) dsp_cycle(apu->dsp);
      const unsigned to_sample = 32u - sample_phase;
      if (span > to_sample) span = to_sample;
    }
    if (span > clocks) span = clocks;
    if (apu->portQHead != apu->portQTail) {
      const ApuPortWrite *write =
          &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)];
      uint64_t distance = write->target_cycle - apu->portClock;
      if (span > distance) span = (uint32_t)distance;
    }
    if (per_clock) {
      const uint32_t remaining = apu_native_remaining(&apu->dspAhead, span);
      for (uint32_t clock = 0; clock < remaining; clock++) dsp_clock(apu->dsp);
    }
    for (unsigned timer = 0; timer < 3; timer++) {
      const uint32_t remaining = apu_native_remaining(&apu->timerAhead[timer], span);
      apu_native_timer(&apu->timer[timer], remaining, timer == 2 ? 16u : 128u, timer == 0);
    }
    apu->cpuCyclesLeft -= (uint8_t)span;
    apu->cycles += span;
    apu->portClock += span;
    clocks -= span;
  }
}
