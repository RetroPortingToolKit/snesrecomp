/* The engine-owned command line (runner/src/host_args.c), checked in
 * isolation: order independence, absolutized paths, value-less flags as
 * usage errors, and --resume-state, which the in-game launcher's restart
 * depends on. */
#include "host_args.h"

#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", what);
    fails++;
  }
}

static int is_absolute(const char *p) {
  if (!p) return 0;
  if (p[0] == '/' || p[0] == '\\') return 1;
  return p[0] && p[1] == ':';
}

static int ends_with(const char *s, const char *tail) {
  size_t n = s ? strlen(s) : 0, m = strlen(tail);
  return n >= m && strcmp(s + n - m, tail) == 0;
}

int main(void) {
  SnesrecompHostArgs a;

  {
    char *argv[] = { "game", "--resume-state", "saves/resume.sav", "--no-launcher",
                     "--rom", "game.sfc", "--port-flag", NULL };
    int argc = 7;
    char **av = argv;
    check(snesrecomp_host_args_parse(&argc, &av, &a), "restart command line parses");
    check(a.no_launcher, "--no-launcher");
    check(is_absolute(a.resume_state) && ends_with(a.resume_state, "resume.sav"),
          "--resume-state is absolutized");
    check(is_absolute(a.rom) && ends_with(a.rom, "game.sfc"), "--rom is absolutized");
    check(argc == 2 && strcmp(av[1], "--port-flag") == 0,
          "an unknown flag is left for the port");
  }
  {
    char *argv[] = { "game", "game.sfc", NULL };
    int argc = 2;
    char **av = argv;
    check(snesrecomp_host_args_parse(&argc, &av, &a), "a plain launch parses");
    check(a.resume_state == NULL, "no --resume-state means none");
  }
  {
    char *argv[] = { "game", "--resume-state", NULL };
    int argc = 2;
    char **av = argv;
    check(!snesrecomp_host_args_parse(&argc, &av, &a),
          "--resume-state without a path is a usage error");
  }
  {
    char *argv[] = { "game", "--launcher", "--no-launcher", NULL };
    int argc = 3;
    char **av = argv;
    check(!snesrecomp_host_args_parse(&argc, &av, &a),
          "--launcher with --no-launcher is a usage error");
  }

  if (fails) return 1;
  printf("ok: host_args\n");
  return 0;
}
