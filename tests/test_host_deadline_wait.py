"""Compile the production waiter with ordinary API-return fixtures."""
import os
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]

@pytest.fixture(scope="module", params=[0, 1])
def waiter(request, tmp_path_factory):
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("C compiler unavailable")
    source = Path(os.environ.get("SNESRECOMP_WAIT_SOURCE", str(ROOT / "runner/src/desktop/host_main.c"))).read_text()
    end = source.index("static double DisplayRefresh(")
    begin = source.find("/* The main thread owns this timer")
    if begin < 0:
        begin = source.index("static void WaitUntil(double deadline)")
    body = source[begin:end].replace("#ifdef _WIN32", "#if TEST_WINDOWS")
    if "static void CloseDeadlineTimer(" not in body:
        body += "static void CloseDeadlineTimer(void) {}\n"
    out = tmp_path_factory.mktemp("waiter-" + str(request.param))
    prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
typedef uint32_t Uint32;
typedef void *HANDLE;
typedef void *HMODULE;
typedef void *LPSECURITY_ATTRIBUTES;
typedef const wchar_t *LPCWSTR;
typedef unsigned long DWORD;
typedef long long LONGLONG;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
#define WINAPI
#define TIMER_MODIFY_STATE 2u
#define SYNCHRONIZE 0x100000u
#define FALSE 0
#define INFINITE 0xffffffffu
#define WAIT_OBJECT_0 0u
static double now;
#if TEST_WINDOWS
static double armed_until;
#endif
static unsigned creates, arms, waits, closes, polls, sleeps;
static const char *scenario;
static double MonotonicSeconds(void) { return now; }
static double ProfileStart(void) { return now; }
static void ProfileEnd(unsigned stage, double started) { (void)stage; (void)started; }
enum { kProfileWait };
#if TEST_WINDOWS
static void host_report_breadcrumb(const char *fmt, ...) { (void)fmt; }
#endif
static void SDL_Delay(Uint32 ms) {
  if (ms) { sleeps++; now += (double)ms / 1000; }
  else { polls++; now += 0.0001; }
}
#if TEST_WINDOWS
static HANDLE create_timer(LPSECURITY_ATTRIBUTES attrs, LPCWSTR name, DWORD flags, DWORD access) {
  assert(!attrs && !name && flags == 2 && access == (TIMER_MODIFY_STATE | SYNCHRONIZE));
  creates++;
  if (!strcmp(scenario, "create-late")) now += 0.002;
  if (!strcmp(scenario, "create-expired")) now += 0.010;
  return !strcmp(scenario, "create-fail") ? NULL : (HANDLE)(uintptr_t)1;
}
static HMODULE GetModuleHandleW(LPCWSTR name) { assert(name); return (HMODULE)(uintptr_t)2; }
static void *GetProcAddress(HMODULE module, const char *name) {
  assert(module && !strcmp(name, "CreateWaitableTimerExW"));
  return !strcmp(scenario, "unsupported") ? NULL : (void *)create_timer;
}
static bool SetWaitableTimer(HANDLE timer, const LARGE_INTEGER *due, int period, void *cb, void *data, int resume) {
  assert(timer && due->QuadPart < 0 && !period && !cb && !data && !resume);
  arms++; armed_until = now - (double)due->QuadPart / 10000000.0;
  return strcmp(scenario, "arm-fail") != 0;
}
static DWORD WaitForSingleObject(HANDLE timer, DWORD duration) {
  assert(timer && duration == INFINITE); waits++;
  if (!strcmp(scenario, "wait-fail")) return 0xffffffffu;
  now = armed_until; return WAIT_OBJECT_0;
}
static bool CloseHandle(HANDLE timer) { assert(timer); closes++; return true; }
#endif
'''
    main = r'''
int main(int argc, char **argv) {
  assert(argc == 2); scenario = argv[1];
  if (!strcmp(scenario, "past")) {
    now = 1; WaitUntil(0); assert(!creates && !arms && !waits && !polls && !sleeps);
  } else {
    WaitUntil(0.005);
    assert(now >= 0.005);
#if TEST_WINDOWS
    if (!strcmp(scenario, "create-expired")) {
      if (!(now == 0.010 && creates == 1 && !arms && !waits && !polls && !sleeps)) return 1;
    } else if (!strcmp(scenario, "success") || !strcmp(scenario, "create-late")) {
      if (now > 0.0050001) return 1;
      if (!(creates == 1 && arms == 1 && waits == 1 && !polls && !sleeps)) return 1;
      WaitUntil(0.010); assert(creates == 1 && arms == 2 && waits == 2 && !closes);
    } else {
      assert(polls && sleeps);
      if (!strcmp(scenario, "arm-fail")) assert(arms == 1 && waits == 0 && closes == 1);
      if (!strcmp(scenario, "wait-fail")) assert(arms == 1 && waits == 1 && closes == 1);
      unsigned attempts = creates;
      WaitUntil(0.010); assert(creates == attempts);
    }
#else
    assert(!creates && !arms && !waits && polls && sleeps);
#endif
  }
  CloseDeadlineTimer();
  assert(closes == (creates && strcmp(scenario, "create-fail") ? 1u : 0u));
  return 0;
}
'''
    cfile = out / "waiter.c"; cfile.write_text(prelude + body + main)
    exe = out / ("waiter.exe" if os.name == "nt" else "waiter")
    unused = [] if "static bool WaitUntilNative(" in body else ["-Wno-unused-function", "-Wno-unused-variable"]
    subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *unused,
                    "-DTEST_WINDOWS=" + str(request.param), str(cfile), "-lm", "-o", str(exe)], check=True)
    return exe

@pytest.mark.parametrize("scenario", ["success", "unsupported", "create-fail", "arm-fail", "wait-fail", "past", "create-late", "create-expired"])
def test_native_wait_and_portable_failure_contract(waiter, scenario):
    subprocess.run([str(waiter), scenario], check=True)
