/* The real browser and offline facade, with only snapshot I/O replaced. */
#include "common_rtl.h"
#include "desktop/sdl_compat.h"
#include "netplay/snes_netplay.h"
#include "snes_osd.h"
#include "snes_savestate_menu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SNESRECOMP_NET
#error This regression must link without enabling netplay
#endif

static int saved, loaded, saved_toast, loaded_toast;
static const char *state_path = "standalone_menu_fixture.sav";

static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

void RtlEnsureSaveDir(void) {}
void RtlSaveSlotPath(int slot, char *buf, size_t size)
{
    (void)slot;
    snprintf(buf, size, "%s", state_path);
}
bool RtlSaveSnapshot(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    fputc(1, file);
    fclose(file);
    saved++;
    return true;
}
bool RtlLoadSnapshot(const char *path)
{
    check(strcmp(path, state_path) == 0, "local snapshot path");
    loaded++;
    return true;
}
void snes_osd_push_slot_saved(int slot) { (void)slot; saved_toast++; }
void snes_osd_push_slot_loaded(int slot) { (void)slot; loaded_toast++; }
void snes_osd_push_slot_empty(int slot) { (void)slot; }

int main(void)
{
    const uint32_t gesture = (1u << 2) | (1u << 11);
    const uint32_t load = 1u << 8, save = 1u << 9, back = 1u;
    const uint32_t *pixels;
    int width, height;
    SnesNetplayConfig cfg;

    snes_netplay_config_defaults(&cfg);
    cfg.enabled = 1;
    check(snes_netplay_start(&cfg) < 0, "offline facade cannot start a session");
    check(!snes_netplay_active(), "netplay remains inactive");
    check(!snes_netplay_is_host() && !snes_netplay_menu_ready(), "offline status");
    check(!snes_netplay_menu_open() && !snes_netplay_menu_load(0), "offline menu facade");

    check(snes_savestate_menu_poll_open(gesture), "offline menu opens");
    check(snes_savestate_menu_overlay_image(&pixels, &width, &height), "menu renders");
    check(pixels && width == SNES_SSM_W && height == SNES_SSM_H, "panel dimensions");
    snes_savestate_menu_poll_nav(0, 0);
    snes_savestate_menu_poll_nav(save, 1);
    check(saved == 1 && saved_toast == 1, "local save succeeds");
    check(snes_savestate_menu_is_open(), "save keeps browser open");
    snes_savestate_menu_poll_nav(0, 2);
    snes_savestate_menu_poll_nav(load, 3);
    check(loaded == 1 && loaded_toast == 1, "local load succeeds");
    check(!snes_savestate_menu_is_open(), "load closes browser");
    check(!snes_savestate_menu_filter_guest_input(load), "load input is guarded");
    snes_savestate_menu_filter_guest_input(0);

    snes_savestate_menu_poll_open(0);
    check(snes_savestate_menu_poll_open(gesture), "browser reopens");
    snes_savestate_menu_poll_nav(0, 4);
    snes_savestate_menu_poll_nav(back, 5);
    check(!snes_savestate_menu_is_open(), "controller cancel closes browser");
    snes_savestate_menu_poll_open(0);
    check(snes_savestate_menu_poll_open(gesture), "browser opens for keyboard");
    snes_savestate_menu_handle_key((int)SDLK_ESCAPE, 0);
    check(!snes_savestate_menu_is_open(), "keyboard cancel closes browser");

    remove(state_path);
    puts("standalone save-state menu: PASS");
    return 0;
}
