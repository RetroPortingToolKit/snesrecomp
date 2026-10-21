# Bounded cheat and teleport routes

`tools/sneslib/route.py` provides `FrameDriver` for game-owned QA scripts.
It records commands and replies, validates frame advancement, releases injected
buttons on failure, bounds WRAM conditions, and captures screenshots, full WRAM,
CPU/PPU state, execution statistics and coverage checkpoints. Game-specific
addresses, original loader prerequisites and progression assists stay in the
game repository.

Launch a private executable/config/save directory with muted output and preserved
audio emulation. On Windows, use `subprocess.CREATE_NO_WINDOW`; dummy SDL audio
and video drivers work for software-rendered unattended cases. Restore normal
audio in builds handed to the owner. Do not load old savestates into a changed
runtime as a substitute for a fresh boot.

```python
from pathlib import Path
from sneslib.client import DebugClient
from sneslib.route import FrameDriver

driver = FrameDriver(DebugClient(port, timeout=20), Path(private_run_directory))
try:
    # Launch running, because custom hosts may expose TCP before initialization.
    driver.wait_started()
    driver.step(60, "right+y")
    driver.snapshot("entry-motion")
finally:
    driver.close()
```

The host must run from `private_run_directory`: screenshot and coverage paths
are relative to its cwd. Use plain snapshot names. The caller owns process
startup, ROM verification, title configuration, and process termination or the
host's finite normal-exit argument. The separate debug `pause` command is
disabled by server policy; bounded `step` is the supported testing interface.

`wait_started()` records a variable initial frame and then enters bounded
stepping. It is appropriate for gameplay reconnaissance, not an exact power-on
comparison. For instruction-cost qualification use continuous, identical
startup-script routes, a finite normal exit, and `tools/coverage_replay.py` with
matching guest state, presented pixels, video resources and relevant audio.
Stepped captures establish observations; they do not prove native equivalence.
For expensive software renderers pass `max_step_frames=5` (default 60), so a
healthy slow renderer stays within the server's bounded step deadline.
An explicit server timeout leaves that step pending. The driver polls the
frame counter for its original target, for up to `step_timeout=30` seconds
(maximum 60), without issuing another step. A lost reply remains an error.

Super FX player and sprite state can live in cartridge RAM. Use
`read_sram <offset_hex> <length_decimal>` and
`write_sram <offset_hex> <hex_bytes>` for that memory, rather than a WRAM
address with the same low bytes. Writes require a stopped frame boundary and
validate every byte and the complete range before modifying memory. Offsets
are within the cartridge RAM buffer; these commands do not write the SNES bus.

`DebugClient.query(..., retry=False)` disables reconnect/replay for ambiguous
requests. All `FrameDriver` commands use it: losing the reply to `step`, reset,
or a state load does not prove that the mutation was not executed. Inspect the
journal and start a fresh case rather than repeating an uncertain mutation.

Check each host's script syntax before using `--script`: older custom hosts
accept only wait/press/loadstate and can ignore the generic host's RAM edits.
Verify actual rendered dimensions and active mod state before labelling a case
widescreen. Raw PPU dumps may omit a game's separate enhanced compositor.
