/* mod_provider_compose.c — see mod_provider_compose.h. */

#include "mod_provider_compose.h"

/* The provider type is recomp-ui's; a mods build without the launcher has
 * nothing to compose. */
#if defined(RECOMP_LAUNCHER)

#include <string.h>

#include "recomp_launcher.h"

typedef const RecompLauncherCModProvider Provider;

static Provider *g_first, *g_second;
static Provider *g_failed;   /* provider whose mutation/commit failed last */
static RecompLauncherCModProvider g_composed;

static int PackageCount(Provider *p) {
  return p && p->package_count ? p->package_count(p->ctx) : 0;
}
static int FeatureCount(Provider *p) {
  return p && p->feature_count ? p->feature_count(p->ctx) : 0;
}
static int CatalogDiagnosticCount(Provider *p) {
  return p && p->catalog_diagnostic_count ? p->catalog_diagnostic_count(p->ctx) : 0;
}

/* A provider lists a package through either surface; a feature-only provider
 * may leave the package callbacks NULL. */
static int Lists(Provider *p, const char *package_id) {
  if (!p || !package_id) return 0;
  for (int i = 0, n = PackageCount(p); i < n; ++i) {
    RecompLauncherCModPackage pkg;
    memset(&pkg, 0, sizeof(pkg));
    if (p->package_get(p->ctx, i, &pkg) && !strcmp(pkg.id, package_id)) return 1;
  }
  for (int i = 0, n = FeatureCount(p); i < n; ++i) {
    RecompLauncherCModFeature feature;
    memset(&feature, 0, sizeof(feature));
    if (p->feature_get(p->ctx, i, &feature) && !strcmp(feature.package_id, package_id))
      return 1;
  }
  return 0;
}

static Provider *Owner(const char *package_id) {
  if (Lists(g_first, package_id)) return g_first;
  if (Lists(g_second, package_id)) return g_second;
  return NULL;
}

static int Track(Provider *p, int ok) {
  if (!ok) g_failed = p;
  return ok;
}

/* ── package surface ─────────────────────────────────────────────────── */

static int CPackageCount(void *ctx) {
  (void)ctx;
  return PackageCount(g_first) + PackageCount(g_second);
}
static int CPackageGet(void *ctx, int index, RecompLauncherCModPackage *out) {
  (void)ctx;
  int n = PackageCount(g_first);
  if (index < 0) return 0;
  if (index < n) return g_first->package_get(g_first->ctx, index, out);
  if (index - n < PackageCount(g_second))
    return g_second->package_get(g_second->ctx, index - n, out);
  return 0;
}
static int COptionGet(void *ctx, const char *package_id, int index,
                      RecompLauncherCModOption *out) {
  (void)ctx;
  Provider *p = Owner(package_id);
  return p && p->option_get ? p->option_get(p->ctx, package_id, index, out) : 0;
}
static int CChoiceGet(void *ctx, const char *package_id, const char *option_id,
                      int index, RecompLauncherCModChoice *out) {
  (void)ctx;
  Provider *p = Owner(package_id);
  return p && p->choice_get ? p->choice_get(p->ctx, package_id, option_id, index, out) : 0;
}
static int CVersionCount(void *ctx, const char *package_id) {
  (void)ctx;
  Provider *p = Owner(package_id);
  return p && p->version_count ? p->version_count(p->ctx, package_id) : 0;
}
static int CVersionGet(void *ctx, const char *package_id, int index,
                       RecompLauncherCModVersion *out) {
  (void)ctx;
  Provider *p = Owner(package_id);
  return p && p->version_get ? p->version_get(p->ctx, package_id, index, out) : 0;
}
static int CInstallArchive(void *ctx, const char *archive_path) {
  (void)ctx;
  Provider *p = g_second && g_second->install_archive ? g_second : g_first;
  if (!p || !p->install_archive) return 0;
  return Track(p, p->install_archive(p->ctx, archive_path));
}
static int CRemovePackage(void *ctx, const char *package_id, const char *version) {
  (void)ctx;
  Provider *p = Owner(package_id);
  if (!p || !p->remove_package) return 0;
  return Track(p, p->remove_package(p->ctx, package_id, version));
}
static int CSetEnabled(void *ctx, const char *package_id, int enabled) {
  (void)ctx;
  Provider *p = Owner(package_id);
  if (!p || !p->set_enabled) return 0;
  return Track(p, p->set_enabled(p->ctx, package_id, enabled));
}
static int CSelectVersion(void *ctx, const char *package_id, const char *version) {
  (void)ctx;
  Provider *p = Owner(package_id);
  if (!p || !p->select_version) return 0;
  return Track(p, p->select_version(p->ctx, package_id, version));
}
static int CSetOption(void *ctx, const char *package_id, const char *option_id,
                      const char *value) {
  (void)ctx;
  Provider *p = Owner(package_id);
  if (!p || !p->set_option) return 0;
  return Track(p, p->set_option(p->ctx, package_id, option_id, value));
}

