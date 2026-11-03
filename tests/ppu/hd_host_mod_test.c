#define SDL_MAIN_HANDLED 1
#include "desktop/config.h"
Config g_config;
bool g_config_dirty;
#include "desktop/hd_host_mod.h"
#include <assert.h>
static int token, commits;
static int Count(void *c) { assert(c == &token); return 1; }
static int Feature(void *c, int i, RecompLauncherCModFeature *o) {
  assert(c == &token && i == 0); memset(o,0,sizeof(*o)); strcpy(o->id,"parent"); return 1;
}
static int Enable(void *c, const char *p, const char *f, int e) {
  assert(c == &token && !strcmp(p,"parent") && !strcmp(f,"feature") && e == 1); return 1;
}
static int Commit(void *c, const char *image) {
  assert(c == &token && !strcmp(image,"test.sfc")); ++commits; return 1;
}
int main(void) {
  RecompLauncherCModProvider parent={0}; parent.ctx=&token;
  parent.feature_count=Count; parent.feature_get=Feature;
  parent.feature_enable=Enable; parent.commit=parent.commit_netplay=Commit;
  g_config.mode7=SnesHdDefaultSettings();
  const RecompLauncherCModProvider *p=HdHostProvider(&parent);
  assert(p->ctx==&token && p->feature_count(p->ctx)==2 && p->commit_netplay==Commit);
  RecompLauncherCModFeature f;
  assert(p->feature_get(p->ctx,0,&f) && !strcmp(f.id,"parent"));
  assert(p->feature_get(p->ctx,1,&f) && f.enabled && !strcmp(f.package_id,SNES_HD_PACKAGE));
  RecompLauncherCModOption o;
  assert(p->feature_option_get(p->ctx,f.package_id,f.id,0,&o));
  assert(!strcmp(o.value,"8") && !strcmp(o.default_value,"8") && o.max_value==64);
  assert(p->feature_enable(p->ctx,"parent","feature",1));
  assert(p->feature_set_option(p->ctx,f.package_id,f.id,"scale","64"));
  assert(!p->feature_set_option(p->ctx,f.package_id,f.id,"scale","65"));
  assert(g_config.mode7.scale==64 && g_config_dirty);
  assert(p->feature_enable(p->ctx,f.package_id,f.id,0) && !g_config.mode7.enabled);
  assert(p->commit(p->ctx,"test.sfc") && p->commit_netplay(p->ctx,"test.sfc") && commits==2);
  p=HdHostProvider(NULL);
  assert(p->package_count(NULL)==1 && p->feature_count(NULL)==1 && p->commit(NULL,"test.sfc"));
  assert(!p->feature_enable(NULL,"unknown","unknown",1));
  return 0;
}
