#include "snes_savestate_menu.h"
#include "common_rtl.h"
#include "desktop/sdl_compat.h"
#include <assert.h>
#include "netplay/snes_netplay.h"

static int value = 7;
static char toast[96];
static int online;

int snes_netplay_active(void) { return online; }
int snes_netplay_is_host(void) { return 1; }
int snes_netplay_menu_ready(void) { return 1; }
int snes_netplay_menu_paused(void) { return 0; }
int snes_netplay_menu_open(void) { return 1; }
int snes_netplay_menu_save(int slot) { assert(slot < SNES_SAVESTATE_SLOTS); return 1; }
int snes_netplay_menu_load(int slot) { assert(slot < SNES_SAVESTATE_SLOTS); return 1; }
int snes_netplay_menu_close(void) { return 1; }
void snes_netplay_menu_cursor(int slot) { if (online) assert(slot < SNES_SAVESTATE_SLOTS); }

void RtlEnsureSaveDir(void) {}
void RtlSaveSlotPath(int slot, char *out, size_t cap) {
    snprintf(out, cap, "menu-test-%d.sav", slot);
}
bool RtlSaveSnapshot(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(&value, sizeof(value), 1, f) == 1;
    fclose(f);
    return ok;
}
bool RtlLoadSnapshot(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(&value, sizeof(value), 1, f) == 1;
    fclose(f);
    return ok;
}
void snes_osd_push(const char *text, int ms) {
    (void)ms; snprintf(toast, sizeof(toast), "%s", text);
}
void snes_osd_push_slot_saved(int slot) { (void)slot; }
void snes_osd_push_slot_loaded(int slot) { (void)slot; }
void snes_osd_push_slot_empty(int slot) { (void)slot; }

static void open_menu(void) {
    snes_savestate_menu_poll_open(0);
    assert(snes_savestate_menu_poll_open((1u << 2) | (1u << 11)));
    snes_savestate_menu_poll_nav(0, 1);
}

int main(void) {
    int x, y, w, h;
    snes_savestate_menu_viewport(1920, 1080, &x, &y, &w, &h);
    assert(x == 240 && y == 0 && w == 1440 && h == 1080);
    snes_savestate_menu_viewport(2520, 1080, &x, &y, &w, &h);
    assert(x == 540 && y == 0 && w == 1440 && h == 1080);
    snes_savestate_menu_viewport(800, 600, &x, &y, &w, &h);
    assert(x == 0 && y == 0 && w == 800 && h == 600);
    snes_savestate_menu_viewport(900, 1200, &x, &y, &w, &h);
    assert(x == 0 && y == 262 && w == 900 && h == 675);

    open_menu();
    snes_savestate_menu_handle_key(SDLK_q, 0);
    assert(snes_savestate_menu_selected() == SNES_SAVESTATE_QUICK_SLOT);
    snes_savestate_menu_poll_nav(1u << 8, 2);
    assert(snes_savestate_menu_is_open());
    assert(strcmp(toast, "Quick slot is empty") == 0);
    snes_savestate_menu_poll_nav(0, 3);
    snes_savestate_menu_handle_key(SDLK_1, 0);
    snes_savestate_menu_poll_nav(1u << 9, 4);
    snes_savestate_menu_close();

    uint32_t picture[16];
    for (int i = 0; i < 16; ++i) picture[i] = 0xff112233;
    snes_savestate_menu_note_frame(picture, 4, 4);
    char path[96];
    value = 42;
    RtlSaveSlotPath(SNES_SAVESTATE_QUICK_SLOT, path, sizeof(path));
    assert(RtlSaveSnapshot(path));
    snes_savestate_menu_note_slot_saved(SNES_SAVESTATE_QUICK_SLOT);
    char thumb[128]; snprintf(thumb, sizeof(thumb), "%s.thumb", path);
    FILE *f = fopen(thumb, "rb"); assert(f); fclose(f);
    open_menu();
    snes_savestate_menu_handle_key(SDLK_q, 0);
    value = 99;
    snes_savestate_menu_poll_nav(1u << 8, 5);
    assert(!snes_savestate_menu_is_open() && value == 42);
    assert(strcmp(toast, "Quick state loaded") == 0);
    open_menu();
    snes_savestate_menu_handle_key(SDLK_1, 0);
    snes_savestate_menu_poll_nav(1u << 8, 6);
    assert(value == 7); /* quick save did not overwrite numbered slot 01 */
    assert(!snes_savestate_menu_is_open());
    online = 1;
    open_menu();
    snes_savestate_menu_handle_key(SDLK_EQUALS, 0);
    assert(snes_savestate_menu_selected() == 11);
    snes_savestate_menu_poll_nav(1u << 5, 10);
    assert(snes_savestate_menu_selected() == 0); /* original network slot range */
    snes_savestate_menu_handle_key(SDLK_q, 0);
    assert(snes_savestate_menu_selected() == 0);
    assert(!snes_savestate_menu_netplay_write_slot(SNES_SAVESTATE_QUICK_SLOT));
    snes_savestate_menu_close();
    remove(path); remove(thumb); remove("menu-test-0.sav");
    puts("Save browser: centered aspect, quick slot, thumbnail and numbered isolation passed");
    return 0;
}
