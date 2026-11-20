/* Same-ROM netplay region agreement at the first-admit decision.
 *
 * PAL and NTSC change the guest master clock and the number of scanlines per
 * field, so two peers that load the same ROM but resolve to different regions
 * cannot simulate alike. This fixture drives the real admission barrier for
 * both peers in one process: peer A resolves PAL from --region, peer B resolves
 * NTSC from its $FFD9 header. Both must refuse before any guest frame, with a
 * player-facing reason naming each side's region and its source. A control pair
 * that resolves the same region must still admit.
 *
 * The two peers are separate SnesNetplayRegionState objects: s_region_override
 * in snes_other.c is a process global, so sequential resolution in one process
 * would only ever describe one peer (see tests/region/region_header_test.c).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "netplay/snes_netplay_region.h"
/* The barrier uses no SDL functions; keep this unit independent of SDL linkage. */
#define SNESRECOMP_DESKTOP_SDL_COMPAT_H
#include "netplay/snes_host_app.c"

/* One content fingerprint: both peers load the same ROM image. */
#define SAME_ROM_FP 0x5a17c0deu

typedef struct Peer {
  SnesNetplayRegionState region;
  int active;
  int running;
  int polls;
  int admits;
  int frames;
  int exits;
  char lobby_error[512];
} Peer;

static Peer peers[2];
static Peer *current;

static const char *peer_name(const Peer *peer) {
  return peer == &peers[0] ? "A" : "B";
}

/* Netplay seams: the barrier sees only the current peer's state. */
int snes_netplay_active(void) { return current->active; }
int snes_netplay_rollback_active(void) { return 1; }
int snes_netplay_draining(void) { return 0; }
int snes_netplay_quiesced(void) { return 0; }
int snes_netplay_peer_disconnected(uint32_t timeout) {
  (void)timeout;
  return 0;
}
int snes_netplay_poll_admit(void) {
  int agreed;
  ++current->polls;
  agreed = snes_netplay_region_check(&current->region);
  if (agreed)
    ++current->admits;
  return agreed;
}
int snes_netplay_return_to_lobby_requested(void) {
  return snes_netplay_region_refused(&current->region);
}
const char *snes_netplay_refusal(void) {
  return snes_netplay_region_reason(&current->region);
}
uint32_t snes_netplay_sim_tick(void) { return 0; }
void snes_netplay_soft_exit_to_lobby(const char *origin, int from_lobby) {
  assert(current->active);
  assert(!strcmp(origin, "region_mismatch"));
  assert(from_lobby == 1);
  ++current->exits;
  current->active = 0;
  current->running = 0;
}
void snes_netplay_connect_wait_reset(void) {}
void snes_host_lobby_set_runtime_error(const char *why) {
  assert(!strcmp(why, "region_mismatch"));
  snprintf(current->lobby_error, sizeof(current->lobby_error), "%s",
           snes_netplay_region_message(&current->region));
}
int snes_netplay_remote_lead(void) { return 0; }
int snes_netplay_input_delay(void) { return 2; }
int snes_netplay_state_barrier(void) { return 0; }
int snes_netplay_is_running(void) { return 1; }
const char *snes_netplay_transport_name(void) { return "lan"; }
int snes_netplay_ice_failed(void) { return 0; }
int snes_netplay_connect_timed_out(uint32_t timeout) {
  (void)timeout;
  return 0;
}
int snes_netplay_input_desync(uint32_t *t, uint32_t *l, uint32_t *r) {
  (void)t; (void)l; (void)r;
  return 0;
}
void snes_netplay_stage_local(uint16_t buttons) { (void)buttons; }
void snes_netplay_pump(void) {}
static uint16_t capture(void *ctx) { (void)ctx; return 0; }

static void peer_init(Peer *peer, int pal, SnesNetplayRegionSource source) {
  memset(peer, 0, sizeof(*peer));
  snes_netplay_region_state_init(&peer->region);
  snes_netplay_region_set_local(&peer->region, pal, source, SAME_ROM_FP);
  peer->active = 1;
  peer->running = 1;
}

