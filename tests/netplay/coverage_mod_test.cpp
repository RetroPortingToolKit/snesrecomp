#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "mod_runtime.h"
#include "content_variant.h"
#include "snes/tier2_capture.h"
#include "recomp_launcher.h"

/* This test exercises the real catalog/selection/netplay layer. Cartridge
 * variants are a separate subsystem; the empty fixture declares none. */
extern "C" void snes_variant_clear_declared(void) {}
extern "C" const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) { return nullptr; }

int main(int argc, char **argv) {
    assert(argc == 2);
    std::filesystem::create_directories(argv[1]);
    constexpr auto sha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    constexpr auto package = "snesrecomp.diagnostics.coverage";
    tier2_capture_configure(0, -1, -1);
    assert(snes_mod_runtime_initialize_c(argv[1], "test-game", sha));
    auto provider = snes_mod_runtime_launcher_provider_c();
    assert(provider && provider->feature_count(provider->ctx) == 1);
    RecompLauncherCModFeature feature{};
    assert(provider->feature_get(provider->ctx, 0, &feature));
    assert(feature.hidden && !feature.enabled && !tier2_capture_enabled());
    assert(provider->package_count(provider->ctx) == 0);
    char before[1024], after[1024];
    snes_mod_runtime_effective_set_c(before, sizeof before);
    tier2_capture_configure(1, -1, -1);
    assert(!tier2_capture_enabled());
    assert(provider->feature_enable(provider->ctx, package, "capture", 1));
    assert(tier2_capture_enabled());
    snes_mod_runtime_effective_set_c(after, sizeof after);
    assert(!strcmp(before, after));
    SnesModPkgRow rows[4];
    assert(snes_mod_runtime_installed_rows_c(rows, 4) == 0);
    assert(snes_mod_runtime_adopt_set_c(before, nullptr, 0) == SNES_MODSET_OK);
    assert(tier2_capture_enabled());
    tier2_capture_configure(1, -1, 0);
    assert(!tier2_capture_enabled());
    tier2_capture_configure(1, -1, -1);
    assert(tier2_capture_enabled());
    assert(provider->feature_enable(provider->ctx, package, "capture", 0));
    assert(!tier2_capture_enabled());
    std::cout << "coverage_mod_test: PASS\n";
}
