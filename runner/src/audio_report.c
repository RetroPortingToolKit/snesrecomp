#include "audio_report.h"
#include "host_report.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

static struct {
  bool active, final, have_counters;
  unsigned epoch, frames;
  uint64_t generation;
  double start, max_guest_ms, max_gap_ms;
  const char *reason;
  SnesDesktopHostFrameStats last;
  AudioTraceStats counters;
} report;

static double guest_ms(const SnesDesktopHostFrameStats *frame) {
  return isfinite(frame->guest_seconds) && frame->guest_seconds > 0
      ? frame->guest_seconds * 1000 : 0;
}
static void begin_epoch(const SnesDesktopHostFrameStats *frame,
                        uint64_t generation, const char *reason) {
  report.active = true; report.final = false; report.have_counters = false;
  ++report.epoch; report.generation = generation;
  report.last = *frame; report.start = frame->run_seconds;
  report.frames = 0; report.max_gap_ms = 0;
  report.max_guest_ms = guest_ms(frame); report.reason = reason;
}
unsigned snes_audio_report_note(int enabled, const SnesDesktopHostFrameStats *frame,
                                uint64_t generation, int final_snapshot) {
  if (!enabled) {
    report.active = report.final = report.have_counters = false;
    return 0;
  }
  if (report.final) return 0;
  if (frame && (!isfinite(frame->run_seconds) || frame->run_seconds < 0)) return 0;
  unsigned flags = 0;
  if (!report.active) {
    const SnesDesktopHostFrameStats empty = {0};
    begin_epoch(frame ? frame : &empty, generation, "start");
    flags = SNES_AUDIO_REPORT_SAMPLE | SNES_AUDIO_REPORT_CONTEXT;
  } else if (generation != report.generation ||
             (frame && (frame->frame < report.last.frame ||
                        frame->run_seconds < report.last.run_seconds))) {
    begin_epoch(frame ? frame : &report.last, generation, "state-reset");
    flags = SNES_AUDIO_REPORT_SAMPLE | SNES_AUDIO_REPORT_CONTEXT;
  } else if (frame) {
    double gap = (frame->run_seconds - report.last.run_seconds) * 1000;
    if (gap > report.max_gap_ms) report.max_gap_ms = gap;
    double guest = guest_ms(frame);
    if (guest > report.max_guest_ms) report.max_guest_ms = guest;
    report.frames += frame->frame - report.last.frame;
    report.last = *frame;
  }
  if (final_snapshot) {
    report.final = true; report.reason = "final";
    return SNES_AUDIO_REPORT_SAMPLE | SNES_AUDIO_REPORT_CONTEXT;
  }
  if (report.last.run_seconds - report.start >= 2.0) {
    report.reason = "window";
    flags |= SNES_AUDIO_REPORT_SAMPLE;
  }
  return flags;
}

static bool counters_regressed(const AudioTraceStats *a) {
  const AudioTraceStats *b = &report.counters;
  return a->produced < b->produced || a->consumed < b->consumed ||
      a->output_underflows < b->output_underflows ||
      a->output_missing_frames < b->output_missing_frames || a->dropped < b->dropped;
}
static void context_text(char *out, size_t capacity, const char *text) {
  if (!text) text = "unavailable";
  size_t bytes = strlen(text);
  if (bytes >= capacity) {
    bytes = capacity - 1;
    while (bytes && ((unsigned char)text[bytes] & 0xc0) == 0x80) --bytes;
  }
  memcpy(out, text, bytes); out[bytes] = 0;
}
void snes_audio_report_emit(const AudioTraceStats *audio,
                           const SnesAudioReportContext *context) {
  if (!report.active || !audio) return;
  if (report.have_counters && counters_regressed(audio)) {
    bool final = report.final;
    begin_epoch(&report.last, report.generation, "counter-reset");
    report.final = final;
  }
  double seconds = report.last.run_seconds - report.start;
  host_report_breadcrumb("audio totals e=%u f=%u p=%llu c=%llu fill=%u hz=%d",
      report.epoch, report.last.frame, (unsigned long long)audio->produced,
      (unsigned long long)audio->consumed, audio->occupancy_current, report.last.audio_output_rate);
  host_report_breadcrumb("audio losses e=%u uf=%llu miss=%llu drop=%llu",
      report.epoch, (unsigned long long)audio->output_underflows,
      (unsigned long long)audio->output_missing_frames, (unsigned long long)audio->dropped);
  host_report_breadcrumb("audio window e=%u reason=%s seconds=%.6g frames=%u fps=%.6g guest_ms=%.6g gap_ms=%.6g (includes pauses)",
      report.epoch, report.reason, seconds, report.frames,
      seconds > 0 ? report.frames / seconds : 0, report.max_guest_ms, report.max_gap_ms);
  host_report_breadcrumb("audio delta e=%u valid=%u uf=%llu miss=%llu drop=%llu",
      report.epoch, (unsigned)report.have_counters,
      (unsigned long long)(report.have_counters ? audio->output_underflows-report.counters.output_underflows : 0),
      (unsigned long long)(report.have_counters ? audio->output_missing_frames-report.counters.output_missing_frames : 0),
      (unsigned long long)(report.have_counters ? audio->dropped-report.counters.dropped : 0));
  if (context) {
    char requested[25], renderer[25], video[17], driver[17], device[97];
    context_text(requested, sizeof requested, context->requested_renderer);
    context_text(renderer, sizeof renderer, context->renderer);
    context_text(video, sizeof video, context->video_driver);
    context_text(driver, sizeof driver, context->audio_driver);
    context_text(device, sizeof device, context->device);
    host_report_breadcrumb("audio backend requested=%s actual=%s video=%s audio=%s",
        requested, renderer, video, driver);
    host_report_breadcrumb("audio output device=%s rate=%d", device, report.last.audio_output_rate);
  }
  report.counters = *audio; report.have_counters = true;
  report.start = report.last.run_seconds; report.frames = 0;
  report.max_guest_ms = report.max_gap_ms = 0;
}
