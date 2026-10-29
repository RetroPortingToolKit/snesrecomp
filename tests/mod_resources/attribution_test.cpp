// Exercise metadata through the real parser AND launcher provider, including
// old catalogs which only supplied a package author.
#include "../../runner/src/mod_runtime.cpp"
#include <cassert>

extern "C" {
void snes_variant_clear_declared(void) {}
const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) { return nullptr; }
}

int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    const fs::path root = fs::u8path(argv[1]);
    fs::create_directories(root);
    const fs::path manifest = root / "manifest.toml";
    const std::string package =
        "format_version = 1\nid = \"test.credits\"\nversion = \"1.0.0\"\n"
        "name = \"Native package\"\nauthor = \"Native authors\"\n";
    const std::string features =
        "[[target]]\ngame_id = \"test\"\nrom_sha256 = \"" + std::string(64, '0') + "\"\n"
        "[[feature]]\nid = \"inherited\"\nname = \"Inherited\"\n"
        "[[feature]]\nid = \"original\"\nname = \"Original\"\n"
        "author = \"Original Author; ASM: Contributor\"\n"
        "source_name = \"Original hack\"\nsource_url = \"https://example.com/hack\"\n";
    auto parse = [&](const std::string &text, bool ok) {
        { std::ofstream out(manifest); out << text; assert(out.good()); }
        SNESRecomp::Package result; std::string error;
        assert(SNESRecomp::read_manifest(manifest, result, &error) == ok);
        return result;
    };
    auto parsed = parse(package + "source_name = \"Package project\"\n"
        "source_url = \"https://example.com/package\"\n" + features, true);
    auto &runtime = SNESRecomp::state();
    runtime = SNESRecomp::Runtime{};
    runtime.game_id = "test";
    runtime.rom_sha256 = std::string(64, '0');
    runtime.packages[parsed.id][parsed.version] = parsed;
    RecompLauncherCModPackage p{};
    assert(SNESRecomp::provider_package_get(nullptr, 0, &p));
    assert(!std::strcmp(p.author, "Native authors"));
    assert(!std::strcmp(p.source_url, "https://example.com/package"));
    bool inherited = false, original = false;
    for (int i = 0; i < SNESRecomp::provider_feature_count(nullptr); ++i) {
        RecompLauncherCModFeature f{};
        assert(SNESRecomp::provider_feature_get(nullptr, i, &f));
        if (!std::strcmp(f.id, "inherited")) {
            inherited = true;
            assert(!std::strcmp(f.author, "Native authors"));
            assert(!std::strcmp(f.source_name, "Package project"));
            assert(!std::strcmp(f.source_url, "https://example.com/package"));
        } else {
            original = true;
            assert(!std::strcmp(f.author, "Original Author; ASM: Contributor"));
            assert(!std::strcmp(f.source_name, "Original hack"));
            assert(!std::strcmp(f.source_url, "https://example.com/hack"));
        }
    }
    assert(inherited && original);
    auto legacy = parse(package + features, true);
    assert(legacy.source_url.empty() && legacy.features[0].author.empty());
    for (const char *url : {"javascript:alert(1)", "file:///private", "https://", "https://example.com/a b"}) {
        parse(package + "source_url = \"" + url + "\"\n" + features, false);
        parse(package + features + "source_url = \"" + url + "\"\n", false);
    }
    fs::remove(manifest);
    if (argc >= 3) {
        runtime.packages.clear();
        runtime.game_id = "supermetroid-jpn";
        runtime.rom_sha256 = "12b77c4bc9c1832cee8881244659065ee1d84c70c3d29e6eaf92e6798cc2ca72";
        const fs::path catalog = fs::u8path(argv[2]);
        for (const auto &entry : fs::directory_iterator(catalog)) {
            SNESRecomp::Package p; std::string error;
            assert(SNESRecomp::read_manifest(entry.path() / "1.0.0/manifest.toml", p, &error));
            runtime.packages[p.id][p.version] = p;
        }
        assert(SNESRecomp::provider_feature_count(nullptr) == 40);
        for (int i = 0; i < 40; ++i) {
            RecompLauncherCModFeature f{};
            assert(SNESRecomp::provider_feature_get(nullptr, i, &f));
            assert(f.author[0] && f.source_name[0] && f.source_url[0]);
            const auto &p = runtime.packages.at(f.package_id).at(f.package_version);
            const auto *feature = SNESRecomp::find_feature(p, f.id);
            assert(feature && !std::strcmp(f.author, feature->author.c_str()));
            assert(!std::strcmp(f.source_url, feature->source_url.c_str()));
            std::printf("%s/%s\t%s\t%s\n", f.package_id, f.id, f.author, f.source_url);
        }
    }
    std::puts("Mod author/source metadata and legacy inheritance passed");
}
