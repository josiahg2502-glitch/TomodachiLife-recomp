/* Tomodachi-Life-PC-Setup.exe: the one download. Its resources hold the release's files (FILES: a zip of the repository
 * at the release's tag with build\work\TomodachiLauncher.exe, SDL3.dll and, at the top, Tomodachi Life.exe and Tomodachi Life
 * Launcher.exe added: launcher\start.c); it unpacks them into the folder the
 * player confirms (C:\Games\TomodachiLife by default), puts a "Tomodachi Life" shortcut on the desktop and starts the
 * launcher there, whose INSTALL screen does the rest (the installer's Python, the player's game files, the build).
 * Nothing else is needed: it unpacks the zip itself (stb's inflate, third_party/stb).
 *   --target <folder>  install there without asking; --quiet: no window at all, no launcher started (tests)
 * Built by scripts/make_setup.py (clang; the zip, a manifest and an icon are written into the resources afterwards). */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG                /* (for its inflate: no image is read here) */
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

static const wchar_t kTitle[] = L"Tomodachi Life PC";
static const wchar_t kDefault[] = L"C:\\Games\\TomodachiLife";

static int ascii_only(const wchar_t *s) { for (; *s; s++) if (*s < 32 || *s > 126) return 0; return 1; }
static int is_empty_dir(const wchar_t *p) {
    wchar_t pat[MAX_PATH + 4];
    WIN32_FIND_DATAW fd;
    swprintf(pat, MAX_PATH + 4, L"%s\\*", p);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int empty = 1;
    do if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) empty = 0; while (empty && FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}
static void say(const wchar_t *text, UINT icon) { MessageBoxW(NULL, text, kTitle, MB_OK | icon); }

/* the folder dialog of Windows; a folder that is not empty and not already called TomodachiLife gets TomodachiLife inside it */
static int pick_folder(wchar_t *target) {
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg))) return 0;
    IFileOpenDialog_SetOptions(dlg, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(dlg, L"Where should Tomodachi Life PC go? (about 15 GB)");
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, NULL)) && SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item)) &&
        SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
        size_t n = wcslen(path);
        while (n > 3 && path[n - 1] == L'\\') path[--n] = 0;
        const wchar_t *base = wcsrchr(path, L'\\');
        if (!is_empty_dir(path) && _wcsicmp(base ? base + 1 : path, L"TomodachiLife"))
            swprintf(target, MAX_PATH, L"%s%sTomodachiLife", path, path[n - 1] == L'\\' ? L"" : L"\\");
        else
            wcsncpy(target, path, MAX_PATH - 1);
        ok = 1;
    }
    if (path) CoTaskMemFree(path);
    if (item) IShellItem_Release(item);
    IFileOpenDialog_Release(dlg);
    return ok;
}

