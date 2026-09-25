#include "snes_netplay_rb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_netplay.h"
#include "snes_state_digest.h"
#include "common_rtl.h"
#include "common_cpu_infra.h"  /* snes_refresh_state_get */
#include "snes/snes.h"

extern Snes *g_snes;

/*
 * The SNES half of rollback: what a snapshot is, what a digest covers, how a
 * pad row is laid out, and how one tick runs and is kept off screen during a
 * replay. Everything that decides WHEN to rewind and talks to the peer about
 * it is recomp-net's episode driver (recomp_net/rb_driver.h), lifted out of
 * this file on 2026-09-24 so n64lle and psxrecomp run the same one. The long
 * comments recording why the driver does what it does moved with it.
 */

/* recomp-net carries 8 seats; SNES reaches that many with a Super Multitap in
 * each port (docs/MULTITAP.md). Seats past the second are published through
 * RtlSetPadState by the facade, so nothing in the resim path needs to know
 * how many there are. */
#define RB_MAX_SLOTS RNET_RB_MAX_SLOTS
/* SNES pads are 12 bits, active high. input_hist's default invent neutral is
 * 0xFFFF because PSX pads are active low — on SNES that would read as every
 * button held. The driver seeds our neutral (0) into the history, and
 * 12-bit rows can never legitimately be 0xFFFF, so the sentinel is still
 * mapped to neutral here should one arrive over the wire from an older peer. */
#define RB_PSX_NEUTRAL 0xFFFFu
#define RB_BUTTON_MASK 0x0FFFu

static struct {
    SnesNetplayRbBindings b;
    RNetRbDriver *drv;           /* created once; survives matches */
    RbeSnapRing  *snaps;
    uint32_t      snap_depth;
    /* Snapshot staging (one reusable buffer; the ring owns its own copies). */
    uint8_t      *snap_scratch;
    size_t        snap_scratch_cap;

    uint16_t staged;
    int      staged_valid;
    uint16_t resolved[RB_MAX_SLOTS];
    uint8_t  sync_bytes[2];
    int      sync_valid;

    /* Resim presentation suppression. */
    uint32_t resim_audio_cursor;
    int      saved_disable_render;
} g_rb;

/* ── env ─────────────────────────────────────────────────────────────── */

static int rb_env_int(const char *name, const char *generic, int def, int lo, int hi)
{
    const char *v = getenv(name);
    long n;
    char *end;
    if (!v && generic)
        v = getenv(generic);
    if (!v || !v[0])
        return def;
    n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < lo || n > hi)
        return def;
    return (int)n;
}

/* The settled session mode, handed in by snes_netplay_start() from the
 * config. ROLLBACK is the framework default (rolled out at scale
 * 2026-08-29): snes_netplay_config_defaults() seeds rollback=1, recomp-ui's
 * lobby settles the mode room-wide (its model also defaults on), and
 * SNES_NET_MODE is the operator override in BOTH directions — all of which
 * is folded into cfg->rollback before start() calls here. NETPLAY.md §4 is
 * satisfied by the settlement being room-wide, not per-process. */
static int s_rb_default;

void snes_netplay_rb_set_default(int on)
{
    s_rb_default = on ? 1 : 0;
}

int snes_netplay_rb_enabled(void)
{
    return s_rb_default;
}

static RNetRbDriver *rb_drv(void)
{
    if (!g_rb.drv)
        g_rb.drv = rnet_rb_driver_create();
    return g_rb.drv;
}

static int rb_slot_count(void)
{
    int n = g_rb.b.slot_count ? *g_rb.b.slot_count : 2;
    if (n < 1) n = 1;
    if (n > RB_MAX_SLOTS) n = RB_MAX_SLOTS;
    return n;
}

/* ── snapshots ───────────────────────────────────────────────────────── */

static int rb_snap_serialize(void *ctx, uint32_t tick, uint8_t **out_data,
                             size_t *out_len)
{
    size_t bound, n;
    uint8_t *copy;
    (void)ctx;
    (void)tick;

    bound = RtlRollbackSnapshotBound();
    if (g_rb.snap_scratch_cap < bound) {
        uint8_t *nb = (uint8_t *)realloc(g_rb.snap_scratch, bound);
        if (!nb)
            return 0;
        g_rb.snap_scratch = nb;
        g_rb.snap_scratch_cap = bound;
    }
    n = RtlRollbackSaveToMemory(g_rb.snap_scratch, g_rb.snap_scratch_cap);
    if (n == 0)
        return 0;
    copy = (uint8_t *)malloc(n);
    if (!copy)
        return 0;
    memcpy(copy, g_rb.snap_scratch, n);
    *out_data = copy;
    *out_len = n;
    return 1;
}

