#ifndef SNESRECOMP_TEST_STUB_RECOMP_NET_SESSION_H
#define SNESRECOMP_TEST_STUB_RECOMP_NET_SESSION_H
/* See lobby_client.h in this directory: test-only stand-in for the
 * recomp-net submodule, used when it is not checked out. */
#include <stdint.h>

#define RNET_MAX_SLOTS 8u
#define RNET_PEER_RX_NEVER 0xFFFFFFFFu

typedef struct RNetSessionStats {
  uint32_t slot_count;
  uint32_t local_slot;
  int peer_gone;
  uint32_t packets_rx;
  uint32_t last_peer_rx_age_ms;
  uint32_t peer_rx_age_ms[RNET_MAX_SLOTS];
} RNetSessionStats;

#endif
