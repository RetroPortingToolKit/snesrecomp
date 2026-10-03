#ifndef SNES_NETPLAY_ROUTE_H
#define SNES_NETPLAY_ROUTE_H

/*
 * snes_netplay_route -- the pure decisions behind "which transport does this
 * launch run on" and "which session slot does each ICE-relay guest take".
 * No lobby, no session, no sockets: ROM-free and unit-testable
 * (tests/netplay/snes_netplay_route_test.c).
 *
 * Host relay over ICE (recomp-net docs/host_integration.md): the launch says
 * transport "host" + relay_via "ice"; the match then runs over the ICE agents
 * the waiting room already connected (host = hub, guest = 1:1 to the host).
 * That is neither single-agent ICE (rnet_session_start_ice) nor LAN, and the
 * bind/peer endpoints in such a launch are placeholders that must never be
 * bound or dialled.
 */

#include "snes_netplay.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SnesNetplayRoute {
    SNES_ROUTE_NONE = 0,        /* not decided here: the legacy rules apply */
    SNES_ROUTE_ICE_HUB_HOST,    /* adopt one connected agent per guest seat */
    SNES_ROUTE_ICE_HUB_GUEST,   /* adopt the one connected agent to the host */
    SNES_ROUTE_REFUSE           /* a launch this build must not start */
} SnesNetplayRoute;

/* Why SNES_ROUTE_REFUSE (0 when not refused). */
typedef enum SnesNetplayRouteWhy {
    SNES_ROUTE_WHY_NONE = 0,
    SNES_ROUTE_WHY_NO_ICE_BUILD,   /* build has no ICE */
    SNES_ROUTE_WHY_SPECTATOR,      /* spectators are not on the ICE path */
    SNES_ROUTE_WHY_BAD_SLOT        /* local slot / count unusable for a hub */
} SnesNetplayRouteWhy;

/*
 * Decide the route for a launch that said transport_ice_hub. Anything that is
 * not an ICE-hub launch returns SNES_ROUTE_NONE and leaves the existing LAN /
 * single-agent ICE / legacy transport_host (UDP endpoint hub) logic alone.
 * `ice_built` is 1 when this build links ICE (RNET_ENABLE_ICE).
 */
SnesNetplayRoute snes_netplay_route_decide(const SnesNetplayConfig *cfg,
                                           int ice_built,
                                           SnesNetplayRouteWhy *why);

/*
 * Map the host's lobby seats to SESSION slots for rnet_session_start_ice_hub_adopt.
 *
 * SNES session slots are SEAT-mapped (snes_host_lobby.c: RECOMP_NETPLAY_SLOTS_
 * SEAT, no host-in-the-gallery offset), so a guest's session slot is its lobby
 * seat. The hub host is session slot 0. Refuses (returns -1, *reason set) when
 * the mapping cannot be proven:
 *   - host_slot != 0 (a host that moved seats is not the hub's slot 0),
 *   - n < 1, or n != slot_count - 1 (a seat with no agent would stall the
 *     match forever; a surplus agent has no seat),
 *   - a lobby slot outside 1..slot_count-1, or used twice.
 * On success session_slots[i] is the slot for lobby_slots[i].
 */
int snes_netplay_ice_hub_map_slots(int host_slot, int slot_count,
                                   const int *lobby_slots, int n,
                                   int *session_slots, const char **reason);

#ifdef __cplusplus
}
#endif

#endif /* SNES_NETPLAY_ROUTE_H */
