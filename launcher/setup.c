/* Tomodachi-Life-PC-Setup.exe: the one download. Its resources hold the release's files (PAYLOAD: a zip of the repository
 * at the release's tag with build\work\TomodachiLauncher.exe and SDL3.dll added); it unpacks them into the folder the
 * player confirms (C:\Games\TomodachiLife by default), puts a "Tomodachi Life" shortcut on the desktop and starts the
 * launcher there, whose INSTALL screen does the rest (the installer's Python, the player's game files, the build).
 * Nothing else is needed: Windows 10 1803 or later unpacks the zip with its own tar.exe.
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
#include <stdio.h>
#include <wchar.h>

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

static int run_wait(wchar_t *cmd, DWORD *code) {
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* the PAYLOAD resource into a temporary zip, unpacked into target by Windows' tar.exe (the zip's top folder dropped) */
static int unpack(const wchar_t *target, wchar_t *err, size_t errn) {
    HRSRC r = FindResourceW(NULL, L"PAYLOAD", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const void *data = g ? LockResource(g) : NULL;
    const DWORD size = r ? SizeofResource(NULL, r) : 0;
    if (!data || !size) { swprintf(err, errn, L"This setup holds no files (it was not built completely)."); return 0; }
    wchar_t tmp[MAX_PATH], zip[MAX_PATH], tar[MAX_PATH], cmd[3 * MAX_PATH + 64];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(zip, MAX_PATH, L"%sTomodachiLifeSetup_%lu.zip", tmp, GetCurrentProcessId());
    HANDLE f = CreateFileW(zip, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    DWORD wrote = 0;
    const BOOL ok = f != INVALID_HANDLE_VALUE && WriteFile(f, data, size, &wrote, NULL) && wrote == size;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) { swprintf(err, errn, L"Could not write %s (is the drive full?).", zip); DeleteFileW(zip); return 0; }
    const int made = SHCreateDirectoryExW(NULL, target, NULL);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) {
        swprintf(err, errn, L"Could not make the folder %s (error %d). Choose another one.", target, made);
        DeleteFileW(zip);
        return 0;
    }
    GetSystemDirectoryW(tar, MAX_PATH);
    wcscat(tar, L"\\tar.exe");
    swprintf(cmd, 3 * MAX_PATH + 64, L"\"%s\" -xf \"%s\" -C \"%s\" --strip-components=1", tar, zip, target);
    DWORD code = 1;
    const int ran = run_wait(cmd, &code);
    DeleteFileW(zip);
    if (!ran) { swprintf(err, errn, L"Windows' tar.exe could not be started: this needs Windows 10 (1803 or later) or Windows 11."); return 0; }
    if (code) { swprintf(err, errn, L"Unpacking into %s failed (tar.exe exit %lu): is the folder writable and the drive not full?", target, code); return 0; }
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
