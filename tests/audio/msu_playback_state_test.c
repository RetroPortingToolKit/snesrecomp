#include "snes/msu1.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned locks;
static double rate=44100;
void RtlApuLock(void) {++locks;}
void RtlApuUnlock(void) {assert(locks);--locks;}
double RtlAudioOutputRate(void) {return rate;}
static void init(const char *base) {
#ifdef _WIN32
    _putenv_s("SNESRECOMP_MSU1",base);
#else
    setenv("SNESRECOMP_MSU1",base,1);
#endif
    msu1_init();
}
static void pack(const char *base,unsigned track,unsigned salt,unsigned loop) {
    char path[128];snprintf(path,sizeof(path),"%s-%u.pcm",base,track);
    FILE *f=fopen(path,"wb");assert(f);
    uint8_t header[8]={'M','S','U','1',(uint8_t)loop,(uint8_t)(loop>>8),0,0};
    assert(fwrite(header,8,1,f)==1);
    for(unsigned i=0;i<4096;++i) {
        int16_t pair[2]={(int16_t)((i*733+salt)%40001-20000),(int16_t)((i*317+salt)%35001-17500)};
        assert(fwrite(pair,4,1,f)==1);
    }
    assert(!fclose(f));
}
static void select_track(unsigned n,unsigned control) {
    msu1_write(0x2004,n&255);msu1_write(0x2005,n>>8);
    msu1_write(0x2007,(uint8_t)control);
}
static void mix(int16_t *out,unsigned n) {
    memset(out,0,n*4);RtlApuLock();msu1_mix(out,(int)n);RtlApuUnlock();
}
int main(void) {
    pack("bookmark",10,123,777);pack("bookmark",11,456,0);pack("bookmark",273,789,0);
    static int16_t discarded[8192],expected[8192],actual[8192];
    uint8_t state[MSU1_PLAYBACK_STATE_SIZE],same[MSU1_PLAYBACK_STATE_SIZE];
    const double rates[]={44100,48000,32040,96000};
    for(unsigned r=0;r<4;++r)for(unsigned repeat=0;repeat<2;++repeat)for(unsigned paused=0;paused<2;++paused) {
        rate=rates[r];init("bookmark");select_track(10,repeat?3:1);msu1_write(0x2006,173);
        mix(discarded,3001);mix(discarded,27); /* fractional carry at device rates */
        if(paused)msu1_write(0x2007,repeat?2:0);
        msu1_capture_playback(state);assert(msu1_playback_state_valid(state));
        if(paused) {mix(actual,80);for(unsigned i=0;i<160;++i)assert(!actual[i]);}
        msu1_write(0x2007,repeat?3:1);
        mix(expected,103);mix(expected+206,719);mix(expected+1644,67);
        select_track(11,3);mix(discarded,80); /* another song/future position */
        assert(msu1_restore_playback(state));msu1_capture_playback(same);
        assert(!memcmp(state,same,sizeof(state)));
        if(paused) {mix(actual,80);for(unsigned i=0;i<160;++i)assert(!actual[i]);}
        msu1_write(0x2007,repeat?3:1);
        mix(actual,103);mix(actual+206,719);mix(actual+1644,67);
        assert(!memcmp(actual,expected,889*4));
    }
    /* Across repeated loop crossings and a changed output device rate. */
    rate=48000;init("bookmark");select_track(10,3);
    for(unsigned i=0;i<20;++i)mix(discarded,319);
    msu1_capture_playback(state);rate=44100;mix(expected,317);
    assert(msu1_restore_playback(state));mix(actual,317);assert(!memcmp(actual,expected,317*4));
    select_track(10,3);mix(discarded,1234);mix(expected,79);
    assert(msu1_seek_playback(1234));mix(actual,79);assert(!memcmp(actual,expected,79*4));
    /* A pending low-byte write remains pending independently of loaded track. */
    select_track(10,3);msu1_write(0x2004,17);msu1_capture_playback(state);
    select_track(11,3);assert(msu1_restore_playback(state));msu1_write(0x2005,1);
    assert(!(msu1_read(0x2000)&8));
    /* Invalid input never changes live audio; no selection/error/EOF persist. */
    select_track(10,3);mix(discarded,19);msu1_capture_playback(state);
    for(unsigned n=0;n<4;++n) {
        memcpy(same,state,sizeof(state));same[n?24:10]=1;
        if(n==1) {memset(same+24,0,8);same[30]=0xf8;same[31]=0x7f;} /* NaN */
        if(n==2) {memset(same+24,0,8);same[30]=0xf0;same[31]=0x7f;} /* infinity */
        if(n==3) {memset(same+24,0,8);same[30]=0xf0;same[31]=0x3f;} /* 1.0 */
        assert(!msu1_restore_playback(same));msu1_capture_playback(same);
        assert(!memcmp(state,same,sizeof(state)));
    }
    select_track(99,3);msu1_capture_playback(state);select_track(10,3);
    assert(msu1_restore_playback(state));assert(msu1_read(0x2000)&8);
    select_track(10,1);mix(discarded,4096);msu1_capture_playback(state);
    select_track(11,3);assert(msu1_restore_playback(state));assert(!(msu1_read(0x2000)&16));
    init("bookmark");msu1_capture_playback(state);select_track(10,3);
    assert(msu1_restore_playback(state));msu1_capture_playback(same);assert(!memcmp(state,same,sizeof(state)));
    select_track(10,3);msu1_capture_playback(state);select_track(99,0);
    assert(!rename("bookmark-10.pcm","bookmark-missing.pcm"));
    assert(!msu1_restore_playback(state));assert(msu1_read(0x2000)&8);
    init("");assert(!msu1_restore_playback(state));assert(!msu1_enabled());
    assert(!remove("bookmark-missing.pcm"));assert(!remove("bookmark-11.pcm"));assert(!remove("bookmark-273.pcm"));
    puts("PASS MSU playback exact continuation, loops, rates, pause, EOF, latch, invalid input, missing files and disabled preference");
    return 0;
}
