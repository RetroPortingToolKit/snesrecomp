/*
 * Nintendo DSP-1 / NEC uPD7725 coprocessor.
 *
 * This is an instruction-level C11 port of ares' uPD96050 core restricted to
 * the uPD7725 geometry used by DSP-1/DSP-1B. Firmware is not included. The
 * build selects either this firmware-dependent LLE floor or the verified
 * command-level HLE model, which fails loudly on any unknown command.
 */

#include "dsp1.h"
#include "dsp1_hle.h"
#include "saveload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kSnesMasterHz = 21477272,
  kDsp1Hz = 7600000,
  kProgramWords = 2048,
  kDataRomWords = 1024,
  kDataRamWords = 256,
};

typedef struct Dsp1Flag {
  uint8_t ov0, ov1, z, c, s0, s1;
} Dsp1Flag;

typedef struct Dsp1Status {
  uint8_t p0, p1, ei, sic, soc, drc, dma, drs, usf0, usf1, rqm;
  uint8_t siack, soack;
} Dsp1Status;

typedef struct Dsp1Regs {
  uint16_t stack[16];
  uint16_t pc, rp, dp;
  uint8_t sp;
  uint16_t si, so;
  int16_t k, l, m, n, a, b;
  uint16_t tr, trb, dr;
  Dsp1Status sr;
} Dsp1Regs;

enum {
  kHlePhaseCommand,
  kHlePhaseInput,
  kHlePhaseOutput,
  kHlePhaseFailed,
};

typedef struct Dsp1HleHost {
  Dsp1HleState model;
  int16_t input[7];
  int16_t output[4];
  uint8_t phase;
  uint8_t command;
  uint8_t input_words;
  uint8_t output_words;
  uint8_t input_byte;
  uint8_t output_byte;
  uint8_t output_discarded;
  uint8_t failed_command;
  uint32_t delay_cycles;
  int16_t timing_projection[7];
  uint64_t last_access_master;
  uint8_t have_last_access;
} Dsp1HleHost;

typedef struct Dsp1HostTrace {
  uint64_t master_clock;
  uint8_t value;
  uint8_t write;
  uint8_t status;
  uint8_t phase;
  uint8_t command;
  uint8_t input_byte;
  uint8_t output_byte;
} Dsp1HostTrace;

struct Dsp1 {
  uint32_t programROM[kProgramWords];
  uint16_t dataROM[kDataRomWords];
  uint16_t dataRAM[kDataRamWords];
  Dsp1Regs r;
  Dsp1Flag fa, fb;
  int firmware_ok;
  int hle_active;
  int warned_missing;
  int in_dsp;
  uint64_t last_master;
  uint32_t frac;
  int64_t budget;
  uint64_t insns;
  uint64_t host_reads;
  uint64_t host_writes;
  uint64_t command_count[256];
  uint64_t trace_fall_insns;
  uint8_t trace_command;
  uint16_t trace_stage;
  uint16_t trace_input[7];
  uint8_t trace_input_words;
  Dsp1HostTrace host_trace[32];
  uint64_t host_trace_index;
  Dsp1HleHost hle;
};

static uint16_t u16(unsigned v) { return (uint16_t)(v & 0xffffu); }
static uint16_t pc11(unsigned v) { return (uint16_t)(v & 0x07ffu); }
static uint16_t rp10(unsigned v) { return (uint16_t)(v & 0x03ffu); }
static uint16_t dp8(unsigned v) { return (uint16_t)(v & 0x00ffu); }
static uint8_t sp4(unsigned v) { return (uint8_t)(v & 0x0fu); }

static uint16_t status_pack(const Dsp1Status *s) {
  uint8_t drs = s->drs && !s->drc;
  return (uint16_t)((s->p0 ? 1 : 0) | ((s->p1 ? 1 : 0) << 1) |
                    ((s->ei ? 1 : 0) << 7) | ((s->sic ? 1 : 0) << 8) |
                    ((s->soc ? 1 : 0) << 9) | ((s->drc ? 1 : 0) << 10) |
                    ((s->dma ? 1 : 0) << 11) | ((drs ? 1 : 0) << 12) |
                    ((s->usf0 ? 1 : 0) << 13) | ((s->usf1 ? 1 : 0) << 14) |
                    ((s->rqm ? 1 : 0) << 15));
}

static void status_unpack(Dsp1Status *s, uint16_t v) {
  s->p0 = (uint8_t)((v >> 0) & 1);
  s->p1 = (uint8_t)((v >> 1) & 1);
  s->ei = (uint8_t)((v >> 7) & 1);
  s->sic = (uint8_t)((v >> 8) & 1);
  s->soc = (uint8_t)((v >> 9) & 1);
  s->drc = (uint8_t)((v >> 10) & 1);
  s->dma = (uint8_t)((v >> 11) & 1);
  s->drs = (uint8_t)((v >> 12) & 1);
  s->usf0 = (uint8_t)((v >> 13) & 1);
  s->usf1 = (uint8_t)((v >> 14) & 1);
  s->rqm = (uint8_t)((v >> 15) & 1);
}

