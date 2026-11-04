/* Tomodachi Life: Living the Dream - PC launcher (TomodachiLauncher.exe, beside the game in build\work).
 *
 * Modelled on the Smash 4 PC launcher: a full-window picture of the island, the game's logo, and a menu for a pad, the
 * keyboard or the mouse (chevrons around the selected item, its value at the right), a status line, a shader note and
 * the version.
 *   PLAY       starts the game the way scripts\play.ps1 does (a copy of the exe, the same environment, the log in
 *              build\work\play_<date>.log), hides itself while it runs and says how it ended;
 *   GAME       the game's files: a folder holding exefs\main.elf and romfs\ (build\dump unless another is chosen);
 *   SETTINGS   the options of build\config\graphics.ini that the port reads, written when changed (comments and other
 *              keys are kept);
 *   SAVE DATA  backups of build\userdata into build\saves, restore (the current save is backed up first), importing a
 *              save from a folder (a console dump: Map.sav, Mii.sav, Player.sav), importing a Mii from a .ltd file
 *              (ShareMii / TomodachiShare: mii_ltd.h) over a resident, folders;
 *   CONTROLS   the controller SDL sees and the keyboard's keys; build\config\controllers.ini;
 *   MORE       logs, graphics.ini, the shader cache, the port's folder.
 * Pictures beside the exe, made from the player's own game files (scripts\launcher_art.py): title-logo.png, the game's
 * logo; launcher-background.jpg, the island. Without them: a plain gradient and the name in text.
 * --shot <file.bmp> [main|settings|saves|controls|more]: draws that screen off screen into a picture and exits.
 * --import <folder>: SAVE DATA > IMPORT A SAVE with that folder (no dialog), the result printed; for tests.
 * --import-mii <file.ltd> <slot>: IMPORT A MII over the resident in that slot (0..69), no dialog; for tests.
 * --bootstrap: the installer's private Python (boot::bootstrap), no window; the INSTALL screen's first step runs it.
 * --play: straight into the game, no window while it runs, gone when it is quit (Tomodachi Life.exe at the top of the
 *          folder, launcher\start.c); the window when the game cannot start or ends with an error.
 * Built by scripts\build_launcher.sh (clang, SDL3 of ..\ssbu-recomp\deps, third_party\stb). */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <urlmon.h>
#pragma comment(lib, "urlmon.lib")

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include "paint.h"      // before the stb_truetype implementation below
#include "mii_ltd.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef LAUNCHER_VERSION
#define LAUNCHER_VERSION "dev"
#endif

namespace fs = std::filesystem;
using namespace paint;

namespace {

/* ------------------------------------------------------------------ paths */
fs::path g_self, g_work, g_build, g_repo, g_ini, g_pads_ini, g_launcher_ini, g_userdata, g_saves, g_cache, g_exe, g_play_exe;
const wchar_t* const kReleases = L"https://github.com/josiahg2502-glitch/TomodachiLife-recomp/releases";      // the player's download

void paths_init() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    g_self = fs::path(p);
    g_work = g_self.parent_path();                      // build\work
    g_build = g_work.parent_path();
    g_repo = g_build.parent_path();
    g_ini = g_build / "config" / "graphics.ini";
    g_pads_ini = g_build / "config" / "controllers.ini";
    g_launcher_ini = g_build / "config" / "launcher.ini";
    g_userdata = g_build / "userdata";
    g_saves = g_build / "saves";
    g_cache = g_build / "cache" / "shaders";
    g_exe = g_work / "tomodachi_native.exe";
    g_play_exe = g_work / "tomodachi_play.exe";
}
bool is_file(const fs::path& p) { std::error_code e; return fs::is_regular_file(p, e); }
bool is_dir(const fs::path& p) { std::error_code e; return fs::is_directory(p, e); }

/* ------------------------------------------------------------------ --bootstrap: the installer's private Python
 * The INSTALL screen's first step runs this exe again as "TomodachiLauncher.exe --bootstrap" (its output goes to
 * build\logs\launcher_install.log): the python.org embeddable package into deps\python and the wheels of requirements.txt
 * (keep the pins the same), each checked against its SHA-256. A wheel is a zip: unpacking it is installing it, so neither pip
 * nor Python's own TLS is involved. pip stopped installs (code 1) when it was damaged, when an antivirus took files from it,
 * and behind proxies that Windows knows and pip does not (issue #22).
 * The launcher used to run that script in a hidden PowerShell with the execution policy bypassed, which is the shape of
 * malware: antivirus programs warned about it (2026-11-02). Now no PowerShell is involved: the download and the hash are
 * Windows calls and the zips are unpacked by Windows' own tar.exe, as Setup.exe does. */
namespace boot {
const wchar_t* const kEmbedUrl = L"https://www.python.org/ftp/python/3.13.16/python-3.13.16-embed-amd64.zip";
const char* const kEmbedSha = "97dae5274cc54867065e8d5a3226e48c35017ed332a0fdb0e27d5b5821961297";
const struct { const wchar_t* url; const char* file; const char* sha; } kWheels[] = {
    {L"https://files.pythonhosted.org/packages/d9/82/b9c06c870f3bd8767c201f1edbdf9e8dc34be5b0fbc5682c4f80fe948475/zstandard-0.25.0-cp313-cp313-win_amd64.whl",
     "zstandard-0.25.0-cp313-cp313-win_amd64.whl", "1f830a0dac88719af0ae43b8b2d6aef487d437036468ef3c2ea59c51f9d55fd5"},
    {L"https://files.pythonhosted.org/packages/d7/ed/1b16fb66a37e1b9e41370b93b2c614c7da761a5b2a41dc333f7af4639962/pillow-13.0.0-cp313-cp313-win_amd64.whl",
     "pillow-13.0.0-cp313-cp313-win_amd64.whl", "9dda0788a9932776a75d8bcc020076edc308ce2508b51f6739a45e3158b3fc42"},
    {L"https://files.pythonhosted.org/packages/80/18/3f8c882d5f729310c99aa784a51b329d4791efd3c43374ed4ef6f57806a4/pycryptodome-4.1.0-cp39-abi3-win_amd64.whl",
     "pycryptodome-4.1.0-cp39-abi3-win_amd64.whl", "f7bb8fcbfd72c840a17b67543b984018ead3247f7054bb729e24a8dda436eb35"},
    {L"https://files.pythonhosted.org/packages/18/e0/f06028aea741bbecb2a7e9648f4643235279a770c7ffaf70bd4860c73661/lz4-4.4.5-cp313-cp313-win_amd64.whl",
     "lz4-4.4.5-cp313-cp313-win_amd64.whl", "da68497f78953017deb20edff0dba95641cc86e7423dfadf7c0264e1ac60dc22"},
};

void say(const std::string& s) { printf("%s\n", s.c_str()); fflush(stdout); }      // the INSTALL screen shows the last line
std::wstring quote(const fs::path& p) { const std::wstring w = p.wstring(); return L"\"" + w + (!w.empty() && w.back() == L'\\' ? L"\\" : L"") + L"\""; }

std::string sha256(const fs::path& p) {                    // lower-case hex; "" when the file cannot be read
    std::string out;
    HCRYPTPROV prov = 0;
    HCRYPTHASH h = 0;
    FILE* f = _wfopen(p.wstring().c_str(), L"rb");
    if (f && CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) && CryptCreateHash(prov, CALG_SHA_256, 0, 0, &h)) {
        std::vector<unsigned char> buf(1 << 16);
        bool ok = true;
        for (size_t n; ok && (n = fread(buf.data(), 1, buf.size(), f)) > 0;) ok = CryptHashData(h, buf.data(), (DWORD)n, 0) != 0;
        unsigned char d[32];
        DWORD dn = sizeof d;
        if (ok && !ferror(f) && CryptGetHashParam(h, HP_HASHVAL, d, &dn, 0))
            for (DWORD i = 0; i < dn; i++) { char x[3]; snprintf(x, sizeof x, "%02x", d[i]); out += x; }
    }
    if (h) CryptDestroyHash(h);
    if (prov) CryptReleaseContext(prov, 0);
    if (f) fclose(f);
    return out;
}

bool fetch(const wchar_t* url, const fs::path& file, const char* sha) {      // a download kept from an earlier try is used while its hash is right
    if (is_file(file) && sha256(file) == sha) return true;
    std::error_code e;
    fs::remove(file, e);
    const std::string name = file.filename().u8string();
    say("Downloading " + name + " ...");
    const HRESULT hr = URLDownloadToFileW(nullptr, url, file.wstring().c_str(), 0, nullptr);
    if (FAILED(hr)) { char m[64]; snprintf(m, sizeof m, "0x%08lx", (unsigned long)hr); say("The download of " + name + " failed (" + m + "): is this PC online?"); return false; }
    const std::string got = sha256(file);
    if (got != sha) { fs::remove(file, e); say("The download of " + name + " is not the file expected (SHA-256 " + got + "); try again later."); return false; }
    return true;
}

int run(const std::wstring& cmd) {                         // a program writing into this process's output (the install log); its exit code
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g_repo.wstring().c_str(), &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

bool unzip(const fs::path& zip, const fs::path& dir) {     // Windows 10 1803 and later have tar.exe, which reads zips
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const int code = run(quote(fs::path(sys) / "tar.exe") + L" -xf " + quote(zip) + L" -C " + quote(dir));
    if (code) say("Unpacking " + zip.filename().u8string() + " failed (tar.exe exit " + std::to_string(code) + "): this needs Windows 10 (1803 or later) or 11.");
    return code == 0;
}

bool python_ok(const fs::path& exe) { return is_file(exe) && run(quote(exe) + L" -c \"import zstandard, PIL, Crypto, lz4\"") == 0; }

bool site_on(const fs::path& py) {                         // the embeddable package's pythonXY._pth has "#import site": site-packages on
    std::error_code e;
    for (const auto& de : fs::directory_iterator(py, e)) {
        const std::wstring n = de.path().filename().wstring();
        if (n.rfind(L"python", 0) != 0 || n.size() < 5 || n.compare(n.size() - 5, 5, L"._pth")) continue;
        std::string txt, out;
        if (FILE* f = _wfopen(de.path().wstring().c_str(), L"rb")) { char b[4096]; for (size_t k; (k = fread(b, 1, sizeof b, f)) > 0;) txt.append(b, k); fclose(f); }
        for (size_t i = 0; i < txt.size();) {
            size_t j = txt.find('\n', i);
            std::string line = txt.substr(i, j == std::string::npos ? std::string::npos : j - i);
            i = j == std::string::npos ? txt.size() : j + 1;
            const size_t a = line.find_first_not_of(" \t"), b = a == std::string::npos ? a : line.find_first_not_of(" \t", a + 1);
            if (a != std::string::npos && line[a] == '#' && b != std::string::npos && line.compare(b, 11, "import site") == 0) line = line.substr(b);
            out += line + (j == std::string::npos ? "" : "\n");
        }
        FILE* f = _wfopen(de.path().wstring().c_str(), L"wb");
        if (!f) return false;
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        return true;
    }
    return false;
}

bool packages(const fs::path& dl, const fs::path& py, const fs::path& exe) {      // site-packages and the wheels of requirements.txt
    const fs::path site = py / "Lib" / "site-packages";
    std::error_code e;
    if (!site_on(py)) { say("The Python package has no python._pth to change."); return false; }
    fs::create_directories(site, e);
    for (const auto& w : kWheels) {
        const fs::path whl = dl / w.file;
        if (!fetch(w.url, whl, w.sha) || !unzip(whl, site)) return false;
    }
    // the embeddable Python leaves the script's folder out of sys.path (its ._pth isolates it), and our scripts import their
    // neighbours: a .pth line puts the running script's folder first, as a normal Python does
    if (FILE* f = _wfopen((site / "ssbu_scriptdir.pth").wstring().c_str(), L"wb")) {
        fputs("import sys, os; a = sys.argv[0] if sys.argv else ''; (a and os.path.isfile(a) and sys.path.insert(0, os.path.dirname(os.path.abspath(a))))\r\n", f);
        fclose(f);
    }
    if (!python_ok(exe)) { say("The installer's Python does not start or lacks its packages (its error is above): is an antivirus holding files back?"); return false; }
    return true;
}

int bootstrap() {
    const fs::path dl = g_build / "downloads", py = g_repo / "deps" / "python", exe = py / "python.exe";
    std::error_code e;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);      // (URLDownloadToFile)
    if (!python_ok(exe)) {
        fs::create_directories(dl, e);
        const fs::path zip = dl / "python-3.13.16-embed-amd64.zip";
        if (!fetch(kEmbedUrl, zip, kEmbedSha)) return 1;
        fs::remove_all(py, e);                              // (a half-made one of an earlier try too)
        fs::create_directories(py, e);
        if (!unzip(zip, py) || !packages(dl, py, exe)) { fs::remove_all(py, e); return 1; }      // the next try unpacks the kept download again
    }
    fs::create_directories(g_build, e);
    if (FILE* f = _wfopen((g_build / "python_path.txt").wstring().c_str(), L"wb")) { fputs(exe.u8string().c_str(), f); fputs("\r\n", f); fclose(f); }
    say("The installer's Python is ready.");
    return 0;
}
} // namespace boot

