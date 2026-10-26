"""Exercise the real host exit API against SDL's event queue.

ELF section collection lets this small fixture include the actual desktop
implementation without linking an entire title. Windows coverage comes from
the built-title shutdown check; PE retains unrelated host references here.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.skipif(sys.platform != "linux", reason="ELF host fixture")
@pytest.mark.parametrize("sdl", ["sdl2", "sdl3"])
def test_exit_uses_normal_events_and_ignores_replay(tmp_path, sdl):
    cc = shutil.which("cc")
    pkg = shutil.which("pkg-config")
    if not cc or not pkg:
        pytest.skip("C compiler and pkg-config required")
    available = subprocess.run([pkg, "--exists", sdl], check=False)
    if available.returncode:
        pytest.skip(f"{sdl} development package required")
    source = tmp_path / "exit_test.c"
    source.write_text(r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <stdarg.h>
#include "desktop/host_main.c"
static bool test_speculative;
bool RtlSpeculativeFrame(void) { return test_speculative; }
void host_report_breadcrumb(const char *fmt, ...) { (void)fmt; }
int main(void) {
#if SNESRECOMP_SDL3
  assert(SDL_Init(SDL_INIT_EVENTS));
#else
  assert(SDL_Init(SDL_INIT_EVENTS) == 0);
#endif
  SDL_Event event;
  while (SDL_PollEvent(&event)) {}
  assert(snesrecomp_host_frame_is_live());
  test_speculative = true;
  assert(!snesrecomp_host_frame_is_live());
  snesrecomp_request_exit();
  assert(!SDL_PollEvent(&event));
  test_speculative = false;
  g_host_replaying_frame = true;
  assert(!snesrecomp_host_frame_is_live());
  snesrecomp_request_exit();
  assert(!SDL_PollEvent(&event));
  g_host_replaying_frame = false;
  assert(snesrecomp_host_frame_is_live());
  snesrecomp_request_exit();
  assert(SDL_PollEvent(&event));
  assert(event.type == SDL_QUIT);
  assert(!SDL_PollEvent(&event));
  SDL_Quit();
  return 0;
}
''', encoding="utf-8")
    flags = shlex.split(subprocess.check_output(
        [pkg, "--cflags", "--libs", sdl], text=True))
    executable = tmp_path / "exit_test"
    subprocess.run([
        cc, "-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections",
        f"-DSNESRECOMP_SDL3={int(sdl == 'sdl3')}",
        f"-DLNG_SDL3={int(sdl == 'sdl3')}", "-DSDL_MAIN_HANDLED",
        "-I" + str(ROOT / "runner/src"),
        "-I" + str(ROOT / "runner/src/snes"),
        "-I" + str(ROOT / "runner/src/desktop"),
        "-I" + str(ROOT / "third_party"),
        "-I" + str(ROOT / "third_party/gl_core"),
        str(source), "-Wl,--gc-sections", *flags, "-lm", "-o", str(executable),
    ], check=True, capture_output=True, text=True, timeout=60)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = subprocess.check_output(
        [pkg, "--variable=libdir", sdl], text=True).strip()
    subprocess.run([str(executable)], env=env, check=True, timeout=15)
