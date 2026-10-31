/* Host display: a real window, 60 Hz pacing and the guest's vsync event.
 * The window's contents are drawn by nvn_gl.c, whose render thread owns the OpenGL context. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <math.h>
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define WIDTH 1280
#define HEIGHT 720
#define FRAME_NS 16666667LL

void os_init_event(uint64_t key, int autoclear);      /* os_threads.c */
void os_signal_event(uint64_t key);

static HWND g_hwnd;
#ifdef _WIN32
static HICON g_icon_small, g_icon_big;                /* the title bar's small icon (opensmash.ico), set after the window is made */
#endif
static volatile LONG g_ready;
static volatile LONG64 g_frames;
long long display_frame_count(void) { return g_frames; }   /* os_threads.c: the sampler's stall report */
volatile LONG64 g_prof15_frame;                       /* presented frames, read by prof15.c (SSBU_PROF15, test only; included by boot.c) */
void nvn_gl_start(void *hwnd);                        /* nvn_gl.c */
void nvn_gl_report(FILE *out);
static uint64_t g_vsync_event;
static LARGE_INTEGER g_freq;

/* P3 round 3: ONE refresh clock. The game counts the signals of the vsync event on a thread of its own ("VSync Count
 * Thread", 0x7103810a40: window object + 0x138) and, at every present (0x710386fc30), advances its frame counter
 * (0x710593a440) by the number of refreshes counted since the previous present. So the refresh count IS the game's
 * time. Before this, the event came from a Sleep() loop and presentation was held by a second, independent 60 Hz
 * clock that restarted its phase after every late frame: between two presents the game counted 0, 1 or 2 refreshes,
 * and "0" counts as one step while "2" counts as two, so the match clock gained (10.77 s per 600 frames).
 * Now: refresh n happens at t0 + n/60 s; at each one the vsync event is signalled and then the frames waiting to be
 * shown are released. A frame queued between refresh n and n+1 is shown at n+1, and no sooner than `interval`
 * refreshes after the frame before it (nvnWindowSetPresentInterval); the game is held in its wait for the window
 * texture (nvnSyncWait on the sync of nvnWindowAcquireTexture), which is AFTER it has read the refresh count, as on
 * the console. SSBU_OLD_PACING=1: the two clocks as before. SSBU_CLOCKSTAT=1: one line every 600 presented frames. */
static int g_old_pacing = -1, g_clockstat;
/* FAST-FORWARD (2026-10-30, the quality bar's "fast-forward" option): while Tab (display_ff_key) or both stick buttons
 * (display_ff_pad, menu.cpp) are held, the refresh clock gives graphics.ini fast_forward (2, 3, 4) x 60 ticks a second
 * from our own clock (the display lock is left and retaken afterwards): the game steps one frame per tick, so it runs
 * that many times faster, the island's clock included. nvn_gl.c presents every ff-th frame (the monitor cannot show
 * more), nnaudio.c plays every ff-th sound buffer and releases the others on a clock ff times faster, tomo.c's audio
 * frame clock ticks ff times faster. TOMO_FF=<n> (test): always on. */
volatile LONG g_ff = 1;
static int g_ff_key, g_ff_pad, g_ff_env = -1;
int gfx_ff_speed(void);
static void ff_update(void) {
    if (g_ff_env < 0) { const char *e = getenv("TOMO_FF"); g_ff_env = e ? atoi(e) : 0; }
    int ff = g_ff_env > 1 ? g_ff_env : (g_ff_key || g_ff_pad) ? gfx_ff_speed() : 1;
    if (ff != g_ff) { InterlockedExchange(&g_ff, ff); printf("display: fast-forward %s%c", ff > 1 ? "on" : "off", 10); fflush(stdout); }
}
int display_ff(void) { return g_ff; }
void display_ff_key(int on) { g_ff_key = on != 0; ff_update(); }
void display_ff_pad(int on) { g_ff_pad = on != 0; ff_update(); }
static volatile LONG64 g_vsync_n;            /* refreshes passed (published after the event was signalled) */
static volatile LONG64 g_vsync_signals, g_vsync_late;
static SRWLOCK g_vsync_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_vsync_cv = CONDITION_VARIABLE_INIT;
static LONG64 g_latch;                       /* refresh at which the last queued frame is shown */
static volatile LONG g_latch_pending;        /* a frame was queued and nobody has waited for its refresh yet */
static volatile LONG64 g_tick_at[16];        /* L1: QPC time at which tick n was published (index n & 15) */
static int old_pacing(void) {
    if (g_old_pacing < 0) { const char *e = getenv("SSBU_OLD_PACING"); g_old_pacing = e && atoi(e) != 0; g_clockstat = getenv("SSBU_CLOCKSTAT") && atoi(getenv("SSBU_CLOCKSTAT")) != 0; }
    return g_old_pacing;
}
static void vsync_test_key(void);

/* ---- Track V session 14: frame pacing against the DISPLAY.
 * (1) The display's vertical blanks are timed on a thread of their own (D3DKMTWaitForVerticalBlankEvent on the output of
 *     the monitor the window is on: the kernel's own vblank interrupt, no swapchain involved): refresh period, a count.
 * (2) PRESENT AT THE TICK: the frame the game finished is handed to the window at the next tick of the refresh clock
 *     (display_present_plan on the game's thread decides the tick, display_hold_present on the render thread waits for
 *     it just before vkQueuePresentKHR), not "tick + the time the frame took". A frame that is LATE (queued after the
 *     tick it was due at) is presented at once, as before. The game is released at the same tick as before: its own
 *     timing does not change. SSBU_PACE_HOLD=0: presents as before (when ready).
 * (3) DISPLAY LOCK (vsync = display | auto in graphics.ini, SSBU_PACE=display|clock): when the measured refresh rate is
 *     within 1.1 % of k x 60 Hz (60, 119.88, 120, 240 ...), the tick is every k-th vblank + half a refresh (the present
 *     then lands in the middle between two vblanks: as far from the compositor's latch as it can be), so every game
 *     frame is on screen for exactly k refreshes. The game then runs at refresh / k frames per second (59.94 on a
 *     119.88 Hz panel: 0.1 % slow). Any other panel (75, 100, 144, 165 Hz): our own 60.000 Hz clock, presents at the tick.
 * (4) What the screen got: for every present, the vblank that follows it; a frame's time on screen = the vblanks between
 *     its present and the next frame's (0 = replaced before it was shown). Histogram in the report and per 600 frames
 *     with SSBU_PACESTAT=1. This is the compositor model "the last picture presented before a vblank is what the next
 *     refresh shows" on MEASURED vblank times; it is not a camera. */
#ifdef _WIN32
typedef struct { HDC hDc; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; } KmtOpenAdapterFromHdc;
typedef struct { UINT hAdapter; UINT hDevice; UINT VidPnSourceId; } KmtWaitVblank;
#endif
static SRWLOCK g_vb_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_vb_cv = CONDITION_VARIABLE_INIT;
static volatile LONG64 g_vb_count, g_vb_qpc;           /* vblanks seen, time of the last one (under g_vb_lock for a consistent pair) */
static double g_vb_period;                             /* QPC ticks per refresh, smoothed; 0 = not measured yet */
static volatile LONG g_vb_state;                       /* 0 not started, 1 measuring, 2 measured, -1 no vblank source */
static int g_pace_want = -1;                           /* 0 clock, 1 display lock when the refresh rate allows */
static int g_hold_on = -1;                             /* present at the tick */
static volatile LONG g_lock_k;                         /* display lock: refreshes per game frame, 0 = our own clock */
static double g_lock_phase = 0.5;                      /* the tick's place between two vblanks */
static volatile LONG64 g_hold_target;                  /* tick the planned present waits for, 0 = present at once */
static volatile LONG64 g_defer_target;                 /* a finished frame is kept on the render thread for this tick (display_present_defer) */
void nvn_front_post_present_flush(void);               /* nvn_gl.c: the kept present, as a job of the render thread */
static volatile LONG g_planned;
volatile LONG g_display_close;                         /* the window was closed (WM_CLOSE): nvn_vk_m4.c ends its first-run hold */
volatile LONG g_display_holding;                      /* hitch.c: the render thread is holding a finished frame for its tick = pacing */
static volatile LONG64 g_hold_ticks, g_hold_n, g_hold_late;
/* what the screen got */
#define PS_HIST 6
static struct { LONG64 n, hist[PS_HIST]; double sum, sum2, mn, mx; } g_ps[2], g_ps_last[2];      /* [0] all frames, [1] fight frames */
static LONG64 g_ps_prev_vb, g_ps_prev_qpc;
static LONG64 g_ps_phase[10];                          /* where in the refresh the presents landed (tenths of a period after a vblank) */

/* ---- Session 14: PERFORMANCE CORES. Found with the four-player timing runs on this PC (Core i9-13950HX: 8 performance
 * + 16 efficiency cores, power plan "balanced"): the same fight cost 8 ms of the main thread per frame in one run and
 * 11-12 ms in the next four (every game thread 40 % slower, 57.5-59.3 fps instead of 60.0): the scheduler had the game's
 * threads on efficiency cores. Now: the process says it is not to be throttled (ProcessPowerThrottling: execution speed
 * off = high quality of service) and the threads that make a frame (the game's main thread, the render thread, the
 * refresh clock) are restricted to the CPU sets of the highest efficiency class. SSBU_PCORES=0: as before.
 * The report says on which class of core the game's thread was seen at each present. */
static ULONG g_pc_ids[128], g_pc_core[128], g_pc_lp[128], g_slow_ids[128];
static int g_slow_n;
static int g_pc_n = -1, g_pc_on = 1;
static uint8_t g_cpu_class[256], g_class_max;
static volatile LONG64 g_pc_frames, g_pc_frames_slow;
#ifdef _WIN32
static void pcore_init(void) {
    ULONG len = 0;
    const char *e = getenv("SSBU_PCORES");
    g_pc_n = 0;
    if (e && !atoi(e)) g_pc_on = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    uint8_t *buf = len ? malloc(len) : NULL;
    if (!buf || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) { free(buf); return; }
    int ncpu = 0, nslow = 0;
    for (int pass = 0; pass < 2; pass++)
        for (ULONG at = 0; at + sizeof(SYSTEM_CPU_SET_INFORMATION) <= len; ) {
            PSYSTEM_CPU_SET_INFORMATION s = (PSYSTEM_CPU_SET_INFORMATION)(buf + at);
            if (!s->Size) break;
            if (s->Type == CpuSetInformation) {
                if (!pass) { if (s->CpuSet.EfficiencyClass > g_class_max) g_class_max = s->CpuSet.EfficiencyClass; g_cpu_class[s->CpuSet.LogicalProcessorIndex] = s->CpuSet.EfficiencyClass; ncpu++; }
                else if (s->CpuSet.EfficiencyClass == g_class_max) { if (g_pc_n < 128) { g_pc_core[g_pc_n] = s->CpuSet.CoreIndex; g_pc_lp[g_pc_n] = s->CpuSet.LogicalProcessorIndex; g_pc_ids[g_pc_n++] = s->CpuSet.Id; } }
                else { nslow++; if (g_slow_n < 128) g_slow_ids[g_slow_n++] = s->CpuSet.Id; }
            }
            at += s->Size;
        }
    free(buf);
    if (!nslow) { g_pc_n = 0; printf("display: CPU: %d logical processors of one class: nothing to choose%c", ncpu, 10); return; }
    printf("display: CPU: %d logical processors, %d of the performance class, %d of an efficiency class: %s%c", ncpu, g_pc_n, nslow,
           g_pc_on ? "the game's main thread, the render thread and the refresh clock run on the performance class only; the process asks not to be throttled (SSBU_PCORES=0: left to the system)" : "left to the system (SSBU_PCORES=0)", 10);
    if (g_pc_on) {
        PROCESS_POWER_THROTTLING_STATE st;
        memset(&st, 0, sizeof st);
        st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION; st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED; st.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
    }
}
#else
/* Linux build: CPU sets and ProcessPowerThrottling are Windows calls; the scheduler is left to choose the cores */
static void pcore_init(void) { g_pc_n = 0; printf("display: CPU: the cores are left to the system (performance-core pinning is a Windows feature of this build)%c", 10); }
#endif
/* the calling thread: the efficiency cores only, where the CPU has them (the Mii chatter's AI: it then never shares a
 * performance core, nor its other hardware thread, with the game's threads) */
void display_slow_thread(void) {
    if (g_pc_n < 0) pcore_init();
#ifdef _WIN32
    if (g_pc_on && g_slow_n > 0) SetThreadSelectedCpuSets(GetCurrentThread(), g_slow_ids, (ULONG)g_slow_n);
#endif
}
/* the calling thread: performance cores only (once per thread is enough; cheap to repeat) */
void display_perf_thread(void) {
    if (g_pc_n < 0) pcore_init();
#ifdef _WIN32
    if (g_pc_on && g_pc_n > 0) SetThreadSelectedCpuSets(GetCurrentThread(), g_pc_ids, (ULONG)g_pc_n);
#endif
}
/* A host thread that makes frames (the render thread, the refresh clock): the performance cores the console core model
 * (os_threads.c, SSBU_OS_CONSOLE_CORES) did not take for the game's threads, neither hardware thread of them. On the shared
 * cores the game's higher-priority threads kept the render thread waiting: 12 ms of preparing draws a frame took 3 ms of
 * processor time on the island (2026-10-24). */
void display_perf_host_thread(void) {
    if (g_pc_n < 0) pcore_init();
#ifdef _WIN32
    uint64_t os_console_lp_mask(void);
    uint64_t cc = os_console_lp_mask();
    ULONG ids[128];
    int n = 0;
    for (int i = 0; i < g_pc_n; i++) {
        int taken = 0;
        for (int j = 0; j < g_pc_n; j++) if (g_pc_lp[j] < 64 && (cc >> g_pc_lp[j] & 1) && g_pc_core[j] == g_pc_core[i]) taken = 1;
        if (!taken) ids[n++] = g_pc_ids[i];
    }
    if (g_pc_on && n > 0) SetThreadSelectedCpuSets(GetCurrentThread(), ids, (ULONG)n);
    else display_perf_thread();
#endif
}
static void precise_wait_until(LONGLONG target, HANDLE timer) {
    LARGE_INTEGER now, due;
    QueryPerformanceCounter(&now);
    LONGLONG left = target - now.QuadPart;
    if (left <= 0) return;
    due.QuadPart = -((left - g_freq.QuadPart / 2000) * 10000000 / g_freq.QuadPart);
    if (due.QuadPart < 0) { if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100); else Sleep((DWORD)(left * 1000 / g_freq.QuadPart)); }
    for (QueryPerformanceCounter(&now); now.QuadPart < target; QueryPerformanceCounter(&now)) YieldProcessor();
}
#ifndef _WIN32
/* Linux build: no vblank source yet (D3DKMT is Windows); vsync = auto / display then run on our own 60 Hz clock, as on a
 * Windows PC without one */
