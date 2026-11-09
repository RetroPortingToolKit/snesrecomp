
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


#include "apu.h"
#include "snes.h"
#include "spc.h"
#include "dsp.h"
#include "../audio_trace.h"

#ifndef SNESRECOMP_TRACE
#define SNESRECOMP_TRACE 0
#endif

#ifndef SNESRECOMP_SPC_DIAGNOSTICS
#define SNESRECOMP_SPC_DIAGNOSTICS SNESRECOMP_TRACE
#endif

#ifndef SNESRECOMP_APU_NATIVE
#define SNESRECOMP_APU_NATIVE 0
#endif
#if SNESRECOMP_APU_NATIVE != 0 && SNESRECOMP_APU_NATIVE != 1
#error SNESRECOMP_APU_NATIVE must be 0 or 1
#endif

static const uint8_t bootRom[0x40] = {
  0xcd, 0xef, 0xbd, 0xe8, 0x00, 0xc6, 0x1d, 0xd0, 0xfc, 0x8f, 0xaa, 0xf4, 0x8f, 0xbb, 0xf5, 0x78,
  0xcc, 0xf4, 0xd0, 0xfb, 0x2f, 0x19, 0xeb, 0xf4, 0xd0, 0xfc, 0x7e, 0xf4, 0xd0, 0x0b, 0xe4, 0xf5,
  0xcb, 0xf4, 0xd7, 0x00, 0xfc, 0xd0, 0xf3, 0xab, 0x01, 0x10, 0xef, 0x7e, 0xf4, 0x10, 0xeb, 0xba,
  0xf6, 0xda, 0x00, 0xba, 0xf4, 0xc4, 0xf4, 0xdd, 0x5d, 0xd0, 0xdb, 0x1f, 0x00, 0x00, 0xc0, 0xff
};

/* SNESRECOMP_SPC_ACCESS_CYCLES=0 restores instruction-start port timing.
 * SNESRECOMP_SPC_READ_BIAS / _WRITE_BIAS shift the cycle a port access is
 * taken to land on (SPC cycles, default 0). */
static int s_access_cycles = -1;
static int apu_accessCycles(void);
/* One SPC cycle of timer i, as apu_cycle runs it. */
static void apu_timerStep(Apu* apu, int i) {
  extern uint64_t g_apu_timer0_total_ticks;
  if(apu->timer[i].cycles == 0) {
    apu->timer[i].cycles = i == 2 ? 16 : 128;
    if(apu->timer[i].enabled) {
      apu->timer[i].divider++;
      if(apu->timer[i].divider == apu->timer[i].target) {
        apu->timer[i].divider = 0;
        apu->timer[i].counter++;
        apu->timer[i].counter &= 0xf;
        if (i == 0) g_apu_timer0_total_ticks++;
      }
    }
  }
  apu->timer[i].cycles--;
}
/* A timer access on cycle `k` of the running opcode sees the timers as they
 * are there: advance them to it now; apu_cycle skips what was done ahead. */
static void apu_timersTo(Apu* apu, uint8_t k) {
  if (!apu_accessCycles()) return;
  for (int i = 0; i < 3; i++)
    while (apu->timerAhead[i] < k) { apu_timerStep(apu, i); apu->timerAhead[i]++; }
}
/* A $F3 access on cycle `k` of the running opcode sees the per-clock S-DSP
 * as it is there (MesenCE steps it before each bus cycle's access). */
