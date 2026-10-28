# Complete SM mod-set publication — 2026-10-28

The Super Metroid 40-feature catalog produces 2,526 bytes of canonical settings.
The desktop session buffer was 2,048 bytes; the legacy SNES lobby bridge was
512 bytes. Both now use recomp-net's public `RNET_MODSET_TEXT_MAX` (4,096,
including NUL). The engine pins recomp-net `445eed76`, whose bounded chunked
startup transfer preserves the existing 1,200-byte session packet limit.

The paired recomp-ui change is `7fdd3a87`: publication, host-change watching,
adoption cookies and launch checks use the complete settings field. Native
tests verify one adoption of the full list and refusal of a tail mismatch.
That UI also passes its host tests against the original `8c76fb6` net pin.

Validation retained under `G:/Temp/Codex/sm-qol-20261027`:

* `snes-caps-test.log`: real SNES bridge/client round trips, including 2,526
  bytes, preserving widescreen and other existing fields.
* `netplay-all-ctest.log`: all 34 native recomp-net tests pass; loadable-module
  ABI test also passes. Windows library/UDP/registry checks pass.
* `netplay-actual-large-final/`: actual SM settings in two-/three-seat driver
  cases, with latency, jitter and 2% loss. Matching sets run and replay;
  mismatch refuses both peers at frame zero and stays refused.
* `netplay-catalog-probe/checks.json`: real runtime comparison rejects a valid
  option difference beyond byte 1,024; adoption and restoration match exactly.
* `netplay-real-caps.log`: the complete six-package plan/settings fit and
  round-trip through the existing online caps contract (3,690-byte fixture).
* `netplay-ui-host-complete.log`, `ui-legacy-host.log`: complete UI settings
  publication/adoption/launch gating and compatibility with the original pin.

Recomp-net now reports wire version 3 and module ABI 2. Any other engine bridge
that duplicates the public settings field must update its size before adopting
this pin. No other engine/game dependency pins are changed by this work.
The validated F-Zero/SMW removal-preservation candidates remain intact.

The SM integration branch still needs its final regeneration, network-enabled
game build and actual game-peer checks after pinning these commits. This note
does not claim those pending gates passed. No merge, push or release.
