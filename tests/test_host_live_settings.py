"""Exercise the host's actual live-setting functions with a controlled SDL window."""
import os
import pathlib
import re
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DESKTOP = ROOT / 'runner/src/desktop'


def _function(source, name):
    match = re.search(r'(?m)^(?:static )?(?:void|int) ' + name + r'\([^;]*?\)\s*\{', source)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def _run(tmp_path, source):
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
    if not cc:
        pytest.skip('C compiler required')
    cfile = tmp_path / 'host.c'
    cfile.write_text(source, encoding='utf-8')
    exe = tmp_path / ('host.exe' if os.name == 'nt' else 'host')
    build = subprocess.run([cc, '-std=c11', str(cfile), '-o', str(exe)],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr


STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef struct { int x,y,w,h; } SDL_Rect;
static void *g_window;
static uint32 g_win_flags = 16;
static uint8 g_cursor = 1, g_current_window_scale = 2;
static int g_snes_width = 256, actual_flags, window_w, window_h, bounds_on;
static bool g_config_dirty;
static struct { uint8 fullscreen, window_scale; } g_config;
#define SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP 1
#define SDL_WINDOW_FULLSCREEN 1
#define SDL_WINDOW_MINIMIZED 2
#define SDL_WINDOW_MAXIMIZED 4
#define SDL_WINDOWPOS_CENTERED 0
#define kMaxWindowScale 10
static int SDL_SetWindowFullscreen(void *w, uint32 flags) { actual_flags = flags; return 0; }
static void snesrecomp_sdl_show_cursor(int c) { (void)c; }
static uint32 SDL_GetWindowFlags(void *w) { return actual_flags; }
static int snesrecomp_sdl_get_display_usable_bounds(void *w, SDL_Rect *r) {
  *r = (SDL_Rect){0,0,800,600}; return bounds_on;
}
static int snesrecomp_sdl_get_window_borders_size(void *w, int *t, int *l, int *b, int *r) { return 0; }
static int WindowBaseWidth(int w) { return w; }
static int WindowBaseHeight(void) { return 224; }
static int IntMax(int a, int b) { return a > b ? a : b; }
static int IntMin(int a, int b) { return a < b ? a : b; }
static void SDL_SetWindowSize(void *w, int x, int y) { window_w = x; window_h = y; }
static void SDL_SetWindowPosition(void *w, int x, int y) {}
static void snesrecomp_sdl_get_global_mouse_state(int *x, int *y) { *x = *y = 0; }
'''


def test_live_scale_and_fullscreen_agree_with_existing_host_paths(tmp_path):
    source = (DESKTOP / 'host_main.c').read_text(encoding='utf-8')
    generated = '\n'.join(_function(source, name) for name in (
        'ChangeWindowScale', 'snesrecomp_set_fullscreen', 'snesrecomp_get_fullscreen',
        'snesrecomp_set_window_scale', 'snesrecomp_get_window_scale'))
    hotkey = re.search(r'    case kKeys_Fullscreen:\n([\s\S]*?)      break;', source).group(1)
    _run(tmp_path, STUBS + generated + '\nstatic void hotkey(void) {\n' + hotkey + r'''
}
int main(void) {
  snesrecomp_set_window_scale(5);
  assert(window_w == 1280 && window_h == 1120);
  assert(snesrecomp_get_window_scale() == 5 && g_current_window_scale == 5);
  snesrecomp_set_fullscreen(1);
  assert(snesrecomp_get_fullscreen() == 1 && !g_cursor && (g_win_flags & 16));
  hotkey();
  assert(!actual_flags && !snesrecomp_get_fullscreen() && g_cursor);
  hotkey();
  snesrecomp_set_fullscreen(0);
  assert(!actual_flags && !(g_win_flags & 1));
  hotkey();
  assert(actual_flags && snesrecomp_get_fullscreen() && !g_cursor);
  g_config_dirty = false;
  snesrecomp_set_window_scale(3);
  assert(snesrecomp_get_window_scale() == 5 && !g_config_dirty);
  snesrecomp_set_fullscreen(0);
  bounds_on = 1;
  snesrecomp_set_window_scale(5);
  assert(window_w == 512 && window_h == 448 && snesrecomp_get_window_scale() == 2);
  return 0;
}
''')


def test_new_commands_preserve_existing_defaults_and_can_be_named(tmp_path):
    config = (DESKTOP / 'config.h').read_text(encoding='utf-8')
    source = (DESKTOP / 'mmx_config.c').read_text(encoding='utf-8')
    keys = re.search(r'enum \{[\s\S]*?\n\};', config).group()
    array = re.search(r'static uint16 kDefaultKbdControls\[kKeys_Total\] = \{[\s\S]*?\n\};', source).group()
    keydefs = '\n'.join(f'#define {key} {index}' for index, key in enumerate(
        sorted(set(re.findall(r'SDLK_\w+', array))), 1))
    keydefs += '\n#define SDLK_SCANCODE_MASK 0x40000000\n#define kKeyMod_ScanCode 0x4000\n#define kKeyMod_Shift 0x8000\n#define kKeyMod_Alt 0x1000\n#define kKeyMod_Ctrl 0x2000\n'
    macros = source[source.index('#define REMAP_SDL_KEYCODE'):source.index('static uint16 kDefaultKbdControls')]
    _run(tmp_path, '#include <assert.h>\ntypedef unsigned short uint16;\n' +
         keys + '\n' + keydefs + macros + array + r'''
int main(void) {
  assert(kKeys_SaveStateMenu == 77 && kKeys_Rewind == 78 && kKeys_ControlsP3 == 81);
  assert(kDefaultKbdControls[kKeys_SaveStateMenu] == SDLK_F11);
  assert(kDefaultKbdControls[kKeys_Rewind] == SDLK_F12);
  assert(!kDefaultKbdControls[kKeys_ToggleVSync]);
  assert(!kDefaultKbdControls[kKeys_ToggleFrameBlend]);
  assert(!kDefaultKbdControls[kKeys_ToggleRunAhead]);
  return 0;
}
''')
    registry = re.search(r'static const KeyNameId kKeyNameId\[\] = \{([\s\S]*?)\n\};', source).group(1)
    for name in ('ToggleVSync', 'ToggleFrameBlend', 'ToggleRunAhead'):
        assert f'S({name})' in registry
