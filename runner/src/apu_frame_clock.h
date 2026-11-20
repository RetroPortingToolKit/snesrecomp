#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RTL_MASTER_CYCLES_PER_FRAME 357368ull
#define RTL_APU_CYCLES_PER_FRAME 17088ull

/* One frame is one field of master clocks and, in the runtime's APU model,
 * 1/60 s (PAL 1/50 s) of SPC time at 32040 Hz x 32 SPC cycles per sample:
 * 17088 SPC cycles on NTSC and 20505.6 on PAL. apu_per_frame / apu_den is
 * that count as a fraction. The PAL remainder is therefore carried exactly. */
typedef struct RtlApuFramePeriod {
  uint64_t master, apu_per_frame, apu_den;
} RtlApuFramePeriod;

static inline RtlApuFramePeriod rtl_apu_frame_period(bool pal) {
  /* NTSC: 262 x 1364 master, 1025280 / 60 SPC cycles.
   * PAL:  312 x 1364 master, 1025280 / 50 = 102528 / 5 SPC cycles. */
  RtlApuFramePeriod ntsc = {RTL_MASTER_CYCLES_PER_FRAME,
                            RTL_APU_CYCLES_PER_FRAME, 1};
  RtlApuFramePeriod pal_period = {425568ull, 102528ull, 5};
  return pal ? pal_period : ntsc;
}

/* SPC cycles from the start of frame 0 to `within` master clocks into frame
 * `frame`. With apu_den 1 (NTSC) this is
 * frame * apu_per_frame + within * apu_per_frame / master. */
static inline uint64_t rtl_apu_cycles_at(const RtlApuFramePeriod *period,
                                         uint64_t frame, uint64_t within) {
  const uint64_t whole = frame * period->apu_per_frame;
  return whole / period->apu_den +
         (whole % period->apu_den * period->master +
          within * period->apu_per_frame) /
             (period->apu_den * period->master);
}

/* A host iteration can include an NMI-disabled loader spanning many hardware
 * frames. Keep its elapsed time when beginning the following iteration. */
typedef struct RtlApuFrameClock {
  uint64_t start_master, start_guest, next_guest, last_duration;
} RtlApuFrameClock;

static inline void rtl_apu_clock_begin(RtlApuFrameClock *clock, uint64_t master) {
  clock->start_master = master;
  clock->start_guest = clock->next_guest;
}

static inline uint64_t rtl_apu_clock_now(const RtlApuFrameClock *clock,
                                        const RtlApuFramePeriod *period,
                                        uint64_t master) {
  /* RESET may initialize CPU state inside the first host iteration. */
  uint64_t within = master >= clock->start_master ? master - clock->start_master : 0;
  return clock->start_guest + rtl_apu_cycles_at(period, 0, within);
}

static inline uint64_t rtl_apu_clock_finish(RtlApuFrameClock *clock,
                                           const RtlApuFramePeriod *period,
                                           uint64_t master) {
  uint64_t end = rtl_apu_clock_now(clock, period, master);
  uint64_t minimum = clock->start_guest + rtl_apu_cycles_at(period, 1, 0);
  if (end < minimum) end = minimum;  /* WAI still advances a hardware frame. */
  clock->last_duration = end - clock->start_guest;
  clock->next_guest = end;
  /* Leave the current origin intact: raster IRQs can still touch APU ports
   * between the completed game iteration and the next clock_begin. */
  return end;
}
