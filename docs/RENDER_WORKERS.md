# Reusable rendering workers

`runner/render_workers.cmake` supplies an explicit, per-target C/C++ helper for
renderers that can split work into independent ranges. It adds no threads or
dependencies to games that only include `runner.cmake`. It does not change the
PPU, CPU execution, save states, or graphics backend. This is CPU scheduling,
not GPU rendering.

```cmake
include("${SNESRECOMP_ROOT}/runner/render_workers.cmake")
snesrecomp_target_render_workers(MyGame)
```

The implementation uses native Windows threads or POSIX pthreads. No SDL,
OpenMP or additional Windows DLL is required. Create once, reuse between
frames, and destroy after the owner stops submitting work:

```c
#include "render_workers.h"

static void draw_rows(void *context, size_t begin, size_t end, unsigned slot) {
  Frame *frame = context;
  Scratch *scratch = &frame->scratch[slot];
  for (size_t row = begin; row < end; ++row)
    draw_row(frame->snapshot, frame->pixels, scratch, row);
}

SnesRenderWorkers *workers = SnesRenderWorkersCreate(0);
/* Allocate one private scratch entry per SnesRenderWorkersCount(workers).
 * Allocation or thread creation failure returns NULL: Count(NULL) is 1 and
 * Run(NULL, ...) runs synchronously on the calling thread. */
SnesRenderWorkersRun(workers, frame.height, 8, draw_rows, &frame);
/* Every row is complete now; frame and buffers can be reused. */
SnesRenderWorkersDestroy(workers);
```

The count includes the caller, which participates at slot zero. Automatic
selection uses up to four participants and leaves one logical CPU free. Set
`SNESRECOMP_RENDER_WORKERS=1` before launch to force serial rendering, or
`2..32` to override automatic selection for profiling. Explicit limits passed
to `Create` ignore the environment override; all limits are bounded by the
available logical CPUs and 32. Invalid environment values use automatic
selection.

Ranges are half-open and non-overlapping. The grain is a scheduling hint;
small or serial batches may arrive as one whole range. Each background slot
keeps its thread for the pool's lifetime, and workers sleep between batches.
Top-level submissions from multiple threads serialize. A nested submission
from any callback returns false instead of deadlocking. Empty ranges succeed;
a missing callback with a nonempty range fails.

The caller must publish immutable source data before submission. Each slot
needs private scratch, and callbacks must write disjoint output regions.
Do not pass a live PPU or other emulated state that simulation can mutate.
The completion barrier makes subsequent simulation, state loads and rewind
safe only when the host performs them after `Run` returns. Do not destroy a
pool from a callback or race destruction with an API call.

F-Zero uses this helper for HD racing and frozen finish scenery. Native
composition and flat menus retain the serial path. Its scanline snapshots are
immutable during the batch; each participant has separate PPU and row scratch.
The shared PPU remains serial because live register writes cannot simply be
scheduled as independent rows.

The ROM-free test suite covers range coverage, overflow, synchronous
completion, persistent identities, simultaneous submitters, nested-call
rejection, serial fallback, and repeated creation/shutdown:

```text
cmake -S runner/tests/render_workers -B build-render-workers
cmake --build build-render-workers --config Release
ctest --test-dir build-render-workers -C Release --output-on-failure
```

CI runs this suite on Windows, Linux and macOS. F-Zero separately compares
rendered images against its serial renderer and measures whole-game pacing.
