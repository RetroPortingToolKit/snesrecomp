#include "diagnostics/trace_writer.h"
#include "desktop/sdl_compat.h"
#include <recomp_diagnostics/trace_writer.h>

/* Row formatting and simulation-thread observations stay in the engine. */
static uint32_t trace_ticks(void *unused) {
  (void)unused;
  return (uint32_t)SDL_GetTicks();
}
static void trace_sleep(void *unused, unsigned milliseconds) {
  (void)unused;
  SDL_Delay(milliseconds);
}
#ifdef SNES_TRACE_WRITER_TEST
static void trace_ready(void *unused) {
  extern void SnesTraceWriterTestGate(void);
  (void)unused;
  SnesTraceWriterTestGate();
}
#endif
SnesTraceWriter *SnesTraceWriterOpen(const char *stem, const char *header,
                                    size_t segment_bytes, unsigned keep) {
  RecompTraceOptions options = recomp_trace_options(segment_bytes, keep);
  options.host.ticks_ms = trace_ticks;
  options.host.sleep_ms = trace_sleep;
#ifdef SNES_TRACE_WRITER_TEST
  options.host.worker_ready = trace_ready;
#endif
  return (SnesTraceWriter *)recomp_trace_open(stem, header, &options);
}
bool SnesTraceWriterWrite(SnesTraceWriter *writer, const char *row, size_t size) {
  return recomp_trace_write((RecompTraceWriter *)writer, row, size) != 0;
}
const char *SnesTraceWriterError(SnesTraceWriter *writer) {
  return recomp_trace_error((RecompTraceWriter *)writer);
}
const char *SnesTraceWriterClose(SnesTraceWriter *writer) {
  return recomp_trace_close((RecompTraceWriter *)writer);
}
