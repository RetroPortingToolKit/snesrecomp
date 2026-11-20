#include "netplay/snes_netplay_region.h"

#include <stdio.h>
#include <string.h>

#define SNES_NETPLAY_REGION_FOLD_NTSC 0x85ebca6bu
#define SNES_NETPLAY_REGION_FOLD_PAL  0x9e3779b9u

uint32_t snes_netplay_region_fold_content_fp(uint32_t content_fp, int pal) {
  /* XOR by a per-region constant is a bijection for each region, so distinct
   * ROMs keep distinct fingerprints and the same ROM never collides across
   * regions. The fold is intentionally source-agnostic: overriding to the
   * region the header already selected is not a mismatch. */
  return content_fp ^ (pal ? SNES_NETPLAY_REGION_FOLD_PAL
                           : SNES_NETPLAY_REGION_FOLD_NTSC);
}

void snes_netplay_region_state_init(SnesNetplayRegionState *state) {
  if (!state)
    return;
  memset(state, 0, sizeof(*state));
  state->peer_pal = -1;
}

void snes_netplay_region_set_local(SnesNetplayRegionState *state, int pal,
                                   SnesRegionSource source,
                                   uint32_t content_fp) {
  if (!state)
    return;
  state->published = 1;
  state->pal = pal ? 1 : 0;
  state->source = source;
  state->content_fp = content_fp;
  state->folded_fp = snes_netplay_region_fold_content_fp(content_fp, pal);
  state->refused = 0;
  state->message[0] = '\0';
  /* A rematch re-resolves the region; a stale peer verdict must not survive. */
  state->peer_known = 0;
  state->peer_pal = -1;
  state->peer_folded_fp = 0;
}

uint32_t snes_netplay_region_folded_fp(const SnesNetplayRegionState *state) {
  return state ? state->folded_fp : 0;
}

void snes_netplay_region_set_peer(SnesNetplayRegionState *state,
                                  uint32_t peer_folded_fp,
                                  SnesRegionSource peer_source) {
  if (!state)
    return;
  state->peer_known = 1;
  state->peer_folded_fp = peer_folded_fp;
  state->peer_source = peer_source;
  state->peer_pal = -1;
  if (peer_folded_fp == snes_netplay_region_fold_content_fp(state->content_fp, 0))
    state->peer_pal = 0;
  else if (peer_folded_fp ==
           snes_netplay_region_fold_content_fp(state->content_fp, 1))
    state->peer_pal = 1;
}

int snes_netplay_region_check(SnesNetplayRegionState *state) {
  const char *peer_region;

  if (!state)
    return 1;
  /* The driver's own identity gate decides until the peer's fingerprint is
   * available. Publishing it is the only way to name the peer in the copy. */
  if (!state->peer_known || state->peer_folded_fp == state->folded_fp) {
    state->refused = 0;
    state->message[0] = '\0';
    return 1;
  }

  state->refused = 1;
  peer_region = state->peer_pal < 0 ? "an unknown region"
                                    : snes_region_name(state->peer_pal);
  snprintf(state->message, sizeof(state->message),
           "region_mismatch: this peer %s (from %s), peer %s (from %s); "
           "the same ROM with different guest timing cannot be a match, "
           "refused before the first frame",
           snes_region_name(state->pal),
           snes_region_source_name(state->source),
           peer_region, snes_region_source_name(state->peer_source));
  return 0;
}

int snes_netplay_region_refused(const SnesNetplayRegionState *state) {
  return state && state->refused;
}

const char *snes_netplay_region_reason(const SnesNetplayRegionState *state) {
  return snes_netplay_region_refused(state) ? "region_mismatch" : NULL;
}

const char *snes_netplay_region_message(const SnesNetplayRegionState *state) {
  return state ? state->message : "";
}

/* ── process-wide state used by the runner ─────────────────────────────── */

static SnesNetplayRegionState g_region;

void snes_netplay_region_publish_local(int pal, SnesRegionSource source,
                                       uint32_t content_fp) {
  snes_netplay_region_state_init(&g_region);
  snes_netplay_region_set_local(&g_region, pal, source, content_fp);
}

void snes_netplay_region_publish_peer(uint32_t peer_folded_fp,
                                      SnesRegionSource peer_source) {
  snes_netplay_region_set_peer(&g_region, peer_folded_fp, peer_source);
}

void snes_netplay_region_reset(void) {
  snes_netplay_region_state_init(&g_region);
}

void snes_netplay_region_reset_peer(void) {
  g_region.peer_known = 0;
  g_region.peer_pal = -1;
  g_region.peer_folded_fp = 0;
  g_region.refused = 0;
  g_region.message[0] = '\0';
}

uint32_t snes_netplay_region_identity_fp(uint32_t content_fp) {
  /* Called by snes_netplay_rb_set_identity. An unpublished region keeps the
   * original fingerprint, so older callers are unaffected. */
  if (!g_region.published)
    return content_fp;
  return snes_netplay_region_fold_content_fp(content_fp, g_region.pal);
}

int snes_netplay_region_check_agreement(void) {
  return snes_netplay_region_check(&g_region);
}

const char *snes_netplay_region_refusal(void) {
  return snes_netplay_region_reason(&g_region);
}

const char *snes_netplay_region_refusal_message(void) {
  return snes_netplay_region_message(&g_region);
}
