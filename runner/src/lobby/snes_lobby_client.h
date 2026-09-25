/*
 * snes_lobby_client.h -- the SNES spelling of recomp-net's lobby client.
 *
 * The WebSocket lobby protocol client used to live here (4,653 lines). It is
 * recomp-net's now (recomp_net/lobby_client.h, src/lobby/rnet_lobby_client.c),
 * so the fixes reach every engine instead of one fork each. What remains is
 * the SNES-specific part and the old names, kept so the ports that call
 * snes_lobby_* (GundamWing, MetalWarriors, SecretOfEvermore, SMW) compile
 * unchanged:
 *
 *   - SnesLobbyMatchCaps keeps its widescreen fields. On the wire they are the
 *     SNES engine keys of match_caps (widescreen, widescreen_hud,
 *     ignore_aspect, ws_extra), carried in RNetLobbyMatchCaps.ext through the
 *     caps codec below. Functions that take or return caps convert.
 *   - SNES_NET_LOBBY_URL / SNES_NET_GAME_VERSION keep working: the SNES
 *     configuration registers "SNES_NET_" as recomp-net's legacy environment
 *     prefix (RNET_LOBBY_URL / RNET_LOBBY_GAME_VERSION win when both are set).
 *   - SNES_GAME_VERSION stays the compile-time fallback pin.
 *
 * Everything else is recomp-net's function under its old name.
 */
#ifndef SNES_LOBBY_CLIENT_H
#define SNES_LOBBY_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "recomp_net/lobby_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNES_LOBBY_ID_LEN             RNET_LOBBY_ID_LEN
#define SNES_LOBBY_NAME_LEN           RNET_LOBBY_NAME_LEN
#define SNES_LOBBY_VERSION_LEN        RNET_LOBBY_VERSION_LEN
#define SNES_LOBBY_ENDPOINT_LEN       RNET_LOBBY_ENDPOINT_LEN
#define SNES_LOBBY_MAX_LIST           RNET_LOBBY_MAX_LIST
/* The SNES title ceiling (what create() is clamped to). The client's arrays
 * are RNET_LOBBY_MAX_PLAYERS wide. */
#define SNES_LOBBY_MAX_PLAYERS        4
#define SNES_LOBBY_MAX_SPECTATORS     RNET_LOBBY_MAX_SPECTATORS
#define SNES_LOBBY_MAX_MEMBERS        RNET_LOBBY_MAX_MEMBERS
#define SNES_LOBBY_SPECTATOR_SLOT_BASE RNET_LOBBY_SPECTATOR_SLOT_BASE
#define SNES_LOBBY_MAX_ONLINE         RNET_LOBBY_MAX_ONLINE
#define SNES_LOBBY_CHAT_TEXT_LEN      RNET_LOBBY_CHAT_TEXT_LEN
#define SNES_LOBBY_CHAT_RING          RNET_LOBBY_CHAT_RING
#define SNES_LOBBY_MAX_MODS           RNET_LOBBY_MAX_MODS
#define SNES_LOBBY_MOD_ID_LEN         RNET_LOBBY_MOD_ID_LEN
#define SNES_LOBBY_MOD_VER_LEN        RNET_LOBBY_MOD_VER_LEN
#define SNES_LOBBY_MOD_NAME_LEN       RNET_LOBBY_MOD_NAME_LEN
#define SNES_LOBBY_MOD_FEATS_LEN      RNET_LOBBY_MOD_FEATS_LEN
#define SNES_LOBBY_MOD_RELAY_MAX_BYTES RNET_LOBBY_MOD_RELAY_MAX_BYTES
#define SNES_LOBBY_MAX_RULESETS       RNET_LOBBY_MAX_RULESETS
#define SNES_LOBBY_RULESET_ID_LEN     RNET_LOBBY_RULESET_ID_LEN
#define SNES_LOBBY_RULESET_LABEL_LEN  RNET_LOBBY_RULESET_LABEL_LEN
#define SNES_LOBBY_CAPS_SUMMARY_LEN   RNET_LOBBY_CAPS_SUMMARY_LEN

