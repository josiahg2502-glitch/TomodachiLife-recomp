/* The on-screen keyboard: nn::swkbd::ShowKeyboard answered by a keyboard of our own drawn in the game's window, used
 * with any controller (like the console's own) or with the PC keyboard, instead of a Windows dialog.
 *
 * The picture is drawn here with GDI into a 1920x1080 bitmap (any character the game's text can hold: accents, kana...);
 * the Vulkan renderer copies that bitmap over the presented frame (nvn_vk_draw.c osk_draw) and, while the game presents
 * nothing (the calling thread waits here), presents its last frame again with it (nvn_front_osk_redraw).
 *
 * Controller: d-pad / left stick move, A types the key, B deletes, Y space, X switches the symbols page, L / R move the
 * text cursor, ZL or the left stick button = shift, + = OK, - = cancel. PC keyboard: type, Backspace / Delete,
 * Left / Right / Home / End, Enter = OK, Esc = cancel. The text is checked against the caller's limits (max / min).
 *
 * osk_run returns 1 (OK, text in out), 0 (cancelled), -1 (not available here: the caller uses its other path). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#ifndef _WIN32
/* Linux build: the keyboard is drawn with GDI, which Linux does not have. Every entry answers "not available here", so
 * nnapplet.c uses its other path (a desktop text dialog). */
int osk_key(UINT msg, WPARAM w) { (void)msg; (void)w; return 0; }
int osk_active(void) { return 0; }
int osk_blocks_input(void) { return 0; }
int osk_inline_open(const wchar_t *initial, int max_len) { (void)initial; (void)max_len; return 0; }      /* tomo.c keeps its text prompt */
int osk_inline_step(wchar_t *text, int cap, int *len, int *cursor) { (void)text; (void)cap; (void)len; (void)cursor; return 0; }
void osk_inline_close(void) {}
int osk_inline_image(uint8_t *dst, int w, int h, int pitch) { (void)dst; (void)w; (void)h; (void)pitch; return 0; }
int osk_take(uint32_t *dst, LONG *seen) { (void)dst; (void)seen; return 0; }
void osk_size(int *w, int *h) { *w = 1920; *h = 1080; }
void osk_test_start(void) {}
int osk_message(const wchar_t *msg, int question) { (void)msg; (void)question; return -1; }
int osk_run(const wchar_t *header, const wchar_t *guide, const wchar_t *initial, int max_len, int min_len, int password, wchar_t *out) {
    (void)header; (void)guide; (void)initial; (void)max_len; (void)min_len; (void)password; (void)out; return -1;
}
#else

#define OSK_W 1920
#define OSK_H 1080
#define OSK_MAX 500

uint64_t nnhid_menu_buttons(void);                 /* nnhid.c */
int nvn_front_osk_ready(void);                     /* nvn_gl.c: 1 when the Vulkan renderer runs (the only one that draws it) */
void nvn_front_osk_redraw(void);                   /* nvn_gl.c: present the last frame again with the keyboard over it */
long long nvn_front_presents(void);                /* nvn_gl.c: frames presented so far */

enum { B_A = 0, B_B, B_X, B_Y, B_LS, B_RS, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN };

static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static uint32_t *g_px;                             /* the picture (BGRA, top-down), valid while g_on */
static volatile LONG g_on, g_version;
static HDC g_dc;
static HBITMAP g_bmp;
static HFONT g_f_head, g_f_text, g_f_key, g_f_small;

/* keys from the window (display.c), consumed by the loop */
#define KQ 64
static struct { UINT msg; WPARAM w; } g_q[KQ];
static int g_qn;

static BOOL CALLBACK osk_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

/* display.c, window thread: 1 when the key went to the keyboard (the game does not see it) */
int osk_key(UINT msg, WPARAM w) {
    if (!g_on) return 0;
    if (msg != WM_CHAR && msg != WM_KEYDOWN) return 0;
    EnterCriticalSection(&g_lock);
    if (g_qn < KQ) { g_q[g_qn].msg = msg; g_q[g_qn].w = w; g_qn++; }
    LeaveCriticalSection(&g_lock);
    return 1;
}

/* nvn_vk_draw.c, render thread: the picture when it changed since `*seen` (copied into dst, OSK_W x OSK_H), else NULL */
static int g_in_open;                              /* the inline keyboard (below) is up: not the full-screen picture */
static ULONGLONG g_closed_at;
int osk_active(void) { return g_on != 0 && !g_in_open; }
/* nnhid.c: the game gets no buttons or keys while a keyboard is up, nor for 250 ms after (the press that closed it) */
int osk_blocks_input(void) { return g_on != 0 || (g_closed_at && GetTickCount64() - g_closed_at < 250); }
int osk_take(uint32_t *dst, LONG *seen) {
    if (!g_on) return 0;
    int got = 0;
    EnterCriticalSection(&g_lock);
    if (g_px && *seen != g_version) { memcpy(dst, g_px, (size_t)OSK_W * OSK_H * 4); *seen = g_version; got = 1; }
    LeaveCriticalSection(&g_lock);
    return got;
}
void osk_size(int *w, int *h) { *w = OSK_W; *h = OSK_H; }