static DWORD WINAPI vblank_thread(LPVOID arg) {
    (void)arg;
    printf("display: no vblank source in the Linux build: the refresh clock stays our own 60 Hz clock, on-screen times are not measured%c", 10);
    InterlockedExchange(&g_vb_state, -1);
    return 0;
}
#else
static DWORD WINAPI vblank_thread(LPVOID arg) {
    (void)arg;
    HMODULE gdi = LoadLibraryA("gdi32.dll");
    LONG (WINAPI *open_adapter)(KmtOpenAdapterFromHdc *) = gdi ? (void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc") : NULL;
    LONG (WINAPI *wait_vblank)(const KmtWaitVblank *) = gdi ? (void *)GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent") : NULL;
    MONITORINFOEXA mi;
    KmtOpenAdapterFromHdc oa;
    KmtWaitVblank wv;
    HDC dc = NULL;
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    memset(&oa, 0, sizeof oa);
    if (open_adapter && wait_vblank && GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO *)&mi)) dc = CreateDCA(NULL, mi.szDevice, NULL, NULL);
    oa.hDc = dc;
    if (!dc || open_adapter(&oa) != 0) { printf("display: no vblank source (D3DKMT): the refresh clock stays our own 60 Hz clock, on-screen times are not measured%c", 10); InterlockedExchange(&g_vb_state, -1); return 0; }
    wv.hAdapter = oa.hAdapter; wv.hDevice = 0; wv.VidPnSourceId = oa.VidPnSourceId;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    InterlockedExchange(&g_vb_state, 1);
    LARGE_INTEGER now;
    LONGLONG prev = 0, first[24];
    int nfirst = 0, fails = 0;
    for (;;) {
        LONG st = wait_vblank(&wv);
        QueryPerformanceCounter(&now);
        if (st != 0) { if (++fails == 20) { printf("display: the vblank wait fails (status 0x%lx): display lock off%c", (unsigned long)st, 10); InterlockedExchange(&g_vb_state, -1); return 0; } Sleep(50); prev = 0; continue; }
        fails = 0;
        LONGLONG dt = prev ? now.QuadPart - prev : 0;
        /* A second return for the same vblank is not a refresh (2026-10-25, the OpenGL swap interval 1: two returns 0.04 -
         * 0.07 ms apart): counted, it measured a 60 Hz monitor as 120 Hz and each one was a tick (the game ran at 42 fps). */
        if (prev && dt < (g_vb_period > 0 ? (LONGLONG)(g_vb_period * 0.25) : g_freq.QuadPart / 500)) {
            static LONG said;
            if (!InterlockedExchange(&said, 1)) { printf("display: a vblank event %.3f ms after the one before is ignored (and every such one)%c", (double)dt * 1000.0 / (double)g_freq.QuadPart, 10); }
            continue;
        }
        prev = now.QuadPart;
        LONG64 steps = 1;
        if (g_vb_state == 1) {                                         /* the first 24 intervals: their median is the refresh period */
            if (dt > 0 && dt < g_freq.QuadPart / 20) first[nfirst++] = dt;
            if (nfirst == 24) {
                for (int i = 1; i < 24; i++) { LONGLONG v = first[i]; int k = i; while (k > 0 && first[k - 1] > v) { first[k] = first[k - 1]; k--; } first[k] = v; }
                AcquireSRWLockExclusive(&g_vb_lock);
                g_vb_period = (double)(first[11] + first[12]) / 2.0;
                ReleaseSRWLockExclusive(&g_vb_lock);
                printf("display: monitor %s: refresh measured %.3f Hz (median of 24 vblank intervals: %.3f ms, shortest %.3f, longest %.3f)%c", mi.szDevice, (double)g_freq.QuadPart / g_vb_period,
                       g_vb_period * 1000.0 / (double)g_freq.QuadPart, (double)first[0] * 1000.0 / (double)g_freq.QuadPart, (double)first[23] * 1000.0 / (double)g_freq.QuadPart, 10);
                InterlockedExchange(&g_vb_state, 2);
            }
        } else if (dt > 0) {
            double r = (double)dt / g_vb_period;
            steps = (LONG64)(r + 0.5);
            if (steps < 1) steps = 1;
            if (steps <= 4 && r / (double)steps > 0.9 && r / (double)steps < 1.1) g_vb_period += ((double)dt / (double)steps - g_vb_period) * 0.01;      /* slow average: the long-run period */
        }
        AcquireSRWLockExclusive(&g_vb_lock);
        g_vb_count += steps;
        g_vb_qpc = now.QuadPart;
        ReleaseSRWLockExclusive(&g_vb_lock);
        WakeAllConditionVariable(&g_vb_cv);
    }
}
#endif
static void pace_options(void) {
    if (g_pace_want >= 0) return;
    const char *e = getenv("SSBU_PACE"), *h = getenv("SSBU_PACE_HOLD"), *p = getenv("SSBU_PACE_PHASE");
    int opt = gfx_options()->present;                                  /* 0 clock, 1 on (fifo), 2 off, 3 mailbox, 4 display, 5 auto */
    g_pace_want = e ? !_stricmp(e, "display") || !_stricmp(e, "auto") : opt == 4 || opt == 5;
    g_hold_on = h ? atoi(h) != 0 : opt != 1 && opt != 2;               /* fifo: the swapchain holds the presents itself; off: at once, by request */
    { int latency_mode(void); if (!h && latency_mode() >= 2) g_hold_on = 0; }      /* L1: latency = lowest: the frame goes to the window as soon as it is ready */
    if (p && atof(p) > 0.02 && atof(p) < 0.98) g_lock_phase = atof(p);
    printf("display: pacing: %s; a finished frame is presented %s%c", g_pace_want ? "locked to the display when its refresh rate is k x 60 Hz within 1.1 % (vsync = display / auto)" : "our own 60.000 Hz clock (vsync = clock)",
           g_hold_on ? "at the next tick (steady cadence)" : "as soon as it is ready", 10);
}
/* vsync thread, once the refresh is measured: lock or not */
static int pace_try_lock(void) {
    double hz = (double)g_freq.QuadPart / g_vb_period;
    int k = (int)(hz / 60.0 + 0.5);
    double per = k ? hz / k : 0, off = k ? (per - 60.0) / 60.0 : 1;
    if (k >= 1 && k <= 8 && off > -0.011 && off < 0.011) {
        printf("display: DISPLAY LOCK: %.3f Hz = %d refresh%s per game frame: the game runs at %.3f frames per second (%+.2f %% against 60), tick %.0f %% of a refresh after every %s vblank%c", hz, k, k > 1 ? "es" : "", per, off * 100.0,
               g_lock_phase * 100.0, k == 1 ? "" : k == 2 ? "second" : "k-th", 10);
        return k;
    }
    printf("display: %.3f Hz is not k x 60 Hz within 1.1 %%: our own 60.000 Hz clock paces the game (a frame is on screen %.2f refreshes on average: they cannot all be equal)%c", hz, hz / 60.0, 10);
    return 0;
}

/* TEST ONLY, SSBU_SPEED=<n> (debug_mode.c, tests\DEBUG_MODE.md): 2, 4 (any number > 0) = the refresh clock runs n times
 * faster; 0 = uncapped. Not set or 1 = nothing changes. In this mode there is no free-running vsync thread: the refresh is
 * given when the frame the game queued is due, ONE refresh per queued frame (interval 1), the event is signalled and the
 * wait goes on until the game's VSync Count Thread has counted it (window object + 0x138), so the game steps one frame per
 * presented frame whatever the host speed; the wall-clock gap between refreshes is 1/60/n s (none when uncapped). */
static double g_speed = 1.0;
static int g_lockstep;
static volatile uint64_t g_wnd_count_addr;           /* guest address of the window object's refresh count, once known */
static void speed_init(void) {
    const char *e = getenv("SSBU_SPEED");
    if (!e || !e[0]) return;
    g_speed = atof(e);
    if (g_speed < 0) g_speed = 0;
    g_lockstep = g_speed != 1.0;
    if (g_lockstep) printf("display: SSBU_SPEED=%s: %s, one refresh per presented frame\n", e, g_speed > 0 ? "paced at n x 60 Hz" : "uncapped");
}

int oe_exit_request(void);                            /* nnmisc.c */
/* PR #24 (v1Ta44): the game's picture is 16:9 (its window texture is 1920x1080). The renderers place it with display_fit;
 * the windowed frame keeps that shape while it is dragged (WM_SIZING), and F11 switches between the window and a
 * borderless window over the whole monitor it is on. */
#define ASPECT_W 16
#define ASPECT_H 9
static double win_aspect(void) {                      /* the window's shape: 16:9, or wider with graphics.ini widescreen (nvn_gl.c wide_k) */
    const GfxOptions *go = gfx_options();
    double a = gfx_widescreen();
    if (a < 0) a = (double)go->width / (double)go->height;
    return a > (double)ASPECT_W / ASPECT_H * 1.01 ? a : (double)ASPECT_W / ASPECT_H;
}
static volatile LONG g_pic[4];                    /* where the picture was last placed in the window (display_mouse) */
void display_fit(int sw, int sh, int dw, int dh, int out[4]) {      /* the largest rectangle of sw:sh in dw x dh, centred: x, y, w, h */
    out[0] = 0; out[1] = 0; out[2] = dw; out[3] = dh;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if ((long long)dw * sh > (long long)dh * sw) { out[2] = (int)((long long)dh * sw / sh); out[0] = (dw - out[2]) / 2; }      /* wider: bars left and right */
    else if ((long long)dw * sh < (long long)dh * sw) { out[3] = (int)((long long)dw * sh / sw); out[1] = (dh - out[3]) / 2; }  /* taller: bars above and below */
    if (dw - out[2] <= 2 && dh - out[3] <= 2) { out[0] = 0; out[1] = 0; out[2] = dw; out[3] = dh; }   /* within 2 px of 16:9 (1366x768): fill, no bars */
    for (int i = 0; i < 4; i++) InterlockedExchange(&g_pic[i], out[i]);
    {   static int last[4] = {-1, -1, -1, -1}, said;
        if (memcmp(last, out, sizeof last) && said++ < 20) { memcpy(last, out, sizeof last); printf("display: picture %dx%d at %d,%d in %dx%d\n", out[2], out[3], out[0], out[1], dw, dh); fflush(stdout); }
    }
}
#ifdef _WIN32
static void sizing_keep_aspect(HWND h, WPARAM edge, RECT *r) {
    RECT frame = {0, 0, 0, 0};
    AdjustWindowRectEx(&frame, (DWORD)GetWindowLongPtrA(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrA(h, GWL_EXSTYLE));
    int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    int cw = r->right - r->left - fw, ch = r->bottom - r->top - fh;
    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) cw = (int)(ch * win_aspect() + 0.5);      /* the edge dragged decides; corners follow the width */
    else ch = (int)(cw / win_aspect() + 0.5);
    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM || edge == WMSZ_RIGHT || edge == WMSZ_LEFT) {
        if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) r->right = r->left + cw + fw;
        else r->bottom = r->top + ch + fh;
    } else {
        if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) r->top = r->bottom - ch - fh; else r->bottom = r->top + ch + fh;
    }
}
/* The process is per-monitor DPI aware (window_thread): window sizes are real pixels. A windowed size from graphics.ini or
 * the settings menu means what it meant before (Windows scaled it): times the DPI scale, kept inside the monitor's work area. */
static void windowed_px(HMONITOR mon, int *cw, int *ch) {
    HDC dc = GetDC(NULL);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    if (dpi > 96) { *cw = MulDiv(*cw, dpi, 96); *ch = MulDiv(*ch, dpi, 96); }
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(mon, &mi)) return;
    int ww = (mi.rcWork.right - mi.rcWork.left) * 9 / 10, wh = (mi.rcWork.bottom - mi.rcWork.top) * 9 / 10;
    if (*cw > ww) { *ch = MulDiv(*ch, ww, *cw); *cw = ww; }
    if (*ch > wh) { *cw = MulDiv(*cw, wh, *ch); *ch = wh; }
}
static int g_full;                                    /* F11 state: 1 = borderless over the monitor */
static volatile LONG g_topmost;                       /* U1: window = fullscreen (borderless and topmost) */
static WINDOWPLACEMENT g_windowed;
static int g_have_windowed;
static void toggle_fullscreen(HWND h) {
    if (!g_full) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
        g_windowed.length = sizeof g_windowed;
        g_have_windowed = GetWindowPlacement(h, &g_windowed);
        SetWindowLongPtrA(h, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_VISIBLE));
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, (LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_VISIBLE));
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);      /* window = fullscreen in graphics.ini made it topmost */
        if (g_have_windowed) SetWindowPlacement(h, &g_windowed);
        else {                                        /* started borderless / fullscreen: the configured client size, centred */
            const GfxOptions *go = gfx_options();
            int cw = go->width, ch = (int)(go->width / win_aspect() + 0.5);
            windowed_px(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &cw, &ch);
            RECT r = {0, 0, cw, ch};
            MONITORINFO mi;
            memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
            GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            int w = r.right - r.left, wh = r.bottom - r.top;
            SetWindowPos(h, NULL, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2, w, wh,
                         SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
    }
    g_full = !g_full;
    if (!g_full) g_topmost = 0;                      /* U1: back to the window = not topmost (set above) */
    printf("display: F11: %s\n", g_full ? "borderless over the monitor" : "window"); fflush(stdout);
}
/* U1, the settings menu (menu.cpp, render thread) asks; the window thread does it (WM_APP + 0x51 / 0x52 posted to the window).
 * mode: 0 windowed, 1 borderless over the monitor, 2 the same and topmost (graphics.ini window = windowed | borderless | fullscreen). */
