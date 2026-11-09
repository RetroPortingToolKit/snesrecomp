#ifndef SNESRECOMP_SNES_REGION_H
#define SNESRECOMP_SNES_REGION_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Console region: the one setting every video-timing site derives from.
 *
 * NTSC and PAL consoles differ in the master clock and in the number of
 * scanlines per field. Both regions share the scanline length (1364 master
 * clocks), the line at which vblank starts (225, 240 with overscan), the
 * APU's own crystal and the 32040 Hz DSP output rate.
 *
 *                    NTSC          PAL
 *   master clock     21477272 Hz   21281370 Hz
 *   lines / field    262           312
 *   field rate       60.0988 Hz    50.0070 Hz
 *
 * Sources: snes9x snes9x.h (NTSC_MASTER_CLOCK, PAL_MASTER_CLOCK,
 * SNES_MAX_NTSC_VCOUNTER, SNES_MAX_PAL_VCOUNTER, PAL_PROGRESSIVE_FRAME_RATE);
 * bsnes sfc/system/system.cpp (Colorburst::PAL * 4.8),
 * sfc/ppu/counter/counter-inline.hpp (vperiod, hperiod) and sfc/ppu/io.cpp
 * (vdisp, STAT78 bit 4).
 *
 * Like the NTSC model this runtime has always used, the PAL model keeps every
 * scanline at 1364 clocks. Hardware has one short NTSC line (240 of field 1
 * when not interlaced) and one long PAL line (311 of field 1 when
 * interlaced). Neither is modelled. Interlaced fields keep the progressive
 * line count.
 *
 * The flag lives in Snes (palTiming) and in the coprocessors that run from
 * their own crystals. false is NTSC: a zeroed struct keeps the behaviour
 * every existing caller and test was written against.
 */

typedef enum SnesRegion {
  kSnesRegionAuto = 0, /* the cartridge header's $FFD9 destination code */
  kSnesRegionNtsc = 1,
  kSnesRegionPal = 2,
} SnesRegion;

enum {
  kSnesMasterClocksPerLine = 1364,
  kSnesVblankLine = 225,
  kSnesNtscLinesPerField = 262,
  kSnesPalLinesPerField = 312,
};

#define SNES_NTSC_MASTER_HZ 21477272u
#define SNES_PAL_MASTER_HZ 21281370u

static inline uint32_t snes_region_lines_per_field(bool pal) {
  return pal ? (uint32_t)kSnesPalLinesPerField
             : (uint32_t)kSnesNtscLinesPerField;
}

static inline uint32_t snes_region_master_clocks_per_field(bool pal) {
  return (uint32_t)kSnesMasterClocksPerLine * snes_region_lines_per_field(pal);
}

static inline uint32_t snes_region_master_hz(bool pal) {
  return pal ? SNES_PAL_MASTER_HZ : SNES_NTSC_MASTER_HZ;
}

/* SPC700 cycles per master clock: 32040 Hz x 32 SPC cycles per sample, over
 * one model second of master clocks. The runtime's APU model takes a field of
 * master clocks as 1/60 s on NTSC and 1/50 s on PAL, as LakeSnes does
 * (apuCyclesPerMaster, apuCyclesPerMasterPal). The host's wall-clock pacing
 * uses the measured field rate instead. */
static inline double snes_region_apu_cycles_per_master(bool pal) {
  return pal ? (32040.0 * 32.0) / (1364.0 * 312.0 * 50.0)
             : (32040.0 * 32.0) / (1364.0 * 262.0 * 60.0);
}

/* Name for logs. */
static inline const char *snes_region_name(bool pal) {
  return pal ? "PAL" : "NTSC";
}

#endif
