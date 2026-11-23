"""One rewind request opens the filmstrip once, not again after it closes."""
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
#include <stdbool.h>
#include <stdio.h>
typedef struct { int unused; } PadGesture;
static PadGesture g_rewind_gesture;
static int g_rewind_hotkey;
static bool gesture, rewind_open, menu_open, refuse_open;
static int loops;
static bool PadGesturePressed(PadGesture *g) { (void)g; return gesture; }
static int snes_rewind_is_open(void) { return rewind_open; }
static int snes_savestate_menu_is_open(void) { return menu_open; }
static int snes_rewind_open(void) {
  if (refuse_open) return 0;
  rewind_open = true;
  return 1;
}
/* The modal loop runs until the strip closes: commit or close. */
static void RunRewindLoop(bool *running) { (void)running; ++loops; rewind_open = false; }
'''

MAIN = r'''
/* The main loop's shape: a frame that ran the strip leaves through
 * `continue`, so nothing after the call runs for that frame. */
static int run_frames(int n) {
  bool running = true;
  int frames_after = 0;
  for (int f = 0; f < n; ++f) {
    if (RunRewindRequest(&running))
      continue;
    ++frames_after;
  }
  return frames_after;
}

int main(void) {
  /* Hotkey: opens once, then the game runs on. */
  g_rewind_hotkey = 1;
  assert(run_frames(5) == 4);
  assert(loops == 1 && "the hotkey reopened rewind after it closed");
  assert(!g_rewind_hotkey);

  /* Pad gesture: the stub reports it for one frame, as the edge-detecting
   * PadGesturePressed does; one open. */
  loops = 0; gesture = true;
  assert(run_frames(1) == 0 && loops == 1);
  gesture = false;
  assert(run_frames(3) == 3 && loops == 1);

  /* While the save-state browser is open the request is dropped, as before. */
  loops = 0; menu_open = true; g_rewind_hotkey = 1;
  assert(run_frames(1) == 1 && loops == 0 && !g_rewind_hotkey);
  menu_open = false;
  assert(run_frames(3) == 3 && loops == 0);

  /* Refused by snes_rewind_open (netplay, too few snapshots): dropped too. */
  refuse_open = true; g_rewind_hotkey = 1;
  assert(run_frames(1) == 1 && loops == 0 && !g_rewind_hotkey);
  refuse_open = false;
  assert(run_frames(3) == 3 && loops == 0);
  puts("ok");
  return 0;
}
'''


def test_rewind_request_opens_once(tmp_path):
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
    if not cc:
        pytest.skip('C compiler required')
    source = (ROOT / 'runner/src/desktop/host_main.c').read_text(encoding='utf-8')
    cfile = tmp_path / 'rewind_request.c'
    cfile.write_text(STUBS + _function(source, 'RunRewindRequest') + MAIN, encoding='utf-8')
    exe = tmp_path / 'rewind_request.exe'
    build = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(exe)],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == 'ok'
