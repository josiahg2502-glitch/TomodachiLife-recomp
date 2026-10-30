/* Tomodachi Life (SDK 20.5.3): imports SSBU's main does not have, where the SDK code is a service call (IPC) or a
 * system answer and the lifted SDK code cannot be used. The pure SDK functions of this game are lifted instead
 * (sdklift_roots.txt, block "Tomodachi Life"). Offline-console answers as nnmisc.c gives them: success, nothing,
 * "not available". Registered after the C runtime (boot.c), so only names nobody else implements are bound here. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RESULT_UNAVAILABLE 0x3e87aull                    /* the "not available" Result nnmisc.c / nnsdk.c use */

static void keep(Ctx *c) { (void)c; }
static void ret0(Ctx *c) { RET(0); }
static void ret1(Ctx *c) { RET(1); }
static void unavailable(Ctx *c) { RET(RESULT_UNAVAILABLE); }
static void abort_trap(Ctx *c) { guest_trap("the game aborted (nn::diag / nn::detail abort or libc++ verbose abort)", c->x[30] - 4); }

void os_init_event(uint64_t key, int autoclear);         /* os_threads.c */

/* ---- nn::hid: GetNpadStates(State *out, int count, const NpadIdType &id): newest first; one state is given */
void hid_fill_pad_state(uint64_t out, uint32_t id, int kind);   /* nnhid.c */
static void npad_states_fullkey(Ctx *c) { if ((int)A1 < 1) { RET(0); return; } hid_fill_pad_state(A0, rd32(A2), 1); RET(1); }
static void npad_states_other(Ctx *c) { if ((int)A1 < 1) { RET(0); return; } hid_fill_pad_state(A0, rd32(A2), 0); RET(1); }
static void touch_states(Ctx *c) {                       /* TouchScreenState<16>: sampling, count, 16 touches: no touch */
    if ((int)A1 < 1) { RET(0); return; }
    memset((void *)(uintptr_t)A0, 0, 0x290);
    RET(1);
}
/* ControllerSupportArg: player count min, max, take over, left justify, permit dual, single mode (+5), ... The game asks in
 * single mode whenever more than one controller is connected; on the console the applet then leaves one (nnhid.c
 * nnhid_single_mode). Left as it was, the game asked again a few frames later and paused all sound every time. */
static void controller_support(Ctx *c) {                 /* ControllerSupportResultInfo { int playerCount; NpadIdType selectedId; } */
    void nnhid_single_mode(int on);
    nnhid_single_mode(rd8(A1 + 5) != 0);
    if (getenv("SSBU_HIDLOG")) printf("hidlog: ShowControllerSupport min %u max %u single %u%c", rd8(A1), rd8(A1 + 1), rd8(A1 + 5), 10);
    wr32(A0, 1); wr32(A0 + 4, 0);
    RET(0);
}

/* ---- nn::time: ClockSnapshot and the adjustable clocks, from the host's clock in the Windows time zone (nntime.c). The island's
 * time of day comes from these: with UTC as the device's zone the island was hours off (evening light in the afternoon). */
void nntime_location(char out[0x24]); void nntime_local_calendar(int64_t posix, uint64_t out); void nntime_to_posix_rule(Ctx *c); int64_t nntime_now(void);
#define posix_now nntime_now
static void time_user_clock_now(Ctx *c) { wr64(A0, (uint64_t)posix_now()); RET(0); }
static void time_location_name(Ctx *c) { nntime_location((char *)(uintptr_t)A0); }
/* ClockSnapshot (0xd0 bytes in SDK 20, opaque to the game): our own layout, read only by the accessors below */
#define SNAP_TIME 0x00
#define SNAP_CAL 0x08                                    /* CalendarTime: year (16), month, day, hour, minute, second (8 each) */
static void time_snapshot_create(Ctx *c) {
    int64_t t = posix_now();
    uint64_t s = A0;
    memset((void *)(uintptr_t)s, 0, 0xd0);
    wr64(s + SNAP_TIME, (uint64_t)t);
    nntime_local_calendar(t, s + SNAP_CAL);
    RET(0);
}
static void snap_posix(Ctx *c) { RET(rd64(A0 + SNAP_TIME)); }
static void snap_calendar(Ctx *c) { RET(rd64(A0 + SNAP_CAL)); }     /* CalendarTime fits in x0 (8 bytes) */
static void snap_context(Ctx *c) {                       /* SystemClockContext (0x20) returned through x8 */
    uint64_t out = c->x[8];
    memset((void *)(uintptr_t)out, 0, 0x20);
    wr64(out, rd64(A0 + SNAP_TIME));
}
static void snap_location(Ctx *c) {                      /* LocationName (0x24) through x8 */
    uint64_t out = c->x[8];
    nntime_location((char *)(uintptr_t)out);
}
static void time_difference(Ctx *c) { RET(0); }          /* TimeSpan between two snapshots of the user clock: none adjusted */

/* ---- nn::fs extras */
static void fs_free_space(Ctx *c) { wr64(A0, 0x40000000ull); RET(0); }       /* 1 GiB free */
static void fs_save_size_max(Ctx *c) { wr64(A0, 0x10000000ull); wr64(A1, 0x10000000ull); RET(0); }
/* IsSaveDataExisting(ApplicationId, Uid): the game asks for the demo version's save (application 0100ca502552a000) to offer
 * a transfer. There is no other application's save data here: answering yes showed the offer in every new game, and
 * "Transfer Data" then aborted (MountSaveDataReadOnly fails and the game's result check aborts). */
