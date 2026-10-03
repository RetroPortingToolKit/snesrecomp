#include "snes/msu1.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned locks, calls;
void RtlApuLock(void) { ++locks; }
void RtlApuUnlock(void) { assert(locks); --locks; }
double RtlAudioOutputRate(void) { return 44100.0; }
static void pack(const char *path, int16_t sample) {
    FILE *f = fopen(path, "wb"); assert(f);
    const uint8_t header[8] = {'M','S','U','1',0,0,0,0};
    assert(fwrite(header, 1, 8, f) == 8);
    for (unsigned i=0; i<64; ++i) {
        int16_t pair[2] = {sample, -sample};
        assert(fwrite(pair, sizeof(pair), 1, f) == 1);
    }
    assert(!fclose(f));
}
static bool resolve(void *context, const char *base, uint16_t track, char *path, size_t cap) {
    assert(locks && !strcmp(base, "msu-route-primary"));
    ++calls;
    if (track == 77) return false; /* primary-77 exists; must not substitute it */
    return snprintf(path, cap, "%s-%u.pcm", (const char *)context, track) < (int)cap;
}
static void select_track(unsigned n) {
    msu1_write(0x2004, n & 255); msu1_write(0x2005, n >> 8);
    msu1_write(0x2007, 3);
}
static void samples(int16_t expected) {
    int16_t out[8] = {0};
    RtlApuLock(); msu1_mix(out, 4); RtlApuUnlock();
    for (unsigned i=0; i<4; ++i) { assert(out[2*i] == expected); assert(out[2*i+1] == -expected); }
}
int main(void) {
    pack("msu-route-primary-10.pcm", 111);
    pack("msu-route-primary-77.pcm", 777);
    pack("msu-route-secondary-10.pcm", 222);
#ifdef _WIN32
    _putenv_s("SNESRECOMP_MSU1", "msu-route-primary");
#else
    setenv("SNESRECOMP_MSU1", "msu-route-primary", 1);
#endif
    msu1_init();
    select_track(10); samples(111); assert(!calls);
    msu1_set_track_resolver(resolve, "msu-route-secondary");
    select_track(10); samples(222); assert(calls == 1);
    select_track(77); assert(msu1_read(0x2000) & 8); samples(0);
    msu1_init(); /* The host's routing policy survives a chip reset. */
    select_track(10); samples(222); assert(calls == 3);
    msu1_set_track_resolver(NULL, NULL);
    select_track(10); samples(111); assert(calls == 3);
    select_track(99); assert(msu1_read(0x2000) & 8); /* close active file */
    assert(!remove("msu-route-primary-10.pcm"));
    assert(!remove("msu-route-primary-77.pcm"));
    assert(!remove("msu-route-secondary-10.pcm"));
    puts("MSU default/override/rejected track resolution and reset passed");
    return 0;
}
