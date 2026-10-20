# Explicit SRAM image publication

Tracking: `beads-8wg.2.140`. Consumer: Super Mario World native co-op PR #19,
`beads-8wg.3.29`.

## Motivation and boundary

SMW's separate host selects normal/co-op save namespaces in the original
player-count menu. Observed synchronous namespace writes took 149.76 ms and
863.44 ms, draining the audio queue. Its worker needs a copied byte image and
captured file path; calling the global-root/global-SRAM writer from that worker
would race with guest execution and the next namespace selection.

`RtlWriteSramImage(path, bytes, size)` performs the existing durable publication
protocol using only explicit inputs. The caller owns directory creation, byte
lifetime and serialization of writes to the same path. The function does not
access mutable SRAM, save roots, guest devices or SDL. Different file paths may
be written independently. Allocation/invalid-input failures return zero before
opening a file.

The existing `RtlTryWriteSram` keeps its no-SRAM success behavior, creates the
current save directory, captures the path, and delegates synchronously. Its
public `RtlWriteSram` wrapper is unchanged. Existing callers keep the same
interface and execution policy.

## Diff audit

One existing publication body is extracted; its synchronous caller delegates.
There is no generated-code edit, multi-site caller migration or new automatic
threading in the framework.

Preserved behavior:

- Write the temporary file, flush userspace buffers and use Windows `_commit`
  or POSIX `fsync`, then close it.
- Preserve one previous revision through the existing backup rotation.
- Report failed writes/rotation/publication and restore the previous file after
  a failed publication; do not fall back to overwriting the destination.

The new API allocates checked sidecar-path storage rather than truncating
caller-supplied paths into the old local buffers. The worker implementation,
pending-image retention/readback and shutdown policy remain game-owned.

## Validation

```text
python tests/data_pack/check_sram.py <C compiler>
```

The harness compiles the actual writer functions extracted from
`common_rtl.c`. It checks real file contents, backup revisions, failed temporary
file creation and preservation of the last good save. Added cases change live
SRAM and the active root while publishing a different explicit image/path,
verify both namespaces, and reject invalid inputs without replacing a file.
This test is already registered as `sram_publication` in the content-pack CTest
suite and the Linux analyzer workflow.

Local Windows/MinGW tests and the combined SMW SDL3 build pass. The game-owned
worker harness passes with SDL2 and SDL3, including deliberately blocked I/O,
FIFO/copy ownership, latest-image readback and failure/retry. Actual normal/co-op
switches and a 3,600-frame fresh SMW entry pass with zero audible drops and zero
underflows. Both namespaces' resulting files match synchronous baseline runs.
These game observations do not establish every title/platform's audio health.
CI qualification of the published framework revision is recorded on its PR.