static void apu_dspTo(Apu* apu, uint8_t k) {
  if (!dsp_perClock() || !apu_accessCycles()) return;
  while (apu->dspAhead < k) { dsp_clock(apu->dsp); apu->dspAhead++; }
}
/* SNESRECOMP_PORT_LOG=1: every SPC access to $F4 on stderr, "[pl] S<R|W> <val> <spc cycle>". */
int g_apu_port_log = -1;
/* SNESRECOMP_DSP_LOG=1: every SPC access to $F3, "[pl] SD|SE <val> <spc cycle> <dsp reg>". */
static int s_dsp_log = -1;
static int apu_dspLog(void) { if (s_dsp_log < 0) s_dsp_log = getenv("SNESRECOMP_DSP_LOG") != NULL; return s_dsp_log; }
static void apu_portLog(Apu* apu, char rw, uint8_t v, unsigned port) {
  if (g_apu_port_log < 0) g_apu_port_log = getenv("SNESRECOMP_PORT_LOG") != NULL;
  if (g_apu_port_log)
    fprintf(stderr, "[pl] S%c %02X %llu %04X %02X %u %u\n", rw, v,
            (unsigned long long)(apu->portClock + apu->spc->accessCycle),
            apu->spc->opPc, apu->spc->op, apu->spc->accessCycle, port);
}
static int s_read_bias, s_write_bias;
static int apu_accessCycles(void) {
  if (s_access_cycles < 0) {
    const char *e = getenv("SNESRECOMP_SPC_ACCESS_CYCLES");
    s_access_cycles = !(e && e[0] == '0');
    e = getenv("SNESRECOMP_SPC_READ_BIAS");
    s_read_bias = e ? atoi(e) : 0;
    e = getenv("SNESRECOMP_SPC_WRITE_BIAS");
    s_write_bias = e ? atoi(e) : 0;
  }
  return s_access_cycles;
}

/* Before an opcode starts at portClock: true if it must wait, because one of
 * its port accesses lands after the clock the CPU has reached. */
static bool apu_holdOpcode(Apu* apu) {
  if (!apu->holdActive) return false;
  if (apu->portClock + 16u <= apu->holdTarget) return false;  /* fits whole */
  int c = spc_probePortCycle(apu->spc);
  if (c < 0) return false;
  int bias = s_read_bias > s_write_bias ? s_read_bias : s_write_bias;
  int64_t at = (int64_t)apu->portClock + c + bias;
  return at > (int64_t)apu->holdTarget;
}

Apu* apu_init(void) {
  Apu* apu = calloc(1, sizeof(Apu));  /* zero padding: saveload/co-sim hash determinism */
  apu->spc = spc_init(apu);
  apu->dsp = dsp_init(apu->ram);
  apu_clearPortQueue(apu);
  return apu;
}

void apu_free(Apu* apu) {
  spc_free(apu->spc);
  dsp_free(apu->dsp);
  free(apu);
}

void apu_reset(Apu* apu) {
  apu->romReadable = true; // before resetting spc, because it reads reset vector from it
  spc_reset(apu->spc);
  dsp_reset(apu->dsp);
  memset(apu->ram, 0, sizeof(apu->ram));
  apu->dspAdr = 0;
  apu->cycles = 0;
  memset(apu->inPorts, 0, sizeof(apu->inPorts));
  memset(apu->outPorts, 0, sizeof(apu->outPorts));
  /* MesenCE's prescaler toggles every half period and clocks the stage-2
   * counter on the falling edge: a timer ticks on the last cycle of each
   * 128 (16) cycle period from power-on, not on the first. */
  const char *tp = getenv("SNESRECOMP_SPC_TIMER_PHASE");
  int timer_phase = tp ? atoi(tp) : (apu_accessCycles() ? 1 : 0);
  for(int i = 0; i < 3; i++) {
    apu->timer[i].cycles = timer_phase ? (i == 2 ? 16 : 128) - timer_phase : 0;
    apu->timer[i].divider = 0;
    apu->timer[i].target = 0;
    apu->timer[i].counter = 0;
    apu->timer[i].enabled = false;
  }
  /* Access-exact timing: the IPL's first $F4 access lands on SPC cycle 2400
   * from power-on (MesenCE), which a 3-cycle reset gives this core. */
  apu->cpuCyclesLeft = apu_accessCycles() ? 3 : 7;
  apu->portClock = 0;
  apu->holdActive = false;
  apu->held = false;
  apu->holdTarget = 0;
  memset(apu->timerAhead, 0, sizeof(apu->timerAhead));
  apu->dspAhead = 0;
  apu_clearPortQueue(apu);
}

