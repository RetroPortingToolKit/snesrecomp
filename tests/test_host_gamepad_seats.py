"""The real mapped and raw allocators must honor the launcher's reservations."""
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


STUBS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t uint8;
typedef uint32_t uint32;
typedef int SDL_JoystickID;
typedef struct { int id; } SDL_Joystick;
typedef SDL_Joystick SDL_GameController;
typedef struct { SDL_Joystick *joystick; bool raw_joystick; int index,joystick_id; } GamepadInfo;
#define kSnesHostPadSeats 2
#define SNESRECOMP_SDL3 1
typedef struct {
  int enable_gamepad[2], player_src[2];
  int fullscreen, window_width, window_height, window_scale, display_aspect;
  int frame_blend, run_ahead, rewind_enabled, rewind_depth, rewind_interval;
  char player_guid[2][33];
} Config;
static Config g_config;
static GamepadInfo g_gamepad[2];
static uint32 g_seat_instance[2];
static SDL_Joystick *g_selftest_pad;
static SDL_Joystick devices[] = {{1},{2},{3}};
static int connected[] = {2,3}, nconnected = 2, raw_closed;
static int HostConfiguredSeats(void) { return 2; }
static bool SelftestPadExcludes(int id) { return false; }
static GamepadInfo *GetGamepadInfo(int id) {
  for (int i=0;i<2;i++) if (g_gamepad[i].joystick_id==id) return &g_gamepad[i];
  return NULL;
}
static bool SDL_IsGameController(int id) { return id != 1; }
static SDL_Joystick *SDL_JoystickOpen(int id) { return &devices[id-1]; }
static void SDL_JoystickClose(SDL_Joystick *p) { ++raw_closed; }
static int SDL_JoystickInstanceID(SDL_Joystick *p) { return p->id; }
typedef struct { int id; } SDL_JoystickGUID;
static SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *p) {
  return (SDL_JoystickGUID){p->id};
}
static void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char *out, int size) {
  snprintf(out,size,"%032x",guid.id);
}
#define SDL_GetJoystickGUID SDL_JoystickGetGUID
#define SDL_GUIDToString SDL_JoystickGetGUIDString
static const char *SDL_JoystickName(SDL_Joystick *p) { return "raw"; }
static SDL_GameController *SDL_GameControllerOpen(int id) { return &devices[id-1]; }
static void SDL_GameControllerClose(SDL_GameController *p) {}
static const char *SDL_GameControllerName(SDL_GameController *p) { return "identical pad"; }
static const char *SDL_GetError(void) { return "fake"; }
static SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *p) { return p; }
static int SDL_GameControllerGetPlayerIndex(SDL_GameController *p) { return 0; }
static SDL_JoystickID *SDL_GetGamepads(int *n) {
  *n=nconnected; int *ids=malloc((size_t)nconnected*sizeof(int));
  memcpy(ids,connected,(size_t)nconnected*sizeof(int)); return ids;
}
#define SDL_free free
static void OpenOneJoystick(int i);
'''


def test_mapped_raw_and_disconnected_reservations(tmp_path):
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
    if not cc:
        pytest.skip('C compiler required')
    source = (ROOT / 'runner/src/desktop/host_main.c').read_text(encoding='utf-8')
    functions = '\n'.join(_function(source, name) for name in (
        'SeatHeldForOtherPad', 'SeatGuidMatches', 'OpenOneGamepad', 'OpenOneJoystick'))
    cfile = tmp_path / 'seats.c'
    cfile.write_text(STUBS + functions + r'''