static void dsp1_ld(Dsp1 *d, uint32_t opcode) {
  uint16_t id = (uint16_t)((opcode >> 6) & 0xffffu);
  uint8_t dst = (uint8_t)(opcode & 15u);

  switch (dst) {
    case 0: break;
    case 1: d->r.a = (int16_t)id; break;
    case 2: d->r.b = (int16_t)id; break;
    case 3: d->r.tr = id; break;
    case 4: d->r.dp = dp8(id); break;
    case 5: d->r.rp = rp10(id); break;
    case 6: d->r.dr = id; d->r.sr.rqm = 1; break;
    case 7: {
      uint16_t sr = status_pack(&d->r.sr);
      status_unpack(&d->r.sr, (uint16_t)((sr & 0x907cu) | (id & ~0x907cu)));
      break;
    }
    case 8: d->r.so = id; break;
    case 9: d->r.so = id; break;
    case 10: d->r.k = (int16_t)id; break;
    case 11: d->r.k = (int16_t)id; d->r.l = (int16_t)d->dataROM[d->r.rp]; break;
    case 12: d->r.l = (int16_t)id; d->r.k = (int16_t)d->dataRAM[(d->r.dp | 0x40u) & 0xffu]; break;
    case 13: d->r.l = (int16_t)id; break;
    case 14: d->r.trb = id; break;
    case 15: d->dataRAM[d->r.dp & 0xffu] = id; break;
  }
}

static void dsp1_op(Dsp1 *d, uint32_t opcode) {
  uint8_t pselect = (uint8_t)((opcode >> 20) & 3u);
  uint8_t alu = (uint8_t)((opcode >> 16) & 15u);
  uint8_t asl = (uint8_t)((opcode >> 15) & 1u);
  uint8_t dpl = (uint8_t)((opcode >> 13) & 3u);
  uint8_t dphm = (uint8_t)((opcode >> 9) & 15u);
  uint8_t rpdcr = (uint8_t)((opcode >> 8) & 1u);
  uint8_t src = (uint8_t)((opcode >> 4) & 15u);
  uint8_t dst = (uint8_t)(opcode & 15u);

  uint16_t idb = 0;
  switch (src) {
    case 0: idb = d->r.trb; break;
    case 1: idb = (uint16_t)d->r.a; break;
    case 2: idb = (uint16_t)d->r.b; break;
    case 3: idb = d->r.tr; break;
    case 4: idb = d->r.dp; break;
    case 5: idb = d->r.rp; break;
    case 6: idb = d->dataROM[d->r.rp]; break;
    case 7: idb = (uint16_t)(0x8000u - (d->fa.s1 ? 1u : 0u)); break;
    case 8: idb = d->r.dr; d->r.sr.rqm = 1; break;
    case 9: idb = d->r.dr; break;
    case 10: idb = status_pack(&d->r.sr); break;
    case 11: idb = d->r.si; break;
    case 12: idb = d->r.si; break;
    case 13: idb = (uint16_t)d->r.k; break;
    case 14: idb = (uint16_t)d->r.l; break;
    case 15: idb = d->dataRAM[d->r.dp & 0xffu]; break;
  }

  if (alu) {
    uint16_t p = 0, q = 0, r = 0;
    Dsp1Flag flag;
    uint8_t c = 0;

    switch (pselect) {
      case 0: p = d->dataRAM[d->r.dp & 0xffu]; break;
      case 1: p = idb; break;
      case 2: p = (uint16_t)d->r.m; break;
      case 3: p = (uint16_t)d->r.n; break;
    }

    if (!asl) {
      q = (uint16_t)d->r.a;
      flag = d->fa;
      c = d->fb.c;
    } else {
      q = (uint16_t)d->r.b;
      flag = d->fb;
      c = d->fa.c;
    }

    switch (alu) {
      case 1: r = u16(q | p); break;
      case 2: r = u16(q & p); break;
      case 3: r = u16(q ^ p); break;
      case 4: r = u16(q - p); break;
      case 5: r = u16(q + p); break;
      case 6: r = u16(q - p - c); break;
      case 7: r = u16(q + p + c); break;
      case 8: r = u16(q - 1); p = 1; break;
      case 9: r = u16(q + 1); p = 1; break;
      case 10: r = u16(~q); break;
      case 11: r = u16((q >> 1) | (q & 0x8000u)); break;
      case 12: r = u16((q << 1) | c); break;
      case 13: r = u16((q << 2) | 3u); break;
      case 14: r = u16((q << 4) | 15u); break;
      case 15: r = u16((q << 8) | (q >> 8)); break;
    }

    flag.z = (r == 0);
    flag.s0 = (uint8_t)((r >> 15) & 1);
    if (!flag.ov1) flag.s1 = flag.s0;

    switch (alu) {
      case 1: case 2: case 3: case 10: case 13: case 14: case 15:
        flag.ov0 = flag.ov1 = flag.c = 0;
        break;
      case 4: case 5: case 6: case 7: case 8: case 9: {
        uint16_t carries = (uint16_t)(q ^ p ^ r);
        uint16_t overflow = (uint16_t)((q ^ r) & (p ^ ((alu & 1) ? r : q)));
        flag.ov0 = (uint8_t)((overflow >> 15) & 1);
        flag.ov1 = (uint8_t)(flag.ov0 && flag.ov1 ? flag.s0 == flag.s1
                                                   : (flag.ov0 | flag.ov1));
        flag.c = (uint8_t)(((carries ^ overflow) >> 15) & 1);
        break;
      }
      case 11:
        flag.ov0 = flag.ov1 = 0;
        flag.c = (uint8_t)(q & 1);
        break;
      case 12:
        flag.ov0 = flag.ov1 = 0;
        flag.c = (uint8_t)((q >> 15) & 1);
        break;
    }

    if (!asl) {
      d->r.a = (int16_t)r;
      d->fa = flag;
    } else {
      d->r.b = (int16_t)r;
      d->fb = flag;
    }
  }

  dsp1_ld(d, ((uint32_t)idb << 6) | dst);

  if (dst != 4) {
    switch (dpl) {
      case 1: d->r.dp = dp8((d->r.dp & 0xf0u) + ((d->r.dp + 1u) & 0x0fu)); break;
      case 2: d->r.dp = dp8((d->r.dp & 0xf0u) + ((d->r.dp - 1u) & 0x0fu)); break;
      case 3: d->r.dp = dp8(d->r.dp & 0xf0u); break;
    }
    d->r.dp = dp8(d->r.dp ^ ((uint16_t)dphm << 4));
  }

  if (dst != 5 && rpdcr) d->r.rp = rp10(d->r.rp - 1u);
}

