# X2 rocket and Serges margin follow-up

The rocket ride exposed uniform terrain being rejected, physical scroll aliases
being interpreted as 1024px maps, and mode2 offset-per-tile drawing bypassing the
world source. The resolver now accepts self-validated uniform BG1 terrain while
retaining the moving-object BG2 safeguard, uses the actual 512x256 map aliases,
and receives effective raster/OPT offsets from the mode2 drawing path. X2 also
uses the existing OAM-only HUD anchor so BG3 scenery is no longer displaced.

Serges's authored room ends at world X0B80. Continue only its BG1 backdrop beyond
that boundary while the original camera is locked and the machine is resident.
The title supplies this room selection; the shared provider offers an optional
right boundary and a margin-only override. Native pixels always retain VRAM,
and the real left-side scenery/door remains visible at wider aspects. BG2 and
OAM, including the moving machine, pilot and platforms, are not repeated.

Validation uses private muted profiles and verified unmodified USA ROMs:

- The actual rocket control/camera ascent and original arena handoff complete
  over 780 frames at hybrid342 and interpreter684. No audited result flags are
  written. Same-width before/after samples agree in full WRAM and VRAM.
- All 24 cropped native-view samples agree outside the relocated HUD. One
  cross-width arena sample has a differing scratch byte at WRAM10; full
  cross-width RAM identity is not claimed.
- Serges entrance replays at hybrid342, hybrid684 and interpreter684 alter only
  the right gutter. All native/left pixels, full WRAM and VRAM agree with the
  prior build in 21 samples per configuration. Current-build save/load agrees
  in all RAM and pixels at342 and684.
- Production C tests cover aliasing, uniform terrain, dynamic object rejection,
  margin-only precedence over uploads, preservation of native pixels and the
  left source, and clearing the optional boundary for ordinary rooms. Actual
  mode2 composition and inactive-provider rendering checks also pass.
- Accepted X3 factory and Byte replays retain all sampled pixels and WRAM
  through the rocket update. The optional boundary defaults to zero for X3.
- Short serial software/debug samples showed about235..256fps versus
  baseline236..249fps. These establish headroom, not release frame-time parity.

The original-ROM arena reference is valid. Unsuccessful original rocket and
Serges preparation attempts are excluded. The reported incomplete Serges
entrance artwork remains a failed owner observation pending a focused replay;
actor/OAM presence alone does not close it. Owner visual acceptance of the
rocket margins and background extension is also pending. Byte and doors are
already accepted and require no repeat owner handoff.

Evidence: `G:/Temp/Codex/mmx23-trigger-audit-20261026/burndown`.
