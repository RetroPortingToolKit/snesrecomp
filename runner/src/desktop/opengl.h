#pragma once
#include <stdint.h>
#include "display_aspect.h"

/* The shared desktop OpenGL presenter (desktop/opengl.c). util.h declares
 * OpenGLRenderer_Create itself; this header carries only the presentation
 * policy a host may need to override. */

#ifdef __cplusplus
extern "C" {
#endif

/* Force the swap interval instead of deriving it from config.
 *
 * Call BEFORE the renderer is created. A host that paces presentation on its
 * own clock (an FPS cap, or a split simulation/presentation rate) passes 0,
 * so SDL_GL_SwapWindow does not block on the display as well and fight that
 * pacing. Left unset, the presenter vsyncs unless config's DisableFrameDelay
 * is on. */
/* Swap interval for the GL presenter: 0 immediate, 1 wait for the panel,
 * -1 late-swap-tearing ("Adaptive"). Call before OpenGLRenderer_Create. */
void snesrecomp_opengl_set_vsync(int interval);
void snesrecomp_opengl_capture_frame(uint32_t frame);
void snesrecomp_opengl_set_viewport(void (*compute)(int, int, int, int,
                                                   SnesDisplayViewport *));
/* Physical hi-res dots are half a logical column wide. This affects display
 * geometry only, including custom viewport callbacks, never texture data. */
void snesrecomp_opengl_set_horizontal_scale(int scale);

/* Capture the next fully composited OpenGL frame to screenshots/*.bmp. */
void OpenGLRenderer_RequestScreenshot(void);

#ifdef __cplusplus
}
#endif