static void dsp1_rt(Dsp1 *d, uint32_t opcode) {
  dsp1_op(d, opcode);
  d->r.sp = sp4(d->r.sp - 1u);
  d->r.pc = pc11(d->r.stack[d->r.sp]);
}

static void dsp1_jp(Dsp1 *d, uint32_t opcode) {
  uint16_t brch = (uint16_t)((opcode >> 13) & 0x01ffu);
  uint16_t na = (uint16_t)((opcode >> 2) & 0x07ffu);
  uint16_t bank = (uint16_t)(opcode & 3u);
  uint16_t jp = pc11((d->r.pc & 0x2000u) | (bank << 11) | na);

  switch (brch) {
    case 0x000: d->r.pc = pc11(d->r.so); return;
    case 0x080: if (!d->fa.c) d->r.pc = jp; return;
    case 0x082: if ( d->fa.c) d->r.pc = jp; return;
    case 0x084: if (!d->fb.c) d->r.pc = jp; return;
    case 0x086: if ( d->fb.c) d->r.pc = jp; return;
    case 0x088: if (!d->fa.z) d->r.pc = jp; return;
    case 0x08a: if ( d->fa.z) d->r.pc = jp; return;
    case 0x08c: if (!d->fb.z) d->r.pc = jp; return;
    case 0x08e: if ( d->fb.z) d->r.pc = jp; return;
    case 0x090: if (!d->fa.ov0) d->r.pc = jp; return;
    case 0x092: if ( d->fa.ov0) d->r.pc = jp; return;
    case 0x094: if (!d->fb.ov0) d->r.pc = jp; return;
    case 0x096: if ( d->fb.ov0) d->r.pc = jp; return;
    case 0x098: if (!d->fa.ov1) d->r.pc = jp; return;
    case 0x09a: if ( d->fa.ov1) d->r.pc = jp; return;
    case 0x09c: if (!d->fb.ov1) d->r.pc = jp; return;
    case 0x09e: if ( d->fb.ov1) d->r.pc = jp; return;
    case 0x0a0: if (!d->fa.s0) d->r.pc = jp; return;
    case 0x0a2: if ( d->fa.s0) d->r.pc = jp; return;
    case 0x0a4: if (!d->fb.s0) d->r.pc = jp; return;
    case 0x0a6: if ( d->fb.s0) d->r.pc = jp; return;
    case 0x0a8: if (!d->fa.s1) d->r.pc = jp; return;
    case 0x0aa: if ( d->fa.s1) d->r.pc = jp; return;
    case 0x0ac: if (!d->fb.s1) d->r.pc = jp; return;
    case 0x0ae: if ( d->fb.s1) d->r.pc = jp; return;
    case 0x0b0: if ((d->r.dp & 0x0fu) == 0x00u) d->r.pc = jp; return;
    case 0x0b1: if ((d->r.dp & 0x0fu) != 0x00u) d->r.pc = jp; return;
    case 0x0b2: if ((d->r.dp & 0x0fu) == 0x0fu) d->r.pc = jp; return;
    case 0x0b3: if ((d->r.dp & 0x0fu) != 0x0fu) d->r.pc = jp; return;
    case 0x0b4: if (!d->r.sr.siack) d->r.pc = jp; return;
    case 0x0b6: if ( d->r.sr.siack) d->r.pc = jp; return;
    case 0x0b8: if (!d->r.sr.soack) d->r.pc = jp; return;
    case 0x0ba: if ( d->r.sr.soack) d->r.pc = jp; return;
    case 0x0bc: if (!d->r.sr.rqm) d->r.pc = jp; return;
    case 0x0be: if ( d->r.sr.rqm) d->r.pc = jp; return;
    case 0x100: d->r.pc = pc11(jp & ~0x2000u); return;
    case 0x101: d->r.pc = pc11(jp |  0x2000u); return;
    case 0x140:
      d->r.stack[d->r.sp] = d->r.pc;
      d->r.sp = sp4(d->r.sp + 1u);
      d->r.pc = pc11(jp & ~0x2000u);
      return;
    case 0x141:
      d->r.stack[d->r.sp] = d->r.pc;
      d->r.sp = sp4(d->r.sp + 1u);
      d->r.pc = pc11(jp | 0x2000u);
      return;
  }
}