void open_path(const fs::path& p) { ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void edit_file(const fs::path& p) { ShellExecuteW(nullptr, L"open", L"notepad.exe", p.wstring().c_str(), nullptr, SW_SHOWNORMAL); }

/* ------------------------------------------------------------------ a "key = value" file edited in place */
struct Ini {
    fs::path path;
    std::vector<std::string> lines;
    void load(const fs::path& p) {
        path = p;
        lines.clear();
        if (FILE* f = _wfopen(p.wstring().c_str(), L"rb")) {
            char buf[1024];
            while (fgets(buf, sizeof buf, f)) {
                std::string s = buf;
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                lines.push_back(s);
            }
            fclose(f);
        }
    }
    int find(const std::string& key) const {           // the line "key = value" (not a comment)
        for (size_t i = 0; i < lines.size(); ++i) {
            const char* s = lines[i].c_str();
            while (*s == ' ' || *s == '\t') s++;
            if (*s == ';' || *s == '#') continue;
            if (!strncmp(s, key.c_str(), key.size()) && (s[key.size()] == ' ' || s[key.size()] == '=' || s[key.size()] == '\t')) return int(i);
        }
        return -1;
    }
    std::string get(const std::string& key, const std::string& def) const {
        const int i = find(key);
        if (i < 0) return def;
        size_t eq = lines[i].find('=');
        if (eq == std::string::npos) return def;
        std::string v = lines[i].substr(eq + 1);
        if (size_t c = v.find_first_of(";#"); c != std::string::npos) v.resize(c);      // trailing comment
        const size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    }
    void set(const std::string& key, const std::string& val) {
        const std::string line = key + " = " + val;
        if (int i = find(key); i >= 0) lines[i] = line;
        else lines.push_back(line);
        save();
    }
    bool save() const {
        fs::create_directories(path.parent_path());
        FILE* f = _wfopen(path.wstring().c_str(), L"wb");
        if (!f) return false;
        for (const std::string& l : lines) fprintf(f, "%s\r\n", l.c_str());
        fclose(f);
        return true;
    }
};

/* ------------------------------------------------------------------ saves */
long long dir_size(const fs::path& dir, fs::file_time_type* newest) {
    long long total = 0;
    std::error_code e;
    for (auto it = fs::recursive_directory_iterator(dir, e); !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        if (!it->is_regular_file(e)) continue;
        total += (long long)it->file_size(e);
        if (newest && it->last_write_time(e) > *newest) *newest = it->last_write_time(e);
    }
    return total;
}
std::string when(fs::file_time_type t) {              // "Oct 25, 13:51"
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const time_t tt = std::chrono::system_clock::to_time_t(sys);
    tm lt;
    localtime_s(&lt, &tt);
    char b[32];
    strftime(b, sizeof b, "%b %d, %H:%M", &lt);
    return b;
}
bool shell_op(UINT func, const fs::path& from, const fs::path& to = {}) {
    std::wstring f = from.wstring(), t = to.wstring();
    f.push_back(0); t.push_back(0);                       // double-NUL lists
    SHFILEOPSTRUCTW op{};
    op.wFunc = func; op.pFrom = f.c_str(); op.pTo = to.empty() ? nullptr : t.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT | (func == FO_DELETE ? FOF_ALLOWUNDO : 0);
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}
bool copy_contents(const fs::path& from, const fs::path& to) { fs::create_directories(to); return shell_op(FO_COPY, from / "*", to); }
bool recycle(const fs::path& p) { return shell_op(FO_DELETE, p); }

/* ------------------------------------------------------------------ a small JSON reader (find_inputs.py --json, install.py's ##ssbu lines) */
struct Json {
    enum Kind { kNull, kBool, kNum, kStr, kArr, kObj } kind = kNull;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json& operator[](const char* key) const {
        static const Json none;
        for (const auto& kv : o) if (kv.first == key) return kv.second;
        return none;
    }
    std::string str() const { return kind == kStr ? s : std::string(); }
};
struct JsonReader {
    const char *p, *e;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool word(const char* w) { const size_t n = strlen(w); if (size_t(e - p) < n || memcmp(p, w, n)) return false; p += n; return true; }
    static void utf8(std::string& out, unsigned c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | c >> 12); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | c >> 18); out += char(0x80 | (c >> 12 & 0x3F)); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    bool hex4(unsigned& c) {
        if (e - p < 4) return false;
        c = 0;
        for (int i = 0; i < 4; i++) { const char h = *p++; c = c << 4 | unsigned(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10)); }
        return true;
    }
    bool string(std::string& out) {
        if (p >= e || *p != '"') return false;
        for (p++; p < e && *p != '"'; ) {
            if (*p != '\\') { out += *p++; continue; }
            if (++p >= e) return false;
            const char c = *p++;
            if (c == 'u') {
                unsigned u = 0, lo = 0;
                if (!hex4(u)) return false;
                if (u >= 0xD800 && u < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') { p += 2; if (!hex4(lo)) return false; u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); }
                utf8(out, u);
            } else out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool value(Json& v, int depth = 0) {
        ws();
        if (p >= e || depth > 64) return false;
        if (*p == '{') {
            v.kind = Json::kObj; p++; ws();
            if (p < e && *p == '}') { p++; return true; }
            for (;;) {
                std::string k;
                ws();
                if (!string(k)) return false;
                ws();
                if (p >= e || *p++ != ':') return false;
                v.o.emplace_back(std::move(k), Json());
                if (!value(v.o.back().second, depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            v.kind = Json::kArr; p++; ws();
            if (p < e && *p == ']') { p++; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.kind = Json::kStr; return string(v.s); }
        if (word("true")) { v.kind = Json::kBool; v.b = true; return true; }
        if (word("false")) { v.kind = Json::kBool; return true; }
        if (word("null")) return true;
        char* end = nullptr;
        v.n = strtod(p, &end);
        if (end == p) return false;
        v.kind = Json::kNum; p = end;
        return true;
    }
};
Json parse_json(const std::string& text) {
    JsonReader r{text.data(), text.data() + text.size()};
    Json v;
    if (!r.value(v)) v = Json();
    return v;
}

/* ------------------------------------------------------------------ the installer's scripts, run without a window
 * Their output lines are read on a thread and copied to a log. The process is in a job object that kills it, and every
 * process it started (setup.py, the compilers), when the launcher closes: the installer resumes where it was next time. */
class Child {
public:
    ~Child() { stop(); }
    bool start(const std::wstring& cmd, const fs::path& cwd, const fs::path& log) {
        stop();
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = si.hStdError = wr;
        si.hStdInput = nul;
        PROCESS_INFORMATION pi{};
        std::wstring c = cmd;
        const BOOL ok = CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, cwd.wstring().c_str(), &si, &pi);
        CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) { CloseHandle(rd); return false; }
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
            AssignProcessToJobObject(job_, pi.hProcess);
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        std::error_code e;
        fs::create_directories(log.parent_path(), e);
        FILE* lf = _wfopen(log.wstring().c_str(), L"ab");
        reader_ = std::thread([this, rd, lf] {
            char buf[4096];
            std::string part;
            DWORD n = 0;
            while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
                if (lf) { fwrite(buf, 1, n, lf); fflush(lf); }
                part.append(buf, n);
                for (size_t i; (i = part.find('\n')) != std::string::npos; part.erase(0, i + 1)) {
                    std::string line = part.substr(0, i);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    std::lock_guard<std::mutex> lock(m_);
                    lines_.push_back(std::move(line));
                }
            }
            if (!part.empty()) { std::lock_guard<std::mutex> lock(m_); lines_.push_back(part); }
            if (lf) fclose(lf);
            CloseHandle(rd);
        });
        return true;
    }
    bool running() const { return proc_ && WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT; }
    bool started() const { return proc_ != nullptr; }
    // after the process ended: its exit code, once every line was read
    int finish() {
        if (reader_.joinable()) reader_.join();
        DWORD code = 1;
        if (proc_) { GetExitCodeProcess(proc_, &code); CloseHandle(proc_); proc_ = nullptr; }
        if (job_) { CloseHandle(job_); job_ = nullptr; }
        return int(code);
    }
    std::vector<std::string> take_lines() { std::lock_guard<std::mutex> lock(m_); std::vector<std::string> v; v.swap(lines_); return v; }
    void stop() {
        if (job_) TerminateJobObject(job_, 1);
        else if (proc_) TerminateProcess(proc_, 1);
        if (proc_ || job_ || reader_.joinable()) finish();
        take_lines();
    }
private:
    HANDLE job_ = nullptr, proc_ = nullptr;
    std::thread reader_;
    std::mutex m_;
    std::vector<std::string> lines_;
};

/* ------------------------------------------------------------------ pictures */
Image load_image(SDL_Renderer* r, const fs::path& path) {
    int w = 0, h = 0, n = 0;
    FILE* f = _wfopen(path.wstring().c_str(), L"rb");
    if (!f) return {};
    stbi_uc* px = stbi_load_from_file(f, &w, &h, &n, 4);
    fclose(f);
    if (!px) return {};
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, nullptr, px, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(px);
    return {t, float(w), float(h)};
}

/* ------------------------------------------------------------------ settings shown in SETTINGS */
struct Choice { const char* value; const char* label; };
struct Option { const char* label; const char* key; const char* def; std::vector<Choice> choices; };
const std::vector<Option>& options() {
    static const std::vector<Option> o = {
        {"DISPLAY", "window", "windowed", {{"windowed", "Window"}, {"borderless", "Borderless"}, {"fullscreen", "Fullscreen"}}},
        {"WINDOW SIZE", "size", "1280x720", {{"1280x720", "1280 x 720"}, {"1600x900", "1600 x 900"}, {"1920x1080", "1920 x 1080"}, {"2560x1440", "2560 x 1440"}, {"3840x2160", "3840 x 2160"}}},
        {"RESOLUTION", "resolution_scale", "1.0", {{"0.5", "540p"}, {"0.6667", "720p"}, {"1.0", "1080p"}, {"1.5", "1620p"}, {"2.0", "4K"}, {"3.0", "3240p"}}},
        // as the game's PC Settings (menu.cpp) since 2026-11-01: 16:9 by default, no fixed 21:9 / 32:9 (the game reads them as 16:9)
        {"WIDESCREEN", "widescreen", "off", {{"off", "Off (16:9)"}, {"auto", "Fit the screen (up to 21:9)"}}},
        {"FRAME RATE", "frame_rate", "60", {{"60", "60 fps (drops to 30 when needed)"}, {"30", "30 fps (as on the console)"}}},
        {"VSYNC", "vsync", "auto", {{"auto", "Automatic"}, {"display", "Follow the display"}, {"clock", "Own 60 Hz clock"}, {"off", "Off"}}},
        {"VOLUME", "volume", "auto", {{"auto", "System"}, {"0", "0 %"}, {"10", "10 %"}, {"20", "20 %"}, {"30", "30 %"}, {"40", "40 %"}, {"50", "50 %"}, {"60", "60 %"}, {"70", "70 %"}, {"80", "80 %"}, {"90", "90 %"}, {"100", "100 %"}}},
        // the game's 13 message sets (romfs/Mals: USen EUen USfr EUfr EUde USes EUes EUit EUnl JPja KRko CNzh TWzh); "en" alone meant en-GB
        {"LANGUAGE", "language", "auto", {{"auto", "Automatic"}, {"en-US", "English (US)"}, {"en-GB", "English (UK)"}, {"fr-CA", "Francais (Canada)"}, {"fr", "Francais (France)"},
                                          {"de", "Deutsch"}, {"es-419", "Espanol (Latinoamerica)"}, {"es", "Espanol (Espana)"}, {"it", "Italiano"}, {"nl", "Nederlands"},
                                          {"ja", "Japanese"}, {"ko", "Korean"}, {"zh-Hans", "Chinese (Simplified)"}, {"zh-Hant", "Chinese (Traditional)"}}},
        {"IN-GAME MENU (F1)", "menu", "on", {{"on", "On"}, {"off", "Off"}}},
        {"FRAME-TIME COUNTER", "frame_time_overlay", "off", {{"off", "Off"}, {"on", "On"}}},
        // Local Play (the game's Mii and creation exchange) over a network instead of the console's radio (net_relay.c)
        {"LOCAL PLAY ONLINE", "online_mode", "off", {{"off", "Off"}, {"lan", "Same network"}, {"host", "Host: share a room code"}, {"join", "Join a friend's code"}}},
    };
    return o;
}
const char* const k_keys[][2] = {                         // nnpad_sdl.c g_kb_default
    {"A / confirm", "Z"}, {"B / back", "X"}, {"X / menu", "C"}, {"Y / residents", "V"}, {"Left stick", "Arrow keys"}, {"Camera", "T F G H"},
    {"D-pad", "I J K L"}, {"L / R", "Q / E"}, {"ZL / ZR", "A / S"}, {"+ / -", "Enter / Backspace"}, {"Fullscreen", "F11"}, {"Settings", "F1"}};

/* ------------------------------------------------------------------ the launcher */
enum class Screen { kMain, kSettings, kSaves, kControls, kMore, kMii, kInstall };
struct Item {
    std::string label, value;
    bool chevrons = false;     // a setting: left / right change the value
};

class Launcher {
public:
    Launcher(SDL_Window* w, SDL_Renderer* r) : window_(w), r_(r), paint_(r) {
        gfx_.load(g_ini);
        lini_.load(g_launcher_ini);
        font_.open({g_work / "fonts" / "launcher.ttf", g_work / "fonts" / "launcher.otf", "C:/Windows/Fonts/bahnschrift.ttf", "C:/Windows/Fonts/segoeui.ttf"});
        logo_ = load_image(r_, g_work / "title-logo.png");
        background_ = load_image(r_, g_work / "launcher-background.jpg");
        if (!background_.texture) background_ = load_image(r_, g_work / "launcher-background.png");
        refresh();
    }
    bool running() const { return !quit_; }
    std::string import_now(const char* dir) { import_save(fs::u8path(dir)); return note_; }      // --import (main)
    std::string import_mii_now(const char* file, int slot) {      // --import-mii (main)
        load_ltd(fs::u8path(file));
        if (screen_ != Screen::kMii) return note_;
        for (const auto& r : mii_slots_) if (r.first == slot) { import_mii(r.first, r.second); return note_; }
        return "There is no Mii in slot " + std::to_string(slot) + ".";
    }
    bool game_running() const { return game_.hProcess != nullptr; }
    // --auto-install <dump folder> (tests): the INSTALL screen's whole way with that dump and no questions; 0 = installed
    void auto_install(const std::string& dump) { auto_dump_ = dump; start_setup(); }
    // LAUNCHER_TEST_EXIT=<code>:<log file> with --shot: the status line after such an end
    void test_exit(const char* spec) {
        exit_code_ = DWORD(strtoul(spec, nullptr, 0));
        if (const char* c = strchr(spec, ':')) log_ = fs::u8path(c + 1);
        exit_reason_ = exit_reason(exit_code_);
    }
    void play_direct() {      // --play (Tomodachi Life.exe): the game at once, the window only if it cannot start or ends badly
        direct_ = true;
        play();
        if (!game_running()) { direct_ = false; SDL_ShowWindow(window_); SDL_RaiseWindow(window_); }
    }
    int auto_result() const { return auto_result_; }
    void open_screen(const std::string& s) {
        if (s == "settings") go(Screen::kSettings);
        if (s == "saves") go(Screen::kSaves);
        if (s == "controls") go(Screen::kControls);
        if (s == "more") go(Screen::kMore);
        if (s == "install") { setup_ = Setup::kReady; go(Screen::kInstall); }      // (--shot: the screen as before any file is found)
    }

    void event(SDL_Event& e) {
        SDL_ConvertEventToRenderCoordinates(r_, &e);
        switch (e.type) {
        case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (setup_ == Setup::kInstall && !quit_armed_) {      // closing stops the install: once more to be sure
                quit_armed_ = true;
                note_ = "Closing stops the install (it goes on where it was when you press PLAY again). Close again to stop.";
                note_ok_ = false;
                break;
            }
            quit_ = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            switch (e.key.key) {
            case SDLK_UP: case SDLK_W: move(-1); break;
            case SDLK_DOWN: case SDLK_S: move(1); break;
            case SDLK_LEFT: case SDLK_A: change(-1); break;
            case SDLK_RIGHT: case SDLK_D: change(1); break;
            case SDLK_RETURN: case SDLK_SPACE: case SDLK_KP_ENTER: if (!e.key.repeat) activate(); break;
            case SDLK_ESCAPE: case SDLK_BACKSPACE: if (!e.key.repeat) back(); break;
            case SDLK_F11: SDL_SetWindowFullscreen(window_, !(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN)); break;
            default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); refresh(); break;
        case SDL_EVENT_GAMEPAD_REMOVED: refresh(); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: move(-1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move(1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: change(-1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: change(1); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: activate(); break;      // A (positional, as the game)
            case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_BACK: back(); break;
            default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) stick_y_ = e.gaxis.value / 32767.0f;
            if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) stick_x_ = e.gaxis.value / 32767.0f;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (time_ > 0.3f && (e.motion.xrel != 0 || e.motion.yrel != 0)) mouse_moved_ = true;     // (not the motion a new window reports)
            if (int i = item_at(e.motion.x, e.motion.y); i >= 0 && mouse_moved_) selected_ = i;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (int i = item_at(e.button.x, e.button.y); i >= 0) {
                    selected_ = i;
                    if (list_[i].chevrons) change(e.button.x < layout_.right_chevron - layout_.h * 0.06f ? -1 : 1);
                    else activate();
                }
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP: if (e.button.button == 4 /* X1 */) back(); break;
        default: break;
        }
    }

    void update(float dt) {
        time_ += dt;
        const auto repeat = [&](float v, float& held, auto&& action) {      // the stick repeats while held
            if (std::fabs(v) < 0.5f) { held = 0; return; }
            if (held == 0 || held > 0.4f) { action(v > 0 ? 1 : -1); if (held > 0.4f) held = 0.28f; }
            held += dt;
        };
        repeat(stick_y_, held_y_, [&](int d) { move(d); });
        repeat(stick_x_, held_x_, [&](int d) { change(d); });
        selection_y_ = ease(selection_y_, float(selected_), dt, 18);
        fade_ = std::min(1.0f, fade_ + dt * 5.5f);
        if (game_.hProcess && WaitForSingleObject(game_.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(game_.hProcess, &code);
            CloseHandle(game_.hProcess); CloseHandle(game_.hThread);
            game_ = {};
            exit_code_ = code;
            exit_reason_ = code ? exit_reason(code) : std::string();
            session_min_ = int((SDL_GetTicks() - started_) / 60000);
            if (direct_ && code == 0) { quit_ = true; return; }      // --play: the game was quit normally, nothing to show
            direct_ = false;
            SDL_ShowWindow(window_); SDL_RaiseWindow(window_);
            refresh();
        }
        poll_setup();
        std::string chosen;
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(dialog_mutex_);
            chosen.swap(dialog_result_);
            std::swap(done, dialog_done_);
        }
        if (done && !chosen.empty() && dialog_for_ >= kForBase) {      // INSTALL: a game file, the update, the keys, a dump folder
            if (dialog_for_ == kForBase) { in_base_ = chosen; in_dump_.clear(); }
            else if (dialog_for_ == kForUpdate) in_update_ = chosen;
            else if (dialog_for_ == kForKeys) in_keys_ = chosen;
            else if (dialog_for_ == kForDump) { in_dump_ = chosen; in_base_.clear(); }
            build_items();
        } else if (done && !chosen.empty() && dialog_for_ == kForImport) import_save(fs::u8path(chosen));
        else if (done && !chosen.empty() && dialog_for_ == kForMii) load_ltd(fs::u8path(chosen));
        else if (done && !chosen.empty()) {
            game_dir_ = chosen;
            lini_.set("game", chosen);
            refresh();
            if (play_after_dialog_ && ready()) play();
        }
        if (done) play_after_dialog_ = false;
    }

    void draw(SDL_Texture* target = nullptr) {
        int w = 0, h = 0;
        if (target) { float fw, fh; SDL_GetTextureSize(target, &fw, &fh); w = int(fw); h = int(fh); }
        else SDL_GetCurrentRenderOutputSize(r_, &w, &h);
        {   // a long list (IMPORT A MII: up to 70 residents) scrolls: 14 items around the selection
            const int n = int(list_.size()), vis = std::min(n, kVisible);
            if (selected_ < first_) first_ = selected_;
            if (selected_ >= first_ + vis) first_ = selected_ - vis + 1;
            first_ = std::max(0, std::min(first_, n - vis));
        }
        layout_ = Layout(float(w), float(h), std::min<size_t>(list_.size(), kVisible), screen_ == Screen::kMain);
        const float W = layout_.w, H = layout_.h;
        SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r_, 10, 14, 24, 255);
        SDL_RenderClear(r_);
        draw_background();
        if (logo_.texture) {                                     // the logo, top left
            const float lh = H * 0.30f, lw = lh * logo_.width / logo_.height;
            paint_.image(logo_, {W * 0.055f, H * 0.055f, lw, lh});
        } else {
            font_.draw(r_, W * 0.07f, H * 0.17f, "TOMODACHI LIFE", H * 0.075f, kWhite);
            font_.draw(r_, W * 0.07f, H * 0.24f, "Living the Dream - PC", H * 0.04f, kGold);
        }
        // the menu: the selected item between chevrons
        const float alpha = fade_, slide = (1 - fade_) * H * 0.03f, px = H * layout_.px;
        const float sel_y = layout_.item_y(selection_y_ - float(first_)) - px * 0.36f;
        const float pulse = 0.85f + 0.15f * std::sin(time_ * 4.0f);
        const Color chev{1, 1, 1, alpha * pulse};
        paint_.chevron(layout_.left_chevron - slide, sel_y, H * 0.034f, -1, H * 0.0042f, chev);
        paint_.chevron(layout_.right_chevron + slide, sel_y, H * 0.034f, 1, H * 0.0042f, chev);
        for (size_t i = size_t(first_); i < list_.size() && int(i) < first_ + kVisible; ++i) {
            const bool on = int(i) == selected_;
            const Color c{1, 1, 1, alpha * (on ? 1.0f : 0.72f)};
            const float y = layout_.item_y(float(int(i) - first_)), x = layout_.text_x + slide;
            const float tw = font_.draw(r_, x, y, list_[i].label, px, c);
            if (list_[i].value.empty()) continue;
            const float vpx = px * 0.74f;
            const std::string v = fit(list_[i].value, vpx, layout_.right_chevron - x - tw - H * 0.08f);
            const float vx = layout_.right_chevron - H * 0.03f - font_.width(r_, v, vpx);
            const std::string& raw = list_[i].value;
            Color vc = raw.rfind("Found", 0) == 0 || raw.rfind("Chosen", 0) == 0 || raw == "On" || raw.rfind("Saved", 0) == 0 ? kGood
                     : raw == "Choose..." || raw == "Press again" || raw == "About 40 minutes" ? kGold
                     : raw == "Wrong folder" || raw == "Not installed" ? kBad : kDim;
            vc.a = c.a;
            font_.draw(r_, vx, y - px * 0.04f, v, vpx, vc);
        }
        if (screen_ == Screen::kControls) draw_keys();
        draw_status();
        if (installed()) draw_toast();      // (before the install there are no shaders to count)
        const std::string version = std::string("Version ") + LAUNCHER_VERSION;
        const float vpx = H * 0.030f;
        font_.draw(r_, W * 0.95f - font_.width(r_, version, vpx), H * 0.94f, version, vpx, kWhite);
    }

private:
    // main menu items
    enum { kPlay, kGame, kSettings, kSaves, kControls, kMore, kQuit };

    struct Layout {
        float w = 1600, h = 900, text_x = 0, left_chevron = 0, right_chevron = 0, top = 0.445f, step = 0.06f, px = 0.044f;
        Layout() = default;
        Layout(float w_, float h_, size_t n, bool main) : w(w_), h(h_) {
            text_x = std::max(w * 0.085f, h * 0.15f);
            left_chevron = text_x - h * 0.05f;
            right_chevron = text_x + std::min(w * 0.42f, h * 0.80f);
            const float span = main ? 0.34f : 0.40f;                 // from the top item to the last
            top = 0.43f;
            step = n > 1 ? std::min(0.075f, span / float(n - 1)) : 0;
            px = std::min(0.046f, step * 0.78f);
        }
        float item_y(float i) const { return h * (top + i * step); }
    } layout_;

    bool exe_ok() const { return is_file(g_exe); }
    // the game DLL is built from the player's dump by the installer: the runtime package alone (its zip unpacked and the
    // launcher started from there) has the exe but not this
    bool installed() const { return exe_ok() && is_file(g_work / "tomodachi_game.dll"); }
    // installed by the installer for another release than version.txt (1.0.0 recorded none; scripts/install.py writes
    // "version" into build\install_state.json when it finishes): PLAY runs INSTALL first, which takes this release's runtime
    // package and redoes only what changed. A build made without the installer (no install_state.json) is never asked.
    bool outdated() const {
        std::string st, want;
        if (FILE* f = _wfopen((g_build / "install_state.json").wstring().c_str(), L"rb")) { char b[4096]; for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) st.append(b, n); fclose(f); }
        if (FILE* f = _wfopen((g_repo / "version.txt").wstring().c_str(), L"rb")) { char b[64] = ""; if (fgets(b, sizeof b, f)) want = b; fclose(f); }
        while (!want.empty() && strchr("\r\n \t", want.back())) want.pop_back();
        return !st.empty() && !want.empty() && can_install() && parse_json(st)["version"].str() != want;
    }
    // an update keeps the built game: the new runtime is linked and the shaders prepared again for it
    std::string install_time() const { return installed() ? "About 20 minutes (update)" : "About 40 minutes"; }
    // exefs\main.elf of the game folder; a dump made by the installer keeps only main (the console's format) and setup.py
    // converts it into build\binaries\<version>\main.elf: the newest of those then.
    // shortcut: that one is taken for any game folder without main.elf, matching or not; a folder check would need the NSO
    fs::path main_elf() const {
        fs::path p = game_dir_ / "exefs" / "main.elf", best;
        if (is_file(p) || !is_file(game_dir_ / "exefs" / "main")) return p;
        fs::file_time_type newest{};
        std::error_code e;
        for (auto it = fs::directory_iterator(g_build / "binaries", e); !e && it != fs::directory_iterator(); it.increment(e)) {
            fs::path q = it->path() / "main.elf";
            std::error_code e2;
            if (is_file(q) && (best.empty() || fs::last_write_time(q, e2) > newest)) { best = q; newest = fs::last_write_time(q, e2); }
        }
        return best.empty() ? p : best;
    }
    bool game_ok() const { return is_file(main_elf()) && is_dir(game_dir_ / "romfs"); }
    // The game's version, told by the size of main's code (the first PT_LOAD of main.elf), as the exe's gamedll.c does
    std::string game_version() const {
        unsigned char h[64], ph[56];
        uint64_t phoff = 0, size = 0;
        FILE* f = _wfopen(main_elf().wstring().c_str(), L"rb");
        if (!f) return "";
        if (fread(h, 1, 64, f) == 64) {
            memcpy(&phoff, h + 0x20, 8);
            for (int i = 0, n = h[0x38] | h[0x39] << 8; i < n && !size; i++)
                if (!_fseeki64(f, (long long)(phoff + 56u * i), SEEK_SET) && fread(ph, 1, 56, f) == 56 && ph[0] == 1) memcpy(&size, ph + 32, 8);
        }
        fclose(f);
        return size == 0x25ce6a0 ? "1.0.0" : size == 0x256d250 ? "1.0.4" : "Unknown version";
    }
    bool ready() const { return installed() && game_ok(); }

    void refresh() {
        std::string g = lini_.get("game", "");
        game_dir_ = g.empty() ? g_build / "dump" : fs::path(g);
        fs::file_time_type newest{};
        save_size_ = is_dir(g_userdata) ? dir_size(g_userdata, &newest) : 0;
        save_when_ = save_size_ ? when(newest) : "";
        backups_.clear();
        std::error_code e;
        for (auto& d : fs::directory_iterator(g_saves, e))
            if (d.is_directory() && d.path().filename().string().rfind("backup_", 0) == 0) backups_.push_back(d.path());
        std::sort(backups_.rbegin(), backups_.rend());                     // newest first (the names sort by date)
        shaders_ = 0;
        if (FILE* f = _wfopen((g_cache / "pairs.txt").wstring().c_str(), L"rb")) {
            int c;
            while ((c = fgetc(f)) != EOF) shaders_ += c == '\n';
            fclose(f);
        }
        pad_name_.clear();
        int n = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
            if (n > 0) if (const char* nm = SDL_GetGamepadNameForID(ids[0])) pad_name_ = nm;
            SDL_free(ids);
        }
        build_items();
    }

    std::string option_value(const Option& o) const {
        std::string v = o.key == std::string("size") ? gfx_.get("width", "1280") + "x" + gfx_.get("height", "720") : gfx_.get(o.key, o.def);
        for (const Choice& c : o.choices) if (!_stricmp(c.value, v.c_str())) return c.label;
        return v;      // a value written by hand
    }

    void build_items() {
        list_.clear();
        switch (screen_) {
        case Screen::kMain:
            list_ = {{"PLAY", game_running() ? "Running" : outdated() ? "Updates first" : ""},
                     {"GAME", !installed() ? "Not installed" : game_ok() ? game_version() : lini_.get("game", "").empty() ? "Choose..." : "Wrong folder"},
                     {"SETTINGS", ""},
                     {"SAVE DATA", save_size_ ? "Saved " + save_when_ : "New island"},
                     {"CONTROLS", pad_name_.empty() ? "Keyboard" : pad_name_},
                     {"MORE", ""},
                     {"QUIT", ""}};
            break;
        case Screen::kSettings: {
            for (const Option& o : options()) list_.push_back({o.label, option_value(o), true});
            const std::string code = gfx_.get("online_join", "");
            list_.push_back({"ROOM CODE TO JOIN", code.empty() ? "Copy a friend's code, then press" : code + "  -  press to paste another"});
            list_.push_back({"BACK", ""});
            break;
        }
        case Screen::kSaves:
            list_.push_back({"BACK UP NOW", ""});
            list_.push_back({"IMPORT A SAVE", "Choose a folder"});
            list_.push_back({"IMPORT A MII", "Choose a .ltd file"});
            for (size_t i = 0; i < backups_.size() && i < 4; ++i) {
                fs::file_time_type t{};
                dir_size(backups_[i], &t);
                list_.push_back({"BACKUP " + std::to_string(i + 1), armed_ == int(list_.size()) ? "Press again" : when(t) + "  -  Restore"});
            }
            list_.push_back({"OPEN SAVE FOLDER", ""});
            list_.push_back({"OPEN BACKUPS FOLDER", ""});
            list_.push_back({"BACK", ""});
            break;
        case Screen::kControls:
            list_ = {{"CONTROLLER", pad_name_.empty() ? "None connected" : pad_name_}, {"EDIT CONTROLLERS.INI", ""}, {"BACK", ""}};
            break;
        case Screen::kMii:      // the residents the .ltd's Mii can replace
            for (const auto& r : mii_slots_) list_.push_back({r.second, armed_ == int(list_.size()) ? "Press again to replace" : "Replace"});
            list_.push_back({"BACK", ""});
            break;
        case Screen::kMore: {
            char mb[32];
            const long long cache = is_dir(g_cache) ? dir_size(g_cache, nullptr) : 0;
            snprintf(mb, sizeof mb, "%.0f MB", cache / 1048576.0);
            list_ = {{"OPEN LOGS FOLDER", ""}, {"EDIT GRAPHICS.INI", ""}, {"CLEAR SHADER CACHE", armed_ == 2 ? "Press again" : mb}, {"OPEN PORT FOLDER", ""}, {"BACK", ""}};
            break;
        }
        case Screen::kInstall: {
            const auto name = [](const std::string& p, const char* none, bool found) {
                if (p.empty()) return std::string(none);
                return std::string(found ? "Found: " : "Chosen: ") + fs::u8path(p).filename().u8string();
            };
            if (setup_ == Setup::kBootstrap || setup_ == Setup::kScan) list_ = {{"GETTING READY", "One moment"}, {"BACK", ""}};
            else if (setup_ == Setup::kInstall) list_ = {{"STOP THE INSTALL", armed_ == 0 ? "Press again" : "You can go on later"}};
            else if (setup_ == Setup::kFailed) list_ = {{"TRY AGAIN", ""}, {"OPEN THE LOG", ""}, {"BACK", ""}};
            else if (!in_dump_.empty())
                list_ = {{"GAME FOLDER", name(in_dump_, "", in_auto_dump_)}, {"USE GAME FILES INSTEAD", "Choose..."}, {"INSTALL", install_time()}, {"BACK", ""}};
            else {
                const bool ok = !in_base_.empty() && !in_keys_.empty();
                list_ = {{"GAME", name(in_base_, "Choose...", in_auto_base_)}, {"UPDATE 1.0.4", name(in_update_, "Optional: Choose...", in_auto_update_)},
                         {"PROD.KEYS", name(in_keys_, "Choose...", in_auto_keys_)}, {"OR A DUMP FOLDER", "Choose..."},
                         {"INSTALL", ok ? install_time() : "Choose the game and keys first"}, {"BACK", ""}};
            }
            break;
        }
        }
        if (selected_ >= int(list_.size())) selected_ = int(list_.size()) - 1;
    }

    void go(Screen s) { screen_ = s; selected_ = 0; selection_y_ = 0; first_ = 0; fade_ = 0; armed_ = -1; note_.clear(); build_items(); }
    void move(int d) { const int n = int(list_.size()); selected_ = (selected_ + d + n) % n; if (armed_ >= 0) { armed_ = -1; build_items(); } }
    void back() {
        if (screen_ == Screen::kInstall && setup_ == Setup::kInstall) return;      // STOP THE INSTALL is the way out
        if (screen_ == Screen::kInstall) { child_.stop(); setup_ = Setup::kIdle; }
        if (screen_ == Screen::kMii) go(Screen::kSaves); else if (screen_ != Screen::kMain) go(Screen::kMain);
    }

    void change(int d) {
        if (screen_ != Screen::kSettings || selected_ >= int(options().size())) return;
        const Option& o = options()[selected_];
        const std::string cur = o.key == std::string("size") ? gfx_.get("width", "1280") + "x" + gfx_.get("height", "720") : gfx_.get(o.key, o.def);
        int i = 0;
        const int n = int(o.choices.size());
        while (i < n && _stricmp(o.choices[i].value, cur.c_str())) i++;
        i = i == n ? 0 : (i + d + n) % n;
        if (o.key == std::string("size")) {
            const std::string v = o.choices[i].value;
            gfx_.set("width", v.substr(0, v.find('x')));
            gfx_.set("height", v.substr(v.find('x') + 1));
        } else gfx_.set(o.key, o.choices[i].value);
        build_items();
    }

    void activate() {
        const int i = selected_;
        switch (screen_) {
        case Screen::kMain:
            if (i == kPlay) play();
            else if (i == kGame && !installed() && can_install()) start_setup();
            else if (i == kGame) choose_game();
            else if (i == kSettings) go(Screen::kSettings);
            else if (i == kSaves) go(Screen::kSaves);
            else if (i == kControls) go(Screen::kControls);
            else if (i == kMore) go(Screen::kMore);
            else if (i == kQuit) quit_ = true;
            break;
        case Screen::kSettings:
            if (i == int(list_.size()) - 1) back();
            else if (i == int(options().size())) paste_room_code();
            else change(1);
            break;
        case Screen::kSaves: {
            const int nb = int(std::min<size_t>(backups_.size(), 4));
            if (i == 0) { note_ = backup_now("") ? "Backup made." : "There is no save to back up yet."; note_ok_ = save_size_ > 0; refresh(); }
            else if (i == 1) choose_folder(kForImport, "");
            else if (i == 2) choose_ltd();
            else if (i <= nb + 2) {
                if (armed_ != i) { armed_ = i; build_items(); break; }
                armed_ = -1;
                restore(backups_[i - 3]);
            } else if (i == nb + 3) { fs::create_directories(g_userdata); open_path(g_userdata); }
            else if (i == nb + 4) { fs::create_directories(g_saves); open_path(g_saves); }
            else back();
            break;
        }
        case Screen::kMii:
            if (i >= int(mii_slots_.size())) { go(Screen::kSaves); break; }
            if (armed_ != i) { armed_ = i; build_items(); break; }
            armed_ = -1;
            import_mii(mii_slots_[i].first, mii_slots_[i].second);
            break;
        case Screen::kControls:
            if (i == 1) edit_file(g_pads_ini);
            else if (i == 2) back();
            break;
        case Screen::kInstall:
            if (setup_ == Setup::kInstall) {                    // STOP THE INSTALL, pressed twice
                if (armed_ != 0) { armed_ = 0; build_items(); break; }
                child_.stop();
                setup_ = Setup::kIdle;
                go(Screen::kMain);
                note_ = "The install stopped. Press PLAY to go on where it was.";
                break;
            }
            if (setup_ == Setup::kBootstrap || setup_ == Setup::kScan) { if (i == 1) back(); break; }
            if (setup_ == Setup::kFailed) {
                if (i == 0) start_setup();
                else if (i == 1) open_path(g_build / "logs" / "launcher_install.log");
                else back();
                break;
            }
            if (!in_dump_.empty()) {
                if (i == 0) choose_folder(kForDump, in_dump_);
                else if (i == 1) { in_dump_.clear(); build_items(); }
                else if (i == 2) start_install();
                else back();
                break;
            }
            if (i == 0) choose_file(kForBase, "Tomodachi Life (base game)", "nsp;nsz;xci;xcz");
            else if (i == 1) choose_file(kForUpdate, "The update 1.0.4", "nsp;nsz");
            else if (i == 2) choose_file(kForKeys, "Your console's keys", "keys");
            else if (i == 3) choose_folder(kForDump, "");
            else if (i == 4) { if (!in_base_.empty() && !in_keys_.empty()) start_install(); }
            else back();
            break;
        case Screen::kMore:
            if (i == 0) open_path(g_work);
            else if (i == 1) edit_file(g_ini);
            else if (i == 2) {
                if (armed_ != 2) { armed_ = 2; build_items(); break; }
                armed_ = -1;
                note_ = recycle(g_cache) ? "Shader cache moved to the Recycle Bin." : "Nothing to clear.";
                note_ok_ = true;
                refresh();
            } else if (i == 3) open_path(g_repo);
            else back();
            break;
        }
    }

    /* Local Play over the internet (net_relay.c): the host's game shows its room code in its title and copies it; the
     * friend copies it from a chat and pastes it here. A code is 10 Crockford base-32 characters, shown as XXXXX-XXXXX. */
    void paste_room_code() {
        char* clip = SDL_GetClipboardText();
        std::string code;
        for (const char* p = clip ? clip : ""; *p && code.size() < 11; p++) {
            const char ch = (char)toupper((unsigned char)*p);
            if (isalnum((unsigned char)ch)) { if (code.size() == 5) code += '-'; code += ch; }
        }
        SDL_free(clip);
        note_ok_ = code.size() == 11;
        if (!note_ok_) { note_ = "No room code on the clipboard: copy the 10-character code your friend sent (XXXXX-XXXXX)."; build_items(); return; }
        gfx_.set("online_join", code);
        gfx_.set("online_mode", "join");
        note_ = "Room code " + code + " saved: LOCAL PLAY ONLINE is set to Join. Start the game and open Local Play.";
        build_items();
    }

    bool backup_now(const std::string& tag) {
        if (!is_dir(g_userdata)) return false;
        time_t now = time(nullptr);
        tm lt;
        localtime_s(&lt, &now);
        char name[64];
        strftime(name, sizeof name, "backup_%Y%m%d_%H%M%S", &lt);
        return copy_contents(g_userdata, g_saves / (std::string(name) + (tag.empty() ? "" : "_" + tag)));
    }
    void restore(const fs::path& from) {
        bool ok = !is_dir(g_userdata) || backup_now("before_restore");      // the current save first: nothing is lost
        if (ok) { recycle(g_userdata); ok = copy_contents(from, g_userdata); }
        note_ = ok ? "Save restored. Your previous save is in the backups." : "The restore failed: your previous save is in the backups.";
        note_ok_ = ok;
        refresh();
    }

    /* A save from elsewhere (a dump made on the console with JKSV or Checkpoint: Map.sav, Mii.sav and Player.sav, in the
     * folder chosen or up to three levels below it, as in JKSV\<game>\<date>; the newest if several). The current save is
     * backed up first; a commit the game left half done (save.commit, nnfs.c) would be finished over the new files at the
     * next start: it goes too. */
    static fs::path find_save(const fs::path& dir, int depth = 3) {
        if (is_file(dir / "Map.sav") && is_file(dir / "Mii.sav") && is_file(dir / "Player.sav")) return dir;
        fs::path best;
        fs::file_time_type best_t{};
        std::error_code e;
        if (depth > 0)
            for (auto& d : fs::directory_iterator(dir, e))
                if (d.is_directory(e))
                    if (fs::path f = find_save(d.path(), depth - 1); !f.empty()) {
                        const auto t = fs::last_write_time(f / "Mii.sav", e);
                        if (best.empty() || t > best_t) { best = f; best_t = t; }
                    }
        return best;
    }
    static bool game_process_running() {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        bool found = false;
        if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe))
            do found = !_wcsicmp(pe.szExeFile, L"tomodachi_play.exe") || !_wcsicmp(pe.szExeFile, L"tomodachi_native.exe");
            while (!found && Process32NextW(snap, &pe));
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        return found;
    }
    static void drop_pending_commit() {      // a commit the game left half done (nnfs.c) would be finished over our files
        std::error_code e;
        for (auto& d : fs::directory_iterator(g_userdata, e)) {
            const std::string n = d.path().filename().string();
            if (n == "save.commit" || n.rfind("save.journal.", 0) == 0) fs::remove(d.path(), e);
        }
    }
    void import_save(const fs::path& dir) {
        note_ok_ = false;
        if (game_running() || game_process_running()) { note_ = "Close the game before importing a save."; return; }
        const fs::path from = find_save(dir);
        if (from.empty()) { note_ = "No save there: the folder needs Map.sav, Mii.sav and Player.sav."; return; }
        bool ok = !is_dir(g_userdata / "save") || backup_now("before_import");
        std::error_code e;
        if (ok) {
            fs::create_directories(g_userdata / "save", e);
            drop_pending_commit();
            for (const char* n : {"Map.sav", "Mii.sav", "Player.sav"})
                ok = ok && fs::copy_file(from / n, g_userdata / "save" / n, fs::copy_options::overwrite_existing, e);
            /* the island's folders come with it (Ugc: Palette House creations and face paint; PhotoStudio: the photo
             * studio's pictures) and the old island's go (they are in the backup); left behind, the imported items were
             * black or orange (issue #16) */
            std::vector<fs::path> old, add;
            for (auto& d : fs::directory_iterator(g_userdata / "save", e)) if (d.is_directory(e)) old.push_back(d.path());
            for (auto& d : fs::directory_iterator(from, e)) if (d.is_directory(e)) add.push_back(d.path());
            for (auto& d : old) if (ok) { fs::remove_all(d, e); ok = !e; }
            for (auto& d : add) if (ok) ok = copy_contents(d, g_userdata / "save" / d.filename());
        }
        note_ = ok ? "Save imported. Your previous save is in the backups." : "The import failed: your previous save is in the backups.";
        note_ok_ = ok;
        refresh();
    }

    /* IMPORT A MII: a .ltd (mii_ltd.h) replaces a resident, chosen on the next screen; the save is backed up first */
    void choose_ltd() {
        dialog_for_ = kForMii;
        static const SDL_DialogFileFilter filters[] = {{"Mii files (.ltd)", "ltd"}};
        SDL_ShowOpenFileDialog(
            [](void* self, const char* const* list, int) {
                auto* l = static_cast<Launcher*>(self);
                std::lock_guard<std::mutex> lock(l->dialog_mutex_);
                l->dialog_result_ = list && list[0] ? list[0] : "";
                l->dialog_done_ = true;
            },
            this, window_, filters, 1, nullptr, false);
    }
    void load_ltd(const fs::path& file) {
        note_ok_ = false;
        const std::string err = ltd::load(file, ltd_);
        if (!err.empty()) { note_ = err; build_items(); return; }
        mii_slots_ = ltd::residents(g_userdata / "save");
        if (mii_slots_.empty()) { note_ = "No island to import into yet: play once and save first."; build_items(); return; }
        ltd_path_ = file;
        ltd_name_ = ltd::name_of(ltd_);
        if (ltd_name_.empty()) ltd_name_ = file.stem().u8string();
        go(Screen::kMii);
    }
    void import_mii(int slot, const std::string& old_name) {
        note_ok_ = false;
        if (game_running() || game_process_running()) { note_ = "Close the game before importing a Mii."; build_items(); return; }
        if (!backup_now("before_mii_import")) { note_ = "The save could not be backed up: nothing was changed."; build_items(); return; }
        drop_pending_commit();
        const std::string err = ltd::import(g_userdata / "save", slot, ltd_, ltd_path_);
        note_ok_ = err.empty();
        note_ = note_ok_ ? ltd_name_ + " replaced " + old_name + ". Your previous save is in the backups." : err + " Your save is unchanged (and backed up).";
        mii_slots_ = ltd::residents(g_userdata / "save");
        refresh();
        build_items();
    }

    enum { kForGame, kForImport, kForMii, kForBase, kForUpdate, kForKeys, kForDump };      // (kForBase and later: INSTALL)
    void choose_game(bool then_play = false) { play_after_dialog_ = then_play; choose_folder(kForGame, game_dir_.string()); }
    void choose_file(int purpose, const char* what, const char* patterns) {
        dialog_for_ = purpose;
        static SDL_DialogFileFilter filter;
        filter = {what, patterns};
        SDL_ShowOpenFileDialog(
            [](void* self, const char* const* list, int) {
                auto* l = static_cast<Launcher*>(self);
                std::lock_guard<std::mutex> lock(l->dialog_mutex_);
                l->dialog_result_ = list && list[0] ? list[0] : "";
                l->dialog_done_ = true;
            },
            this, window_, &filter, 1, nullptr, false);
    }

    /* ---------------------------------------------------------------- INSTALL
     * The launcher runs the installer itself, without a window of its own: this exe with --bootstrap puts the private Python
     * in deps\python (once), scripts\find_inputs.py --json finds the player's game files and keys, and scripts\install.py
     * --progress-json builds the game; its "##ssbu {json}" lines drive the progress bar (the plan's steps and their
     * expected seconds, each step's start and end, the compile's file counts). Its output goes to build\logs\
     * launcher_install.log. install.py is resumable: STOP, closing the launcher or an error lose nothing. */
    bool can_install() const { return is_file(g_repo / "scripts" / "install.py"); }
    fs::path python() const {
        if (is_file(g_repo / "deps" / "python" / "python.exe")) return g_repo / "deps" / "python" / "python.exe";
        std::string p;
        if (FILE* f = _wfopen((g_build / "python_path.txt").wstring().c_str(), L"rb")) { char b[1024]; if (fgets(b, sizeof b, f)) p = b; fclose(f); }
        while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' ')) p.pop_back();
        return p.empty() ? fs::path() : fs::u8path(p);
    }
    static std::wstring q(const fs::path& p) {      // a quoted argument (a trailing backslash doubled, as CommandLineToArgvW reads it)
        const std::wstring w = p.wstring();
        return L"\"" + w + (!w.empty() && w.back() == L'\\' ? L"\\" : L"") + L"\"";
    }
    fs::path install_log() const { return g_build / "logs" / "launcher_install.log"; }
    void start_setup() {
        go(Screen::kInstall);
        fail_.clear(); hint_.clear(); last_line_.clear();
        quit_armed_ = false;
        SetEnvironmentVariableA("PYTHONIOENCODING", "utf-8");      // find_inputs.py prints file names such as "Tomodachi Life(TM)_ ..."
        SetEnvironmentVariableA("PYTHONUNBUFFERED", "1");
        // issue #22: a python.exe whose pip or packages are damaged stopped every install at "pip could not install": bootstrap repairs it
        if (python().empty() || !boot::python_ok(python())) start_bootstrap();
        else start_scan();
    }
    void start_bootstrap() {
        setup_ = Setup::kBootstrap;
        const std::wstring cmd = q(g_self) + L" --bootstrap";      // this exe again: boot::bootstrap
        if (!child_.start(cmd, g_repo, install_log())) setup_failed("The installer's first step could not be started.", g_self.u8string());
        build_items();
    }
    void start_scan() {
        setup_ = Setup::kScan;
        scan_out_.clear();
        const std::wstring cmd = q(python()) + L" -u " + q(g_repo / "scripts" / "find_inputs.py") + L" --json";
        if (!child_.start(cmd, g_repo, install_log())) setup_failed("The installer's Python could not be started.", python().u8string());
        build_items();
    }
    void scan_done() {
        const Json r = parse_json(scan_out_), &best = r["best"];
        in_base_ = best["base"].str(); in_update_ = best["update"].str(); in_keys_ = best["prod_keys"].str();
        in_firmware_ = best["firmware"].str(); in_dump_ = best["dump"].str();
        in_auto_base_ = !in_base_.empty(); in_auto_update_ = !in_update_.empty(); in_auto_keys_ = !in_keys_.empty(); in_auto_dump_ = !in_dump_.empty();
        setup_ = Setup::kReady;
        selected_ = 0;
        if (!auto_dump_.empty()) { in_dump_ = auto_dump_; in_base_.clear(); start_install(); return; }
        build_items();
        if (!in_dump_.empty() || (!in_base_.empty() && !in_keys_.empty())) selected_ = int(list_.size()) - 2;      // INSTALL
    }
    void start_install() {
        std::wstring cmd = q(python()) + L" -u " + q(g_repo / "scripts" / "install.py") + L" --no-gui --progress-json";
        if (!in_dump_.empty()) cmd += L" --dump " + q(fs::u8path(in_dump_));
        else {
            if (!in_base_.empty()) cmd += L" --base " + q(fs::u8path(in_base_));
            if (!in_update_.empty()) cmd += L" --update " + q(fs::u8path(in_update_));
            if (!in_keys_.empty()) cmd += L" --keys " + q(fs::u8path(in_keys_));
        }
        if (!in_firmware_.empty()) cmd += L" --firmware " + q(fs::u8path(in_firmware_));
        steps_.clear(); step_ = -1; step_done_ = step_total_ = 0;
        last_line_.clear(); fail_.clear(); hint_.clear();
        setup_ = Setup::kInstall;
        armed_ = -1;
        if (!child_.start(cmd, g_repo, install_log())) setup_failed("The installer's Python could not be started.", python().u8string());
        build_items();
    }
    void setup_failed(const std::string& why, const std::string& hint) {
        setup_ = Setup::kFailed;
        fail_ = why; hint_ = hint;
        if (!auto_dump_.empty()) { printf("FAILED: %s %s\n", why.c_str(), hint.c_str()); fflush(stdout); auto_result_ = 1; quit_ = true; }
        selected_ = 0;
        build_items();
    }
    void setup_line(const std::string& l) {
        if (setup_ == Setup::kScan) { scan_out_ += l; scan_out_ += '\n'; return; }
        if (!auto_dump_.empty()) { printf("%s\n", l.c_str()); fflush(stdout); }
        if (setup_ == Setup::kInstall && l.rfind("##ssbu ", 0) == 0) { install_event(parse_json(l.substr(7))); return; }
        const size_t a = l.find_first_not_of(" \t");
        if (a != std::string::npos) last_line_ = l.substr(a);
    }
    void install_event(const Json& j) {
        const std::string ev = j["ev"].str();
        if (ev == "plan") {
            steps_.clear();
            for (const Json& s : j["steps"].a) steps_.push_back({s["id"].str(), s["title"].str(), std::max(1.0, s["est_s"].n), 0});
        } else if (ev == "step") {
            const std::string id = j["id"].str(), st = j["state"].str();
            int k = 0;
            while (k < int(steps_.size()) && steps_[k].id != id) k++;
            if (k == int(steps_.size())) steps_.push_back({id, id, 1, 0});
            if (st == "start") { steps_[k].state = 1; step_ = k; step_t0_ = SDL_GetTicks(); step_done_ = step_total_ = 0; }
            else { steps_[k].state = 2; if (st == "fail" && fail_.empty()) fail_ = j["msg"].str(); }
        } else if (ev == "progress") {
            step_done_ = j["done"].n; step_total_ = j["total"].n;
        } else if (ev == "error") {
            fail_ = j["msg"].str(); hint_ = j["hint"].str();
        } else if (ev == "done" && !j["ok"].b && fail_.empty()) fail_ = j["msg"].str();
    }
    // how far the install is (0..1) and the seconds left, from the plan's expected seconds
    void install_progress(double& frac, double& left) const {
        double total = 0, done = 0;
        for (const Step& s : steps_) total += s.est;
        for (int k = 0; k < int(steps_.size()); k++) {
            if (steps_[k].state == 2) done += steps_[k].est;
            else if (k == step_) {
                const double el = (SDL_GetTicks() - step_t0_) / 1000.0;
                const double f = step_total_ > 0 ? step_done_ / step_total_ : std::min(0.95, el / steps_[k].est);
                done += steps_[k].est * std::min(1.0, f);
            }
        }
        frac = total > 0 ? std::min(1.0, done / total) : 0;
        left = std::max(0.0, total - done);
    }
    void poll_setup() {
        if (!child_.started()) return;
        for (const std::string& l : child_.take_lines()) setup_line(l);
        if (child_.running()) return;
        const int code = child_.finish();
        for (const std::string& l : child_.take_lines()) setup_line(l);
        if (setup_ == Setup::kBootstrap) {
            if (code == 0 && is_file(python())) start_scan();
            else setup_failed("The installer could not set up its Python (it needs the internet the first time).", last_line_);
        } else if (setup_ == Setup::kScan) {
            if (code == 0) scan_done();
            else setup_failed("Looking for your game files failed.", last_line_);
        } else if (setup_ == Setup::kInstall) {
            if (code == 0 && installed()) {
                setup_ = Setup::kIdle;
                logo_ = load_image(r_, g_work / "title-logo.png");          // made from your game files by the install
                background_ = load_image(r_, g_work / "launcher-background.jpg");
                refresh();
                go(Screen::kMain);
                note_ = "Installed. Press PLAY.";
                if (!auto_dump_.empty()) { printf("INSTALLED\n"); fflush(stdout); auto_result_ = 0; quit_ = true; }
            } else setup_failed(fail_.empty() ? "The install stopped with an error." : fail_, hint_.empty() ? last_line_ : hint_);
        }
    }
    void choose_folder(int purpose, const std::string& def) {
        dialog_for_ = purpose;
        SDL_ShowOpenFolderDialog(
            [](void* self, const char* const* list, int) {
                auto* l = static_cast<Launcher*>(self);
                std::lock_guard<std::mutex> lock(l->dialog_mutex_);
                l->dialog_result_ = list && list[0] ? list[0] : "";
                l->dialog_done_ = true;
            },
            this, window_, def.empty() ? nullptr : def.c_str(), false);
    }

    void play() {
        if (game_running()) return;
        launch_error_.clear();
        if (!installed() || outdated()) {      // the install (or the update), here; else (the runtime package unpacked alone) the release page
            if (can_install()) start_setup();
            else ShellExecuteW(nullptr, L"open", kReleases, nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (!game_ok()) { choose_game(true); return; }
        if (!CopyFileW(g_exe.wstring().c_str(), g_play_exe.wstring().c_str(), FALSE) && !is_file(g_play_exe)) { launch_error_ = "could not copy the game's exe"; return; }
        // the environment of scripts\play.ps1
        SetEnvironmentVariableW(L"SSBU_ROMFS", (game_dir_ / "romfs").wstring().c_str());
        SetEnvironmentVariableA("SSBU_ERROR_DIALOG", "0");
        SetEnvironmentVariableA("SSBU_OS_CONSOLE_CORES", "1");
        SetEnvironmentVariableA("SSBU_KEYBOARD_PLAYER", "1");
        SetEnvironmentVariableA("SSBU_SHOW", "1");
        if (!GetEnvironmentVariableA("SSBU_HITCH", nullptr, 0)) SetEnvironmentVariableA("SSBU_HITCH", "300");      // the play log names what held any frame over 300 ms
        time_t now = time(nullptr);
        tm lt;
        localtime_s(&lt, &now);
        char name[64];
        strftime(name, sizeof name, "play_%Y%m%d_%H%M%S", &lt);
        SetEnvironmentVariableW(L"SSBU_REC", (g_work / (std::string(name) + ".rec")).wstring().c_str());      // player 1's input, for replaying a bug
        log_ = g_work / (std::string(name) + ".log");
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE log = CreateFileW(log_.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        if (log != INVALID_HANDLE_VALUE) { si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = log; si.hStdError = log; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); }
        std::wstring cmd = L"\"" + g_play_exe.wstring() + L"\" \"" + main_elf().wstring() + L"\"";
        const BOOL ok = CreateProcessW(g_play_exe.wstring().c_str(), cmd.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr, g_work.wstring().c_str(), &si, &game_);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (!ok) { game_ = {}; launch_error_ = "Windows error " + std::to_string(GetLastError()); return; }
        started_ = SDL_GetTicks();
        exit_code_ = 0;
        SDL_HideWindow(window_);
    }

    // What the game's log says about the way it ended, in a line a player can act on (it must fit the status line: about 75 letters).
    // 0x00000004: the crash handler (hostrt.c); 0x00000001: it could not start (exit(1)), or Task Manager or an antivirus ended it.
    std::string exit_reason(DWORD code) const {
        std::string text;
        if (FILE* f = _wfopen(log_.wstring().c_str(), L"rb")) {
            fseek(f, 0, SEEK_END);
            const long size = ftell(f), keep = std::min(size, 1L << 20);
            fseek(f, size - keep, SEEK_SET);
            text.resize(size_t(keep));
            text.resize(fread(&text[0], 1, text.size(), f));
            fclose(f);
        }
        const auto has = [&](const char* s) { return text.find(s) != std::string::npos; };
        if (has("could not be committed") || has("cannot map ") || has("cannot allocate"))
            return "Not enough memory: close other programs or enlarge Windows' page file.";
        if (code == 4) {
            std::string mod;
            const size_t at = text.rfind("HOST EXCEPTION");
            if (at != std::string::npos) {
                const size_t a = text.find('(', at), b = text.find(" + ", at);
                if (a != std::string::npos && b != std::string::npos && a < b) mod = text.substr(a + 1, b - a - 1);
            }
            if (has("the next start uses")) return "The graphics driver crashed (" + mod + "): the next start uses the other renderer.";
            std::string low = mod;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            for (const char* d : {"ati", "amd", "nvo", "nvg", "nv-", "ig", "vulkan-1", "opengl32"})
                if (low.rfind(d, 0) == 0) return "The graphics driver crashed (" + mod + "): please update it.";
            return "The game crashed" + (mod.empty() ? std::string() : " in " + mod) + ". Please report it with its log (see MORE).";
        }
        if (code == 1) {
            if (has("cannot open ") || has("short read") || has("is not an ELF")) return "A game file could not be read (log: see MORE).";
            return "It could not start, or Task Manager or an antivirus ended it (log: see MORE).";
        }
        return "The reason is in its log: see MORE.";
    }

    std::string fit(const std::string& text, float px, float width) {
        if (width <= 0 || font_.width(r_, text, px) <= width) return text;
        std::string t = text;
        while (t.size() > 4 && font_.width(r_, t + "...", px) > width) t.pop_back();
        return t + "...";
    }

    int item_at(float x, float y) const {
        const float H = layout_.h;
        if (x < layout_.left_chevron - H * 0.03f || x > layout_.right_chevron + H * 0.03f) return -1;
        for (size_t i = size_t(first_); i < list_.size() && int(i) < first_ + kVisible; ++i) {
            const float iy = layout_.item_y(float(int(i) - first_));
            if (y > iy - H * layout_.step * 0.75f && y < iy + H * layout_.step * 0.25f) return int(i);
        }
        return -1;
    }

    void draw_background() {
        const float W = layout_.w, H = layout_.h;
        if (background_.texture) {      // covering the window, drifting slowly
            const float zoom = 1.08f + 0.02f * std::sin(time_ * 0.07f);
            const float scale = std::max(W / background_.width, H / background_.height) * zoom;
            const float bw = background_.width * scale, bh = background_.height * scale;
            const float dx = (W - bw) / 2 + std::sin(time_ * 0.05f) * W * 0.012f, dy = (H - bh) / 2;
            SDL_SetTextureColorModFloat(background_.texture, 0.86f, 0.86f, 0.90f);
            paint_.image(background_, {dx, dy, bw, bh});
        } else {
            paint_.rect(0, 0, W, H, {0.10f, 0.42f, 0.70f}, {0.20f, 0.60f, 0.80f}, {0.10f, 0.30f, 0.20f}, {0.05f, 0.18f, 0.32f});
        }
        // dark from the left and the bottom, where the menu is
        const Color black{0, 0, 0, 0.84f}, clear{0, 0, 0, 0};
        paint_.rect(0, 0, W * 0.30f, H, black, {0, 0, 0, 0.58f}, {0, 0, 0, 0.58f}, black);
        paint_.rect(W * 0.30f, 0, W * 0.38f, H, {0, 0, 0, 0.58f}, clear, clear, {0, 0, 0, 0.58f});
        paint_.rect(0, H * 0.62f, W, H * 0.38f, clear, clear, {0, 0, 0, 0.70f}, {0, 0, 0, 0.70f});
        paint_.rect(0, 0, W, H * 0.16f, {0, 0, 0, 0.30f}, {0, 0, 0, 0.30f}, clear, clear);
    }

    void draw_line(float x, float y, const std::string& text, float px, Color c) {      // with bullets for 0x7f
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i < text.size() && text[i] != '\x7f') continue;
            x += font_.draw(r_, x, y, text.substr(start, i - start), px, c);
            if (i < text.size()) {
                const float rr = px * 0.09f;
                paint_.round_rect(x + px * 0.1f - rr, y - px * 0.3f - rr, rr * 2, rr * 2, rr, c);
                x += px * 0.2f;
            }
            start = i + 1;
        }
    }

    void draw_status() {
        const float H = layout_.h, x = layout_.text_x, y = H * 0.875f, px = H * 0.033f;
        const float icon = px * 0.75f, ix = x - icon * 1.55f;
        std::string line, detail;
        Color c = kGood;
        if (screen_ == Screen::kInstall) { draw_install_status(x, y, px, icon, ix); return; }
        if (screen_ != Screen::kMain) {
            if (!note_.empty()) { line = note_; c = note_ok_ ? kGood : kBad; }
            if (screen_ == Screen::kSettings) detail = "Saved to graphics.ini as you change it. The game reads it when it starts.";
            if (screen_ == Screen::kSaves) {
                if (line.empty()) line = save_size_ ? "Your island, saved " + save_when_ + "." : "No save yet: the game starts a new island.";
                detail = "Backups go to build\\saves. Restoring or importing backs up your current save first.";
            }
            if (screen_ == Screen::kControls) {
                if (line.empty()) line = pad_name_.empty() ? "Plug in a controller at any time." : "Controller ready.";
                detail = "The keyboard always works for player 1.";
            }
            if (screen_ == Screen::kMore) detail = (g_repo / "build").string();
            if (screen_ == Screen::kMii) {
                if (line.empty()) line = "Which resident does " + ltd_name_ + " replace?";
                detail = "The resident's look, name, voice and personality become the imported Mii's. Your save is backed up first.";
            }
            if (!line.empty()) font_.draw(r_, x, y, fit(line, px, layout_.w * 0.6f), px, c);
            if (!detail.empty()) draw_line(x, y + H * 0.048f, fit(detail, px * 0.8f, layout_.w * 0.62f), px * 0.8f, kDim);
            return;
        }
        if (game_running()) {
            paint_.spinner(ix + icon / 2, y - px * 0.33f, icon * 0.5f, H * 0.0035f, time_, kGood);
            line = "Tomodachi Life is running...";
        } else if (!launch_error_.empty()) {
            c = kBad; paint_.cross(ix, y - px * 0.7f, icon * 0.8f, H * 0.0035f, c);
            line = "The game did not start: " + launch_error_ + ".";
        } else if (exit_code_ != 0) {
            c = kBad; paint_.cross(ix, y - px * 0.7f, icon * 0.8f, H * 0.0035f, c);
            char b[160];
            snprintf(b, sizeof b, "The game closed with an error (0x%08lX).", (unsigned long)exit_code_);
            line = b;
        } else if (ready()) {
            paint_.check(ix, y - px * 0.78f, icon, H * 0.0035f, c);
            line = session_min_ >= 0 ? "Game ready. Last session: " + std::to_string(session_min_) + " min." : "Game ready.";
        } else {
            c = kBad; paint_.cross(ix, y - px * 0.7f, icon * 0.8f, H * 0.0035f, c);
            line = installed() ? (lini_.get("game", "").empty() ? "Choose the game folder." : "That folder is not Tomodachi Life (exefs\\ and romfs\\).")
                 : !note_.empty() ? note_
                 : can_install() ? (is_file(g_build / "state.json") ? "The install is not finished: press PLAY to go on." : "Not installed yet: press PLAY to install.")
                 : "This folder is not the game, only a part its installer downloads.";
        }
        font_.draw(r_, x, y, fit(line, px, layout_.w * 0.6f), px, c);
        if (exit_code_ != 0 && !game_running()) detail = exit_reason_;
        else if (!installed() && !can_install())
            detail = "Press PLAY and download Tomodachi Life PC Setup.exe from the release page.";
        else if (!installed()) detail = "It builds the game on this PC from your own game files and keys: about 40 minutes, once.";
        else if (selected_ == kGame) detail = game_dir_.string();
        else if (ready()) detail = std::string("Nintendo Switch \x7f Living the Dream \x7f ") + (save_size_ ? "Island saved " + save_when_ : std::string("New island")) +
                                   " \x7f " + (gfx_.get("resolution_scale", "1.0") == "2.0" ? "4K" : gfx_.get("resolution_scale", "1.0") == "1.0" ? "1080p" : gfx_.get("resolution_scale", "1.0") + "x");
        else detail = "Needs your own dump of the game: exefs\\ and romfs\\ in one folder";
        draw_line(x, y + H * 0.048f, fit(detail, px * 0.8f, layout_.w * 0.62f), px * 0.8f, kDim);
    }

    void draw_install_status(float x, float y, float px, float icon, float ix) {      // INSTALL: what is happening, a progress bar
        const float H = layout_.h, W = layout_.w;
        std::string line, detail;
        Color c = kGood;
        if (!note_.empty() && setup_ == Setup::kInstall) { line = note_; c = kBad; detail = last_line_; }
        else if (setup_ == Setup::kBootstrap) {
            paint_.spinner(ix + icon / 2, y - px * 0.33f, icon * 0.5f, H * 0.0035f, time_, kGood);
            line = "Getting the installer ready (a minute or two, once)...";
            detail = last_line_;
        } else if (setup_ == Setup::kScan) {
            paint_.spinner(ix + icon / 2, y - px * 0.33f, icon * 0.5f, H * 0.0035f, time_, kGood);
            line = "Looking for your game files and keys on this PC...";
            detail = "Downloads, Desktop, Documents and emulator folders.";
        } else if (setup_ == Setup::kFailed) {
            c = kBad; paint_.cross(ix, y - px * 0.7f, icon * 0.8f, H * 0.0035f, c);
            line = fail_;
            detail = hint_;
        } else if (setup_ == Setup::kInstall) {
            double frac = 0, left = 0;
            install_progress(frac, left);
            const std::string title = step_ >= 0 && step_ < int(steps_.size()) ? steps_[step_].title : "Starting";
            char b[160];
            const int mins = int(left / 60 + 0.5);
            snprintf(b, sizeof b, "%s  -  %d %%  -  %s", title.c_str(), int(frac * 100), mins <= 1 ? "about a minute left" : ("about " + std::to_string(mins) + " minutes left").c_str());
            line = b;
            detail = last_line_;
            const float bx = x, by = y - H * 0.105f, bw = std::min(W * 0.6f, H * 1.05f), bh = H * 0.016f;      // above the line
            paint_.round_rect(bx, by, bw, bh, bh / 2, {1, 1, 1, 0.18f});
            paint_.round_rect(bx, by, std::max(bh, bw * float(frac)), bh, bh / 2, kGold);
            paint_.spinner(ix + icon / 2, y - px * 0.33f, icon * 0.5f, H * 0.0035f, time_, kGood);
        } else {
            const bool ok = !in_dump_.empty() || (!in_base_.empty() && !in_keys_.empty());
            if (ok) { paint_.check(ix, y - px * 0.78f, icon, H * 0.0035f, c); line = "Ready: press INSTALL."; }
            else { c = kBad; paint_.cross(ix, y - px * 0.7f, icon * 0.8f, H * 0.0035f, c); line = in_base_.empty() ? "Choose your Tomodachi Life game file (.nsp, .nsz or .xci)." : "Choose your console's prod.keys."; }
            detail = "From your own Switch. The game is built on this PC: about 40 minutes, once.";
        }
        font_.draw(r_, x, y, fit(line, px, W * 0.6f), px, c);
        if (!detail.empty()) draw_line(x, y + H * 0.048f, fit(detail, px * 0.8f, W * 0.62f), px * 0.8f, kDim);
    }

    void draw_keys() {      // CONTROLS: the keyboard's keys, on a card at the right
        const float W = layout_.w, H = layout_.h, px = H * 0.027f, row = H * 0.044f;
        const int n = int(sizeof k_keys / sizeof k_keys[0]);
        const float cw = std::min(W * 0.36f, H * 0.66f), ch = row * (n + 1.2f), cx = W * 0.93f - cw, cy = H * 0.17f;      // below the toast
        paint_.round_rect(cx, cy, cw, ch, H * 0.012f, {kGold.r, kGold.g, kGold.b, 0.70f * fade_});
        paint_.round_rect(cx + 1.5f, cy + 1.5f, cw - 3, ch - 3, H * 0.011f, {0.04f, 0.035f, 0.03f, 0.86f * fade_});
        font_.draw(r_, cx + H * 0.03f, cy + row * 0.95f, "KEYBOARD", px * 1.05f, {kGold.r, kGold.g, kGold.b, fade_});
        for (int i = 0; i < n; ++i) {
            const float y = cy + row * (i + 2.0f) - row * 0.2f;
            font_.draw(r_, cx + H * 0.03f, y, k_keys[i][0], px, {kDim.r, kDim.g, kDim.b, fade_}, false);
            font_.draw(r_, cx + cw - H * 0.03f - font_.width(r_, k_keys[i][1], px), y, k_keys[i][1], px, {1, 1, 1, fade_}, false);
        }
    }

    void draw_toast() {
        const float W = layout_.w, H = layout_.h, px = H * 0.027f;      // top right: the status lines own the bottom left
        const std::string text = shaders_ > 0 ? std::to_string(shaders_) + " shaders ready" : "Shaders compile the first time you play";
        const float tw = font_.width(r_, text, px);
        const float bh = H * 0.068f, bw = tw + H * 0.11f, bx = W - bw - H * 0.03f, by = H * 0.03f;
        paint_.round_rect(bx, by, bw, bh, H * 0.008f, {kGold.r, kGold.g, kGold.b, 0.75f});
        paint_.round_rect(bx + 1.5f, by + 1.5f, bw - 3, bh - 3, H * 0.007f, {0.04f, 0.035f, 0.03f, 0.88f});
        const float icon = px * 0.8f, ix = bx + H * 0.03f, cy = by + bh / 2;
        if (shaders_ > 0) paint_.check(ix, cy - icon / 2, icon, H * 0.0032f, kGood);
        else paint_.spinner(ix + icon / 2, cy, icon / 2, H * 0.003f, time_, kWhite);
        font_.draw(r_, ix + H * 0.045f, cy + px * 0.36f, text, px, kWhite, false);
    }

    SDL_Window* window_;
    SDL_Renderer* r_;
    Painter paint_;
    Font font_;
    Image logo_, background_;
    Ini gfx_, lini_;
    fs::path game_dir_, log_;
    std::vector<fs::path> backups_;
    long long save_size_ = 0;
    std::string save_when_, pad_name_, launch_error_, note_;
    bool note_ok_ = true;
    int shaders_ = 0, armed_ = -1, session_min_ = -1;
    Screen screen_ = Screen::kMain;
    std::vector<Item> list_;
    static constexpr int kVisible = 14;      // items shown at once; a longer list scrolls (first_: the top one)
    int selected_ = 0, first_ = 0;
    float selection_y_ = 0, fade_ = 0, time_ = 0, stick_x_ = 0, stick_y_ = 0, held_x_ = 0, held_y_ = 0;
    bool mouse_moved_ = false, quit_ = false, play_after_dialog_ = false;
    int dialog_for_ = 0;      // kForGame / kForImport / kForMii: what the dialog is for
    ltd::Bytes ltd_;          // IMPORT A MII: the file (version 3), its path and Mii name, the residents it can replace
    fs::path ltd_path_;
    std::string ltd_name_;
    std::vector<std::pair<int, std::string>> mii_slots_;
    PROCESS_INFORMATION game_{};
    DWORD exit_code_ = 0;
    std::string exit_reason_;
    bool direct_ = false;      // --play: quit with the game
    Uint64 started_ = 0;
    std::mutex dialog_mutex_;
    std::string dialog_result_;
    bool dialog_done_ = false;
    // INSTALL
    enum class Setup { kIdle, kBootstrap, kScan, kReady, kInstall, kFailed };
    struct Step { std::string id, title; double est; int state; };      // state: 0 waiting, 1 running, 2 finished
    Setup setup_ = Setup::kIdle;
    Child child_;
    std::string scan_out_, in_base_, in_update_, in_keys_, in_firmware_, in_dump_, last_line_, fail_, hint_, auto_dump_;
    bool in_auto_base_ = false, in_auto_update_ = false, in_auto_keys_ = false, in_auto_dump_ = false, quit_armed_ = false;
    std::vector<Step> steps_;
    int step_ = -1, auto_result_ = 1;
    double step_done_ = 0, step_total_ = 0;
    Uint64 step_t0_ = 0;
};