/* The identity exchange: each peer publishes its folded content fingerprint,
 * which is how the driver learns that the two machines disagree. */
static void exchange(Peer *a, Peer *b) {
  snes_netplay_region_set_peer(&a->region,
                               snes_netplay_region_folded_fp(&b->region),
                               b->region.source);
  snes_netplay_region_set_peer(&b->region,
                               snes_netplay_region_folded_fp(&a->region),
                               a->region.source);
}

/* One trip through the real barrier plus the one guest frame the host would
 * run on an admit. A refusal must leave both counters at zero. */
static void drive(Peer *peer) {
  SnesHostBarrierHooks hooks;
  int admitted;
  current = peer;
  memset(&hooks, 0, sizeof(hooks));
  hooks.capture_local_pad = &capture;
  admitted = snes_host_barrier_admit(1, &peer->running, &hooks);
  if (admitted)
    ++peer->frames;
}

static int contains(const char *haystack, const char *needle) {
  return haystack && strstr(haystack, needle) != NULL;
}

int main(void) {
  Peer *a = &peers[0];
  Peer *b = &peers[1];

  /* The identity fold is driven by the resolved region, never by its source:
   * overriding to the region the header already says is not a mismatch. */
  peer_init(a, 1, kSnesNetplayRegionSourceOverride);
  peer_init(b, 0, kSnesNetplayRegionSourceHeader);
  assert(snes_netplay_region_folded_fp(&a->region) !=
         snes_netplay_region_folded_fp(&b->region));
  exchange(a, b);
  assert(snes_netplay_region_check(&a->region) == 0);
  assert(snes_netplay_region_check(&b->region) == 0);

  drive(a);
  drive(b);
  assert(!strcmp(snes_netplay_region_reason(&a->region), "region_mismatch"));
  assert(!strcmp(snes_netplay_region_reason(&b->region), "region_mismatch"));
  assert(snes_netplay_region_refused(&a->region) &&
         snes_netplay_region_refused(&b->region));
  assert(!a->running && !b->running);
  assert(a->exits == 1 && b->exits == 1);
  assert(a->admits == 0 && b->admits == 0);
  assert(a->frames == 0 && b->frames == 0);
  assert(contains(a->lobby_error, "PAL (from override)"));
  assert(contains(a->lobby_error, "NTSC (from header)"));
  assert(contains(b->lobby_error, "PAL (from override)"));
  assert(contains(b->lobby_error, "NTSC (from header)"));
  printf("peer %s: %s\n", peer_name(a), a->lobby_error);
  printf("peer %s: %s\n", peer_name(b), b->lobby_error);

  /* Same ROM, same resolved region: the control pair still plays. */
  peer_init(a, 0, kSnesNetplayRegionSourceHeader);
  peer_init(b, 0, kSnesNetplayRegionSourceHeader);
  assert(snes_netplay_region_folded_fp(&a->region) ==
         snes_netplay_region_folded_fp(&b->region));
  exchange(a, b);
  assert(snes_netplay_region_check(&a->region) == 1);
  assert(snes_netplay_region_check(&b->region) == 1);
  drive(a);
  drive(b);
  assert(a->running && b->running);
  assert(a->admits == 1 && b->admits == 1);
  assert(a->frames == 1 && b->frames == 1);
  assert(!a->exits && !b->exits);
  assert(!a->lobby_error[0] && !b->lobby_error[0]);

  /* SNESRECOMP_REGION is a source too, and must be named in the copy. */
  peer_init(a, 1, kSnesNetplayRegionSourceEnv);
  peer_init(b, 0, kSnesNetplayRegionSourceHeader);
  exchange(a, b);
  assert(snes_netplay_region_check(&a->region) == 0);
  assert(contains(snes_netplay_region_message(&a->region),
                  "PAL (from SNESRECOMP_REGION)"));
  assert(contains(snes_netplay_region_message(&b->region),
                  "NTSC (from header)"));

  puts("region agreement: PASS");
  return 0;
}