void apu_clearPortQueue(Apu* apu) {
  apu->portQHead = apu->portQTail = 0;
  apu->portGuestAnchor = 0;
  apu->portTargetAnchor = 0;
  apu->portLastGuest = 0;
  apu->portLastTarget = 0;
  apu->portTimeValid = false;
}

void apu_writePortNow(Apu* apu, uint8_t port, uint8_t val) {
  port &= 3;
  apu->inPorts[port] = val;
  audio_trace_on_cpu_port_apply(port, val);
}

static void apu_applyPortWrite(Apu* apu, const ApuPortWrite *w) {
  apu_writePortNow(apu, w->port, w->val);
}

uint32_t apu_portQueueDepth(const Apu* apu) {
  return apu->portQTail - apu->portQHead;
}

bool apu_schedulePortWrite(Apu* apu, uint8_t port, uint8_t val,
                           uint64_t guest_cycle) {
  if (apu_portQueueDepth(apu) >= APU_PORT_QUEUE_LEN)
    return false;

  /* Establish a correspondence between guest time and wherever the
   * callback-driven SPC is now. If the callback later runs ahead, rebase at
   * the new current point; future guest deltas are still preserved. */
  if (!apu->portTimeValid || guest_cycle < apu->portLastGuest) {
    apu->portGuestAnchor = guest_cycle;
    apu->portTargetAnchor = apu->portClock;
    apu->portTimeValid = true;
  }

  uint64_t target = apu->portTargetAnchor +
                    (guest_cycle - apu->portGuestAnchor);
  if (target < apu->portClock) {
    /* The SPC ran past the write's time inside an opcode that touched no port
     * (access-exact timing holds the others), so nothing saw the port in
     * between: apply it now and keep the mapping. Only a real lead (callback
     * audio) moves the mapping. */
    if (!apu_accessCycles() || apu->portClock - target > 64u) {
      apu->portGuestAnchor = guest_cycle;
      apu->portTargetAnchor = apu->portClock;
    }
    target = apu->portClock;
  }
  if (target < apu->portLastTarget)
    target = apu->portLastTarget;

  ApuPortWrite *w = &apu->portQueue[apu->portQTail & (APU_PORT_QUEUE_LEN - 1)];
  w->target_cycle = target;
  w->port = (uint8_t)(port & 3);
  w->val = val;
  apu->portQTail++;
  apu->portLastGuest = guest_cycle;
  apu->portLastTarget = target;
  return true;
}

/* Apply all CPU bus events due before this SPC cycle executes. */
static void apu_drainPortQueue(Apu* apu) {
  while (apu->portQHead != apu->portQTail) {
    ApuPortWrite *w = &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1)];
    if (w->target_cycle > apu->portClock)
      break;
    apu_applyPortWrite(apu, w);
    apu->portQHead++;
  }
}

/* A port read on cycle `cycle` of the running opcode sees the CPU writes due
 * by then. Writes are queued in time order, so taking them early is safe:
 * every later access of this opcode is later still. */
static void apu_drainPortQueueTo(Apu* apu, uint64_t clock) {
  while (apu->portQHead != apu->portQTail) {
    ApuPortWrite *w = &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1)];
    if (w->target_cycle > clock)
      break;
    apu_applyPortWrite(apu, w);
    apu->portQHead++;
  }
}

bool apu_runUntilPortQueueEmpty(Apu* apu, uint32_t max_cycles) {
  while (apu->portQHead != apu->portQTail) {
    if (!max_cycles) return false;
    const ApuPortWrite *write =
        &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)];
    uint64_t distance = write->target_cycle > apu->portClock
        ? write->target_cycle - apu->portClock : 0;
    uint32_t span = distance >= max_cycles ? max_cycles : (uint32_t)distance + 1u;
    apu_runCycles(apu, span);
    max_cycles -= span;
  }
  return true;
}

