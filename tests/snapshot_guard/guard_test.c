#include "snapshot_guard.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t data[256]={0};const void *payload=NULL;size_t size=0;
    const char *id="test-game/coop/v1";
    for(unsigned i=0;i<128;++i)data[SNES_SNAPSHOT_GUARD_PREFIX+i]=(uint8_t)i;
    size_t n=snes_snapshot_guard_finish(data,sizeof(data),128,id);
    assert(n==128+SNES_SNAPSHOT_GUARD_OVERHEAD);
    assert(snes_snapshot_guard_finish(NULL,0,128,id)==n);
    assert(snes_snapshot_guard_open(data,n,id,&payload,&size));
    assert(size==128 && payload==data+SNES_SNAPSHOT_GUARD_PREFIX);
    assert(!snes_snapshot_guard_open(data,n,NULL,&payload,&size));
    assert(!snes_snapshot_guard_open(data,n,"different/mode",&payload,&size));
    for(size_t i=0;i<n;++i) {
        data[i]^=1;assert(!snes_snapshot_guard_open(data,n,id,&payload,&size));data[i]^=1;
    }
    for(size_t i=0;i<n;++i)assert(!snes_snapshot_guard_open(data,i,id,&payload,&size));
    assert(!snes_snapshot_guard_open(data,n+1,id,&payload,&size));
    const uint8_t old[]={1,2,3,4};
    assert(snes_snapshot_guard_open(old,sizeof(old),NULL,&payload,&size));
    assert(payload==old && size==sizeof(old));
    assert(!snes_snapshot_guard_open(old,sizeof(old),id,&payload,&size));
    assert(!snes_snapshot_guard_finish(data,4,128,id));
    assert(!snes_snapshot_guard_finish(NULL,0,SIZE_MAX,id));
    assert(!snes_snapshot_guard_identity_valid("12345678901234567890123456789012"));
    puts("snapshot guard: integrity, mode identity, bounds and unguarded compatibility passed");
    return 0;
}
