#include "snapshot_guard.h"
#include "crc32.h"
#include <stdint.h>
#include <string.h>

static const uint8_t magic[8]={'R','S','G','U','A','R','D',1};
bool snes_snapshot_guard_identity_valid(const char *id) {
    if (!id || !*id) return false;
    size_t n=0; while(n<32 && id[n]) ++n;
    return n<32;
}
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
size_t snes_snapshot_guard_finish(void *data,size_t capacity,size_t payload_size,const char *id) {
    if (!snes_snapshot_guard_identity_valid(id) || payload_size>SIZE_MAX-SNES_SNAPSHOT_GUARD_OVERHEAD) return 0;
    size_t size=payload_size+SNES_SNAPSHOT_GUARD_OVERHEAD;
    if (!data) return size;
    if (capacity<size) return 0;
    uint8_t *p=data;
    memcpy(p,magic,8);
    put32(p+8,(uint32_t)payload_size);put32(p+12,(uint32_t)((uint64_t)payload_size>>32));
    memset(p+16,0,32);memcpy(p+16,id,strlen(id));
    put32(p+size-4,crc32_compute(p,size-4));
    return size;
}
bool snes_snapshot_guard_open(const void *data,size_t size,const char *id,const void **payload,size_t *length) {
    if (!data || !payload || !length) return false;
    const uint8_t *p=data;
    bool guarded=size>=8 && !memcmp(p,magic,8);
    if (!guarded) {
        if (id) return false;
        *payload=data;*length=size;return true;
    }
    if (!snes_snapshot_guard_identity_valid(id) || size<SNES_SNAPSHOT_GUARD_OVERHEAD) return false;
    uint64_t n=(uint64_t)get32(p+8)|((uint64_t)get32(p+12)<<32);
    uint8_t expected[32]={0};memcpy(expected,id,strlen(id));
    if (n!=size-SNES_SNAPSHOT_GUARD_OVERHEAD || memcmp(expected,p+16,32) ||
        get32(p+size-4)!=crc32_compute(p,size-4)) return false;
    *payload=p+SNES_SNAPSHOT_GUARD_PREFIX;*length=(size_t)n;
    return true;
}