static void window_mode_set(HWND h, int mode) {
    if ((mode != 0) != (g_full != 0)) toggle_fullscreen(h);
    SetWindowPos(h, mode == 2 ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
    InterlockedExchange(&g_topmost, mode == 2);
    printf("display: window mode %s (settings menu)\n", mode == 2 ? "fullscreen (borderless, topmost)" : mode ? "borderless" : "windowed"); fflush(stdout);
}
static void window_size_set(HWND h, int cw, int ch) {
    if (g_full) { g_have_windowed = 0; printf("display: window size %dx%d kept for the windowed mode (settings menu)\n", cw, ch); fflush(stdout); return; }
    windowed_px(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &cw, &ch);
    RECT r = {0, 0, cw, ch}, cur;
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrA(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrA(h, GWL_EXSTYLE));
    GetWindowRect(h, &cur);
    SetWindowPos(h, NULL, cur.left, cur.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOOWNERZORDER);
    printf("display: window client %dx%d (settings menu)\n", cw, ch); fflush(stdout);
}
static HWND g_hwnd_menu;                               /* g_hwnd, for the functions below (set by window_thread) */
void display_set_window_mode(int mode) { if (g_hwnd_menu) PostMessageA(g_hwnd_menu, WM_APP + 0x51, (WPARAM)mode, 0); }
void display_set_client_size(int w, int h) { if (g_hwnd_menu) PostMessageA(g_hwnd_menu, WM_APP + 0x52, (WPARAM)w, (LPARAM)h); }
int display_window_mode(void) { return g_full ? (g_topmost ? 2 : 1) : 0; }
#include "loading_anim.c"                            /* U3: the loading animation until the game's first present */
/* closing the window: a game that does not end within 4 s (an exit handler or a thread that never returns: testers saw the
 * window freeze after the X while closing the console window worked) is ended here, its log flushed first */
static DWORD WINAPI close_watchdog(LPVOID arg) {
    (void)arg;
    Sleep(4000);
    printf("\nwindow closed but the game did not end within 4 s: ended now\n");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
/* The mouse for the game's hand pointer (nnhid.c mouse_poll, tomo.c pointer_hook): where the cursor is in the game's
 * 1280x720 screen (the 16:9 picture, centred in a wider window) and the buttons (bit 0 left, bit 1 right), while this window
 * is the foreground one and the cursor is over the picture. The arrow is hidden over the window while the hand follows it. */
static volatile LONG g_mouse_hand_tick, g_mouse_wheel;
static char g_title_note[72];                       /* after the frame counter in the title (net_relay.c: the Local Play room code) */
void display_room_note(const char *s) { snprintf(g_title_note, sizeof g_title_note, "%s", s ? s : ""); }
void display_mouse_hand(void) { InterlockedExchange(&g_mouse_hand_tick, (LONG)GetTickCount()); }
int display_mouse_wheel(void) { return (int)InterlockedExchange(&g_mouse_wheel, 0); }      /* notches since the last call, + = away from the user */
int display_mouse(float *x, float *y, int *buttons) {
    POINT p; RECT rc; int fit[4];
    HWND h = g_hwnd;
    if (!h || GetForegroundWindow() != h || !GetCursorPos(&p) || !ScreenToClient(h, &p) || !GetClientRect(h, &rc)) return 0;
    fit[0] = g_pic[0]; fit[1] = g_pic[1]; fit[2] = g_pic[2]; fit[3] = g_pic[3];      /* the picture as presented: 16:9, or wider (widescreen),
                                                                                     * maybe with bars; the game's 2D stays 16:9 in its middle */
    if (fit[2] <= 0 || fit[3] <= 0 || fit[0] + fit[2] > rc.right + 2 || fit[1] + fit[3] > rc.bottom + 2) {      /* (none presented yet in this size) */
        fit[0] = 0; fit[1] = 0; fit[2] = rc.right; fit[3] = rc.bottom;
        if ((long long)rc.right * ASPECT_H > (long long)rc.bottom * ASPECT_W) { fit[2] = rc.bottom * ASPECT_W / ASPECT_H; fit[0] = (rc.right - fit[2]) / 2; }
        else if ((long long)rc.right * ASPECT_H < (long long)rc.bottom * ASPECT_W) { fit[3] = rc.right * ASPECT_H / ASPECT_W; fit[1] = (rc.bottom - fit[3]) / 2; }
    }
    if (fit[2] <= 0 || fit[3] <= 0 || p.x < fit[0] || p.y < fit[1] || p.x >= fit[0] + fit[2] || p.y >= fit[1] + fit[3]) return 0;
    float uw = (float)fit[3] * 16.0f / 9.0f, ux = (float)fit[0] + ((float)fit[2] - uw) * 0.5f;      /* the 16:9 middle: 0..1280 (the wide sides beyond) */
    *x = ((float)p.x - ux) * 1280.0f / uw;
    *y = (float)(p.y - fit[1]) * 720.0f / (float)fit[3];
    *buttons = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0) | ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 2 : 0) | ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 4 : 0);
    return 1;
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (la_wndproc(h, m, w)) return 0;
    /* the pointer over the picture: hidden while the mouse moves the hand (but not over the name keyboard, which hides the
     * hand), and after 2 s without moving (mouse controls are off by default, so it stayed in view, 2026-10-30) unless PC
     * Settings is open; a timer looks again twice a second */
    {   static DWORD moved; int menu_is_open(void); int osk_blocks_input(void);
        if (m == WM_MOUSEMOVE) moved = GetTickCount();
        if (m == WM_TIMER && w == 0x7A11) { POINT p; if (GetCursorPos(&p) && WindowFromPoint(p) == h) SendMessageA(h, WM_SETCURSOR, (WPARAM)h, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)); return 0; }
        if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT && ((GetTickCount() - (DWORD)g_mouse_hand_tick < 300 && !osk_blocks_input()) || (GetTickCount() - moved > 2000 && !menu_is_open()))) { SetCursor(NULL); return TRUE; }
    }
    if (m == WM_CLOSE) {                             /* the exit request: nn::oe decides (nnmisc.c oe_exit_request) whether the game ends now or after its exit-request handling section */
        { static LONG once; if (!InterlockedExchange(&once, 1)) CloseHandle(CreateThread(NULL, 0, close_watchdog, NULL, 0, NULL)); }
        printf("\nwindow closed by the user after %lld frames\n", (long long)g_frames);
        { extern volatile LONG g_display_close; InterlockedExchange(&g_display_close, 1); }      /* session 14: a hold of ours (the first-run shader preparation) ends at once */
        if (!oe_exit_request()) report_and_exit(0);
        return 0;
    }
    if (m == WM_ERASEBKGND) {
        /* 2026-10-16: black, not white, until the game's first frame reaches the window (the boot and the shader loading);
         * from then on the swapchain owns every pixel and nothing is erased (no flicker) */
        if (!g_frames) { RECT rc; GetClientRect(h, &rc); FillRect((HDC)w, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
        return 1;
    }
    if (m == WM_SIZING && !g_full) { sizing_keep_aspect(h, w, (RECT *)l); return TRUE; }
    if (m == 0x02E0 /* WM_DPICHANGED */ && !g_full) {      /* moved to a monitor of another scale: the size Windows proposes */
        const RECT *r = (const RECT *)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30))) { toggle_fullscreen(h); return 0; }      /* bit 30: auto-repeat of a held key */
    if (m == WM_KEYDOWN && w == VK_F12 && !(l & (1 << 30))) { void nvn_front_flicker_sample(void); nvn_front_flicker_sample(); return 0; }      /* a flicker sample (nvn_gl.c) */
    {   int menu_is_open(void);
        if (m == WM_KEYDOWN && w == VK_TAB && !(l & (1 << 30)) && !menu_is_open()) { display_ff_key(1); return 0; }      /* fast-forward while held */
        if (m == WM_KEYUP && w == VK_TAB) { display_ff_key(0); return 0; }
        if (m == WM_KILLFOCUS) display_ff_key(0); }
    if (m == WM_APP + 0x51) { window_mode_set(h, (int)w); return 0; }                                 /* U1: the settings menu (menu.cpp) */
    if (m == WM_APP + 0x52) { window_size_set(h, (int)w, (int)l); return 0; }
    {   int menu_wndproc(void *hwnd, unsigned msg, uintptr_t w, intptr_t l);      /* U1 (menu.cpp): F1 opens / closes; the menu takes the mouse and keys while open */
        if (menu_wndproc(h, m, (uintptr_t)w, (intptr_t)l)) return 0; }
    {   /* the on-screen keyboard (osk.c) takes the PC keyboard while it is open; this window is an ANSI one: its WM_CHAR
         * carries a character of the system code page, made UTF-16 here */
        int osk_key(UINT msg, WPARAM w);
        if (m == WM_CHAR) {
            char a = (char)w;
            wchar_t u = 0;
            if (w < 0x80 || MultiByteToWideChar(CP_ACP, 0, &a, 1, &u, 1) != 1) u = (wchar_t)w;
            if (osk_key(WM_CHAR, (WPARAM)u)) return 0;
        } else if (m == WM_KEYDOWN && osk_key(WM_KEYDOWN, w)) return 0;
    }
    if (m == WM_MOUSEWHEEL) { static int rest; rest += (short)HIWORD(w); InterlockedExchangeAdd(&g_mouse_wheel, rest / WHEEL_DELTA); rest %= WHEEL_DELTA; return 0; }      /* (nnhid.c mouse_poll) */
    return DefWindowProcA(h, m, w, l);
}

static char g_title[96];