static void dsp1_exec(Dsp1 *d) {
  uint32_t opcode = d->programROM[d->r.pc];
  d->r.pc = pc11(d->r.pc + 1u);

  switch (opcode >> 22) {
    case 0: dsp1_op(d, opcode); break;
    case 1: dsp1_rt(d, opcode); break;
    case 2: dsp1_jp(d, opcode); break;
    case 3: dsp1_ld(d, opcode); break;
  }

  int32_t result = (int32_t)d->r.k * (int32_t)d->r.l;
  d->r.m = (int16_t)(result >> 15);
  d->r.n = (int16_t)((uint32_t)result << 1);
  d->insns++;
}

static int dsp1_timing_trace_enabled(void) {
  static int initialized;
  static int enabled;
  if (!initialized) {
    const char *value = getenv("SNESRECOMP_DSP1_TIMING_TRACE");
    enabled = value && value[0] == '1';
    initialized = 1;
  }
  return enabled;
}

static void dsp1_trace_rqm_fall(Dsp1 *d) {
  if (!dsp1_timing_trace_enabled()) return;
  d->trace_stage++;
  d->trace_fall_insns = d->insns;
}

static void dsp1_hle_command_ready(Dsp1 *d) {
  d->hle.phase = kHlePhaseCommand;
  d->hle.command = 0;
  d->hle.input_words = 0;
  d->hle.output_words = 0;
  d->hle.input_byte = 0;
  d->hle.output_byte = 0;
  d->hle.output_discarded = 0;
  d->r.sr.drc = 1;
  d->r.sr.drs = 0;
  d->r.sr.rqm = 1;
}

static void dsp1_hle_host_reset(Dsp1 *d) {
  memset(&d->hle, 0, sizeof(d->hle));
  dsp1_hle_state_reset(&d->hle.model);
  dsp1_hle_command_ready(d);
}

static void dsp1_hle_schedule(Dsp1 *d, uint32_t cycles) {
  d->hle.delay_cycles = cycles;
  d->r.sr.rqm = 0;
}

static void dsp1_hle_account_aot_access_spacing(Dsp1 *d) {
  /*
   * Generated blocks charge master time at block granularity. Consecutive
   * DSP data accesses can therefore carry one timestamp even though each
   * 65C816 access took time. Consume the omitted minimum spacing only from
   * the HLE readiness delay; the block still owns the architectural clock.
   */
  if (d->hle.have_last_access &&
      d->hle.last_access_master == d->last_master &&
      d->hle.delay_cycles) {
    const uint32_t master_clocks = 16;
    const uint32_t dsp_cycles =
        (master_clocks * kDsp1Hz + kSnesMasterHz - 1u) / kSnesMasterHz;
    if (dsp_cycles >= d->hle.delay_cycles) {
      d->hle.delay_cycles = 0;
      d->r.sr.rqm = 1;
    } else {
      d->hle.delay_cycles -= dsp_cycles;
    }
  }
  d->hle.last_access_master = d->last_master;
  d->hle.have_last_access = 1;
}

static void dsp1_hle_trace_host(Dsp1 *d, int write, uint8_t value) {
  const unsigned capacity =
      sizeof(d->host_trace) / sizeof(d->host_trace[0]);
  Dsp1HostTrace *trace =
      &d->host_trace[(unsigned)(d->host_trace_index++ % capacity)];
  trace->master_clock = d->last_master;
  trace->value = value;
  trace->write = (uint8_t)write;
  trace->status = (uint8_t)((d->r.sr.rqm << 7) | (d->r.sr.drs << 4) |
                            (d->r.sr.drc << 2));
  trace->phase = (uint8_t)d->hle.phase;
  trace->command = d->hle.command;
  trace->input_byte = d->hle.input_byte;
  trace->output_byte = d->hle.output_byte;
}

static void dsp1_hle_dump_host_trace(const Dsp1 *d) {
  const unsigned capacity =
      sizeof(d->host_trace) / sizeof(d->host_trace[0]);
  const unsigned count = d->host_trace_index < capacity
                             ? (unsigned)d->host_trace_index
                             : capacity;
  const uint64_t start = d->host_trace_index - count;
  for (unsigned i = 0; i < count; i++) {
    const Dsp1HostTrace *trace =
        &d->host_trace[(unsigned)((start + i) % capacity)];
    fprintf(stderr,
            "[dsp1-host] master=%llu %c=%02x sr=%02x phase=%u "
            "command=%02x input_byte=%u output_byte=%u\n",
            (unsigned long long)trace->master_clock,
            trace->write ? 'W' : 'R', trace->value, trace->status,
            trace->phase, trace->command, trace->input_byte,
            trace->output_byte);
  }
}

