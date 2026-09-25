/* See host_relaunch.h. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE 1   /* posix_spawn, environ */
#endif

#include "host_relaunch.h"

#include "host_paths.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif !defined(__ANDROID__)
#  include <spawn.h>
extern char **environ;
#endif

#if defined(_WIN32)
/* One argument, quoted by the rules CommandLineToArgvW and the CRT parse
 * with: backslashes are literal except when they precede a quote. */
static int AppendQuoted(char *out, size_t cap, size_t *len, const char *arg) {
  size_t n = *len;
#define PUT(c) do { if (n + 1 >= cap) return 0; out[n++] = (c); } while (0)
  if (n) PUT(' ');
  PUT('"');
  for (const char *p = arg; ; ++p) {
    size_t slashes = 0;
    while (*p == '\\') { ++slashes; ++p; }
    if (!*p) {
      for (size_t i = 0; i < slashes * 2; ++i) PUT('\\');
      break;
    }
    if (*p == '"') {
      for (size_t i = 0; i < slashes * 2 + 1; ++i) PUT('\\');
    } else {
      for (size_t i = 0; i < slashes; ++i) PUT('\\');
    }
    PUT(*p);
  }
  PUT('"');
#undef PUT
  out[n] = '\0';
  *len = n;
  return 1;
}
#endif

int snesrecomp_host_relaunch(int argc, const char *const *args) {
#if defined(__ANDROID__)
  /* An app is started by the system, not by itself (and posix_spawn needs
   * API 28). The in-game launcher's restart is desktop-only. */
  (void)argc; (void)args;
  return 0;
#else
  char exe[1024];
  if (!snesrecomp_exe_path(exe, sizeof(exe))) {
    fprintf(stderr, "[relaunch] cannot determine this executable's path\n");
    return 0;
  }
#if defined(_WIN32)
  char cmdline[8192];
  size_t len = 0;
  cmdline[0] = '\0';
  if (!AppendQuoted(cmdline, sizeof(cmdline), &len, exe)) return 0;
  for (int i = 0; i < argc; ++i)
    if (!args[i] || !AppendQuoted(cmdline, sizeof(cmdline), &len, args[i]))
      return 0;
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  memset(&pi, 0, sizeof(pi));
  si.cb = sizeof(si);
  if (!CreateProcessA(exe, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    fprintf(stderr, "[relaunch] CreateProcess failed (%lu): %s\n",
            (unsigned long)GetLastError(), cmdline);
    return 0;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 1;
#else
  enum { kMaxArgs = 32 };
  char *argv[kMaxArgs + 2];
  if (argc > kMaxArgs) return 0;
  argv[0] = exe;
  for (int i = 0; i < argc; ++i) argv[i + 1] = (char *)args[i];
  argv[argc + 1] = NULL;
  pid_t pid = 0;
  int rc = posix_spawn(&pid, exe, NULL, NULL, argv, environ);
  if (rc != 0) {
    fprintf(stderr, "[relaunch] posix_spawn(%s) failed: %s\n", exe, strerror(rc));
    return 0;
  }
  return 1;
#endif
#endif
}