static void fs_save_exists(Ctx *c) { RET(0); }
static void fs_read_file_opt(Ctx *c) {                   /* ReadFile(size_t *out, handle, offset, buf, size, ReadOption): the 5-argument form */
    ImportFn f = import_lookup("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
    if (!f) { RET(RESULT_UNAVAILABLE); return; }
    f(c);
}

/* ---- nn::mii::Database offline: no Miis in the console's database */
static void mii_db_get_index(Ctx *c) { wr32(A0, (uint32_t)-1); RET(RESULT_UNAVAILABLE); }

/* ---- nn::ngc::ProfanityFilter: no word is profane */
/* CheckProfanityWords is a member: (this, uint32_t *results, uint32_t patterns, const char16_t / char **words, size_t count);
 * each result is the bit set of the patterns the word matches: 0 for every word */
static void ngc_check_words(Ctx *c) {
    uint64_t results = A1, count = c->x[4];
    static int said;
    if (said++ < 5) printf("nn::ngc: CheckProfanityWords: %llu word(s), none profane\n", (unsigned long long)count);
    if (results && count < 4096) memset((void *)(uintptr_t)results, 0, (size_t)count * 4);
    RET(0);
}

/* ---- nn::swkbd::InlineKeyboard. The console draws its keyboard into an image the game shows and reports through
 * callbacks; here the text comes from the host's text prompt (nnapplet.c swkbd_prompt: a dialog, or SSBU_SWKBD=text:<..>)
 * or, by default, from our own keyboard (osk.c) drawn into the image the game shows, the text handed over as it is typed
 * (ik_ours_calc). Order the game relies on [0x710209b8d4, 0x710209b9f0]: Initialize, then the
 * callbacks are set; Calc runs only once the game wants the keyboard (+0x4b1) and FinishedInitialize (which sets +0x4b0
 * and calls Appear) must come from a Calc. Then ChangedString carries the text (UTF-16 at +0, two ints at +0x3f0 / +0x3f4
 * [0x710209dd6c]) and DecidedEnter / DecidedCancel close the keyboard (their argument is not read [0x710209bd18]).
 * FinishedKeyboard is never sent: it means the keyboard applet ended, and the game initializes a new one [0x710209dcbc].
 * Calc returns the state (see ik_calc): 0 idle (the game then sets up its text box, [0x71020999e8] -> 0x710209af7c, which the text
 * callbacks need), 3 shown. One keyboard at a time. */
int swkbd_prompt(const wchar_t *guide, const wchar_t *initial, int max_len, wchar_t *out);      /* nnapplet.c */
#define IK_W 1280
#define IK_H 720
static struct {
    uint64_t cb_init, cb_finished, cb_changed, cb_moved, cb_enter, cb_cancel;
    int initialized, init_reported, shown, max_len, shown_frames, text_sent, closed_frames;
    volatile LONG answer;                                 /* 0 none yet, 1 text ready, 2 cancelled */
    HANDLE thread;
    wchar_t initial[512], text[512];
    int image_sent;
    int ours, sent_len, sent_cur, dirty, decided_at;      /* our keyboard (osk.c) in the game's image, the text sent live */
} g_ik;
/* osk.c: the keyboard drawn into the game's keyboard image, the text handed over as it is typed */
int osk_inline_open(const wchar_t *initial, int max_len);
int osk_inline_step(wchar_t *text, int cap, int *len, int *cursor);
void osk_inline_close(void);
int osk_inline_image(uint8_t *dst, int w, int h, int pitch);
int osk_key(UINT msg, WPARAM w);
/* Ours unless a test answers the keyboard (SSBU_SWKBD, or a scripted run: nnapplet.c kbd_mode) or TOMO_IK=dialog asks for
 * the desktop prompt. TOMO_IK_TEST=<text>: ours in a scripted run too, the text typed into it, then OK (test only). */
static int ik_use_ours(void) {
    const char *t = getenv("TOMO_IK");
    if (getenv("TOMO_IK_TEST")) return 1;
    if (t && !strcmp(t, "dialog")) return 0;
    return !getenv("SSBU_SWKBD") && !getenv("SSBU_SEQ") && !getenv("SSBU_PRESS") && !getenv("SSBU_SCRIPT");
}
void nvn_front_shot_step(int step);                       /* nvn_gl.c: the next frame as step_<n>.bmp (SSBU_SHOT_STEPS) */
static DWORD WINAPI ik_test_typist(void *p) {
    const char *s = p;
    int i = 0;
    Sleep(1500);
    nvn_front_shot_step(900);                             /* the keyboard just open */
    for (; *s; s++, i++) { osk_key(WM_CHAR, (WPARAM)(unsigned char)*s); Sleep(150); if (i == 2) nvn_front_shot_step(901); }
    Sleep(400);
    nvn_front_shot_step(902);                             /* all typed */
    Sleep(400);
    osk_key(WM_CHAR, 13);
    Sleep(1500);
    nvn_front_shot_step(903);                             /* after OK */
    return 0;
}
static uint8_t g_ik_arg[0x400] __attribute__((aligned(16)));
static void ik_call(Ctx *c, uint64_t fn, uint64_t arg) {
    if (!fn) return;
    uint64_t saved[12];
    for (int i = 0; i < 12; i++) saved[i] = c->x[19 + i];
    c->x[0] = arg;
    call_guest(c, fn);
    for (int i = 0; i < 12; i++) c->x[19 + i] = saved[i];
}
static DWORD WINAPI ik_prompt_thread(void *p) {
    (void)p;
    wchar_t out[512];
    int ok = swkbd_prompt(L"Enter text", g_ik.initial, g_ik.max_len, out);
    if (ok) { wcsncpy(g_ik.text, out, 511); g_ik.text[511] = 0; }
    InterlockedExchange(&g_ik.answer, ok ? 1 : 2);
    return 0;
}
static void ik_ctor(Ctx *c) { (void)c; memset(&g_ik, 0, sizeof g_ik); }
static void swkbd_work_size(Ctx *c) { RET(0x1000); }
static void swkbd_image_req(Ctx *c) { wr64(A1, (uint64_t)IK_W * IK_H * 4); wr64(A2, 0x1000); }
static void swkbd_window_size(Ctx *c) { wr32(A1, IK_W); wr32(A2, IK_H); }
static void ik_initialize(Ctx *c) { g_ik.initialized = 1; g_ik.init_reported = 0; RET(1); }
static void ik_finalize(Ctx *c) { (void)c; if (g_ik.ours) osk_inline_close(); g_ik.initialized = 0; g_ik.shown = 0; }
static void ik_set_init_cb(Ctx *c) { g_ik.cb_init = A1; }
static void ik_set_finished_cb(Ctx *c) { g_ik.cb_finished = A1; }
static void ik_set_changed_cb(Ctx *c) { g_ik.cb_changed = A1; }
static void ik_set_moved_cb(Ctx *c) { g_ik.cb_moved = A1; }
static void ik_set_enter_cb(Ctx *c) { g_ik.cb_enter = A1; }
static void ik_set_cancel_cb(Ctx *c) { g_ik.cb_cancel = A1; }
static void ik_input_text_set(Ctx *c) {                   /* InputText::Set(const char16_t *): the text at +0 of the InputText */
    uint64_t it = A0, s = A1;
    int n = 0;
    for (; s && n < 0x1f7; n++) { uint16_t ch = rd16(s + (uint64_t)n * 2); if (!ch) break; wr16(it + (uint64_t)n * 2, ch); }
    wr16(it + (uint64_t)n * 2, 0);
}
static void ik_set_input_text(Ctx *c) {
    uint64_t it = A1;
    int n = 0;
    for (; it && n < 500; n++) { uint16_t ch = rd16(it + (uint64_t)n * 2); if (!ch) break; g_ik.initial[n] = ch; }
    g_ik.initial[n] = 0;
}
static uint8_t g_ik_snap[0x1d00];                     /* TOMO_IKLOG: the owner at the first Appear, diffed 3 s after the decision */
static uint64_t g_ik_snap_owner;
static volatile ULONGLONG g_ik_decided_at;
static void ik_appear(Ctx *c) {                           /* AppearArg: +0x1c the most characters the game allows */
    int ml = (int)rd32(A1 + 0x1c);
    g_ik.max_len = ml > 0 && ml <= 500 ? ml : 32;
    if (g_ik.shown) return;
    if (getenv("TOMO_IKLOG") && !g_ik_snap_owner) { g_ik_snap_owner = c->x[19]; memcpy(g_ik_snap, (void *)(uintptr_t)c->x[19], sizeof g_ik_snap); }
    g_ik.shown = 1; g_ik.shown_frames = 0; g_ik.text_sent = 0;
    InterlockedExchange(&g_ik.answer, 0);
    g_ik.ours = ik_use_ours();
    if (g_ik.ours) {
        osk_inline_open(g_ik.initial, g_ik.max_len);
        g_ik.image_sent = 0; g_ik.sent_len = -1; g_ik.sent_cur = -1; g_ik.dirty = 1; g_ik.decided_at = 0;
        if (getenv("TOMO_IK_TEST")) CloseHandle(CreateThread(NULL, 0, ik_test_typist, (void *)getenv("TOMO_IK_TEST"), 0, NULL));
    } else g_ik.thread = CreateThread(NULL, 0, ik_prompt_thread, NULL, 0, NULL);
    printf("nn::swkbd: inline keyboard shown (at most %d characters, initial text \"%ls\", %s)\n", g_ik.max_len, g_ik.initial, g_ik.ours ? "our keyboard in the game's image" : "text prompt");
}
static void ik_disappear(Ctx *c) {
    (void)c;
    if (g_ik.shown) { printf("nn::swkbd: inline keyboard: Disappear\n"); g_ik.closed_frames = 30; }
    if (g_ik.ours) osk_inline_close();
    g_ik.shown = 0;
    InterlockedExchange(&g_ik.answer, 0);
}
/* ours, once a Calc: the text to the game whenever it changed (ChangedString: UTF-16 at +0, then length and cursor at
 * +0x3f0..+0x3fc), OK / cancel decided 10 frames after the last text (as the prompt's answer was) */
static void ik_text_send(Ctx *c, const wchar_t *text, int n, int cur) {
    uint64_t arg = (uint64_t)(uintptr_t)g_ik_arg;
    memset(g_ik_arg, 0, sizeof g_ik_arg);
    for (int i = 0; i < n && i < 0x1f7; i++) wr16(arg + (uint64_t)i * 2, (uint16_t)text[i]);
    wr32(arg + 0x3f0, (uint32_t)n);
    for (int k = 1; k < 4; k++) wr32(arg + 0x3f0 + (uint64_t)k * 4, (uint32_t)cur);
    ik_call(c, g_ik.cb_changed, arg);
    g_ik.sent_len = n; g_ik.sent_cur = cur; g_ik.text_sent = g_ik.shown_frames;
}
static void ik_ours_calc(Ctx *c) {
    wchar_t text[512];
    int n = 0, cur = 0, r = osk_inline_step(text, 512, &n, &cur);
    if (r == 1) g_ik.dirty = 1;
    if (r >= 2 && !g_ik.decided_at) { g_ik.decided_at = r; g_ik.dirty = 1; wcsncpy(g_ik.text, text, 511); g_ik.text[511] = 0; }
    if (g_ik.shown_frames < 10) return;                    /* the game makes its text box first */
    if (g_ik.dirty) { g_ik.dirty = 0; if (n != g_ik.sent_len || cur != g_ik.sent_cur || g_ik.decided_at) ik_text_send(c, text, n, cur); }
    if (g_ik.decided_at && g_ik.shown_frames >= g_ik.text_sent + 10) {
        int ok = g_ik.decided_at == 2;
        osk_inline_close();
        g_ik.shown = 0; g_ik.closed_frames = 30; g_ik.decided_at = 0;
        g_ik_decided_at = GetTickCount64();
        printf("nn::swkbd: inline keyboard: %s \"%ls\"\n", ok ? "decided" : "cancelled", ok ? g_ik.text : L"");
        if (ok) ik_call(c, g_ik.cb_enter, (uint64_t)(uintptr_t)g_ik_arg); else ik_call(c, g_ik.cb_cancel, 0);
    }
}
/* TOMO_IKLOG=1 (diagnosis): the game's keyboard owner (x19 at Calc [0x7100359e8c]) printed when it changes, from Calc and
 * from a watcher thread once a second (Calc stops being called when the keyboard layer hides) */
static volatile uint64_t g_ik_owner;
static void ik_print(const char *what) {
    static uint32_t last[9];
    uint64_t o = g_ik_owner;
    if (!o) return;
    uint32_t v[9] = {rd32(o + 0x348), rd8(o + 0x4b0), rd8(o + 0x4b1), rd8(o + 0x4b2), rd32(o + 0x4b4), rd32(o + 0x1c18), (uint32_t)(rd64(o + 0x4d8) != 0), (uint32_t)g_ik.shown, rd32(o + 0x470)};
    if (!memcmp(v, last, sizeof v)) return;
    memcpy(last, v, sizeof v);
    printf("ik: %s state %u init %u want %u decided %u 4b4 %u 1c18 %u textbox %u shown %u calc %u\n", what, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
    fflush(stdout);
}
static DWORD WINAPI ik_watch(void *p) {
    (void)p;
    for (;;) {
        Sleep(1000);
        ik_print("watch");
        if (g_ik_decided_at && GetTickCount64() - g_ik_decided_at > 3000 && g_ik_snap_owner) {
            g_ik_decided_at = 0;
            {   const char *fx = getenv("TOMO_IKFIX");    /* experiment: <hex offset>=<hex value> written into the owner */
                unsigned off, val;
                if (fx && sscanf(fx, "%x=%x", &off, &val) == 2) { wr32(g_ik_snap_owner + off, val); printf("ik: wrote %x to owner +%x\n", val, off); }
            }
            printf("ik: owner fields changed since Appear (offset: then -> now):\n");
            int n = 0;
            for (uint32_t off = 0; off < sizeof g_ik_snap && n < 120; off += 4) {
                uint32_t then, now = rd32(g_ik_snap_owner + off);
                memcpy(&then, g_ik_snap + off, 4);
                if (then != now) { printf("  +%04x: %08x -> %08x\n", off, then, now); n++; }
            }
            fflush(stdout);
        }
    }
}
static void ik_trace(Ctx *c, const char *what) {
    static int on = -1;
    if (on < 0) { on = getenv("TOMO_IKLOG") != NULL; if (on) CloseHandle(CreateThread(NULL, 0, ik_watch, NULL, 0, NULL)); }
    if (!on) return;
    g_ik_owner = c->x[19];
    ik_print(what);
}
static void ik_calc(Ctx *c) {
    ik_trace(c, "calc");
    if (!g_ik.initialized) { RET(0); return; }
    if (!g_ik.init_reported) { g_ik.init_reported = 1; ik_call(c, g_ik.cb_init, 0); }
    LONG a = g_ik.answer;
    if (g_ik.shown) g_ik.shown_frames++;
    if (g_ik.shown && g_ik.ours) ik_ours_calc(c);
    /* the text some frames after Appear (the game makes its text box then), the decision some frames after the text */
    if (g_ik.shown && !g_ik.ours && a == 1 && !g_ik.text_sent && g_ik.shown_frames >= 10) {
        uint64_t arg = (uint64_t)(uintptr_t)g_ik_arg;
        int n = 0;
        memset(g_ik_arg, 0, sizeof g_ik_arg);
        for (; g_ik.text[n] && n < 0x1f7; n++) wr16(arg + (uint64_t)n * 2, (uint16_t)g_ik.text[n]);
        for (int k = 0; k < 4; k++) wr32(arg + 0x3f0 + (uint64_t)k * 4, (uint32_t)n);
        printf("nn::swkbd: inline keyboard text \"%ls\"\n", g_ik.text);
        ik_call(c, g_ik.cb_changed, arg);
        g_ik.text_sent = g_ik.shown_frames;
    }
    if (g_ik.shown && !g_ik.ours && (a == 2 || (a == 1 && g_ik.text_sent && g_ik.shown_frames >= g_ik.text_sent + 10))) {
        InterlockedExchange(&g_ik.answer, 0);
        if (g_ik.thread) { CloseHandle(g_ik.thread); g_ik.thread = NULL; }
        /* decided: the keyboard closes in this same Calc (state 1). The game's closing step [0x7102099a80] waits for a 1 and
         * otherwise calls Disappear, after which Calc is no longer called: a later 1 would never arrive. */
        g_ik.shown = 0; g_ik.closed_frames = 30;
        g_ik_decided_at = GetTickCount64();
        if (a == 1) { printf("nn::swkbd: inline keyboard: decided\n"); ik_call(c, g_ik.cb_enter, (uint64_t)(uintptr_t)g_ik_arg); }
        else { printf("nn::swkbd: inline keyboard cancelled\n"); ik_call(c, g_ik.cb_cancel, 0); }
    }
    /* 3 shown; 1 closed (after Disappear the game calls Disappear again each frame until it sees 1 [0x7102099a98]);
     * 0 idle (the game sets up its text box then [0x71020999e8]) */
    if (g_ik.shown) { RET(3); return; }
    if (g_ik.closed_frames > 0) { g_ik.closed_frames--; RET(1); return; }
    RET(0);
}
static void ik_get_image(Ctx *c) {                        /* (void *image, size_t size): transparent, sent once */
    /* our keyboard: redrawn into the buffer when it changed; the answer is 1 on every call while it is up. The game reads 1
     * as "the keyboard is visible": it writes the buffer into its texture (nvnTextureWriteTexels, linear RGBA) and shows the
     * pane; a 0 hides the pane [0x7100359eb0]. */
    if (g_ik.ours && g_ik.shown && A1 && A2 >= (uint64_t)IK_W * IK_H * 4) {
        osk_inline_image((uint8_t *)(uintptr_t)A1, IK_W, IK_H, IK_W * 4);
        RET(1);
        return;
    }
    if (g_ik.image_sent || !A1) { RET(0); return; }
    memset((void *)(uintptr_t)A1, 0, (size_t)A2);
    g_ik.image_sent = 1;
    RET(1);
}

/* ---- nn::prepo: the report is dropped; the "over frequency" event is one that is never signalled */
static uint8_t g_prepo_event[0x40];
static void prepo_limit_event(Ctx *c) {
    static int done;
    if (!done) { os_init_event((uint64_t)(uintptr_t)g_prepo_event, 0); done = 1; }
    RET((uint64_t)(uintptr_t)g_prepo_event);
}


/* ---- nn::audio renderer service. The configuration API is the SDK's own (lifted); these are the calls that go to the
 * audren service on the console. For now the renderer only keeps time: once started it signals the game's system event
 * every audio frame (5 ms, the console's rate), and an update is accepted without mixing (no sound yet). */
void os_signal_event(uint64_t key);                      /* os_threads.c */
typedef struct { uint64_t obj[2]; uint64_t vtbl[32]; uint64_t event; HANDLE timer; uint64_t frames; volatile LONG run; } Renderer;
static Renderer g_ren[4];
static void audren_request_update(Ctx *c, void *user);
/* The audio frame clock: the game's system event every 5 ms (200 a second, 240 samples each at 48 kHz). A timer-queue
 * timer ran at the system's 15.6 ms granularity, 64 ticks a second: the game made a third of the sound it should and the
 * output stuttered. A thread on a high-resolution waitable timer, catching up when it falls behind. */
static DWORD WINAPI ren_clock(void *p) {
    Renderer *r = p;
    HANDLE t = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerW(NULL, FALSE, NULL);
    LARGE_INTEGER f, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&now);
    long long period = f.QuadPart / 200, next = now.QuadPart + period;
    int display_ff(void);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (r->run) {
        period = f.QuadPart / (200 * (long long)display_ff());                 /* fast-forward: as many audio frames as game frames */
        QueryPerformanceCounter(&now);
        long long wait = next - now.QuadPart;
        if (wait > 0) { LARGE_INTEGER due; due.QuadPart = -(wait * 10000000 / f.QuadPart); SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE); WaitForSingleObject(t, 20); }
        os_signal_event(r->event);
        next += period;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - next > f.QuadPart / 10) next = now.QuadPart;        /* more than 100 ms behind: no burst to catch up */
    }
    CloseHandle(t);
    return 0;
}
static void audio_open(Ctx *c) {                         /* (AudioRendererHandle *out, SystemEvent *event, const Parameter &, void *work, size_t) */
    int i = 0;
    while (i < 4 && g_ren[i].event) i++;
    if (i == 4) { RET(RESULT_UNAVAILABLE); return; }
    Renderer *r = &g_ren[i];
    static uint64_t fn_update, fn_other;
    if (!fn_update) { fn_update = hostfn_closure("audren:RequestUpdate", audren_request_update, NULL); fn_other = hostfn_address("audren:other", ret0); }
    for (int k = 0; k < 32; k++) r->vtbl[k] = fn_other;
    r->vtbl[0x40 / 8] = fn_update;                       /* [sdk 0x221160]: ldr x8, [handle]; ldr x8, [x8, #0x40]; blr x8 */
    r->obj[0] = (uint64_t)(uintptr_t)r->vtbl; r->obj[1] = (uint64_t)i;
    r->event = A1; r->frames = 0;
    os_init_event(A1, 1);
    wr64(A0, (uint64_t)(uintptr_t)r->obj);               /* AudioRendererHandle is 16 bytes: the object, then nothing */
    wr64(A0 + 8, 0);
    RET(0);
}
static Renderer *ren_of(uint64_t h) { for (int i = 0; i < 4; i++) if (h == (uint64_t)(uintptr_t)g_ren[i].obj) return &g_ren[i]; return NULL; }
static void audio_start(Ctx *c) {
    Renderer *r = ren_of(A0);
    if (r && !r->timer) { r->run = 1; r->timer = CreateThread(NULL, 0, ren_clock, r, 0, NULL); }
    RET(0);
}
static void audio_stop(Ctx *c) {
    Renderer *r = ren_of(A0);
    if (r && r->timer) { r->run = 0; WaitForSingleObject(r->timer, 1000); CloseHandle(r->timer); r->timer = NULL; }
    RET(0);
}
static void audio_close(Ctx *c) { audio_stop(c); Renderer *r = ren_of(A0); if (r) r->event = 0; }
static void audio_work_size(Ctx *c) { RET(0x100000); }
/* RequestUpdateAudioRenderer is the SDK's own (lifted, sdklift_roots.txt): it serializes the game's voices, pools, effects
 * and sinks into the config's input buffer, calls the service (audren_request_update below) and unpacks the reply into the
 * game's objects: played samples and released wave buffers of each voice, the elapsed frame count (render info). Before,
 * the public function itself was answered here and the game's voices never moved (any wait for a sound to end hung). */
/* The audren update, as the console's service answers it (layouts: Eden audio_core, renderer/behavior/info_updater.cpp).
 * Input (config +0x48, the SDK's own serialization): header 0x40 {revision, behaviour, memory pools, voices, voice
 * resources, effects, mixes, sinks, performance, splitter, render info sizes; total at +0x3c}, then the sections in that
 * order. Output (config +0x58, capacity +0x60): header 0x40 with the output sizes, then memory pools (0x10 each), voices
 * (0x10), effects (0x10 / 0x90 from revision 9), sinks (0x20), performance (0x10), behaviour (0xb0), render info (0x10).
 * The client [sdk 0x22116c..0x221238] checks every section size and that the total equals the capacity.
 * A SILENT renderer: no mixing; a playing voice consumes its wave buffers at its sample rate (240 samples a frame at
 * 48 kHz) so the game's streaming and sound-state logic run as on the console. */
typedef struct { uint64_t played; uint32_t consumed, popped, pos, last_index, seen; float frac; int32_t af; int16_t yn1, yn2, abuf[14];
                 int bq_on[2]; double bq[2][6][4]; int heard; } SilentVoice;      /* the voice's two biquad filters: on last update, state per channel */
static SilentVoice g_sv[1024];
static int g_bq_off = -1;                                /* TOMO_AUDIO_BQ=0: the voices' biquad filters are not applied (A/B) */
static unsigned long long g_bq_voices;                   /* voice updates with a filter on (TOMO_AUDIOLOG) */
/* ---- the mixer: what the console's DSP does, cut down. Each playing voice is read from its wave buffers (PCM16, or
 * Nintendo's DSP-ADPCM: 8-byte frames of 14 samples, coefficients at the voice's source data, history from the wave
 * buffer's context or carried over), resampled to 48 kHz by its rate and pitch (linear interpolation), scaled by its volume and by the left /
 * right mix volumes of its channel resources, and summed into one stereo frame of 240 samples per update (5 ms). The
 * mixes and splitters behind a voice are applied (the mix graph above); effects and filters are not.
 * TOMO_AUDIO=0: silent (the voices still advance). */
