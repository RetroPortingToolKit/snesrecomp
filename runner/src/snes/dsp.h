
#ifndef DSP_H
#define DSP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "saveload.h"

typedef struct Dsp Dsp;

typedef struct Apu Apu;

// Output-sample ring capacity (stereo pairs). Must be a power of two so
// the monotonic write/read counters can index with a mask and survive
// uint32 wraparound. 8192 samples ≈ 256 ms at 32 kHz — far larger than
// any single-frame APU catch-up burst, while typical fill stays ~534
// (one block), so playback latency is unchanged. See the sampleBuffer
// comment in struct Dsp and the music-tick post-mortem in MMX ISSUES.md.
#define DSP_SAMPLE_RING 8192

typedef struct DspChannel {
  // pitch
  uint16_t pitch;
  uint16_t pitchCounter;
  bool pitchModulation;
  // brr decoding
  int16_t decodeBuffer[19]; // 16 samples per brr-block, +3 for interpolation
  uint8_t srcn;
  uint16_t decodeOffset;
  uint8_t previousFlags; // from last sample
  int16_t old;
  int16_t older;
  bool useNoise;
  // adsr, envelope, gain
  uint16_t adsrRates[4]; // attack, decay, sustain, gain
  uint16_t rateCounter;
  uint8_t adsrState; // 0: attack, 1: decay, 2: sustain, 3: gain, 4: release
  uint16_t sustainLevel;
  bool useGain;
  uint8_t gainMode;
  bool directGain;
  uint16_t gainValue; // for direct gain
  uint16_t gain;
  // keyon/off
  bool keyOn;
  bool keyOff;
  // output
  int16_t sampleOut; // final sample, to be multiplied by channel volume
  int8_t volumeL;
  int8_t volumeR;
  bool echoEnable;
} DspChannel;

/* The per-clock S-DSP (dsp_clock.c): one voice's state. */
typedef struct DspClockVoice {
  int32_t env;       /* envelope, 11 bits */
  int32_t envPrev;   /* last computed value before clamping (bent increase) */
  int32_t pos;       /* interpolation position: 3 bits of sample, 12 of fraction */
  uint8_t mode;      /* release, attack, decay, sustain */
  uint16_t brrAdr;
  uint8_t brrOff;
  uint8_t konDelay;
  uint8_t envx;
  uint8_t bufPos;
  int16_t buf[12];   /* decoded samples, a ring of three groups of 4 */
} DspClockVoice;

/* The per-clock S-DSP: the registers as its steps read them, and the values
 * latched between steps (shared by the voices, as on the chip). */
typedef struct DspClock {
  uint8_t regs[0x80];
  uint8_t step;
  uint16_t counter;
  int32_t noise;
  uint8_t outxBuf, envxBuf, endxBuf;
  int32_t voiceOut;
  int32_t out[2];
  int32_t pitch;
  uint16_t dirAdr, nextAdr;
  uint8_t dir, non, pmon, eon, kon, konNew, kof, everyOther, srcn, brrHeader, brrData, looped, adsr1;
  int32_t echoIn[2], echoOut[2];
  int16_t echoHist[8][2];
  uint16_t echoPtr, echoLen, echoOff;
  uint8_t echoHistPos, esa;
  bool echoOn;
  DspClockVoice v[8];
} DspClock;

