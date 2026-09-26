#include "content_pack.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static char error[256];
static uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t v = ~0u; while (n--) { v ^= *p++; for (int i=0;i<8;i++) v = (v>>1) ^ ((v&1) ? 0xedb88320u : 0); } return ~v;
}
static void word(uint8_t *p, uint32_t n) { for (int i=0;i<4;i++) { p[i]=(uint8_t)n; n>>=8; } }
static void number(uint8_t *p, size_t *n, uint64_t v) {
    for (;;) { uint8_t x=v&127; v>>=7; if (!v) {p[(*n)++]=x|128;return;} p[(*n)++]=x; --v; }
}
static void finish(uint8_t *p, size_t *n, const uint8_t *s, size_t sn, const uint8_t *t, size_t tn) {
    word(p+*n, crc(s,sn)); *n+=4; word(p+*n,crc(t,tn)); *n+=4; word(p+*n,crc(p,*n)); *n+=4;
}
static void patches(void) {
    const uint8_t source[]="abcdef";
    uint8_t ips[]={ 'P','A','T','C','H', 0,0,1,0,2,'X','Y', 0,0,5,0,0,0,3,'Z', 'E','O','F' };
    uint8_t *out=NULL; size_t n=0;
    CHECK(cp_patch_apply(source,6,ips,sizeof(ips),&out,&n,error,sizeof(error)));
    CHECK(n==8 && !memcmp(out,"aXYdeZZZ",8) && !memcmp(source,"abcdef",6)); free(out);
    for (size_t i=0;i<sizeof(ips);i++) {
        out=(uint8_t*)source;n=77;
        CHECK(!cp_patch_apply(source,6,ips,i,&out,&n,error,sizeof(error)));
        CHECK(out==source && n==77);
    }
    uint8_t trim[sizeof(ips)+3]; memcpy(trim,ips,sizeof(ips)); trim[sizeof(ips)]=0;trim[sizeof(ips)+1]=0;trim[sizeof(ips)+2]=4;
    CHECK(cp_patch_apply(source,6,trim,sizeof(trim),&out,&n,error,sizeof(error)) && n==4 && !memcmp(out,"aXYd",4)); free(out);
    /* Exercises every BPS mode, negative relative movement, and overlapping
     * TargetCopy. These commands cannot be implemented as patch literals. */
    uint8_t bps[128]="BPS1"; size_t at=4;
    number(bps,&at,6);number(bps,&at,14);number(bps,&at,0);
    number(bps,&at,4); /* SourceRead ab */
    number(bps,&at,1);bps[at++]='X'; /* TargetRead X */
    number(bps,&at,6);number(bps,&at,6); /* SourceCopy de from +3 */
    number(bps,&at,6);number(bps,&at,9); /* SourceCopy bc from -4 */
    number(bps,&at,11);number(bps,&at,0); /* TargetCopy abX */
    number(bps,&at,15);number(bps,&at,5); /* TargetCopy bXde from -2 */
    const uint8_t target[]="abXdebcabXbXde";
    finish(bps,&at,source,6,target,14);
    CHECK(cp_patch_apply(source,6,bps,at,&out,&n,error,sizeof(error)) && n==14 && !memcmp(out,target,n)); free(out);
    for (size_t i=0;i<at;i++) {
        uint8_t old=bps[i];bps[i]^=0x20;out=NULL;
        CHECK(!cp_patch_apply(source,6,bps,at,&out,&n,error,sizeof(error)) && !out);bps[i]=old;
    }
    at=4; memcpy(bps,"BPS1",4);number(bps,&at,6);number(bps,&at,7);number(bps,&at,0);
    number(bps,&at,1);bps[at++]='A';number(bps,&at,23);number(bps,&at,0);
    finish(bps,&at,source,6,(const uint8_t*)"AAAAAAA",7);
    CHECK(cp_patch_apply(source,6,bps,at,&out,&n,error,sizeof(error)) && !memcmp(out,"AAAAAAA",7));free(out);
    /* A self-reference before any output is invalid even with valid CRCs. */
    at=4;number(bps,&at,6);number(bps,&at,1);number(bps,&at,0);number(bps,&at,3);number(bps,&at,0);
    finish(bps,&at,source,6,(const uint8_t*)"A",1);
    CHECK(!cp_patch_apply(source,6,bps,at,&out,&n,error,sizeof(error)));
}
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
    sha256_compute((const uint8_t *)"abc",3,p->source_hash);
    FILE *patch=fopen("identity.ips","wb");CHECK(patch);CHECK(fwrite("PATCHEOF",1,8,patch)==8);fclose(patch);
    uint8_t *out=NULL;size_t size=0;
    CHECK(cp_pack_apply(p,(const uint8_t *)"abc",3,"identity.ips",&out,&size,error,sizeof(error)));
    CHECK(size==3 && !memcmp(out,"abc",3));free(out);
    CHECK(!cp_pack_apply(p,(const uint8_t *)"abd",3,"identity.ips",&out,&size,error,sizeof(error)));
    p->alternate_target_hash[0][0]^=1;
    CHECK(!cp_pack_apply(p,(const uint8_t *)"abc",3,"identity.ips",&out,&size,error,sizeof(error)));
    remove("identity.ips");
    cp_catalog_free(&c);CHECK(!c.count);free(p);remove("pack.ini");
}
int main(void) { patches();catalog();puts("Content patches and additive catalog passed");return 0; }
