// Standalone, ROM-free provider test; compile with RECOMP_LAUNCHER enabled.
// A feature's requires list is a dependency: enabling it enables what it
// requires, and losing a requirement (turned off, or deselected as another
// feature's plugin rival) turns the dependent off with it.
#include "mod_runtime.h"
#include "content_variant.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
namespace fs=std::filesystem;
extern "C" void snes_variant_clear_declared(void) {}
extern "C" const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) { return nullptr; }
static const char *kDigest="0000000000000000000000000000000000000000000000000000000000000000";
static void package(const fs::path& root,const char *id,const char *name,const char *feature,
                    const char *requires_list,std::initializer_list<const char*> plugins) {
  fs::path dir=root/"packages"/id/"1.0.0";fs::create_directories(dir);
  std::ofstream f(dir/"manifest.toml");
  f<<"format_version = 1\nid = \""<<id<<"\"\nversion = \"1.0.0\"\nname = \""<<name<<"\"\n"
     "resolver = \"declarative\"\n[[target]]\ngame_id = \"test\"\nrom_sha256 = \""<<kDigest<<"\"\n"
     "[[feature]]\nid = \""<<feature<<"\"\nname = \""<<feature<<"\"\ndescription = \"Test.\"\ndefault_enabled = false\n";
  if(requires_list) f<<"requires = \""<<requires_list<<"\"\n";
  for(const char *plugin:plugins) f<<"[[plugin]]\nfeature = \""<<feature<<"\"\nid = \""<<plugin<<"\"\n";
}
static const RecompLauncherCModProvider *p;
static bool get(const char *id,RecompLauncherCModFeature *out) {
  for(int i=0;i<p->feature_count(p->ctx);++i)
    if(p->feature_get(p->ctx,i,out) && !std::strcmp(out->package_id,id)) return true;
  return false;
}
static bool enabled(const char *id) {
  RecompLauncherCModFeature f{};assert(get(id,&f));return f.enabled!=0;
}
static bool needs_attention(const char *id) {
  RecompLauncherCModFeature f{};assert(get(id,&f));return f.has_error!=0;
}
static void activate_none(void) {}
int main(int argc,char **argv) {
  assert(argc==2);fs::path root=fs::absolute(argv[1]);
  fs::remove_all(root);fs::create_directories(root);
  package(root,"test.zero","Zero","zero",nullptr,{"test.plugin.zero"});
  package(root,"test.coop","Co-op","coop",nullptr,{"test.plugin.coop","test.plugin.zero"});
  package(root,"test.learner","Learner","learner","test.coop/coop",{});
  package(root,"test.chain","Chain","chain","test.learner/learner",{});
  package(root,"test.orphan","Orphan","orphan","test.missing/feature",{});
  for(const char *plugin:{"test.plugin.zero","test.plugin.coop"})
    assert(snes_mod_register_activation_plugin(plugin,activate_none));
  std::ofstream(root/"state.toml")<<"format_version = 1\n";
  assert(SNESRecomp::mod_runtime_initialize(root,"test",kDigest));
  p=snes_mod_runtime_launcher_provider_c();assert(p);
  RecompLauncherCModFeature f{};
  assert(get("test.learner",&f) && std::strstr(f.description,"Test. Requires Co-op."));

  assert(p->feature_enable(p->ctx,"test.learner","learner",1));
  assert(enabled("test.learner") && enabled("test.coop") && !needs_attention("test.learner"));
  assert(p->feature_enable(p->ctx,"test.coop","coop",0)); // Losing the requirement.
  assert(!enabled("test.learner") && !enabled("test.coop"));

  assert(p->feature_enable(p->ctx,"test.chain","chain",1)); // Transitive.
  assert(enabled("test.chain") && enabled("test.learner") && enabled("test.coop"));
  assert(p->feature_enable(p->ctx,"test.zero","zero",1)); // Rival deselects co-op...
  assert(enabled("test.zero") && !enabled("test.coop"));
  assert(!enabled("test.learner") && !enabled("test.chain")); // ...and its dependents.

  assert(p->feature_enable(p->ctx,"test.learner","learner",1)); // Requirement wins its rival.
  assert(enabled("test.coop") && !enabled("test.zero"));
  assert(p->feature_enable(p->ctx,"test.learner","learner",0));
  assert(enabled("test.coop")); // Disabling a dependent leaves what it required.
  assert(p->feature_enable(p->ctx,"test.coop","coop",0));
  assert(p->set_enabled(p->ctx,"test.learner",1)); // Package-level enable too.
  assert(enabled("test.learner") && enabled("test.coop"));
  assert(p->set_enabled(p->ctx,"test.coop",0) && !enabled("test.learner"));

  assert(p->feature_enable(p->ctx,"test.orphan","orphan",1)); // Not installed: an error,
  assert(enabled("test.orphan") && needs_attention("test.orphan")); // not a deselection.
  assert(p->feature_enable(p->ctx,"test.orphan","orphan",0));

  // A saved state holding a dependent without its requirement is refused.
  std::ofstream(root/"state.toml")<<"format_version = 1\n"
      "[[feature]]\npackage_id = \"test.learner\"\nid = \"learner\"\nenabled = true\n"
      "[[feature]]\npackage_id = \"test.coop\"\nid = \"coop\"\nenabled = false\n";
  assert(SNESRecomp::mod_runtime_initialize(root,"test",kDigest));
  p=snes_mod_runtime_launcher_provider_c();assert(p);
  assert(enabled("test.learner") && !enabled("test.coop") && needs_attention("test.learner"));
  package(root,"test.bad","Bad","bad","no-slash",{}); // Malformed entries reject the catalog.
  assert(!SNESRecomp::mod_runtime_initialize(root,"test",kDigest));
  puts("feature requires checks passed");
}
