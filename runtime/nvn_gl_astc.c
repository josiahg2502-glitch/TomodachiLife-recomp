/* ASTC textures decoded on the GPU (included by nvn_gl.c before tex_upload_memory; render thread only).
 * Desktop GPUs have no ASTC, so every ASTC level was decoded in software on the render thread and uploaded as RGBA8:
 * a scene change (dozens of 1024x1024 textures at 8 ms each, or 1,300 small ones) cost 100-500 ms of one frame
 * (2026-10-30). Now the levels' blocks are queued and astc_decode.comp (the port of tex_decode.c's decoder, precompiled:
 * astc_decode_spv.h) decodes the whole queue in one dispatch before the next command that reads textures (ga_barrier:
 * exec, blits, copies, read-backs); each level is then uploaded into its texture from the dispatch's output. A first
 * form decoded into a staging image and copied with a barrier per texture: 1,339 small textures of the title screen held
 * the GPU 640 ms; the second stored into the textures with one dispatch per texture: 292 of them 94 ms.
 * TOMO_GPU_ASTC=0: the software decoder, as before. TOMO_GPU_ASTC=2 (TEST): every level is decoded both ways and
 * compared ("gpu-astc check" lines; the total in the report). */
#include "astc_decode_spv.h"
typedef void (APIENTRY *ga_shader_binary_t)(GLsizei, const GLuint *, GLenum, const void *, GLsizei);
typedef void (APIENTRY *ga_specialize_t)(GLuint, const char *, GLuint, const GLuint *, const GLuint *);
typedef void (APIENTRY *ga_u4i_t)(GLuint, GLint, GLint, GLint, GLint, GLint);
static struct {
    int state;                                     /* 0 not tried, 1 ready, -1 off (no driver support, or TOMO_GPU_ASTC=0) */
    int check;                                     /* TOMO_GPU_ASTC=2 */
    int pending;                                   /* levels queued (ga_flush) */
    GLuint prog, ssbo;
    GLuint ring; uint8_t *ring_map; size_t ring_pos; void *ring_fence[4];      /* the blocks (ga_ring) */
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
/* THE QUEUE (2026-10-31). The levels of the textures decoded since the last command that read textures wait here and go in
 * ONE dispatch (ga_flush, from ga_barrier: exec, blits, copies, read-backs): its texels into a buffer, then each level copied
 * into its texture (a buffer-to-texture upload: the bytes as they are, sRGB too). A dispatch per texture lasted as long as
 * one block's decode on one invocation however few blocks it had: the title's 292 small textures held the GPU 94 ms. */
#define GA_QMAX 4096
#define GA_LEVELS 8                            /* levels of one layer queued by one call */
#define GA_RING_PART ((size_t)16 << 20)            /* the block ring's parts (ga_ring) */
#define GA_STAGE ((size_t)8 << 20)                 /* texels a flush holds at most (32 MB), unless one texture alone has more */
static struct { Tex *t; int level, layer, w, h; size_t texel; } g_gq[GA_QMAX];
static GLint g_gq_lv[GA_QMAX][12];
static int g_gq_n, g_gq_groups;
static size_t g_gq_texels;
static GLuint g_ga_stage;
/* TEST ONLY, TOMO_GA_TIME=1: the GPU time of the decoder's dispatches per frame (a TIME_ELAPSED query around each, read
 * when the next frame's first flush comes: it waits for the GPU) */
static int g_gat_on = -1, g_gat_n;
static GLuint g_gat_q[512];
static long long g_gat_frame = -1, g_gat_blocks, g_gat_levels;
static void ga_time_flush(void) {
    uint64_t sum = 0, ns = 0;
    for (int i = 0; i < g_gat_n; i++) { glGetQueryObjectui64v(g_gat_q[i], 0x8866 /* QUERY_RESULT */, &ns); sum += ns; }
    if (g_gat_n) printf("ga-time: frame %lld: %d dispatches, %lld levels, %lld blocks, %.2f ms of GPU%c", g_gat_frame, g_gat_n, g_gat_levels, g_gat_blocks, (double)sum / 1e6, 10);
    g_gat_n = 0; g_gat_blocks = 0; g_gat_levels = 0;
}
static void ga_flush(void) {
    if (!g_gq_n) return;
    HS_IN("ASTC decode of the queued levels (dispatch, uploads)", 0);
    while (glGetError()) {}
    if (!g_ga_stage) glCreateBuffers(1, &g_ga_stage);
    glNamedBufferData(g_ga.ssbo, (ptrdiff_t)g_gq_n * 48, g_gq_lv, 0x88E0 /* STREAM_DRAW */);
    glNamedBufferData(g_ga_stage, (ptrdiff_t)(g_gq_texels * 4), NULL, 0x88E6 /* STREAM_COPY */);
    gs_ssbo(91, g_ga.ring, 0, (ptrdiff_t)(4 * GA_RING_PART));
    gs_ssbo(92, g_ga.ssbo, 0, (ptrdiff_t)g_gq_n * 48);
    gs_ssbo(93, g_ga_stage, 0, (ptrdiff_t)(g_gq_texels * 4));
    pair_off();
    glUseProgram(g_ga.prog);
    g_ga.u4i(g_ga.prog, 0, g_gq_n, 0, 0, 0);
    if (g_gat_on < 0) g_gat_on = getenv("TOMO_GA_TIME") != NULL && gl_has(glGetQueryObjectui64v, "glGetQueryObjectui64v");
    int timed = 0;
    if (g_gat_on) {
        if (g_gat_frame != (long long)g_st.frames) { ga_time_flush(); g_gat_frame = (long long)g_st.frames; }
        if (g_gat_n < 512) { if (!g_gat_q[g_gat_n]) glGenQueries(1, &g_gat_q[g_gat_n]); glBeginQuery(0x88BF /* TIME_ELAPSED */, g_gat_q[g_gat_n]); timed = 1; }
        g_gat_levels += g_gq_n;
        for (int i = 0; i < g_gq_n; i++) g_gat_blocks += g_gq_lv[i][5];
    }
    glDispatchCompute((GLuint)g_gq_groups, 1, 1);
    if (timed) { glEndQuery(0x88BF); g_gat_n++; }
    glUseProgram(0);
    glMemoryBarrier(0x80 /* PIXEL_BUFFER_BARRIER_BIT: the uploads below read what the dispatch wrote */);
    Fmt sw;
    memset(&sw, 0, sizeof sw);
    sw.format = 0x1908; sw.type = 0x1401; sw.bw = sw.bh = 1; sw.bpb = 4;      /* RGBA8, as the software decoder's levels */
    glBindBuffer(0x88EC /* PIXEL_UNPACK_BUFFER */, g_ga_stage);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
    for (int i = 0; i < g_gq_n; i++)
        if (g_gq[i].t->o.gl) tex_put(g_gq[i].t, &sw, g_gq[i].level, g_gq[i].layer, 0, 0, 0, g_gq[i].w, g_gq[i].h, 1, (const void *)(uintptr_t)(g_gq[i].texel * 4));
    glBindBuffer(0x88EC, 0);
    for (int i = 0; g_ga.check && i < g_gq_n; i++) {     /* TOMO_GPU_ASTC=2: each level against the software decoder (its blocks are still in the ring) */
        int w = g_gq[i].w, h = g_gq[i].h;
        size_t n = (size_t)w * h * 4;
        uint8_t *cpu = malloc(n), *gpu = malloc(n);
        tex_decode_rgba8(g_gq[i].t->b.format, g_ga.ring_map + (size_t)g_gq_lv[i][6] * 16, w, h, cpu);
        glGetTextureSubImage(g_gq[i].t->o.gl, g_gq[i].level, 0, 0, g_gq[i].layer, w, h, 1, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, (GLsizei)n, gpu);
        LONG64 bad = 0; size_t first = n;
        for (size_t k = 0; k < n; k += 4) if (memcmp(cpu + k, gpu + k, 4)) { bad++; if (first == n) first = k; }
        g_ga.check_levels++; g_ga.check_texels += (LONG64)w * h;
        if (bad) {
            static int nlog;
            g_ga.check_bad += bad;
            logf_limited(&nlog, 40, "gpu-astc check: %dx%d format 0x%x level %d (%d of a flush of %d): %lld of %lld texels differ; first at %zu,%zu: gpu %02x %02x %02x %02x, cpu %02x %02x %02x %02x\n",
                         w, h, g_gq[i].t->b.format, g_gq[i].level, i, g_gq_n, (long long)bad, (long long)w * h, (first / 4) % (size_t)w, (first / 4) / (size_t)w, gpu[first], gpu[first + 1], gpu[first + 2], gpu[first + 3], cpu[first], cpu[first + 1], cpu[first + 2], cpu[first + 3]);
        }
        free(cpu); free(gpu);
    }
    GLenum err = glGetError();
    if (err) { static int nlog; logf_limited(&nlog, 5, "nvn_gl: GPU ASTC decode of %d levels failed with GL error 0x%x\n", g_gq_n, (unsigned)err); }
    g_ga.dispatches++; g_ga.barriers++;
    g_gq_n = 0; g_gq_groups = 0; g_gq_texels = 0;
    g_ga.pending = 0;
    HS_OUT();
}
/* Before any GL command that reads a texture (a draw, a copy, a blit, a read-back): the queued levels are decoded and in place. */
static void ga_barrier(void) { if (g_gq_n) ga_flush(); }
/* The blocks go into a ring the CPU writes directly (persistently mapped, 4 parts of 16 MB; the queue is decoded before a
 * part is left, and entering a part waits for the fence set when it was last left, long passed). Returns the offset of
 * `total` bytes holding `blocks`, or -1. */
static ptrdiff_t ga_ring(const uint8_t *blocks, size_t total) {
    if (!g_ga.ring) {
        if (!gl_has(glNamedBufferStorage, "glNamedBufferStorage") || !gl_has(glMapNamedBufferRange, "glMapNamedBufferRange")) return -1;
        glCreateBuffers(1, &g_ga.ring);
        glNamedBufferStorage(g_ga.ring, (ptrdiff_t)(4 * GA_RING_PART), NULL, 0x2 | 0x40 | 0x80);      /* MAP_WRITE | PERSISTENT | COHERENT */
        g_ga.ring_map = glMapNamedBufferRange(g_ga.ring, 0, (ptrdiff_t)(4 * GA_RING_PART), 0x2 | 0x40 | 0x80);
        if (!g_ga.ring_map) { glDeleteBuffers(1, &g_ga.ring); g_ga.ring = 0; return -1; }
    }
    if (!g_ga.ring_map || total > GA_RING_PART) return -1;
    size_t pos = (g_ga.ring_pos + 255) & ~(size_t)255;
    int part = (int)(pos / GA_RING_PART);
    if (part > 3) part = 3;                                          /* (a write that ended at the ring's end: past it is not ours) */
    if (pos + total > (size_t)(part + 1) * GA_RING_PART) {           /* into the next part */
        ga_flush();
        g_ga.ring_fence[part] = glFenceSync(0x9117 /* SYNC_GPU_COMMANDS_COMPLETE */, 0);
        part = (part + 1) & 3; pos = (size_t)part * GA_RING_PART;
        if (g_ga.ring_fence[part]) { glClientWaitSync(g_ga.ring_fence[part], 1, 2000000000ull); glDeleteSync(g_ga.ring_fence[part]); g_ga.ring_fence[part] = 0; }
    }
    memcpy(g_ga.ring_map + pos, blocks, total);
    g_ga.ring_pos = pos + total;
    return (ptrdiff_t)pos;
}
/* The first nl levels of a layer, their deswizzled blocks at blocks + offs[l], queued; returns nl, or 0 = nothing done
 * (decode in software). (scratch: no longer used: the check runs at the flush.) */
static int ga_decode_levels(Tex *t, const TexFormat *tf, const TexLayout *L, const uint8_t *blocks, const size_t *offs, int nl, int layer, GLenum target, uint8_t *scratch) {
    (void)scratch;
    if (!ga_on() || !t->o.gl || t->rw || nl <= 0 || target == 0x806F /* TEXTURE_3D */) return 0;
    size_t total = 0, texels = 0;
    for (int l = 0; l < nl; l++) {
        int w = L->lvl_w[l], h = L->lvl_h[l], wb = (w + tf->bw - 1) / tf->bw, hb = (h + tf->bh - 1) / tf->bh;
        if (w <= 0 || h <= 0 || offs[l] % 16 || (size_t)wb * hb * 16 > L->lvl_linear_size[l]) return 0;
        if (offs[l] + L->lvl_linear_size[l] > total) total = offs[l] + L->lvl_linear_size[l];
        texels += (size_t)w * h;
    }
    if (total > GA_RING_PART || texels > ((size_t)64 << 20)) return 0;
    if (g_gq_n + nl > GA_QMAX || (g_gq_texels && g_gq_texels + texels > GA_STAGE)) ga_flush();
    ptrdiff_t at = ga_ring(blocks, total);
    if (at < 0) return 0;
    for (int l = 0; l < nl; l++) {
        int w = L->lvl_w[l], h = L->lvl_h[l], wb = (w + tf->bw - 1) / tf->bw, hb = (h + tf->bh - 1) / tf->bh;
        GLint *d = g_gq_lv[g_gq_n];
        d[0] = w; d[1] = h; d[2] = wb; d[3] = hb;
        d[4] = g_gq_groups; d[5] = wb * hb; d[6] = (GLint)(((size_t)at + offs[l]) / 16); d[7] = (GLint)g_gq_texels;
        d[8] = tf->bw; d[9] = tf->bh; d[10] = tf->srgb; d[11] = 0;
        g_gq[g_gq_n].t = t; g_gq[g_gq_n].level = l; g_gq[g_gq_n].layer = layer; g_gq[g_gq_n].w = w; g_gq[g_gq_n].h = h; g_gq[g_gq_n].texel = g_gq_texels;
        g_gq_groups += (wb * hb + 63) / 64;
        g_gq_texels += (size_t)w * h;
        g_gq_n++;
    }
    g_ga.pending = 1;
    g_ga.levels += nl; g_ga.bytes += (LONG64)total;
    return nl;
}
static void ga_report(FILE *out) {
    if (g_ga.state > 0) fprintf(out, "nvn_gl: ASTC levels decoded on the GPU: %lld in %lld dispatches (%.0f MB of blocks)%s", (long long)g_ga.levels, (long long)g_ga.dispatches, (double)g_ga.bytes / 1048576.0, "\n");
    if (g_ga.check) fprintf(out, "gpu-astc check: %lld levels, %lld texels, %lld texels differ from the software decoder\n", (long long)g_ga.check_levels, (long long)g_ga.check_texels, (long long)g_ga.check_bad);
}
