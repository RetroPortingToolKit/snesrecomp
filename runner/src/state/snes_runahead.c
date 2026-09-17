/* snes_runahead.c — see snes_runahead.h. */

#include "snes_runahead.h"

#include <stdio.h>
#include <stdlib.h>

#include "common_rtl.h"
#include "snes/snes.h"
#include "common_cpu_infra.h"   /* g_snes, g_rtl_game_info */
#include "netplay/snes_netplay.h"

static int      s_frames;
static uint8_t *s_blob;
static size_t   s_cap;
static int      s_warned;
static int      s_unrewindable;
static SnesRunaheadCapture s_capture;
static void    *s_capture_ctx;

/* Generous first-probe ceiling; the real snapshot size replaces it, exactly
 * as snes_rewind.c does. */
#define RA_PROBE_CAP (2u * 1024u * 1024u)

void snes_runahead_set_frames(int frames)
{
    if (frames < 0) frames = 0;
    if (frames > SNES_RUNAHEAD_MAX) frames = SNES_RUNAHEAD_MAX;
    if (frames == s_frames) return;
    s_frames = frames;
    fprintf(stderr, "[runahead] %d frame(s)%s\n", s_frames,
            s_frames ? "" : " (off)");
}

int snes_runahead_frames(void) { return s_frames; }

void snes_runahead_set_capture(SnesRunaheadCapture fn, void *context)
{
    s_capture = fn;
    s_capture_ctx = context;
}

void snes_runahead_configure(void)
{
    const char *env = getenv("SNESRECOMP_RUNAHEAD");
    if (env && *env)
        snes_runahead_set_frames(atoi(env));
}

int snes_runahead_active(void)
{
    if (s_frames <= 0) return 0;
    if (!g_snes) return 0;
    /* Offline only. See the header: two peers cannot each speculate about a
     * shared timeline, and netplay owns this same rollback machinery. */
#if defined(SNESRECOMP_NET)
    if (snes_netplay_active()) return 0;
#endif
    return 1;
}

void snes_runahead_shutdown(void)
{
    free(s_blob);
    s_blob = NULL;
    s_cap = 0;
}

/* Sized from the framework's bound, and RE-sized when it grows. A game that
 * puts its execution position in the snapshot -- a fiber's live stack -- has a
 * snapshot that grows and shrinks with the guest's call depth, so a buffer cut
 * to fit the first frame would start refusing later ones at exactly the
 * moments (scene changes, deep call chains) where it matters most. */
static int ensure_blob(void)
{
    size_t want = RtlRollbackSnapshotBound();
    uint8_t *grown;

    if (!want) return 0;
    if (s_blob && s_cap >= want) return 1;
    grown = (uint8_t *)realloc(s_blob, want);
    if (!grown) return 0;
    s_blob = grown;
    s_cap = want;
    fprintf(stderr, "[runahead] snapshot buffer is %zu bytes\n", s_cap);
    return 1;
}

int snes_runahead_run_frame(uint32_t inputs)
{
    size_t len;
    uint32_t audio_cursor;
    int saved_counter, i;
    bool saved_render;

    if (!snes_runahead_active()) return 0;

    /* A host that never handed over its capture step would show the rewound
     * frame, which is the feature doing nothing at a cost. Refuse instead. */
    if (!s_capture) {
        if (!s_warned) {
            fprintf(stderr, "[runahead] host provided no frame capture; "
                            "disabling run-ahead\n");
            s_warned = 1;
        }
        s_frames = 0;
        return 0;
    }

    /* Frame 0 is the real one: real input, real audio. From here on this call
     * owns the frame -- every path below returns 1, because the guest has
     * advanced and the caller must not advance it again. */
    RtlRunFrame(inputs);

    /*
     * Only now is the machine up far enough to ask. A game that keeps its
     * execution position somewhere a snapshot cannot reach (Win32 fibers are
     * opaque; an Android fiber is a real thread) bounds that state at zero,
     * and speculating anyway would advance the guest's position in its own
     * code while rewinding only the machine. Asked AFTER the first frame
     * because before it there is no fiber yet to bound, and a probe that runs
     * too early answers zero for every game.
     */
    if (g_rtl_game_info && g_rtl_game_info->exec_state_bound &&
        g_rtl_game_info->exec_state_bound() == 0) {
        /* Zero also means "not yet": a game whose position lives on a fiber
         * has no fiber to bound until it has booted one, which is several
         * frames after the host starts asking. Give it a second of frames to
         * appear before concluding that it never will. */
        if (++s_unrewindable < 120) {
            s_capture(s_capture_ctx, 1);
            return 1;
        }
        fprintf(stderr, "[runahead] this build cannot rewind the game's "
                        "execution position; disabling run-ahead\n");
        s_frames = 0;
        s_capture(s_capture_ctx, 1);
        return 1;
    }
    s_unrewindable = 0;

    if (!ensure_blob()) {
        if (!s_warned) {
            fprintf(stderr, "[runahead] no snapshot buffer; running normally\n");
            s_warned = 1;
        }
        s_capture(s_capture_ctx, 1);
        return 1;
    }

    /* The real frame's raster pass, for its side effects, BEFORE the snapshot
     * so they are inside it. Its picture is discarded: the speculated frame
     * below is the one the player sees. */
    s_capture(s_capture_ctx, 0);

    len = RtlRollbackSaveToMemory(s_blob, s_cap);
    if (!len) {
        /* Advanced correctly, just without speculation. Not an error worth
         * spamming: a snapshot can fail transiently while the machine settles.
         * The picture is still this frame's, so it still has to be captured. */
        s_capture(s_capture_ctx, 1);
        return 1;
    }

    /* Everything below must leave no trace except the picture. */
    saved_render  = g_snes->disableRender;
    saved_counter = snes_frame_counter;
    audio_cursor  = RtlAudioProducerCursor();

    RtlSetSpeculativeFrame(true);
    for (i = 1; i <= s_frames; i++) {
        /* Only the last speculative frame is rasterised -- it is the one the
         * player sees. The others exist solely to carry the guest forward. */
        g_snes->disableRender = (i < s_frames);
        RtlRunFrame(inputs);
    }
    /* HERE, before the rewind: the speculated frame is the picture. A host
     * that draws after the guest step would otherwise draw the rewound one.
     * Still inside the speculative window, so a per-frame mod hook does not
     * fire for it and the frame counter is restored below either way. */
    g_snes->disableRender = saved_render;
    s_capture(s_capture_ctx, 1);
    RtlSetSpeculativeFrame(false);

    /* Audio produced by speculation is thrown away; frame 0 already produced
     * this tick's real samples. The DSP output ring itself belongs to the live
     * audio thread and is deliberately NOT rewound by the rollback load. */
    RtlAudioRewindProducer(audio_cursor);

    if (!RtlRollbackLoadFromMemory(s_blob, len)) {
        /* The guest is now N frames ahead of where the host thinks it is, and
         * there is no way back. Loud, and run-ahead turns itself off rather
         * than corrupting every subsequent frame the same way. */
        fprintf(stderr, "[runahead] rollback load FAILED; disabling run-ahead "
                        "(the guest has advanced %d extra frame(s))\n", s_frames);
        s_frames = 0;
    }

    /* snes_frame_counter is host state and is not in the snapshot, so it does
     * not rewind on its own. Left alone it would advance N+1 per displayed
     * frame and every frame-numbered instrument -- the perf log, the OSD, the
     * frame-keyed traps -- would read N+1x fast. */
    snes_frame_counter = saved_counter;
    g_snes->disableRender = saved_render;
    return 1;
}