/* Both always run: a failure in one must not leave the other's staged
 * selection unpersisted. */
static int CCommit(void *ctx, const char *image_path) {
  (void)ctx;
  int ok = 1;
  if (g_first && g_first->commit && !Track(g_first, g_first->commit(g_first->ctx, image_path)))
    ok = 0;
  if (g_second && g_second->commit && !Track(g_second, g_second->commit(g_second->ctx, image_path)))
    ok = 0;
  return ok;
}
static int CCommitNetplay(void *ctx, const char *image_path) {
  (void)ctx;
  int ok = 1;
  if (g_first && g_first->commit_netplay &&
      !Track(g_first, g_first->commit_netplay(g_first->ctx, image_path)))
    ok = 0;
  if (g_second && g_second->commit_netplay &&
      !Track(g_second, g_second->commit_netplay(g_second->ctx, image_path)))
    ok = 0;
  return ok;
}
static const char *CLastError(void *ctx) {
  (void)ctx;
  Provider *p = g_failed;
  const char *error = p && p->last_error ? p->last_error(p->ctx) : NULL;
  return error ? error : "";
}

/* ── feature surface ─────────────────────────────────────────────────── */

static int CFeatureCount(void *ctx) {
  (void)ctx;
  return FeatureCount(g_first) + FeatureCount(g_second);
}
static int CFeatureGet(void *ctx, int index, RecompLauncherCModFeature *out) {
  (void)ctx;
  int n = FeatureCount(g_first);
  if (index < 0) return 0;
  if (index < n) return g_first->feature_get(g_first->ctx, index, out);
  if (index - n < FeatureCount(g_second))
    return g_second->feature_get(g_second->ctx, index - n, out);
  return 0;
}
static int CFeatureOptionGet(void *ctx, const char *package_id, const char *feature_id,
                             int index, RecompLauncherCModOption *out) {
  (void)ctx;
  Provider *p = Owner(package_id);
  return p && p->feature_option_get
      ? p->feature_option_get(p->ctx, package_id, feature_id, index, out) : 0;
}
static int CFeatureChoiceGet(void *ctx, const char *package_id, const char *feature_id,
                             const char *option_id, int index, RecompLauncherCModChoice *out) {
  (void)ctx;
  Provider *p = Owner(package_id);
  return p && p->feature_choice_get
      ? p->feature_choice_get(p->ctx, package_id, feature_id, option_id, index, out) : 0;
}
static int CFeatureEnable(void *ctx, const char *package_id, const char *feature_id,
                          int enabled) {
  (void)ctx;
  Provider *p = Owner(package_id);
  if (!p || !p->feature_enable) return 0;
  return Track(p, p->feature_enable(p->ctx, package_id, feature_id, enabled));
}
static int CFeatureSetOption(void *ctx, const char *package_id, const char *feature_id,
                             const char *option_id, const char *value) {
  (void)ctx;
  Provider *p = Owner(package_id);
  if (!p || !p->feature_set_option) return 0;
  return Track(p, p->feature_set_option(p->ctx, package_id, feature_id, option_id, value));
}
static int CDiagnosticCount(void *ctx, const char *package_id, const char *feature_id) {
  (void)ctx;
  Provider *p = Owner(package_id);
  return p && p->diagnostic_count ? p->diagnostic_count(p->ctx, package_id, feature_id) : 0;
}
static int CDiagnosticGet(void *ctx, const char *package_id, const char *feature_id,
                          int index, RecompLauncherCModDiagnostic *out) {
  (void)ctx;
  Provider *p = Owner(package_id);
  return p && p->diagnostic_get
      ? p->diagnostic_get(p->ctx, package_id, feature_id, index, out) : 0;
}
static int CResourceCount(void *ctx, const char *package_id, const char *feature_id) {
  (void)ctx;
  Provider *p = Owner(package_id);
  return p && p->feature_resource_count
      ? p->feature_resource_count(p->ctx, package_id, feature_id) : 0;
}
static int CResourceGet(void *ctx, const char *package_id, const char *feature_id,
                        int index, RecompLauncherCModResource *out) {
  (void)ctx;
  Provider *p = Owner(package_id);
  return p && p->feature_resource_get
      ? p->feature_resource_get(p->ctx, package_id, feature_id, index, out) : 0;
}
static int CResourceSetPath(void *ctx, const char *package_id, const char *feature_id,
                            const char *resource_id, const char *path) {
  (void)ctx;
  Provider *p = Owner(package_id);
  if (!p || !p->feature_resource_set_path) return 0;
  return Track(p, p->feature_resource_set_path(p->ctx, package_id, feature_id,
                                               resource_id, path));
}
static int CCatalogDiagnosticCount(void *ctx) {
  (void)ctx;
  return CatalogDiagnosticCount(g_first) + CatalogDiagnosticCount(g_second);
}
static int CCatalogDiagnosticGet(void *ctx, int index, RecompLauncherCModDiagnostic *out) {
  (void)ctx;
  int n = CatalogDiagnosticCount(g_first);
  if (index < 0) return 0;
  if (index < n) return g_first->catalog_diagnostic_get(g_first->ctx, index, out);
  if (index - n < CatalogDiagnosticCount(g_second))
    return g_second->catalog_diagnostic_get(g_second->ctx, index - n, out);
  return 0;
}

