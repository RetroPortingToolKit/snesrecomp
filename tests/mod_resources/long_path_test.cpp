// Exercise the real catalog, state reader/writer and trusted activation plan.
// Fixture I/O explicitly uses extended Windows paths so it works before the fix.
#include "mod_runtime.h"
#include "content_variant.h"
#include "mod_paths.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
extern "C" void snes_variant_clear_declared(void) {}
extern "C" const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) {
    return nullptr;
}
// The SHA-256 of an empty file lets commit exercise its binary reader without
// requiring a copyrighted ROM or a game-specific fixture.
static const char *digest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
static unsigned activations;
static void activate(void) { ++activations; }
static void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

static fs::path fixture_io(const fs::path& path) {
#ifdef _WIN32
    fs::path absolute = fs::absolute(path);
    absolute.make_preferred();
    const auto native = absolute.native();
    if (native.compare(0, 4, L"\\\\?\\") == 0) return absolute;
    if (native.compare(0, 2, L"\\\\") == 0)
        return fs::path(L"\\\\?\\UNC\\" + native.substr(2));
    return fs::path(L"\\\\?\\" + native);
#else
    return path;
#endif
}

static void write(const fs::path& path, const std::string& text) {
    fs::create_directories(fixture_io(path.parent_path()));
    std::ofstream file(fixture_io(path), std::ios::binary);
    file << text;
    require(bool(file), "cannot create fixture");
}

static void cleanup_fixture(const fs::path& path) {
    // MinGW's recursive remove_all uses the same limited directory iterator.
    // This is only called on the unique directory created by this test.
    const auto io = fixture_io(path);
    require(!fs::is_symlink(io), "unexpected fixture symlink; cleanup refused");
    if (fs::is_directory(io)) {
        std::error_code ec;
        const auto entries = SNESRecomp::ModPaths::entries(path, ec);
        require(!ec, "cannot enumerate fixture during cleanup");
        for (const auto& entry : entries) cleanup_fixture(entry);
    }
    require(fs::remove(io), "cannot remove fixture");
}

static fs::path package_dir(const fs::path& root) {
    return root / "packages" / "test.longpath" / "1.0.0";
}
static fs::path padded(fs::path root, size_t manifest_length) {
    while ((package_dir(root) / "manifest.toml").native().size() < manifest_length) {
        const size_t remaining = manifest_length - (package_dir(root) / "manifest.toml").native().size();
        if (remaining == 1) root += "x";
        else root /= std::string(remaining > 41 ? 40 : remaining - 1, 'x');
    }
    require((package_dir(root) / "manifest.toml").native().size() == manifest_length,
            "test parent is too long for the requested boundary");
    return root;
}