bool apu_runToGuestCycle(Apu* apu, uint64_t guest_cycle,
                         uint32_t max_cycles) {
  if (!apu->portTimeValid || guest_cycle < apu->portGuestAnchor)
    return true;

  uint64_t target = apu->portTargetAnchor +
                    (guest_cycle - apu->portGuestAnchor);
  if (target < apu->portLastTarget)
    target = apu->portLastTarget;

  apu->held = false;
  apu->holdActive = apu_accessCycles() != 0;
  apu->holdTarget = target;
  for (;;) {
    if (apu->held) break;
    /* Bus writes at the boundary precede that cycle's SPC work. Drain them
     * directly without advancing the SPC beyond the requested clock. */
    apu_drainPortQueue(apu);
    if (apu->portClock >= target)
      break;
    if (!max_cycles) { apu->holdActive = false; return false; }
    uint64_t distance = target - apu->portClock;
    uint32_t span = distance > max_cycles ? max_cycles : (uint32_t)distance;
    apu_runCycles(apu, span);
    max_cycles -= span;
  }
  apu->holdActive = false;
  apu->held = false;
  return true;
}

bool apu_enqueuePortWriteAt(Apu* apu, uint8_t port, uint8_t val, uint64_t target) {
  if (apu_portQueueDepth(apu) >= APU_PORT_QUEUE_LEN)
    return false;
  /* Late only past opcodes that touch no port (the others are held). */
  if (target < apu->portClock) target = apu->portClock;
  /* Keep the queue in time order: a word write arrives high byte first but
   * lands low byte first. */
  uint32_t i = apu->portQTail;
  while (i != apu->portQHead &&
         apu->portQueue[(i - 1) & (APU_PORT_QUEUE_LEN - 1)].target_cycle > target) {
    apu->portQueue[i & (APU_PORT_QUEUE_LEN - 1)] = apu->portQueue[(i - 1) & (APU_PORT_QUEUE_LEN - 1)];
    i--;
  }
  ApuPortWrite *w = &apu->portQueue[i & (APU_PORT_QUEUE_LEN - 1)];
  w->target_cycle = target;
  w->port = (uint8_t)(port & 3);
  w->val = val;
  apu->portQTail++;
  if (target > apu->portLastTarget) apu->portLastTarget = target;
  return true;
}

bool apu_runThroughCycle(Apu* apu, uint64_t last, uint32_t max_cycles) {
  apu_accessCycles();
  apu->held = false;
  apu->holdActive = true;
  apu->holdTarget = last;
  bool done = true;
  while (apu->portClock <= last && !apu->held) {
    if (!max_cycles) { done = false; break; }
    uint64_t distance = last + 1 - apu->portClock;
    uint32_t span = distance > max_cycles ? max_cycles : (uint32_t)distance;
    apu_runCycles(apu, span);
    max_cycles -= span;
  }
  apu->holdActive = false;
  apu->held = false;
  return done;
}

bool apu_waitForTransferReady(Apu* apu, uint8_t request_port,
                              uint8_t request_value, uint32_t max_cycles) {
  for (;;) {
    if (apu->outPorts[0] == 0xaa && apu->outPorts[1] == 0xbb)
      return true;
    if (apu->inPorts[request_port & 3] != request_value)
      apu_writePortNow(apu, request_port, request_value);
    if (max_cycles-- == 0)
      return false;
    apu_cycle(apu);
  }
}

bool apu_finishHleTransfer(Apu* apu, uint16_t final_pc,
                           uint32_t max_cycles) {
  apu_writePortNow(apu, 2, (uint8_t)final_pc);
  apu_writePortNow(apu, 3, (uint8_t)(final_pc >> 8));
  apu_writePortNow(apu, 1, 0);
  apu_writePortNow(apu, 0, 0xcc);
  for (;;) {
    if (apu->outPorts[0] == 0xcc) {
      memset(apu->inPorts, 0, 4);
      return true;
    }
    if (max_cycles-- == 0)
      return false;
    apu_cycle(apu);
  }
}