static void reset(void) {
  memset(&g_config,0,sizeof(g_config));
  memset(g_gamepad,0,sizeof(g_gamepad));
  for (int i=0;i<2;i++) { g_config.enable_gamepad[i]=1; g_gamepad[i].joystick_id=-1; }
  g_seat_instance[0]=3; g_seat_instance[1]=4;
  connected[0]=2; connected[1]=3; nconnected=2;
}
int main(void) {
  reset(); OpenOneGamepad(3); OpenOneGamepad(2);
  assert(g_gamepad[0].joystick_id==2 && g_gamepad[1].joystick_id==3);
  reset(); strcpy(g_config.player_guid[0],"00000000000000000000000000000003");
  OpenOneGamepad(3); OpenOneGamepad(2); /* Explicit physical choice wins. */
  assert(g_gamepad[0].joystick_id==2 && g_gamepad[1].joystick_id==3);
  reset(); OpenOneGamepad(1);
  assert(g_gamepad[0].joystick_id==-1 && g_gamepad[1].joystick_id==-1 && raw_closed==1);
  OpenOneGamepad(3); OpenOneGamepad(2);
  assert(g_gamepad[0].joystick_id==2 && g_gamepad[1].joystick_id==3);
  reset(); connected[0]=3; nconnected=1;
  OpenOneGamepad(1); OpenOneGamepad(3);
  assert(g_gamepad[0].joystick_id==1 && g_gamepad[1].joystick_id==3);
  return 0;
}
''', encoding='utf-8')
    exe = tmp_path / ('seats.exe' if os.name == 'nt' else 'seats')
    build = subprocess.run([cc, '-std=c11', str(cfile), '-o', str(exe)],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr


def test_live_physical_selection_without_source_kind_change(tmp_path):
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
    if not cc:
        pytest.skip('C compiler required')
    source = (ROOT / 'runner/src/desktop/host_main.c').read_text(encoding='utf-8')
    support = r'''
#define SNESRECOMP_SDL_GLES2_ONLY 1
#define SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP 4096
#define SDL_WINDOW_FULLSCREEN 1
#define kMaxWindowScale 8
static uint32 g_win_flags;
static void *g_window;
static int g_current_window_scale, reassigned;
static const char *g_program_path=".";
static struct { void (*Reconfigure)(void); } g_renderer_funcs;
typedef struct { bool rewind_settings; } TestGame;
static TestGame game;
static const TestGame *g_game=&game;
static int IntMin(int a,int b) { return a<b?a:b; }
static void SDL_SetWindowFullscreen(void *window,uint32 flags) {}
static void RefreshGameCursor(bool force) {}
static void ChangeWindowScale(int scale) {}
static void FrameBlendConfigure(void) {}
static void snes_runahead_set_frames(int frames) {}
static void snes_rewind_set_defaults(int enabled,int depth,int interval) {}
static void snes_rewind_shutdown(void) {}
static void snes_rewind_configure(void) {}
static void keybinds_init(const char *path) {}
static void ReassignGamepads(void) { ++reassigned; }
'''
    functions = '\n'.join(_function(source, name) for name in
                          ('SeatHeldForOtherPad', 'ApplyLiveSettings'))
    main = r'''
int main(void) {
  g_config.enable_gamepad[0]=1; g_config.player_src[0]=2;
  g_gamepad[0].joystick_id=2; g_gamepad[1].joystick_id=-1;
  g_seat_instance[0]=3;
  Config before=g_config;
  strcpy(g_config.player_guid[1],"00000000000000000000000000000003");
  ApplyLiveSettings(&before);
  if (reassigned) return 40; /* Unchanged selection must not disconnect input. */
  before=g_config; /* An inactive player's stored preference does not reseat. */
  g_seat_instance[0]=4; /* The launcher selected the other connected pad. */
  ApplyLiveSettings(&before);
  if (reassigned!=1) {
    fputs("Different connected gamepad was ignored while Source stayed Gamepad\n",stderr);
    return 41;
  }
  g_gamepad[0].joystick_id=3;
  ApplyLiveSettings(&before);
  if (reassigned!=1) return 42;
  g_seat_instance[0]=100; /* Absent preference must not churn a working fallback. */
  ApplyLiveSettings(&before);
  if (reassigned!=1) return 43;
  connected[0]=99; nconnected=1;
  ApplyLiveSettings(&before);
  if (reassigned!=2) return 44;
  g_config.enable_gamepad[0]=0;
  ApplyLiveSettings(&before);
  if (reassigned!=3) return 45;
  before=g_config; g_config.player_src[0]=1;
  ApplyLiveSettings(&before);
  if (reassigned!=4) return 46;
  return 0;
}
'''
    cfile = tmp_path / 'live.c'
    cfile.write_text(STUBS + support + functions + main, encoding='utf-8')
    exe = tmp_path / ('live.exe' if os.name == 'nt' else 'live')
    build = subprocess.run([cc, '-std=c11', str(cfile), '-o', str(exe)],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr


def test_saved_guid_routes_real_config_to_virtual_gamepad(tmp_path):
    """Exercise the actual INI parser and allocator, without a launcher window."""
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
    pkg = os.environ.get('PKG_CONFIG') or shutil.which('pkg-config')
    if not cc or not pkg:
        pytest.skip('C compiler and SDL3 development package required')
    flags = subprocess.run([pkg, '--cflags', '--libs', 'sdl3'], capture_output=True,
                           text=True)
    if flags.returncode:
        pytest.skip('SDL3 development package required')
    import shlex
    # This is a console-only input test, even if SDL's package defaults to GUI.
    sdl_flags = [flag for flag in shlex.split(flags.stdout) if flag != '-mwindows']
    source = (ROOT / 'runner/src/desktop/host_main.c').read_text(encoding='utf-8')
    prefix = r'''
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { SDL_Joystick *joystick; bool raw_joystick; int index,joystick_id; } GamepadInfo;
static GamepadInfo g_gamepad[kSnesHostPadSeats];
static uint32 g_seat_instance[kSnesHostPadSeats];
static SDL_Joystick *g_selftest_pad;
void Die(const char *error) { fprintf(stderr,"%s\n",error); exit(2); }
static int HostConfiguredSeats(void) { return 2; }
static bool SelftestPadExcludes(uint32 id) { return false; }
static GamepadInfo *GetGamepadInfo(uint32 id) {
  for(int i=0;i<kSnesHostPadSeats;++i)
    if((uint32)g_gamepad[i].joystick_id==id)return &g_gamepad[i];
  return NULL;
}
static void OpenOneJoystick(int i);
'''
    functions = '\n'.join(_function(source, name) for name in
                          ('SeatHeldForOtherPad', 'SeatGuidMatches', 'OpenOneGamepad', 'OpenOneJoystick'))
    main = r'''
