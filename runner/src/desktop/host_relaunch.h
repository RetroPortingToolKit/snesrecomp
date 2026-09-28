/* Start a fresh copy of this executable, for the in-game launcher's restart.
 *
 * Some launcher edits cannot be applied to a live session: the presenter
 * backend, the audio device rate, the mod plan (plugins activate once, before
 * the first frame), the ROM itself. The host saves the machine, starts the
 * executable again with arguments that boot straight into that state, and
 * exits. This is the "start again" half; the host owns the rest. */
#pragma once

/* Spawn the running executable with `args` (argv[1..], not including the
 * program name), detached from this process. On an AppImage this starts the
 * .AppImage file rather than the binary inside its read-only mount. Returns 1
 * when the new process started. */
int snesrecomp_host_relaunch(int argc, const char *const *args);