void apu_port_sched_save(const Apu *apu, ApuPortSched *out) {
  if (!apu || !out) return;
  memcpy(out->queue, apu->portQueue, sizeof(out->queue));
  out->qHead = apu->portQHead;
  out->qTail = apu->portQTail;
  out->clock = apu->portClock;
  out->guestAnchor = apu->portGuestAnchor;
  out->targetAnchor = apu->portTargetAnchor;
  out->lastGuest = apu->portLastGuest;
  out->lastTarget = apu->portLastTarget;
  out->timeValid = apu->portTimeValid;
}

void apu_port_sched_restore(Apu *apu, const ApuPortSched *in) {
  if (!apu || !in) return;
  memcpy(apu->portQueue, in->queue, sizeof(in->queue));
  apu->portQHead = in->qHead;
  apu->portQTail = in->qTail;
  apu->portClock = in->clock;
  apu->portGuestAnchor = in->guestAnchor;
  apu->portTargetAnchor = in->targetAnchor;
  apu->portLastGuest = in->lastGuest;
  apu->portLastTarget = in->lastTarget;
  apu->portTimeValid = in->timeValid;
}

void apu_saveload(Apu *apu, SaveLoadInfo *sli) {
  sli->func(sli, apu->ram, offsetof(Apu, pad) + 6 - offsetof(Apu, ram));
  sli->func(sli, apu->timerAhead, sizeof(apu->timerAhead));
  sli->func(sli, &apu->dspAhead, sizeof(apu->dspAhead));
  dsp_saveload(apu->dsp, sli);
  spc_saveload(apu->spc, sli);
}

#if SNESRECOMP_SPC_DIAGNOSTICS || SNESRECOMP_TRACE
extern uint64_t g_spc_pc_histogram[0x10000];
#else
extern uint64_t g_spc_pc_histogram[1];
#endif
extern int g_spc_pc_max_seen;

void apu_cycle(Apu* apu) {
  apu_drainPortQueue(apu);
  if(apu->cpuCyclesLeft == 0) {
    if (apu_holdOpcode(apu)) { apu->held = true; return; }
    { static long long lo = -2, hi = 0;
      if (lo == -2) { const char *e = getenv("SNESRECOMP_SPC_EXEC_LOG"); lo = -1; if (e) sscanf(e, "%lld-%lld", &lo, &hi); }
      if (lo >= 0 && (long long)apu->portClock >= lo && (long long)apu->portClock <= hi)
        fprintf(stderr, "[sx] %04X %llu %02X %02X %02X a=%02X y=%02X\n", apu->spc->pc, (unsigned long long)apu->portClock,
                apu->ram[apu->spc->pc], apu->ram[(uint16_t)(apu->spc->pc + 1)], apu->ram[(uint16_t)(apu->spc->pc + 2)],
                apu->spc->a, apu->spc->y); }
    /* Sample PC right BEFORE running the opcode â€” so PC reflects the
     * instruction we're about to execute, not the post-opcode PC. */
#if SNESRECOMP_SPC_DIAGNOSTICS
    g_spc_pc_histogram[apu->spc->pc]++;
    if (apu->spc->pc > g_spc_pc_max_seen) g_spc_pc_max_seen = apu->spc->pc;
#endif
    apu->cpuCyclesLeft = spc_runOpcode(apu->spc);
  }
  apu->cpuCyclesLeft--;

  if (dsp_perClock()) {
    if (apu->dspAhead) apu->dspAhead--;
    else dsp_clock(apu->dsp);
  } else if((apu->cycles & 0x1f) == 0) {
    // every 32 cycles
    uint8_t endx = apu->dsp->ram[0x7c];
    dsp_cycle(apu->dsp);
    if (apu_dspLog() && (apu->dsp->ram[0x7c] & ~endx))
      fprintf(stderr, "[pl] SX %02X %llu 7C\n", apu->dsp->ram[0x7c] & ~endx, (unsigned long long)apu->portClock);
  }

  // handle timers
  extern uint64_t g_apu_timer0_total_ticks;
  for(int i = 0; i < 3; i++) {
    if (apu->timerAhead[i]) { apu->timerAhead[i]--; continue; }
    if(apu->timer[i].cycles == 0) {
      apu->timer[i].cycles = i == 2 ? 16 : 128;
      if(apu->timer[i].enabled) {
        apu->timer[i].divider++;
        if(apu->timer[i].divider == apu->timer[i].target) {
          apu->timer[i].divider = 0;
          apu->timer[i].counter++;
          apu->timer[i].counter &= 0xf;
          if (i == 0) g_apu_timer0_total_ticks++;
        }
      }
    }
    apu->timer[i].cycles--;
  }

  apu->cycles++;
  apu->portClock++;
}

