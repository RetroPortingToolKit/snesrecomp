"""The overlays read a keyboard player's SNES buttons from the same physical keys
as the game: keybinds.ini, by scancode."""
import os
import pathlib
import re
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def _function(source, name):
    match = re.search(r'(?m)^static (?:void|bool) ' + name + r'\([^;]*?\)\s*\{', source)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


# A QWERTZ keyboard: the key labelled Y sits where US keyboards have Z
# (scancode 29) and sends 'y'; the key labelled Z sits at scancode 28.
# keybinds.ini binds SNES B to scancode 29 and Left to the arrow key, so in
# the game B is the key labelled Y. The legacy [KeyMap] controls name the
# keycode 'z' for B.
STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef int SDL_Scancode;
typedef int SDL_Keycode;
#define SDL_SCANCODE_UNKNOWN 0
#define SDL_SCANCODE_COUNT 512
#define SDLK_RETURN '\r'
#define SDLK_ESCAPE 27
#define SDLK_SPACE ' '
#define SDLK_RIGHT 0x4000004F
#define SDLK_LEFT 0x40000050
enum { SDL_QUIT = 1, SDL_KEYDOWN, SDL_KEYUP };
typedef struct { int type; struct { struct { int sym, mod; } keysym; int repeat; } key; } SDL_Event;
#define SNESRECOMP_SDL_EVENT_KEY(event) ((event).key.keysym.sym)
#define SNESRECOMP_SDL_EVENT_MOD(event) ((event).key.keysym.mod)
#define SNESRECOMP_SDL_EVENT_REPEAT(event) ((event).key.repeat)
#define kSnesHostPadSeats 2
enum { kKeys_Controls = 1, kKeys_Controls_Last = 12 };
#define SC_LABEL_Y 29
#define SC_LABEL_Z 28
#define SC_LEFT 80
#define PAD_B 0x001u
#define PAD_LEFT 0x040u

static struct { int player_src[kSnesHostPadSeats]; } g_config = {{1, 1}};
static uint32 g_input_state;
static uint32 g_mouse_buttons;
static uint16 g_seat_keys[kSnesHostPadSeats];
static uint8_t keyboard[SDL_SCANCODE_COUNT];
static SDL_Event queue[8];
static int queued, polled;
static int browser_keys, rewind_steps;
static bool left_bound = true;

static int SDL_PollEvent(SDL_Event *e) {
  if (polled == queued) return 0;
  *e = queue[polled++];
  return 1;
}
static void push_key(int type, int sym) {
  memset(&queue[queued], 0, sizeof(queue[0]));
  queue[queued].type = type;
  queue[queued].key.keysym.sym = sym;
  ++queued;
}
static bool HandleDeviceEvent(const SDL_Event *e) { (void)e; return false; }
static const uint8_t *snesrecomp_sdl_get_keyboard_state(void) { return keyboard; }
static SDL_Scancode snesrecomp_sdl_scancode_from_key(SDL_Keycode key) {
  return key == 'y' ? SC_LABEL_Y : key == 'z' ? SC_LABEL_Z : key == SDLK_LEFT ? SC_LEFT : 0;
}
static uint16_t keybinds_read_player_runner(const uint8_t *keys, int player) {
  if (player != 1) return 0;
  return (uint16_t)((keys[SC_LABEL_Y] ? PAD_B : 0) | (left_bound && keys[SC_LEFT] ? PAD_LEFT : 0));
}
static uint16_t keybinds_read_player_desktop(const uint8_t *keys, uint32 buttons, int player) {
  return (uint16_t)(keybinds_read_player_runner(keys, player) |
                    (player == 1 && (buttons & 1) ? PAD_B : 0));
}
/* The [KeyMap] controls, by keycode ('z' is SNES B), which HandleInput
 * still applies on key release. */
static int FindCmdForSdlKey(int key, int mod) { (void)mod; return key == 'z' ? kKeys_Controls + 7 : 0; }
static void HandleCommand(uint32 j, bool pressed) {
  if (j == kKeys_Controls + 7)
    g_input_state = pressed ? (g_input_state | PAD_B) : (g_input_state & ~PAD_B);
}
static void HandleInput(int key, int mod, bool pressed) { HandleCommand(FindCmdForSdlKey(key, mod), pressed); }
static void snes_savestate_menu_handle_key(int key, int repeat) { (void)key; (void)repeat; ++browser_keys; }
static void snes_rewind_step(int dir) { rewind_steps += dir; }
static bool rewind_open;
static int commits, closes;
static void snes_rewind_commit(void) { ++commits; rewind_open = false; }
static void snes_rewind_close(void) { ++closes; rewind_open = false; }
static void PollLocalControls(const uint8_t *keys);
static void RewindKeyDown(int key, int repeat);
'''

MAIN = r'''
static void pump(void (*key_down)(int, int)) {
  bool running = true;
  PumpOverlayEvents(&running, key_down);
  assert(running);
  queued = polled = 0;
}

