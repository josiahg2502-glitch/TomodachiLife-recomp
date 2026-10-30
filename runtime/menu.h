/* U1: the in-game settings menu (Dear ImGui over the game's picture, plan\phases\U1_settings_menu.md).
 * menu.cpp holds the interface and the settings; nvn_vk_menu.c (Vulkan) or nvn_gl_menu.c (OpenGL) draws what it built.
 * Off unless SSBU_MENU=1 or graphics.ini `menu = on` (SSBU_MENU=0 wins over the file). */
#ifndef MENU_H
#define MENU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* any thread */
int  menu_enabled(void);                /* the menu exists in this run (read once) */
int  menu_is_open(void);
int  menu_input_blocked(void);          /* nnhid.c: the game gets no input from the real pads and keyboard (open, or a button still held after closing) */
void menu_request_toggle(void);         /* F1 (display.c) or the pad combination: applied at the next drawn frame */
/* R3 (rb_net.c): a message box in the middle of the picture while the game itself shows nothing new (the online match
 * waiting for the other player). NULL or "" removes it. The caller redraws the last frame (nvn_front_osk_redraw). */
void menu_notice(const char *text);
/* O1 (net_relay.c): a small box at the top of the picture for 7 s, over the game, taking no input ("Cannot join this
 * room" + the cause). Any thread; a new one replaces the old. */
void menu_popup(const char *title, const char *text);

/* display.c, window thread: 1 = the message was taken by the menu (the window procedure returns 0) */
int  menu_wndproc(void *hwnd, unsigned msg, uintptr_t w, intptr_t l);

/* render thread (nvn_vk_menu.c) */
typedef struct { float clip[4]; uint32_t idx_off, vtx_off, count; } MenuCmd;
typedef struct {
    const void *vtx; const uint16_t *idx; const MenuCmd *cmd;   /* vertices: 20 bytes each (pos x y, uv u v, colour RGBA8) */
    int nvtx, nidx, ncmd;
    float disp_w, disp_h;
} MenuDraw;
int  menu_frame(int width, int height, MenuDraw *out);      /* 1 = something to draw this frame (out filled, valid until the next call) */
const unsigned char *menu_font_rgba(int *w, int *h);         /* the font atlas, after the first menu_frame that returned 1 */
int  menu_shot_wanted(char *path, int n);                    /* SSBU_MENU_TEST: 1 = read the swapchain image back after drawing this frame, into path */
void menu_shot_done(const char *path, int w, int h, int ok); /* the BMP was written (or not) */

#ifdef __cplusplus
}
#endif
#endif