#if SNESRECOMP_APU_NATIVE
#include "apu_native.inc"
#endif

void apu_runCycles(Apu* apu, uint32_t clocks) {
#if SNESRECOMP_APU_NATIVE
  apu_runCycles_native(apu, clocks);
#else
  while (clocks-- && !apu->held) apu_cycle(apu);
#endif
}

const char *apu_time_implementation(void) {
  return SNESRECOMP_APU_NATIVE ? "NATIVE" : "LLE";
}

uint8_t apu_cpuRead(Apu* apu, uint16_t adr) {
  switch(adr) {
    case 0xf0:
    case 0xf1:
    case 0xfa:
    case 0xfb:
    case 0xfc: {
      return 0;
    }
    case 0xf2: {
      return apu->dspAdr;
    }
    case 0xf3: {
      if (!apu->spc->dry) apu_dspTo(apu, apu->spc->accessCycle);
      uint8_t v = dsp_read(apu->dsp, apu->dspAdr & 0x7f);
      if (apu_dspLog() && !apu->spc->dry)
        fprintf(stderr, "[pl] SD %02X %llu %02X\n", v,
                (unsigned long long)(apu->portClock + apu->spc->accessCycle), apu->dspAdr);
      return v;
    }
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7: {
      if (apu_accessCycles() && apu->spc->accessCycle) {
        int64_t at = (int64_t)apu->portClock + apu->spc->accessCycle + s_read_bias;
        if (at > (int64_t)apu->portClock) apu_drainPortQueueTo(apu, (uint64_t)at);
      }
      uint8_t v = apu->inPorts[adr - 0xf4];
      apu_portLog(apu, 'R', v, adr - 0xf4u);
      audio_trace_on_spc_port_read((uint8_t)(adr - 0xf4), v);
#if defined(SNESRECOMP_TRACE) && SNESRECOMP_TRACE
      if (getenv("SNESRECOMP_SPC_PORT_TRACE")) {
        static unsigned long n;
        if (n++ < 2000000)
          fprintf(stderr, "[spcport] pc=%04X port=%d val=%02X clock=%llu q=%u\n",
                  apu->spc->pc, adr - 0xf4, v,
                  (unsigned long long)apu->portClock,
                  (unsigned)(apu->portQTail - apu->portQHead));
      }
#endif
      return v;
    }
    case 0xf8:
    case 0xf9: {
      return apu->inPorts[adr - 0xf4];
    }
    case 0xfd:
    case 0xfe:
    case 0xff: {
      apu_timersTo(apu, apu->spc->accessCycle);
      uint8_t ret = apu->timer[adr - 0xfd].counter;
      apu->timer[adr - 0xfd].counter = 0;
      return ret;
    }
  }
  if(apu->romReadable && adr >= 0xffc0) {
    return bootRom[adr - 0xffc0];
  }
  return apu->ram[adr];
}

/* Diagnostic counters: track SPC writes to specific addresses so we
 * can see whether the engine is touching outPorts at all. */
#if SNESRECOMP_SPC_DIAGNOSTICS || SNESRECOMP_TRACE
uint64_t g_spc_write_counts[0x100] = {0};

/* SPC PC histogram. Sampled once per apu_cycle that starts a new
 * opcode. Lets us answer "which PCs does the SPC spend time in?" */
