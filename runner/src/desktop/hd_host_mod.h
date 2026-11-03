#pragma once
/* Included once by host_main.c. Preserve the parent context and every unrelated
 * callback, notably netplay commit, resource preparation and preset behavior. */
#include "hd_mod.h"
static RecompLauncherCModProvider g_hd_parent, g_hd_provider;
static int HdPackageCount(void *c) { return (g_hd_parent.package_count ? g_hd_parent.package_count(c) : 0) + 1; }
static int HdFeatureCount(void *c) { return (g_hd_parent.feature_count ? g_hd_parent.feature_count(c) : 0) + 1; }
static int HdPackageGet(void *c, int i, RecompLauncherCModPackage *o) {
  if (i == HdPackageCount(c) - 1) return SnesHdModPackage(g_config.mode7.enabled, o);
  return g_hd_parent.package_get ? g_hd_parent.package_get(c, i, o) : 0;
}
static int HdFeatureGet(void *c, int i, RecompLauncherCModFeature *o) {
  if (i == HdFeatureCount(c) - 1) return SnesHdModFeature(g_config.mode7.enabled, o);
  return g_hd_parent.feature_get ? g_hd_parent.feature_get(c, i, o) : 0;
}
static int HdOption(void *c, const char *p, const char *f, int i, RecompLauncherCModOption *o) {
  if (SnesHdModIdentity(p, f)) return SnesHdModOption(g_config.mode7.scale, i, o);
  return g_hd_parent.feature_option_get ? g_hd_parent.feature_option_get(c, p, f, i, o) : 0;
}
static int HdEnable(void *c, const char *p, const char *f, int e) {
  if (!SnesHdModIdentity(p, f)) return g_hd_parent.feature_enable ? g_hd_parent.feature_enable(c, p, f, e) : 0;
  g_config.mode7.enabled = e != 0; g_config_dirty = true; return 1;
}
static int HdSetOption(void *c, const char *p, const char *f, const char *o, const char *v) {
  if (!SnesHdModIdentity(p, f)) return g_hd_parent.feature_set_option ? g_hd_parent.feature_set_option(c, p, f, o, v) : 0;
  if (!o || strcmp(o, "scale") || !SnesHdParseScale(v, &g_config.mode7.scale)) return 0;
  g_config_dirty = true; return 1;
}
static int HdLegacyOption(void *c, const char *p, int i, RecompLauncherCModOption *o) {
  if (SnesHdModIdentity(p, NULL)) return SnesHdModOption(g_config.mode7.scale, i, o);
  return g_hd_parent.option_get ? g_hd_parent.option_get(c, p, i, o) : 0;
}
static int HdLegacyEnable(void *c, const char *p, int e) {
  if (SnesHdModIdentity(p, NULL)) return HdEnable(c, p, SNES_HD_FEATURE, e);
  return g_hd_parent.set_enabled ? g_hd_parent.set_enabled(c, p, e) : 0;
}
static int HdLegacySetOption(void *c, const char *p, const char *o, const char *v) {
  if (SnesHdModIdentity(p, NULL)) return HdSetOption(c, p, SNES_HD_FEATURE, o, v);
  return g_hd_parent.set_option ? g_hd_parent.set_option(c, p, o, v) : 0;
}
static int HdCommit(void *c, const char *image) {
  return g_hd_parent.commit ? g_hd_parent.commit(c, image) : 1;
}
static const RecompLauncherCModProvider *HdHostProvider(const RecompLauncherCModProvider *parent) {
  g_hd_parent = parent ? *parent : (RecompLauncherCModProvider){0};
  g_hd_provider = g_hd_parent;
  g_hd_provider.package_count = HdPackageCount; g_hd_provider.package_get = HdPackageGet;
  g_hd_provider.feature_count = HdFeatureCount; g_hd_provider.feature_get = HdFeatureGet;
  g_hd_provider.feature_option_get = HdOption; g_hd_provider.feature_enable = HdEnable;
  g_hd_provider.feature_set_option = HdSetOption; g_hd_provider.commit = HdCommit;
  g_hd_provider.option_get = HdLegacyOption; g_hd_provider.set_enabled = HdLegacyEnable;
  g_hd_provider.set_option = HdLegacySetOption;
  return &g_hd_provider;
}
