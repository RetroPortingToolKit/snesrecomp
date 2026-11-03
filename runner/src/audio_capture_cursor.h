#ifndef AUDIO_CAPTURE_CURSOR_H
#define AUDIO_CAPTURE_CURSOR_H
#include <stdbool.h>
#include <stdint.h>

/* Diagnostic sample counters wrap normally, but reset/load starts a new ring.
 * Establish that epoch before running the frame, never by interpreting a
 * backwards counter as billions of new samples. */
typedef struct AudioCaptureCursor {
  uint32_t write;
  uint64_t generation;
  bool primed;
} AudioCaptureCursor;
static inline void audio_capture_begin(AudioCaptureCursor *c,
                                        uint64_t generation, uint32_t write) {
  if (!c->primed || c->generation != generation) {
    c->write = write;
    c->generation = generation;
    c->primed = true;
  }
}
static inline bool audio_capture_span(AudioCaptureCursor *c,
    uint64_t generation, uint32_t write, uint32_t capacity, uint32_t *first,
    uint32_t *count) {
  *first = c->write;
  *count = write - c->write;
  bool ok = c->primed && c->generation == generation && *count <= capacity;
  c->write = write;
  return ok;
}
#endif
