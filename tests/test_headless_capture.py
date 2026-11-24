"""Exercise capture outputs and script scheduling without SDL or a game image."""

import os
from pathlib import Path
import shutil
import subprocess
import wave

import pytest


@pytest.fixture(scope="module")
def capture_smoke(tmp_path_factory):
    cc = os.environ.get("CC") or shutil.which("gcc")
    if not cc:
        pytest.skip("C compiler unavailable")
    src = Path(__file__).resolve().parents[1] / "runner/src"
    root = tmp_path_factory.mktemp("headless-capture")
    driver = root / "smoke.c"
    driver.write_text(
        r'''
#include <assert.h>
#include "host_args.h"
#include "desktop/headless_capture.h"

int main(int argc, char **argv) {
  assert(argc == 4);
  const struct SnesrecompHostArgs args = {
    .capture_out = argv[1], .capture_wav = argv[2], .input_script = argv[3],
    .capture_width = 16, .capture_height = 16,
    .capture_from = 2, .capture_to = 8, .capture_every = 3,
  };
  HeadlessCapture_Configure(&args);
  for (uint32_t frame = 1; frame <= 8 && !HeadlessCapture_Failed(); ++frame) {
    const uint16_t seat3 = HeadlessCapture_Pad(2, frame);
    const uint16_t seat4 = HeadlessCapture_Pad(3, frame);
    assert(seat3 == (frame >= 2 && frame < 4 ? 0x80 : 0));
    assert(seat4 == (frame >= 3 && frame < 7 ? 0x100 : 0));
    const uint8_t pixel[] = {0x02, 0x01, 0xFF, 0};
    const int16_t samples[2048] = {0};
    HeadlessCapture_Present(pixel, 4, 1, 1, 0, 0, 16, 16, frame);
    HeadlessCapture_Audio(samples, HeadlessCapture_AudioFramesFor(frame), frame);
  }
  HeadlessCapture_Finish();
  return HeadlessCapture_Failed() ? 3 : 0;
}
''',
        encoding="utf-8",
    )
    exe = root / "capture-smoke.exe"
    subprocess.run(
        [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(src),
         str(driver), str(src / "desktop/headless_capture.c"), "-o", str(exe)],
        check=True,
    )
    return exe


@pytest.fixture
def capture_paths(tmp_path):
    script = tmp_path / "input.txt"
    script.write_text("7 4:-\n2 3:RIGHT\n4 3:-\n3 4:A\n", encoding="ascii")
    return tmp_path / "frames", tmp_path / "audio.wav", script


def test_picture_stride_keeps_continuous_audio(capture_smoke, capture_paths):
    frames, audio, script = capture_paths
    subprocess.run([capture_smoke, frames, audio, script], check=True, capture_output=True)
    assert sorted(path.name for path in frames.iterdir()) == [
        "frame_000002.png", "frame_000005.png", "frame_000008.png",
    ]
    with wave.open(str(audio), "rb") as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2, 2, 48000)
        assert wav.getnframes() == round(48000 * 8 / 60.0988) - round(48000 / 60.0988)


@pytest.mark.parametrize("failure", ["script", "wav", "frame"])
def test_capture_errors_are_visible(capture_smoke, capture_paths, failure):
    frames, audio, script = capture_paths
    if failure == "script":
        script = script.with_name("missing.txt")
    elif failure == "wav":
        audio = audio.parent / "missing" / audio.name
    else:
        frames.write_text("not a directory", encoding="ascii")
    result = subprocess.run([capture_smoke, frames, audio, script], capture_output=True)
    assert result.returncode == 3
    assert {"script": b"input script", "wav": b"cannot start WAV",
            "frame": b"failed to write frame"}[failure] in result.stderr
