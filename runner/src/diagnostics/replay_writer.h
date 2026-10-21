#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One simulation producer, one disk worker. All calls except Destroy only
 * copy into a fixed 16 MiB queue; a slow sink stops capture, never waits.
 * The worker owns files and hashes immutable copies of completed frames. */
typedef struct SnesReplayWriter SnesReplayWriter;
SnesReplayWriter *SnesReplayWriterCreate(void);
bool SnesReplayWriterBegin(SnesReplayWriter *, const char *path, const char *header,
                          const char *config, const char *mods, const char *effective);
bool SnesReplayWriterSnapshot(SnesReplayWriter *, const void *, size_t);
bool SnesReplayWriterLine(SnesReplayWriter *, const char *);
bool SnesReplayWriterFrame(SnesReplayWriter *, unsigned frame,
    const void *wram, size_t nw, const void *coop, size_t nc,
    const void *views, size_t nv, const void *vram, size_t np);
bool SnesReplayWriterRemove(SnesReplayWriter *, const char *path);
bool SnesReplayWriterEnd(SnesReplayWriter *);
const char *SnesReplayWriterError(SnesReplayWriter *);
/* Only at shutdown or an explicit transition to offline playback. */
const char *SnesReplayWriterDestroy(SnesReplayWriter *);
#ifdef __cplusplus
}
#endif
