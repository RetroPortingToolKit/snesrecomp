/* Internal bookkeeping only: these observations never drive netplay policy. */
#ifndef SNES_NETPLAY_DIAGNOSTIC_STATE_H
#define SNES_NETPLAY_DIAGNOSTIC_STATE_H

#include <stdint.h>
#include <string.h>
#include "recomp_net/session.h"

enum { NP_SLOW_MS = 250, NP_REPORT_INTERVAL_MS = 5000 };
enum { NP_REPORT_GAP, NP_REPORT_DURATION, NP_REPORT_OPERATION, NP_REPORT_COUNT };

typedef struct NetplayDiagnosticState {
    uint64_t session_started_ms, pump_started_ms, pump_finished_ms;
    uint64_t gap_ms, max_gap_ms, last_slow_gap_ms;
    uint64_t duration_ms, max_duration_ms;
    uint64_t operation_ms, max_operation_ms;
    uint64_t reported_ms[NP_REPORT_COUNT];
    unsigned report_seen;
    int pump_seen, termination_logged;
} NetplayDiagnosticState;

static inline void np_diagnostic_reset(NetplayDiagnosticState *d, uint64_t now)
{
    memset(d, 0, sizeof(*d));
    d->session_started_ms = now;
}

static inline void np_diagnostic_pump_begin(NetplayDiagnosticState *d, uint64_t now)
{
    d->pump_started_ms = now;
    d->gap_ms = d->pump_seen && now >= d->pump_finished_ms
                    ? now - d->pump_finished_ms : 0;
    if (d->gap_ms > d->max_gap_ms) d->max_gap_ms = d->gap_ms;
    if (d->gap_ms >= NP_SLOW_MS) d->last_slow_gap_ms = d->gap_ms;
}

static inline void np_diagnostic_pump_end(NetplayDiagnosticState *d, uint64_t now)
{
    d->duration_ms = now >= d->pump_started_ms ? now - d->pump_started_ms : 0;
    if (d->duration_ms > d->max_duration_ms) d->max_duration_ms = d->duration_ms;
    d->pump_finished_ms = now;
    d->pump_seen = 1;
}

static inline int np_diagnostic_report(NetplayDiagnosticState *d, unsigned kind,
                                       uint64_t elapsed, uint64_t now)
{
    unsigned bit = 1u << kind;
    if (elapsed < NP_SLOW_MS) return 0;
    if ((d->report_seen & bit) && now >= d->reported_ms[kind] &&
        now - d->reported_ms[kind] < NP_REPORT_INTERVAL_MS) return 0;
    d->report_seen |= bit;
    d->reported_ms[kind] = now;
    return 1;
}

static inline const char *np_diagnostic_disconnect_reason(const RNetSessionStats *s)
{
    unsigned i;
    if (s->peer_gone) return "received_bye";
    if (s->packets_rx || s->last_peer_rx_age_ms) return "packet_silence";
    /* touch_peer_liveness may stamp liveness without a received packet. */
    for (i = 0; i < s->slot_count && i < RNET_MAX_SLOTS; ++i)
        if (i != s->local_slot && s->peer_rx_age_ms[i] != RNET_PEER_RX_NEVER)
            return "packet_silence";
    return "initial_link_budget";
}

#endif
