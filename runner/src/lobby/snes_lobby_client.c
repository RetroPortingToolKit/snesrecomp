/*
 * snes_lobby_client.c -- the SNES adapter over recomp-net's lobby client.
 *
 * See snes_lobby_client.h. The protocol client itself is recomp-net's
 * (src/lobby/rnet_lobby_client.c); this file holds only what is SNES:
 * the widescreen keys of match_caps, the conversion between the historic
 * SnesLobbyMatchCaps and RNetLobbyMatchCaps, and the build's identity.
 */
#include "snes_lobby_client.h"

#include <stdio.h>
#include <string.h>

/* The SNES keys ride in RNetLobbyMatchCaps.ext, which is opaque to recomp-net
 * and sized for exactly this kind of overlay. */
typedef char snes_caps_ext_fits[
    sizeof(SnesLobbyCapsExt) <= RNET_LOBBY_CAPS_EXT_BYTES ? 1 : -1];

static SnesLobbyCapsExt *caps_ext(RNetLobbyMatchCaps *c)
{
    return (SnesLobbyCapsExt *)(void *)c->ext.bytes;
}

static const SnesLobbyCapsExt *caps_ext_c(const RNetLobbyMatchCaps *c)
{
    return (const SnesLobbyCapsExt *)(const void *)c->ext.bytes;
}

/* ── the SNES match_caps keys ────────────────────────────────────────────
 *
 * Same keys, same defaults as the client always used: widescreen /
 * widescreen_hud / ignore_aspect booleans, ws_extra an int >= 0, and
 * widescreen_hud reading 1 when absent. */
static int snes_caps_write(const RNetLobbyMatchCaps *caps, char *out,
                           size_t cap, void *ctx)
{
    const SnesLobbyCapsExt *e = caps_ext_c(caps);
    int n;
    (void)ctx;
    n = snprintf(out, cap,
                 ",\"widescreen\":%s,\"widescreen_hud\":%s,"
                 "\"ignore_aspect\":%s,\"ws_extra\":%d",
                 e->widescreen ? "true" : "false",
                 e->widescreen_hud ? "true" : "false",
                 e->ignore_aspect ? "true" : "false",
                 e->ws_extra);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

static void snes_caps_parse(const char *obj, RNetLobbyMatchCaps *caps,
                            void *ctx)
{
    SnesLobbyCapsExt *e = caps_ext(caps);
    (void)ctx;
    e->widescreen = rnet_lobby_json_get_bool(obj, "widescreen", 0);
    e->widescreen_hud = rnet_lobby_json_get_bool(obj, "widescreen_hud", 1);
    e->ignore_aspect = rnet_lobby_json_get_bool(obj, "ignore_aspect", 0);
    e->ws_extra = rnet_lobby_json_get_int(obj, "ws_extra", 0);
    if (e->ws_extra < 0) e->ws_extra = 0;
}

static const RNetLobbyCapsCodec g_snes_codec = {
    snes_caps_write, snes_caps_parse, NULL
};

const RNetLobbyCapsCodec *snes_lobby_caps_codec(void)
{
    return &g_snes_codec;
}

void snes_lobby_caps_to_rnet(const SnesLobbyMatchCaps *in,
                             RNetLobbyMatchCaps *out)
{
    SnesLobbyCapsExt *e;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!in) return;
    out->valid = in->valid;
    out->input_delay = in->input_delay;
    out->input_prediction = in->input_prediction;
    out->force_turn = in->force_turn;
    out->force_input_relay = in->force_input_relay;
    out->rollback = in->rollback;
    out->mod_count = in->mod_count;
    memcpy(out->mods, in->mods, sizeof(out->mods));
    memcpy(out->mod_set, in->mod_set, sizeof(out->mod_set));
    memcpy(out->mod_cosmetic_allow, in->mod_cosmetic_allow,
           sizeof(out->mod_cosmetic_allow));
    e = caps_ext(out);
    e->widescreen = in->widescreen;
    e->widescreen_hud = in->widescreen_hud;
    e->ignore_aspect = in->ignore_aspect;
    e->ws_extra = in->ws_extra;
}