static DWORD WINAPI window_thread(LPVOID arg) {
    (void)arg;
    {   /* Per-monitor DPI aware before the first window. Unaware, a 4K monitor at 300 % looked 1280x720 to us: the borderless
         * window got that client size and the 4K picture reached the screen shrunk to 1280x720 and blown up 3x (blocky). */
        typedef BOOL (WINAPI *SetCtx)(HANDLE);
        SetCtx set = (SetCtx)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
        if (!set || !set((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
    }
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ssbu_native_window";
    wc.style = CS_OWNDC;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    {   /* the project's logo (media/opensmash.ico, copied beside the exe by scripts/setup.py): the window's and taskbar's icon */
        char ico[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, ico, MAX_PATH);
        while (n && ico[n - 1] != '\\' && ico[n - 1] != '/') n--;
        if (n && n + 16 < MAX_PATH) {
            strcpy(ico + n, "opensmash.ico");
            wc.hIcon = (HICON)LoadImageA(NULL, ico, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
            g_icon_small = (HICON)LoadImageA(NULL, ico, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
            g_icon_big = (HICON)LoadImageA(NULL, ico, IMAGE_ICON, 256, 256, LR_LOADFROMFILE);
        }
    }
    RegisterClassA(&wc);
    /* Track O: window mode and size from config\graphics.ini (gfx_options.c). borderless / fullscreen: a frameless window
     * over the primary monitor (fullscreen: also topmost). */
    const GfxOptions *go = gfx_options();
    DWORD style = go->window_mode ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    g_full = go->window_mode != 0;
    int cw = go->width, ch = go->height, px = CW_USEDEFAULT, py = CW_USEDEFAULT;
    if (go->window_mode) {
        MONITORINFO mi;
        POINT origin = {0, 0};
        memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi)) { cw = mi.rcMonitor.right - mi.rcMonitor.left; ch = mi.rcMonitor.bottom - mi.rcMonitor.top; px = mi.rcMonitor.left; py = mi.rcMonitor.top; }
    } else {
        POINT origin = {0, 0};
        windowed_px(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &cw, &ch);
    }
    RECT r = {0, 0, cw, ch};
    AdjustWindowRect(&r, style, FALSE);
    printf("display: window %s, client %dx%d (graphics.ini)\n", go->window_mode == 2 ? "fullscreen (borderless, topmost)" : go->window_mode ? "borderless" : "windowed", cw, ch);
    /* Only the copies a person plays (ssbu_native.exe, ssbu_user.exe, or SSBU_SHOW=1) get a window on screen;
     * test copies started under any other name keep theirs off-screen and out of the taskbar, so automated
     * runs do not cover the desktop. SSBU_TITLE names the window. */
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe, *show = getenv("SSBU_SHOW");
    int visible = show ? atoi(show) != 0 : (!_stricmp(base, "tomodachi_native.exe") || !_stricmp(base, "ssbu_native.exe") || !_stricmp(base, "ssbu_user.exe") || !_stricmp(base, "ssbu_native_rb.exe"));      /* (_rb: the rollback pair a player starts) */
    snprintf(g_title, sizeof g_title, "%s", getenv("SSBU_TITLE") ? getenv("SSBU_TITLE") : "Tomodachi Life: Living the Dream (native)");
    if (visible) {
        g_hwnd = CreateWindowExA(go->window_mode == 2 ? WS_EX_TOPMOST : 0, wc.lpszClassName, g_title, style | WS_VISIBLE, px, py, r.right - r.left,
                                 r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
        if (g_hwnd && g_icon_small) SendMessageA(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_icon_small);
        if (g_hwnd && g_icon_big) SendMessageA(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_icon_big);      /* the 256 px image: the taskbar / Alt+Tab scale it down sharp (the class icon is only 32 px: blurry on a high-DPI screen) */
    } else {
        g_hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, g_title, style, -32000, -32000,
                                 r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }
    g_hwnd_menu = g_hwnd; g_topmost = go->window_mode == 2;      /* U1 (settings menu) */
    la_start(g_hwnd, visible);                                   /* U3: the loading animation (loading_anim.c) */
    SetTimer(g_hwnd, 0x7A11, 500, NULL);                         /* the idle pointer (wndproc) */
    InterlockedExchange(&g_ready, 1);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

#else
/* Linux build: an SDL3 window (X11 or Wayland) made and serviced by its own thread, as the Win32 one is. Vulkan draws
 * into it (nvn_vk.c makes the surface with SDL). The keyboard state that nnhid.c reads (GetAsyncKeyState, plat_posix.c)
 * is SDL's, which this thread's event pumping keeps current; nnpad_sdl.c flushes the queue on its own thread, so the
 * close request is caught by an event watch rather than read from the queue. */
#include <SDL3/SDL.h>
/* U3: no loading animation in the SDL window yet (the Windows one is loading_anim.c) */
static volatile LONG g_la_first = 1;
static void la_stop(void) {}
static void la_note(const char *note) { (void)note; }
int oe_exit_request(void);                            /* nnmisc.c */
static char g_title[96];
static bool SDLCALL close_watch(void *user, SDL_Event *e) {
    (void)user;
    if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || e->type == SDL_EVENT_QUIT) {
        static volatile LONG once;
        if (InterlockedExchange(&once, 1)) return true;
        printf("\nwindow closed by the user after %lld frames\n", (long long)g_frames);
        if (!oe_exit_request()) report_and_exit(0);
    }
    return true;
}
/* U2, the settings menu (menu.cpp, render thread) asks; the window thread does it (window_mode_service), as WM_APP + 0x51 /
 * 0x52 on Windows. mode: 0 windowed, 1 borderless over the screen, 2 the desktop's fullscreen (graphics.ini window). */
static volatile LONG g_mode_req = -1, g_size_req, g_size_w, g_size_h, g_mode_now;
static int g_win_w, g_win_h;                          /* the windowed size, for the way back from borderless / fullscreen */
void display_set_window_mode(int mode) { InterlockedExchange(&g_mode_req, mode < 0 ? 0 : mode > 2 ? 2 : mode); }
void display_set_client_size(int w, int h) { InterlockedExchange(&g_size_w, w); InterlockedExchange(&g_size_h, h); InterlockedExchange(&g_size_req, 1); }
int display_window_mode(void) { return g_mode_now; }
static void window_mode_service(SDL_Window *w) {
    LONG m = InterlockedExchange(&g_mode_req, -1);
    if (m >= 0) {
        if (m == 2) { SDL_SetWindowFullscreenMode(w, NULL); SDL_SetWindowFullscreen(w, true); }      /* NULL: the desktop's mode, no mode change */
        else {
            SDL_SetWindowFullscreen(w, false);
            SDL_SetWindowBordered(w, m == 0);
            SDL_Rect r;
            if (m == 1 && SDL_GetDisplayBounds(SDL_GetDisplayForWindow(w), &r)) { SDL_SetWindowPosition(w, r.x, r.y); SDL_SetWindowSize(w, r.w, r.h); }
            else if (m == 0) { SDL_SetWindowSize(w, g_win_w, g_win_h); SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED); }
        }
        InterlockedExchange(&g_mode_now, m);
        printf("display: window mode %s (settings menu)\n", m == 2 ? "fullscreen" : m ? "borderless" : "windowed"); fflush(stdout);
    }
    if (InterlockedExchange(&g_size_req, 0)) {
        g_win_w = g_size_w; g_win_h = g_size_h;
        if (g_mode_now) printf("display: window size %dx%d kept for the windowed mode (settings menu)\n", g_win_w, g_win_h);
        else { SDL_SetWindowSize(w, g_win_w, g_win_h); printf("display: window client %dx%d (settings menu)\n", g_win_w, g_win_h); }
        fflush(stdout);
    }
}
void menu_sdl_attach(void *window); void menu_sdl_service(void);      /* menu.cpp (or menu_stub.c): the settings menu's input */
static DWORD WINAPI window_thread(LPVOID arg) {
    (void)arg;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { printf("display: SDL video cannot start: %s\n", SDL_GetError()); fflush(stdout); ExitProcess(5); }
    const GfxOptions *go = gfx_options();
    int cw = go->width, ch = go->height;
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (go->window_mode) {
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (dm) { cw = dm->w; ch = dm->h; }
        flags |= go->window_mode == 2 ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_BORDERLESS;
    }
    printf("display: window %s, client %dx%d (graphics.ini), SDL video driver %s\n", go->window_mode == 2 ? "fullscreen" : go->window_mode ? "borderless" : "windowed", cw, ch, SDL_GetCurrentVideoDriver());
    snprintf(g_title, sizeof g_title, "%s", getenv("SSBU_TITLE") ? getenv("SSBU_TITLE") : "Tomodachi Life: Living the Dream (native)");
    SDL_Window *w = SDL_CreateWindow(g_title, cw, ch, flags);
    if (!w) { printf("display: SDL_CreateWindow failed: %s\n", SDL_GetError()); fflush(stdout); ExitProcess(5); }
    SDL_AddEventWatch(close_watch, NULL);
    g_win_w = go->width; g_win_h = go->height; g_mode_now = go->window_mode;
    menu_sdl_attach(w);
    g_hwnd = (HWND)w;
    plat_game_window = (HWND)w;
    InterlockedExchange(&g_ready, 1);
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 8)) { while (SDL_PollEvent(&e)) {} }       /* the watch saw them; the keyboard state is current */
        plat_window_service();
        window_mode_service(w);
        menu_sdl_service();
    }
    return 0;
}
#endif
/* Signals the guest's vsync event 60 times per second. */
static DWORD WINAPI vsync_thread(LPVOID arg) {
    (void)arg;
    LARGE_INTEGER next;
    QueryPerformanceCounter(&next);
    if (!old_pacing()) {
        LARGE_INTEGER t0 = next, now;
        LONG64 n = 0;
        HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        int lock_tried = 0, ff_was = 1;
        LONG64 vb_last = 0, relock_at = 0;      /* the lock was dropped (no vblank for 100 ms): tried again from this time on */
        pace_options();
        display_perf_host_thread();
        for (;;) {
            if (g_pace_want && !lock_tried && g_vb_state == 2) {                  /* session 14: the refresh is measured: lock to it or not */
                lock_tried = 1;
                int k = pace_try_lock();
                if (k) { vb_last = g_vb_count; InterlockedExchange(&g_lock_k, k); }
            }
            if (g_pace_want && !lock_tried && g_vb_state == -1) { lock_tried = 1; printf("display: no vblank source: our own 60 Hz clock%c", 10); }
            if (g_pace_want && !g_lock_k && relock_at && g_vb_state == 2) {
                /* 2026-10-30: the vblanks stop for a moment when the window takes the screen (the first present on a
                 * 240 Hz monitor, 2026-10-29 log): the lock was dropped and never taken again, so the whole session ran
                 * on our own clock. Now: once the vblanks are back, the lock is taken again. */
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= relock_at) {
                    LONG64 vc, vq; double period;
                    AcquireSRWLockShared(&g_vb_lock); vc = g_vb_count; vq = g_vb_qpc; period = g_vb_period; ReleaseSRWLockShared(&g_vb_lock);
                    if ((double)(now.QuadPart - vq) < period * 3.0) {
                        int k = pace_try_lock();
                        if (k) { vb_last = vc; InterlockedExchange(&g_lock_k, k); printf("display: the vblanks are back: display lock on again%c", 10); }
                        relock_at = 0;
                    } else relock_at = now.QuadPart + 2 * g_freq.QuadPart;
                }
            }
            {   int ff = g_ff;                                                     /* fast-forward: our own clock, ff x 60 Hz */
                if (ff != ff_was) {
                    QueryPerformanceCounter(&now);
                    if (ff > 1 && g_lock_k) { InterlockedExchange(&g_lock_k, 0); }
                    t0.QuadPart = now.QuadPart - n * g_freq.QuadPart / (60 * ff);
                    if (ff == 1) relock_at = now.QuadPart + g_freq.QuadPart / 2;
                    ff_was = ff;
                }
            }
            if (g_lock_k) {
                LONG64 k = g_lock_k, vb_target = vb_last + k, vc, vq;
                int timeouts = 0;
                double period;
                AcquireSRWLockExclusive(&g_vb_lock);
                while (g_vb_count < vb_target && timeouts < 5) if (!SleepConditionVariableSRW(&g_vb_cv, &g_vb_lock, 20, 0)) timeouts++;
                vc = g_vb_count; vq = g_vb_qpc; period = g_vb_period;
                ReleaseSRWLockExclusive(&g_vb_lock);
                if (vc < vb_target) {                                              /* no vblank for 100 ms (display off, mode change): our clock goes on from here */
                    printf("display: no vblank for 100 ms: display lock off, our own 60 Hz clock goes on (the lock is tried again once the vblanks are back)%c", 10);
                    InterlockedExchange(&g_lock_k, 0);
                    QueryPerformanceCounter(&now);
                    t0.QuadPart = now.QuadPart - n * g_freq.QuadPart / 60;
                    relock_at = now.QuadPart + 2 * g_freq.QuadPart;
                    continue;
                }
                LONG64 extra = (vc - vb_target) / k;                               /* whole game frames this thread slept through */
                vb_last = vb_target + extra * k;
                precise_wait_until(vq - (LONGLONG)((double)(vc - vb_last) * period) + (LONGLONG)(period * g_lock_phase), timer);
                if (extra) InterlockedExchangeAdd64(&g_vsync_late, extra);
                n += 1 + extra;
                t0.QuadPart = 0;
            } else {
            LONGLONG hz = 60 * (LONGLONG)ff_was;
            if (!t0.QuadPart) { QueryPerformanceCounter(&now); t0.QuadPart = now.QuadPart - n * g_freq.QuadPart / hz; }
            LONGLONG target = t0.QuadPart + (n + 1) * g_freq.QuadPart / hz;       /* no accumulated rounding */
            precise_wait_until(target, timer);
            QueryPerformanceCounter(&now);
            LONG64 passed = (now.QuadPart - t0.QuadPart) * hz / g_freq.QuadPart;   /* >= n + 1; more when this thread was kept off the CPU */
            if (passed < n + 1) passed = n + 1;
            if (passed > n + 1) InterlockedExchangeAdd64(&g_vsync_late, passed - (n + 1));
            n = passed;
            }
            if (g_vsync_event) { os_signal_event(g_vsync_event); InterlockedIncrement64(&g_vsync_signals); }
            AcquireSRWLockExclusive(&g_vsync_lock);
            { LARGE_INTEGER tq; QueryPerformanceCounter(&tq); g_tick_at[n & 15] = tq.QuadPart; }      /* L1: when this tick was given */
            InterlockedExchange64(&g_vsync_n, n);
            ReleaseSRWLockExclusive(&g_vsync_lock);
            { LONG64 dt = g_defer_target; if (dt && n >= dt) nvn_front_post_present_flush(); }      /* session 14: the frame kept for this tick goes to the window now */
            WakeAllConditionVariable(&g_vsync_cv);
            vsync_test_key();
        }
    }
    for (;;) {
        next.QuadPart += g_freq.QuadPart / 60;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG ms = (next.QuadPart - now.QuadPart) * 1000 / g_freq.QuadPart;
        if (ms > 0) Sleep((DWORD)ms); else next = now;
        if (g_vsync_event) { os_signal_event(g_vsync_event); InterlockedIncrement64(&g_vsync_signals); }
        vsync_test_key();
    }
}
static void vsync_test_key(void) {
#ifndef _WIN32
    {   static int said; if (!said && getenv("SSBU_AUTOKEY")) { said = 1; printf("display: SSBU_AUTOKEY is not available in the Linux build (use SSBU_SEQ)\n"); } }
    return;
#endif
    {
        /* Test aid: SSBU_AUTOKEY=<letter>,<first second>,<every n seconds> presses a key now and then, so a run without a person can get
         * past "press any button". Only while this window is the foreground window, so the key never reaches another program. */
        static int key = -1, first, every, tick, down;
        if (key == -1) {
            const char *e = getenv("SSBU_AUTOKEY");
            key = 0;
            if (e && e[0]) { key = e[0]; first = 30; every = 5; sscanf(e + 1, ",%d,%d", &first, &every); if (every < 1) every = 1; }
        }
        tick++;
        if (key && down && tick - down >= 8) { keybd_event((BYTE)key, 0, KEYEVENTF_KEYUP, 0); down = 0; }
        else if (key && !down && tick >= first * 60 && (tick - first * 60) % (every * 60) == 0 && ((tick % 600 == 0 ? (void)printf("display: test key wanted, window foreground: %d%c", GetForegroundWindow() == g_hwnd, 10) : (void)0), 1) && (GetForegroundWindow() == g_hwnd || (SetForegroundWindow(g_hwnd), GetForegroundWindow() == g_hwnd))) { keybd_event((BYTE)key, 0, 0, 0); down = tick; printf("display: test key %c pressed at frame tick %d%c", key, tick, 10); }
    }
}

void hitch_init(void); void hitch_pacing(int on);      /* hitch.c: SSBU_HITCH */
static void pace_release_selftest(void);
void display_init(void) {
    QueryPerformanceFrequency(&g_freq);
    pace_release_selftest();
    /* P3 round 3: the console's waits with a time-out (nn::os::TimedWaitEvent, TimedReceiveMessageQueue, timed condition
     * variables: os_threads.c, SleepConditionVariableCS with whole milliseconds) end at the time asked for. At the
     * default Windows timer period they ended at the next 15.6 ms tick, so a thread that polls with a 1 ms time-out ran
     * 64 times per second: the movie player's two feed threads delivered one sample per tick and its pre-roll loop
     * [game 0x71036f27b4, 125 x 10 ms, holding the player's mutex] ran out every time (1.3 s, main thread blocked).
     * SSBU_TIMER_PERIOD=<ms> (default 1; 0 = leave the system default). */
    { const char *e = getenv("SSBU_TIMER_PERIOD"); int ms = e ? atoi(e) : 1; if (ms > 0) { MMRESULT r = timeBeginPeriod((UINT)ms); printf("display: system timer period %d ms requested (%s)%c", ms, r == TIMERR_NOERROR ? "ok" : "refused", 10); } }
    hitch_init();
    CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    while (!g_ready) Sleep(1);
    nvn_gl_start(g_hwnd);
    speed_init();
    if (!g_lockstep && !old_pacing()) CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);      /* session 14: the display's vblanks, timed */
    if (!g_lockstep) CreateThread(NULL, 0, vsync_thread, NULL, 0, NULL);
    { void nvn_vk_selftest_hook(void); nvn_vk_selftest_hook(); }      /* nvn_vk.c: SSBU_RENDERER=vk SSBU_VK_SELFTEST=<frames>, then exit (plan\phases\V1_vulkan.md, M1) */
}

/* Called after every presented frame (nvn_gl.c has already put it in the window): holds the caller to 60 Hz. */
static volatile LONG g_interval = 1;         /* nvnWindowSetPresentInterval: display refreshes per presented frame (0 = do not wait) */
int gfx_frame_rate(void);
/* frame_rate = 60, adaptive: the game steps its logic once per refresh that passed, so a frame that misses a refresh makes the
 * next one heavier (two or three steps): forced to one refresh per frame, a scene that cannot hold 60 fell to 20 fps (the
 * house close-up, 2026-10-24). Two refreshes per frame (30 fps) when, for a second, the busy time of the frames (from the end
 * of their refresh wait to their present) is over 15.5 ms on average or 6 presents came a refresh late; one again on a
 * try every 20 s. */