#include <SDL3/SDL.h>
static SDL_AudioStream *g_audio_out;
static int g_audio_on = -1;
static float g_mix[240][2];
int nnaudio_default_volume(void);
int nnaudio_volume(void);
void nnaudio_set_volume(int percent);
int tomo_audio_volume(void) { return g_audio_out ? (int)(SDL_GetAudioStreamGain(g_audio_out) * 100.0f + 0.5f) : -1; }
int tomo_audio_set_volume(int percent) { return g_audio_out && SDL_SetAudioStreamGain(g_audio_out, (float)percent / 100.0f); }
static float rdf(uint64_t a) { uint32_t u = rd32(a); float f; memcpy(&f, &u, 4); return f; }
static int audio_out_open(void) {
    if (g_audio_on >= 0) return g_audio_on;
    const char *e = getenv("TOMO_AUDIO");
    g_audio_on = 0;
    if ((e && e[0] == '0') || getenv("SSBU_AUDIO_MUTE")) return 0;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { printf("audio: SDL audio cannot start: %s (silent)\n", SDL_GetError()); return 0; }
    SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, 48000};
    g_audio_out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_audio_out) { printf("audio: no playback device: %s (silent)\n", SDL_GetError()); return 0; }
    int percent = nnaudio_default_volume();
    if (!tomo_audio_set_volume(percent) || !SDL_ResumeAudioStreamDevice(g_audio_out)) {
        printf("audio: playback setup failed: %s (silent)\n", SDL_GetError());
        SDL_DestroyAudioStream(g_audio_out); g_audio_out = NULL; return 0;
    }
    printf("audio: output 48 kHz stereo through SDL, host volume %d%% (TOMO_AUDIO=0 silences it)\n", percent);
    g_audio_on = 1;
    return 1;
}
static void audio_volume_selftest(void) {
    const char *test = getenv("SSBU_AUDIO_VOLUME_SELFTEST");
    if (!test) return;
    int expected = atoi(test), bad = 0;
    if (!SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE)) {
        printf("audio-volume self-test: cannot select dummy driver: FAIL\n"); fflush(stdout); ExitProcess(1);
    }
    int opened = audio_out_open();
    if (expected < 0) {
        nnaudio_set_volume(100);
        bad = opened || g_audio_out || nnaudio_volume() != -1;
    } else {
        float want = (float)expected / 100.0f;
        bad = !opened || nnaudio_volume() != expected || !g_audio_out;
        if (g_audio_out) {
            bad |= SDL_GetAudioStreamGain(g_audio_out) != want;
            const int input[] = {-10, 23, 150, 0}, output[] = {0, 23, 100, 0};
            for (int i = 0; i < 4; i++) {
                nnaudio_set_volume(input[i]);
                bad |= nnaudio_volume() != output[i] || SDL_GetAudioStreamGain(g_audio_out) != (float)output[i] / 100.0f;
            }
        }
    }
    printf("audio-volume self-test: startup %d%%, SDL gain/getter/live setter/clamps: %s\n", expected, bad ? "FAIL" : "PASS");
    if (g_audio_out) { SDL_DestroyAudioStream(g_audio_out); g_audio_out = NULL; }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    fflush(stdout); ExitProcess(bad ? 1 : 0);
}
static void audio_out_frame(void) {
    if (!g_audio_out) return;
    int16_t pcm[240 * 2];
    for (int k = 0; k < 240; k++)
        for (int ch = 0; ch < 2; ch++) {
            float x = g_mix[k][ch] * 0.8f;
            pcm[k * 2 + ch] = (int16_t)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : x);
        }
    if (SDL_GetAudioStreamQueued(g_audio_out) < 48000 * 4 / 6)          /* at most ~170 ms queued: the game runs ahead otherwise */
        SDL_PutAudioStreamData(g_audio_out, pcm, (int)sizeof pcm);
    {   /* TOMO_AUDIO_WAV=<file>: everything mixed, as a 48 kHz stereo WAV (the header is rewritten every second) */
        static FILE *wav; static int tried; static uint32_t bytes;
        if (!tried) { tried = 1; const char *e = getenv("TOMO_AUDIO_WAV"); if (e && e[0]) { wav = fopen(e, "wb"); if (wav) fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x80\xbb\0\0\0\xee\x02\0\x04\0\x10\0data\0\0\0\0", 1, 44, wav); } }
        if (wav) {
            fwrite(pcm, 1, sizeof pcm, wav); bytes += sizeof pcm;
            if (bytes % (48000 * 4) < sizeof pcm) { uint32_t r = bytes + 36; long pos = ftell(wav); fseek(wav, 4, SEEK_SET); fwrite(&r, 4, 1, wav); fseek(wav, 40, SEEK_SET); fwrite(&bytes, 4, 1, wav); fseek(wav, pos, SEEK_SET); fflush(wav); }
        }
    }
}
static void adpcm_frame(SilentVoice *s, uint64_t data, int32_t f, uint64_t coef) {
    uint64_t base = data + (uint64_t)f * 8;
    uint8_t h = rd8(base);
    int scale = 1 << (h & 0xf), ci = (h >> 4) & 7;
    int c1 = (int16_t)rd16(coef + (uint64_t)ci * 4), c2 = (int16_t)rd16(coef + (uint64_t)ci * 4 + 2);
    for (int k = 0; k < 14; k++) {
        int nib = (rd8(base + 1 + (uint64_t)(k >> 1)) >> ((k & 1) ? 0 : 4)) & 0xf;
        if (nib >= 8) nib -= 16;
        int v = ((nib * scale) << 11) + 1024 + c1 * s->yn1 + c2 * s->yn2;
        v >>= 11;
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        s->yn2 = s->yn1; s->yn1 = (int16_t)v; s->abuf[k] = (int16_t)v;
    }
    s->af = f;
}
static int adpcm_peek(const SilentVoice *s, uint64_t data, int32_t f, uint64_t coef) {    /* the first sample of frame f, the state untouched */
    uint64_t base = data + (uint64_t)f * 8;
    uint8_t h = rd8(base);
    int scale = 1 << (h & 0xf), ci = (h >> 4) & 7;
    int c1 = (int16_t)rd16(coef + (uint64_t)ci * 4), c2 = (int16_t)rd16(coef + (uint64_t)ci * 4 + 2);
    int nib = (rd8(base + 1) >> 4) & 0xf;
    if (nib >= 8) nib -= 16;
    int v = (((nib * scale) << 11) + 1024 + c1 * s->yn1 + c2 * s->yn2) >> 11;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}
/* The mix graph (after the reference implementation, yuzu/eden audio_core: info_updater.cpp, command_generator.cpp,
 * mix_info.h, splitter_*.h). Mixes and splitter destinations are kept from update to update: with "dirty only" updates
 * the game sends just the mixes that changed. A voice adds into the buffers of its mix (its channel resource's 24 mix
 * volumes), or, without a mix, into the mixes of its splitter's destinations (channel, channel + channel count, ...); a
 * mix adds its buffers into its destination mix (its volume times its 24 x 24 volumes) or into its splitter's
 * destinations; the final mix (0) is the output. Effects (reverb, delay, aux) and biquad filters are not applied. */
#define AMIX_MAX 128
#define ASPL_MAX 128
#define ADST_MAX 512
typedef struct { int in_use, buffers, dest_mix, dest_splitter; float volume, vol[24][24]; } AMix;
static AMix g_amix[AMIX_MAX];
static struct { int ndest, dest[32]; } g_aspl[ASPL_MAX];
static struct { int in_use, mix; float vol[24]; } g_adst[ADST_MAX];
static float g_abuf[AMIX_MAX][24][240];
static unsigned long long g_routed, g_direct;            /* voice updates mixed through the graph / by the direct path (TOMO_AUDIOLOG) */
static int amix_ok(int m) { return m >= 0 && m < AMIX_MAX && g_amix[m].in_use && g_amix[m].buffers > 0; }
static int adst_of(int splitter, int i) {               /* the i-th destination of a splitter, -1 past the last */
    if (splitter < 0 || splitter >= ASPL_MAX || i < 0 || i >= g_aspl[splitter].ndest) return -1;
    int d = g_aspl[splitter].dest[i];
    return d >= 0 && d < ADST_MAX ? d : -1;
}
static void amix_parse(uint64_t in, uint64_t isp, uint64_t imx) {
    uint32_t ssz = rd32(in + 0x24), msz = rd32(in + 0x18);
    if (ssz >= 0x20 && rd32(isp) == 0x48444e53u) {      /* 'SNDH' {info count, destination count} */
        uint32_t ninfo = rd32(isp + 4), ndst = rd32(isp + 8), off = 0x20;
        {   static int log = -1, n; if (log < 0) log = getenv("TOMO_AUDIOLOG") != NULL;
            if (log && (ninfo || ndst) && n++ < 40) {
                printf("audio: splitter update: %u infos, %u destinations, %u bytes; first entry:", ninfo, ndst, ssz);
                for (int k = 0; k < 12 && 0x20 + k * 4 < (int)ssz; k++) printf(" %08x", rd32(isp + 0x20 + (uint64_t)k * 4));
                printf("\n");
            }
        }
        for (uint32_t i = 0; i < ninfo && off + 0x10 <= ssz; i++) {
            uint64_t h = isp + off;
            if (rd32(h) != 0x49444e53u) break;          /* 'SNDI' {id, sample rate, destination count, destination ids} */
            int id = (int)rd32(h + 4); uint32_t dc = rd32(h + 0xc);
            if (id >= 0 && id < ASPL_MAX) {
                g_aspl[id].ndest = dc < 32 ? (int)dc : 32;
                for (int k = 0; k < g_aspl[id].ndest; k++) g_aspl[id].dest[k] = (int)rd32(h + 0x10 + (uint64_t)k * 4);
            }
            off += (0x10 + dc * 4 + 15) & ~15u;           /* an info is padded to 16 bytes (seen: one destination = 0x20) */
        }
        uint32_t stride = ndst ? (ssz - off) / ndst : 0;
        stride = stride >= 0xa8 ? 0xa8 : stride >= 0x90 ? 0x90 : 0x70;   /* the section may end in padding: the largest format that fits */
        uint32_t use_at = stride == 0x70 ? 0x6c : stride == 0x90 ? 0x84 : 0x9c;
        for (uint32_t i = 0; i < ndst && off + stride <= ssz; i++, off += stride) {
            uint64_t d = isp + off;
            if (rd32(d) != 0x44444e53u) continue;       /* 'SNDD' {id, 24 volumes, mix id, ...} */
            int id = (int)rd32(d + 4);
            if (id < 0 || id >= ADST_MAX) continue;
            for (int k = 0; k < 24; k++) g_adst[id].vol[k] = rdf(d + 8 + (uint64_t)k * 4);
            g_adst[id].mix = (int)rd32(d + 0x68); g_adst[id].in_use = rd8(d + use_at);
        }
    }
    uint32_t count = msz / 0x930, dirty = msz % 0x930 == 0x20;
    uint64_t p = imx;
    if (dirty) { count = rd32(imx + 4); p += 0x20; }
    for (uint32_t i = 0; i < count; i++, p += 0x930) {
        int id = dirty ? (int)rd32(p + 0x10) : (int)i;
        if (id < 0 || id >= AMIX_MAX) continue;
        AMix *m = &g_amix[id];
        m->volume = rdf(p); m->buffers = (int)rd32(p + 8); m->in_use = rd8(p + 0xc);
        if (m->buffers > 24) m->buffers = 24;
        m->dest_mix = (int)rd32(p + 0x924); m->dest_splitter = (int)rd32(p + 0x928);
        memcpy(m->vol, (void *)(uintptr_t)(p + 0x24), sizeof m->vol);
    }
}

/* ---- effects on mix buffers (after the reference implementation, yuzu/eden audio_core command/effect/*.cpp, ported to
 * float): delay (3), reverb (4; also used for I3DL2 reverb, 5, with its own parameters mapped onto it) and biquad filter
 * (6) are applied; aux (2), limiter (7), capture (8) and compressor (9) pass the mix through. An effect runs on the
 * buffers of its mix, in the game's process order, before the mix goes on to its destination. Parameters are Q14 fixed
 * point (value / 16384) unless noted. */
#define AFX_MAX 32
typedef struct { float *buf; int max, count, end, in, out; float decay; } RLine;
static void rl_init(RLine *l, int n, float decay) {
    if (n < 1) n = 1;
    free(l->buf); l->buf = calloc((size_t)n + 1, sizeof(float));
    l->max = l->count = l->end = n; l->in = l->out = 0; l->decay = decay;
}
static float rl_read(const RLine *l) { return l->buf[l->out]; }
static void rl_write(RLine *l, float x) { l->buf[l->in] = x; if (++l->in >= l->end) l->in = 0; }
static float rl_tick(RLine *l, float x) { float o = l->buf[l->out]; if (++l->out >= l->end) l->out = 0; rl_write(l, x); return o; }
static float rl_tap(const RLine *l, int idx) { int p = l->in - (idx + 1); if (p < 0) p += l->count; if (p < 0) p = 0; return l->buf[p]; }
typedef struct {
    int type, ready;
    float *dl[6]; int dlen[6], dpos[6]; float fb, fbd, fbx, lpg, lpf, lpz[6];            /* delay */
    RLine pre, center, dec[4], fdn[4]; int early_t[10], pre_t; float early_g[10], hf_g[4], hf_pg[4], prev[4], cosv;   /* reverb */
    double bq[6][4];                                                                     /* biquad state */
} AFx;
static AFx g_afx[AFX_MAX];
static uint64_t g_efx_in; static uint32_t g_efx_n;       /* this update's effect parameters (input memory) */
static float q14(uint64_t a) { return (float)(int32_t)rd32(a) / 16384.0f; }
static void fx_delay(AFx *x, uint64_t pp, float *const *io, int nch) {
    float sr = q14(pp + 0x18); if (sr > 1000.0f) sr /= 1000.0f;   /* kHz */
    if (!x->ready) {
        for (int c = 0; c < nch; c++) {
            int n = (int)((float)rd32(pp + 0x14) * sr); if (n < 1) n = 1; if (n > 48000 * 2) n = 48000 * 2;
            free(x->dl[c]); x->dl[c] = calloc((size_t)n, sizeof(float)); x->dlen[c] = n; x->dpos[c] = 0; x->lpz[c] = 0;
        }
        x->ready = 1;
    }
    float spread = q14(pp + 0x2c), in_gain = q14(pp + 0x1c), wet = q14(pp + 0x24), dry = q14(pp + 0x28);
    x->fb = q14(pp + 0x20) * 0.97998046875f; x->fbd = x->fb * (1.0f - spread);
    if (nch == 4 || nch == 6) spread *= 0.5f;
    x->fbx = spread * x->fb; x->lpf = q14(pp + 0x30) * 0.949951171875f; x->lpg = 1.0f - x->lpf;
    for (int k = 0; k < 240; k++) {
        float in[6], d[6], g[6];
        for (int c = 0; c < nch; c++) { in[c] = io[c][k]; d[c] = x->dl[c][x->dpos[c]]; }
        for (int c = 0; c < nch; c++) {
            float acc = 0;
            for (int j = 0; j < nch; j++) {
                float m;
                if (nch == 1) m = x->fb;
                else if (nch == 2) m = j == c ? x->fbd : x->fbx;
                else if (nch == 4) { static const int M4[4][4] = {{1, 2, 2, 0}, {2, 1, 0, 2}, {2, 0, 1, 2}, {0, 2, 2, 1}}; int t = M4[j][c]; m = t == 1 ? x->fbd : t == 2 ? x->fbx : 0; }
                else { static const int M6[6][6] = {{1, 0, 2, 0, 2, 0}, {0, 1, 2, 0, 0, 2}, {2, 2, 1, 0, 0, 0}, {0, 0, 0, 3, 0, 0}, {2, 0, 0, 0, 1, 2}, {0, 2, 0, 0, 2, 1}}; int t = M6[j][c]; m = t == 1 ? x->fbd : t == 2 ? x->fbx : t == 3 ? q14(pp + 0x20) : 0; }
                acc += d[j] * m;
            }
            g[c] = in[c] * in_gain + acc;
        }
        for (int c = 0; c < nch; c++) {
            x->lpz[c] = g[c] * x->lpg + x->lpz[c] * x->lpf;
            x->dl[c][x->dpos[c]] = x->lpz[c]; if (++x->dpos[c] >= x->dlen[c]) x->dpos[c] = 0;
            io[c][k] = in[c] * dry + d[c] * wet;
        }
    }
}
static const float RV_FDN_MAX[4] = {53.9532470703125f, 79.19256591796875f, 116.23876953125f, 170.61529541015625f};
static const float RV_DEC_MAX[4] = {7.0f, 9.0f, 13.0f, 17.0f};
static const float RV_EARLY_T[5][11] = {{0.0f, 3.5f, 2.799988f, 3.899963f, 2.699951f, 13.399963f, 7.899963f, 8.399963f, 9.899963f, 12.0f, 12.5f},
    {0.0f, 11.799988f, 5.5f, 11.199951f, 10.399963f, 38.099976f, 22.199951f, 29.599976f, 21.199951f, 24.799988f, 40.0f},
    {0.0f, 41.5f, 20.5f, 41.299988f, 0.0f, 29.5f, 33.799988f, 45.199951f, 46.799988f, 0.0f, 50.0f},
    {33.099976f, 43.299988f, 22.799988f, 37.899963f, 14.899963f, 35.299988f, 17.899963f, 34.199951f, 0.0f, 43.299988f, 50.0f}, {0}};
static const float RV_EARLY_G[5][10] = {{0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.679993f, 0.679993f},
    {0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.679993f, 0.679993f, 0.679993f, 0.679993f},
    {0.5f, 0.699951f, 0.699951f, 0.679993f, 0.5f, 0.679993f, 0.679993f, 0.699951f, 0.679993f, 0.0f},
    {0.929993f, 0.919983f, 0.869995f, 0.859985f, 0.939941f, 0.809998f, 0.799988f, 0.769958f, 0.759949f, 0.649963f}, {0}};
static const float RV_FDN_T[5][4] = {{53.953247f, 79.192566f, 116.238770f, 130.615295f}, {53.953247f, 79.192566f, 116.238770f, 170.615295f},
    {5.0f, 10.0f, 5.0f, 10.0f}, {47.029968f, 71.0f, 103.0f, 170.0f}, {53.953247f, 79.192566f, 116.238770f, 170.615295f}};
static const float RV_DEC_T[5][4] = {{7.0f, 9.0f, 13.0f, 17.0f}, {7.0f, 9.0f, 13.0f, 17.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {7.0f, 7.0f, 13.0f, 9.0f}, {7.0f, 9.0f, 13.0f, 17.0f}};
typedef struct { int nch, early_mode, late_mode; float sr, early_gain, pre_delay, late_gain, decay_time, hf_ratio, colouration, base_gain, wet, dry; } RvP;
static float pow10c(float v) { return v >= 0.0f ? 1.0f : v <= -5.3f ? 0.0f : powf(10.0f, v); }
static void rv_update(AFx *x, const RvP *p) {
    float sr = p->sr < 1.0f ? 1.0f : p->sr, pre = p->pre_delay < 0 ? 0 : p->pre_delay;
    int em = p->early_mode >= 0 && p->early_mode < 5 ? p->early_mode : 0, lm = p->late_mode >= 0 && p->late_mode < 5 ? p->late_mode : 0;
    for (int i = 0; i < 10; i++) {
        int t = (int)((pre + RV_EARLY_T[em][i]) * sr); if (t < 0) t = 0; if (t > x->pre.max) t = x->pre.max;
        int old = x->early_t[i] - 1; x->early_t[i] = old + (t - old) / 4 + 1;
        float g = p->early_gain * RV_EARLY_G[em][i]; if (g < 0) g = 0; if (g > 1) g = 1;
        x->early_g[i] += (g - x->early_g[i]) / 4;
    }
    if (p->nch == 2) { x->early_g[4] *= 0.5f; x->early_g[5] *= 0.5f; }
    int tp = (int)((pre + RV_EARLY_T[em][10]) * sr); if (tp < 0) tp = 0; if (tp > x->pre.max) tp = x->pre.max;
    x->pre_t += (tp - x->pre_t) / 4;
    if (!x->cosv) x->cosv = cosf(1280.0f / sr * 3.14159265f / 180.0f);
    for (int i = 0; i < 4; i++) {
        int t = (int)(RV_FDN_T[lm][i] * sr); if (t < 0) t = 0; if (t > x->fdn[i].max) t = x->fdn[i].max;
        x->fdn[i].count += (t - x->fdn[i].count) / 4; if (x->fdn[i].count < 1) x->fdn[i].count = 1; x->fdn[i].end = x->fdn[i].count - 1 > 0 ? x->fdn[i].count - 1 : 1;
        if (x->fdn[i].in >= x->fdn[i].end) x->fdn[i].in = 0; if (x->fdn[i].out >= x->fdn[i].end) x->fdn[i].out = 0;
        int d = (int)(RV_DEC_T[lm][i] * sr); if (d < 0) d = 0; if (d > x->dec[i].max) d = x->dec[i].max;
        x->dec[i].count += (d - x->dec[i].count) / 4; if (x->dec[i].count < 1) x->dec[i].count = 1; x->dec[i].end = x->dec[i].count - 1 > 0 ? x->dec[i].count - 1 : 1;
        if (x->dec[i].in >= x->dec[i].end) x->dec[i].in = 0; if (x->dec[i].out >= x->dec[i].end) x->dec[i].out = 0;
        float col = p->colouration < 0 ? 0 : p->colouration > 1 ? 1 : p->colouration;
        x->dec[i].decay += (0.5999755859375f * (1.0f - col) - x->dec[i].decay) / 4;
        float dt = p->decay_time <= 0 ? 0.0001f : p->decay_time;
        float a = (float)(x->fdn[i].max + x->dec[i].max) * -3.0f, b = a / (dt * sr);
        float hf = p->hf_ratio < 0.001f ? 0.001f : p->hf_ratio > 0.999f ? 0.999f : p->hf_ratio, c, dd;
        if (hf > 0.9949f) { c = 0; dd = 1; }
        else {
            float e = pow10c(((1.0f / hf) - 1.0f) * 2 / 100 * (b / 10)); if (e < 0.0001f) e = 0.0001f; if (e > 1) e = 1;
            float f = 1.0f - e; if (f < 0.0001f) f = 0.0001f;
            float g = 2.0f - x->cosv * e * 2, h2 = g * g - f * f * 4; if (h2 < 0) h2 = 0;
            c = (g - sqrtf(h2)) / (f * 2.0f); dd = 1.0f - c;
        }
        x->hf_pg[i] += (c - x->hf_pg[i]) / 4;
        x->hf_g[i] += (pow10c(b / 1000) * dd * 0.70709228515625f - x->hf_g[i]) / 4;
        x->prev[i] = 0;
    }
}
static void fx_reverb(AFx *x, const RvP *p, float *const *io, int nch) {
    if (!x->ready) {
        float sr = p->sr < 1.0f ? 1.0f : p->sr;
        memset(x->early_t, 0, sizeof x->early_t); memset(x->early_g, 0, sizeof x->early_g); x->pre_t = 0;
        memset(x->hf_g, 0, sizeof x->hf_g); memset(x->hf_pg, 0, sizeof x->hf_pg); x->cosv = 0;
        for (int i = 0; i < 4; i++) { rl_init(&x->fdn[i], (int)(RV_FDN_MAX[i] * sr), 1.0f); rl_init(&x->dec[i], (int)(RV_DEC_MAX[i] * sr), 0.0f); }
        rl_init(&x->pre, (int)(350.0f * sr), 1.0f); rl_init(&x->center, (int)(5 * sr), 1.0f);
        rv_update(x, p);
        x->ready = 1;
    } else rv_update(x, p);
    static const int T1[10] = {0}, T2[10] = {0, 0, 1, 1, 0, 1, 0, 0, 1, 1}, T4[10] = {0, 0, 1, 1, 0, 1, 2, 2, 3, 3}, T6[10] = {0, 0, 1, 1, 2, 2, 4, 4, 5, 5};
    const int *tap = nch == 1 ? T1 : nch == 2 ? T2 : nch == 4 ? T4 : T6;
    for (int k = 0; k < 240; k++) {
        float o[6] = {0}, in = 0;
        for (int t = 0; t < 10; t++) { float v = rl_tap(&x->pre, x->early_t[t]) * x->early_g[t]; o[tap[t]] += v; if (nch == 6) o[3] += v; }
        if (nch == 6) o[3] *= 0.2f;
        for (int c = 0; c < nch; c++) in += io[c][k];
        rl_write(&x->pre, in * p->base_gain);
        for (int i = 0; i < 4; i++) x->prev[i] = x->prev[i] * x->hf_pg[i] + rl_read(&x->fdn[i]) * x->hf_g[i];
        float ps = rl_tap(&x->pre, x->pre_t) * p->late_gain;
        float mm[4] = {x->prev[2] + x->prev[1] + ps, -x->prev[0] - x->prev[3] + ps, x->prev[0] - x->prev[3] + ps, x->prev[1] - x->prev[2] + ps}, ap[4];
        for (int i = 0; i < 4; i++) {
            float v = rl_read(&x->dec[i]), mixed = mm[i] - v * x->dec[i].decay;
            ap[i] = rl_tick(&x->dec[i], mixed) + mixed * x->dec[i].decay;
            rl_tick(&x->fdn[i], ap[i]);
        }
        if (nch == 6) {
            float apo[6] = {ap[0], ap[1], ap[2] - ap[3], ap[3], ap[2], ap[3]};
            for (int c = 0; c < 6; c++) { float a = c == 2 ? rl_tick(&x->center, apo[c] * 0.5f) : apo[c]; io[c][k] = io[c][k] * p->dry + (o[c] + a) * p->wet; }
        } else for (int c = 0; c < nch; c++) io[c][k] = io[c][k] * p->dry + (o[c] + ap[c < 4 ? c : 0]) * p->wet;
    }
}
static void fx_biquad(AFx *x, uint64_t pp, float *const *io, int nch) {
    double b[3], a[2];
    if (rd32(pp + 0xc) == 0 && rdf(pp + 0x10) != 0.0f) { for (int i = 0; i < 3; i++) b[i] = rdf(pp + 0x10 + (uint64_t)i * 4); a[0] = rdf(pp + 0x1c); a[1] = rdf(pp + 0x20); }
    else { for (int i = 0; i < 3; i++) b[i] = (int16_t)rd16(pp + 0xc + (uint64_t)i * 2) / 16384.0; a[0] = (int16_t)rd16(pp + 0x12) / 16384.0; a[1] = (int16_t)rd16(pp + 0x14) / 16384.0; }
    for (int c = 0; c < nch; c++) {
        double *st = x->bq[c];
        for (int k = 0; k < 240; k++) {
            double in = io[c][k], y = in * b[0] + st[0] * b[1] + st[1] * b[2] + st[2] * a[0] + st[3] * a[1];
            if (!(y > -1e7 && y < 1e7)) { y = 0; memset(st, 0, sizeof x->bq[c]); }
            st[1] = st[0]; st[0] = in; st[3] = st[2]; st[2] = y;
            io[c][k] = (float)y;
        }
    }
}
/* Aux (2): the game's own CPU effect on a mix. Each channel's 240 samples (32-bit, the DSP's mix scale) go into the send
 * ring and the mix plays what the game wrote into the return ring (ReadAuxSendBuffer / WriteAuxReturnBuffer, the SDK's
 * own lifted code, on the other side). Rings: info block at the parameter's address, the DSP's {read, write offset} at
 * +0x40, the samples at +0x80; channel i at offset i x 240, both offsets advance after the last channel (reference: yuzu
 * audio_core command/effect/aux_.cpp, CommandGenerator::GenerateAuxCommand). Passed through before: the game's
 * NintendoWare busses on eight mixes reached the speakers unprocessed. Disabled: both offsets reset, input to output. */
static void fx_aux(uint64_t ep, uint64_t pp, int mix) {
    static int off = -1;
    if (off < 0) off = getenv("TOMO_AUDIO_AUX") && getenv("TOMO_AUDIO_AUX")[0] == '0';      /* TEST ONLY: pass through, as before */
    int n = (int)rd32(pp + 0x30);
    uint32_t cmax = rd32(pp + 0x38);
    uint64_t si = rd64(pp + 0x40), ri = rd64(pp + 0x50);
    if (off || n < 1 || n > 24 || !si || !ri || cmax < 240) return;
    uint64_t sinfo = si + 0x40, sbuf = si + 0x80, rinfo = ri + 0x40, rbuf = ri + 0x80;
    if (!rd8(ep + 2)) {
        for (uint64_t inf = sinfo, k = 0; k < 2; k++, inf = rinfo) { wr32(inf, 0); wr32(inf + 4, 0); wr32(inf + 0xc, 0); }
        for (int c = 0; c < n; c++) {
            int bi = (int8_t)rd8(pp + (uint64_t)c), bo = (int8_t)rd8(pp + 0x18 + (uint64_t)c);
            if (bi >= 0 && bi < g_amix[mix].buffers && bo >= 0 && bo < g_amix[mix].buffers && bi != bo) memcpy(g_abuf[mix][bo], g_abuf[mix][bi], sizeof g_abuf[mix][bo]);
        }
        return;
    }
    static int log = -1; static double ein[32], eout[32]; static unsigned long long calls[32];
    if (log < 0) log = getenv("TOMO_AUDIOLOG") != NULL;
    unsigned slot = (unsigned)((ep >> 6) & 31);
    uint32_t wo = 0;
    for (int c = 0; c < n; c++, wo += 240) {
        int bi = (int8_t)rd8(pp + (uint64_t)c), bo = (int8_t)rd8(pp + 0x18 + (uint64_t)c);
        uint32_t upd = c == n - 1 ? wo + 240 : 0;
        if (bi < 0 || bi >= g_amix[mix].buffers || bo < 0 || bo >= g_amix[mix].buffers) continue;
        if (log) for (int k = 0; k < 240; k++) ein[slot] += (double)g_abuf[mix][bi][k] * g_abuf[mix][bi][k];
        uint32_t t = rd32(sinfo + 4) + wo;
        if (t <= cmax)
            for (int k = 0; k < 240; k++, t++) {
                if (t >= cmax) t = 0;
                float s = g_abuf[mix][bi][k];
                wr32(sbuf + (uint64_t)t * 4, (uint32_t)(int32_t)(s > 2.1e9f ? 2.1e9f : s < -2.1e9f ? -2.1e9f : s));
            }
        if (upd) wr32(sinfo + 4, (rd32(sinfo + 4) + upd) % cmax);
        uint32_t r = rd32(rinfo) + wo;
        if (r <= cmax)
            for (int k = 0; k < 240; k++, r++) { if (r >= cmax) r = 0; g_abuf[mix][bo][k] = (float)(int32_t)rd32(rbuf + (uint64_t)r * 4); }
        else memset(g_abuf[mix][bo], 0, sizeof g_abuf[mix][bo]);
        if (upd) wr32(rinfo, (rd32(rinfo) + upd) % cmax);
        if (log) for (int k = 0; k < 240; k++) eout[slot] += (double)g_abuf[mix][bo][k] * g_abuf[mix][bo][k];
    }
    if (log && ++calls[slot] % 2000 == 0) {      /* TOMO_AUDIOLOG: send / return loudness (RMS, 16-bit scale) and the four ring offsets */
        double nsmp = 2000.0 * 240 * n;
        printf("audio: aux on mix %d (%d ch, ring %u): send rms %.1f return rms %.1f | send: cpu read %u dsp write %u | return: cpu write %u dsp read %u\n", mix, n, cmax,
               sqrt(ein[slot] / nsmp), sqrt(eout[slot] / nsmp), rd32(si), rd32(sinfo + 4), rd32(ri + 4), rd32(rinfo));
        ein[slot] = eout[slot] = 0;
    }
}
static void afx_run_mix(int mix) {                       /* the effects of one mix, in process order, on its buffers */
    static int off = -1;
    if (off < 0) off = getenv("TOMO_AUDIO_FX") && getenv("TOMO_AUDIO_FX")[0] == '0';
    if (off || !g_efx_in) return;
    for (uint32_t order = 0; order < 8; order++)
        for (uint32_t e = 0; e < g_efx_n && e < AFX_MAX; e++) {
            uint64_t ep = g_efx_in + e * 0xc0, pp = ep + 0x20;
            int type = rd8(ep);
            if (!type || (int)rd32(ep + 4) != mix || rd32(ep + 0x18) != order) continue;
            AFx *x = &g_afx[e];
            if (x->type != type || rd8(ep + 1)) { x->type = type; x->ready = 0; memset(x->bq, 0, sizeof x->bq); }
            if (type == 2) { fx_aux(ep, pp, mix); continue; }
            if (!rd8(ep + 2) || type >= 7) continue;   /* disabled, or passed through */
            int nch = type == 6 ? (int8_t)rd8(pp + (rd32(pp + 0xc) == 0 && rdf(pp + 0x10) != 0.0f ? 0x24 : 0x16)) : (int)rd16(pp + 0xe);
            if (nch != 1 && nch != 2 && nch != 4 && nch != 6) continue;
            float *io[6];
            int bad = 0;
            for (int c = 0; c < nch; c++) {                 /* in place: the reference reads inputs[c] and writes outputs[c] */
                int bi = (int8_t)rd8(pp + (uint64_t)c), bo = (int8_t)rd8(pp + 6 + (uint64_t)c);
                if (bi < 0 || bi >= g_amix[mix].buffers || bo < 0 || bo >= g_amix[mix].buffers) { bad = 1; break; }
                if (bi != bo) memcpy(g_abuf[mix][bo], g_abuf[mix][bi], sizeof g_abuf[mix][bo]);
                io[c] = g_abuf[mix][bo];
            }
            if (bad) continue;
            {   static int log = -1; static uint32_t said;      /* TOMO_AUDIOLOG: each effect's parameters once, as read */
                if (log < 0) log = getenv("TOMO_AUDIOLOG") != NULL;
                if (log && e < 32 && !(said >> e & 1)) {
                    said |= 1u << e;
                    printf("audio: effect %u type %d on mix %d, %d channels:", e, type, mix, nch);
                    for (int w = 0x10; w < 0x48; w += 4) printf(" %08x", rd32(pp + (uint64_t)w));
                    printf("\n");
                }
            }
            if (type == 3) fx_delay(x, pp, io, nch);
            else if (type == 6) fx_biquad(x, pp, io, nch);
            else if (type == 4) {
                RvP p = {nch, (int)rd32(pp + 0x14), (int)rd32(pp + 0x20), q14(pp + 0x10), q14(pp + 0x18), q14(pp + 0x1c), q14(pp + 0x24), q14(pp + 0x28),
                         q14(pp + 0x2c), q14(pp + 0x30), q14(pp + 0x34), q14(pp + 0x38), q14(pp + 0x3c)};
                if (p.sr > 1000.0f) p.sr /= 1000.0f;
                fx_reverb(x, &p, io, nch);
            } else if (type == 5) {                         /* I3DL2 (floats, millibels and seconds) mapped onto the reverb above */
                float sr = (float)rd32(pp + 0x14) / 1000.0f, decay = rdf(pp + 0x20), hfr = rdf(pp + 0x24), room = rdf(pp + 0x28), refl = rdf(pp + 0x2c),
                      rev = rdf(pp + 0x30), rdel = rdf(pp + 0x38), dry = rdf(pp + 0x44);
                if (sr < 1.0f) sr = 48.0f;
                if (!(dry >= 0.0f && dry <= 2.0f)) dry = 1.0f;
                RvP p = {nch, 1, 1, sr, powf(10.0f, refl / 2000.0f), rdel * 1000.0f, powf(10.0f, rev / 2000.0f), decay, hfr, 0.0f,
                         powf(10.0f, room / 2000.0f), 1.0f, dry};
                if (!(p.decay_time > 0.0f && p.decay_time < 30.0f)) continue;
                fx_reverb(x, &p, io, nch);
            }
        }
}
static int amix_depth(int m) {                           /* hops to the final mix (cycles cut at 16) */
    int d = 0;
    for (int guard = 0; m != 0 && guard < 16; guard++, d++) {
        if (!amix_ok(m)) return d;
        if (amix_ok(g_amix[m].dest_mix)) { m = g_amix[m].dest_mix; continue; }
        int k = adst_of(g_amix[m].dest_splitter, 0);
        if (k < 0) return d;
        m = g_adst[k].mix;
    }
    return d;
}
static void amix_run(void) {                            /* sub-mixes into their destinations, then the final mix to g_mix */
    int order[AMIX_MAX], n = 0;
    for (int m = 1; m < AMIX_MAX; m++) if (amix_ok(m)) order[n++] = m;
    int depth[AMIX_MAX];
    for (int i = 0; i < n; i++) depth[order[i]] = amix_depth(order[i]);
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && depth[order[j]] > depth[order[j - 1]]; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    for (int i = 0; i < n; i++) {
        AMix *m = &g_amix[order[i]];
        afx_run_mix(order[i]);
        if (amix_ok(m->dest_mix)) {
            AMix *dm = &g_amix[m->dest_mix];
            for (int a = 0; a < m->buffers; a++)
                for (int b = 0; b < dm->buffers; b++) {
                    float g = m->volume * m->vol[a][b];
                    if (g != 0.0f) for (int k = 0; k < 240; k++) g_abuf[m->dest_mix][b][k] += g_abuf[order[i]][a][k] * g;
                }
        } else for (int di = 0; ; di++) {
            int d = adst_of(m->dest_splitter, di);
            if (d < 0) break;
            if (!g_adst[d].in_use || !amix_ok(g_adst[d].mix)) continue;
            AMix *dm = &g_amix[g_adst[d].mix];
            int a = di % m->buffers;
            for (int b = 0; b < dm->buffers; b++) {
                float g = m->volume * g_adst[d].vol[b];
                if (g != 0.0f) for (int k = 0; k < 240; k++) g_abuf[g_adst[d].mix][b][k] += g_abuf[order[i]][a][k] * g;
            }
        }
    }
    AMix *f = &g_amix[0];
    afx_run_mix(0);
    for (int k = 0; k < 240; k++) {
        float l, r;
        if (f->buffers >= 6) { l = g_abuf[0][0][k] + 0.707f * (g_abuf[0][2][k] + g_abuf[0][4][k]); r = g_abuf[0][1][k] + 0.707f * (g_abuf[0][2][k] + g_abuf[0][5][k]); }
        else if (f->buffers >= 2) { l = g_abuf[0][0][k]; r = g_abuf[0][1][k]; }
        else l = r = g_abuf[0][0][k];
        g_mix[k][0] += l * f->volume; g_mix[k][1] += r * f->volume;
    }
}
static uint32_t audren_core(uint64_t in, uint64_t out, uint64_t cap, uint64_t frames) {
    static int said;
    if (!in || !out) return 0;
    uint32_t rev = (rd32(in) >> 24) - '0';
    uint32_t vsz = rev >= 15 ? 0x188 : 0x170, esz_out = rev >= 9 ? 0x90 : 0x10;
    uint32_t npool = rd32(in + 8) / 0x20, nvoice = rd32(in + 0xc) / vsz, neffect = rd32(in + 0x14) / 0xc0, nsink = rd32(in + 0x1c) / 0x140;
    uint64_t ip = in + 0x40 + rd32(in + 4);              /* memory pools follow the behaviour parameter */
    uint64_t iv = ip + rd32(in + 8) + rd32(in + 0x10);   /* voices follow the voice channel resources */
    uint64_t ires = ip + rd32(in + 8);                   /* voice channel resources: {id, 24 mix volumes, in use} 0x70 each */
    uint32_t nres = rd32(in + 0x10) / 0x70;
    int mixing = audio_out_open();
    if (g_bq_off < 0) g_bq_off = getenv("TOMO_AUDIO_BQ") && getenv("TOMO_AUDIO_BQ")[0] == '0';
    memset(g_mix, 0, sizeof g_mix);
    uint64_t ie = iv + rd32(in + 0xc);
    amix_parse(in, ie + rd32(in + 0x14), ie + rd32(in + 0x14) + rd32(in + 0x24));
    int graph = amix_ok(0);
    if (graph) for (int m = 0; m < AMIX_MAX; m++) if (amix_ok(m)) memset(g_abuf[m], 0, sizeof g_abuf[m][0] * (size_t)g_amix[m].buffers);
    uint64_t total = 0x40 + npool * 0x10 + nvoice * 0x10 + neffect * esz_out + nsink * 0x20 + 0x10 + 0xb0 + 0x10;
    if (total > cap) { if (said++ < 3) printf("audio: update output needs %llu bytes, the buffer holds %llu\n", (unsigned long long)total, (unsigned long long)cap); return 0x5299; }
    if (total != cap && said++ < 3) printf("audio: update output %llu bytes, buffer %llu (revision %u, %u pools %u voices %u effects %u sinks)\n", (unsigned long long)total, (unsigned long long)cap, rev, npool, nvoice, neffect, nsink);
    memset((void *)(uintptr_t)out, 0, (size_t)cap);
    uint64_t op = out + 0x40;
    for (uint32_t i = 0; i < npool; i++, op += 0x10) {   /* RequestAttach -> Attached, RequestDetach -> Detached */
        uint32_t s = rd32(ip + i * 0x20 + 0x10);
        wr32(op, s == 4 ? 5 : s == 2 ? 3 : s);
    }
    wr32(out + 8, npool * 0x10);
    for (uint32_t i = 0; i < nvoice; i++, op += 0x10) {
        uint64_t v = iv + i * vsz;
        uint32_t id = rd32(v), in_use = rd8(v + 9), play = rd8(v + 0xa), rate = rd32(v + 0xc), is_new = rd8(v + 8);
        uint32_t count = rev >= 15 ? rd32(v + 0x54) : rd32(v + 0x3c), index = rev >= 15 ? rd32(v + 0x58) : rd16(v + 0x40);
        uint64_t wb = v + (rev >= 15 ? 0x78 : 0x60);
        SilentVoice *s = &g_sv[id < 1024 ? id : 0];
        if (is_new || !s->seen) { memset(s, 0, sizeof *s); s->seen = 1; s->last_index = index; s->af = -1; }
        if (in_use && play == 0 && !is_new) {
            s->popped += (index - s->last_index) & 3;    /* the client released that many buffers since the last update */
            s->last_index = index;
            uint32_t pending = s->consumed - s->popped, avail = count > pending ? count - pending : 0;
            uint32_t fmt = rd8(v + 0xb), cc = rd32(v + 0x18);
            float step = (float)(rate ? rate : 48000) / 48000.0f * rdf(v + 0x1c), vol = rdf(v + 0x20);
            if (!(step > 0.0f && step < 16.0f)) step = (float)(rate ? rate : 48000) / 48000.0f;
            if (cc < 1 || cc > 6) cc = 1;
            uint64_t coef = rd64(v + (rev >= 15 ? 0x60 : 0x48));
            /* The voice's two biquad filters at +0x24 (the DSP runs them on the resampled samples): revision 15 has float
             * coefficients, 0x18 bytes each {enable, 3 pad, b0 b1 b2, a1 a2}, older ones Q14 shorts, 0xc bytes each. The a
             * terms are added, as in the reference (yuzu audio_core command/effect/biquad_filter.cpp). NintendoWare's 3D
             * sound muffles far sounds and the music inside buildings with them; without them both came through clear. */
            double bqb[2][3], bqa[2][2]; int bqn = 0, bqi[2];
            for (int j = 0; j < 2; j++) {
                uint64_t f = v + 0x24 + (uint64_t)j * (rev >= 15 ? 0x18 : 0xc);
                int on = rd8(f) != 0;
                if (on && !s->bq_on[j]) memset(s->bq[j], 0, sizeof s->bq[j]);      /* newly enabled: from silence */
                s->bq_on[j] = on;
                if (!on || g_bq_off) continue;
                for (int k = 0; k < 3; k++) bqb[j][k] = rev >= 15 ? rdf(f + 4 + (uint64_t)k * 4) : (int16_t)rd16(f + 2 + (uint64_t)k * 2) / 16384.0;
                for (int k = 0; k < 2; k++) bqa[j][k] = rev >= 15 ? rdf(f + 0x10 + (uint64_t)k * 4) : (int16_t)rd16(f + 8 + (uint64_t)k * 2) / 16384.0;
                bqi[bqn++] = j;
            }
            if (bqn && vol > 0.0f) g_bq_voices++;
            float *tb[6][48]; float tg[6][48]; int tn[6] = {0};   /* with the mix graph: the buffers each channel adds into, and the gains */
            if (graph) {
                uint64_t ids = v + (rev >= 15 ? 0x158 : 0x140);
                int mid = (int)rd32(v + (rev >= 15 ? 0x70 : 0x58)), spl = (int)rd32(v + (rev >= 15 ? 0x74 : 0x5c));
                for (uint32_t ch = 0; ch < cc && ch < 6; ch++) {
                    if (amix_ok(mid)) {
                        uint32_t rid = rd32(ids + ch * 4);
                        for (uint32_t k = 0; k < nres; k++) {
                            uint64_t r = ires + k * 0x70;
                            if (rd32(r) != rid || !rd8(r + 0x64)) continue;
                            for (int b = 0; b < g_amix[mid].buffers && tn[ch] < 48; b++) {
                                float g = rdf(r + 4 + (uint64_t)b * 4) * vol;
                                if (g != 0.0f) { tb[ch][tn[ch]] = g_abuf[mid][b]; tg[ch][tn[ch]++] = g; }
                            }
                            break;
                        }
                    } else for (int di = (int)ch; ; di += (int)cc) {
                        int d = adst_of(spl, di);
                        if (d < 0) break;
                        if (!g_adst[d].in_use || !amix_ok(g_adst[d].mix)) continue;
                        for (int b = 0; b < g_amix[g_adst[d].mix].buffers && tn[ch] < 48; b++) {
                            float g = g_adst[d].vol[b] * vol;
                            if (g != 0.0f) { tb[ch][tn[ch]] = g_abuf[g_adst[d].mix][b]; tg[ch][tn[ch]++] = g; }
                        }
                    }
                }
            }
            float gl[6], gr[6];                          /* left / right gain of each channel: its channel resource's mix volumes */
            {   uint64_t ids = v + (rev >= 15 ? 0x158 : 0x140);
                uint32_t splitter = rd32(v + (rev >= 15 ? 0x74 : 0x5c));
                for (uint32_t ch = 0; ch < cc; ch++) {
                    gl[ch] = gr[ch] = cc == 1 ? 0.7f : (ch == 0 ? 1.0f : 0.0f);
                    if (cc == 2 && ch == 1) { gl[1] = 0.0f; gr[1] = 1.0f; }
                    uint32_t rid = rd32(ids + ch * 4);
                    for (uint32_t k = 0; splitter == 0xffffffffu && k < nres; k++) {
                        uint64_t r = ires + k * 0x70;
                        if (rd32(r) == rid && rd8(r + 0x64)) { gl[ch] = rdf(r + 4); gr[ch] = rdf(r + 8); break; }
                    }
                    gl[ch] *= vol; gr[ch] *= vol;
                }
            }
            int routed = graph && (tn[0] || (cc > 1 && tn[1]));   /* no destination known for this voice: the direct stereo path */
            {   static int vlog = -1; static unsigned long long vu;      /* TOMO_AUDIOLOG=2: every playing voice's total gain, every 400 updates (2 s); =3: also each voice's first audible update */
                if (vlog < 0) vlog = getenv("TOMO_AUDIOLOG") ? atoi(getenv("TOMO_AUDIOLOG")) : 0;
                if (vlog >= 2 && i == 0) vu++;
                int first = vlog >= 3 && vol > 0.0f && !s->heard; if (vol > 0.0f) s->heard = 1;
                if (vlog >= 2 && (vu % 400 == 0 || first) && vol > 0.0f) {
                    float sum = 0; for (uint32_t ch = 0; ch < cc && ch < 6; ch++) { if (routed) for (int t = 0; t < tn[ch]; t++) sum += tg[ch][t]; else sum += gl[ch] + gr[ch]; }
                    printf("audio-voice%s: update %llu voice %u fmt %u rate %u ch %u vol %.3f gain %.3f %s mix %d splitter %d filters %d data %llx\n", first ? " START" : "", vu, id, fmt, rate, cc, vol, sum,
                           routed ? "routed" : "direct", (int)rd32(v + (rev >= 15 ? 0x70 : 0x58)), (int)rd32(v + (rev >= 15 ? 0x74 : 0x5c)), bqn, (unsigned long long)rd64(wb));
                }
            }
            if (vol > 0.0f && play == 0) { if (routed) g_routed++; else g_direct++; }
            int mix = mixing && (fmt == 2 || (fmt == 6 && coef > 0x10000)) && (vol > 0.0f);
            if (mixing && fmt == 6 && coef <= 0x10000) {       /* an ADPCM voice without coefficients where they were looked for: the layout */
                static int said_v;
                if (said_v++ < 2) {
                    printf("audio: ADPCM voice %u without coefficients at +0x%x; the voice (revision %u):\n", id, rev >= 15 ? 0x60 : 0x48, rev);
                    for (uint32_t o = 0; o < vsz; o += 0x20) printf("  +%03x: %016llx %016llx %016llx %016llx\n", o, (unsigned long long)rd64(v + o), (unsigned long long)rd64(v + o + 8),
                                                                    (unsigned long long)rd64(v + o + 16), (unsigned long long)rd64(v + o + 24));
                }
            }
            uint32_t guard = 0;
            for (int o = 0; o < 240 && avail && guard++ < 4096; ) {
                uint64_t b = wb + ((index + pending) & 3) * 0x38;
                int32_t start = (int32_t)rd32(b + 0x10), end = (int32_t)rd32(b + 0x14);
                uint32_t len = end > start ? (uint32_t)(end - start) : 0;
                if (s->pos >= len) {                     /* this buffer is played */
                    s->pos = 0; s->frac = 0; s->af = -1;
                    if (rd8(b + 0x18) && len) continue;  /* a looping buffer plays again */
                    s->consumed++; pending++; avail--;
                    continue;
                }
                if (mix && rd64(b) > 0x10000) {          /* a buffer without data plays as silence */
                    uint64_t data = rd64(b);
                    uint32_t idx = (uint32_t)start + s->pos;
                    for (uint32_t ch = 0; ch < cc; ch++) {
                        int smp = 0, nxt = 0;                    /* the samples at idx and idx + 1: linear interpolation by frac */
                        if (fmt == 2) {
                            smp = (int16_t)rd16(data + ((uint64_t)idx * cc + ch) * 2);
                            nxt = (int32_t)(idx + 1) < end ? (int16_t)rd16(data + ((uint64_t)(idx + 1) * cc + ch) * 2) : smp;
                        } else if (ch == 0) {
                            int32_t f = (int32_t)(idx / 14);
                            if (s->af == -1) {                       /* a new buffer: its context, else the history carried on */
                                uint64_t ctx = rd64(b + 0x28);
                                if (ctx > 0x10000) { s->yn1 = (int16_t)rd16(ctx + 2); s->yn2 = (int16_t)rd16(ctx + 4); }      /* not a pointer: no context */
                                adpcm_frame(s, data, f, coef);
                            } else if (f < s->af) adpcm_frame(s, data, f, coef);
                            else while (s->af < f) adpcm_frame(s, data, s->af + 1, coef);
                            smp = s->abuf[idx % 14];
                            nxt = (idx + 1) % 14 ? s->abuf[(idx + 1) % 14] : (int32_t)(idx + 1) < end ? adpcm_peek(s, data, f + 1, coef) : smp;
                        }
                        float val = (float)smp + (float)(nxt - smp) * s->frac;
                        for (int q = 0; q < bqn && ch < 6; q++) {
                            int j = bqi[q]; double *st = s->bq[j][ch];
                            double y = val * bqb[j][0] + st[0] * bqb[j][1] + st[1] * bqb[j][2] + st[2] * bqa[j][0] + st[3] * bqa[j][1];
                            if (!(y > -1e7 && y < 1e7)) { y = 0; memset(st, 0, 4 * sizeof *st); }
                            st[1] = st[0]; st[0] = val; st[3] = st[2]; st[2] = y;
                            val = (float)y;
                        }
                        if (routed) { if (ch < 6) for (int t = 0; t < tn[ch]; t++) tb[ch][t][o] += val * tg[ch][t]; }
                        else { g_mix[o][0] += val * gl[ch]; g_mix[o][1] += val * gr[ch]; }
                    }
                }
                s->frac += step;
                uint32_t adv = (uint32_t)s->frac;
                s->frac -= (float)adv;
                s->pos += adv; s->played += adv;
                o++;
            }
        }
        {   static int log = -1; static unsigned long long upd;    /* TOMO_AUDIOLOG=1: the voices in use, every 1000 updates (5 s) */
            if (log < 0) log = getenv("TOMO_AUDIOLOG") != NULL;
            if (log && i == 0) upd++;
            if (log && upd % 1000 == 0 && in_use)
                printf("audio: voice %u play %u rate %u buffers %u index %u consumed %u popped %u played %llu new %u\n", id, play, rate, count, index, s->consumed, s->popped, (unsigned long long)s->played, is_new);
        }
        wr64(op, is_new ? 0 : s->played);
        wr32(op + 8, is_new ? 0 : s->consumed);
    }
    {   static int log = -1; static unsigned long long n;
        if (log < 0) log = getenv("TOMO_AUDIOLOG") != NULL;
        if (log && n++ % 1000 == 0) {
            unsigned used = 0, playing = 0;
            for (uint32_t i = 0; i < nvoice; i++) { uint64_t v = iv + i * vsz; used += rd8(v + 9) != 0; playing += rd8(v + 9) && rd8(v + 0xa) == 0; }
            printf("audio: update %llu: revision %u, %u voices (%u in use, %u playing), %u pools, %u effects, %u sinks; %llu voice updates filtered%s\n", n, rev, nvoice, used, playing, npool,
                   neffect, nsink, g_bq_voices, g_bq_off ? " (filters off: TOMO_AUDIO_BQ=0)" : "");
            {                                             /* every effect: type, mix, enabled, process order */
                printf("audio:   effects:");
                for (uint32_t e = 0; e < neffect && e < AFX_MAX; e++) {
                    uint64_t ep = ie + e * 0xc0;
                    if (rd8(ep)) printf(" %u:t%d/m%d/%s/o%u", e, rd8(ep), (int)rd32(ep + 4), rd8(ep + 2) ? "on" : "off", rd32(ep + 0x18));
                }
                printf("\n");
            }
            for (uint32_t i = 0; i < nvoice; i++) {      /* the first playing voice with a filter on: its coefficients */
                uint64_t v = iv + i * vsz, f = v + 0x24;
                if (!rd8(v + 9) || rd8(v + 0xa) || !rd8(f)) continue;
                if (rev >= 15) printf("audio:   voice %u filter 0: b %.4f %.4f %.4f a %.4f %.4f, volume %.3f\n", rd32(v), rdf(f + 4), rdf(f + 8), rdf(f + 12), rdf(f + 16), rdf(f + 20), rdf(v + 0x20));
                else printf("audio:   voice %u filter 0 (Q14): b %d %d %d a %d %d, volume %.3f\n", rd32(v), (int16_t)rd16(f + 2), (int16_t)rd16(f + 4), (int16_t)rd16(f + 6), (int16_t)rd16(f + 8), (int16_t)rd16(f + 10), rdf(v + 0x20));
                break;
            }
            {   /* the mix graph: splitters (header 'SNDH'), then the mixes (an 0x20 'dirty' header first when the count is not size / 0x930) */
                uint64_t isp = ie + rd32(in + 0x14), imx = isp + rd32(in + 0x24);
                uint32_t msz = rd32(in + 0x18), mcount = msz / 0x930;
                printf("audio:   splitter section %u bytes: magic %08x infos %d destinations %d; mix section %u bytes, first words %08x %08x\n", rd32(in + 0x24),
                       rd32(isp), (int)rd32(isp + 4), (int)rd32(isp + 8), msz, rd32(imx), rd32(imx + 4));
                if (msz % 0x930) { mcount = rd32(imx + 4); imx += 0x20; }
                for (uint32_t m = 0; m < mcount && m < 64; m++) {
                    uint64_t mp = imx + m * 0x930;
                    if (!rd8(mp + 0xc)) continue;
                    printf("audio:   mix %d: volume %.3f buffers %u dest mix %d dest splitter %d effects %u; volumes to dest buffer 0/1 from buffer 0: %.3f %.3f, from 1: %.3f %.3f\n",
                           (int)rd32(mp + 0x10), rdf(mp), rd32(mp + 8), (int)rd32(mp + 0x924), (int)rd32(mp + 0x928), rd32(mp + 0x14),
                           rdf(mp + 0x24), rdf(mp + 0x28), rdf(mp + 0x24 + 96), rdf(mp + 0x28 + 96));
                }
                unsigned to[8] = {0}; unsigned via_splitter = 0;
                for (uint32_t i = 0; i < nvoice; i++) { uint64_t v = iv + i * vsz; if (!rd8(v + 9)) continue; int mid = (int)rd32(v + (rev >= 15 ? 0x70 : 0x58)); if (mid >= 0 && mid < 8) to[mid]++; via_splitter += rd32(v + (rev >= 15 ? 0x74 : 0x5c)) != 0xffffffffu; }
                printf("audio:   voices in use by destination mix: 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u, through a splitter: %u; voice updates routed %llu, direct %llu\n", to[0], to[1], to[2], to[3], to[4], to[5], to[6], to[7], via_splitter, g_routed, g_direct);
                for (int m = 0; m < AMIX_MAX; m++) if (amix_ok(m)) printf("audio:   mix %d: volume %.3f buffers %d -> mix %d splitter %d\n", m, g_amix[m].volume, g_amix[m].buffers, g_amix[m].dest_mix, g_amix[m].dest_splitter);
                for (uint32_t e = 0; e < neffect; e++) {           /* effects: type (2 aux 3 delay 4 reverb 5 i3dl2 6 biquad 7 limiter 8 capture 9 compressor), enabled, mix */
                    uint64_t ep = ie + e * 0xc0;
                    if (rd8(ep)) printf("audio:   effect %u: type %u enabled %u mix %d order %u\n", e, rd8(ep), rd8(ep + 2), (int)rd32(ep + 4), rd32(ep + 0x18));
                }
            }
        }
    }
    wr32(out + 0xc, nvoice * 0x10);
    for (uint32_t i = 0; i < neffect; i++, op += esz_out) wr8(op, rd8(ie + i * 0xc0 + 2) ? 3 : 2);   /* enabled: Used, else Initialized */
    wr32(out + 0x14, neffect * esz_out);
    op += nsink * 0x20; wr32(out + 0x1c, nsink * 0x20);
    op += 0x10; wr32(out + 0x20, 0x10);                  /* performance: no history */
    op += 0xb0; wr32(out + 4, 0xb0);                     /* behaviour: no errors */
    wr64(op, frames + 1); wr32(out + 0x28, 0x10);
    wr32(out + 0x3c, (uint32_t)total);
    wr32(out, rd32(in));
    g_efx_in = ie; g_efx_n = neffect;
    if (mixing && graph) amix_run();
    g_efx_in = 0;
    if (mixing) audio_out_frame();
    return 0;
}
/* IAudioRenderer::RequestUpdate (vtable +0x40), called by the lifted SDK [sdk 0x221164]: x1 -> {output, size},
 * x2 -> {performance buffer, size}, x3 -> {input, size}; 0 = success */
static void audren_request_update(Ctx *c, void *user) {
    (void)user;
    Renderer *r = ren_of(A0);
    uint64_t out = rd64(A1), cap = rd64(A1 + 8), in = rd64(A3);
    uint64_t frames = r ? r->frames++ : 0;
    RET(audren_core(in, out, cap, frames));
}
static void ret_float1(Ctx *c) { c->v[0].d[0] = 0; c->v[0].d[1] = 0; c->v[0].f[0] = 1.0f; }
static void audio_device_list(Ctx *c) {                  /* (AudioDeviceName *out, int count) -> count written; a name is 256 bytes */
    if ((int)A1 < 1) { RET(0); return; }
    memset((void *)(uintptr_t)A0, 0, 256);
    strcpy((char *)(uintptr_t)A0, "AudioTvOutput");
    RET(1);
}
static void audio_active_device(Ctx *c) { memset((void *)(uintptr_t)A0, 0, 256); strcpy((char *)(uintptr_t)A0, "AudioTvOutput"); RET(0); }
static void audio_channels(Ctx *c) { RET(2); }

static const ImportDef tomo_imports[] = {
    {"_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", audio_open},
    {"_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", audio_start},
    {"_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE", audio_stop},
    {"_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", audio_close},
    {"_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", audio_work_size},
    {"_ZN2nn5audio34GetAudioRendererVoiceDropParameterENS0_19AudioRendererHandleE", ret_float1},
    {"_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", audio_device_list},
    {"_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", audio_active_device},
    {"_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE", ret_float1},
    {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", audio_channels},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", npad_states_fullkey},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", npad_states_other},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", npad_states_other},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", npad_states_other},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", npad_states_other},
    {"_ZN2nn3hid17InitializeGestureEv", keep},
    {"_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi", ret0},
    {"_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", touch_states},
    {"_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", ret0},
    {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", controller_support},
    {"_ZN2nn3hid15VibrationWriter5WriteERKNS0_14VibrationValueE", keep},
    {"_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", time_user_clock_now},
    {"_ZN2nn4time25AdjustableUserSystemClock6AdjustEv", ret0},
    {"_ZN2nn4time28AdjustableNetworkSystemClock6AdjustEv", ret0},
    {"_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", time_location_name},
    {"_ZN2nn4time13ClockSnapshot31CreateWithAdjustableSystemClockEPS1_", time_snapshot_create},
    {"_ZNK2nn4time13ClockSnapshot35GetStandardUserSystemClockPosixTimeEv", snap_posix},
    {"_ZNK2nn4time13ClockSnapshot38GetStandardUserSystemClockCalendarTimeEv", snap_calendar},
    {"_ZNK2nn4time13ClockSnapshot33GetStandardUserSystemClockContextEv", snap_context},
    {"_ZNK2nn4time13ClockSnapshot15GetLocationNameEv", snap_location},
    {"_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", time_difference},
    {"_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", nntime_to_posix_rule},
    {"_ZN2nn2fs16GetFreeSpaceSizeEPlPKc", fs_free_space},
    {"_ZN2nn2fs18GetSaveDataSizeMaxEPlS1_", fs_save_size_max},
    {"_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll", ret0},
    {"_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", fs_save_exists},
    {"_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE", unavailable},
    {"_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE", keep},
    {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", fs_read_file_opt},
    {"_ZN2nn2oe13GetRegionCodeEv", ret1},                /* 1 = America */
    {"_ZN2nn2oe18GetPerformanceModeEv", ret1},           /* 1 = boost (docked) */
    {"_ZN2nn2oe28SetResumeNotificationEnabledEb", keep},
    {"_ZN2nn2oe33EnableAlbumImageTakenNotificationEv", keep},
    {"_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", keep},
    {"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", keep},
    {"_ZN2nn2pl16IsRunningOnOunceEv", ret0},             /* not the Switch 2 */
    {"_ZN2nn2ro12ProtectRelroEPKvS2_S2_S2_S2_", keep},
    {"_ZN2nn3ldn11SetProtocolENS0_8ProtocolE", keep},
    {"_ZN2nn3ssl10InitializeEj", ret0}, {"_ZN2nn3ssl8FinalizeEv", ret0},
    {"_ZN2nn4pctl25TryBeginFreeCommunicationEb", ret1}, {"_ZN2nn4pctl20EndFreeCommunicationEv", keep},
    {"_ZN2nn6socket10InitializeERKNS0_6ConfigE", ret0},
    {"_ZN2nn6socket12GetLastErrorEv", ret0},
    {"_ZN2nn4diag12GetBacktraceEPmi", ret0},
    {"_ZN2nn4diag13GetSymbolNameEPcmm", ret0},
    {"_ZN2nn4diag6detail9AbortImplEPKNS_6ResultE", abort_trap},
    {"_ZN2nn4diag6detail9AbortImplEv", abort_trap},
    {"_ZN2nn6detail21UnexpectedDefaultImplEv", abort_trap},
    {"_ZNSt3__122__libcpp_verbose_abortEPKcz", abort_trap},
    {"_ZN2nn3mii19IsEnabledSpecialMiiEv", ret0},
    {"_ZN2nn3mii6detail19GetMergedRegionCodeEv", ret1},
    {"_ZN2nn3mii8Database8FinalizeEv", keep},
    {"_ZNK2nn3mii8Database11IsAvailableEv", ret1},
    {"_ZNK2nn3mii8Database8GetIndexEPiRKNS0_8CharInfoE", mii_db_get_index},
    {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv", ret0},
    {"_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", ret1},
    {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", ngc_check_words},
    {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ngc_check_words},
    {"_ZN2nn5swkbd14InlineKeyboardC1Ev", ik_ctor},
    {"_ZN2nn5swkbd14InlineKeyboard10InitializeEPv", ik_initialize},
    {"_ZN2nn5swkbd14InlineKeyboard8FinalizeEv", ik_finalize},
    {"_ZN2nn5swkbd14InlineKeyboard25GetRequiredWorkBufferSizeEv", swkbd_work_size},
    {"_ZN2nn5swkbd14InlineKeyboard25GetImageMemoryRequirementEPmS2_", swkbd_image_req},
    {"_ZN2nn5swkbd14InlineKeyboard8GetImageEPvm", ik_get_image},
    {"_ZN2nn5swkbd14InlineKeyboard4CalcEv", ik_calc},
    {"_ZN2nn5swkbd14InlineKeyboard6AppearERKNS0_9AppearArgE", ik_appear},
    {"_ZN2nn5swkbd14InlineKeyboard9DisappearEv", ik_disappear},
    {"_ZN2nn5swkbd14InlineKeyboard12SetCursorPosEi", keep},
    {"_ZN2nn5swkbd14InlineKeyboard12SetInputTextERKNS0_9InputTextE", ik_set_input_text},
    {"_ZN2nn5swkbd14InlineKeyboard22SetMovedCursorCallbackEPFvPKNS0_14MovedCursorArgEE", ik_set_moved_cb},
    {"_ZN2nn5swkbd14InlineKeyboard23SetDecidedEnterCallbackEPFvPKNS0_15DecidedEnterArgEE", ik_set_enter_cb},
    {"_ZN2nn5swkbd14InlineKeyboard24SetChangedStringCallbackEPFvPKNS0_16ChangedStringArgEE", ik_set_changed_cb},
    {"_ZN2nn5swkbd14InlineKeyboard24SetDecidedCancelCallbackEPFvvE", ik_set_cancel_cb},
    {"_ZN2nn5swkbd14InlineKeyboard27SetFinishedKeyboardCallbackEPFvvE", ik_set_finished_cb},
    {"_ZN2nn5swkbd14InlineKeyboard29SetFinishedInitializeCallbackEPFvvE", ik_set_init_cb},
    {"_ZNK2nn5swkbd14InlineKeyboard13GetWindowSizeEPiS2_", swkbd_window_size},
    {"_ZN2nn5swkbd9InputText3SetEPKDs", ik_input_text_set},
    {"_ZN2nn5prepo10PlayReport4SaveEv", ret0},
    {"_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", ret0},
    {"_ZN2nn5prepo30GetOverSaveFrequencyLimitEventEv", prepo_limit_event},
};

/* ---- SDK-internal locks, cut out of the lifted SDK code (sdklift_roots.txt): their console code ends in kernel calls */
void os_sdk_mutex_lock(uint64_t m); void os_sdk_mutex_unlock(uint64_t m); int os_sdk_mutex_trylock(uint64_t m);
int os_sdk_mutex_mine(uint64_t m); int os_sdk_cv_wait(uint64_t cv, uint64_t m, int64_t ns); void os_sdk_cv_signal(uint64_t cv, int all);
void os_sdk_obj_reset(uint64_t key);                     /* os_threads.c */
static void smx_init(Ctx *c) { os_sdk_obj_reset(A0); }
static void smx_lock(Ctx *c) { os_sdk_mutex_lock(A0); }
static void smx_unlock(Ctx *c) { os_sdk_mutex_unlock(A0); }
static void smx_trylock(Ctx *c) { RET(os_sdk_mutex_trylock(A0)); }
static void smx_mine(Ctx *c) { RET(os_sdk_mutex_mine(A0)); }
static void scv_wait(Ctx *c) { os_sdk_cv_wait(A0, A1, -1); }
static void scv_timedwait(Ctx *c) { RET(os_sdk_cv_wait(A0, A1, (int64_t)A2)); }    /* ConditionVariableStatus: 0 timeout, 1 signalled */
static void scv_signal(Ctx *c) { os_sdk_cv_signal(A0, 0); }
static void scv_broadcast(Ctx *c) { os_sdk_cv_signal(A0, 1); }
static const ImportDef sdk_lock_overrides[] = {
    {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ngc_check_words},      /* also reached through a pointer by SDK code (Local Play checks a Mii's names before sending it: unlifted, the game stopped there) */
    {"_ZN2nn2os12SdkMutexType10InitializeEv", smx_init}, {"_ZN2nn2os12SdkMutexType4LockEv", smx_lock},
    {"_ZN2nn2os12SdkMutexType6UnlockEv", smx_unlock}, {"_ZN2nn2os12SdkMutexType7TryLockEv", smx_trylock},
    {"_ZNK2nn2os12SdkMutexType23IsLockedByCurrentThreadEv", smx_mine},
    {"_ZN2nn2os21SdkRecursiveMutexType10InitializeEv", smx_init}, {"_ZN2nn2os21SdkRecursiveMutexType4LockEv", smx_lock},
    {"_ZN2nn2os21SdkRecursiveMutexType6UnlockEv", smx_unlock}, {"_ZN2nn2os21SdkRecursiveMutexType7TryLockEv", smx_trylock},
    {"_ZNK2nn2os21SdkRecursiveMutexType23IsLockedByCurrentThreadEv", smx_mine},
    {"_ZN2nn2os24SdkConditionVariableType10InitializeEv", smx_init},
    {"_ZN2nn2os24SdkConditionVariableType4WaitERNS0_12SdkMutexTypeE", scv_wait},
    {"_ZN2nn2os24SdkConditionVariableType4WaitERNS0_21SdkRecursiveMutexTypeE", scv_wait},
    {"_ZN2nn2os24SdkConditionVariableType9TimedWaitERNS0_12SdkMutexTypeENS_8TimeSpanE", scv_timedwait},
    {"_ZN2nn2os24SdkConditionVariableType9TimedWaitERNS0_21SdkRecursiveMutexTypeENS_8TimeSpanE", scv_timedwait},
    {"_ZN2nn2os24SdkConditionVariableType6SignalEv", scv_signal},
    {"_ZN2nn2os24SdkConditionVariableType9BroadcastEv", scv_broadcast},
    /* rumble: dropped for now (nnhid.c's SDL rumble could take these) */
    {"_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", ret0},
    {"_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", ret0},
    {"_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", ret0},
};
void sdk_host_override(const char *name, ImportFn fn);   /* sdklift_rt.c */

/* TOMO_HEAPLOG=1 (diagnosis): every failed allocation of the game's sead::ExpHeap::tryAlloc [game 0x71001e68c0]
 * (this, size, alignment) with the heap's name (sead::Heap: the name pointer after the IDisposer, +0x20) and its size. */
int hook_install(uint64_t addr, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);      /* fighter_sdk/hook_host.c */
/* The game addresses written here are 1.0.0's (main's code 0x25ce6a0 bytes). Update v262144 (code 0x256d250 bytes) has the
 * same functions elsewhere (found by their code; the struct offsets they use are unchanged). Any other build gets no hook. */
static int tomo_hook(uint64_t a, void (*repl)(Ctx *, void *), ImportFn *orig) {
    static const uint64_t v262144[][2] = {{0x71008ce838ull, 0x7100897d48ull}, {0x71006ec3c0ull, 0x71006ce020ull},
        {0x710022dce0ull, 0x7100241170ull}, {0x71005e1ffcull, 0x71005824b8ull}, {0x71001e68c0ull, 0x7100232360ull}};
    uint64_t at = 0;
    if (g_main.text_size == 0x25ce6a0) at = a;
    else if (g_main.text_size == 0x256d250)
        for (size_t i = 0; i < sizeof v262144 / sizeof v262144[0]; i++) if (v262144[i][0] == a) at = v262144[i][1];
    return at && hook_install(at, repl, NULL, orig);
}
static ImportFn g_try_alloc;
const char *guest_thread_name_of(DWORD tid);               /* os_threads.c */
static const char *heap_name(uint64_t heap) {
    uint64_t np = rd64(heap + 0x20);
    return np > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)np, 1) ? (const char *)(uintptr_t)np : "?";
}
/* sead::ExpHeap: mSize +0x30, mFreeList +0xd0 (sentinel, next +0xd8, count +0xe0, node offset +0xe4), mUseList +0xf0 (next +0xf8,
 * count +0x100, node offset +0x104); MemBlock: mSize +0x18, the memory at +0x20. Prints the biggest blocks of a list. */
static void heap_list_report(uint64_t heap, uint64_t list, const char *what) {
    uint64_t top[6] = {0}, topat[6] = {0}, node = rd64(list + 8), sum = 0;
    int32_t off = (int32_t)rd32(list + 0x14);
    uint32_t n = 0;
    while (node != list && node > 0x10000 && n < 200000 && !IsBadReadPtr((void *)(uintptr_t)node, 0x20)) {
        uint64_t blk = node - (uint64_t)off, sz = rd64(blk + 0x18);
        sum += sz; n++;
        for (int i = 0; i < 6; i++) if (sz > top[i]) { for (int j = 5; j > i; j--) { top[j] = top[j - 1]; topat[j] = topat[j - 1]; } top[i] = sz; topat[i] = blk + 0x20; break; }
        node = rd64(node + 8);
    }
    printf("heaplog:   %s: %u blocks, 0x%llx bytes in all (list count %u); biggest:", what, n, (unsigned long long)sum, rd32(list + 0x10));
    for (int i = 0; i < 6 && top[i]; i++) printf(" 0x%llx@%llx", (unsigned long long)top[i], (unsigned long long)topat[i]);
    printf("\n");
    (void)heap;
}
static void heaplog_try_alloc(Ctx *c, void *user) {
    (void)user;
    uint64_t heap = A0, size = A1;
    uint64_t from = c->x[30];
    g_try_alloc(c);
    const char *name = heap_name(heap);
    const char *tn = guest_thread_name_of(GetCurrentThreadId());
    const char *want = getenv("TOMO_HEAPLOG");
    if (c->x[0]) {                                       /* TOMO_HEAPLOG=<heap name>: that heap's allocations too; =1: every one of 1 MB or more */
        static int n;
        if (want[0] == '1' ? size >= 0x100000 : (!strcmp(name, want) && (size >= 0x100000 || n++ < 400)))
            printf("heaplog: %s +0x%llx at %llx (from %llx, %s)\n", name, (unsigned long long)size, (unsigned long long)c->x[0], (unsigned long long)from, tn ? tn : "?");
        return;
    }
    static int said;
    if (said++ >= 40) return;
    printf("heaplog: %.40s (heap %llx, size 0x%llx) could not give 0x%llx bytes (alignment %d), called from %llx on %s\n", name,
           (unsigned long long)heap, (unsigned long long)rd64(heap + 0x30), (unsigned long long)size, (int)(int32_t)A2, (unsigned long long)from, tn ? tn : "?");
    heap_list_report(heap, heap + 0xd0, "free");
    heap_list_report(heap, heap + 0xf0, "used");
}
/* TOMO_TASKLOG=1 (diagnosis): the scene start-up's task step [game 0x71005e1ffc] (task): every change of a task's state
 * (+0x10) or sub-state (+0x68), with its method table */
static ImportFn g_task_step;
static void tasklog_step(Ctx *c, void *user) {
    (void)user;
    uint64_t t = A0;
    g_task_step(c);
    static struct { uint64_t t; uint32_t s, s2; } seen[32];
    uint32_t s = rd32(t + 0x10), s2 = rd32(t + 0x68);
    int i = 0;
    while (i < 32 && seen[i].t && seen[i].t != t) i++;
    if (i == 32) return;
    if (seen[i].t == t && seen[i].s == s && seen[i].s2 == s2) return;
    seen[i].t = t; seen[i].s = s; seen[i].s2 = s2;
    printf("tasklog: task %llx (method table %llx) state %u sub %u\n", (unsigned long long)t, (unsigned long long)rd64(t), s, s2);
}
/* TOMO_TRACE=<hex addr>[,<hex addr>...] (diagnosis): every call of those guest functions is logged with x0..x3 at entry and
 * x0 at return (the first 20 calls of each, then one in 500), with the caller and the thread. No rebuild between probes. */
const char *guest_thread_name_of(DWORD tid);               /* os_threads.c */
typedef struct { uint64_t addr; ImportFn orig; unsigned n; } TraceSite;
static TraceSite g_trace_sites[32];
static void trace_site_call(Ctx *c, void *user) {
    TraceSite *t = user;
    unsigned n = ++t->n;
    static volatile LONG total; static int all = -1;
    if (all < 0) all = getenv("TOMO_TRACE_ALL") != NULL;                  /* TOMO_TRACE_ALL=1: every call, the first 30000 in all */
    int say = n <= 20 || n % 500 == 0 || (all && InterlockedIncrement(&total) < 30000);
    uint64_t a0 = A0, a1 = A1, a2 = A2, a3 = A3, from = c->x[30];
    float f0 = c->v[0].f[0], f1 = c->v[1].f[0], f2 = c->v[2].f[0], f3 = c->v[3].f[0];
    const char *name = guest_thread_name_of(GetCurrentThreadId());
    t->orig(c);
    if (say) printf("trace %llx #%u [%s]: x0 %llx x1 %llx x2 %llx x3 %llx s0-3 %g %g %g %g -> %llx (from %llx)\n", (unsigned long long)t->addr, n, name ? name : "main",
                    (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, f0, f1, f2, f3, (unsigned long long)c->x[0], (unsigned long long)from);
}
static void trace_install(void) {
    const char *e = getenv("TOMO_TRACE");
    int k = 0;
    for (const char *p = e; p && *p && k < 32;) {
        uint64_t a = strtoull(p, (char **)&p, 16);
        if (a) { g_trace_sites[k].addr = a; printf("trace: %llx %s\n", (unsigned long long)a, hook_install(a, trace_site_call, &g_trace_sites[k], &g_trace_sites[k].orig) ? "hooked" : "NOT hooked (not a function entry?)"); k++; }
        while (*p == ',' || *p == ' ') p++;
    }
}
/* Widescreen (graphics.ini widescreen, nvn_gl.c wide_k): the game's cameras set their projection with [0x71008ce838]
 * (this, near s0, far s1, fovy s2, aspect s3; the island camera from 0x7100205c84 with its aspect at camera + 0x234). A
 * 16:9 one is given the wider aspect, so the 3D view itself is wider: hor+, and the frustum culling and every pass that
 * rebuilds positions from depth with the camera's matrices (water, lighting: compute) agree with it. Squeezing the clip x
 * of the vertices instead left those passes on the 16:9 matrices (blocks in the water, ghost palms). 2D keeps its 16:9
 * shape (mx_wide_fix, 2D only). TOMO_WIDE_CAMERA=0: the old vertex squeeze for 3D too. */
static ImportFn g_persp;
static void persp_wide(Ctx *c, void *user) {
    (void)user;
    float nvn_wide_k(void), k = nvn_wide_k();
    if (k != 1.0f && fabsf(c->v[3].f[0] - 16.0f / 9.0f) < 0.01f) c->v[3].f[0] *= k;
    g_persp(c);
}
/* The layouts (menus, dialogs) are drawn with a perspective camera of their own, made by [0x71006ec3c0] (this, width s0,
 * height s1, fovy s2: 1920 x 1080, 20 degrees) at the distance where that rectangle fills the view. Wider by k, the layout
 * keeps its size and shape in the middle and what lies beyond 16:9 shows at the sides: nothing is stretched. */
static ImportFn g_layout_cam;
static void layout_cam_wide(Ctx *c, void *user) {
    (void)user;
    float nvn_wide_k(void), k = nvn_wide_k();
    if (k != 1.0f && c->v[1].f[0] > 0.0f && fabsf(c->v[0].f[0] / c->v[1].f[0] - 16.0f / 9.0f) < 0.01f) c->v[0].f[0] *= k;
    g_layout_cam(c);
}
static void persp_install(void) {
    void nvn_wide_camera(void);
    if (getenv("TOMO_WIDE_CAMERA") && getenv("TOMO_WIDE_CAMERA")[0] == '0') return;
    if (tomo_hook(0x71008ce838ull, persp_wide, &g_persp) && tomo_hook(0x71006ec3c0ull, layout_cam_wide, &g_layout_cam)) nvn_wide_camera();
    else printf("widescreen: the camera projection hook is not installed (the 3D view is squeezed instead)\n");
}
/* The hand pointer follows the mouse (nnhid.c mouse_poll). [0x710022dce0] (pointer) updates the hand once a frame: it
 * keeps the position at +0x118 (x, y in the 1280x720 screen from the top left) as the previous one (+0x1b0), moves it by
 * the stick (a direct call to 0x710022e7e0, which also places the drawn hand) and works out what lies under it
 * (0x7100284b80: the panes under the hand). With the mouse in use the position is the mouse's before that, so hovering,
 * picking and dragging happen where the mouse is; the game still hides an idle hand 2-3 s after the last stick input
 * (a position put in is none), hence nnhid.c's nudge while the mouse moves. The hand is shown while bit 0 of the halfword
 * at +0x14 is set. Found 2026-10-28 with TOMO_MEMSCAN and write watchpoints. TOMO_MOUSE=0 off. */
static ImportFn g_pointer_update;
static void pointer_hook(Ctx *c, void *user) {
    int nnhid_mouse(float xy[2]); void display_mouse_hand(void);
    float xy[2];
    (void)user;
    {   static int n; static const char *dbg = (const char *)-1; if (dbg == (const char *)-1) dbg = getenv("TOMO_MOUSE_DEBUG");      /* TEST ONLY: the pointer's state */
        const float *f = (const float *)(uintptr_t)A0; const int *w = (const int *)(uintptr_t)A0;
        if (dbg && n++ % 30 == 0) printf("mouse: state flags %x 1a0 %d 1a4 %d pos %g,%g prev %g,%g 08 %x 10 %x 114 %x%c", *(const uint16_t *)(uintptr_t)(A0 + 0x14), w[0x1a0 / 4], w[0x1a4 / 4],
                                        f[0x118 / 4], f[0x11c / 4], f[0x1b0 / 4], f[0x1b4 / 4], w[2], w[4], w[0x114 / 4], 10); }
    if (*(const uint16_t *)(uintptr_t)(A0 + 0x14) & 1) { void nnhid_pointer_free(void); nnhid_pointer_free(); }
    if ((*(const uint16_t *)(uintptr_t)(A0 + 0x14) & 1) && nnhid_mouse(xy)) { memcpy((void *)(uintptr_t)(A0 + 0x118), xy, 8); display_mouse_hand(); }
    g_pointer_update(c);
}
static void pointer_install(void) {
    if (getenv("TOMO_MOUSE") && getenv("TOMO_MOUSE")[0] == '0') return;
    if (!tomo_hook(0x710022dce0ull, pointer_hook, &g_pointer_update)) printf("mouse: the hand pointer hook is not installed (the mouse cannot move the hand)%c", 10);
}
static void heaplog_install(void) {
    trace_install();
    if (getenv("TOMO_TASKLOG")) printf("tasklog: hook %s\n", tomo_hook(0x71005e1ffcull, tasklog_step, &g_task_step) ? "installed" : "NOT installed");
    if (!getenv("TOMO_HEAPLOG")) return;
    printf("heaplog: hook %s\n", tomo_hook(0x71001e68c0ull, heaplog_try_alloc, &g_try_alloc) ? "installed" : "NOT installed");
}

/* TEST ONLY, TOMO_MEMSCAN=<step>:<op>,... with SSBU_SEQ: a search for a game value that follows the input (the hand cursor).
 * At the start of each listed sequence step the 32-bit floats of the game's heap (committed pages of the write-watched
 * regions) are compared with the previous look: snap = copy everything; up / down = keep the floats that grew / shrank by
 * 0.01..100000 since the previous look; same = keep those unchanged. Up to 40 survivors are printed after each look. */
int nvn_ww_regions(uint64_t *base, uint64_t *size, int max);      /* nvn_gl.c */
typedef struct { uint64_t a; float v; } MsCand;
static struct { uint64_t a, n; uint8_t *copy; } g_ms_snap[4096];
static int g_ms_nsnap;
static MsCand *g_ms; static size_t g_ms_n;
static int ms_keep(float was, float now, char op) {
    if (now != now || was != was || now > 1e9f || now < -1e9f) return 0;
    float d = now - was;
    return op == 'u' ? d > 0.01f && d < 1e5f : op == 'd' ? -d > 0.01f && -d < 1e5f : d == 0.0f && was != 0.0f;
}
void tomo_memscan_step(int step) {
    static const char *spec = (const char *)-1;
    if (spec == (const char *)-1) spec = getenv("TOMO_MEMSCAN");
    if (!spec) return;
    char op = 0;
    for (const char *p = spec; *p;) {
        int s = atoi(p); const char *q = strchr(p, ':');
        if (q && s == step) { op = q[1]; break; }
        if (!(p = strchr(p, ','))) break;
        p++;
    }
    if (!op) return;
    if (op == 'w' || op == 'W') {                  /* watch: write watchpoints (debug_mode.c) on up to 4 survivors that look like screen x, y (W: any) */
        void debug_watch(const uint64_t *addrs, int n);
        uint64_t w[4]; int n = 0;
        for (size_t i = 0; i < g_ms_n && n < 4; i++) {
            float y = *(const float *)(uintptr_t)(g_ms[i].a + 4);
            if (op == 'W' || (g_ms[i].v >= 10.0f && g_ms[i].v <= 1280.0f && y >= 0.0f && y <= 720.0f)) { w[n++] = g_ms[i].a; printf("memscan: watching %llx%c", (unsigned long long)g_ms[i].a, 10); }
        }
        if (n) debug_watch(w, n);
        return;
    }
    if (op == 's') {                               /* snap */
        uint64_t base[64], size[64], total = 0;
        int nr = nvn_ww_regions(base, size, 64);
        for (int i = 0; i < g_ms_nsnap; i++) free(g_ms_snap[i].copy);
        g_ms_nsnap = 0;
        for (int r = 0; r < nr; r++)
            for (uint64_t a = base[r]; a < base[r] + size[r];) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
                uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
                if (end > base[r] + size[r]) end = base[r] + size[r];
                if (mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE && g_ms_nsnap < 4096) {
                    uint8_t *cp = malloc((size_t)(end - a));
                    if (cp) { memcpy(cp, (void *)(uintptr_t)a, (size_t)(end - a)); g_ms_snap[g_ms_nsnap].a = a; g_ms_snap[g_ms_nsnap].n = end - a; g_ms_snap[g_ms_nsnap++].copy = cp; total += end - a; }
                }
                a = end;
            }
        free(g_ms); g_ms = NULL; g_ms_n = 0;
        printf("memscan: step %d snap: %d ranges, %.1f MB%c", step, g_ms_nsnap, (double)total / 1048576.0, 10);
        return;
    }
    size_t cap = 0, k = 0;
    if (g_ms_nsnap) {                              /* first compare: against the copy */
        for (int i = 0; i < g_ms_nsnap; i++) {
            const float *was = (const float *)g_ms_snap[i].copy, *now = (const float *)(uintptr_t)g_ms_snap[i].a;
            for (uint64_t w = 0; w < g_ms_snap[i].n / 4; w++)
                if (ms_keep(was[w], now[w], op)) {
                    if (k == cap) { cap = cap ? cap * 2 : 1 << 20; g_ms = realloc(g_ms, cap * sizeof *g_ms); }
                    g_ms[k].a = g_ms_snap[i].a + w * 4; g_ms[k++].v = now[w];
                }
            free(g_ms_snap[i].copy);
        }
        g_ms_nsnap = 0;
    } else {
        for (size_t i = 0; i < g_ms_n; i++) {
            MEMORY_BASIC_INFORMATION mi;
            if (i == 0 || (g_ms[i].a >> 12) != (g_ms[i - 1].a >> 12)) {
                if (!VirtualQuery((void *)(uintptr_t)g_ms[i].a, &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Protect != PAGE_READWRITE) {      /* (page gone) */
                    uint64_t pg = g_ms[i].a >> 12; while (i + 1 < g_ms_n && (g_ms[i + 1].a >> 12) == pg) i++;
                    continue;
                }
            }
            float now = *(const float *)(uintptr_t)g_ms[i].a;
            if (ms_keep(g_ms[i].v, now, op)) { g_ms[k].a = g_ms[i].a; g_ms[k++].v = now; }
        }
    }
    g_ms_n = k;
    printf("memscan: step %d %s: %llu candidates%c", step, op == 'u' ? "up" : op == 'd' ? "down" : "same", (unsigned long long)k, 10);
    for (size_t i = 0; i < k && i < 40; i++) printf("memscan:   %llx = %g (next %g %g)%c", (unsigned long long)g_ms[i].a, g_ms[i].v,
                                                    *(const float *)(uintptr_t)(g_ms[i].a + 4), *(const float *)(uintptr_t)(g_ms[i].a + 8), 10);
}

static void ret7(Ctx *c) { RET(7); }
/* answers that replace the SSBU runtime's do-nothing ones: a do-nothing handler leaves `this` in x0, which this game reads
 * as a failed Result (nn::mii::Database::Initialize) or as a core mask */
static const ImportDef tomo_overrides[] = {
    {"_ZN2nn3mii8Database10InitializeEv", ret0},
    {"_ZN2nn4time13IsInitializedEv", ret1},
    {"_ZN2nn2os26GetThreadAvailableCoreMaskEv", ret7},
};
void register_tomo_imports(void) {
    persp_install();
    pointer_install();
    audio_volume_selftest();                     /* SSBU_AUDIO_VOLUME_SELFTEST=<expected startup percent>, -1 for no device */
    for (size_t i = 0; i < sizeof tomo_imports / sizeof tomo_imports[0]; i++)
        if (!import_lookup(tomo_imports[i].name)) register_imports(&tomo_imports[i], 1);
    register_imports(tomo_overrides, (int)(sizeof tomo_overrides / sizeof tomo_overrides[0]));
    for (size_t i = 0; i < sizeof sdk_lock_overrides / sizeof sdk_lock_overrides[0]; i++)
        sdk_host_override(sdk_lock_overrides[i].name, sdk_lock_overrides[i].fn);
    heaplog_install();
}
