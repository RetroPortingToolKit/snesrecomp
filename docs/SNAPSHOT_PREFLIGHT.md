# Snapshot rejection boundaries

File and memory loaders validate the complete guest prefix with the production
serializer before writing live guest state. A malformed PPU layout header or a
truncated guest prefix is rejected without calling title serializers. The
supported RTLS versions and early v6/v7 layouts remain unchanged.

Title extensions are opaque to the framework. A title's current save output
does not describe all inputs its loader accepts: older versions may be shorter,
and foreign chunks may deliberately be ignored. Save callbacks may also capture
mutable title state. The framework therefore never calls `state_save_extra` to
validate incoming input and never infers an accepted extension size from it.

Existing `state_load_extra` callbacks retain their contract. They run once, only
when trailing bytes remain, after the guest prefix is restored. A consumer with
no extension loader continues to ignore trailing bytes. A legacy callback that
fails after mutating state can still leave those effects behind; this path is
**not transactional**, and the framework cannot undo arbitrary callback effects.

Titles requiring early rejection of invalid extensions can opt into the existing
`snapshot_preflight(data, size)` callback. It receives the unwrapped RTLS stream
and runs before any live restoration, outside the APU lock. It must validate all
supported historical chunk layouts without mutating live state; returning false
rejects the load. Its corresponding loader must accept validated payloads without
late failure. `RtlSnapshotGuestLayout` can locate fields in the **current** guest
layout; it is not a parser for every historical version. Historical formats need
their own format-aware validation. Guard identities continue to provide their
existing envelope integrity checks.

No new callback or purity requirement is imposed on existing save/load hooks.
There is no automatic validation of arbitrary legacy title chunks. This change
establishes guest-prefix rejection and preserves consumer compatibility; title
extension validation and cross-title gameplay qualification remain separate work.

The ROM-free fixture in `tests/snapshot_guard/transaction_test.c` exercises
unregistered one-byte/current chunks, ignored trailing chunks, side-effecting
save callbacks that must not run on load, and the original late-error semantics.
Its separate opt-in preflight cases check rejection before guest/title writes.