/* Delays measured from LLE firmware RQM transitions during SMK gameplay. */
static uint32_t dsp1_hle_project_cycles(const Dsp1 *d) {
  uint16_t fx = (uint16_t)d->hle.timing_projection[0];
  uint16_t aas = (uint16_t)d->hle.timing_projection[5];
  uint16_t x = (uint16_t)d->hle.input[0];
  uint16_t y = (uint16_t)d->hle.input[1];

  if (fx == 0x0800)
    return y >= 0x0b10 ? 334 : 336;
  if (fx != 0x0ee0)
    return 338;

  if (x <= 0x0430)
    return aas == 0x00c0 ? 327 : (aas == 0x8000 ? 339 : 334);
  if (x == 0x0e60)
    return aas == 0x00c0 ? (uint32_t)(353 - ((y >> 6) & 2u))
                         : (aas == 0x8000 ? 353 : 356);
  if (x == 0x0ee0) {
    if (aas == 0x00c0) {
      if (y == 0x0bd0) return 346;
      if (y == 0x0b10) return 346;
      if (y == 0x0a50) return 342;
      return 344;
    }
    if (aas == 0x8000)
      return 321;
    return y == 0x0bd0 ? 298 : 338;
  }
  return 338;
}

static uint32_t dsp1_hle_command_cycles(const Dsp1 *d) {
  switch (d->hle.command) {
    case 0x00:
    case 0x04:
    case 0x0c:
    case 0x20:
      return 4;
    case 0x08:
    case 0x18:
      return 6;
    case 0x10:
      return 40;
    case 0x28:
      return 65;
    case 0x02: {
      uint16_t fx = (uint16_t)d->hle.input[0];
      uint16_t aas = (uint16_t)d->hle.input[5];
      if (fx == 0x0880) return 529;
      if (fx == 0x0800) return 483;
      if (fx == 0x0ee0)
        return aas == 0x8000 ? 505 : (aas == 0x00c0 ? 517 : 521);
      return 503;
    }
    case 0x06:
      return dsp1_hle_project_cycles(d);
    case 0x0a:
      return 130;
    default:
      return 2;
  }
}

static void dsp1_hle_fail(Dsp1 *d, uint8_t command) {
  d->hle.phase = kHlePhaseFailed;
  d->hle.failed_command = command;
  d->r.sr.drc = 1;
  d->r.sr.drs = 0;
  d->r.sr.rqm = 0;
  dsp1_hle_dump_host_trace(d);
  fprintf(stderr,
          "[dsp1] HLE stopped on unsupported or invalid command %02x; "
          "rebuild with SNESRECOMP_DSP1_IMPL=LLE and provide firmware\n",
          command);
}

static int dsp1_hle_run_command(Dsp1 *d) {
  uint8_t output_words = 0;
  if (d->hle.command == 0x02)
    memcpy(d->hle.timing_projection, d->hle.input,
           sizeof(d->hle.timing_projection));
  if (!dsp1_hle_execute_state(
          &d->hle.model, d->hle.command, d->hle.input, d->hle.input_words,
          d->hle.output, (uint8_t)(sizeof(d->hle.output) /
                                   sizeof(d->hle.output[0])),
          &output_words)) {
    dsp1_hle_fail(d, d->hle.command);
    return 0;
  }
  d->hle.output_words = output_words;
  d->hle.output_byte = 0;
  d->hle.output_discarded = 0;
  if (!output_words) {
    dsp1_hle_command_ready(d);
    return 1;
  }
  d->hle.phase = kHlePhaseOutput;
  d->r.sr.drc = 0;
  d->r.sr.drs = 0;
  d->r.sr.rqm = 1;
  return 1;
}

static void dsp1_hle_begin_command(Dsp1 *d, uint8_t command) {
  uint8_t input_words = 0;
  uint8_t output_words = 0;
  d->command_count[command]++;
  d->hle.command = command;
  if (!dsp1_hle_command_shape(command, &input_words, &output_words)) {
    dsp1_hle_fail(d, command);
    return;
  }
  d->hle.input_words = input_words;
  d->hle.output_words = output_words;
  d->hle.input_byte = 0;
  d->hle.output_byte = 0;
  d->hle.output_discarded = 0;
  memset(d->hle.input, 0, sizeof(d->hle.input));
  if (!input_words) {
    (void)dsp1_hle_run_command(d);
    if (d->hle.phase != kHlePhaseFailed) dsp1_hle_schedule(d, 2);
    return;
  }
  d->hle.phase = kHlePhaseInput;
  d->r.sr.drc = 0;
  d->r.sr.drs = 0;
  d->r.sr.rqm = 1;
  dsp1_hle_schedule(d, 2);
}

static int dsp1_hle_finish_output(Dsp1 *d) {
  if (d->hle.command != 0x0a || d->hle.output_discarded) {
    dsp1_hle_command_ready(d);
    return 0;
  }
  d->hle.input[0] =
      (int16_t)(uint16_t)((uint16_t)d->hle.input[0] + 1u);
  (void)dsp1_hle_run_command(d);
  return 1;
}

static uint8_t dsp1_hle_read_data(Dsp1 *d) {
  if (d->hle.phase != kHlePhaseOutput || !d->r.sr.rqm) return 0;
  uint8_t byte = d->hle.output_byte;
  uint16_t word = (uint16_t)d->hle.output[byte >> 1];
  uint8_t value = (byte & 1) ? (uint8_t)(word >> 8) : (uint8_t)word;
  d->hle.output_byte++;
  d->r.sr.drs = (uint8_t)(d->hle.output_byte & 1u);
  if (!(d->hle.output_byte & 1u)) {
    if (d->hle.output_byte == (uint8_t)(d->hle.output_words * 2u)) {
      int continued = dsp1_hle_finish_output(d);
      if (d->hle.phase != kHlePhaseFailed)
        dsp1_hle_schedule(d, continued ? 121 : 3);
    } else {
      dsp1_hle_schedule(d, 2);
    }
  }
  return value;
}

