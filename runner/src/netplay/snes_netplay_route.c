#include "snes_netplay_route.h"

#include <stddef.h>

SnesNetplayRoute snes_netplay_route_decide(const SnesNetplayConfig *cfg,
                                           int ice_built,
                                           SnesNetplayRouteWhy *why)
{
    if (why) *why = SNES_ROUTE_WHY_NONE;
    if (!cfg || !cfg->transport_ice_hub)
        return SNES_ROUTE_NONE;
    /* The server owns the transport; a forced-LAN env/UI choice must not
     * override a launch whose endpoints are placeholders, so cfg->transport
     * is deliberately not consulted. */
    if (!ice_built) {
        if (why) *why = SNES_ROUTE_WHY_NO_ICE_BUILD;
        return SNES_ROUTE_REFUSE;
    }
    if (cfg->spectator) {
        if (why) *why = SNES_ROUTE_WHY_SPECTATOR;
        return SNES_ROUTE_REFUSE;
    }
    if (cfg->local_slot < 0 ||
        (cfg->slot_count > 0 && cfg->local_slot >= cfg->slot_count)) {
        if (why) *why = SNES_ROUTE_WHY_BAD_SLOT;
        return SNES_ROUTE_REFUSE;
    }
    return cfg->local_slot == 0 ? SNES_ROUTE_ICE_HUB_HOST
                                : SNES_ROUTE_ICE_HUB_GUEST;
}

int snes_netplay_ice_hub_map_slots(int host_slot, int slot_count,
                                   const int *lobby_slots, int n,
                                   int *session_slots, const char **reason)
{
    unsigned seen = 0;
    int i;
    const char *r = NULL;

    if (!lobby_slots || !session_slots) r = "no seats";
    else if (slot_count < 2 || slot_count > SNES_NETPLAY_MAX_SLOTS)
        r = "seat count out of range";
    else if (host_slot != 0)
        r = "the ICE hub host must hold seat 0";
    else if (n < 1 || n != slot_count - 1)
        r = "ICE agents do not match the seated guests";
    for (i = 0; !r && i < n; ++i) {
        const int s = lobby_slots[i];
        if (s < 1 || s >= slot_count) r = "guest seat out of range";
        else if (seen & (1u << s)) r = "guest seat used twice";
        else { seen |= 1u << s; session_slots[i] = s; }
    }
    if (r) {
        if (reason) *reason = r;
        return -1;
    }
    return 0;
}
