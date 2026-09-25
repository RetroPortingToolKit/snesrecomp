/* See host_args.h for why the engine, not each port, owns these flags. */
#include "host_args.h"

#include "host_paths.h"

#include <stdio.h>
#include <string.h>

static const char *absolutize(const char *path, char *buf, size_t size) {
  /* Every path argument is resolved BEFORE the host anchors its cwd to the
   * executable's directory. Without this a relative --script or --framedump
   * meant one directory on an engine-hosted port and another on Zelda or SMW,
   * because only those two absolutized. */
  if (!path) return NULL;
  return snesrecomp_abspath(path, buf, size) ? buf : path;
}

void snesrecomp_host_args_usage(const char *program, const char *extra) {
  fprintf(stderr, "usage: %s [options] [<rom>]\n\n",
          program && program[0] ? program : "<game>");
  fprintf(stderr,
          "  <rom>                 ROM to run. Optional: the launcher or a\n"
          "                        cached rom.cfg supplies one otherwise.\n"
          "  --rom <path>          the same thing under a flag, for callers\n"
          "                        that build a command line programmatically.\n"
          "  --no-launcher         boot straight into the game.\n"
          "  --launcher            force the launcher even with a ROM given.\n"
          "  --config <path>       use this config.ini instead of anchoring to\n"
          "                        the executable's directory.\n"
          "  --paused              start paused.\n"
          "  --script <path>       run an input script.\n"
          "  --framedump <dir>     write frames to this directory.\n"
          "  --resume-state <path> load this save state before the first frame,\n"
          "                        then delete it (the in-game launcher's restart).\n"
          "  --help, -h            this message.\n");
  if (extra && extra[0]) fprintf(stderr, "%s", extra);
}

int snesrecomp_host_args_reject_unknown(int argc, char **argv,
                                        const char *program,
                                        const char *extra_usage) {
  for (int i = 1; i < argc; ++i) {
    if (!argv[i] || argv[i][0] != '-' || !argv[i][1]) continue;
    fprintf(stderr, "unknown option: %s\n\n", argv[i]);
    snesrecomp_host_args_usage(program ? program : argv[0], extra_usage);
    return 0;
  }
  return 1;
}

int snesrecomp_host_args_parse(int *argc_io, char ***argv_io,
                               SnesrecompHostArgs *out) {
  if (!argc_io || !argv_io || !out) return 0;
  memset(out, 0, sizeof(*out));

  int argc = *argc_io;
  char **argv = *argv_io;
  const char *program = (argc > 0 && argv[0]) ? argv[0] : "<game>";

  const char *rom_flag = NULL;   /* --rom */
  const char *rom_pos = NULL;    /* first bare positional */
  const char *config_raw = NULL;
  const char *script_raw = NULL;
  const char *framedump_raw = NULL;
  const char *resume_raw = NULL;

  /* Single order-independent pass. argv[0] is kept; everything this function
   * does not claim is compacted back into argv in its original order. */
  int w = 1;
  for (int i = 1; i < argc; ++i) {
    char *a = argv[i];
    if (!a) continue;

    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      out->help = 1;
      continue;
    }
    if (strcmp(a, "--no-launcher") == 0) { out->no_launcher = 1; continue; }
    if (strcmp(a, "--launcher") == 0) { out->force_launcher = 1; continue; }
    if (strcmp(a, "--paused") == 0) { out->start_paused = 1; continue; }

    /* Flags taking a value. A missing value is a usage error rather than a
     * silently dropped flag, which is how `--script` with nothing after it
     * used to behave. */
    if (strcmp(a, "--rom") == 0 || strcmp(a, "--config") == 0 ||
        strcmp(a, "--script") == 0 || strcmp(a, "--framedump") == 0 ||
        strcmp(a, "--resume-state") == 0) {
      if (i + 1 >= argc || !argv[i + 1]) {
        fprintf(stderr, "%s requires a path\n\n", a);
        snesrecomp_host_args_usage(program, NULL);
        return 0;
      }
      const char *v = argv[++i];
      if (strcmp(a, "--rom") == 0) rom_flag = v;
      else if (strcmp(a, "--config") == 0) config_raw = v;
      else if (strcmp(a, "--script") == 0) script_raw = v;
      else if (strcmp(a, "--resume-state") == 0) resume_raw = v;
      else framedump_raw = v;
      continue;
    }

    /* First bare word is the ROM; later ones stay for the port. */
    if (a[0] != '-' && a[0] != '\0' && !rom_pos) { rom_pos = a; continue; }

    argv[w++] = a;
  }
  argv[w] = NULL;
  *argc_io = w;
  *argv_io = argv;

  if (out->help) return 1;  /* the port prints usage and exits */

  if (out->force_launcher && out->no_launcher) {
    fprintf(stderr, "--launcher and --no-launcher are mutually exclusive\n\n");
    snesrecomp_host_args_usage(program, NULL);
    return 0;
  }

  /* An explicit flag beats a positional. Warn rather than fail: harnesses do
   * pass both, and refusing to start is worse than picking the explicit one. */
  const char *rom_raw = rom_flag ? rom_flag : rom_pos;
  if (rom_flag && rom_pos && strcmp(rom_flag, rom_pos) != 0)
    fprintf(stderr, "note: --rom %s overrides the positional ROM %s\n",
            rom_flag, rom_pos);

  out->rom = absolutize(rom_raw, out->rom_buf, sizeof(out->rom_buf));
  out->config_file = absolutize(config_raw, out->config_buf,
                                sizeof(out->config_buf));
  out->script_file = absolutize(script_raw, out->script_buf,
                                sizeof(out->script_buf));
  out->framedump_dir = absolutize(framedump_raw, out->framedump_buf,
                                  sizeof(out->framedump_buf));
  out->resume_state = absolutize(resume_raw, out->resume_buf,
                                 sizeof(out->resume_buf));
  return 1;
}
