# MSU-1 support

SNESRecomp's shared runner implements near's MSU-1 streaming-audio and data
coprocessor. It exposes two independent channels through `$2000-$2007`:

- **Audio:** 44.1 kHz signed 16-bit stereo PCM tracks
  (`<base>-<N>.pcm`), selected and controlled by the guest.
- **Data:** a byte-addressable `<base>.msu` file with a seek pointer and
  auto-incrementing read port.

The hardware model is available to every game, but remains inert unless a pack
is selected and the game contains an MSU-1 driver.

## Runtime implementation

| Piece | Location |
|---|---|
| Register, PCM, resampling, and data-channel core | `runner/src/snes/msu1.{c,h}` |
| `$2000-$2007` register dispatch | `runner/src/cpu/common_rtl.c` |
| PCM mix after S-DSP rendering | `runner/src/cpu/common_rtl.c` |
| Environment initialization | `runner/src/cpu/common_cpu_infra.c` |
| Shared CMake source list | `runner/runner.cmake` |

`is_hw_reg()` already routes `$2000-$5FFF` through the hardware-register path,
so MSU-1 does not need a separate memory-map exception.

### Threading

Guest reads and writes happen on the CPU thread and take the existing recursive
APU mutex through `RtlApuLock`. `msu1_mix()` runs inside
`RtlRenderAudio()` while that mutex is already held, so it deliberately does
not acquire it a second time.

### Timing and resampling

The runner renders one 60 Hz audio block at a time. MSU-1 therefore consumes
`44100 / 60 = 735` source frames for each block and linearly resamples them to
the host block size. This keeps PCM playback on the same block clock as the
S-DSP while allowing different host output rates.

## Default-off behavior

With `SNESRECOMP_MSU1` unset and no game-side launcher selection:

- `msu1_enabled()` is false;
- reads from the MSU-1 range retain the runner's previous open-bus value of
  zero;
- writes are ignored; and
- the audio mixer is a no-op.

A PCM pack alone is not enough. An unmodified game has no code that selects
tracks or writes the MSU-1 control registers.

## Player integration

Super Mario World and A Link to the Past provide the complete user-facing path:

1. Their regeneration scripts apply a bundled MSU-1 driver patch to a
   **temporary copy** of the user's verified stock ROM.
2. SNESRecomp analyzes that temporary image, so the driver becomes native code
   in the executable.
3. The temporary image is discarded. The user's stock ROM is neither modified
   nor redistributed.
4. At runtime, the launcher accepts a compatible PCM-pack directory and still
   loads the stock ROM.
5. With no pack enabled, the driver falls back to the authentic SPC soundtrack.

Drivers and track mappings are game-specific. SMW's audio-only driver, for
example, does not use the same track layout as SMW MSU+ or Plus Ultra. Consult
the game repository's README and patch attribution before choosing a pack.

## Direct environment usage

Game integrations may expose the same selection in a launcher, but the shared
runtime can also be configured directly:

```sh
# Directory: find the dominant <name>-<N>.pcm base automatically.
SNESRECOMP_MSU1=/path/to/msu_pack ./game rom.sfc

# Explicit prefix: resolve <prefix>-<N>.pcm and <prefix>.msu.
SNESRECOMP_MSU1=/path/to/msu_pack/alttp_msu ./game rom.sfc

# Derive the prefix later from msu1_set_rom_path().
SNESRECOMP_MSU1=auto ./game rom.sfc
```

`msu1_set_rom_path()` is useful for `auto` mode. A game should call it after
resolving the runtime ROM path.

Standard PCM files begin with an eight-byte header: the ASCII magic `MSU1`
followed by a little-endian 32-bit loop point measured in stereo sample frames.
The remaining data is raw signed 16-bit little-endian stereo PCM at 44.1 kHz.
The runner also accepts headerless raw PCM.

## Register map

| Register | Access | Behavior |
|---|---|---|
| `$2000` | read/write | Status on read; low byte of the 32-bit data seek on write. |
| `$2001` | read/write | Auto-incrementing data port on read; second seek byte on write. |
| `$2002-$2003` | read/write | Identification bytes on read; remaining seek bytes on write, committing on `$2003`. |
| `$2004-$2005` | read/write | Identification bytes on read; 16-bit track select on write, committing on `$2005`. |
| `$2006` | read/write | Identification byte on read; volume on write. |
| `$2007` | read/write | Identification byte on read; play/repeat control on write. |

The identification string is `S-MSU1`. A missing or invalid track sets the
audio-error status bit so a guest driver can fall back to native music.

## Validation status

The core and its integration points compile in the shared CMake and MSVC source
paths. SMW and ALttP exercise the live launcher, stock-ROM, compiled-driver, and
PCM-pack workflow. A dedicated standalone register/timing conformance suite is
not yet present; additions should validate status transitions, seek behavior,
loop points, missing-track fallback, and sample-block timing.
