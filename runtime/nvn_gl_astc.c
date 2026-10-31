/* ASTC textures decoded on the GPU (included by nvn_gl.c before tex_upload_memory; render thread only).
 * Desktop GPUs have no ASTC, so every ASTC level was decoded in software on the render thread and uploaded as RGBA8:
 * a scene change (dozens of 1024x1024 textures at 8 ms each, or 1,300 small ones) cost 100-500 ms of one frame
 * (2026-10-30). Now the level's blocks go into a buffer and astc_decode.comp (the port of tex_decode.c's decoder,
 * precompiled: astc_decode_spv.h) stores the texels straight into the texture's level (an sRGB texture through an RGBA8
 * view of that level: the same bytes). The dispatches of a batch run back to back; one memory barrier before the next
 * command that reads textures (ga_barrier: exec, blits, copies, read-backs). A first form decoded into a staging image
 * and copied, with a barrier per texture: 1,339 small textures of the title screen then held the GPU 640 ms.
 * TOMO_GPU_ASTC=0: the software decoder, as before. TOMO_GPU_ASTC=2 (TEST): every level is decoded both ways and
 * compared ("gpu-astc check" lines; the total in the report). */
#include "astc_decode_spv.h"
typedef void (APIENTRY *ga_shader_binary_t)(GLsizei, const GLuint *, GLenum, const void *, GLsizei);
typedef void (APIENTRY *ga_specialize_t)(GLuint, const char *, GLuint, const GLuint *, const GLuint *);
typedef void (APIENTRY *ga_u4i_t)(GLuint, GLint, GLint, GLint, GLint, GLint);
static struct {
    int state;                                     /* 0 not tried, 1 ready, -1 off (no driver support, or TOMO_GPU_ASTC=0) */
    int check;                                     /* TOMO_GPU_ASTC=2 */
    int pending;                                   /* image stores issued since the last barrier */
    GLuint prog, ssbo;
    ga_u4i_t u4i;
    LONG64 levels, bytes, barriers, dispatches, check_levels, check_bad, check_texels;
} g_ga;
static void pair_off(void);                        /* nvn_gl.c, below */
static void gs_ssbo(GLuint i, GLuint buf, ptrdiff_t off, ptrdiff_t size);
static int ga_init(void) {
    const char *e = getenv("TOMO_GPU_ASTC");
    g_ga.state = -1;
    if (e && e[0] == '0') { printf("nvn_gl: ASTC textures decoded in software (TOMO_GPU_ASTC=0)\n"); return 0; }
    g_ga.check = e && e[0] == '2';
    ga_shader_binary_t shader_binary = (ga_shader_binary_t)wglGetProcAddress("glShaderBinary");
    ga_specialize_t specialize = (ga_specialize_t)wglGetProcAddress("glSpecializeShader");
    g_ga.u4i = (ga_u4i_t)wglGetProcAddress("glProgramUniform4i");
    if (!shader_binary || !specialize || !g_ga.u4i || !glDispatchCompute || !glBindImageTexture || !glMemoryBarrier || !glTextureView) {
        printf("nvn_gl: ASTC textures decoded in software: the driver lacks GL_ARB_gl_spirv, compute or texture views\n");
        return 0;
    }
    while (glGetError()) {}
    GLuint sh = glCreateShader(0x91B9 /* COMPUTE_SHADER */);
    shader_binary(1, &sh, 0x9551 /* SHADER_BINARY_FORMAT_SPIR_V */, k_astc_decode_spv, (GLsizei)sizeof k_astc_decode_spv);
    specialize(sh, "main", 0, NULL, NULL);
    GLint ok = 0;
    g_ga.prog = glCreateProgram();
    glAttachShader(g_ga.prog, sh);
    glLinkProgram(g_ga.prog);
    glGetProgramiv(g_ga.prog, 0x8B82 /* LINK_STATUS */, &ok);
    glDeleteShader(sh);
    if (!ok || glGetError()) { char log[512] = ""; glGetProgramInfoLog(g_ga.prog, sizeof log, NULL, log); printf("nvn_gl: ASTC textures decoded in software: the GPU decoder does not link: %s\n", log); return 0; }
    glCreateBuffers(1, &g_ga.ssbo);
    g_ga.state = 1;
    printf("nvn_gl: ASTC textures decoded on the GPU (astc_decode.comp)%s\n", g_ga.check ? "; every level checked against the software decoder" : "");
    return 1;
}
static int ga_on(void) { if (g_ga.state) return g_ga.state > 0; HS_IN("ASTC decoder made (driver)", 0); int ok = ga_init(); HS_OUT(); return ok; }
/* Before any GL command that reads a texture (a draw, a copy, a blit, a read-back): the texels stored by the decoder are visible. */
static void ga_barrier(void) {
    if (!g_ga.pending) return;
    glMemoryBarrier(0xFFFFFFFFu /* ALL_BARRIER_BITS */);
    g_ga.pending = 0; g_ga.barriers++;
}
#define GA_LEVELS 8                            /* levels of one layer decoded by one dispatch (one image unit each) */
typedef void (APIENTRY *ga_u4iv_t)(GLuint, GLint, GLsizei, const GLint *);
/* The first nl levels of a layer, their deswizzled blocks at blocks + offs[l], decoded by ONE dispatch; returns nl, or 0 =
 * nothing done (decode in software). scratch: lvl_w[0] * lvl_h[0] * 4 bytes for the check mode's software decode. */
