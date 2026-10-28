#include "content_pack.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static char error[256];
static void manifest(const char *id, const char *extra) {
    FILE *f=fopen("pack.ini","wb");CHECK(f);
    fprintf(f,"format=1\nid=%s\nname=One Track\nauthor=Test\nadapter=test\n"
        "source_sha256=0000000000000000000000000000000000000000000000000000000000000000\n"
        "target_sha256=1111111111111111111111111111111111111111111111111111111111111111\n"
        "cup=solo|Solo Cup|7\ntrack=one|A Course|solo|4\n%s",id,extra);
    CHECK(!fclose(f));
}
static void catalog(void) {
    CpCatalog c={0}; CpPack *p=calloc(1,sizeof(*p));CHECK(p);
    manifest("z-pack",""); CHECK(cp_manifest_read("pack.ini",p,error,sizeof(error)));
    CHECK(p->cup_count==1 && p->track_count==1 && p->tracks[0].slot==4);
    CHECK(cp_catalog_add(&c,p,error,sizeof(error)));
    const CpPack *z=cp_catalog_find(&c,"z-pack");CHECK(z);
    const CpPack *owner=NULL; const CpCup *cup=cp_catalog_cup(&c,"z-pack/solo",&owner);
    CHECK(cup && owner==z && cup->slot==7);
    CHECK(!cp_catalog_add(&c,p,error,sizeof(error)) && c.count==1);
    strcpy(p->id,"a-pack");CHECK(cp_catalog_add(&c,p,error,sizeof(error)));
    CHECK(c.packs[1]==z && cp_catalog_cup(&c,"z-pack/solo",&owner)==cup);
    CHECK(!cp_catalog_cup(&c,"removed/solo",NULL));
    const char *bad[]={"id=duplicate\n","cup=solo|Clobber|1\n","track=one|Duplicate|solo|1\n","track=two|Dangling|missing|1\n","unknown=1\n","track=two|Overflow|solo|256\n"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        manifest("valid",bad[i]);strcpy(p->id,"sentinel");
        CHECK(!cp_manifest_read("pack.ini",p,error,sizeof(error)) && !strcmp(p->id,"sentinel"));
    }
    manifest("../escape","");CHECK(!cp_manifest_read("pack.ini",p,error,sizeof(error)));
    manifest("alias","alternate_target_sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n");
    CHECK(cp_manifest_read("pack.ini",p,error,sizeof(error)) && p->alternate_target_count==1);
    cp_catalog_free(&c);CHECK(!c.count);free(p);remove("pack.ini");
}
int main(void) { catalog();puts("Additive catalog passed");return 0; }
