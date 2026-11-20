# Game-owned authoritative state

The shared netplay facade still synchronizes controller inputs and uses the
existing rollback driver. A game that keeps authoritative state outside SNES
RAM must include it in both the snapshot and digest domains.

Use `RtlGameInfo.state_save_extra`, `state_load_extra` and `snapshot_preflight`
for a canonical, validated image. Register `snes_state_digest_set_extra` before
starting netplay to fold that same authoritative state into the WRAM digest
partition. Its result is folded as four little-endian bytes. Return a stable
digest of game state only: cameras, windows, audio output and transport state
must stay outside it. Clear the callback after shutdown. NULL preserves the
legacy digest domain exactly.

The resolved console region is also part of the compatibility identity. NTSC
and PAL change the guest master clock and field geometry, so a per-peer
`--region` / `snes_set_region_override` must resolve to the same region on every
peer; `SNESRECOMP_REGION` and the `$FFD9` header are the other sources. Publish
the resolution with `snes_netplay_region_publish_local` after the ROM is loaded
and before `snes_netplay_start`. The rollback adapter folds it into the content
fingerprint passed to `rnet_rb_driver_set_identity`, and
`snes_netplay_poll_admit` refuses a disagreement before the first admitted frame
with reason `region_mismatch`. Both peers then soft-return to the waiting room
with a message naming each side's region and source. This is a stopgap that
leaves recomp-net untouched; a dedicated region field there would be cleaner
and is pending reviewer preference. See
[docs/REGION_PACED_TIMING.md](../docs/REGION_PACED_TIMING.md#netplay-region-agreement).

For an execution-profile barrier, call `snes_netplay_set_profile_caps` before
`snes_netplay_start`. Supply actual runtime CPU features, available backend and
qualified profile masks, architecture, and a qualification revision/namespace.
The latest recomp-net startup profile exchange settles once before rollback's
boot snapshot or the first admitted game frame. The callback receives the
selected profile and capability transcript; incorporate those in the existing
full compatibility identity before returning. Do not relax build or content
checks. Refusal or a 30-second negotiation timeout admits no frame and returns
through the normal shared lobby error path. Qualification zero retains the
legacy startup flow. Capabilities and profile stay fixed until shutdown.

Quick-save queries the complete snapshot size rather than assuming SNES state
fits 512 KiB. The existing 8 MiB transfer/file bound remains. A synchronized
quick-load starts fresh rollback history at the loaded state after the wire
epoch resets, matching the slot browser's existing behavior.

F-Zero's integration regression lives in its `tests/validate_netplay.py`:
two actual SDL peers, native/HD and asymmetric resizing, packet loss and real
rollback, exact identity rejection before frame zero, authoritative setup/race
state, a 593142-byte shared snapshot, verified slot-browser saves and quick-load
continuation. These are Windows same-build tests, not cross-build qualification.