static void dsp1_hle_write_data(Dsp1 *d, uint8_t value) {
  if (d->hle.phase == kHlePhaseCommand) {
    dsp1_hle_begin_command(d, value);
    return;
  }
  if (d->hle.phase == kHlePhaseInput) {
    uint8_t byte = d->hle.input_byte;
    uint16_t word = (uint16_t)d->hle.input[byte >> 1];
    if (byte & 1) word = (uint16_t)((word & 0x00ffu) |
                                    ((uint16_t)value << 8));
    else word = (uint16_t)((word & 0xff00u) | value);
    d->hle.input[byte >> 1] = (int16_t)word;
    d->hle.input_byte++;
    d->r.sr.drs = (uint8_t)(d->hle.input_byte & 1u);
    if (d->hle.input_byte == (uint8_t)(d->hle.input_words * 2u)) {
      (void)dsp1_hle_run_command(d);
      if (d->hle.phase != kHlePhaseFailed)
        dsp1_hle_schedule(d, dsp1_hle_command_cycles(d));
    } else if (!(d->hle.input_byte & 1u)) {
      dsp1_hle_schedule(d, 2);
    }
    return;
  }
  if (d->hle.phase == kHlePhaseOutput) {
    d->hle.output_discarded = 1;
    d->hle.output_byte++;
    d->r.sr.drs = (uint8_t)(d->hle.output_byte & 1u);
    if (!(d->hle.output_byte & 1u)) {
      if (d->hle.output_byte == (uint8_t)(d->hle.output_words * 2u)) {
        (void)dsp1_hle_finish_output(d);
        if (d->hle.phase != kHlePhaseFailed) dsp1_hle_schedule(d, 3);
      } else {
        dsp1_hle_schedule(d, 2);
      }
    }
  }
}

Dsp1 *dsp1_create(void) {
  Dsp1 *d = (Dsp1 *)calloc(1, sizeof(Dsp1));
  if (d) {
    d->hle_active = SNESRECOMP_DSP1_HLE;
    dsp1_reset(d);
  }
  return d;
}

void dsp1_destroy(Dsp1 *d) { free(d); }

void dsp1_reset(Dsp1 *d) {
  if (!d) return;
  Dsp1Regs r;
  memset(&r, 0, sizeof(r));
  d->r = r;
  memset(&d->fa, 0, sizeof(d->fa));
  memset(&d->fb, 0, sizeof(d->fb));
  d->budget = 0;
  d->frac = 0;
  d->last_master = 0;
  if (d->hle_active) dsp1_hle_host_reset(d);
}

void dsp1_sync(Dsp1 *d, uint64_t master_clock) {
  if (!d || d->in_dsp) return;
  if (d->hle_active) {
    if (master_clock <= d->last_master) {
      d->last_master = master_clock;
      return;
    }
    uint64_t delta = master_clock - d->last_master;
    d->last_master = master_clock;
    uint64_t num = delta * (uint64_t)kDsp1Hz + d->frac;
    uint64_t cycles = num / (uint64_t)kSnesMasterHz;
    d->frac = (uint32_t)(num % (uint64_t)kSnesMasterHz);
    if (d->hle.delay_cycles) {
      if (cycles >= d->hle.delay_cycles) {
        d->hle.delay_cycles = 0;
        d->r.sr.rqm = 1;
      } else {
        d->hle.delay_cycles -= (uint32_t)cycles;
      }
    }
    return;
  }
  if (master_clock <= d->last_master) { d->last_master = master_clock; return; }

  uint64_t delta = master_clock - d->last_master;
  d->last_master = master_clock;
  uint64_t num = delta * (uint64_t)kDsp1Hz + d->frac;
  d->budget += (int64_t)(num / (uint64_t)kSnesMasterHz);
  d->frac = (uint32_t)(num % (uint64_t)kSnesMasterHz);
  if (d->budget <= 0) return;

  d->in_dsp = 1;
  int64_t guard = d->budget + 4096;
  while (d->budget > 0 && guard-- > 0) {
    if (!d->firmware_ok) {
      if (!d->warned_missing++) {
        fprintf(stderr,
                "[dsp1] firmware missing; DSP-1 will not execute. Set "
                "SNESRECOMP_DSP1_ROM or place dsp1b.rom/dsp1.rom nearby.\n");
      }
      d->budget = 0;
      break;
    }
    uint8_t previous_rqm = d->r.sr.rqm;
    dsp1_exec(d);
    if (!previous_rqm && d->r.sr.rqm && dsp1_timing_trace_enabled()) {
      fprintf(stderr,
              "[dsp1-timing] command=%02x stage=%u dsp_cycles=%llu\n",
              d->trace_command, (unsigned)d->trace_stage,
              (unsigned long long)(d->insns - d->trace_fall_insns));
      uint8_t input_words = 0;
      if (dsp1_hle_command_shape(d->trace_command, &input_words, NULL) &&
          d->trace_stage == (uint16_t)input_words + 1u) {
        fprintf(stderr, "[dsp1-compute] command=%02x input=",
                d->trace_command);
        for (uint8_t i = 0; i < d->trace_input_words; i++)
          fprintf(stderr, "%s%04x", i ? "," : "", d->trace_input[i]);
        fprintf(stderr, " dsp_cycles=%llu\n",
                (unsigned long long)(d->insns - d->trace_fall_insns));
      }
    }
    d->budget--;
  }
  if (guard <= 0) {
    static int reported;
    if (!reported++)
      fprintf(stderr, "[dsp1] sync guard tripped; DSP is not converging\n");
    d->budget = 0;
  }
  d->in_dsp = 0;
}

