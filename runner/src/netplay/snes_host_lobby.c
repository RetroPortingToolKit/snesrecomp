/*
 * snes_host_lobby.c -- the SNES adapter over recomp-ui's shared netplay
 * backend (recomp_netplay_host.h).
 *
 * This file used to BE the backend (2,967 lines: create / join / list / chat
 * / seats / LAN / automatch / mod plan / fill_launch). That now lives in
 * recomp-ui's recomp_netplay_host.c, over recomp-net's lobby client, so the
 * next engine links it instead of copying it. What is left is what only the
 * SNES host knows:
 *
 *   - its identity and policies (SnesHostLobbyIdentity / SnesHostLobbyOpts,
 *     unchanged for the ports),
 *   - the widescreen settlement (the launcher's settings, then the game's
 *     fill_match_caps, in the SNES caps shape),
 *   - the mod runtime (snes_mod_runtime_*_c) as the backend's mod hooks,
 *   - the rollback engine's fork report,
 *   - where files beside the executable live, and the persisted name.
 *
 * Session slots stay SEAT-mapped (RECOMP_NETPLAY_SLOTS_SEAT): the SNES engine
 * does not read launch.slot_port[] yet, so mapping the host to session slot 0
 * would put a host who moved seats on the wrong controller port. The launch
 * still carries occupied_mask and an identity slot_port[].
 */
#include "snes_host_lobby.h"

#include <stdio.h>
#include <string.h>

#if !defined(RECOMP_LAUNCHER) && !defined(SNES_HOST_HAS_RECOMP_UI)

int snes_host_lobby_init(const SnesHostLobbyIdentity *id,
                         const SnesHostLobbyOpts *opts)
{
  (void)id;
  (void)opts;
  return -1;
}
void snes_host_lobby_shutdown(void) {}
void snes_host_lobby_prepare_rematch(void) {}
int snes_host_lobby_leave(void) { return -1; }
void snes_host_lobby_disconnect(void) {}
const char *snes_host_lobby_resume_endpoint(void) { return ""; }
int snes_host_lobby_in_lan(void) { return 0; }
void snes_host_lobby_set_runtime_error(const char *error_code)
{
  (void)error_code;
}

#else

#include "recomp_netplay_host.h"
#include "host_paths.h"
#include "snes_netplay_identity.h"
/* Fork detection lives in the rollback engine; this file only hands on what
 * it already found. The accessors answer "no fork" in a build without it. */
#include "snes_netplay_rb.h"
#if SNESRECOMP_ENABLE_MODS
#include "mod_runtime.h"
#endif

static SnesHostLobbyOpts g_opts;

/* ---- widescreen settlement ------------------------------------------------
 * The launcher's widescreen / HUD / aspect choices first, then the game's own
 * hook over the SNES-shaped caps -- the order the old default_caps used. */
static void snes_fill_caps(void *ctx, const RecompLauncherCSettings *settings,
                           RNetLobbyMatchCaps *caps)
{
  SnesLobbyMatchCaps snes;
  (void)ctx;
  snes_lobby_caps_from_rnet(caps, &snes);
  if (settings) {
    snes.widescreen = settings->widescreen != 0;
    snes.widescreen_hud = settings->widescreen_hud != 0;
    snes.ignore_aspect = settings->ignore_aspect != 0;
  }
  if (g_opts.fill_match_caps)
    g_opts.fill_match_caps(g_opts.caps_ctx, settings, &snes);
  snes_lobby_caps_to_rnet(&snes, caps);
}

static int snes_mods_enabled(void *ctx, const char *ruleset_id, char *why,
                             size_t why_cap)
{
  (void)ctx;
  return g_opts.mods_enabled
             ? g_opts.mods_enabled(g_opts.mods_ctx, ruleset_id, why, why_cap)
             : 0;
}

/* ---- host environment ------------------------------------------------------ */
static int snes_exe_dir_path(void *ctx, const char *leaf, char *out, size_t cap)
{
  (void)ctx;
  return snesrecomp_exe_dir_path(leaf, out, cap) ? 1 : 0;
}

