// Exercise the real manifest parser and localization loader. Stock variants
// and RAM translations must keep working; obsolete ROM writes must fail.
#include "../../runner/src/mod_runtime.cpp"
#include "snes_text_xlate.h"
#include "snes/snes.h"
#include <cassert>

extern "C" {
Snes *g_snes;
Ppu *g_ppu;
void snes_variant_clear_declared(void) {}
const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) {
    return nullptr;
}
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const fs::path root = fs::u8path(argv[1]);
    fs::create_directories(root);
    const fs::path manifest = root / "manifest.toml";
    const std::string base =
        "format_version = 1\nid = \"test.immutable\"\nversion = \"1.0.0\"\n"
        "name = \"Immutable ROM\"\n[[target]]\ngame_id = \"test\"\n"
        "rom_sha256 = \"" + std::string(64, '0') + "\"\n"
        "[[feature]]\nid = \"data\"\nname = \"Data\"\n";
    auto parse = [&](const std::string& extra, bool expected) {
        { std::ofstream out(manifest); out << base << extra; assert(out.good()); }
        SNESRecomp::Package package;
        std::string error;
        assert(SNESRecomp::read_manifest(manifest, package, &error) == expected);
        if (!expected) assert(error.find("ROM patching is forbidden") != std::string::npos);
    };
    parse("[[variant]]\nfeature = \"data\"\nid = \"retail\"\nmodule = \"main\"\n", true);
    parse("[[patch]]\nfeature = \"data\"\nid = \"old\"\nfile = \"hack.ips\"\n", false);
    parse("[[variant]]\nfeature = \"data\"\nid = \"old\"\nmodule = \"main\"\npatch = \"old\"\n", false);
    // Even an empty obsolete field must fail rather than silently loading.
    parse("[[variant]]\nfeature = \"data\"\nid = \"old\"\nmodule = \"main\"\npatch = \"\"\n", false);

    const fs::path table = root / "translation.toml";
    {
        std::ofstream out(table);
        out << "[[ram_patch]]\naddress = 0x20\nsource_hex = \"01\"\nfr_hex = \"02\"\n"
               "[[rom_patch]]\naddress = 0\nsource_hex = \"03\"\nfr_hex = \"04\"\n";
    }
    uint8_t rom[1] = {3};
    Cart cart = {}; cart.rom = rom; cart.romSize = sizeof(rom);
    uint8_t ram[0x20000] = {};
    Snes snes = {}; snes.cart = &cart; snes.ram = ram;
    snes.ram[0x20] = 1; g_snes = &snes;
    const auto path = table.u8string();
    assert(!snes_text_xlate_init_c(path.c_str(), "fr"));
    assert(std::strstr(snes_text_xlate_last_error_c(), "ROM patching is forbidden"));
    snes_text_xlate_on_frame_c();
    assert(rom[0] == 3 && snes.ram[0x20] == 1);
    char identity[32]; snes_text_xlate_identity_c(identity, sizeof(identity));
    assert(!std::strcmp(identity, "off"));
    {
        std::ofstream out(table);
        out << "[[ram_patch]]\naddress = 0x20\nsource_hex = \"01\"\nfr_hex = \"02\"\n";
    }
    assert(snes_text_xlate_init_c(path.c_str(), "fr"));
    snes_text_xlate_on_frame_c();
    assert(snes.ram[0x20] == 2 && rom[0] == 3);
    snes_text_xlate_shutdown_c();
    g_snes = nullptr;
    fs::remove(table); fs::remove(manifest);
    std::puts("ROM patch rejection, stock variants and RAM translation passed");
}
