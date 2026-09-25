/*
 * The SNES half of the lobby: the widescreen match_caps keys and the
 * SnesLobbyMatchCaps <-> RNetLobbyMatchCaps conversion.
 *
 * The lobby client itself moved to recomp-net (with its own test,
 * lib/recomp-net tests/lobby_client_test.c, which is where this file's
 * predecessor lobby_mod_plan_test.c went). What stays here is only what
 * snesrecomp still owns, tested through the REAL client: both translation
 * units are included, so the codec runs inside the actual encoder and parser.
 *
 * No networking: every entry point the client could reach for a socket is a
 * trap that aborts, so a case that starts depending on one fails loudly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Found through -I lib/recomp-net/src (run_c_tests.sh). */
#include "lobby/rnet_lobby_client.c"
#include "../../runner/src/lobby/snes_lobby_client.c"

#define TRAP(name) \
    do { fprintf(stderr, "snes_lobby_caps_test: %s called; this test does " \
                         "no networking\n", name); abort(); } while (0)

int rnet_udp_find_free_port(int preferred, int span)
{ (void)preferred; (void)span; TRAP("rnet_udp_find_free_port"); }
int  rnet_ice_xfer_open(RNetIceXfer **o, const RNetIceConfig *c,
                        RNetIceXferSignalEmitFn e, void *u)
{ (void)o; (void)c; (void)e; (void)u; TRAP("rnet_ice_xfer_open"); }
void rnet_ice_xfer_close(RNetIceXfer **x) { (void)x; TRAP("close"); }
void rnet_ice_xfer_push_signal(RNetIceXfer *x, const RNetSignal *m)
{ (void)x; (void)m; TRAP("push_signal"); }
void rnet_ice_xfer_pump(RNetIceXfer *x) { (void)x; TRAP("pump"); }
int  rnet_ice_xfer_queue_blob(RNetIceXfer *x, uint8_t *d, size_t l)
{ (void)x; (void)d; (void)l; TRAP("queue_blob"); }
int  rnet_ice_xfer_send_idle(const RNetIceXfer *x) { (void)x; TRAP("send_idle"); }
int  rnet_ice_xfer_take_blob(RNetIceXfer *x, uint8_t **d, size_t *l)
{ (void)x; (void)d; (void)l; TRAP("take_blob"); }
int  rnet_ice_xfer_progress(const RNetIceXfer *x) { (void)x; TRAP("progress"); }
int  rnet_ice_xfer_failed(const RNetIceXfer *x, char *e, size_t c)
{ (void)x; (void)e; (void)c; TRAP("failed"); }
void rnet_ice_xfer_path(const RNetIceXfer *x, char *o, size_t c)
{ (void)x; (void)o; (void)c; TRAP("path"); }
RNetIceState rnet_ice_xfer_state(const RNetIceXfer *x) { (void)x; TRAP("state"); }
const char *rnet_ice_state_name(RNetIceState st) { (void)st; TRAP("state_name"); }
int rnet_ws_write_text(int fd, const char *text, int client_mask)
{ (void)fd; (void)text; (void)client_mask; TRAP("rnet_ws_write_text"); }
const char *rnet_account_session(void) { TRAP("rnet_account_session"); }

static int fails;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("    FAIL %s\n", what); fails++; }
}

static void case_widescreen_keys_round_trip(void)
{
    SnesLobbyMatchCaps in, out;
    RNetLobbyMatchCaps wire, back;
    char json[RNET_LOBBY_MAX_MODS * 256 + 1024];
    char obj[RNET_LOBBY_MAX_MODS * 256 + 1024];
    printf("  widescreen keys\n");

    snes_lobby_configure();
    memset(&in, 0, sizeof(in));
    in.valid = 1;
    in.widescreen = 1;
    in.widescreen_hud = 0;
    in.ignore_aspect = 1;
    in.ws_extra = 43;
    in.input_delay = 8;
    in.rollback = 1;
    snes_lobby_caps_to_rnet(&in, &wire);
    ck(append_match_caps_json(json, sizeof(json), &wire) > 0, "encode");
    /* The same four keys, spelled as the pre-lift client spelled them, so a
     * peer on an older build reads them unchanged. */
    ck(strstr(json, "\"widescreen\":true") != NULL, "widescreen");
    ck(strstr(json, "\"widescreen_hud\":false") != NULL, "widescreen_hud");
    ck(strstr(json, "\"ignore_aspect\":true") != NULL, "ignore_aspect");
    ck(strstr(json, "\"ws_extra\":43") != NULL, "ws_extra");
    ck(json_extract_object(json, "match_caps", obj, sizeof(obj)) != 0, "extract");
    parse_match_caps_object(obj, &back);
    snes_lobby_caps_from_rnet(&back, &out);
    ck(out.valid && out.widescreen == 1 && out.widescreen_hud == 0 &&
       out.ignore_aspect == 1 && out.ws_extra == 43, "all four survive");
    ck(out.input_delay == 8 && out.rollback == 1, "generic keys survive");
}

