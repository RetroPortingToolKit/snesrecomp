/* Headless capture: record exactly what a window would show, without one.
 *
 * Contract (shared with the sibling engines):
 *   --capture-out DIR        frame_NNNNNN.png of the presented picture
 *   --capture-size WxH       the drawable size a window of that size would have
 *                            (default 1920x1080; 2560x1080 for 21:9)
 *   --capture-from/-to N     simulated-frame range (default: all)
 *   --capture-every K        keep every Kth picture of the range
 *   --capture-wav FILE       the mixed output (post resampler), 48 kHz stereo,
 *                            starting at --capture-from so it lines up with the
 *                            first frame written
 *   --input-script FILE      `<frame> [P:]BUTTONS`; BUTTONS joined by `+`
 *                            (B Y SELECT START UP DOWN LEFT RIGHT A X L R);
 *                            `-` or nothing releases; `2:` selects pad 2.
 *                            Each line holds until that pad's next line.
 *   --exit-at-frame N        stop after N simulated frames
 *
 * Capture mode reads the completed SDL or OpenGL back buffer, including
 * presentation filters. It has no visible window or playback device and
 * mixes audio per simulated frame.
 */
#ifndef SNESRECOMP_HEADLESS_CAPTURE_H
#define SNESRECOMP_HEADLESS_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SnesrecompHostArgs;

/* Reads the capture/input flags (and SNESRECOMP_CAPTURE_* environment
 * equivalents). Returns true when capture mode is on. */
bool HeadlessCapture_Configure(const struct SnesrecompHostArgs *args);
bool HeadlessCapture_Enabled(void);
bool HeadlessCapture_Failed(void);
/* Select the capture video driver before SDL video is initialised. */
void HeadlessCapture_SelectVideoDriver(bool opengl);

/* The drawable size the presenter reports. */
void HeadlessCapture_OutputSize(int *width, int *height);

/* Audio rate captured at, and whether this simulated frame's audio is wanted. */
int HeadlessCapture_AudioRate(void);
bool HeadlessCapture_AudioEnabled(void);

/* True when a PNG was requested for this 1-based simulated frame. */
bool HeadlessCapture_FrameWanted(uint32_t frame);

/* Present one frame: BGRA source `w`x`h` placed in the viewport (vx,vy,vw,vh)
 * of the output canvas (nearest sampling, black elsewhere), written as PNG. */
void HeadlessCapture_Present(const uint8_t *bgra, int pitch, int w, int h,
                             int vx, int vy, int vw, int vh, uint32_t frame);

/* Interleaved stereo s16 at HeadlessCapture_AudioRate(), one block per
 * simulated frame. */
void HeadlessCapture_Audio(const int16_t *samples, int frames, uint32_t frame);
/* How many output samples the given simulated frame should render. */
int HeadlessCapture_AudioFramesFor(uint32_t frame);

/* Held-button word for the frame: pad 1 in bits 0..11, pad 2 in bits 12..23
 * (the runner's input word). */
uint32_t HeadlessCapture_InputWord(uint32_t frame);

/* One seat's buttons (runner bit layout), seats 0..3: `<frame> [1..4:]BUTTONS`.
 * Frames must be requested in non-decreasing order. */
uint16_t HeadlessCapture_Pad(unsigned seat, uint32_t frame);
bool HeadlessCapture_HasInput(void);

/* Exit frame (0 = unset). */
uint32_t HeadlessCapture_ExitFrame(void);

/* Finalise the WAV header and print the one-line summary. */
void HeadlessCapture_Finish(void);

#ifdef __cplusplus
}
#endif
#endif
