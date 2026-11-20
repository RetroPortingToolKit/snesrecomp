#ifndef SNESRECOMP_SNES_NETPLAY_REGION_H
#define SNESRECOMP_SNES_NETPLAY_REGION_H

/*
 * Netplay match identity for the console region.
 *
 * NTSC and PAL run the guest at different master clocks and with different
 * field geometry (262 vs 312 lines). Two peers that load the same ROM can
 * therefore simulate differently if one has an explicit region override and
 * the other resolves from its $FFD9 header, while presenting the same ROM
 * identity to the existing content-fingerprint gate.
 *
 * Policy:
 *   - The resolved region is part of the match compatibility identity.
 *   - With auto, each peer resolves from the header (or SNESRECOMP_REGION).
 *   - An explicit --region / snes_set_region_override is per-peer and must
 *     resolve to the same region on every peer.
 *   - A mismatch is a hard refusal before frame 0, never a silent correction.
 *     Both peers report reason "region_mismatch" with a player-facing message
 *     naming each side's region and its source, then soft-return to the lobby.
 *
 * Wire stopgap: the region is folded into the content fingerprint passed to
 * rnet_rb_driver_set_identity, so the existing identity gate compares it too.
 * A dedicated recomp-net field would be cleaner and is pending reviewer
 * preference; see docs/REGION_PACED_TIMING.md.
 */

#include <stddef.h>
#include <stdint.h>

#include "snes/snes_region.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNES_NETPLAY_REGION_MESSAGE_MAX 256

typedef struct SnesNetplayRegionState {
  int published;                   /* set_local has run for this session */
  int pal;                         /* resolved region: 0 NTSC, 1 PAL */
  SnesRegionSource source;         /* where this peer resolved it from */
  uint32_t content_fp;             /* ROM content fingerprint, before folding */
  uint32_t folded_fp;              /* what the identity gate compares */
  int peer_known;                  /* peer's folded fingerprint published */
  int peer_pal;                    /* -1 when it cannot be decoded */
  SnesRegionSource peer_source;    /* informational only */
  uint32_t peer_folded_fp;
  int refused;
  char message[SNES_NETPLAY_REGION_MESSAGE_MAX];
} SnesNetplayRegionState;

/* Bijective fold: the same region always produces the same fingerprint and a
 * different region always produces a different one for the same ROM. */
uint32_t snes_netplay_region_fold_content_fp(uint32_t content_fp, int pal);

void snes_netplay_region_state_init(SnesNetplayRegionState *state);
void snes_netplay_region_set_local(SnesNetplayRegionState *state, int pal,
                                   SnesRegionSource source,
                                   uint32_t content_fp);
uint32_t snes_netplay_region_folded_fp(const SnesNetplayRegionState *state);
/* Publish the peer's fingerprint from the identity exchange. The source is
 * carried out of band in this stopgap; the region is decoded by comparing the
 * peer's folded fingerprint with both folds of our own ROM fingerprint. */
void snes_netplay_region_set_peer(SnesNetplayRegionState *state,
                                  uint32_t peer_folded_fp,
                                  SnesRegionSource peer_source);

/* The single pre-admit gate. 1 = all published peers agree (or the peer has
 * not published yet, in which case the driver's identity gate decides);
 * 0 = mismatch, with the refusal and player-facing message recorded. */
int snes_netplay_region_check(SnesNetplayRegionState *state);
int snes_netplay_region_refused(const SnesNetplayRegionState *state);
const char *snes_netplay_region_reason(const SnesNetplayRegionState *state);
const char *snes_netplay_region_message(const SnesNetplayRegionState *state);

/* Process-wide state for the runner: published after SnesInit, before start. */
void snes_netplay_region_publish_local(int pal, SnesRegionSource source,
                                       uint32_t content_fp);
void snes_netplay_region_publish_peer(uint32_t peer_folded_fp,
                                      SnesRegionSource peer_source);
void snes_netplay_region_reset(void);
/* Rematch: forget the peer's verdict but keep this process's resolution. */
void snes_netplay_region_reset_peer(void);
uint32_t snes_netplay_region_identity_fp(uint32_t content_fp);
int snes_netplay_region_check_agreement(void);
const char *snes_netplay_region_refusal(void);
const char *snes_netplay_region_refusal_message(void);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_SNES_NETPLAY_REGION_H */
