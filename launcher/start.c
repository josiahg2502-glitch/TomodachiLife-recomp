/* The two programs at the top of the release's folder, in place of the Install / Play .bat files of 1.0.0:
 *   Tomodachi Life.exe           the game: the launcher with --play, straight into the game (its INSTALL screen when
 *                                nothing is installed yet); built with START_PLAY
 *   Tomodachi Life Launcher.exe  the launcher: PLAY, SETTINGS, SAVE DATA, INSTALL
 * Both start TomodachiLauncher.exe of the install location (the folder in build_location.txt beside this exe, else
 * build\) and end. No console, nothing but Windows itself. Built by scripts/make_setup.py, which puts them into the
 * release's files; scripts/launcher_art.py gives them the game's icon during the install. */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#ifdef START_PLAY
#define ARGS L" --play"
#else
#define ARGS L""
#endif

static int launcher_in(const wchar_t *work, wchar_t *exe, size_t n) {
    swprintf(exe, n, L"%s\\TomodachiLauncher.exe", work);
    return GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES;
}

/* build_location.txt: one line, the install location (scripts/build_location.py; UTF-8, perhaps a BOM or quotes) */
static int location(const wchar_t *dir, wchar_t *out, DWORD n) {
    wchar_t path[MAX_PATH + 32], w[MAX_PATH];
    char line[4 * MAX_PATH] = "", *s = line;
    swprintf(path, MAX_PATH + 32, L"%s\\build_location.txt", dir);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
    size_t k = strlen(s);
    while (k && strchr("\r\n \t\"", s[k - 1])) s[--k] = 0;
    while (*s == ' ' || *s == '\t' || *s == '"') s++;
    return *s && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, MAX_PATH) && ExpandEnvironmentStringsW(w, out, n) > 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)inst; (void)prev; (void)cmdline; (void)show;
    wchar_t dir[MAX_PATH], loc[MAX_PATH], work[MAX_PATH + 16], exe[MAX_PATH + 48], cmd[MAX_PATH + 64];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    int found = 0;
    if (location(dir, loc, MAX_PATH)) { swprintf(work, MAX_PATH + 16, L"%s\\work", loc); found = launcher_in(work, exe, MAX_PATH + 48); }
    if (!found) { swprintf(work, MAX_PATH + 16, L"%s\\build\\work", dir); found = launcher_in(work, exe, MAX_PATH + 48); }
    if (!found) {
        MessageBoxW(NULL, L"The launcher (build\\work\\TomodachiLauncher.exe) is not in this folder. Run Tomodachi-Life-PC-Setup.exe "
                          L"again (Releases of the TomodachiLife-recomp project on GitHub) and choose this folder.", L"Tomodachi Life", MB_OK | MB_ICONERROR);
        return 1;
    }
    swprintf(cmd, MAX_PATH + 64, L"\"%s\"%s", exe, ARGS);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, work, &si, &pi)) {
        wchar_t err[MAX_PATH + 96];
        swprintf(err, MAX_PATH + 96, L"Windows could not start %s (error %lu).", exe, GetLastError());
        MessageBoxW(NULL, err, L"Tomodachi Life", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
