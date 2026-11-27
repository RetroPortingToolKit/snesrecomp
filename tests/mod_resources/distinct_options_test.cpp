// ROM-free checks through the existing launcher provider and commit gate.
#include "mod_runtime.h"
#include "content_variant.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
extern "C" void snes_variant_clear_declared(void) {}
extern "C" const SnesContentVariant *snes_variant_register_declared(const SnesVariantDecl *) { return nullptr; }
static const char *digest="0000000000000000000000000000000000000000000000000000000000000000";
static void noop() {}
static void manifest(const fs::path& root,const char *declaration) {
    fs::create_directories(root/"packages/test.positions/1");
    std::ofstream f(root/"packages/test.positions/1/manifest.toml");
    f<<"format_version=1\nid=\"test.positions\"\nversion=\"1\"\nname=\"Positions\"\n"
       "[[target]]\ngame_id=\"test\"\nrom_sha256=\""<<digest<<"\"\n"
       "[[feature]]\nid=\"wheel\"\nname=\"Weapon wheel\"\ndefault_enabled=true\n";
    if(declaration)f<<"distinct_options=\""<<declaration<<"\"\n";
    for(const char *id:{"beam","missiles"}) {
        f<<"[[option]]\nfeature=\"wheel\"\nid=\""<<id<<"\"\nlabel=\""
         <<(!std::strcmp(id,"beam")?"Beam position":"Missile position")<<"\"\ntype=\"choice\"\ndefault=\""
         <<(!std::strcmp(id,"beam")?"up":"right")<<"\"\n";
        for(const char *value:{"up","right","down"})
            f<<"[[option.choice]]\nvalue=\""<<value<<"\"\nlabel=\""<<value<<"\"\n";
    }
    f<<"[[plugin]]\nfeature=\"wheel\"\nid=\"test.positions\"\n";
}
int main(int argc,char **argv) {
    assert(argc==2);const fs::path root=fs::absolute(argv[1]);
    assert(!fs::exists(root));fs::create_directories(root);
    manifest(root,"beam, missiles");
    assert(snes_mod_register_activation_plugin("test.positions",noop));
    assert(SNESRecomp::mod_runtime_initialize(root,"test",digest));
    const auto *p=snes_mod_runtime_launcher_provider_c();assert(p);
    assert(SNESRecomp::mod_runtime_commit());
    assert(p->feature_set_option(p->ctx,"test.positions","wheel","missiles","up"));
    assert(!SNESRecomp::mod_runtime_commit());
    assert(p->diagnostic_count(p->ctx,"test.positions","wheel")==1);
    RecompLauncherCModDiagnostic d{};
    assert(p->diagnostic_get(p->ctx,"test.positions","wheel",0,&d));
    assert(std::strstr(d.message,"Beam position") && std::strstr(d.message,"Missile position"));
    // A disabled wheel never blocks Play; selection changes remain editable.
    assert(p->feature_enable(p->ctx,"test.positions","wheel",0));
    assert(SNESRecomp::mod_runtime_commit());
    assert(p->feature_enable(p->ctx,"test.positions","wheel",1));
    assert(!SNESRecomp::mod_runtime_commit());
    assert(p->feature_set_option(p->ctx,"test.positions","wheel","beam","down"));
    assert(SNESRecomp::mod_runtime_commit());
    assert(SNESRecomp::mod_runtime_initialize(root,"test",digest));
    assert(SNESRecomp::mod_runtime_commit());
    p=snes_mod_runtime_launcher_provider_c();
    assert(p->feature_set_option(p->ctx,"test.positions","wheel","missiles","down"));
    assert(!SNESRecomp::mod_runtime_commit());
    // Restore defaults through ordinary setters, as the UI does.
    assert(p->feature_set_option(p->ctx,"test.positions","wheel","beam","up"));
    assert(p->feature_set_option(p->ctx,"test.positions","wheel","missiles","right"));
    assert(SNESRecomp::mod_runtime_commit());
    // Temporary lobby selections use the same gate and restore offline values.
    assert(snes_mod_runtime_begin_temporary_c());
    assert(p->feature_set_option(p->ctx,"test.positions","wheel","missiles","up"));
    assert(!SNESRecomp::mod_runtime_commit());
    snes_mod_runtime_end_temporary_c();
    assert(SNESRecomp::mod_runtime_commit());
    const auto saved=root/"saved-conflict";manifest(saved,"beam,missiles");
    std::ofstream(saved/"state.toml")<<
        "format_version=1\n[[package]]\nid=\"test.positions\"\nversion=\"1\"\n"
        "[[feature]]\npackage_id=\"test.positions\"\nid=\"wheel\"\nenabled=true\n"
        "[feature.values]\nbeam=\"up\"\nmissiles=\"up\"\n";
    assert(SNESRecomp::mod_runtime_initialize(saved,"test",digest));
    assert(!SNESRecomp::mod_runtime_commit());
    p=snes_mod_runtime_launcher_provider_c();
    assert(p->diagnostic_count(p->ctx,"test.positions","wheel")==1);
    // Existing unannotated features may still intentionally share a value.
    const auto legacy=root/"legacy";manifest(legacy,nullptr);
    assert(SNESRecomp::mod_runtime_initialize(legacy,"test",digest));p=snes_mod_runtime_launcher_provider_c();
    assert(p->feature_set_option(p->ctx,"test.positions","wheel","missiles","up"));
    assert(SNESRecomp::mod_runtime_commit());
    unsigned bad_index=0;
    for(const char *bad:{"beam,missing","beam","beam,beam","beam,missiles,"}) {
        const auto invalid=root/("invalid-"+std::to_string(bad_index++));manifest(invalid,bad);
        assert(!SNESRecomp::mod_runtime_initialize(invalid,"test",digest));
    }
    std::cout<<"distinct choices, friendly diagnostics, disabled features, persistence and reset pass\n";
}