static int snes_name_store(void *ctx, const char *name)
{
  (void)ctx;
  return snes_netplay_identity_store(name);
}

static int snes_name_load(void *ctx, char *out, size_t cap)
{
  (void)ctx;
  return snes_netplay_identity_load(out, cap);
}

static int snes_last_fork(void *ctx, uint32_t *tick, const char **partition,
                          uint32_t *mine, uint32_t *theirs)
{
  (void)ctx;
  if (!snes_netplay_rb_last_fork(tick, partition))
    return 0;
  snes_netplay_rb_fork_digests(mine, theirs);
  return 1;
}

/* ---- the mod runtime, as the backend's hooks ------------------------------ */
#if SNESRECOMP_ENABLE_MODS
static int rows_from_runtime(const SnesModPkgRow *rows, int n,
                             RNetLobbyModPkg *out, int max)
{
  int i;
  if (n > max) n = max;
  for (i = 0; i < n; ++i) {
    memset(&out[i], 0, sizeof(out[i]));
    snprintf(out[i].id, sizeof(out[i].id), "%s", rows[i].id);
    snprintf(out[i].ver, sizeof(out[i].ver), "%s", rows[i].version);
    snprintf(out[i].name, sizeof(out[i].name), "%s", rows[i].name);
    snprintf(out[i].feats, sizeof(out[i].feats), "%s", rows[i].features);
  }
  return n < 0 ? 0 : n;
}

static int m_plan_rows(void *ctx, RNetLobbyModPkg *out, int max)
{
  SnesModPkgRow rows[SNES_LOBBY_MAX_MODS];
  (void)ctx;
  return rows_from_runtime(rows,
                           snes_mod_runtime_plan_rows_c(rows, SNES_LOBBY_MAX_MODS),
                           out, max);
}

static int m_installed_rows(void *ctx, RNetLobbyModPkg *out, int max)
{
  SnesModPkgRow rows[SNES_LOBBY_MAX_MODS];
  (void)ctx;
  return rows_from_runtime(
      rows, snes_mod_runtime_installed_rows_c(rows, SNES_LOBBY_MAX_MODS), out,
      max);
}

static int m_effective_set(void *ctx, char *out, uint32_t cap)
{
  (void)ctx;
  return snes_mod_runtime_effective_set_c(out, cap);
}

static void m_set_cosmetic_allow(void *ctx, const char *allow)
{
  (void)ctx;
  snes_mod_runtime_set_cosmetic_allow_c(allow);
}

static int m_check_set(void *ctx, const char *want, char *reason, uint32_t cap)
{
  (void)ctx;
  /* SNES_MODSET_OK is 0, the backend's "already matches". */
  return snes_mod_runtime_check_set_c(want, reason, cap) == SNES_MODSET_OK
             ? 0 : 1;
}

static int m_adopt_set(void *ctx, const char *want, char *reason, uint32_t cap)
{
  (void)ctx;
  return snes_mod_runtime_adopt_set_c(want, reason, cap);
}

static int m_have_package(void *ctx, const char *id, const char *version,
                          char *name, uint32_t name_cap)
{
  (void)ctx;
  return snes_mod_runtime_have_package_c(id, version, name, name_cap);
}

static int m_unapproved(void *ctx, char *out, uint32_t cap)
{
  (void)ctx;
  return snes_mod_runtime_unapproved_cosmetics_c(out, cap);
}

static int m_exempted(void *ctx, char *out, uint32_t cap)
{
  (void)ctx;
  return snes_mod_runtime_exempted_packages_c(out, cap);
}

static int m_export(const char *id, const char *ver, uint8_t **out,
                    uint32_t *out_len, char *sha, uint32_t sha_cap, char *err,
                    uint32_t err_cap, void *ctx)
{
  (void)ctx;
  return snes_mod_runtime_export_package_c(id, ver, out, out_len, sha, sha_cap,
                                           err, err_cap);
}

