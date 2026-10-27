/* Exercise the same reload used at desktop game startup and launcher return. */
#define _POSIX_C_SOURCE 200809L
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "keybinds.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif

int main(void) {
#if defined(__linux__)
    char root[] = "/tmp/snes-keybinds-XXXXXX", cwd[4096];
    char user[4096], mount[4096], exe[4096], app[4096], ini[4096], payload_ini[4096];
    const char *old_app = getenv("APPIMAGE");
    char *saved_app = old_app ? strdup(old_app) : NULL;
    assert(getcwd(cwd, sizeof(cwd)) && mkdtemp(root));
    snprintf(user, sizeof(user), "%s/user", root);
    snprintf(mount, sizeof(mount), "%s/mount", root);
    snprintf(exe, sizeof(exe), "%s/mount/Game", root);
    snprintf(app, sizeof(app), "%s/user/Game.AppImage", root);
    snprintf(ini, sizeof(ini), "%s/user/keybinds.ini", root);
    snprintf(payload_ini, sizeof(payload_ini), "%s/mount/keybinds.ini", root);
    assert(!mkdir(user, 0700) && !mkdir(mount, 0700));
    FILE *f = fopen(ini, "w"); assert(f);
    fputs("[player1]\nb = F9\ny = F10\n", f); assert(!fclose(f));
    assert(!setenv("APPIMAGE", app, 1) && !chdir(root));
    keybinds_init(exe);
    assert(keybinds_get()->p1.b == SDL_SCANCODE_F9);
    assert(keybinds_get()->p1.y == SDL_SCANCODE_F10);
    uint8_t keys[512] = {0}; keys[SDL_SCANCODE_F9] = 1;
    assert(keybinds_read_player_runner(keys, 1) == 1);
    keybinds_set_button(1, 1, SDL_SCANCODE_F11);
    keybinds_save();
    keybinds_init(exe);
    assert(keybinds_get()->p1.b == SDL_SCANCODE_F11);
    assert(keybinds_get()->p1.y == SDL_SCANCODE_F10);
    assert(access(payload_ini, F_OK) != 0);

    /* Ordinary Linux executables still honor the supplied executable path. */
    assert(!unsetenv("APPIMAGE"));
    f = fopen(payload_ini, "w"); assert(f);
    fputs("[player1]\nb = F12\n", f); assert(!fclose(f));
    keybinds_init(exe);
    assert(keybinds_get()->p1.b == SDL_SCANCODE_F12);
    if (saved_app) { assert(!setenv("APPIMAGE", saved_app, 1)); free(saved_app); }
    assert(!chdir(cwd));
    assert(!unlink(ini) && !unlink(payload_ini));
    assert(!rmdir(user) && !rmdir(mount) && !rmdir(root));
    puts("AppImage keyboard startup, reload and persistence passed");
#else
    puts("AppImage keyboard paths apply only on Linux");
#endif
    return 0;
}
