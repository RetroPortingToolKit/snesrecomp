// An exception inside the mod runtime must not reach C callers. A Mega Man X
// player whose install folder was "Mega Man X Recompilé" crashed at startup:
// without a UTF-8 process code page (this test links no manifest), libstdc++
// cannot convert the ANSI path to std::filesystem::path and throws. The host
// must get a refusal and an error message, and keep running without mods.
#include "mod_runtime.h"
#include "content_variant.h"
#include <cstdio>
#include <cstring>

extern "C" void snes_variant_clear_declared(void) {}
extern "C" const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) {
  return nullptr;
}

int main() {
#if defined(_WIN32)
  // "mods\Recompil<0xE9>" in Windows-1252: invalid as UTF-8, so the
  // conversion throws whenever the process code page is not UTF-8.
  const char *root = "mods-Recompil\xE9";
  const char *sha = "b8f70a6e7fb93819f79693578887e2c11e196bdf1ac6ddc7cb924b1ad0be2d32";
  if (snes_mod_runtime_initialize_c(root, "test", sha) != 0) {
    std::fprintf(stderr, "c_boundary_test: FAIL: initialize accepted an unconvertible root\n");
    return 1;
  }
  const char *error = snes_mod_runtime_last_error_c();
  if (!error || !std::strstr(error, "Mods are unavailable")) {
    std::fprintf(stderr, "c_boundary_test: FAIL: error not reported (%s)\n",
                 error ? error : "(null)");
    return 1;
  }
  if (snes_mod_runtime_commit_c("rom-Recompil\xE9.sfc") != 0) {
    std::fprintf(stderr, "c_boundary_test: FAIL: commit accepted an unconvertible path\n");
    return 1;
  }
  std::puts("c_boundary_test: PASS");
#else
  std::puts("c_boundary_test: skipped (narrow paths are bytes outside Windows)");
#endif
  return 0;
}
