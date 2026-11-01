#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include "audio_capture_cursor.h"
int main(void) {
  AudioCaptureCursor c = {0}; uint32_t first, count;
  assert(!audio_capture_span(&c, 0, 0, 8192, &first, &count));
  audio_capture_begin(&c, 1, 100);
  assert(audio_capture_span(&c, 1, 634, 8192, &first, &count));
  assert(first == 100 && count == 534);
  /* A restored state has a lower counter; only newly generated audio counts. */
  audio_capture_begin(&c, 2, 30);
  assert(audio_capture_span(&c, 2, 564, 8192, &first, &count));
  assert(first == 30 && count == 534);
  /* Ordinary uint32 wrap is valid within the same generation. */
  audio_capture_begin(&c, 3, UINT32_MAX - 2);
  assert(audio_capture_span(&c, 3, 2, 8192, &first, &count));
  assert(count == 5);
  assert(audio_capture_span(&c, 3, 8194, 8192, &first, &count));
  assert(count == 8192);
  assert(!audio_capture_span(&c, 3, 16387, 8192, &first, &count));
  /* An unannounced mid-frame load is invalid, never a giant output. */
  assert(!audio_capture_span(&c, 4, 1, 8192, &first, &count));
  return 0;
}
