// Standalone, ROM-free provider test; compile with RECOMP_LAUNCHER enabled.
#include "mod_runtime.h"
#include "sha256.h"
#include "content_variant.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace fs=std::filesystem;
// Like coverage_mod_test, isolate resource providers from program switching.
// These fixtures declare no content variants. Link the real tier2_capture.c.
extern "C" void snes_variant_clear_declared(void) {}
extern "C" const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) { return nullptr; }
static std::string hash(const std::vector<uint8_t>& b) {
  uint8_t digest[32];sha256_compute(b.data(),b.size(),digest);char text[65];
  for(unsigned i=0;i<32;++i) std::snprintf(text+i*2,3,"%02x",digest[i]);return text;
}
static void write(const fs::path& p,const std::vector<uint8_t>& b) {
  std::ofstream f(p,std::ios::binary);f.write((const char*)b.data(),b.size());assert(f.good());
}
static void package(const fs::path& root,const char *id,const char *key,const std::string& digest) {
  fs::path dir=root/"packages"/id/"1.0.0";fs::create_directories(dir);
  std::ofstream f(dir/"manifest.toml");
  f<<"format_version = 1\nid = \""<<id<<"\"\nversion = \"1.0.0\"\nname = \"Source test\"\n"
     "resolver = \"declarative\"\n[[target]]\ngame_id = \"test\"\nrom_sha256 = \""<<digest<<"\"\n"
     "[[feature]]\nid = \"enabled\"\nname = \"Enabled\"\ndefault_enabled = false\n"
     "[[resource]]\nfeature = \"enabled\"\nid = \"rom\"\nlabel = \"ROM\"\nrequired = true\n"
     "shared_key = \""<<key<<"\"\nnormalized_sha256 = \""<<digest<<"\"\n";
}
int main(int argc,char **argv) {
  assert(argc==2);fs::path root=fs::absolute(argv[1]);fs::create_directories(root);
  std::vector<uint8_t> raw(32768,37),headered(512,0),wrong(32768,38);
  headered.insert(headered.end(),raw.begin(),raw.end());std::string digest=hash(raw);
  write(root/"source.sfc",raw);write(root/"header.smc",headered);write(root/"wrong.sfc",wrong);
  package(root,"test.zero","source.x3",digest);package(root,"test.x3","source.x3",digest);package(root,"test.x2","source.x2",digest);
  std::ofstream(root/"state.toml")<<"format_version = 1\n";
  assert(SNESRecomp::mod_runtime_initialize(root,"test",digest));
  const auto *p=snes_mod_runtime_launcher_provider_c();assert(p);
  auto set=[&](const char *id,const fs::path& file){assert(p->feature_resource_set_path(p->ctx,id,"enabled","rom",file.string().c_str()));};
  auto get=[&](const char *id){RecompLauncherCModResource r{};assert(p->feature_resource_get(p->ctx,id,"enabled",0,&r));return r;};
  set("test.zero",root/"source.sfc");assert(std::string(get("test.x3").path)==(root/"source.sfc").string());
  assert(get("test.x3").verified && !get("test.x2").path[0]);
  // The netplay offer's "files missing" list follows the shared path.
  char missing[64];
  assert(snes_mod_runtime_missing_files_c("test.x3",missing,sizeof(missing))==0 && !missing[0]);
  assert(snes_mod_runtime_missing_files_c("test.x2",missing,sizeof(missing))==1 && !std::strcmp(missing,"enabled"));
  assert(snes_mod_runtime_missing_files_c("absent.pkg",missing,sizeof(missing))==0);
  set("test.x3",root/"header.smc");assert(std::string(get("test.zero").path)==(root/"header.smc").string());
  assert(get("test.zero").verified); // Header normalized; both directions auto-fill.
  assert(p->feature_enable(p->ctx,"test.zero","enabled",1));assert(SNESRecomp::mod_runtime_commit());
  assert(SNESRecomp::mod_runtime_initialize(root,"test",digest));
  assert(std::string(get("test.zero").path)==std::string(get("test.x3").path));assert(get("test.zero").verified);
  set("test.x3",root/"wrong.sfc");assert(!get("test.zero").verified);
  assert(snes_mod_runtime_missing_files_c("test.zero",missing,sizeof(missing))==1); // wrong ROM counts as missingassert(!SNESRecomp::mod_runtime_commit());
  assert(p->feature_resource_set_path(p->ctx,"test.x3","enabled","rom",""));
  assert(!get("test.zero").path[0] && !get("test.x3").path[0]);
  assert(p->feature_enable(p->ctx,"test.zero","enabled",0));assert(SNESRecomp::mod_runtime_commit());
  assert(SNESRecomp::mod_runtime_initialize(root,"test",digest));assert(!get("test.zero").path[0]);
  puts("shared source ROM provider checks passed");
}