static int rb_snap_deserialize(void *ctx, uint32_t tick, const uint8_t *data,
                               size_t len)
{
    (void)ctx;
    (void)tick;
    return RtlRollbackLoadFromMemory(data, len) ? 1 : 0;
}

static const RbeSnapVTable g_snap_vt = {
    NULL, &rb_snap_serialize, &rb_snap_deserialize
};

static int rb_host_snap_save(void *ctx, uint32_t tick)
{
    (void)ctx;
    return g_rb.snaps ? rbe_snap_ring_save(g_rb.snaps, tick, &g_snap_vt) : 0;
}

static int rb_host_snap_load(void *ctx, uint32_t tick)
{
    (void)ctx;
    return g_rb.snaps ? rbe_snap_ring_load(g_rb.snaps, tick, &g_snap_vt) : 0;
}

static int rb_host_snap_has(void *ctx, uint32_t tick)
{
    (void)ctx;
    return g_rb.snaps ? rbe_snap_ring_has(g_rb.snaps, tick) : 0;
}

static int rb_host_snap_oldest(void *ctx, uint32_t *oldest)
{
    (void)ctx;
    if (!g_rb.snaps || rbe_snap_ring_count(g_rb.snaps) == 0)
        return 0;
    *oldest = rbe_snap_ring_oldest_tick(g_rb.snaps);
    return 1;
}

static void rb_host_snap_drop_after(void *ctx, uint32_t tick)
{
    (void)ctx;
    if (g_rb.snaps)
        (void)rbe_snap_ring_drop_after(g_rb.snaps, tick);
}

/* ── one tick ────────────────────────────────────────────────────────── */

static void rb_host_publish(void *ctx, uint32_t tick, const RNetRbFrame *rows,
                            int slots, int replay)
{
    int i;
    (void)ctx;
    (void)replay;
    for (i = 0; i < slots && i < RB_MAX_SLOTS; ++i)
        g_rb.resolved[i] = rows[i].buttons & RB_BUTTON_MASK;
    if (g_rb.b.publish)
        g_rb.b.publish(tick, g_rb.resolved, slots);
    if (g_rb.sync_valid && g_rb.b.apply_sync_bytes)
        g_rb.b.apply_sync_bytes(g_rb.sync_bytes);
}

static uint32_t rb_run_frame_inputs(void)
{
    uint32_t p1 = g_rb.resolved[0] & RB_BUTTON_MASK;
    uint32_t p2 = (rb_slot_count() > 1 ? g_rb.resolved[1] : 0u) & RB_BUTTON_MASK;
    /* Seats 0 and 1 ride the packed word; 2..7 were already applied by
     * publish, which routes them through RtlSetPadState. Both seats plugged,
     * matching snes_netplay_active_mask(). */
    return p1 | (p2 << 12) | (3u << 30);
}

/* A replayed tick. Inline: an SNES tick is a plain RtlRunFrame that returns,
 * so the whole replay is tens of frames with no host stack to unwind. */
static int rb_host_run_tick(void *ctx, uint32_t tick)
{
    (void)ctx;
    (void)tick;
    RtlRunFrame(rb_run_frame_inputs());
    return 1;
}

/*
 * Resim replays ticks the player has already seen and heard. Presentation
 * must not repeat with it (recomp-ai-rules/NETPLAY.md §1: the presented image
 * is never simulation), so the renderer is off for the span and the audio
 * the resim re-produces is dropped by rewinding the DSP producer cursor back
 * to where it stood before the rewind. The consumer cursor is never touched —
 * it belongs to the audio thread.
 */
static void rb_host_resim_begin(void *ctx)
{
    (void)ctx;
    g_rb.resim_audio_cursor = RtlAudioProducerCursor();
    if (g_snes) {
        g_rb.saved_disable_render = g_snes->disableRender ? 1 : 0;
        g_snes->disableRender = true;
    }
}