/* 1: install to target, 2: choose another folder, 0: cancel */
static int ask(const wchar_t *target) {
    wchar_t first[MAX_PATH + 96];
    swprintf(first, MAX_PATH + 96, L"Install to %s\nAbout 15 GB of free space", target);
    TASKDIALOG_BUTTON buttons[] = {{1001, first}, {1002, L"Choose another folder..."}};
    TASKDIALOGCONFIG c = {sizeof c};
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = kTitle;
    c.pszMainIcon = TD_INFORMATION_ICON;
    c.pszMainInstruction = L"Install Tomodachi Life PC";
    c.pszContent = L"A native PC port of Tomodachi Life: Living the Dream. It is built on this PC from your own copy of the game "
                   L"(dumped from your Switch) and your console's prod.keys: nothing of the game is in this download.\n\n"
                   L"Next, the launcher opens, finds your game files and builds the game (about 40 minutes, once).";
    c.pButtons = buttons;
    c.cButtons = 2;
    int pressed = 0;
    if (SUCCEEDED(TaskDialogIndirect(&c, &pressed, NULL, NULL))) return pressed == 1001 ? 1 : pressed == 1002 ? 2 : 0;
    wchar_t text[MAX_PATH + 160];        // (no comctl32 6: the plain message box)
    swprintf(text, MAX_PATH + 160, L"Install Tomodachi Life PC to %s?\n\nYes: install there.  No: choose another folder.", target);
    const int r = MessageBoxW(NULL, text, kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
    return r == IDYES ? 1 : r == IDNO ? 2 : 0;
}

static uint32_t rd2(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd4(const uint8_t *p) { return rd2(p) | rd2(p + 2) << 16; }

/* The FILES resource, a zip, into target (the zip's top folder dropped). This used to write the zip to the disk and run Windows' tar.exe
 * in a hidden window on it: antivirus programs read a setup that starts hidden programs as a dropper (issues #21, #26, #31). */
static int unpack(const wchar_t *target, wchar_t *err, size_t errn) {
    HRSRC r = FindResourceW(NULL, L"FILES", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const uint8_t *z = g ? LockResource(g) : NULL;
    const DWORD size = r ? SizeofResource(NULL, r) : 0;
    DWORD end = size >= 22 ? size - 22 : 0;
    while (z && end > 0 && rd4(z + end) != 0x06054b50) end--;      /* the end of the central directory */
    if (!z || size < 22 || rd4(z + end) != 0x06054b50) { swprintf(err, errn, L"This setup holds no files (it was not built completely)."); return 0; }
    const int made = SHCreateDirectoryExW(NULL, target, NULL);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) {
        swprintf(err, errn, L"Could not make the folder %s (error %d). Choose another one.", target, made);
        return 0;
    }
    const uint8_t *e = z + rd4(z + end + 16);
    for (uint32_t i = rd2(z + end + 10); i > 0; i--, e += 46 + rd2(e + 28) + rd2(e + 30) + rd2(e + 32)) {
        const char *name = (const char *)e + 46;
        const int nlen = rd2(e + 28);
        const char *rel = memchr(name, '/', nlen);
        if (rd4(e) != 0x02014b50) { swprintf(err, errn, L"This setup's files are damaged. Please download it again."); return 0; }
        if (!rel || rel + 1 == name + nlen) continue;              /* the top folder */
        rel++;
        wchar_t w[MAX_PATH], path[2 * MAX_PATH], dir[2 * MAX_PATH];
        const int k = MultiByteToWideChar(CP_UTF8, 0, rel, (int)(name + nlen - rel), w, MAX_PATH - 1);
        w[k] = 0;
        for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
        swprintf(path, 2 * MAX_PATH, L"%s\\%s", target, w);
        const int is_dir = w[k - 1] == L'\\';
        wcscpy(dir, path);
        if (is_dir) dir[wcslen(dir) - 1] = 0; else *wcsrchr(dir, L'\\') = 0;
        SHCreateDirectoryExW(NULL, dir, NULL);
        if (is_dir) continue;
        const uint32_t method = rd2(e + 10), csize = rd4(e + 20), usize = rd4(e + 24);
        const uint8_t *local = z + rd4(e + 42), *comp = local + 30 + rd2(local + 26) + rd2(local + 28);
        char *buf = malloc(usize ? usize : 1);
        const int ok = buf && (method == 0 ? (memcpy(buf, comp, usize), 1) : method == 8 && (!usize || stbi_zlib_decode_noheader_buffer(buf, (int)usize, (const char *)comp, (int)csize) == (int)usize));
        HANDLE f = ok ? CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        DWORD wrote = 0;
        const BOOL done = f != INVALID_HANDLE_VALUE && WriteFile(f, buf, usize, &wrote, NULL) && wrote == usize;
        if (f != INVALID_HANDLE_VALUE) {                           /* the time in the zip, as tar kept it: the build goes by file times */
            FILETIME loc, utc;
            if (DosDateTimeToFileTime((WORD)rd2(e + 14), (WORD)rd2(e + 12), &loc) && LocalFileTimeToFileTime(&loc, &utc)) SetFileTime(f, NULL, NULL, &utc);
            CloseHandle(f);
        }
        free(buf);
        if (!ok) { swprintf(err, errn, L"This setup's files are damaged (%s). Please download it again.", w); return 0; }
        if (!done) { swprintf(err, errn, L"Could not write %s: is the game or the launcher still open, is the folder writable and the drive not full?", path); return 0; }
    }
    return 1;
}

/* "Tomodachi Life" on the desktop: the launcher (scripts\shortcuts.py makes the same one again at the end of the install) */
static void shortcut(const wchar_t *exe, const wchar_t *dir) {
    wchar_t desk[MAX_PATH], lnk[MAX_PATH + 32];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desk))) return;
    swprintf(lnk, MAX_PATH + 32, L"%s\\Tomodachi Life.lnk", desk);
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return;
    IShellLinkW_SetPath(sl, exe);
    IShellLinkW_SetWorkingDirectory(sl, dir);
    IShellLinkW_SetDescription(sl, L"Tomodachi Life PC");
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) { IPersistFile_Save(pf, lnk, TRUE); IPersistFile_Release(pf); }
    IShellLinkW_Release(sl);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)inst; (void)prev; (void)cmdline; (void)show;
    wchar_t target[MAX_PATH];
    int quiet = 0, given = 0, argc = 0;
    wcscpy(target, kDefault);
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--target") && i + 1 < argc) { wcsncpy(target, argv[++i], MAX_PATH - 1); given = 1; }
        else if (!wcscmp(argv[i], L"--quiet")) quiet = 1;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitCommonControls();
    while (!given) {
        const int choice = ask(target);
        if (choice == 0) return 1;
        if (choice == 2) { pick_folder(target); continue; }
        if (!ascii_only(target)) { say(L"Please choose a folder whose path has only plain letters (no accents or special characters): some build tools fail on others.", MB_ICONWARNING); continue; }
        if (wcslen(target) > 80) { say(L"Please choose a folder with a shorter path (for example C:\\Games\\TomodachiLife).", MB_ICONWARNING); continue; }
        break;
    }
    wchar_t err[2 * MAX_PATH], work[MAX_PATH + 16], exe[MAX_PATH + 48];
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    const int ok = unpack(target, err, sizeof err / sizeof err[0]);
    SetCursor(old);
    if (!ok) { if (!quiet) say(err, MB_ICONERROR); else fwprintf(stderr, L"%s\n", err); return 2; }
    {   /* 1.0.0's Install / Play .bat files, replaced by Tomodachi Life.exe and Tomodachi Life Launcher.exe (launcher\start.c):
         * gone from a folder this setup updates (the old Install .bat ran the installer through PowerShell) */
        static const wchar_t *const old[] = {L"Install Tomodachi Life.bat", L"Play Tomodachi Life.bat"};
        for (int i = 0; i < 2; i++) { wchar_t p[MAX_PATH + 48]; swprintf(p, MAX_PATH + 48, L"%s\\%s", target, old[i]); DeleteFileW(p); }
    }
    swprintf(work, MAX_PATH + 16, L"%s\\build\\work", target);
    swprintf(exe, MAX_PATH + 48, L"%s\\TomodachiLauncher.exe", work);
    if (quiet) return 0;
    shortcut(exe, work);
    if ((INT_PTR)ShellExecuteW(NULL, L"open", exe, NULL, work, SW_SHOWNORMAL) <= 32) {
        swprintf(err, sizeof err / sizeof err[0], L"The files are in %s, but the launcher did not start. Start %s yourself.", target, exe);
        say(err, MB_ICONWARNING);
        return 3;
    }
    return 0;
}
