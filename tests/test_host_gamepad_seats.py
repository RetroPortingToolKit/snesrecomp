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
static struct { int enable_gamepad[2]; } g_config;
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
        'SeatHeldForOtherPad', 'OpenOneGamepad', 'OpenOneJoystick'))
    cfile = tmp_path / 'seats.c'
    cfile.write_text(STUBS + functions + r'''
static void reset(void) {
  memset(g_gamepad,0,sizeof(g_gamepad));
  for (int i=0;i<2;i++) { g_config.enable_gamepad[i]=1; g_gamepad[i].joystick_id=-1; }
  g_seat_instance[0]=3; g_seat_instance[1]=4;
  connected[0]=2; connected[1]=3; nconnected=2;
}
int main(void) {
  reset(); OpenOneGamepad(3); OpenOneGamepad(2);
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