static int ga_decode_levels(Tex *t, const TexFormat *tf, const TexLayout *L, const uint8_t *blocks, const size_t *offs, int nl, int layer, GLenum target, uint8_t *scratch) {
    static ga_u4iv_t u4iv;
    if (!ga_on() || !t->o.gl || nl <= 0 || target == 0x806F /* TEXTURE_3D */) return 0;
    if (!u4iv && !(u4iv = (ga_u4iv_t)wglGetProcAddress("glProgramUniform4iv"))) return 0;
    GLint lv[17][4];
    int groups = 0;
    size_t total = 0;
    for (int l = 0; l < nl; l++) {
        int w = L->lvl_w[l], h = L->lvl_h[l], wb = (w + tf->bw - 1) / tf->bw, hb = (h + tf->bh - 1) / tf->bh;
        if (w <= 0 || h <= 0 || offs[l] % 16 || (size_t)wb * hb * 16 > L->lvl_linear_size[l]) return 0;
        lv[2 * l][0] = w; lv[2 * l][1] = h; lv[2 * l][2] = wb; lv[2 * l][3] = hb;
        lv[2 * l + 1][0] = groups; lv[2 * l + 1][1] = wb * hb; lv[2 * l + 1][2] = (GLint)(offs[l] / 16); lv[2 * l + 1][3] = 0;
        groups += (wb * hb + 63) / 64;
        if (offs[l] + L->lvl_linear_size[l] > total) total = offs[l] + L->lvl_linear_size[l];
    }
    for (int l = nl; l < 8; l++) memset(lv[2 * l], 0, 32);
    lv[16][0] = tf->bw; lv[16][1] = tf->bh; lv[16][2] = nl; lv[16][3] = tf->srgb;
    if (total > (size_t)INT32_MAX) return 0;
    while (glGetError()) {}
    GLuint view = 0, image = t->o.gl;
    if (tf->srgb) {                                /* an sRGB texture cannot be an image: an RGBA8 view of its levels is (the same bytes) */
        glGenTextures(1, &view);
        glTextureView(view, GL_TEXTURE_2D, t->o.gl, 0x8058 /* RGBA8 */, 0, (GLuint)nl, (GLuint)layer, 1);
        if (glGetError()) { glDeleteTextures(1, &view); static int nlog; logf_limited(&nlog, 5, "nvn_gl: GPU ASTC decode: no RGBA8 view of a %dx%d texture (target %d): software decode\n", L->lvl_w[0], L->lvl_h[0], (int)target); return 0; }
        image = view;
    }
    glNamedBufferData(g_ga.ssbo, (ptrdiff_t)total, blocks, 0x88E0 /* STREAM_DRAW */);
    gs_ssbo(91, g_ga.ssbo, 0, (ptrdiff_t)total);
    pair_off();
    glUseProgram(g_ga.prog);
    u4iv(g_ga.prog, 0, 17, &lv[0][0]);
    for (int l = 0; l < nl; l++) glBindImageTexture((GLuint)l, image, l, GL_FALSE, view ? 0 : layer, 0x88B9 /* WRITE_ONLY */, 0x8058 /* RGBA8 */);
    glDispatchCompute((GLuint)groups, 1, 1);
    for (int l = 0; l < nl; l++) glBindImageTexture((GLuint)l, 0, 0, GL_FALSE, 0, 0x88B8 /* READ_ONLY */, 0x8058);
    glUseProgram(0);
    if (view) glDeleteTextures(1, &view);          /* (the GPU keeps it until the dispatch is done) */
    GLenum err = glGetError();
    if (err) {
        static int nlog;
        logf_limited(&nlog, 5, "nvn_gl: GPU ASTC decode of a %dx%d texture (%d levels, target %d, layer %d) failed with GL error 0x%x: software decode\n", L->lvl_w[0], L->lvl_h[0], nl, (int)target, layer, (unsigned)err);
        return 0;
    }
    g_ga.pending = 1;
    g_ga.levels += nl; g_ga.bytes += (LONG64)total; g_ga.dispatches++;
    if (g_ga.check) {
        ga_barrier();
        for (int l = 0; l < nl; l++) {
            int w = L->lvl_w[l], h = L->lvl_h[l];
            size_t n = (size_t)w * h * 4;
            uint8_t *cpu = scratch, *gpu = malloc(n);
            tex_decode_rgba8(t->b.format, blocks + offs[l], w, h, cpu);
            glGetTextureSubImage(t->o.gl, l, 0, 0, layer, w, h, 1, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, (GLsizei)n, gpu);
            LONG64 bad = 0; size_t first = n;
            for (size_t i = 0; i < n; i += 4) if (memcmp(cpu + i, gpu + i, 4)) { bad++; if (first == n) first = i; }
            g_ga.check_levels++; g_ga.check_texels += (LONG64)w * h;
            if (bad) {
                static int nlog;
                g_ga.check_bad += bad;
                logf_limited(&nlog, 40, "gpu-astc check: %dx%d format 0x%x level %d: %lld of %lld texels differ; first at %zu,%zu: gpu %02x %02x %02x %02x, cpu %02x %02x %02x %02x\n",
                             w, h, t->b.format, l, (long long)bad, (long long)w * h, (first / 4) % (size_t)w, (first / 4) / (size_t)w, gpu[first], gpu[first + 1], gpu[first + 2], gpu[first + 3], cpu[first], cpu[first + 1], cpu[first + 2], cpu[first + 3]);
            }
            free(gpu);
        }
    }
    return nl;
}
static void ga_report(FILE *out) {
    if (g_ga.state > 0) fprintf(out, "nvn_gl: ASTC levels decoded on the GPU: %lld in %lld dispatches (%.0f MB of blocks), %lld barriers%s", (long long)g_ga.levels, (long long)g_ga.dispatches, (double)g_ga.bytes / 1048576.0, (long long)g_ga.barriers, "\n");
    if (g_ga.check) fprintf(out, "gpu-astc check: %lld levels, %lld texels, %lld texels differ from the software decoder\n", (long long)g_ga.check_levels, (long long)g_ga.check_texels, (long long)g_ga.check_bad);
}