struct Dsp {
  uint8_t *apu_ram;
  // Host publication policy, outside the guest snapshot. Speculation still
  // executes every DSP cycle but must never enter the live consumer queue.
  bool suppressOutput;
  // MP2K-style verified-enhancement shadow mixer (opt-in; default off).
  // Placed BEFORE `ram` so it lies outside the dsp_saveload region
  // (which serializes from `ram` to end) — savestate layout is unchanged.
  // void* to keep dsp.h free of the shadow header; dsp.c owns the type.
  void *shadow;
  // Host voice/echo routing, outside the serialized simulation like shadow.
  void *output_filter;
  /* Canonical timing-model state. Serialized separately from the historical
   * ram-through-output region; a current-build load must resume these phases. */
  /* Samples left of each voice's key-on delay (hardware: 5 silent samples,
   * then playback from the sample's first block). */
  uint8_t konDelay[8];
  /* The envelope/noise rate counter all voices share (hardware): counts
   * down once per sample through 0x7800 values. */
  int rateCounter;
  /* A decoded end block whose ENDx bit is still to be set (when playback
   * reaches sample `endxAt` of the block; hardware's decoder runs ahead). */
  bool endPending[8];
  DspClock clk;   /* the per-clock S-DSP (dsp_clock.c) */
  // mirror ram
  uint8_t ram[0x80];
  // 8 channels
  DspChannel channel[8];
  // overarching
  uint16_t dirPage;
  bool evenCycle;
  bool mute;
  bool reset;
  int8_t masterVolumeL;
  int8_t masterVolumeR;
  // noise
  int16_t noiseSample;
  uint16_t noiseRate;
  uint16_t noiseCounter;
  // echo
  bool echoWrites;
  int8_t echoVolumeL;
  int8_t echoVolumeR;
  int8_t feedbackVolume;
  uint16_t echoBufferAdr;
  uint16_t echoDelay;
  uint16_t echoRemain;
  uint16_t echoBufferIndex;
  uint8_t firBufferIndex;
  int8_t firValues[8];
  int16_t firBufferL[8];
  int16_t firBufferR[8];
  // Output-sample ring. Two producers feed it (serialized by RtlApuLock):
  // the audio thread (RtlRenderAudio) and the CPU thread (snes_catchupApu,
  // on APU-port access). The old fixed 534-sample buffer dropped every
  // sample produced past 534, so a catch-up burst between audio callbacks
  // lost samples → music-rate ticks + timing jitter. The ring buffers the
  // burst instead; the audio thread consumes the oldest 534 per block at
  // the steady output rate, smoothing bursty production. (1 native block
  // = 534 samples @ ~32 kHz; *2 for stereo.)
  int16_t sampleBuffer[DSP_SAMPLE_RING * 2];
  uint32_t sampleWrite; // total samples produced (monotonic; index = & mask)
  uint32_t sampleRead;  // total samples consumed (monotonic; index = & mask)
};


Dsp *dsp_init(uint8_t *ram);
void dsp_free(Dsp* dsp);
void dsp_reset(Dsp* dsp);
void dsp_cycle(Dsp* dsp);
/* The per-clock S-DSP (dsp_clock.c): on unless SNESRECOMP_DSP_PER_CLOCK=0; then apu_cycle runs dsp_clock every
 * SPC cycle instead of dsp_cycle every 32nd. */
bool dsp_perClock(void);
void dsp_clock(Dsp* dsp);
void dsp_clockReset(Dsp* dsp);
void dsp_clockWrite(Dsp* dsp, uint8_t adr, uint8_t val);
/* Internal DAC-only mirror for the clocked mixer; never executes voices or
 * writes canonical APU memory. NULL means ordinary unfiltered publication. */
Dsp *dsp_clock_output_filter(Dsp *dsp, uint8_t *voices);
/* Presentation only. Select dry voices AND their native echo at the DAC;
 * every canonical voice, echo RAM write and clock still runs unchanged.
 * 0xff restores ordinary output (the default). Caller holds the APU lock.
 * False means allocation failed; ordinary output remains available. */
bool dsp_set_output_voices(Dsp *dsp, uint8_t voices);
/* Preserve the presentation echo during a discarded rollback replay. The
 * canonical mixer still runs; its temporary output will be discarded. */
void dsp_suspend_output_filter(Dsp *dsp, bool suspended);
/* Call under the APU lock. This gates host PCM publication, not synthesis. */
void dsp_set_output_suppressed(Dsp *dsp, bool suppressed);
uint8_t dsp_read(Dsp* dsp, uint8_t adr);
void dsp_write(Dsp* dsp, uint8_t adr, uint8_t val);
/* Native-rate output-ring access. The consumer (RtlRenderAudio) reconciles the
 * guest production clock against the host device clock, which needs to read the
 * ring at a fractional offset and retire a count it computes itself — so the
 * ring is exposed directly rather than through a fixed-block accessor.
 *
 * dsp_available  natives currently queued (sampleWrite - sampleRead).
 * dsp_peek       sample at `offset` natives past the read cursor. The caller
 *                must have checked offset < dsp_available().
 * dsp_advance    retire `natives` from the read cursor and record the consume
 *                event. Clamped to what is available.
 */
uint32_t dsp_available(const Dsp* dsp);
void dsp_peek(const Dsp* dsp, uint32_t offset, int16_t* l, int16_t* r);
void dsp_advance(Dsp* dsp, uint32_t natives);
/* Exact-consume block read at the native rate: copies `frames` natives 1:1 and
 * retires exactly `frames`. Returns the count actually copied (< frames when
 * the ring is short; the tail is left untouched for the caller to fill).
 *
 * This replaces a fixed-534-block sample-and-hold resampler inherited from
 * upstream LakeSnes, which retired 534 natives per call regardless of how many
 * frames were requested. Any caller not asking for exactly 534 therefore put
 * the ring permanently out of balance: measured on Super Mario Kart at 33
 * calls/s x 534 = 17.8 k natives/s consumed against 32.04 k/s produced, which
 * pinned the ring at its 8192 cap and destroyed 43% of all samples (384 k of
 * them carrying audible music) inside 33 s. */
