# Windows startup and frame delivery investigation (2026-09-30)

Tracked in `beads-h7t4` (shared startup, GitHub #129) and `beads-8wg.1.56`
(Mega Man X frame delivery). MMX used the shared SDL3 desktop host, native
60.098811862 Hz simulation, audio enabled, and its existing mods/settings.

## Reproduced startup cause and fix

The desktop host forced stdout/stderr to `_IONBF`. MinGW formatted output
can then issue writes for individual characters. PowerShell `Start-Process`
redirection supplied a disk handle with `FILE_WRITE_THROUGH`: each tiny write
waited for storage. The apparent multi-second gaps between startup milestones
were mostly the previous diagnostic line blocking, not config/ROM parsing.

A standalone C probe, with no SDL, ROM, or game code, measured:

| Operation | Elapsed |
| --- | ---: |
| Unbuffered formatted diagnostic line, PowerShell redirected file | 3.39–3.85 s |
| Buffered formatting plus one flush, same redirect | 0.033 s |
| Reading the complete config | 0.00023 s |

`NtQueryInformationFile(FileModeInformation)` reported mode `0x22`
(synchronous, write-through) for PowerShell redirects, versus `0x20` for
an ordinary Python-opened file. A fresh console was fast as well. No Windows
file flags are overridden by the fix.

The host now supplies 4 KiB stdio buffers **before** its first breadcrumb.
Breadcrumb boundaries flush both streams; boot progress and fatal reports
remain immediately available. Existing crash handlers flush stderr. Other
diagnostics can remain buffered until a breadcrumb, explicit flush, a full
buffer, or normal exit. The in-memory crash breadcrumb ring is unchanged.

Same executable/config/hidden scripted launcher, before and after:

| Launch path | Before GUI initialization | After |
| --- | ---: | ---: |
| PowerShell `Start-Process`, stdout/stderr redirected | 13.232 s | 0.115 s |
| Python, ordinary file redirection | 0.019 s | 0.016 s |

These are breadcrumb timestamps for entering launcher initialization, not
physical first-pixel measurements or a cold filesystem-cache benchmark.
The owner also reported double-click slowness; that path needs confirmation
on the updated build. This fix addresses the reproduced write-through stall,
not every possible source of Windows process-start latency.

## Frame timing evidence and limits

A stale private MMX test process from September 28 was still consuming one
full CPU core. It was stopped before the frame comparisons. It could have
added contention, but its role in the reported stutter was not established.

The primary display reported 165 Hz; SDL selected Direct3D 11. Independent
offline runs captured 1,200 frames with real audio and presentation. The
steady section (frames 601–1200) gave these completed-present intervals:

| Run | Mean | 95th percentile | 99th percentile | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Offline, VSync on | 16.639 ms | 17.875 ms | 21.436 ms | 26.178 ms |
| Offline, VSync off | 16.640 ms | 17.424 ms | 17.976 ms | 18.664 ms |
| Local UDP peer 0, VSync on | 16.640 ms | 17.934 ms | 18.626 ms | 20.124 ms |
| Local UDP peer 1, VSync on | 16.641 ms | 17.873 ms | 18.496 ms | 20.680 ms |

Both real desktop peers agreed on boot digest `c5d5e6fb`. Early large intervals
also coincide with native loading frames spanning 74.65 or 19.16 SNES periods;
those must be distinguished from ordinary one-period gameplay.

Replacing the wait loop with `SDL_DelayPrecise` produced mixed results:
VSync-on p99 improved to 19.203 ms, but VSync-off p99 worsened to 20.722 ms.
That experiment was reverted. No simulation-rate, audio, VSync, or display
settings were changed for the owner.

At fixed 165 Hz, 60.0988 Hz content cannot occupy an equal integer number of
refreshes per game frame (typically two or three, about 12.12/18.18 ms).
This is a plausible contributor to visible uneven scrolling. Variable refresh
may change that behavior, and these traces do **not** measure monitor scanout
or establish whether VRR is engaged. They demonstrate host interval variation,
not a complete diagnosis of every user-observed hitch or a WAN performance
certification. Player confirmation and a scanout/VRR comparison remain open.

## Reusable, low-overhead capture

Set `SNESRECOMP_FRAME_TIMING` to an absolute CSV path and launch normally.
Optionally set `SNESRECOMP_HOST_PROFILE_START_FRAME` (default 1). Exit normally
to write the file. The feature also activates aggregate host profiling in
offline and netplay loops. Without either profiling option it does not allocate
the timing buffer or sample performance counters for these measurements.

Up to 36,000 completed presentations are retained in memory, about ten minutes
at 60 FPS. No file writes occur per frame. The CSV records simulation frame,
completed-present timestamp, native guest periods, and accumulated guest,
raster/rewind, surface acquisition, composition, upload/present, state trace,
game hook, event pump, and deadline wait times since the previous presentation.
Repeated frame numbers can represent netplay waiting or presentation without
advancing the guest. The first row includes work since profiling activation.
Network admission/replay and miscellaneous host work are not separately
attributed; stage sums need not equal the presentation interval.

Renderer, effective VSync, display refresh, and simulation rate appear in
the log when profiling is requested. Recording stops when the buffer is full;
it does not grow without bound. Exit-time CSV I/O is excluded from the reported
run/profile duration. The older pixel-checksum `PRESENT_LOG` flushes per frame
and should not be used to measure subtle timing differences.
