# X2/X3 door presentation

X1 already selected the boss-door column facing the camera's current room.
Port that structural rule to the shared X2/X3 exact background source. The
outward-facing duplicate samples the adjoining wall. Require all three
mirrored metatiles and distinct cap/body characters; X2's symmetric wall
decoration is a negative fixture, not a second door style.

The world provider can explicitly return a presentation override. Only BG1
opts into native-region overrides. Ordinary exact results preserve native
VRAM and recent game writes; a hidden door face must also suppress recent
uploads of that face during animation. Other world-provider consumers retain
their existing behavior.

Validation in the paired X2/X3 trace builds:

- Production C tests cover measured X2/X3 door words, rejection of the similar
  wall, native pixel identity and explicit override precedence over game writes.
- X2 Serges approach and interior show one face per doorway on both sides.
- X3's 400-frame entry/open/cross/close sequence was sampled every four frames
  against a simultaneous native-width replay. Excluding relocated HUD pixels
  and precisely the hidden 16-pixel face, all 100 frames match: zero other
  differing pixels, including the retained animated door face.
- Continuous and save/load branches match WRAM and every output pixel through
  the X2 door crossing and X3 crossing/intro. The title load callbacks reset
  unsaved presentation history rather than retaining the abandoned timeline.
- X2's muted, unthrottled software/debug build advances about 227 frames/sec
  in the measured doorway scene. This measures simulation headroom, not a
  display frame-time benchmark.

Private ROM/state evidence: `G:/Temp/Codex/mmx23-trigger-audit-20261026/feedback3`.
The broader audits and owner acceptance remain open. This does not qualify the
previously reported X2 rocket margins or Serges entrance artwork.
