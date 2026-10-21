#ifndef SNESRECOMP_RENDER_WORKERS_H
#define SNESRECOMP_RENDER_WORKERS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRenderWorkers SnesRenderWorkers;
enum { SNES_RENDER_WORKERS_MAX = 32 };
/* Each half-open range [begin,end) belongs to exactly one callback. slot is
 * stable for a participant, in [0,Count(pool)); slot 0 is the submitting thread.
 * Different callbacks may run concurrently. Inputs must remain immutable and
 * output ranges must not overlap. Per-participant scratch may be indexed by slot. */
typedef void (*SnesRenderRangeFn)(void *context, size_t begin, size_t end,
                                unsigned slot);

/* max_participants includes the caller. 0 selects up to four participants,
 * leaving one logical CPU free; SNESRECOMP_RENDER_WORKERS=1..32 overrides auto.
 * Explicit limits are bounded by available CPUs and SNES_RENDER_WORKERS_MAX.
 * Disabled/unavailable workers return NULL, which all other APIs accept as a
 * synchronous serial fallback. No SDL, graphics backend or emulated state. */
SnesRenderWorkers *SnesRenderWorkersCreate(unsigned max_participants);
unsigned SnesRenderWorkersCount(const SnesRenderWorkers *pool);

/* Returns only after every callback has finished; context and buffers can then
 * be reused. Persistent workers sleep between batches. grain is a scheduling
 * hint (0 means 1); small/serial batches use one whole-range callback at slot 0.
 * Concurrent top-level submitters are serialized. Nested submissions from any
 * callback are rejected with false, as is a NULL callback with nonzero count.
 * An empty range succeeds without callbacks. */
bool SnesRenderWorkersRun(SnesRenderWorkers *pool, size_t count, size_t grain,
                         SnesRenderRangeFn callback, void *context);

/* Owner must stop submitting first; destruction joins every worker. Never call
 * from a callback or race destruction with another API call. NULL is harmless. */
void SnesRenderWorkersDestroy(SnesRenderWorkers *pool);

#ifdef __cplusplus
}
#endif
#endif
