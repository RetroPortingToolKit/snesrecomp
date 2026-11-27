/*
 * osd_test.c — snes_osd.c's composition and image path, with no window.
 *
 * The OSD is host chrome, so the thing worth regressing is not how it looks
 * but what it CLAIMS: that the status line says what fps and turbo are, that a
 * toast expires, and that the image is only produced when something is
 * actually visible. A port that draws an overlay every frame because
 * needs_present() never goes false is a real bug this catches.
 */

#include "snes_osd.h"

#include "desktop/sdl_compat.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* The clocks work without a video device on both SDL backends. */
    if (!snesrecomp_sdl_init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const uint32_t *px = NULL;
    int w = 0, h = 0;

    /* Nothing on screen: no image, and nothing to present. */
    check(!snes_osd_image(&px, &w, &h), "idle OSD produces no image");
    check(!snes_osd_needs_present(), "and asks for no present");
    check(!snes_osd_fps_visible(), "FPS readout starts hidden");

    /* FPS readout on. */
    snes_osd_toggle_fps();
    check(snes_osd_fps_visible(), "toggle shows the FPS readout");
    check(snes_osd_needs_present(), "which needs a present");
    check(snes_osd_image(&px, &w, &h) && px && w > 0 && h > 0,
          "and produces a non-empty image");

    /* The readout must be right EARLY, not after a second of climbing.
     *
     * The rolling mean divides by the full 64-slot window, so before the
     * window has filled the raw accumulator is diluted by the zeroed slots --
     * which is exactly why the overlay used to count up from 0 to 60 over its
     * first second. Ten frames at ~10ms is ~100fps; a diluted value would read
     * around 10/64 of that. */
    for (int i = 0; i < 10; i++) { snes_osd_note_frame(); SDL_Delay(10); }
    {
        const float early = snes_osd_fps();
        printf("  ..  after 10 frames: %.1f fps (raw window would show ~%.1f)\n",
               (double)early, (double)early * 10.0 / 64.0);
        check(early > 40.0f,
              "the average is window-corrected, so it does not ramp from zero");
    }

    /* Feed frames at a known cadence and confirm the average lands near it.
     * Deliberately loose: SDL_Delay is not precise, and asserting an exact
     * fps here would be a test of the host's scheduler, not of the OSD. */
    for (int i = 0; i < 70; i++) {
        snes_osd_note_frame();
        SDL_Delay(10);          /* ~100 fps */
    }
    const float fps = snes_osd_fps();
    printf("  ..  measured %.1f fps over 70 frames at ~10ms\n", (double)fps);
    check(fps > 40.0f && fps < 200.0f, "rolling average is in a sane band");

    char expected[64];
    snprintf(expected, sizeof(expected), "%d FPS", (int)(fps + 0.5f));
    check(!strcmp(snes_osd_status_text(), expected),
          "hosts without presentation samples retain the single-rate text");

    snes_osd_reset_fps();
    for (int i = 0; i < 90; ++i) {
        if (i % 3 == 0) snes_osd_note_frame();
        snes_osd_note_presentation();
        SDL_Delay(5);
    }
    const float sim = snes_osd_fps(), interp = snes_osd_interpolated_fps();
    check(sim > 0 && interp > sim * 2 && interp < sim * 4,
          "independent counters measure three presentations per simulation tick");
    snprintf(expected, sizeof(expected), "%d FPS (%d interp)",
             (int)(sim + 0.5f), (int)(interp + 0.5f));
    check(!strcmp(snes_osd_status_text(), expected),
          "dual-rate status uses the requested parenthesized format");
    check(snes_osd_image(&px, &w, &h) &&
          w == (int)(4 + strlen(expected) * 8) * 2,
          "the rendered image includes the complete interpolated rate");
    snes_osd_set_turbo(1);
    strncat(expected, "  TURBO", sizeof(expected) - strlen(expected) - 1);
    check(!strcmp(snes_osd_status_text(), expected), "dual rates retain the turbo indicator");
    snes_osd_set_turbo(0);
    snes_osd_reset_fps();
    check(snes_osd_fps() == 0 && snes_osd_interpolated_fps() == 0 &&
          !strcmp(snes_osd_status_text(), "0 FPS (0 interp)"),
          "pause/reset drops both stale measurement windows");

    snes_osd_toggle_fps();
    check(!snes_osd_fps_visible(), "toggle hides it again");
    snes_osd_toggle_fps();
    check(!strcmp(snes_osd_status_text(), "0 FPS (0 interp)"),
          "showing the readout starts both rates from fresh samples");
    snes_osd_toggle_fps();

    /* Turbo alone still shows a status line — a port with turbo but no FPS
     * readout must still get the indicator. */
    snes_osd_set_turbo(1);
    check(snes_osd_image(&px, &w, &h), "turbo alone produces a status line");
    snes_osd_set_turbo(0);

    /* A toast appears and then expires on its own. */
    snes_osd_push_slot_saved(3);
    check(snes_osd_image(&px, &w, &h), "a slot toast is visible immediately");
    SDL_Delay(60);
    check(snes_osd_image(&px, &w, &h), "and still visible well before expiry");

    snes_osd_push("brief", 30);
    SDL_Delay(120);
    check(!snes_osd_image(&px, &w, &h), "a short toast expires by itself");
    check(snes_osd_needs_present(),
          "and asks for one more present so the host can clear it");
    snes_osd_present_done();
    check(!snes_osd_needs_present(), "which present_done() then satisfies");

    printf(failures ? "FAILED\n" : "PASSED\n");
    return failures;
}