static volatile LONG g_adapt;                 /* 1: the interval is the game's halved and adapts (frame_rate = 60) */
static LARGE_INTEGER g_release_at;            /* end of the last refresh wait */
static volatile LONG64 g_held_qpc;            /* time the game's thread was held for a refresh since display_take_held (QPC ticks) */
/* The time the game's main thread spent held for its refresh (display_wait_refresh / the old pacing's wait) since the last
 * call, in the console's 19.2 MHz system ticks (tomo.c: the game's own frame-rate controller judges by a span that holds it). */
long long display_take_held(void) {
    LONG64 q = InterlockedExchange64(&g_held_qpc, 0);
    return g_freq.QuadPart ? (long long)((double)q * 19200000.0 / (double)g_freq.QuadPart) : 0;
}
/* THE GAME'S OWN FRAME-RATE CONTROLLER (2026-10-30). Tomodachi Life picks its present interval itself, every frame
 * (1.0.4: 0x710027f474, called from 0x710027efe4; 1.0.0: 0x710028b2b8 from 0x710028ae34): interval = clamp(int(frame time
 * x 60 + bias) + 1, minimum, maximum) (fields +0 and +4 of the controller, a singleton behind a global), fps = 60 / interval
 * (+0x50), and the game steps its logic by that. frame_rate = 60 used to halve the interval the game asked for: the game was
 * sure it ran at 30 and every movement went twice as fast (user, 2026-10-30). Now its minimum (2 on the console) is set to 1:
 * the game chooses 60 itself and steps 1/60 a frame; the 30 fps fallback below moves that minimum, never the interval. */
static uint64_t g_rate_ctl;                   /* the controller (0: not found: the interval is halved as before) */
static uint32_t g_rate_min0;                  /* its own minimum */
static int g_rate_v104;                       /* found through 1.0.4's call site (TOMO_STEPLOG knows only its addresses) */
static volatile LONG g_rate_floor = 1;        /* the minimum it has from us (frame_rate = 60) */
static void rate_floor(int n) {
    if (!g_rate_ctl) return;
    InterlockedExchange(&g_rate_floor, n);
    uint32_t mn = (uint32_t)(gfx_frame_rate() >= 60 ? n : (int)g_rate_min0);
    wr32(g_rate_ctl, mn);
    /* +0x44: the lower interval the controller tries next once its countdown (+0x4c) ends, set when it goes UP to
     * max(new - 1, minimum): raised while the minimum was 2, it stayed 2 and the game never came back to 60 (2026-10-30). */
    if (rd32(g_rate_ctl + 0x40) > mn && rd32(g_rate_ctl + 0x44) > mn) wr32(g_rate_ctl + 0x44, mn);
}
/* nvn_gl.c, at nvnWindowSetPresentInterval: lr = the game's return address. The controller's call site finds the controller. */
void display_rate_site(uint64_t lr) {
    static int tried;
    uint64_t slot = lr == 0x710027f5c4ULL ? 0x710326d880ULL : lr == 0x710028b408ULL ? 0x71032cb138ULL : 0;
    if (g_rate_ctl || tried || !slot || getenv("TOMO_INTERVAL")) return;
    tried = 1;
    if ((rd32(lr - 4) & 0xFFFFFC1Fu) != 0xD63F0000u) return;          /* BLR at the call site: the code is the one studied */
    uint64_t g = rd64(slot), ctl = g ? rd64(g) : 0;
    uint32_t mn = ctl ? rd32(ctl) : 0, mx = ctl ? rd32(ctl + 4) : 0, cur = ctl ? rd32(ctl + 0x40) : 0;
    float bias = 0; if (ctl) { uint32_t b = rd32(ctl + 8); memcpy(&bias, &b, 4); }
    printf("display: the game's frame-rate controller at %llx: interval %u..%u (now %u, %u fps), bias %.2f%c", (unsigned long long)ctl, mn, mx, cur, ctl ? rd32(ctl + 0x50) : 0, bias, 10);
    if (!ctl || mn < 1 || mn > 4 || mx < mn || mx > 8 || cur < mn || cur > mx) { printf("display: ... not the expected values: frame_rate = 60 halves the interval as before%c", 10); return; }
    g_rate_min0 = mn; g_rate_ctl = ctl; g_rate_v104 = lr == 0x710027f5c4ULL;
    rate_floor(1);
    printf("display: frame_rate %d: the game's minimum interval is now %u (it steps its logic by the interval it chooses)%c", gfx_frame_rate(), rd32(ctl), 10);
}
static void adapt_frame(LARGE_INTEGER now) {
    static double ema;
    static int over, at30, window, missed;
    static LARGE_INTEGER last;
    if (g_rate_ctl && (int)rd32(g_rate_ctl) != (gfx_frame_rate() >= 60 ? (int)g_rate_floor : (int)g_rate_min0)) rate_floor(g_rate_floor);      /* (frame_rate changed in the menu) */
    {   static LONG64 n; static int every = -1;              /* TOMO_STEPLOG=<k> (TEST): the game's own step every k frames (1.0.4: its frame object's +0x10, 60 Hz ticks a frame) */
        if (every < 0) every = getenv("TOMO_STEPLOG") ? atoi(getenv("TOMO_STEPLOG")) : 0;
        if (every > 0 && g_rate_ctl && g_rate_v104 && ++n % every == 0) { uint64_t fo = rd64(0x710390f270ULL); printf("steplog: frame %lld: interval %u, the game's step %u ticks%c", (long long)n, rd32(g_rate_ctl + 0x40), fo ? rd32(fo + 0x10) : 0, 10); }
    }
    if (!g_adapt || !g_release_at.QuadPart || g_ff > 1) return;
    double ms = (double)(now.QuadPart - g_release_at.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
    double gap = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)g_freq.QuadPart : 0;      /* since the last present */
    last = now;
    if (ms < 0 || ms > 500) return;
    ema = ema ? ema * 0.85 + ms * 0.15 : ms;
    if (g_rate_ctl) {                                  /* the game's controller is in charge: the fallback moves its minimum */
        static int over, at30, window, missed, off = -1;
        if (off < 0) off = getenv("TOMO_ADAPT") && getenv("TOMO_ADAPT")[0] == '0';      /* TEST: no fallback to 30 */
        {   static int pl = -1, n, late; static double worst;      /* TOMO_PACELOG=1 (TEST): the game's controller and the presents, per 60 frames */
            if (pl < 0) pl = getenv("TOMO_PACELOG") != NULL;
            if (pl) {
                late += gap > 16.7 * (g_interval > 0 ? g_interval : 1) + 8.3; if (gap > worst) worst = gap;
                if (++n >= 60) {
                    void tomo_pick_stats(double *avg, double *mx, double *raw); double pa, pm, pr; tomo_pick_stats(&pa, &pm, &pr);
                    printf("pacelog: floor %ld | game min %u max %u now %u next %u counters %u %u fps %u, judged frame time %.1f ms (max %.1f; %.1f with the refresh wait) | our interval %ld | late %d, worst gap %.1f ms, busy ema %.1f ms%c",
                           (long)g_rate_floor, rd32(g_rate_ctl), rd32(g_rate_ctl + 4), rd32(g_rate_ctl + 0x40), rd32(g_rate_ctl + 0x44), rd32(g_rate_ctl + 0x48), rd32(g_rate_ctl + 0x4c),
                           rd32(g_rate_ctl + 0x50), pa, pm, pr, (long)g_interval, late, worst, ema, 10);
                    n = late = 0; worst = 0;
                }
            }
        }
        if (off) return;
        /* Not before the island has been up 5 s: the loading screen, the title and the island's own load came late for the
         * loading, which 30 fps does not help, and each such drop doubled the next stay (2026-10-31: 30 fps from the loading
         * screen on, 2 min of it by the time the island was up). The game's own controller still picks 2 or 3 meanwhile. */
        { int tomo_play_started(void); if (!tomo_play_started()) { window = missed = over = 0; return; } }
        /* The stay at 30 doubles (20 s, 40 s, 80 s, at most 2 min) when 60 fails again within 10 s of a try: a scene that cannot
         * hold 60 no longer switches back and forth every 20 s (each switch shows); a minute at 60 resets it. */
        static int stay = 600, at60 = 600;              /* (the first drop, often at a load, is not a failed try) */
        if (g_rate_floor == 1) {
            double due = (double)(g_interval > 0 ? g_interval : 1) * 1000.0 / 60.0;      /* (the game may choose 2 for a while itself: not late) */
            missed += gap > due + 8.3 && gap < due + 33.4; if (++window >= 60) { if (missed >= 4) over = 60; window = missed = 0; }      /* 4 a second: steady 30 beats a 55 fps judder (the quality bar) */
            over = ema > 15.5 || over >= 60 ? over + 1 : 0;
            if (++at60 >= 3600) stay = 600;
            if (over >= 60) {
                stay = at60 < 600 ? (stay * 2 > 3600 ? 3600 : stay * 2) : 600;      /* (frames at 30 fps: 2 min at most) */
                window = missed = 0; over = at30 = at60 = 0; rate_floor(2);
                static int n; if (n++ < 20) printf("display: frame_rate 60: frames take %.1f ms: 30 fps for %d s (the game's minimum interval 2)%c", ema, stay / 30, 10);
            }
        } else if (++at30 >= stay) { over = at30 = at60 = 0; rate_floor(1); static int n; if (n++ < 20) printf("display: frame_rate 60: 60 fps again (the game's minimum interval 1)%c", 10); }
        return;
    }
    if (g_interval == 1) {
        /* also: presents a refresh late (the render thread or the GPU, which the busy time does not see): 40-50 fps on a
         * 60 Hz display judders; 6 late ones in a second -> 30. A stall of 50 ms or more is not counted: 30 fps would not
         * have hidden it (the island's autosave: 2 s of 60-100 ms frames, then 20 s at 30 for nothing, 2026-10-25). */
        missed += gap > 25.0 && gap < 50.0; if (++window >= 60) { if (missed >= 6) over = 60; window = missed = 0; }
        over = ema > 15.5 || over >= 60 ? over + 1 : 0;
        if (over >= 60) { window = missed = 0; InterlockedExchange(&g_interval, 2); over = at30 = 0; static int n; if (n++ < 20) printf("display: frame_rate 60: frames take %.1f ms: 30 fps for now%c", ema, 10); }
    } else if (g_interval == 2) {
        /* every 20 s at 30, 60 is tried again (a lighter scene, or a GPU idling at low clocks under the 30 fps cap, which
         * measures slower than it runs once asked for more: nvidia-smi 780 MHz at 30 fps, 1935 MHz under load). The busy
         * time alone does not decide: it does not see the render thread or the GPU. */
        if (++at30 >= 600) { InterlockedExchange(&g_interval, 1); over = at30 = 0; static int n; if (n++ < 20) printf("display: frame_rate 60: frames take %.1f ms: 60 fps again%c", ema, 10); }
    }
}
void display_set_interval(int n) {
    /* frame_rate = 60 (graphics.ini): the game asks for 2 refreshes per frame (30 fps); its logic steps by the refreshes
     * its VSync Count Thread counted, so presenting every refresh keeps its speed (measured 2026-10-24: a fade after a
     * key took the same time at 30 and 60 fps). TOMO_INTERVAL=<n> replaces the game's value outright (tests). */
    static int force = -2;
    if (force == -2) force = getenv("TOMO_INTERVAL") ? atoi(getenv("TOMO_INTERVAL")) : -1;
    if (force >= 0) n = force;
    else if (g_rate_ctl) InterlockedExchange(&g_adapt, gfx_frame_rate() >= 60);      /* the game chose this interval itself (display_rate_site) */
    else if (n > 1 && gfx_frame_rate() >= 60) { n /= 2; InterlockedExchange(&g_adapt, n == 1); }
    else InterlockedExchange(&g_adapt, 0);
    InterlockedExchange(&g_interval, n < 0 ? 0 : n > 5 ? 5 : n);
}
void nvn_pf_pace(LONG64 ticks);            /* nvn_gl.c: timing scopes */
/* SSBU_CLOCKSTAT=1: per 600 presented frames, the real time, the vsync signals sent, the refreshes the game counted
 * (its frame counter at 0x710593a440: 600 = one step per presented frame) and the refreshes this thread slept through. */
static LONG64 g_cs_steps, g_cs_hist[4];      /* game steps (1 + refreshes skipped) and the histogram of "refreshes counted since the last present" 0, 1, 2, 3+ */
/* nvn_gl.c, at nvnWindowAcquireTexture: the game's window object is the index pointer - 0x30; + 0x138 = refreshes its
 * VSync Count Thread has counted, + 0x140 = the count at its previous present. Right after this call the game computes
 * steps = max(count - last, 1) (0x710386fc30) and then stores last = count. */
