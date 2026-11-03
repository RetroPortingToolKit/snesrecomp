/*
 * The S-DSP stepped per SPC clock (SNESRECOMP_DSP_PER_CLOCK, default on).
 *
 * dsp.c computes a whole output sample at one clock. The hardware spreads a
 * sample over 32 clocks: each voice does its work in nine steps at fixed
 * clocks, staggered from voice to voice, and reads every register at its own
 * step (anomie's S-DSP document gives the schedule). A register the SPC writes
 * inside a sample takes effect at the step that reads it: a SRCN switch just
 * before a voice's directory read selects the new loop address, just after it
 * the old one. This file runs that schedule; the SPC brings it to an access's
 * own cycle before it touches $F3 (apu.c), so reads of ENVX, OUTX and ENDX see
 * the values as they are on that clock.
 *
 * Behaviour checked against MesenCE 2.2.1's S-DSP (its $F3 reads, ENDX and
 * output); the code is our own. Gaussian interpolation only. The output filter
 * and the enhancement shadow of dsp_cycle are not applied on this path.
 */
#include <string.h>

#include "dsp.h"
#include "../audio_trace.h"

enum { R_MVOLL = 0x0c, R_MVOLR = 0x1c, R_EVOLL = 0x2c, R_EVOLR = 0x3c, R_KON = 0x4c, R_KOF = 0x5c,
       R_FLG = 0x6c, R_ENDX = 0x7c, R_EFB = 0x0d, R_PMON = 0x2d, R_NON = 0x3d, R_EON = 0x4d,
       R_DIR = 0x5d, R_ESA = 0x6d, R_EDL = 0x7d, R_FIR = 0x0f };
enum { V_VOLL, V_VOLR, V_PITCHL, V_PITCHH, V_SRCN, V_ADSR1, V_ADSR2, V_GAIN, V_ENVX, V_OUTX };
enum { M_RELEASE, M_ATTACK, M_DECAY, M_SUSTAIN };

static int s_per_clock = -1;
bool dsp_perClock(void) {
  if (s_per_clock < 0) {
    const char *e = getenv("SNESRECOMP_DSP_PER_CLOCK");
    s_per_clock = !(e && e[0] == '0');
  }
  return s_per_clock;
}

static int16_t clamp16(int32_t v) { return v < -0x8000 ? -0x8000 : v > 0x7fff ? 0x7fff : (int16_t)v; }
static uint8_t vreg(const DspClock *c, int v, int r) { return c->regs[(v << 4) | r]; }
/* A value the DSP itself produces: the SPC reads it from $F3 at once. */
static void dsp_publish(Dsp *dsp, uint8_t adr, uint8_t val) {
  dsp->clk.regs[adr] = val;
  dsp->ram[adr] = val;
}
static uint8_t ram8(const Dsp *dsp, uint16_t a) { return dsp->apu_ram[a]; }

void dsp_clockReset(Dsp *dsp) {
  DspClock *c = &dsp->clk;
  memset(c, 0, sizeof(*c));
  /* After power-on or reset the S-DSP acts as if FLG were $E0 (soft reset, mute, echo writes off), while the
   * register reads back what was last written. */
  c->regs[R_FLG] = 0xe0;
  c->everyOther = 1;
  c->noise = 0x4000;
}

/* The global rate counter: does an event of `rate` (0-31) happen on this sample? */
static bool rate_due(const DspClock *c, int rate) {
  static const uint16_t period[32] = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1 };
  static const uint16_t phase[32] = {
    0, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
    0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0 };
  if (!rate) return false;
  return (c->counter + phase[rate]) % period[rate] == 0;
}

static void brr_decode4(Dsp *dsp, int v) {
  DspClock *c = &dsp->clk;
  DspClockVoice *x = &c->v[v];
  uint8_t second = ram8(dsp, (uint16_t)(x->brrAdr + x->brrOff + 1));
  int nib[4] = { c->brrData >> 4, c->brrData & 15, second >> 4, second & 15 };
  int shift = c->brrHeader >> 4, filter = (c->brrHeader >> 2) & 3;
  int p1 = x->buf[(x->bufPos + 11) % 12] >> 1;
  int p2 = x->buf[(x->bufPos + 10) % 12] >> 1;
  for (int i = 0; i < 4; i++) {
    int d = nib[i] >= 8 ? nib[i] - 16 : nib[i];
    int s = shift > 12 ? (d < 0 ? -0x800 : 0) : (d * (1 << shift)) >> 1;
    if (filter == 1) s += p1 + (-p1 >> 4);
    else if (filter == 2) s += 2 * p1 + ((-3 * p1) >> 5) - p2 + (p2 >> 4);
    else if (filter == 3) s += 2 * p1 + ((-13 * p1) >> 6) - p2 + ((3 * p2) >> 4);
    int16_t out = (int16_t)(clamp16(s) * 2);   /* 15 bits, kept shifted left */
    x->buf[x->bufPos + i] = out;
    p2 = p1;
    p1 = out >> 1;
  }
  x->bufPos = x->bufPos >= 8 ? 0 : x->bufPos + 4;
}