static void rb_host_resim_end(void *ctx)
{
    (void)ctx;
    RtlAudioRewindProducer(g_rb.resim_audio_cursor);
    if (g_snes)
        g_snes->disableRender = g_rb.saved_disable_render ? true : false;
}

/* ── digests ─────────────────────────────────────────────────────────── */

static uint32_t rb_host_digest_master(void *ctx)
{
    (void)ctx;
    return snes_state_digest(SNES_DIGEST_PART_MASTER);
}

static void rb_host_digest_parts(void *ctx, RNetRbDigestParts *out)
{
    SnesStateDigestParts p;
    (void)ctx;
    snes_state_digest_parts(&p);
    out->master = p.master;
    out->part[0] = p.wram;
    out->part[1] = p.apu;
    out->part[2] = p.ppu;
}

/* ── pads ────────────────────────────────────────────────────────────── */

static void rb_host_decode(void *ctx, int slot, const RNetInputSample *in,
                           RNetRbFrame *out)
{
    (void)ctx;
    (void)slot;
    out->buttons = (uint16_t)((in->bytes[0] | ((uint16_t)in->bytes[1] << 8)) &
                              RB_BUTTON_MASK);
    out->stick_x = 0;
    out->stick_y = 0;
    out->analog = 0;
}

static void rb_host_sanitize(void *ctx, int slot, RNetRbFrame *f)
{
    (void)ctx;
    (void)slot;
    if (f->buttons == RB_PSX_NEUTRAL)
        f->buttons = 0u; /* PSX-shaped neutral → SNES neutral */
    f->buttons &= RB_BUTTON_MASK;
    f->stick_x = 0;
    f->stick_y = 0;
    f->analog = 0;
}

static void rb_host_neutral(void *ctx, int slot, RNetRbFrame *out)
{
    (void)ctx;
    (void)slot;
    out->buttons = 0u;   /* active high: nothing held */
}

/* Slot 0 carries the authoritative game sync bytes in pad bytes 2..3. Taken
 * from the sample a live admit used, local or wire, so both peers apply the
 * same ones. */
static void rb_host_admit_sample(void *ctx, int slot, uint32_t tick,
                                 const RNetInputSample *in)
{
    (void)ctx;
    (void)tick;
    if (slot == 0 && in->size >= 4) {
        g_rb.sync_bytes[0] = in->bytes[2];
        g_rb.sync_bytes[1] = in->bytes[3];
        g_rb.sync_valid = 1;
    }
}

/* ── session control ─────────────────────────────────────────────────── */

/* The refresh tax's sub-scanline carry at capture. A cold boot starts at 0;
 * anything else means the machine inherited a phase from a previous session,
 * which changes how the first block is charged. */
static uint64_t rb_refresh_phase_now(void)
{
    uint64_t phase = 0;
    uint64_t upto = 0;
    snes_refresh_state_get(&phase, &upto);
    return phase;
}

/* Tick 0's digest was latched. The chain carries only the master digest, so a
 * mismatch says the two sides differ without saying WHERE; establishing that
 * took a long forensic pass over two logs and three wrong guesses -- save
 * root, localization, a beam anchor that turned out not to be digested at
 * all. Every one would have been a glance with the partitions in the log.
 * Printed on agreement too: knowing which partitions are identical is how the
 * next reader rules things out, and it costs one line per session. */
static void rb_host_boot_digest_noted(void *ctx)
{
    SnesStateDigestParts p;
    (void)ctx;
    snes_state_digest_parts(&p);
    fprintf(stderr,
            "snes_netplay: RB boot parts master=%08x cpu=%08x wram=%08x "
            "apu=%08x ppu=%08x dma=%08x cart=%08x\n",
            (unsigned)p.master, (unsigned)p.cpu, (unsigned)p.wram,
            (unsigned)p.apu, (unsigned)p.ppu, (unsigned)p.dma,
            (unsigned)p.cart);
    /* How far the machine had already run when tick 0 was captured.
     *
     * WRAM is zeroed by snes_reset(hard) at SnesInit, so a tick-0 WRAM digest
     * that differs between peers means one of them executed guest code before
     * the snapshot -- the partitions say WHICH state differs and never how it
     * got that way. On a rematch these should read identically on both peers;
     * if they do not, the boot is not the cold boot it claims to be, and that
     * is upstream of anything rollback can reconcile. */
    fprintf(stderr,
            "snes_netplay: RB boot taken at frame=%llu "
            "apu_catchup=%.3f vpos=%u hpos=%u refresh_phase=%llu\n",
            (unsigned long long)snes_frame_counter,
            g_snes ? g_snes->apuCatchupCycles : 0.0,
            (unsigned)(g_snes ? g_snes->vPos : 0),
            (unsigned)(g_snes ? g_snes->hPos : 0),
            (unsigned long long)rb_refresh_phase_now());
}