uint64_t display_wnd_count_addr(void) { return g_wnd_count_addr; }      /* track R1 round 5 (r1_state.c run-ahead): the refresh count is wall time, not rolled back */
void display_clock_sample(uint64_t index_ptr) {
    if (index_ptr) g_wnd_count_addr = index_ptr - 0x30 + 0x138;      /* session 13 (track V): known at normal speed too, for the wait below */
    if (!g_clockstat || !index_ptr) return;
    uint64_t count = rd64(index_ptr - 0x30 + 0x138), last = rd64(index_ptr - 0x30 + 0x140);
    LONG64 d = (LONG64)(count - last);
    g_cs_steps += d > 1 ? d : 1;
    g_cs_hist[d <= 0 ? 0 : d >= 3 ? 3 : d]++;
}
static void clock_stat(LONG64 n, LARGE_INTEGER now) {
    static LARGE_INTEGER t_last; static LONG64 sig_last, late_last, steps_last, h_last[4];
    if (!g_clockstat || n % 600) return;
    LONG64 sig = g_vsync_signals, late = g_vsync_late;
    if (t_last.QuadPart) printf("clock: frames %lld-%lld real %.3f s, vsync signals %lld (%.2f Hz), game steps %lld = %.2f s of match time, refreshes counted between presents 0:%lld 1:%lld 2:%lld 3+:%lld, slept through by the vsync thread %lld%c", (long long)(n - 600), (long long)n,
        (double)(now.QuadPart - t_last.QuadPart) / (double)g_freq.QuadPart, (long long)(sig - sig_last), (double)(sig - sig_last) * (double)g_freq.QuadPart / (double)(now.QuadPart - t_last.QuadPart), (long long)(g_cs_steps - steps_last), (double)(g_cs_steps - steps_last) / 60.0,
        (long long)(g_cs_hist[0] - h_last[0]), (long long)(g_cs_hist[1] - h_last[1]), (long long)(g_cs_hist[2] - h_last[2]), (long long)(g_cs_hist[3] - h_last[3]), (long long)(late - late_last), 10);
    t_last = now; sig_last = sig; late_last = late; steps_last = g_cs_steps; memcpy(h_last, g_cs_hist, sizeof h_last);
}
/* New pacing, part 1 (nvnQueuePresentTexture, after the frame is in the window): decides the refresh that shows it. */
static void frame_queued(void) {
    static LARGE_INTEGER second_start;
    static LONG64 second_frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!second_start.QuadPart) second_start = now;
    LONG64 n = InterlockedIncrement64(&g_frames);
    second_frames++;
    if (n == 120) { void os_cc_relax(void); os_cc_relax(); }      /* os_threads.c: the game is running: its low-priority threads may leave their console cores */
    adapt_frame(now);
    { void display_l1_trace_frame(long long n); display_l1_trace_frame(n); }
    {   /* session 14: the game's thread on the performance cores (set once), and where it is seen at each present */
        static DWORD tid;
        if (tid != GetCurrentThreadId()) { tid = GetCurrentThreadId(); display_perf_thread(); }
        if (g_pc_n > 0) { g_pc_frames++; if (g_cpu_class[GetCurrentProcessorNumber() & 255] < g_class_max) g_pc_frames_slow++; }
    }
    if (InterlockedExchange(&g_planned, 0)) {                      /* session 14: decided before the present (display_present_plan) */
        LONG64 held = InterlockedExchange64(&g_hold_ticks, 0);
        if (held) nvn_pf_pace(held);                               /* the hold of the finished frame is pacing time (it replaces the wait after the present) */
    } else {
    LONG64 at = g_vsync_n + 1, iv = g_interval;
    if (!iv) at = g_vsync_n;                                       /* interval 0: shown at once, nobody waits */
    else if (at < g_latch + iv) at = g_latch + iv;
    g_latch = at;
    InterlockedExchange(&g_latch_pending, iv != 0);
    }
    clock_stat(n, now);
    {   /* TEST ONLY, SSBU_FRAMELOG=<k>: a line "frame-mark: <n>" every k presented frames, to place log events in game frames */
        static int every = -1;
        if (every < 0) { const char *e = getenv("SSBU_FRAMELOG"); every = e ? atoi(e) : 0; }
        if (every > 0 && n % every == 0) printf("frame-mark: %lld%c", (long long)n, 10);
        { void debug_mode_frame(long long frame); debug_mode_frame((long long)n); }      /* debug_mode.c: returns at once unless SSBU_DEBUG_END / SSBU_DEBUG_MATCH is set */
        g_prof15_frame = n;                                                              /* prof15.c (SSBU_PROF15, test only): presented frames, to turn CPU time into ms per frame */
    }
    if ((now.QuadPart - second_start.QuadPart) >= g_freq.QuadPart) {
        char title[160];
        if (g_lockstep) printf("speed: frame %lld, %lld frames presented in the last %.2f s (%.1f per second)%c", (long long)n, (long long)second_frames,
                               (double)(now.QuadPart - second_start.QuadPart) / (double)g_freq.QuadPart, (double)second_frames * (double)g_freq.QuadPart / (double)(now.QuadPart - second_start.QuadPart), 10);
        if (gfx_options()->overlay && second_frames) {       /* track O: frame_time_overlay = on */
            double ms = (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames;
            printf("frame-time: %.2f ms per frame over the last second (%lld frames)\n", ms, (long long)second_frames);
            snprintf(title, sizeof title, "%s - frame %lld - %lld fps - %.2f ms per frame", g_title, (long long)n, (long long)second_frames, ms);
        } else
        snprintf(title, sizeof title, "%s - frame %lld - %lld fps", g_title, (long long)n, (long long)second_frames);
        if (g_title_note[0]) { size_t k = strlen(title); snprintf(title + k, sizeof title - k, " - %s", g_title_note); }
        SetWindowTextA(g_hwnd, title);
        second_frames = 0;
        second_start = now;
    }
}
/* ---- Track L1 (input latency), 2026-10-14.
 * (1) SSBU_L1=1, TEST ONLY: per presented game frame, on the host's clock: the game's release from its wait for the window
 *     texture (display_wait_refresh), player 1's pad poll (nnhid.c), the present call (display_present_plan), the frame
 *     READY on the render thread (recorded and submitted: display_present_defer), vkQueuePresentKHR (display_present_done)
 *     and the display's vblank that follows it (the "shown" model of session 14: the first refresh that can show it).
 *     Percentiles over fight frames at exit (`l1-stage:` lines); `l1-press:` / `l1-after:` lines for every new press of
 *     player 1 in a fight (the injected press of the sequence) with the next presents.
 * (2) latency = normal | low | lowest (graphics.ini; SSBU_LATENCY=0|1|2 or the names wins):
 *     low: the game is released from its wait LATER than the tick, by D = tick period - (largest release-to-ready time of
 *          the last 64 frames + margin), so that it reads the pad as late as the frame can still be ready for the next
 *          tick; the present stays at the tick (display lock and cadence unchanged). The refresh count the game reads is
 *          the same (the tick was given; only the moment the game goes on moves), so its logic steps do not change.
 *          A late frame that had been delayed sets D to 0 for 60 frames (one late without a delay is not ours). SSBU_LATENCY_MARGIN=<ms> (default 2.0).
 *     lowest: low + the frame is presented as soon as it is ready (no hold for the tick, as vsync = off does). */
static int g_lat_mode = -1, g_l1 = -1;
static double g_lat_margin = 3.0;
static volatile LONG g_lat_backoff;
static LONG64 g_lat_r[256];                  /* release -> ready of the last 256 frames (QPC ticks) */
static LONG64 g_lat_near;                    /* round 2: frames ready less than 2 ms before their tick while delayed */
static volatile LONG g_lat_ri;
static volatile LONG64 g_lat_release, g_lat_d_sum, g_lat_d_n, g_lat_waits, g_lat_last_d, g_lat_backoffs;
int gfx_latency(void);                       /* gfx_options.c: graphics.ini latency = normal 0 | low 1 | lowest 2 */
int latency_mode(void) {
    if (g_lat_mode < 0) {
        const char *e = getenv("SSBU_LATENCY"), *m = getenv("SSBU_LATENCY_MARGIN");
        g_lat_mode = e ? (!_stricmp(e, "lowest") ? 2 : !_stricmp(e, "low") ? 1 : !_stricmp(e, "normal") ? 0 : atoi(e)) : gfx_latency();
        if (g_lat_mode < 0 || g_lat_mode > 2) g_lat_mode = 0;
        if (m && atof(m) >= 0.0) g_lat_margin = atof(m);
        g_l1 = getenv("SSBU_L1") && atoi(getenv("SSBU_L1"));
        printf("display: latency = %s%s (margin %.1f ms)%c", g_lat_mode == 2 ? "lowest" : g_lat_mode == 1 ? "low" : "normal",
               g_lat_mode ? ": the game is released late in the frame so that it reads the pad just in time for the next tick" : "", g_lat_margin, 10);
    }
    return g_lat_mode;
}
typedef struct { LONG64 release, poll, call, ready, qp, shown, n, fight_ticks; int polls, press; } L1Frame;
static L1Frame g_l1_ring[64], g_l1_cur;
static volatile LONG64 g_l1_wr, g_l1_rd;
static volatile LONG64 g_l1_poll_qpc, g_l1_polls, g_l1_ticks;
static volatile LONG g_l1_press;             /* a new press was seen at a poll since the last present call */
static LONG64 g_l1_press_qpc, g_l1_press_left, g_l1_press_n;
#define L1_BINS 1000                         /* 0.1 ms bins up to 100 ms */
#define L1_ST 7
static LONG64 g_l1_hist[L1_ST][L1_BINS + 1], g_l1_cnt[L1_ST];
static double g_l1_sum[L1_ST], g_l1_max[L1_ST];
static const char *const k_l1_name[L1_ST] = {"release -> poll", "poll -> present call", "present call -> ready (render thread)", "ready -> vkQueuePresentKHR (hold)",
                                             "vkQueuePresentKHR -> next vblank", "poll -> next vblank after the present (TOTAL of our pipeline)", "tick -> release (latency mode delay)"};
static void l1_add(int k, LONG64 a, LONG64 b) {
    if (!a || !b) return;
    double ms = (double)(b - a) * 1000.0 / (double)g_freq.QuadPart;
    if (ms < 0) return;
    int bin = (int)(ms * 10.0); if (bin > L1_BINS) bin = L1_BINS;
    g_l1_hist[k][bin]++; g_l1_cnt[k]++; g_l1_sum[k] += ms; if (ms > g_l1_max[k]) g_l1_max[k] = ms;
}
static volatile LONG g_l1_trace_left, g_l1_trace_done;
static LONG64 g_l1_trace_t0;
int display_l1_trace(void) { return g_l1_trace_left > 0; }
double display_l1_ms(void) { LARGE_INTEGER n; QueryPerformanceCounter(&n); return (double)(n.QuadPart - g_l1_trace_t0) * 1000.0 / (double)g_freq.QuadPart; }
/* TEST ONLY, SSBU_L1_TRACE=<presented frame> (any game, either renderer): the l1-trace timeline (every event / condition
 * wait and signal of the game's threads, with the thread and the caller) for 7 frames from that frame; any other value:
 * from a fight's button press (display_l1_poll). Called at each present (frame_queued). */
void display_l1_trace_frame(long long n) {
    static long long at = -2;
    if (at == -2) { const char *e = getenv("SSBU_L1_TRACE"); at = e && atoll(e) > 1 ? atoll(e) : -1; }
    if (at < 0) return;
    if (n == at && !g_l1_trace_done) { LARGE_INTEGER now; QueryPerformanceCounter(&now); g_l1_trace_done = 1; g_l1_trace_t0 = now.QuadPart; InterlockedExchange(&g_l1_trace_left, 7); }
    else if (g_l1_trace_left > 0) { printf("l1-trace: %+8.2f PRESENT (frames left %ld)%c", display_l1_ms(), (long)g_l1_trace_left, 10); InterlockedDecrement(&g_l1_trace_left); }
}
void display_l1_poll(uint64_t buttons, long long fight_ticks, long long poll) {
    static uint64_t prev;
    LARGE_INTEGER now;
    latency_mode();
    QueryPerformanceCounter(&now);
    g_l1_poll_qpc = now.QuadPart; g_l1_polls++; g_l1_ticks = fight_ticks;
    if (g_l1_trace_left > 0) printf("l1-trace: %+8.2f %-26s POLL player 1 buttons %llx, fight ticks %lld%c", display_l1_ms(), "(poll)", (unsigned long long)buttons, fight_ticks, 10);
    if (g_l1 && fight_ticks > 0 && buttons && !(prev & buttons)) {
        unsigned long nvn_front_present_tid(void);
        printf("l1-press: game frame counter [0x710593a440] %llu, window refresh count %llu; player 1 poll %lld (fight-frame ticks %lld, presented frames %lld, poll thread %lu, presenting thread %lu): buttons %llx, %.2f ms after the game's release%c", (unsigned long long)rd64(GA(0x710593a440ull)), (unsigned long long)(g_wnd_count_addr ? rd64(g_wnd_count_addr) : 0), poll, fight_ticks, (long long)g_frames, GetCurrentThreadId(), nvn_front_present_tid(),
               (unsigned long long)buttons, g_lat_release ? (double)(now.QuadPart - g_lat_release) * 1000.0 / (double)g_freq.QuadPart : -1.0, 10);
        g_l1_press_qpc = now.QuadPart; g_l1_press_left = 6; g_l1_press_n = 0;
        if (!g_l1_trace_done && getenv("SSBU_L1_TRACE")) { g_l1_trace_done = 1; g_l1_trace_t0 = now.QuadPart; InterlockedExchange(&g_l1_trace_left, 7); }
        InterlockedExchange(&g_l1_press, 1);
    }
    prev = buttons;
}
/* L1 early present (nvn_gl.c l1_early_present): called at the start of the game's wait for the window texture. Also counts
 * the game's submissions between that wait and its present (must be 0 for the early present to show the whole picture). */