static int32_t gauss4(const DspClockVoice *x) {
  int i = (x->pos >> 12) + x->bufPos, f = (x->pos >> 4) & 0xff;
  int32_t sum = (int16_t)(((gaussValues[255 - f] * x->buf[i % 12]) >> 11) +
                          ((gaussValues[511 - f] * x->buf[(i + 1) % 12]) >> 11) +
                          ((gaussValues[256 + f] * x->buf[(i + 2) % 12]) >> 11));
  sum += (gaussValues[f] * x->buf[(i + 3) % 12]) >> 11;
  return clamp16(sum) & ~1;
}

static void envelope(Dsp *dsp, int v) {
  DspClock *c = &dsp->clk;
  DspClockVoice *x = &c->v[v];
  int32_t e = x->env;
  if (x->mode == M_RELEASE) {
    x->env = e > 8 ? e - 8 : 0;
    return;
  }
  int rate;
  uint8_t level;   /* the byte whose top 3 bits are the sustain level */
  if (c->adsr1 & 0x80) {
    uint8_t adsr2 = vreg(c, v, V_ADSR2);
    level = adsr2;
    if (x->mode == M_ATTACK) {
      int a = c->adsr1 & 15;
      rate = a == 15 ? 31 : a * 2 + 1;
      e += a == 15 ? 1024 : 32;
    } else {
      e -= ((e - 1) >> 8) + 1;
      rate = x->mode == M_DECAY ? (((c->adsr1 >> 4) & 7) * 2 + 16) : (adsr2 & 31);
    }
  } else {
    uint8_t gain = vreg(c, v, V_GAIN);
    level = gain;
    if (gain & 0x80) {
      rate = gain & 31;
      switch (gain & 0x60) {
        case 0x00: e -= 32; break;
        case 0x20: e -= ((e - 1) >> 8) + 1; break;
        case 0x40: e += 32; break;
        default: e += (uint16_t)x->envPrev < 0x600 ? 32 : 8; break;
      }
    } else {
      e = gain << 4;
      rate = 31;
    }
  }
  if (x->mode == M_DECAY && (e >> 8) == (level >> 5)) x->mode = M_SUSTAIN;
  x->envPrev = e;
  if (e < 0 || e > 0x7ff) {
    e = e < 0 ? 0 : 0x7ff;
    if (x->mode == M_ATTACK) x->mode = M_DECAY;
  }
  if (rate_due(c, rate)) x->env = e;
}

static void mix(Dsp *dsp, int v, int side) {
  DspClock *c = &dsp->clk;
  int32_t o = (c->voiceOut * (int8_t)vreg(c, v, V_VOLL + side)) >> 7;
  c->out[side] = clamp16(c->out[side] + o);
  if (c->eon & (1 << v)) c->echoOut[side] = clamp16(c->echoOut[side] + o);
}