#ifndef SNES_GAME_VERSION
#ifdef SNESRECOMP_BUILD_VERSION
#define SNES_GAME_VERSION SNESRECOMP_BUILD_VERSION
#else
#define SNES_GAME_VERSION "dev"
#endif
#endif

typedef RNetLobbyRow             SnesLobbyRow;
typedef RNetLobbyOnlinePlayer    SnesLobbyOnlinePlayer;
typedef RNetLobbyChatMsg         SnesLobbyChatMsg;
typedef RNetLobbyMember          SnesLobbyMember;
typedef RNetLobbyModPkg          SnesLobbyModPkg;
typedef RNetLobbyJoinInfo        SnesLobbyJoinInfo;
typedef RNetLobbyTurnCredentials SnesLobbyTurnCredentials;
typedef RNetLobbyAutomatchFound  SnesLobbyAutomatchFound;
typedef RNetLobbyDesyncReport    SnesLobbyDesyncReport;
typedef RNetLobbyModOfferFn      SnesLobbyModOfferFn;
typedef RNetLobbyModExportFn     SnesLobbyModExportFn;
typedef RNetLobbyModFreeFn       SnesLobbyModFreeFn;
typedef RNetLobbyModInstallFn    SnesLobbyModInstallFn;

enum {
    SNES_LOBBY_AUTOMATCH_IDLE = RNET_LOBBY_AUTOMATCH_IDLE,
    SNES_LOBBY_AUTOMATCH_QUEUED = RNET_LOBBY_AUTOMATCH_QUEUED,
    SNES_LOBBY_AUTOMATCH_FOUND = RNET_LOBBY_AUTOMATCH_FOUND,
    SNES_LOBBY_AUTOMATCH_ACCEPTED = RNET_LOBBY_AUTOMATCH_ACCEPTED,
    SNES_LOBBY_AUTOMATCH_FAILED = RNET_LOBBY_AUTOMATCH_FAILED
};

/*
 * Host-authoritative sim settings negotiated over the lobby -- the generic
 * fields of RNetLobbyMatchCaps (see recomp_net/lobby_client.h for what each
 * one means) plus the SNES engine's own keys. Field names are the historic
 * ones; the ports read and write them directly.
 */
typedef struct SnesLobbyMatchCaps {
    int  valid;
    int  widescreen;       /* SNES key: 0/1 */
    int  widescreen_hud;   /* SNES key: 0/1 (absent on the wire reads 1) */
    int  ignore_aspect;    /* SNES key: 0/1 */
    int  input_delay;
    int  ws_extra;         /* SNES key: widescreen margin; 0 = game default */
    int  force_turn;
    int  force_input_relay;
    int  rollback;
    /* Rollback invent runway P; 0 = the host published none. */
    int  input_prediction;
    int  mod_count;
    SnesLobbyModPkg mods[SNES_LOBBY_MAX_MODS];
    char mod_set[512];
    char mod_cosmetic_allow[512];
} SnesLobbyMatchCaps;

typedef struct SnesLobbyRuleset {
    char id[SNES_LOBBY_RULESET_ID_LEN];
    char label[SNES_LOBBY_RULESET_LABEL_LEN];
    char caps_summary[SNES_LOBBY_CAPS_SUMMARY_LEN];
    SnesLobbyMatchCaps caps;
    char game_version[SNES_LOBBY_VERSION_LEN];
} SnesLobbyRuleset;

/* The SNES keys as they sit in RNetLobbyMatchCaps.ext. */
typedef struct SnesLobbyCapsExt {
    int widescreen;
    int widescreen_hud;
    int ignore_aspect;
    int ws_extra;
} SnesLobbyCapsExt;

void snes_lobby_caps_to_rnet(const SnesLobbyMatchCaps *in, RNetLobbyMatchCaps *out);
void snes_lobby_caps_from_rnet(const RNetLobbyMatchCaps *in, SnesLobbyMatchCaps *out);
/* The codec that writes / parses the SNES keys. */
const RNetLobbyCapsCodec *snes_lobby_caps_codec(void);
/* Tell recomp-net what this build is: platform "snes", the SNES seat ceiling,
 * the SNES_NET_ environment alias, the SNES codec, SNES_GAME_VERSION as the
 * fallback pin. Idempotent; every snes_lobby_* entry point that could be the
 * first one calls it. */
