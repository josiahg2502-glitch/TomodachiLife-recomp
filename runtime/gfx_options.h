/* Track O (options), first set: config\graphics.ini. See gfx_options.c and plan\phases\O1_options.md. */
#ifndef GFX_OPTIONS_H
#define GFX_OPTIONS_H
typedef struct {
    char path[260];
    int renderer;            /* 0 Vulkan (default), 1 OpenGL                               env SSBU_RENDERER=vk|gl wins */
    int window_mode;         /* 0 windowed, 1 borderless (desktop size), 2 fullscreen (borderless, topmost, covering the monitor) */
    int width, height;       /* client size of the windowed mode */
    int present;             /* 0 clock (mailbox, else immediate, paced by our 60 Hz clock), 1 vsync on (fifo), 2 vsync off (immediate), 3 mailbox;   env SSBU_VK_PRESENT wins */
    char gpu[64];            /* "" = discrete first; an index of the printed list, or a part of the device name   env SSBU_VK_DEVICE wins */
    float scale;             /* internal resolution scale asked for (Vulkan: nvn_vk_draw.c rs_scale, 0.25 .. 2.0 since PR #8 (stephenjayakar); env SSBU_VK_SCALE wins) */
    int handheld;            /* 1: the game is told it runs handheld = it makes its own 1280x720 render targets instead of 1920x1080 */
    int aniso;               /* 0 = the game's own value; 2, 4, 8, 16 = at least that on samplers that filter between mip levels */
    int overlay;             /* 1: frame time in the title bar and a "frame-time:" log line every second */
    int shader_skip;         /* shader_wait: 0 hold (a draw waits for its shader or pipeline: the frame is late), 1 skip (near a fight the draw is left out until they are ready; nvn_vk_m4.c skip_on; env SSBU_VK_NOHOLD wins) */
    int profile;            /* 0 custom (the keys as written), 1 low: scale, aniso and present are set by the profile (gfx_options.c) */
    int shader_prepare;     /* track V session 14 (appended: older objects keep their offsets): 0 first_run (the game is held at its first frame until every shader of the game's data is prepared, once per GPU + driver), 1 background (as before), 2 off;  env SSBU_VK_PREPARE wins */
    int shader_load;        /* 2026-10-16 (appended): 0 start (on a prepared PC every shader of the game's data is loaded into memory before the first frame, behind a "LOADING SHADERS" screen: no shader work at the loading screens before a fight), 1 loading (as before: each at its loading screen);  env SSBU_VK_LOAD_AT_START=0/1 wins */
} GfxOptions;
const GfxOptions *gfx_options(void);      /* any thread; read once (the file is written with defaults when it does not exist) */
/* U1 (settings menu, menu.cpp): the keys of graphics.ini that are not in GfxOptions, and the writer */
int gfx_latency(void);                    /* latency = normal 0 | low 1 | lowest 2 */
int gfx_runahead(void);                   /* runahead = off 0 | on 1 */
int gfx_frame_rate(void);                 /* frame_rate = 30 | 60 (display.c: the present interval, halved and adaptive at 60) */
int gfx_present_api(void);               /* present_api = auto 0 | dxgi 1 | opengl 2 (nvn_gl_dxgi.c: how the picture reaches the screen) */
float gfx_widescreen(void);
int gfx_ff_speed(void);                   /* fast_forward = 2 | 3 | 4: the speed while the fast-forward key is held */
void gfx_ff_set(int n);                   /* changed while running (PC Settings) */               /* widescreen = off 0 | auto -1 | an aspect ratio (nvn_gl.c wide_k, display.c) */
int gfx_stats_overlay(void);              /* stats_overlay = off 0 | on 1 (menu.cpp, F2) */
int gfx_menu(void);                       /* menu = off 0 | on 1 (the in-game settings menu, F1) */
int gfx_volume(void);                     /* volume = auto -1 | 0..100 (the copies a person plays; nnaudio.c) */
const char *gfx_language(void);           /* language = auto "" | a language code of the game (nnmisc.c) */
void gfx_set_overlay(int on);             /* frame_time_overlay, changed while running */
int gfx_ini_set(const char *key, const char *value);      /* writes `key = value` into graphics.ini (the line replaced, comments kept; appended when missing); 1 = written */
#endif