static void run_case(const fs::path& root, const fs::path& input, bool c_api) {
    const fs::path manifest = package_dir(root) / "manifest.toml";
    std::printf("checking manifest=%zu\n", manifest.native().size());
    write(manifest,
        "format_version = 1\nid = \"test.longpath\"\nversion = \"1.0.0\"\n"
        "name = \"Long path fixture\"\nresolver = \"declarative\"\n"
        "[[target]]\ngame_id = \"test\"\nrom_sha256 = \"" + std::string(digest) + "\"\n"
        "[[feature]]\nid = \"enabled\"\nname = \"Enabled\"\ndefault_enabled = false\n"
        "presentation_only = true\n"
        "[[plugin]]\nfeature = \"enabled\"\nid = \"test.longpath.plugin\"\n");
    write(root / "state.toml",
        "format_version = 1\n[[package]]\nid = \"test.longpath\"\nversion = \"1.0.0\"\n"
        "[[feature]]\npackage_id = \"test.longpath\"\nid = \"enabled\"\nenabled = true\n");
    write(root / "empty.bin", "");
    std::string error;
    const auto initialize = [&] {
        return c_api ? snes_mod_runtime_initialize_c(input.u8string().c_str(), "test", digest) != 0
                     : SNESRecomp::mod_runtime_initialize(input, "test", digest, &error);
    };
    require(fs::is_regular_file(fixture_io(manifest)), "fixture manifest is missing");
    require(initialize(), "initialize: " + error + " / " + snes_mod_runtime_last_error_c());
    std::puts("initialized");
    require(SNESRecomp::mod_runtime_commit(input / "empty.bin", &error), "commit: " + error);
    activations = 0;
    SNESRecomp::mod_runtime_activate_plugins();
    std::printf("manifest=%zu activation=%u\n", manifest.native().size(), activations);
    require(activations == 1, "enabled package was not activated");
    char output[4096]{};
    require(snes_mod_runtime_package_root_c("test.longpath", "1.0.0", output, sizeof output) != 0,
            "package root was not returned");
    require(fs::u8path(output) == package_dir(input), "I/O path leaked into the logical package root");
    require(initialize() && snes_mod_runtime_feature_enabled_c("test.longpath", "enabled"),
            "persisted enabled selection was not reloaded");
    std::puts("checking manifest-only classification");
    snes_mod_runtime_set_cosmetic_allow_c("test.longpath@1.0.0");
    char effective[4096]{};
    require(snes_mod_runtime_effective_set_c(effective, sizeof effective) != 0 &&
                std::string(effective).find("test.longpath") == std::string::npos,
            "manifest-only classification failed");
    const auto payload = package_dir(root) / "nested" / "payload.bin";
    write(payload, "data");
    require(initialize(), "reload package with payload");
    snes_mod_runtime_set_cosmetic_allow_c("test.longpath@1.0.0");
    require(snes_mod_runtime_effective_set_c(effective, sizeof effective) != 0 &&
                std::string(effective).find("test.longpath") != std::string::npos,
            "nested package payload was not detected");
    fs::remove(fixture_io(payload));
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) return 2;
    fs::path root = fs::absolute(fs::u8path(argv[1])) /
        ("mod-paths-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        require(!fs::exists(fixture_io(root)), "fixture already exists");
        require(snes_mod_register_presentation_plugin("test.longpath.plugin", activate) != 0,
                "plugin registration failed");
        run_case(root / "short", root / "short", false);
        const auto below = padded(root / "below", 259);
        const auto above = padded(root / "above", 264);
        run_case(below, below, false);
        run_case(above, above, false);
        const auto deep = padded(root / "deep", 380);
        run_case(deep, deep, false);
        const auto unicode = padded(root / fs::u8path(u8"caf\u00e9-\u8def\u5f84"), 360);
        run_case(unicode, unicode, true);
        run_case(above, above.lexically_relative(fs::current_path()), false);
#ifdef _WIN32
        require(SNESRecomp::ModPaths::for_io(fs::path(L"\\\\server\\share\\folder\\..\\pack")) ==
                    fs::path(L"\\\\?\\UNC\\server\\share\\pack"), "UNC path conversion");
        require(SNESRecomp::ModPaths::for_io(fs::path(L"//server/share/../pack")) ==
                    fs::path(L"\\\\?\\UNC\\server\\share\\pack"), "UNC share root conversion");
        require(SNESRecomp::ModPaths::for_io(fs::path(L"\\\\?\\C:\\mods\\pack")) ==
                    fs::path(L"\\\\?\\C:\\mods\\pack"), "already extended path conversion");
        require(SNESRecomp::ModPaths::for_io(fs::path(L"\\\\.\\pipe\\test")) ==
                    fs::path(L"\\\\.\\pipe\\test"), "device path conversion");
        run_case(deep, fixture_io(deep), false);
#else
        require(SNESRecomp::ModPaths::for_io("../relative/path") == fs::path("../relative/path"),
                "non-Windows path changed");
#endif
        cleanup_fixture(root);
        std::puts("mod filesystem path checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "long_path_test: %s; fixture retained at %s\n",
                     error.what(), root.u8string().c_str());
        return 1;
    }
}
