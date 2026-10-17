# Host DSP voice routing

`dsp_set_output_voices(dsp, mask)` selects audible dry voices and their native
FIR echo without changing guest simulation. The default `0xff` takes the
ordinary mixer path and allocates nothing. Hosts must hold `RtlApuLock` when
changing the mask. Allocation failure leaves ordinary audio available.

The filter imports canonical register and channel outputs into a presentation
mirror, retaining a separate echo RAM/FIR history. The original mixer, echo
writes, voice clocks, pitch modulation, noise and forensic checks still run
once against guest memory. The mirror reuses the native echo implementation;
it never runs another SPC or voice simulation. The presentation pointer is
before `Dsp.ram`, outside snapshot serialization and state digests. Existing
saved DSP stream sizes remain unchanged.

Rollback's resimulation callbacks suspend this presentation echo while discarded
ticks run, then resume it alongside the preserved live output ring. A host must
also avoid restarting its external soundtrack from speculative state-load
callbacks; `RtlStateGeneration` distinguishes actual timeline changes after a
rollback completes. Cold profile initialization closes previous MSU audio/data
handles before selecting another soundtrack.

This is portable C, with no assembly or additional CPU feature requirement.
`tests/audio/cmake_output_voices` qualifies native selected-voice echo,
canonical DSP/APU RAM isolation, snapshot layout, ordinary output restoration
and resimulation suspension under default, TRACE=0, TRACE=1 and COSIM builds.
The current integration was validated on Windows x64; other-target runtime
qualification is not implied.