/* The steps of a voice; 3 is split in three parts (a, b, c) for voice 0, whose step 3 straddles the echo. */
static void step1(Dsp *dsp, int v) {
  DspClock *c = &dsp->clk;
  c->dirAdr = (uint16_t)(c->dir * 0x100 + c->srcn * 4);   /* the SRCN latched one voice earlier */
  c->srcn = vreg(c, v, V_SRCN);
}
static void step2(Dsp *dsp, int v) {
  DspClock *c = &dsp->clk;
  uint16_t a = c->dirAdr + (c->v[v].konDelay ? 0 : 2);   /* start address on key-on, else the loop address */
  c->nextAdr = (uint16_t)(ram8(dsp, a) | ram8(dsp, (uint16_t)(a + 1)) << 8);
  c->adsr1 = vreg(c, v, V_ADSR1);
  c->pitch = vreg(c, v, V_PITCHL);
}
static void step3a(Dsp *dsp, int v) {
  DspClock *c = &dsp->clk;
  c->pitch |= (vreg(c, v, V_PITCHH) & 0x3f) << 8;
}
static void step3b(Dsp *dsp, int v) {
  DspClock *c = &dsp->clk;
  c->brrHeader = ram8(dsp, c->v[v].brrAdr);
  c->brrData = ram8(dsp, (uint16_t)(c->v[v].brrAdr + c->v[v].brrOff));
}
static void step3c(Dsp *dsp, int v) {
  DspClock *c = &dsp->clk;
  DspClockVoice *x = &c->v[v];
  if (c->pmon & (1 << v)) c->pitch += ((c->voiceOut >> 5) * c->pitch) >> 10;
  if (x->konDelay) {
    if (x->konDelay == 5) {
      x->brrAdr = c->nextAdr;
      x->brrOff = 1;
      x->bufPos = 0;
      c->brrHeader = 0;
    }
    x->env = 0;
    x->envPrev = 0;
    x->konDelay--;
    x->pos = (x->konDelay & 3) ? 0x4000 : 0;
    c->pitch = 0;
  }
  int32_t s = gauss4(x);
  if (c->non & (1 << v)) s = (int16_t)(c->noise * 2);
  c->voiceOut = ((s * x->env) >> 11) & ~1;
  x->envx = (uint8_t)(x->env >> 4);
  if ((c->regs[R_FLG] & 0x80) || (c->brrHeader & 3) == 1) {
    x->mode = M_RELEASE;
    x->env = 0;
  }
  if (c->everyOther) {
    if (c->kof & (1 << v)) x->mode = M_RELEASE;
    if (c->kon & (1 << v)) { x->konDelay = 5; x->mode = M_ATTACK; }
  }
  if (!x->konDelay) envelope(dsp, v);
}
static void step4(Dsp *dsp, int v) {
  DspClock *c = &dsp->clk;
  DspClockVoice *x = &c->v[v];
  c->looped = 0;
  if (x->pos >= 0x4000) {
    brr_decode4(dsp, v);
    if (x->brrOff >= 7) {
      if (c->brrHeader & 1) { x->brrAdr = c->nextAdr; c->looped = (uint8_t)(1 << v); }
      else x->brrAdr += 9;
      x->brrOff = 1;
    } else {
      x->brrOff += 2;
    }
  }
  x->pos = (x->pos & 0x3fff) + c->pitch;
  if (x->pos > 0x7fff) x->pos = 0x7fff;
  mix(dsp, v, 0);
}
static void step5(Dsp *dsp, int v) {
  DspClock *c = &dsp->clk;
  mix(dsp, v, 1);
  uint8_t endx = c->regs[R_ENDX] | c->looped;
  if (c->v[v].konDelay == 5) endx &= (uint8_t)~(1 << v);
  c->endxBuf = endx;
}
static void step6(Dsp *dsp, int v) { (void)v; dsp->clk.outxBuf = (uint8_t)(dsp->clk.voiceOut >> 8); }
static void step7(Dsp *dsp, int v) {
  dsp_publish(dsp, R_ENDX, dsp->clk.endxBuf);
  dsp->clk.envxBuf = dsp->clk.v[v].envx;
}
static void step8(Dsp *dsp, int v) { dsp_publish(dsp, (uint8_t)((v << 4) | V_OUTX), dsp->clk.outxBuf); }
static void step9(Dsp *dsp, int v) { dsp_publish(dsp, (uint8_t)((v << 4) | V_ENVX), dsp->clk.envxBuf); }