const RecompLauncherCModProvider *snes_mod_provider_compose(Provider *first,
                                                            Provider *second) {
  if (!first) return second;
  if (!second) return first;
  g_first = first;
  g_second = second;
  g_failed = NULL;
  memset(&g_composed, 0, sizeof(g_composed));
  g_composed.package_count = CPackageCount;
  g_composed.package_get = CPackageGet;
  g_composed.option_get = COptionGet;
  g_composed.choice_get = CChoiceGet;
  g_composed.version_count = CVersionCount;
  g_composed.version_get = CVersionGet;
  g_composed.install_archive = CInstallArchive;
  g_composed.remove_package = CRemovePackage;
  g_composed.set_enabled = CSetEnabled;
  g_composed.select_version = CSelectVersion;
  g_composed.set_option = CSetOption;
  g_composed.commit = CCommit;
  g_composed.last_error = CLastError;
  g_composed.feature_count = CFeatureCount;
  g_composed.feature_get = CFeatureGet;
  g_composed.feature_option_get = CFeatureOptionGet;
  g_composed.feature_choice_get = CFeatureChoiceGet;
  g_composed.feature_enable = CFeatureEnable;
  g_composed.feature_set_option = CFeatureSetOption;
  g_composed.diagnostic_count = CDiagnosticCount;
  g_composed.diagnostic_get = CDiagnosticGet;
  g_composed.archive_extension =
      second->archive_extension ? second->archive_extension : first->archive_extension;
  g_composed.archive_description =
      second->archive_description ? second->archive_description : first->archive_description;
  /* NULL means "skip commit for a netplay launch"; keep that when neither
   * provider has one. */
  if (first->commit_netplay || second->commit_netplay)
    g_composed.commit_netplay = CCommitNetplay;
  g_composed.feature_resource_count = CResourceCount;
  g_composed.feature_resource_get = CResourceGet;
  g_composed.feature_resource_set_path = CResourceSetPath;
  g_composed.catalog_diagnostic_count = CCatalogDiagnosticCount;
  g_composed.catalog_diagnostic_get = CCatalogDiagnosticGet;
  return &g_composed;
}

#endif  /* RECOMP_LAUNCHER */