static void case_pre_lift_blob_reads_the_same(void)
{
    /* Exactly what a pre-lift SNES host published (snes_lobby_client.c
     * 36d6ce5 append_match_caps_json, key order and all). */
    const char *old =
        "{\"v\":1,\"widescreen\":true,\"widescreen_hud\":true,"
        "\"ignore_aspect\":false,\"input_delay\":9,\"ws_extra\":24,"
        "\"force_turn\":false,\"force_input_relay\":true,\"rollback\":false,"
        "\"mod_plan\":[],\"mod_set\":\"\",\"mod_cosmetic_allow\":\"\"}";
    RNetLobbyMatchCaps r;
    SnesLobbyMatchCaps s;
    printf("  a pre-lift host's caps\n");
    snes_lobby_configure();
    parse_match_caps_object(old, &r);
    snes_lobby_caps_from_rnet(&r, &s);
    ck(s.widescreen == 1 && s.widescreen_hud == 1 && s.ignore_aspect == 0 &&
       s.ws_extra == 24, "SNES keys");
    ck(s.input_delay == 9 && s.force_input_relay == 1 && s.rollback == 0,
       "generic keys");
    ck(s.input_prediction == 0, "no runway published");
}

static void case_hud_absent_reads_on(void)
{
    RNetLobbyMatchCaps r;
    SnesLobbyMatchCaps s;
    printf("  widescreen_hud default\n");
    snes_lobby_configure();
    parse_match_caps_object("{\"v\":1,\"input_delay\":4}", &r);
    snes_lobby_caps_from_rnet(&r, &s);
    ck(s.widescreen_hud == 1, "an absent widescreen_hud reads 1, as it always did");
    ck(s.widescreen == 0 && s.ws_extra == 0, "the rest read 0");
    parse_match_caps_object("{\"ws_extra\":-5}", &r);
    snes_lobby_caps_from_rnet(&r, &s);
    ck(s.ws_extra == 0, "a negative margin is clamped to 0");
}

static void case_conversion_is_lossless(void)
{
    SnesLobbyMatchCaps a, b;
    RNetLobbyMatchCaps mid;
    printf("  conversion\n");
    memset(&a, 0, sizeof(a));
    a.valid = 1; a.widescreen = 1; a.widescreen_hud = 1; a.ignore_aspect = 1;
    a.input_delay = 11; a.ws_extra = 64; a.force_turn = 1;
    a.force_input_relay = 1; a.rollback = 1; a.input_prediction = 12;
    a.mod_count = 1;
    snprintf(a.mods[0].id, sizeof(a.mods[0].id), "%s", "gwed.localization");
    snprintf(a.mods[0].ver, sizeof(a.mods[0].ver), "%s", "1.0.0");
    snprintf(a.mod_set, sizeof(a.mod_set), "%s", "x@1/y z=1");
    snprintf(a.mod_cosmetic_allow, sizeof(a.mod_cosmetic_allow), "%s", "p@1");
    snes_lobby_caps_to_rnet(&a, &mid);
    snes_lobby_caps_from_rnet(&mid, &b);
    ck(memcmp(&a, &b, sizeof(a)) == 0, "SNES -> RNet -> SNES is the identity");
}

static void case_snes_env_alias(void)
{
    printf("  SNES_NET_ environment names\n");
    snes_lobby_configure();
    unsetenv("RNET_LOBBY_URL");
    setenv("SNES_NET_LOBBY_URL", "ws://127.0.0.1:1", 1);
    ck(!strcmp(snes_lobby_default_url(), "ws://127.0.0.1:1"),
       "SNES_NET_LOBBY_URL still points the SNES build at a server");
    unsetenv("SNES_NET_LOBBY_URL");
    ck(!strcmp(g_cfg.platform, "snes"), "chat reports say snes");
    ck(g_cfg.max_players == SNES_LOBBY_MAX_PLAYERS, "the SNES seat ceiling");
}

int main(void)
{
    case_widescreen_keys_round_trip();
    case_pre_lift_blob_reads_the_same();
    case_hud_absent_reads_on();
    case_conversion_is_lossless();
    case_snes_env_alias();
    printf(fails ? "\n%d failure(s)\n" : "\nall SNES lobby caps cases passed\n",
           fails);
    return fails != 0;
}
