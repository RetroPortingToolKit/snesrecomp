#pragma once
/* One built-in presentation feature for shared and game-owned frontends. */
#include "snes/hd_jobs.h"
#include "recomp_launcher.h"
#include <stdio.h>
#include <string.h>
#define SNES_HD_PACKAGE "snes-hd-mode7"
#define SNES_HD_FEATURE "gpu-mode7"
#define SNES_HD_NAME "High-resolution Mode 7"
#define SNES_HD_DESCRIPTION "Sample rotating and perspective backgrounds on the GPU. Defaults to 8x; select up to 64x. Pixel-art sprites and HUD retain their original detail. Unsupported scenes or devices keep native rendering."
static inline int SnesHdModIdentity(const char *p, const char *f) {
  return p && !strcmp(p, SNES_HD_PACKAGE) && (!f || !strcmp(f, SNES_HD_FEATURE));
}
static inline int SnesHdModPackage(bool enabled, RecompLauncherCModPackage *out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
#define HD_TEXT(field, text) snprintf(out->field, sizeof(out->field), "%s", text)
  HD_TEXT(id, SNES_HD_PACKAGE); HD_TEXT(version, "1"); HD_TEXT(name, SNES_HD_NAME);
  HD_TEXT(author, "snesrecomp contributors"); HD_TEXT(description, SNES_HD_DESCRIPTION);
  out->enabled = enabled; out->option_count = 1;
#undef HD_TEXT
  return 1;
}
static inline int SnesHdModFeature(bool enabled, RecompLauncherCModFeature *out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
#define HD_TEXT(field, text) snprintf(out->field, sizeof(out->field), "%s", text)
  HD_TEXT(id, SNES_HD_FEATURE); HD_TEXT(package_id, SNES_HD_PACKAGE);
  HD_TEXT(package_version, "1"); HD_TEXT(package_name, SNES_HD_NAME); HD_TEXT(name, SNES_HD_NAME);
  HD_TEXT(author, "snesrecomp contributors"); HD_TEXT(description, SNES_HD_DESCRIPTION);
  HD_TEXT(group, "Presentation"); HD_TEXT(status, enabled ? "Enabled" : "Native resolution");
  out->enabled = enabled; out->option_count = 1;
#undef HD_TEXT
  return 1;
}
static inline int SnesHdModOption(unsigned scale, int index, RecompLauncherCModOption *out) {
  if (index != 0 || !out) return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->id, sizeof(out->id), "scale");
  snprintf(out->label, sizeof(out->label), "Resolution multiplier");
  snprintf(out->description, sizeof(out->description), "1x to 64x. Nearest presentation samples only visible pixels; filters and shader presets can require larger GPU targets. Device limits may reduce the effective resolution.");
  snprintf(out->value, sizeof(out->value), "%u", scale);
  snprintf(out->default_value, sizeof(out->default_value), "%u", kSnesHdDefaultScale);
  out->type = RECOMP_MOD_OPTION_INTEGER; out->min_value = 1;
  out->max_value = kSnesHdMaxScale; out->step = 1;
  return 1;
}
