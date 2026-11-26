#ifndef SNES_AUDIO_REPORT_H
#define SNES_AUDIO_REPORT_H
#include "audio_trace.h"
#include "desktop/host_main.h"

/* Low-rate observations for the existing bounded host-report breadcrumbs.
 * The desktop wrapper supplies a locked counter copy only when requested.
 * No state here is guest/save-state state. */
enum { SNES_AUDIO_REPORT_SAMPLE = 1, SNES_AUDIO_REPORT_CONTEXT = 2 };
typedef struct SnesAudioReportContext {
  const char *video_driver, *audio_driver, *requested_renderer;
  const char *renderer, *device;
} SnesAudioReportContext;
unsigned snes_audio_report_note(int enabled, const SnesDesktopHostFrameStats *frame,
                                uint64_t generation, int final_snapshot);
void snes_audio_report_emit(const AudioTraceStats *audio,
                           const SnesAudioReportContext *context);
#endif
