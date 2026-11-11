"""ROM-free execution of the production screenshot command and its replies."""

import json
import os
from pathlib import Path
import shutil
import subprocess

import pytest


REPO = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def screenshot_command(tmp_path_factory):
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
    if not compiler:
        pytest.skip("C compiler required")
    source = (REPO / "runner/src/debug_server.c").read_text(encoding="utf-8")
    # Execute the actual command, including its JSON escaping, without linking
    # a game or starting a server. Only hardware, file destination and transport
    # are substituted; BMP writing and response formatting remain production C.
    start = source.find("static int debug_json_escape(")
    if start < 0:
        start = source.index("static void cmd_screenshot(")
    command = source[start:source.index("/* raster_journal", start)]
    formatter_start = source.index("static void send_fmt(")
    formatter = source[formatter_start:source.index("// ---- Command handlers ----", formatter_start)]
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define DEBUG_RESPONSE_CAPACITY 8192
struct Ppu { int extraLeftRight; uint8_t *renderBuffer; uint32_t renderPitch; };
static struct Ppu ppu, *g_ppu = &ppu;
static int snes_frame_counter = 37;
static uint8_t *s_composed_pixels, *s_screenshot_pixels;
static size_t s_screenshot_capacity;
static int s_composed_valid, s_composed_width = 5, s_composed_height = 3;
static int fail_open, opened, bmp_written;
static const char *expected_path;
static void lock_mutex(void) {}
static void unlock_mutex(void) {}
static void send_line(const char *line) { puts(line); }
static FILE *mock_open(const char *path, const char *mode) {
    assert(strcmp(path, expected_path) == 0 && strcmp(mode, "wb") == 0);
    opened++;
    return fail_open ? NULL : tmpfile();
}
static int mock_close(FILE *f) {
    unsigned char hdr[54];
    int w = s_composed_valid ? 5 : 264, h = s_composed_valid ? 3 : 224;
    assert(ftell(f) == 54 + ((w * 3 + 3) & ~3) * h);
    rewind(f); assert(fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    assert(hdr[0] == 'B' && hdr[1] == 'M');
    assert((hdr[18] | (hdr[19] << 8)) == w);
    bmp_written++;
    return fclose(f);
}
#define fopen mock_open
#define fclose mock_close
'''
    main = r'''
#undef fopen
#undef fclose
int main(int argc, char **argv) {
    assert(argc == 4);
    fail_open = strcmp(argv[2], "error") == 0;
    int too_long = strcmp(argv[2], "limit") == 0;
    s_composed_valid = strcmp(argv[3], "composed") == 0;
    uint8_t pixels[264 * 224 * 4] = {0};
    ppu.extraLeftRight = 4; ppu.renderBuffer = pixels; ppu.renderPitch = 264 * 4;
    s_composed_pixels = pixels;
    expected_path = argv[1][0] ? argv[1] : "debug_screenshot.bmp";
    cmd_screenshot(argv[1]);
    assert(opened == !too_long && bmp_written == (!fail_open && !too_long));
    free(s_screenshot_pixels);
    return 0;
}
'''
    root = tmp_path_factory.mktemp("screenshot-command")
    c_file = root / "screenshot.c"
    c_file.write_text(harness + formatter + command + main, encoding="utf-8")
    exe = root / ("screenshot.exe" if os.name == "nt" else "screenshot")
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(c_file), "-o", str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    return exe


@pytest.mark.parametrize("path", [
    r"C:\temp\gaia.bmp", r"C:\tools\test.bmp", "C:/temp/gaia.bmp",
    "gaia.bmp", "", 'quote"name.bmp', "controls\t\r\n\x01.bmp",
])
@pytest.mark.parametrize("outcome", ["success", "error"])
@pytest.mark.parametrize("surface", ["ppu", "composed"])
def test_screenshot_path_round_trips(screenshot_command, path, outcome, surface):
    result = subprocess.run([str(screenshot_command), path, outcome, surface],
                            capture_output=True, text=True, check=True)
    response = json.loads(result.stdout)
    assert response["path"] == (path or "debug_screenshot.bmp")
    if outcome == "error":
        assert response == {"error": "cannot open file", "path": response["path"]}
    else:
        assert response == {"ok": True, "path": response["path"],
                            "width": 5 if surface == "composed" else 264,
                            "height": 3 if surface == "composed" else 224,
                            "ws_extra": 4, "source": surface, "frame": 37}


def test_escaped_path_cannot_truncate_json(screenshot_command):
    result = subprocess.run([str(screenshot_command), "\\" * 7900, "limit", "ppu"],
                            capture_output=True, text=True, check=True)
    assert json.loads(result.stdout) == {"error": "screenshot path exceeds reply capacity"}
