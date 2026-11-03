# DSP dry/echo mixing

`SNESRECOMP_DSP_MIX_IMPL=LLE|NATIVE` selects voice-bus mixing at build time.
The default is `LLE`; `NATIVE` is an opt-in shared-engine experiment pending
combined title qualification. Both paths are portable C. CMake reports the
effective choice. Keep the game's other configuration arguments, for example:

```sh
cmake -S . -B build -DSNESRECOMP_DSP_MIX_IMPL=NATIVE
cmake --build build
```

The native pass computes each voice's signed, scaled left/right contribution
once, then accumulates the dry and enabled echo buses in the original voice
order, saturating each bus after each contributing voice. The optional output
voice mirror uses the same fusion with its independently masked voice outputs.
The scalar path retains the second echo-input pass.

SPC execution, CPU/SPC port ordering, timers, live BRR RAM reads, Gaussian
interpolation, key-on/off and envelope changes, DSP register readback, FIR
rounding, echo feedback/RAM writes and PCM publication keep their current
behavior. The enhanced shadow mixer may substitute the dry total but does not
modify canonical voice outputs. Host output-rate conversion, the occupancy
servo and recovery fades are unchanged. No music-driver ABI is assumed.

The exact contract is identical signed 16-bit stereo PCM, sample counts and
DSP/RAM effects. Both implementations use the existing state fields and ring;
there is no new cache or unfinished job to serialize. Current-build snapshots,
rewind and rollback retain that representation. Runtime implementation changes
and old-state migration are not part of this selector.

The ROM-free differential test builds both implementations in one executable:

```sh
python tests/audio/run_dsp_mix_test.py --cc gcc
```

It compares PCM, DSP state and all 64 KiB of shared RAM across 65,536 samples
with live BRR/register writes, all 256 output voice masks, echo, suppression,
ring wrap and state restore. Built-game measurements and current-build state
checks remain necessary; a component result alone does not prove a game gain.
