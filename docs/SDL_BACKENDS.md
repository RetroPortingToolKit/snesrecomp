# Desktop SDL backends

SDL3 is the default desktop backend for CMake game hosts that include
`runner/runner.cmake` and call `snesrecomp_target_sdl(<target>)`.

```sh
# Default: SDL3
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/SDL3

# Explicit compatibility fallback: SDL2
cmake -S . -B build-sdl2 -DSNESRECOMP_SDL_BACKEND=SDL2
```

The helper links the matching imported CMake target, sets the host and
recomp-ui compile definitions together, and copies the selected SDL runtime
DLL beside Windows executables. Use separate build directories when comparing
backends.

## Game benchmark mode

Super Mario World and Mega Man X accept `--benchmark <frames> <rom>`. The mode
uses the SDL renderer with VSync, frame-delay pacing, audio, autosave, and the
launcher disabled, then prints one machine-readable `SNESRECOMP_BENCHMARK`
JSON record. `--benchmark-audio` keeps audio enabled for finite callback smoke
tests; its timing should not be compared with the audio-disabled benchmark.

For a fair comparison, use Release builds from the same compiler and source
revision, close other CPU/GPU-heavy workloads, warm each executable once, and
report the median of multiple equal-length runs. The benchmark output also
reports the SDL renderer and effective VSync state.


## Optional SDL2 OpenGL ES 2 renderer

Desktop builds keep their existing SDL3/OpenGL defaults. To build the SDL2
host with its native GLES2 renderer, opt in explicitly:

```sh
cmake -S . -B build-gles2 -DSNESRECOMP_SDL_GLES2_ONLY=ON
```

This selects SDL2, requests its `opengles2` render driver, and omits the
desktop OpenGL presenter and its OpenGL link dependency. If SDL2 cannot create
the requested GLES2 renderer, initialization fails instead of silently
falling back to another renderer. The option does not require GL4ES or KMS/DRM.

To return to the normal desktop configuration, use a separate build directory
without `SNESRECOMP_SDL_GLES2_ONLY`.