/* Echo, clocks 22-30. */
static int32_t fir(const DspClock *c, int tap, int side) {
  return (c->echoHist[(c->echoHistPos + tap + 1) & 7][side] * (int8_t)c->regs[R_FIR + (tap << 4)]) >> 6;
}
static int16_t echo_read(const Dsp *dsp, uint16_t a) { return (int16_t)(ram8(dsp, a) | ram8(dsp, (uint16_t)(a + 1)) << 8); }
static void echo_write(Dsp *dsp, uint16_t a, int32_t v) {
  dsp->apu_ram[a] = (uint8_t)v;
  dsp->apu_ram[(uint16_t)(a + 1)] = (uint8_t)(v >> 8);
}
static void echo_clock(Dsp *dsp, int clock) {
  DspClock *c = &dsp->clk;
  switch (clock) {
    case 22:
      c->echoHistPos = (c->echoHistPos + 1) & 7;
      c->echoPtr = (uint16_t)((c->esa << 8) + c->echoOff);
      c->echoHist[c->echoHistPos][0] = echo_read(dsp, c->echoPtr) >> 1;
      c->echoIn[0] = fir(c, 0, 0);
      c->echoIn[1] = fir(c, 0, 1);
      break;
    case 23:
      c->echoHist[c->echoHistPos][1] = echo_read(dsp, (uint16_t)(c->echoPtr + 2)) >> 1;
      for (int s = 0; s < 2; s++) c->echoIn[s] += fir(c, 1, s) + fir(c, 2, s);
      break;
    case 24:
      for (int s = 0; s < 2; s++) c->echoIn[s] += fir(c, 3, s) + fir(c, 4, s) + fir(c, 5, s);
      break;
    case 25:
      for (int s = 0; s < 2; s++) {
        int32_t partial = (int16_t)(c->echoIn[s] + fir(c, 6, s));
        c->echoIn[s] = clamp16(partial + (int16_t)fir(c, 7, s)) & ~1;
      }
      break;
    case 26:
      c->out[0] = clamp16(((c->out[0] * (int8_t)c->regs[R_MVOLL]) >> 7) + ((c->echoIn[0] * (int8_t)c->regs[R_EVOLL]) >> 7));
      for (int s = 0; s < 2; s++)
        c->echoOut[s] = clamp16(c->echoOut[s] + (int16_t)((c->echoIn[s] * (int8_t)c->regs[R_EFB]) >> 7)) & ~1;
      break;
    case 27:
      c->out[1] = clamp16(((c->out[1] * (int8_t)c->regs[R_MVOLR]) >> 7) + ((c->echoIn[1] * (int8_t)c->regs[R_EVOLR]) >> 7));
      break;
    case 28:
      c->echoOn = !(c->regs[R_FLG] & 0x20);
      break;
    case 29:
      if (c->echoOff == 0) c->echoLen = (uint16_t)((c->regs[R_EDL] & 15) << 11);
      c->echoOff += 4;
      if (c->echoOff >= c->echoLen) c->echoOff = 0;
      if (c->echoOn) echo_write(dsp, c->echoPtr, c->echoOut[0]);
      c->echoOut[0] = 0;
      c->esa = c->regs[R_ESA];
      c->echoOn = !(c->regs[R_FLG] & 0x20);
      break;
    case 30:
      if (c->echoOn) echo_write(dsp, (uint16_t)(c->echoPtr + 2), c->echoOut[1]);
      c->echoOut[1] = 0;
      break;
  }
}

static void emit_sample(Dsp *dsp, Dsp *filtered) {
  DspClock *c = &dsp->clk;
  int16_t l = (int16_t)c->out[0], r = (int16_t)c->out[1];
  if (filtered) {
    l = (int16_t)filtered->clk.out[0];
    r = (int16_t)filtered->clk.out[1];
    filtered->clk.out[0] = filtered->clk.out[1] = 0;
  }
  if (c->regs[R_FLG] & 0x40) l = r = 0;
  c->out[0] = c->out[1] = 0;
  if (dsp->suppressOutput) return;
  uint32_t fill = dsp->sampleWrite - dsp->sampleRead;
  int dropped = fill >= DSP_SAMPLE_RING;
  if (!dropped) {
    uint32_t w = dsp->sampleWrite & (DSP_SAMPLE_RING - 1);
    dsp->sampleBuffer[w * 2] = l;
    dsp->sampleBuffer[w * 2 + 1] = r;
    dsp->sampleWrite++;
  }
  audio_trace_on_sample(l, r, dropped, dropped ? fill : fill + 1);
}

