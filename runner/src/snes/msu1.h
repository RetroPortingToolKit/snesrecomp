/* MSU-1 — SNES streaming-audio + data coprocessor (near's spec).
 *
 * Two channels:
 *   - Data:  a byte-addressable file (<base>.msu) read through a seek
 *            pointer ($2000-$2003 write) + auto-incrementing read port
 *            ($2001 read).
 *   - Audio: 44.1 kHz signed-16 stereo PCM tracks (<base>-<N>.pcm),
 *            selected via $2004-$2005, played/looped via $2007, mixed on
 *            top of the S-DSP output at the host rate.
 *
 * The register window ($2000-$2007) already routes through the
 * framework's ReadReg/WriteReg dispatch (is_hw_reg covers $2000-$5FFF),
 * so wiring is just a branch there plus the audio mix in RtlRenderAudio.
 *
 * Default-OFF: with no pack present (env SNESRECOMP_MSU1 unset) every
 * entry point is inert and the registers read back as open bus (0),
 * exactly as before — output stays byte-identical.
 */
#ifndef SNESRECOMP_MSU1_H
#define SNESRECOMP_MSU1_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Optional host PCM routing. Called when selecting a track, under the APU
 * lock; do not call MSU APIs from the callback. Write the full PCM filename
 * and return true, or return false for missing-track/SPC fallback. A NULL
 * callback restores ordinary <base>-<N>.pcm lookup. Registration survives
 * chip resets; it does not change the .msu data channel or enable the chip. */
typedef bool (*Msu1TrackResolver)(void *context, const char *base, uint16_t track,
                                  char *path, size_t capacity);
void msu1_set_track_resolver(Msu1TrackResolver resolver, void *context);

/* Read the SNESRECOMP_MSU1 environment variable and arm the chip.
 *
 *   SNESRECOMP_MSU1 unset/empty  -> disabled (default; byte-identical).
 *   SNESRECOMP_MSU1=<path-prefix> -> pack base; tracks resolve as
 *                                    "<prefix>-<N>.pcm", data as
 *                                    "<prefix>.msu". e.g. C:/msu/alttp
 *   SNESRECOMP_MSU1=1 | on | auto -> enabled, base derived later from the
 *                                    ROM path via msu1_set_rom_path().
 *
 * Safe to call once at startup, before any register access. Idempotent. */
void msu1_init(void);

/* True once msu1_init armed the chip AND a usable base path is known.
 * When false, msu1_read returns 0 and msu1_write / msu1_mix are no-ops. */
bool msu1_enabled(void);

/* Supply the loaded ROM's path so an "auto" base can be derived (strips
 * the directory-preserving extension: foo/zelda.sfc -> foo/zelda).
 * Ignored when an explicit base path was given to the env var. Optional —
 * lets a game's main.c enable MSU with zero per-pack configuration. */
void msu1_set_rom_path(const char *rom_path);

/* $2000-$2007 register interface, dispatched from common_rtl ReadReg /
 * WriteReg. `reg` is the full 16-bit address ($2000-$2007). These take
 * the APU lock internally to serialise against the audio thread. */
uint8_t msu1_read(uint16_t reg);
void    msu1_write(uint16_t reg, uint8_t val);

/* Mix MSU PCM into `out` (int16 interleaved L/R, `out_frames` sample-pairs,
 * already filled with the S-DSP block). `out_frames` is whatever the host
 * device requested THIS call, at whatever rate it opened (see
 * RtlSetAudioOutputRate) -- callers are not required to call once per
 * emulated 60 Hz frame, nor with a fixed chunk size. msu1_mix continuously
 * resamples the fixed 44.1 kHz track rate onto that rate proportionally to
 * out_frames, carrying the fractional resample phase across calls (the same
 * pattern rtl_render_native uses in common_rtl.c for the S-DSP output), so
 * playback speed and pitch are correct regardless of call frequency/size and
 * there is no seam at chunk boundaries.
 *
 * MUST be called with the APU lock already held — it is invoked only from
 * inside RtlRenderAudio's locked region. No-op when disabled / not
 * playing. */
void msu1_mix(int16_t *out, int out_frames);

#endif /* SNESRECOMP_MSU1_H */