static volatile LONG g_l1_waited, g_l1_subs_now;
static LONG64 g_l1_chk_frames, g_l1_chk_bad, g_l1_chk_subs, g_l1_early_skips;
static int g_l1_early = -1;
int display_l1_early(void) {
    if (g_l1_early < 0) { const char *e = getenv("SSBU_L1_EARLY"); latency_mode(); g_l1_early = e && atoi(e) != 0;      /* TEST ONLY: 1 game submission comes after the wait in every frame (l1-submit), so the picture is NOT complete there */ printf("display: early present %s%c", g_l1_early ? "ON: the picture the game finished before its wait goes to the window at the tick it waits for" : "off", 10); }
    g_l1_subs_now = 0;
    InterlockedExchange(&g_l1_waited, 1);
    return g_l1_early;
}
int display_l1_pipe_default(void) { return 1; }      /* L1 round 4: pipe ON by default (coordinator 2026-10-14); SSBU_L1_PIPE=0 = the old path */
void display_l1_submit(void) { if (g_l1_waited) InterlockedIncrement(&g_l1_subs_now); }
void display_l1_early_skip(void) { g_l1_early_skips++; }
/* game thread, display_present_plan */
static void l1_plan(int late) {
    if (g_l1_trace_left > 0) { printf("l1-trace: %+8.2f present call planned (frames left %ld)%c", display_l1_ms(), (long)g_l1_trace_left, 10); InterlockedDecrement(&g_l1_trace_left); }
    if (InterlockedExchange(&g_l1_waited, 0)) { g_l1_chk_frames++; if (g_l1_subs_now) { g_l1_chk_bad++; g_l1_chk_subs += g_l1_subs_now; } }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (late && g_lat_mode > 0) { InterlockedExchange(&g_lat_backoff, 300); g_lat_backoffs++; }      /* round 2: ANY late frame: no delay for 300 frames (a spike tends to come with others) */
    L1Frame f;
    memset(&f, 0, sizeof f);
    f.release = g_lat_release; f.poll = g_l1_poll_qpc; f.call = now.QuadPart; f.n = g_frames + 1; f.polls = (int)InterlockedExchange64(&g_l1_polls, 0); f.fight_ticks = g_l1_ticks;
    f.press = InterlockedExchange(&g_l1_press, 0);
    if (f.poll && f.poll < f.release) f.poll = 0;                  /* no poll since the release: the frame did not read the pad after it */
    g_l1_ring[g_l1_wr & 63] = f;
    InterlockedIncrement64(&g_l1_wr);
}
/* render thread, display_present_defer: the frame is recorded and submitted */
static void l1_ready(LONG64 tgt) {
    int late = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_l1_wr - g_l1_rd > 4) g_l1_rd = g_l1_wr - 1;               /* lost step (first-run screen, a movie): resynchronise */
    if (g_l1_rd >= g_l1_wr) { memset(&g_l1_cur, 0, sizeof g_l1_cur); return; }
    g_l1_cur = g_l1_ring[g_l1_rd & 63];
    InterlockedIncrement64(&g_l1_rd);
    g_l1_cur.ready = now.QuadPart;
    (void)late;
    if (g_lat_mode > 0 && tgt && g_lat_last_d > 0) {      /* round 2: a NEAR miss of a delayed frame (ready < 2 ms before its tick): no delay for 120 frames */
        LONG64 n = g_vsync_n, t = g_tick_at[n & 15];
        double per = g_lock_k ? (double)g_lock_k * g_vb_period : (double)g_freq.QuadPart / 60.0;
        if (t && (double)t + (double)(tgt - n) * per - (double)now.QuadPart < 2.0 * (double)g_freq.QuadPart / 1000.0) { if (g_lat_backoff < 120) InterlockedExchange(&g_lat_backoff, 120); g_lat_near++; }
    }
    if (g_l1_cur.release && now.QuadPart > g_l1_cur.release && now.QuadPart - g_l1_cur.release < g_freq.QuadPart / 10) {
        LONG i = InterlockedIncrement(&g_lat_ri);
        g_lat_r[i & 255] = now.QuadPart - g_l1_cur.release;
    }
}
/* render thread, display_present_done: vkQueuePresentKHR returned; shown = QPC of the vblank after it */
static void l1_done(int fight, LONG64 shown) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    L1Frame *f = &g_l1_cur;
    if (!f->call) return;
    f->qp = now.QuadPart; f->shown = shown;
    if (fight && g_l1) {
        l1_add(0, f->release, f->poll); l1_add(1, f->poll, f->call); l1_add(2, f->call, f->ready); l1_add(3, f->ready, f->qp); l1_add(4, f->qp, f->shown); l1_add(5, f->poll, f->shown);
    }
    if (g_l1 && g_l1_press_left > 0) {
        double fq = (double)g_freq.QuadPart / 1000.0, p = (double)g_l1_press_qpc;
        printf("l1-after: present %lld after the press poll (game frame %lld, fight ticks %lld, polls %d%s): release %+.2f, poll %+.2f, present call %+.2f, ready %+.2f, vkQueuePresentKHR %+.2f, next vblank %+.2f ms%c",
               (long long)g_l1_press_n, (long long)f->n, (long long)f->fight_ticks, f->polls, f->press ? ", THE PRESS POLL" : "", ((double)f->release - p) / fq, f->poll ? ((double)f->poll - p) / fq : -999.0,
               ((double)f->call - p) / fq, ((double)f->ready - p) / fq, ((double)f->qp - p) / fq, ((double)f->shown - p) / fq, 10);
        if (f->press || g_l1_press_n) { g_l1_press_n++; g_l1_press_left--; }
    }
    f->call = 0;
}
/* game thread, end of display_wait_refresh (normal speed): latency = low / lowest delays the release */
static void lat_release(void) {
    static HANDLE timer;
    LARGE_INTEGER now;
    latency_mode();
    if (g_lat_mode > 0 && !g_lockstep) {
        LONG64 n = g_vsync_n, t = g_tick_at[n & 15];
        double per = g_lock_k ? (double)g_lock_k * g_vb_period : (double)g_freq.QuadPart / 60.0;
        g_lat_last_d = 0;
        if (g_lat_backoff > 0) InterlockedDecrement(&g_lat_backoff);
        else if (t && n == g_latch && g_lat_ri >= 256) {
            LONG64 mx = 0;
            for (int i = 0; i < 256; i++) if (g_lat_r[i] > mx) mx = g_lat_r[i];
            double d = per - (double)mx - g_lat_margin * (double)g_freq.QuadPart / 1000.0, dmax = per - 4.0 * (double)g_freq.QuadPart / 1000.0;
            if (d > dmax) d = dmax;
            if (d > (double)g_freq.QuadPart / 2000.0) {
                if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);
                precise_wait_until(t + (LONG64)d, timer);
                g_lat_d_sum += (LONG64)d; g_lat_d_n++; g_lat_last_d = (LONG64)d;
            }
        }
        g_lat_waits++;
    }
    QueryPerformanceCounter(&now);
    g_lat_release = now.QuadPart;
    if (g_l1 && g_tick_at[g_vsync_n & 15]) l1_add(6, g_tick_at[g_vsync_n & 15], now.QuadPart);
}
static void l1_report(FILE *out) {
    latency_mode();
    fprintf(out, "l1-submit: frames whose present followed a wait for the window texture: %lld; of them with submissions of the game BETWEEN the wait and the present: %lld (%lld submissions)%s; early presents used by the game's present: %lld\n", (long long)g_l1_chk_frames, (long long)g_l1_chk_bad, (long long)g_l1_chk_subs,
            g_l1_chk_bad ? " (an early present would miss them)" : " (the picture is complete when the wait begins)", (long long)g_l1_early_skips);
    { void nvn_l1_report(FILE *out); nvn_l1_report(out); }
    if (g_lat_mode) fprintf(out, "l1: latency = %s: release delayed in %lld of %lld waits, mean delay %.2f ms, late frames (then 300 frames without) %lld, near misses (then 120 without) %lld\n", g_lat_mode == 2 ? "lowest" : "low", (long long)g_lat_d_n, (long long)g_lat_waits,
                            g_lat_d_n ? (double)g_lat_d_sum / (double)g_lat_d_n * 1000.0 / (double)g_freq.QuadPart : 0.0, (long long)g_lat_backoffs, (long long)g_lat_near);
    if (!g_l1) return;
    for (int k = 0; k < L1_ST; k++) {
        if (!g_l1_cnt[k]) continue;
        double q[4] = {0.5, 0.95, 0.99, 0}; double v[3] = {0, 0, 0};
        for (int j = 0; j < 3; j++) { LONG64 want = (LONG64)(q[j] * (double)g_l1_cnt[k]), acc = 0; for (int b = 0; b <= L1_BINS; b++) { acc += g_l1_hist[k][b]; if (acc > want) { v[j] = (b + 0.5) / 10.0; break; } } }
        fprintf(out, "l1-stage: %-62s fight frames %6lld: mean %6.2f ms, median %6.2f, 95 %% %6.2f, 99 %% %6.2f, max %6.2f\n", k_l1_name[k], (long long)g_l1_cnt[k], g_l1_sum[k] / (double)g_l1_cnt[k], v[0], v[1], v[2], g_l1_max[k]);
    }
}

/* Session 14, game thread, BEFORE the present job goes to the render thread (Vulkan only; OpenGL keeps the path above):
 * the refresh that shows this frame, by the same rule as frame_queued, and whether the present is held for it. A frame
 * queued before the tick it is due at (on time) is held until that tick; a late one is presented at once. */
void display_present_plan(int vulkan) {
    if (!g_la_first) la_stop();                            /* U3: the first present: the loading animation stops before it (loading_anim.c) */
    if (old_pacing()) return;                          /* 2026-10-30: OpenGL presents at the tick too (nvn_gl.c job_present / job_gl_swap_flush) */
    (void)vulkan;
    pace_options();
    LONG64 now_n = g_vsync_n, iv = g_interval, prev = g_latch, at = now_n + 1;
    int on_time = iv && now_n < prev + iv && prev + iv - now_n <= 6;
    if (!iv) at = now_n;
    else if (at < prev + iv) at = prev + iv;
    g_latch = at;
    InterlockedExchange(&g_latch_pending, iv != 0);
    if (iv && !on_time) g_hold_late++;
    l1_plan(iv && !on_time && now_n > prev + iv);      /* L1 (a frame queued after the tick it was due at = late) */
    InterlockedExchange64(&g_hold_target, on_time && g_hold_on && !g_lockstep && g_ff == 1 ? at : 0);
    InterlockedExchange(&g_planned, 1);
}
/* Render thread, at the place of vkQueuePresentKHR. Returns 1 when the finished frame is to be KEPT for the tick decided
 * above: the render thread then does not call vkQueuePresentKHR and does not wait either (it goes on serving the game's
 * other threads); the vsync thread posts the job that presents it at the tick (nvn_front_post_present_flush).
 * First form of this session (runs s14pB / s14pC): the render thread SLEPT here until the tick, with the game's thread
 * standing inside its present call: measured WORSE than before (2.2-4.8 % of the fight frames on screen for 3-4 refreshes
 * against 1.8 %, CPU time of every game thread up by 40 %): the game was held where it does not expect to wait.
 * Now the game's thread returns from its present at once and waits where it always did (the window texture's sync). */
static volatile LONG64 g_defer_target;                 /* a finished frame is kept on the render thread for this tick */
void nvn_front_post_present_flush(void);               /* nvn_gl.c: the kept present, as a job of the render thread */
int display_present_defer(void) {
    { static DWORD tid; if (tid != GetCurrentThreadId()) { tid = GetCurrentThreadId(); display_perf_thread(); } }      /* the render thread: performance cores */
    LONG64 tgt = InterlockedExchange64(&g_hold_target, 0);
    l1_ready(tgt);
    if (!tgt || g_vsync_n >= tgt) return 0;
    InterlockedExchange64(&g_defer_target, tgt);
    g_hold_n++;
    return 1;
}
/* Render thread, after it has stored the kept present: 1 = the tick came meanwhile, present now. (The vsync thread looks
 * at g_defer_target AFTER it has published the tick, this looks at the tick AFTER the target is set: one of the two sees it.) */
