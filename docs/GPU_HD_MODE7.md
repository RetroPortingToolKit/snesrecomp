# Shared GPU Mode 7

Desktop hosts default to high-resolution Mode 7 at **8x**, with a single
**High-resolution Mode 7** feature offering integer scales **1..64**. These
are local presentation settings, not guest memory or simulation state.

```ini
[Graphics]
Mode7Enabled = 1
Mode7Scale = 8
```

Auto presentation uses desktop OpenGL when available. Explicit renderer choices
remain explicit. Unsupported scenes/devices use native output; the default
never starts an expanded CPU renderer. SDL/Android and builds with
`SNESRECOMP_HD_GPU=OFF` retain native rendering. Ordinary 2D frames allocate no
HD packet or GPU source cache. Frame blending currently uses native output.

## Capture and presentation

`SnesHdCapture` lazily captures an ordinary PPU frame at its first Mode 7 row.
It owns source images, palettes, native pixels, transforms and composition
metadata. Reset/load discards the published packet. Detached/guest-scoped
rendering cannot publish into the live presenter.

Custom compositors publish through `SnesDesktopHostGame.hd_frame`; their native
output, camera and widescreen policy remain authoritative. Do not stretch the
stock PPU packet over a different game-owned image. Prepared affine cameras and
owned character maps use the same job contract as register-based PPU rows.

`SnesHdGpuRender` owns a bounded offscreen target. VRAM/maps stay resident and
are uploaded when their content changes. CPU preparation/uploads depend on
native scene dimensions, not the square of the resolution scale. Nearest
presentation evaluates exact visible centers of the requested raster at window
resolution, equivalent to a full raster followed by nearest sampling. It does
not create a giant 64x intermediate the window cannot show. Linear filtering
and shader presets retain a bounded full target because they inspect adjacent
texels. Hardware/memory admission may lower that target.

Foreground opacity is explicit premultiplied alpha, retaining opaque black
HUD/OSD pixels. The presenter restores caller GL state and preserves native
frames for debugging, snapshots, rewind and unsupported rows. GL3.3 handles
register jobs; interpolated double-precision camera jobs require GL4. The
portable bounded ROI consumer remains available as a reference/host fallback.

## Qualification and consumers

- CPU checks cover native identity, owned-source isolation, lazy capture,
  bounds, fallback, sprites, windows/color math and fractional scanout.
- GPU checks compare 504 ROIs and 252 visible-sample cases at 1/2/3/4/16/32/64x,
  plus shared-target, foreground and GL-state checks.
- F-Zero publishes prepared cameras/course maps through this shared backend.
  The owner approved the 32x/64x interactive preview on 2026-10-30. This is
  responsiveness/visual evidence, not a measured percentage gain or
  qualification of every GPU or the original Xbox.
- Super Metroid can adapt its existing rotating Mode 7 scenes while retaining
  adaptive widescreen, native HUD and 2D composition. Its adapter needs its own
  consumer qualification.
- Mega Man X's custom scene compositor handles Mode 1. No useful Mode 7 scene
  has been established for a dedicated adapter. Raising a setting or changing
  a pin alone does not establish a visual upgrade.

Tests: `tests/ppu/ppu_mode7_hd_test.c`, `ppu_hd_gpu_test.c` and
`hd_host_mod_test.c`. Retain portable/native fallbacks under the same interface.
No new netplay feature or simulation-profile negotiation is introduced:
presentation jobs do not affect the guest, and peers can present locally at
different resolutions without changing simulation.