uint64_t g_spc_pc_histogram[0x10000] = {0};
int g_spc_pc_max_seen = 0;
/* Per-value count for outPorts $F4-$F7. Index = (port_idx * 256) + val. */
uint64_t g_spc_outport_value_counts[4 * 256] = {0};
/* Last 32 outPort writes as a ring buffer: [adr, val] pairs. */
typedef struct { uint8_t adr; uint8_t val; } SpcWriteRec;
SpcWriteRec g_spc_recent_outport_writes[32];
int g_spc_recent_outport_idx = 0;
#else
uint64_t g_spc_write_counts[1] = {0};
uint64_t g_spc_pc_histogram[1] = {0};
int g_spc_pc_max_seen = 0;
uint64_t g_spc_outport_value_counts[1] = {0};
typedef struct { uint8_t adr; uint8_t val; } SpcWriteRec;
SpcWriteRec g_spc_recent_outport_writes[1];
int g_spc_recent_outport_idx = 0;
#endif

void apu_cpuWrite(Apu* apu, uint16_t adr, uint8_t val) {
#if SNESRECOMP_SPC_DIAGNOSTICS
  if (adr < 0x100) g_spc_write_counts[adr]++;
  if (adr >= 0xF4 && adr <= 0xF7) {
    int port = adr - 0xF4;
    g_spc_outport_value_counts[port * 256 + val]++;
    int i = g_spc_recent_outport_idx++ & 31;
    g_spc_recent_outport_writes[i].adr = (uint8_t)adr;
    g_spc_recent_outport_writes[i].val = val;
  }
#endif
  switch(adr) {
    case 0xf0: {
      break; // test register
    }
    case 0xf1: {
      apu_timersTo(apu, apu->spc->accessCycle);
      for(int i = 0; i < 3; i++) {
        if(!apu->timer[i].enabled && (val & (1 << i))) {
          apu->timer[i].divider = 0;
          apu->timer[i].counter = 0;
        }
        apu->timer[i].enabled = val & (1 << i);
      }
      if(val & 0x10) {
        apu->inPorts[0] = 0;
        apu->inPorts[1] = 0;
      }
      if(val & 0x20) {
        apu->inPorts[2] = 0;
        apu->inPorts[3] = 0;
      }
      apu->romReadable = val & 0x80;
      break;
    }
    case 0xf2: {
      apu->dspAdr = val;
      break;
    }
    case 0xf3: {
      if (apu_dspLog())
        fprintf(stderr, "[pl] SE %02X %llu %02X\n", val,
                (unsigned long long)(apu->portClock + apu->spc->accessCycle), apu->dspAdr);
      apu_dspTo(apu, apu->spc->accessCycle);
      if(apu->dspAdr < 0x80) dsp_write(apu->dsp, apu->dspAdr, val);
      break;
    }
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7: {
      apu_portLog(apu, 'W', val, adr - 0xf4u);
      audio_trace_on_spc_port_write((uint8_t)(adr - 0xf4), val);
      apu->outPorts[adr - 0xf4] = val;
      break;
    }
    case 0xf8:
    case 0xf9: {
      apu->inPorts[adr - 0xf4] = val;
      break;
    }
    case 0xfa:
    case 0xfb:
    case 0xfc: {
      apu_timersTo(apu, apu->spc->accessCycle);
      apu->timer[adr - 0xfa].target = val;
      break;
    }
  }
#if defined(SNESRECOMP_TRACE) && SNESRECOMP_TRACE
  if ((adr >= 0x0200 && adr < 0x02c0) || (adr >= 0x3900 && adr < 0x3c00)) {
    if (getenv("SNESRECOMP_RAMWRITE_TRACE")) {
      static unsigned long n;
      if (n++ < 4000000)
        fprintf(stderr, "[ramw] pc=%04X adr=%04X val=%02X\n", apu->spc->pc, adr, val);
    }
  }
#endif
  apu->ram[adr] = val;
}