/* the window icon: the game's logo, else a smiling yellow face drawn here */
void set_icon(SDL_Window* win) {
    int w = 0, h = 0, n = 0;
    FILE* f = _wfopen((g_work / "title-logo.png").wstring().c_str(), L"rb");
    stbi_uc* px = f ? stbi_load_from_file(f, &w, &h, &n, 4) : nullptr;
    if (f) fclose(f);
    if (px) {
        if (SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4)) { SDL_SetWindowIcon(win, s); SDL_DestroySurface(s); }
        stbi_image_free(px);
        return;
    }
    const int S = 64;
    SDL_Surface* s = SDL_CreateSurface(S, S, SDL_PIXELFORMAT_RGBA32);
    if (!s) return;
    Uint32* p = (Uint32*)s->pixels;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            const float dx = x - S / 2 + 0.5f, dy = y - S / 2 + 0.5f, d = std::sqrt(dx * dx + dy * dy);
            Uint8 r = 0, g = 0, b = 0, a = 0;
            if (d < S / 2 - 1) { r = 255; g = 200; b = 40; a = 255; }
            if (d >= S / 2 - 4 && d < S / 2 - 1) { r = 240; g = 120; b = 20; }
            const float ex = std::fabs(dx) - 9, ey = dy + 7, m = std::sqrt(dx * dx + (dy - 2) * (dy - 2));
            if (ex * ex + ey * ey < 14 || (dy > 4 && m > 13 && m < 17)) { r = 60; g = 40; b = 20; }
            p[y * S + x] = Uint32(r) | Uint32(g) << 8 | Uint32(b) << 16 | Uint32(a) << 24;
        }
    SDL_SetWindowIcon(win, s);
    SDL_DestroySurface(s);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--bootstrap")) { paths_init(); return boot::bootstrap(); }      // the INSTALL screen's first step: no window
    const char* shot = nullptr;
    std::string shot_screen;
    const char* import_dir = nullptr;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--shot")) { shot = argv[i + 1]; if (i + 2 < argc) shot_screen = argv[i + 2]; }
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--import")) import_dir = argv[i + 1];      // SAVE DATA > IMPORT A SAVE without the dialog
    const char* import_mii = nullptr; int import_slot = -1;
    for (int i = 1; i + 2 < argc; i++) if (!strcmp(argv[i], "--import-mii")) { import_mii = argv[i + 1]; import_slot = atoi(argv[i + 2]); }
    if (import_mii) import_dir = "";
    if (import_dir) shot = "";
    const char* auto_dump = nullptr;      // --auto-install <dump folder>: the install without questions (tests); the exit code says how it went
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--auto-install")) auto_dump = argv[i + 1];
    bool play_now = false;      // --play (Tomodachi Life.exe): straight into the game
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--play")) play_now = true;
    paths_init();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return 1;
    int ww = 1600, wh = 900;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {      // 70 % of the screen: 1600x900 was small on a 4K one
        ww = usable.w * 7 / 10; wh = ww * 9 / 16;
        if (wh > usable.h * 8 / 10) { wh = usable.h * 8 / 10; ww = wh * 16 / 9; }
    }
    SDL_Window* window = SDL_CreateWindow("Tomodachi Life", ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (shot || auto_dump || play_now ? SDL_WINDOW_HIDDEN : 0));
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tomodachi Life", SDL_GetError(), nullptr); return 1; }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowMinimumSize(window, 960, 540);
    set_icon(window);
    int rc = 0;
    {
        Launcher launcher(window, renderer);
        if (const char* te = getenv("LAUNCHER_TEST_EXIT")) launcher.test_exit(te);
        if (auto_dump) launcher.auto_install(auto_dump);
        else if (play_now && !shot && !import_dir) launcher.play_direct();
        if (import_mii) { printf("%s\n", launcher.import_mii_now(import_mii, import_slot).c_str()); fflush(stdout); }
        else if (import_dir) { printf("%s\n", launcher.import_now(import_dir).c_str()); fflush(stdout); }
        SDL_Texture* target = shot ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 1600, 900) : nullptr;
        if (shot) launcher.open_screen(shot_screen);
        Uint64 last = SDL_GetTicksNS(), started = last;
        while (!import_dir && launcher.running()) {
            SDL_Event e;
            if (launcher.game_running() ? SDL_WaitEventTimeout(&e, 250) : SDL_PollEvent(&e)) {      // while the game runs: only wait for it
                do launcher.event(e);
                while (SDL_PollEvent(&e));
            }
            const Uint64 now = SDL_GetTicksNS();
            launcher.update(std::min(0.1f, (now - last) / 1e9f));
            last = now;
            if (launcher.game_running()) continue;
            if (target) SDL_SetRenderTarget(renderer, target);
            launcher.draw(target);
            if (target && now - started > 1200000000ull) {      // the fade-in is over: save and stop
                SDL_Surface* px = SDL_RenderReadPixels(renderer, nullptr);
                const bool ok = px && SDL_SaveBMP(px, shot);
                if (px) SDL_DestroySurface(px);
                printf("%s %s\n", ok ? "saved" : "could not save", shot);
                break;
            }
            SDL_RenderPresent(renderer);
        }
        if (auto_dump) rc = launcher.auto_result();
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return rc;
}