void snes_lobby_configure(void);

/* ---- the calls that differ from recomp-net's: SNES-typed caps, or the
 * first-use configuration above. */
const char *snes_lobby_default_url(void);
int  snes_lobby_connect(const char *ws_url);
void snes_lobby_set_game_identity(const char *game_name,
                                  const char *game_version);
const char *snes_lobby_game_version(void);
int  snes_lobby_version_filter_strict(void);
int  snes_lobby_create(const char *name, const char *game_name,
                       const char *game_version, const char *password,
                       const char *host_bind,
                       const SnesLobbyMatchCaps *match_caps, int max_slots);
/* A snapshot of the latest host caps, converted; valid until the next call. */
const SnesLobbyMatchCaps *snes_lobby_match_caps(void);
int  snes_lobby_set_match_caps(const SnesLobbyMatchCaps *caps);
int  snes_lobby_request_start(const SnesLobbyMatchCaps *match_caps);
int  snes_lobby_automatch_ruleset_get(int index, SnesLobbyRuleset *out);

/* ---- everything else: recomp-net's function, old name. */
#define snes_lobby_set_mod_offer_supplier   rnet_lobby_set_mod_offer_supplier
#define snes_lobby_need_mods_count          rnet_lobby_need_mods_count
#define snes_lobby_need_mods_get            rnet_lobby_need_mods_get
#define snes_lobby_need_mods_can_transfer   rnet_lobby_need_mods_can_transfer
#define snes_lobby_match_blocked_by_mods    rnet_lobby_match_blocked_by_mods
#define snes_lobby_local_missing_mods       rnet_lobby_local_missing_mods
#define snes_lobby_disconnect               rnet_lobby_disconnect
#define snes_lobby_connected                rnet_lobby_connected
#define snes_lobby_url                      rnet_lobby_url
#define snes_lobby_set_display_name         rnet_lobby_set_display_name
#define snes_lobby_display_name             rnet_lobby_display_name
#define snes_lobby_player_id                rnet_lobby_player_id
#define snes_lobby_pump                     rnet_lobby_pump
#define snes_lobby_request_list             rnet_lobby_request_list
#define snes_lobby_list_count               rnet_lobby_list_count
#define snes_lobby_list_get                 rnet_lobby_list_get
#define snes_lobby_online_count             rnet_lobby_online_count
#define snes_lobby_online_get               rnet_lobby_online_get
#define snes_lobby_join                     rnet_lobby_join
#define snes_lobby_leave                    rnet_lobby_leave
#define snes_lobby_set_allow_spectators     rnet_lobby_set_allow_spectators
#define snes_lobby_allow_spectators_pref    rnet_lobby_allow_spectators_pref
#define snes_lobby_allow_spectators         rnet_lobby_allow_spectators
#define snes_lobby_max_spectators           rnet_lobby_max_spectators
#define snes_lobby_spectator_count          rnet_lobby_spectator_count
#define snes_lobby_local_is_spectator       rnet_lobby_local_is_spectator
#define snes_lobby_spectator_slot_base      rnet_lobby_spectator_slot_base
#define snes_lobby_seat_valid               rnet_lobby_seat_valid
#define snes_lobby_spectator_slot           rnet_lobby_spectator_slot
#define snes_lobby_local_wire_slot          rnet_lobby_local_wire_slot
#define snes_lobby_report_chat              rnet_lobby_report_chat
#define snes_lobby_send_chat                rnet_lobby_send_chat
#define snes_lobby_send_server_chat         rnet_lobby_send_server_chat
#define snes_lobby_server_chat_count        rnet_lobby_server_chat_count
#define snes_lobby_server_chat_get          rnet_lobby_server_chat_get
#define snes_lobby_seat_move_self           rnet_lobby_seat_move_self
#define snes_lobby_seat_swap_request        rnet_lobby_seat_swap_request
#define snes_lobby_seat_swap_incoming       rnet_lobby_seat_swap_incoming
#define snes_lobby_seat_swap_respond        rnet_lobby_seat_swap_respond
#define snes_lobby_seat_swap_outgoing       rnet_lobby_seat_swap_outgoing
#define snes_lobby_seat_swap_clear          rnet_lobby_seat_swap_clear
#define snes_lobby_chat_count               rnet_lobby_chat_count
#define snes_lobby_chat_get                 rnet_lobby_chat_get
#define snes_lobby_chat_clear               rnet_lobby_chat_clear
#define snes_lobby_kick                     rnet_lobby_kick
#define snes_lobby_move                     rnet_lobby_move
#define snes_lobby_in_lobby                 rnet_lobby_in_lobby
#define snes_lobby_is_host                  rnet_lobby_is_host
#define snes_lobby_host_player_id           rnet_lobby_host_player_id
#define snes_lobby_join_info                rnet_lobby_join_info
#define snes_lobby_member_count             rnet_lobby_member_count
#define snes_lobby_member_get               rnet_lobby_member_get
#define snes_lobby_member_latency_ms        rnet_lobby_member_latency_ms
#define snes_lobby_member_is_host           rnet_lobby_member_is_host
#define snes_lobby_local_ready              rnet_lobby_local_ready
#define snes_lobby_all_ready                rnet_lobby_all_ready
#define snes_lobby_set_ready                rnet_lobby_set_ready
#define snes_lobby_launch_pending           rnet_lobby_launch_pending
#define snes_lobby_clear_launch_pending     rnet_lobby_clear_launch_pending
#define snes_lobby_clear_last_error         rnet_lobby_clear_last_error
#define snes_lobby_try_fill_launch          rnet_lobby_try_fill_launch
#define snes_lobby_send_signal_to           rnet_lobby_send_signal_to
#define snes_lobby_send_signal              rnet_lobby_send_signal
#define snes_lobby_set_mod_transfer_hooks   rnet_lobby_set_mod_transfer_hooks
#define snes_lobby_mod_relay_size_allows    rnet_lobby_mod_relay_size_allows
#define snes_lobby_mod_request              rnet_lobby_mod_request
#define snes_lobby_mod_cancel               rnet_lobby_mod_cancel
#define snes_lobby_mod_progress             rnet_lobby_mod_progress
#define snes_lobby_mod_failed               rnet_lobby_mod_failed
#define snes_lobby_mod_in_flight            rnet_lobby_mod_in_flight
#define snes_lobby_mod_xfer_pump            rnet_lobby_mod_xfer_pump
#define snes_lobby_poll_signal              rnet_lobby_poll_signal
#define snes_lobby_request_turn_credentials rnet_lobby_request_turn_credentials
#define snes_lobby_turn_credentials         rnet_lobby_turn_credentials
#define snes_lobby_set_disc_fp              rnet_lobby_set_disc_fp
#define snes_lobby_disc_fp                  rnet_lobby_disc_fp
#define snes_lobby_report_desync            rnet_lobby_report_desync
#define snes_lobby_set_blocks               rnet_lobby_set_blocks
#define snes_lobby_automatch_request_rulesets rnet_lobby_automatch_request_rulesets
#define snes_lobby_automatch_available      rnet_lobby_automatch_available
#define snes_lobby_automatch_ruleset_count  rnet_lobby_automatch_ruleset_count
#define snes_lobby_automatch_queue          rnet_lobby_automatch_queue
#define snes_lobby_automatch_cancel         rnet_lobby_automatch_cancel
#define snes_lobby_automatch_state          rnet_lobby_automatch_state
#define snes_lobby_automatch_queued_secs    rnet_lobby_automatch_queued_secs
#define snes_lobby_automatch_pool           rnet_lobby_automatch_pool
#define snes_lobby_automatch_found_get      rnet_lobby_automatch_found_get
#define snes_lobby_automatch_accept         rnet_lobby_automatch_accept
#define snes_lobby_automatch_room           rnet_lobby_automatch_room
#define snes_lobby_automatch_refuse_local   rnet_lobby_automatch_refuse_local
#define snes_lobby_automatch_error          rnet_lobby_automatch_error

#ifdef __cplusplus
}
#endif

#endif /* SNES_LOBBY_CLIENT_H */
