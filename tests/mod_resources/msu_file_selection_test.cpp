// ROM-free test through the actual launcher resource provider and saved state.
#include "mod_runtime.h"
#include "content_variant.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
extern "C" void snes_variant_clear_declared(void) {}
extern "C" const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) { return nullptr; }
static const char *digest = "0000000000000000000000000000000000000000000000000000000000000000";
static void manifest(const fs::path& root, const char *id, const char *format, const char *identity) {
    const fs::path dir = root / "packages" / id / "1.0.0";
    fs::create_directories(dir);
    std::ofstream f(dir / "manifest.toml");
    f << "format_version = 1\nid = \"" << id << "\"\nversion = \"1.0.0\"\nname = \"Music\"\n"
         "resolver = \"declarative\"\n[[target]]\ngame_id = \"test\"\nrom_sha256 = \"" << digest << "\"\n"
         "[[feature]]\nid = \"music\"\nname = \"Music\"\ndefault_enabled = false\n"
         "[[resource]]\nfeature = \"music\"\nid = \"pack\"\nlabel = \"Music pack\"\nrequired = true\n"
         "format = \"" << format << "\"\nidentity = \"" << identity << "\"\n"
         "file_patterns = \"*.msu,*.MSU\"\nfile_description = \"MSU-1 pack\"\n";
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const fs::path root = fs::absolute(argv[1]);
    fs::create_directories(root / "mixed");
    std::ofstream(root / "mixed" / "mmx.msu");
    std::ofstream(root / "wrong.txt");
    manifest(root, "test.msu", "directory", "snes.msu1.pack");
    manifest(root, "test.other", "file", "test.other.file");
    manifest(root, "test.folder", "directory", "test.other.folder");
    manifest(root, "test.either", "file-or-directory", "test.either");
    std::ofstream(root / "state.toml") << "format_version = 1\n";
    auto initialize = [&] { assert(SNESRecomp::mod_runtime_initialize(root, "test", digest)); };
    initialize();
    const auto *p = snes_mod_runtime_launcher_provider_c(); assert(p);
    auto set = [&](const char *id, const fs::path& path) {
        assert(p->feature_resource_set_path(p->ctx, id, "music", "pack", path.string().c_str()));
    };
    auto get = [&](const char *id) {
        RecompLauncherCModResource r{};
        assert(p->feature_resource_get(p->ctx, id, "music", 0, &r)); return r;
    };
    // Write a real old-format folder choice, then update only the manifest.
    set("test.msu", root / "mixed"); assert(get("test.msu").verified);
    assert(p->feature_enable(p->ctx, "test.msu", "music", 1));
    assert(SNESRecomp::mod_runtime_commit());
    manifest(root, "test.msu", "file-or-directory", "snes.msu1.pack");
    initialize();
    auto r = get("test.msu");
    assert(r.verified && !strcmp(r.format, "file-or-directory"));
    assert(!strcmp(r.file_patterns, "*.msu,*.MSU"));
    assert(std::string(r.path) == (root / "mixed").string());
    assert(SNESRecomp::mod_runtime_commit());
    assert(std::string(getenv("SNESRECOMP_MSU1")) == (root / "mixed").string());
    // Ordinary file resources must still reject folders.
    set("test.other", root / "mixed"); assert(!get("test.other").verified);
    set("test.other", root / "wrong.txt"); assert(get("test.other").verified);
    set("test.folder", root / "wrong.txt"); assert(!get("test.folder").verified);
    set("test.folder", root / "mixed"); assert(get("test.folder").verified);
    set("test.either", root / "mixed"); assert(get("test.either").verified);
    set("test.either", root / "wrong.txt"); assert(get("test.either").verified);
    set("test.msu", root / "wrong.txt"); assert(!get("test.msu").verified);
    assert(!SNESRecomp::mod_runtime_commit());
    set("test.msu", root / "mixed" / "mmx.msu"); assert(get("test.msu").verified);
    assert(SNESRecomp::mod_runtime_commit());
    initialize();
    r = get("test.msu"); assert(r.verified);
    assert(std::string(r.path) == (root / "mixed" / "mmx.msu").string());
    assert(SNESRecomp::mod_runtime_commit());
    assert(std::string(getenv("SNESRECOMP_MSU1")) == r.path);
    // A new PCM-only user can select a folder through the same resource ID.
    set("test.msu", ""); assert(!get("test.msu").verified);
    set("test.msu", root / "mixed"); assert(get("test.msu").verified);
    assert(SNESRecomp::mod_runtime_commit()); initialize();
    assert(std::string(get("test.msu").path) == (root / "mixed").string());
    assert(get("test.msu").verified);
    set("test.msu", root / "mixed" / "mmx.msu");
    assert(p->feature_enable(p->ctx, "test.msu", "music", 0));
    assert(SNESRecomp::mod_runtime_commit());
    const char *value = getenv("SNESRECOMP_MSU1"); assert(!value || !*value);
    initialize(); assert(std::string(get("test.msu").path) == r.path);
    puts("MSU resource file picker metadata, saved folder/file choices, validation, commit and disabled controls passed");
}
