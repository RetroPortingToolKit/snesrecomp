// Standalone, ROM-free provider test; compile with RECOMP_LAUNCHER enabled.
// Features claiming one trusted plugin are alternatives: enabling one
// deselects the others, and unrelated features are left alone.
#include "mod_runtime.h"
#include "content_variant.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
namespace fs=std::filesystem;
// Like shared_source_rom_test, isolate providers from program switching.
extern "C" void snes_variant_clear_declared(void) {}
extern "C" const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) { return nullptr; }
static const char *kDigest="0000000000000000000000000000000000000000000000000000000000000000";
static void package(const fs::path& root,const char *id,const char *feature,
                    std::initializer_list<const char*> plugins) {
  fs::path dir=root/"packages"/id/"1.0.0";fs::create_directories(dir);
  std::ofstream f(dir/"manifest.toml");
  f<<"format_version = 1\nid = \""<<id<<"\"\nversion = \"1.0.0\"\nname = \"Exclusive test\"\n"
     "resolver = \"declarative\"\n[[target]]\ngame_id = \"test\"\nrom_sha256 = \""<<kDigest<<"\"\n"
     "[[feature]]\nid = \""<<feature<<"\"\nname = \""<<feature<<"\"\ndefault_enabled = false\n";
  for(const char *plugin:plugins) f<<"[[plugin]]\nfeature = \""<<feature<<"\"\nid = \""<<plugin<<"\"\n";
}
int main(int argc,char **argv) {
  assert(argc==2);fs::path root=fs::absolute(argv[1]);
  fs::remove_all(root);fs::create_directories(root);
  package(root,"test.zero","zero",{"test.plugin.zero"});
  package(root,"test.coop","coop",{"test.plugin.coop","test.plugin.zero"});
  package(root,"test.other","other",{"test.plugin.other"});
  std::ofstream(root/"state.toml")<<"format_version = 1\n";
  assert(SNESRecomp::mod_runtime_initialize(root,"test",kDigest));
  const auto *p=snes_mod_runtime_launcher_provider_c();assert(p);
  auto enabled=[&](const char *id){
    for(int i=0;i<p->feature_count(p->ctx);++i){
      RecompLauncherCModFeature f{};assert(p->feature_get(p->ctx,i,&f));
      if(!std::strcmp(f.package_id,id)) return f.enabled!=0;
    }
    assert(!"feature missing");return false;
  };
  assert(p->feature_enable(p->ctx,"test.other","other",1));
  assert(p->feature_enable(p->ctx,"test.zero","zero",1));
  assert(enabled("test.zero") && !enabled("test.coop") && enabled("test.other"));
  assert(p->feature_enable(p->ctx,"test.coop","coop",1));
  assert(!enabled("test.zero") && enabled("test.coop") && enabled("test.other"));
  assert(p->feature_enable(p->ctx,"test.zero","zero",1));
  assert(enabled("test.zero") && !enabled("test.coop") && enabled("test.other"));
  assert(p->feature_enable(p->ctx,"test.zero","zero",0)); // Disabling restores nothing.
  assert(!enabled("test.zero") && !enabled("test.coop"));
  assert(p->set_enabled(p->ctx,"test.coop",1));assert(p->set_enabled(p->ctx,"test.zero",1));
  assert(enabled("test.zero") && !enabled("test.coop")); // Package-level enable too.
  puts("exclusive plugin claim checks passed");
}
