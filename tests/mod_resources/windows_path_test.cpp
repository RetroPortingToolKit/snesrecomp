#include "mod_runtime.h"
#include "content_variant.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
namespace fs = std::filesystem;
extern "C" void snes_variant_clear_declared(void) {}
extern "C" const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) { return nullptr; }
static std::string host_path(const fs::path &p) {
#ifdef _WIN32
  int n = WideCharToMultiByte(CP_ACP, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
  assert(n > 0); std::string text(n, '\0');
  assert(WideCharToMultiByte(CP_ACP, 0, p.c_str(), -1, text.data(), n, nullptr, nullptr));
  text.pop_back(); return text;
#else
  return p.string();
#endif
}
int main(int argc, char **argv) {
  assert(argc == 2);
  fs::path root = fs::absolute(argv[1]) / fs::u8path(u8"Área de Trabalho");
  fs::path dir = root / "packages" / "test.paths" / "1.0.0";
  fs::create_directories(dir);
  constexpr auto sha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  std::ofstream(dir / "manifest.toml") <<
    "format_version = 1\nid = \"test.paths\"\nversion = \"1.0.0\"\nname = \"Paths\"\n"
    "resolver = \"declarative\"\n[[target]]\ngame_id = \"test\"\nrom_sha256 = \"" << sha << "\"\n"
    "[[feature]]\nid = \"enabled\"\nname = \"Enabled\"\ndefault_enabled = false\n"
    "[[resource]]\nfeature = \"enabled\"\nid = \"rom\"\nlabel = \"ROM\"\nrequired = true\n";
  fs::path rom = root / fs::u8path(u8"référence.sfc");
  std::ofstream(rom) << "test ROM";
  assert(snes_mod_runtime_initialize_c(host_path(root).c_str(), "test", sha));
  const auto *p = snes_mod_runtime_launcher_provider_c(); assert(p);
  assert(p->feature_resource_set_path(p->ctx, "test.paths", "enabled", "rom", rom.u8string().c_str()));
  RecompLauncherCModResource resource{};
  assert(p->feature_resource_get(p->ctx, "test.paths", "enabled", 0, &resource));
  assert(resource.verified && resource.path == rom.u8string());
  char out[4096];
  assert(snes_mod_runtime_package_root_c("test.paths", "1.0.0", out, sizeof(out)));
  assert(out == host_path(dir));
  assert(p->feature_enable(p->ctx, "test.paths", "enabled", 1));
  assert(snes_mod_runtime_commit_c(nullptr));
  assert(snes_mod_runtime_initialize_c(host_path(root).c_str(), "test", sha));
  assert(p->feature_resource_get(p->ctx, "test.paths", "enabled", 0, &resource));
  assert(resource.verified && resource.path == rom.u8string());
  assert(!snes_mod_runtime_commit_c(host_path(rom).c_str()));
  assert(std::string(snes_mod_runtime_last_error_c()).find("does not match") != std::string::npos);
  puts("Windows mod path checks passed");
}
