/* Real barrier ordering with a recovered provisional timeout, plus deterministic
 * bookkeeping for stalls that recover before the next liveness decision. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "netplay/snes_netplay_diagnostic_state.h"
/* The barrier uses no SDL functions; keep this unit independent of SDL linkage. */
#define SNESRECOMP_DESKTOP_SDL_COMPAT_H
#include "netplay/snes_host_app.c"

static int active, disconnected, recover_on_poll, polls, reports, exits;
static uint32_t reported_timeout;
int snes_netplay_active(void) { return active; }
int snes_netplay_rollback_active(void) { return 1; }
int snes_netplay_draining(void) { return 0; }
int snes_netplay_quiesced(void) { return 0; }
int snes_netplay_peer_disconnected(uint32_t timeout)
{ assert(timeout == 1500); return disconnected; }
int snes_netplay_poll_admit(void)
{ ++polls; if (recover_on_poll) disconnected = 0; return !disconnected; }
void snes_netplay_log_peer_disconnect(uint32_t timeout)
{ assert(active && disconnected && polls == 1); ++reports; reported_timeout = timeout; }
void snes_netplay_soft_exit_to_lobby(const char *origin, int from_lobby)
{ assert(!strcmp(origin, "peer_disconnect") && !from_lobby && reports == 1 && active); ++exits; active = 0; }
void snes_netplay_connect_wait_reset(void) {}
int snes_netplay_return_to_lobby_requested(void) { return 0; }
const char *snes_netplay_refusal(void) { return NULL; }
uint32_t snes_netplay_sim_tick(void) { return 300; }
void snes_host_lobby_set_runtime_error(const char *why) { (void)why; }
int snes_netplay_remote_lead(void) { return 0; }
int snes_netplay_input_delay(void) { return 4; }
int snes_netplay_state_barrier(void) { return 0; }
int snes_netplay_is_running(void) { return 1; }
const char *snes_netplay_transport_name(void) { return "lan"; }
int snes_netplay_ice_failed(void) { return 0; }
int snes_netplay_connect_timed_out(uint32_t timeout) { (void)timeout; return 0; }
int snes_netplay_input_desync(uint32_t *t, uint32_t *l, uint32_t *r)
{ (void)t; (void)l; (void)r; return 0; }
void snes_netplay_stage_local(uint16_t buttons) { (void)buttons; }
void snes_netplay_pump(void) {}
static uint16_t capture(void *ctx) { (void)ctx; return 0; }

static void reason_cases(void)
{
    RNetSessionStats s = {0};
    unsigned i;
    s.slot_count = 2;
    for (i = 0; i < RNET_MAX_SLOTS; ++i) s.peer_rx_age_ms[i] = RNET_PEER_RX_NEVER;
    assert(!strcmp(np_diagnostic_disconnect_reason(&s), "initial_link_budget"));
    s.peer_rx_age_ms[1] = 0; /* A liveness touch, even without decoded traffic. */
    assert(!strcmp(np_diagnostic_disconnect_reason(&s), "packet_silence"));
    s.peer_rx_age_ms[1] = RNET_PEER_RX_NEVER;
    s.packets_rx = 1;
    s.last_peer_rx_age_ms = 1500;
    assert(!strcmp(np_diagnostic_disconnect_reason(&s), "packet_silence"));
    s.peer_gone = 1;
    s.last_peer_rx_age_ms = 0;
    assert(!strcmp(np_diagnostic_disconnect_reason(&s), "received_bye"));
}

static void timing_cases(void)
{
    NetplayDiagnosticState d;
    np_diagnostic_reset(&d, 0);
    np_diagnostic_pump_begin(&d, 20000); /* Boot time is not an inter-pump gap. */
    assert(d.gap_ms == 0);
    np_diagnostic_pump_end(&d, 20010);
    np_diagnostic_pump_begin(&d, 21610);
    assert(d.gap_ms == 1600 && d.max_gap_ms == 1600 && d.last_slow_gap_ms == 1600);
    assert(np_diagnostic_report(&d, NP_REPORT_GAP, d.gap_ms, 21610));
    np_diagnostic_pump_end(&d, 21910); /* Gap excludes time spent pumping. */
    assert(d.duration_ms == 300 && d.max_duration_ms == 300);
    assert(np_diagnostic_report(&d, NP_REPORT_DURATION, d.duration_ms, 21910));
    np_diagnostic_pump_begin(&d, 21911); /* Recovered stall retains evidence. */
    assert(d.gap_ms == 1 && d.max_gap_ms == 1600 && d.last_slow_gap_ms == 1600);
    assert(!np_diagnostic_report(&d, NP_REPORT_GAP, 1600, 26609));
    assert(np_diagnostic_report(&d, NP_REPORT_GAP, 1600, 26610));
    assert(!np_diagnostic_report(&d, NP_REPORT_OPERATION, 249, 0));
    assert(np_diagnostic_report(&d, NP_REPORT_OPERATION, 250, 0));
    assert(!np_diagnostic_report(&d, NP_REPORT_OPERATION, 250, 1));
    d.termination_logged = 1;
    np_diagnostic_reset(&d, 30000); /* Rematch cannot inherit maxima/rate gates. */
    assert(!d.max_gap_ms && !d.max_duration_ms && !d.last_slow_gap_ms && !d.termination_logged);
    assert(np_diagnostic_report(&d, NP_REPORT_GAP, 250, 30000));
    np_diagnostic_pump_begin(&d, 30001);
    assert(!d.gap_ms);
}

int main(void)
{
    SnesHostBarrierHooks hooks = {0};
    int running = 1;
    hooks.capture_local_pad = capture;
    reason_cases();
    timing_cases();
    active = disconnected = recover_on_poll = 1;
    assert(snes_host_barrier_admit(0, &running, &hooks) == 1);
    assert(running && active && polls == 2 && !reports && !exits);
    polls = 0;
    recover_on_poll = 0;
    disconnected = 1;
    assert(snes_host_barrier_admit(0, &running, &hooks) == 0);
    assert(!running && !active && reports == 1 && exits == 1 && reported_timeout == 1500);
    assert(snes_host_barrier_admit(0, &running, &hooks) == 0 && reports == 1);
    puts("disconnect diagnostics: PASS");
    return 0;
}