void snes_lobby_caps_from_rnet(const RNetLobbyMatchCaps *in,
                               SnesLobbyMatchCaps *out)
{
    const SnesLobbyCapsExt *e;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!in) return;
    out->valid = in->valid;
    out->input_delay = in->input_delay;
    out->input_prediction = in->input_prediction;
    out->force_turn = in->force_turn;
    out->force_input_relay = in->force_input_relay;
    out->rollback = in->rollback;
    out->mod_count = in->mod_count;
    memcpy(out->mods, in->mods, sizeof(out->mods));
    memcpy(out->mod_set, in->mod_set, sizeof(out->mod_set));
    memcpy(out->mod_cosmetic_allow, in->mod_cosmetic_allow,
           sizeof(out->mod_cosmetic_allow));
    e = caps_ext_c(in);
    out->widescreen = e->widescreen;
    out->widescreen_hud = e->widescreen_hud;
    out->ignore_aspect = e->ignore_aspect;
    out->ws_extra = e->ws_extra;
}

void snes_lobby_configure(void)
{
    static int done;
    RNetLobbyConfig cfg;
    if (done) return;
    done = 1;
    memset(&cfg, 0, sizeof(cfg));
    /* The compile-time pin is the fallback, exactly as the static initialiser
     * of the old client made it; a host that sets an identity overrides it. */
    cfg.game_version = SNES_GAME_VERSION;
    cfg.platform = "snes";
    cfg.max_players = SNES_LOBBY_MAX_PLAYERS;
    cfg.legacy_env_prefix = "SNES_NET_";
    rnet_lobby_configure(&cfg);
    rnet_lobby_set_caps_codec(&g_snes_codec);
}

const char *snes_lobby_default_url(void)
{
    snes_lobby_configure();
    return rnet_lobby_default_url();
}

int snes_lobby_connect(const char *ws_url)
{
    snes_lobby_configure();
    return rnet_lobby_connect(ws_url);
}

void snes_lobby_set_game_identity(const char *game_name,
                                  const char *game_version)
{
    snes_lobby_configure();
    rnet_lobby_set_game_identity(game_name,
                                 (game_version && game_version[0])
                                     ? game_version : SNES_GAME_VERSION);
}

const char *snes_lobby_game_version(void)
{
    snes_lobby_configure();
    return rnet_lobby_game_version();
}

int snes_lobby_version_filter_strict(void)
{
    snes_lobby_configure();
    return rnet_lobby_version_filter_strict();
}

int snes_lobby_create(const char *name, const char *game_name,
                      const char *game_version, const char *password,
                      const char *host_bind,
                      const SnesLobbyMatchCaps *match_caps, int max_slots)
{
    RNetLobbyMatchCaps caps;
    snes_lobby_configure();
    if (!match_caps)
        return rnet_lobby_create(name, game_name, game_version, password,
                                 host_bind, NULL, max_slots);
    snes_lobby_caps_to_rnet(match_caps, &caps);
    return rnet_lobby_create(name, game_name, game_version, password,
                             host_bind, &caps, max_slots);
}

const SnesLobbyMatchCaps *snes_lobby_match_caps(void)
{
    static SnesLobbyMatchCaps snap;
    snes_lobby_caps_from_rnet(rnet_lobby_match_caps(), &snap);
    return &snap;
}

int snes_lobby_set_match_caps(const SnesLobbyMatchCaps *caps)
{
    RNetLobbyMatchCaps c;
    if (!caps) return rnet_lobby_set_match_caps(NULL);
    snes_lobby_caps_to_rnet(caps, &c);
    return rnet_lobby_set_match_caps(&c);
}

int snes_lobby_request_start(const SnesLobbyMatchCaps *match_caps)
{
    RNetLobbyMatchCaps c;
    if (!match_caps) return rnet_lobby_request_start(NULL);
    snes_lobby_caps_to_rnet(match_caps, &c);
    return rnet_lobby_request_start(&c);
}

int snes_lobby_automatch_ruleset_get(int index, SnesLobbyRuleset *out)
{
    RNetLobbyRuleset r;
    if (!out || !rnet_lobby_automatch_ruleset_get(index, &r)) return 0;
    memset(out, 0, sizeof(*out));
    memcpy(out->id, r.id, sizeof(out->id));
    memcpy(out->label, r.label, sizeof(out->label));
    memcpy(out->caps_summary, r.caps_summary, sizeof(out->caps_summary));
    memcpy(out->game_version, r.game_version, sizeof(out->game_version));
    snes_lobby_caps_from_rnet(&r.caps, &out->caps);
    return 1;
}