uint8_t dsp1_read(Dsp1 *d, uint16_t addr) {
  if (!d) return 0;
  d->host_reads++;
  if (addr & 1) return (uint8_t)(status_pack(&d->r.sr) >> 8);
  if (d->hle_active) {
    dsp1_hle_account_aot_access_spacing(d);
    uint8_t value = dsp1_hle_read_data(d);
    dsp1_hle_trace_host(d, 0, value);
    return value;
  }
  if (!d->r.sr.drc) {
    if (!d->r.sr.drs) {
      d->r.sr.drs = 1;
      return (uint8_t)(d->r.dr >> 0);
    }
    d->r.sr.rqm = 0;
    d->r.sr.drs = 0;
    dsp1_trace_rqm_fall(d);
    return (uint8_t)(d->r.dr >> 8);
  }
  d->r.sr.rqm = 0;
  return (uint8_t)(d->r.dr >> 0);
}

void dsp1_write(Dsp1 *d, uint16_t addr, uint8_t value) {
  if (!d || (addr & 1)) return;
  d->host_writes++;
  if (d->hle_active) {
    dsp1_hle_account_aot_access_spacing(d);
    dsp1_hle_trace_host(d, 1, value);
    dsp1_hle_write_data(d, value);
    return;
  }
  if (d->r.sr.drc && d->r.sr.rqm)
    d->command_count[value]++;
  if (d->r.sr.drc && d->r.sr.rqm) {
    d->trace_command = value;
    d->trace_stage = 0;
    d->trace_input_words = 0;
  }
  if (!d->r.sr.drc) {
    if (!d->r.sr.drs) {
      d->r.sr.drs = 1;
      d->r.dr = (uint16_t)((d->r.dr & 0xff00u) | value);
    } else {
      d->r.sr.rqm = 0;
      d->r.sr.drs = 0;
      d->r.dr = (uint16_t)(((uint16_t)value << 8) | (d->r.dr & 0x00ffu));
      if (dsp1_timing_trace_enabled() &&
          d->trace_input_words <
              sizeof(d->trace_input) / sizeof(d->trace_input[0]))
        d->trace_input[d->trace_input_words++] = d->r.dr;
      dsp1_trace_rqm_fall(d);
    }
  } else {
    d->r.sr.rqm = 0;
    d->r.dr = (uint16_t)((d->r.dr & 0xff00u) | value);
    dsp1_trace_rqm_fall(d);
  }
}

uint8_t dsp1_read_data_ram(Dsp1 *d, uint16_t addr) {
  if (!d) return 0;
  uint16_t word = (uint16_t)((addr >> 1) & 0xffu);
  return (addr & 1) ? (uint8_t)(d->dataRAM[word] >> 8)
                    : (uint8_t)(d->dataRAM[word] & 0xffu);
}

void dsp1_write_data_ram(Dsp1 *d, uint16_t addr, uint8_t value) {
  if (!d) return;
  uint16_t word = (uint16_t)((addr >> 1) & 0xffu);
  if (addr & 1) d->dataRAM[word] = (uint16_t)((d->dataRAM[word] & 0x00ffu) | ((uint16_t)value << 8));
  else d->dataRAM[word] = (uint16_t)((d->dataRAM[word] & 0xff00u) | value);
}

