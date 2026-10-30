/* The in-game settings menu on the OpenGL renderer: draws what menu.cpp built (MenuDraw) over the presented picture.
 * Included by nvn_gl.c; called by job_present on the render thread between the picture's blit and SwapBuffers.
 *
 * The menu draws in a CONTEXT OF ITS OWN (sharing objects with the renderer's): the renderer caches the GL state it set
 * (gs_*, its pair programs, the ARB programs, blend and depth state), and nothing of the menu may change that. The order
 * on the GPU is kept with two fences: the menu waits for the picture, the renderer's SwapBuffers waits for the menu.
 * Costs nothing while the menu shows nothing (menu_frame returns 0).
 *
 * Shaders: SPIR-V made ahead of time (menu_gl_spv.h, scripts/gl_shaders.sh), taken by the driver through
 * GL_ARB_gl_spirv; without it the menu stays off and the log says so. */
#include "menu.h"
#include "menu_gl_spv.h"

typedef void (APIENTRY *mg_shader_binary_t)(GLsizei, const GLuint *, GLenum, const void *, GLsizei);
typedef void (APIENTRY *mg_specialize_t)(GLuint, const char *, GLuint, const GLuint *, const GLuint *);
typedef void (APIENTRY *mg_u4f_t)(GLuint, GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *mg_vao_t)(GLsizei, GLuint *);
typedef void (APIENTRY *mg_vab_t)(GLuint, GLuint, GLuint, ptrdiff_t, GLsizei);
typedef void (APIENTRY *mg_veb_t)(GLuint, GLuint);
typedef void (APIENTRY *mg_ena_t)(GLuint, GLuint);
typedef void (APIENTRY *mg_fmt_t)(GLuint, GLuint, GLint, GLenum, GLboolean, GLuint);
typedef void (APIENTRY *mg_bind_t)(GLuint, GLuint, GLuint);
typedef void (APIENTRY *mg_bindvao_t)(GLuint);
typedef void (APIENTRY *mg_texs_t)(GLenum, GLsizei, GLuint *);
typedef void (APIENTRY *mg_store_t)(GLuint, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *mg_sub_t)(GLuint, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (APIENTRY *mg_tpi_t)(GLuint, GLenum, GLint);
typedef void (APIENTRY *mg_unit_t)(GLuint, GLuint);
typedef void (APIENTRY *mg_dbv_t)(GLenum, GLsizei, GLenum, const void *, GLint);
typedef void (APIENTRY *mg_bfs_t)(GLenum, GLenum, GLenum, GLenum);
static struct {
    int state;                                     /* 0 not tried, 1 ready, -1 failed (the menu stays off) */
    HGLRC rc;
    GLuint prog, vao, vbo, ibo, font;
    int font_w, font_h;
    mg_u4f_t u4f; mg_bindvao_t bind_vao; mg_unit_t unit; mg_dbv_t draw; mg_bfs_t blend;
} g_mg;

static int mg_init(HGLRC main_rc) {
    g_mg.state = -1;
    if (!wglGetProcAddress("glSpecializeShader")) { printf("menu: OpenGL: the driver lacks GL_ARB_gl_spirv (OpenGL 4.6): no in-game menu\n"); return 0; }
    g_mg.rc = wglCreateContext(g_dc);
    if (!g_mg.rc || !wglShareLists(main_rc, g_mg.rc) || !wglMakeCurrent(g_dc, g_mg.rc)) { printf("menu: OpenGL: no context of its own (error %lu): no in-game menu\n", GetLastError()); return 0; }
    mg_shader_binary_t shader_binary = (mg_shader_binary_t)wglGetProcAddress("glShaderBinary");
    mg_specialize_t specialize = (mg_specialize_t)wglGetProcAddress("glSpecializeShader");
    mg_vao_t vaos = (mg_vao_t)wglGetProcAddress("glCreateVertexArrays");
    mg_vab_t vab = (mg_vab_t)wglGetProcAddress("glVertexArrayVertexBuffer");
    mg_veb_t veb = (mg_veb_t)wglGetProcAddress("glVertexArrayElementBuffer");
    mg_ena_t ena = (mg_ena_t)wglGetProcAddress("glEnableVertexArrayAttrib");
    mg_fmt_t fmt = (mg_fmt_t)wglGetProcAddress("glVertexArrayAttribFormat");
    mg_bind_t bind = (mg_bind_t)wglGetProcAddress("glVertexArrayAttribBinding");
    g_mg.u4f = (mg_u4f_t)wglGetProcAddress("glProgramUniform4f");
    g_mg.bind_vao = (mg_bindvao_t)wglGetProcAddress("glBindVertexArray");
    g_mg.unit = (mg_unit_t)wglGetProcAddress("glBindTextureUnit");
    g_mg.draw = (mg_dbv_t)wglGetProcAddress("glDrawElementsBaseVertex");
    g_mg.blend = (mg_bfs_t)wglGetProcAddress("glBlendFuncSeparate");
    GLuint sh[2] = {glCreateShader(0x8B31 /* VERTEX */), glCreateShader(0x8B30 /* FRAGMENT */)};
    shader_binary(1, &sh[0], 0x9551 /* SHADER_BINARY_FORMAT_SPIR_V */, k_menu_gl_vert_spv, (GLsizei)sizeof k_menu_gl_vert_spv);
    shader_binary(1, &sh[1], 0x9551, k_menu_gl_frag_spv, (GLsizei)sizeof k_menu_gl_frag_spv);
    specialize(sh[0], "main", 0, NULL, NULL);
    specialize(sh[1], "main", 0, NULL, NULL);
    GLint ok = 0;
    g_mg.prog = glCreateProgram();
    glAttachShader(g_mg.prog, sh[0]); glAttachShader(g_mg.prog, sh[1]);
    glLinkProgram(g_mg.prog);
    glGetProgramiv(g_mg.prog, 0x8B82 /* LINK_STATUS */, &ok);
    glDeleteShader(sh[0]); glDeleteShader(sh[1]);
    if (!ok) { char log[512] = ""; glGetProgramInfoLog(g_mg.prog, sizeof log, NULL, log); printf("menu: OpenGL: the menu's shaders do not link: %s\n", log); wglMakeCurrent(g_dc, main_rc); return 0; }
    vaos(1, &g_mg.vao);
    glCreateBuffers(1, &g_mg.vbo); glCreateBuffers(1, &g_mg.ibo);
    vab(g_mg.vao, 0, g_mg.vbo, 0, 20);
    veb(g_mg.vao, g_mg.ibo);
    for (GLuint a = 0; a < 3; a++) { ena(g_mg.vao, a); bind(g_mg.vao, a, 0); }
    fmt(g_mg.vao, 0, 2, GL_FLOAT, GL_FALSE, 0);
    fmt(g_mg.vao, 1, 2, GL_FLOAT, GL_FALSE, 8);
    fmt(g_mg.vao, 2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 16);
    wglMakeCurrent(g_dc, main_rc);
    g_mg.state = 1;
    printf("menu: OpenGL: ready (own context, SPIR-V shaders)\n");
    return 1;
}

static void mg_font(void) {                       /* the atlas: made once, again when menu.cpp rebuilds it */
    int w = 0, h = 0;
    const unsigned char *px = menu_font_rgba(&w, &h);
    if (!px || (g_mg.font && w == g_mg.font_w && h == g_mg.font_h)) return;
    mg_texs_t texs = (mg_texs_t)wglGetProcAddress("glCreateTextures");
    mg_store_t store = (mg_store_t)wglGetProcAddress("glTextureStorage2D");
    mg_sub_t sub = (mg_sub_t)wglGetProcAddress("glTextureSubImage2D");
    mg_tpi_t tpi = (mg_tpi_t)wglGetProcAddress("glTextureParameteri");
    if (g_mg.font) glDeleteTextures(1, &g_mg.font);
    texs(GL_TEXTURE_2D, 1, &g_mg.font);
    store(g_mg.font, 1, 0x8058 /* RGBA8 */, w, h);
    sub(g_mg.font, 0, 0, 0, w, h, 0x1908 /* RGBA */, GL_UNSIGNED_BYTE, px);
    tpi(g_mg.font, GL_TEXTURE_MIN_FILTER, GL_LINEAR); tpi(g_mg.font, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    tpi(g_mg.font, GL_TEXTURE_WRAP_S, 0x812F /* CLAMP_TO_EDGE */); tpi(g_mg.font, GL_TEXTURE_WRAP_T, 0x812F);
    g_mg.font_w = w; g_mg.font_h = h;
}

static void mg_shot(int W, int H) {               /* SSBU_MENU_TEST: the window's picture with the menu, as a BMP */
    char path[MAX_PATH];
    if (!menu_shot_wanted(path, sizeof path)) return;
    unsigned char *px = malloc((size_t)W * H * 3);
    FILE *f = px ? fopen(path, "wb") : NULL;
    if (f) {
        int row = (W * 3 + 3) & ~3;
        uint32_t size = 54 + (uint32_t)row * H;
        uint8_t hd[54] = {'B', 'M'};
        memcpy(hd + 2, &size, 4); hd[10] = 54; hd[14] = 40;
        memcpy(hd + 18, &W, 4); memcpy(hd + 22, &H, 4); hd[26] = 1; hd[28] = 24;
        fwrite(hd, 1, 54, f);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, W, H, 0x80E0 /* BGR */, GL_UNSIGNED_BYTE, px);      /* bottom-up rows, as a BMP stores them */
        for (int y = 0; y < H; y++) { fwrite(px + (size_t)y * W * 3, 1, (size_t)W * 3, f); fwrite("\0\0\0", 1, (size_t)(row - W * 3), f); }
        fclose(f);
    }
    free(px);
    menu_shot_done(path, W, H, f != NULL);
}

static void menu_gl_draw(int W, int H) {
    MenuDraw d;
    if (g_mg.state < 0 || !menu_enabled() || W <= 0 || H <= 0) return;
    if (!menu_frame(W, H, &d) || !d.nidx || !d.nvtx) return;
    HGLRC main_rc = wglGetCurrentContext();
    if (!g_mg.state && !mg_init(main_rc)) return;
    void *picture = glFenceSync(0x9117 /* SYNC_GPU_COMMANDS_COMPLETE */, 0);
    glFlush();
    if (!wglMakeCurrent(g_dc, g_mg.rc)) { glDeleteSync(picture); return; }
    glWaitSync(picture, 0, 0xFFFFFFFFFFFFFFFFull);
    glDeleteSync(picture);
    mg_font();
    glNamedBufferData(g_mg.vbo, (ptrdiff_t)d.nvtx * 20, d.vtx, 0x88E0 /* STREAM_DRAW */);
    glNamedBufferData(g_mg.ibo, (ptrdiff_t)d.nidx * 2, d.idx, 0x88E0);
    glBindFramebuffer(GL_FRAMEBUFFER_, 0);
    glViewport(0, 0, W, H);
    glEnable(GL_BLEND);
    g_mg.blend(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_SCISSOR_TEST);
    glUseProgram(g_mg.prog);
    g_mg.u4f(g_mg.prog, 0, 2.0f / d.disp_w, -2.0f / d.disp_h, -1.0f, 1.0f);
    g_mg.bind_vao(g_mg.vao);
    g_mg.unit(0, g_mg.font);
    for (int i = 0; i < d.ncmd; i++) {
        const MenuCmd *c = &d.cmd[i];
        float x0 = c->clip[0] < 0 ? 0 : c->clip[0], y0 = c->clip[1] < 0 ? 0 : c->clip[1];
        float x1 = c->clip[2] > (float)W ? (float)W : c->clip[2], y1 = c->clip[3] > (float)H ? (float)H : c->clip[3];
        if (x1 <= x0 || y1 <= y0) continue;
        glScissor((GLint)x0, (GLint)((float)H - y1), (GLsizei)(x1 - x0), (GLsizei)(y1 - y0));
        g_mg.draw(GL_TRIANGLES, (GLsizei)c->count, GL_UNSIGNED_SHORT, (const void *)(uintptr_t)(c->idx_off * 2u), (GLint)c->vtx_off);
    }
    mg_shot(W, H);
    void *menu = glFenceSync(0x9117, 0);
    glFlush();
    wglMakeCurrent(g_dc, main_rc);
    glWaitSync(menu, 0, 0xFFFFFFFFFFFFFFFFull);
    glDeleteSync(menu);
}
