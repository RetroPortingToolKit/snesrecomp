/* ROM-free test of the host-relay-over-ICE launch decision and slot map. */
#include <stdio.h>
#include <string.h>

#include "netplay/snes_netplay_route.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static SnesNetplayConfig cfg_for(int ice_hub, int slot, int count)
{
    SnesNetplayConfig c;
    memset(&c, 0, sizeof(c));
    c.enabled = 1;
    c.transport_host = 1;
    c.transport_ice_hub = ice_hub;
    c.local_slot = slot;
    c.slot_count = count;
    /* the launch's placeholder endpoints */
    strcpy(c.bind_hostport, "0.0.0.0:0");
    c.peer_hostport[0] = '\0';
    return c;
}

int main(void)
{
    SnesNetplayRouteWhy why;
    SnesNetplayConfig c;
    int lobby[3], sess[3];
    const char *r = NULL;

    /* Legacy transport_host (UDP endpoint hub) and plain launches untouched. */
    c = cfg_for(0, 0, 3);
    CHECK(snes_netplay_route_decide(&c, 1, &why) == SNES_ROUTE_NONE);
    c.transport_host = 0;
    CHECK(snes_netplay_route_decide(&c, 1, &why) == SNES_ROUTE_NONE);
    CHECK(snes_netplay_route_decide(NULL, 1, &why) == SNES_ROUTE_NONE);

    /* ICE hub launches: host / guest, regardless of forced transport. */
    c = cfg_for(1, 0, 3);
    CHECK(snes_netplay_route_decide(&c, 1, &why) == SNES_ROUTE_ICE_HUB_HOST);
    c.transport = 2; /* forced LAN must not win over the launch */
    CHECK(snes_netplay_route_decide(&c, 1, &why) == SNES_ROUTE_ICE_HUB_HOST);
    c.transport = 1;
    CHECK(snes_netplay_route_decide(&c, 1, &why) == SNES_ROUTE_ICE_HUB_HOST);
    c = cfg_for(1, 2, 3);
    CHECK(snes_netplay_route_decide(&c, 1, &why) == SNES_ROUTE_ICE_HUB_GUEST);
    c = cfg_for(1, 1, 2);
    CHECK(snes_netplay_route_decide(&c, 1, &why) == SNES_ROUTE_ICE_HUB_GUEST);

    /* Refusals. */
    c = cfg_for(1, 0, 2);
    CHECK(snes_netplay_route_decide(&c, 0, &why) == SNES_ROUTE_REFUSE &&
          why == SNES_ROUTE_WHY_NO_ICE_BUILD);
    c.spectator = 1;
    CHECK(snes_netplay_route_decide(&c, 1, &why) == SNES_ROUTE_REFUSE &&
          why == SNES_ROUTE_WHY_SPECTATOR);
    c = cfg_for(1, 3, 3);
    CHECK(snes_netplay_route_decide(&c, 1, &why) == SNES_ROUTE_REFUSE &&
          why == SNES_ROUTE_WHY_BAD_SLOT);
    c = cfg_for(1, -1, 3);
    CHECK(snes_netplay_route_decide(&c, 1, &why) == SNES_ROUTE_REFUSE);

    /* Slot map: lobby seat == session slot (SEAT policy), host is 0. */
    lobby[0] = 2; lobby[1] = 1;
    CHECK(snes_netplay_ice_hub_map_slots(0, 3, lobby, 2, sess, &r) == 0);
    CHECK(sess[0] == 2 && sess[1] == 1);
    lobby[0] = 1;
    CHECK(snes_netplay_ice_hub_map_slots(0, 2, lobby, 1, sess, &r) == 0 && sess[0] == 1);

    /* Unprovable maps are refused, never guessed. */
    lobby[0] = 1; lobby[1] = 1;
    CHECK(snes_netplay_ice_hub_map_slots(0, 3, lobby, 2, sess, &r) == -1 && r); /* dup */
    lobby[0] = 0; lobby[1] = 1;
    CHECK(snes_netplay_ice_hub_map_slots(0, 3, lobby, 2, sess, &r) == -1);       /* host seat */
    lobby[0] = 1; lobby[1] = 3;
    CHECK(snes_netplay_ice_hub_map_slots(0, 3, lobby, 2, sess, &r) == -1);       /* >= count */
    lobby[0] = 1;
    CHECK(snes_netplay_ice_hub_map_slots(0, 3, lobby, 1, sess, &r) == -1);       /* missing agent */
    CHECK(snes_netplay_ice_hub_map_slots(1, 2, lobby, 1, sess, &r) == -1);       /* host moved */
    CHECK(snes_netplay_ice_hub_map_slots(0, 2, lobby, 0, sess, &r) == -1);
    CHECK(snes_netplay_ice_hub_map_slots(0, 9, lobby, 1, sess, &r) == -1);

    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("snes_netplay_route_test: ok\n");
    return 0;
}