static int dsp1_parse_firmware(Dsp1 *d, const uint8_t *buf) {
  int reversed = 0;

  uint32_t first_le = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                      ((uint32_t)buf[2] << 16);
  uint32_t first_be = (uint32_t)buf[2] | ((uint32_t)buf[1] << 8) |
                      ((uint32_t)buf[0] << 16);
  if (first_le == 0x97c000u) reversed = 0;
  else if (first_be == 0x97c000u) reversed = 1;
  else if ((first_le >> 22) == 3 && (first_be >> 22) != 3) reversed = 0;
  else if ((first_be >> 22) == 3 && (first_le >> 22) != 3) reversed = 1;
  else {
    unsigned plausible_le = 0, plausible_be = 0;
    for (unsigned i = 0; i < 32; i++) {
      uint32_t le = (uint32_t)buf[i * 3] | ((uint32_t)buf[i * 3 + 1] << 8) |
                    ((uint32_t)buf[i * 3 + 2] << 16);
      uint32_t be = (uint32_t)buf[i * 3 + 2] | ((uint32_t)buf[i * 3 + 1] << 8) |
                    ((uint32_t)buf[i * 3] << 16);
      plausible_le += (le >> 22) <= 3;
      plausible_be += (be >> 22) <= 3;
    }
    reversed = plausible_be > plausible_le;
  }

  for (unsigned i = 0; i < kProgramWords; i++) {
    const uint8_t *p = &buf[i * 3];
    d->programROM[i] = reversed
        ? ((uint32_t)p[2] | ((uint32_t)p[1] << 8) | ((uint32_t)p[0] << 16))
        : ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
  }
  const uint8_t *data = buf + kProgramWords * 3;
  for (unsigned i = 0; i < kDataRomWords; i++) {
    const uint8_t *p = &data[i * 2];
    d->dataROM[i] = reversed ? (uint16_t)(p[1] | ((uint16_t)p[0] << 8))
                             : (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
  }
  d->firmware_ok = 1;
  dsp1_reset(d);
  return reversed;
}

static int dsp1_load_firmware_file(Dsp1 *d, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  uint8_t buf[8192];
  size_t n = fread(buf, 1, sizeof(buf), f);
  int extra = fgetc(f);
  fclose(f);
  if (n != sizeof(buf) || extra != EOF) {
    fprintf(stderr, "[dsp1] %s is %u bytes, expected 8192 - ignoring\n",
            path, (unsigned)(n + (extra == EOF ? 0 : 1)));
    return 0;
  }
  int reversed = dsp1_parse_firmware(d, buf);
  fprintf(stderr, "[dsp1] loaded %s (%s firmware byte order)\n",
          path, reversed ? "reversed" : "ares");
  return 1;
}

int dsp1_load_firmware(Dsp1 *d, const char *rom_path) {
  if (!d) return 0;
  d->firmware_ok = 0;
  d->hle_active = SNESRECOMP_DSP1_HLE;
  fprintf(stderr, "[dsp1] implementation=%s (build-fixed)\n",
          dsp1_build_implementation());
  if (SNESRECOMP_DSP1_HLE) {
    dsp1_reset(d);
    fprintf(stderr, "[dsp1] firmware inputs ignored by HLE build; "
                    "unverified commands stop execution.\n");
    return 0;
  }

  const char *env = getenv("SNESRECOMP_DSP1_ROM");
  if (env && env[0] && dsp1_load_firmware_file(d, env)) return 1;
  if (dsp1_load_firmware_file(d, "dsp1b.rom")) return 1;
  if (dsp1_load_firmware_file(d, "dsp1.rom")) return 1;
  if (dsp1_load_firmware_file(d, "dsp1.bin")) return 1;
  if (dsp1_load_firmware_file(d, "firmware/dsp1b.rom")) return 1;
  if (dsp1_load_firmware_file(d, "firmware/dsp1.rom")) return 1;
  if (dsp1_load_firmware_file(d, "firmware/dsp1.bin")) return 1;

  if (rom_path && rom_path[0]) {
    char buf[1024];
    size_t n = strlen(rom_path);
    if (n < sizeof(buf) - 20) {
      memcpy(buf, rom_path, n + 1);
      while (n && buf[n - 1] != '/' && buf[n - 1] != '\\') n--;
      buf[n] = '\0';
      strcat(buf, "dsp1b.rom");
      if (dsp1_load_firmware_file(d, buf)) return 1;
      buf[n] = '\0';
      strcat(buf, "dsp1.rom");
      if (dsp1_load_firmware_file(d, buf)) return 1;
      buf[n] = '\0';
      strcat(buf, "dsp1.bin");
      if (dsp1_load_firmware_file(d, buf)) return 1;
    }
  }

  dsp1_reset(d);
  fprintf(stderr,
          "[dsp1] LLE build requires an 8192-byte DSP-1 firmware image. "
          "Set SNESRECOMP_DSP1_ROM or place dsp1b.rom/dsp1.rom nearby. "
          "DSP-1 will not execute without firmware.\n");
  return 0;
}

const char *dsp1_build_implementation(void) {
  return SNESRECOMP_DSP1_HLE ? "HLE" : "LLE";
}

int dsp1_firmware_loaded(const Dsp1 *d) { return d ? d->firmware_ok : 0; }
int dsp1_hle_active(const Dsp1 *d) { return d ? d->hle_active : 0; }
int dsp1_hle_failed(const Dsp1 *d) {
  return d ? d->hle.phase == kHlePhaseFailed : 0;
}
uint8_t dsp1_hle_failed_command(const Dsp1 *d) {
  return d ? d->hle.failed_command : 0;
}
uint64_t dsp1_instructions_executed(const Dsp1 *d) { return d ? d->insns : 0; }
uint64_t dsp1_host_reads(const Dsp1 *d) { return d ? d->host_reads : 0; }
uint64_t dsp1_host_writes(const Dsp1 *d) { return d ? d->host_writes : 0; }
uint64_t dsp1_command_count(const Dsp1 *d, uint8_t command) {
  return d ? d->command_count[command] : 0;
}

void dsp1_saveload(Dsp1 *d, struct SaveLoadInfo *sli) {
  if (!d || !sli) return;
  sli->func(sli, d->dataRAM, sizeof(d->dataRAM));
  sli->func(sli, &d->r, sizeof(d->r));
  sli->func(sli, &d->fa, sizeof(d->fa));
  sli->func(sli, &d->fb, sizeof(d->fb));
  sli->func(sli, &d->budget, sizeof(d->budget));
  sli->func(sli, &d->frac, sizeof(d->frac));
  sli->func(sli, &d->hle, sizeof(d->hle));
}
