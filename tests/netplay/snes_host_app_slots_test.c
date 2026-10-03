/*
 * ROM-free test: snes_host_app_apply_launch -> SnesNetplayConfig.slot_count.
 *
 * The launch struct carries the room's seated-player count (player_count).
 * Only the ICE-hub branch used to copy it into slot_count, so an ordinary
 * (LAN / relay / legacy transport_host) lobby with 3+ players silently ran
 * two seats. The legacy 3+ seat hub (transport_host, empty peer endpoint) was
 * therefore reachable only through SNES_NET_SLOTS.
 *
 * The real snes_host_app.c and the real snes_netplay_config_defaults /
 * snes_netplay_apply_env (snes_netplay.c, built without SNESRECOMP_NET) are
 * under test. Only the two lobby-client lookups apply_launch makes are
 * provided here; no networking, no ROM.
 *
 * Game-level limits are NOT applied here and must still win: a game refuses
 * a launch in its own fill_launch (Mega Man X: player_count != 2 fails), which
 * runs before apply_launch ever sees the struct. The engine-level limit
 * (seats > 2 needs a multitap, RtlPlayerCount) is enforced by
 * snes_netplay_start, which refuses loudly rather than degrading.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netplay/snes_host_app.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

/* Collaborators apply_launch consults; neither affects slot_count here. */
static SnesLobbyJoinInfo g_join;
const SnesLobbyJoinInfo *rnet_lobby_join_info(void) { return &g_join; }
static SnesLobbyMatchCaps g_caps;
const SnesLobbyMatchCaps *snes_lobby_match_caps(void) { return &g_caps; }

static int slots_for(int player_count, int transport_host, int spectator)
{
    RecompLauncherCNetplayLaunch l;
    SnesHostLaunchResult r;
    memset(&l, 0, sizeof(l));
    l.enabled = 1;
    l.player_count = player_count;
    l.max_slots = player_count > 2 ? player_count : 2;
    l.transport_host = transport_host;
    l.is_spectator = spectator;
    if (spectator) l.spectator_wire_slot = 9;
    snes_host_app_apply_launch(&l, &r);
    CHECK(r.netplay_enabled);
    return r.net_cfg.slot_count;
}

int main(void)
{
    unsetenv("SNES_NET_SLOTS");
    memset(&g_join, 0, sizeof(g_join));
    memset(&g_caps, 0, sizeof(g_caps));

    /* Unchanged: 2-seat launches, and "unknown" (0) player_count. */
    CHECK(slots_for(2, 0, 0) == 2);
    CHECK(slots_for(2, 1, 0) == 2);
    CHECK(slots_for(0, 0, 0) == 2);
    CHECK(slots_for(1, 0, 0) == 2);

    /* 3+ seats on an ordinary (non-ICE) launch follow the room. */
    CHECK(slots_for(3, 0, 0) == 3);
    CHECK(slots_for(3, 1, 0) == 3);   /* legacy transport_host hub */
    CHECK(slots_for(4, 1, 0) == 4);
    CHECK(slots_for(SNES_NETPLAY_MAX_SLOTS, 0, 0) == SNES_NETPLAY_MAX_SLOTS);
    CHECK(slots_for(3, 0, 1) == 3);   /* spectator: seats = players only */

    /* Out of range stays at the default rather than being clamped silently. */
    CHECK(slots_for(SNES_NETPLAY_MAX_SLOTS + 1, 0, 0) == 2);
    CHECK(slots_for(-1, 0, 0) == 2);

    /* The operator override still outranks the room. */
    setenv("SNES_NET_SLOTS", "5", 1);
    CHECK(slots_for(3, 0, 0) == 5);
    CHECK(slots_for(2, 0, 0) == 5);
    unsetenv("SNES_NET_SLOTS");

    if (g_fail) { printf("snes_host_app_slots_test: %d FAILED\n", g_fail); return 1; }
    printf("snes_host_app_slots_test: ok\n");
    return 0;
}
