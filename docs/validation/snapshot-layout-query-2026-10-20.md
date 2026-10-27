# Serializer-derived snapshot WRAM location

Tracking: `beads-8wg.2.141`. Consumer: SMW native co-op, game PR #19.

## Interface and ownership

`RtlSnapshotGuestLayout(version_out, wram_offset_out)` returns the current RTLS
guest-prefix size and locates its 128 KiB WRAM field through `snes_saveload`.
The offset is relative to the RTLS header, excluding any `RSGUARD` envelope.
It is not an input decoder: consumers must validate saved version and length
before reading input bytes at that offset. A missing field or counting overflow
returns zero. Either output pointer can be null.

The memory stream has optional field-location metadata. It matches both the
live field's address and its serialized size while counting; other equal-sized
fields cannot supply the WRAM offset. Normal save/load streams leave that
metadata zero-initialized and retain their existing behavior.

The query uses the existing APU lock and current-schema serializer. It restores
the previous serializer version and the CPU E mirror normalized by serialization.
It neither applies a state nor writes a file. `RtlSnapshotGuestSize` remains a
compatible wrapper. No guest format/version changes.

## Validation

`python tests/snapshot_guard/test_layout.py` compiles the production stream and
query functions with a serializer/lock test double. Nine varied layouts, an
equal-sized decoy field, omitted WRAM, overflow, unchanged WRAM/CPU mirror,
version restoration and the legacy size interface pass on Windows/MinGW.
The normal Python test discovery includes this test.

The combined SMW SDL3 build also passes. Its real preflight now checks live
Yoshi mouth indices against serialized WRAM before applying any guest bytes.
Ten running native/bounce checks load a valid two-mount state, then attempt a
duplicate, self-reference, out-of-range reference or an actual old duplicate
save. Each rejected attempt yields a **byte-identical entire guarded snapshot**
to a missing-slot control after the same running interval. Accepted states
still load. The game unit preflight also covers v9/v10 compatibility, truncation,
malformed schemas, all invalid nonnegative mouth indices and atomic staging.

Real evidence is retained in the consumer worktree under
`build-adaptive/playtest/goal-20261019/mouth-ownership/preflight-live`;
the permanent consumer audit and reproduction tool are committed in game PR #19.
This qualifies the layout interface and bounded preflight behavior, not the
full SMW campaign or every cross-version guest format.
