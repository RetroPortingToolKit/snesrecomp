#ifndef SNESRECOMP_TEST_STUB_RECOMP_NET_LOBBY_CLIENT_H
#define SNESRECOMP_TEST_STUB_RECOMP_NET_LOBBY_CLIENT_H
/*
 * Minimal declarations for compiling ROM-free SNES netplay unit tests when the
 * recomp-net submodule is not checked out. The test wrappers put the real
 * include path first, so an initialized submodule always wins over this file.
 * Nothing here is part of the shipped host or the netplay protocol.
 */
#include <stddef.h>
#include <stdint.h>

#define RNET_LOBBY_ID_LEN 64
#define RNET_LOBBY_NAME_LEN 64
#define RNET_LOBBY_VERSION_LEN 32
#define RNET_LOBBY_ENDPOINT_LEN 128
#define RNET_LOBBY_MAX_LIST 64
#define RNET_LOBBY_MAX_PLAYERS 4
#define RNET_LOBBY_MAX_SPECTATORS 8
#define RNET_LOBBY_MAX_MEMBERS 16
#define RNET_LOBBY_SPECTATOR_SLOT_BASE 4
#define RNET_LOBBY_MAX_ONLINE 64
#define RNET_LOBBY_CHAT_TEXT_LEN 256
#define RNET_LOBBY_CHAT_RING 64
#define RNET_LOBBY_MAX_MODS 32
#define RNET_LOBBY_MOD_ID_LEN 64
#define RNET_LOBBY_MOD_VER_LEN 32
#define RNET_LOBBY_MOD_NAME_LEN 128
#define RNET_LOBBY_MOD_FEATS_LEN 128
#define RNET_LOBBY_MOD_RELAY_MAX_BYTES 16
#define RNET_LOBBY_MAX_RULESETS 16
#define RNET_LOBBY_RULESET_ID_LEN 64
#define RNET_LOBBY_RULESET_LABEL_LEN 128
#define RNET_LOBBY_CAPS_SUMMARY_LEN 256

typedef enum RNetLobbyAutomatchState {
  RNET_LOBBY_AUTOMATCH_IDLE = 0,
  RNET_LOBBY_AUTOMATCH_QUEUED,
  RNET_LOBBY_AUTOMATCH_FOUND,
  RNET_LOBBY_AUTOMATCH_ACCEPTED,
  RNET_LOBBY_AUTOMATCH_FAILED
} RNetLobbyAutomatchState;

typedef struct RNetLobbyRow { int unused; } RNetLobbyRow;
typedef struct RNetLobbyOnlinePlayer { int unused; } RNetLobbyOnlinePlayer;
typedef struct RNetLobbyChatMsg { int unused; } RNetLobbyChatMsg;
typedef struct RNetLobbyMember { int unused; } RNetLobbyMember;
typedef struct RNetLobbyModPkg { int unused; } RNetLobbyModPkg;
typedef struct RNetLobbyJoinInfo { int unused; } RNetLobbyJoinInfo;
typedef struct RNetLobbyTurnCredentials { int unused; } RNetLobbyTurnCredentials;
typedef struct RNetLobbyAutomatchFound { int unused; } RNetLobbyAutomatchFound;
typedef struct RNetLobbyDesyncReport { int unused; } RNetLobbyDesyncReport;
typedef struct RNetLobbyMatchCaps { int unused; } RNetLobbyMatchCaps;
typedef struct RNetLobbyCapsCodec { int unused; } RNetLobbyCapsCodec;
typedef void (*RNetLobbyModOfferFn)(void);
typedef void (*RNetLobbyModExportFn)(void);
typedef void (*RNetLobbyModFreeFn)(void);
typedef void (*RNetLobbyModInstallFn)(void);

#endif