int display_present_due(void) { LONG64 t = g_defer_target; return t && g_vsync_n >= t; }
void display_present_flushed(void) { InterlockedExchange64(&g_defer_target, 0); }
/* Render thread, right after vkQueuePresentKHR returned: what the screen gets (see the comment at the top). */
static void pace_stat_line(FILE *out, const char *what, int i, int since) {
    LONG64 n = g_ps[i].n - (since ? g_ps_last[i].n : 0), h[PS_HIST];
    double sum = g_ps[i].sum - (since ? g_ps_last[i].sum : 0), sum2 = g_ps[i].sum2 - (since ? g_ps_last[i].sum2 : 0);
    if (n < 2) return;
    for (int k = 0; k < PS_HIST; k++) h[k] = g_ps[i].hist[k] - (since ? g_ps_last[i].hist[k] : 0);
    double mean = sum / (double)n, var = sum2 / (double)n - mean * mean, per = g_vb_period * 1000.0 / (double)g_freq.QuadPart;
    LONG64 mode = 0;
    for (int k = 1; k < PS_HIST; k++) if (h[k] > h[mode]) mode = k;
    fprintf(out, "display-timing: %s: %lld frames; on screen for 0 / 1 / 2 / 3 / 4 / 5+ refreshes (%.2f ms each): %lld / %lld / %lld / %lld / %lld / %lld = %.2f %% of the frames NOT on screen for the usual %lld; time between presents: mean %.3f ms, standard deviation %.3f ms%s",
            what, (long long)n, per, (long long)h[0], (long long)h[1], (long long)h[2], (long long)h[3], (long long)h[4], (long long)h[5], 100.0 * (double)(n - h[mode]) / (double)n, (long long)mode, mean, var > 0 ? sqrt(var) : 0.0, since ? "" : "");
    if (!since) fprintf(out, ", shortest %.2f, longest %.2f ms", g_ps[i].mn, g_ps[i].mx);
    fputc(10, out);
}
void display_present_done(int fight) {
    static int stat = -1;
    LARGE_INTEGER now;
    if (g_vb_state != 2 || g_lockstep) return;
    if (stat < 0) stat = getenv("SSBU_PACESTAT") && atoi(getenv("SSBU_PACESTAT"));
    QueryPerformanceCounter(&now);
    AcquireSRWLockShared(&g_vb_lock);
    LONG64 vc = g_vb_count, vq = g_vb_qpc;
    double period = g_vb_period;
    ReleaseSRWLockShared(&g_vb_lock);
    double since = (double)(now.QuadPart - vq) / period;          /* refreshes since the last vblank seen (more than 1: the vblank thread is behind) */
    LONG64 whole = since > 0 ? (LONG64)since : 0, shown = vc + whole + 1;      /* the vblank after this present */
    int ph = (int)((since - (double)whole) * 10.0);
    g_ps_phase[ph < 0 ? 0 : ph > 9 ? 9 : ph]++;
    if (g_ps_prev_qpc) {
        LONG64 refs = shown - g_ps_prev_vb;                         /* the PREVIOUS frame was on screen from its vblank to this one's */
        double ms = (double)(now.QuadPart - g_ps_prev_qpc) * 1000.0 / (double)g_freq.QuadPart;
        {   /* Round 16 (P3 section 15), SSBU_LATELOG=1 (test): one line per fight frame that was not on screen for exactly
             * 2 refreshes, with the presented-frame number the hitch lines use ("hitch: frame N"), so that every late frame
             * can be given its cause. Frames 100 ms or more apart are NOT in the display-timing counts (the test above):
             * they are printed here as well, marked. */
            static int ll = -1;
            static LONG64 n_long;
            if (ll < 0) ll = getenv("SSBU_LATELOG") && atoi(getenv("SSBU_LATELOG"));
            if (fight && ms >= 100.0) n_long++;
            if (ll && fight && (refs != 2 || ms >= 100.0))
                printf("display-late: the frame shown before presented frame %lld was on screen for %lld refreshes; %.1f ms between the two presents%s\n", (long long)g_prof15_frame, (long long)refs, ms,
                       ms >= 100.0 ? " (100 ms or more: NOT counted in the display-timing lines)" : "");
        }
        if (ms < 100.0) for (int i = 0; i <= (fight ? 1 : 0); i++) {
            g_ps[i].n++; g_ps[i].hist[refs < 0 ? 0 : refs >= PS_HIST ? PS_HIST - 1 : refs]++; g_ps[i].sum += ms; g_ps[i].sum2 += ms * ms;
            if (g_ps[i].n == 1 || ms < g_ps[i].mn) g_ps[i].mn = ms;
            if (ms > g_ps[i].mx) g_ps[i].mx = ms;
        }
    }
    g_ps_prev_vb = shown; g_ps_prev_qpc = now.QuadPart;
    l1_done(fight, vq + (LONG64)((double)(whole + 1) * period));      /* L1: the vblank after this present */
    if (stat && g_ps[0].n && g_ps[0].n % 600 == 0 && g_ps[0].n != g_ps_last[0].n) {
        pace_stat_line(stdout, "last 600 frames", 0, 1);
        if (g_ps[1].n - g_ps_last[1].n > 300) pace_stat_line(stdout, "fight frames among them", 1, 1);
        memcpy(g_ps_last, g_ps, sizeof g_ps);
    }
}
/* New pacing, part 2: holds the caller until the refresh that shows the last queued frame. Returns 1 if it waited. */
int display_wait_refresh(void) {
    if (old_pacing() || !InterlockedExchange(&g_latch_pending, 0)) return 0;
    if (g_vsync_n >= g_latch) {
        QueryPerformanceCounter(&g_release_at);        /* An already-due refresh still starts a new frame's busy time. */
        return 0;
    }
    if (g_lockstep) {                                              /* SSBU_SPEED: see the comment at g_speed */
        static LONG64 due;                                         /* wall-clock time of the next refresh (QPC ticks) */
        LARGE_INTEGER a, now;
        QueryPerformanceCounter(&a);
        while (g_vsync_n < g_latch) {
            if (g_speed > 0) {
                LONGLONG step = (LONGLONG)((double)g_freq.QuadPart / (60.0 * g_speed));
                if (!due) due = a.QuadPart;
                due += step;
                QueryPerformanceCounter(&now);
                if (now.QuadPart - due > 4 * step) due = now.QuadPart;      /* far behind (a load): do not run to catch up */
                while (now.QuadPart < due) {
                    LONGLONG left = due - now.QuadPart;
                    if (left * 1000 > 2 * g_freq.QuadPart) Sleep(1); else YieldProcessor();
                    QueryPerformanceCounter(&now);
                }
            }
            uint64_t ca = g_wnd_count_addr, c0 = ca ? rd64(ca) : 0;
            AcquireSRWLockExclusive(&g_vsync_lock);
            g_vsync_n++;
            ReleaseSRWLockExclusive(&g_vsync_lock);
            if (g_vsync_event) { os_signal_event(g_vsync_event); InterlockedIncrement64(&g_vsync_signals); }
            if (ca) {                                              /* wait until the game's VSync Count Thread has counted it */
                LARGE_INTEGER t0, t1;
                QueryPerformanceCounter(&t0);
                while (rd64(ca) == c0) {
                    QueryPerformanceCounter(&t1);
                    if ((t1.QuadPart - t0.QuadPart) * 1000 > 100 * g_freq.QuadPart) { printf("display: speed mode: the game did not count a refresh within 100 ms%c", 10); break; }
                    Sleep(0);
                }
            }
        }
        return 1;
    }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    hitch_pacing(1);
    /* Track V session 13 (the owner's microstutter at a steady 60 fps): the game steps its logic by the refreshes its own
     * VSync Count Thread has COUNTED when the window texture is acquired right after this wait (steps = max(count - last, 1),
     * main 0x710386fc30). That thread is woken by the same signal as this wait, so the main thread could read the count
     * before the refresh was counted: 0 this frame, 2 the next = one frame with a doubled step at a perfect 60 fps
     * (R0_determinism.md measured 104 / 559 / 20 polls with 0 / 1 / 2 steps at normal speed). Here: after the refresh, wait
     * (at most 3 ms, yielding) until the count has advanced by the refreshes that passed during this wait.
     * OFF by default (SSBU_PACE_COUNTWAIT=1 turns it on): measured in two quiet runs of three fights each (tp0 / tp1) it
     * did not change the frames over 18 ms and the doubled steps were 17 against 10 in 14,000 frames: not shown to matter. */
    static int cw = -1;
    if (cw < 0) cw = getenv("SSBU_PACE_COUNTWAIT") && atoi(getenv("SSBU_PACE_COUNTWAIT"));
    uint64_t ca = cw ? g_wnd_count_addr : 0, c0 = ca ? rd64(ca) : 0;
    LONG64 n0;
    AcquireSRWLockExclusive(&g_vsync_lock);
    n0 = g_vsync_n;
    while (g_vsync_n < g_latch) SleepConditionVariableSRW(&g_vsync_cv, &g_vsync_lock, 100, 0);
    n0 = g_vsync_n - n0;
    ReleaseSRWLockExclusive(&g_vsync_lock);
    if (ca && n0 > 0) {
        LARGE_INTEGER t0, t1;
        static LONG64 waited, timed_out;
        QueryPerformanceCounter(&t0);
        while (rd64(ca) - c0 < (uint64_t)n0) {
            QueryPerformanceCounter(&t1);
            if ((t1.QuadPart - t0.QuadPart) * 1000 > 3 * g_freq.QuadPart) { if (timed_out++ < 5) printf("display: the game had not counted the refresh 3 ms after it (count wait)%c", 10); break; }
            Sleep(0);
            waited++;
        }
    }
    lat_release();                                         /* L1: latency = low / lowest; the release time */
    QueryPerformanceCounter(&g_release_at);                /* frame_rate = 60: the frame's busy time starts here (adapt_frame) */
    hitch_pacing(0);
    QueryPerformanceCounter(&b);
    nvn_pf_pace(b.QuadPart - a.QuadPart);
    InterlockedExchangeAdd64(&g_held_qpc, b.QuadPart - a.QuadPart);
    return 1;
}
static void pace_release_selftest(void) {
    if (!getenv("SSBU_PACE_RELEASE_SELFTEST")) return;
    int failed = 0;
    g_old_pacing = 0;
    g_adapt = 1;
    g_interval = 1;
    for (int i = 0; i < 15; i++) {
        LARGE_INTEGER before, after, present;
        QueryPerformanceCounter(&before);
        g_release_at.QuadPart = before.QuadPart - g_freq.QuadPart / 10;
        g_latch = 1;
        g_vsync_n = 1 + (i & 1);
        g_latch_pending = 1;
        int waited = display_wait_refresh();
        QueryPerformanceCounter(&after);
        failed += waited != 0 || g_latch_pending != 0 || g_release_at.QuadPart < before.QuadPart || g_release_at.QuadPart > after.QuadPart;
        present.QuadPart = after.QuadPart + g_freq.QuadPart * 8 / 1000;
        adapt_frame(present);
        LONGLONG released = g_release_at.QuadPart;
        failed += display_wait_refresh() != 0 || g_release_at.QuadPart != released;
    }
    failed += g_interval != 1;
    printf("display: release timestamp self-test: %s (%d failures, interval %ld)\n", failed ? "FAIL" : "PASS", failed, (long)g_interval);
    fflush(stdout);
    ExitProcess(failed ? 1 : 0);
}
/* Before a present: a frame queued earlier that nobody waited for (a caller that does not wait on the window sync). */
void display_before_present(void) {
    static int said;
    if (!g_la_first) la_stop();
    if (old_pacing() || !g_latch_pending) return;
    if (!said) { said = 1; printf("display: a present without a wait on the window texture's sync: held at the next present instead%c", 10); }
    display_wait_refresh();
}
void display_pace(void) {
    static LARGE_INTEGER next, second_start;
    static LONG64 second_frames;
    LARGE_INTEGER now;
    if (!g_la_first) la_stop();                            /* (a present that came by neither call above) */
    if (!old_pacing()) { frame_queued(); return; }
    QueryPerformanceCounter(&now);
    if (!next.QuadPart) { next = now; second_start = now; }
    LONG64 n = InterlockedIncrement64(&g_frames);
    second_frames++;
    clock_stat(n, now);
    next.QuadPart += g_freq.QuadPart * g_interval / 60;
    /* P3: wait until `next` exactly. Sleep(ms) cut the wait to whole milliseconds and, at the default 15.6 ms timer period,
     * could overshoot by a frame; a frame that came late was then followed by frames shown early (up to 100 ms of debt).
     * Now: a high-resolution waitable timer up to 0.5 ms before the deadline, then a spin; a late frame may be made up for
     * by at most one refresh interval, as a display that missed a refresh shows the next frame at the next one. */
    LONGLONG left = next.QuadPart - now.QuadPart, frame_ticks = g_freq.QuadPart * (g_interval ? g_interval : 1) / 60;
    if (!g_interval) next = now;
    else if (left > 0) {
        static HANDLE timer;
        LARGE_INTEGER after, due;
        hitch_pacing(1);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        due.QuadPart = -((left - g_freq.QuadPart / 2000) * 10000000 / g_freq.QuadPart);          /* relative, 100 ns units */
        if (due.QuadPart < 0) { if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100); else Sleep((DWORD)(left * 1000 / g_freq.QuadPart)); }
        for (QueryPerformanceCounter(&after); after.QuadPart < next.QuadPart; QueryPerformanceCounter(&after)) YieldProcessor();
        hitch_pacing(0);
        nvn_pf_pace(after.QuadPart - now.QuadPart);
        InterlockedExchangeAdd64(&g_held_qpc, after.QuadPart - now.QuadPart);
    } else if (-left > frame_ticks) next.QuadPart = now.QuadPart - frame_ticks;
    if ((now.QuadPart - second_start.QuadPart) >= g_freq.QuadPart) {
        char title[160];
        if (gfx_options()->overlay && second_frames) printf("frame-time: %.2f ms per frame over the last second (%lld frames)\n", (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames, (long long)second_frames);
        if (gfx_options()->overlay && second_frames) snprintf(title, sizeof title, "%s - frame %lld - %lld fps - %.2f ms per frame", g_title, (long long)n, (long long)second_frames, (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames);
        else
        snprintf(title, sizeof title, "%s - frame %lld - %lld fps", g_title, (long long)n, (long long)second_frames);
        SetWindowTextA(g_hwnd, title);
        second_frames = 0;
        second_start = now;
    }
}

void display_report(FILE *out) {
    fprintf(out, "\ndisplay: %lld frames presented\n", (long long)g_frames);
    fprintf(out, "display: %s; %lld vsync signals, %lld refreshes slept through by the vsync thread\n", old_pacing() ? "old pacing (SSBU_OLD_PACING=1: two clocks)" : "one refresh clock", (long long)g_vsync_signals, (long long)g_vsync_late);
    if (g_pc_n > 0) fprintf(out, "display: the game's thread was on an EFFICIENCY core at %lld of %lld presents (%s)\n", (long long)g_pc_frames_slow, (long long)g_pc_frames, g_pc_on ? "performance cores asked for" : "SSBU_PCORES=0: left to the system");
    if (g_vb_state == 2) {
        fprintf(out, "display: refresh %.3f Hz measured; %s; presents held for their tick %lld, late frames presented at once %lld\n", (double)g_freq.QuadPart / g_vb_period,
                g_lock_k ? "DISPLAY LOCK (the tick is the display's vblank)" : "our own 60.000 Hz clock", (long long)g_hold_n, (long long)g_hold_late);
        pace_stat_line(out, "all frames", 0, 0);
        pace_stat_line(out, "fight frames", 1, 0);
        fprintf(out, "display-timing: presents by tenth of a refresh after the vblank before them: %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\n", (long long)g_ps_phase[0], (long long)g_ps_phase[1], (long long)g_ps_phase[2], (long long)g_ps_phase[3],
                (long long)g_ps_phase[4], (long long)g_ps_phase[5], (long long)g_ps_phase[6], (long long)g_ps_phase[7], (long long)g_ps_phase[8], (long long)g_ps_phase[9]);
    }
    l1_report(out);
    nvn_gl_report(out);
}

/* ---- nn::vi: one display, one layer; the native window is an opaque token for NVN */
static uint8_t g_display_token[16], g_layer_token[16], g_native_window_token[16];
static void vi_OpenDefaultDisplay(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_display_token); RET(0); }
static void vi_CreateLayer(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_layer_token); RET(0); }
static void vi_GetNativeWindow(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_native_window_token); RET(0); }
static void vi_GetDisplayVsyncEvent(Ctx *c) {         /* (SystemEventType *out, Display *) */
    os_init_event(A0, 1);
    g_vsync_event = A0;
    RET(0);
}
static void nothing(Ctx *c) { (void)c; }

static const ImportDef vi_imports[] = {
    {"_ZN2nn2vi10InitializeEv", nothing}, {"_ZN2nn2vi8FinalizeEv", nothing},
    {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_OpenDefaultDisplay}, {"_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", nothing},
    {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_CreateLayer}, {"_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", nothing},
    {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_GetNativeWindow},
    {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_GetDisplayVsyncEvent},
};

void register_display_imports(void) {
    register_imports(vi_imports, (int)(sizeof vi_imports / sizeof vi_imports[0]));
}

/* menu.cpp (F1 > Display): the measured refresh rate (0 = not measured) and the display lock's refreshes per game frame
 * (0 = our own 60 Hz clock: a panel that is not k x 60 Hz, where frames cannot all stay on screen equally long) */
void display_refresh_info(double *hz, int *lock_k) {
    *hz = g_vb_period > 0 && g_freq.QuadPart ? (double)g_freq.QuadPart / g_vb_period : 0;
    *lock_k = (int)g_lock_k;
}

/* nvn_vk_store.c: a note in the window's title while the first start reads the game data (minutes on a slow disk, before
 * the game shows any frame: issue #57 looked like a black screen); NULL puts the normal title back */
void display_title_note(const char *note) {
    la_note(note);                                         /* U3: the line beside the loading animation */
#ifdef _WIN32
    if (!g_hwnd) return;
    char t[400];
    if (note && note[0]) snprintf(t, sizeof t, "%s - %s", g_title, note); else snprintf(t, sizeof t, "%s", g_title);
    SetWindowTextA(g_hwnd, t);
#else
    (void)note;
#endif
}
