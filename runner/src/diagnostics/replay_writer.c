#include "diagnostics/replay_writer.h"
#include <recomp_diagnostics/replay_c.h>
#include <stdlib.h>
#include <string.h>

enum { QUEUE_BYTES = 16 * 1024 * 1024 };
struct SnesReplayWriter { RecompReplayWriter *writer; const char *failure; };
static bool invalid(SnesReplayWriter *w) {
  if (w && !w->failure) w->failure = "invalid or oversized replay write";
  return false;
}
static bool bounded(SnesReplayWriter *w, const char *text, size_t capacity) {
  return text && strlen(text) < capacity ? true : invalid(w);
}
SnesReplayWriter *SnesReplayWriterCreate(void) {
  SnesReplayWriter *w = calloc(1, sizeof(*w));
  if (!w) return NULL;
  w->writer = recomp_replay_create_profile(RECOMP_REPLAY_SNES32, QUEUE_BYTES);
  if (!w->writer) { free(w); return NULL; }
  return w;
}
bool SnesReplayWriterBegin(SnesReplayWriter *w, const char *path, const char *header,
                          const char *config, const char *mods, const char *effective) {
  if (!w || w->failure) return false;
  if (!bounded(w, path, 1280) || !bounded(w, header, 512) ||
      !bounded(w, config, 1200) || !bounded(w, mods, 1200) ||
      !bounded(w, effective, 16384)) return false;
  return recomp_replay_begin_files(w->writer, path, header, config, mods, effective);
}
bool SnesReplayWriterSnapshot(SnesReplayWriter *w, const void *data, size_t size) {
  if (!w || w->failure) return false;
  return size >= QUEUE_BYTES ? invalid(w) : recomp_replay_snapshot(w->writer, data, size);
}
bool SnesReplayWriterLine(SnesReplayWriter *w, const char *line) {
  if (!w || w->failure || !line) return false;
  return strlen(line) >= QUEUE_BYTES ? invalid(w) : recomp_replay_line(w->writer, line);
}
bool SnesReplayWriterFrame(SnesReplayWriter *w, unsigned frame,
    const void *wram, size_t nw, const void *coop, size_t nc,
    const void *views, size_t nv, const void *vram, size_t np) {
  if (!w || w->failure) return false;
  if (nw >= QUEUE_BYTES || nc >= QUEUE_BYTES || nv >= QUEUE_BYTES || np >= QUEUE_BYTES)
    return invalid(w);
  const RecompReplayPart parts[4] = {{wram,nw,false},{coop,nc,false},
                                    {views,nv,false},{vram,np,false}};
  return recomp_replay_frame_parts(w->writer, frame, parts, 4);
}
bool SnesReplayWriterRemove(SnesReplayWriter *w, const char *path) {
  return w && !w->failure && bounded(w, path, 1400) && recomp_replay_remove(w->writer, path);
}
bool SnesReplayWriterEnd(SnesReplayWriter *w) {
  return w && !w->failure && recomp_replay_end(w->writer);
}
const char *SnesReplayWriterError(SnesReplayWriter *w) {
  return w ? (w->failure ? w->failure : recomp_replay_error(w->writer)) : NULL;
}
const char *SnesReplayWriterDestroy(SnesReplayWriter *w) {
  if (!w) return NULL;
  const char *error = recomp_replay_destroy(w->writer);
  if (w->failure) error = w->failure;
  free(w);
  return error;
}
