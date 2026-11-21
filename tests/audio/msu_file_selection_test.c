#include "snes/msu1.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#define rmdir_one(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_one(p) mkdir(p, 0700)
#define rmdir_one(p) rmdir(p)
#endif

static unsigned locks;
void RtlApuLock(void) { ++locks; }
void RtlApuUnlock(void) { assert(locks); --locks; }
double RtlAudioOutputRate(void) { return 44100; }
static void init(const char *path) {
#ifdef _WIN32
    assert(!_putenv_s("SNESRECOMP_MSU1", path));
#else
    assert(!setenv("SNESRECOMP_MSU1", path, 1));
#endif
    msu1_init();
}
static void pcm(const char *path, int16_t sample) {
    FILE *f = fopen(path, "wb"); assert(f);
    const uint8_t header[8] = {'M','S','U','1',0,0,0,0};
    assert(fwrite(header, 1, sizeof(header), f) == sizeof(header));
    for (unsigned i = 0; i < 64; ++i) {
        int16_t pair[2] = {sample, -sample};
        assert(fwrite(pair, sizeof(pair), 1, f) == 1);
    }
    assert(!fclose(f));
}
static void data(const char *path, uint8_t value) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fputc(value, f) != EOF); assert(!fclose(f));
}
static void play(unsigned track, int16_t expected) {
    msu1_write(0x2004, track); msu1_write(0x2005, 0); msu1_write(0x2007, 3);
    assert(!(msu1_read(0x2000) & 8));
    int16_t out[4] = {0};
    RtlApuLock(); msu1_mix(out, 2); RtlApuUnlock();
    assert(out[0] == expected && out[1] == -expected);
}
int main(void) {
    assert(!mkdir_one("mixed.msu"));
    pcm("mixed.msu/mmx-7.pcm", 111);
    pcm("mixed.msu/castlevania-7.pcm", 222);
    pcm("mixed.msu/castlevania-8.pcm", 333);
    data("mixed.msu/mmx.msu", 0x58);
    data("mixed.msu/castlevania.msu", 0x43);
    data("mixed.msu/caps.MSU", 0x55); pcm("mixed.msu/caps-7.pcm", 444);
    // Legacy folders still work, including a directory whose name ends .msu.
    init("mixed.msu"); play(7, 222); assert(msu1_read(0x2001) == 0x43);
    // An explicit file overrides the larger unrelated soundtrack in that folder.
    init("mixed.msu/mmx.msu"); play(7, 111); assert(msu1_read(0x2001) == 0x58);
    uint8_t bookmark[MSU1_PLAYBACK_STATE_SIZE]; msu1_capture_playback(bookmark);
    int16_t before[4] = {0}, after[4] = {0};
    RtlApuLock(); msu1_mix(before, 2); RtlApuUnlock();
    msu1_write(0x2004, 8); msu1_write(0x2005, 0); msu1_write(0x2007, 3);
    assert(msu1_read(0x2000) & 8); // Never borrow the other pack's track 8.
    int16_t missing[4] = {0};
    RtlApuLock(); msu1_mix(missing, 2); RtlApuUnlock();
    assert(!missing[0] && !missing[1]);
    assert(msu1_restore_playback(bookmark));
    RtlApuLock(); msu1_mix(after, 2); RtlApuUnlock();
    assert(!memcmp(before, after, sizeof(before)));
    init("mixed.msu/caps.MSU");
    // Opening data through the seek port must retain the selected file's case too.
    for (unsigned port = 0x2000; port <= 0x2003; ++port) msu1_write(port, 0);
    play(7, 444); assert(msu1_read(0x2001) == 0x55);
    // Explicit prefixes remain supported independently of file selection.
    init("mixed.msu/mmx"); play(7, 111);
    init(""); assert(!msu1_enabled());
    const char *files[] = {"mmx-7.pcm", "castlevania-7.pcm", "castlevania-8.pcm",
                          "mmx.msu", "castlevania.msu", "caps.MSU", "caps-7.pcm"};
    for (unsigned i = 0; i < sizeof(files)/sizeof(files[0]); ++i) {
        char path[128]; snprintf(path, sizeof(path), "mixed.msu/%s", files[i]);
        assert(!remove(path));
    }
    assert(!rmdir_one("mixed.msu"));
    puts("MSU explicit file, mixed-pack audio/data, saved bookmark, legacy folder/prefix and disabled controls passed");
    return 0;
}
