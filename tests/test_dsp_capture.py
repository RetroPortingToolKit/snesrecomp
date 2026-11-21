"""Compile the actual diagnostic recorder with bounded, ROM-free fake I/O."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def recorder(tmp_path_factory):
    cc = os.environ.get("CC") or shutil.which("gcc")
    if not cc and Path("C:/msys64/mingw64/bin/gcc.exe").is_file():
        cc = "C:/msys64/mingw64/bin/gcc.exe"
    if not cc:
        pytest.skip("C compiler unavailable")
    out = tmp_path_factory.mktemp("dsp-capture")
    source_path = Path(os.environ.get("SNESRECOMP_TEST_DSP_CAPTURE_SOURCE", ROOT / "runner/src/common_rtl.c"))
    source = source_path.read_text(encoding="utf-8")
    begin = source.index("static int s_dspout_enabled")
    end = source.index("/* ", source.index("\n}\n", source.index("static void recomp_dspout_capture", begin)) + 3)
    body = source[begin:end]
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_capture_cursor.h"
typedef int16_t int16;
#define DSP_SAMPLE_RING 8192
#define AUDIO_TRACE_PCM_RING 8192
typedef struct { uint32_t sampleWrite; int16 sampleBuffer[16384]; } Dsp;
typedef struct { uint64_t produced; } AudioTraceStats;
typedef struct { Dsp *dsp; } Apu;
typedef struct { Apu *apu; } Snes;
static Dsp dsp;
static Apu apu = { &dsp };
static Snes snes = { &apu }, *g_snes = &snes;
static uint64_t generation;
static uint64_t produced;
static int16 trace_samples[16384];
static int g_rtl_speculative_frame;
static unsigned writes, opens, flushes;
static uint64_t expected_first;
static const char *scenario;
static uint64_t RtlStateGeneration(void) { return generation; }
void audio_trace_get_stats(AudioTraceStats *out) { out->produced = produced; }
uint32_t audio_trace_copy_pcm(uint64_t first, uint32_t max, int16_t *out,
                                    uint64_t *oldest, uint64_t *total) {
    *oldest = produced > AUDIO_TRACE_PCM_RING ? produced - AUDIO_TRACE_PCM_RING : 0;
    *total = produced;
    if (!strcmp(scenario, "missing-history") || first < *oldest || first > produced) return 0;
    uint32_t count = produced - first < max ? (uint32_t)(produced - first) : max;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = (uint32_t)((first + i) & (AUDIO_TRACE_PCM_RING - 1)) * 2;
        out[i * 2] = trace_samples[index];
        out[i * 2 + 1] = trace_samples[index + 1];
    }
    return count;
}
static char *capture_getenv(const char *key) {
    assert(!strcmp(key, "SNESRECOMP_DSPOUT"));
    return !strcmp(scenario, "disabled") ? NULL : "fake-output";
}
static FILE *capture_fopen(const char *path, const char *mode) {
    assert(!strcmp(path, "fake-output") && !strcmp(mode, "wb"));
    ++opens;
    return !strcmp(scenario, "open-fail") ? NULL : (FILE *)&dsp;
}
static size_t capture_fwrite(const void *ptr, size_t size, size_t count, FILE *f) {
    const int16 *pair = ptr;
    uint32_t idx = ((expected_first + writes) & (DSP_SAMPLE_RING - 1)) * 2;
    assert(f == (FILE *)&dsp && size == sizeof(int16) && count == 2);
    assert(writes < DSP_SAMPLE_RING); /* Abort any unbounded reproduction. */
    assert(pair[0] == trace_samples[idx] && pair[1] == trace_samples[idx + 1]);
    ++writes;
    return !strcmp(scenario, "write-fail") ? 1 : 2;
}
static int capture_fflush(FILE *f) {
    assert(f == (FILE *)&dsp);
    ++flushes;
    return !strcmp(scenario, "flush-fail") ? EOF : 0;
}
#define getenv capture_getenv
#define fopen capture_fopen
#define fwrite capture_fwrite
#define fflush capture_fflush
''' + body + r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    scenario = argv[1];
    for (unsigned i = 0; i < 16384; ++i)
        dsp.sampleBuffer[i] = trace_samples[i] = (int16)(i * 17);
    generation = 1;
    dsp.sampleWrite = (uint32_t)(produced = expected_first = 10000);
    recomp_dspout_begin_frame();
    if (!strcmp(scenario, "wrap")) {
        ++generation;
        dsp.sampleWrite = (uint32_t)(produced = expected_first = UINT32_MAX - 2);
        recomp_dspout_begin_frame();
        dsp.sampleWrite = 2;
        produced += 5;
    } else if (!strcmp(scenario, "load")) {
        ++generation;
        dsp.sampleWrite = (uint32_t)(produced = expected_first = 100);
        recomp_dspout_begin_frame();
        dsp.sampleWrite += 534;
        produced += 534;
    } else if (!strcmp(scenario, "mid-frame-load")) {
        ++generation;
        dsp.sampleWrite = 100;
        produced = 100;
    } else if (!strcmp(scenario, "rewind")) {
        dsp.sampleWrite = 100;
        produced = 100;
    } else if (!strcmp(scenario, "overrun")) {
        dsp.sampleWrite += DSP_SAMPLE_RING + 1;
        produced += AUDIO_TRACE_PCM_RING + 1;
    } else if (!strcmp(scenario, "speculative")) {
        g_rtl_speculative_frame = 1;
        dsp.sampleWrite += 534;
        produced += 534;
        recomp_dspout_begin_frame();
        recomp_dspout_capture();
        assert(writes == 0 && opens == 0);
        g_rtl_speculative_frame = 0;
        dsp.sampleWrite = (uint32_t)(produced = expected_first);
        recomp_dspout_begin_frame();
        dsp.sampleWrite += 534;
        produced += 534;
    } else if (!strcmp(scenario, "fifo-drop")) {
        /* A full playback FIFO accepts nothing; native production still exists. */
        memset(dsp.sampleBuffer, 0, sizeof(dsp.sampleBuffer));
        produced += 534;
    } else {
        dsp.sampleWrite += 534;
        produced += 534;
    }
    recomp_dspout_capture();
    if (!strcmp(scenario, "disabled")) {
        assert(writes == 0 && opens == 0 && flushes == 0);
    } else {
        assert(writes == (!strcmp(scenario, "wrap") ? 5u : 534u));
        assert(opens == 1 && flushes == 1);
    }
    return 0;
}
'''
    cfile = out / "recorder.c"
    cfile.write_text(harness, encoding="utf-8")
    exe = out / ("recorder.exe" if os.name == "nt" else "recorder")
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I", str(ROOT / "runner/src"), str(cfile), "-o", str(exe)], check=True)
    return exe


@pytest.mark.parametrize("scenario", ["normal", "load", "wrap", "disabled", "speculative", "fifo-drop"])
def test_valid_capture(recorder, scenario):
    subprocess.run([str(recorder), scenario], check=True)


@pytest.mark.parametrize("scenario,diagnostic", [
    ("mid-frame-load", "invalid epoch"), ("rewind", "overwritten sample span"),
    ("overrun", "overwritten sample span"), ("open-fail", "cannot open"),
    ("write-fail", "sample write failed"), ("flush-fail", "sample flush failed"),
    ("missing-history", "unavailable or overwritten sample span"),
])
def test_invalid_capture_fails_run(recorder, scenario, diagnostic):
    result = subprocess.run([str(recorder), scenario], capture_output=True, text=True)
    assert result.returncode == 1
    assert "[dsp_capture]" in result.stderr and diagnostic in result.stderr


def test_frame_primes_after_title_load_hook_before_execution():
    source_path = Path(os.environ.get("SNESRECOMP_TEST_DSP_CAPTURE_SOURCE", ROOT / "runner/src/common_rtl.c"))
    source = source_path.read_text(encoding="utf-8")
    frame = source[source.index("bool RtlRunFrame("):]
    assert frame.index("g_rtl_game_info->frame_begin") < frame.index("recomp_dspout_begin_frame();")
    assert frame.index("recomp_dspout_begin_frame();") < frame.index("g_rtl_game_info->run_frame();")
    assert frame.index("rtl_sync_apu_frame_boundary();") < frame.index("recomp_dspout_capture();")