int main(void) {
  void (*panels[2])(int, int) = { snes_savestate_menu_handle_key, RewindKeyDown };
  for (int p = 0; p < 2; ++p) {
    /* The key labelled Y is the game's B: it reaches both panels. */
    keyboard[SC_LABEL_Y] = 1; push_key(SDL_KEYDOWN, 'y');
    pump(panels[p]);
    assert((g_input_state & PAD_B) && "the game's B did not reach the panel");
    keyboard[SC_LABEL_Y] = 0; push_key(SDL_KEYUP, 'y');
    pump(panels[p]);
    assert(!(g_input_state & PAD_B) && "B stuck after release");
    /* The key labelled Z is unbound in keybinds.ini, so it sets nothing. */
    keyboard[SC_LABEL_Z] = 1; push_key(SDL_KEYDOWN, 'z');
    pump(panels[p]);
    assert(!(g_input_state & PAD_B) && "the key labelled Z set B");
    keyboard[SC_LABEL_Z] = 0; push_key(SDL_KEYUP, 'z');
    pump(panels[p]);

    g_mouse_buttons = 1;
    pump(panels[p]);
    assert((g_input_state & PAD_B) && "the mouse binding did not reach the panel");
    g_mouse_buttons = 0;
    pump(panels[p]);
    assert(!(g_input_state & PAD_B) && "mouse binding stuck after release");
  }

  /* A bound arrow reaches rewind through the pad word, where RunRewindLoop
   * scrubs; the key event must not step a second time. */
  RewindKeyDown(SDLK_LEFT, 0);
  assert(rewind_steps == 0 && "a bound arrow stepped twice");
  /* An unbound arrow, or a keyboard not assigned to player 1, still scrubs
   * through the key event. */
  left_bound = false;
  RewindKeyDown(SDLK_LEFT, 0);
  assert(rewind_steps == -1);
  left_bound = true; g_config.player_src[0] = 0;
  RewindKeyDown(SDLK_LEFT, 0);
  assert(rewind_steps == -2);
  puts("ok");
  return 0;
}
'''


LOOP_STUBS = r'''
#define SNES_PAD_B 0x001u
#define SNES_PAD_LEFT 0x040u
#define SNES_PAD_RIGHT 0x080u
#define SNES_PAD_A 0x100u
#define SNES_OVL_REPEAT_DELAY 350u
#define SNES_OVL_REPEAT_RATE 90u
static void *g_selftest_pad;
static int g_selftest_rewind_opens;
static bool g_overlay_modal, g_paused;
static uint64_t g_state_generation;
static unsigned current_pump;
static uint32 SDL_GetTicks(void) { return current_pump * 8u; }
static void SDL_Delay(uint32 ms) { (void)ms; }
static void host_report_breadcrumb(const char *fmt, ...) { (void)fmt; }
static void SetAudioPaused(bool paused) { (void)paused; }
static void ResetAudioTimeline(void) {}
static uint64_t RtlStateGeneration(void) { return 0; }
static void OverlayNoteClosed(void) {}
static void GameReset(void) {}
static void PresentFrozenWithOverlay(void) {}
static int snes_rewind_is_open(void) { return rewind_open; }
static uint32 OverlayNavInputs(void) { return g_input_state; }
/* The keyboard player lets go of the B held at open after two pumps and
 * presses it again two pumps later; pump 20 ends the loop by force. */
static void OverlaySelftestPadTick(unsigned pump) {
  current_pump = pump;
  if (pump == 2) keyboard[SC_LABEL_Y] = 0;
  if (pump == 4) keyboard[SC_LABEL_Y] = 1;
  if (pump == 20) rewind_open = false;
}
'''

LOOP_MAIN = r'''
int main(void) {
  (void)push_key; (void)snes_savestate_menu_handle_key;   /* used by the other test */
  /* The game's B is held (a dash) when the rewind hotkey opens the strip;
   * the main loop's poll has it in the word already. */
  keyboard[SC_LABEL_Y] = 1;
  PollLocalControls(keyboard);
  rewind_open = true;
  bool running = true;
  RunRewindLoop(&running);
  printf("closes=%d commits=%d at pump %u\n", closes, commits, current_pump);
  assert(closes == 1 && commits == 0);
  assert(current_pump == 5 && "B held at open closed the strip at once");
  puts("ok");
  return 0;
}
'''


def _compiler():
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
    if not cc:
        pytest.skip('C compiler required')
    return cc


def _build_and_run(tmp_path, cc, name, text):
    cfile = tmp_path / (name + '.c')
    cfile.write_text(text, encoding='utf-8')
    exe = tmp_path / (name + '.exe')
    build = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(exe)],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip().endswith('ok')


def _functions(*names):
    source = (ROOT / 'runner/src/desktop/host_main.c').read_text(encoding='utf-8')
    return '\n'.join(_function(source, name) for name in names)


def test_overlay_keys_follow_keybinds(tmp_path):
    cc = _compiler()
    functions = _functions('PollLocalControls', 'KeyIsPadControl', 'PumpOverlayEvents',
                           'RewindKeyDown')
    _build_and_run(tmp_path, cc, 'overlay_keys', STUBS + functions + MAIN)


def test_rewind_ignores_a_button_held_at_open(tmp_path):
    cc = _compiler()
    functions = _functions('PollLocalControls', 'KeyIsPadControl', 'PumpOverlayEvents',
                           'RewindKeyDown', 'RunRewindLoop')
    _build_and_run(tmp_path, cc, 'rewind_held', STUBS + LOOP_STUBS + functions + LOOP_MAIN)