/* ---------------------------------------------------------------- layout */
#define COLS 11
#define ROWS 4
static const wchar_t *const k_pages[3][ROWS] = {
    {L"1234567890-", L"qwertyuiop/", L"asdfghjkl:'", L"zxcvbnm,.?!"},
    {L"1234567890_", L"QWERTYUIOP@", L"ASDFGHJKL;\"", L"ZXCVBNM<>?!"},
    {L"!@#$%^&*()_", L"~`=+[]{}\\|;", L"àèéìòùçñüöä", L"«»¿¡€£¥°ßæø"},
};
/* the bottom row: shift, symbols, space (wide), delete, OK */
enum { F_SHIFT, F_SYM, F_SPACE, F_DEL, F_OK, F_N };
static const int k_fspan[F_N] = {2, 2, 4, 2, 1};   /* in key widths: 11 in all */

typedef struct {
    const wchar_t *header, *guide;
    int max_len, min_len, password;
    wchar_t text[OSK_MAX + 2];
    int len, cur;                                  /* characters, cursor position */
    int row, col, page, shift_once;                /* focus: row 0..4 (4 = the bottom row), col in keys / function index */
    const wchar_t *note;                           /* a line under the text (too short ...) */
} Osk;

static COLORREF rgb(int r, int g, int b) { return RGB(r, g, b); }
static void fill(int x, int y, int w, int h, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c); RECT r = {x, y, x + w, y + h}; FillRect(g_dc, &r, br); DeleteObject(br);
}
static void rbox(int x, int y, int w, int h, int rad, COLORREF fillc, COLORREF edge, int pen) {
    HBRUSH br = CreateSolidBrush(fillc); HPEN pn = CreatePen(PS_SOLID, pen, edge);
    HGDIOBJ ob = SelectObject(g_dc, br), op = SelectObject(g_dc, pn);
    RoundRect(g_dc, x, y, x + w, y + h, rad, rad);
    SelectObject(g_dc, ob); SelectObject(g_dc, op); DeleteObject(br); DeleteObject(pn);
}
static void text(HFONT f, COLORREF c, int x, int y, int w, int h, const wchar_t *s, int n, UINT how) {
    RECT r = {x, y, x + w, y + h};
    SelectObject(g_dc, f); SetTextColor(g_dc, c); SetBkMode(g_dc, TRANSPARENT);
    DrawTextW(g_dc, s, n, &r, how | DT_NOPREFIX);
}

static wchar_t key_char(const Osk *k, int row, int col) {
    int page = k->page == 2 ? 2 : (k->page == 1 || k->shift_once) ? 1 : 0;
    return k_pages[page][row][col];
}