/* Which voice steps run on each of the 32 clocks: {voice, step} pairs, step 31/32/33 = 3a/3b/3c. */
static const uint8_t kSchedule[32][3][2] = {
  {{0, 5}, {1, 2}, {9, 0}}, {{0, 6}, {1, 3}, {9, 0}}, {{0, 7}, {1, 4}, {3, 1}}, {{0, 8}, {1, 5}, {2, 2}},
  {{0, 9}, {1, 6}, {2, 3}}, {{1, 7}, {2, 4}, {4, 1}}, {{1, 8}, {2, 5}, {3, 2}}, {{1, 9}, {2, 6}, {3, 3}},
  {{2, 7}, {3, 4}, {5, 1}}, {{2, 8}, {3, 5}, {4, 2}}, {{2, 9}, {3, 6}, {4, 3}}, {{3, 7}, {4, 4}, {6, 1}},
  {{3, 8}, {4, 5}, {5, 2}}, {{3, 9}, {4, 6}, {5, 3}}, {{4, 7}, {5, 4}, {7, 1}}, {{4, 8}, {5, 5}, {6, 2}},
  {{4, 9}, {5, 6}, {6, 3}}, {{0, 1}, {5, 7}, {6, 4}}, {{5, 8}, {6, 5}, {7, 2}}, {{5, 9}, {6, 6}, {7, 3}},
  {{1, 1}, {6, 7}, {7, 4}}, {{6, 8}, {7, 5}, {0, 2}}, {{0, 31}, {6, 9}, {7, 6}}, {{7, 7}, {9, 0}, {9, 0}},
  {{7, 8}, {9, 0}, {9, 0}}, {{0, 32}, {7, 9}, {9, 0}}, {{9, 0}, {9, 0}, {9, 0}}, {{9, 0}, {9, 0}, {9, 0}},
  {{9, 0}, {9, 0}, {9, 0}}, {{9, 0}, {9, 0}, {9, 0}}, {{0, 33}, {9, 0}, {9, 0}}, {{0, 4}, {2, 1}, {9, 0}},
};

static void voice_step(Dsp *dsp, int v, int s) {
  switch (s) {
    case 1: step1(dsp, v); break;
    case 2: step2(dsp, v); break;
    case 3: step3a(dsp, v); step3b(dsp, v); step3c(dsp, v); break;
    case 31: step3a(dsp, v); break;
    case 32: step3b(dsp, v); break;
    case 33: step3c(dsp, v); break;
    case 4: step4(dsp, v); break;
    case 5: step5(dsp, v); break;
    case 6: step6(dsp, v); break;
    case 7: step7(dsp, v); break;
    case 8: step8(dsp, v); break;
    case 9: step9(dsp, v); break;
  }
}

void dsp_clock(Dsp *dsp) {
  DspClock *c = &dsp->clk;
  int clock = c->step;
  uint8_t voices = 0xff;
  Dsp *filtered = dsp->output_filter ? dsp_clock_output_filter(dsp, &voices) : NULL;
  c->step = (uint8_t)((c->step + 1) & 31);
  /* Clock 30: KON/KOF and the counter come before voice 0's step 3c; clock 22: voice 0's 3a before the echo
   * read (the order inside a clock matters only where they share a latch). */
  if (clock == 27) c->pmon = c->regs[R_PMON] & 0xfe;   /* voice 0 has no pitch modulation */
  if (clock == 28) { c->dir = c->regs[R_DIR]; c->non = c->regs[R_NON]; c->eon = c->regs[R_EON]; }
  if (clock == 29) { c->everyOther ^= 1; if (c->everyOther) c->konNew &= (uint8_t)~c->kon; }
  if (clock == 30) {
    if (c->everyOther) { c->kon = c->konNew; c->kof = c->regs[R_KOF]; }
    c->counter = c->counter ? (uint16_t)(c->counter - 1) : 0x77ff;
    if (rate_due(c, c->regs[R_FLG] & 31))
      c->noise = (((c->noise << 14) ^ (c->noise << 13)) & 0x4000) ^ (c->noise >> 1);
  }
  for (int i = 0; i < 3; i++) {
    int v = kSchedule[clock][i][0], step = kSchedule[clock][i][1];
    if (v < 8) {
      voice_step(dsp, v, step);
      /* Feed the same synthesized voice to a presentation-only mixer. Its
       * echo RAM/FIR are private; canonical synthesis and echo keep running. */
      if (filtered && (voices & (1u << v)) && (step == 4 || step == 5)) {
        filtered->clk.voiceOut = c->voiceOut;
        mix(filtered, v, step == 4 ? 0 : 1);
      }
    }
  }
  echo_clock(dsp, clock);
  if (filtered) echo_clock(filtered, clock);
  if (clock == 27) emit_sample(dsp, filtered);
}

/* An SPC write to a DSP register on this path. */
void dsp_clockWrite(Dsp *dsp, uint8_t adr, uint8_t val) {
  DspClock *c = &dsp->clk;
  c->regs[adr] = val;
  dsp->ram[adr] = val;
  if ((adr & 15) == V_ENVX) c->envxBuf = val;
  else if ((adr & 15) == V_OUTX) c->outxBuf = val;
  else if (adr == R_KON) c->konNew = val;
  else if (adr == R_ENDX) { c->endxBuf = 0; dsp_publish(dsp, R_ENDX, 0); }
}