uint32_t dsp_getSamples(Dsp* dsp, int16_t* sampleData, int frames);
/* Drop queued host-output samples without rewinding SPC/DSP state. Used only
 * when leaving fast-forward, where buffered samples describe obsolete guest
 * time and retaining them would create persistent A/V latency. */
uint32_t dsp_trimSamples(Dsp* dsp, uint32_t samples_to_keep);
/* Per-voice Gaussian-interpolated sample (canon hardware math). Exposed for the
 * dev-only in-process faithful reference (dsp_shadow) to diff against blargg's
 * reference Gaussian. Pure: reads channel[ch].decodeBuffer[sampleNum..+3], no
 * state change. */
int16_t dsp_getSample(Dsp* dsp, int ch, int sampleNum, int offset);
/* Canonical SNES Gaussian table (byte-identical to blargg's snes9x/bsnes
 * gauss[512]); exposed so the dev faithful reference can apply blargg's exact
 * integer algorithm to it. */
extern const uint16_t gaussValues[512];
/*
 * Output-ring save/restore for rollback.
 *
 * The ring lies inside the frozen dsp_saveload blob, but it is not guest
 * state: the read cursor is advanced by the host audio thread at the audio
 * device's rate. A rollback load must therefore restore the SIMULATION and
 * leave the ring where the live consumer left it — rewinding sampleRead
 * would replay already-played audio, and rewinding sampleWrite below a
 * cursor the audio thread has already passed makes dsp_available() wrap to
 * ~4 billion. Callers capture the ring before the guest blob goes in and
 * put it back afterwards.
 *
 * Rewinding sampleWrite alone (with the buffer left as-is) is also how a
 * resim discards the audio it re-produced for ticks the player already
 * heard. Hold RtlApuLock across both calls.
 */
typedef struct DspOutputRing {
  int16_t  samples[DSP_SAMPLE_RING * 2];
  uint32_t write;
  uint32_t read;
} DspOutputRing;

void dsp_output_ring_save(const Dsp *dsp, DspOutputRing *out);
void dsp_output_ring_restore(Dsp *dsp, const DspOutputRing *in);
/* Cursor-only rewind: drop everything produced past `write`. */
void dsp_output_ring_set_write(Dsp *dsp, uint32_t write);
uint32_t dsp_output_ring_write(const Dsp *dsp);
/* Exact-consume block read at the native rate: copies `frames` natives 1:1 and
 * retires exactly `frames`. Returns the count actually copied (< frames when
 * the ring is short; the tail is left untouched for the caller to fill).
 *
 * This replaces a fixed-534-block sample-and-hold resampler inherited from
 * upstream LakeSnes, which retired 534 natives per call regardless of how many
 * frames were requested. Any caller not asking for exactly 534 therefore put
 * the ring permanently out of balance: measured on Super Mario Kart at 33
 * calls/s x 534 = 17.8 k natives/s consumed against 32.04 k/s produced, which
 * pinned the ring at its 8192 cap and destroyed 43% of all samples (384 k of
 * them carrying audible music) inside 33 s. */
uint32_t dsp_getSamples(Dsp* dsp, int16_t* sampleData, int frames);
/* Drop queued host-output samples without rewinding SPC/DSP state. Used only
 * when leaving fast-forward, where buffered samples describe obsolete guest
 * time and retaining them would create persistent A/V latency. */
uint32_t dsp_trimSamples(Dsp* dsp, uint32_t samples_to_keep);
/* Per-voice Gaussian-interpolated sample (canon hardware math). Exposed for the
 * dev-only in-process faithful reference (dsp_shadow) to diff against blargg's
 * reference Gaussian. Pure: reads channel[ch].decodeBuffer[sampleNum..+3], no
 * state change. */
int16_t dsp_getSample(Dsp* dsp, int ch, int sampleNum, int offset);
/* Canonical SNES Gaussian table (byte-identical to blargg's snes9x/bsnes
 * gauss[512]); exposed so the dev faithful reference can apply blargg's exact
 * integer algorithm to it. */
extern const uint16_t gaussValues[512];
void dsp_saveload(Dsp *dsp, SaveLoadInfo *sli);

#endif