static void rb_host_return_to_lobby(void *ctx)
{
    (void)ctx;
    snes_netplay_request_return_to_lobby();
}

static uint32_t rb_host_now_ms(void *ctx)
{
    (void)ctx;
    return rbe_mono_ms();
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

void snes_netplay_rb_bind(const SnesNetplayRbBindings *b)
{
    if (!b) {
        memset(&g_rb.b, 0, sizeof(g_rb.b));
        return;
    }
    g_rb.b = *b;
}

int snes_netplay_rb_start(void)
{
    RNetRbDriverConfig cfg;
    RNetRbHost host;
    RNetRbDriver *drv = rb_drv();

    snes_netplay_rb_shutdown();

    /* Cold reset of the SNES half: everything but the bindings and the driver
     * object (which owns identity and mod set, and cold-resets itself). */
    {
        SnesNetplayRbBindings keep_bindings = g_rb.b;
        memset(&g_rb, 0, sizeof(g_rb));
        g_rb.b = keep_bindings;
        g_rb.drv = drv;
    }
    if (!drv) {
        fprintf(stderr, "snes_netplay: RB start refused — could not allocate "
                        "the rollback driver\n");
        return 0;
    }
    /* Bindings are set immediately before this call today; if that order is
     * ever changed the host would come up bound to nothing -- a dead rollback
     * path with no error, the failure this file has spent the most time on. */
    if (!g_rb.b.session) {
        fprintf(stderr,
                "snes_netplay: RB start with no session binding — "
                "call snes_netplay_rb_bind() before snes_netplay_rb_start().\n");
        return 0;
    }

    g_rb.snaps = rbe_snap_ring_create(
        /* Floor is 16, not 8. Measured at 200 ms RTT: depth 8 loses 15
         * episodes a minute to peer NACKs because the follower cannot reach
         * back to the load tick, while 16 and every depth above it lose none.
         * At 300 ms the depth-attributable NACKs saturate the same way -- 17
         * at depth 8, a flat 8 at 16, 24 and 40 alike, the remainder being
         * refusals that have nothing to do with the ring. Eight was a settable
         * value that silently disabled a share of rollback, so it is no longer
         * accepted -- an out-of-range value falls back to the DEFAULT rather
         * than clamping to the bound, so a request for 8 now yields 40, which
         * the startup banner states outright. */
        (g_rb.snap_depth = (uint32_t)rb_env_int(
             "SNES_RB_SNAP_DEPTH", "RNET_RB_SNAP_DEPTH",
             (int)RBE_SNAP_RING_DEFAULT_DEPTH, 16, 240)));
    if (!g_rb.snaps)
        return 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = g_rb.b.session;
    cfg.local_slot = g_rb.b.local_slot;
    cfg.slot_count = g_rb.b.slot_count;
    cfg.input_delay = g_rb.b.input_delay;
    cfg.input_prediction = g_rb.b.input_prediction;
    cfg.force_turn = g_rb.b.force_turn;
    cfg.replay_mode = RNET_RB_REPLAY_INLINE;
    cfg.part_names[0] = snes_state_digest_part_name(SNES_DIGEST_PART_WRAM);
    cfg.part_names[1] = snes_state_digest_part_name(SNES_DIGEST_PART_APU);
    cfg.part_names[2] = snes_state_digest_part_name(SNES_DIGEST_PART_PPU);
    cfg.snap_depth = g_rb.snap_depth;
    cfg.log_prefix = "snes_netplay";
    cfg.env_alias = "SNES_RB";

    memset(&host, 0, sizeof(host));
    host.snap_save = &rb_host_snap_save;
    host.snap_load = &rb_host_snap_load;
    host.snap_has = &rb_host_snap_has;
    host.snap_oldest = &rb_host_snap_oldest;
    host.snap_drop_after = &rb_host_snap_drop_after;
    host.publish = &rb_host_publish;
    host.run_tick = &rb_host_run_tick;
    host.resim_begin = &rb_host_resim_begin;
    host.resim_end = &rb_host_resim_end;
    host.digest_master = &rb_host_digest_master;
    host.digest_parts = &rb_host_digest_parts;
    host.decode_sample = &rb_host_decode;
    host.sanitize_row = &rb_host_sanitize;
    host.neutral_row = &rb_host_neutral;
    host.admit_sample = &rb_host_admit_sample;
    host.boot_digest_noted = &rb_host_boot_digest_noted;
    host.request_return_to_lobby = &rb_host_return_to_lobby;
    host.now_ms = &rb_host_now_ms;

    if (!rnet_rb_driver_start(drv, &cfg, &host)) {
        rbe_snap_ring_destroy(g_rb.snaps);
        g_rb.snaps = NULL;
        return 0;
    }
    return 1;
}

void snes_netplay_rb_shutdown(void)
{
    if (g_rb.drv)
        rnet_rb_driver_shutdown(g_rb.drv);
    if (g_rb.snaps) {
        rbe_snap_ring_destroy(g_rb.snaps);
        g_rb.snaps = NULL;
    }
    free(g_rb.snap_scratch);
    g_rb.snap_scratch = NULL;
    g_rb.snap_scratch_cap = 0;
    g_rb.staged_valid = 0;
    g_rb.sync_valid = 0;
}

void snes_netplay_rb_stage_local(uint16_t buttons)
{
    g_rb.staged = buttons & RB_BUTTON_MASK;
    g_rb.staged_valid = 1;
}

int snes_netplay_rb_poll_admit(void)
{
    /* INLINE replay: the driver never hands back a replay tick, so the only
     * admit this loop runs is a live one. */
    return rnet_rb_driver_poll_admit(g_rb.drv) == RNET_RB_ADMIT_LIVE;
}

void snes_netplay_rb_finish_frame(void)
{
    rnet_rb_driver_finish_frame(g_rb.drv);
}

/* ── diagnostics ─────────────────────────────────────────────────────── */

uint32_t snes_netplay_rb_sim_tick(void) { return rnet_rb_driver_sim_tick(g_rb.drv); }
uint32_t snes_netplay_rb_episode_count(void) { return rnet_rb_driver_episode_count(g_rb.drv); }
uint32_t snes_netplay_rb_invent_count(void) { return rnet_rb_driver_invent_count(g_rb.drv); }
uint32_t snes_netplay_rb_promote_count(void) { return rnet_rb_driver_promote_count(g_rb.drv); }
uint64_t snes_netplay_rb_resim_ticks(void) { return rnet_rb_driver_resim_ticks(g_rb.drv); }
uint32_t snes_netplay_rb_desync_count(void) { return rnet_rb_driver_desync_count(g_rb.drv); }

void snes_netplay_rb_set_modset(const char *text,
                                SnesNetplayModSetCheckFn check,
                                SnesNetplayModSetAdoptFn adopt)
{
    rnet_rb_driver_set_modset(rb_drv(), text, check, adopt);
}

void snes_netplay_rb_set_identity(uint32_t build_fp, uint32_t content_fp)
{
    rnet_rb_driver_set_identity(rb_drv(), build_fp, content_fp);
}

uint32_t snes_netplay_rb_rtt_estimate_ms(void)
{
    return rnet_rb_driver_rtt_estimate_ms(g_rb.drv);
}

uint32_t snes_netplay_rb_confirmed_through(void)
{
    return rnet_rb_driver_confirmed_through(g_rb.drv);
}

uint32_t snes_netplay_rb_confirmed_remaining(void)
{
    return rnet_rb_driver_confirmed_remaining(g_rb.drv);
}

int snes_netplay_rb_episode_active(void)
{
    return rnet_rb_driver_episode_active(g_rb.drv);
}

const char *snes_netplay_rb_stall_tag(void)
{
    return rnet_rb_driver_stall_tag(g_rb.drv);
}

int snes_netplay_rb_last_fork(uint32_t *tick, const char **partition)
{
    return rnet_rb_driver_last_fork(g_rb.drv, tick, partition);
}

int snes_netplay_rb_fork_digests(uint32_t *mine, uint32_t *theirs)
{
    return rnet_rb_driver_fork_digests(g_rb.drv, mine, theirs);
}