static void m_free(uint8_t *blob)
{
  snes_mod_runtime_free_blob_c(blob);
}

static int m_install(const uint8_t *data, uint32_t len,
                     const char *expect_sha256, char *id, uint32_t id_cap,
                     char *ver, uint32_t ver_cap, char *err, uint32_t err_cap,
                     void *ctx)
{
  (void)ctx;
  return snes_mod_runtime_install_blob_c(data, len, expect_sha256, id, id_cap,
                                         ver, ver_cap, err, err_cap);
}

static const RecompNetplayModHooks g_snes_mods = {
  m_plan_rows, m_installed_rows, m_effective_set, m_set_cosmetic_allow,
  m_check_set, m_adopt_set, m_have_package, m_unapproved, m_exempted,
  m_export, m_free, m_install, NULL
};
#endif /* SNESRECOMP_ENABLE_MODS */

int snes_host_lobby_init(const SnesHostLobbyIdentity *id,
                         const SnesHostLobbyOpts *opts)
{
  RecompNetplayHostHooks h;
  if (!id || !id->game_name || !id->game_name[0])
    return -1;
  memset(&g_opts, 0, sizeof(g_opts));
  if (opts)
    g_opts = *opts;
  /* The SNES lobby configuration (SNES_NET_ alias, codec, fallback pin) goes
   * in before the backend sets the identity, so it can never overwrite it. */
  snes_lobby_configure();

  memset(&h, 0, sizeof(h));
  h.game_name = id->game_name;
  h.game_version = id->game_version;
  h.default_lobby_name = id->default_lobby_name;
  h.lan_registry_path = id->lan_registry_path;
  h.platform = "snes";
  h.legacy_env_prefix = "SNES_NET_";
  h.max_players = SNES_LOBBY_MAX_PLAYERS;
  h.slot_policy = RECOMP_NETPLAY_SLOTS_SEAT;
  h.input_player = 0;
  h.exe_dir_path = snes_exe_dir_path;
  h.name_store = snes_name_store;
  h.name_load = snes_name_load;
  h.caps_codec = snes_lobby_caps_codec();
  h.fill_match_caps = snes_fill_caps;
  h.mods_enabled = snes_mods_enabled;
  h.cosmetic_allow = g_opts.cosmetic_allow;
  h.last_fork = snes_last_fork;
  h.auto_ready_guests = g_opts.auto_ready_guests;
  h.rematch_set_ready = g_opts.rematch_set_ready;
#if SNESRECOMP_ENABLE_MODS
  h.mods = &g_snes_mods;
#endif
  return recomp_netplay_host_init(&h);
}

void snes_host_lobby_shutdown(void) { recomp_netplay_host_shutdown(); }

const RecompLauncherCNetplayCallbacks *snes_host_lobby_callbacks(void)
{
  return recomp_netplay_host_callbacks();
}

void snes_host_lobby_prepare_rematch(void)
{
  recomp_netplay_host_prepare_rematch();
}

int snes_host_lobby_leave(void) { return recomp_netplay_host_leave(); }

void snes_host_lobby_disconnect(void) { recomp_netplay_host_disconnect(); }

const char *snes_host_lobby_resume_endpoint(void)
{
  return recomp_netplay_host_resume_endpoint();
}

int snes_host_lobby_in_lan(void) { return recomp_netplay_host_in_lan(); }

void snes_host_lobby_set_runtime_error(const char *error_code)
{
  recomp_netplay_host_set_runtime_error(error_code);
}

int snes_host_lobby_auto_launch(const char *role, const char *player_name,
                                const char *lobby_name, unsigned timeout_ms,
                                RecompLauncherCNetplayLaunch *out)
{
  return recomp_netplay_host_auto_launch(role, player_name, lobby_name,
                                         timeout_ms, out);
}

#endif /* RECOMP_LAUNCHER || SNES_HOST_HAS_RECOMP_UI */
