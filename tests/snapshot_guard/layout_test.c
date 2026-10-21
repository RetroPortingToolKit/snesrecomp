#include "common_rtl.h"
#include "snes/snes.h"
#include "snes/cpu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static Cpu cpu;
static Snes machine;
Snes *g_snes=&machine;
static uint8 ram[0x20000],decoy[0x20000];
static uint32 version=9;
static unsigned locks;
static size_t prefix,suffix;
static bool omit_ram,overflow;
void RtlApuLock(void) {assert(!locks);++locks;}
void RtlApuUnlock(void) {assert(locks==1);--locks;}
void snes_saveload_set_version(uint32_t value) {version=value;}
uint32_t snes_saveload_get_version(void) {return version;}
void snes_saveload(Snes *snes,SaveLoadInfo *sli) {
    assert(locks==1 && version==10);
    sli->func(sli,decoy,sizeof(decoy));
    sli->func(sli,&prefix,prefix);
    if(!omit_ram)sli->func(sli,snes->ram,sizeof(ram));
    sli->func(sli,&suffix,overflow?SIZE_MAX:suffix);
    snes->cpu->e=false; /* actual serializer normalizes this mirror */
}
int main(void) {
    machine.ram=ram;machine.cpu=&cpu;
    memset(ram,0x5a,sizeof(ram));
    const size_t prefixes[]={0,7,4097},suffixes[]={1,69,8192};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j) {
        prefix=prefixes[i];suffix=suffixes[j];cpu.e=true;
        size_t offset=0;uint32 reported=0;
        size_t size=RtlSnapshotGuestLayout(&reported,&offset);
        assert(reported==10 && version==9 && cpu.e && !locks);
        assert(offset==8+sizeof(decoy)+prefix);
        assert(size==offset+sizeof(ram)+suffix);
        assert(RtlSnapshotGuestSize(NULL)==size);
        for(size_t k=0;k<sizeof(ram);++k)assert(ram[k]==0x5a);
    }
    size_t offset;
    omit_ram=true;assert(!RtlSnapshotGuestLayout(NULL,&offset));assert(offset==SIZE_MAX);
    omit_ram=false;overflow=true;assert(!RtlSnapshotGuestLayout(NULL,&offset));
    assert(cpu.e && version==9 && !locks);
    puts("snapshot layout: serializer-derived offsets, decoy fields, bounds and nonmutation passed");
    return 0;
}
