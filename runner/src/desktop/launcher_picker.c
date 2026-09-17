#include "launcher_picker.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "comdlg32.lib")
#  endif
#endif

#ifndef _WIN32
/* Run one shell-wrapped native chooser and read its selected path. */
static int run_picker_cmd(const char *cmd, char *out, size_t max_len) {
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    char buf[1024];
    buf[0] = '\0';
    char *got = fgets(buf, sizeof(buf), p);
    int rc = pclose(p);
    if (!got) return 0;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    if (rc != 0 || n == 0 || n >= max_len) return 0;
    memcpy(out, buf, n + 1);
    return 1;
}
#endif

int snesrecomp_pick_rom_file(char *out, size_t max_len) {
    if (!out || max_len == 0) return 0;
#ifdef _WIN32
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter =
        "SNES ROMs (*.sfc;*.smc)\0*.sfc;*.smc\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = out;
    ofn.nMaxFile = (DWORD)max_len;
    ofn.lpstrTitle = "Select SNES ROM";
    /* The common dialog must not undo the executable-directory anchor. */
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
#else
    out[0] = '\0';
    static const char *const pickers[] = {
        "command -v zenity >/dev/null 2>&1 && "
        "zenity --file-selection --title='Select SNES ROM' "
        "--file-filter='SNES ROMs (.sfc .smc) | *.sfc *.smc *.SFC *.SMC' "
        "--file-filter='All files | *' 2>/dev/null",
        "command -v kdialog >/dev/null 2>&1 && "
        "kdialog --getopenfilename \"${HOME:-/}\" "
        "'*.sfc *.smc *.SFC *.SMC|SNES ROMs' 2>/dev/null",
        "command -v qarma >/dev/null 2>&1 && "
        "qarma --file-selection --title='Select SNES ROM' 2>/dev/null",
        "command -v osascript >/dev/null 2>&1 && "
        "osascript -e 'POSIX path of "
        "(choose file with prompt \"Select SNES ROM\")' 2>/dev/null",
    };
    for (size_t i = 0; i < sizeof(pickers) / sizeof(pickers[0]); i++)
        if (run_picker_cmd(pickers[i], out, max_len))
            return 1;
    fprintf(stderr,
            "[Launcher] No ROM specified and no graphical file chooser found "
            "(install zenity or kdialog), and no cached rom.cfg.\n"
            "Pass the ROM path as the first argument.\n");
    return 0;
#endif
}
