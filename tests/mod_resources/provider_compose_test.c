/* Standalone, ROM-free test of snes_mod_provider_compose. Compile with
 * -DRECOMP_LAUNCHER and recomp-ui's src/ on the include path.
 *
 * Two fake providers: "title" lists one feature through the feature surface
 * only; "runtime" lists two packages and a catalog diagnostic. The composite
 * must show all of them, route every package-named call to the owner, run
 * both commits, and report the failing provider's error. */
#include "mod_provider_compose.h"
#include "recomp_launcher.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fake {
  const char *name;
  const char *packages[2];
  int package_count;
  int commits, commit_ok, enables, options, installs;
  char last_enabled[64];
} Fake;

static Fake g_title = {"title", {"title.video"}, 1, 0, 1, 0, 0, 0, ""};
static Fake g_runtime = {"runtime", {"pkg.respin", "pkg.doors"}, 2, 0, 1, 0, 0, 0, ""};

static int PackageCount(void *ctx) { return ((Fake *)ctx)->package_count; }
static int PackageGet(void *ctx, int i, RecompLauncherCModPackage *out) {
  Fake *f = ctx;
  if (i < 0 || i >= f->package_count) return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->id, sizeof(out->id), "%s", f->packages[i]);
  return 1;
}
static int FeatureCount(void *ctx) { return ((Fake *)ctx)->package_count; }
static int FeatureGet(void *ctx, int i, RecompLauncherCModFeature *out) {
  Fake *f = ctx;
  if (i < 0 || i >= f->package_count) return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->package_id, sizeof(out->package_id), "%s", f->packages[i]);
  snprintf(out->id, sizeof(out->id), "main");
  return 1;
}
static int FeatureEnable(void *ctx, const char *package, const char *feature, int on) {
  Fake *f = ctx;
  (void)feature; (void)on;
  f->enables++;
  snprintf(f->last_enabled, sizeof(f->last_enabled), "%s", package);
  return 1;
}
static int FeatureSetOption(void *ctx, const char *package, const char *feature,
                            const char *option, const char *value) {
  (void)package; (void)feature; (void)option; (void)value;
  ((Fake *)ctx)->options++;
  return 1;
}
static int Commit(void *ctx, const char *image) {
  Fake *f = ctx;
  (void)image;
  f->commits++;
  return f->commit_ok;
}
static const char *LastError(void *ctx) { return ((Fake *)ctx)->name; }
static int Install(void *ctx, const char *path) {
  (void)path;
  ((Fake *)ctx)->installs++;
  return 1;
}
static int CatalogCount(void *ctx) { return ctx == &g_runtime ? 1 : 0; }
static int CatalogGet(void *ctx, int i, RecompLauncherCModDiagnostic *out) {
  (void)ctx;
  if (i != 0) return 0;
  memset(out, 0, sizeof(*out));
  return 1;
}

int main(void) {
  RecompLauncherCModProvider title, runtime;
  memset(&title, 0, sizeof(title));
  memset(&runtime, 0, sizeof(runtime));
  /* The title provider is feature-only, like Super Metroid's. */
  title.ctx = &g_title;
  title.feature_count = FeatureCount;
  title.feature_get = FeatureGet;
  title.feature_enable = FeatureEnable;
  title.feature_set_option = FeatureSetOption;
  title.commit = Commit;
  title.last_error = LastError;
  title.archive_extension = ".title";
  runtime.ctx = &g_runtime;
  runtime.package_count = PackageCount;
  runtime.package_get = PackageGet;
  runtime.feature_count = FeatureCount;
  runtime.feature_get = FeatureGet;
  runtime.feature_enable = FeatureEnable;
  runtime.feature_set_option = FeatureSetOption;
  runtime.commit = Commit;
  runtime.last_error = LastError;
  runtime.install_archive = Install;
  runtime.archive_extension = ".snesmod";
  runtime.catalog_diagnostic_count = CatalogCount;
  runtime.catalog_diagnostic_get = CatalogGet;

  assert(snes_mod_provider_compose(NULL, &runtime) == &runtime);
  assert(snes_mod_provider_compose(&title, NULL) == &title);

  const RecompLauncherCModProvider *p = snes_mod_provider_compose(&title, &runtime);
  assert(p != &title && p != &runtime);
  assert(p->feature_count(p->ctx) == 3);
  assert(p->package_count(p->ctx) == 2);
  RecompLauncherCModFeature feature;
  assert(p->feature_get(p->ctx, 0, &feature) && !strcmp(feature.package_id, "title.video"));
  assert(p->feature_get(p->ctx, 2, &feature) && !strcmp(feature.package_id, "pkg.doors"));
  assert(!p->feature_get(p->ctx, 3, &feature));
  assert(!strcmp(p->archive_extension, ".snesmod"));
  assert(p->catalog_diagnostic_count(p->ctx) == 1);
  assert(p->commit_netplay == NULL);

  /* Routing by owner, including a package only the feature surface lists. */
  assert(p->feature_enable(p->ctx, "title.video", "main", 1));
  assert(g_title.enables == 1 && g_runtime.enables == 0);
  assert(p->feature_enable(p->ctx, "pkg.respin", "main", 1));
  assert(g_runtime.enables == 1 && !strcmp(g_runtime.last_enabled, "pkg.respin"));
  assert(p->feature_set_option(p->ctx, "pkg.doors", "main", "speed", "2x"));
  assert(g_runtime.options == 1 && g_title.options == 0);
  assert(!p->feature_enable(p->ctx, "pkg.unknown", "main", 1));
  assert(p->install_archive(p->ctx, "x.snesmod") && g_runtime.installs == 1);

  /* Both commit; a failure is attributed to its provider. */
  assert(p->commit(p->ctx, "rom.sfc"));
  assert(g_title.commits == 1 && g_runtime.commits == 1);
  g_title.commit_ok = 0;
  assert(!p->commit(p->ctx, "rom.sfc"));
  assert(g_title.commits == 2 && g_runtime.commits == 2);
  assert(!strcmp(p->last_error(p->ctx), "title"));

  puts("provider_compose_test: PASS");
  return 0;
}
