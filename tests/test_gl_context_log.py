"""Compile the actual startup diagnostic against SDL2/SDL3 API contracts."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module", params=[0, 1])
def diagnostic(request, tmp_path_factory):
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("C compiler unavailable")
    out = tmp_path_factory.mktemp("gl-context-" + str(request.param))
    source = (ROOT / "runner/src/desktop/opengl.c").read_text(encoding="utf-8")
    begin = source.index("static void LogGlContext(")
    body = source[begin:source.index("\n}\n", begin) + 3]
    harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static void host_report_breadcrumb(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
typedef void SDL_Window;
#define GL_VENDOR 1
#define GL_RENDERER 2
#define GL_VERSION 3
#define SDL_WINDOW_HIDDEN 8u
#define SDL_WINDOW_MINIMIZED 64u
#define SDL_WINDOW_INPUT_FOCUS 512u
static const char *scenario;
static const unsigned char *glGetString(unsigned key) {
  if (!strcmp(scenario, "missing")) return NULL;
  return (const unsigned char *)(key == GL_VENDOR ? "Test Vendor" :
      key == GL_RENDERER ? "Test Renderer" : "4.0 Test Version");
}
static uint64_t SDL_GetWindowFlags(SDL_Window *window) {
  (void)window;
  return !strcmp(scenario, "hidden") ? SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED :
         !strcmp(scenario, "focused") ? SDL_WINDOW_INPUT_FOCUS : 0;
}
typedef int SDL_DisplayID;
typedef struct SDL_DisplayMode { int w, h; float refresh_rate; } SDL_DisplayMode;
static const char *SDL_GetDisplayName(int display) { return display == 1 ? "Test Display" : NULL; }
#if SNESRECOMP_SDL3
static SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window) { (void)window; return 1; }
static const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID display) {
  static const SDL_DisplayMode mode = { 1920, 1080, 59.94f }; (void)display;
  return !strcmp(scenario, "no-display") ? NULL : &mode;
}
#else
static int SDL_GetWindowDisplayIndex(SDL_Window *window) { (void)window; return 1; }
static int SDL_GetCurrentDisplayMode(int display, SDL_DisplayMode *mode) {
  (void)display;
  if (!strcmp(scenario, "no-display")) return -1;
  *mode = (SDL_DisplayMode){ 1920, 1080, 60.0f }; return 0;
}
#endif
#if SNESRECOMP_SDL3
static bool SDL_GL_GetSwapInterval(int *out) {
  if (!strcmp(scenario, "failed")) return false;
  *out = 1; return true;
}
#else
static int SDL_GL_GetSwapInterval(void) { return 1; }
#endif
''' + body + r'''
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  scenario = argv[1]; LogGlContext(NULL); return 0;
}
'''
    cfile = out / "diagnostic.c"
    cfile.write_text(harness)
    exe = out / ("diagnostic.exe" if os.name == "nt" else "diagnostic")
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                    "-DSNESRECOMP_SDL3=" + str(request.param), str(cfile), "-o", str(exe)], check=True)
    return exe, request.param


@pytest.mark.parametrize("scenario", ["normal", "hidden", "focused", "missing", "failed", "no-display"])
def test_current_context_and_window_observations(diagnostic, scenario):
    exe, sdl3 = diagnostic
    result = subprocess.run([str(exe), scenario], check=True, capture_output=True, text=True)
    text = result.stderr
    if scenario == "missing":
        assert text.count("<unavailable>") == 3
    else:
        assert "video GL vendor: Test Vendor" in text
        assert "video GL renderer: Test Renderer" in text
        assert "video GL version: 4.0 Test Version" in text
    query = "failed" if sdl3 and scenario == "failed" else "ok" if sdl3 else "sdl2-value"
    interval = 0 if query == "failed" else 1
    assert f"swap_interval={interval} swap_query={query}" in text
    expected = "visible=0 minimized=1 focused=0" if scenario == "hidden" else (
        "visible=1 minimized=0 focused=1" if scenario == "focused" else "visible=1 minimized=0 focused=0")
    assert expected in text

    if scenario == "no-display":
        assert "width=0 height=0 refresh_hz=0.000" in text
    else:
        assert "video GL display: Test Display width=1920 height=1080" in text
        assert ("refresh_hz=59.940" if sdl3 else "refresh_hz=60.000") in text
