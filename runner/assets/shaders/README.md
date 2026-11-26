# Shared SNES shader presets

Choose a preset in the launcher's Display settings with OpenGL output.
The empty shader selection keeps the original output and does no filtering.
Only one preset runs at a time. Adding presets preserves saved selections.

The catalog includes **xBRZ**, **HQ2x**, **HQ3x**, **HQ4x**, **CRT Soft**,
**LCD Grid**, **Sharp**, and **Warm Composite**.

The xBRZ and HQ kernels and HQ lookup images are unmodified sources from
libretro/glsl-shaders at `a35069864b4bf30f8c8e740d57206543fb7e745e`.
Exact source links and hashes are in `_scaling-sources.json`. The separate
shader programs retain their original notices: xBRZ includes GPL-3.0 and
Hyllian's MIT notice; HQ uses LGPL-2.1-or-later. Full license texts are in
`_licenses/`. These sources are loaded as GPU programs; no scaler library is
compiled or linked into the host executable.

HQ computes its edge pattern once at source resolution, scales to the named
integer factor, then presents that result with nearest sampling at the chosen
viewport size. xBRZ scales directly to the viewport. Edge samples clamp to the
source edge. The game view and HUD keep their original geometry.

The four original presets and the final nearest-copy pass are public domain /
CC0-1.0; their original attribution is retained.

Preset paths and `_passes/` filenames are stable so existing MMX configurations
continue to work. `_passes/` is hidden by the launcher picker. Users can add
their own `.glsl` and `.glslp` presets alongside the bundled ones.