int main(void) {
  if(!SDL_Init(SDL_INIT_GAMEPAD))return 70;
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.vendor_id=0x1209;
  desc.naxes=SDL_GAMEPAD_AXIS_COUNT;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
  desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
  desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
  desc.name="Preference test A";desc.product_id=1;
  SDL_JoystickID a=SDL_AttachVirtualJoystick(&desc);
  desc.name="Preference test B";desc.product_id=2;
  SDL_JoystickID b=SDL_AttachVirtualJoystick(&desc);
  if(!a||!b||!SDL_IsGamepad(a)||!SDL_IsGamepad(b))return 71;
  char guid[33];SDL_GUIDToString(SDL_GetJoystickGUIDForID(b),guid,sizeof guid);
  FILE *f=fopen("preferred.ini","wb");if(!f)return 72;
  fprintf(f,"[Controller]\nSourceP1=2\nSourceP2=0\nGuidP1=%s\n"
            "[GamepadMap]\nEnableGamepad1=true\nEnableGamepad2=false\n",guid);
  fclose(f);
  ParseConfigFile("preferred.ini");
  if(g_config.player_src[0]!=2||!g_config.enable_gamepad[0]||g_config.enable_gamepad[1])return 73;
  for(int i=0;i<kSnesHostPadSeats;++i)g_gamepad[i].joystick_id=-1;
  OpenOneGamepad(a);OpenOneGamepad(b);
  int result=g_gamepad[0].joystick_id==(int)b?0:74;
  if(result)fprintf(stderr,"Saved GUID chose %u but allocator seated %d\n",(unsigned)b,g_gamepad[0].joystick_id);
  SDL_Gamepad *pa=SDL_GetGamepadFromID(a),*pb=SDL_GetGamepadFromID(b);
  if(pa)SDL_CloseGamepad(pa);if(pb)SDL_CloseGamepad(pb);
  SDL_DetachVirtualJoystick(a);SDL_DetachVirtualJoystick(b);SDL_Quit();
  return result;
}
'''
    cfile = tmp_path / 'saved_guid.c'
    cfile.write_text(prefix + functions + main, encoding='utf-8')
    exe = tmp_path / ('saved_guid.exe' if os.name == 'nt' else 'saved_guid')
    build = subprocess.run([cc, '-std=c11', '-DSDL_MAIN_HANDLED', '-DSNESRECOMP_SDL3=1',
                           '-I' + str(ROOT / 'runner/src'),
                           '-I' + str(ROOT / 'runner/src/desktop'),
                           str(cfile), str(ROOT / 'runner/src/desktop/mmx_config.c'),
                           str(ROOT / 'runner/src/util.c'), *sdl_flags, '-o', str(exe)],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], cwd=tmp_path, capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