static void render(const Osk *k) {
    const COLORREF bg = rgb(40, 40, 46), panel = rgb(28, 28, 32), keyc = rgb(70, 70, 80), keyf = rgb(0, 190, 220),
                   white = rgb(240, 240, 245), grey = rgb(160, 160, 170), field = rgb(250, 250, 252), dark = rgb(20, 20, 24);
    fill(0, 0, OSK_W, OSK_H, bg);
    /* header, guide, the text field */
    text(g_f_head, white, 160, 70, OSK_W - 320, 80, k->header && k->header[0] ? k->header : L"Enter text", -1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    rbox(160, 170, OSK_W - 320, 110, 24, field, rgb(0, 190, 220), 4);
    wchar_t shown[OSK_MAX + 2];
    for (int i = 0; i < k->len; i++) shown[i] = k->password ? L'•' : k->text[i];
    shown[k->len] = 0;
    if (!k->len && k->guide && k->guide[0]) text(g_f_text, grey, 190, 170, OSK_W - 380, 110, k->guide, -1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    else {
        /* the text, scrolled so that the cursor stays inside the field */
        SelectObject(g_dc, g_f_text);
        int start = 0, fieldw = OSK_W - 380;
        SIZE sz;
        for (;;) { GetTextExtentPoint32W(g_dc, shown + start, k->cur - start, &sz); if (sz.cx <= fieldw - 20 || start >= k->cur) break; start++; }
        text(g_f_text, dark, 190, 170, fieldw, 110, shown + start, k->len - start, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        GetTextExtentPoint32W(g_dc, shown + start, k->cur - start, &sz);
        fill(190 + sz.cx + 1, 195, 4, 60, rgb(0, 150, 200));                   /* the cursor */
    }
    wchar_t count[64];
    _snwprintf(count, 63, L"%d / %d", k->len, k->max_len); count[63] = 0;
    text(g_f_small, grey, OSK_W - 520, 290, 360, 40, count, -1, DT_RIGHT | DT_SINGLELINE);
    if (k->note) text(g_f_small, rgb(255, 120, 110), 160, 290, OSK_W - 700, 40, k->note, -1, DT_LEFT | DT_SINGLELINE);
    /* the keys */
    int kx0 = 160, ky0 = 380, kw = (OSK_W - 320) / COLS, kh = 120, gap = 10;
    fill(0, ky0 - 30, OSK_W, OSK_H - ky0 + 30, panel);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int on = k->row == r && k->col == c, x = kx0 + c * kw, y = ky0 + r * (kh + gap);
            rbox(x + gap / 2, y, kw - gap, kh, 18, on ? keyf : keyc, on ? white : keyc, on ? 4 : 1);
            wchar_t ch[2] = {key_char(k, r, c), 0};
            text(g_f_key, on ? dark : white, x, y, kw, kh, ch, 1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    static const wchar_t *const fname[F_N] = {L"⇧ Shift", L"#+=", L"Space", L"⌫", L"OK"};
    int x = kx0, y = ky0 + ROWS * (kh + gap);
    for (int f = 0; f < F_N; f++) {
        int w = k_fspan[f] * kw, on = k->row == ROWS && k->col == f;
        int lit = (f == F_SHIFT && (k->page == 1 || k->shift_once)) || (f == F_SYM && k->page == 2);
        COLORREF c = on ? keyf : f == F_OK ? rgb(0, 140, 170) : lit ? rgb(110, 110, 125) : keyc;
        rbox(x + gap / 2, y, w - gap, kh, 18, c, on ? white : c, on ? 4 : 1);
        text(g_f_key, on ? dark : white, x, y, w, kh, f == F_SYM && k->page == 2 ? L"abc" : fname[f], -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += w;
    }
    text(g_f_small, grey, 160, OSK_H - 60, OSK_W - 320, 50,
         L"Ⓐ type   Ⓑ delete   Ⓨ space   Ⓧ symbols   L / R cursor   ZL shift   + OK   − cancel", -1, DT_CENTER | DT_SINGLELINE);
    GdiFlush();
}

static void publish(const Osk *k) {
    render(k);
    EnterCriticalSection(&g_lock);
    for (size_t i = 0, n = (size_t)OSK_W * OSK_H; i < n; i++) g_px[i] |= 0xff000000u;      /* GDI leaves alpha 0 */
    InterlockedIncrement(&g_version);
    LeaveCriticalSection(&g_lock);
}

static void ins(Osk *k, wchar_t ch) {
    if (k->len >= k->max_len || ch < 0x20) return;
    memmove(k->text + k->cur + 1, k->text + k->cur, (size_t)(k->len - k->cur) * sizeof(wchar_t));
    k->text[k->cur++] = ch; k->len++; k->text[k->len] = 0;
    k->shift_once = 0;
}
static void del_back(Osk *k) {
    if (k->cur <= 0) return;
    memmove(k->text + k->cur - 1, k->text + k->cur, (size_t)(k->len - k->cur) * sizeof(wchar_t));
    k->cur--; k->len--; k->text[k->len] = 0;
}
static void del_fwd(Osk *k) { if (k->cur < k->len) { k->cur++; del_back(k); } }

/* 1 = OK accepted, 0 = not yet */
static int try_ok(Osk *k) {
    if (k->len < k->min_len) { k->note = L"The text is too short"; return 0; }
    return 1;
}

static int setup_gdi(void) {
    if (g_dc) return 1;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = OSK_W; bi.bmiHeader.biHeight = -OSK_H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(NULL);
    void *bits = NULL;
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!bmp || !bits) { if (dc) DeleteDC(dc); return 0; }
    SelectObject(dc, bmp);
    g_dc = dc; g_bmp = bmp; g_px = bits;
    g_f_head = CreateFontW(-56, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_f_text = CreateFontW(-60, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_f_key = CreateFontW(-52, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
    g_f_small = CreateFontW(-34, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
    return 1;
}

static void move(Osk *k, int dx, int dy) {
    if (dy) {
        int old = k->row;
        k->row = (k->row + dy + ROWS + 1) % (ROWS + 1);
        if (k->row == ROWS && old != ROWS) {      /* into the bottom row: the function key under the column */
            int c = 0, x = 0;
            while (c < F_N - 1 && x + k_fspan[c] <= k->col) x += k_fspan[c++];
            k->col = c;
        } else if (old == ROWS && k->row != ROWS) {
            int x = 0;
            for (int c = 0; c < k->col; c++) x += k_fspan[c];
            k->col = x;
        }
    }
    if (dx) {
        int n = k->row == ROWS ? F_N : COLS;
        k->col = (k->col + dx + n) % n;
    }
}

static void press(Osk *k, int *done, int *result) {
    if (k->row < ROWS) { ins(k, key_char(k, k->row, k->col)); return; }
    switch (k->col) {
    case F_SHIFT: if (k->page == 2) k->page = 0; else if (k->page == 1) k->page = 0; else if (k->shift_once) { k->shift_once = 0; k->page = 1; } else k->shift_once = 1; break;
    case F_SYM: k->page = k->page == 2 ? 0 : 2; k->shift_once = 0; break;
    case F_SPACE: ins(k, L' '); break;
    case F_DEL: del_back(k); break;
    case F_OK: if (try_ok(k)) { *done = 1; *result = 1; } break;
    }
}

/* TEST ONLY: SSBU_OSK_TEST=<presented frame>[,<keys>] opens the keyboard from a thread of its own at that frame, feeds
 * <keys> as typed characters (default "Hello"; '>' = OK at the end), writes the picture as osk_test.bmp beside the exe
 * and logs what came back. Shows that the keyboard opens, draws over the game, takes keys and closes. */
static void dump_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh; BITMAPINFOHEADER ih;
    memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4d42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + OSK_W * OSK_H * 4;
    ih.biSize = sizeof ih; ih.biWidth = OSK_W; ih.biHeight = -OSK_H; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
    EnterCriticalSection(&g_lock); fwrite(g_px, 4, (size_t)OSK_W * OSK_H, f); LeaveCriticalSection(&g_lock);
    fclose(f);
}
static DWORD WINAPI feeder(LPVOID p);
static DWORD WINAPI osk_test_thread(LPVOID arg) {
    const char *e = arg;
    long long at = _atoi64(e);
    static char feed[256];
    strncpy(feed, strchr(e, ',') ? strchr(e, ',') + 1 : "Hello>", sizeof feed - 1);
    while (nvn_front_presents() < at) Sleep(50);
    static wchar_t out[OSK_MAX + 2];
    CreateThread(NULL, 0, feeder, feed, 0, NULL);  /* the keys come from a second thread while this one runs the keyboard */
    int r = osk_run(L"Test of the on-screen keyboard", L"Type a name", L"", 20, 1, 0, out);
    printf("osk: TEST (SSBU_OSK_TEST): result %d, text \"%ls\"\n", r, r == 1 ? out : L"");
    fflush(stdout);
    return 0;
}
static DWORD WINAPI feeder(LPVOID p) {
    const char *s = p;
    while (!g_on) Sleep(10);
    Sleep(500);
    for (; *s; s++) {
        Sleep(120);
        if (*s == '>') { dump_bmp("osk_test.bmp"); printf("osk: TEST: picture written to osk_test.bmp\n"); fflush(stdout); osk_key(WM_CHAR, 13); }
        else osk_key(WM_CHAR, (WPARAM)(unsigned char)*s);
    }
    return 0;
}
void osk_test_start(void) {
    const char *e = getenv("SSBU_OSK_TEST");
    if (!e || !*e) return;
    InitOnceExecuteOnce(&g_once, osk_init, NULL, NULL);
    CreateThread(NULL, 0, osk_test_thread, (LPVOID)e, 0, NULL);
}

/* A message of the game's text check (the console shows it over the keyboard): A = OK / Yes, B = No (question only);
 * Enter / Esc on the PC keyboard. Returns 1 for OK / Yes, 0 for No, -1 when not available here. */
int osk_message(const wchar_t *msg, int question) {
    InitOnceExecuteOnce(&g_once, osk_init, NULL, NULL);
    if (!nvn_front_osk_ready() || !setup_gdi()) return -1;
    const COLORREF bg = rgb(40, 40, 46), white = rgb(240, 240, 245), grey = rgb(160, 160, 170), keyf = rgb(0, 190, 220);
    fill(0, 0, OSK_W, OSK_H, bg);
    rbox(260, 260, OSK_W - 520, OSK_H - 520, 30, rgb(28, 28, 32), keyf, 4);
    text(g_f_text, white, 320, 300, OSK_W - 640, OSK_H - 700, msg, -1, DT_CENTER | DT_WORDBREAK);
    text(g_f_small, grey, 320, OSK_H - 380, OSK_W - 640, 60, question ? L"Ⓐ Yes      Ⓑ No" : L"Ⓐ OK", -1, DT_CENTER | DT_SINGLELINE);
    GdiFlush();
    EnterCriticalSection(&g_lock);
    for (size_t i = 0, n = (size_t)OSK_W * OSK_H; i < n; i++) g_px[i] |= 0xff000000u;
    InterlockedIncrement(&g_version);
    g_qn = 0;
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_on, 1);
    uint64_t prev = nnhid_menu_buttons();
    ULONGLONG last_redraw = 0;
    int answer = -2;
    while (answer == -2) {
        Sleep(16);
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_qn; i++) if (g_q[i].msg == WM_CHAR) { if (g_q[i].w == 13) answer = 1; else if (g_q[i].w == 27) answer = question ? 0 : 1; }
        g_qn = 0;
        LeaveCriticalSection(&g_lock);
        uint64_t b = nnhid_menu_buttons(), down = b & ~prev;
        prev = b;
        if (down >> B_A & 1) answer = 1;
        else if (down >> B_B & 1) answer = question ? 0 : 1;
        ULONGLONG now = GetTickCount64();
        if (now - last_redraw >= 33) { last_redraw = now; nvn_front_osk_redraw(); }
    }
    InterlockedExchange(&g_on, 0);
    return answer;
}

/* One round of input: the PC keys the window queued and the controllers (a press, and the d-pad / L / R / B repeating
 * while held). 1 when the keyboard changed; *done / *result as press. Buttons held when the keyboard opened (the A
 * that opened it) count only after they are released: Pad.prev starts with them. */
typedef struct { uint64_t prev, held_since[16]; ULONGLONG last_rep[16]; } Pad;
static int step(Osk *k, Pad *pd, int *done, int *result) {
    int dirty = 0;
    UINT qm[KQ]; WPARAM qw[KQ]; int qn;
    EnterCriticalSection(&g_lock);
    qn = g_qn; for (int i = 0; i < qn; i++) { qm[i] = g_q[i].msg; qw[i] = g_q[i].w; } g_qn = 0;
    LeaveCriticalSection(&g_lock);
    for (int i = 0; i < qn && !*done; i++) {
        dirty = 1; k->note = NULL;
        if (qm[i] == WM_CHAR) {
            wchar_t ch = (wchar_t)qw[i];
            if (ch == 8) del_back(k);
            else if (ch == 13) { if (try_ok(k)) { *done = 1; *result = 1; } }
            else if (ch == 27) { *done = 1; *result = 0; }
            else if (ch >= 0x20) ins(k, ch);
        } else switch (qw[i]) {
            case VK_LEFT: if (k->cur > 0) k->cur--; break;
            case VK_RIGHT: if (k->cur < k->len) k->cur++; break;
            case VK_HOME: k->cur = 0; break;
            case VK_END: k->cur = k->len; break;
            case VK_DELETE: del_fwd(k); break;
        }
    }
    uint64_t b = nnhid_menu_buttons(), down = b & ~pd->prev;
    ULONGLONG now = GetTickCount64();
    static const int rep[] = {B_LEFT, B_RIGHT, B_UP, B_DOWN, B_L, B_R, B_B};
    uint64_t fire = down;
    for (size_t i = 0; i < sizeof rep / sizeof rep[0]; i++) {
        int bit = rep[i];
        if (down >> bit & 1) { pd->held_since[bit] = now; pd->last_rep[bit] = now; }
        else if ((b >> bit & 1) && now - pd->held_since[bit] > 400 && now - pd->last_rep[bit] > 70) { fire |= 1ull << bit; pd->last_rep[bit] = now; }
    }
    pd->prev = b;
    if (fire && !*done) {
        dirty = 1; k->note = NULL;
        if (fire >> B_LEFT & 1) move(k, -1, 0);
        if (fire >> B_RIGHT & 1) move(k, 1, 0);
        if (fire >> B_UP & 1) move(k, 0, -1);
        if (fire >> B_DOWN & 1) move(k, 0, 1);
        if (fire >> B_L & 1) { if (k->cur > 0) k->cur--; }
        if (fire >> B_R & 1) { if (k->cur < k->len) k->cur++; }
        if (fire >> B_B & 1) del_back(k);
        if (fire >> B_Y & 1) ins(k, L' ');
        if (fire >> B_X & 1) { k->page = k->page == 2 ? 0 : 2; k->shift_once = 0; }
        if ((fire >> B_ZL & 1) || (fire >> B_LS & 1)) { k->page = k->page == 1 ? 0 : 1; k->shift_once = 0; }
        if (fire >> B_A & 1) press(k, done, result);
        if (fire >> B_PLUS & 1) { if (try_ok(k)) { *done = 1; *result = 1; } }
        if (fire >> B_MINUS & 1) { *done = 1; *result = 0; }
    }
    return dirty;
}

int osk_run(const wchar_t *header, const wchar_t *guide, const wchar_t *initial, int max_len, int min_len, int password, wchar_t *out) {
    InitOnceExecuteOnce(&g_once, osk_init, NULL, NULL);
    if (!nvn_front_osk_ready() || !setup_gdi()) return -1;
    static Osk k;
    memset(&k, 0, sizeof k);
    k.header = header; k.guide = guide; k.password = password;
    k.max_len = max_len > 0 && max_len <= OSK_MAX ? max_len : OSK_MAX;
    k.min_len = min_len > 0 && min_len <= k.max_len ? min_len : 0;
    if (initial) { wcsncpy(k.text, initial, (size_t)k.max_len); k.text[k.max_len] = 0; }
    k.len = k.cur = (int)wcslen(k.text);
    k.row = 1; k.col = 0;
    EnterCriticalSection(&g_lock); g_qn = 0; LeaveCriticalSection(&g_lock);
    publish(&k);
    InterlockedExchange(&g_on, 1);
    printf("osk: on-screen keyboard open (max %d, min %d%s)\n", k.max_len, k.min_len, password ? ", password" : "");
    fflush(stdout);
    static Pad pd;
    memset(&pd, 0, sizeof pd);
    pd.prev = nnhid_menu_buttons();
    long long seen_presents = nvn_front_presents();
    ULONGLONG last_present = GetTickCount64(), last_redraw = 0;
    int done = 0, result = 0;
    while (!done) {
        Sleep(16);
        ULONGLONG now = GetTickCount64();
        if (step(&k, &pd, &done, &result)) publish(&k);
        /* the game presents nothing while it waits here: present its last frame again with the keyboard, 30 times a second */
        long long p = nvn_front_presents();
        if (p != seen_presents) { seen_presents = p; last_present = now; }
        if (now - last_present > 50 && now - last_redraw >= 33) { last_redraw = now; nvn_front_osk_redraw(); }
    }
    g_closed_at = GetTickCount64();
    InterlockedExchange(&g_on, 0);
    nvn_front_osk_redraw();                         /* the frame without the keyboard */
    if (result) { wcsncpy(out, k.text, OSK_MAX + 1); out[OSK_MAX + 1] = 0; }
    printf("osk: closed: %s (%d characters)\n", result ? "OK" : "cancelled", k.len);
    fflush(stdout);
    return result;
}

/* ---------------------------------------------------------------- the inline keyboard (tomo.c: nn::swkbd::InlineKeyboard)
 * Tomodachi Life shows the console's keyboard as an image inside its own pages, and its own text box shows the text. The
 * same keyboard is drawn here into that image (osk_inline_image: the game's Rodin font, cream and orange, the rest of the
 * image transparent) and tomo.c hands every change of the text to the game at once: typing happens in the game's page,
 * nothing opens over the window. While it is up the game gets no buttons or keys (nnhid.c osk_blocks_input). */
int nnfontll_gdi_face(const char *prefix, WCHAR *face, int *weight);      /* nnfontll.c: a font the game loaded */
static Osk g_in;
static Pad g_in_pad;
static int g_in_drawn = -1, g_in_ver;
static HDC g_idc;
static uint32_t *g_ipx;
static int g_iw, g_ih;
static WCHAR g_face[2][LF_FACESIZE];               /* [0] regular text, [1] the keys: the game's Rodin B / UB, else Segoe UI */
static int g_face_w[2];
static struct { int px, heavy; HFONT f; } g_fc[16];
static HFONT font_px(int px, int heavy) {
    for (int i = 0; i < 16; i++) if (g_fc[i].f && g_fc[i].px == px && g_fc[i].heavy == heavy) return g_fc[i].f;
    if (!g_face[0][0]) {
        static const char *const want[2] = {"nintendoP_RodinNTLG-B", "nintendoP_RodinNTLG-UB"};
        for (int h = 0; h < 2; h++)
            if (!nnfontll_gdi_face(want[h], g_face[h], &g_face_w[h])) { wcscpy(g_face[h], L"Segoe UI"); g_face_w[h] = h ? FW_HEAVY : FW_BOLD; }
        printf("osk: inline keyboard font \"%ls\" / \"%ls\"\n", g_face[0], g_face[1]);
    }
    int slot = 0;
    for (int i = 0; i < 16; i++) if (!g_fc[i].f) { slot = i; break; }
    if (g_fc[slot].f) DeleteObject(g_fc[slot].f);
    g_fc[slot].px = px; g_fc[slot].heavy = heavy;
    g_fc[slot].f = CreateFontW(-px, 0, 0, 0, g_face_w[heavy], 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, g_face[heavy]);
    return g_fc[slot].f;
}
#define IK_KEY RGB(1, 254, 1)                      /* the transparent part of the picture (no drawing produces it) */
/* The keyboard panel at the bottom of a w x h picture (design units: 1920 x 1080, s = w / 1920), into g_idc. */
static void draw_inline(const Osk *k, int w, int h) {
    double s = w / 1920.0;
#define U(v) ((int)((v) * s + 0.5))
    /* the game's own colours (its name page): navy text, its yellow, cream, the orange of its OK buttons */
    const COLORREF panel = RGB(255, 243, 207), rim = RGB(255, 194, 14), keyc = RGB(255, 255, 255), keyrim = RGB(240, 222, 170),
                   ink = RGB(28, 40, 72), focus = RGB(255, 122, 0), focusrim = RGB(230, 95, 0), fkey = RGB(255, 226, 120),
                   lit = RGB(255, 200, 40), okc = RGB(255, 122, 0), soft = RGB(70, 80, 110), white = RGB(255, 255, 255);
    HDC dc = g_idc;
    { HBRUSH br = CreateSolidBrush(IK_KEY); RECT r = {0, 0, w, h}; FillRect(dc, &r, br); DeleteObject(br); }
    HDC keep = g_dc;
    g_dc = dc;                                     /* rbox / text draw into g_dc */
    rbox(U(60), h - U(592), U(1800), U(568), U(60), panel, rim, U(6) > 1 ? U(6) : 1);
    /* the help line: a button and what it does */
    static const struct { const wchar_t *b, *what; } hint[] = {{L"A", L"Type"}, {L"B", L"Delete"}, {L"Y", L"Space"}, {L"X", L"Symbols"}, {L"+", L"OK"}, {L"\x2212", L"Cancel"}};
    int hx = U(1820), hy = h - U(566), hs = U(40);
    for (int i = (int)(sizeof hint / sizeof hint[0]) - 1; i >= 0; i--) {
        SIZE sz;
        SelectObject(dc, font_px(U(30), 0));
        GetTextExtentPoint32W(dc, hint[i].what, (int)wcslen(hint[i].what), &sz);
        hx -= sz.cx;
        text(font_px(U(30), 0), soft, hx, hy, sz.cx + U(4), hs, hint[i].what, -1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        hx -= U(8) + hs;
        rbox(hx, hy, hs, hs, hs, ink, ink, 1);
        text(font_px(U(26), 1), white, hx, hy, hs, hs, hint[i].b, -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        hx -= U(34);
    }
    static const wchar_t *const pname[3] = {L"abc", L"ABC", L"#+="};
    text(font_px(U(30), 1), soft, U(110), hy, U(300), hs, pname[k->page == 2 ? 2 : (k->page == 1 || k->shift_once) ? 1 : 0], -1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    /* the keys */
    int kx0 = U(100), ky0 = h - U(516), kh = U(82), gap = U(10), rowh = kh + gap;
    double kw = 1720.0 * s / COLS;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int on = k->row == r && k->col == c, x = kx0 + (int)(c * kw), x2 = kx0 + (int)((c + 1) * kw), y = ky0 + r * rowh;
            rbox(x + gap / 2, y, x2 - x - gap, kh, U(24), on ? focus : keyc, on ? focusrim : keyrim, on ? U(4) : U(2) > 1 ? U(2) : 1);
            wchar_t ch[2] = {key_char(k, r, c), 0};
            text(font_px(U(46), 1), on ? white : ink, x, y, x2 - x, kh, ch, 1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    static const wchar_t *const fname[F_N] = {L"Shift", L"#+=", L"Space", L"Delete", L"OK"};
    int col = 0, y = ky0 + ROWS * rowh;
    for (int f = 0; f < F_N; f++) {
        int x = kx0 + (int)(col * kw), x2 = kx0 + (int)((col + k_fspan[f]) * kw), on = k->row == ROWS && k->col == f;
        int lt = (f == F_SHIFT && (k->page == 1 || k->shift_once)) || (f == F_SYM && k->page == 2);
        COLORREF c = on ? focus : f == F_OK ? okc : lt ? lit : fkey;
        rbox(x + gap / 2, y, x2 - x - gap, kh, U(24), c, on ? focusrim : f == F_OK ? focusrim : keyrim, on ? U(4) : U(2) > 1 ? U(2) : 1);
        text(font_px(U(40), 1), on || f == F_OK ? white : ink, x, y, x2 - x, kh, f == F_SYM && k->page == 2 ? L"abc" : fname[f], -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        col += k_fspan[f];
    }
    GdiFlush();
    g_dc = keep;
#undef U
}
int osk_inline_open(const wchar_t *initial, int max_len) {      /* game thread (Appear) */
    InitOnceExecuteOnce(&g_once, osk_init, NULL, NULL);
    memset(&g_in, 0, sizeof g_in);
    g_in.max_len = max_len > 0 && max_len <= OSK_MAX ? max_len : 32;
    if (initial) { wcsncpy(g_in.text, initial, (size_t)g_in.max_len); g_in.text[g_in.max_len] = 0; }
    g_in.len = g_in.cur = (int)wcslen(g_in.text);
    g_in.row = 1;
    memset(&g_in_pad, 0, sizeof g_in_pad);
    g_in_pad.prev = nnhid_menu_buttons();
    EnterCriticalSection(&g_lock); g_qn = 0; LeaveCriticalSection(&g_lock);
    g_in_open = 1; g_in_ver++;
    InterlockedExchange(&g_on, 1);                 /* the window's keys come here, the game gets none */
    return 1;
}
/* The key under a point of the screen, in draw_inline's design units (1920 x 1080; the picture covers the screen): its row
 * (ROWS = the bottom row) and column (the function key there); 0 = no key. */
static int inline_hit(double ux, double uy, int *row, int *col) {
    const double kx0 = 100, ky0 = 1080 - 516, kh = 82, rowh = 92, kw = 1720.0 / COLS;
    if (ux < kx0 || ux >= kx0 + COLS * kw || uy < ky0) return 0;
    int r = (int)((uy - ky0) / rowh), c = (int)((ux - kx0) / kw);
    if (r > ROWS || uy - ky0 - r * rowh > kh) return 0;
    if (r == ROWS) { int f = 0, x = 0; while (f < F_N - 1 && x + k_fspan[f] <= c) x += k_fspan[f++]; c = f; }
    *row = r; *col = c;
    return 1;
}
/* game thread, once a frame (Calc): 0 nothing new, 1 the text or the cursor changed, 2 OK, 3 cancelled */
int osk_inline_step(wchar_t *text_out, int cap, int *len, int *cursor) {
    if (!g_in_open) return 0;
    int done = 0, result = 0, changed = step(&g_in, &g_in_pad, &done, &result);
    {   /* the mouse (PC Settings, Mouse controls): the key under it takes the focus when it moves; a left click types it, a
         * right click deletes (as B) */
        int gfx_mouse(void); int display_mouse(float *x, float *y, int *buttons); int nnhid_mouse(float xy[2]); int nnhid_mouse_buttons(void);
        static float px = -1.0f, py = -1.0f; static int pb = -1, test = -1;
        float mx, my, xy[2]; int mb, r, c, got;
        if (test < 0) test = getenv("TOMO_MOUSE_TEST") != NULL;      /* (a scripted mouse: the one nnhid.c plays) */
        if (test) { got = nnhid_mouse(xy); mx = xy[0]; my = xy[1]; mb = nnhid_mouse_buttons(); }
        else got = gfx_mouse() && display_mouse(&mx, &my, &mb);
        if (!done && got) {
            int hit = inline_hit(mx * 1.5, my * 1.5, &r, &c), click = (mb & 1) && pb >= 0 && !(pb & 1);
            if (hit && (mx != px || my != py || click) && (g_in.row != r || g_in.col != c)) { g_in.row = r; g_in.col = c; changed = 1; }
            if (hit && click) { g_in.note = NULL; press(&g_in, &done, &result); changed = 1; }
            if ((mb & 2) && pb >= 0 && !(pb & 2)) { g_in.note = NULL; del_back(&g_in); changed = 1; }
            px = mx; py = my; pb = mb;
        } else pb = -1;                             /* (a button already down when the mouse comes in does not click) */
    }
    if (changed) g_in_ver++;
    int n = g_in.len < cap - 1 ? g_in.len : cap - 1;
    memcpy(text_out, g_in.text, (size_t)n * sizeof(wchar_t)); text_out[n] = 0;
    *len = n; *cursor = g_in.cur < n ? g_in.cur : n;
    return done ? (result ? 2 : 3) : changed;
}
void osk_inline_close(void) {
    if (!g_in_open) return;
    g_in_open = 0;
    g_closed_at = GetTickCount64();
    InterlockedExchange(&g_on, 0);
}
/* game thread (GetImage): the keyboard drawn into dst (w x h, 4 bytes a pixel, R G B A, rows `pitch` bytes apart) when it
 * changed since the last call; 1 when dst was written. Drawn at twice the size and averaged: smooth edges and alpha. */
int osk_inline_image(uint8_t *dst, int w, int h, int pitch) {
    if (!g_in_open || w <= 0 || h <= 0 || (g_in_drawn == g_in_ver && g_iw == w && g_ih == h)) return 0;
    int W = w * 2, H = h * 2;
    if (!g_idc || g_iw != w || g_ih != h) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        if (!g_idc) g_idc = CreateCompatibleDC(NULL);
        void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(g_idc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!bmp || !bits) return 0;
        DeleteObject(SelectObject(g_idc, bmp));
        g_ipx = bits; g_iw = w; g_ih = h;
    }
    draw_inline(&g_in, W, H);
    const uint32_t key = 0x0001fe01u;              /* IK_KEY as the DIB stores it (B G R: 01 fe 01) */
    for (int y = 0; y < h; y++) {
        uint8_t *o = dst + (size_t)y * (size_t)pitch;
        for (int x = 0; x < w; x++, o += 4) {
            const uint32_t *p = g_ipx + (size_t)(y * 2) * W + x * 2, q[4] = {p[0] & 0xffffff, p[1] & 0xffffff, p[W] & 0xffffff, p[W + 1] & 0xffffff};
            unsigned r = 0, g = 0, b = 0, a = 0;
            for (int i = 0; i < 4; i++) if (q[i] != key) { r += q[i] >> 16 & 255; g += q[i] >> 8 & 255; b += q[i] & 255; a++; }
            if (!a) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            o[0] = (uint8_t)(r / a); o[1] = (uint8_t)(g / a); o[2] = (uint8_t)(b / a); o[3] = (uint8_t)(a * 255 / 4);
        }
    }
    g_in_drawn = g_in_ver;
    return 1;
}
#endif
