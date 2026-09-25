/* Engine-owned command line for every SNES port.
 *
 * Why this exists
 * ---------------
 * The desktop host's flags used to be defined in whichever main() a port
 * happened to have. Ports on the shared host_main.c got the full set; ports
 * with their own main() re-implemented a subset by hand, and the subsets did
 * not agree:
 *
 *   host_main.c (MMX, X2, X3, Star Fox)  --rom --no-launcher --launcher
 *                                        --config --paused --script --framedump
 *   Zelda, SMW                           --launcher --config --paused --script
 *                                        --framedump   (no --rom, no --no-launcher)
 *   Super Metroid, SMK, SMRPG            nothing at all: a positional ROM only
 *
 * So `--no-launcher` silently showed the launcher on Zelda, and SMRPG took the
 * flag itself to BE the ROM path and refused to start:
 *
 *     A verified Super Mario RPG (USA) ROM is required: --no-launcher
 *
 * The flags also disagreed on semantics, not just presence. host_main.c
 * consumed --config/--paused/--script/--framedump from the FRONT in that fixed
 * order, so `--script s --config c` quietly ignored --config. And Zelda and SMW
 * absolutized path arguments while host_main.c did not, even though the host
 * chdir()s to the executable's directory immediately afterwards -- which meant
 * a relative --script or --framedump resolved against the wrong directory
 * depending on which port you ran.
 *
 * This module is the single definition. Ports call it BEFORE their own parsing,
 * it consumes what it owns, and it leaves everything else in argv for the port.
 *
 * Contract
 * --------
 *   - Order-independent. Every engine flag may appear anywhere.
 *   - Path arguments are absolutized before the host anchors its cwd.
 *   - The ROM may be positional or `--rom <path>`; the flag wins if both.
 *   - `--launcher` with `--no-launcher` is a usage error, not a coin toss.
 *   - Unknown arguments are preserved for the port, in their original order.
 */
#ifndef SNESRECOMP_HOST_ARGS_H
#define SNESRECOMP_HOST_ARGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SNESRECOMP_HOST_ARGS_PATH_MAX = 1024 };

typedef struct SnesrecompHostArgs {
  /* Resolved ROM: --rom <path>, else the first positional. NULL when absent,
   * which is not an error -- the launcher or a cached rom.cfg supplies one. */
  const char *rom;
  /* --config <path>. NULL means "anchor to the executable's directory and use
   * the config.ini there", which is what the absence of the flag has always
   * meant; it is load-bearing, not merely a default. */
  const char *config_file;
  const char *script_file;    /* --script <path> */
  const char *framedump_dir;  /* --framedump <dir> */
  /* --resume-state <path>: a save state to load before the first frame and then
   * delete. Written by the in-game launcher when an edit needs a restart. */
  const char *resume_state;
  int start_paused;           /* --paused */
  int force_launcher;         /* --launcher */
  int no_launcher;            /* --no-launcher */
  int help;                   /* --help / -h: the port should print and exit 0 */

  /* Storage for the absolutized forms above. Not read directly. */
  char rom_buf[SNESRECOMP_HOST_ARGS_PATH_MAX];
  char config_buf[SNESRECOMP_HOST_ARGS_PATH_MAX];
  char script_buf[SNESRECOMP_HOST_ARGS_PATH_MAX];
  char framedump_buf[SNESRECOMP_HOST_ARGS_PATH_MAX];
  char resume_buf[SNESRECOMP_HOST_ARGS_PATH_MAX];
} SnesrecompHostArgs;

/* Consume the engine-owned arguments from *argc / *argv, in place.
 *
 * Pass argc/argv exactly as main() received them, INCLUDING argv[0]; the
 * program name is preserved as argv[0] on return so a port can still report
 * it. Unrecognized arguments remain in argv after argv[0], in order, for the
 * port's own parser.
 *
 * Returns 1 on success, 0 on a usage error (a message is printed to stderr).
 * On a usage error the port should exit non-zero rather than continue.
 */
int snesrecomp_host_args_parse(int *argc, char ***argv, SnesrecompHostArgs *out);

/* Print the engine-owned usage block to stderr. `program` may be NULL.
 * `extra` is an optional port-specific block appended verbatim, so a port with
 * its own flags documents them in the same place; pass NULL when there are
 * none. */
void snesrecomp_host_args_usage(const char *program, const char *extra);

/* Fail on any leftover argument that still looks like a flag. Ports call this
 * AFTER consuming their own, so an unknown flag is reported instead of being
 * silently treated as a ROM path -- the failure mode that made SMRPG refuse to
 * start with a message naming the flag as a missing ROM.
 *
 * Returns 1 when nothing unknown remains, 0 after printing a message. */
int snesrecomp_host_args_reject_unknown(int argc, char **argv,
                                        const char *program,
                                        const char *extra_usage);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_HOST_ARGS_H */
