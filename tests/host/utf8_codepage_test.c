/* Windows only: an executable linked with runner/windows/snesrecomp_utf8.manifest
 * runs with a UTF-8 process code page, so a narrow UTF-8 path with non-ASCII
 * characters (the "Mega Man X Recompilé" install folder that used to abort at
 * startup) opens through fopen and names the same file the wide API sees. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

int main(void) {
    if (GetACP() != CP_UTF8) {
        fprintf(stderr, "utf8_codepage_test: FAIL: process code page %u\n",
                (unsigned)GetACP());
        return 1;
    }
    const char *dir = "utf8_codepage_Recompil\xC3\xA9";
    const char *file = "utf8_codepage_Recompil\xC3\xA9/rom.sfc";
    CreateDirectoryA(dir, NULL);
    FILE *f = fopen(file, "wb");
    if (!f || fputs("x", f) < 0 || fclose(f) != 0) {
        fprintf(stderr, "utf8_codepage_test: FAIL: fopen of a UTF-8 path\n");
        return 1;
    }
    const DWORD attrs =
        GetFileAttributesW(L"utf8_codepage_Recompilé\\rom.sfc");
    remove(file);
    RemoveDirectoryA(dir);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "utf8_codepage_test: FAIL: wide API cannot see the file\n");
        return 1;
    }
    puts("utf8_codepage_test: PASS");
    return 0;
}
