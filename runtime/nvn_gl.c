/* NVN on OpenGL: the game's graphics calls drawn for real.
 *
 * How it fits together
 *   - One render thread owns the OpenGL context (4.6 compatibility, NVIDIA). Guest threads hand it work with
 *     gl_run() and wait for it.
 *   - Shaders: only about 20 of the game's ~8200 shaders carry NV_gpu_program5 assembly text in their control
 *     section; all of them carry Maxwell machine code. The machine code is translated to GLSL (mx_translate.c,
 *     tested offline by mxtest.exe on every dumped stage) and used as separable programs in one pipeline. The assembly-text path
 *     (glProgramStringARB) is kept as a fallback for pairs where both stages have text.
 *   - Hardware constant buffers as the machine code sees them: 1 = constants stored after the shader's code
 *     (also jump tables), 2 = texture handles (8 bytes per unit from byte 32), 3 + n = uniform buffer n bound
 *     by the game. They are bound as storage blocks (binding 20 * stage + index); the game's storage buffers
 *     (LDG / STG in machine code) as storage blocks at 40 + 16 * stage + index. Alpha test is done by the
 *     translated fragment shader (uniform at location 0), not by fixed function.
 *   - Command buffers are recorded into host lists and replayed at nvnQueueSubmitCommands.
 *   - Buffers are guest memory: at draw time the bound ranges are copied into a streaming GL buffer.
 *   - Textures are created lazily; texels come from the pool memory (Tegra block-linear layout) and are
 *     re-read when that memory changes. Samplers + textures become bindless handles.
 *   - Window origin is upper-left and depth is 0..1 (glClipControl), so row 0 of every GL texture is the
 *     top row, as in NVN; only the final blit to the window is flipped.
 *
 * Debug aids (environment): SSBU_SHOT=<n> / SSBU_SHOT_EVERY=<k> save presented frames as frame_<n>.bmp (in SSBU_SHOT_DIR if set, from frame SSBU_SHOT_FROM on, up to SSBU_SHOT_TO);
 * SSBU_RTDUMP=<n> saves every colour target at frame n (SSBU_RTDUMP_ALPHA=1: also its alpha as _a.bmp); SSBU_DRAWSHOT=<n> the target after each draw
 * (with SSBU_DRAWSHOT_CLEAR, cleared before each); SSBU_DRAWLOG=<n> prints state, uniforms and textures of
 * every draw of frame n (with SSBU_DRAWLOG_TEX=1 also the textures as tex_*.rgba); SSBU_SHADERDUMP=<dir> writes shader text,
 * machine code and generated GLSL; SSBU_NOSCISSOR, SSBU_SKIPDRAW=<i>, SSBU_FRAGDEBUG (magenta fragments),
 * SSBU_GLDEBUG (driver messages, error check after every command); SSBU_NOCOMPUTE skips every compute dispatch (fighters then
 * stay in their bind pose); SSBU_DRAWLOG_MINDISP=<n> makes SSBU_DRAWLOG count frames with at least n dispatches (the draw log
 * lists each dispatch with its buffers and marks vertex buffers written by compute); every 300 frames a line with the frame
 * rate. SSBU_NVN_MISSING=warn (nvn_stub.c): a call of an NVN function without an implementation does not stop the game.
 *
 * Compute (nvnCommandBufferDispatchCompute): see the block above compute_dispatch. The five compute programs built into the
 * executable carry NV assembly text and run through GL_NV_compute_program5; the others are Maxwell machine code without a
 * program header (control +0x714 = 5, work group size at control +0x748) translated by mx_translate.c. Storage buffers a
 * dispatch writes are copied back to guest memory before the next draw / copy and at the end of the batch.
 * The whole API surface is audited in plan\phases\G1_nvn_audit.md (python nvn_audit.py).
 *
 * NVN enum values and argument orders: public nvn.h (open-ead/nnheaders), checked against the trace.
 * Maxwell encodings: envytools envydis/gm107.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <GL/gl.h>
#include "runtime.h"
#include "hostrt.h"
#include "mx_translate.h"
#include "gfx_options.h"
#include "nvn_front.h"
#include "tex_decode.h"
#include "nvn_sdk.h"
#include "nvn_c4.h"

#pragma comment(lib, "opengl32.lib")

/* ask laptop drivers for the discrete GPU */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define A6 (c->x[6])
#define A7 (c->x[7])
#define F0 (c->v[0].f[0])
#define F1 (c->v[1].f[0])
#define F2 (c->v[2].f[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(a) ((void *)(uintptr_t)(a))

int nvn_pool_range(uint64_t pool, uint64_t *mem, uint64_t *size);     /* nvn_stub.c */
uint64_t nvn_pool_flags_of(uint64_t pool);
void input_frame_draws(int draws);                                    /* nnhid.c */
void display_pace(void);                                              /* display.c */
int display_present_defer(void); int display_present_due(void); void display_present_flushed(void); void display_present_done(int fight);      /* display.c: the present at the tick */
int display_pace_lead(void); int display_wait_present_due(int max_ms);      /* display.c: TOMO_PACE_LEAD */
extern volatile LONG g_display_holding;                              /* display.c: a finished frame is held for its tick (hitch.c: pacing, not a cost) */
int display_wait_refresh(void);                                       /* display.c, P3 round 3: one refresh clock */
void display_before_present(void);
void display_clock_sample(uint64_t index_ptr);                        /* SSBU_CLOCKSTAT */
static void *volatile g_window_sync;                                  /* the sync object of the last nvnWindowAcquireTexture */
void display_set_interval(int n);
/* V1 (plan\phases\V1_vulkan.md, seam step S0): SSBU_RENDERER=vk. The render thread then starts nvn_vk.c instead of an
 * OpenGL context and its jobs go through vk_route (end of this file); nothing else in this file changes behaviour. */
int nvn_vk_init(void *hwnd);
void nvn_vk_present_black(void);
void nvn_vk_flush(int finish);
void nvn_vk_shutdown(void);
void nvn_vk_missing(const char *what);
void nvn_vk_report(FILE *out);
static int g_vk;
const RBackend *g_rb = &rb_gl;                 /* seam step S2: chosen once in nvn_gl_start (SSBU_RENDERER) */
static void vk_shutdown_at_exit(void);

/* ---------------------------------------------------------------- OpenGL beyond 1.1 */
#define GLF(X) \
    X(void, GenProgramsARB, (GLsizei, GLuint *)) \
    X(void, DeleteProgramsARB, (GLsizei, const GLuint *)) \
    X(void, BindProgramARB, (GLenum, GLuint)) \
    X(void, ProgramStringARB, (GLenum, GLenum, GLsizei, const void *)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, ptrdiff_t, const void *, GLenum)) \
    X(void, BufferSubData, (GLenum, ptrdiff_t, ptrdiff_t, const void *)) \
    X(void, BindBufferRange, (GLenum, GLuint, GLuint, ptrdiff_t, ptrdiff_t)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture, (GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, DrawBuffers, (GLsizei, const GLenum *)) \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, TexStorage1D, (GLenum, GLsizei, GLenum, GLsizei)) \
    X(void, TexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, TexStorage3D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei)) \
    X(void, TexStorage2DMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean)) \
    X(void, TexStorage3DMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLboolean)) \
    X(void, TexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, CompressedTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *)) \
    X(void, CompressedTexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, const void *)) \
    X(void, TextureView, (GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint)) \
    X(void, GenSamplers, (GLsizei, GLuint *)) \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint)) \
    X(void, SamplerParameterf, (GLuint, GLenum, GLfloat)) \
    X(void, SamplerParameterfv, (GLuint, GLenum, const GLfloat *)) \
    X(uint64_t, GetTextureSamplerHandleARB, (GLuint, GLuint)) \
    X(uint64_t, GetTextureHandleARB, (GLuint)) \
    X(void, MakeTextureHandleResidentARB, (uint64_t)) \
    X(void, MakeTextureHandleNonResidentARB, (uint64_t)) \
    X(GLboolean, IsTextureHandleResidentARB, (uint64_t)) \
    X(void, VertexAttribFormat, (GLuint, GLint, GLenum, GLboolean, GLuint)) \
    X(void, VertexAttribIFormat, (GLuint, GLint, GLenum, GLuint)) \
    X(void, VertexAttribBinding, (GLuint, GLuint)) \
    X(void, BindVertexBuffer, (GLuint, GLuint, ptrdiff_t, GLsizei)) \
    X(void, VertexBindingDivisor, (GLuint, GLuint)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, DisableVertexAttribArray, (GLuint)) \
    X(void, DrawElementsInstancedBaseVertexBaseInstance, (GLenum, GLsizei, GLenum, const void *, GLsizei, GLint, GLuint)) \
    X(void, DrawArraysInstancedBaseInstance, (GLenum, GLint, GLsizei, GLsizei, GLuint)) \
    X(void, BlendFuncSeparatei, (GLuint, GLenum, GLenum, GLenum, GLenum)) \
    X(void, BlendEquationSeparatei, (GLuint, GLenum, GLenum)) \
    X(void, Enablei, (GLenum, GLuint)) \
    X(void, Disablei, (GLenum, GLuint)) \
    X(void, ColorMaski, (GLuint, GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, StencilFuncSeparate, (GLenum, GLenum, GLint, GLuint)) \
    X(void, StencilOpSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, StencilMaskSeparate, (GLenum, GLuint)) \
    X(void, ClearBufferfv, (GLenum, GLint, const GLfloat *)) \
    X(void, ViewportIndexedf, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, ScissorIndexed, (GLuint, GLint, GLint, GLsizei, GLsizei)) \
    X(void, DepthRangeIndexed, (GLuint, GLdouble, GLdouble)) \
    X(void, ClipControl, (GLenum, GLenum)) \
    X(void, PrimitiveRestartIndex, (GLuint)) \
    X(void, PatchParameteri, (GLenum, GLint)) \
    X(void, DispatchCompute, (GLuint, GLuint, GLuint)) \
    X(void, CreateBuffers, (GLsizei, GLuint *)) \
    X(void, DeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, NamedBufferData, (GLuint, ptrdiff_t, const void *, GLenum)) \
    X(void, NamedBufferSubData, (GLuint, ptrdiff_t, ptrdiff_t, const void *)) \
    X(void, GetNamedBufferSubData, (GLuint, ptrdiff_t, ptrdiff_t, void *)) \
    X(void, NamedBufferStorage, (GLuint, ptrdiff_t, const void *, GLbitfield)) \
    X(void *, MapNamedBufferRange, (GLuint, ptrdiff_t, ptrdiff_t, GLbitfield)) \
    X(void, CopyNamedBufferSubData, (GLuint, GLuint, ptrdiff_t, ptrdiff_t, ptrdiff_t)) \
    X(void, MemoryBarrier, (GLbitfield)) \
    X(void, BindImageTexture, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum)) \
    X(void, DebugMessageCallback, (void *, const void *))     X(GLuint, CreateShaderProgramv, (GLenum, GLsizei, const char *const *))     X(void, GenProgramPipelines, (GLsizei, GLuint *))     X(void, BindProgramPipeline, (GLuint))     X(void, UseProgramStages, (GLuint, GLbitfield, GLuint))     X(void, GetProgramiv, (GLuint, GLenum, GLint *))     X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *))     X(void, DeleteProgram, (GLuint))     X(void, ProgramUniform2ui, (GLuint, GLint, GLuint, GLuint))     X(void, ProgramUniform1f, (GLuint, GLint, GLfloat))     X(void, ProgramUniform2f, (GLuint, GLint, GLfloat, GLfloat))     X(void, PolygonOffsetClamp, (GLfloat, GLfloat, GLfloat))     X(void, ClearBufferiv, (GLenum, GLint, const GLint *))     X(void, ClearBufferuiv, (GLenum, GLint, const GLuint *))     X(void, SampleMaski, (GLuint, GLbitfield))     X(void, GetTextureSubImage, (GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, GLsizei, void *))     X(void, GetCompressedTextureSubImage, (GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLsizei, void *))     X(void, GenQueries, (GLsizei, GLuint *))     X(void, BeginQuery, (GLenum, GLuint))     X(void, EndQuery, (GLenum))     X(void, GetQueryObjectuiv, (GLuint, GLenum, GLuint *))     X(void, QueryCounter, (GLuint, GLenum))
#define X(ret, name, args) static ret (APIENTRY *gl##name) args;
GLF(X)
#undef X
/* OpenGL entry points that only some NVN features need: looked up without stopping when one is absent (gl_has reports a use). */
#define GLO(X) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const char *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, DetachShader, (GLuint, GLuint)) \
    X(void, DeleteShader, (GLuint)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, ProgramParameteri, (GLuint, GLenum, GLint)) \
    X(void, GetProgramBinary, (GLuint, GLsizei, GLsizei *, GLenum *, void *)) \
    X(void, ProgramBinary, (GLuint, GLenum, const void *, GLsizei)) \
    X(void, UseProgram, (GLuint)) \
    X(void, DepthBoundsEXT, (GLdouble, GLdouble)) \
    X(void, ConservativeRasterParameterfNV, (GLenum, GLfloat)) \
    X(void, SubpixelPrecisionBiasNV, (GLuint, GLuint)) \
    X(void, ViewportSwizzleNV, (GLuint, GLenum, GLenum, GLenum, GLenum)) \
    X(void, PatchParameterfv, (GLenum, const GLfloat *)) \
    X(void, TextureBarrier, (void)) \
    X(void, InvalidateFramebuffer, (GLenum, GLsizei, const GLenum *)) \
    X(void, FramebufferTextureLayer, (GLenum, GLenum, GLuint, GLint, GLint)) \
    X(void, CopyImageSubData, (GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei)) \
    X(void, ClearTexImage, (GLuint, GLint, GLenum, GLenum, const void *)) \
    X(void *, FenceSync, (GLenum, GLbitfield)) \
    X(GLenum, ClientWaitSync, (void *, GLbitfield, uint64_t)) \
    X(void, WaitSync, (void *, GLbitfield, uint64_t)) \
    X(void, DeleteSync, (void *)) \
    X(void, PushDebugGroup, (GLenum, GLuint, GLsizei, const char *)) \
    X(void, PopDebugGroup, (void)) \
    X(void, DebugMessageInsert, (GLenum, GLenum, GLuint, GLenum, GLsizei, const char *)) \
    X(void, ObjectLabel, (GLenum, GLuint, GLsizei, const char *)) \
    X(void, DebugMessageControl, (GLenum, GLenum, GLenum, GLsizei, const GLuint *, GLboolean)) \
    X(void, CoverageModulationNV, (GLenum)) \
    X(void, CoverageModulationTableNV, (GLsizei, const GLfloat *)) \
    X(void, FragmentCoverageColorNV, (GLuint)) \
    X(void, RasterSamplesEXT, (GLuint, GLboolean)) \
    X(void, FramebufferParameteri, (GLenum, GLenum, GLint)) \
    X(void, FramebufferSampleLocationsfvNV, (GLenum, GLuint, GLsizei, const GLfloat *)) \
    X(void, MinSampleShading, (GLfloat)) \
    X(void, GetQueryObjectui64v, (GLuint, GLenum, uint64_t *)) \
    X(GLenum, GetGraphicsResetStatus, (void)) \
    X(void, SamplerParameterIiv, (GLuint, GLenum, const GLint *)) \
    X(void, SamplerParameterIuiv, (GLuint, GLenum, const GLuint *)) \
    X(void, BlendParameteriNV, (GLenum, GLint)) \
    X(void, BlendEquationi, (GLuint, GLenum))
#define X(ret, name, args) static ret (APIENTRY *gl##name) args;
GLO(X)
#undef X
static int gl_has_(const void *fn, const char *name) {
    static int nlog;
    if (fn) return 1;
    if (nlog < 30) { nlog++; printf("nvn_gl: this OpenGL driver lacks %s: the NVN feature that needs it has no effect\n", name); fflush(stdout); }
    return 0;
}
#define gl_has(fn, name) gl_has_((const void *)(uintptr_t)(fn), name)

enum {
    GL_VERTEX_PROGRAM_ARB_ = 0x8620, GL_FRAGMENT_PROGRAM_ARB_ = 0x8804, GL_GEOMETRY_PROGRAM_NV_ = 0x8C26,
    GL_TESS_CONTROL_PROGRAM_NV_ = 0x891E, GL_TESS_EVALUATION_PROGRAM_NV_ = 0x891F, GL_COMPUTE_PROGRAM_NV_ = 0x90FB,
    GL_PROGRAM_FORMAT_ASCII_ARB_ = 0x8875, GL_PROGRAM_ERROR_POSITION_ARB_ = 0x864B, GL_PROGRAM_ERROR_STRING_ARB_ = 0x8874,
    GL_MAX_PROGRAM_PARAMETER_BUFFER_BINDINGS_NV_ = 0x8DA0,
    GL_ARRAY_BUFFER_ = 0x8892, GL_ELEMENT_ARRAY_BUFFER_ = 0x8893, GL_STREAM_DRAW_ = 0x88E0, GL_SHADER_STORAGE_BUFFER_ = 0x90D2, GL_UNIFORM_BUFFER_ = 0x8A11,
    GL_FRAMEBUFFER_ = 0x8D40, GL_READ_FRAMEBUFFER_ = 0x8CA8, GL_DRAW_FRAMEBUFFER_ = 0x8CA9, GL_COLOR_ATTACHMENT0_ = 0x8CE0,
    GL_DEPTH_ATTACHMENT_ = 0x8D00, GL_STENCIL_ATTACHMENT_ = 0x8D20, GL_DEPTH_STENCIL_ATTACHMENT_ = 0x821A,
    GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5, GL_FRAMEBUFFER_SRGB_ = 0x8DB9,
    GL_TEXTURE_3D_ = 0x806F, GL_TEXTURE_1D_ARRAY_ = 0x8C18, GL_TEXTURE_2D_ARRAY_ = 0x8C1A, GL_TEXTURE_2D_MULTISAMPLE_ = 0x9100,
    GL_TEXTURE_2D_MULTISAMPLE_ARRAY_ = 0x9102, GL_TEXTURE_RECTANGLE_ = 0x84F5, GL_TEXTURE_CUBE_MAP_ = 0x8513,
    GL_TEXTURE_CUBE_MAP_POSITIVE_X_ = 0x8515, GL_TEXTURE_CUBE_MAP_ARRAY_ = 0x9009, GL_TEXTURE_BUFFER_ = 0x8C2A,
    GL_TEXTURE_MAX_LEVEL_ = 0x813D, GL_TEXTURE_SWIZZLE_RGBA_ = 0x8E46, GL_DEPTH_STENCIL_TEXTURE_MODE_ = 0x90EA,
    GL_TEXTURE_WRAP_R_ = 0x8072, GL_TEXTURE_MIN_LOD_ = 0x813A, GL_TEXTURE_MAX_LOD_ = 0x813B, GL_TEXTURE_LOD_BIAS_ = 0x8501,
    GL_TEXTURE_COMPARE_MODE_ = 0x884C, GL_TEXTURE_COMPARE_FUNC_ = 0x884D, GL_COMPARE_REF_TO_TEXTURE_ = 0x884E,
    GL_TEXTURE_MAX_ANISOTROPY_ = 0x84FE, GL_TEXTURE_CUBE_MAP_SEAMLESS_ = 0x884F,
    GL_UPPER_LEFT_ = 0x8CA2, GL_LOWER_LEFT_ = 0x8CA1, GL_ZERO_TO_ONE_ = 0x935F, GL_NEGATIVE_ONE_TO_ONE_ = 0x935E,
    GL_MULTISAMPLE_ = 0x809D, GL_SAMPLE_ALPHA_TO_COVERAGE_ = 0x809E, GL_PRIMITIVE_RESTART_ = 0x8F9D, GL_PATCH_VERTICES_ = 0x8E72,
    GL_DEPTH_CLAMP_ = 0x864F, GL_RASTERIZER_DISCARD_ = 0x8C89, GL_DEBUG_OUTPUT_ = 0x92E0, GL_DEBUG_OUTPUT_SYNCHRONOUS_ = 0x8242,
    GL_BGR_ = 0x80E0, GL_BGRA_ = 0x80E1, GL_UNPACK_ROW_LENGTH_ = 0x0CF2, GL_DEPTH_STENCIL_ = 0x84F9, GL_STENCIL_INDEX_ = 0x1901,
};

/* ---------------------------------------------------------------- the render thread */
typedef struct Job { void (*fn)(void *); void *arg; HANDLE done; struct Job *next; } Job;
static CRITICAL_SECTION g_job_lock;
static Job *g_job_head, *g_job_tail;
static HANDLE g_job_event, g_ready_event;
static DWORD g_render_tid;
static HWND g_hwnd;
static HDC g_dc;
static __declspec(thread) HANDLE t_done;
static void gl_housekeeping(void);
static void (*pf_job_submit)(void *), (*pf_job_present)(void *);
static LONG64 g_joblog_frame, g_joblog_cur;
static void joblog(void (*fn)(void *), LONG64 t); static void job_read_texels(void *arg); static void job_write_texels(void *arg); static void job_rb_land_range(void *arg); static void job_submit_async(void *arg);
static inline LONG64 pf_now(void);
static int g_copy_views;                       /* views made as copies (tex_view_obj); none: view_copies_refresh has nothing to do */
static inline void pf_add(int k, LONG64 t0);
enum { PF_WAIT_SUBMIT_ = 13, PF_WAIT_PRESENT_ = 14, PF_WAIT_OTHER_ = 15 };
/* Hitch log (hitch.c, SSBU_HITCH=<ms>): what the render thread is doing right now, for the watchdog. Two stores per scope. */
extern int g_hitch_ms;
extern volatile const char *g_hitch_rt;
extern volatile uint64_t g_hitch_rt_arg;
void hitch_wait_render(int on); void hitch_frame(long long frame, double ms, int battle); void hitch_report(FILE *out); void hitch_set_render(void);
#define HS_IN(name, arg) const char *hs_p_ = (const char *)g_hitch_rt; uint64_t hs_a_ = g_hitch_rt_arg; g_hitch_rt_arg = (uint64_t)(arg); g_hitch_rt = (name)
#define HS_OUT() (g_hitch_rt = hs_p_, g_hitch_rt_arg = hs_a_)
/* GPU timestamps (counter reports) within a frame: the frame's start plus 1 us per report. Taken when the render thread
 * reached the command, a stall of the render thread (a shader compiled on first use, a texture upload) read as GPU time,
 * and the game then lowered its render resolution for a while (dynamic resolution: a new render target size every frame,
 * the churn that crashed the NVIDIA driver). Between frames the stamps follow the real clock. TOMO_REAL_GPU_TIME=1: the
 * old stamps. */
static uint64_t g_gpu_ts_base, g_gpu_ts_k;
static uint64_t now_ns(void);
static uint64_t gpu_ts(void) {
    static int real = -1;
    if (real < 0) real = getenv("TOMO_REAL_GPU_TIME") != NULL;
    if (real || !g_gpu_ts_base) return now_ns();
    return g_gpu_ts_base + ++g_gpu_ts_k * 1000;
}

static struct { void (*fn)(void *); volatile LONG64 ticks, n; } g_wj[16];
static void gl_run(void (*fn)(void *), void *arg) {
    if (GetCurrentThreadId() == g_render_tid) { fn(arg); return; }
    if (!t_done) t_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    Job j = {fn, arg, t_done, NULL};
    LONG64 pf0 = pf_now();
    if (g_joblog_frame > 0 && g_joblog_cur >= g_joblog_frame && g_joblog_cur < g_joblog_frame + 3) joblog(fn, pf0);
    EnterCriticalSection(&g_job_lock);
    if (g_job_tail) g_job_tail->next = &j; else g_job_head = &j;
    g_job_tail = &j;
    LeaveCriticalSection(&g_job_lock);
    SetEvent(g_job_event);
    hitch_wait_render(1);
    WaitForSingleObject(j.done, INFINITE);
    hitch_wait_render(0);
    if (g_joblog_frame > 0 && g_joblog_cur >= g_joblog_frame && g_joblog_cur < g_joblog_frame + 3) printf("joblog:   done after %.3f ms%c", (double)(pf_now() - pf0) * 1000.0 / 1e7, 10);
    pf_add(fn == pf_job_submit ? PF_WAIT_SUBMIT_ : fn == pf_job_present ? PF_WAIT_PRESENT_ : PF_WAIT_OTHER_, pf0);
    if (fn != pf_job_submit && fn != pf_job_present) {           /* nvn_perf: the other jobs waited for, by function */
        LONG64 dt = pf_now() - pf0;
        for (int i = 0; i < 16; i++) {
            if (g_wj[i].fn == fn || !g_wj[i].fn) { if (!g_wj[i].fn) g_wj[i].fn = fn; InterlockedExchangeAdd64(&g_wj[i].ticks, dt); InterlockedIncrement64(&g_wj[i].n); break; }
        }
    }
}

/* P3: nvnQueueSubmitCommands / FenceSync / Flush do not wait for the render thread (SSBU_SYNC_SUBMIT=1: they wait, as before).
 * Measured in a battle: the game's main thread calls, per frame, SyncWait x2, SubmitCommands, FenceSync, Flush, then works
 * for about 8 ms and calls PresentTexture; the replay of the submitted commands takes 6-9 ms, during which the main
 * thread used to stand still. NVN itself executes submitted commands later, on the GPU, and the game waits for its
 * fence (SyncWait) before reusing what the commands refer to, so this is the order of events the game was written for.
 * What keeps the result the same as with a waiting submit:
 *   - the jobs run in the order they were issued (one queue); every other job (PresentTexture, SyncWait, texel
 *     reads and writes, Finish, ...) still waits, and so runs after everything issued before it;
 *   - the handles given to SubmitCommands are copied; the command lists themselves were copied when recorded;
 *   - entry points that change or release a host record a queued command may name (programs, textures, pools, samplers,
 *     windows, queues) first wait until the queue is empty (gl_barrier); FinalizeCommandHandle is queued in order;
 *   - guest memory the commands refer to (vertex / uniform / storage buffers, texture storage) is read when the commands
 *     run, as on the console's GPU: the game must not change it before the fence signals, and that rule is the game's own. */
static volatile LONG g_async_pending;
static int g_sync_submit = -1;
static void job_nop(void *arg) { (void)arg; }
static void job_sync_wait(void *arg); static void job_flush(void *arg); static void job_finish(void *arg); static void job_fence(void *arg);   /* nvn_perf: the waits by job */
static void job_housekeeping(void *arg); static void job_write_texels(void *arg); static void job_read_texels(void *arg); static void job_server_wait(void *arg);
static int gl_async_on(void) {
    if (g_sync_submit < 0) g_sync_submit = getenv("SSBU_SYNC_SUBMIT") != NULL;
    return !g_sync_submit;
}
static void gl_post(void (*fn)(void *), const void *arg, size_t size) {       /* run fn(copy of arg) on the render thread, in order, without waiting */
    if (GetCurrentThreadId() == g_render_tid) { fn((void *)arg); return; }
    if (g_joblog_frame > 0 && g_joblog_cur >= g_joblog_frame && g_joblog_cur < g_joblog_frame + 3) joblog(fn, pf_now());      /* (posted, not waited for) */
    Job *j = malloc(sizeof *j + size + 8);
    if (size) memcpy(j + 1, arg, size);
    j->fn = fn; j->arg = j + 1; j->done = NULL; j->next = NULL;
    InterlockedIncrement(&g_async_pending);
    EnterCriticalSection(&g_job_lock);
    if (g_job_tail) g_job_tail->next = j; else g_job_head = j;
    g_job_tail = j;
    LeaveCriticalSection(&g_job_lock);
    SetEvent(g_job_event);
}
static struct { const char *who; volatile LONG64 ticks, n; } g_bar[16];     /* nvn_perf: the barriers by caller */
static void gl_barrier_from(const char *who) {              /* everything queued so far has run when this returns */
    if (!g_async_pending) return;
    LONG64 t0 = pf_now();
    gl_run(job_nop, NULL);
    LONG64 dt = pf_now() - t0;
    for (int i = 0; i < 16; i++) if (g_bar[i].who == who || !g_bar[i].who) { g_bar[i].who = who; InterlockedExchangeAdd64(&g_bar[i].ticks, dt); InterlockedIncrement64(&g_bar[i].n); break; }
}
#define gl_barrier() gl_barrier_from(__func__)

/* ---------------------------------------------------------------- statistics and diagnostics */
static struct {
    LONG64 frames, submits, lists, draws, draws_skipped, clears, stale, tex_created, tex_uploads, tex_bytes, views, handles,
           prog_ok, prog_fail, binary_used, glsl_ok, glsl_fail, glsl_draws, fbo_made, fbo_bad, gl_errors, copies, unknown_fmt, arena_bytes,
           dispatches, dispatch_run, dispatch_skipped, compute_bytes, draws_from_compute;
} g_st;
static LONG64 g_frame_draws, g_frame_skipped, g_frame_clears;
static LONG64 g_fs_big, g_fs_bigclr, g_fs_rt, g_fs_disp;     /* TOMO_FRAMESUM: draws and clears on 2048-wide targets, target switches, dispatches */
static int g_debug, g_logn;
/* NVIDIA's own GL thread (threaded optimization) takes the driver's half of every call off the render thread, unless the
 * context has synchronous debug output. Island at forced 60, 2026-10-31: a frame's command list replayed in 10.5-12 ms
 * instead of 15-15.7, frames over 20 ms after the island was up 36-55 instead of 266 a run, p99 20.5 instead of 26.9 ms.
 * The driver picks its thread when the context starts: turning synchronous debug output off later (tried: once the
 * shader workers were idle) left the replay as slow as before. SSBU_GL_DRIVER_THREAD=0 (or SSBU_GLDEBUG): synchronous
 * debug output, as before. */
static int gl_driver_thread(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_GL_DRIVER_THREAD"); on = !getenv("SSBU_GLDEBUG") && !(e && e[0] == '0'); }
    return on;
}
static int g_gl_thr_on;                          /* synchronous debug output is off on the render context now (render thread) */
/* GL error checks on paths taken every frame or for every new object: with NVIDIA's GL thread each glGetError waits for
 * everything queued before it, so there errors are left to the debug callback (still printed, without the stall). */
#define GL_ERR_SOFT() (g_gl_thr_on ? 0 : glGetError())
#define GL_ERR_CLEAR_SOFT() do { if (!g_gl_thr_on) while (glGetError()) {} } while (0)
static const char *g_cur_op = "";
static volatile LONG g_setshaders_calls, g_with_text, g_without_text;
static LONG g_bufhist[6][32], g_storhist[32];

/* ---- timing scopes (plan\phases\P3_performance.md). Always on: two QueryPerformanceCounter calls per scope.
 * Printed with the frame-rate line every 300 frames as milliseconds per frame. Scopes nest (PREP contains ARENA and
 * TEXSYNC; SUBMIT contains everything the render thread does for a batch); WAIT_* are measured on the calling guest
 * thread (how long it was blocked in gl_run), PACE is the sleep of display_pace. */
enum { PF_SUBMIT, PF_PREP, PF_DRAWCALL, PF_ARENA, PF_TEXHASH, PF_TEXUP, PF_HANDLE, PF_DISPATCH, PF_CFLUSH, PF_COPY, PF_PRESENT, PF_SWAP,
       PF_OTHERJOB, PF_WAIT_SUBMIT, PF_WAIT_PRESENT, PF_WAIT_OTHER, PF_PACE, PF_XLATE, PF_GLCOMPILE, PF_FEEDBACK, PF_OTHERCMD, PF_N };
static const char *const k_pf_name[PF_N] = {"submit", "prep", "drawcall", "arena", "texhash", "texupload", "handle", "dispatch", "cflush", "copy", "present", "swap",
                                            "otherjob", "wait-submit", "wait-present", "wait-other", "pace", "shader-translate", "shader-compile", "feedback", "other-cmd"};
static volatile LONG64 g_pf[PF_N], g_pfn[PF_N];
static LONG64 g_pf_arena_bytes, g_pf_cflush_bytes, g_pf_hash_bytes;
/* SSBU_GLSTATS=1: one more line every 300 frames with per-frame counts (render-thread waits of guest threads, compute
 * read-backs, objects created, shader work, and how many per-draw GL settings repeat the value of the previous draw). */
static LONG64 g_gs_draws, g_gs_prog_same, g_gs_attr_set, g_gs_attr_same, g_gs_vbuf_bind, g_gs_buf_bind, g_gs_gl_binds;      /* (gl_binds: glBindBufferRange calls made) */
static GLuint g_gs_last_prog[2];
static uint32_t g_gs_last_attr[16];
static inline LONG64 pf_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static LONG64 g_pp[10]; static int g_pp_on = -1;     /* TOMO_PREPPROF=1: draw_prepare by part (ms per frame) */
/* The per-command and per-draw scopes (commands, prep, draw calls, texture handles) cost two QueryPerformanceCounter calls
 * each: ~25,000 commands a frame on the island, about 1.5 ms of the render thread. TOMO_PF_DETAIL=1 turns them on for the
 * nvn_perf lines; the draw call's own time is still taken for the hitch report (SSBU_HITCH). */
static int pf_detail(void) { static int on = -1; if (on < 0) on = getenv("TOMO_PF_DETAIL") != NULL; return on; }
static inline void pf_add(int k, LONG64 t0) { InterlockedExchangeAdd64(&g_pf[k], pf_now() - t0); InterlockedIncrement64(&g_pfn[k]); }
void nvn_pf_pace(LONG64 ticks) { InterlockedExchangeAdd64(&g_pf[PF_PACE], ticks); InterlockedIncrement64(&g_pfn[PF_PACE]); }     /* display.c */

static void logf_limited(int *counter, int limit, const char *fmt, ...) {
    if (*counter >= limit) return;
    (*counter)++;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* P3 round 4, SSBU_FDLOG=1 (diagnostics): every message of the driver is kept with its time (ring of 64) and counted
 * per id; hitch_draw prints the ones that arrived during a slow draw call, nvn_gl_report the table. */
static int g_fdlog;
static unsigned g_fd_prim, g_fd_poly, g_fd_dclamp, g_fd_rdisc, g_fd_count;
static struct { LONG64 t; GLuint id; GLenum type, sev; char text[232]; } g_fd_ring[64];
static unsigned g_fd_ring_n;
static struct { GLuint id; GLenum type, sev; LONG64 n; char text[200]; } g_fd_ids[64];
static void fd_note(GLenum type, GLuint id, GLenum severity, const char *message) {
    unsigned k = g_fd_ring_n++ & 63;
    g_fd_ring[k].t = pf_now(); g_fd_ring[k].id = id; g_fd_ring[k].type = type; g_fd_ring[k].sev = severity;
    snprintf(g_fd_ring[k].text, sizeof g_fd_ring[k].text, "%s", message ? message : "");
    for (int i = 0; i < 64; i++) {
        if (g_fd_ids[i].n && g_fd_ids[i].id != id) continue;
        if (!g_fd_ids[i].n) { g_fd_ids[i].id = id; g_fd_ids[i].type = type; g_fd_ids[i].sev = severity; snprintf(g_fd_ids[i].text, sizeof g_fd_ids[i].text, "%s", message ? message : ""); }
        g_fd_ids[i].n++;
        break;
    }
}
static void APIENTRY debug_cb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const char *message, const void *user) {
    static int n;
    (void)source; (void)length; (void)user;
    if (g_fdlog) fd_note(type, id, severity, message);
    if (severity == 0x826B) return;                       /* notifications */
    if (type != 0x824C && !g_debug) return;               /* errors always; the rest only with SSBU_GLDEBUG */
    logf_limited(&n, g_debug ? 3000 : 60, "GL debug (type %x id %u) during %s / %s: %s\n", type, id, g_cur_op, (const char *)g_hitch_rt, message);
}

/* ---------------------------------------------------------------- formats */
static const uint16_t k_int12[4][12] = {
    {0x8229, 0x8F94, 0x8232, 0x8231, 0x822D, 0x822A, 0x8F98, 0x8234, 0x8233, 0x822E, 0x8236, 0x8235},
    {0x822B, 0x8F95, 0x8238, 0x8237, 0x822F, 0x822C, 0x8F99, 0x823A, 0x8239, 0x8230, 0x823C, 0x823B},
    {0x8051, 0x8F96, 0x8D7D, 0x8D8F, 0x881B, 0x8054, 0x8F9A, 0x8D77, 0x8D89, 0x8815, 0x8D71, 0x8D83},
    {0x8058, 0x8F97, 0x8D7C, 0x8D8E, 0x881A, 0x805B, 0x8F9B, 0x8D76, 0x8D88, 0x8814, 0x8D70, 0x8D82}};
static const uint16_t k_type12[12] = {0x1401, 0x1400, 0x1401, 0x1400, 0x140B, 0x1403, 0x1402, 0x1403, 0x1402, 0x1406, 0x1405, 0x1404};
static const uint8_t k_size12[12] = {1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4};
static const uint8_t k_isint12[12] = {0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1};
static const uint8_t k_norm12[12] = {1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0};
static const uint16_t k_pix[4] = {0x1903, 0x8227, 0x1907, 0x1908}, k_pixint[4] = {0x8D94, 0x8228, 0x8D98, 0x8D99};
static const uint8_t k_astc[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

static int bl_has(uint32_t nh);
static int fmt_get(int nf, Fmt *f) {
    memset(f, 0, sizeof *f);
    f->bw = f->bh = 1;
#define SET(i, fo, ty, b, k) (f->internal = (i), f->format = (fo), f->type = (ty), f->bpb = (b), f->kind = (k), 1)
#define BC(i, b) (f->bw = f->bh = 4, SET(i, 0, 0, b, 4))
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int comps = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        return SET(k_int12[comps - 1][i], k_isint12[i] ? k_pixint[comps - 1] : k_pix[comps - 1], k_type12[i], comps * k_size12[i], 0);
    }
    if (nf >= 0x79 && nf <= 0x94) {
        int i = (nf - 0x79) % 14;
        f->bw = k_astc[i][0]; f->bh = k_astc[i][1];
        return SET((nf >= 0x87 ? 0x93D0 : 0x93B0) + i, 0, 0, 16, 4);
    }
    switch (nf) {
    case 0x31: return SET(0x8D48, GL_STENCIL_INDEX_, 0x1401, 1, 2);
    case 0x32: return SET(0x81A5, 0x1902, 0x1403, 2, 1);
    case 0x33: return SET(0x81A6, 0x1902, 0x1405, 4, 1);
    case 0x34: return SET(0x8CAC, 0x1902, 0x1406, 4, 1);
    case 0x35: return SET(0x88F0, GL_DEPTH_STENCIL_, 0x84FA, 4, 3);
    case 0x36: return SET(0x8CAD, GL_DEPTH_STENCIL_, 0x8DAD, 8, 3);
    case 0x37: case 0x38: return SET(0x8C43, 0x1908, 0x1401, 4, 0);
    case 0x39: return SET(0x8056, 0x1908, 0x8033, 2, 0);
    case 0x3A: return SET(0x8050, 0x1908, 0x8034, 2, 0);
    case 0x3B: return SET(0x8057, 0x1908, 0x8034, 2, 0);
    case 0x3C: return SET(0x8D62, 0x1907, 0x8363, 2, 0);
    case 0x3D: return SET(0x8059, 0x1908, 0x8368, 4, 0);
    case 0x3E: return SET(0x906F, 0x8D99, 0x8368, 4, 0);
    case 0x3F: return SET(0x8C3A, 0x1907, 0x8C3B, 4, 0);
    case 0x40: return SET(0x8C3D, 0x1907, 0x8C3E, 4, 0);
    case 0x41: return BC(0x83F0, 8);
    case 0x42: return BC(0x83F1, 8);
    case 0x43: return BC(0x83F2, 16);
    case 0x44: return BC(0x83F3, 16);
    case 0x45: return BC(0x8C4C, 8);
    case 0x46: return BC(0x8C4D, 8);
    case 0x47: return BC(0x8C4E, 16);
    case 0x48: return BC(0x8C4F, 16);
    case 0x49: return BC(0x8DBB, 8);
    case 0x4A: return BC(0x8DBC, 8);
    case 0x4B: return BC(0x8DBD, 16);
    case 0x4C: return BC(0x8DBE, 16);
    case 0x4D: return BC(0x8E8C, 16);
    case 0x4E: return BC(0x8E8D, 16);
    case 0x4F: return BC(0x8E8E, 16);
    case 0x50: return BC(0x8E8F, 16);
    case 0x95: return SET(0x8D62, GL_BGR_, 0x8363, 2, 0);
    case 0x99: case 0x9A: return SET(0x8058, GL_BGRA_, 0x1401, 4, 0);
    case 0x9B: case 0x9C: return SET(0x8C43, GL_BGRA_, 0x1401, 4, 0);
    }
    return 0;
#undef SET
#undef BC
}

/* Vertex attribute format: component count, GL type, normalised, integer. */
static int vfmt_get(int nf, int *size, GLenum *type, int *norm, int *integer) {
    static const uint16_t t6[6] = {0x1401, 0x1400, 0x1403, 0x1402, 0x1405, 0x1404};
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        *size = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1; *type = k_type12[i]; *norm = k_norm12[i]; *integer = k_isint12[i];
        return 1;
    }
    if (nf >= 0x51 && nf <= 0x68) { *size = (nf - 0x51) / 6 + 1; *type = t6[(nf - 0x51) % 6]; *norm = 0; *integer = 0; return 1; }
    switch (nf) {
    case 0x3D: *size = 4; *type = 0x8368; *norm = 1; *integer = 0; return 1;
    case 0x3E: *size = 4; *type = 0x8368; *norm = 0; *integer = 1; return 1;
    case 0x69: *size = 4; *type = 0x8D9F; *norm = 1; *integer = 0; return 1;
    case 0x6A: *size = 4; *type = 0x8D9F; *norm = 0; *integer = 1; return 1;
    case 0x6B: *size = 4; *type = 0x8368; *norm = 0; *integer = 0; return 1;
    case 0x6C: *size = 4; *type = 0x8D9F; *norm = 0; *integer = 0; return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- guest objects
 * Small state objects and builders keep their contents in the guest's own memory (the game copies them
 * around freely); textures, programs, command buffers, pools and windows keep a pointer to a host struct
 * at +0 and a tag at +8. Host structs of textures and programs are never freed (recorded command lists
 * and pool entries may still name them); they are only marked dead. */
static void *hobj(uint64_t g, uint32_t kind) { return g && rd32(g + 8) == kind ? P(rd64(g)) : NULL; }
static void *hnew(uint64_t g, uint32_t kind, size_t size) {
    void *p = calloc(1, size);
    wr64(g, (uint64_t)(uintptr_t)p);
    wr32(g + 8, kind);
    return p;
}

/* SamplerParams, View, TexBuilder, TexObj, EView, VNode, Tex, TexPoolEnt, TexPool, SamplerPool, Stage, Prog, Cmd, CmdBuf, Window:
 * nvn_front.h (seam step S1) */

static SRWLOCK g_pool_lock = SRWLOCK_INIT;
static Tex *volatile g_all_tex;            /* every texture, newest first (never unlinked: host records of textures are not freed) */
/* PR #30 (v1Ta44): flush notifications query only live texture ranges. History remains intact for renderer references and
 * prewarming. Sorted starts plus prefix maxima include overlapping/aliased textures without scanning every historical record. */
typedef struct { Tex *tex; uint64_t begin, end, prefix_end; } TexRange;
static SRWLOCK g_tex_range_lock = SRWLOCK_INIT;
static TexRange *g_tex_ranges;
static size_t g_tex_range_count, g_tex_range_cap;
static int g_tex_range_fallback;
static uint64_t tex_range_end(uint64_t begin, uint64_t size) { return size > UINT64_MAX - begin ? UINT64_MAX : begin + size; }
static size_t tex_range_lower(uint64_t addr) {
    size_t lo = 0, hi = g_tex_range_count;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (g_tex_ranges[mid].begin < addr) lo = mid + 1; else hi = mid; }
    return lo;
}
static void tex_range_prefix(size_t first) {
    uint64_t end = first ? g_tex_ranges[first - 1].prefix_end : 0;
    for (size_t i = first; i < g_tex_range_count; i++) {
        if (g_tex_ranges[i].end > end) end = g_tex_ranges[i].end;
        g_tex_ranges[i].prefix_end = end;
    }
}
static void tex_range_add(Tex *t) {
    if (!t->mem || !t->storage) return;
    AcquireSRWLockExclusive(&g_tex_range_lock);
    if (g_tex_range_fallback) { ReleaseSRWLockExclusive(&g_tex_range_lock); return; }
    if (g_tex_range_count == g_tex_range_cap) {
        size_t cap = g_tex_range_cap ? g_tex_range_cap * 2 : 256;
        TexRange *r = realloc(g_tex_ranges, cap * sizeof *r);
        if (!r) {                         /* never drop invalidations because the optional index could not grow */
            free(g_tex_ranges); g_tex_ranges = NULL; g_tex_range_count = g_tex_range_cap = 0; g_tex_range_fallback = 1;
            ReleaseSRWLockExclusive(&g_tex_range_lock);
            printf("nvn_gl: the live-texture range index could not grow: flush notifications walk every texture again\n");
            return;
        }
        g_tex_ranges = r; g_tex_range_cap = cap;
    }
    uint64_t begin = (uint64_t)(uintptr_t)t->mem;
    size_t i = tex_range_lower(begin);
    memmove(g_tex_ranges + i + 1, g_tex_ranges + i, (g_tex_range_count - i) * sizeof *g_tex_ranges);
    g_tex_ranges[i].tex = t; g_tex_ranges[i].begin = begin; g_tex_ranges[i].end = tex_range_end(begin, t->storage);
    g_tex_range_count++; tex_range_prefix(i);
    ReleaseSRWLockExclusive(&g_tex_range_lock);
}
static void tex_range_remove(Tex *t) {
    if (!t->mem || !t->storage) return;
    AcquireSRWLockExclusive(&g_tex_range_lock);
    uint64_t begin = (uint64_t)(uintptr_t)t->mem;
    for (size_t i = tex_range_lower(begin); i < g_tex_range_count && g_tex_ranges[i].begin == begin; i++) {
        if (g_tex_ranges[i].tex != t) continue;
        g_tex_range_count--;
        memmove(g_tex_ranges + i, g_tex_ranges + i + 1, (g_tex_range_count - i) * sizeof *g_tex_ranges);
        tex_range_prefix(i);
        break;
    }
    if (!g_tex_range_count) { free(g_tex_ranges); g_tex_ranges = NULL; g_tex_range_cap = 0; }
    ReleaseSRWLockExclusive(&g_tex_range_lock);
}
void nvn_label_set(uint64_t obj, const char *text);
const char *nvn_label_get(uint64_t obj);
void nvn_label_drop(uint64_t obj);
static int g_origin_mode = 1, g_depth_mode = 1;        /* what the game asks for; applied by the render thread */
static volatile LONG g_clip_dirty;                   /* a mode changed after the GL start-up: job_submit applies it */

/* ---------------------------------------------------------------- Tegra block-linear layout
 * Rules, storage sizes and de-swizzling come from tex_decode.c (tex_layout / tex_deswizzle_level), which was
 * checked against the game's packaged textures; in particular 3D textures use block height 1 and a block depth. */
static void tex_lay(const TexBuilder *b, TexLayout *L) {
    tex_layout(L, b->target, b->format, b->width, b->height, b->depth, b->levels, b->flags & ~0x50, 0, b->has_layout && b->layout[0] <= 4 ? b->layout[0] : -1);
}
static int tex_layers(const TexBuilder *b) {
    switch (b->target) {
    case 3: case 4: case 6: case 9: return b->depth > 0 ? b->depth : 1;     /* arrays; a cube array counts faces */
    case 8: return 6;
    }
    return 1;
}
static int lvl(int v, int l) { v >>= l; return v > 0 ? v : 1; }
/* Size of the whole texture in pool memory (reserved) and of the part that holds texels (for bounds checks). */
static size_t tex_storage_size2(const TexBuilder *b, size_t *data_size) {
    Fmt f;
    TexLayout L;
    size_t s;
    if (!fmt_get(b->format, &f)) { if (data_size) *data_size = 0x1000; return 0x1000; }
    if (b->flags & 0x10) {
        s = (size_t)(b->stride ? b->stride : (int64_t)b->width * f.bpb) * (size_t)(b->height > 0 ? b->height : 1);
        if (data_size) *data_size = s;
        return s;
    }
    tex_lay(b, &L);
    s = L.size;
    if (data_size) *data_size = L.data_size;
    if (b->samples > 1) { s *= (size_t)b->samples; if (data_size) *data_size *= (size_t)b->samples; }
    return s;
}
static size_t tex_storage_size(const TexBuilder *b) { return tex_storage_size2(b, NULL); }

/* ---------------------------------------------------------------- textures on the render thread */
static uint32_t g_serial = 1;              /* one per submitted batch: texture memory is re-checked once per batch */
static GLuint g_fbo_read, g_dummy_tex;
static uint64_t g_dummy_handle;
static void fbo_purge(GLuint tex);

static GLenum gl_target(int t, int samples) {
    static const GLenum m[11] = {GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D_, GL_TEXTURE_1D_ARRAY_, GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_2D_MULTISAMPLE_,
                                 GL_TEXTURE_2D_MULTISAMPLE_ARRAY_, GL_TEXTURE_RECTANGLE_, GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_CUBE_MAP_ARRAY_, GL_TEXTURE_BUFFER_};
    if (samples > 1 && t == 1) return GL_TEXTURE_2D_MULTISAMPLE_;
    if (samples > 1 && t == 4) return GL_TEXTURE_2D_MULTISAMPLE_ARRAY_;
    return t >= 0 && t < 11 ? m[t] : GL_TEXTURE_2D;
}
static void set_swizzle(GLenum tg, const uint8_t *swz) {
    static const GLint m[6] = {GL_ZERO, GL_ONE, GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    GLint v[4];
    if (swz[0] == 2 && swz[1] == 3 && swz[2] == 4 && swz[3] == 5) return;
    for (int i = 0; i < 4; i++) v[i] = m[swz[i] < 6 ? swz[i] : 0];
    glTexParameteriv(tg, GL_TEXTURE_SWIZZLE_RGBA_, v);
}

static volatile LONG64 g_handles_live, g_tex_live;       /* resident bindless handles and GL textures alive now (nvn_perf) */
/* Render targets the game finalized, kept with their views, handles and framebuffers for the next texture of the same shape:
 * Tomodachi Life makes its render targets anew every frame (a dozen a frame in the island), and deleting and recreating
 * their GL objects each frame crashed the driver in glDeleteTextures after some 2400 frames. Only GPU-written textures
 * (CPU_NO_ACCESS pools) are kept: nothing is ever uploaded into them, so stale texels are what the pool memory holds too. */
/* Resolution scale (graphics.ini resolution_scale; TOMO_SCALE wins), the OpenGL side of nvn_vk_draw.c's track O: a
 * texture of the screen class (2D, one sample, not block-compressed, the game's size exactly 1920x1080 >> k) gets GL
 * storage of round(size * scale) (Tex.rw / rh); what the game says in ITS pixels about such a texture (viewports,
 * scissors, clear and copy rectangles, uploads, read-backs, the present) is converted where it is used, and fragment
 * stages map gl_FragCoord and texelFetch back to the game's grid (MxIn.screen_scale). At 1.0 none of it runs. */
static float g_rsf = -1.0f;
static float rs_factor(void) {
    if (g_rsf < 0) {
        const char *e = getenv("TOMO_SCALE");
        float v = e && atof(e) > 0 ? (float)atof(e) : gfx_options()->scale;
        g_rsf = v < 0.5f ? 0.5f : v > 3.0f ? 3.0f : v;
    }
    return g_rsf;
}
/* Widescreen (graphics.ini widescreen = off | auto | 21:9 | <w:h>; TOMO_WIDESCREEN wins): the screen-class targets are made
 * k times wider than the game's (k = the aspect / 16:9; the resolution scale's per-axis mapping does the rest), the vertex
 * stages multiply the clip-space x of their positions by 1/k on such targets (mx_translate.c mx_wide_fix: hor+, the 3D
 * scene shows more at the sides, menus stay 16:9 in the middle; full-screen passes keep covering the picture), and the
 * present fits the wider picture to the window. */
static float g_wide_k = -1.0f;
static float wide_k(void) {
    if (g_wide_k < 0) {
        float a = gfx_widescreen();
        const char *e = getenv("TOMO_WIDESCREEN");
        if (e) a = !_stricmp(e, "auto") ? -1.0f : strchr(e, ':') && atof(strchr(e, ':') + 1) > 0 ? (float)(atof(e) / atof(strchr(e, ':') + 1)) : (float)atof(e);
        if (a < 0) {
            const GfxOptions *o = gfx_options();
            MONITORINFO mi;
            memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
            if (o->window_mode && GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi) && mi.rcMonitor.bottom > mi.rcMonitor.top)
                a = (float)(mi.rcMonitor.right - mi.rcMonitor.left) / (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
            else a = (float)o->width / (float)o->height;
        }
        float k = a > 0 ? a / (16.0f / 9.0f) : 1.0f;
        g_wide_k = k > 1.01f ? (k > 1.35f ? 1.35f : k) : 1.0f;      /* 21:9 (2.4:1) at most: no 32:9 (user 2026-11-01) */
        if (g_wide_k != 1.0f) printf("nvn_gl: widescreen: %.3f:1, the screen-sized targets %.3f times as wide as the game's (OpenGL)%c", 16.0f / 9.0f * g_wide_k, g_wide_k, 10);
    }
    return g_wide_k;
}
float nvn_wide_k(void) { return wide_k(); }
static int g_wide_camera;                         /* the game's cameras are given the wide aspect (tomo.c): the squeeze is for 2D only */
void nvn_wide_camera(void) { g_wide_camera = 1; }
static int rs_on(void) { return rs_factor() != 1.0f || wide_k() != 1.0f; }
static int rs_class(const TexBuilder *b) {
    Fmt f;
    if (!rs_on() || b->target != 1 || b->samples > 1 || b->depth > 1 || (b->flags & 0x10) || !fmt_get(b->format, &f) || f.kind == 4) return 0;
    for (int k = 0; k < 7; k++) if (b->width == 1920 >> k && b->height == 1080 >> k) return 1;
    return 0;
}
static int tw(const Tex *t) { return t->rw ? t->rw : t->b.width > 0 ? t->b.width : 1; }       /* the GL storage's level-0 size */
static int th(const Tex *t) { return t->rh ? t->rh : t->b.height > 0 ? t->b.height : 1; }
static int rsx(const Tex *t, int v) { return t->rw ? (int)floorf((float)v * (float)t->rw / (float)t->b.width + 0.5f) : v; }      /* the game's x on t -> storage texels */
static int rsy(const Tex *t, int v) { return t->rh ? (int)floorf((float)v * (float)t->rh / (float)t->b.height + 0.5f) : v; }

#define SPARE_KEY (offsetof(TexBuilder, layout))        /* flags .. has_layout, swz: what tex_realize makes the storage from */
#define SPARE_MAX 96
static struct { TexBuilder b; TexObj o; VNode *views; int glevels; Tex *crop, *scratch; } g_spare[SPARE_MAX];
static int g_spare_n;
static LONG64 g_spare_hits, g_spare_kept, g_spare_full;
static LONG64 g_spare_same_mem, g_spare_cleared;
/* The game remakes a render target over the same pool memory and may read what the last one there held (a UI layer
 * kept from frame to frame): a spare made at the same memory is taken first, its texels are the right ones. A spare from
 * other memory holds another target's picture: it is cleared to zero (a new GL texture would start out that way). */
static void alias_inherit(Tex *t);
/* Zero texels for GL storage the game will only reach through the GPU: new GL storage is not cleared by the driver (it may
 * hold a texture deleted just before; a 128x128 noise target there gave the dialog cloud its torn "heatwave" edge on some
 * runs, 2026-10-24), and a spare from other memory holds another target's picture. */
static void tex_zero(Tex *t) {
    Fmt f;
    if (!fmt_get(t->b.format, &f) || f.kind == 4 || !gl_has(glClearTexImage, "glClearTexImage")) return;
    GLenum fm = f.format, ty = f.type;
    if (f.kind == 1) { fm = 0x1902; ty = 0x1406; }                 /* DEPTH_COMPONENT, FLOAT */
    else if (f.kind == 2) { fm = 0x1901; ty = 0x1401; }            /* STENCIL_INDEX, UNSIGNED_BYTE */
    else if (f.kind == 3) { fm = 0x84F9; ty = f.internal == 0x8CAD ? 0x8DAD : 0x84FA; }   /* DEPTH_STENCIL: 32F_8 or 24_8 */
    for (int l = 0; l < t->glevels; l++) glClearTexImage(t->o.gl, l, fm, ty, NULL);
    GL_ERR_CLEAR_SOFT();
}
static int tex_take_spare(Tex *t) {
    if (!t->gpu_written || g_rb == &rb_vk) return 0;
    int pick = -1;
    for (int i = g_spare_n - 1; i >= 0; i--) {
        if (memcmp(&g_spare[i].b, &t->b, SPARE_KEY)) continue;
        if (g_spare[i].b.pool == t->b.pool && g_spare[i].b.offset == t->b.offset) { pick = i; break; }
        if (pick < 0) pick = i;
    }
    if (pick < 0) return 0;
    int same = g_spare[pick].b.pool == t->b.pool && g_spare[pick].b.offset == t->b.offset;
    t->o = g_spare[pick].o; t->views = g_spare[pick].views; t->glevels = g_spare[pick].glevels;
    t->crop = g_spare[pick].crop; t->crop_gen = t->rt_gen + 1;      /* (its 16:9 part, tex_crop: copied again at the first use) */
    t->scratch = g_spare[pick].scratch;                              /* (its feedback copy, tex_scratch: copied before every use) */
    memmove(&g_spare[pick], &g_spare[pick + 1], sizeof g_spare[0] * (size_t)(g_spare_n - 1 - pick)); g_spare_n--;
    g_spare_hits++;
    if (same) g_spare_same_mem++;
    else {
        tex_zero(t); g_spare_cleared++;
        alias_inherit(t);
    }
    return 1;
}
static int g_realize_narrow;                          /* tex_crop: the game's shape at the resolution scale, not widened */
static void tex_realize(Tex *t) {
    static int nlog, nbad;
    Fmt f;
    TexBuilder *b = &t->b;
    t->rw = t->rh = 0;
    if (rs_class(b)) {
        t->rw = (int)((float)b->width * rs_factor() * (g_realize_narrow ? 1.0f : wide_k()) + 0.5f); t->rh = (int)((float)b->height * rs_factor() + 0.5f);
        if (t->rw < 1) t->rw = 1; if (t->rh < 1) t->rh = 1;
    }
    if (tex_take_spare(t)) return;
    if (!fmt_get(b->format, &f)) {
        g_st.unknown_fmt++;
        logf_limited(&nbad, 20, "nvn_gl: texture format 0x%x is not mapped; using RGBA8\n", b->format);
        fmt_get(0x25, &f);
    }
    { const TexFormat *tf = tex_format(b->format); if (tf && tf->sw_decode && tf->gl_sw_internal) f.internal = tf->gl_sw_internal; }
    GLenum tg = gl_target(b->target, b->samples);
    int w = b->width > 0 ? b->width : 1, h = b->height > 0 ? b->height : 1, d = b->depth > 0 ? b->depth : 1, levels = b->levels > 0 ? b->levels : 1, maxdim = w > h ? w : h, maxl = 1;
    if (b->target == 2 && d > maxdim) maxdim = d;
    while ((maxdim >> maxl) > 0) maxl++;
    if (levels > maxl) levels = maxl;
    if (t->rw) {                                       /* resolution scale: the storage's size (and the levels it can have) */
        w = t->rw; h = t->rh; maxdim = w > h ? w : h; maxl = 1;
        while ((maxdim >> maxl) > 0) maxl++;
        if (levels > maxl) levels = maxl;
    }
    t->glevels = levels;
    GL_ERR_CLEAR_SOFT();
    glGenTextures(1, &t->o.gl);
    InterlockedIncrement64(&g_tex_live);
    glBindTexture(tg, t->o.gl);
    switch (tg) {
    case GL_TEXTURE_1D: glTexStorage1D(tg, levels, f.internal, w); break;
    case GL_TEXTURE_2D: case GL_TEXTURE_RECTANGLE_: case GL_TEXTURE_CUBE_MAP_: case GL_TEXTURE_1D_ARRAY_:
        glTexStorage2D(tg, levels, f.internal, w, tg == GL_TEXTURE_1D_ARRAY_ ? d : h); break;
    case GL_TEXTURE_3D_: case GL_TEXTURE_2D_ARRAY_: case GL_TEXTURE_CUBE_MAP_ARRAY_: glTexStorage3D(tg, levels, f.internal, w, h, d); break;
    case GL_TEXTURE_2D_MULTISAMPLE_: glTexStorage2DMultisample(tg, b->samples, f.internal, w, h, GL_TRUE); t->glevels = 1; break;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY_: glTexStorage3DMultisample(tg, b->samples, f.internal, w, h, d, GL_TRUE); t->glevels = 1; break;
    default: glTexStorage2D(GL_TEXTURE_2D, levels, f.internal, w, h); break;
    }
    if (tg != GL_TEXTURE_2D_MULTISAMPLE_ && tg != GL_TEXTURE_2D_MULTISAMPLE_ARRAY_) {
        glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        set_swizzle(tg, b->swz);
        if (b->dsmode == 1) glTexParameteri(tg, GL_DEPTH_STENCIL_TEXTURE_MODE_, GL_STENCIL_INDEX_);
    }
    GLenum e = GL_ERR_SOFT();
    g_st.tex_created++;
    if (b->format >= 0x01 && b->format <= 0x0c && !(b->flags & 0x10)) {       /* candidates for a glyph cache: block-linear single-channel textures */
        static int n1;
        logf_limited(&n1, 12, "nvn_gl: single-channel texture %dx%dx%d format 0x%x levels %d flags 0x%x swizzle %d%d%d%d -> gl %u\n", b->width, b->height, b->depth, b->format, b->levels, b->flags,
                     b->swz[0], b->swz[1], b->swz[2], b->swz[3], t->o.gl);
    }
    if (e || g_debug) logf_limited(&nlog, 80, "nvn_gl: texture target %d format 0x%x %dx%dx%d levels %d samples %d flags 0x%x -> gl %u%s (error 0x%x)\n", b->target, b->format,
                                   b->width, b->height, b->depth, b->levels, b->samples, b->flags, t->o.gl, e ? " FAILED" : "", e);
    if (!e) { if (t->gpu_written) tex_zero(t); alias_inherit(t); }
}

/* One level (one layer / face, or all slices of a 3D level) of tightly packed texels into the GL texture. */
static void rs_put(Tex *t, const Fmt *f, int level, int x, int y, int w, int h, const void *data);
static void rs_get(Tex *t, int level, int x, int y, int w, int h, GLenum fo, GLenum ty, GLsizei bytes, void *out);
static void tex_put(Tex *t, const Fmt *f, int level, int layer, int x, int y, int z, int w, int h, int d, const void *data) {
    GLenum tg = gl_target(t->b.target, t->b.samples);
    if (t->rw && tg == GL_TEXTURE_2D && f->kind != 4) { rs_put(t, f, level, x, y, w, h, data); return; }      /* resolution scale */
    GLsizei size = (GLsizei)(((size_t)(w + f->bw - 1) / f->bw) * ((size_t)(h + f->bh - 1) / f->bh) * f->bpb * (size_t)d);
    glBindTexture(tg, t->o.gl);
    switch (tg) {
    case GL_TEXTURE_1D: glTexSubImage1D(tg, level, x, w, f->format, f->type, data); break;
    case GL_TEXTURE_CUBE_MAP_: tg = GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + (GLenum)(layer % 6);    /* fall through */
    case GL_TEXTURE_2D: case GL_TEXTURE_RECTANGLE_: case GL_TEXTURE_1D_ARRAY_:
        if (f->kind == 4) glCompressedTexSubImage2D(tg, level, x, y, w, h, f->internal, size, data);
        else glTexSubImage2D(tg, level, x, y, w, h, f->format, f->type, data);
        break;
    case GL_TEXTURE_3D_: case GL_TEXTURE_2D_ARRAY_: case GL_TEXTURE_CUBE_MAP_ARRAY_:
        if (tg != GL_TEXTURE_3D_) z = layer;
        if (f->kind == 4) glCompressedTexSubImage3D(tg, level, x, y, z, w, h, d, f->internal, size, data);
        else glTexSubImage3D(tg, level, x, y, z, w, h, d, f->format, f->type, data);
        break;
    }
}

/* PR #41: this renderer consumes packaged texels once, at the deferred first upload. Capture its source before publishing
 * the texture so that later hashing and upload do not depend on the caller keeping the original storage mapped. */
static int tex_capture_packaged(Tex *t) {
    if (!t->b.packaged || !t->data_size) return 1;
    t->packaged_copy = malloc(t->data_size);
    if (!t->packaged_copy) return 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), P(t->b.packaged), t->packaged_copy, t->data_size, &got) || got != t->data_size) {
        free(t->packaged_copy); t->packaged_copy = NULL; return 0;
    }
    return 1;
}
static const uint8_t *tex_source(const Tex *t) {
    return t->packaged_copy ? t->packaged_copy : t->b.packaged ? (const uint8_t *)P(t->b.packaged) : t->mem;
}
static void tex_release_packaged(Tex *t) {
    free(t->packaged_copy); t->packaged_copy = NULL;
}

static uint64_t mem_hash(const uint8_t *p, size_t n, int full) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ n, w;
    size_t words = n / 8, step = full || words <= 8192 ? 1 : words / 4096;
    for (size_t i = 0; i < words; i += step) { memcpy(&w, p + i * 8, 8); h = (h ^ w) * 0x100000001B3ULL; h ^= h >> 29; }
    return h;
}

/* ---- which texture memory did the game write? (P3)
 * A texture's texels are guest memory, and NVN has no call that says "I changed them", so every texture bound in a batch
 * used to be hashed in every batch (35 MB per frame in a battle; large block-linear ones were only sampled, so a small
 * change could be missed). The game's heap is reserved with MEM_WRITE_WATCH (nnsdk.c, nn::os::AllocateAddressRegion):
 * GetWriteWatch names the pages written since the previous question. Rule:
 *   - no page of the texture's range written since this texture was last looked at -> the GL copy is current, nothing to do;
 *   - otherwise the whole range is hashed (every word) and uploaded if the hash changed, as before.
 * Asking resets the answer for those pages, and two textures can share a page (small textures) or the whole range (two
 * NVN textures on the same storage), so every written page gets a stamp in a table and a texture compares the newest stamp
 * of its pages with the stamp it saw last. Memory that is not write-watched (the image's own data, host heap) is hashed
 * as before. SSBU_NO_WRITEWATCH=1 restores the old behaviour everywhere. Render thread only. */
/* The stamps sit in a 4 KB array per 2 MB chunk (512 pages), found through a small table of the chunks, with the chunk's
 * newest stamp beside it: a range's pages are read in a row, and a chunk with nothing newer than what the caller saw last is
 * not read at all. (A table of single pages, 16 MB of scattered reads, was 5-7 % of the render thread at the title,
 * 2026-11-01.) At most ~9 MB for a 4.5 GB heap; nothing is ever emptied. */
#define WWK_SLOTS (1u << 14)
static struct { uint64_t chunk, stamp; uint64_t *pages; } g_wwk[WWK_SLOTS];
static uint64_t g_ww_stamp, g_ww_floor;                  /* g_ww_floor: a stamp every page counts as having (raised when a scan found too much) */
static int g_ww_off = -1;
static LONG64 g_ww_clean, g_ww_dirty, g_ww_unwatched;
static int wwk_find(uint64_t ch, int insert) {           /* the chunk's slot, or -1 */
    for (unsigned h = (unsigned)((ch * 0x9E3779B97F4A7C15ULL) >> 50), k = 0; k < WWK_SLOTS; h = (h + 1) & (WWK_SLOTS - 1), k++) {
        if (g_wwk[h].chunk == ch) return (int)h;
        if (!g_wwk[h].chunk) {
            if (!insert || !(g_wwk[h].pages = calloc(512, sizeof(uint64_t)))) return -1;
            g_wwk[h].chunk = ch;
            return (int)h;
        }
    }
    return -1;
}
static void ww_stamp(uint64_t page, uint64_t stamp) {
    int i = wwk_find(page >> 9, 1);
    if (i < 0) { g_ww_floor = stamp; return; }           /* (no room: everything counts as written now) */
    g_wwk[i].pages[page & 511] = stamp;
    if (stamp > g_wwk[i].stamp) g_wwk[i].stamp = stamp;
}
/* newest: the floor; seen: the stamp the caller saw last (0: none). Above `seen`, the answer is exact. */
static uint64_t ww_pages_newest(uint64_t first, uint64_t last, uint64_t newest, uint64_t seen) {
    for (uint64_t ch = first >> 9; ch <= last >> 9; ch++) {
        int i = wwk_find(ch, 0);
        if (i < 0 || g_wwk[i].stamp <= newest) continue;
        if (seen && g_wwk[i].stamp <= seen) { newest = g_wwk[i].stamp; continue; }
        const uint64_t *pg = g_wwk[i].pages;
        for (uint64_t k = ch == first >> 9 ? first & 511 : 0, hi = ch == last >> 9 ? last & 511 : 511; k <= hi; k++) if (pg[k] > newest) newest = pg[k];
    }
    return newest;
}
/* Newest write stamp of the pages of [p, p+n), or 0 when the memory is not write-watched. Never returns less than 1 otherwise. */
static void hot_mark(void *const *pages, ULONG_PTR count, int all);
static uint64_t ww_newest(const uint8_t *p, size_t n) {
    static void *pages[16384];
    ULONG_PTR count = 16384;
    DWORD gran = 0;
    if (g_ww_off < 0) { g_ww_off = getenv("SSBU_NO_WRITEWATCH") != NULL; g_ww_stamp = 1; }
    if (g_ww_off || !n || GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)p, n, pages, &count, &gran)) return 0;
    uint64_t first = (uint64_t)(uintptr_t)p >> 12, last = ((uint64_t)(uintptr_t)p + n - 1) >> 12, newest = g_ww_floor ? g_ww_floor : 1;
    if (count) {
        uint64_t stamp = ++g_ww_stamp;
        hot_mark(pages, count, count == 16384);
        if (count == 16384) {                      /* more written pages than the list holds: stamp the whole range */
            ResetWriteWatch((void *)p, n);
            for (uint64_t pg = first; pg <= last; pg++) ww_stamp(pg, stamp);
        } else for (ULONG_PTR i = 0; i < count; i++) ww_stamp((uint64_t)(uintptr_t)pages[i] >> 12, stamp);
    }
    return ww_pages_newest(first, last, newest, 0);
}
/* Uploads kept across batches (arena_put): a guest range streamed into the arena stays there while no page of it is
 * written. Once per batch every write-watched region is asked which pages were written (ww_scan_regions; they get a
 * stamp), and the emulator's own writes into guest memory (reports, copies, read-backs) stamp their range (ww_note); a
 * range's newest stamp is then table lookups. Before, every batch and every such write streamed everything again: about
 * 3,250 uploads (14 MB) a frame on the island. Memory that is not write-watched streams once per batch, as before. */
static uint64_t g_wwr_base[64], g_wwr_size[64], g_wwl_base[64], g_wwl_size[64];
static int g_wwr_n, g_wwl_n;
static SRWLOCK g_wwl_lock = SRWLOCK_INIT;
int nvn_ww_regions(uint64_t *base, uint64_t *size, int max) {      /* tomo.c TOMO_MEMSCAN: the game's heap */
    AcquireSRWLockShared(&g_wwl_lock);
    int n = g_wwl_n < max ? g_wwl_n : max;
    for (int i = 0; i < n; i++) { base[i] = g_wwl_base[i]; size[i] = g_wwl_size[i]; }
    ReleaseSRWLockShared(&g_wwl_lock);
    return n;
}
void nvn_ww_region(uint64_t base, uint64_t size, int add) {      /* nnsdk.c: memory reserved with MEM_WRITE_WATCH, made or released */
    AcquireSRWLockExclusive(&g_wwl_lock);
    int i = 0;
    while (i < g_wwl_n && g_wwl_base[i] != base) i++;
    if (add && i == g_wwl_n && g_wwl_n < 64) { g_wwl_base[g_wwl_n] = base; g_wwl_size[g_wwl_n++] = size; }
    else if (!add && i < g_wwl_n) { g_wwl_base[i] = g_wwl_base[--g_wwl_n]; g_wwl_size[i] = g_wwl_size[g_wwl_n]; }
    ReleaseSRWLockExclusive(&g_wwl_lock);
}
/* The scan asks only for the 2 MB chunks that hold a range the arena keeps (arena_put marks them): the regions are the
 * game's whole heap, 4.5 GB on the island, and GetWriteWatch over it cost 3.2 ms per batch, three batches a frame, on the
 * render thread (nearly 10 ms of an 18 ms frame at frame_rate = 60). A chunk asked for the first time may report writes
 * from long ago: its kept ranges are streamed again once. TOMO_WW_FULL=1: the whole regions, as before. */
#define WW_CHUNK_SHIFT 21
static uint64_t *g_wwc, g_wwc_n, g_wwc_cap;       /* the chunks (address >> WW_CHUNK_SHIFT), sorted when g_wwc_sorted */
static int g_wwc_sorted = 1;
static struct { uint64_t chunk; } g_wwc_set[1 << 14];      /* membership (open addressing; 0 = empty) */
static SRWLOCK g_wwc_lock = SRWLOCK_INIT;                  /* g_wwc: the render thread adds, the scan thread reads */
static void ww_chunk_mark(uint64_t addr, uint64_t size) {
    for (uint64_t ch = addr >> WW_CHUNK_SHIFT, last = (addr + size - 1) >> WW_CHUNK_SHIFT; ch <= last; ch++) {
        unsigned h = (unsigned)((ch * 0x9E3779B97F4A7C15ULL) >> 50);
        for (;; h = (h + 1) & ((1u << 14) - 1)) {
            if (g_wwc_set[h].chunk == ch) break;
            if (g_wwc_set[h].chunk) continue;
            if (g_wwc_n >= (1u << 13)) return;     /* (16 GB of chunks: the full scan below takes over) */
            g_wwc_set[h].chunk = ch;
            AcquireSRWLockExclusive(&g_wwc_lock);
            if (g_wwc_n == g_wwc_cap) { g_wwc_cap = g_wwc_cap ? g_wwc_cap * 2 : 256; g_wwc = realloc(g_wwc, g_wwc_cap * sizeof *g_wwc); }
            g_wwc[g_wwc_n++] = ch; g_wwc_sorted = 0;
            ReleaseSRWLockExclusive(&g_wwc_lock);
            break;
        }
    }
}
static int u64_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
/* The scan runs on a thread of its own (2026-10-30): GetWriteWatch with its reset costs about 60 us a call, a few ms a frame
 * on the island, 18 % of the render thread at 4K. The game asks for it when it submits a batch (ww_scan_request: its writes
 * for that batch are done then); the render thread, starting that batch, waits for that scan (normally long done while it
 * replayed the batch before) and stamps the pages it found (ww_scan_apply). Writes the scan sees early belong to a later
 * batch and only make a kept upload stream again sooner, as before. TOMO_WW_SYNC=1: the scan on the render thread. */
static SRWLOCK g_wws_lock = SRWLOCK_INIT;                  /* the pages found and not stamped yet */
static uint64_t *g_wws_pg;
static size_t g_wws_n, g_wws_cap;
static int g_wws_all, g_wws_sync = -1;
static volatile LONG64 g_wws_asked, g_wws_done;
static volatile LONG g_wws_started;
static HANDLE g_wws_ev, g_wws_done_ev;
static volatile LONG g_hot_page[1 << 18];                 /* by page (hashed buckets): the frame a question last found it written by the game (SnapSet) */
static volatile LONG g_hot_all;                           /* the frame of a question that found more written pages than it could list */
#define HOT_BUCKET(pg) (((uint64_t)(pg) * 0x9E3779B97F4A7C15ULL) >> 46)
static void hot_mark(void *const *pages, ULONG_PTR count, int all) {
    LONG fr = (LONG)g_st.frames;
    if (all) { g_hot_all = fr; return; }
    for (ULONG_PTR i = 0; i < count; i++) g_hot_page[HOT_BUCKET((uint64_t)(uintptr_t)pages[i] >> 12)] = fr;
}
static void ww_ask(uint64_t base, uint64_t size, void **pages, LONG64 *wpages) {
    ULONG_PTR count = 1 << 18;
    DWORD gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)base, (SIZE_T)size, pages, &count, &gran) || !count) return;
    *wpages += (LONG64)count;
    hot_mark(pages, count, count == (ULONG_PTR)1 << 18);
    AcquireSRWLockExclusive(&g_wws_lock);
    if (count == (ULONG_PTR)1 << 18) g_wws_all = 1;          /* more than the list holds: everything counts as written */
    else {
        if (g_wws_n + count > g_wws_cap) { g_wws_cap = (g_wws_n + count) * 2; g_wws_pg = realloc(g_wws_pg, g_wws_cap * sizeof *g_wws_pg); }
        for (ULONG_PTR i = 0; i < count; i++) g_wws_pg[g_wws_n++] = (uint64_t)(uintptr_t)pages[i] >> 12;
    }
    ReleaseSRWLockExclusive(&g_wws_lock);
}
static void ww_scan_once(void) {
    static void *pages[1 << 18];
    static uint64_t *ch, cap;
    uint64_t base[64], size[64], n;
    int nr;
    AcquireSRWLockShared(&g_wwl_lock);
    nr = g_wwl_n;
    memcpy(base, g_wwl_base, sizeof base); memcpy(size, g_wwl_size, sizeof size);
    ReleaseSRWLockShared(&g_wwl_lock);
    AcquireSRWLockShared(&g_wwc_lock);
    n = g_wwc_n;
    if (n > cap) { cap = n * 2; ch = realloc(ch, cap * sizeof *ch); }
    if (n) memcpy(ch, g_wwc, n * sizeof *ch);
    ReleaseSRWLockShared(&g_wwc_lock);
    static int wwlog = -1, full = -1; static LONG64 wt, wn, wpages, wcalls;
    if (wwlog < 0) { wwlog = getenv("TOMO_WWLOG") != NULL; full = getenv("TOMO_WW_FULL") != NULL; }      /* TEST ONLY: the scan's cost; the old full scan */
    LONG64 w0 = wwlog ? pf_now() : 0;
    if (full || n >= (1u << 13)) {
        for (int r = 0; r < nr; r++) { ww_ask(base[r], size[r], pages, &wpages); wcalls++; }
    } else {
        qsort(ch, (size_t)n, sizeof *ch, u64_cmp);
        for (uint64_t i = 0; i < n;) {      /* runs of chunks less than 64 MB apart, cut to each region: a call costs about
                                             * 60 us whatever its size (its reset), 64 MB more about 0.04 ms */
            uint64_t j = i + 1;
            while (j < n && ch[j] - ch[j - 1] <= (64u << 20 >> WW_CHUNK_SHIFT)) j++;
            uint64_t a = ch[i] << WW_CHUNK_SHIFT, b = (ch[j - 1] + 1) << WW_CHUNK_SHIFT;
            for (int r = 0; r < nr; r++) {
                uint64_t lo = a > base[r] ? a : base[r], hi = b < base[r] + size[r] ? b : base[r] + size[r];
                if (lo < hi) { ww_ask(lo, hi - lo, pages, &wpages); wcalls++; }
            }
            i = j;
        }
    }
    if (wwlog && (wt += pf_now() - w0, ++wn % 300 == 0)) {
        uint64_t total = 0; for (int r = 0; r < nr; r++) total += size[r];
        printf("wwlog: %d regions, %.1f MB watched, %llu chunks of 2 MB asked for; %.3f ms, %.1f calls and %.0f written pages per scan (last 300)%c", nr, (double)total / 1048576.0,
               (unsigned long long)n, (double)wt / 1e4 / 300.0, (double)wcalls / 300.0, (double)wpages / 300.0, 10);
        wt = 0; wpages = 0; wcalls = 0;
    }
}
static DWORD WINAPI ww_scan_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_wws_ev, INFINITE);
        for (LONG64 want; (want = g_wws_asked) > g_wws_done;) { ww_scan_once(); InterlockedExchange64(&g_wws_done, want); SetEvent(g_wws_done_ev); }
    }
    return 0;
}
static uint64_t ww_scan_request(void) {             /* game thread, at a submission: 0 = the render thread scans itself */
    if (g_wws_sync < 0) g_wws_sync = getenv("TOMO_WW_SYNC") != NULL || getenv("SSBU_NO_WRITEWATCH") != NULL;
    if (g_wws_sync) return 0;
    if (InterlockedCompareExchange(&g_wws_started, 1, 0) == 0) {
        g_wws_done_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_wws_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, ww_scan_thread, NULL, 0, NULL);
        InterlockedExchange(&g_wws_started, 2);
    }
    while (g_wws_started != 2) SwitchToThread();
    uint64_t seq = (uint64_t)InterlockedIncrement64(&g_wws_asked);
    SetEvent(g_wws_ev);
    return seq;
}
static void ww_scan_apply(uint64_t seq) {           /* render thread, at the start of a batch */
    ww_newest(NULL, 0);                              /* (the table) */
    if (g_ww_off) { g_wwr_n = 0; return; }
    AcquireSRWLockShared(&g_wwl_lock);
    g_wwr_n = g_wwl_n;
    memcpy(g_wwr_base, g_wwl_base, sizeof g_wwr_base); memcpy(g_wwr_size, g_wwl_size, sizeof g_wwr_size);
    ReleaseSRWLockShared(&g_wwl_lock);
    {   /* TEST ONLY (TOMO_WWLOG): how long batches wait for their scan. Island 2026-11-01: the frame's big batch waits the whole
         * scan, 1.9 ms (279 chunks; 1.4 ms without the textures' chunks), the others nothing. A call costs ~0.07 ms plus ~1 us per
         * MB it covers, written or not, and calls do not overlap (one thread or two: the same). */
        static LONG64 wt, wn, wbig; static int lg = -1; LONG64 w0 = pf_now();
        if (lg < 0) lg = getenv("TOMO_WWLOG") != NULL;
        if (!seq) ww_scan_once();
        else while (g_wws_done < (LONG64)seq) WaitForSingleObject(g_wws_done_ev, 1);
        if (lg) { LONG64 d = pf_now() - w0; wt += d; wn++; if (d > 5000) wbig++;
                  if (wn % 900 == 0) { printf("wwwait: %.3f ms a batch on average, %lld of 900 batches over 0.5 ms%c", (double)wt / 1e4 / 900.0, (long long)wbig, 10); wt = 0; wbig = 0; } } }
    AcquireSRWLockExclusive(&g_wws_lock);
    if (g_wws_all) g_ww_floor = ++g_ww_stamp;
    if (g_wws_n) { uint64_t stamp = ++g_ww_stamp; for (size_t i = 0; i < g_wws_n; i++) ww_stamp(g_wws_pg[i], stamp); }
    g_wws_n = 0; g_wws_all = 0;
    ReleaseSRWLockExclusive(&g_wws_lock);
}
static uint64_t ww_range_stamp(uint64_t addr, uint64_t size, uint64_t seen) {      /* newest stamp of the range's pages; 0 = not write-watched */
    int r = 0;
    while (r < g_wwr_n && !(addr >= g_wwr_base[r] && addr + size <= g_wwr_base[r] + g_wwr_size[r])) r++;
    if (r == g_wwr_n || !size) return 0;
    return ww_pages_newest(addr >> 12, (addr + size - 1) >> 12, g_ww_floor ? g_ww_floor : 1, seen);
}
static uint32_t g_arena_gen;
static volatile LONG64 g_emu_serial;                     /* writes of ours into guest memory (ww_note), counted (SnapSet) */
static volatile LONG64 g_emu_page[1 << 16];              /* by page (hashed buckets): the count at the newest of them */
#define EMU_RING 65536
static struct { uint64_t addr, size; } g_emu_ring[EMU_RING];      /* the last writes of ours, by count (render thread) */
static void ww_note(uint64_t addr, uint64_t size) {      /* the emulator wrote guest memory: later reads stream it again */
    g_arena_gen++;
    if (size) { LONG64 v = g_emu_serial + 1; g_emu_ring[v % EMU_RING].addr = addr; g_emu_ring[v % EMU_RING].size = size; InterlockedExchange64(&g_emu_serial, v);
                for (uint64_t pg = addr >> 12, last = (addr + size - 1) >> 12; pg <= last; pg++) g_emu_page[(pg * 0x9E3779B97F4A7C15ULL) >> 48] = v; }
    if (g_ww_off != 0 || !size) return;
    uint64_t stamp = ++g_ww_stamp;
    for (uint64_t pg = addr >> 12, last = (addr + size - 1) >> 12; pg <= last; pg++) ww_stamp(pg, stamp);
}
/* Round 16 (nvn_vk_draw.c, SSBU_VK_CBKEEP): the same question for any guest range: has the CPU written a page of it since
 * the stamp the caller remembers? 0 = the memory is not write-watched. Render thread only. */
uint64_t nvn_front_mem_newest(const void *p, size_t n) { return ww_newest(p, n); }

/* ---- software-decoded (ASTC) textures off the render thread
 * A room or a talking scene brings dozens of ASTC textures at once; decoded here, the frame waited 100-300 ms (the game's
 * main thread with it). Only a burst goes elsewhere: a large texture (256 KB of blocks and up) never seen before, once this
 * frame has decoded DEC_BUDGET_MS here, is deswizzled here and decoded by the decoder thread (below normal priority, its
 * bands on the thread pool), and lands at the start of a later batch, a frame or two late, its small levels standing in
 * meanwhile (tex_upload_memory). Textures the game makes anew with the same blocks are copied instead (twin_copy). The user
 * saw flicker each time it went further (2026-10-28): zeros meanwhile (the island), then blurry item pictures in the
 * catalogue (small textures past a 2 ms budget, and the item's textures made anew every few frames). TOMO_DECODE_ASYNC=0 off. */
#define DEC_BUDGET_MS 8
typedef struct Dec { struct Dec *next; Tex *t; GLuint gl; uint32_t gen; int format, level, layer, w, h; Fmt f; uint8_t *buf; size_t lin; } Dec;
static SRWLOCK g_dec_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_dec_cv = CONDITION_VARIABLE_INIT;      /* the decoder waits for work, dec_finish for results */
static Dec *g_dec_todo, **g_dec_tail = &g_dec_todo, *volatile g_dec_done;
static LONG64 g_dec_spent, g_dec_budget;          /* render thread: QPC ticks of decoding this frame (job_present resets it) */
static size_t g_dec_bytes;                         /* render thread: held by queued levels (past 256 MB the decoding is done here again) */
static DWORD WINAPI dec_main(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);     /* the game's threads first; its bands inherit it */
    for (;;) {
        AcquireSRWLockExclusive(&g_dec_lock);
        while (!g_dec_todo) SleepConditionVariableSRW(&g_dec_cv, &g_dec_lock, INFINITE, 0);
        Dec *d = g_dec_todo;
        if (!(g_dec_todo = d->next)) g_dec_tail = &g_dec_todo;
        ReleaseSRWLockExclusive(&g_dec_lock);
        tex_decode_rgba8(d->format, d->buf, d->w, d->h, d->buf + d->lin);
        AcquireSRWLockExclusive(&g_dec_lock);
        d->next = g_dec_done; g_dec_done = d;
        ReleaseSRWLockExclusive(&g_dec_lock);
        WakeAllConditionVariable(&g_dec_cv);
    }
}
static int dec_async(void) {
    static int on = -1;
    if (on < 0) {
        on = !(getenv("TOMO_DECODE_ASYNC") && !strcmp(getenv("TOMO_DECODE_ASYNC"), "0")) && g_rb != &rb_vk;
        if (on) { HANDLE h = CreateThread(NULL, 0, dec_main, NULL, 0, NULL); if (h) CloseHandle(h); else on = 0; }
        LARGE_INTEGER q; QueryPerformanceFrequency(&q); g_dec_budget = q.QuadPart * DEC_BUDGET_MS / 1000;
    }
    return on;
}
static void tex_put(Tex *t, const Fmt *f, int level, int layer, int x, int y, int z, int w, int h, int d, const void *data);
static void dec_land(void) {                       /* render thread: the decoded levels into their textures */
    if (!g_dec_done) return;
    AcquireSRWLockExclusive(&g_dec_lock);
    Dec *d = g_dec_done; g_dec_done = NULL;
    ReleaseSRWLockExclusive(&g_dec_lock);
    for (Dec *next; d; d = next) {
        next = d->next;
        Tex *t = d->t;
        if (t->o.gl == d->gl && t->dec_gen == d->gen) tex_put(t, &d->f, d->level, d->layer, 0, 0, 0, d->w, d->h, 1, d->buf + d->lin);      /* (else newer texels came, or its GL object went) */
        t->dec_pending--;
        g_dec_bytes -= d->lin + (size_t)d->w * d->h * 4;
        free(d->buf); free(d);
    }
    GL_ERR_CLEAR_SOFT();
}
static void dec_finish(Tex *t) {                   /* render thread: wait for the texture's levels (a copy or a CPU write of it comes next) */
    while (t->dec_pending > 0) {
        AcquireSRWLockExclusive(&g_dec_lock);
        if (!g_dec_done) SleepConditionVariableSRW(&g_dec_cv, &g_dec_lock, 100, 0);
        ReleaseSRWLockExclusive(&g_dec_lock);
        dec_land();
    }
}

static void rb_land_range(uint64_t addr, uint64_t size);
static void rs_blit(GLuint from, int fl, GLuint to, int tl, GLenum att, const int *a, const int *b, GLbitfield mask);
static int g_rbp_n;
#include "nvn_gl_astc.c"                       /* ASTC levels decoded on the GPU */
static void tex_upload_memory(Tex *t, const Fmt *f, int async) {
    static uint8_t *tmp;
    static size_t tmp_cap;
    static int nlog;
    if (g_rbp_n && t->mem) rb_land_range((uint64_t)(uintptr_t)t->mem, t->storage ? t->storage : 1);
    TexBuilder *b = &t->b;
    const uint8_t *src = tex_source(t);
    if (b->flags & 0x10) {                         /* linear: plain rows with a stride */
        int64_t stride = b->stride ? b->stride : (int64_t)b->width * f->bpb;
        if (f->kind == 4) return;
        glPixelStorei(GL_UNPACK_ROW_LENGTH_, (GLint)(stride / f->bpb));
        tex_put(t, f, 0, 0, 0, 0, 0, b->width, b->height, 1, src);
        glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
        g_st.tex_bytes += stride * b->height;
        return;
    }
    TexLayout L;
    const TexFormat *tf = tex_format(b->format);
    Fmt sw = *f;
    tex_lay(b, &L);
    if (tf && tf->sw_decode) { sw.kind = 0; sw.format = 0x1908; sw.type = 0x1401; sw.bw = sw.bh = 1; sw.bpb = 4; }     /* decoded in software to RGBA8 */
    int defer = 0, queued = 0, keep = 0;
    if (tf && tf->sw_decode) {
        t->dec_gen++;                              /* (levels still with the decoder are older now) */
        defer = async && !ga_on() && dec_async() && t->data_size >= (256u << 10) && g_dec_spent >= g_dec_budget && g_dec_bytes < (256u << 20);
        if (defer) {                               /* (async: a first upload of blocks never seen before, tex_sync_a) the levels from
                                                    * `keep` (128x128 and smaller) are decoded now and stand in, scaled up, for the
                                                    * larger ones until they land (zeros flickered: a dark frame on the island whenever
                                                    * new textures came); no such level: decoded now */
            for (keep = 1; keep < t->glevels - 1 && keep < L.levels - 1 && (long long)L.lvl_w[keep] * L.lvl_h[keep] > 128 * 128; keep++) {}
            if (gl_target(b->target, b->samples) != GL_TEXTURE_2D || L.layers != 1 || t->rw || t->glevels < 2 || L.levels < 2) defer = keep = 0;
        }
    }
    for (int layer = 0; layer < L.layers; layer++) {
        int gpu_done = 0;                          /* the first levels of this layer, decoded on the GPU by one dispatch (nvn_gl_astc.c) */
        if (tf && tf->sw_decode && !defer && ga_on()) {
            size_t offs[GA_LEVELS], total = 0;
            int nl = 0;
            for (; nl < t->glevels && nl < L.levels && nl < GA_LEVELS; nl++) {
                const uint8_t *p = tex_level_ptr(&L, src, nl, layer);
                if (!b->packaged && t->limit && p + L.lvl_size[nl] > t->limit) break;      /* (reported by the loop below) */
                offs[nl] = total; total += (L.lvl_linear_size[nl] + 15) & ~(size_t)15;
            }
            size_t scratch = g_ga.check ? (size_t)L.lvl_w[0] * L.lvl_h[0] * 4 : 0;
            if (nl && total + scratch > tmp_cap) { tmp_cap = (total + scratch) * 2; tmp = realloc(tmp, tmp_cap); }
            for (int l = 0; l < nl; l++) tex_deswizzle_level(&L, l, tex_level_ptr(&L, src, l, layer), tmp + offs[l]);
            if (nl) { LONG64 d0 = pf_now(); gpu_done = ga_decode_levels(t, tf, &L, tmp, offs, nl, layer, gl_target(b->target, b->samples), tmp + total); g_dec_spent += pf_now() - d0; }
            for (int l = 0; l < gpu_done; l++) g_st.tex_bytes += (LONG64)L.lvl_linear_size[l];
        }
        for (int l = gpu_done; l < t->glevels && l < L.levels; l++) {
            const uint8_t *p = tex_level_ptr(&L, src, l, layer);
            size_t lin = L.lvl_linear_size[l], need = lin + (tf && tf->sw_decode ? (size_t)L.lvl_w[l] * L.lvl_h[l] * 4 : 0);
            if (!b->packaged && t->limit && p + L.lvl_size[l] > t->limit) {
                logf_limited(&nlog, 20, "nvn_gl: texture %dx%d format 0x%x level %d layer %d runs past its memory pool; not uploaded\n", b->width, b->height, b->format, l, layer);
                if (queued) WakeAllConditionVariable(&g_dec_cv);
                return;
            }
            Dec *d = defer && (!keep || l < keep) ? malloc(sizeof *d) : NULL;
            if (d && !(d->buf = malloc(need))) { free(d); d = NULL; }
            if (d) {
                tex_deswizzle_level(&L, l, p, d->buf);
                d->t = t; d->gl = t->o.gl; d->gen = t->dec_gen; d->format = b->format; d->level = l; d->layer = layer; d->w = L.lvl_w[l]; d->h = L.lvl_h[l]; d->f = sw; d->lin = lin; d->next = NULL;
                t->dec_pending++; queued = 1; g_dec_bytes += need;
                AcquireSRWLockExclusive(&g_dec_lock); *g_dec_tail = d; g_dec_tail = &d->next; ReleaseSRWLockExclusive(&g_dec_lock);
                g_st.tex_bytes += (LONG64)lin;
                continue;
            }
            if (need > tmp_cap) { tmp_cap = need * 2; tmp = realloc(tmp, tmp_cap); }
            tex_deswizzle_level(&L, l, p, tmp);
            if (tf && tf->sw_decode) {
                LONG64 d0 = pf_now();
                tex_decode_rgba8(b->format, tmp, L.lvl_w[l], L.lvl_h[l], tmp + lin);
                tex_put(t, &sw, l, layer, 0, 0, 0, L.lvl_w[l], L.lvl_h[l], 1, tmp + lin);
                g_dec_spent += pf_now() - d0;
            } else tex_put(t, f, l, layer, 0, 0, 0, L.lvl_w[l], L.lvl_h[l], L.lvl_d[l], tmp);
            g_st.tex_bytes += (LONG64)lin;
        }
    }
    if (queued) WakeAllConditionVariable(&g_dec_cv);
    for (int l = 0; queued && l < keep; l++) {
        int a[4] = {0, 0, L.lvl_w[keep], L.lvl_h[keep]}, z[4] = {0, 0, L.lvl_w[l], L.lvl_h[l]};
        rs_blit(t->o.gl, keep, t->o.gl, l, GL_COLOR_ATTACHMENT0_, a, z, GL_COLOR_BUFFER_BIT);
    }
}

/* ---- the same texels again: the game's texture streaming makes textures anew every few frames, the same blocks at another
 * place. A software-decoded texture whose blocks (full hash) and shape equal an earlier one's is copied from that one's GL
 * texture on the GPU instead of being decoded again (nothing to wait for, nothing blank). TOMO_TWIN=0 off. */
#define TWIN_SLOTS 4096
static struct { uint64_t key, h; Tex *t; } g_twin[TWIN_SLOTS];
static LONG64 g_twin_hits;
/* Every block hash uploaded so far: blocks seen before are texels the game showed already (a texture refilled or made anew
 * after its twin went): decoded at once, never late, so a picture on screen does not turn blurry or old for a frame. */
#define SEEN_SLOTS (1u << 16)
static uint64_t g_seen[SEEN_SLOTS];
static int seen_has(uint64_t key, int add) {
    for (unsigned i = (unsigned)(key * 0x9E3779B97F4A7C15ULL >> 48), n = 0; key && n < 64; i = (i + 1) & (SEEN_SLOTS - 1), n++) {
        if (g_seen[i] == key) return 1;
        if (!g_seen[i]) { if (add) g_seen[i] = key; return 0; }
    }
    return 0;                                      /* (a full neighbourhood: not recorded) */
}
static uint64_t twin_key(Tex *t, uint64_t h, int full) {      /* 0: not a candidate */
    static int on = -1;
    if (on < 0) on = !(getenv("TOMO_TWIN") && !strcmp(getenv("TOMO_TWIN"), "0"));
    const TexFormat *tf = tex_format(t->b.format);
    if (!on || !tf || !tf->sw_decode || (t->b.flags & 0x10) || t->rw) return 0;
    uint64_t k = full ? h : mem_hash(tex_source(t), t->data_size, 1);
    return k ? k : 1;
}
static GLuint twin_copy(Tex *t, uint64_t key) {   /* the GL texture copied from, 0 = none */
    unsigned i = (unsigned)key & (TWIN_SLOTS - 1);
    Tex *c = g_twin[i].t;
    if (!key || !c || c == t || g_twin[i].key != key || c->hash != g_twin[i].h || !c->o.gl || c->gpu_written || c->rw || c->b.format != t->b.format
        || c->b.width != t->b.width || c->b.height != t->b.height || c->b.depth != t->b.depth || c->b.target != t->b.target || c->glevels != t->glevels
        || !gl_has(glCopyImageSubData, "glCopyImageSubData")) return 0;
    ga_barrier();                                  /* (the twin's own levels may still be queued for the GPU decoder) */
    if (c->dec_pending) dec_finish(c);
    TexLayout L;
    GLenum tg = gl_target(t->b.target, t->b.samples);
    tex_lay(&t->b, &L);
    GL_ERR_CLEAR_SOFT();
    for (int l = 0; l < t->glevels && l < L.levels; l++)
        glCopyImageSubData(c->o.gl, tg, l, 0, 0, 0, t->o.gl, tg, l, 0, 0, 0, L.lvl_w[l], L.lvl_h[l], tg == GL_TEXTURE_3D_ ? L.lvl_d[l] : L.layers);
    if (GL_ERR_SOFT()) return 0;
    t->dec_gen++;                                  /* (levels of older texels still with the decoder are dropped) */
    g_twin_hits++;
    return c->o.gl;
}

/* Make the GL texture exist and hold what the guest memory holds. async: only sampled next (its decoded levels may land a
 * frame or two late, dec_*); otherwise the levels still with the decoder are waited for. */
/* A texture's newest write stamp. Once its memory is in the scan's chunks (the scan thread asks for them at every
 * submission, ww_scan_apply stamps them at the batch start), table lookups: no GetWriteWatch call of its own, which cost
 * about 12 us a texture on the render thread (1 ms a frame on the island, 2-4 ms at the title: 370-660 checks). The
 * first look asks itself and marks the chunks. TOMO_TEX_WWSCAN=0: every look asks, as before. */
static uint64_t tex_ww_newest(Tex *t, const uint8_t *src) {
    static int on = -1;
    if (on < 0) on = !(getenv("TOMO_TEX_WWSCAN") && getenv("TOMO_TEX_WWSCAN")[0] == '0');
    if (on && t->ww_marked && !g_wws_sync) { uint64_t s = ww_range_stamp((uint64_t)(uintptr_t)src, t->data_size, t->uploaded && !t->dirty && t->ww_hashed ? t->ww_seen : 0); if (s) return s; }
    uint64_t r = ww_newest(src, t->data_size);
    if (on && r && !g_wws_sync && !t->b.packaged) { ww_chunk_mark((uint64_t)(uintptr_t)src, t->data_size); t->ww_marked = 1; }
    return r;
}
static void tex_sync_a(Tex *t, int async) {
    Fmt f;
    if (t->dec_pending && !async) dec_finish(t);
    if (t->dead) return;                           /* finalized: its memory may hold something else now */
    if (!t->o.gl) tex_realize(t);
    if (t->label_dirty && t->o.gl) { t->label_dirty = 0; if (glObjectLabel) glObjectLabel(0x1702 /* TEXTURE */, t->o.gl, -1, nvn_label_get(t->guest)); }
    if (t->checked == g_serial) return;
    t->checked = g_serial;
    const uint8_t *src = tex_source(t);
    if (t->gpu_written || !src || t->b.samples > 1 || !fmt_get(t->b.format, &f) || (f.kind >= 1 && f.kind <= 3)) return;
    if (!t->b.packaged && t->limit && src + t->data_size > t->limit) return;
    if (t->b.packaged && t->uploaded) return;      /* packaged data is consumed once at creation */
    LONG64 pf0 = pf_now();
    HS_IN("texture check (write watch, hash)", 0);
    uint64_t newest = tex_ww_newest(t, src);
    if (newest) {
        if (t->uploaded && !t->dirty && t->ww_hashed && newest <= t->ww_seen) { g_ww_clean++; pf_add(PF_TEXHASH, pf0); HS_OUT(); return; }      /* not written since the last look */
        t->ww_seen = newest;
        g_ww_dirty++;
    } else g_ww_unwatched++;
    int full = newest != 0 || (t->b.flags & 0x10) != 0;
    uint64_t h = mem_hash(src, t->data_size, full);
    int same = t->uploaded && !t->dirty && h == t->hash && (!newest || t->ww_hashed);
    t->ww_hashed = newest != 0;                    /* t->hash is the hash of every word (needed to compare the next time) */
    pf_add(PF_TEXHASH, pf0); g_pf_hash_bytes += (LONG64)t->data_size;
    HS_OUT();
    if (same) return;
    GL_ERR_CLEAR_SOFT();
    pf0 = pf_now();
    uint64_t key;
    GLuint twin;
    { static int lf = -2; if (lf == -2) lf = getenv("TOMO_UPLOADLOG") ? atoi(getenv("TOMO_UPLOADLOG")) : -1;
      static int ln = -2; if (ln == -2) ln = getenv("TOMO_UPLOADLOG_N") ? atoi(getenv("TOMO_UPLOADLOG_N")) : 2;
      static LONG64 qpf; if (!qpf) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); qpf = q.QuadPart; }
      int log_it = lf >= 0 && g_st.frames >= lf && g_st.frames < lf + ln, again = t->uploaded, dirty = t->dirty;
      key = twin_key(t, h, full);
      int seen = seen_has(key, 0);
      if (!(twin = twin_copy(t, key))) { HS_IN("texture upload (deswizzle, decode, glTexSubImage)", 0); tex_upload_memory(t, &f, async && !again && !seen); HS_OUT(); }
      if (log_it) printf("uploadlog: frame %lld: texture target %d %dx%dx%d format 0x%x levels %d flags 0x%x %llu bytes gl %u%s%s, %.1f ms%s\n", (long long)g_st.frames,
          t->b.target, t->b.width, t->b.height, t->b.depth, t->b.format, t->b.levels, (unsigned)t->b.flags, (unsigned long long)t->data_size, t->o.gl, again ? " (again)" : " (first)", dirty ? " dirty" : "",
          (double)(pf_now() - pf0) * 1000.0 / (double)qpf, twin ? " (copied from an earlier texture)" : t->dec_pending ? " (deferred)" : seen ? " (seen before)" : ""); }
    pf_add(PF_TEXUP, pf0);
    GLenum e = GL_ERR_SOFT();
    static int nlog;
    if (e) logf_limited(&nlog, 20, "nvn_gl: upload of texture %dx%d format 0x%x target %d failed with GL error 0x%x\n", t->b.width, t->b.height, t->b.format, t->b.target, e);
    t->hash = h; t->uploaded = 1; t->dirty = 0; t->upframe = g_st.frames;
    if (key && !e) { unsigned i = (unsigned)key & (TWIN_SLOTS - 1); g_twin[i].key = key; g_twin[i].h = h; g_twin[i].t = t; seen_has(key, 1); }
    tex_release_packaged(t);
    g_st.tex_uploads++;
}
static void tex_sync(Tex *t) { tex_sync_a(t, 0); }

static void eview_base(Tex *t, EView *e) {
    memset(e, 0, sizeof *e);
    e->target = t->b.target; e->format = t->b.format; e->nlvl = t->glevels; e->nlay = tex_layers(&t->b); e->dsmode = t->b.dsmode;
    memcpy(e->swz, t->b.swz, 4);
}
/* The GL texture object that stands for `t` seen through `v` (the texture itself when the view changes nothing). */
static TexObj *tex_view_obj(Tex *t, const View *v) {
    EView base, e;
    eview_base(t, &base);
    e = base;
    if (!v || !(v->set & 0x80)) return &t->o;
    if (v->set & 1) { e.lvl0 = v->lvl0; e.nlvl = v->nlvl; }
    if (v->set & 2) { e.lay0 = v->lay0; e.nlay = v->nlay; }
    if ((v->set & 4) && v->format) e.format = v->format;
    if (v->set & 8) memcpy(e.swz, v->swz, 4);
    if (v->set & 16) e.dsmode = (v->set >> 6) & 1;
    if (v->set & 32) e.target = v->target;
    if (e.lvl0 < 0 || e.lvl0 >= base.nlvl) e.lvl0 = 0;
    if (e.nlvl <= 0 || e.lvl0 + e.nlvl > base.nlvl) e.nlvl = base.nlvl - e.lvl0;
    if (e.lay0 < 0 || e.lay0 >= base.nlay) e.lay0 = 0;
    if (e.nlay <= 0 || e.lay0 + e.nlay > base.nlay) e.nlay = base.nlay - e.lay0;
    if (t->b.target == 2 || t->b.samples > 1) { e.target = base.target; e.lay0 = 0; e.nlay = base.nlay; }      /* no layer views of 3D / multisample */
    if ((e.target == 1 || e.target == 7 || e.target == 0) && e.nlay > 1) e.nlay = 1;
    if (e.target == 8 && e.nlay != 6) e.target = e.nlay == 1 ? 1 : 4;
    if (e.target == 9 && e.nlay % 6) e.target = 4;
    if (!memcmp(&e, &base, sizeof e)) return &t->o;
    for (VNode *n = t->views; n; n = n->next) if (!memcmp(&n->e, &e, sizeof e)) return &n->o;
    VNode *n = calloc(1, sizeof *n);
    Fmt f;
    static int nlog;
    n->e = e; n->next = t->views; t->views = n;
    if (!fmt_get(e.format, &f)) fmt_get(t->b.format, &f);
    {   /* a software-decoded format (ASTC: no desktop GPU support) is stored as its decoded format: the view must name that one */
        const TexFormat *tf = tex_format(t->b.format);
        if (tf && tf->sw_decode && tf->gl_sw_internal) { const TexFormat *vf = e.format ? tex_format(e.format) : NULL; f.internal = vf && vf->sw_decode && vf->gl_sw_internal ? vf->gl_sw_internal : tf->gl_sw_internal; }
    }
    GLenum tg = gl_target(e.target, t->b.samples);
    {   /* A render target seen through a view that only changes the swizzle: sampled through its bindless handle, such a view
         * kept giving texels from before the latest rendering (the NVIDIA driver resolves what was drawn for the texture
         * object that was the render target, not for other objects over its storage): Tomodachi Life's sun mask read 0
         * and the island had no sunlight in a new game. Such a view is a copy of the texture with the view's swizzle,
         * copied again before a draw that samples it whenever the texture was a render target since (view_copies_refresh). */
        int same_shape = e.target == base.target && e.lvl0 == 0 && e.nlvl == base.nlvl && e.lay0 == 0 && e.nlay == base.nlay && (!e.format || e.format == base.format) && e.dsmode == base.dsmode;
        static int novc = -1; if (novc < 0) novc = getenv("TOMO_NOVIEWCOPY") != NULL;
        if (!novc && t->gpu_written && same_shape && memcmp(e.swz, base.swz, 4) && t->b.samples <= 1 && f.kind == 0 && (tg == GL_TEXTURE_2D || tg == GL_TEXTURE_2D_ARRAY_)
            && gl_has(glCopyImageSubData, "glCopyImageSubData")) {
            int w = tw(t), hgt = th(t);
            while (glGetError()) {}
            glGenTextures(1, &n->o.gl);
            glBindTexture(tg, n->o.gl);
            if (tg == GL_TEXTURE_2D_ARRAY_) glTexStorage3D(tg, e.nlvl, f.internal, w, hgt, e.nlay); else glTexStorage2D(tg, e.nlvl, f.internal, w, hgt);
            if (!glGetError()) {
                glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, e.nlvl > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
                glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                set_swizzle(tg, e.swz);
                n->is_copy = 1; n->copy_gen = t->rt_gen - 1; t->vc_gen = 0;    /* filled by view_copies_refresh */
                g_copy_views++;
                InterlockedIncrement64(&g_tex_live);
                g_st.views++;
                return &n->o;
            }
            glDeleteTextures(1, &n->o.gl); n->o.gl = 0;
            while (glGetError()) {}
        }
    }
    /* Whether the driver takes a view of this kind is asked once (glGetError waits for NVIDIA's own GL thread: views made
     * while the title runs were 2 % of the render thread, 2026-11-01); a kind it took before is not asked again. */
    static uint64_t vok[1024];
    uint64_t vkey = ((uint64_t)t->b.format << 40 | (uint64_t)e.format << 24 | (uint64_t)e.target << 16 | (uint64_t)t->b.target << 8 | (t->b.samples > 1)) + 1;
    unsigned vi = (unsigned)((vkey * 0x9E3779B97F4A7C15ULL) >> 54);
    int known = vok[vi] == vkey;
    if (!known) while (glGetError()) {}
    glGenTextures(1, &n->o.gl);
    glTextureView(n->o.gl, tg, t->o.gl, f.internal, (GLuint)e.lvl0, (GLuint)e.nlvl, (GLuint)e.lay0, (GLuint)e.nlay);
    GLenum err = known ? 0 : glGetError();
    if (!err) vok[vi] = vkey;
    if (err) {
        logf_limited(&nlog, 30, "nvn_gl: view (target %d format 0x%x levels %d+%d layers %d+%d) of texture (target %d format 0x%x %dx%d) refused: GL error 0x%x\n",
                     e.target, e.format, e.lvl0, e.nlvl, e.lay0, e.nlay, t->b.target, t->b.format, t->b.width, t->b.height, err);
        glDeleteTextures(1, &n->o.gl);
        /* A compressed format the driver cannot view (ASTC: no view class), the view keeping the texture's shape, so only the
         * swizzle or the depth-stencil mode differs: a copy of the texture carries the view's parameters. Static assets:
         * the copy is not refreshed if the texture is uploaded again. */
        int same_shape = e.target == base.target && e.lvl0 == 0 && e.nlvl == base.nlvl && e.lay0 == 0 && e.nlay == base.nlay && (!e.format || e.format == base.format);
        if (f.kind == 4 && same_shape && t->b.samples <= 1 && (tg == GL_TEXTURE_2D || tg == GL_TEXTURE_2D_ARRAY_ || tg == GL_TEXTURE_CUBE_MAP_) && gl_has(glCopyImageSubData, "glCopyImageSubData")) {
            static int ncopy;
            int w = t->b.width > 0 ? t->b.width : 1, hgt = t->b.height > 0 ? t->b.height : 1, layers = tex_layers(&t->b);
            while (glGetError()) {}
            glGenTextures(1, &n->o.gl);
            glBindTexture(tg, n->o.gl);
            if (tg == GL_TEXTURE_2D_ARRAY_) glTexStorage3D(tg, e.nlvl, f.internal, w, hgt, layers); else glTexStorage2D(tg, e.nlvl, f.internal, w, hgt);
            GLenum e1 = glGetError(), e2 = 0;
            for (int l = 0; l < e.nlvl && !e2; l++) { glCopyImageSubData(t->o.gl, tg, l, 0, 0, 0, n->o.gl, tg, l, 0, 0, 0, lvl(w, l), lvl(hgt, l), tg == GL_TEXTURE_2D ? 1 : layers); e2 = glGetError(); if (e2) logf_limited(&ncopy, 6, "nvn_gl:   level %d (%dx%d) copy: GL error 0x%x\n", l, lvl(w, l), lvl(hgt, l), e2); }
            if (!e1 && !e2) {
                logf_limited(&ncopy, 6, "nvn_gl:   the view is a copy of the texture with the view's swizzle (%d %d %d %d)\n", e.swz[0], e.swz[1], e.swz[2], e.swz[3]);
                glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, e.nlvl > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
                glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                set_swizzle(tg, e.swz);
                InterlockedIncrement64(&g_tex_live);
                g_st.views++;
                return &n->o;
            }
            logf_limited(&ncopy, 6, "nvn_gl:   the copy failed: storage GL error 0x%x, copy 0x%x (internal 0x%x, %d levels)\n", e1, e2, f.internal, e.nlvl);
            glDeleteTextures(1, &n->o.gl);
        } else logf_limited(&nlog, 30, "nvn_gl:   no copy: kind %d same shape %d target 0x%x (view target %d base %d, levels %d/%d, layers %d/%d)\n", f.kind, same_shape, tg, e.target, base.target, e.nlvl, base.nlvl, e.nlay, base.nlay);
        n->o.gl = t->o.gl; n->o.alias = 1;
        return &n->o;
    }
    glBindTexture(tg, n->o.gl);
    InterlockedIncrement64(&g_tex_live);
    if (tg != GL_TEXTURE_2D_MULTISAMPLE_ && tg != GL_TEXTURE_2D_MULTISAMPLE_ARRAY_) {
        glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, e.nlvl > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        set_swizzle(tg, e.swz);
        if (e.dsmode == 1) glTexParameteri(tg, GL_DEPTH_STENCIL_TEXTURE_MODE_, GL_STENCIL_INDEX_);
    }
    g_st.views++;
    return &n->o;
}

static int g_texfree_skip = -1;                        /* TOMO_TEXFREE=<bits> (diagnosis): 1 keep base textures, 2 keep views, 4 keep handles resident */
/* Freed GL textures wait GRAVE_FRAMES presented frames before their bindless handles go non-resident and the texture is
 * deleted: commands the GPU has not run yet may still use the handles, and making a handle non-resident or deleting its
 * texture under them is undefined (ARB_bindless_texture). Done at once, the NVIDIA driver crashed inside glDeleteTextures
 * when the game freed many render targets in a few frames (a scene change, dynamic resolution). */
#define GRAVE_FRAMES 4
typedef struct { GLuint gl; int nh, del; uint64_t *h; long long frame; } Grave;
static Grave *g_grave;
static int g_grave_n, g_grave_cap;
static long long grave_now(void) { return (long long)g_st.frames; }
static void grave_bury(const TexObj *o, int del) {
    if (g_grave_n == g_grave_cap) { g_grave_cap = g_grave_cap ? g_grave_cap * 2 : 256; g_grave = realloc(g_grave, sizeof *g_grave * (size_t)g_grave_cap); }
    Grave *g = &g_grave[g_grave_n++];
    g->gl = o->gl; g->del = del; g->nh = o->nh; g->frame = grave_now();
    g->h = NULL;
    if (o->nh) { g->h = malloc(sizeof *g->h * (size_t)o->nh); for (int i = 0; i < o->nh; i++) g->h[i] = o->h[i].h; }
}
static void grave_sweep(int all) {                  /* render thread: the textures freed GRAVE_FRAMES frames ago (all: every one) */
    long long now = grave_now();
    int k = 0;
    for (int i = 0; i < g_grave_n; i++) {
        Grave *g = &g_grave[i];
        if (!all && now - g->frame < GRAVE_FRAMES) { g_grave[k++] = *g; continue; }
        HS_IN("texture free (handles non-resident)", g->gl);
        if (!(g_texfree_skip & 4)) for (int j = 0; j < g->nh; j++) glMakeTextureHandleNonResidentARB(g->h[j]);
        HS_OUT();
        free(g->h);
        if (g->del) { HS_IN("texture free (framebuffers, glDeleteTextures)", g->gl); fbo_purge(g->gl); glDeleteTextures(1, &g->gl); HS_OUT(); }
    }
    g_grave_n = k;
}
static void texobj_free(TexObj *o, int is_view) {
    if (g_texfree_skip < 0) { const char *e = getenv("TOMO_TEXFREE"); g_texfree_skip = e ? atoi(e) : 0; }
    InterlockedExchangeAdd64(&g_handles_live, -(LONG64)o->nh);
    if (o->gl && !o->alias) InterlockedDecrement64(&g_tex_live);
    if (o->gl || o->nh) grave_bury(o, o->gl && !o->alias && !(g_texfree_skip & (is_view ? 2 : 1)));
    free(o->h);
    o->h = NULL; o->nh = 0;
    o->gl = 0;
}
static struct { GLuint gl; int target, format, w, h, d, levels, nh, views, gpu; uint32_t flags; char vinfo[160]; } g_freed[16];
static unsigned g_freed_n;
void nvn_front_crash_notes(FILE *out) {                   /* hostrt.c: the textures freed last (the driver crashed in glDeleteTextures once) */
    fprintf(out, "  textures freed last (newest first; %d spare render targets held):\n", g_spare_n);
    for (unsigned k = 0; k < 16 && k < g_freed_n; k++) {
        unsigned i = (g_freed_n - 1 - k) % 16;
        fprintf(out, "    gl %u target %d format 0x%x %dx%dx%d levels %d flags 0x%x handles %d views %d%s\n", g_freed[i].gl, g_freed[i].target, g_freed[i].format, g_freed[i].w, g_freed[i].h, g_freed[i].d,
                g_freed[i].levels, g_freed[i].flags, g_freed[i].nh, g_freed[i].views, g_freed[i].gpu ? " gpu-written" : "");
        if (g_freed[i].vinfo[0]) fprintf(out, "      views:%s\n", g_freed[i].vinfo);
    }
}
static void alias_handoff(Tex *t);
static int rtlog_match(const Tex *t) {         /* TEST ONLY, TOMO_RTLOG=<w>x<h>[:<format hex>]: writes into such textures are logged */
    static int lw = -2, lh, lf = -1;
    if (lw == -2) { const char *e = getenv("TOMO_RTLOG"); lw = -1; if (e && sscanf(e, "%dx%d", &lw, &lh) != 2) lw = -1; if (e && strchr(e, ':')) lf = (int)strtol(strchr(e, ':') + 1, NULL, 16); }
    return lw > 0 && t && t->b.width == lw && t->b.height == lh && (lf < 0 || t->b.format == lf);
}
static void tex_crop_free(Tex *c) {
    if (!c) return;
    for (VNode *n = c->views, *next; n; n = next) { next = n->next; texobj_free(&n->o, 1); free(n); }
    texobj_free(&c->o, 0); free(c);
}
static void tex_free_gl(Tex *t) {
    alias_handoff(t);
    if (t->frz_snap) { glDeleteTextures(1, &t->frz_snap); t->frz_snap = 0; }
    {   unsigned i = g_freed_n % 16; int nv = 0;
        for (VNode *n = t->views; n; n = n->next) nv++;
        g_freed[i].gl = t->o.gl; g_freed[i].target = t->b.target; g_freed[i].format = t->b.format; g_freed[i].w = t->b.width; g_freed[i].h = t->b.height; g_freed[i].d = t->b.depth;
        g_freed[i].levels = t->glevels; g_freed[i].flags = (uint32_t)t->b.flags; g_freed[i].nh = t->o.nh; g_freed[i].views = nv; g_freed[i].gpu = t->gpu_written;
        g_freed[i].vinfo[0] = 0;
        for (VNode *n = t->views; n; n = n->next) {
            size_t k = strlen(g_freed[i].vinfo);
            if (k + 60 >= sizeof g_freed[i].vinfo) break;
            snprintf(g_freed[i].vinfo + k, sizeof g_freed[i].vinfo - k, " [gl %u%s target %d fmt 0x%x lvl %d+%d lay %d+%d swz %d%d%d%d ds %d h %d]", n->o.gl, n->o.alias ? " alias" : "", n->e.target, n->e.format,
                     n->e.lvl0, n->e.nlvl, n->e.lay0, n->e.nlay, n->e.swz[0], n->e.swz[1], n->e.swz[2], n->e.swz[3], n->e.dsmode, n->o.nh);
        }
        g_freed_n++;
    }
    if (t->gpu_written && t->o.gl && !t->o.alias && g_rb != &rb_vk) {
        /* dynamic resolution (the game lowers its render size while frames are slow, a new size every frame): at most a few
         * spares of one shape and SPARE_MAX in all; the oldest goes (freed for real) to make room */
        int same = 0, oldest_same = -1;
        for (int i = 0; i < g_spare_n; i++) if (!memcmp(&g_spare[i].b, &t->b, SPARE_KEY)) { if (oldest_same < 0) oldest_same = i; same++; }
        int drop = same >= 4 ? oldest_same : g_spare_n >= SPARE_MAX ? 0 : -1;
        if (drop >= 0) {
            for (VNode *n = g_spare[drop].views, *next; n; n = next) { next = n->next; texobj_free(&n->o, 1); free(n); }
            texobj_free(&g_spare[drop].o, 0);
            tex_crop_free(g_spare[drop].crop); tex_crop_free(g_spare[drop].scratch);
            memmove(&g_spare[drop], &g_spare[drop + 1], sizeof g_spare[0] * (size_t)(g_spare_n - 1 - drop)); g_spare_n--;
            g_spare_full++;
        }
        if (g_spare_n < SPARE_MAX) {
            g_spare[g_spare_n].b = t->b; g_spare[g_spare_n].o = t->o; g_spare[g_spare_n].views = t->views; g_spare[g_spare_n].glevels = t->glevels;
            g_spare[g_spare_n].crop = t->crop;     /* the 16:9 part goes with it: made anew for every remade target, its creation waited for NVIDIA's GL thread */
            g_spare[g_spare_n].scratch = t->scratch;
            g_spare_n++; g_spare_kept++;
            memset(&t->o, 0, sizeof t->o); t->views = NULL; t->crop = NULL; t->scratch = NULL;
            tex_release_packaged(t);
            return;
        }
        g_spare_full++;
    }
    for (VNode *n = t->views, *next; n; n = next) { next = n->next; texobj_free(&n->o, 1); free(n); }
    t->views = NULL;
    texobj_free(&t->o, 0);
    tex_crop_free(t->crop); t->crop = NULL;
    tex_crop_free(t->scratch); t->scratch = NULL;    /* (once never freed: a texture and its handles per remade target, until the driver's residency limit) */
    tex_release_packaged(t);
}

/* GL sampler objects are shared by value and never deleted. */
static struct { SamplerParams p; GLuint gl; } g_samplers[512];
static int g_sampler_count;
static GLuint sampler_gl(const SamplerParams *p) {
    static const GLenum minf[6] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR};
    static const GLenum wrap[8] = {GL_CLAMP, GL_REPEAT, 0x8742, 0x8743, 0x8912, 0x812D, 0x8370, 0x812F};
    /* by a hash of the parameters (1024 slots, indices + 1): a texture bind searched the ~80 samplers one by one (3,000 binds
     * a frame on the island) */
    static int16_t slot[1024];
    uint64_t hh = 0xcbf29ce484222325ULL;
    for (size_t k = 0; k < sizeof *p; k++) hh = (hh ^ ((const uint8_t *)p)[k]) * 0x100000001b3ULL;
    unsigned at = (unsigned)(hh >> 54);
    for (; slot[at]; at = (at + 1) & 1023) if (!memcmp(&g_samplers[slot[at] - 1].p, p, sizeof *p)) return g_samplers[slot[at] - 1].gl;
    GLuint s = 0;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, (GLint)minf[p->min < 6 ? p->min : 1]);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, p->mag ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (GLint)wrap[p->ws & 7]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (GLint)wrap[p->wt & 7]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R_, (GLint)wrap[p->wr & 7]);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD_, p->lodmin);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD_, p->lodmax);
    glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS_, p->bias);
    {   /* ARB_bindless_texture: a sampler used for a texture handle may only have one of four border colours,
         * (0,0,0,0) (0,0,0,1) (1,1,1,0) (1,1,1,1); anything else makes glGetTextureSamplerHandleARB fail with
         * INVALID_OPERATION and the texture is then not bound at all. The nearest allowed colour is used instead. */
        float b[4] = {p->border[0], p->border[1], p->border[2], p->border[3]};
        float mx = b[0] > b[1] ? b[0] : b[1];         /* a depth or one-channel texture only looks at red: (1,0,0,0) must stay 1 */
        if (b[2] > mx) mx = b[2];
        float rgb = mx >= 0.5f ? 1.0f : 0.0f, al = b[3] >= 0.5f ? 1.0f : 0.0f;
        if (b[0] != rgb || b[1] != rgb || b[2] != rgb || b[3] != al) {
            static int said;
            if (said++ < 4) printf("nvn_gl: sampler border colour (%g, %g, %g, %g) is not usable with bindless handles: using (%g, %g, %g, %g)\n", b[0], b[1], b[2], b[3], rgb, rgb, rgb, al);
            b[0] = b[1] = b[2] = rgb; b[3] = al;
        }
        if (p->border_kind && gl_has(glSamplerParameterIiv, "glSamplerParameterIiv")) {       /* integer border colour (SetBorderColori / ui): the same four values only */
            GLint bi[4];
            memcpy(bi, p->border, 16);
            GLint rgbi = bi[0] || bi[1] || bi[2], ali = bi[3] != 0;
            bi[0] = bi[1] = bi[2] = rgbi; bi[3] = ali;
            if (p->border_kind == 1) glSamplerParameterIuiv(s, GL_TEXTURE_BORDER_COLOR, (const GLuint *)bi); else glSamplerParameterIiv(s, GL_TEXTURE_BORDER_COLOR, bi);
        } else glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, b);
    }
    if (p->aniso > 1.0f) glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_, p->aniso);
    if (p->reduction == 1 || p->reduction == 2) glSamplerParameteri(s, 0x9366 /* TEXTURE_REDUCTION_MODE_ARB */, p->reduction == 1 ? 0x8007 : 0x8008);   /* min / max instead of average */
    if (p->cmode == 1) {
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_MODE_, GL_COMPARE_REF_TO_TEXTURE_);
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC_, GL_NEVER + (p->cfunc ? p->cfunc - 1 : 7));
        {   static int ns = -1;                         /* TOMO_NOSHADOW=1 (diagnosis): every depth comparison passes */
            if (ns < 0) ns = getenv("TOMO_NOSHADOW") != NULL;
            if (ns) glSamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC_, 0x0207 /* ALWAYS */);
        }
        {   static int said;
            if (said++ < 8) printf("nvn_gl: comparison sampler: func %d (GL 0x%x)\n", p->cfunc, GL_NEVER + (p->cfunc ? p->cfunc - 1 : 7));
        }
    }
    if (g_sampler_count < 512) { g_samplers[g_sampler_count].p = *p; g_samplers[g_sampler_count++].gl = s; slot[at] = (int16_t)g_sampler_count; }
    return s;
}

/* NVN texture handle (texture ID | sampler ID << 20, bit 32 set: dev_texture_handle, as [sdk 0x41bc80]) -> resident GL bindless handle. */
static int gputime_ms(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOMO_GPUTIME"); v = e ? atoi(e) : 0; if (e && v <= 0) v = 30; } return v; }
static int handle_lookup(TexPool *tp, SamplerPool *sp, uint64_t nh, Tex **t, View *v, SamplerParams *p) {
    uint32_t tid = (uint32_t)nh & 0xfffff, sid = (uint32_t)(nh >> 20) & 0xfff;   /* real layout [sdk 0x41bc80]; DrawTexture splits it the same way [0x4382f0] */
    int have_sampler = 0;
    *t = NULL; memset(v, 0, sizeof *v); memset(p, 0, sizeof *p);
    if (!nh || !tp) return 0;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)tp->count) { *t = tp->e[tid].tex; *v = tp->e[tid].view; }
    if (sp && sid < (uint32_t)sp->count && sp->used[sid]) { *p = sp->e[sid]; have_sampler = 1; }
    ReleaseSRWLockShared(&g_pool_lock);
    return have_sampler;
}
static uint64_t handle_make(Tex *t, View v, SamplerParams p, int have_sampler, uint64_t nh, uint32_t *dims, int *isint) {
    /* A finalized texture keeps being sampled until its GL object goes (deferred, gl_housekeeping): the game finalizes a texture
     * while commands already recorded still name its pool entry, and the GPU reads the old texels from memory that is still
     * there (its texture streaming moves the house's textures every few frames: they turned black or dark red for a frame). */
    if (!t || (t->dead && !t->o.gl)) return g_dummy_handle;
    t->last_bind = g_st.frames;
    tex_sync_a(t, 1);
    TexObj *o = tex_view_obj(t, &v);
    {   /* what TXQ reports, and whether the shader must use an integer sampler */
        Fmt f;
        int l0 = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, nl = (v.set & 0x81) == 0x81 && v.nlvl > 0 ? v.nlvl : t->glevels - l0, is3d = t->b.target == 2;
        int layers = (v.set & 0x82) == 0x82 && v.nlay > 0 ? v.nlay : tex_layers(&t->b);
        dims[0] = (uint32_t)lvl(t->b.width, l0); dims[1] = (uint32_t)lvl(t->b.height, l0);
        dims[2] = is3d ? (uint32_t)lvl(t->b.depth, l0) : (uint32_t)(t->b.target == 9 ? layers / 6 : layers);
        dims[3] = (uint32_t)(nl > 0 ? nl : 1) | ((uint32_t)is3d << 16);
        if (fmt_get((v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, &f) && f.kind == 0 && (f.format == 0x8D94 || f.format == 0x8228 || f.format == 0x8D98 || f.format == 0x8D99))
            *isint = f.type == 0x1400 || f.type == 0x1402 || f.type == 0x1404 ? 2 : 1;
    }
    {   /* Maxwell compares depth only when the shader's instruction asks (TEX.DC); the game shares a comparison sampler between
         * its shadow map and colour textures. In GL a compare mode on a sampler makes a colour texture incomplete: it read
         * (0,0,0,1) (Tomodachi Life's sun mask on the island: no sunlight on the ground). Colour textures get the sampler
         * without the comparison. */
        Fmt cf;
        if (have_sampler && p.cmode && fmt_get((v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, &cf) && cf.kind == 0) p.cmode = 0;
    }
    if (have_sampler) {     /* the border colour's type follows the texture's (a float 1.0 read as an integer is no allowed bindless value: a
                             * handle of an integer texture then failed, 32 GL errors and dummy textures in the user's session, 2026-10-30) */
        Fmt bf;
        int intfmt = fmt_get((v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, &bf) && bf.kind == 0 &&
                     (bf.format == 0x8D94 || bf.format == 0x8228 || bf.format == 0x8D98 || bf.format == 0x8D99);
        if (intfmt && !p.border_kind) {
            int32_t bi[4]; for (int k = 0; k < 4; k++) bi[k] = p.border[k] >= 0.5f;
            memcpy(p.border, bi, 16); p.border_kind = bf.type == 0x1400 || bf.type == 0x1402 || bf.type == 0x1404 ? 2 : 1;
        } else if (!intfmt && p.border_kind) {
            int32_t bi[4]; memcpy(bi, p.border, 16);
            for (int k = 0; k < 4; k++) p.border[k] = bi[k] ? 1.0f : 0.0f;
            p.border_kind = 0;
        }
    }
    GLuint smp = have_sampler ? sampler_gl(&p) : 0;
    for (int i = 0; i < o->nh; i++) if (o->h[i].smp == smp) return o->h[i].h;
    uint64_t h = smp ? glGetTextureSamplerHandleARB(o->gl, smp) : glGetTextureHandleARB(o->gl);
    if (!h) {
        static int nlog;
        logf_limited(&nlog, 8, "nvn_gl: no bindless handle for texture format 0x%x (view 0x%x) with sampler border %g %g %g %g (kind %d): the dummy texture instead\n",
                     t->b.format, (v.set & 0x84) == 0x84 ? v.format : 0, p.border[0], p.border[1], p.border[2], p.border[3], p.border_kind);
        return g_dummy_handle;
    }
    glMakeTextureHandleResidentARB(h);
    o->h = realloc(o->h, sizeof *o->h * (size_t)(o->nh + 1));
    o->h[o->nh].smp = smp; o->h[o->nh++].h = h;
    g_st.handles++; InterlockedIncrement64(&g_handles_live);
    if (gputime_ms() && (t->b.target == 8 || t->b.target == 9 || (v.set & 0xa0) == 0xa0)) {     /* TOMO_GPUTIME: cube handles and target-changing views */
        static int nlog;
        logf_limited(&nlog, 60, "gputime: handle %llx (nvn %llx) = texture target %d format 0x%x %dx%dx%d levels %d flags 0x%x gl %u, view set 0x%x target %d lvl %d+%d lay %d+%d format 0x%x -> gl %u%s, sampler %u\n",
                     (unsigned long long)h, (unsigned long long)nh, t->b.target, t->b.format, t->b.width, t->b.height, t->b.depth, t->b.levels, (unsigned)t->b.flags, t->o.gl,
                     v.set, v.target, v.lvl0, v.nlvl, v.lay0, v.nlay, v.format, o->gl, o->alias ? " (alias)" : "", smp);
    }
    return h;
}
/* A texture bind the batch resolved before (same pools, same ID word, no pool entry registered since: g_pool_gen) takes that
 * answer: the island's big list binds a few hundred textures in some thousand binds, and each resolve read the pool entries,
 * the formats and the sampler table again (9 % of the render thread, 2026-10-31). The first bind in a batch still syncs the
 * texture (tex_sync_a). TOMO_HMEMO=0: every bind resolved. */
static volatile LONG g_pool_gen;                  /* (below: pool entries registered) */
static struct { TexPool *tp; SamplerPool *sp; uint64_t nh, gl; uint32_t dims[4], ser; LONG gen; int isint; } g_hmemo[2048];
static uint64_t resolve_handle(TexPool *tp, SamplerPool *sp, uint64_t nh, uint32_t *dims, int *isint);
static uint64_t resolve_handle_memo(TexPool *tp, SamplerPool *sp, uint64_t nh, uint32_t *dims, int *isint) {
    static int on = -1;
    if (on < 0) on = !(getenv("TOMO_HMEMO") && getenv("TOMO_HMEMO")[0] == '0');
    if (!on) return resolve_handle(tp, sp, nh, dims, isint);
    unsigned i = (unsigned)(((nh ^ ((uint64_t)(uintptr_t)tp << 7) ^ ((uint64_t)(uintptr_t)sp << 13)) * 0x9E3779B97F4A7C15ULL) >> 53);
    LONG gen = g_pool_gen;
    if (g_hmemo[i].ser == g_serial && g_hmemo[i].gen == gen && g_hmemo[i].nh == nh && g_hmemo[i].tp == tp && g_hmemo[i].sp == sp) {
        memcpy(dims, g_hmemo[i].dims, 16); *isint = g_hmemo[i].isint;
        return g_hmemo[i].gl;
    }
    uint64_t gl = resolve_handle(tp, sp, nh, dims, isint);
    g_hmemo[i].tp = tp; g_hmemo[i].sp = sp; g_hmemo[i].nh = nh; g_hmemo[i].gl = gl; memcpy(g_hmemo[i].dims, dims, 16);
    g_hmemo[i].isint = *isint; g_hmemo[i].ser = g_serial; g_hmemo[i].gen = gen;
    return gl;
}
static uint64_t resolve_handle(TexPool *tp, SamplerPool *sp, uint64_t nh, uint32_t *dims, int *isint) {
    Tex *t;
    View v;
    SamplerParams p;
    int have_sampler = handle_lookup(tp, sp, nh, &t, &v, &p);
    return handle_make(t, v, p, have_sampler, nh, dims, isint);
}
/* The same lookup for nvnCommandBufferDrawTexture, which needs the OpenGL names: 0 = no live texture under that ID (resolve_handle
 * gives the 1x1 dummy there; a DrawTexture of it is reported instead). */
static int resolve_names(TexPool *tp, SamplerPool *sp, uint64_t nh, GLuint *tex, GLuint *smp) {
    uint32_t tid = (uint32_t)nh & 0xfffff, sid = (uint32_t)(nh >> 20) & 0xfff;   /* real layout [sdk 0x41bc80]; DrawTexture splits it the same way [0x4382f0] */
    Tex *t = NULL;
    View v;
    SamplerParams p;
    int have_sampler = 0;
    if (!tp) return 0;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)tp->count) { t = tp->e[tid].tex; v = tp->e[tid].view; }
    if (sp && sid < (uint32_t)sp->count && sp->used[sid]) { p = sp->e[sid]; have_sampler = 1; }
    ReleaseSRWLockShared(&g_pool_lock);
    if (!t || t->dead) return 0;
    tex_sync_a(t, 1);
    *tex = tex_view_obj(t, &v)->gl;
    *smp = have_sampler ? sampler_gl(&p) : 0;
    return 1;
}

/* ---------------------------------------------------------------- framebuffers */
static void bl_bind(void);                            /* below: the bindless handle table of TEX.B */
typedef struct { GLuint color[8], depth; int n, dkind; int layer[8], level[8], dlayer, dlevel; } FboKey;   /* layer -1: the whole texture (or a view) */
#define NFBO 1024
static struct { FboKey k; GLuint fbo; int w, h, used; } g_fbos[NFBO];
static int g_fbo_next;
static void fbo_drop_bound(GLuint fbo);
/* Every attachment of a framebuffer off (through the read binding: the draw binding stays). A framebuffer that still names a
 * deleted texture is undefined, and the driver crashed in glDeleteTextures over such records: nothing is ever deleted while
 * attached, and the cache keeps its framebuffer names for reuse instead of deleting them. */
static void fbo_detach(GLuint fbo) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, fbo);
    for (int i = 0; i < 8; i++) glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_ + (GLenum)i, 0, 0);
    glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_, 0, 0);
    glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_DEPTH_ATTACHMENT_, 0, 0);
    glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_STENCIL_ATTACHMENT_, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
}
static void fbo_purge(GLuint tex) {
    for (int i = 0; i < NFBO; i++) {
        if (!g_fbos[i].used) continue;
        int hit = g_fbos[i].k.depth == tex;
        for (int j = 0; j < 8; j++) hit |= g_fbos[i].k.color[j] == tex;
        if (hit) { fbo_drop_bound(g_fbos[i].fbo); fbo_detach(g_fbos[i].fbo); g_fbos[i].used = 0; }
    }
}
static int g_fbo_bad_last;                         /* set by fbo_get for an incomplete framebuffer: set_targets then names the textures */
static LONG64 g_fbo_evicted, g_fbo_t_detach, g_fbo_t_make;      /* TOMO_RTSTAT */
static GLuint fbo_get(const FboKey *k) {
    static int nlog;
    /* by a hash of the key first (the slot it was found in last time): each target switch compared the key with up to 1,024
     * cached framebuffers (8 us a switch, 200 switches a frame on the island) */
    static int16_t front[512];
    uint64_t hh = 0xcbf29ce484222325ULL;
    for (size_t b = 0; b < sizeof *k; b++) hh = (hh ^ ((const uint8_t *)k)[b]) * 0x100000001b3ULL;
    int fi = (int)(hh >> 55), fs = front[fi] - 1;
    if (fs >= 0 && g_fbos[fs].used && !memcmp(&g_fbos[fs].k, k, sizeof *k)) return g_fbos[fs].fbo;
    for (int i = 0; i < NFBO; i++) if (g_fbos[i].used && !memcmp(&g_fbos[i].k, k, sizeof *k)) { front[fi] = (int16_t)(i + 1); return g_fbos[i].fbo; }
    LONG64 fq0 = pf_now();
    int slot = -1;
    for (int i = 0; i < NFBO && slot < 0; i++) if (!g_fbos[i].used) slot = i;
    if (slot < 0) { slot = g_fbo_next++ & (NFBO - 1); fbo_drop_bound(g_fbos[slot].fbo); fbo_detach(g_fbos[slot].fbo); g_fbo_evicted++; }
    g_fbo_t_detach += pf_now() - fq0;
    GLuint fbo = g_fbos[slot].fbo;                 /* a name kept from a purged or evicted entry, detached */
    GLenum bufs[8];
    if (!fbo) glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER_, fbo);
    for (int i = 0; i < 8; i++) {
        bufs[i] = i < k->n && k->color[i] ? GL_COLOR_ATTACHMENT0_ + (GLenum)i : GL_NONE;
        if (i < k->n && k->color[i]) {
            if (k->layer[i] >= 0) glFramebufferTextureLayer(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_ + (GLenum)i, k->color[i], k->level[i], k->layer[i]);
            else glFramebufferTexture(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_ + (GLenum)i, k->color[i], 0);
        }
    }
    if (k->depth) {
        GLenum att = k->dkind == 3 ? GL_DEPTH_STENCIL_ATTACHMENT_ : k->dkind == 2 ? GL_STENCIL_ATTACHMENT_ : GL_DEPTH_ATTACHMENT_;
        if (k->dlayer >= 0) glFramebufferTextureLayer(GL_FRAMEBUFFER_, att, k->depth, k->dlevel, k->dlayer);
        else glFramebufferTexture(GL_FRAMEBUFFER_, att, k->depth, 0);
    }
    glDrawBuffers(8, bufs);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER_);
    g_st.fbo_made++;
    if (st != GL_FRAMEBUFFER_COMPLETE_) {
        g_st.fbo_bad++; g_fbo_bad_last = 1;
        logf_limited(&nlog, 20, "nvn_gl: framebuffer incomplete (status 0x%x): %d colour targets (first gl %u), depth gl %u kind %d\n", st, k->n, k->color[0], k->depth, k->dkind);
    }
    g_fbos[slot].k = *k; g_fbos[slot].fbo = fbo; g_fbos[slot].used = 1; front[fi] = (int16_t)(slot + 1);
    g_fbo_t_make += pf_now() - fq0;
    return fbo;
}

/* ---------------------------------------------------------------- streaming buffer: guest memory -> GL */
#define ARENA_SIZE ((size_t)96 << 20)
static GLuint g_arena;
static size_t g_arena_pos;
static struct { uint64_t addr, size; size_t off; uint32_t gen, arena; uint64_t stamp; } g_acache[8192];
static uint32_t g_arena_serial = 1;      /* each new store: a deleted store's GL name comes back for a later one */
/* Small ranges (<= 16 KB: uniform blocks, texture handle tables, small vertex and index data) go into a persistently
 * mapped ring of four 16 MB parts, written with memcpy. Each was a glBufferSubData before: 2,600 a frame on the island,
 * each copied again by the driver's own thread, which was what limited the frame (half the resolution: 21.5 -> 20.4 ms).
 * A part is written again once the fence set when it was left has passed. Larger ranges (meshes) stay in the 96 MB store
 * in video memory. TOMO_RING=0: everything in the store, as before. */
#define RING_PARTS 4
#define RING_PART ((size_t)16 << 20)
#define RING_MAX ((size_t)16 << 10)
#define RING_GUARD ((size_t)1 << 20)
static GLuint g_ring;
static uint8_t *g_ring_map;
static size_t g_ring_pos;
static void *g_ring_fence[RING_PARTS];
static uint32_t g_ring_gen[RING_PARTS];
static int g_ring_on = -1;
static LONG64 g_ring_calls, g_ring_bytes, g_ring_waits;
static GLuint g_arena_buf;                 /* the buffer of the range arena_put / arena_put_raw returned last */
static uint32_t g_arena_tag;               /* and what it is in: a store serial, or (high bit) a ring part and its generation */
static int arena_tag_ok(uint32_t tag) { return tag & 0x80000000u ? (tag & 0xffffffu) == (g_ring_gen[(tag >> 24) & 0x7f] & 0xffffffu) : tag == g_arena_serial; }
static GLuint arena_tag_buf(uint32_t tag) { return tag & 0x80000000u ? g_ring : g_arena; }
static uint32_t g_arena_gen = 1;      /* (declared above ww_note) */
/* Redundant state calls skipped: on the island (2760 draws a frame) most of the 80 000 GL calls a frame repeated the draw
 * before. These remember what the render context has bound: every storage buffer range goes through gs_ssbo, and the code
 * that binds pipeline 0 or deletes buffers calls gs_invalidate. TOMO_GSCACHE=0: every call made, as before. */
static struct { GLuint buf; ptrdiff_t off, size; } g_gs_ssbo[96], g_gs_ubo[40];
static struct { GLuint buf; ptrdiff_t off; GLsizei stride; GLuint div; } g_gs_vb[16];
static uint32_t g_gs_attr[16];
static GLuint g_gs_vs, g_gs_fs;
static int g_gs_pipe, g_gs_arb_on = 1, g_gs_alpha_on = 1, g_gs_on = -1;
static void gs_invalidate(void) {
    memset(g_gs_ssbo, 0, sizeof g_gs_ssbo); memset(g_gs_ubo, 0, sizeof g_gs_ubo); memset(g_gs_vb, 0, sizeof g_gs_vb); memset(g_gs_attr, 0, sizeof g_gs_attr);
    g_gs_pipe = 0; g_gs_vs = g_gs_fs = 0; g_gs_arb_on = g_gs_alpha_on = 1;
}
static void gs_mode(void) { if (g_gs_on < 0) g_gs_on = getenv("TOMO_GSCACHE") ? atoi(getenv("TOMO_GSCACHE")) : 15; }      /* bits: 1 storage ranges, 2 programs, 4 attributes, 8 vertex buffers */
static void gs_ssbo(GLuint i, GLuint buf, ptrdiff_t off, ptrdiff_t size) {
    gs_mode();
    if ((g_gs_on & 1) && i < 96 && g_gs_ssbo[i].buf == buf && g_gs_ssbo[i].off == off && g_gs_ssbo[i].size == size) return;
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, i, buf, off, size); g_gs_gl_binds++;
    if (i < 96) { g_gs_ssbo[i].buf = buf; g_gs_ssbo[i].off = off; g_gs_ssbo[i].size = size; }
}
static void gs_ubo(GLuint i, GLuint buf, ptrdiff_t off, ptrdiff_t size) {
    if (size > 65536) size = 65536;                  /* GL_MAX_UNIFORM_BLOCK_SIZE; the hardware's constant buffers are no larger */
    if (g_gs_on && i < 40 && g_gs_ubo[i].buf == buf && g_gs_ubo[i].off == off && g_gs_ubo[i].size == size) return;
    glBindBufferRange(GL_UNIFORM_BUFFER_, i, buf, off, size); g_gs_gl_binds++;
    if (i < 40) { g_gs_ubo[i].buf = buf; g_gs_ubo[i].off = off; g_gs_ubo[i].size = size; }
}
/* TOMO_UBO=0: the graphics stages read their constant buffers from storage buffers, as before (mx_translate.h MxIn.ubo) */
static int ubo_on(void) { static int on = -1; if (on < 0) on = !(getenv("TOMO_UBO") && getenv("TOMO_UBO")[0] == '0'); return on; }
typedef struct ArenaOld { GLuint buf; struct ArenaOld *next; } ArenaOld;
static ArenaOld *g_arena_old;
static void arena_retire(void) {
    while (g_arena_old) {
        ArenaOld *old = g_arena_old; g_arena_old = old->next;
        glDeleteBuffers(1, &old->buf); free(old);
        gs_invalidate();                             /* (a new buffer may get the old name) */
    }
}

static size_t ring_put(const void *p, size_t n) {     /* offset in g_ring, or (size_t)-1 */
    if (!g_ring) {
        if (!gl_has(glNamedBufferStorage, "glNamedBufferStorage") || !gl_has(glMapNamedBufferRange, "glMapNamedBufferRange")) { g_ring_on = 0; return (size_t)-1; }
        glCreateBuffers(1, &g_ring);
        /* + 1 MB that is never written: a shader reads a uniform block past the range bound for it, and a draw fetches vertices
         * past its buffer's size (the console returns zeros there); at the ring's end such a read left the mapping and the GPU
         * faulted (the NVIDIA driver then closes the game: "A TDR has been detected", 7 times in 25 runs before this) */
        glNamedBufferStorage(g_ring, (ptrdiff_t)(RING_PARTS * RING_PART + RING_GUARD), NULL, 0x2 | 0x40 | 0x80);      /* MAP_WRITE | PERSISTENT | COHERENT */
        g_ring_map = glMapNamedBufferRange(g_ring, 0, (ptrdiff_t)(RING_PARTS * RING_PART + RING_GUARD), 0x2 | 0x40 | 0x80);
        if (g_ring_map) memset(g_ring_map + RING_PARTS * RING_PART, 0, RING_GUARD);
        if (!g_ring_map) { glDeleteBuffers(1, &g_ring); g_ring = 0; g_ring_on = 0; gs_invalidate(); return (size_t)-1; }
        g_ring_gen[0] = 1;
    }
    size_t pos = (g_ring_pos + 255) & ~(size_t)255;
    static int part;                                     /* (from the position, a write that ended at the ring's end gave part 4) */
    if (pos + n > (size_t)(part + 1) * RING_PART) {      /* into the next part, once the GPU is done with it */
        /* The part left is not handed out again (its kept uploads, handle tables and copies stop being valid here): before,
         * they stayed valid while the next parts filled, draws issued later still read them, and when the ring came round
         * the part was written over under draws its fence did not cover: corrupted index and uniform data, the GPU faulted
         * and the NVIDIA driver closed the game ("A TDR has been detected"; 2 of 2 runs with the driver's own GL thread).
         * A draw can still bind ranges from both sides of the switch: the part is written again after the fence of the
         * part that followed it, which came after that draw. */
        g_ring_gen[part]++;
        g_ring_fence[part] = glFenceSync(0x9117 /* SYNC_GPU_COMMANDS_COMPLETE */, 0);
        part = (part + 1) % RING_PARTS; pos = (size_t)part * RING_PART;
        int after = (part + 1) % RING_PARTS;
        if (g_ring_fence[after]) {
            if (glClientWaitSync(g_ring_fence[after], 0, 0) == 0x911B /* TIMEOUT_EXPIRED */) { g_ring_waits++; HS_IN("buffer streaming: ring part still in use (wait)", 0); glClientWaitSync(g_ring_fence[after], 1, 2000000000ull); HS_OUT(); }
            glDeleteSync(g_ring_fence[after]); g_ring_fence[after] = NULL;
        }
    }
    memcpy(g_ring_map + pos, p, n);
    g_ring_pos = pos + n;
    g_ring_calls++; g_ring_bytes += (LONG64)n;
    g_arena_buf = g_ring; g_arena_tag = 0x80000000u | ((uint32_t)part << 24) | (g_ring_gen[part] & 0xffffffu);
    return pos;
}
static size_t arena_put_raw(const void *p, size_t n) {
    if (g_ring_on < 0) g_ring_on = !(getenv("TOMO_RING") && getenv("TOMO_RING")[0] == '0');
    if (g_ring_on && n <= RING_MAX) { size_t off = ring_put(p, n); if (off != (size_t)-1) return off; }
    g_arena_pos = (g_arena_pos + 255) & ~(size_t)255;
    if (g_arena_pos + n > ARENA_SIZE) {
        /* The next of three stores, once the GPU is done with what it held (its fence): a new 96 MB buffer at every rollover
         * (every ~13 frames on the island, 7.5 MB a frame of large ranges) cost the driver an allocation and the old one's
         * release each time. (Earlier bindings of this draw still read the store being left: it is not written again before
         * its fence has passed.) */
        static GLuint store[3]; static void *fence[3]; static int cur;
        if (!store[cur]) store[cur] = g_arena;
        fence[cur] = glFenceSync(0x9117 /* SYNC_GPU_COMMANDS_COMPLETE */, 0);
        cur = (cur + 1) % 3;
        int after = (cur + 1) % 3;                      /* (a draw binding ranges from both sides of the rollover: as for the ring) */
        if (fence[after]) {
            if (glClientWaitSync(fence[after], 0, 0) == 0x911B /* TIMEOUT_EXPIRED */) { HS_IN("buffer streaming: store still in use (wait)", 0); glClientWaitSync(fence[after], 1, 2000000000ull); HS_OUT(); }
            glDeleteSync(fence[after]); fence[after] = NULL;
        }
        if (!store[cur]) {
            HS_IN("buffer streaming: new 96 MB store (glBufferData)", 0);
            glGenBuffers(1, &store[cur]); glBindBuffer(GL_ARRAY_BUFFER_, store[cur]);
            glBufferData(GL_ARRAY_BUFFER_, (ptrdiff_t)ARENA_SIZE, NULL, GL_STREAM_DRAW_);
            HS_OUT();
        }
        g_arena = store[cur]; glBindBuffer(GL_ARRAY_BUFFER_, g_arena);
        g_arena_pos = 0; g_arena_gen++; g_arena_serial++;
    }
    size_t off = g_arena_pos;
    LONG64 pf0 = pf_now();
    HS_IN("buffer streaming (glBufferSubData)", 0);
    glBufferSubData(GL_ARRAY_BUFFER_, (ptrdiff_t)off, (ptrdiff_t)n, p);
    HS_OUT();
    pf_add(PF_ARENA, pf0); g_pf_arena_bytes += (LONG64)n;
    g_arena_pos += n;
    g_st.arena_bytes += (LONG64)n;
    g_arena_buf = g_arena; g_arena_tag = g_arena_serial;
    return off;
}
static void arena_selftest(void) {
    if (!getenv("SSBU_ARENA_SELFTEST")) return;
    const uint32_t first = 0x12345678, second = 0xabcdef01;
    uint32_t got = 0;
    g_ring_on = 0;                                /* (the store's rollover is tested) */
    size_t off = arena_put_raw(&first, sizeof first);
    GLuint before = g_arena;
    gs_ssbo(0, before, (ptrdiff_t)off, sizeof first);
    g_arena_pos = ARENA_SIZE;                     /* wrap after a draw has already bound one of its inputs */
    arena_put_raw(&second, sizeof second);
    glGetNamedBufferSubData(before, (ptrdiff_t)off, sizeof got, &got);
    int bad = before == g_arena || got != first || glGetError() != GL_NO_ERROR;
    printf("arena self-test: rollover keeps bound input %08x (expected %08x): %s\n", got, first, bad ? "FAIL" : "PASS");
    arena_retire();
    fflush(stdout);
    ExitProcess(bad ? 1 : 0);
}
/* Read-backs in flight: at the end of a batch the buffers the GPU wrote (dispatches, fragment stages) are copied into mapped
 * staging memory behind a fence instead of read with a stall (the CPU waited for the whole batch: 6 ms a frame on the island).
 * They land in guest memory when the game's own wait on a later fence succeeds (job_sync_wait, Finish), at a present when
 * done, and before anything on this thread reads or writes guest memory over them (rb_land_range). Render thread only. */
typedef struct { uint64_t addr, size; int slot; void *fence; uint8_t *snap; } RbPend;
/* GPU results go back to guest memory only where the GPU changed them: the words that differ from what the buffer held when it
 * went to the GPU (snap). The game's CPU writes its inputs (camera near, far, aspect...) into the same buffers the compute
 * programs write their results into; copied back whole, a late copy put last frame's inputs back over the new ones and the
 * island's shadows jumped for a frame. */
static void gpu_merge(uint8_t *dst, const uint8_t *gpu, const uint8_t *snap, size_t n) {
    size_t i = 0;
    if (!snap) { memcpy(dst, gpu, n); return; }
    {   static int dbg = -1, nlog;                 /* TOMO_MERGELOG=1: words the CPU changed while the buffer was on the GPU */
        if (dbg < 0) dbg = getenv("TOMO_MERGELOG") != NULL;
        if (dbg && nlog < 400) {
            int cpu = 0, both = 0, first = -1;
            for (size_t k = 0; k + 4 <= n; k += 4) {
                uint32_t d, g, o; memcpy(&d, dst + k, 4); memcpy(&g, gpu + k, 4); memcpy(&o, snap + k, 4);
                if (d != o) { cpu++; if (first < 0) first = (int)k; if (g != o) both++; }
            }
            if (cpu) { nlog++; printf("mergelog: frame %lld buffer %p+%zu: %d words changed by the CPU (%d also by the GPU), first at +0x%x: cpu %08x snap %08x gpu %08x\n", (long long)g_st.frames, (void *)dst, n, cpu, both, first,
                                    *(const uint32_t *)(dst + first), *(const uint32_t *)(snap + first), *(const uint32_t *)(gpu + first)); }
        }
    }
    for (; i + 4 <= n; i += 4) { uint32_t g, o; memcpy(&g, gpu + i, 4); memcpy(&o, snap + i, 4); if (g != o) memcpy(dst + i, &g, 4); }
    for (; i < n; i++) if (gpu[i] != snap[i]) dst[i] = gpu[i];
}
static RbPend g_rbp[64];
static struct { GLuint buf; void *map; uint64_t cap; int busy; } g_rbs[64];
static int rb_stage(uint64_t size) {               /* a free staging buffer of at least size bytes, or -1 */
    int k, best = -1;
    for (k = 0; k < 64; k++) if (!g_rbs[k].busy && g_rbs[k].buf && g_rbs[k].cap >= size && (best < 0 || g_rbs[k].cap < g_rbs[best].cap)) best = k;
    if (best >= 0) return best;
    for (k = 0; k < 64 && g_rbs[k].buf; k++) {}
    if (k == 64 || !gl_has(glNamedBufferStorage, "glNamedBufferStorage") || !gl_has(glMapNamedBufferRange, "glMapNamedBufferRange")) return -1;
    uint64_t cap = 4096; while (cap < size) cap <<= 1;
    HS_IN("compute results: staging buffer made", 0);
    glCreateBuffers(1, &g_rbs[k].buf);
    glNamedBufferStorage(g_rbs[k].buf, (ptrdiff_t)cap, NULL, 0x1 | 0x40 | 0x80 | 0x200);      /* MAP_READ | PERSISTENT | COHERENT | CLIENT_STORAGE */
    g_rbs[k].map = glMapNamedBufferRange(g_rbs[k].buf, 0, (ptrdiff_t)cap, 0x1 | 0x40 | 0x80);
    HS_OUT();
    if (!g_rbs[k].map) { glDeleteBuffers(1, &g_rbs[k].buf); g_rbs[k].buf = 0; gs_invalidate(); return -1; }
    g_rbs[k].cap = cap;
    return k;
}
/* TEST ONLY: the render thread's waits for the GPU, by source line (the stall line with "one frame in flight") */
static struct { int line; LONG64 ticks, n; } g_stall[32];
static void stall_add(int line, LONG64 t0) {
    int k = 0;
    while (k < 31 && g_stall[k].line && g_stall[k].line != line) k++;
    g_stall[k].line = line; g_stall[k].ticks += pf_now() - t0; g_stall[k].n++;
}
#define rb_land(w) rb_land_at(w, __LINE__)
static void rb_land_at(int wait, int line) {        /* land the read-backs the GPU has finished (all of them, waiting, when wait) */
    int i, landed = 0;
    for (i = 0; i < g_rbp_n; i++) {
        RbPend *q = &g_rbp[i];
        LONG64 w0 = pf_now();
        GLenum r = glClientWaitSync(q->fence, wait ? 1 : 0, wait ? 0xFFFFFFFFFFFFFFFFull : 0);
        if (wait && r != 0x911A) stall_add(line, w0);
        if (r == 0x911B /* TIMEOUT_EXPIRED */) break;        /* the later ones are not done either (the GPU finishes in order) */
        gpu_merge(P(q->addr), g_rbs[q->slot].map, q->snap, (size_t)q->size);
        ww_note(q->addr, q->size);
        free(q->snap); q->snap = NULL;
        g_rbs[q->slot].busy = 0;
        if (i + 1 == g_rbp_n || g_rbp[i + 1].fence != q->fence) glDeleteSync(q->fence);
        landed++;
    }
    if (!landed) return;
    memmove(g_rbp, g_rbp + landed, sizeof g_rbp[0] * (size_t)(g_rbp_n - landed));
    g_rbp_n -= landed;
    g_arena_gen++;                                 /* guest memory changed: later reads stream it again */
}
static void rb_land_range(uint64_t addr, uint64_t size) {
    for (int i = 0; i < g_rbp_n; i++) if (addr < g_rbp[i].addr + g_rbp[i].size && g_rbp[i].addr < addr + size) { rb_land(1); return; }
}
/* ---- what a batch binds, as it was when the game submitted it (SnapSet)
 * The game rewrites uniform buffers a few ms after it has submitted and flushed a batch that reads them (the console's GPU
 * has read them by then), for its next batch or frame: on the island about ten ranges a frame, the 2D layer's element data
 * (positions jump by hundreds of pixels: another element's), at the title an animated model's. The render thread replays a
 * batch later and read the new contents, so an element drew for one frame with another one's data, at random moments,
 * anywhere (the user's "random flicker", 2026-10-31). At the submission the game's thread copies the small uniform and
 * vertex ranges the batch binds that lie on pages the game wrote in the last 5 s; the replay takes the copy where guest
 * memory no longer matches it, with the bytes our own writes (read-backs, buffer copies, uniform updates: ww_note) put
 * there since the submission, which the batch must see. (Taking guest memory whenever one of ours touched the pages, as
 * first written, left ranges such as a 64 KB uniform block that read-backs land in racing: in the wishing fountain scene
 * the far background still took a glow pulse's pink tint for single frames.) Measured: the game never changes a copied
 * range between its submission and its next flush, sync wait or present (TOMO_SNAP_FLUSHCHECK=1), so the copy holds what
 * the console's GPU reads. Cost: 0.07-0.6 ms a batch on the game's thread (100-1100 KB). TOMO_SNAP=0: off, as before. */
#define SNAP_MAX_UBO 65536
#define SNAP_MAX_VBUF 32768
typedef struct { uint64_t addr, size; size_t off, aoff; LONG64 emu_at; uint32_t aserial; int state; } SnapEnt;      /* state: 0 not looked at in the replay, 1 guest memory, 2 the copy (uploaded at aoff) */
typedef struct SnapSet { SnapEnt *e; unsigned cap, n; int bad; uint8_t *data; size_t len, dcap; LONG64 emu; struct SnapSet *next; volatile LONG ready; } SnapSet;
static SnapSet *g_snap_cur;                              /* the batch being replayed (render thread) */
static SnapSet *g_snap_pool;                             /* sets to use again (g_snap_lock) */
static SRWLOCK g_snap_lock = SRWLOCK_INIT;
static volatile LONG64 g_snap_used, g_snap_bytes, g_snap_ranges, g_snap_sets, g_snap_ticks;
static int snap_on(void) { static int on = -1; if (on < 0) on = getenv("TOMO_SNAP") ? atoi(getenv("TOMO_SNAP")) : 1; return on; }      /* TEST: 2 = copies made and compared, guest memory used (as before) */
static int emu_wrote_since(uint64_t addr, uint64_t size, LONG64 since) {
    for (uint64_t pg = addr >> 12, last = (addr + size - 1) >> 12; pg <= last; pg++) if (g_emu_page[(pg * 0x9E3779B97F4A7C15ULL) >> 48] > since) return 1;
    return 0;
}
static SnapEnt *snap_slot(SnapSet *s, uint64_t addr, uint64_t size) {
    for (unsigned h = (unsigned)((((addr >> 4) ^ (size << 40)) * 0x9E3779B97F4A7C15ULL) >> 40) & (s->cap - 1);; h = (h + 1) & (s->cap - 1))
        if (!s->e[h].addr || (s->e[h].addr == addr && s->e[h].size == size)) return &s->e[h];
}
static void snaplog_where(char *buf, size_t n);
/* The copy takes the bytes our writes (counts after `from`) put into [addr, addr+size): 0 when the ring no longer holds them */
static int snap_merge(uint8_t *copy, uint64_t addr, uint64_t size, LONG64 from) {
    LONG64 now = g_emu_serial;
    if (now - from >= EMU_RING) return 0;
    for (LONG64 v = from + 1; v <= now; v++) {
        uint64_t a = g_emu_ring[v % EMU_RING].addr, z = g_emu_ring[v % EMU_RING].size;
        uint64_t lo = a > addr ? a : addr, hi = a + z < addr + size ? a + z : addr + size;
        if (lo < hi) memcpy(copy + (lo - addr), (const uint8_t *)P(lo), (size_t)(hi - lo));
    }
    return 1;
}
/* The batch's entry for [addr, addr+size) when it must read the copy (render thread), else NULL: guest memory is right.
 * The copy is what the range held at the submission plus what our writes (read-backs of what the GPU wrote, buffer copies,
 * uniform updates) put there since: those come before this batch on the console's GPU, the game's later writes after it. */
static SnapEnt *snap_pick(uint64_t addr, uint64_t size) {
    SnapSet *s = g_snap_cur;
    if (!s || !s->cap) return NULL;
    SnapEnt *e = snap_slot(s, addr, size);
    if (!e->addr) return NULL;
    if (!e->state || (e->state == 2 && emu_wrote_since(addr, size, e->emu_at))) {
        LONG64 from = e->state ? e->emu_at : s->emu, now = g_emu_serial;
        uint8_t *copy = s->data + e->off;
        int was = e->state, ours = emu_wrote_since(addr, size, from);
        if (ours && !snap_merge(copy, addr, size, from)) e->state = 1;      /* (too many writes of ours since: guest memory, as before) */
        else e->state = memcmp(copy, P(addr), (size_t)size) ? 2 : 1;
        e->emu_at = now;
        e->aserial = 0;
        if (e->state == 2 && !was) {
            InterlockedIncrement64(&g_snap_used);
            static int sl = -2, nlog;      /* TEST: TOMO_SNAPLOG=<from frame>: where the copies are used */
            if (sl == -2) sl = getenv("TOMO_SNAPLOG") ? atoi(getenv("TOMO_SNAPLOG")) : -1;
            if (sl >= 0 && g_st.frames >= sl && nlog++ < 4000) {
                char wh[96]; int k = -1;
                for (uint64_t o = 0; o + 4 <= size; o += 4) if (memcmp(copy + o, (const uint8_t *)P(addr) + o, 4)) { k = (int)o; break; }
                snaplog_where(wh, sizeof wh);
                printf("snaplog: frame %lld %s range %llx+%llu: copy used%s, first change +0x%x %08x -> %08x%c", (long long)g_st.frames, wh, (unsigned long long)addr, (unsigned long long)size, ours ? " (with writes of ours)" : "", k,
                       k >= 0 ? *(const uint32_t *)(copy + k) : 0, k >= 0 ? *(const uint32_t *)((const uint8_t *)P(addr) + k) : 0, 10);
            }
        }
    }
    return e->state == 2 && snap_on() == 1 ? e : NULL;
}
/* Offset in the streaming buffer of a copy of guest memory [addr, addr+size), made at most once per batch. */
static size_t arena_put_slow(uint64_t addr, uint64_t size);
/* The same range again with nothing in between that can change it (g_arena_gen moves at every batch, read-back, copy or
 * uniform update of ours, and the range's store or ring part is still the same): the last answer. Most draws bind the
 * ranges the draw before bound (17,000 binds a frame on the island, 60 % repeats); each took the kept-upload table, the
 * write stamps of its pages and the submission copies. TOMO_AFRONT=0: off. */
static struct { uint64_t addr, size; size_t off; uint32_t gen, tag; } g_afront[1024];
static size_t arena_put(uint64_t addr, uint64_t size) {
    static int on = -1; if (on < 0) on = !(getenv("TOMO_AFRONT") && getenv("TOMO_AFRONT")[0] == '0');
    unsigned fi = (unsigned)((((addr >> 4) ^ (size << 32)) * 0x9E3779B97F4A7C15ULL) >> 54);
    if (on && g_afront[fi].addr == addr && g_afront[fi].size == size && g_afront[fi].gen == g_arena_gen && arena_tag_ok(g_afront[fi].tag)) {
        g_arena_tag = g_afront[fi].tag; g_arena_buf = arena_tag_buf(g_arena_tag);
        return g_afront[fi].off;
    }
    size_t off = arena_put_slow(addr, size);
    g_afront[fi].addr = addr; g_afront[fi].size = size; g_afront[fi].off = off; g_afront[fi].gen = g_arena_gen; g_afront[fi].tag = g_arena_tag;
    return off;
}
static size_t arena_put_slow(uint64_t addr, uint64_t size) {
    static int nlog;
    if (g_rbp_n) rb_land_range(addr, size);
    if (size > ARENA_SIZE / 4) { logf_limited(&nlog, 10, "nvn_gl: buffer range of %llu bytes clamped to %llu\n", (unsigned long long)size, (unsigned long long)(ARENA_SIZE / 4)); size = ARENA_SIZE / 4; }
    {   SnapEnt *se = g_snap_cur ? snap_pick(addr, size) : NULL;      /* the game rewrote it after the submission: what it held then */
        if (se) {
            if (!arena_tag_ok(se->aserial)) { se->aoff = arena_put_raw(g_snap_cur->data + se->off, (size_t)size); se->aserial = g_arena_tag; }
            else { g_arena_buf = arena_tag_buf(se->aserial); g_arena_tag = se->aserial; }
            return se->aoff;
        }
    }
    static int keep = -1; if (keep < 0) keep = !(getenv("TOMO_ARENA_KEEP") && getenv("TOMO_ARENA_KEEP")[0] == '0');
    unsigned h = (unsigned)(((addr >> 4) ^ (size * 0x9E3779B1u)) * 2654435761u) >> 19, pick = 8192;
    uint64_t now = keep ? ww_range_stamp(addr, size, 0) : 0;
    if (now) ww_chunk_mark(addr, size);             /* its pages are asked for at every batch from now on (ww_scan_regions) */
    for (int probe = 0; probe < 8; probe++) {
        unsigned i = (h + (unsigned)probe) & 8191;
        if (g_acache[i].addr == addr && g_acache[i].size == size && arena_tag_ok(g_acache[i].arena)) {
            if (g_acache[i].gen == g_arena_gen || (now && now <= g_acache[i].stamp)) { g_arena_tag = g_acache[i].arena; g_arena_buf = arena_tag_buf(g_arena_tag); return g_acache[i].off; }
            pick = i; break;
        }
        if (pick == 8192 && (!arena_tag_ok(g_acache[i].arena) || (g_acache[i].gen != g_arena_gen && !g_acache[i].stamp))) pick = i;
    }
    if (pick == 8192) pick = h & 8191;
    uint64_t stamp = g_ww_stamp;                    /* a later write gets a larger stamp */
    size_t off = arena_put_raw(P(addr), (size_t)size);      /* may start a new store */
    g_acache[pick].addr = addr; g_acache[pick].size = size; g_acache[pick].off = off; g_acache[pick].gen = g_arena_gen;
    g_acache[pick].arena = g_arena_tag; g_acache[pick].stamp = now ? stamp : 0;
    return off;
}

/* ---------------------------------------------------------------- programs */
static const GLenum k_prog_target[6] = {GL_VERTEX_PROGRAM_ARB_, GL_FRAGMENT_PROGRAM_ARB_, GL_GEOMETRY_PROGRAM_NV_, GL_TESS_CONTROL_PROGRAM_NV_, GL_TESS_EVALUATION_PROGRAM_NV_, GL_COMPUTE_PROGRAM_NV_};
static const GLenum k_pbuf_target[6] = {0x8DA2, 0x8DA4, 0x8DA3, 0x8C74, 0x8C75, 0x90FC};
static const char *const k_stage_name[6] = {"vertex", "fragment", "geometry", "tess-control", "tess-eval", "compute"};
#define TEXTURE_HANDLE_BUFFER 14           /* program.buffer[14] of a stage holds the 64-bit handles set with BindTexture, 8 bytes per unit */
static int g_max_pbuf;

static int stage_of(const char *text, int len) {
    static const char *const tag[6] = {"!!NVvp", "!!NVfp", "!!NVgp", "!!NVtcp", "!!NVtep", "!!NVcp"};
    for (int i = 0; i < 6; i++) if (len > 8 && !memcmp(text, tag[i], strlen(tag[i]))) return i;
    return -1;
}
/* Keeps a copy of the program text with program.buffer[n] renumbered to slots 0,1,2.. in order of appearance. */
static void prog_add_stage(Prog *p, const char *text, int len, int serial) {
    static int nlog;
    len = (int)strnlen(text, (size_t)len);
    int s = stage_of(text, len);
    if (s < 0) { logf_limited(&nlog, 10, "nvn_gl: program #%d: shader text of unknown kind '%.12s'\n", serial, text); return; }
    Stage *st = &p->st[s];
    char *out = malloc((size_t)len + 64);
    int o = 0;
    st->nbuf = 0; st->storage = 0;
    for (int i = 0; i < len;) {
        if (len - i > 16 && !memcmp(text + i, "program.buffer[", 15)) {
            int n = 0, slot;
            i += 15;
            while (i < len && text[i] >= '0' && text[i] <= '9') n = n * 10 + (text[i++] - '0');
            for (slot = 0; slot < st->nbuf && st->map[slot] != n; slot++) {}
            if (slot == st->nbuf && st->nbuf < 16) st->map[st->nbuf++] = (uint8_t)n;
            o += sprintf(out + o, "program.buffer[%d", slot);
            InterlockedIncrement(&g_bufhist[s][n & 31]);
            continue;
        }
        if (len - i > 17 && !memcmp(text + i, "program.storage[", 16)) {
            int n = atoi(text + i + 16);
            st->storage |= 1u << (n & 31);
            InterlockedIncrement(&g_storhist[n & 31]);
        }
        out[o++] = text[i++];
    }
    out[o] = 0;
    st->storage_w = 0;
    {   /* storage buffers the program writes: STB / ATOMB / CSWAPB lines name "sbo_bufK", declared as STORAGE sbo_bufK[] = { program.storage[n] } */
        int8_t decl[32];
        memset(decl, -1, sizeof decl);
        for (const char *q = out; (q = strstr(q, "STORAGE sbo_buf")) != NULL; q += 15) {
            int k = atoi(q + 15);
            const char *r = strstr(q, "program.storage[");
            if (r && k >= 0 && k < 32) decl[k] = (int8_t)(atoi(r + 16) & 31);
        }
        for (const char *q = out; q && *q; q = strchr(q, 10), q = q ? q + 1 : NULL) {
            if (strncmp(q, "STB", 3) && strncmp(q, "ATOM", 4) && strncmp(q, "CSWAP", 5)) continue;
            const char *eol = strchr(q, 10), *r = strstr(q, "sbo_buf");
            if (r && (!eol || r < eol)) { int k = atoi(r + 7); if (k >= 0 && k < 32 && decl[k] >= 0) st->storage_w |= 1u << decl[k]; }
        }
    }
    free(st->text);
    st->text = out; st->len = o; st->state = 1;
    const char *dir = getenv("SSBU_SHADERDUMP");
    if (dir) {
        char path[260];
        snprintf(path, sizeof path, "%s/p%05d_%s.txt", dir, serial, k_stage_name[s]);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(text, 1, (size_t)len, f); fclose(f); }
    }
}
static int stage_ready(Prog *p, int s) {
    static int nlog;
    Stage *st = &p->st[s];
    if (st->state == 2) return 1;
    if (st->state != 1) return 0;
    GLint pos = -1;
    glGenProgramsARB(1, &st->gl);
    glBindProgramARB(k_prog_target[s], st->gl);
    while (glGetError()) {}
    glProgramStringARB(k_prog_target[s], GL_PROGRAM_FORMAT_ASCII_ARB_, st->len, st->text);
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB_, &pos);
    if (pos != -1 || glGetError()) {
        st->state = 3;
        g_st.prog_fail++;
        if (nlog < 12) {
            char path[64];
            snprintf(path, sizeof path, "shader_fail_%02d.txt", nlog);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(st->text, 1, (size_t)st->len, f); fclose(f); }
        }
        logf_limited(&nlog, 40, "nvn_gl: program #%d %s stage REJECTED at %d (%d buffers): %s\n", p->serial, k_stage_name[s], pos, st->nbuf, (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB_));
        return 0;
    }
    st->state = 2;
    g_st.prog_ok++;
    free(st->text);
    st->text = NULL;
    return 1;
}

/* A shader that came without assembly text: Maxwell machine code (program header + 64-bit instruction words). */
static void dump_binary(Prog *p, int s) {
    const char *dir = getenv("SSBU_SHADERDUMP");
    char path[260];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/b%05d_%s.bin", dir, p->serial, k_stage_name[s]);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(p->st[s].ctl, 1, 32, f); fwrite(p->st[s].code, 1, (size_t)p->st[s].code_len, f); fclose(f); }
}
static GLuint g_pipeline;
/* The GLSL program for a stage given as machine code; 0 when it cannot be translated or the driver refuses it. */
/* key: integer vertex inputs (all, signed) and integer textures (all, signed) as seen at this draw; a stage is translated
 * again when the bits it depends on change. */
/* P3: translated vertex / fragment stages are shared and kept (SSBU_NO_SHADERCACHE=1 turns this off).
 *   level 1: (stage, machine code, constants, the four key words) -> the translation's result and its GL program:
 *            a program object the game creates again from the same shader binary, or a stage that alternates between two
 *            keys, costs a table look-up instead of a translation and a driver compile;
 *   level 2: generated GLSL text -> GL program: two keys that lead to the same text share the program.
 * Shared GL programs are never deleted (a few thousand at most: the game has about 6,900 distinct stages). Per-draw state
 * of a program (the alpha-test uniform) is set at every draw that uses it, so sharing does not change what is drawn.
 * Entries are found by 64-bit hashes of the contents (plus length, stage and key compared exactly). */
typedef struct ShEnt { uint64_t h; int stage, code_len; uint32_t key[4]; GLuint prog; uint32_t cbufs, ssbos, in_attr, tex_units, ssbo_written; int uses_alpha, uses_fcs, ubo; struct ShEnt *next; } ShEnt;
typedef struct ShSrc { uint64_t h; size_t len; GLuint prog; struct ShSrc *next; char *text; } ShSrc;
static ShSrc *g_prog_src[65536];                 /* P3 round 3: GL stage program -> its text record (pair programs) */
static GLuint g_pair_cur;                        /* the linked pair program in use (glUseProgram), 0 = none */
static void pair_off(void);
static ShEnt *g_shc[4096];
static ShSrc *g_shs[4096];
static int g_shc_off = -1;
static LONG64 g_shc_hit, g_shc_src_hit, g_shc_compiled;
static uint64_t sh_hash(uint64_t h, const void *data, size_t n);
static uint32_t code_tag(Stage *st) {          /* a stage's machine code, hashed (debug aids: the same in every run) */
    if (!st->code_tag && st->code && st->code_len > 0) st->code_tag = (uint32_t)sh_hash(0, st->code, (size_t)st->code_len) | 1u;
    return st->code_tag;
}
static uint64_t sh_hash(uint64_t h, const void *data, size_t n) {
    const uint8_t *b = data;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x100000001B3ULL; h ^= h >> 31; }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL;
    return h;
}
/* P3: the driver's compiled form of a translated stage is kept on disk. The driver needs 100-300 ms to compile one of
 * these stages (measured: about 500 new stages in a 200 s run with a battle, 8-60 ms per frame on average while they
 * arrive), which is what makes the frame time jump whenever something new comes on screen. With the binary on disk the
 * second and later runs load it in about a millisecond.
 *   file: <dir>/<hash of the GLSL text>_<text length>_<hash of GL_VENDOR, GL_RENDERER, GL_VERSION>.bin = format, size, bytes
 *   The key is the generated TEXT, so a change of the translator can never pick up an old binary; another driver or GPU
 *   gets other file names, and a binary the driver refuses (glProgramBinary leaves the program unlinked) is compiled again
 *   and rewritten. Files are written under a temporary name and renamed, so several running copies can share the folder.
 *   SSBU_SHADER_CACHE=<dir> chooses the folder (default: ..\\cache\\shaders, i.e. Documents\\SSBU\\cache\\shaders), SSBU_SHADER_CACHE=0
 *   turns it off (every stage is then compiled with glCreateShaderProgramv, as before). */
static LONG64 g_shd_loaded, g_shd_saved, g_shd_refused;
static volatile LONG64 g_shw_queued, g_shw_done, g_shw_compiled, g_shw_present, g_shw_failed;
static volatile LONG g_shp_queueing;                       /* the shader store's variations are being queued (shp_queue) */
static DWORD g_shw_tid[8];
/* hostrt.c, the crash report: which of this file's host threads a thread id is, or NULL */
const char *nvn_front_thread_kind(unsigned long tid) { static char b[32]; if (tid == g_render_tid) return "render thread"; for (int i = 0; i < 8; i++) if (g_shw_tid[i] && g_shw_tid[i] == tid) { snprintf(b, sizeof b, "shader worker %d", i); return b; } return NULL; }
static volatile LONG g_pw_listed_done;                      /* the start-up queueing of pairs.txt is over (boot.c waits for the workers then) */
/* boot.c: how many worker jobs are still pending, and how many were queued in all; -1 until the start-up queueing is over */
long long nvn_front_warm_pending(long long *total) {
    LONG64 q = g_shw_queued, d = g_shw_done;
    *total = q;
    return g_pw_listed_done && !g_shp_queueing ? (q > d ? q - d : 0) : -1;
}
static int g_shd_mode = -1;
static char g_shd_dir[200];
static uint64_t g_shd_drv;
static void shd_workers_start(void);
static void prec_load(void);
/* Render thread, once, after the context exists. */
static void shd_init(void) {
    const char *e = getenv("SSBU_SHADER_CACHE");
    g_shd_mode = !(e && !strcmp(e, "0")) && glCreateShader && glShaderSource && glCompileShader && glGetShaderiv && glAttachShader && glDetachShader && glDeleteShader &&
                 glCreateProgram && glLinkProgram && glProgramParameteri && glGetProgramBinary && glProgramBinary;
    if (!g_shd_mode) return;
    const char *v[3] = {(const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION)};
    snprintf(g_shd_dir, sizeof g_shd_dir, "%s", e && e[0] ? e : "../cache/shaders");      /* Documents\\SSBU\\cache\\shaders when run from recomp-study */
    if (!(e && e[0])) CreateDirectoryA("../cache", NULL);
    CreateDirectoryA(g_shd_dir, NULL);
    g_shd_drv = 0x243F6A8885A308D3ULL;
    for (int i = 0; i < 3; i++) if (v[i]) g_shd_drv = sh_hash(g_shd_drv, v[i], strlen(v[i]));
    printf("nvn_gl: shader binaries are kept in %s (SSBU_SHADER_CACHE=0 turns this off)%c", g_shd_dir, 10);
    shd_workers_start();
    prec_load();
    { void xmemo_load(void); xmemo_load(); }
}
static void shd_path(char *path, size_t size, uint64_t h, size_t len) {
    snprintf(path, size, "%s/%016llx_%zx_%016llx.bin", g_shd_dir, (unsigned long long)h, len, (unsigned long long)g_shd_drv);
}
/* Compile + link as a separable program (what glCreateShaderProgramv does, plus the hint that the binary will be asked
 * for) and write the binary to `path`. Returns the program, 0 when the shader does not compile. Any thread with a context. */
static GLuint shd_compile_save(GLenum type, const char *src, const char *path, GLint *ok, int *saved) {
    char tmp[360];
    GLint cs = 0, n = 0;
    GLuint sh = glCreateShader(type), q;
    *ok = 0; *saved = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, 0x8B81 /* COMPILE_STATUS */, &cs);
    if (!cs) { glDeleteShader(sh); return 0; }
    q = glCreateProgram();
    glProgramParameteri(q, 0x8258 /* PROGRAM_SEPARABLE */, 1);
    glProgramParameteri(q, 0x8257 /* PROGRAM_BINARY_RETRIEVABLE_HINT */, 1);
    glAttachShader(q, sh);
    glLinkProgram(q);
    glDetachShader(q, sh);
    glDeleteShader(sh);
    glGetProgramiv(q, 0x8B82, ok);
    if (!*ok) return q;
    glGetProgramiv(q, 0x8741 /* PROGRAM_BINARY_LENGTH */, &n);
    if (n > 0) {
        uint32_t head[2] = {0, 0};
        GLsizei got = 0;
        GLenum fmt = 0;
        void *data = malloc((size_t)n);
        while (glGetError()) {}
        glGetProgramBinary(q, n, &got, &fmt, data);
        if (!glGetError() && got > 0) {
            snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId());
            FILE *f = fopen(tmp, "wb");
            if (f) {
                head[0] = fmt; head[1] = (uint32_t)got;
                int good = fwrite(head, 4, 2, f) == 2 && fwrite(data, 1, (size_t)got, f) == (size_t)got;
                fclose(f);
                if (good && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) *saved = 1; else DeleteFileA(tmp);
            }
        }
        free(data);
    }
    return q;
}
/* Render thread: the program for this text, from disk when its binary is there. */
/* The program of a GLSL text (hash h, length len) from its binary in the cache; 0 = not there or refused. */
static GLuint shader_binary_load(uint64_t h, size_t len, GLint *ok);
static GLuint shader_binary_load(uint64_t h, size_t len, GLint *ok) {
    char path[320];
    *ok = 0;
    shd_path(path, sizeof path, h, len);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint32_t head[2] = {0, 0};
        if (fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < (64u << 20)) {
            void *data = malloc(head[1]);
            if (fread(data, 1, head[1], f) == head[1]) {
                GLuint q = glCreateProgram();
                while (glGetError()) {}
                HS_IN("shader binary from disk (glProgramBinary)", 0);
                glProgramParameteri(q, 0x8258 /* PROGRAM_SEPARABLE */, 1);
                glProgramBinary(q, head[0], data, (GLsizei)head[1]);
                glGetProgramiv(q, 0x8B82, ok);
                HS_OUT();
                while (glGetError()) {}
                if (*ok) { free(data); fclose(f); g_shd_loaded++; return q; }
                glDeleteProgram(q);
                g_shd_refused++;
            }
            free(data);
        }
        fclose(f);
    }
    return 0;
}
static GLuint shader_build(GLenum type, const char *src, size_t len, uint64_t h, GLint *ok) {
    char path[320];
    int saved = 0;
    *ok = 0;
    if (g_shd_mode <= 0) { GLuint q = glCreateShaderProgramv(type, 1, &src); glGetProgramiv(q, 0x8B82, ok); return q; }
    GLuint q = shader_binary_load(h, len, ok);
    if (q) return q;
    shd_path(path, sizeof path, h, len);
    { HS_IN(type == 0x8B31 ? "shader compile + link, vertex stage (driver)" : type == 0x91B9 ? "shader compile + link, compute stage (driver)" : "shader compile + link, fragment stage (driver)", 0); q = shd_compile_save(type, src, path, ok, &saved); HS_OUT(); }
    if (!q) { q = glCreateShaderProgramv(type, 1, &src); glGetProgramiv(q, 0x8B82, ok); return q; }     /* does not compile: the old path reports it (its log holds the compiler's messages) */
    g_shd_saved += saved;
    return q;
}
/* The translations made before (2026-10-31): a stage's machine code + key + the settings that shape its GLSL -> the GLSL's
 * name (hash, length) and what the translator found in it, in <cache>/xlate_<exe>.txt (the exe by its link stamp: another
 * build translates otherwise). A stage whose binary is there needs no translation at its first draw then: the title's first
 * 3D frame translated 30-60 stages, 56-66 ms. TOMO_XMEMO=0: every stage translated at its first draw, as before. */
void xmemo_load(void);
typedef struct { uint64_t hm, h2; uint32_t src_len, cbufs, ssbos, in_attr, tex_units, ssbo_written; int stage, code_len, uses_alpha, uses_fcs, ubo; } XMemo;
#define XM_SLOTS (1 << 16)
static XMemo *g_xm;
static int g_xm_n, g_xm_on = -1, g_xm_new;
static FILE *g_xm_out;
static LONG64 g_xm_hit;
static uint64_t xmemo_key(uint64_t h1, const MxIn *in) { int k[4] = {in->wide, in->screen_scale, in->ubo, in->fragdebug}; return sh_hash(h1 ^ 0x584d454dULL, k, sizeof k) | 1; }
static XMemo *xmemo_slot(uint64_t hm, int insert) {
    for (unsigned i = (unsigned)(hm >> 20) & (XM_SLOTS - 1), k = 0; k < XM_SLOTS; i = (i + 1) & (XM_SLOTS - 1), k++) {
        if (g_xm[i].hm == hm) return &g_xm[i];
        if (!g_xm[i].hm) { if (!insert || g_xm_n >= XM_SLOTS / 2) return NULL; g_xm_n++; g_xm[i].hm = hm; return &g_xm[i]; }
    }
    return NULL;
}
void xmemo_load(void) {                            /* render thread, shd_init */
    char path[320], line[256];
    if (g_xm_on < 0) g_xm_on = !(getenv("TOMO_XMEMO") && getenv("TOMO_XMEMO")[0] == '0') && !getenv("TOMO_SHOVR");
    if (!g_xm_on || g_shd_mode <= 0 || !(g_xm = calloc(XM_SLOTS, sizeof *g_xm))) { g_xm_on = 0; return; }
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)dos + dos->e_lfanew);
    char exe[MAX_PATH] = "", head[96];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;
    snprintf(path, sizeof path, "%s/xlate_%s.txt", g_shd_dir, base);
    snprintf(head, sizeof head, "xlate %08lx %08lx %016llx", (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage, (unsigned long long)g_shd_drv);
    FILE *f = fopen(path, "r");
    int same = f && fgets(line, sizeof line, f) && !strncmp(line, head, strlen(head));
    while (same && fgets(line, sizeof line, f)) {
        XMemo m = {0};
        unsigned long long hm, h2;
        if (sscanf(line, "%llx %llx %x %x %x %x %x %x %d %d %d %d %d", &hm, &h2, &m.src_len, &m.cbufs, &m.ssbos, &m.in_attr, &m.tex_units, &m.ssbo_written, &m.stage, &m.code_len, &m.uses_alpha, &m.uses_fcs, &m.ubo) != 13 || !hm) continue;
        XMemo *e = xmemo_slot(hm, 1);
        if (e) { m.hm = hm; m.h2 = h2; *e = m; }
    }
    if (f) fclose(f);
    g_xm_out = fopen(path, same ? "a" : "w");
    if (g_xm_out && !same) fprintf(g_xm_out, "%s\n", head);
    printf("nvn_gl: %d stage translations known from earlier runs (%s)%c", g_xm_n, path, 10);
}
static void xmemo_put(uint64_t hm, int stage, int code_len, uint64_t h2, uint32_t src_len, const MxOut *o) {
    if (!g_xm_on || !hm) return;
    XMemo *e = xmemo_slot(hm, 1);
    if (!e || e->h2) return;
    e->h2 = h2; e->src_len = src_len; e->stage = stage; e->code_len = code_len;
    e->cbufs = o->cbufs; e->ssbos = o->ssbos; e->in_attr = o->in_attr; e->tex_units = o->tex_units; e->ssbo_written = o->ssbo_written; e->uses_alpha = o->uses_alpha; e->uses_fcs = o->uses_fcs; e->ubo = o->ubo;
    if (g_xm_out) {
        fprintf(g_xm_out, "%016llx %016llx %x %x %x %x %x %x %d %d %d %d %d\n", (unsigned long long)hm, (unsigned long long)h2, src_len, e->cbufs, e->ssbos, e->in_attr, e->tex_units, e->ssbo_written, stage, code_len, e->uses_alpha, e->uses_fcs, e->ubo);
        if (++g_xm_new % 32 == 0) fflush(g_xm_out);
    }
}

/* ---- compiling ahead of the first draw (P3)
 * The game hands over a program's shaders (nvnProgramSetShaders) when it loads a fighter, a stage or an effect, usually
 * long before the first draw that uses it. Worker threads, each with a small OpenGL context of its own (nothing shared
 * with the render thread's context), translate those stages at once and put the driver's binary into the disk cache; when
 * the first draw comes, the render thread finds the binary there. Nothing is drawn differently: a stage whose binary is
 * not ready is compiled by the render thread exactly as before. The workers assume the common case for what a stage's
 * text can depend on (no integer vertex inputs or textures); a draw with another combination compiles its own text.
 * SSBU_SHADER_WORKERS=<n> (default 3, 0 = none). */
typedef struct ShJob { struct ShJob *next; int stage, code_len; uint32_t cdata_size; uint32_t key[4]; uint8_t data[1]; } ShJob;
/* P3 round 2: which text variants of a stage were needed. A stage's GLSL depends on which vertex inputs and textures hold
 * integers at the draw (the four key words); the workers used to compile the variant with none only, so a stage first
 * drawn with another combination was compiled by the render thread in the middle of a frame (0.3-1.3 s for a vertex
 * stage, hitch log). Every key the render thread had to compile for is appended to <cache>/variants.txt as
 * "<hash of code and constants> <stage> <key words>"; at the next start the workers compile those variants as well, as
 * soon as the game hands the stage over. Nothing is drawn differently: this only decides what is compiled ahead. */
typedef struct ShVar { uint64_t h; int stage; uint32_t key[4]; struct ShVar *next; } ShVar;
static ShVar *g_shv[4096];
static CRITICAL_SECTION g_shw_lock;
static LONG64 g_shv_loaded, g_shv_added, g_shv_queued;
static volatile LONG64 g_shw_held, g_shw_held_ticks;
static void shv_put(uint64_t h, int stage, const uint32_t *key) {
    ShVar *e = calloc(1, sizeof *e);
    e->h = h; e->stage = stage; memcpy(e->key, key, 16); e->next = g_shv[h & 4095]; g_shv[h & 4095] = e;
}
static int shv_has(uint64_t h, int stage, const uint32_t *key) {
    for (ShVar *e = g_shv[h & 4095]; e; e = e->next) if (e->h == h && e->stage == stage && !memcmp(e->key, key, 16)) return 1;
    return 0;
}
static void shv_load(void) {                       /* render thread, before the workers start */
    char path[260], line[200];
    snprintf(path, sizeof path, "%s/variants.txt", g_shd_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long long h; int stage; uint32_t k[4];
        if (sscanf(line, "%llx %d %x %x %x %x", &h, &stage, &k[0], &k[1], &k[2], &k[3]) == 6 && stage >= 0 && stage < 2 && !shv_has(h, stage, k)) { shv_put(h, stage, k); g_shv_loaded++; }
    }
    fclose(f);
}
static void shv_record(uint64_t h, int stage, const uint32_t *key) {      /* render thread: this key had to be compiled here */
    char path[260];
    if (g_shd_mode <= 0 || !(key[0] | key[1] | key[2] | key[3])) return;
    EnterCriticalSection(&g_shw_lock);
    int known = shv_has(h, stage, key);
    if (!known) shv_put(h, stage, key);
    LeaveCriticalSection(&g_shw_lock);
    if (known) return;
    snprintf(path, sizeof path, "%s/variants.txt", g_shd_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx %d %x %x %x %x\n", (unsigned long long)h, stage, key[0], key[1], key[2], key[3]); fclose(f); g_shv_added++; }
}
static CRITICAL_SECTION g_shw_lock;
static ShJob *g_shw_head, *g_shw_tail;
static HANDLE g_shw_sem;
static uint64_t g_shw_seen[16384];
static int g_shw_n;
/* ---- P3 round 4: the driver's work at the first draw of a shader pair, done ahead by the workers
 * What the driver does (measured with tools_p3/p4lab.c, P3_performance.md section 13): a program loaded with
 * glProgramBinary (or linked) is not ready to run; at the FIRST draw call that uses a (vertex, fragment) combination the
 * driver generates its code, 40-190 ms, without any debug message, under a lock that also stops the draw calls of every
 * other context of the process. It keeps the result in its own cache: in memory for the process (keyed by the CONTENT
 * of the two programs: other program objects, other contexts and other threads get it), and on disk in a file per
 * executable NAME (a copy of the exe under another name starts cold, the same name in another folder or relinked is
 * warm). Our program binaries do not contain it (read back after the draw and loaded under a new name: cold again).
 * So: a worker thread, in its small context of its own, loads the two programs and draws ONE point with the rasterizer
 * off into its own 8x8 window. Nothing of the game's frame is involved; when the render thread later draws with its
 * own program objects of the same binaries, the driver finds the pair in its memory cache (1 ms instead of 65).
 * Which pairs: (1) every pair a draw used in an earlier run (<cache>/pairs.txt), queued at start; (2) the vertex and
 * fragment stage of a program the game hands over (nvnProgramSetShaders) after start-up, or always with
 * SSBU_PAIR_WARM=2; the caller is held until its program is ready, as the stage compile already did.
 * When: the driver's lock means this work cannot hide behind the game (a 65 ms stall per cold pair on the render
 * thread), so while COLD work is pending (a warm draw or a compile took more than 8 ms) the render thread holds the
 * next present until the workers are done: the cost is paid as loading time, before the frames that need it.
 * With a warm driver cache a pair costs the workers about 2 ms and nothing is held. */
typedef struct PwSeen { uint64_t vh, fh; uint32_t vl, fl, mask; struct PwSeen *next; } PwSeen;
static PwSeen *g_pw_seen[4096];
static uint64_t g_pw_qseen[16384];
static CRITICAL_SECTION g_pw_stripe[64];
static volatile LONG g_pw_cold, g_shw_ctx_failed;
static volatile LONG64 g_pw_named, g_pw_progs, g_pw_warmed, g_pw_cold_n, g_pw_ticks, g_pw_missing, g_pw_dup, g_pw_gate_ticks, g_pw_gates, g_pw_compiled;
static int pw_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("SSBU_PAIR_WARM"); m = e ? atoi(e) : 1; } return m; }
static int pw_seen(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask) {
    unsigned k = (unsigned)((vh ^ fh * 0x9E3779B97F4A7C15ULL) >> 20) & 4095;
    int found = 0;
    EnterCriticalSection(&g_shw_lock);
    for (PwSeen *e = g_pw_seen[k]; e && !found; e = e->next) found = e->vh == vh && e->fh == fh && e->vl == vl && e->fl == fl && e->mask == mask;
    if (!found) { PwSeen *e = calloc(1, sizeof *e); e->vh = vh; e->vl = vl; e->fh = fh; e->fl = fl; e->mask = mask; e->next = g_pw_seen[k]; g_pw_seen[k] = e; }
    LeaveCriticalSection(&g_shw_lock);
    return found;
}
static void pw_cold_mark(LONG64 t0) {
    static LONG64 freq;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    if ((pf_now() - t0) * 125 > freq) InterlockedExchange(&g_pw_cold, 1);          /* more than 8 ms */
}
/* Worker: the program for a stage text, from its binary; compiled (and saved) when there is none and the text is given. */
static GLuint pw_load(int stage, uint64_t th, size_t tl, const char *src) {
    char path[320];
    GLuint q = 0;
    GLint ok = 0;
    shd_path(path, sizeof path, th, tl);
    EnterCriticalSection(&g_pw_stripe[th & 63]);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint32_t head[2] = {0, 0};
        if (fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < (64u << 20)) {
            void *data = malloc(head[1]);
            if (fread(data, 1, head[1], f) == head[1]) {
                q = glCreateProgram();
                glProgramParameteri(q, 0x8258 /* PROGRAM_SEPARABLE */, 1);
                glProgramBinary(q, head[0], data, (GLsizei)head[1]);
                glGetProgramiv(q, 0x8B82, &ok);
                if (!ok) { glDeleteProgram(q); q = 0; }
            }
            free(data);
        }
        fclose(f);
    }
    if (!q && src) {
        int saved = 0;
        LONG64 t0 = pf_now();
        q = shd_compile_save(stage == 0 ? 0x8B31 : 0x8B30, src, path, &ok, &saved);
        if (q && !ok) { glDeleteProgram(q); q = 0; }
        if (saved) { InterlockedIncrement64(&g_shw_compiled); InterlockedIncrement64(&g_pw_compiled); }
        pw_cold_mark(t0);
    }
    LeaveCriticalSection(&g_pw_stripe[th & 63]);
    while (glGetError()) {}
    return q;
}
static char *pw_translate(int stage, const uint8_t *code, int code_len, const uint8_t *cdata, uint32_t cdata_size, uint32_t *in_attr, uint32_t int_attrs) {
    MxIn in;
    MxOut out;
    memset(&in, 0, sizeof in);
    in.stage = stage; in.code = code; in.code_len = code_len; in.int_attrs = int_attrs;
    if (cdata_size) { in.cdata = cdata; in.cdata_size = cdata_size; }
    in.binding_base = (unsigned)stage * 20; in.ssbo_base = 40 + (unsigned)stage * 16; in.ubo = ubo_on(); in.ubo_base = (unsigned)stage * 18;
    in.fragdebug = getenv("SSBU_FRAGDEBUG") != NULL; in.screen_scale = in.stage == 1 && rs_on(); in.wide = wide_k() != 1.0f ? 1 + g_wide_camera : 0;
    if (!mx_translate(&in, &out)) return NULL;
    if (in_attr) *in_attr = out.in_attr;
    return out.glsl;
}
/* <cache>/progs.txt: one line per program (hash of the machine code of its stages, as pw_queue_prog computes it) whose
 * pair a worker has made ready and written to pairs.txt. Read at start into the table pw_queue_prog looks at. */
static void pw_prog_known(const ShJob *j) {
    char path[260];
    uint64_t h;
    if (j->stage != 3 || getenv("SSBU_NO_PAIR_LIST")) return;
    memcpy(&h, &j->key[2], 8);
    snprintf(path, sizeof path, "%s/progs.txt", g_shd_dir);
    EnterCriticalSection(&g_shw_lock);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx\n", (unsigned long long)h); fclose(f); }
    LeaveCriticalSection(&g_shw_lock);
}
static LONG64 g_pw_progs_known;
static void pw_progs_load(void) {                  /* render thread, start, only when pairs.txt was read */
    char path[260], line[64];
    snprintf(path, sizeof path, "%s/progs.txt", g_shd_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    EnterCriticalSection(&g_shw_lock);
    while (fgets(line, sizeof line, f)) {
        unsigned long long h = 0;
        if (sscanf(line, "%llx", &h) != 1 || !h) continue;
        unsigned k = (unsigned)(h >> 20) & 16383;
        for (int probe = 0; probe < 64; probe++, k = (k + 1) & 16383) { if (g_pw_qseen[k] == h) break; if (!g_pw_qseen[k]) { g_pw_qseen[k] = h; g_pw_progs_known++; break; } }
    }
    LeaveCriticalSection(&g_shw_lock);
    fclose(f);
}
static void shp_variant(const ShJob *j, uint32_t int_attrs);
static void shp_finish_check(void);
static int shp_warmed(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask, int add);
static volatile LONG64 g_shp_total, g_shp_done, g_shp_t0;      /* shader preparation (below shd_workers_start) */
/* Worker: one pair job. stage 2 = named by the two text hashes (data: vh, fh, vl, fl); stage 3 = the machine code of a
 * program's vertex stage (code_len, cdata_size) and fragment stage (key[0], key[1]; 0 = none: depth-only); stage 4 = the
 * same for a variation of the shader store (shader preparation), key[2] = the vertex stage's integer inputs. */
/* 2026-10-30: the driver does not keep the pairs' first-draw work for good (a full re-warm found 3,095 of 8,243 pairs cold
 * a day after they were warmed; the 2026-10-29 session stalled at 651 of 850 first draws, 26 ms each). Re-warming the
 * whole list at each start costs 15 s, so: the first PW_SAMPLE listed pairs (the title's and the island's) are warmed at
 * every start, 0.2 s when the driver has them; when more than PW_SAMPLE_COLD of them are cold the cache was purged, and
 * the rest of the list is warmed too. TOMO_PAIR_REWARM=1: the whole list, every start.
 * 2026-11-01: NEVER while the game runs. A worker's warm draw holds a lock of the driver while it compiles, and the driver's
 * thread that runs the render thread's calls waits for that lock (call stacks at the freeze): warmed while the game started,
 * the game's 4th frame (its render targets) waited 3.9-4.3 s, 1.7 s with two workers. With its own cache folder (boot.c) the
 * driver keeps the pairs, and 8-12 of the 64 took 20 ms or more at every start anyway (twelve contexts warming at once), so
 * over 10 had set that off in every second start: more than half now (a new driver), and the rest is warmed before the game
 * starts, in list order (the earliest used first), for 60 s at most; what is left then compiles at its first draw.
 * TOMO_PW_COLD=<n>: another limit (test). */
#define PW_SAMPLE 64
#define PW_SAMPLE_COLD (getenv("TOMO_PW_COLD") ? atoi(getenv("TOMO_PW_COLD")) : 32)
#define PW_REWARM_S 60
typedef struct { uint64_t vh, fh; uint32_t vl, fl, mask; } PwPair;
static PwPair *g_pw_rest;
static int g_pw_rest_n, g_pw_rest_cap;
static volatile LONG g_pw_sample_n, g_pw_sample_done, g_pw_sample_cold, g_pw_sample_decided;
static LONG64 g_pw_rewarm_end;
static void pw_queue_named(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask, int sample);
static void pw_sample_note(int cold) {              /* worker: a sampled pair is done */
    if (cold) InterlockedIncrement(&g_pw_sample_cold);
    if (InterlockedIncrement(&g_pw_sample_done) != g_pw_sample_n || InterlockedExchange(&g_pw_sample_decided, 1)) return;
    if (g_pw_sample_cold > PW_SAMPLE_COLD) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_pw_rewarm_end = pf_now() + f.QuadPart * PW_REWARM_S;
        printf("nvn_gl: shader pairs: %ld of %ld sampled pairs were cold: the driver's cache was purged (a new driver?), the other %d listed pairs are warmed before the game starts, %d s at most%c", g_pw_sample_cold, g_pw_sample_n, g_pw_rest_n, PW_REWARM_S, 10);
        for (int i = 0; i < g_pw_rest_n; i++) pw_queue_named(g_pw_rest[i].vh, g_pw_rest[i].vl, g_pw_rest[i].fh, g_pw_rest[i].fl, g_pw_rest[i].mask, 2);      /* (queued before the last sampled job counts as done: the start waits for them) */
    } else printf("nvn_gl: shader pairs: %ld of %ld sampled pairs were cold: the driver's cache holds them, the other %d listed pairs are not warmed again%c", g_pw_sample_cold, g_pw_sample_n, g_pw_rest_n, 10);
    fflush(stdout);
    free(g_pw_rest); g_pw_rest = NULL; g_pw_rest_n = g_pw_rest_cap = 0;
}
static int pw_job_inner(ShJob *j, GLuint pipe, GLuint empty_fs) {      /* 1 = the pair's warm draw was cold (20 ms or more) */
    int cold = 0;
    uint64_t vh = 0, fh = 0;
    uint32_t vl = 0, fl = 0, mask = 0;             /* mask: the vertex arrays that are enabled for the warm draw */
    char *vt = NULL, *ft = NULL;
    if (j->stage == 2) { memcpy(&vh, j->data, 8); memcpy(&fh, j->data + 8, 8); memcpy(&vl, j->data + 16, 4); memcpy(&fl, j->data + 20, 4); memcpy(&mask, j->data + 24, 4); }
    else {
        const uint8_t *vc = j->data, *vd = vc + j->code_len, *fc = vd + j->cdata_size, *fd = fc + j->key[0];
        vt = pw_translate(0, vc, j->code_len, vd, j->cdata_size, &mask, j->stage == 4 ? j->key[2] : 0);      /* a program handed over: every input the vertex stage reads comes from an array */
        mask &= 0xffff;
        if (j->key[0]) ft = pw_translate(1, fc, (int)j->key[0], fd, j->key[1], NULL, 0);
        if (j->stage == 4 && !j->key[2] && (mask >> 6 & 1)) shp_variant(j, 0x40);      /* attribute 6 as integers too (skinning's bone indices) */
        if (!vt || (j->key[0] && !ft)) { free(vt); free(ft); InterlockedIncrement64(&g_pw_missing); return cold; }
        vl = (uint32_t)strlen(vt); vh = sh_hash(0x9E3779B97F4A7C15ULL ^ 0, vt, vl);
        if (ft) { fl = (uint32_t)strlen(ft); fh = sh_hash(0x9E3779B97F4A7C15ULL ^ 1, ft, fl); }
    }
    if (pw_seen(vh, vl, fh, fl, mask)) { free(vt); free(ft); InterlockedIncrement64(&g_pw_dup); pw_prog_known(j); return cold; }
    if (j->stage == 4 && shp_warmed(vh, vl, fh, fl, mask, 0)) { free(vt); free(ft); return cold; }      /* a preparation before this one did it */
    if (mask & 0x10000) {                          /* a compute program: one work group with no buffer and no image bound (reads give 0, writes go nowhere) */
        GLuint cs = pw_load(5, vh, vl, NULL);
        if (cs) {
            static LONG64 freq;
            if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
            glUseProgramStages(pipe, 0x20 /* COMPUTE_SHADER_BIT */, cs);
            LONG64 t0 = pf_now();
            glDispatchCompute(1, 1, 1);
            glFinish();
            LONG64 dt = pf_now() - t0;
            InterlockedExchangeAdd64(&g_pw_ticks, dt); InterlockedIncrement64(&g_pw_warmed);
            if (dt * 125 > freq) { InterlockedIncrement64(&g_pw_cold_n); InterlockedExchange(&g_pw_cold, 1); }
        cold = dt * 50 > freq;
            if (g_fdlog) printf("fd-warm: %016llx %x %016llx %x mask %x kind %d %.1f ms frame %lld%c", (unsigned long long)vh, vl, 0ull, 0u, mask, j->stage, (double)dt * 1000.0 / (double)freq, (long long)g_st.frames, 10);
            glUseProgramStages(pipe, 0x20, 0);
            glDeleteProgram(cs);
        } else InterlockedIncrement64(&g_pw_missing);
        while (glGetError()) {}
        return cold;
    }
    GLuint vs = pw_load(0, vh, vl, vt), fs = fl ? pw_load(1, fh, fl, ft) : empty_fs;
    free(vt); free(ft);
    if (vs && fs) {
        glUseProgramStages(pipe, 1, vs);
        glUseProgramStages(pipe, 2, fs);
        for (GLuint k = 0; k < 16; k++) { if (mask >> k & 1) glEnableVertexAttribArray(k); else glDisableVertexAttribArray(k); }
        LONG64 t0 = pf_now();
        glDrawArrays(GL_POINTS, 0, 1);
        glFinish();
        LONG64 dt = pf_now() - t0;
        static LONG64 freq;
        if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
        InterlockedExchangeAdd64(&g_pw_ticks, dt); InterlockedIncrement64(&g_pw_warmed);
        if (dt * 125 > freq) { InterlockedIncrement64(&g_pw_cold_n); InterlockedExchange(&g_pw_cold, 1); }
        cold = dt * 50 > freq;
        if (g_fdlog) printf("fd-warm: %016llx %x %016llx %x mask %x kind %d %.1f ms frame %lld%c", (unsigned long long)vh, vl, (unsigned long long)fh, fl, mask, j->stage, (double)dt * 1000.0 / (double)freq, (long long)g_st.frames, 10);
        glUseProgramStages(pipe, 1, 0);
        glUseProgramStages(pipe, 2, 0);
        if (j->stage == 4) shp_warmed(vh, vl, fh, fl, mask, 1);
        if (j->stage == 3 && !getenv("SSBU_NO_PAIR_LIST")) {          /* into <cache>/pairs.txt (same line format as prec_note) */
            char path[260];
            snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
            EnterCriticalSection(&g_shw_lock);
            FILE *f = fopen(path, "a");
            if (f) { fprintf(f, "%016llx %x %016llx %x %x\n", (unsigned long long)vh, (unsigned)vl, (unsigned long long)fh, (unsigned)fl, (unsigned)mask); fclose(f); }
            LeaveCriticalSection(&g_shw_lock);
            pw_prog_known(j);
        }
    } else InterlockedIncrement64(&g_pw_missing);
    if (vs) glDeleteProgram(vs);
    if (fs && fs != empty_fs) glDeleteProgram(fs);
    while (glGetError()) {}
    return cold;
}
static void pw_job(ShJob *j, GLuint pipe, GLuint empty_fs) {
    if (j->stage == 2 && j->key[3] == 2 && pf_now() > g_pw_rewarm_end) return;      /* the start's re-warm time is up (PW_REWARM_S) */
    int c = pw_job_inner(j, pipe, empty_fs);
    if (j->stage == 2 && j->key[3] == 1) pw_sample_note(c);
}
static void pw_enqueue(ShJob *j) {
    j->next = NULL;
    EnterCriticalSection(&g_shw_lock);
    if (g_shw_tail) g_shw_tail->next = j; else g_shw_head = j;
    g_shw_tail = j;
    LeaveCriticalSection(&g_shw_lock);
    InterlockedIncrement64(&g_shw_queued);
    ReleaseSemaphore(g_shw_sem, 1, NULL);
}
/* Start-up (prec_load): a pair that a draw used in an earlier run. */
static void pw_queue_named(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask, int sample) {
    if (!pw_mode() || g_shd_mode <= 0 || !g_shw_n) return;
    ShJob *j = calloc(1, sizeof *j + 28);
    j->stage = 2; j->key[3] = (uint32_t)sample;
    memcpy(j->data, &vh, 8); memcpy(j->data + 8, &fh, 8); memcpy(j->data + 16, &vl, 4); memcpy(j->data + 20, &fl, 4); memcpy(j->data + 24, &mask, 4);
    g_pw_named++;
    pw_enqueue(j);
}
static volatile LONG64 g_pw_gate_t0;
/* boot.c: milliseconds presents were held for shader preparation so far (not counted by the run-time limit). */
unsigned nvn_gl_held_ms(void) {
    LARGE_INTEGER f;
    LONG64 t0 = g_pw_gate_t0, ticks = g_pw_gate_ticks + (t0 ? pf_now() - t0 : 0);
    QueryPerformanceFrequency(&f);
    return (unsigned)(ticks * 1000 / f.QuadPart);
}
/* Render thread, before a present: hold it while cold shader work is pending (see above). */
/* Asynchronous shaders (default; SSBU_SHADER_SYNC=1 = the old waits). A stage the driver has not compiled on this PC took
 * it 3 to 5 s (the large island and room shaders): the present was held that long (pw_gate) or the render thread compiled
 * it in the middle of a frame, and the game froze each time it showed something new. Now the stage goes to a worker and
 * the draws that need it are skipped until its binary is on disk (stage_glsl): the object appears a few seconds later
 * instead. A stage still missing after 20 s is compiled on the render thread as before. */
static int shader_async(void) { static int v = -1; if (v < 0) v = getenv("SSBU_SHADER_SYNC") == NULL; return v; }
static struct { uint64_t h1; LONG64 t0; char path[320]; } g_pend[512];
static int g_pend_n;
static LONG64 g_pend_skipped, g_pend_started, g_pend_late;
static void pw_gate(void) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("SSBU_PAIR_GATE"); off = (e && !strcmp(e, "0")) || !pw_mode() || shader_async(); }
    if (off || !g_pw_cold) return;
    if (g_shw_queued == g_shw_done || g_shw_ctx_failed) { InterlockedExchange(&g_pw_cold, 0); return; }
    LONG64 t0 = pf_now(), last = t0, freq, start_done = g_shw_done;
    char title[200] = "", text[300];
    g_pw_gate_t0 = t0;
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    HS_IN("shader preparation (present held while the workers prepare programs)", 0);
    while (g_shw_queued != g_shw_done) {
        Sleep(2);
        if (pf_now() - last > freq) {            /* once per second while it lasts: the log and the window title */
            if (!title[0] && !GetWindowTextA(g_hwnd, title, sizeof title)) strcpy(title, " ");
            last = pf_now();
            snprintf(text, sizeof text, "%s - preparing shaders: %lld to go", title, (long long)(g_shw_queued - g_shw_done));
            SetWindowTextA(g_hwnd, text);
            printf("nvn_gl: preparing shaders, present held for %.0f s so far: %lld jobs to go (%lld pairs made ready, %lld of them not in the driver's cache)%c", (double)(last - t0) / (double)freq,
                   (long long)(g_shw_queued - g_shw_done), (long long)g_pw_warmed, (long long)g_pw_cold_n, 10);
            fflush(stdout);
        }
    }
    HS_OUT();
    if (title[0]) SetWindowTextA(g_hwnd, title);
    InterlockedExchange(&g_pw_cold, 0);
    g_pw_gate_ticks += pf_now() - t0; g_pw_gates++;
    g_pw_gate_t0 = 0;
    printf("nvn_gl: shader preparation held the present of frame %lld for %.2f s (%lld jobs)%c", (long long)g_st.frames + 1, (double)(pf_now() - t0) / (double)freq, (long long)(g_shw_done - start_done), 10);
}
static DWORD WINAPI shd_worker(LPVOID arg) {
#ifndef _WIN32
    (void)arg; InterlockedIncrement(&g_shw_ctx_failed); return 0;      /* Linux build: no OpenGL renderer */
#else
    PIXELFORMATDESCRIPTOR pfd;
    (void)arg;
    HWND w = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 8, 8, NULL, NULL, NULL, NULL);
    HDC dc = w ? GetDC(w) : NULL;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    if (!dc || !SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd)) return 0;
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { InterlockedIncrement(&g_shw_ctx_failed); return 0; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    { void display_background_thread(void); display_background_thread(); }      /* display.c: off the game's cores */
    GLuint pw_pipe = 0, pw_empty = 0;                  /* P3 round 4: the pair jobs draw one discarded point through this pipeline */
    glEnable(GL_DEBUG_OUTPUT_); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_);      /* (no driver thread here: compiles and links answer every call, gl_driver_thread is for the render thread's draws) */
    {   static const char *const src[1] = {"#version 450\nvoid main() {}\n"};
        static const float zero[64];
        GLuint vb = 0;
        glGenProgramPipelines(1, &pw_pipe);
        glBindProgramPipeline(pw_pipe);
        glEnable(GL_RASTERIZER_DISCARD_);
        pw_empty = glCreateShaderProgramv(0x8B30, 1, src);
        glGenBuffers(1, &vb);                          /* every vertex array reads this small buffer of zeros */
        glBindBuffer(GL_ARRAY_BUFFER_, vb);
        glBufferData(GL_ARRAY_BUFFER_, (ptrdiff_t)sizeof zero, zero, GL_STREAM_DRAW_);
        glBindVertexBuffer(0, vb, 0, 16);
        for (GLuint k = 0; k < 16; k++) { glVertexAttribFormat(k, 4, 0x1406 /* FLOAT */, 0, 0); glVertexAttribBinding(k, 0); }
    }
    for (;;) {
        WaitForSingleObject(g_shw_sem, INFINITE);
        EnterCriticalSection(&g_shw_lock);
        ShJob *j = g_shw_head;
        if (j) { g_shw_head = j->next; if (!g_shw_head) g_shw_tail = NULL; }
        LeaveCriticalSection(&g_shw_lock);
        if (!j) continue;
        if (j->stage >= 2) {
            pw_job(j, pw_pipe, pw_empty);
            if (j->stage == 4) { InterlockedIncrement64(&g_shp_done); shp_finish_check(); }
            InterlockedIncrement64(&g_shw_done); free(j); continue;
        }
        MxIn in;
        MxOut out;
        memset(&in, 0, sizeof in);
        in.stage = j->stage; in.code = j->data; in.code_len = j->code_len;
        if (j->cdata_size) { in.cdata = j->data + j->code_len; in.cdata_size = j->cdata_size; }
        in.binding_base = (unsigned)j->stage * 20; in.ssbo_base = 40 + (unsigned)j->stage * 16; in.ubo = ubo_on(); in.ubo_base = (unsigned)j->stage * 18;
        in.int_attrs = j->key[0]; in.sint_attrs = j->key[1]; in.int_tex = j->key[2]; in.sint_tex = j->key[3];
        in.fragdebug = getenv("SSBU_FRAGDEBUG") != NULL; in.screen_scale = in.stage == 1 && rs_on(); in.wide = wide_k() != 1.0f ? 1 + g_wide_camera : 0;
        if (mx_translate(&in, &out)) {
            char path[320];
            size_t len = strlen(out.glsl);
            shd_path(path, sizeof path, sh_hash(0x9E3779B97F4A7C15ULL ^ (uint64_t)j->stage, out.glsl, len), len);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) InterlockedIncrement64(&g_shw_present);
            else {
                GLint ok = 0;
                int saved = 0;
                GLuint q = shd_compile_save(j->stage == 0 ? 0x8B31 : 0x8B30, out.glsl, path, &ok, &saved);
                if (q) glDeleteProgram(q);
                InterlockedExchange(&g_pw_cold, 1);            /* a compile: presents wait for the workers (pw_gate) */
                if (saved) InterlockedIncrement64(&g_shw_compiled); else InterlockedIncrement64(&g_shw_failed);
                while (glGetError()) {}
            }
            free(out.glsl);
        } else InterlockedIncrement64(&g_shw_failed);
        InterlockedIncrement64(&g_shw_done);
        free(j);
    }
#endif
}
static void shd_workers_start(void) {
    const char *e = getenv("SSBU_SHADER_WORKERS");
    g_shw_n = e ? atoi(e) : 3;
    if (g_shw_n < 0) g_shw_n = 0;
    if (g_shw_n > 8) g_shw_n = 8;
    InitializeCriticalSection(&g_shw_lock);
    for (int i = 0; i < 64; i++) InitializeCriticalSection(&g_pw_stripe[i]);
    if (!getenv("SSBU_NO_SHADER_VARIANTS")) shv_load();
    g_shw_sem = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL);
    for (int i = 0; i < g_shw_n; i++) { DWORD tid = 0; CloseHandle(CreateThread(NULL, 0, shd_worker, NULL, 0, &tid)); if (i < 8) g_shw_tid[i] = tid; }
}
/* ---- Shader preparation (2026-10-25): every shader of the game compiled before the title
 * scripts/shader_store.py takes every "BNSH" container out of the game files into ..\cache\shader_store (460 containers,
 * 14,676 variations, 6,049 distinct vertex / fragment stages: the 3D shader archive, effects, layouts, the exe's own). At
 * start, while the loading screen says "preparing shaders" (boot.c waits for the workers), every variation's vertex and
 * fragment stage is translated, compiled when its binary is not in the cache, and the pair gets the driver's first-draw
 * work (pw_job, stage 4): nothing is compiled while the game runs. A vertex stage that reads attribute 6 is also made with
 * that input as integers (skinned models' bone indices: the only integer key variants.txt ever recorded for a vertex
 * stage). Measured on the RTX 3060: a stage compile 81 ms, a cold pair 6 ms; the first preparation takes minutes, once.
 * Done once per store, shader-text settings (resolution scale, widescreen), driver and exe (name, size, time): the key of
 * the last complete one is in <cache>/prepared_<exe>.txt; a start with the same key queues nothing. TOMO_SHADER_PREP=0 = off,
 * TOMO_SHADER_STORE=<dir> = another store. Compute stages are left out (their text depends on the images bound). */
static volatile LONG g_shp_marked;
static int g_shp_store;                            /* the store is read: its preparation covers every pair (prec_load warms none at start) */
static uint64_t g_shp_key;
static char g_shp_mark[320];
/* The pairs earlier preparations had the driver do its first-draw work for, by this exe name (the driver keeps that work
 * in a cache of its own per exe name): <cache>/warmed_<exe>.txt, "<vs hash> <vs length> <fs hash> <fs length> <mask>".
 * A preparation after an update then warms only pairs whose text changed (all 8,243 again: 46 s, measured). */
static uint64_t g_shp_wset[1 << 17];
static char g_shp_wpath[320];
static volatile LONG64 g_shp_skipped;
static int shp_wput(uint64_t k, int add) {          /* g_shw_lock held: 1 when k was in the set */
    unsigned s = (unsigned)(k >> 20) & ((1u << 17) - 1);
    for (int probe = 0; probe < 1024; probe++, s = (s + 1) & ((1u << 17) - 1)) {
        if (g_shp_wset[s] == k) return 1;
        if (!g_shp_wset[s]) { if (add) g_shp_wset[s] = k; return 0; }
    }
    return 0;
}
static uint64_t shp_wkey(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask) { uint64_t w[3] = {vh ^ vl, fh ^ fl, mask}; return sh_hash(0x57524d44ULL, w, sizeof w) | 1; }
static int shp_warmed(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask, int add) {      /* worker */
    uint64_t k = shp_wkey(vh, vl, fh, fl, mask);
    EnterCriticalSection(&g_shw_lock);
    int found = shp_wput(k, add);
    if (add && !found && g_shp_wpath[0]) {
        FILE *f = fopen(g_shp_wpath, "a");
        if (f) { fprintf(f, "%016llx %x %016llx %x %x\n", (unsigned long long)vh, vl, (unsigned long long)fh, fl, mask); fclose(f); }
    }
    LeaveCriticalSection(&g_shw_lock);
    if (add || !found) return found;
    char path[320];                                /* and both binaries are still in the cache */
    shd_path(path, sizeof path, vh, vl);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 0;
    if (fl) { shd_path(path, sizeof path, fh, fl); if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 0; }
    InterlockedIncrement64(&g_shp_skipped);
    return 1;
}
static char *shp_read(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = len > 0 ? malloc((size_t)len + 1) : NULL;
    if (b && fread(b, 1, (size_t)len, f) == (size_t)len) { b[len] = 0; *n = (size_t)len; } else { free(b); b = NULL; }
    fclose(f);
    return b;
}
static void shp_finish_check(void) {                /* any thread: the last job of a preparation writes its key */
    if (g_shp_queueing || !g_shp_total || g_shp_done < g_shp_total || InterlockedExchange(&g_shp_marked, 1)) return;
    FILE *f = fopen(g_shp_mark, "w");
    if (f) { fprintf(f, "%016llx\n", (unsigned long long)g_shp_key); fclose(f); }
    LARGE_INTEGER q; QueryPerformanceFrequency(&q);
    printf("nvn_gl: shader preparation done: %lld variations in %.1f s (%lld stages compiled, %lld cold pairs, %lld warmed before, %lld not translated)%c", (long long)g_shp_total,
           (double)(pf_now() - g_shp_t0) / (double)q.QuadPart, (long long)g_pw_compiled, (long long)g_pw_cold_n, (long long)g_shp_skipped, (long long)g_pw_missing, 10);
    fflush(stdout);
}
static void shp_variant(const ShJob *j, uint32_t int_attrs) {      /* worker: the same variation once more with other vertex inputs */
    size_t n = (size_t)j->code_len + j->cdata_size + j->key[0] + j->key[1];
    ShJob *v = malloc(sizeof *v + n);
    memcpy(v, j, sizeof *v + n);
    v->key[2] = int_attrs;
    InterlockedIncrement64(&g_shp_total);
    pw_enqueue(v);
}
static int shp_stage(const uint8_t *c, size_t size, uint64_t control, uint64_t data, const uint8_t **code, int *len, const uint8_t **cd, uint32_t *cds) {
    static const int8_t stage_of_type[16] = {-1, 0, 3, 4, 2, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t v;
    if (control + 0x760 > size || data + 0x40 > size) return -1;
    memcpy(&v, c + control, 4); if (v != 0x98761234u) return -1;
    memcpy(&v, c + data, 4); if (v != 0x12345678u) return -1;
    memcpy(&v, c + control + 0x714, 4); if (v == 5) return -1;      /* compute */
    memcpy(&v, c + data + 0x30, 4);
    int s = stage_of_type[(v >> 10) & 15];
    uint32_t ctl[8];
    memcpy(ctl, c + control + 0x6f0, 32);
    if (s < 0 || s > 1 || !ctl[2] || ctl[2] > (4u << 20) || data + 0x30 + ctl[2] > size) return -1;
    *code = c + data + 0x30; *len = (int)ctl[2]; *cd = NULL; *cds = 0;
    if (ctl[3] && ctl[4] && data + ctl[4] + ctl[3] <= size) { *cd = c + data + ctl[4]; *cds = ctl[3]; }
    return s;
}
static void shp_queue(void) {                      /* render thread, start (prec_load), before the start-up queueing is declared over */
    const char *e = getenv("TOMO_SHADER_PREP"), *dir = getenv("TOMO_SHADER_STORE");
    char path[320], exe[MAX_PATH] = "";
    size_t n = 0;
    if ((e && e[0] == '0') || !pw_mode() || g_shd_mode <= 0 || !g_shw_n || g_shw_ctx_failed) return;
    if (!dir || !dir[0]) dir = "../cache/shader_store";
    snprintf(path, sizeof path, "%s/store.txt", dir);
    char *txt = shp_read(path, &n);
    if (!txt) { printf("nvn_gl: shader preparation: no store at %s (scripts/shader_store.py makes it): shaders are compiled when first needed%c", dir, 10); return; }
    g_shp_store = 1;
    {   WIN32_FILE_ATTRIBUTE_DATA fa;
        char key[512];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        memset(&fa, 0, sizeof fa);
        GetFileAttributesExA(exe, GetFileExInfoStandard, &fa);
        const char *base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;
        int kl = snprintf(key, sizeof key, "%s %lu %lu %lu %d %d %d %016llx%s", base, fa.nFileSizeLow, fa.ftLastWriteTime.dwLowDateTime, fa.ftLastWriteTime.dwHighDateTime,
                          ubo_on(), rs_on(), wide_k() != 1.0f ? 1 + g_wide_camera : 0, (unsigned long long)g_shd_drv, mx_bl_misslog() ? " blmiss" : "");
        g_shp_key = sh_hash(sh_hash(0x5348505245ULL, txt, n), key, (size_t)kl);
        snprintf(g_shp_mark, sizeof g_shp_mark, "%s/prepared_%s.txt", g_shd_dir, base);
        /* 2026-10-30: by driver too. The driver's own cache of the pairs' first-draw work is per driver version: after a driver
         * update every pair is cold again, and this file said they were warm (the 2026-10-29 session: 651 first draws of
         * 850 pairs stalled, 26 ms on average, nothing re-warmed). */
        snprintf(g_shp_wpath, sizeof g_shp_wpath, "%s/warmed_%s_%08x.txt", g_shd_dir, base, (unsigned)(g_shd_drv >> 32 ^ g_shd_drv));
    }
    {   size_t ml = 0;
        char *m = shp_read(g_shp_mark, &ml);
        unsigned long long k = 0;
        int same = m && sscanf(m, "%llx", &k) == 1 && k == g_shp_key;
        free(m);
        if (same) { printf("nvn_gl: shader preparation: every shader of the store was made ready before (%s)%c", g_shp_mark, 10); free(txt); return; }
    }
    {   size_t wl = 0;                             /* the pairs warmed by earlier preparations */
        char *w = shp_read(g_shp_wpath, &wl);
        int nw = 0;
        EnterCriticalSection(&g_shw_lock);
        for (char *l = w; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
            unsigned long long vh, fh; unsigned vl, fl, mask;
            if (sscanf(l, "%llx %x %llx %x %x", &vh, &vl, &fh, &fl, &mask) == 5) nw += !shp_wput(shp_wkey(vh, vl, fh, fl, mask), 1);
        }
        LeaveCriticalSection(&g_shw_lock);
        free(w);
        if (nw) printf("nvn_gl: shader preparation: %d pairs were warmed before (%s)%c", nw, g_shp_wpath, 10);
    }
    static uint64_t seen[1 << 15];
    int ncont = 0, nvar = 0, ndup = 0;
    g_shp_t0 = pf_now();
    InterlockedExchange(&g_shp_queueing, 1);
    for (char *line = txt, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char name[40];
        if (sscanf(line, "%32s", name) != 1) continue;
        snprintf(path, sizeof path, "%s/%s.bnsh", dir, name);
        size_t size = 0;
        uint8_t *c = (uint8_t *)shp_read(path, &size);
        if (!c) continue;
        uint32_t w;
        uint64_t blk, vars;
        memcpy(&w, c + 0x14, 4); blk = w >> 16;
        if (size < 0x60 || blk + 0x28 > size || memcmp(c + blk, "grsc", 4)) { free(c); continue; }
        int cnt;
        memcpy(&cnt, c + blk + 0x1C, 4); memcpy(&vars, c + blk + 0x20, 8);
        if (cnt <= 0 || vars + (uint64_t)cnt * 0x40 > size) { free(c); continue; }
        ncont++;
        for (int vi = 0; vi < cnt; vi++) {
            uint64_t prog;
            memcpy(&prog, c + vars + (uint64_t)vi * 0x40 + 0x10, 8);      /* the binary program (file offsets: not relocated) */
            if (!prog || prog + 0x70 > size) continue;
            const uint8_t *code[2] = {NULL, NULL}, *cd[2] = {NULL, NULL};
            int len[2] = {0, 0};
            uint32_t cds[2] = {0, 0};
            for (int sl = 0; sl < 6; sl++) {
                uint64_t sc, control, data;
                memcpy(&sc, c + prog + 8 + (uint64_t)sl * 8, 8);
                if (!sc || sc + 0x20 > size) continue;
                memcpy(&control, c + sc + 8, 8); memcpy(&data, c + sc + 16, 8);
                const uint8_t *k; const uint8_t *kd; int kl; uint32_t kds;
                int s = shp_stage(c, size, control, data, &k, &kl, &kd, &kds);
                if (s >= 0) { code[s] = k; len[s] = kl; cd[s] = kd; cds[s] = kds; }
            }
            if (!code[0]) continue;
            size_t need = (size_t)len[0] + cds[0] + (size_t)len[1] + cds[1];
            ShJob *j = calloc(1, sizeof *j + need);
            uint8_t *d = j->data;
            j->stage = 4; j->code_len = len[0]; j->cdata_size = cds[0]; j->key[0] = (uint32_t)len[1]; j->key[1] = cds[1];
            memcpy(d, code[0], (size_t)len[0]); d += len[0];
            if (cds[0]) { memcpy(d, cd[0], cds[0]); d += cds[0]; }
            if (len[1]) { memcpy(d, code[1], (size_t)len[1]); d += len[1]; if (cds[1]) memcpy(d, cd[1], cds[1]); }
            uint64_t h = sh_hash(0x5348505ULL ^ (uint64_t)len[0] ^ ((uint64_t)len[1] << 32), j->data, need) | 1;
            unsigned slot = (unsigned)(h >> 20) & ((1u << 15) - 1);
            int dup = 0;
            for (int probe = 0; probe < 256; probe++, slot = (slot + 1) & ((1u << 15) - 1)) { if (seen[slot] == h) { dup = 1; break; } if (!seen[slot]) { seen[slot] = h; break; } }
            if (dup) { free(j); ndup++; continue; }
            nvar++;
            InterlockedIncrement64(&g_shp_total);
            pw_enqueue(j);
        }
        free(c);
    }
    free(txt);
    if (nvar) {                                    /* while the loading screen waits, more workers than in play */
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int want = (int)si.dwNumberOfProcessors - 2;
        if (want > 12) want = 12;
        if (getenv("SSBU_SHADER_WORKERS")) want = g_shw_n;      /* (the count asked for, as for the run-time workers) */
        for (; g_shw_n < want; g_shw_n++) { DWORD tid = 0; CloseHandle(CreateThread(NULL, 0, shd_worker, NULL, 0, &tid)); if (g_shw_n < 8) g_shw_tid[g_shw_n] = tid; }
    }
    printf("nvn_gl: shader preparation: %d variations of %d containers queued (%d repeated), %d workers%c", nvar, ncont, ndup, g_shw_n, 10);
    InterlockedExchange(&g_shp_queueing, 0);
    shp_finish_check();
}
/* Guest thread, nvnProgramSetShaders: a vertex or fragment stage given as machine code. */
static void shd_prewarm(int stage, const uint8_t *code, int code_len, const uint8_t *cdata, uint32_t cdata_size) {
    if (g_shd_mode <= 0 || !g_shw_n || stage > 1 || code_len <= 0) return;
    uint64_t h = sh_hash(0xcbf29ce484222325ULL ^ 0x20261023ULL ^ (uint64_t)stage, code, (size_t)code_len);
    if (cdata) h = sh_hash(h ^ cdata_size, cdata, cdata_size);
    if (!h) h = 1;
    ShJob *j = malloc(sizeof *j + (size_t)code_len + cdata_size);
    j->next = NULL; j->stage = stage; j->code_len = code_len; j->cdata_size = cdata ? cdata_size : 0;
    memset(j->key, 0, sizeof j->key);
    memcpy(j->data, code, (size_t)code_len);
    if (cdata) memcpy(j->data + code_len, cdata, cdata_size);
    EnterCriticalSection(&g_shw_lock);
    unsigned k = (unsigned)(h >> 20) & 16383;
    int seen = 0, extra = 0;
    for (int probe = 0; probe < 64; probe++, k = (k + 1) & 16383) { if (g_shw_seen[k] == h) { seen = 1; break; } if (!g_shw_seen[k]) { g_shw_seen[k] = h; break; } }
    if (!seen) {
        if (g_shw_tail) g_shw_tail->next = j; else g_shw_head = j;
        g_shw_tail = j;
        for (ShVar *e = g_shv[h & 4095]; e; e = e->next) {         /* the variants recorded in earlier runs (variants.txt) */
            if (e->h != h || e->stage != stage) continue;
            ShJob *v = malloc(sizeof *v + (size_t)code_len + cdata_size);
            memcpy(v, j, sizeof *v + (size_t)code_len + (cdata ? cdata_size : 0));
            v->next = NULL; memcpy(v->key, e->key, 16);
            g_shw_tail->next = v; g_shw_tail = v;
            extra++;
        }
    }
    LeaveCriticalSection(&g_shw_lock);
    if (seen) { free(j); return; }
    InterlockedExchangeAdd64(&g_shw_queued, 1 + extra);
    g_shv_queued += extra;
    ReleaseSemaphore(g_shw_sem, 1 + extra, NULL);
    /* P3 round 2: nvnProgramSetShaders holds its caller while every worker is busy compiling (not during start-up, where
     * the game hands over 6,800 stages at once and nothing of them is drawn yet). A stage or fighter loaded later brings
     * stages that were never compiled on this PC; without this the game finished loading while the workers were still
     * compiling, and the first draws compiled the same stages again on the render thread, in the middle of the battle
     * (7 vertex stages, 5.3 s, tests/logs/p3b_r2a.log). On the console the program is ready when this call returns; here
     * the loading takes as long as the compiler needs, once per stage per PC. SSBU_SHADER_NOWAIT=1 = never hold. */
    {
        static int nowait = -1;
        if (nowait < 0) nowait = getenv("SSBU_SHADER_NOWAIT") != NULL;
        if (!nowait && !shader_async() && g_st.frames >= 300 && GetCurrentThreadId() != g_render_tid && g_shw_queued - g_shw_done >= g_shw_n) {
            LONG64 t0 = pf_now(), freq = 0;
            { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
            while (g_shw_queued - g_shw_done >= g_shw_n && pf_now() - t0 < 30 * freq) Sleep(1);
            InterlockedExchangeAdd64(&g_shw_held_ticks, pf_now() - t0); InterlockedIncrement64(&g_shw_held);
        }
    }
}
/* ---- P3 round 3: linked program pairs (SSBU_PAIR=1; default off until measured)
 * The two stages of a draw are separable programs in one pipeline object, and this driver does its last piece of work
 * for a (vertex, fragment) combination inside the first draw call that uses it (50-90 ms; it keeps the result in a
 * cache of its own that depends on the name of the executable). Here the same two TEXTS are also linked into one
 * program; its binary goes into our cache folder under the hash of both texts (file <hash>_<total length>_<driver>.bin,
 * same folder and driver key as the stage binaries, nothing in it depends on the executable). A draw uses the linked
 * program when it is there (same shaders, same bindings: every binding is fixed by a layout qualifier in the text; the
 * only uniform, the alpha test at location 0, is set on whichever program is in use); when the binary is not on disk
 * yet the draw goes through the pipeline exactly as before and the pair is linked for the next time. */
static int pair_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("SSBU_PAIR"); m = e ? atoi(e) : 0; } return m; }
typedef struct { uint64_t key; GLuint prog; int state; } PairEnt;      /* state 0 = free, 1 = ready, 2 = no linked program (pipeline) */
static PairEnt g_pairs[16384];
static LONG64 g_pair_loaded, g_pair_linked, g_pair_failed, g_pair_draws, g_pair_refused;
static void pair_off(void) { if (g_pair_cur) { glUseProgram(0); g_pair_cur = 0; } }
static GLuint pair_link(const char *vs, const char *fs, const char *path, int *saved) {
    GLint ok = 0, n = 0;
    GLuint sh[2] = {glCreateShader(0x8B31), glCreateShader(0x8B30)}, q = glCreateProgram();
    const char *src[2] = {vs, fs};
    *saved = 0;
    for (int i = 0; i < 2; i++) { glShaderSource(sh[i], 1, &src[i], NULL); glCompileShader(sh[i]); glAttachShader(q, sh[i]); }
    glProgramParameteri(q, 0x8257 /* PROGRAM_BINARY_RETRIEVABLE_HINT */, 1);
    glLinkProgram(q);
    for (int i = 0; i < 2; i++) { glDetachShader(q, sh[i]); glDeleteShader(sh[i]); }
    glGetProgramiv(q, 0x8B82, &ok);
    if (!ok) {
        static int nlog;
        char log[600] = "";
        glGetProgramInfoLog(q, sizeof log - 1, NULL, log);
        logf_limited(&nlog, 6, "nvn_gl: pair program not linked (the pipeline is used for this pair): %s\n", log);
        glDeleteProgram(q);
        return 0;
    }
    glGetProgramiv(q, 0x8741 /* PROGRAM_BINARY_LENGTH */, &n);
    if (n > 0) {
        char tmp[360];
        uint32_t head[2] = {0, 0};
        GLsizei got = 0;
        GLenum fmt = 0;
        void *data = malloc((size_t)n);
        while (glGetError()) {}
        glGetProgramBinary(q, n, &got, &fmt, data);
        if (!glGetError() && got > 0) {
            snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId());
            FILE *f = fopen(tmp, "wb");
            if (f) {
                head[0] = fmt; head[1] = (uint32_t)got;
                int good = fwrite(head, 4, 2, f) == 2 && fwrite(data, 1, (size_t)got, f) == (size_t)got;
                fclose(f);
                if (good && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) *saved = 1; else DeleteFileA(tmp);
            }
        }
        free(data);
    }
    return q;
}
/* Render thread, at a draw: the linked program for these two stage programs, 0 = use the pipeline. */
static GLuint pair_get(GLuint vs, GLuint fs, const char *fs_text_fixed) {
    uint64_t key = ((uint64_t)vs << 32) | fs;
    unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ULL) >> 50);
    PairEnt *e = NULL;
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 16383) { if (g_pairs[h].state && g_pairs[h].key == key) return g_pairs[h].prog; if (!g_pairs[h].state) { e = &g_pairs[h]; break; } }
    if (!e || g_shd_mode <= 0) return 0;
    e->key = key; e->state = 2; e->prog = 0;
    ShSrc *a = vs < 65536 ? g_prog_src[vs] : NULL, *b = fs_text_fixed ? NULL : fs < 65536 ? g_prog_src[fs] : NULL;
    const char *ft = fs_text_fixed ? fs_text_fixed : b ? b->text : NULL;
    if (!a || !a->text || !ft) return 0;
    size_t flen = strlen(ft);
    char path[320];
    uint64_t ph = sh_hash(sh_hash(0x50414953ULL, a->text, a->len) ^ a->len, ft, flen);
    shd_path(path, sizeof path, ph, a->len + flen);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint32_t head[2] = {0, 0};
        if (fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < (64u << 20)) {
            void *data = malloc(head[1]);
            if (fread(data, 1, head[1], f) == head[1]) {
                GLint ok = 0;
                GLuint q = glCreateProgram();
                while (glGetError()) {}
                HS_IN("pair program binary from disk (glProgramBinary)", 0);
                glProgramBinary(q, head[0], data, (GLsizei)head[1]);
                glGetProgramiv(q, 0x8B82, &ok);
                HS_OUT();
                while (glGetError()) {}
                if (ok) { e->prog = q; e->state = 1; g_pair_loaded++; } else { glDeleteProgram(q); g_pair_refused++; }
            }
            free(data);
        }
        fclose(f);
        if (e->state == 1) return e->prog;
    }
    if (pair_mode() == 1) {            /* 1: link here, at the first draw (measurement of the idea); 2: only use what is on disk */
        int saved = 0;
        /* Both stage texts name their buffer blocks CB<n> / SB<n> (mx_translate.c) with different bindings; in ONE program
         * blocks of the same name are the same block, so the fragment stage's are renamed FB<n> / TB<n> for the link
         * (block names are not used anywhere else in the text; the members c<n> / s<n> belong to each stage's own block). */
        char *ft2 = _strdup(ft);
        for (char *p = ft2; (p = strstr(p, " buffer CB")) != NULL; p += 9) p[8] = 'F';
        for (char *p = ft2; (p = strstr(p, " buffer SB")) != NULL; p += 9) p[8] = 'T';
        /* ... and so are their members: the translator writes them only as c<n>[ and s<n>[ (whole identifiers), and no
         * identifier of the text starts with K or Q; an unnamed block's member is a global name of the whole program. */
        for (char *p = ft2 + 1; *p; p++) {
            if ((*p != 'c' && *p != 's') || p[1] < '0' || p[1] > '9') continue;
            char b = p[-1];
            if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_' || b == '.') continue;
            char *e = p + 1;
            while (*e >= '0' && *e <= '9') e++;
            if (*e == '[') *p = *p == 'c' ? 'K' : 'Q';
        }
        HS_IN("pair program compile + link (driver)", 0);
        GLuint q = pair_link(a->text, ft2, path, &saved);
        HS_OUT();
        free(ft2);
        while (glGetError()) {}
        if (q) { e->prog = q; e->state = 1; g_pair_linked++; } else g_pair_failed++;
    }
    return e->prog;
}
/* ---- P3 round 4: the list of shader pairs that were drawn (<cache>/pairs.txt)
 * One line per (vertex text, fragment text, enabled arrays) combination a draw used: "<vs hash> <vs length> <fs hash>
 * <fs length> <mask>": the names the two stage binaries have in the cache folder (fragment 0 0 = the empty fragment
 * shader of a depth-only pass) and which of the vertex attribute arrays the vertex stage reads were enabled (the
 * driver's code for a pair depends on exactly that, p4lab mode M). Read at start (prec_load), appended to when a draw
 * uses a combination that is not in it. */
typedef struct PairRec { uint64_t vh, fh; uint32_t vl, fl, mask; struct PairRec *next; } PairRec;
static PairRec *g_prec[4096];
static LONG64 g_prec_loaded, g_prec_added;
static int g_prec_off = -1;
static int prec_has(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask, int add) {
    unsigned k = (unsigned)((vh ^ fh * 0x9E3779B97F4A7C15ULL) >> 20) & 4095;
    for (PairRec *e = g_prec[k]; e; e = e->next) if (e->vh == vh && e->fh == fh && e->vl == vl && e->fl == fl && e->mask == mask) return 1;
    if (add) { PairRec *e = calloc(1, sizeof *e); e->vh = vh; e->vl = vl; e->fh = fh; e->fl = fl; e->mask = mask; e->next = g_prec[k]; g_prec[k] = e; }
    return 0;
}
/* Render thread, at a draw through the pipeline: note the pair of GL stage programs (fs_empty: depth-only pass). */
static void prec_note(GLuint vs, GLuint fs, int fs_empty, uint32_t mask) {
    static uint64_t seen[1 << 15];
    if (g_prec_off < 0) g_prec_off = getenv("SSBU_NO_PAIR_LIST") != NULL || g_shd_mode <= 0;
    if (g_prec_off || fs >= 65536) return;
    uint64_t pair = ((uint64_t)vs << 32) | (uint64_t)(mask & 0xffff) << 16 | fs | 1ull << 63;
    unsigned h = (unsigned)((pair * 0x9E3779B97F4A7C15ULL) >> 49);
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 0x7fff) { if (seen[h] == pair) return; if (!seen[h]) { seen[h] = pair; break; } }
    ShSrc *a = vs < 65536 ? g_prog_src[vs] : NULL, *b = fs_empty ? NULL : fs < 65536 ? g_prog_src[fs] : NULL;
    if (!a || (!b && !fs_empty)) return;
    if (prec_has(a->h, (uint32_t)a->len, b ? b->h : 0, b ? (uint32_t)b->len : 0, mask, 1)) return;
    char path[260];
    snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx %x %016llx %x %x\n", (unsigned long long)a->h, (unsigned)a->len, (unsigned long long)(b ? b->h : 0), b ? (unsigned)b->len : 0, (unsigned)mask); fclose(f); g_prec_added++; }
}
/* Render thread: a compute program was built (its first dispatch follows). List line: <hash> <length> 0 0 10000. */
static void prec_note_compute(uint64_t h, size_t len) {
    char path[260];
    if (g_prec_off < 0) g_prec_off = getenv("SSBU_NO_PAIR_LIST") != NULL || g_shd_mode <= 0;
    if (g_prec_off || prec_has(h, (uint32_t)len, 0, 0, 0x10000, 1)) return;
    snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx %x %016llx %x %x\n", (unsigned long long)h, (unsigned)len, 0ull, 0u, 0x10000u); fclose(f); g_prec_added++; }
}
/* The stage programs of the listed pairs, loaded from their binaries before their first draw. Until 2026-10-25 the render
 * thread did that before the game started: 5.5 s of the start, 13 s while the workers warmed pairs, and with the files out
 * of Windows' cache the reading alone took 26 s (4,645 small files; measured: 0.35 s of reading and 3 s of glProgramBinary
 * when cached, the driver serializes glProgramBinary across contexts). Now a reader thread reads them in the list's order
 * (the order they were first drawn) at background I/O priority, and the render thread makes the programs whenever it has
 * no job, a couple of ms at a time (render_loop -> pre_idle); a stage drawn before its turn is loaded by that draw, as
 * before. SSBU_PAIR_PRELOAD=0 = none. */
typedef struct PreBuf { struct PreBuf *next; uint64_t h; uint32_t len, fmt, size; uint8_t data[1]; } PreBuf;
typedef struct { uint64_t h; uint32_t len; } PreItem;
static PreItem *g_pre_items;
static int g_pre_count, g_pre_cap, g_pre_said;
static PreBuf *g_pre_head, *g_pre_tail;
static CRITICAL_SECTION g_pre_lock;
static volatile LONG64 g_pre_bytes;
static volatile LONG g_pre_reading;
static LONG64 g_pre_n, g_pre_ticks, g_pre_t0;
static void pre_add(int stage, uint64_t h, uint32_t len) {      /* render thread, prec_load: each binary once (pairs share stages) */
    static uint64_t seen[1 << 15];
    uint64_t k = (h ^ len) | 1;
    unsigned s = (unsigned)(k >> 20) & ((1u << 15) - 1);
    (void)stage;
    if (!len) return;
    for (int probe = 0; probe < 1024; probe++, s = (s + 1) & ((1u << 15) - 1)) { if (seen[s] == k) return; if (!seen[s]) { seen[s] = k; break; } }
    if (g_pre_count == g_pre_cap) { g_pre_cap = g_pre_cap ? g_pre_cap * 2 : 8192; g_pre_items = realloc(g_pre_items, (size_t)g_pre_cap * sizeof *g_pre_items); }
    g_pre_items[g_pre_count].h = h; g_pre_items[g_pre_count++].len = len;
}
static DWORD WINAPI pre_reader(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);      /* the game's own loading reads first */
    for (int i = 0; i < g_pre_count; i++) {
        char path[320];
        uint32_t head[2] = {0, 0};
        PreBuf *b = NULL;
        while (g_pre_bytes > (64 << 20)) Sleep(2);
        shd_path(path, sizeof path, g_pre_items[i].h, g_pre_items[i].len);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        if (fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < (64u << 20) && (b = malloc(sizeof *b + head[1])) && fread(b->data, 1, head[1], f) == head[1]) {
            b->next = NULL; b->h = g_pre_items[i].h; b->len = g_pre_items[i].len; b->fmt = head[0]; b->size = head[1];
            InterlockedAdd64(&g_pre_bytes, head[1]);
            EnterCriticalSection(&g_pre_lock);
            if (g_pre_tail) g_pre_tail->next = b; else g_pre_head = b;
            g_pre_tail = b;
            LeaveCriticalSection(&g_pre_lock);
        } else free(b);
        fclose(f);
    }
    InterlockedExchange(&g_pre_reading, 0);
    return 0;
}
static int pre_pending(void) { return g_pre_reading || g_pre_head; }
/* With NVIDIA's GL thread (gl_driver_thread) these calls return before the driver has done the work, and each status query
 * waits for everything queued before it: unbounded, a 2 ms slice could queue far more work than 2 ms ahead of the next
 * frame's commands (5,465 binaries at every start). There a slice loads at most 3 and asks for their link status once at
 * its end (the one wait = the slice's own work, as without the driver's thread); a binary the driver refused is taken out
 * again before any draw could use it (draws run after this returns). */
static void pre_idle(void) {                       /* render thread, no job queued: programs from the binaries read, ~2 ms */
    static LONG64 freq;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    LONG64 t0 = pf_now();
    ShSrc *chk[3];
    int nchk = 0;
    while (pf_now() - t0 < freq / 500 && !g_job_head && nchk < (g_gl_thr_on ? 3 : 1)) {
        EnterCriticalSection(&g_pre_lock);
        PreBuf *b = g_pre_head;
        if (b) { g_pre_head = b->next; if (!g_pre_head) g_pre_tail = NULL; }
        LeaveCriticalSection(&g_pre_lock);
        if (!b) break;
        InterlockedAdd64(&g_pre_bytes, -(LONG64)b->size);
        int have = 0;
        for (ShSrc *e = g_shs[b->h & 4095]; e && !have; e = e->next) have = e->h == b->h && e->len == b->len;
        if (!have) {
            GLint ok = 1;
            GLuint q = glCreateProgram();
            glProgramParameteri(q, 0x8258 /* PROGRAM_SEPARABLE */, 1);
            glProgramBinary(q, b->fmt, b->data, (GLsizei)b->size);
            if (!g_gl_thr_on) { glGetProgramiv(q, 0x8B82, &ok); while (glGetError()) {} }
            if (ok) {
                ShSrc *e = calloc(1, sizeof *e);
                e->h = b->h; e->len = b->len; e->prog = q; e->next = g_shs[b->h & 4095]; g_shs[b->h & 4095] = e;
                if (q < 65536) g_prog_src[q] = e;
                g_pre_n++; g_shd_loaded++;
                if (g_gl_thr_on) chk[nchk++] = e;
            } else glDeleteProgram(q);
        }
        free(b);
        if (!g_gl_thr_on) nchk = 0;
    }
    for (int i = 0; i < nchk; i++) {               /* (driver's thread) the slice's binaries: refused ones out again */
        GLint ok = 0;
        ShSrc *e = chk[i], **pp = &g_shs[e->h & 4095];
        glGetProgramiv(e->prog, 0x8B82, &ok);
        if (ok) continue;
        while (*pp && *pp != e) pp = &(*pp)->next;
        if (*pp) *pp = e->next;
        if (e->prog < 65536 && g_prog_src[e->prog] == e) g_prog_src[e->prog] = NULL;
        glDeleteProgram(e->prog); free(e);
        g_pre_n--; g_shd_loaded--;
    }
    g_pre_ticks += pf_now() - t0;
    if (!pre_pending() && !g_pre_said) {
        g_pre_said = 1;
        printf("nvn_gl: %lld stage binaries of the pair list loaded in the render thread's idle time (%.0f ms of it), the last %.1f s after the start%c", (long long)g_pre_n,
               (double)g_pre_ticks * 1000.0 / (double)freq, (double)(pf_now() - g_pre_t0) / (double)freq, 10);
    }
}
static void pre_start(void) {                      /* render thread, end of prec_load */
    const char *e = getenv("SSBU_PAIR_PRELOAD");
    if (!g_pre_count || (e && !strcmp(e, "0")) || g_shc_off > 0 || getenv("SSBU_NO_SHADERCACHE")) return;
    InitializeCriticalSection(&g_pre_lock);
    g_pre_t0 = pf_now();
    InterlockedExchange(&g_pre_reading, 1);
    HANDLE t = CreateThread(NULL, 0, pre_reader, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_pre_reading, 0);
}
static void prec_load(void) {                      /* render thread, once (shd_init) */
    char path[260], line[200];
    if (g_prec_off < 0) g_prec_off = getenv("SSBU_NO_PAIR_LIST") != NULL || g_shd_mode <= 0;
    if (g_prec_off) return;
    shp_queue();
    snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
    FILE *f = fopen(path, "r");
    if (!f) { printf("nvn_gl: no pair list at %s (first run here, or the exe was not started from build/work)%c", path, 10); InterlockedExchange(&g_pw_listed_done, 1); return; }
    while (fgets(line, sizeof line, f)) {
        unsigned long long vh, fh; unsigned vl, fl, mask;
        if (sscanf(line, "%llx %x %llx %x %x", &vh, &vl, &fh, &fl, &mask) == 5 && !prec_has(vh, vl, fh, fl, mask, 1)) {
            g_prec_loaded++;
            /* With the shader store in use its preparation had the driver do every pair's work, which the driver keeps on
             * disk: warming the listed pairs again at each start (14 s of driver time, the start waited for it) changed
             * nothing measurable in play (first draws: 2 over 4 ms instead of 1, 17 ms in all; 2026-10-25). */
            {   static int rewarm = -1;      /* TOMO_PAIR_REWARM=1: the whole list at every start (see PW_SAMPLE) */
                if (rewarm < 0) rewarm = getenv("TOMO_PAIR_REWARM") != NULL;
                if (!g_shp_store || rewarm) pw_queue_named(vh, vl, fh, fl, mask, 0);
                else if (g_pw_sample_n < PW_SAMPLE) { pw_queue_named(vh, vl, fh, fl, mask, 1); g_pw_sample_n++; }
                else {
                    if (g_pw_rest_n == g_pw_rest_cap) { g_pw_rest_cap = g_pw_rest_cap ? g_pw_rest_cap * 2 : 1024; g_pw_rest = realloc(g_pw_rest, (size_t)g_pw_rest_cap * sizeof *g_pw_rest); }
                    PwPair *q = &g_pw_rest[g_pw_rest_n++]; q->vh = vh; q->fh = fh; q->vl = vl; q->fl = fl; q->mask = mask;
                } }
            if (!(mask & 0x10000)) { pre_add(0, vh, vl); pre_add(1, fh, fl); }
        }
    }
    fclose(f);
    pw_progs_load();
    { LARGE_INTEGER q; QueryPerformanceFrequency(&q); printf("nvn_gl: %lld programs known by their machine code (progs.txt)%c", (long long)g_pw_progs_known, 10); }
    printf("nvn_gl: %lld listed shader pairs%s; %d stage binaries to load while the game starts%c", (long long)g_prec_loaded, g_shp_store ? " (not warmed again: the shader store's preparation did that)" : " queued for the workers", g_pre_count, 10);
    pre_start();
    InterlockedExchange(&g_pw_listed_done, 1);
}
static int stage_glsl(Prog *p, int s, const uint32_t *key) {
    static int nfail, nlink;
    Stage *st = &p->st[s];
    MxIn in;
    MxOut out;
    if (!st->code || s > 1) return 0;
    if (!st->dumped) { st->dumped = 1; g_st.binary_used++; dump_binary(p, s); }
    if (st->glsl_state == 2) return 0;
    uint32_t dep_a = s == 0 ? st->glsl_dep : 0;
    if (st->glsl_state == 1 && ((key[0] ^ st->glsl_key[0]) & dep_a) == 0 && ((key[1] ^ st->glsl_key[1]) & dep_a) == 0 &&
        ((key[2] ^ st->glsl_key[2]) & st->glsl_tex) == 0 && ((key[3] ^ st->glsl_key[3]) & st->glsl_tex) == 0) return 1;
    if (g_shc_off < 0) g_shc_off = getenv("SSBU_NO_SHADERCACHE") != NULL;
    if (st->glsl && g_shc_off) glDeleteProgram(st->glsl);
    st->glsl = 0;
    memset(&in, 0, sizeof in);
    in.stage = s; in.code = st->code; in.code_len = st->code_len;
    if (st->cdata_copy) { in.cdata = st->cdata_copy; in.cdata_size = st->ctl[3]; }
    uint64_t h1 = 0, h0 = 0;
    if (!g_shc_off) {
        h1 = sh_hash(0xcbf29ce484222325ULL ^ 0x20261023ULL ^ (uint64_t)s, st->code, (size_t)st->code_len);
        if (in.cdata) h1 = sh_hash(h1 ^ in.cdata_size, in.cdata, in.cdata_size);
        h0 = h1 ? h1 : 1;                           /* the workers' name for this stage (shd_prewarm) */
        h1 = sh_hash(h1, key, 16);
        for (ShEnt *e = g_shc[h1 & 4095]; e; e = e->next)
            if (e->h == h1 && e->stage == s && e->code_len == st->code_len && !memcmp(e->key, key, 16)) {
                st->glsl = e->prog; st->glsl_state = 1;
                memcpy(st->glsl_key, key, sizeof st->glsl_key);
                st->glsl_cbufs = e->cbufs; st->glsl_ssbos = e->ssbos; st->glsl_dep = e->in_attr; st->glsl_tex = e->tex_units; st->glsl_alpha = e->uses_alpha; st->glsl_fcs = e->uses_fcs; st->glsl_ubo = e->ubo; st->glsl_ssbo_w = e->ssbo_written;
                g_shc_hit++;
                return 1;
            }
    }
    int pend_i = -1;
    if (h1 && shader_async())
        for (int i = 0; i < g_pend_n; i++) if (g_pend[i].h1 == h1) { pend_i = i; break; }
    if (pend_i >= 0) {                                  /* a worker is compiling this stage: skip the draw until its binary is there */
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        int late = pf_now() - g_pend[pend_i].t0 > 20 * f.QuadPart;
        if (GetFileAttributesA(g_pend[pend_i].path) == INVALID_FILE_ATTRIBUTES && !late) { g_pend_skipped++; return 0; }
        if (late) g_pend_late++;
        g_pend[pend_i] = g_pend[--g_pend_n];
    }
    in.int_attrs = key[0]; in.sint_attrs = key[1]; in.int_tex = key[2]; in.sint_tex = key[3];
    in.binding_base = (unsigned)s * 20; in.ssbo_base = 40 + (unsigned)s * 16; in.ubo = ubo_on(); in.ubo_base = (unsigned)s * 18;
    in.fragdebug = getenv("SSBU_FRAGDEBUG") != NULL; in.screen_scale = in.stage == 1 && rs_on(); in.wide = wide_k() != 1.0f ? 1 + g_wide_camera : 0;
    uint64_t hm = h1 && g_xm_on > 0 ? xmemo_key(h1, &in) : 0;
    if (hm) {                                           /* translated before: the GLSL's binary without translating again */
        XMemo *m = xmemo_slot(hm, 0);
        static int verify = -1;                         /* TEST: TOMO_XMEMO=2 translates anyway and compares */
        if (verify < 0) verify = getenv("TOMO_XMEMO") && getenv("TOMO_XMEMO")[0] == '2';
        if (m && m->h2 && verify) {
            MxOut vo;
            static LONG64 nv, nbad;
            if (mx_translate(&in, &vo)) {
                size_t vl = strlen(vo.glsl);
                uint64_t vh = sh_hash(0x9E3779B97F4A7C15ULL ^ (uint64_t)s, vo.glsl, vl);
                int bad = vh != m->h2 || vl != m->src_len || vo.cbufs != m->cbufs || vo.ssbos != m->ssbos || vo.in_attr != m->in_attr || vo.tex_units != m->tex_units ||
                          vo.ssbo_written != m->ssbo_written || vo.uses_alpha != m->uses_alpha || vo.uses_fcs != m->uses_fcs || vo.ubo != m->ubo;
                nv++; nbad += bad;
                if (bad || nv % 100 == 0) printf("xmemo verify: %lld checked, %lld differ%s%c", (long long)nv, (long long)nbad, bad ? " (this one)" : "", 10);
                free(vo.glsl);
            }
        }
        if (m && m->h2 && m->stage == s && m->code_len == st->code_len) {
            ShSrc *hit = NULL;
            GLint mok = 0;
            for (ShSrc *e = g_shs[m->h2 & 4095]; e && !hit; e = e->next) if (e->h == m->h2 && e->len == m->src_len) hit = e;
            GLuint q = hit ? hit->prog : shader_binary_load(m->h2, m->src_len, &mok);
            if (q && (hit || mok)) {
                if (!hit) { ShSrc *e = calloc(1, sizeof *e); e->h = m->h2; e->len = m->src_len; e->prog = q; e->next = g_shs[m->h2 & 4095]; g_shs[m->h2 & 4095] = e; if (q < 65536) g_prog_src[q] = e; }
                st->glsl = q; st->glsl_state = 1;
                memcpy(st->glsl_key, key, sizeof st->glsl_key);
                st->glsl_cbufs = m->cbufs; st->glsl_ssbos = m->ssbos; st->glsl_dep = m->in_attr; st->glsl_tex = m->tex_units; st->glsl_alpha = m->uses_alpha; st->glsl_fcs = m->uses_fcs; st->glsl_ubo = m->ubo; st->glsl_ssbo_w = m->ssbo_written;
                ShEnt *e = calloc(1, sizeof *e);
                e->h = h1; e->stage = s; e->code_len = st->code_len; memcpy(e->key, key, 16); e->prog = q;
                e->cbufs = m->cbufs; e->ssbos = m->ssbos; e->in_attr = m->in_attr; e->tex_units = m->tex_units; e->ssbo_written = m->ssbo_written; e->uses_alpha = m->uses_alpha; e->uses_fcs = m->uses_fcs; e->ubo = m->ubo;
                e->next = g_shc[h1 & 4095]; g_shc[h1 & 4095] = e;
                g_xm_hit++;
                return 1;
            }
            if (q && !hit) glDeleteProgram(q);
        }
    }
    LONG64 pf0 = pf_now();
    int xl;
    { HS_IN("shader translate (machine code to GLSL)", 0); xl = mx_translate(&in, &out); HS_OUT(); }
    pf_add(PF_XLATE, pf0);
    if (!xl) {
        st->glsl_state = 2;
        g_st.glsl_fail++;
        logf_limited(&nfail, 60, "nvn_gl: program #%d %s stage: machine code not translated: %s\n", p->serial, k_stage_name[s], out.why);
        return 0;
    }
    char *src = out.glsl;
    const char *dir = getenv("SSBU_SHADERDUMP");
    if (dir) {
        char path[260];
        snprintf(path, sizeof path, "%s/b%05d_%s.glsl", dir, p->serial, k_stage_name[s]);
        FILE *f = fopen(path, "wb");
        if (f) { fputs(src, f); fclose(f); }
        snprintf(path, sizeof path, "%s/b%05d_%s.hash", dir, p->serial, k_stage_name[s]);   /* the name an override takes */
        if ((f = fopen(path, "wb"))) { fprintf(f, "%016llx\n", (unsigned long long)sh_hash(0, st->code, (size_t)st->code_len)); fclose(f); }
    }
    {   /* debug: TOMO_SHOVR=<dir> replaces the GLSL of a stage by <dir>/<hash of its GLSL>.glsl when that file exists */
        const char *ov = getenv("TOMO_SHOVR");
        char path[260];
        FILE *f;
        snprintf(path, sizeof path, "%s/%016llx.glsl", ov ? ov : "", (unsigned long long)sh_hash(0, src, strlen(src)));
        if (ov && (f = fopen(path, "rb"))) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            char *b = malloc((size_t)n + 1);
            b[fread(b, 1, (size_t)n, f)] = 0;
            fclose(f);
            free(out.glsl); out.glsl = src = b;
            printf("nvn_gl: program #%d %s stage: GLSL replaced by %s\n", p->serial, k_stage_name[s], path);
        }
    }
    const char *srcs[1] = {src};
    GLint ok = 0;
    size_t src_len = strlen(src);
    uint64_t h2 = g_shc_off ? 0 : sh_hash(0x9E3779B97F4A7C15ULL ^ (uint64_t)s, src, src_len);
    ShSrc *hit = NULL;
    for (ShSrc *e = g_shc_off ? NULL : g_shs[h2 & 4095]; e && !hit; e = e->next) if (e->h == h2 && e->len == src_len) hit = e;
    if (hit) { st->glsl = hit->prog; ok = 1; g_shc_src_hit++; }
    else if (shader_async() && !g_shc_off && h1 && pend_i < 0 && !getenv("TOMO_SHOVR") && g_shd_mode > 0 && g_shw_n && GetCurrentThreadId() == g_render_tid && g_pend_n < 512) {
        char path[320];
        shd_path(path, sizeof path, h2, src_len);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {     /* not compiled on this PC yet: a worker does it */
            ShJob *j = malloc(sizeof *j + (size_t)st->code_len + (in.cdata ? in.cdata_size : 0));
            j->next = NULL; j->stage = s; j->code_len = st->code_len; j->cdata_size = in.cdata ? in.cdata_size : 0;
            memcpy(j->key, key, sizeof j->key);
            memcpy(j->data, st->code, (size_t)st->code_len);
            if (in.cdata) memcpy(j->data + st->code_len, in.cdata, in.cdata_size);
            EnterCriticalSection(&g_shw_lock);
            if (g_shw_tail) g_shw_tail->next = j; else g_shw_head = j;
            g_shw_tail = j;
            LeaveCriticalSection(&g_shw_lock);
            InterlockedIncrement64(&g_shw_queued);
            ReleaseSemaphore(g_shw_sem, 1, NULL);
            g_pend[g_pend_n].h1 = h1; g_pend[g_pend_n].t0 = pf_now(); strcpy(g_pend[g_pend_n].path, path); g_pend_n++;
            g_pend_started++; g_pend_skipped++;
            if (h0) shv_record(h0, s, key);             /* the next start compiles it with the others (variants.txt) */
            free(src);
            return 0;
        }
    }
    if (!st->glsl && !hit) {
        pf0 = pf_now();
        LONG64 saved0 = g_shd_saved;
        st->glsl = g_shc_off ? glCreateShaderProgramv(s == 0 ? 0x8B31 : 0x8B30, 1, srcs) : shader_build(s == 0 ? 0x8B31 : 0x8B30, src, src_len, h2, &ok);
        if (g_shc_off) glGetProgramiv(st->glsl, 0x8B82, &ok);
        pf_add(PF_GLCOMPILE, pf0);
        if (g_shd_saved != saved0 && h0) shv_record(h0, s, key);
        g_shc_compiled++;
        if (ok && !g_shc_off) { ShSrc *e = calloc(1, sizeof *e); e->h = h2; e->len = src_len; e->prog = st->glsl; e->next = g_shs[h2 & 4095]; g_shs[h2 & 4095] = e;
            if (st->glsl < 65536) { if (pair_mode()) e->text = _strdup(src); g_prog_src[st->glsl] = e; } }
    }
    if (!ok) {
        char log[2000] = "";
        glGetProgramInfoLog(st->glsl, sizeof log - 1, NULL, log);
        if (nlink < 8) {
            char path[64];
            snprintf(path, sizeof path, "shader_fail_glsl_%02d.txt", nlink);
            FILE *f = fopen(path, "wb");
            if (f) { fputs(src, f); fclose(f); }
        }
        logf_limited(&nlink, 20, "nvn_gl: program #%d %s stage: the driver refuses the translated GLSL:\n%s\n", p->serial, k_stage_name[s], log);
        glDeleteProgram(st->glsl);
        st->glsl = 0; st->glsl_state = 2;
        g_st.glsl_fail++;
        free(src);
        return 0;
    }
    free(src);
    st->glsl_state = 1;
    memcpy(st->glsl_key, key, sizeof st->glsl_key);
    st->glsl_cbufs = out.cbufs; st->glsl_ssbos = out.ssbos; st->glsl_dep = out.in_attr; st->glsl_tex = out.tex_units; st->glsl_alpha = out.uses_alpha; st->glsl_fcs = out.uses_fcs; st->glsl_ubo = out.ubo; st->glsl_ssbo_w = out.ssbo_written;
    if (out.ssbo_written) { static int nw; logf_limited(&nw, 5, "nvn_gl: program #%d %s stage writes storage buffers (mask 0x%x): kept in GL buffers and copied back to guest memory\n", p->serial, k_stage_name[s], out.ssbo_written); }
    if (!getenv("TOMO_SHOVR")) xmemo_put(hm, s, st->code_len, h2, (uint32_t)src_len, &out);
    if (!g_shc_off) {
        ShEnt *e = calloc(1, sizeof *e);
        e->h = h1; e->stage = s; e->code_len = st->code_len; memcpy(e->key, key, 16); e->prog = st->glsl;
        e->cbufs = out.cbufs; e->ssbos = out.ssbos; e->in_attr = out.in_attr; e->tex_units = out.tex_units; e->ssbo_written = out.ssbo_written; e->uses_alpha = out.uses_alpha; e->uses_fcs = out.uses_fcs; e->ubo = out.ubo;
        e->next = g_shc[h1 & 4095]; g_shc[h1 & 4095] = e;
    }
    g_st.glsl_ok++;
    return 1;
}

/* ---------------------------------------------------------------- deferred deletion (objects finalized on guest threads) */
static CRITICAL_SECTION g_dead_lock;
static struct { int kind; void *p; } *g_dead;
static int g_dead_n, g_dead_cap;
static void defer_free(int kind, void *p) {
    EnterCriticalSection(&g_dead_lock);
    if (g_dead_n == g_dead_cap) { g_dead_cap = g_dead_cap ? g_dead_cap * 2 : 256; g_dead = realloc(g_dead, sizeof *g_dead * (size_t)g_dead_cap); }
    g_dead[g_dead_n].kind = kind; g_dead[g_dead_n++].p = p;
    LeaveCriticalSection(&g_dead_lock);
}
static void gl_housekeeping(void) {
    if (g_grave_n) grave_sweep(0);
    if (!g_dead_n) return;
    EnterCriticalSection(&g_dead_lock);
    int kept = 0;
    for (int i = 0; i < g_dead_n; i++) {
        /* a finalized texture that a texture pool entry still names and that draws still use keeps its GL object for a while:
         * the game goes on drawing through that entry for up to a second (the GPU reads the descriptor and the texels still in
         * memory) before it registers the moved texture there; freed at once, those draws sampled the black dummy (the
         * house's walls, deck and door flashing while its textures moved). Not drawn in the last two frames: freed. */
        if (g_dead[i].kind == K_TEX) {
            Tex *dt = g_dead[i].p;
            if (dt->pool_refs > 0 && dt->last_bind >= g_st.frames - 2 && g_st.frames - dt->dead_frame < 300) { g_dead[kept++] = g_dead[i]; continue; }
        }
        if (g_dead[i].kind == K_TEX) tex_free_gl(g_dead[i].p);
        else if (g_dead[i].kind == K_PROG) {
            Prog *p = g_dead[i].p;
            for (int s = 0; s < 6; s++) { if (p->st[s].gl) glDeleteProgramsARB(1, &p->st[s].gl); if (p->st[s].glsl && (s > 1 || g_shc_off > 0)) glDeleteProgram(p->st[s].glsl); free(p->st[s].text); memset(&p->st[s], 0, sizeof p->st[s]); }
        }
    }
    g_dead_n = kept;
    LeaveCriticalSection(&g_dead_lock);
}

/* ---------------------------------------------------------------- command lists */
/* the OP_* list: nvn_front.h */
static const char *const k_op_name[OP_COUNT] = {
    "nop", "SetTexturePool", "SetSamplerPool", "BindProgram", "BindUniformBuffer", "BindStorageBuffer", "BindTexture", "BindVertexBuffer", "BindVertexAttribState",
    "BindVertexStreamState", "BindBlendState", "BindColorState", "BindChannelMaskState", "BindMultisampleState", "BindPolygonState", "BindDepthStencilState",
    "SetViewports", "SetScissors", "SetDepthRanges", "SetRenderTargets", "SetStencilRef", "SetStencilValueMask", "SetStencilMask", "SetBlendColor", "SetAlphaRef",
    "SetPointSize", "SetLineWidth", "SetPolygonOffsetClamp", "SetSampleMask", "SetRasterizerDiscard", "SetDepthClamp", "SetPrimitiveRestart", "SetPatchSize",
    "ClearColor", "ClearDepthStencil", "DrawArrays", "DrawElements", "CopyBufferToTexture", "CopyTextureToTexture", "Downsample", "CopyBufferToBuffer",
    "UpdateUniformBuffer", "ClearBuffer", "CallCommands", "SetCopyRowStride", "CopyTextureToBuffer", "DispatchCompute", "BindImage", "SetCopyImageStride", "SetDepthBounds", "SetConservativeRasterEnable", "SetConservativeRasterDilate",
    "SetSubpixelPrecisionBias", "SetViewportSwizzles", "SetTessellationLevels", "Barrier", "Discard", "ClearTexture", "DownsampleTextureView", "DrawIndirect",
    "DispatchComputeIndirect", "SetRenderEnable", "ResetCounter", "ReportCounter", "ReportValue", "FenceSync", "WaitSync", "SignalEvent", "WaitEvent",
    "BindCoverageModulationTable", "DebugGroup", "DrawTexture", "BindDeferred"};

typedef struct { uint8_t *data; size_t len; uint32_t gen, model_cmd, model_ctrl, dispatches; int finalized, r_n, inv_k; uint64_t inv_off, inv_size; LONG64 created, used, inval; uint64_t r_pool[4], r_lo[4], r_hi[4]; } List;   /* r_*: the game's command memory the list occupies; inval: frame + 1 at which the game gave that memory out again */
static LONG64 g_list_bytes;
static int g_listlog_dbg;      /* TOMO_LISTLOG=1: calls of lists whose memory the game had given out again */
static List *g_lists;
static int g_list_count, g_list_cap, *g_list_free, g_list_free_n;
static SRWLOCK g_list_lock = SRWLOCK_INIT;

/* Track R1 round 5 (run-ahead, r1_state.c): every command buffer's host record is listed, so that a restore of the game's
 * state can put the recordings in progress back as they were at the save (the commands recorded so far are host data:
 * without this a speculative pass's commands would stay in the buffer and be submitted again by the real pass, or the
 * commands recorded before the save would be gone after the speculative pass ended the buffer). */
static CmdBuf *g_all_cb[512]; static int g_all_cb_n; static SRWLOCK g_all_cb_lock = SRWLOCK_INIT;
static CmdBuf *cb_new(uint64_t g) {
    CmdBuf *cb = hnew(g, K_CMD, sizeof(CmdBuf));
    AcquireSRWLockExclusive(&g_all_cb_lock);
    int i; for (i = 0; i < g_all_cb_n && g_all_cb[i]; i++) {}
    if (i < 512) { g_all_cb[i] = cb; if (i == g_all_cb_n) g_all_cb_n++; }
    ReleaseSRWLockExclusive(&g_all_cb_lock);
    return cb;
}
static void cb_forget(CmdBuf *cb) { AcquireSRWLockExclusive(&g_all_cb_lock); for (int i = 0; i < g_all_cb_n; i++) if (g_all_cb[i] == cb) g_all_cb[i] = NULL; ReleaseSRWLockExclusive(&g_all_cb_lock); }
typedef struct { CmdBuf *cb, copy; uint8_t *data; size_t cap; } CbSave;
static CbSave g_cbs[512]; static int g_ncbs;
/* (R3 pub1) r1_ring.c: the save area's addresses, without a linker map (the prebuilt runtime package has none) */
void nvn_gl_r1_cb_area(void **cbs, void **ncbs, void **all) { *cbs = g_cbs; *ncbs = &g_ncbs; *all = g_all_cb; }
size_t nvn_gl_r1_cb_size(void) { return sizeof(CbSave); }      /* (R3 pub1) the entry size r1_ring.c expects (RG_CBS_SIZE) */
size_t nvn_r1_cb_save(void) {                      /* the guest threads are held */
    size_t bytes = 0;
    AcquireSRWLockShared(&g_all_cb_lock);
    g_ncbs = g_all_cb_n;
    for (int i = 0; i < g_ncbs; i++) {
        CbSave *s = &g_cbs[i]; CmdBuf *cb = g_all_cb[i];
        s->cb = cb;
        if (!cb) continue;
        s->copy = *cb;
        if (cb->len) { if (s->cap < cb->len) { s->cap = cb->len + 65536; s->data = realloc(s->data, s->cap); } memcpy(s->data, cb->data, cb->len); bytes += cb->len; }
    }
    ReleaseSRWLockShared(&g_all_cb_lock);
    return bytes;
}
void nvn_r1_cb_restore(void) {
    AcquireSRWLockShared(&g_all_cb_lock);
    for (int i = 0; i < g_ncbs; i++) {
        CbSave *s = &g_cbs[i]; CmdBuf *cb = s->cb;
        if (!cb || g_all_cb[i] != cb) continue;           /* finalized meanwhile */
        uint8_t *d = cb->data; size_t cap = cb->cap;       /* (the saved record's buffer may belong to a list now: never reused) */
        *cb = s->copy;
        if (s->copy.len && cap < s->copy.len) { cap = s->copy.len + 65536; d = realloc(d, cap); }
        if (s->copy.len) memcpy(d, s->data, s->copy.len);
        cb->data = d; cb->cap = cap;
    }
    ReleaseSRWLockShared(&g_all_cb_lock);
}
static CmdBuf *cb_get(uint64_t g) {
    CmdBuf *cb = hobj(g, K_CMD);
    return cb ? cb : cb_new(g);
}
/* A recording's buffer goes to its list at the end, so every recording started from nothing and doubled its way up
 * (realloc 0.6 % of a game job thread, 2026-11-01): the first one is as big as the last recording of this command buffer. */
static void cb_grow(CmdBuf *cb, size_t need) {
    size_t want = (cb->cap + need) * 2 + 4096;
    if (!cb->cap && (size_t)cb->hint + need + 4096 > want) want = (size_t)cb->hint + need + 4096;
    cb->cap = want; cb->data = realloc(cb->data, cb->cap);
}
/* Appends a command with x1..x7 as its arguments and room for `blob` more bytes; returns it (valid until the next append). */
static Cmd *rec(Ctx *c, uint32_t op, size_t blob) {
    CmdBuf *cb = cb_get(A0);
    size_t need = sizeof(Cmd) + ((blob + 7) & ~(size_t)7);
    if (cb->len + need > cb->cap) cb_grow(cb, need);
    {   /* Command memory as the game sees it. The real encoding is the driver's own; the model is 8 bytes per command plus its
         * array payload (control memory: 32 bytes per recording, charged by EndRecording). When the memory the game added is
         * used up, its callback is asked for more, as NVN does (NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY = 0). */
        uint64_t use = (g_game_build == GAME_OTHER ? 4 : 8) + ((blob + 7) & ~(size_t)7);   /* Tomodachi Life: its gsys display lists (ShapeDL) live in fixed-size heaps sized for the real encoding (Maxwell methods: a 4-byte header and data words); 8 per command ran the MiiBodyBaseModel heap dry */
        if (cb->cmd_size && cb->cmd_used + use > cb->cmd_size && cb->callback && !cb->callback_off && !cb->in_callback) {
            uint64_t sx[19];
            V128 sv[8];
            memcpy(sx, c->x, sizeof sx); memcpy(sv, c->v, sizeof sv);
            cb->in_callback = 1;
            c->x[1] = 0; c->x[2] = (use + 0xff) & ~0xffull; c->x[3] = cb->callback_data;
            uint64_t fp0 = c->x[29], sp0 = c->x[31];
            call_guest(c, cb->callback);
            if (c->x[29] != fp0 || c->x[31] != sp0) {
                static int said;
                if (said++ < 5) printf("nvn_gl: the command memory callback %llx changed x29 %llx -> %llx, sp %llx -> %llx\n", (unsigned long long)cb->callback,
                                       (unsigned long long)fp0, (unsigned long long)c->x[29], (unsigned long long)sp0, (unsigned long long)c->x[31]);
            }
            cb->in_callback = 0;
            {   static int dbg = -1, nlog;          /* TOMO_CBLOG=1: each out-of-command-memory callback and whether the game added memory */
                if (dbg < 0) dbg = getenv("TOMO_CBLOG") != NULL;
                if (dbg && nlog++ < 300)
                    printf("cblog: frame %lld cmdbuf %llx asked for %llu bytes (recording %u bytes so far): now %llu of %llu used, pool %llx offset %llx\n", (long long)g_st.frames, (unsigned long long)(uintptr_t)cb,
                           (unsigned long long)((use + 0xff) & ~0xffull), (unsigned)cb->rec_cmd, (unsigned long long)cb->cmd_used, (unsigned long long)cb->cmd_size, (unsigned long long)cb->cmd_pool, (unsigned long long)cb->cmd_off);
            }
            memcpy(c->x, sx, sizeof sx); memcpy(c->v, sv, sizeof sv);
            if (cb->len + need > cb->cap) cb_grow(cb, need);
        }
        cb->cmd_used += use; cb->rec_cmd += use;
    }
    {   /* TOMO_RECLOG=<frame>: while the game builds that frame, its vertex binds, indexed draws, program binds and calls */
        static long long rl = -2;
        if (rl == -2) rl = getenv("TOMO_RECLOG") ? atoll(getenv("TOMO_RECLOG")) : -1;
        if (rl >= 0 && (op == OP_VBUF || op == OP_DRAW_ELEMENTS || op == OP_CALL || op == OP_PROGRAM)) {
            long long display_frame_count(void);
            long long f = display_frame_count();
            if (rl == 0 || (f >= rl && f <= rl + 1))
                printf("rec: frame %lld cmdbuf %llx %s a1 %llx a2 %llx a3 %llx\n", f, (unsigned long long)A0, k_op_name[op], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
        }
    }
    Cmd *m = (Cmd *)(cb->data + cb->len);
    cb->len += need;
    m->op = op; m->size = (uint32_t)need;
    for (int i = 0; i < 7; i++) m->a[i] = c->x[i + 1];
    return m;
}

/* The lists by the command memory they occupy (2026-10-31): an invalidation (each EndRecording and AddCommandMemory, 70 to
 * 280 a frame on the island) looked at every list (17,700): 5-17 ms a frame on the game's job threads, and their new
 * recordings waited up to 10 ms for the lock meanwhile; the main thread waits for those jobs. Buckets of 64 KB of a pool's
 * memory now hold the lists with a range there; an invalidation looks at its buckets only. Changed under the exclusive
 * lock only (list_register, list_collect), read under the shared one. TOMO_LIST_INDEX=0: every list, as before. */
#define LB_SHIFT 16
typedef struct { uint64_t pool, bucket; int *id, n, cap, used; } LBucket;
static LBucket *g_lb;
static int g_lb_cap, g_lb_used, g_lb_on = -1;
static unsigned lb_hash(uint64_t pool, uint64_t b) { return (unsigned)(((pool >> 4) * 0x9E3779B97F4A7C15ULL ^ (b + 1) * 0xC2B2AE3D27D4EB4FULL) >> 32); }
static LBucket *lb_find(uint64_t pool, uint64_t b, int make) {
    if (!g_lb_cap) {
        if (!make) return NULL;
        g_lb_cap = 4096; g_lb = calloc((size_t)g_lb_cap, sizeof *g_lb);
        if (!g_lb) { g_lb_cap = 0; g_lb_on = 0; return NULL; }
    }
    if (make && (g_lb_used + 1) * 2 > g_lb_cap) {                  /* grow: rehash every bucket */
        int cap = g_lb_cap * 2;
        LBucket *nb = calloc((size_t)cap, sizeof *nb);
        if (!nb) { g_lb_on = 0; return NULL; }
        for (int i = 0; i < g_lb_cap; i++)
            if (g_lb[i].used) { unsigned h = lb_hash(g_lb[i].pool, g_lb[i].bucket) & (unsigned)(cap - 1); while (nb[h].used) h = (h + 1) & (unsigned)(cap - 1); nb[h] = g_lb[i]; }
        free(g_lb); g_lb = nb; g_lb_cap = cap;
    }
    for (unsigned h = lb_hash(pool, b) & (unsigned)(g_lb_cap - 1);; h = (h + 1) & (unsigned)(g_lb_cap - 1)) {
        LBucket *e = &g_lb[h];
        if (!e->used) {
            if (!make) return NULL;
            e->used = 1; e->pool = pool; e->bucket = b; g_lb_used++;
            return e;
        }
        if (e->pool == pool && e->bucket == b) return e;
    }
}
static void lb_list(int i, int add) {               /* list i into (add) or out of its buckets; the exclusive lock is held */
    if (g_lb_on < 0) g_lb_on = !(getenv("TOMO_LIST_INDEX") && getenv("TOMO_LIST_INDEX")[0] == '0');
    if (!g_lb_on) return;
    const List *l = &g_lists[i];
    for (int k = 0; k < l->r_n; k++) {
        if (l->r_hi[k] <= l->r_lo[k]) continue;
        if (add && ((l->r_hi[k] - 1) >> LB_SHIFT) - (l->r_lo[k] >> LB_SHIFT) >= 4096) { g_lb_on = 0; return; }      /* (a range of 256 MB: every list again) */
        for (uint64_t b = l->r_lo[k] >> LB_SHIFT, last = (l->r_hi[k] - 1) >> LB_SHIFT; b <= last; b++) {
            LBucket *e = lb_find(l->r_pool[k], b, add);
            if (!e) { if (add) return; continue; }
            if (add) {
                if (e->n && e->id[e->n - 1] == i) continue;          /* (two ranges of the list in one bucket) */
                if (e->n == e->cap) { int cap = e->cap ? e->cap * 2 : 8; int *id = realloc(e->id, sizeof *id * (size_t)cap); if (!id) { g_lb_on = 0; return; } e->id = id; e->cap = cap; }
                e->id[e->n++] = i;
            } else for (int j = 0; j < e->n; j++) if (e->id[j] == i) { e->id[j] = e->id[--e->n]; break; }
        }
    }
}
static volatile LONG64 g_lp_inv_n, g_lp_inv_t, g_lp_inv_wait, g_lp_reg_n, g_lp_reg_wait;      /* TOMO_LISTPROF=1 (TEST): list table costs */
static int g_lp_on = -1;
static uint64_t list_register(uint8_t *data, size_t len, uint32_t model_cmd, uint32_t model_ctrl, uint32_t dispatches, const CmdBuf *cb) {
    if (!data) data = malloc(8);                       /* an empty recording is still a valid handle */
    LONG64 lp0 = g_lp_on > 0 ? pf_now() : 0;
    AcquireSRWLockExclusive(&g_list_lock);
    if (g_lp_on > 0) { InterlockedIncrement64(&g_lp_reg_n); InterlockedExchangeAdd64(&g_lp_reg_wait, pf_now() - lp0); }
    int i;
    if (g_list_free_n) i = g_list_free[--g_list_free_n];
    else {
        if (g_list_count == g_list_cap) {
            g_list_cap = g_list_cap ? g_list_cap * 2 : 1024;
            g_lists = realloc(g_lists, sizeof *g_lists * (size_t)g_list_cap);
            g_list_free = realloc(g_list_free, sizeof *g_list_free * (size_t)g_list_cap);
            memset(g_lists + g_list_count, 0, sizeof *g_lists * (size_t)(g_list_cap - g_list_count));
        }
        i = g_list_count++;
    }
    g_lists[i].data = data; g_lists[i].len = len; g_lists[i].created = g_st.frames; g_lists[i].used = -1;
    g_lists[i].model_cmd = model_cmd; g_lists[i].model_ctrl = model_ctrl; g_lists[i].dispatches = dispatches; g_lists[i].finalized = 0;
    g_lists[i].inval = 0; g_lists[i].r_n = cb->r_n;
    for (int k = 0; k < cb->r_n; k++) { g_lists[i].r_pool[k] = cb->r_pool[k]; g_lists[i].r_lo[k] = cb->r_lo[k]; g_lists[i].r_hi[k] = cb->r_hi[k]; }
    lb_list(i, 1);
    g_list_bytes += (LONG64)len;
    uint64_t h = ((uint64_t)(g_lists[i].gen & 0x7fffffff) << 32) | (uint32_t)(i + 1);
    ReleaseSRWLockExclusive(&g_list_lock);
    InterlockedIncrement64(&g_st.lists);
    return h;
}
static uint8_t *list_lookup(uint64_t h, size_t *len) {
    uint8_t *data = NULL;
    uint32_t i = (uint32_t)h - 1;
    AcquireSRWLockShared(&g_list_lock);
    if (h && i < (uint32_t)g_list_count && g_lists[i].data && (g_lists[i].gen & 0x7fffffff) == (uint32_t)(h >> 32)) { data = g_lists[i].data; *len = g_lists[i].len; g_lists[i].used = g_st.frames; }
    if (data && g_listlog_dbg && g_lists[i].inval) {
        static LONG nlog; List *l = &g_lists[i];
        if (InterlockedIncrement(&nlog) <= 60)
            printf("listlog: call of list %u (made frame %lld, %zu bytes, %d ranges, range %d %llx..%llx) at frame %lld after new memory %llx+%llx at frame %lld\n", i, (long long)l->created, l->len, l->r_n, l->inv_k,
                   (unsigned long long)l->r_lo[l->inv_k], (unsigned long long)l->r_hi[l->inv_k], (long long)g_st.frames, (unsigned long long)l->inv_off, (unsigned long long)l->inv_size, (long long)l->inval - 1);
    }
    ReleaseSRWLockShared(&g_list_lock);
    return data;
}
/* A command handle stays valid as long as the command memory it was recorded into: the game gives that memory to a command
 * buffer again (AddCommandMemory) or finalizes its pool only when it no longer calls the handle. Tomodachi Life records the
 * display lists of its models (gsys ShapeDL) once and calls them whenever the model is drawn: dropping lists after two
 * seconds without a call (as before) lost the Mii's, the palm trees' and the shrubs' draws for good once they had been out of
 * sight (blank Mii in the editor and the resident view of a new game; 235 calls of dropped lists a frame). */
static void list_hit(int i, uint64_t pool, uint64_t off, uint64_t size) {      /* list i, if it occupies that memory: marked given out again */
    List *l = &g_lists[i];
    if (!l->data || l->inval) return;
    for (int k = 0; k < l->r_n; k++)
        if (l->r_pool[k] == pool && l->r_lo[k] < off + size && off < l->r_hi[k]) {
            InterlockedCompareExchange64(&l->inval, g_st.frames + 1, 0);
            if (g_listlog_dbg) { l->inv_off = off; l->inv_size = size; l->inv_k = k; }
            return;
        }
}
static void list_invalidate(uint64_t pool, uint64_t off, uint64_t size) {
    if (g_lp_on < 0) g_lp_on = getenv("TOMO_LISTPROF") != NULL;
    LONG64 t0 = g_lp_on > 0 ? pf_now() : 0;
    AcquireSRWLockShared(&g_list_lock);                   /* shared: lookups go on meanwhile; only this marks inval */
    LONG64 t1 = g_lp_on > 0 ? pf_now() : 0;
    if (g_lb_on > 0 && size && size <= ((uint64_t)64 << LB_SHIFT)) {
        for (uint64_t b = off >> LB_SHIFT, last = (off + size - 1) >> LB_SHIFT; b <= last; b++) {
            const LBucket *e = lb_find(pool, b, 0);
            if (e) for (int j = 0; j < e->n; j++) list_hit(e->id[j], pool, off, size);
        }
    } else for (int i = 0; i < g_list_count; i++) list_hit(i, pool, off, size);
    ReleaseSRWLockShared(&g_list_lock);
    if (g_lp_on > 0) { InterlockedIncrement64(&g_lp_inv_n); InterlockedExchangeAdd64(&g_lp_inv_wait, t1 - t0); InterlockedExchangeAdd64(&g_lp_inv_t, pf_now() - t1); }
}
void nvn_gl_lists_pool_gone(uint64_t pool) { list_invalidate(pool, 0, ~0ull >> 1); }      /* nvn_stub.c: nvnMemoryPoolFinalize */
/* Render thread, once per frame: drop the lists whose memory the game reused or released (a few frames later: replays queued
 * before the reuse still run), those of a finalized handle, and lists without known memory after two seconds unused. Above
 * 1 GB of recorded lists the ones unused for 20 seconds go too (shortcut: a guard against a game that never reuses its
 * command memory; raise TOMO_LIST_CAP_MB if a model then disappears). */
static void list_collect(void) {
    static LONG64 cap = -1;
    if (cap < 0) g_listlog_dbg = getenv("TOMO_LISTLOG") != NULL;
    if (cap < 0) cap = (getenv("TOMO_LIST_CAP_MB") ? _atoi64(getenv("TOMO_LIST_CAP_MB")) : 1024) << 20;
    AcquireSRWLockExclusive(&g_list_lock);
    int over = g_list_bytes > cap;
    for (int i = 0; i < g_list_count; i++) {
        List *l = &g_lists[i];
        if (!l->data) continue;
        LONG64 last = l->used >= 0 ? l->used : l->created;
        if (l->finalized || (l->inval && g_st.frames + 1 - l->inval > 4) || (!l->r_n && g_st.frames - last > 120) || (over && g_st.frames - last > 600)) {
            g_list_bytes -= (LONG64)l->len;
            lb_list(i, 0);
            free(l->data); l->data = NULL; l->gen++; g_list_free[g_list_free_n++] = i;
        }
    }
    if (over) { static int nlog; logf_limited(&nlog, 5, "nvn_gl: recorded command lists above %lld MB: lists unused for 20 s dropped (%lld MB left)\n", (long long)(cap >> 20), (long long)(g_list_bytes >> 20)); }
    ReleaseSRWLockExclusive(&g_list_lock);
}

/* ---------------------------------------------------------------- GPU state as left by the commands executed so far */
static struct {
    TexPool *tpool; SamplerPool *spool;
    Prog *prog[6];
    Range ubo[6][16], ssbo[6][32], vbuf[32];
    uint64_t nvn_img[6][8];                       /* image handles (nvnCommandBufferBindImage) */
    uint64_t nvn_tex[6][32], gl_tex[6][112];      /* constant buffer 2 as the shaders see it (mx_translate.h): 32 bytes reserved, one 64-bit handle per unit,
                                                     8 image handle slots (bytes 288..351, not read: images go to image units), then from byte 384
                                                     four words per unit: width, height, depth or layers, levels | is3d << 16 */
    uint32_t tex_int[6], tex_sint[6];             /* units holding integer formats (and which are signed) */
    int tex_dirty[6]; size_t tex_off[6]; uint32_t tex_gen[6];
    uint32_t vattr[32]; int nattr;
    struct { uint32_t stride, divisor; } vstream[32];
    uint64_t ds;
    uint32_t chanmask;
    int sref[2], svmask[2], swmask[2], stencil_dirty;
    float alpha_ref; int alpha_func;
    int have_fbo, fbo_w, fbo_h; GLuint fbo;
    Tex *color0, *color[8], *depth_tex;
    int64_t row_stride, image_stride;
    int attr_enabled[16];
    uint8_t ms[24]; unsigned ms_ext;              /* the bound multisample state, and which of its extensions are on in OpenGL */
    int sc0[4], render_off, debug_depth, sample_shading;
    float gvp[16][4], prs, prsy;               /* resolution scale: the game's viewports, and the scale of the targets bound in x / y (0 = 1) */
    int gsc[16][4];                            /* ... and its scissors */
    uint32_t gvp_set;
} S;
static void fbo_drop_bound(GLuint fbo) { if (S.have_fbo && S.fbo == fbo) { S.have_fbo = 0; glBindFramebuffer(GL_FRAMEBUFFER_, 0); } }

static GLenum nvn_func(unsigned f) { return GL_NEVER + (f >= 1 && f <= 8 ? f - 1 : 7); }
static GLenum blend_func(unsigned f) {
    if (f >= 3 && f <= 0xB) return 0x300 + f - 3;
    switch (f) { case 1: return GL_ZERO; case 2: return GL_ONE; case 0x10: return 0x88F9; case 0x11: return 0x88FA; case 0x12: return 0x8589; case 0x13: return 0x88FB;
                 case 0x61: return 0x8001; case 0x62: return 0x8002; case 0x63: return 0x8003; case 0x64: return 0x8004; }
    return GL_ONE;
}
/* NVNdrawPrimitive has the OpenGL values for 0..9 (points .. polygon); adjacency 10..13 and patches 14 map to the GL enums.
 * Winding: glClipControl(GL_UPPER_LEFT) keeps OpenGL's rule (facing is decided as with a lower-left origin), and NVN is taken to
 * behave the same; SSBU_FLIPFACE inverts it for experiments (SSBU_DRAWLOG shows the samples each draw produces). */
static GLenum nvn_prim(unsigned p) {
    static const GLenum m[15] = {GL_POINTS, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS, GL_QUAD_STRIP, GL_POLYGON,
                                 0x000A, 0x000B, 0x000C, 0x000D, 0x000E};
    return p < 15 ? m[p] : GL_TRIANGLES;
}
static GLenum blend_eq(unsigned e) { static const GLenum m[6] = {0x8006, 0x8006, 0x800A, 0x800B, 0x8007, 0x8008}; return m[e < 6 ? e : 1]; }
static GLenum stencil_op(unsigned o) { static const GLenum m[9] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, 0x8507, 0x8508}; return m[o < 9 ? o : 1]; }

static void apply_stencil(void) {
    if (!S.stencil_dirty) return;
    S.stencil_dirty = 0;
    glStencilFuncSeparate(GL_FRONT, nvn_func((unsigned)(S.ds >> 8) & 15), S.sref[0], (GLuint)S.svmask[0]);
    glStencilFuncSeparate(GL_BACK, nvn_func((unsigned)(S.ds >> 12) & 15), S.sref[1], (GLuint)S.svmask[1]);
    glStencilMaskSeparate(GL_FRONT, (GLuint)S.swmask[0]);
    glStencilMaskSeparate(GL_BACK, (GLuint)S.swmask[1]);
}
static void apply_chanmask(void) {
    for (unsigned i = 0; i < 8; i++) { unsigned m = S.chanmask >> (i * 4); glColorMaski(i, m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1); }
}

/* Feedback guard: a draw that samples, through a texture handle, the texture it renders to (the game draws each mip level of
 * its cube map arrays from the previous one). In GL that is undefined, and here the NVIDIA driver hung the GPU (a TDR ended
 * the process). The draw samples a copy of the texture instead, refreshed before every such draw. */
static Tex *tex_scratch(Tex *t) {
    static int nlog;
    if (!gl_has(glCopyImageSubData, "glCopyImageSubData") || t->b.samples > 1 || !t->o.gl) return NULL;
    if (!(t->b.target == 1 || t->b.target == 2 || t->b.target == 4 || t->b.target == 8 || t->b.target == 9)) return NULL;
    if (!t->scratch) {
        Tex *sc = calloc(1, sizeof *sc);
        if (!sc) return NULL;
        sc->b = t->b;
        tex_realize(sc);
        sc->gpu_written = 1; sc->uploaded = 1;
        t->scratch = sc;
        logf_limited(&nlog, 8, "nvn_gl: feedback guard: texture target %d %dx%dx%d format 0x%x levels %d is sampled by a draw into it: a copy is sampled\n",
                     t->b.target, t->b.width, t->b.height, t->b.depth, t->b.format, t->glevels);
    }
    Tex *sc = t->scratch;
    GLenum tg = gl_target(t->b.target, t->b.samples);
    int layers = t->b.target == 2 ? 0 : tex_layers(&t->b);
    LONG64 pf0 = pf_now();
    for (int l = 0; l < t->glevels && l < sc->glevels; l++) {
        int w = lvl(tw(t), l), h = lvl(th(t), l), d = t->b.target == 2 ? lvl(t->b.depth, l) : layers;
        glCopyImageSubData(t->o.gl, tg, l, 0, 0, 0, sc->o.gl, tg, l, 0, 0, 0, w, h, d);
    }
    sc->rt_gen++;                                  /* its own view copies (a reinterpreted format) refresh too */
    pf_add(PF_FEEDBACK, pf0);
    return sc;
}
static uint64_t g_fb_saved[6][32];
static uint32_t g_fb_mask[6];
static int g_fb_active;
static void feedback_restore(void) {
    if (!g_fb_active) return;
    for (int s = 0; s < 5; s++) {
        if (!g_fb_mask[s]) continue;
        for (unsigned u = 0; u < 32; u++) if (g_fb_mask[s] >> u & 1) S.gl_tex[s][4 + u] = g_fb_saved[s][u];
        S.tex_dirty[s] = 1; g_fb_mask[s] = 0;
    }
    g_fb_active = 0;
}
static int feedback_copy_mode(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOMO_FEEDBACK_COPY"); v = e ? atoi(e) : 0; } return v; }
static void view_copy_fill(Tex *t, VNode *n) {
    GLenum tg = gl_target(n->e.target, t->b.samples);
    int w = tw(t), hgt = th(t);
    for (int l = 0; l < n->e.nlvl; l++)
        glCopyImageSubData(t->o.gl, tg, l, 0, 0, 0, n->o.gl, tg, l, 0, 0, 0, lvl(w, l), lvl(hgt, l), tg == GL_TEXTURE_2D ? 1 : n->e.nlay);
    n->copy_gen = t->rt_gen;
}
/* Per-draw checks look at the units the bound programs sample (all 32 until a stage is translated), and read the texture
 * pointer of a unit's handle without the pool lock or a sampler copy: pool arrays are made once. Doing full lookups of
 * every unit twice per draw cost 15 ms a frame where the island draws 1,600 times. */
static uint32_t units_sampled(int s) { Prog *p = S.prog[s]; return !p ? 0 : p->st[s].glsl_state == 1 ? p->st[s].glsl_tex : 0xffffffffu; }
static Tex *unit_tex(int s, unsigned u) {
    uint64_t nh = S.nvn_tex[s][u];
    uint32_t tid = (uint32_t)nh & 0xfffff;
    return nh && S.tpool && tid < (uint32_t)S.tpool->count ? S.tpool->e[tid].tex : NULL;
}
static void alias_refresh(Tex *t);
static int g_alias_any;
static void view_copies_refresh(void) {               /* the copies standing for views of render targets (tex_view_obj), and aliases, before a draw */
    if (!g_copy_views && !g_alias_any) return;
    for (int s = 0; s < 5; s++)
        for (uint32_t m = units_sampled(s), u = 0; m; m >>= 1, u++) {
            Tex *t;
            if (!(m & 1) || !(t = unit_tex(s, u)) || t->dead || !t->o.gl) continue;
            if (t->alias_from) alias_refresh(t);
            if (!t->views || t->vc_gen == t->rt_gen + 1) continue;      /* every copy already up to date: no walk (2760 draws a frame) */
            for (VNode *n = t->views; n; n = n->next) if (n->is_copy && n->copy_gen != t->rt_gen) view_copy_fill(t, n);
            t->vc_gen = t->rt_gen + 1;
        }
}
/* Widescreen: a draw into a target that is not screen-shaped reads a widened texture (the picture) in the game's own 16:9
 * coordinates: the layouts capture the picture behind a pane (a 160x1080 strip at each side of the food list, the 480x890
 * list) and blend it back at the pane's soft edges. Over the whole width those captures took the wrong part of the picture
 * (bands of the room's far corner over the menu at 21:9, 2026-10-25): such a draw samples the centred 16:9 part instead.
 * Screen-shaped targets (blur and bloom chains, 480x272 and down) keep the whole width: what they make goes back wide. */
static Tex *tex_crop(Tex *t) {
    Fmt f;
    if (!gl_has(glCopyImageSubData, "glCopyImageSubData") || t->b.target != 1 || t->b.samples > 1 || !t->o.gl) return NULL;
    if (!fmt_get(t->b.format, &f) || f.kind != 0) return NULL;      /* the picture only: depth is read by 3D passes, in the wide view */
    if (!t->crop) {
        Tex *c = calloc(1, sizeof *c);
        if (!c) return NULL;
        c->b = t->b;
        g_realize_narrow = 1; tex_realize(c); g_realize_narrow = 0;
        c->gpu_written = 1; c->uploaded = 1;
        t->crop = c; t->crop_gen = t->rt_gen + 1;
    }
    Tex *c = t->crop;
    if (t->crop_gen != t->rt_gen) {
        for (int l = 0; l < t->glevels && l < c->glevels; l++) {
            int w = lvl(tw(c), l), h = lvl(th(c), l);
            glCopyImageSubData(t->o.gl, GL_TEXTURE_2D, l, (lvl(tw(t), l) - w) / 2, 0, 0, c->o.gl, GL_TEXTURE_2D, l, 0, 0, 0, w, h, 1);
        }
        t->crop_gen = t->rt_gen; c->rt_gen++;            /* its own view copies (a reinterpreted format) refresh too */
    }
    return c;
}
static int widened(const Tex *t) { return t->rw && (float)t->rw * (float)t->b.height != (float)t->rh * (float)t->b.width; }
static void wide_crop_guard(void) {
    Tex *rt = S.color0;
    if (wide_k() == 1.0f || !g_wide_camera || !rt || widened(rt) || fabsf((float)rt->b.width * 9.0f / ((float)rt->b.height * 16.0f) - 1.0f) < 0.02f) return;
    for (int s = 0; s < 5; s++)
        for (uint32_t m = units_sampled(s), u = 0; m && u < 32; m >>= 1, u++) {
            Tex *t, *c;
            View v;
            SamplerParams p;
            if (!(m & 1) || !(t = unit_tex(s, u)) || t->dead || !widened(t)) continue;
            int hs = handle_lookup(S.tpool, S.spool, S.nvn_tex[s][u], &t, &v, &p);
            if (!t || t->dead || !(c = tex_crop(t))) continue;
            uint32_t dims[4] = {0, 0, 0, 0};
            int isint = 0;
            g_fb_saved[s][u] = S.gl_tex[s][4 + u];
            S.gl_tex[s][4 + u] = handle_make(c, v, p, hs, S.nvn_tex[s][u], dims, &isint);
            for (VNode *n = c->views; n; n = n->next) if (n->is_copy && n->copy_gen != c->rt_gen) view_copy_fill(c, n);
            g_fb_mask[s] |= 1u << u; S.tex_dirty[s] = 1; g_fb_active = 1;
        }
}
static void feedback_guard(void) {
    view_copies_refresh();
    feedback_restore();
    wide_crop_guard();
    if (!S.have_fbo) return;
    if (feedback_copy_mode() <= 0) {             /* GL's own rule for a draw that samples its render target: a texture barrier before it (TEST: -1 = only that) */
        static int said, said_copy;
        int barrier_done = 0;
        for (int s = 0; s < 5; s++) {
            for (uint32_t m = units_sampled(s), u = 0; m; m >>= 1, u++) {
                Tex *t, *sc;
                if (!(m & 1) || !(t = unit_tex(s, u)) || t->dead) continue;
                int col = 0, hit = t == S.depth_tex;
                for (int i = 0; i < 8 && !hit; i++) hit = col = t == S.color[i];
                if (!hit) continue;
                /* A 2D colour target its own draw samples: the draw reads a copy made just before it. The barrier orders draws
                 * only; texels the same draw writes are undefined to read in GL (the Mii face pictures, 128x128 with mips, are
                 * drawn this way every frame). The copies go with the spare targets (tex_take_spare), ~0.01 ms a frame. Cube
                 * and array mip chains (each level drawn from the one before) keep the barrier. (Tried 2026-11-01 for the
                 * centre line in Melissa's room: not its cause.) */
                if (col && t->b.target == 1 && !feedback_copy_mode() && (sc = tex_scratch(t)) != NULL) {
                    View v; SamplerParams p; Tex *t2;
                    int hs = handle_lookup(S.tpool, S.spool, S.nvn_tex[s][u], &t2, &v, &p);
                    uint32_t dims[4] = {0, 0, 0, 0};
                    int isint = 0;
                    g_fb_saved[s][u] = S.gl_tex[s][4 + u];
                    S.gl_tex[s][4 + u] = handle_make(sc, v, p, hs, S.nvn_tex[s][u], dims, &isint);
                    for (VNode *n = sc->views; n; n = n->next) if (n->is_copy && n->copy_gen != sc->rt_gen) view_copy_fill(sc, n);
                    g_fb_mask[s] |= 1u << u; S.tex_dirty[s] = 1; g_fb_active = 1;
                    if (!said_copy++) printf("nvn_gl: feedback: a draw samples its own render target (2D %dx%d format 0x%x): it samples a copy made before it\n", t->b.width, t->b.height, t->b.format);
                    continue;
                }
                if (!barrier_done && gl_has(glTextureBarrier, "glTextureBarrier")) { glTextureBarrier(); barrier_done = 1; }
                if (!said++) printf("nvn_gl: feedback: a draw samples its own render target (texture target %d %dx%d format 0x%x): a texture barrier before each such draw (TOMO_FEEDBACK_COPY=1 samples a copy instead)\n", t->b.target, t->b.width, t->b.height, t->b.format);
            }
        }
        return;
    }
    for (int s = 0; s < 5; s++) {
        for (unsigned u = 0; u < 32; u++) {
            uint64_t nh = S.nvn_tex[s][u];
            Tex *t;
            View v;
            SamplerParams p;
            if (!nh || (g_fb_mask[s] >> u & 1)) continue;      /* already swapped (wide_crop_guard) */
            int hs = handle_lookup(S.tpool, S.spool, nh, &t, &v, &p);
            if (!t || t->dead) continue;
            int hit = t == S.depth_tex;
            for (int i = 0; i < 8 && !hit; i++) hit = t == S.color[i];
            if (!hit) continue;
            Tex *sc = tex_scratch(t);
            if (!sc) continue;
            uint32_t dims[4] = {0, 0, 0, 0};
            int isint = 0;
            g_fb_saved[s][u] = S.gl_tex[s][4 + u];
            S.gl_tex[s][4 + u] = handle_make(sc, v, p, hs, nh, dims, &isint);
            g_fb_mask[s] |= 1u << u; S.tex_dirty[s] = 1; g_fb_active = 1;
        }
    }
}
/* One layer of an array / cube / 3D texture as a render target is attached by layer from the texture itself
 * (glFramebufferTextureLayer): a layered attachment next to the usual 2D depth target makes the framebuffer incomplete
 * (GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS). A view that changes the format, or one of several layers, keeps the view path. */
static int layered_rt(Tex *t, const View *v) {
    int tg = t->b.target;
    if (tg != 2 || t->b.samples > 1 || !t->o.gl) return 0;                     /* 3D textures only: arrays and cubes get a 2D view (rt_view) */
    if ((v->set & 0x84) == 0x84 && v->format && v->format != t->b.format) return 0;
    if ((v->set & 0x82) == 0x82 ? v->nlay > 1 : tex_layers(&t->b) > 1) return 0;
    return gl_has(glFramebufferTextureLayer, "glFramebufferTextureLayer");
}
/* One layer of an array / cube texture as a render target: a 2D (1D) view of that level and layer, its own texture object. The
 * game samples the whole cube map array while it draws its next mip level (a feedback loop in GL terms); drawn through the
 * texture itself (glFramebufferTextureLayer) that hung the GPU in the driver (TDR), through a view it does not. */
/* Rendering into a block-compressed texture through an uncompressed view of the block's size (BC3 as RGBA32UI: the game
 * compresses its Mii face textures on the GPU, one fragment = one 4x4 block). OpenGL has no such view ("internal formats
 * neither compatible nor identical": the framebuffer was incomplete, the texture stayed empty and Mii faces black). The
 * target is a scratch texture of the view's format, a quarter the size, and its texels are copied into the compressed
 * texture (glCopyImageSubData: an uncompressed texel of the block's size is one block) when the render targets change. */
typedef struct { Tex *t; int level, layer, w, h; GLuint gl; TexObj o; int pending; GLenum fmt, type, internal; int bpb; } BcShadow;
static BcShadow g_bcsh[64];
static int g_bcsh_n;
static LONG64 g_bc_made;                           /* scratches made (TOMO_RTSTAT) */
static unsigned g_bcsh_next;
static void bc_flush(void) {
    for (int i = 0; i < g_bcsh_n; i++) {
        BcShadow *b = &g_bcsh[i];
        if (!b->pending) continue;
        b->pending = 0;
        if (!b->t || b->t->dead || !b->t->o.gl || !gl_has(glCopyImageSubData, "glCopyImageSubData")) continue;
        GLenum tg = gl_target(b->t->b.target, b->t->b.samples);
        while (glGetError()) {}
        int tw = lvl(b->t->b.width, b->level), th = lvl(b->t->b.height, b->level);
        Fmt tf;
        if ((tw < 4 || th < 4) && tg == GL_TEXTURE_2D && b->bpb && fmt_get(b->t->b.format, &tf)) {
            /* a level smaller than one block: glCopyImageSubData refuses it (a block is 4x4 texels, the level is not); the block
             * goes through a buffer and is uploaded as compressed data of the level's real size. A buffer the GPU writes and
             * reads (pack, then unpack): read into memory, each such level waited for the GPU, 28-35 ms a frame while the title
             * made its Mii faces (2026-10-31). */
            static GLuint pbo;
            static unsigned slot;
            int n = b->w * b->h * b->bpb;
            if (n <= 64) {
                if (!pbo) { glCreateBuffers(1, &pbo); glNamedBufferData(pbo, 64 * 1024, NULL, 0x88EA /* DYNAMIC_COPY */); }
                uintptr_t off = (uintptr_t)(slot++ % 1024) * 64;
                glBindBuffer(0x88EB /* PIXEL_PACK_BUFFER */, pbo);
                glGetTextureSubImage(b->gl, 0, 0, 0, 0, b->w, b->h, 1, b->fmt, b->type, 64, (void *)off);
                glBindBuffer(0x88EB, 0);
                glBindBuffer(0x88EC /* PIXEL_UNPACK_BUFFER */, pbo);
                glBindTexture(GL_TEXTURE_2D, b->t->o.gl);
                glCompressedTexSubImage2D(GL_TEXTURE_2D, b->level, 0, 0, tw, th, tf.internal, n, (const void *)off);
                glBindBuffer(0x88EC, 0);
            }
        } else
        glCopyImageSubData(b->gl, GL_TEXTURE_2D, 0, 0, 0, 0, b->t->o.gl, tg, b->level, 0, 0, tg == GL_TEXTURE_2D ? 0 : b->layer, b->w, b->h, 1);
        GLenum e = glGetError();
        static int nlog;
        if (e) logf_limited(&nlog, 10, "nvn_gl: compressed render target: the copy into the texture failed (GL error 0x%x, level %d %dx%d blocks)\n", e, b->level, b->w, b->h);
    }
}
static TexObj *bc_target(Tex *t, const View *v) {     /* NULL: not a render target view of a compressed texture */
    Fmt tf, vf;
    if (!(v->set & 0x80) || !(v->set & 4) || !v->format || !fmt_get(t->b.format, &tf) || tf.kind != 4) return NULL;
    if (!fmt_get(v->format, &vf) || vf.kind != 0 || vf.bpb != tf.bpb) return NULL;
    int level = (v->set & 1) ? v->lvl0 : 0, layer = (v->set & 2) ? v->lay0 : 0;
    int w = (lvl(t->b.width, level) + 3) / 4, h = (lvl(t->b.height, level) + 3) / 4;
    for (int i = 0; i < g_bcsh_n; i++) {
        BcShadow *b = &g_bcsh[i];
        if (b->t == t && b->level == level && b->layer == layer && b->w == w && b->h == h) { b->pending = 1; return &b->o; }
    }
    /* Scratches are kept by format and size for the next (with their framebuffers): the title compresses 4 Mii faces x 9
     * levels every third frame, and making 36 scratches and their framebuffers anew each time held the render thread
     * 25-40 ms (2026-10-31). */
    static struct { GLuint gl; GLenum internal; int w, h; } pool[128];
    static int npool;
    BcShadow *b = g_bcsh_n < 64 ? &g_bcsh[g_bcsh_n++] : &g_bcsh[(g_bcsh_next++) & 63];
    if (b->gl) {
        if (npool == 128) { fbo_purge(pool[0].gl); glDeleteTextures(1, &pool[0].gl); memmove(&pool[0], &pool[1], sizeof pool[0] * 127); npool--; }
        pool[npool].gl = b->gl; pool[npool].internal = b->internal; pool[npool].w = b->w; pool[npool].h = b->h; npool++;
    }
    memset(b, 0, sizeof *b);
    b->t = t; b->level = level; b->layer = layer; b->w = w; b->h = h; b->fmt = vf.format; b->type = vf.type; b->bpb = tf.bpb; b->internal = vf.internal;
    for (int i = npool - 1; i >= 0 && !b->gl; i--)
        if (pool[i].internal == vf.internal && pool[i].w == w && pool[i].h == h) { b->gl = pool[i].gl; memmove(&pool[i], &pool[i + 1], sizeof pool[0] * (size_t)(npool - 1 - i)); npool--; }
    if (!b->gl) {
        glGenTextures(1, &b->gl);
        glBindTexture(GL_TEXTURE_2D, b->gl);
        glTexStorage2D(GL_TEXTURE_2D, 1, vf.internal, w, h);
        g_bc_made++;
    }
    b->o.gl = b->gl; b->o.alias = 1;                  /* not counted, not freed with the texture */
    b->pending = 1;
    static int nlog;
    logf_limited(&nlog, 5, "nvn_gl: compressed render target: texture format 0x%x %dx%d level %d drawn through a %dx%d scratch of format 0x%x\n", t->b.format, t->b.width, t->b.height, level, w, h, v->format);
    return &b->o;
}
static void bc_cache_selftest(void) {
    if (!getenv("SSBU_BC_CACHE_SELFTEST")) return;
    Tex targets[130] = {0};
    View view = {0};
    Fmt tf, vf;
    view.set = 0x84; view.format = 0x2f;
    fmt_get(0x48, &tf); fmt_get(view.format, &vf);
    int bad = 0, checked = 0;
    GLenum error = GL_NO_ERROR;
    for (int i = 0; i < 130; i++) {
        Tex *t = &targets[i];
        t->b.target = 1; t->b.width = t->b.height = 4; t->b.depth = t->b.levels = 1; t->b.format = 0x48;
        glGenTextures(1, &t->o.gl); glBindTexture(GL_TEXTURE_2D, t->o.gl);
        glTexStorage2D(GL_TEXTURE_2D, 1, tf.internal, 4, 4);
        TexObj *scratch = bc_target(t, &view);
        if (!scratch || g_bcsh_n > 64) { bad = 1; break; }  /* fail before an unbounded scan can read past the cache */
        uint32_t block[4] = {(uint32_t)i, 0x12345678, 0x87654321, 0xabcdef01};
        glClearTexImage(scratch->gl, 0, vf.format, vf.type, block);
        error = glGetError();
        if (error) { bad = 1; break; }
        bc_flush();
        checked++;
    }
    for (int i = 0; i < checked; i++) {
        uint32_t got[4] = {0}, expected[4] = {(uint32_t)i, 0x12345678, 0x87654321, 0xabcdef01};
        glGetCompressedTextureSubImage(targets[i].o.gl, 0, 0, 0, 0, 4, 4, 1, sizeof got, got);
        if (memcmp(got, expected, sizeof got)) bad = 1;
    }
    if (!bad) {
        int count = g_bcsh_n;
        TexObj *first = bc_target(&targets[129], &view), *again = bc_target(&targets[129], &view);
        if (!first || first != again || g_bcsh_n != count) bad = 1;
        bc_flush();
    }
    GLenum read_error = glGetError();
    if (read_error) { error = read_error; bad = 1; }
    printf("BC cache self-test: %d targets, %d cached, preserved compressed blocks and reused target, GL error 0x%x: %s\n",
           checked, g_bcsh_n, error, bad ? "FAIL" : "PASS");
    fflush(stdout); ExitProcess(bad ? 1 : 0);
}
static TexObj *rt_view(Tex *t, const View *v) {
    {   TexObj *o = bc_target(t, v); if (o) return o; }
    int tg = t->b.target;
    if (!(tg == 3 || tg == 4 || tg == 8 || tg == 9) || t->b.samples > 1) return tex_view_obj(t, v);
    if ((v->set & 0x82) == 0x82 ? v->nlay > 1 : tex_layers(&t->b) > 1) return tex_view_obj(t, v);      /* layered rendering */
    View v2 = *v;
    v2.set = (uint8_t)(0x80 | 1 | 2 | 32 | ((v->set & 0x84) == 0x84 ? 4 : 0));
    v2.target = tg == 3 ? 0 : 1;
    v2.lvl0 = (int16_t)((v->set & 0x81) == 0x81 ? v->lvl0 : 0); v2.nlvl = 1;
    v2.lay0 = (int16_t)((v->set & 0x82) == 0x82 ? v->lay0 : 0); v2.nlay = 1;
    return tex_view_obj(t, &v2);
}
static int g_rt0_layer, g_rt0_level, g_rt0_vset;      /* the draw log: layer / level of colour target 0 (-1: the whole texture or a view) */
/* Textures of one shape over the same memory: the game renders into one (made anew each frame) and samples another one it
 * keeps; on the console both are that memory. Here each has its own GL texture: the sampled one takes the texels of the
 * one rendered last before a draw reads it (alias_refresh). Seen: the 140x900 side panels of the residents list showed
 * another target's leftovers as coloured noise. */
static long long g_rt_tick;
static void rt_alias_note(Tex *t) {
    t->rt_tick = ++g_rt_tick;
    if (!t->mem || !t->storage) return;
    uint64_t begin = (uint64_t)(uintptr_t)t->mem;
    AcquireSRWLockShared(&g_tex_range_lock);
    for (size_t i = tex_range_lower(begin); i < g_tex_range_count && g_tex_ranges[i].begin == begin; i++) {
        Tex *o = g_tex_ranges[i].tex;
        if (o == t || o->dead || o->b.width != t->b.width || o->b.height != t->b.height || o->b.depth != t->b.depth || o->b.format != t->b.format
            || o->b.target != t->b.target || o->b.levels != t->b.levels || o->b.samples != t->b.samples) continue;
        o->alias_from = t; g_alias_any = 1;
    }
    ReleaseSRWLockShared(&g_tex_range_lock);
}
static void alias_refresh(Tex *t) {
    Tex *a = t->alias_from;
    if (!a || a->rt_tick <= t->alias_tick || a->rt_tick <= t->rt_tick || !a->o.gl || !t->o.gl || a->o.gl == t->o.gl) return;
    if (a == S.depth_tex) return;
    for (int i = 0; i < 8; i++) if (S.color[i] == a) return;           /* still being drawn: later */
    if (!gl_has(glCopyImageSubData, "glCopyImageSubData")) return;
    GLenum tg = gl_target(t->b.target, t->b.samples);
    int w = tw(t), h = th(t), d = tex_layers(&t->b);
    for (int l = 0; l < t->glevels && l < a->glevels; l++)
        glCopyImageSubData(a->o.gl, tg, l, 0, 0, 0, t->o.gl, tg, l, 0, 0, 0, lvl(w, l), lvl(h, l), t->b.target == 2 ? lvl(t->b.depth > 0 ? t->b.depth : 1, l) : d);
    t->alias_tick = a->rt_tick;
}
/* A texture's new GL storage starts with the texels of the newest picture drawn into a texture of its shape over the same
 * memory, as the console's memory would hold them. Seen: behind "Add a Mii" the game keeps a blurred copy of the frozen
 * island in a texture object it makes anew every few frames over the same memory; with fresh (empty) storage the menu's
 * background turned black (the older object, still named by the pool, kept its storage, so no same-memory spare existed). */
static void alias_inherit(Tex *t) {
    if (!t->mem || !t->storage || !t->o.gl || t->b.samples > 1) return;
    uint64_t begin = (uint64_t)(uintptr_t)t->mem;
    Tex *best = NULL;
    AcquireSRWLockShared(&g_tex_range_lock);
    for (size_t i = tex_range_lower(begin); i < g_tex_range_count && g_tex_ranges[i].begin == begin; i++) {
        Tex *o = g_tex_ranges[i].tex;
        if (o == t || !o->o.gl || o->o.gl == t->o.gl || !o->rt_tick || (best && o->rt_tick <= best->rt_tick)) continue;
        if (o->b.width != t->b.width || o->b.height != t->b.height || o->b.depth != t->b.depth || o->b.format != t->b.format
            || o->b.target != t->b.target || o->b.levels != t->b.levels || o->b.samples != t->b.samples || o->rw != t->rw) continue;
        best = o;
    }
    ReleaseSRWLockShared(&g_tex_range_lock);
    if (!best || !gl_has(glCopyImageSubData, "glCopyImageSubData")) return;
    GLenum tg = gl_target(t->b.target, t->b.samples);
    int w = tw(t), h = th(t), d = tex_layers(&t->b);
    for (int l = 0; l < t->glevels && l < best->glevels; l++)
        glCopyImageSubData(best->o.gl, tg, l, 0, 0, 0, t->o.gl, tg, l, 0, 0, 0, lvl(w, l), lvl(h, l), t->b.target == 2 ? lvl(t->b.depth > 0 ? t->b.depth : 1, l) : d);
    while (glGetError()) {}
    t->alias_tick = best->rt_tick; t->gpu_written = 1;
    {   static int nlog; logf_limited(&nlog, 5, "nvn_gl: new texture %dx%d format 0x%x takes the texels of the texture drawn last over its memory\n", t->b.width, t->b.height, t->b.format); }
}
/* Before the GL storage of a texture goes (finalized): the textures of its shape over the same memory that take their texels
 * from it (alias_from) take them now; later there is nothing to take them from. Seen: the game draws the island into a
 * texture made anew each frame and, behind its menus ("Add a Mii"), stops drawing it and shows a long-lived texture over the
 * same memory (the last picture on the console): it stayed black. */
static void alias_handoff(Tex *t) {
    Tex *to[16];
    int n = 0;
    if (!g_alias_any || !t->mem || !t->storage || !t->o.gl || !t->rt_tick) return;
    uint64_t begin = (uint64_t)(uintptr_t)t->mem;
    AcquireSRWLockShared(&g_tex_range_lock);
    for (size_t i = tex_range_lower(begin); i < g_tex_range_count && g_tex_ranges[i].begin == begin && n < 16; i++) {
        Tex *o = g_tex_ranges[i].tex;
        if (o != t && o->alias_from == t && o->o.gl && o->o.gl != t->o.gl) to[n++] = o;
    }
    ReleaseSRWLockShared(&g_tex_range_lock);
    for (int i = 0; i < n; i++) { alias_refresh(to[i]); to[i]->alias_from = NULL; }
}
/* Before a texture becomes a render target: the newest picture drawn into another texture of its shape over the same memory
 * comes into it (the game binds a second texture object over a depth buffer for the colour pass of a depth pre-pass; with
 * its own empty GL storage the colour pass's depth test for equality let nothing through: the new Mii stayed invisible). */
static void alias_pull(Tex *t) {
    if (!t->mem || !t->storage || !t->o.gl) return;
    uint64_t begin = (uint64_t)(uintptr_t)t->mem;
    Tex *best = NULL;
    long long mine = t->rt_tick > t->alias_tick ? t->rt_tick : t->alias_tick;
    AcquireSRWLockShared(&g_tex_range_lock);
    for (size_t i = tex_range_lower(begin); i < g_tex_range_count && g_tex_ranges[i].begin == begin; i++) {
        Tex *o = g_tex_ranges[i].tex;
        if (o == t || !o->o.gl || o->o.gl == t->o.gl || o->rt_tick <= mine || (best && o->rt_tick <= best->rt_tick)) continue;
        if (o->b.width != t->b.width || o->b.height != t->b.height || o->b.depth != t->b.depth || o->b.format != t->b.format
            || o->b.target != t->b.target || o->b.levels != t->b.levels || o->b.samples != t->b.samples) continue;
        best = o;
    }
    ReleaseSRWLockShared(&g_tex_range_lock);
    if (!best || !gl_has(glCopyImageSubData, "glCopyImageSubData")) return;
    GLenum tg = gl_target(t->b.target, t->b.samples);
    int w = tw(t), h = th(t), d = tex_layers(&t->b);
    for (int l = 0; l < t->glevels && l < best->glevels; l++)
        glCopyImageSubData(best->o.gl, tg, l, 0, 0, 0, t->o.gl, tg, l, 0, 0, 0, lvl(w, l), lvl(h, l), t->b.target == 2 ? lvl(t->b.depth > 0 ? t->b.depth : 1, l) : d);
    t->alias_tick = best->rt_tick;
    {   static int nlog; logf_limited(&nlog, 5, "nvn_gl: render target %dx%d format 0x%x takes the picture of the texture drawn last over its memory (another texture object)\n", t->b.width, t->b.height, t->b.format); }
}
static int g_dbg_noscissor;                       /* SSBU_NOSCISSOR (below) */
/* Resolution scale: viewports and scissors are kept in the game's pixels and set on the bound targets' storage. */
static void apply_vp(int i) {
    float s = S.prs ? S.prs : 1.0f, sy = S.prsy ? S.prsy : 1.0f;
    glViewportIndexedf((GLuint)i, S.gvp[i][0] * s, S.gvp[i][1] * sy, S.gvp[i][2] * s, S.gvp[i][3] * sy);
}
static void apply_sc(int i) {
    const int *v = S.gsc[i];
    if (!S.prs || v[2] >= 16384) { glScissorIndexed((GLuint)i, v[0], v[1], v[2], v[3]); return; }
    int x0 = (int)floorf((float)v[0] * S.prs + 0.5f), y0 = (int)floorf((float)v[1] * S.prsy + 0.5f);
    glScissorIndexed((GLuint)i, x0, y0, (int)floorf((float)(v[0] + v[2]) * S.prs + 0.5f) - x0, (int)floorf((float)(v[1] + v[3]) * S.prsy + 0.5f) - y0);
}
static void rs_pass(Tex *t) {                         /* the scale of the targets now bound (their first): viewports and scissors follow */
    float s = t && t->rw ? (float)t->rw / (float)t->b.width : 0.0f, sy = t && t->rh ? (float)t->rh / (float)t->b.height : 0.0f;
    if (s == S.prs && sy == S.prsy) return;
    S.prs = s; S.prsy = sy;
    for (int i = 0; i < 16; i++) { if (S.gvp_set >> i & 1) apply_vp(i); if (!g_dbg_noscissor) apply_sc(i); }
}
static void set_targets(int n, const TargetRef *col, const TargetRef *depth) {
    FboKey k;
    Fmt f;
    if (g_bcsh_n) { HS_IN("render targets: compressed targets copied in (bc_flush)", 0); bc_flush(); HS_OUT(); }      /* what was drawn into compressed textures goes into them now */
    memset(&k, 0, sizeof k);
    S.have_fbo = 0; S.color0 = NULL; S.depth_tex = NULL; S.fbo_w = S.fbo_h = 0;
    memset(S.color, 0, sizeof S.color);
    HS_IN("render targets: textures (made, spare, cleared, aliases pulled, views)", 0);
    for (int i = 0; i < n && i < 8; i++) {
        Tex *t = col[i].tex;
        if (!t || t->dead) continue;
        tex_sync(t);
        alias_pull(t);
        t->gpu_written = 1; t->rt_gen++; rt_alias_note(t);
        k.layer[i] = -1;
        if (layered_rt(t, &col[i].view)) {
            k.color[i] = t->o.gl;
            k.layer[i] = (col[i].view.set & 0x82) == 0x82 ? col[i].view.lay0 : 0;
            k.level[i] = (col[i].view.set & 0x81) == 0x81 ? col[i].view.lvl0 : 0;
        } else k.color[i] = rt_view(t, &col[i].view)->gl;
        k.n = i + 1;
        if (!S.fbo_w) { int l = (col[i].view.set & 0x81) == 0x81 ? col[i].view.lvl0 : 0; S.fbo_w = lvl(t->b.width, l); S.fbo_h = lvl(t->b.height, l); }
        if (i == 0) S.color0 = t;
        S.color[i] = t;
    }
    if (depth && depth->tex && !depth->tex->dead) {
        Tex *t = depth->tex;
        S.depth_tex = t;
        tex_sync(t);
        alias_pull(t);
        t->gpu_written = 1; t->rt_gen++; rt_alias_note(t);
        k.dlayer = -1;
        if (layered_rt(t, &depth->view)) {
            k.depth = t->o.gl;
            k.dlayer = (depth->view.set & 0x82) == 0x82 ? depth->view.lay0 : 0;
            k.dlevel = (depth->view.set & 0x81) == 0x81 ? depth->view.lvl0 : 0;
        } else k.depth = rt_view(t, &depth->view)->gl;
        k.dkind = fmt_get(t->b.format, &f) ? f.kind : 1;
        if (!S.fbo_w) { S.fbo_w = t->b.width; S.fbo_h = t->b.height; }
    }
    HS_OUT();
    g_rt0_layer = k.layer[0]; g_rt0_level = k.level[0]; g_rt0_vset = n ? col[0].view.set : 0;
    if (rtlog_match(S.color0)) printf("rtlog: frame %lld gl %u mem %p born %lld\n", (long long)g_st.frames + 1, S.color0->o.gl, (void *)S.color0->mem, S.color0->born);      /* TOMO_RTLOG */
    if (rs_on()) rs_pass(S.color0 ? S.color0 : S.depth_tex);
    if (!k.n && !k.depth) { glBindFramebuffer(GL_FRAMEBUFFER_, 0); return; }
    g_fbo_bad_last = 0;
    { HS_IN("render targets: framebuffer (found or made)", 0); S.fbo = fbo_get(&k); HS_OUT(); }
    if (g_fbo_bad_last) {
        static int nlog;
        for (int i = 0; i < k.n; i++) if (S.color[i]) logf_limited(&nlog, 40, "nvn_gl:   colour %d: texture target %d format 0x%x %dx%d levels %d samples %d flags 0x%x gl %u\n", i,
            S.color[i]->b.target, S.color[i]->b.format, S.color[i]->b.width, S.color[i]->b.height, S.color[i]->b.levels, S.color[i]->b.samples, S.color[i]->b.flags, k.color[i]);
        if (depth && depth->tex) logf_limited(&nlog, 40, "nvn_gl:   depth: texture target %d format 0x%x %dx%d levels %d samples %d flags 0x%x gl %u (view set 0x%x)\n",
            depth->tex->b.target, depth->tex->b.format, depth->tex->b.width, depth->tex->b.height, depth->tex->b.levels, depth->tex->b.samples, depth->tex->b.flags, k.depth, depth->view.set);
    }
    S.have_fbo = 1;
    glBindFramebuffer(GL_FRAMEBUFFER_, S.fbo);
}

/* Everything a draw needs that is not plain GL state: programs, their buffers, texture handles, vertex streams. */
static GLuint g_tex_buf[6];                       /* the buffer each stage's handle table went into (S.tex_off) */
static uint32_t g_tex_tag[6];                     /* and where in it (arena_tag_ok) */
static void bind_handle_table(int s, GLenum target, GLuint index, int skip) {
    if (S.tex_dirty[s] || S.tex_gen[s] != g_arena_gen || !arena_tag_ok(g_tex_tag[s])) {
        S.tex_off[s] = arena_put_raw(S.gl_tex[s], sizeof S.gl_tex[s]);
        S.tex_gen[s] = g_arena_gen; S.tex_dirty[s] = 0; g_tex_buf[s] = g_arena_buf; g_tex_tag[s] = g_arena_tag;
    }
    if (target == GL_SHADER_STORAGE_BUFFER_) gs_ssbo(index, g_tex_buf[s], (ptrdiff_t)S.tex_off[s] + skip, skip ? (ptrdiff_t)256 : (ptrdiff_t)sizeof S.gl_tex[s]);
    else if (target == GL_UNIFORM_BUFFER_) gs_ubo(index, g_tex_buf[s], (ptrdiff_t)S.tex_off[s] + skip, skip ? (ptrdiff_t)256 : (ptrdiff_t)sizeof S.gl_tex[s]);
    else glBindBufferRange(target, index, g_tex_buf[s], (ptrdiff_t)S.tex_off[s] + skip, skip ? (ptrdiff_t)256 : (ptrdiff_t)sizeof S.gl_tex[s]);
}
/* Debug aid SSBU_DRAWLOG=<frame>: state of every draw of that frame, and how many samples each draw produced. */
static LONG64 g_prev_frame_draws, g_drawlog_frame = -1, g_disp_frames, g_frame_dispatch;
static int g_drawlog_mindisp_n;     /* with SSBU_DRAWLOG_MIN=<n>: the first frame from <frame> on that follows a frame of at least n draws */
static LONG64 g_dl_force[2] = {-1, -1};      /* TOMO_FLICKER_PERIOD: the frames predicted to flicker, and the one before */
static int g_dlrt_n;                        /* draws logged by SSBU_DRAWLOG_RT */
/* SSBU_DRAWLOG_RT=<w>x<h>: the draws into a colour target of that size, in any frame (at most SSBU_DRAWLOG_RT_MAX, 300);
 * SSBU_DRAWLOG_RT_SHOT=1: also the target after each of them (dlrt_<n>.bmp and its alpha, dlrt_<n>_a.bmp) */
static int dlrt_match(void) {
    static int rw = -2, rh, rmax, rf = -1;      /* <w>x<h>[:<format hex>] */
    if (rw == -2) { const char *e = getenv("SSBU_DRAWLOG_RT"); rw = -1; if (e && sscanf(e, "%dx%d", &rw, &rh) != 2) rw = -1; if (e && strchr(e, ':')) rf = (int)strtol(strchr(e, ':') + 1, NULL, 16); rmax = getenv("SSBU_DRAWLOG_RT_MAX") ? atoi(getenv("SSBU_DRAWLOG_RT_MAX")) : 300; }
    static long long from = -2;                 /* SSBU_DRAWLOG_RT_FROM=<frame>: not before that frame */
    if (from == -2) from = getenv("SSBU_DRAWLOG_RT_FROM") ? atoll(getenv("SSBU_DRAWLOG_RT_FROM")) : 0;
    return rw > 0 && (long long)g_st.frames + 1 >= from && S.color0 && S.color0->b.width == rw && S.color0->b.height == rh && (rf < 0 || S.color0->b.format == rf) && g_dlrt_n < rmax;
}
static int drawlog_now(void) {
    static int drawlog = -2, mindraws, mindisp;
    if (g_dl_force[0] >= 0 && (g_st.frames == g_dl_force[0] || g_st.frames == g_dl_force[1])) return 1;
    if (drawlog == -2) {
        drawlog = getenv("SSBU_DRAWLOG") ? atoi(getenv("SSBU_DRAWLOG")) : -1; mindraws = getenv("SSBU_DRAWLOG_MIN") ? atoi(getenv("SSBU_DRAWLOG_MIN")) : 0;
        mindisp = getenv("SSBU_DRAWLOG_MINDISP") ? atoi(getenv("SSBU_DRAWLOG_MINDISP")) : 0;
    }
    if (dlrt_match()) return 1;
    if (drawlog < 0) return 0;
    if (mindisp) {                             /* SSBU_DRAWLOG_MINDISP=<n>: SSBU_DRAWLOG then counts the frames that had at least n compute dispatches */
        if (g_drawlog_frame < 0 && g_disp_frames >= drawlog) g_drawlog_frame = g_st.frames;
        return g_st.frames == g_drawlog_frame;
    }
    if (!mindraws) return g_st.frames == drawlog;
    if (g_drawlog_frame < 0 && g_st.frames >= drawlog && g_prev_frame_draws >= mindraws) g_drawlog_frame = g_st.frames;
    return g_st.frames == g_drawlog_frame;
}
static GLuint g_dbg_query;
static struct { GLuint q; int active; uint64_t acc; } g_counter[17];
static struct { uint64_t addr; long long frame; } g_tsrep[256];      /* TIMESTAMP_TOP reports: where, in which frame (OP_RENDER_ENABLE) */
static void drawlog_begin(void) { if (!drawlog_now() || g_counter[1].active) return; if (!g_dbg_query) glGenQueries(1, &g_dbg_query); glBeginQuery(0x8914, g_dbg_query); }
static void drawlog_end(void) {
    GLuint n = 0;
    if (!drawlog_now() || g_counter[1].active) return;
    glEndQuery(0x8914);
    glGetQueryObjectuiv(g_dbg_query, 0x8866, &n);
    printf("  -> %u samples passed\n", n);
}
static int g_dbg_flipface;
static struct { uint64_t addr, size; int prog, n; } g_crange[1024];       /* storage ranges written by this frame's compute dispatches */
static int g_ncrange;
static int crange_hit(uint64_t addr, uint64_t size);
static void compute_flush_at(int line);
#define compute_flush() compute_flush_at(__LINE__)   /* the line: TOMO_LISTLOG counts read-backs by call site */
static struct { int line; LONG64 n; } g_cfl_site[32];
typedef struct { uint64_t addr, size; GLuint buf; int dirty, live, frag; uint8_t *snap; } CBuf;   /* frag: written by a draw's fragment stage only; snap: guest memory when it went to GL (gpu_merge) */
static CBuf g_cbuf[64];
static int g_frag_ssbo_pending;               /* a draw's fragment stage wrote a storage buffer since the last storage barrier */
static int g_cbuf_live;
static CBuf *cbuf_find(uint64_t addr, uint64_t size);
static CBuf *cbuf_get(uint64_t addr, uint64_t size);
/* A vertex stream that lies inside a buffer the GPU wrote in this batch (still held in GL) is read from that GL buffer: it
 * used to go back to guest memory first (compute_flush: a wait for the GPU) and be streamed in again; some island scenes
 * did that 11 times a frame (3.7-4.3 ms of the render thread waiting, 2026-10-31). TOMO_VB_DIRECT=0: as before. */
static int vb_direct_on(void) { static int on = -1; if (on < 0) on = !(getenv("TOMO_VB_DIRECT") && getenv("TOMO_VB_DIRECT")[0] == '0'); return on; }
/* (Asked for every vertex stream of every draw while one is live: 5 % of the render thread at the title. The span of the live
 * ones and the highest slot used answer most of them at once.) */
static uint64_t g_cbuf_lo = ~0ull, g_cbuf_hi;
static int g_cbuf_top;
static void cbuf_none(void) { g_cbuf_live = 0; g_cbuf_lo = ~0ull; g_cbuf_hi = 0; g_cbuf_top = 0; }
static CBuf *cbuf_holding(uint64_t addr, uint64_t size) {
    if (addr < g_cbuf_lo || addr + size > g_cbuf_hi) return NULL;
    for (int k = 0; k < g_cbuf_top; k++) if (g_cbuf[k].live && addr >= g_cbuf[k].addr && addr + size <= g_cbuf[k].addr + g_cbuf[k].size) return &g_cbuf[k];
    return NULL;
}
static int g_dbg_noscissor, g_dbg_skipdraw = -1, g_frame_draw_index;
static void snaplog_where(char *buf, size_t n) { snprintf(buf, n, "draw %d fbo %u (%dx%d)", g_frame_draw_index, S.fbo, S.fbo_w, S.fbo_h); }      /* debug aids: SSBU_NOSCISSOR=1, SSBU_SKIPDRAW=<n-th draw of every frame> */
/* Audit measurement (fake audit, row "unbound or dead texture slots sample a 1x1 dummy texture"): draws whose bound
 * programs reference a texture unit (MxOut.tex_units of the translated stage) that holds the dummy texture; split by cause:
 * the game bound handle 0 to the unit / never bound it, or the handle names a texture that is gone or was never registered. */
static long long g_dummy_draws, g_dummy_unbound, g_dummy_dead;
/* TEST ONLY, TOMO_DRAWSIG=<from>-<to>: one line per presented frame with a signature of each draw (programs, index count,
 * target size, size and format of each texture the fragment stage samples): a one-frame flash shows as the draws whose
 * signature differs from the frames around it (texture identities change every frame here, their shapes do not). */
static uint32_t *g_dsig; static int g_dsig_n, g_dsig_cap;
static long long g_dsig_lo = -2, g_dsig_hi;
static uint32_t fnv(uint32_t h, uint32_t v) { for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 255; h *= 16777619u; } return h; }
static void drawsig_note(void) {
    if (g_dsig_lo == -2) { const char *e = getenv("TOMO_DRAWSIG"); g_dsig_lo = -1; if (e) sscanf(e, "%lld-%lld", &g_dsig_lo, &g_dsig_hi); }
    if (g_dsig_lo < 0 || (long long)g_st.frames + 1 < g_dsig_lo || (long long)g_st.frames + 1 > g_dsig_hi) return;
    uint32_t h = 2166136261u;
    h = fnv(h, S.prog[0] ? (uint32_t)S.prog[0]->serial : 0); h = fnv(h, S.prog[1] ? (uint32_t)S.prog[1]->serial : 0);
    h = fnv(h, g_fd_count); h = fnv(h, (uint32_t)S.fbo_w << 16 | (uint32_t)S.fbo_h);
    for (uint32_t m = units_sampled(1), u = 0; m; m >>= 1, u++) {
        Tex *t;
        if (!(m & 1)) continue;
        t = unit_tex(1, u);
        h = fnv(h, t ? (uint32_t)t->b.width << 16 ^ (uint32_t)t->b.height ^ (uint32_t)t->b.format << 24 : 0xffffffffu);
        h = fnv(h, S.gl_tex[1][4 + u] == g_dummy_handle);
    }
    if (g_dsig_n + 1 >= g_dsig_cap) { g_dsig_cap = g_dsig_cap ? g_dsig_cap * 2 : 4096; g_dsig = realloc(g_dsig, sizeof *g_dsig * (size_t)g_dsig_cap); }
    g_dsig[g_dsig_n++] = h;
    g_dsig[g_dsig_n++] = (uint32_t)S.fbo_w << 16 | (uint32_t)S.fbo_h;      /* (printed with it: the target's size) */
}
static void drawsig_frame(long long frame) {             /* job_present */
    if (g_dsig_lo < 0 || !g_dsig_n) { g_dsig_n = 0; return; }
    printf("drawsig %lld %d:", frame, g_dsig_n / 2);
    for (int i = 0; i + 1 < g_dsig_n; i += 2) printf(" %08x@%ux%u", g_dsig[i], g_dsig[i + 1] >> 16, g_dsig[i + 1] & 0xffff);
    printf("\n");
    g_dsig_n = 0;
}
static void count_dummy_samples(void) {
    drawsig_note();
    {   /* TOMO_GLERR_DRAW=1 (diagnosis): a GL error after a draw, with the framebuffer it went to */
        static int on = -1, nlog;
        if (on < 0) on = getenv("TOMO_GLERR_DRAW") != NULL;
        GLenum e;
        if (on && (e = glGetError()) != 0 && nlog++ < 40) {
            GLint fb = 0; glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
            GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER_);
            printf("glerr: GL error 0x%x after draw %d of frame %lld (fs #%d): framebuffer %d status 0x%x, colour target %s%dx%d format 0x%x gl %u, depth %s%dx%d format 0x%x gl %u\n", e, g_frame_draw_index,
                   (long long)g_st.frames + 1, S.prog[1] ? S.prog[1]->serial : -1, fb, st, S.color0 && S.color0->dead ? "DEAD " : "", S.color0 ? S.color0->b.width : 0, S.color0 ? S.color0->b.height : 0,
                   S.color0 ? S.color0->b.format : 0, S.color0 ? S.color0->o.gl : 0, S.depth_tex && S.depth_tex->dead ? "DEAD " : "", S.depth_tex ? S.depth_tex->b.width : 0, S.depth_tex ? S.depth_tex->b.height : 0,
                   S.depth_tex ? S.depth_tex->b.format : 0, S.depth_tex ? S.depth_tex->o.gl : 0);
            while (glGetError()) {}
        }
    }
    int hit = 0;
    for (int s = 0; s < 6; s++) {
        uint32_t m = S.prog[s] ? S.prog[s]->st[s].glsl_tex : 0;
        for (int u = 0; m && u < 32; u++)
            if ((m >> u & 1) && S.gl_tex[s][4 + u] == g_dummy_handle) { hit = 1; if (S.nvn_tex[s][u]) g_dummy_dead++; else g_dummy_unbound++; }
    }
    g_dummy_draws += hit;
}
#define PP(k) do { if (g_pp_on > 0) { LONG64 n_ = pf_now(); g_pp[k] += n_ - pp_t; pp_t = n_; } } while (0)
static Range g_draw_ibuf;                     /* the index buffer of the draw being prepared (none for DrawArrays) */
/* Frozen scenes. From the island-name screen on, the game stops drawing the island and draws its dialogs and menus over the
 * last island picture left in the screen target, every frame, never clearing it. Drawn again and again over that picture,
 * the translucent parts piled up here: the soft black cloud around a dialog hardened into streaks as it wobbled in, and the
 * dimming behind "Add a Mii" turned the island black within a few frames. The console does not show that. So: a
 * screen-sized target that gets 3D draws (with a depth target) is copied at its first draw without one in that frame (the
 * picture before the 2D on top); in a later frame with no 3D draws, the copy goes back into it before its first draw.
 * TOMO_FREEZE_RESTORE=0 turns this off.
 * 2026-10-30: the restore came at a frame's first 2D draw, before it was known whether 3D followed. A target whose frames
 * draw 2D before their 3D (the sky first) got the older picture under that 2D every frame, and the background and the
 * shadows flickered (the user's recording of a Mii by the sea: the sky in two colours, every third frame). Now a frame
 * right after a 3D frame that began with 2D is not restored; once a whole frame has passed without 3D, it is. */
static LONG64 g_frz_rest, g_frz_wrong, g_frz_skip;            /* restores, restores followed by 3D in the same frame, restores left out */
static void frozen_scene(int has_depth) {
    static int on = -1;
    if (on < 0) on = !getenv("TOMO_FREEZE_RESTORE") || atoi(getenv("TOMO_FREEZE_RESTORE"));
    Tex *t = S.color0;
    if (!on || !t || t->dead || !t->o.gl || t->b.target != 1 || t->b.samples > 1 || t->b.width < 1280 || t->b.height < 720 || g_rt0_level > 0) return;
    for (int i = 1; i < 8; i++) if (S.color[i]) return;
    long long frame = (long long)g_st.frames + 1;
    if (has_depth) {
        if (t->frz_depth != frame && t->frz_2d == frame) t->frz_lead2d = frame;      /* this frame's 3D comes after 2D draws */
        if (t->frz_depth != frame && t->frz_rf == frame) g_frz_wrong++;
        t->frz_depth = frame; return;
    }
    if (!t->frz_depth) return;
    if (t->frz_depth != frame) t->frz_2d = frame;
    Fmt f;
    if (!gl_has(glCopyImageSubData, "glCopyImageSubData") || !fmt_get(t->b.format, &f) || f.kind != 0) return;
    if (t->frz_depth == frame) {                       /* the 3D of this frame is done: keep it */
        if (t->frz_snapf == frame) return;
        if (!t->frz_snap) {
            const TexFormat *tf = tex_format(t->b.format);
            glGenTextures(1, &t->frz_snap);
            glBindTexture(GL_TEXTURE_2D, t->frz_snap);
            glTexStorage2D(GL_TEXTURE_2D, 1, tf && tf->sw_decode && tf->gl_sw_internal ? tf->gl_sw_internal : f.internal, tw(t), th(t));
        }
        glCopyImageSubData(t->o.gl, GL_TEXTURE_2D, 0, 0, 0, 0, t->frz_snap, GL_TEXTURE_2D, 0, 0, 0, 0, tw(t), th(t), 1);
        t->frz_snapf = frame;
        return;
    }
    if (t->frz_snap && t->frz_snapf == t->frz_depth && t->frz_restf != frame && t->frz_lead2d == t->frz_depth && t->frz_depth == frame - 1) {
        if (t->frz_restf != -frame) { g_frz_skip++; t->frz_restf = -frame; }      /* (counted once a frame) */
        return;
    }
    if (t->frz_snap && t->frz_snapf == t->frz_depth && t->frz_restf != frame) {   /* no 3D this frame (so far): the last 3D picture back */
        g_frz_rest++;
        glCopyImageSubData(t->frz_snap, GL_TEXTURE_2D, 0, 0, 0, 0, t->o.gl, GL_TEXTURE_2D, 0, 0, 0, 0, tw(t), th(t), 1);
        t->frz_restf = frame; t->frz_rf = frame;
        if (drawlog_now()) printf("frozen scene: gl %u (%dx%d) gets the 3D picture of frame %lld back%c", t->o.gl, t->b.width, t->b.height, t->frz_depth, 10);
        static int said; if (!said++) printf("nvn_gl: frozen scene: the target %dx%d (gl %u) gets its last 3D picture back each frame (TOMO_FREEZE_RESTORE=0: off)\n", t->b.width, t->b.height, t->o.gl);
    }
}
static int draw_prepare(void) {
    static int nlog;
    uint32_t ints = 0, sints = 0;
    LONG64 pp_t = 0;
    if (g_pp_on < 0) g_pp_on = getenv("TOMO_PREPPROF") != NULL;
    if (g_pp_on > 0) pp_t = pf_now();
    GLuint pn_vs = 0, pn_fs = 0;                /* the pair of GLSL stage programs of this draw (prec_note) */
    int pn_empty = 0;
    if (g_cbuf_live) {   /* storage buffers written on the GPU (dispatches, fragment stages) go back to guest memory before a draw only when
         * the draw streams that memory from the guest (vertex, index, uniform, storage buffers); otherwise at the next point
         * that needs guest memory (a report, a copy, the end of the batch). A read-back stalls on the GPU: flushing before
         * every first draw after a dispatch cost 4 ms a frame on the island. */
        int hit = 0;
        uint32_t vm = 0, um[5], sm[5];               /* only what the draw reads: streams of enabled attributes, buffers the programs use */
        for (int i = 0; i < 16 && i < S.nattr; i++) if (S.vattr[i] & 0xff) vm |= 1u << ((S.vattr[i] >> 8) & 31);
        for (int st = 0; st < 5; st++) {
            Prog *p = S.prog[st];
            um[st] = !p ? 0 : p->st[st].glsl_state == 1 ? (p->st[st].glsl_cbufs >> 3) & 0xffff : 0xffff;
            sm[st] = !p ? 0 : p->st[st].glsl_state == 1 ? p->st[st].glsl_ssbos & 0xffff : 0xffff;
        }
        #define OVL(r) ((r).addr && (r).addr < a + z && a < (r).addr + (r).size)
        for (int k = 0; k < 64 && !hit; k++) {
            if (!g_cbuf[k].live) continue;
            uint64_t a = g_cbuf[k].addr, z = g_cbuf[k].size;
            hit = OVL(g_draw_ibuf);
            for (uint32_t m = vm, i = 0; m && !hit; m >>= 1, i++) { hit = (m & 1) && OVL(S.vbuf[i]) && !(vb_direct_on() && S.vbuf[i].addr >= a && S.vbuf[i].addr + S.vbuf[i].size <= a + z); }
            for (int st = 0; st < 5 && !hit; st++) {
                for (uint32_t m = um[st], n = 0; m && !hit; m >>= 1, n++) { hit = (m & 1) && OVL(S.ubo[st][n]); }
                for (uint32_t m = sm[st], n = 0; m && !hit; m >>= 1, n++) { hit = (m & 1) && OVL(S.ssbo[st][n]) && !(S.ssbo[st][n].addr == a && S.ssbo[st][n].size == z); }
            }
        }
        #undef OVL
        if (hit && g_listlog_dbg) {         /* TOMO_LISTLOG: which reads of GPU-held buffers flush */
            static int nlog;
            if (nlog++ < 40) {
                for (int k = 0; k < 64; k++) {
                    if (!g_cbuf[k].live) continue;
                    uint64_t a = g_cbuf[k].addr, z = g_cbuf[k].size;
                    #define OV2(r) ((r).addr && (r).addr < a + z && a < (r).addr + (r).size)
                    if (OV2(g_draw_ibuf)) printf("cflushlog: index buffer %llx+%llu in buffer %llx+%llu (frag %d)\n", (unsigned long long)g_draw_ibuf.addr, (unsigned long long)g_draw_ibuf.size, (unsigned long long)a, (unsigned long long)z, g_cbuf[k].frag);
                    for (int i = 0; i < 32; i++) if (OV2(S.vbuf[i])) printf("cflushlog: vertex buffer %d %llx+%llu in buffer %llx+%llu (frag %d)\n", i, (unsigned long long)S.vbuf[i].addr, (unsigned long long)S.vbuf[i].size, (unsigned long long)a, (unsigned long long)z, g_cbuf[k].frag);
                    for (int st = 0; st < 5; st++) for (int n = 0; n < 16; n++) {
                        if (OV2(S.ubo[st][n])) printf("cflushlog: stage %d uniform %d %llx+%llu in buffer %llx+%llu (frag %d) program #%d\n", st, n, (unsigned long long)S.ubo[st][n].addr, (unsigned long long)S.ubo[st][n].size, (unsigned long long)a, (unsigned long long)z, g_cbuf[k].frag, S.prog[st] ? S.prog[st]->serial : -1);
                        if (OV2(S.ssbo[st][n]) && !(S.ssbo[st][n].addr == a && S.ssbo[st][n].size == z)) printf("cflushlog: stage %d storage %d %llx+%llu in buffer %llx+%llu (frag %d)\n", st, n, (unsigned long long)S.ssbo[st][n].addr, (unsigned long long)S.ssbo[st][n].size, (unsigned long long)a, (unsigned long long)z, g_cbuf[k].frag);
                    }
                    #undef OV2
                }
            }
        }
        if (hit) compute_flush();
    }
    PP(0);
    if (!S.have_fbo || S.render_off) return 0;
    if (g_frame_draw_index++ == g_dbg_skipdraw) return 0;
    {   /* debug aid TOMO_SKIPPROG=<tag>[,...]: the draws whose fragment stage has that code tag (draw log "tag", the same in
         * every run; program numbers are not) are left out: which pass draws what */
        static uint32_t skip[16]; static int nskip = -1;
        if (nskip < 0) { const char *e = getenv("TOMO_SKIPPROG"); nskip = 0; for (char *p = (char *)e; p && *p && nskip < 16; ) { skip[nskip++] = (uint32_t)strtoul(p, &p, 16); while (*p == ',') p++; } }
        for (int i = 0; i < nskip; i++) if (S.prog[1] && code_tag(&S.prog[1]->st[1]) == skip[i]) return 0;
    }
    gs_mode();
    frozen_scene(S.depth_tex != NULL);
    for (int i = 0; i < 16 && i < S.nattr; i++) {
        int size, norm, integer = 0;
        GLenum type;
        if ((S.vattr[i] & 0xff) && vfmt_get((int)(S.vattr[i] & 0xff), &size, &type, &norm, &integer) && integer) {
            ints |= 1u << i;
            if (type == 0x1400 || type == 0x1402 || type == 0x1404 || type == 0x8D9F) sints |= 1u << i;
        }
    }
    uint32_t key0[4] = {ints, sints, S.tex_int[0], S.tex_sint[0]}, key1[4] = {0, 0, S.tex_int[1], S.tex_sint[1]};
    /* preferred: both stages translated from machine code to GLSL (separable programs in one pipeline) */
    /* A program with a vertex stage only is a depth-only pass (shadow maps, depth pre-pass): NVN then writes depth and
     * stencil and no colour. Here it runs with an empty fragment shader. */
    int depth_only = S.prog[0] && !S.prog[1];
    PP(1);
    if (S.prog[0] && !S.prog[2] && !S.prog[3] && !S.prog[4] && stage_glsl(S.prog[0], 0, key0) && (depth_only || stage_glsl(S.prog[1], 1, key1))) {
        static GLuint empty_fs;
        PP(2);
        if (depth_only && !empty_fs) {
            static const char *const src[1] = {"#version 450\nvoid main() {}\n"};
            empty_fs = glCreateShaderProgramv(0x8B30, 1, src);
            printf("nvn_gl: first draw with a vertex stage only (program #%d): depth-only pass, empty fragment shader %u\n", S.prog[0]->serial, empty_fs);
        }
        if (g_gs_arb_on || !(g_gs_on & 2)) { for (int s = 0; s < 5; s++) glDisable(k_prog_target[s]); g_gs_arb_on = 0; }
        GLuint pair = pair_mode() ? pair_get(S.prog[0]->st[0].glsl, depth_only ? empty_fs : S.prog[1]->st[1].glsl, depth_only ? "#version 450\nvoid main() {}\n" : NULL) : 0;
        if (pair) { if (g_pair_cur != pair) { glUseProgram(pair); g_pair_cur = pair; } g_pair_draws++; }
        else {
        pair_off();
        GLuint fsp = depth_only ? empty_fs : S.prog[1]->st[1].glsl;
        if (!g_gs_pipe || !(g_gs_on & 2)) { glBindProgramPipeline(g_pipeline); g_gs_pipe = 1; g_gs_vs = g_gs_fs = 0; }
        if (g_gs_vs != S.prog[0]->st[0].glsl) { glUseProgramStages(g_pipeline, 1, S.prog[0]->st[0].glsl); g_gs_vs = S.prog[0]->st[0].glsl; }
        if (g_gs_fs != fsp) { glUseProgramStages(g_pipeline, 2, fsp); g_gs_fs = fsp; }
        }
        {   GLuint now[2] = {S.prog[0]->st[0].glsl, depth_only ? empty_fs : S.prog[1]->st[1].glsl};
            g_gs_draws++; g_gs_prog_same += now[0] == g_gs_last_prog[0] && now[1] == g_gs_last_prog[1];
            g_gs_last_prog[0] = now[0]; g_gs_last_prog[1] = now[1];
            pn_vs = now[0]; pn_fs = now[1]; pn_empty = depth_only; }
        if (g_gs_alpha_on || !(g_gs_on & 2)) { glDisable(GL_ALPHA_TEST); g_gs_alpha_on = 0; }
        for (int s = 0; s < (depth_only ? 1 : 2); s++)
            if (S.prog[s]->label_dirty) {          /* nvnProgramSetDebugLabel: the label goes to the OpenGL programs of its stages */
                Prog *lp = S.prog[s];
                lp->label_dirty = 0;
                for (int q = 0; q < 6; q++) if (lp->st[q].glsl && glObjectLabel) glObjectLabel(0x82E2 /* PROGRAM */, lp->st[q].glsl, -1, nvn_label_get(lp->guest));
            }
        PP(3);
        {   /* nvnProgramSetSampleShading of the fragment program: 1 = run the fragment shader per sample, 2 = never, 0 = as the shader says */
            int ss = depth_only ? 0 : S.prog[1]->sample_shading;
            if (ss != S.sample_shading) { S.sample_shading = ss; if (ss == 1 && gl_has(glMinSampleShading, "glMinSampleShading")) { glEnable(0x8C36 /* SAMPLE_SHADING */); glMinSampleShading(1.0f); } else glDisable(0x8C36); }
        }
        if (!depth_only && S.prog[1]->st[1].glsl_alpha) { uint32_t ref; memcpy(&ref, &S.alpha_ref, 4); glProgramUniform2ui(pair ? pair : S.prog[1]->st[1].glsl, 0, (GLuint)S.alpha_func, ref); }
        if (!depth_only && S.prog[1]->st[1].glsl_fcs) {
            if (wide_k() != 1.0f) glProgramUniform2f(pair ? pair : S.prog[1]->st[1].glsl, 1, S.prs ? 1.0f / S.prs : 1.0f, S.prsy ? 1.0f / S.prsy : 1.0f);
            else glProgramUniform1f(pair ? pair : S.prog[1]->st[1].glsl, 1, S.prs ? 1.0f / S.prs : 1.0f);
        }
        if (wide_k() != 1.0f) {     /* widescreen: the vertex stage's clip-space x shrinks by 1/k on a widened target, else 1 */
            static int nosq = -1; if (nosq < 0) nosq = getenv("TOMO_WIDE_NOSQUEEZE") != NULL;      /* TEST ONLY: the picture stretched instead */
            float q = S.prs && S.prsy && !nosq ? S.prsy / S.prs : 1.0f;
            if (q != 1.0f && g_wide_camera && !depth_only) {     /* a full-screen 2D quad keeps the width only to read a widened texture or to fill with
                                                                  * no texture (fades); 2D art keeps its 16:9 shape (no stretching) */
                int screen = 1;
                for (uint32_t m = units_sampled(1), u = 0; m && u < 32; m >>= 1, u++) {
                    Tex *t;
                    if (!(m & 1) || !(t = unit_tex(1, u)) || t->dead) continue;
                    screen = t->rw && t->rh && (float)t->rw * (float)t->b.height != (float)t->rh * (float)t->b.width;
                    if (screen) break;
                }
                if (screen) q = -q;
            }
            glProgramUniform1f(pair ? pair : S.prog[0]->st[0].glsl, 2, q);
        }      /* resolution scale: game pixels per real one */
        for (int s = 0; s < (depth_only ? 1 : 2); s++) {
            Stage *st = &S.prog[s]->st[s];
            for (unsigned n = 0; n < 16; n++)
                if ((st->glsl_ssbos >> n & 1) && S.ssbo[s][n].addr && S.ssbo[s][n].size) {
                    /* a stage that writes a storage buffer writes it in a GL buffer that goes back to guest memory with the
                     * dispatches' results (compute_flush); the game reads such flags back (its occlusion culling: the fragment
                     * stage of bounding-box draws marks what is visible; before, nothing was ever marked and the palm trees and
                     * shrubs of the island were never drawn) */
                    static int nofrag = -1; if (nofrag < 0) nofrag = getenv("TOMO_NOFRAGSSBO") != NULL;
                    CBuf *cb = nofrag ? NULL : (st->glsl_ssbo_w >> n & 1) ? cbuf_get(S.ssbo[s][n].addr, S.ssbo[s][n].size) : cbuf_find(S.ssbo[s][n].addr, S.ssbo[s][n].size);
                    if (cb) {
                        if (st->glsl_ssbo_w >> n & 1) { if (!cb->dirty) cb->frag = 1; cb->dirty = 1; g_frag_ssbo_pending = 1; }
                        else if (g_frag_ssbo_pending) { glMemoryBarrier(0x2000); g_frag_ssbo_pending = 0; }     /* reads what an earlier draw's fragment stage wrote */
                        gs_ssbo(40 + (GLuint)s * 16 + n, cb->buf, 0, (ptrdiff_t)S.ssbo[s][n].size);
                    } else {
                        size_t off = arena_put(S.ssbo[s][n].addr, S.ssbo[s][n].size);
                        gs_ssbo(40 + (GLuint)s * 16 + n, g_arena_buf, (ptrdiff_t)off, (ptrdiff_t)S.ssbo[s][n].size);
                    }
                }
            if (st->glsl_cbufs & 0x80000000u) bl_bind();        /* TEX.B: the bindless handle table */
            /* hardware constant buffers: 1 = constants stored after the shader's code, 2 = texture handles, 3.. = the uniform buffers bound by the game */
            for (unsigned n = 1; n < 18; n++) {
                Range r = {0, 0};
                if (!(st->glsl_cbufs >> n & 1)) continue;
                if (n == 2) { if (st->glsl_ubo) bind_handle_table(s, GL_UNIFORM_BUFFER_, (GLuint)s * 18 + n, 0); else bind_handle_table(s, GL_SHADER_STORAGE_BUFFER_, (GLuint)s * 20 + n, 0); continue; }
                if (n == 1) { r.addr = (uint64_t)(uintptr_t)st->cdata_copy; r.size = st->cdata_copy ? st->ctl[3] : 0; } else r = S.ubo[s][n - 3];
                if (n == 1 && gputime_ms() && !st->c1_logged) {       /* TOMO_GPUTIME: the shader's own constants (jump tables) as bound */
                    const uint32_t *w = (const uint32_t *)(uintptr_t)r.addr;
                    st->c1_logged = 1;
                    printf("gputime: program #%d stage %d constant buffer 1: ctl[3]=%u ctl[4]=%u at %p (code %p): %08x %08x %08x %08x %08x %08x %08x %08x\n", S.prog[s]->serial, s,
                           st->ctl[3], st->ctl[4], (void *)(uintptr_t)r.addr, (void *)st->code, r.size >= 4 ? w[0] : 0, r.size >= 8 ? w[1] : 0, r.size >= 12 ? w[2] : 0,
                           r.size >= 16 ? w[3] : 0, r.size >= 20 ? w[4] : 0, r.size >= 24 ? w[5] : 0, r.size >= 28 ? w[6] : 0, r.size >= 32 ? w[7] : 0);
                    fflush(stdout);
                }
                if (r.addr && r.size) {
                    size_t off = arena_put(r.addr, r.size);
                    if (st->glsl_ubo) gs_ubo((GLuint)s * 18 + n, g_arena_buf, (ptrdiff_t)off, (ptrdiff_t)r.size);
                    else gs_ssbo((GLuint)s * 20 + n, g_arena_buf, (ptrdiff_t)off, (ptrdiff_t)r.size);
                    g_gs_buf_bind++;
                }
            }
        }
        g_st.glsl_draws++;
        PP(4);
    } else {
    pair_off();
    glBindProgramPipeline(0); g_gs_pipe = 0; g_gs_arb_on = g_gs_alpha_on = 1;
    if (S.alpha_func && S.alpha_func != 8) { glEnable(GL_ALPHA_TEST); glAlphaFunc(nvn_func((unsigned)S.alpha_func), S.alpha_ref); } else glDisable(GL_ALPHA_TEST);
    if (!S.prog[0] || !stage_ready(S.prog[0], 0) || !S.prog[1] || !stage_ready(S.prog[1], 1)) {
        logf_limited(&nlog, 10, "nvn_gl: draw skipped: no usable vertex/fragment program (vertex #%d state %d, fragment #%d state %d)\n", S.prog[0] ? S.prog[0]->serial : -1,
                     S.prog[0] ? S.prog[0]->st[0].state : -1, S.prog[1] ? S.prog[1]->serial : -1, S.prog[1] ? S.prog[1]->st[1].state : -1);
        return 0;
    }
    uint32_t storage = 0;
    for (int s = 0; s < 5; s++) {
        Prog *p = S.prog[s];
        if (!p || !stage_ready(p, s)) { glDisable(k_prog_target[s]); continue; }
        Stage *st = &p->st[s];
        glEnable(k_prog_target[s]);
        glBindProgramARB(k_prog_target[s], st->gl);
        for (int i = 0; i < st->nbuf; i++) {
            int n = st->map[i];
            if (n == TEXTURE_HANDLE_BUFFER) {
                bind_handle_table(s, k_pbuf_target[s], (GLuint)i, 32);
            } else if (n < 16 && S.ubo[s][n].addr && S.ubo[s][n].size) {
                size_t off = arena_put(S.ubo[s][n].addr, S.ubo[s][n].size);
                glBindBufferRange(k_pbuf_target[s], (GLuint)i, g_arena_buf, (ptrdiff_t)off, (ptrdiff_t)S.ubo[s][n].size);
            }
        }
        for (uint32_t m = st->storage & ~storage, n = 0; m; m >>= 1, n++)
            if ((m & 1) && S.ssbo[s][n].addr && S.ssbo[s][n].size) {
                size_t off = arena_put(S.ssbo[s][n].addr, S.ssbo[s][n].size);
                gs_ssbo(n, g_arena_buf, (ptrdiff_t)off, (ptrdiff_t)S.ssbo[s][n].size);
                storage |= 1u << n;
            }
    }
    }
    for (int i = 0; i < 16; i++) {
        uint32_t a = i < S.nattr ? S.vattr[i] : 0;
        int fmt = (int)(a & 0xff), stream = (int)((a >> 8) & 31), size, norm, integer, on = 0;
        GLenum type;
        if (fmt && S.vbuf[stream].addr && S.vbuf[stream].size && vfmt_get(fmt, &size, &type, &norm, &integer)) {
            if (g_gs_attr[i] != a || !(g_gs_on & 4)) {
                if (integer) glVertexAttribIFormat((GLuint)i, size, type, a >> 14);
                else glVertexAttribFormat((GLuint)i, size, type, (GLboolean)norm, a >> 14);
                glVertexAttribBinding((GLuint)i, (GLuint)stream);
                g_gs_attr[i] = a;
            }
            g_gs_attr_set++; g_gs_attr_same += g_gs_last_attr[i] == a; g_gs_last_attr[i] = a;
            on = 1;
        }
        if (on != S.attr_enabled[i]) { if (on) glEnableVertexAttribArray((GLuint)i); else glDisableVertexAttribArray((GLuint)i); S.attr_enabled[i] = on; }
    }
    PP(5);
    if (pn_vs) {
        uint32_t mask = 0;
        for (int i = 0; i < 16; i++) mask |= (uint32_t)(S.attr_enabled[i] != 0) << i;
        prec_note(pn_vs, pn_fs, pn_empty, mask & S.prog[0]->st[0].glsl_dep);
    }
    for (int i = 0; i < 16; i++)
        if (S.vbuf[i].addr && S.vbuf[i].size) {
            CBuf *cb = g_cbuf_live && vb_direct_on() ? cbuf_holding(S.vbuf[i].addr, S.vbuf[i].size) : NULL;
            GLuint vbo = g_arena_buf;
            size_t off;
            if (cb) { vbo = cb->buf; off = (size_t)(S.vbuf[i].addr - cb->addr); if (cb->frag && cb->dirty) glMemoryBarrier(0x1 /* VERTEX_ATTRIB_ARRAY */); }
            else { off = arena_put(S.vbuf[i].addr, S.vbuf[i].size); vbo = g_arena_buf; }
            if (g_gs_vb[i].buf != vbo || g_gs_vb[i].off != (ptrdiff_t)off || g_gs_vb[i].stride != (GLsizei)S.vstream[i].stride || !(g_gs_on & 8)) {
                glBindVertexBuffer((GLuint)i, vbo, (ptrdiff_t)off, (GLsizei)S.vstream[i].stride);
                g_gs_vb[i].buf = vbo; g_gs_vb[i].off = (ptrdiff_t)off; g_gs_vb[i].stride = (GLsizei)S.vstream[i].stride;
                g_gs_vbuf_bind++;
            }
            if (g_gs_vb[i].div != S.vstream[i].divisor + 1 || !(g_gs_on & 8)) { glVertexBindingDivisor((GLuint)i, S.vstream[i].divisor); g_gs_vb[i].div = S.vstream[i].divisor + 1; }
        }
    PP(6);
    apply_stencil();
    if (g_ncrange) {
        int hit = 0;
        for (int i = 0; i < 16 && i < S.nattr; i++) {
            int stream = (int)((S.vattr[i] >> 8) & 31);
            if ((S.vattr[i] & 0xff) && S.vbuf[stream].addr && crange_hit(S.vbuf[stream].addr, S.vbuf[stream].size) >= 0) hit = 1;
        }
        g_st.draws_from_compute += hit;
    }
    PP(7);
    if (drawlog_now()) {                    /* debug: what the draws of one frame look like */
        GLfloat vp[4];
        GLint sc[4], dfunc = 0;
        GLboolean cm[4];
        glGetFloatv(GL_VIEWPORT, vp); glGetIntegerv(GL_SCISSOR_BOX, sc); glGetBooleanv(GL_COLOR_WRITEMASK, cm); glGetIntegerv(GL_DEPTH_FUNC, &dfunc);
        g_dlrt_n++;
        printf("draw: fbo %u (%dx%d) viewport %.0f,%.0f %.0fx%.0f scissor %d,%d %dx%d vs #%d fs #%d tag %08x depth %d(func %x) stencil %d blend %d cull %d alpha %d mask %d%d%d%d\n", S.fbo, S.fbo_w, S.fbo_h,
               vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3], S.prog[0] ? S.prog[0]->serial : -1, S.prog[1] ? S.prog[1]->serial : -1, S.prog[1] ? code_tag(&S.prog[1]->st[1]) : 0, glIsEnabled(GL_DEPTH_TEST), dfunc,
               glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_BLEND), glIsEnabled(GL_CULL_FACE), S.alpha_func, cm[0], cm[1], cm[2], cm[3]);
        {   GLint bf[6] = {0, 0, 0, 0, 0, 0};
            static const GLenum q[6] = {0x80C9 /* SRC_RGB */, 0x80C8 /* DST_RGB */, 0x80CB /* SRC_ALPHA */, 0x80CA /* DST_ALPHA */, 0x8009 /* EQUATION_RGB */, 0x883D /* EQUATION_ALPHA */};
            for (int i = 0; i < 6; i++) glGetIntegerv(q[i], &bf[i]);
            printf("  frame %lld | blend 0: rgb 0x%x 0x%x alpha 0x%x 0x%x eq 0x%x 0x%x | framebuffer srgb %d\n", (long long)g_st.frames, bf[0], bf[1], bf[2], bf[3], bf[4], bf[5], glIsEnabled(0x8DB9));
        }
        if (S.color0) printf("  colour target 0: gl %u mem %p %dx%d format 0x%x layer %d level %d view set 0x%x\n", S.color0->o.gl, (void *)S.color0->mem, S.color0->b.width, S.color0->b.height, S.color0->b.format, g_rt0_layer, g_rt0_level, g_rt0_vset);
        for (int ci = 1; ci < 8; ci++) if (S.color[ci]) printf("  colour target %d: gl %u mem %p %dx%dx%d format 0x%x target %d\n", ci, S.color[ci]->o.gl, (void *)S.color[ci]->mem, S.color[ci]->b.width, S.color[ci]->b.height, S.color[ci]->b.depth, S.color[ci]->b.format, S.color[ci]->b.target);
        if (S.depth_tex) {
            GLboolean dm = 0; GLfloat dr[2] = {0, 0}, po[2] = {0, 0}; GLint clampd = 0;
            glGetBooleanv(0x0B72 /* DEPTH_WRITEMASK */, &dm); glGetFloatv(0x0B70 /* DEPTH_RANGE */, dr);
            glGetFloatv(0x8038 /* POLYGON_OFFSET_FACTOR */, &po[0]); glGetFloatv(0x2A00 /* POLYGON_OFFSET_UNITS */, &po[1]); clampd = glIsEnabled(0x864F /* DEPTH_CLAMP */);
            printf("  depth target: gl %u mem %p %dx%d format 0x%x | write %d range %g..%g offset %g/%g (fill %d) clamp %d\n", S.depth_tex->o.gl, (void *)S.depth_tex->mem, S.depth_tex->b.width, S.depth_tex->b.height, S.depth_tex->b.format,
                   dm, dr[0], dr[1], po[0], po[1], glIsEnabled(0x8037 /* POLYGON_OFFSET_FILL */), clampd);
        }
        for (int s = 0; s < 2; s++) {
            if (S.prog[s] && S.prog[s]->st[s].cdata_copy) {     /* the stage's own constants (c1: jump tables) */
                const Stage *cs = &S.prog[s]->st[s];
                const uint32_t *w = (const uint32_t *)cs->cdata_copy;
                printf("  %s constants (c1, %u bytes at +0x%x):", k_stage_name[s], cs->ctl[3], cs->ctl[4]);
                for (unsigned i = 0; i < cs->ctl[3] / 4 && i < 24; i++) printf(" %08x", w[i]);
                printf("\n");
            }
            printf("  %s buffers:", k_stage_name[s]);
            for (int n = 0; n < 16; n++) if (S.ubo[s][n].addr) printf(" c%d=%llx+%llu", n, (unsigned long long)S.ubo[s][n].addr, (unsigned long long)S.ubo[s][n].size);
            for (int n = 0; n < 16; n++)
                if (S.ssbo[s][n].addr) {
                    const float *f = P(S.ssbo[s][n].addr);
                    printf(" storage%d=%llx+%llu%s [", n, (unsigned long long)S.ssbo[s][n].addr, (unsigned long long)S.ssbo[s][n].size, S.prog[s] && (S.prog[s]->st[s].glsl_ssbos >> n & 1) ? "(used)" : "");
                    for (unsigned k = 0; k < 16 && k * 4 < S.ssbo[s][n].size; k++) printf(" %g", f[k]);
                    printf(" ]");
                }
            printf("  textures:");
            for (int n = 0; n < 32; n++) if (S.nvn_tex[s][n]) printf(" %d:%llx%s", n, (unsigned long long)S.nvn_tex[s][n], S.gl_tex[s][4 + n] == g_dummy_handle ? "(dummy)" : "");
            printf("\n");
            for (int n = 0; n < 16; n++)
                if (S.ubo[s][n].addr && S.prog[s] && (S.prog[s]->st[s].glsl_cbufs >> (n + 3) & 1)) {
                    const float *f = P(S.ubo[s][n].addr);
                    printf("    c%d:", n);
                    static int full = -1;              /* SSBU_DRAWLOG_FULL=1: every float of the buffer, not the first 24 */
                    if (full < 0) full = getenv("SSBU_DRAWLOG_FULL") != NULL;
                    for (unsigned k = 0; k < (full ? 1024u : 24u) && k * 4 < S.ubo[s][n].size; k++) printf(" %g", f[k]);
                    printf("\n");
                    static long cbw = -2;              /* TOMO_CBWORDS=<word>: 64 words from there, as hex, '*' after a bindless handle never noted */
                    if (cbw == -2) cbw = getenv("TOMO_CBWORDS") ? atol(getenv("TOMO_CBWORDS")) : -1;
                    static long cbn = -2; if (cbn == -2) cbn = getenv("TOMO_CBCOUNT") ? atol(getenv("TOMO_CBCOUNT")) : 64;
                    if (cbw >= 0 && (cbw + cbn) * 4 <= (long)S.ubo[s][n].size) {
                        const uint32_t *w = P(S.ubo[s][n].addr);
                        printf("    c%d words %ld:", n, cbw);
                        for (long k = cbw; k < cbw + cbn; k++) printf(" %x%s", w[k], bl_has(w[k]) ? "" : "*");
                        printf("\n");
                        for (long k = cbw; k < cbw + cbn; k++) {
                            uint32_t tid = w[k] & 0xfffff;
                            Tex *t = bl_has(w[k]) && S.tpool && tid < (uint32_t)S.tpool->count ? S.tpool->e[tid].tex : NULL;
                            if (t) printf("      handle %x: target %d format 0x%x %dx%dx%d levels %d gl %u uploaded %d rendered %d dead %d view set 0x%x format 0x%x target 0x%x\n", w[k], t->b.target, t->b.format,
                                          t->b.width, t->b.height, t->b.depth, t->b.levels, t->o.gl, t->uploaded, t->gpu_written, t->dead, S.tpool->e[tid].view.set, S.tpool->e[tid].view.format, S.tpool->e[tid].view.target);
                            int sw = t && t->rw ? t->rw : t ? t->b.width : 0, sh = t && t->rw ? t->rh : t ? t->b.height : 0;      /* storage size (scaled) */
                            if (t && t->o.gl && t->b.target == 1 && sw <= 4096) {
                                char path[64];
                                uint8_t *px = malloc((size_t)sw * sh * 4);
                                glBindTexture(GL_TEXTURE_2D, t->o.gl);
                                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                                while (glGetError()) {}
                                snprintf(path, sizeof path, "tex_%04x_%dx%d.rgba", tid, sw, sh);
                                FILE *fp = fopen(path, "wb");
                                if (fp) { fwrite(px, 1, (size_t)sw * sh * 4, fp); fclose(fp); }
                                free(px);
                            }
                        }
                    }
                }
            for (int n = 0; n < 32; n++) {
                uint32_t tid = (uint32_t)S.nvn_tex[s][n] & 0xfffff;
                Tex *t = S.tpool && tid && tid < (uint32_t)S.tpool->count ? S.tpool->e[tid].tex : NULL;
                static uint8_t seen[0x4000];
                if (!t) continue;
                printf("    texture %d: id 0x%x target %d format 0x%x %dx%dx%d levels %d flags 0x%x gl %u mem %p uploaded %d rendered %d view set 0x%x format 0x%x\n", n, tid, t->b.target, t->b.format, t->b.width,
                       t->b.height, t->b.depth, t->b.levels, t->b.flags, t->o.gl, (void *)t->mem, t->uploaded, t->gpu_written, S.tpool->e[tid].view.set, S.tpool->e[tid].view.format);
                if (!t->uploaded && !t->rt_tick && t->mem) {     /* never drawn into itself: what else covers its memory */
                    uint64_t b0 = (uint64_t)(uintptr_t)t->mem, e0 = b0 + t->storage;
                    AcquireSRWLockShared(&g_tex_range_lock);
                    for (size_t i = 0; i < g_tex_range_count && g_tex_ranges[i].begin < e0; i++) {
                        Tex *o = g_tex_ranges[i].tex;
                        if (o == t || g_tex_ranges[i].end <= b0) continue;
                        printf("      overlaps: mem %p+%llx %dx%dx%d format 0x%x gl %u rt_tick %lld dead %d\n", (void *)o->mem, (unsigned long long)o->storage, o->b.width, o->b.height,
                               o->b.depth, o->b.format, o->o.gl, o->rt_tick, o->dead);
                    }
                    ReleaseSRWLockShared(&g_tex_range_lock);
                }
                {   uint32_t dims2[4]; int isint2 = 0;
                    uint64_t fresh = resolve_handle(S.tpool, S.spool, S.nvn_tex[s][n], dims2, &isint2);
                    if (fresh != S.gl_tex[s][4 + n]) printf("      STALE handle: bound %llx, now %llx\n", (unsigned long long)S.gl_tex[s][4 + n], (unsigned long long)fresh);
                    uint32_t sid2 = (uint32_t)(S.nvn_tex[s][n] >> 20) & 0xfff;
                    if (!S.spool || sid2 >= (uint32_t)S.spool->count || !S.spool->used[sid2])
                        printf("      no sampler: handle %llx sampler id 0x%x, pool of %d, used %d\n", (unsigned long long)S.nvn_tex[s][n], sid2, S.spool ? S.spool->count : -1,
                               S.spool && sid2 < (uint32_t)S.spool->count ? S.spool->used[sid2] : -1);
                }
                if (t->b.target == 1 && t->gpu_written && t->o.gl && t->b.width >= 64) {     /* the centre texel through the texture and through the view sampled */
                    TexObj *vo = tex_view_obj(t, &S.tpool->e[tid].view);
                    float a[4] = {-1, -1, -1, -1}, b[4] = {-1, -1, -1, -1};
                    glGetTextureSubImage(t->o.gl, 0, t->b.width / 2, t->b.height / 2, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof a, a);
                    if (vo && vo->gl) glGetTextureSubImage(vo->gl, 0, t->b.width / 2, t->b.height / 2, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof b, b);
                    while (glGetError()) {}
                    if (vo && vo->gl) {
                        GLint fmt = 0, imm = 0, minl = 0, numl = 0, base = 0, maxl = 0, minf = 0, w = 0;
                        glBindTexture(GL_TEXTURE_2D, vo->gl);
                        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x1003 /* INTERNAL_FORMAT */, &fmt);
                        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
                        glGetTexParameteriv(GL_TEXTURE_2D, 0x912F /* IMMUTABLE_FORMAT */, &imm);
                        glGetTexParameteriv(GL_TEXTURE_2D, 0x82DB /* VIEW_MIN_LEVEL */, &minl);
                        glGetTexParameteriv(GL_TEXTURE_2D, 0x82DC /* VIEW_NUM_LEVELS */, &numl);
                        glGetTexParameteriv(GL_TEXTURE_2D, 0x813C /* BASE_LEVEL */, &base);
                        glGetTexParameteriv(GL_TEXTURE_2D, 0x813D /* MAX_LEVEL */, &maxl);
                        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
                        while (glGetError()) {}
                        printf("      view object: internal 0x%x width %d immutable %d levels %d+%d base %d max %d min filter 0x%x\n", fmt, w, imm, minl, numl, base, maxl, minf);
                    }
                    printf("      centre texel: texture gl %u %g %g %g %g | view gl %u%s %g %g %g %g\n", t->o.gl, a[0], a[1], a[2], a[3], vo ? vo->gl : 0, vo && vo->alias ? " (alias)" : "", b[0], b[1], b[2], b[3]);
                    {   Tex *t2; View v2; SamplerParams p2;
                        int hs2 = handle_lookup(S.tpool, S.spool, S.nvn_tex[s][n], &t2, &v2, &p2);
                        uint64_t hh = vo && vo->gl ? (hs2 ? glGetTextureSamplerHandleARB(vo->gl, sampler_gl(&p2)) : glGetTextureHandleARB(vo->gl)) : 0;
                        printf("      handle check: bound %llx, driver's for view gl %u now %llx%s; view handles cached %d\n", (unsigned long long)S.gl_tex[s][4 + n], vo ? vo->gl : 0,
                               (unsigned long long)hh, hh == S.gl_tex[s][4 + n] ? "" : "  MISMATCH", vo ? vo->nh : -1);
                        if (S.gl_tex[s][4 + n] && !glIsTextureHandleResidentARB(S.gl_tex[s][4 + n])) printf("      NOT RESIDENT: handle %llx\n", (unsigned long long)S.gl_tex[s][4 + n]);
                    }
                }
                printf("      swizzle: view %d %d %d %d, texture %d %d %d %d\n", S.tpool->e[tid].view.swz[0], S.tpool->e[tid].view.swz[1], S.tpool->e[tid].view.swz[2], S.tpool->e[tid].view.swz[3],
                       t->b.swz[0], t->b.swz[1], t->b.swz[2], t->b.swz[3]);
                if (t->mem && t->storage >= 65536)     /* other live textures over the same memory (a target written under another object) */
                    for (Tex *o = g_all_tex; o; o = o->all_next)
                        if (o != t && !o->dead && o->mem && o->mem < t->mem + t->storage && o->mem + o->storage > t->mem)
                            printf("      overlaps gl %u mem %+lld %dx%dx%d format 0x%x target %d levels %d rendered %d\n", o->o.gl, (long long)(o->mem - t->mem), o->b.width, o->b.height,
                                   o->b.depth, o->b.format, o->b.target, o->b.levels, o->gpu_written);
                {   uint32_t sid = (uint32_t)(S.nvn_tex[s][n] >> 20) & 0xfff;
                    if (S.spool && sid < (uint32_t)S.spool->count) {
                        const SamplerParams *sp = &S.spool->e[sid];
                        printf("      sampler 0x%x: min %d mag %d wrap %d %d %d lod %g..%g border %g %g %g %g (kind %d) compare %d func %d reduction %d aniso %g\n", sid, sp->min, sp->mag, sp->ws, sp->wt, sp->wr, sp->lodmin, sp->lodmax, sp->border[0], sp->border[1], sp->border[2], sp->border[3], sp->border_kind, sp->cmode, sp->cfunc, sp->reduction, sp->aniso);
                    }
                }
                static int dumptex = -1; if (dumptex < 0) dumptex = getenv("SSBU_DRAWLOG_TEX") != NULL;      /* SSBU_DRAWLOG_TEX=1: each sampled texture as tex_<id>_<w>x<h>.rgba (~1 GB a frame) */
                int sw = t->rw ? t->rw : t->b.width, sh = t->rw ? t->rh : t->b.height;      /* the storage: wider than the game's size when scaled */
                if (dumptex && tid < sizeof seen && !seen[tid] && t->o.gl && t->b.target == 1 && t->b.samples <= 1 && sw <= 4096) {
                    char path[64];
                    uint8_t *px = malloc((size_t)sw * sh * 4);
                    seen[tid] = 1;
                    glBindTexture(GL_TEXTURE_2D, t->o.gl);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    while (glGetError()) {}
                    snprintf(path, sizeof path, "tex_%04x_%dx%d.rgba", tid, sw, sh);
                    FILE *fp = fopen(path, "wb");
                    if (fp) { fwrite(px, 1, (size_t)sw * sh * 4, fp); fclose(fp); }
                    free(px);
                }
            }
        }
        for (int i = 0; i < 16 && i < S.nattr; i++) {
            uint32_t a = S.vattr[i];
            int stream = (int)((a >> 8) & 31);
            if (!(a & 0xff)) continue;
            printf("  attr %d: format 0x%x stream %d offset %u stride %u buffer %llx+%llu", i, a & 0xff, stream, a >> 14, S.vstream[stream].stride, (unsigned long long)S.vbuf[stream].addr, (unsigned long long)S.vbuf[stream].size);
            if (S.vbuf[stream].addr && S.vbuf[stream].size >= (a >> 14) + 16) { const float *f = (const float *)((const uint8_t *)P(S.vbuf[stream].addr) + (a >> 14)); printf("  first: %g %g %g %g", f[0], f[1], f[2], f[3]); }
            if (i == 0 && (a & 0xff) == 0x22 && S.vstream[stream].stride >= 12 && S.vbuf[stream].addr) {     /* three floats: the box of all the buffer's vertices */
                float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
                const uint8_t *b0 = (const uint8_t *)P(S.vbuf[stream].addr) + (a >> 14);
                for (uint64_t o = 0; o + 12 <= S.vbuf[stream].size - (a >> 14); o += S.vstream[stream].stride) {
                    const float *v = (const float *)(b0 + o);
                    for (int k = 0; k < 3; k++) { if (v[k] < mn[k]) mn[k] = v[k]; if (v[k] > mx[k]) mx[k] = v[k]; }
                }
                printf("  box: %g..%g %g..%g %g..%g", mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
            }
            if (S.vbuf[stream].addr && (a & 0xff) == 0x29) {          /* RGBA16F: the first four vertices as half floats */
                const uint16_t *h = (const uint16_t *)((const uint8_t *)P(S.vbuf[stream].addr) + (a >> 14));
                printf("  halves:");
                for (int v = 0; v < 4 && (uint64_t)(v * S.vstream[stream].stride + 8) <= S.vbuf[stream].size; v++, h = (const uint16_t *)((const uint8_t *)h + S.vstream[stream].stride))
                    printf(" (%g %g %g %g)", half_to_f32(h[0]), half_to_f32(h[1]), half_to_f32(h[2]), half_to_f32(h[3]));
            }
            { int k = S.vbuf[stream].addr ? crange_hit(S.vbuf[stream].addr, S.vbuf[stream].size) : -1; if (k >= 0) printf("  <- storage %d written by compute program #%d (%llx+%llu)", g_crange[k].n, g_crange[k].prog, (unsigned long long)g_crange[k].addr, (unsigned long long)g_crange[k].size); }
            printf("\n");
        }
    }
    /* The texture handles of this draw were resolved above (handle_make -> tex_sync_a): a texture made anew whose blocks
     * were seen before is decoded on the GPU right there, after exec's barrier. Without this one the draw sampled it before
     * the decoder's stores were visible: the game's texture streaming remakes textures every few frames, and each time the
     * object showed its old or empty texels for a frame (the Island Builder's flicker, 2026-10-30). TOMO_GA_DRAW_BARRIER=0: TEST, without. */
    {   static int off = -1; if (off < 0) off = getenv("TOMO_GA_DRAW_BARRIER") && getenv("TOMO_GA_DRAW_BARRIER")[0] == '0';
        if (!off) ga_barrier(); }
    return 1;
}

/* ---------------------------------------------------------------- compute
 * nvnCommandBufferDispatchCompute(cmdbuf, groupsX, groupsY, groupsZ) runs the program bound to the compute stage with the
 * uniform buffers, storage buffers, textures and images bound to that stage. The game skins its fighter meshes this way:
 * per mesh one dispatch reads bind-pose vertices and bone matrices and writes posed vertices into a storage buffer that later
 * draws bind as a vertex buffer. Storage buffers are guest memory: a buffer the program writes is copied into a GL buffer,
 * the dispatch runs, and the result is copied back to guest memory, where the CPU and later draws read it.
 * Debug aids: SSBU_NOCOMPUTE=1 skips every dispatch (the state before this was implemented); the draw log (SSBU_DRAWLOG)
 * also lists every dispatch of the frame with its buffers, and marks vertex buffers that a dispatch of the frame wrote. */
static int g_dbg_nocompute;
static struct { int serial; LONG64 calls, run, groups; } g_chist[64];
/* Storage buffers written by dispatches live in GL buffers of their own from the first dispatch that writes them until the next
 * command that reads guest memory (a draw, a copy, the end of the batch): compute_flush() then copies the written ones back to
 * guest memory, once, however many dispatches wrote into them (a fighter's meshes are skinned by ~10 dispatches into one buffer). */
static CBuf *cbuf_find(uint64_t addr, uint64_t size) {
    if (addr < g_cbuf_lo || addr + size > g_cbuf_hi) return NULL;
    for (int k = 0; k < g_cbuf_top; k++) if (g_cbuf[k].live && g_cbuf[k].addr == addr && g_cbuf[k].size == size) return &g_cbuf[k];
    return NULL;
}
static int cbuf_conflict(uint64_t addr, uint64_t size) {      /* overlaps a buffer held in GL without being that buffer */
    if (addr >= g_cbuf_hi || addr + size <= g_cbuf_lo) return 0;
    for (int k = 0; k < g_cbuf_top; k++)
        if (g_cbuf[k].live && addr < g_cbuf[k].addr + g_cbuf[k].size && g_cbuf[k].addr < addr + size && !(g_cbuf[k].addr == addr && g_cbuf[k].size == size)) return 1;
    return 0;
}
static void compute_flush_async(void);
static void compute_flush_at(int line) {
    int any = 0;
    if (!g_cbuf_live) return;
    if (g_rbp_n) rb_land(1);                     /* older read-backs land first: guest memory takes the GPU's writes in order */
    for (int k = 0; k < 64; k++) if (g_cbuf[k].live && g_cbuf[k].frag && g_cbuf[k].dirty) { glMemoryBarrier(0x2000 | 0x200 /* SHADER_STORAGE | BUFFER_UPDATE */); break; }
    for (int k = 0; k < 64; k++) {
        CBuf *b = &g_cbuf[k];
        if (!b->live) continue;
        if (b->dirty) {
            static uint8_t *tmp; static size_t cap;
            LONG64 pf0 = pf_now();
            if (cap < b->size) { cap = (size_t)b->size; tmp = realloc(tmp, cap); }
            HS_IN("compute read-back (glGetNamedBufferSubData)", 0); glGetNamedBufferSubData(b->buf, 0, (ptrdiff_t)b->size, tmp); HS_OUT();
            stall_add(100000 + line, pf0);
            gpu_merge(P(b->addr), tmp, b->snap, (size_t)b->size);
            ww_note(b->addr, b->size);
            pf_add(PF_CFLUSH, pf0); g_pf_cflush_bytes += (LONG64)b->size; g_st.compute_bytes += (LONG64)b->size; any = 1;
        }
        b->live = b->dirty = 0;
    }
    cbuf_none();
    if (any && g_listlog_dbg) { int k = 0; while (k < 31 && g_cfl_site[k].line && g_cfl_site[k].line != line) k++; g_cfl_site[k].line = line; g_cfl_site[k].n++; }
    nvn_front_ccmp_landed();
    if (any) g_arena_gen++;                      /* guest memory changed: later draws must stream it again */
}
/* Reports and conditional rendering touch 16-32 bytes of guest memory: they wait for GPU-held buffers only over those bytes.
 * The CPU sees the GPU's writes after its fences, which follow the end of the batch, where every buffer goes back (job_submit); flushing at every report stalled on the GPU 15 times a frame (7 ms on the island). */
static void cbuf_flush_if(uint64_t addr, uint64_t size) {
    for (int k = 0; k < 64 && g_cbuf_live; k++)
        if (g_cbuf[k].live && addr < g_cbuf[k].addr + g_cbuf[k].size && g_cbuf[k].addr < addr + size) { compute_flush(); return; }
}
/* End of a batch: the buffers the GPU wrote go back to guest memory without waiting (see RbPend); the ones that do not fit the
 * staging memory are read back the old way. */
static void compute_flush_async(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOMO_ASYNC_READBACK"); on = e ? atoi(e) : 1; }
    if (!g_cbuf_live) return;
    if (!on || !gl_has(glCopyNamedBufferSubData, "glCopyNamedBufferSubData")) { compute_flush(); return; }
    int n0 = g_rbp_n, rest = 0;
    for (int k = 0; k < 64; k++) if (g_cbuf[k].live && g_cbuf[k].frag && g_cbuf[k].dirty) { glMemoryBarrier(0x2000 | 0x200); break; }
    for (int k = 0; k < 64; k++) {
        CBuf *b = &g_cbuf[k];
        if (!b->live) continue;
        if (b->dirty) {
            int st = g_rbp_n < 64 ? rb_stage(b->size) : -1;
            if (st < 0) { rest = 1; continue; }
            HS_IN("compute results: copy to staging", 0);
            glCopyNamedBufferSubData(b->buf, g_rbs[st].buf, 0, 0, (ptrdiff_t)b->size);
            HS_OUT();
            g_rbs[st].busy = 1;
            g_rbp[g_rbp_n].addr = b->addr; g_rbp[g_rbp_n].size = b->size; g_rbp[g_rbp_n].slot = st; g_rbp[g_rbp_n].fence = NULL;
            g_rbp[g_rbp_n].snap = b->snap; b->snap = NULL;          /* the pending read-back owns it now */
            g_rbp_n++;
            g_st.compute_bytes += (LONG64)b->size;
        }
        b->live = b->dirty = 0;
        g_cbuf_live--;
    }
    if (g_rbp_n > n0) {
        glMemoryBarrier(0x1000 /* CLIENT_MAPPED_BUFFER */);
        void *f = glFenceSync(0x9117, 0);
        for (int i = n0; i < g_rbp_n; i++) g_rbp[i].fence = f;
        { HS_IN("batch end: compute results: glFlush", 0); glFlush(); HS_OUT(); }
    }
    if (rest) { HS_IN("batch end: compute results read back at once (no staging)", 0); compute_flush(); HS_OUT(); }      /* what found no staging memory (lands the pending ones first) */
    else { cbuf_none(); nvn_front_ccmp_landed(); }
}
static CBuf *cbuf_get(uint64_t addr, uint64_t size) {
    CBuf *b = cbuf_find(addr, size);
    if (b) return b;
    if (g_rbp_n) rb_land_range(addr, size);      /* the GL copy starts from guest memory: what the GPU wrote there before must have landed */
    for (int k = 0; k < 64 && !b; k++) if (!g_cbuf[k].live) b = &g_cbuf[k];
    if (!b) { compute_flush(); b = &g_cbuf[0]; }
    if (!b->buf) glCreateBuffers(1, &b->buf);
    glNamedBufferData(b->buf, (ptrdiff_t)size, P(addr), 0x88EA /* DYNAMIC_COPY */);
    b->snap = realloc(b->snap, (size_t)size); if (b->snap) memcpy(b->snap, P(addr), (size_t)size);
    b->addr = addr; b->size = size; b->live = 1; b->dirty = 0; b->frag = 0;
    g_cbuf_live++;
    if (addr < g_cbuf_lo) g_cbuf_lo = addr;
    if (addr + size > g_cbuf_hi) g_cbuf_hi = addr + size;
    if ((int)(b - g_cbuf) >= g_cbuf_top) g_cbuf_top = (int)(b - g_cbuf) + 1;
    return b;
}
static int crange_hit(uint64_t addr, uint64_t size) {
    for (int k = 0; k < g_ncrange; k++) if (addr < g_crange[k].addr + g_crange[k].size && g_crange[k].addr < addr + size) return k;
    return -1;
}
static void mem_stats(const char *what, uint64_t addr, uint64_t size) {
    const uint8_t *p = P(addr);
    const float *f = P(addr);
    size_t nz = 0, n = size > (8u << 20) ? (8u << 20) : (size_t)size;
    for (size_t q = 0; q < n; q++) nz += p[q] != 0;
    printf("    %s %llx+%llu: %zu of %zu bytes non-zero, hash %016llx, first:", what, (unsigned long long)addr, (unsigned long long)size, nz, n, (unsigned long long)mem_hash(p, n, 1));
    for (unsigned k = 0; k < 12 && k * 4 + 4 <= size; k++) printf(" %g", f[k]);
    printf("\n");
}
#define WMASK_BINDING 90                   /* storage block that collects which storage buffers a translated compute program wrote */
static Tex *image_tex(int stage, int k, View *v) {            /* NVN image handle = texture ID in the texture pool (nvnDeviceGetImageHandle) */
    uint32_t tid = (uint32_t)S.nvn_img[stage][k];
    Tex *t = NULL;
    if (!S.tpool || !tid) return NULL;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)S.tpool->count) { t = S.tpool->e[tid].tex; *v = S.tpool->e[tid].view; }
    ReleaseSRWLockShared(&g_pool_lock);
    return t && !t->dead ? t : NULL;
}
/* The GLSL program of a compute stage given as machine code (translated again when the integer-ness of a texture it samples or
 * the format of an image it loads changes); 0 when it cannot be translated or the driver refuses it. */
static int stage_glsl_compute(Prog *p) {
    static int nfail, nlink;
    Stage *st = &p->st[5];
    MxIn in;
    MxOut out;
    uint16_t ifmt[8];
    if (!st->code || st->glsl_state == 2) return 0;
    for (int k = 0; k < 8; k++) {
        View v;
        Fmt f;
        Tex *t = image_tex(5, k, &v);
        ifmt[k] = t && fmt_get((v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, &f) ? f.internal : 0;
    }
    if (st->glsl_state == 1 && ((S.tex_int[5] ^ st->glsl_key[2]) & st->glsl_tex) == 0 && ((S.tex_sint[5] ^ st->glsl_key[3]) & st->glsl_tex) == 0) {
        int same = 1;
        for (int k = 0; k < 8; k++) if ((st->glsl_images >> k & 1) && st->glsl_img_key[k] != ifmt[k]) same = 0;
        if (same) return 1;
    }
    if (st->glsl) { glDeleteProgram(st->glsl); st->glsl = 0; }
    memset(&in, 0, sizeof in);
    in.stage = MX_COMPUTE; in.code = st->code; in.code_len = st->code_len;
    if (st->cdata_copy) { in.cdata = st->cdata_copy; in.cdata_size = st->ctl[3]; }
    in.int_tex = S.tex_int[5]; in.sint_tex = S.tex_sint[5];
    in.binding_base = 0; in.ssbo_base = 40;        /* the bindings of the vertex stage: every draw and every dispatch binds what it uses */
    memcpy(in.local_size, st->local, sizeof in.local_size);
    in.smem_bytes = st->smem_bytes; in.lmem_bytes = st->lmem_bytes;
    in.image_base = 0; memcpy(in.image_format, ifmt, sizeof ifmt);
    in.track_writes = 1; in.wmask_binding = WMASK_BINDING;
    if (!mx_translate(&in, &out)) {
        st->glsl_state = 2;
        g_st.glsl_fail++;
        logf_limited(&nfail, 60, "nvn_gl: program #%d compute stage: machine code not translated: %s\n", p->serial, out.why);
        return 0;
    }
    const char *dir = getenv("SSBU_SHADERDUMP");
    if (dir) {
        char path[260];
        snprintf(path, sizeof path, "%s/b%05d_compute.glsl", dir, p->serial);
        FILE *f = fopen(path, "wb");
        if (f) { fputs(out.glsl, f); fclose(f); }
    }
    const char *srcs[1] = {out.glsl};
    GLint ok = 0;
    {   /* P3 round 2: compute stages go through the binary cache as well (a first dispatch cost 0.1-3.3 s, hitch log) */
        size_t clen = strlen(out.glsl);
        HS_IN("shader compile or load, compute stage", 0);
        if (g_shd_mode > 0) st->glsl = shader_build(0x91B9 /* COMPUTE_SHADER */, out.glsl, clen, sh_hash(0x9E3779B97F4A7C15ULL ^ 5u, out.glsl, clen), &ok);
        if (g_shd_mode > 0 && ok) prec_note_compute(sh_hash(0x9E3779B97F4A7C15ULL ^ 5u, out.glsl, clen), clen);
        else { st->glsl = glCreateShaderProgramv(0x91B9, 1, srcs); glGetProgramiv(st->glsl, 0x8B82, &ok); }
        HS_OUT();
    }
    if (!ok) {
        char log[2000] = "";
        glGetProgramInfoLog(st->glsl, sizeof log - 1, NULL, log);
        if (nlink < 4) {
            char path[64];
            snprintf(path, sizeof path, "shader_fail_glsl_c%d.txt", nlink);
            FILE *f = fopen(path, "wb");
            if (f) { fputs(out.glsl, f); fclose(f); }
        }
        logf_limited(&nlink, 20, "nvn_gl: program #%d compute stage: the driver refuses the translated GLSL:\n%s\n", p->serial, log);
        glDeleteProgram(st->glsl);
        st->glsl = 0; st->glsl_state = 2;
        g_st.glsl_fail++;
        free(out.glsl);
        return 0;
    }
    free(out.glsl);
    st->glsl_state = 1;
    st->glsl_key[2] = S.tex_int[5]; st->glsl_key[3] = S.tex_sint[5];
    memcpy(st->glsl_img_key, ifmt, sizeof ifmt); memcpy(st->glsl_img_fmt, out.image_gl_format, sizeof st->glsl_img_fmt);
    st->glsl_cbufs = out.cbufs; st->glsl_ssbos = out.ssbos; st->glsl_ssbo_w = out.ssbo_written; st->glsl_tex = out.tex_units;
    st->glsl_images = out.images; st->glsl_images_w = out.images_written; st->glsl_images_r = out.images_read;
    g_st.glsl_ok++;
    return 1;
}
static GLuint g_wmask_buf;
static void compute_bind_glsl(Prog *p) {
    static int nlog;
    Stage *st = &p->st[5];
    pair_off();
    glBindProgramPipeline(g_pipeline);
    glUseProgramStages(g_pipeline, 0x20 /* COMPUTE_SHADER_BIT */, st->glsl);
    if (st->glsl_cbufs & 0x80000000u) bl_bind();
    for (unsigned n = 0; n < 18; n++) {
        Range r = {0, 0};
        if (!(st->glsl_cbufs >> n & 1)) continue;
        if (n == 0) { logf_limited(&nlog, 10, "nvn_gl: compute program #%d reads the driver's constant buffer 0 outside the storage buffer descriptors: not provided\n", p->serial); continue; }
        if (n == 2) { bind_handle_table(5, GL_SHADER_STORAGE_BUFFER_, n, 0); continue; }
        if (n == 1) { r.addr = (uint64_t)(uintptr_t)st->cdata_copy; r.size = st->cdata_copy ? st->ctl[3] : 0; } else r = S.ubo[5][n - 3];
        if (r.addr && r.size) {
            size_t off = arena_put(r.addr, r.size);
            gs_ssbo(n, g_arena_buf, (ptrdiff_t)off, (ptrdiff_t)r.size);
        }
    }
    for (int k = 0; k < 8; k++) {
        View v;
        Fmt f;
        Tex *t;
        if (!(st->glsl_images >> k & 1)) continue;
        if (!(t = image_tex(5, k, &v))) { logf_limited(&nlog, 10, "nvn_gl: compute program #%d uses image %d but none is bound\n", p->serial, k); continue; }
        tex_sync(t);
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, w = (st->glsl_images_w >> k & 1) != 0;
        GLenum fmt = st->glsl_img_fmt[k] ? st->glsl_img_fmt[k] : fmt_get(t->b.format, &f) ? f.internal : 0x8058;
        if (t->rw) { static int said; if (!said++) printf("nvn_gl: resolution scale: a compute stage uses a scaled texture (%dx%d format 0x%x) as an image: its coordinates are the game's (not converted)\n", t->b.width, t->b.height, t->b.format); }
        if (w) { t->gpu_written = 1; t->rt_gen++; }              /* written by the dispatch: view copies of it refresh */
        if (rtlog_match(t)) printf("rtlog: frame %lld image %d (compute) gl %u mem %p\n", (long long)g_st.frames + 1, k, t->o.gl, (void *)t->mem);
        glBindImageTexture((GLuint)k, tex_view_obj(t, &v)->gl, level, GL_TRUE, 0, w ? ((st->glsl_images_r >> k & 1) ? 0x88BA : 0x88B9) : 0x88B8, fmt);
        if (w) { t->gpu_written = 1; t->uploaded = 1; }
    }
    if (st->glsl_ssbo_w) {
        static const uint32_t zero;
        if (!g_wmask_buf) glCreateBuffers(1, &g_wmask_buf);
        glNamedBufferData(g_wmask_buf, 4, &zero, 0x88EA);
        gs_ssbo(WMASK_BINDING, g_wmask_buf, 0, 4);
    }
}
static void compute_dispatch(unsigned gx, unsigned gy, unsigned gz) {
    static int nlog;
    Prog *p = S.prog[5];
    int log = drawlog_now(), h;
    g_st.dispatches++; g_frame_dispatch++;
    for (h = 0; h < 63 && g_chist[h].serial && g_chist[h].serial != (p ? p->serial : -1); h++) {}
    g_chist[h].serial = p ? p->serial : -1; g_chist[h].calls++; g_chist[h].groups += (LONG64)gx * gy * gz;
    if (!p) { logf_limited(&nlog, 10, "nvn_gl: DispatchCompute with no compute program bound\n"); return; }
    Stage *st = &p->st[5];
    int text = st->state == 1 || st->state == 2, glsl = 0;
    if (st->state == 4 && g_chist[h].calls <= 2) log = 1;         /* the first dispatches of every machine-code program are always described */
    uint32_t used = 0, written = 0;
    { static int mc = -1; if (mc < 0) mc = getenv("SSBU_COMPUTE_MC") != NULL; if (mc && text && st->code) text = 0; }      /* run the translated machine code instead of the text */
    if (text && !g_dbg_nocompute && !stage_ready(p, 5)) text = 0;
    if (text) { used = st->storage; written = st->storage_w; }
    else if (!g_dbg_nocompute && stage_glsl_compute(p)) { glsl = 1; used = st->glsl_ssbos; written = st->glsl_ssbo_w; }
    else if (st->state == 1 || st->state == 2) { used = st->storage; written = st->storage_w; }
    if (log) {
        printf("dispatch %lld: compute program #%d (%s) groups %u x %u x %u, storage used 0x%x written 0x%x\n", (long long)g_frame_dispatch - 1, p->serial,
               st->state == 1 || st->state == 2 ? "assembly text" : st->code ? "machine code" : "nothing usable", gx, gy, gz, used, written);
        for (int n = 0; n < 16; n++)
            if (S.ubo[5][n].addr) {
                const uint32_t *w = P(S.ubo[5][n].addr);
                printf("    uniform %d %llx+%llu:", n, (unsigned long long)S.ubo[5][n].addr, (unsigned long long)S.ubo[5][n].size);
                for (unsigned k = 0; k < 20 && k * 4 + 4 <= S.ubo[5][n].size; k++) printf(w[k] < 0x100000 ? " %u" : " %08x", w[k]);
                printf("\n");
            }
        for (int n = 0; n < 32; n++) if (S.ssbo[5][n].addr) { char tag[40]; snprintf(tag, sizeof tag, "storage %d%s before", n, written >> n & 1 ? " (written)" : ""); mem_stats(tag, S.ssbo[5][n].addr, S.ssbo[5][n].size); }
        for (int n = 0; n < 8; n++) if (S.nvn_tex[5][n]) printf("    texture %d: %llx\n", n, (unsigned long long)S.nvn_tex[5][n]);
        for (int n = 0; n < 8; n++) if (S.nvn_img[5][n]) printf("    image %d: %llx\n", n, (unsigned long long)S.nvn_img[5][n]);
    }
    for (int n = 0; n < 32; n++)
        if ((written >> n & 1) && S.ssbo[5][n].addr && g_ncrange < 1024) { g_crange[g_ncrange].addr = S.ssbo[5][n].addr; g_crange[g_ncrange].size = S.ssbo[5][n].size; g_crange[g_ncrange].prog = p->serial; g_crange[g_ncrange++].n = n; }
    if (g_dbg_nocompute) return;
    if (!text && !glsl) { logf_limited(&nlog, 20, "nvn_gl: DispatchCompute skipped: compute program #%d cannot be used (state %d, machine code %d bytes)\n", p->serial, st->state, st->code_len); g_st.dispatch_skipped++; return; }
    if (!gx || !gy || !gz) return;
    /* storage buffers: read-only ones are streamed like any other buffer (unless an earlier dispatch wrote them and they are still
     * held in GL); written ones get a GL buffer of their own (see compute_flush) */
    CBuf *held[32];
    memset(held, 0, sizeof held);
    for (int n = 0; n < 32; n++) {
        Range r = S.ssbo[5][n];
        if ((used >> n & 1) && r.addr && r.size && cbuf_conflict(r.addr, r.size)) { compute_flush(); break; }
    }
    for (int n = 0; n < 16; n++) if (S.ubo[5][n].addr && cbuf_conflict(S.ubo[5][n].addr, S.ubo[5][n].size)) { compute_flush(); break; }
    for (int n = 0; n < 16; n++) if (S.ubo[5][n].addr && cbuf_find(S.ubo[5][n].addr, S.ubo[5][n].size)) { compute_flush(); break; }
    for (int n = 0; n < 32; n++) {
        Range r = S.ssbo[5][n];
        GLuint binding = text ? (GLuint)n : 40 + (GLuint)n;
        if (!(used >> n & 1) || !r.addr || !r.size) continue;
        held[n] = written >> n & 1 ? cbuf_get(r.addr, r.size) : cbuf_find(r.addr, r.size);
        if (held[n]) gs_ssbo(binding, held[n]->buf, 0, (ptrdiff_t)r.size);
        else {
            size_t off = arena_put(r.addr, r.size);
            gs_ssbo(binding, g_arena_buf, (ptrdiff_t)off, (ptrdiff_t)r.size);
        }
    }
    if (text) {
        pair_off();
        glBindProgramPipeline(0); g_gs_pipe = 0;
        glEnable(GL_COMPUTE_PROGRAM_NV_);
        glBindProgramARB(GL_COMPUTE_PROGRAM_NV_, st->gl);
        for (int i = 0; i < st->nbuf; i++) {
            int n = st->map[i];
            /* a parameter buffer binding of an assembly program must start at offset 0 of its buffer object (the driver answers
             * INVALID_VALUE "<offset> must be zero" otherwise), so each one gets a buffer of its own instead of a range of the stream */
            static GLuint param[16];
            if (!param[i]) glCreateBuffers(1, &param[i]);
            if (n == TEXTURE_HANDLE_BUFFER) glNamedBufferData(param[i], 256, (const uint8_t *)S.gl_tex[5] + 32, GL_STREAM_DRAW_);
            else if (n < 16 && S.ubo[5][n].addr && S.ubo[5][n].size) glNamedBufferData(param[i], (ptrdiff_t)S.ubo[5][n].size, P(S.ubo[5][n].addr), GL_STREAM_DRAW_);
            else continue;
            glBindBufferRange(k_pbuf_target[5], (GLuint)i, param[i], 0, n == TEXTURE_HANDLE_BUFFER ? 256 : (ptrdiff_t)S.ubo[5][n].size);
        }
    } else {
        glDisable(GL_COMPUTE_PROGRAM_NV_);
        compute_bind_glsl(p);
    }
    nvn_front_ccmp_dispatch(S.ubo[5], S.ssbo[5], written, st->code, st->code_len, gx, gy, gz);      /* SSBU_COMPUTE_CMP */
    if (g_frag_ssbo_pending) { glMemoryBarrier(0x2000 | 0x4 /* SHADER_STORAGE | UNIFORM */); g_frag_ssbo_pending = 0; }     /* what fragment stages wrote */
    ga_barrier();                                  /* textures the GPU decoder filled while this dispatch's handles were resolved */
    glDispatchCompute(gx, gy, gz);
    if (glsl && st->glsl_images_w) glMemoryBarrier(0xffffffffu);
    if (written) {
        /* later draws and dispatches read these buffers from GL (bound as storage, uniform, vertex, index or indirect data): without
         * the barrier they could see part of the old contents (the island's shadow fit is computed this way: it jumped for a frame) */
        glMemoryBarrier(0x2000 | 0x200 | 0x4 | 0x1 | 0x2 | 0x40 /* SHADER_STORAGE | BUFFER_UPDATE | UNIFORM | VERTEX_ATTRIB | ELEMENT | COMMAND */);
        if (glsl && log) {                       /* a translated program reports which storage buffers it really wrote (a GPU sync: only for the log) */
            uint32_t wm = 0;
            glGetNamedBufferSubData(g_wmask_buf, 0, 4, &wm);
            written &= wm;
            printf("    storage buffers written by this dispatch: 0x%x\n", wm);
        }
        for (int n = 0; n < 32; n++) if ((written >> n & 1) && held[n]) held[n]->dirty = 1;
        if (log) {
            compute_flush();
            for (int n = 0; n < 32; n++) if ((written >> n & 1) && held[n]) { char tag[40]; snprintf(tag, sizeof tag, "storage %d after ", n); mem_stats(tag, S.ssbo[5][n].addr, S.ssbo[5][n].size); }
        }
    }
    if (text) glDisable(GL_COMPUTE_PROGRAM_NV_);
    g_chist[h].run++;
    g_st.dispatch_run++; g_fs_disp++;
}



typedef struct { uint64_t cmd_size, compute_size, control_size, flush_threshold; int flags, priority; volatile LONG64 cmd_used, control_used, compute_used; } QueueObj;
static uint64_t now_ns(void);
static void fence_now(SyncObj *o, int flags);
static void event_signal_at(uint64_t a, int mode, uint32_t value);
/* ---- counters (nvnCommandBufferResetCounter / ReportCounter): OpenGL queries that run from one report to the next */
static GLenum counter_target(int type) {
    static const GLenum m[16] = {0, 0x8914 /* SAMPLES_PASSED */, 0x82EE /* VERTICES_SUBMITTED */, 0x82EF /* PRIMITIVES_SUBMITTED */, 0x82F0 /* VERTEX_SHADER_INVOCATIONS */,
                                 0x82F1 /* TESS_CONTROL_SHADER_PATCHES */, 0x82F2 /* TESS_EVALUATION_SHADER_INVOCATIONS */, 0x887F /* GEOMETRY_SHADER_INVOCATIONS */,
                                 0x82F4 /* FRAGMENT_SHADER_INVOCATIONS */, 0, 0x82F3 /* GEOMETRY_SHADER_PRIMITIVES_EMITTED */, 0x82F6 /* CLIPPING_INPUT_PRIMITIVES */,
                                 0x82F7 /* CLIPPING_OUTPUT_PRIMITIVES */, 0x8C87 /* PRIMITIVES_GENERATED */, 0x8C88 /* TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN */, 0};
    return type >= 0 && type < 16 ? m[type] : 0;
}
static void counter_stop(int type) {
    uint64_t r = 0;
    if (type < 0 || type >= 17 || !g_counter[type].active) return;
    glEndQuery(counter_target(type));
    if (gl_has(glGetQueryObjectui64v, "glGetQueryObjectui64v")) glGetQueryObjectui64v(g_counter[type].q, 0x8866 /* QUERY_RESULT */, &r);
    g_counter[type].acc += r; g_counter[type].active = 0;
}
static void counter_start(int type) {
    if (!counter_target(type)) return;
    if (!g_counter[type].q) glGenQueries(1, &g_counter[type].q);
    glBeginQuery(counter_target(type), g_counter[type].q);
    g_counter[type].active = 1;
}
/* What a multisample state asks for beyond the two enables; some of it is state of the bound framebuffer. */
static void apply_multisample(void) {
    const uint8_t *b = S.ms;
    static int warned;
    unsigned fl = b[3];
    if (b[5]) { if (gl_has(glRasterSamplesEXT, "glRasterSamplesEXT")) { glEnable(0x9327 /* RASTER_MULTISAMPLE_EXT */); glRasterSamplesEXT(b[5], GL_TRUE); } } else if (S.ms_ext & 1) glDisable(0x9327);
    if (((fl >> 3) & 3) || (S.ms_ext & 2)) { static const GLenum m[4] = {GL_NONE, GL_RGB, GL_ALPHA, GL_RGBA}; if (gl_has(glCoverageModulationNV, "glCoverageModulationNV")) glCoverageModulationNV(m[(fl >> 3) & 3]); }
    if (fl & 1) { if (gl_has(glFragmentCoverageColorNV, "glFragmentCoverageColorNV")) { glEnable(0x92DD /* FRAGMENT_COVERAGE_TO_COLOR_NV */); glFragmentCoverageColorNV(fl >> 5); } } else if (S.ms_ext & 4) glDisable(0x92DD);
    if ((fl & 2) && S.have_fbo) {
        GLfloat loc[32];
        for (int i = 0; i < 16; i++) { loc[2 * i] = (float)(b[8 + i] & 15) / 16.0f; loc[2 * i + 1] = (float)(b[8 + i] >> 4) / 16.0f; }
        if (gl_has(glFramebufferSampleLocationsfvNV, "glFramebufferSampleLocationsfvNV") && gl_has(glFramebufferParameteri, "glFramebufferParameteri")) {
            glFramebufferParameteri(GL_FRAMEBUFFER_, 0x9342 /* FRAMEBUFFER_PROGRAMMABLE_SAMPLE_LOCATIONS_NV */, 1);
            glFramebufferParameteri(GL_FRAMEBUFFER_, 0x9343 /* FRAMEBUFFER_SAMPLE_LOCATION_PIXEL_GRID_NV */, (fl >> 2) & 1);
            glFramebufferSampleLocationsfvNV(GL_FRAMEBUFFER_, 0, b[4] ? b[4] : 1, loc);
        }
    } else if ((S.ms_ext & 8) && S.have_fbo && glFramebufferParameteri) glFramebufferParameteri(GL_FRAMEBUFFER_, 0x9342, 0);
    S.ms_ext = (b[5] ? 1u : 0) | (((fl >> 3) & 3) ? 2u : 0) | ((fl & 1) ? 4u : 0) | ((fl & 2) ? 8u : 0);
    (void)warned;
}

static void copy_region(const uint8_t *p, int *r) { memcpy(r, p, 24); }
/* A copy addresses layers (and 3D slices) by the z of its region; a layered texture is attached one layer at a time (attached
 * whole, the blit read and wrote layer 0 whatever the region said). NVN targets: 2 3D, 3 1D array, 4 2D array, 6 2D multisample
 * array, 8 cube, 9 cube array. */
static int tex_layered(const Tex *t, const View *v) { int tg = v && (v->set & 0xa0) == 0xa0 ? v->target : t->b.target; return tg == 2 || tg == 3 || tg == 4 || tg == 6 || tg == 8 || tg == 9; }
static GLuint temp_fbo_layer(GLenum target, GLuint slot_fbo, Tex *t, const View *v, int layer) {
    Fmt f;
    int kind = fmt_get(t->b.format, &f) ? f.kind : 0;
    GLenum att = kind == 3 ? GL_DEPTH_STENCIL_ATTACHMENT_ : kind == 1 ? GL_DEPTH_ATTACHMENT_ : kind == 2 ? GL_STENCIL_ATTACHMENT_ : GL_COLOR_ATTACHMENT0_;
    glBindFramebuffer(target, slot_fbo);
    glFramebufferTexture(target, GL_COLOR_ATTACHMENT0_, 0, 0);
    glFramebufferTexture(target, GL_DEPTH_STENCIL_ATTACHMENT_, 0, 0);
    if (layer >= 0 && tex_layered(t, v)) glFramebufferTextureLayer(target, att, tex_view_obj(t, v)->gl, 0, layer);
    else glFramebufferTexture(target, att, tex_view_obj(t, v)->gl, 0);
    return (GLuint)(kind == 3 ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : kind == 1 ? GL_DEPTH_BUFFER_BIT : kind == 2 ? GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT);
}
static GLuint temp_fbo(GLenum target, GLuint slot_fbo, Tex *t, const View *v) {
    Fmt f;
    int kind = fmt_get(t->b.format, &f) ? f.kind : 0;
    glBindFramebuffer(target, slot_fbo);
    glFramebufferTexture(target, GL_COLOR_ATTACHMENT0_, 0, 0);
    glFramebufferTexture(target, GL_DEPTH_STENCIL_ATTACHMENT_, 0, 0);
    glFramebufferTexture(target, kind == 3 ? GL_DEPTH_STENCIL_ATTACHMENT_ : kind == 1 ? GL_DEPTH_ATTACHMENT_ : kind == 2 ? GL_STENCIL_ATTACHMENT_ : GL_COLOR_ATTACHMENT0_, tex_view_obj(t, v)->gl, 0);
    return (GLuint)(kind == 3 ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : kind == 1 ? GL_DEPTH_BUFFER_BIT : kind == 2 ? GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT);
}
static GLuint g_fbo_tmp[2];
/* Resolution scale: texels of the game's grid to and from a scaled texture, through a temporary texture of the game's
 * rectangle (one level, one layer) blitted to / from the scaled storage. */
static GLuint rs_temp(Tex *t, int w, int h, GLbitfield *mask, GLenum *att) {
    Fmt f;
    GLuint tmp = 0;
    if (!fmt_get(t->b.format, &f)) return 0;
    *mask = f.kind == 0 ? GL_COLOR_BUFFER_BIT : f.kind == 1 ? GL_DEPTH_BUFFER_BIT : f.kind == 2 ? GL_STENCIL_BUFFER_BIT : GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    *att = f.kind == 0 ? GL_COLOR_ATTACHMENT0_ : f.kind == 1 ? GL_DEPTH_ATTACHMENT_ : f.kind == 2 ? GL_STENCIL_ATTACHMENT_ : GL_DEPTH_STENCIL_ATTACHMENT_;
    glGenTextures(1, &tmp);
    glBindTexture(GL_TEXTURE_2D, tmp);
    glTexStorage2D(GL_TEXTURE_2D, 1, f.internal, w, h);
    return tmp;
}
static void rs_blit(GLuint from, int fl, GLuint to, int tl, GLenum att, const int *a, const int *b, GLbitfield mask) {
    ga_barrier();
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_tmp[0]); glFramebufferTexture(GL_READ_FRAMEBUFFER_, att, from, fl);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_fbo_tmp[1]); glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, att, to, tl);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB_);
    glBlitFramebuffer(a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], mask, mask == GL_COLOR_BUFFER_BIT ? GL_LINEAR : GL_NEAREST);
    glEnable(GL_FRAMEBUFFER_SRGB_); glEnable(GL_SCISSOR_TEST);
    fbo_detach(g_fbo_tmp[0]); fbo_detach(g_fbo_tmp[1]);
    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
}
static void rs_put(Tex *t, const Fmt *f, int level, int x, int y, int w, int h, const void *data) {
    GLbitfield mask; GLenum att;
    GLuint tmp = rs_temp(t, w, h, &mask, &att);
    if (!tmp) return;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, f->format, f->type, data);
    int a[4] = {0, 0, w, h}, b[4] = {rsx(t, x), rsy(t, y), rsx(t, x + w), rsy(t, y + h)};
    rs_blit(tmp, 0, t->o.gl, level, att, a, b, mask);
    glDeleteTextures(1, &tmp);
}
static void rs_get(Tex *t, int level, int x, int y, int w, int h, GLenum fo, GLenum ty, GLsizei bytes, void *out) {
    GLbitfield mask; GLenum att;
    GLuint tmp = rs_temp(t, w, h, &mask, &att);
    if (!tmp) return;
    int a[4] = {rsx(t, x), rsy(t, y), rsx(t, x + w), rsy(t, y + h)}, b[4] = {0, 0, w, h};
    rs_blit(t->o.gl, level, tmp, 0, att, a, b, mask);
    glGetTextureSubImage(tmp, 0, 0, 0, 0, w, h, 1, fo, ty, bytes, out);
    glDeleteTextures(1, &tmp);
}
static void blit_tex(Tex *src, const View *sv, const int *sr, Tex *dst, const View *dv, const int *dr, int linear) {
    if (rtlog_match(dst)) printf("rtlog: frame %lld copy into gl %u mem %p from %dx%d format 0x%x gl %u mem %p\n", (long long)g_st.frames + 1, dst->o.gl, (void *)dst->mem, src->b.width, src->b.height, src->b.format, src->o.gl, (void *)src->mem);
    tex_sync(src); tex_sync(dst);
    dst->gpu_written = 1; dst->rt_gen++;              /* view copies of it are refreshed (view_copies_refresh) */
    int s2[6], d2[6];
    if (src->rw || dst->rw) {                         /* resolution scale: the game's rectangles on the storage of each */
        memcpy(s2, sr, sizeof s2); memcpy(d2, dr, sizeof d2);
        s2[0] = rsx(src, sr[0]); s2[1] = rsy(src, sr[1]); s2[3] = rsx(src, sr[0] + sr[3]) - s2[0]; s2[4] = rsy(src, sr[1] + sr[4]) - s2[1];
        d2[0] = rsx(dst, dr[0]); d2[1] = rsy(dst, dr[1]); d2[3] = rsx(dst, dr[0] + dr[3]) - d2[0]; d2[4] = rsy(dst, dr[1] + dr[4]) - d2[1];
        if ((src->rw != 0) != (dst->rw != 0)) linear = 1;     /* a scaled and an unscaled texture: filtered */
        sr = s2; dr = d2;
    }
    {   /* Block-compressed textures cannot be framebuffer attachments: a copy between them (Tomodachi Life copies the Mii face
         * textures it compresses on the GPU, level by level) failed as an incomplete framebuffer and left the face cards and
         * the new Mii blank. Same size, no mirroring: the blocks are copied as they are. */
        Fmt fs, fd;
        int cs = fmt_get(src->b.format, &fs) && fs.kind == 4, cd = fmt_get(dst->b.format, &fd) && fd.kind == 4;
        if ((cs || cd) && sr[3] == dr[3] && sr[4] == dr[4] && dr[3] > 0 && dr[4] > 0 && gl_has(glCopyImageSubData, "glCopyImageSubData") && src->o.gl && dst->o.gl) {
            int sl = sv && (sv->set & 0x81) == 0x81 ? sv->lvl0 : 0, dl = dv && (dv->set & 0x81) == 0x81 ? dv->lvl0 : 0;
            int sz = sr[2] + (sv && (sv->set & 0x82) == 0x82 ? sv->lay0 : 0), dz = dr[2] + (dv && (dv->set & 0x82) == 0x82 ? dv->lay0 : 0);
            static int nlog;
            while (glGetError()) {}
            glCopyImageSubData(src->o.gl, gl_target(src->b.target, src->b.samples), sl, sr[0], sr[1], sz, dst->o.gl, gl_target(dst->b.target, dst->b.samples), dl, dr[0], dr[1], dz, sr[3], sr[4], sr[5] > 1 ? sr[5] : 1);
            GLenum e = glGetError();
            if (e) logf_limited(&nlog, 10, "nvn_gl: compressed texture copy (format 0x%x -> 0x%x, level %d -> %d, %dx%d at %d,%d) failed with GL error 0x%x\n", src->b.format, dst->b.format, sl, dl, sr[3], sr[4], sr[0], sr[1], e);
            g_st.copies++;
            return;
        }
    }
    if (tex_layered(src, sv) || tex_layered(dst, dv)) {     /* layer by layer: z of the regions, depth = the number of layers */
        int n = sr[5] > 1 ? sr[5] : 1;
        for (int k = 0; k < n; k++) {
            GLuint m = temp_fbo_layer(GL_READ_FRAMEBUFFER_, g_fbo_tmp[0], src, sv, sr[2] + k);
            temp_fbo_layer(GL_DRAW_FRAMEBUFFER_, g_fbo_tmp[1], dst, dv, dr[2] + k);
            glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB_);
            glBlitFramebuffer(sr[0], sr[1], sr[0] + sr[3], sr[1] + sr[4], dr[0], dr[1], dr[0] + dr[3], dr[1] + dr[4], m, linear && m == GL_COLOR_BUFFER_BIT ? GL_LINEAR : GL_NEAREST);
            glEnable(GL_FRAMEBUFFER_SRGB_); glEnable(GL_SCISSOR_TEST);
        }
        fbo_detach(g_fbo_tmp[0]); fbo_detach(g_fbo_tmp[1]);
        glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
        g_st.copies++;
        return;
    }
    GLuint mask = temp_fbo(GL_READ_FRAMEBUFFER_, g_fbo_tmp[0], src, sv);
    temp_fbo(GL_DRAW_FRAMEBUFFER_, g_fbo_tmp[1], dst, dv);
    {   static int dbg = -1, nlog; if (dbg < 0) dbg = getenv("TOMO_GLERR_DRAW") != NULL;
        if (dbg) {
            GLenum rs = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER_), ds = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER_);
            if ((rs != 0x8CD5 || ds != 0x8CD5) && nlog++ < 40)
                printf("blit: incomplete (read 0x%x draw 0x%x): src fmt 0x%x %dx%dx%d target %d levels %d view target %d fmt 0x%x lvl %d+%d lay %d+%d region %d,%d,%d %dx%dx%d | dst fmt 0x%x %dx%dx%d target %d levels %d view target %d fmt 0x%x lvl %d+%d lay %d+%d region %d,%d,%d %dx%dx%d\n",
                       rs, ds, src->b.format, src->b.width, src->b.height, src->b.depth, src->b.target, src->b.levels, sv ? sv->target : -1, sv ? sv->format : 0, sv ? sv->lvl0 : 0, sv ? sv->nlvl : 0, sv ? sv->lay0 : 0, sv ? sv->nlay : 0, sr[0], sr[1], sr[2], sr[3], sr[4], sr[5],
                       dst->b.format, dst->b.width, dst->b.height, dst->b.depth, dst->b.target, dst->b.levels, dv ? dv->target : -1, dv ? dv->format : 0, dv ? dv->lvl0 : 0, dv ? dv->nlvl : 0, dv ? dv->lay0 : 0, dv ? dv->nlay : 0, dr[0], dr[1], dr[2], dr[3], dr[4], dr[5]);
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB_);
    glBlitFramebuffer(sr[0], sr[1], sr[0] + sr[3], sr[1] + sr[4], dr[0], dr[1], dr[0] + dr[3], dr[1] + dr[4], mask, linear && mask == GL_COLOR_BUFFER_BIT ? GL_LINEAR : GL_NEAREST);
    glEnable(GL_FRAMEBUFFER_SRGB_);
    glEnable(GL_SCISSOR_TEST);
    fbo_detach(g_fbo_tmp[0]); fbo_detach(g_fbo_tmp[1]);
    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
    g_st.copies++;
}

static void replay(uint64_t handle, int depth);
static void save_bmp(const char *path, Tex *t);
/* Round 17, TEST ONLY (nvn_dt.h): SSBU_DRAWTAG=1 with SSBU_SHOT_FIGHT: the submissions the game queues between the pad poll
 * that reached a wanted fight frame and the present that is saved are "tagged": g_draw_tag = that fight frame while the
 * render thread replays them (set by a job queued before the submission, cleared by the back end at the present). */
#include "nvn_dt.h"
static int g_draw_tag, g_dt_idx;                 /* render thread */
volatile LONG g_drawtag_next;                    /* game thread: the fight frame the next submission belongs to, 0 = not tagged */
static void *dt_host(uint64_t a) { return P(a); }
static int drawtag_env(void) { static int on = -1; if (on < 0) on = getenv("SSBU_DRAWTAG") != NULL; return on; }
static void job_drawtag(void *arg) { int n = *(int *)arg; if (g_draw_tag != n) { g_draw_tag = n; g_dt_idx = 0; printf("DT begin: the commands of fight frame %d (%s)\n", n, g_vk ? "Vulkan" : "OpenGL"); } }
int nvn_front_draw_tag(void) { return g_draw_tag; }
int nvn_front_draw_tag_next(void) { return g_dt_idx++; }
void nvn_front_draw_tag_clear(void) { if (g_draw_tag) { printf("DT end: fight frame %d, %d draw commands\n", g_draw_tag, g_dt_idx); g_draw_tag = 0; } }
static volatile LONG64 g_drawshot_at;
static int seam_rows(GLuint fbo, int fw, int h) {      /* TEST (SSBU_DRAWSHOT_SEAM): rows of a target with a step between its two centre columns that the neighbours lack */
    int w = 8, x0 = fw / 2 - 4, rows = 0;
    uint8_t *px = x0 >= 0 ? malloc((size_t)w * h * 4) : NULL;
    if (!px) return -1;
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x0, 0, w, h, 0x1908 /* GL_RGBA */, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++) {
        int s[7];
        for (int j = 0; j < 7; j++) { const uint8_t *a = px + ((size_t)y * w + j) * 4; s[j] = abs(a[0] - a[4]) + abs(a[1] - a[5]) + abs(a[2] - a[6]); }
        int nb = (s[1] + s[2] + s[4] + s[5]) / 4;
        rows += s[3] > nb + 6 && s[3] > 2 * nb;
    }
    free(px);
    return rows;
}
/* Debug aid SSBU_DRAWSHOT=<frame>: the colour target after every draw of that frame, as draw_<n>.bmp. */
static void draw_shot(void) {
    static int want = -2;
    Tex fake;
    char path[64];
    static int rtshot = -1;                      /* per draw: getenv only once (a scan of the environment, 2700 draws a frame) */
    if (want == -2) want = getenv("SSBU_DRAWSHOT") ? atoi(getenv("SSBU_DRAWSHOT")) : -1;
    if (rtshot < 0) rtshot = getenv("SSBU_DRAWLOG_RT_SHOT") != NULL;
    if (S.fbo_w && rtshot && g_dlrt_n > 0 && S.color0 && S.color0->b.width == S.fbo_w && dlrt_match()) {      /* level 0 only */
        memset(&fake, 0, sizeof fake);
        fake.b.width = S.fbo_w; fake.b.height = S.fbo_h; if (S.prs) { fake.rw = (int)(S.fbo_w * S.prs + 0.5f); fake.rh = (int)(S.fbo_h * S.prsy + 0.5f); }
        glBindFramebuffer(GL_READ_FRAMEBUFFER_, S.fbo);
        snprintf(path, sizeof path, "dlrt_%03d.bmp", g_dlrt_n); save_bmp(path, &fake);
        fake.b.depth = -1; snprintf(path, sizeof path, "dlrt_%03d_a.bmp", g_dlrt_n); save_bmp(path, &fake);
        return;
    }
    if ((g_st.frames != want && g_st.frames != g_drawshot_at && !(g_draw_tag && getenv("SSBU_DRAWSHOT_BOX"))) || !S.fbo_w) return;      /* round 17: with SSBU_DRAWTAG the box aid follows the tagged fight frame */
    if (getenv("SSBU_DRAWSHOT_SEAM")) {     /* TEST: no files; per draw into a big target, the rows with a step between its two centre
                                             * columns that the neighbours lack (the centre line, user 2026-11-01) */
        if (S.fbo_w < 1280) return;
        printf("  SEAM: frame %lld draw %d fbo %u (%dx%d gl %u) fs #%d tag %08x: %d rows\n", (long long)g_st.frames, g_frame_draw_index - 1, S.fbo, S.fbo_w, S.fbo_h,
               S.color[0] ? S.color[0]->o.gl : 0, S.prog[1] ? S.prog[1]->serial : -1, S.prog[1] ? code_tag(&S.prog[1]->st[1]) : 0, seam_rows(S.fbo, S.fbo_w, S.fbo_h));
        return;
    }
    {   /* SSBU_DRAWSHOT_BOX=x,y,w,h (in 1920x1080 picture coordinates): no files; one line per draw that CHANGED the mean
         * colour of that box of its colour target (finds the draws that paint a given place of the screen) */
        static int box[4] = {-1, 0, 0, 0};
        static unsigned last_fbo; static double last[4];
        if (box[0] == -1) { const char *e = getenv("SSBU_DRAWSHOT_BOX"); box[0] = -2; if (e && sscanf(e, "%d,%d,%d,%d", &box[0], &box[1], &box[2], &box[3]) != 4) box[0] = -2; }
        if (box[0] >= 0) {
            int x = box[0] * S.fbo_w / 1920, y = box[1] * S.fbo_h / 1080, w = box[2] * S.fbo_w / 1920, h = box[3] * S.fbo_h / 1080;
            if (w < 1 || h < 1 || x + w > S.fbo_w || y + h > S.fbo_h) return;
            uint8_t *px = malloc((size_t)w * h * 4);
            double m[4] = {0, 0, 0, 0};
            glBindFramebuffer(GL_READ_FRAMEBUFFER_, S.fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(x, y, w, h, 0x1908 /* GL_RGBA */, GL_UNSIGNED_BYTE, px);
            for (int i = 0; i < w * h; i++) for (int k = 0; k < 4; k++) m[k] += px[i * 4 + k];
            for (int k = 0; k < 4; k++) m[k] /= (double)(w * h);
            free(px);
            if (last_fbo != S.fbo || m[0] != last[0] || m[1] != last[1] || m[2] != last[2] || m[3] != last[3])
                printf("  BOX: draw %d (DT %d) fbo %u (%dx%d): mean RGBA of the box %.2f %.2f %.2f %.2f%s\n", g_frame_draw_index - 1, g_dt_idx - 1, S.fbo, S.fbo_w, S.fbo_h, m[0], m[1], m[2], m[3], last_fbo != S.fbo ? "  (first draw seen on this target)" : "  CHANGED");
            last_fbo = S.fbo; memcpy(last, m, sizeof last);
            return;
        }
    }
    memset(&fake, 0, sizeof fake);
    fake.b.width = S.fbo_w; fake.b.height = S.fbo_h; if (S.prs) { fake.rw = (int)(S.fbo_w * S.prs + 0.5f); fake.rh = (int)(S.fbo_h * (S.prsy ? S.prsy : S.prs) + 0.5f); }
    snprintf(path, sizeof path, "draw_%02d_fbo%u.bmp", g_frame_draw_index - 1, S.fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, S.fbo);
    save_bmp(path, &fake);
}
/* P3 round 2, EXPERIMENT, off by default (SSBU_FIRSTDRAW_WARM=1 turns it on): the first draw with a pair of GLSL
 * programs is preceded by one point drawn with the rasterizer off. Result: it does NOT remove the cost, it only moves it.
 * The driver spends 50-90 ms inside the first draw call of 48-93 of the 420-520 program pairs of a run (3.4-6.2 s per
 * run); with the point, that time is spent in the point instead (tests/logs/p3b_r2c.log: 48 points over 4 ms, 3,979 ms).
 * One run (p3b_warm.log) had only 5 slow pairs, with or without the point: the cost belongs to pairs the DRIVER has
 * never linked on this PC (its own disk cache), not to the first draw of a run. Kept as a measuring aid: the end
 * report counts the slow first draws. What the point does: the vertex stage runs once on vertex 0 of the bound
 * buffers; nothing reaches the framebuffer. It is not done while a counter other than "samples passed" is running or
 * when the vertex stage writes storage buffers. */
static LONG64 g_fdw_n, g_fdw_slow, g_fdw_skipped, g_fdw_ticks;
static void first_draw_warm(void) {
    static uint64_t seen[1 << 16];
    static int off = -1;
    if (off < 0) off = getenv("SSBU_FIRSTDRAW_WARM") == NULL;
    if (off || !g_gs_last_prog[0]) return;
    uint64_t pair = ((uint64_t)g_gs_last_prog[0] << 32) | g_gs_last_prog[1] | 1ull << 63;
    unsigned h = (unsigned)((pair * 0x9E3779B97F4A7C15ULL) >> 48);
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 0xffff) { if (seen[h] == pair) return; if (!seen[h]) { seen[h] = pair; break; } }
    if (!S.prog[0] || S.prog[0]->st[0].glsl != g_gs_last_prog[0] || S.prog[0]->st[0].glsl_ssbo_w) { g_fdw_skipped++; return; }      /* not the GLSL path, or a vertex stage with side effects */
    for (int t = 0; t < 17; t++) if (t != 1 && g_counter[t].active) { g_fdw_skipped++; return; }
    LONG64 t0 = pf_now(), freq;
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    HS_IN("first draw of a program pair: discarded point (driver)", 0);
    glEnable(GL_RASTERIZER_DISCARD_);
    glDrawArrays(GL_POINTS, 0, 1);
    glDisable(GL_RASTERIZER_DISCARD_);
    HS_OUT();
    LONG64 dt = pf_now() - t0;
    g_fdw_n++; g_fdw_ticks += dt; g_fdw_slow += dt * 250 >= freq;
}
/* SSBU_HITCH: a draw call that took more than 4 ms, and whether it was the first draw with this pair of programs. */
static void hitch_draw(LONG64 t0) {
    static uint64_t seen[1 << 16];
    static LONG64 freq, n_first, n_first_slow, n_old_slow;
    static double ms_first, ms_old;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    double ms = (double)(pf_now() - t0) * 1000.0 / (double)freq;
    uint64_t pair = ((uint64_t)g_gs_last_prog[0] << 32) | g_gs_last_prog[1] | 1ull << 63;
    unsigned h = (unsigned)((pair * 0x9E3779B97F4A7C15ULL) >> 48);
    int first = 1;
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 0xffff) { if (seen[h] == pair) { first = 0; break; } if (!seen[h]) { seen[h] = pair; break; } }
    n_first += first;
    if (ms < 4.0) return;
    if (first) { n_first_slow++; ms_first += ms; } else { n_old_slow++; ms_old += ms; }
    if (g_fdlog) {
        int any = 0;
        uint32_t mask = 0;
        ShSrc *a = g_gs_last_prog[0] < 65536 ? g_prog_src[g_gs_last_prog[0]] : NULL, *b = g_gs_last_prog[1] < 65536 ? g_prog_src[g_gs_last_prog[1]] : NULL;
        for (int i = 0; i < 16; i++) mask |= (uint32_t)(S.attr_enabled[i] != 0) << i;
        printf("fd-slow: %016llx %x %016llx %x mask %x (enabled %x) %.1f ms frame %lld %s%c", (unsigned long long)(a ? a->h : 0), a ? (unsigned)a->len : 0, (unsigned long long)(b ? b->h : 0), b ? (unsigned)b->len : 0,
               S.prog[0] ? mask & S.prog[0]->st[0].glsl_dep : 0, mask, ms, (long long)g_st.frames + 1, first ? "first" : "later", 10);
        {   int nc = 0; for (int i = 0; i < 8; i++) nc += S.color[i] != NULL;
            printf("fd-state: prim %u count %u poly %x depth clamp %u discard %u sample shading %d alpha %d func %d ms %02x %02x %02x %02x %02x %02x colour targets %d (format %x) %dx%d ds %llx dep %x key ints %x texint %x %x nattr %d%c",
                   g_fd_prim, g_fd_count, g_fd_poly, g_fd_dclamp, g_fd_rdisc, S.sample_shading, S.prog[1] ? S.prog[1]->st[1].glsl_alpha : -1, S.alpha_func, S.ms[0], S.ms[1], S.ms[2], S.ms[3], S.ms[4], S.ms[5], nc,
                   S.color0 ? S.color0->b.format : 0, S.fbo_w, S.fbo_h, (unsigned long long)S.ds, S.prog[0] ? S.prog[0]->st[0].glsl_dep : 0, S.prog[0] ? S.prog[0]->st[0].glsl_key[0] : 0, S.tex_int[0], S.tex_int[1], S.nattr, 10); }
        for (unsigned i = 0; i < 64; i++) if (g_fd_ring[i].t >= t0 && g_fd_ring[i].text[0]) { any++; printf("fdlog: frame %lld draw %.1f ms: id %u type %x severity %x: %s%c", (long long)g_st.frames + 1, ms, g_fd_ring[i].id, g_fd_ring[i].type, g_fd_ring[i].sev, g_fd_ring[i].text, 10); }
        if (!any) printf("fdlog: frame %lld draw %.1f ms: NO driver message during this draw call%c", (long long)g_st.frames + 1, ms, 10);
    }
    printf("hitch-draw: frame %lld %.1f ms, programs vs %u fs %u, %s | so far: %lld pairs, slow first draws %lld (%.0f ms), slow later draws %lld (%.0f ms)%c", (long long)g_st.frames + 1, ms, g_gs_last_prog[0],
           g_gs_last_prog[1], first ? "FIRST draw with this pair" : "pair drawn before", (long long)n_first, (long long)n_first_slow, ms_first, (long long)n_old_slow, ms_old, 10);
}
/* TOMO_GPUTIME=<ms> (diagnosis, slow): every draw / dispatch is finished on the GPU at once and timed; one over the limit is
 * named with its programs and counts (a GPU hang of 2 s ends the process by a driver reset, so this finds the command). */
const char *nvn_front_cur_op(void) { return g_cur_op ? g_cur_op : "-"; }      /* hostrt.c: the crash report */
static LONG64 g_optime[OP_COUNT], g_opn[OP_COUNT];       /* render thread: time and count per command type (nvn_perf) */
static void gputime_note(const Cmd *m);
/* TEST ONLY, TOMO_GPUPROF=1: GPU time by kind of command. A timestamp before each draw, dispatch, clear and copy; a frame's
 * timestamps are read GP_SLOTS frames later (no stall: the GPU is long done with them), the intervals charged to the
 * command that began them; every 300 frames the 24 costliest kinds (ms a frame). Gaps where the GPU waited for us are
 * charged too, so the numbers mean GPU time only while the GPU is the bottleneck. */
#define GP_Q 16384
#define GP_SLOTS 4
static GLuint g_gp_q[GP_SLOTS][GP_Q];
static char g_gp_key[GP_SLOTS][GP_Q][40];
static int g_gp_cnt[GP_SLOTS], g_gp_slot, g_gp_n, g_gp_on = -1, g_gp_frames, g_gp_tn;
static struct { char key[40]; double ms; long long n; } g_gp_tab[1024];
static void gpuprof_mark(const char *key) {
    if (g_gp_on < 0) g_gp_on = getenv("TOMO_GPUPROF") != NULL;
    if (!g_gp_on || g_gp_n >= GP_Q) return;
    if (!g_gp_q[0][0]) for (int k = 0; k < GP_SLOTS; k++) glGenQueries(GP_Q, g_gp_q[k]);
    glQueryCounter(g_gp_q[g_gp_slot][g_gp_n], 0x8E28 /* TIMESTAMP */);
    snprintf(g_gp_key[g_gp_slot][g_gp_n++], sizeof g_gp_key[0][0], "%s", key);
}
static int gp_cmp(const void *x, const void *y) { double a = ((const typeof(g_gp_tab[0]) *)x)->ms, b = ((const typeof(g_gp_tab[0]) *)y)->ms; return a < b ? 1 : a > b ? -1 : 0; }
static void gpuprof_frame(void) {
    if (g_gp_on <= 0) return;
    gpuprof_mark("(end)");
    g_gp_cnt[g_gp_slot] = g_gp_n;
    g_gp_slot = (g_gp_slot + 1) % GP_SLOTS;          /* the oldest frame's timestamps: read now, then the slot is reused */
    const int slot = g_gp_slot, n = g_gp_cnt[slot];
    g_gp_n = 0; g_gp_cnt[slot] = 0;
    if (!n) return;
    uint64_t prev = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t = 0;
        glGetQueryObjectui64v(g_gp_q[slot][i], 0x8866 /* QUERY_RESULT */, &t);
        if (i) {
            int k = 0;
            while (k < g_gp_tn && strcmp(g_gp_tab[k].key, g_gp_key[slot][i - 1])) k++;
            if (k == g_gp_tn && g_gp_tn < 1024) { memset(&g_gp_tab[k], 0, sizeof g_gp_tab[k]); snprintf(g_gp_tab[g_gp_tn++].key, sizeof g_gp_tab[0].key, "%s", g_gp_key[slot][i - 1]); }
            if (k < 1024) { g_gp_tab[k].ms += (double)(t - prev) / 1e6; g_gp_tab[k].n++; }
        }
        prev = t;
    }
    double per = 300;
    {   static long long wlo = -2, whi;                 /* TOMO_GPUPROF=<from>-<to>: every frame in that window instead of every 300 */
        if (wlo == -2) { wlo = -1; const char *e = getenv("TOMO_GPUPROF"); if (e && strchr(e, '-')) sscanf(e, "%lld-%lld", &wlo, &whi); }
        if (wlo >= 0) { long long f = (long long)g_st.frames - GP_SLOTS; if (f < wlo || f > whi) { g_gp_tn = 0; return; } g_gp_frames = 299; per = 1; }
    }
    if (++g_gp_frames % 300) return;
    qsort(g_gp_tab, (size_t)g_gp_tn, sizeof g_gp_tab[0], gp_cmp);
    double all = 0; for (int k = 0; k < g_gp_tn; k++) all += g_gp_tab[k].ms;
    printf("gpuprof: frame %lld: %.2f ms of GPU a frame;", (long long)g_st.frames, all / per);
    for (int k = 0; k < g_gp_tn && k < 24; k++) printf(" | %s %.2f (%.0f)", g_gp_tab[k].key, g_gp_tab[k].ms / per, (double)g_gp_tab[k].n / per);
    printf("%c", 10);
    g_gp_tn = 0;
}
static void gputime_check(const Cmd *m, LONG64 t0) {
    static LARGE_INTEGER fq;
    static int nlog;
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    glFinish();
    gputime_note(NULL);                              /* marks the record: everything before this line is done on the GPU */
    double ms = (double)(pf_now() - t0) * 1000.0 / (double)fq.QuadPart;
    if (ms < gputime_ms()) return;
    char progs[128] = "";
    for (int i = 0; i < 6; i++) if (S.prog[i]) { char b[24]; snprintf(b, sizeof b, " s%d#%d", i, S.prog[i]->serial); strncat(progs, b, sizeof progs - strlen(progs) - 1); }
    logf_limited(&nlog, 40, "gputime: %s took %.0f ms (frame %lld): a0=%llx a1=%llx a2=%llx a3=%llx a4=%llx a5=%llx a6=%llx; programs%s\n", g_cur_op, ms, (long long)g_st.frames,
                 (unsigned long long)m->a[0], (unsigned long long)m->a[1], (unsigned long long)m->a[2], (unsigned long long)m->a[3], (unsigned long long)m->a[4],
                 (unsigned long long)m->a[5], (unsigned long long)m->a[6], progs);
}
/* The last 8 timed commands go to gputime_last.txt through a file mapping before they run: a GPU hang ends the process
 * (driver reset) before anything can be logged after the command, but the mapped pages survive the process. */
static void gputime_note(const Cmd *m) {
    static char *map;
    static unsigned n;
    if (!map) {
        HANDLE f = CreateFileA("gputime_last.txt", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        HANDLE sec = f != INVALID_HANDLE_VALUE ? CreateFileMappingA(f, NULL, PAGE_READWRITE, 0, 256 * 256, NULL) : NULL;
        map = sec ? MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, 256 * 256) : NULL;
        if (!map) { printf("gputime: no gputime_last.txt mapping (error %lu)\n", GetLastError()); map = (char *)1; return; }
        memset(map, ' ', 256 * 256);
    }
    if (map == (char *)1) return;
    if (!m) { char *sl = map + (n % 256) * 256; int k = snprintf(sl, 255, "#%u ---- finished on the GPU ----\n", n); if (k > 0 && k < 255) memset(sl + k, ' ', 255 - k); n++; return; }
    char progs[128] = "";
    for (int i = 0; i < 6; i++) if (S.prog[i]) { char b[24]; snprintf(b, sizeof b, " s%d#%d", i, S.prog[i]->serial); strncat(progs, b, sizeof progs - strlen(progs) - 1); }
    char *slot = map + (n % 256) * 256;
    uint64_t fh0 = S.gl_tex[1][4], fh1 = S.nvn_tex[1][0];
    int k = snprintf(slot, 255, "#%u frame %lld %s a0=%llx a1=%llx a2=%llx a3=%llx a4=%llx a5=%llx a6=%llx programs%s fbo %dx%d frag handles %llx %llx\n", n, (long long)g_st.frames, g_cur_op,
                     (unsigned long long)m->a[0], (unsigned long long)m->a[1], (unsigned long long)m->a[2], (unsigned long long)m->a[3], (unsigned long long)m->a[4],
                     (unsigned long long)m->a[5], (unsigned long long)m->a[6], progs, S.fbo_w, S.fbo_h, (unsigned long long)fh0, (unsigned long long)fh1);
    if (k > 0 && k < 255) memset(slot + k, ' ', 255 - k);
    n++;
}
static void exec(const Cmd *m, int depth) {
    const uint8_t *blob = BLOB(m);
    const uint64_t *a = m->a;
    g_cur_op = k_op_name[m->op < OP_COUNT ? m->op : 0];
    HS_IN(g_cur_op, 0);                            /* (the hitch report names the command; the scopes inside name its parts) */
    ga_barrier();                                  /* texels the GPU decoder stored are visible to this command */
    /* TOMO_GPUTIME_EVERY=<n>: only every n-th draw / dispatch is finished and timed (the others run pipelined, as without the
     * diagnostic); the last commands are noted for the record either way */
    static int every = -1;
    static unsigned nth;
    if (every < 0) { const char *e = getenv("TOMO_GPUTIME_EVERY"); every = e ? atoi(e) : 0; }
    if (g_gp_on) {
        char key[40];
        switch (m->op) {
        case OP_DRAW_ARRAYS: case OP_DRAW_ELEMENTS: case OP_DRAW_INDIRECT:
            snprintf(key, sizeof key, "draw %dx%d #%d", S.fbo_w, S.fbo_h, S.prog[1] ? S.prog[1]->serial : S.prog[0] ? S.prog[0]->serial : 0); gpuprof_mark(key); break;
        case OP_DISPATCH: case OP_DISPATCH_INDIRECT: case OP_CLEARCOLOR: case OP_CLEARDS: case OP_CLEARTEX: case OP_COPY_BUF_TEX: case OP_COPY_TEX_TEX: case OP_COPY_TEX_BUF: case OP_COPY_BUF_BUF: case OP_DRAW_TEXTURE:
            snprintf(key, sizeof key, "%s %dx%d", g_cur_op, S.fbo_w, S.fbo_h); gpuprof_mark(key); break;
        default: break;
        }
    }
    int noted = gputime_ms() != 0 && m->op != OP_CALL;
    int isdraw = m->op == OP_DRAW_ARRAYS || m->op == OP_DRAW_ELEMENTS || m->op == OP_DRAW_INDIRECT || m->op == OP_DISPATCH || m->op == OP_DISPATCH_INDIRECT;
    int timed = noted && (every ? isdraw && ++nth % (unsigned)every == 0 : 1);
    LONG64 gt0 = timed ? pf_now() : 0, oc0 = isdraw || !pf_detail() ? 0 : pf_now();
    if (noted) gputime_note(m);
    if (g_rbp_n) switch (m->op) {             /* commands that touch guest memory: read-backs in flight over it land first */
        case OP_UPDATE_UBO: rb_land_range(a[0] + a[2], a[3]); break;
        case OP_CLEAR_BUF: rb_land_range(a[0], a[1]); break;
        case OP_COPY_BUF_BUF: rb_land_range(a[0], a[2]); rb_land_range(a[1], a[2]); break;
        case OP_COPY_BUF_TEX: case OP_COPY_TEX_BUF: case OP_EVENT_SIGNAL: case OP_EVENT_WAIT: rb_land(1); break;
        case OP_COUNTER_REPORT: case OP_REPORT_VALUE: rb_land_range(a[1], 16); break;
        case OP_RENDER_ENABLE: if (a[1]) rb_land_range(a[2], 32); break;
        default: break;
    }
    if (g_cbuf_live) switch (m->op) {         /* commands that write guest memory: GPU-held buffers over it go back first */
        case OP_UPDATE_UBO: cbuf_flush_if(a[0] + a[2], a[3]); break;
        case OP_CLEAR_BUF: cbuf_flush_if(a[0], a[1]); break;
        case OP_COPY_BUF_BUF: cbuf_flush_if(a[0], a[2]); cbuf_flush_if(a[1], a[2]); break;
        case OP_COPY_BUF_TEX: case OP_COPY_TEX_BUF: compute_flush(); break;
        default: break;
    }
    switch (m->op) {
    case OP_DEFERRED: {                        /* NVN 55 Bind*Deferred: the array is read now, when the commands run (a[5] = bind op, a[6] = element size) */
        size_t n = (size_t)a[2], es = (size_t)a[6];
        Cmd *t = malloc(sizeof(Cmd) + n * es);
        if (!t) break;
        *t = *m;
        t->op = (uint32_t)a[5];
        if (a[3]) memcpy(BLOB(t), P(a[3]), n * es); else memset(BLOB(t), 0, n * es);
        exec(t, depth);
        free(t);
        break;
    }
    case OP_TEXPOOL: S.tpool = P(a[0]); break;
    case OP_SMPPOOL: S.spool = P(a[0]); break;
    case OP_PROGRAM:
        for (int s = 0; s < 6; s++) if (a[1] & (1u << s)) { Prog *p = P(a[0]); S.prog[s] = p && !p->dead && p->st[s].state ? p : NULL; }
        break;
    case OP_UBO: case OP_SSBO: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2], max = m->op == OP_UBO ? 16 : 32;
        Range *dst = m->op == OP_UBO ? S.ubo[stage] : S.ssbo[stage];
        for (int i = 0; i < count && first + i < max; i++) memcpy(&dst[first + i], blob + i * 16, 16);
        break;
    }
    case OP_TEXTURES: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2];
        for (int i = 0; i < count && first + i < 32; i++) {
            uint64_t h;
            memcpy(&h, blob + i * 8, 8);
            S.nvn_tex[stage][first + i] = h;
            uint32_t dims[4] = {0, 0, 0, 0};
            int isint = 0;
            unsigned u = (unsigned)(first + i);
            { LONG64 pf0 = pf_detail() ? pf_now() : 0; HS_IN("texture handle (view, sampler, bindless handle)", 0); S.gl_tex[stage][4 + u] = resolve_handle_memo(S.tpool, S.spool, h, dims, &isint); HS_OUT(); if (pf0) pf_add(PF_HANDLE, pf0); }
            {   static long long lo = -2, hi;      /* TOMO_TEXBINDLOG=<from>-<to>: binds of textures born or uploaded in the last 8 frames, or finalized */
                if (lo == -2) { const char *e = getenv("TOMO_TEXBINDLOG"); lo = -1; if (e) sscanf(e, "%lld-%lld", &lo, &hi); }
                if (lo >= 0 && g_st.frames >= lo && g_st.frames <= hi && S.tpool) {
                    uint32_t tid = (uint32_t)h & 0xfffff;
                    Tex *bt = tid < (uint32_t)S.tpool->count ? S.tpool->e[tid].tex : NULL;
                    if (bt && (bt->dead || g_st.frames - bt->born < 8 || (bt->uploaded && g_st.frames - bt->upframe < 8)))
                        printf("texbind: frame %lld draw %d stage %d unit %u id 0x%x: %dx%dx%d fmt 0x%x mem %p gl %u born %lld uploaded %lld%s%s\n", (long long)g_st.frames, g_frame_draw_index, stage, u, tid,
                               bt->b.width, bt->b.height, bt->b.depth, bt->b.format, (void *)bt->mem, bt->o.gl, bt->born, bt->uploaded ? bt->upframe : -1LL, bt->dead ? " DEAD" : "", S.gl_tex[stage][4 + u] == g_dummy_handle ? " DUMMY" : "");
                }
            }
            memcpy(&S.gl_tex[stage][48 + 2 * u], dims, 16);
            S.tex_int[stage] = (S.tex_int[stage] & ~(1u << u)) | ((uint32_t)(isint != 0) << u);
            S.tex_sint[stage] = (S.tex_sint[stage] & ~(1u << u)) | ((uint32_t)(isint == 2) << u);
        }
        S.tex_dirty[stage] = 1;
        break;
    }
    case OP_VBUF:
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 32; i++) memcpy(&S.vbuf[a[0] + i], blob + i * 16, 16);
        {   static long long dl = -2;                   /* SSBU_DRAWLOG: the vertex buffer bindings of that frame too */
            if (dl == -2) dl = getenv("SSBU_DRAWLOG") ? atoll(getenv("SSBU_DRAWLOG")) : -1;
            if (dl >= 0 && (long long)g_st.frames + 1 == dl) {
                uint64_t ad; memcpy(&ad, blob, 8);
                printf("vbuf: before draw %d: first %d count %d, first address %llx\n", g_frame_draw_index, (int)a[0], (int)a[1], (unsigned long long)ad);
            }
        }
        break;
    case OP_VATTR:
        S.nattr = (int)a[0] < 32 ? (int)a[0] : 32;
        memcpy(S.vattr, blob, (size_t)S.nattr * 4);
        break;
    case OP_VSTREAM:
        for (int i = 0; i < (int)a[0] && i < 32; i++) memcpy(&S.vstream[i], blob + i * 8, 8);
        break;
    case OP_BLEND: {
        const uint8_t *b = (const uint8_t *)&a[0];
        glBlendFuncSeparatei(b[0] & 7, blend_func(b[1]), blend_func(b[2]), blend_func(b[3]), blend_func(b[4]));
        glBlendEquationSeparatei(b[0] & 7, blend_eq(b[5]), blend_eq(b[6]));
        if (b[7]) {                            /* advanced blend equation (NV_blend_equation_advanced) instead of the functions above */
            static const uint16_t adv[0x33] = {0, 0, 0, 0, 0, 0 /* ZERO */, 0x9286, 0x9287, 0x9288, 0x9289, 0x928A, 0x928B, 0x928C, 0x928D, 0x928E, 0x928F, 0x1506, 0x9291, 0x92B1, 0x92B2, 0x9292,
                                               0x9294, 0x9295, 0x9296, 0x9297, 0x9298, 0x9299, 0x929A, 0x929B, 0x929C, 0x929E, 0x929F, 0x92B3, 0x92A0, 0x92A1, 0x150A, 0x92A3, 0x92B4, 0x92A4,
                                               0x92A5, 0x92A6, 0x92A7, 0x92A8, 0x92A9, 0x1903, 0x1904, 0x1905, 0x92AD, 0x92AE, 0x92AF, 0x92B0};
            if (b[7] < 0x33 && gl_has(glBlendEquationi, "glBlendEquationi") && gl_has(glBlendParameteriNV, "glBlendParameteriNV")) {
                glBlendParameteriNV(0x9280 /* BLEND_PREMULTIPLIED_SRC_NV */, (b[0] >> 5) & 1);
                glBlendParameteriNV(0x9281 /* BLEND_OVERLAP_NV */, 0x9282 + ((b[0] >> 3) & 3));
                glBlendEquationi(b[0] & 7, adv[b[7]]);
            }
        }
        break;
    }
    case OP_COLOR: {
        const uint8_t *b = (const uint8_t *)&a[0];
        for (unsigned i = 0; i < 8; i++) { if (b[0] & (1u << i)) glEnablei(GL_BLEND, i); else glDisablei(GL_BLEND, i); }
        if (b[1] != 3) { glEnable(GL_COLOR_LOGIC_OP); glLogicOp(GL_CLEAR + (b[1] & 15)); } else glDisable(GL_COLOR_LOGIC_OP);
        S.alpha_func = b[2];
        break;
    }
    case OP_CHANMASK: S.chanmask = (uint32_t)a[0]; apply_chanmask(); break;
    case OP_MULTISAMPLE: {
        const uint8_t *b = (const uint8_t *)&a[0];
        if (b[0]) glEnable(GL_MULTISAMPLE_); else glDisable(GL_MULTISAMPLE_);
        if (b[1]) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE_); else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE_);
        memcpy(S.ms, b, 24);
        if (b[3] || b[5] || S.ms_ext) apply_multisample();
        break;
    }
    case OP_POLYGON: {
        const uint8_t *b = (const uint8_t *)&a[0];
        static const GLenum face[4] = {0, GL_FRONT, GL_BACK, GL_FRONT_AND_BACK};
        if (b[0] & 3) { glEnable(GL_CULL_FACE); glCullFace(face[b[0] & 3]); } else glDisable(GL_CULL_FACE);
        glFrontFace((b[1] != 0) != (g_dbg_flipface != 0) ? GL_CCW : GL_CW);       /* see the note on winding at nvn_prim */
        glPolygonMode(GL_FRONT_AND_BACK, GL_POINT + (b[2] < 3 ? b[2] : 2));
        g_fd_poly = b[0] | (unsigned)b[2] << 8;
        if (b[3] & 1) glEnable(GL_POLYGON_OFFSET_POINT); else glDisable(GL_POLYGON_OFFSET_POINT);
        if (b[3] & 2) glEnable(GL_POLYGON_OFFSET_LINE); else glDisable(GL_POLYGON_OFFSET_LINE);
        if (b[3] & 4) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);
        {   static long long dl = -2;
            if (dl == -2) dl = getenv("SSBU_DRAWLOG") ? atoll(getenv("SSBU_DRAWLOG")) : -1;
            if (dl >= 0 && (long long)g_st.frames + 1 == dl) printf("polygon: before draw %d: cull %d front %d mode %d offset enables %x\n", g_frame_draw_index, b[0], b[1], b[2], b[3]);
        }
        break;
    }
    case OP_DEPTHSTENCIL: {
        uint64_t d = a[0];
        S.ds = d;
        if (d & 1) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask((d >> 1) & 1);
        {   /* TEST ONLY, TOMO_EQUAL_FIX=1: a depth test for equality without depth writes runs as less-or-equal. It once made the
             * Mii of Mii creation visible, but the cause was a second (lazy) clear of its depth between the depth pre-pass and the
             * colour pass (OP_RENDER_ENABLE); less-or-equal also let a 169-vertex mask mesh through as a black card. */
            static int eqfix = -1; if (eqfix < 0) eqfix = getenv("TOMO_EQUAL_FIX") ? atoi(getenv("TOMO_EQUAL_FIX")) : 0;
            GLenum df = nvn_func((unsigned)(d >> 4) & 15);
            if (eqfix && df == 0x202 /* EQUAL */ && !((d >> 1) & 1)) df = 0x203 /* LEQUAL */;
            glDepthFunc(df); }
        if (d & 4) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        glStencilOpSeparate(GL_FRONT, stencil_op((unsigned)(d >> 16) & 15), stencil_op((unsigned)(d >> 20) & 15), stencil_op((unsigned)(d >> 24) & 15));
        glStencilOpSeparate(GL_BACK, stencil_op((unsigned)(d >> 28) & 15), stencil_op((unsigned)(d >> 32) & 15), stencil_op((unsigned)(d >> 36) & 15));
        S.stencil_dirty = 1;
        break;
    }
    case OP_VIEWPORTS:
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) { int k = (int)a[0] + i; memcpy(S.gvp[k], blob + i * 16, 16); S.gvp_set |= 1u << k; apply_vp(k); }
        break;
    case OP_SCISSORS:
        if (g_dbg_noscissor) break;
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) {
            int v[4];
            memcpy(v, blob + i * 16, 16);
            int k = (int)a[0] + i;
            S.gsc[k][0] = v[0]; S.gsc[k][1] = v[1]; S.gsc[k][2] = v[2] < 0 ? 0 : v[2]; S.gsc[k][3] = v[3] < 0 ? 0 : v[3];
            apply_sc(k);
            if (k == 0) memcpy(S.sc0, S.gsc[0], sizeof S.sc0);
        }
        break;
    case OP_DEPTHRANGES:
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) { float v[2]; memcpy(v, blob + i * 8, 8); glDepthRangeIndexed((GLuint)a[0] + (GLuint)i, v[0], v[1]); }
        break;
    case OP_TARGETS: {
        /* At a render target switch, once 300 draws went by since the last flush: the GPU starts on the frame while we replay
         * the rest of it. Without, the driver held the frame until the game's flush at its end, so the GPU's time came after
         * our replay's: 4K at frame_rate = 60 ran at 57 fps (12 ms of GPU after 8 ms of replay), 60.0 with this.
         * TOMO_RT_FLUSH=<draws>: another spacing, 0 = none. */
        static int every = -1; static LONG64 last;
        if (every < 0) every = getenv("TOMO_RT_FLUSH") ? atoi(getenv("TOMO_RT_FLUSH")) : 300;
        if (every > 0 && g_st.draws - last >= every) { HS_IN("render targets: glFlush (every 300 draws)", 0); glFlush(); last = g_st.draws; HS_OUT(); }
        g_fs_rt++; set_targets((int)a[0], (const TargetRef *)blob, (const TargetRef *)blob + 8); if (S.ms_ext & 8) apply_multisample(); break;
    }
    case OP_STENCIL_REF: case OP_STENCIL_VMASK: case OP_STENCIL_MASK: {
        int *dst = m->op == OP_STENCIL_REF ? S.sref : m->op == OP_STENCIL_VMASK ? S.svmask : S.swmask;
        if (a[0] & 1) dst[0] = (int)a[1];
        if (a[0] & 2) dst[1] = (int)a[1];
        S.stencil_dirty = 1;
        break;
    }
    case OP_BLENDCOLOR: { float v[4]; memcpy(v, blob, 16); glBlendColor(v[0], v[1], v[2], v[3]); break; }
    case OP_ALPHAREF: memcpy(&S.alpha_ref, &a[0], 4); break;                       /* applied per draw (draw_prepare) */
    case OP_POINTSIZE: { float v; memcpy(&v, &a[0], 4); glPointSize(g_rsf > 0 ? v * g_rsf : v); break; }      /* resolution scale: in real pixels */
    case OP_LINEWIDTH: { float v; memcpy(&v, &a[0], 4); glLineWidth(g_rsf > 0 ? v * g_rsf : v); break; }
    case OP_POLYOFFSET: { float v[3]; memcpy(v, &a[0], 12); glPolygonOffsetClamp(v[0], v[1], v[2]);
        {   static long long dl = -2;
            if (dl == -2) dl = getenv("SSBU_DRAWLOG") ? atoll(getenv("SSBU_DRAWLOG")) : -1;
            if (dl >= 0 && (long long)g_st.frames + 1 == dl) printf("polyoffset: before draw %d: factor %g units %g clamp %g\n", g_frame_draw_index, v[0], v[1], v[2]);
        }
        break; }      /* (factor, units, clamp) */
    case OP_SAMPLEMASK: if ((uint32_t)a[0] == 0xffffffffu) glDisable(0x8E51); else { glEnable(0x8E51); glSampleMaski(0, (GLbitfield)a[0]); } break;
    case OP_RASTDISCARD: g_fd_rdisc = a[0] & 1; if (a[0] & 1) glEnable(GL_RASTERIZER_DISCARD_); else glDisable(GL_RASTERIZER_DISCARD_); break;
    case OP_DEPTHCLAMP: g_fd_dclamp = a[0] & 1; if (a[0] & 1) glEnable(GL_DEPTH_CLAMP_); else glDisable(GL_DEPTH_CLAMP_); break;
    case OP_PRIMRESTART: if (a[0] & 1) { glEnable(GL_PRIMITIVE_RESTART_); glPrimitiveRestartIndex((GLuint)a[1]); } else glDisable(GL_PRIMITIVE_RESTART_); break;
    case OP_PATCHSIZE: glPatchParameteri(GL_PATCH_VERTICES_, (GLint)a[0]); break;
    case OP_CLEARCOLOR: {
        float v[4];
        unsigned i = (unsigned)a[0] & 7, mask = (unsigned)a[2];
        if (!S.have_fbo || S.render_off) break;
        memcpy(v, blob, 16);
        if (S.color[i]) {
            S.color[i]->frz_restf = (long long)g_st.frames + 1;
            if (mask & 7) S.color[i]->frz_depth = 0;     /* a new colour background invalidates the old 3D history on later frames too */
        }
        if (drawlog_now() || rtlog_match(S.color[i])) printf("clear: frame %lld colour %u of fbo %u (%dx%d, gl %u): %g %g %g %g mask %x\n", (long long)g_st.frames + 1, i, S.fbo, S.fbo_w, S.fbo_h,
                                                          S.color[i] ? S.color[i]->o.gl : 0, v[0], v[1], v[2], v[3], mask);
        glColorMaski(i, mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
        if (a[5] == 1) glClearBufferiv(GL_COLOR, (GLint)i, (const GLint *)v);              /* ClearColori / ClearColorui say what the four words are */
        else if (a[5] == 2) glClearBufferuiv(GL_COLOR, (GLint)i, (const GLuint *)v);
        else
        {
            Fmt f;
            int isint = S.color[i] && fmt_get(S.color[i]->b.format, &f) && f.kind == 0 && (f.format == 0x8D94 || f.format == 0x8228 || f.format == 0x8D98 || f.format == 0x8D99);
            if (!isint) glClearBufferfv(GL_COLOR, (GLint)i, v);      /* NVN passes the same four words for ClearColori / ClearColorui */
            else if (f.type == 0x1400 || f.type == 0x1402 || f.type == 0x1404) glClearBufferiv(GL_COLOR, (GLint)i, (const GLint *)v);
            else glClearBufferuiv(GL_COLOR, (GLint)i, (const GLuint *)v);
        }
        apply_chanmask();
        g_st.clears++; g_frame_clears++; if (S.fbo_w >= 2048) g_fs_bigclr++;
        break;
    }
    case OP_CLEARDS: {
        float depth;
        GLbitfield bits = (a[0] & 1 ? GL_DEPTH_BUFFER_BIT : 0) | ((uint32_t)a[2] ? GL_STENCIL_BUFFER_BIT : 0);
        if (!S.have_fbo || !bits || S.render_off) break;
        memcpy(&depth, &a[3], 4);
        if (drawlog_now()) printf("clear: frame %lld depth/stencil of fbo %u: bits %x depth %g stencil %d%c", (long long)g_st.frames + 1, S.fbo, (unsigned)bits, depth, (int)a[1], 10);
        glDepthMask(GL_TRUE);
        glStencilMaskSeparate(GL_FRONT_AND_BACK, (GLuint)a[2]);
        glClearDepth(depth);
        glClearStencil((GLint)a[1]);
        glClear(bits);
        glDepthMask((S.ds >> 1) & 1);
        S.stencil_dirty = 1;
        g_st.clears++; g_frame_clears++; if (S.fbo_w >= 2048) g_fs_bigclr++;
        break;
    }
    case OP_DRAW_ARRAYS: {
        LONG64 pf0 = pf_detail() ? pf_now() : 0;
        int ok;
        feedback_guard();
        { HS_IN("draw state (pipeline, bindings)", 0); ok = draw_prepare(); HS_OUT(); }
        if (pf0) pf_add(PF_PREP, pf0);
        if (g_draw_tag) dt_line("gl", g_draw_tag, g_dt_idx++, ok, (unsigned)a[0], -1, (int)a[2], 0, (long long)a[1], (unsigned)a[3], (unsigned)a[4], S.prog, S.nattr, S.vattr, S.vbuf, (const uint32_t *)S.vstream, S.ubo, S.ssbo, dt_host);
        if (!ok) { g_st.draws_skipped++; g_frame_skipped++; break; }
        if (drawlog_now()) printf("  DrawArrays mode %d first %d count %d base instance %d instances %d%c", (int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], 10);
        drawlog_begin();
        first_draw_warm();
        pf0 = pf_detail() || g_hitch_ms ? pf_now() : 0;
        g_fd_prim = (unsigned)a[0]; g_fd_count = (unsigned)a[2];
        { HS_IN("draw call (driver)", 0); glDrawArraysInstancedBaseInstance(nvn_prim((unsigned)a[0]), (GLint)a[1], (GLsizei)a[2], (GLsizei)a[4], (GLuint)a[3]); HS_OUT(); }
        feedback_restore();
        if (pf0) pf_add(PF_DRAWCALL, pf0);
        if (g_hitch_ms) hitch_draw(pf0);
        drawlog_end();
        draw_shot();
        g_st.draws++; g_frame_draws++; count_dummy_samples(); if (S.fbo_w >= 2048) g_fs_big++;
        break;
    }
    case OP_DRAW_ELEMENTS: {
        static const GLenum itype[3] = {GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT};
        unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1;
        int count = (int)a[2];
        LONG64 pf0 = pf_detail() ? pf_now() : 0;
        int ok;
        {   /* TEST ONLY, TOMO_WATCHDRAW=<index count>,<uniform binding>,<byte offset>,<from frame>: at the first such draw, a
             * hardware write watchpoint on that word of the vertex stage's buffer (debug_mode.c: the writers' guest stacks) */
            static int wc = -2, wb, wo, wf;
            if (wc == -2) { const char *e = getenv("TOMO_WATCHDRAW"); wc = -1; if (e && sscanf(e, "%d,%d,%d,%d", &wc, &wb, &wo, &wf) != 4) wc = -1; }
            if (wc > 0 && count == wc && (int)g_st.frames >= wf && wb >= 0 && wb < 16 && S.ubo[0][wb].addr) {
                void debug_watch(const uint64_t *addrs, int n);
                uint64_t w = (uint64_t)(uintptr_t)P(S.ubo[0][wb].addr) + (uint64_t)wo;
                printf("watchdraw: frame %lld: draw of %d indices, binding %d at %llx+%d = host %llx (value %08x)\n", (long long)g_st.frames, count, wb,
                       (unsigned long long)S.ubo[0][wb].addr, wo, (unsigned long long)w, *(uint32_t *)(uintptr_t)w);
                {   /* the 48 bytes (a 3x4 matrix) at the start of that buffer: the other copies in memory (the scene's own); the first
                     * outside the buffer is watched at the same offset */
                    uint8_t pat[48]; const uint8_t *src = P(S.ubo[0][wb].addr);
                    memcpy(pat, src, sizeof pat);
                    printf("watchdraw: matrix"); for (int i = 0; i < 12; i++) { float f; memcpy(&f, pat + 4 * i, 4); printf(" %g", f); } printf("\n");
                    SYSTEM_INFO si; GetSystemInfo(&si);
                    uint8_t *p = (uint8_t *)si.lpMinimumApplicationAddress; MEMORY_BASIC_INFORMATION mi; int hits = 0; uint64_t first = 0;
                    while (p < (uint8_t *)si.lpMaximumApplicationAddress && VirtualQuery(p, &mi, sizeof mi)) {
                        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && mi.RegionSize < (1ull << 32)) {
                            const uint8_t *bb = mi.BaseAddress;
                            for (size_t i = 0; i + 48 <= mi.RegionSize; i += 4)
                                if (bb[i] == pat[0] && !memcmp(bb + i, pat, 48) && bb + i != src) {
                                    if (hits < 16) printf("watchdraw: copy at host %p\n", (const void *)(bb + i));
                                    if (!first) first = (uint64_t)(uintptr_t)(bb + i);
                                    hits++;
                                }
                        }
                        p = (uint8_t *)mi.BaseAddress + mi.RegionSize;
                    }
                    printf("watchdraw: %d copies\n", hits);
                    if (first) w = first + (uint64_t)wo;
                }
                debug_watch(&w, 1);
                wc = -1;
            }
        }
        feedback_guard();
        g_draw_ibuf.addr = a[3]; g_draw_ibuf.size = (uint64_t)(count > 0 ? count : 0) << t;
        { HS_IN("draw state (pipeline, bindings)", 0); ok = count > 0 && a[3] && draw_prepare(); HS_OUT(); }
        g_draw_ibuf.addr = 0;
        if (g_draw_tag) dt_line("gl", g_draw_tag, g_dt_idx++, ok, (unsigned)a[0], (int)t, count, a[3], (long long)(int32_t)a[4], (unsigned)a[5], (unsigned)a[6], S.prog, S.nattr, S.vattr, S.vbuf, (const uint32_t *)S.vstream, S.ubo, S.ssbo, dt_host);
        if (!ok) { if (pf0) pf_add(PF_PREP, pf0); g_st.draws_skipped++; g_frame_skipped++; break; }
        size_t off = arena_put(a[3], (uint64_t)count << t);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, g_arena_buf);
        if (pf0) pf_add(PF_PREP, pf0);
        if (drawlog_now()) {
            printf("  DrawElements mode %d type %d count %d base vertex %d base instance %d instances %d%c", (int)a[0], (int)t, count, (int)a[4], (int)a[5], (int)a[6], 10);
            uint32_t mx = 0, mn = 0xffffffffu, first[6] = {0};
            for (int k = 0; k < count; k++) { uint32_t ix = t == 0 ? rd8(a[3] + (uint64_t)k) : t == 1 ? rd16(a[3] + (uint64_t)k * 2) : rd32(a[3] + (uint64_t)k * 4); if (k < 6) first[k] = ix; if (ix > mx) mx = ix; if (ix < mn) mn = ix; }
            printf("  indices at %llx: %u %u %u %u %u %u ... range %u..%u%c", (unsigned long long)a[3], first[0], first[1], first[2], first[3], first[4], first[5], mn, mx, 10);
            if (S.nattr > 0 && (S.vattr[0] & 0xff) == 0x22 && mn <= mx) {          /* attribute 0 as three floats: the box of the vertices drawn */
                int st = (int)((S.vattr[0] >> 8) & 31);
                uint64_t stride = S.vstream[st].stride, base = S.vbuf[st].addr + (S.vattr[0] >> 14);
                float bmn[3] = {1e30f, 1e30f, 1e30f}, bmx[3] = {-1e30f, -1e30f, -1e30f};
                for (uint64_t v = mn + (uint64_t)(int64_t)(int32_t)a[4]; S.vbuf[st].addr && stride >= 12 && v <= mx + (uint64_t)(int64_t)(int32_t)a[4] && (v * stride + 12 + (S.vattr[0] >> 14)) <= S.vbuf[st].size; v++) {
                    const float *p = P(base + v * stride);
                    for (int k = 0; k < 3; k++) { if (p[k] < bmn[k]) bmn[k] = p[k]; if (p[k] > bmx[k]) bmx[k] = p[k]; }
                }
                printf("  drawn box: %g..%g %g..%g %g..%g%c", bmn[0], bmx[0], bmn[1], bmx[1], bmn[2], bmx[2], 10);
            }
        }
        static int dsclear = -2;
        if (dsclear == -2) dsclear = getenv("SSBU_DRAWSHOT_CLEAR") && getenv("SSBU_DRAWSHOT") ? atoi(getenv("SSBU_DRAWSHOT")) : -1;
        if (dsclear >= 0 && g_st.frames == dsclear) { glDisable(GL_SCISSOR_TEST); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT); glEnable(GL_SCISSOR_TEST); }
        drawlog_begin();
        first_draw_warm();
        pf0 = pf_detail() || g_hitch_ms ? pf_now() : 0;
        g_fd_prim = (unsigned)a[0]; g_fd_count = (unsigned)count;
        { HS_IN("draw call (driver)", 0); glDrawElementsInstancedBaseVertexBaseInstance(nvn_prim((unsigned)a[0]), count, itype[t], (const void *)off, (GLsizei)a[6], (GLint)a[4], (GLuint)a[5]); HS_OUT(); }
        feedback_restore();
        if (pf0) pf_add(PF_DRAWCALL, pf0);
        if (g_hitch_ms) hitch_draw(pf0);
        drawlog_end();
        draw_shot();
        g_st.draws++; g_frame_draws++; count_dummy_samples(); if (S.fbo_w >= 2048) g_fs_big++;
        break;
    }
    case OP_COPY_BUF_TEX: {                    /* a0 buffer address, a1 texture, a4 flags, a5 / a6 row / image stride of its command buffer; blob: view, region */
        /* PR #14 (xtlg511): replay with the strides recorded in the command (a5/a6), not the running S.row_stride/S.image_stride */
        Tex *t = P(a[1]);
        View v;
        Fmt f;
        int r[6];
        if (!t || t->dead || !a[0]) break;
        memcpy(&v, blob, sizeof v);
        copy_region(blob + 24, r);
        if (!t->o.gl) tex_realize(t);
        if (!fmt_get(t->b.format, &f)) break;
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0;
        {   /* one line per distinct destination (format, size): shows what is updated this way (movie frames, glyph caches, ...) */
            static struct { int fmt, w, h; } seen[32];
            static int nseen;
            int k;
            for (k = 0; k < nseen && !(seen[k].fmt == t->b.format && seen[k].w == t->b.width && seen[k].h == t->b.height); k++) {}
            if (k == nseen && nseen < 32) {
                size_t nz = 0, bytes = (size_t)r[3] * (size_t)r[4] * f.bpb / (size_t)(f.bw * f.bh);
                for (size_t q = 0; q < bytes; q++) nz += ((const uint8_t *)P(a[0]))[q] != 0;
                seen[nseen].fmt = t->b.format; seen[nseen].w = t->b.width; seen[nseen++].h = t->b.height;
                printf("nvn_gl: CopyBufferToTexture into %dx%dx%d format 0x%x target %d levels %d flags 0x%x swizzle %d%d%d%d: region %d,%d,%d %dx%dx%d level %d layer %d, row stride %lld, %zu of %zu source bytes non-zero\n",
                       t->b.width, t->b.height, t->b.depth, t->b.format, t->b.target, t->b.levels, t->b.flags, t->b.swz[0], t->b.swz[1], t->b.swz[2], t->b.swz[3],
                       r[0], r[1], r[2], r[3], r[4], r[5], level, layer, (long long)a[5], nz, bytes);     /* PR #14 (xtlg511): the command's own row stride */
            }
        }
        /* PR #14 (xtlg511): the strides of the command buffer that recorded this copy (a5 / a6), not the running S state */
        if (a[5] && f.kind != 4) glPixelStorei(GL_UNPACK_ROW_LENGTH_, (GLint)((int64_t)a[5] / f.bpb));      /* nvnCommandBufferSetCopyRowStride */
        if (a[6] && a[5] && f.kind != 4) glPixelStorei(0x806E /* UNPACK_IMAGE_HEIGHT */, (GLint)((int64_t)a[6] / (int64_t)a[5]));   /* SetCopyImageStride */
        if (t->b.target != 2 && t->b.target != 4 && t->b.target != 9 && t->b.target != 8) layer = 0;
        else if (t->b.target != 2) layer += r[2];
        tex_put(t, &f, level, layer, r[0], r[1], r[2], r[3], r[4], r[5] > 0 ? r[5] : 1, P(a[0]));
        glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0); glPixelStorei(0x806E, 0);
        t->gpu_written = 1; t->uploaded = 1; t->rt_gen++;
        g_st.copies++;
        break;
    }
    case OP_COPY_TEX_TEX: {                    /* a0 src, a3 dst, a6 flags; blob: src view, src region, dst view, dst region */
        Tex *s = P(a[0]), *d = P(a[3]);
        View sv, dv;
        int sr[6], dr[6];
        if (!s || !d || s->dead || d->dead) break;
        memcpy(&sv, blob, sizeof sv); copy_region(blob + 24, sr);
        memcpy(&dv, blob + 48, sizeof dv); copy_region(blob + 72, dr);
        if (a[6] & 4) { dr[0] += dr[3]; dr[3] = -dr[3]; }      /* NVN_COPY_FLAGS_MIRROR_X / MIRROR_Y */
        if (a[6] & 8) { dr[1] += dr[4]; dr[4] = -dr[4]; }
        if (drawlog_now()) printf("copy texture: %dx%d gl %u%s region %d,%d %dx%d -> %dx%d gl %u%s region %d,%d %dx%d\n", s->b.width, s->b.height, s->o.gl, s->rw ? " (scaled)" : "", sr[0], sr[1], sr[3], sr[4],
                                  d->b.width, d->b.height, d->o.gl, d->rw ? " (scaled)" : "", dr[0], dr[1], dr[3], dr[4]);
        { LONG64 pf0 = pf_now(); blit_tex(s, &sv, sr, d, &dv, dr, (int)(a[6] & 1)); pf_add(PF_COPY, pf0); }
        break;
    }
    case OP_DOWNSAMPLE: {
        Tex *s = P(a[0]), *d = P(a[1]);
        if (!s || !d || s->dead || d->dead) break;
        int sr[6] = {0, 0, 0, s->b.width, s->b.height, 1}, dr[6] = {0, 0, 0, d->b.width, d->b.height, 1};
        blit_tex(s, NULL, sr, d, NULL, dr, 0);
        break;
    }
    case OP_COPY_TEX_BUF: {                    /* a0 texture, a3 buffer address, a5 / a6 row / image stride of its command buffer; blob: view, region */
        Tex *t = P(a[0]);
        View v;
        Fmt f;
        int r[6];
        if (!t || t->dead || !a[3] || !t->o.gl || !fmt_get(t->b.format, &f)) break;
        memcpy(&v, blob, sizeof v);
        copy_region(blob + 24, r);
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0, d = r[5] > 0 ? r[5] : 1;
        int z = t->b.target == 2 ? r[2] : layer + r[2];
        size_t bytes = ((size_t)(r[3] + f.bw - 1) / f.bw) * ((size_t)(r[4] + f.bh - 1) / f.bh) * f.bpb * (size_t)d;
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        compute_flush();
        /* PR #14 (xtlg511): the strides of the command buffer that recorded this copy (a5 / a6), not the running S state */
        if (a[5] && f.kind != 4) { glPixelStorei(0x0D02 /* PACK_ROW_LENGTH */, (GLint)((int64_t)a[5] / f.bpb)); bytes = (size_t)a[5] * (size_t)r[4] * (size_t)d; }
        if (a[6] && a[5] && f.kind != 4) { glPixelStorei(0x806C /* PACK_IMAGE_HEIGHT */, (GLint)((int64_t)a[6] / (int64_t)a[5])); bytes = (size_t)a[6] * (size_t)d; }
        if (f.kind == 4) glGetCompressedTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, (GLsizei)bytes, P(a[3]));
        else if (t->rw && d == 1) rs_get(t, level, r[0], r[1], r[3], r[4], f.format, f.type, (GLsizei)bytes, P(a[3]));      /* resolution scale */
        else glGetTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, f.format, f.type, (GLsizei)bytes, P(a[3]));
        glPixelStorei(0x0D02, 0); glPixelStorei(0x806C, 0);
        static int copylog = -1; if (copylog < 0) copylog = getenv("TOMO_COPYLOG") != NULL;      /* TOMO_COPYLOG=1: every copy to a buffer, any frame */
        if (drawlog_now() || copylog) printf("copy to buffer: frame %lld texture %dx%d format 0x%x (gl %u%s) region %d,%d %dx%d level %d -> %llx, %zu bytes: %08x %08x%c", (long long)g_st.frames + 1, t->b.width, t->b.height, t->b.format,
                                  t->o.gl, t->rw ? ", scaled" : "", r[0], r[1], r[3], r[4], level, (unsigned long long)a[3], bytes, rd32(a[3]), bytes >= 8 ? rd32(a[3] + 4) : 0, 10);
        ww_note(a[3], bytes);
        g_st.copies++;
        break;
    }
    case OP_COPY_BUF_BUF: if (a[0] && a[1]) memmove(P(a[1]), P(a[0]), (size_t)a[2]); ww_note(a[1], a[2]); break;
    case OP_UPDATE_UBO: if (a[0]) memcpy((uint8_t *)P(a[0]) + a[2], blob, (size_t)a[3]); ww_note(a[0] + a[2], a[3]); break;
    case OP_CLEAR_BUF: if (a[0]) { uint32_t v = (uint32_t)a[2]; for (uint64_t i = 0; i + 4 <= a[1]; i += 4) memcpy((uint8_t *)P(a[0]) + i, &v, 4); } ww_note(a[0], a[1]); break;
    case OP_ROWSTRIDE: S.row_stride = (int64_t)a[0]; break;      /* PR #14 (xtlg511): no longer read by the copies (they carry their own strides); kept as state */
    case OP_IMAGESTRIDE: S.image_stride = (int64_t)a[0]; break;
    case OP_DEPTHBOUNDS: {                     /* depth bounds test: fragments pass when the depth already in the buffer is inside [near, far] */
        float v[2];
        memcpy(v, &a[1], 8);
        if (!gl_has(glDepthBoundsEXT, "glDepthBoundsEXT")) break;
        if (a[0] & 1) { glEnable(0x8890 /* DEPTH_BOUNDS_TEST_EXT */); glDepthBoundsEXT(v[0], v[1]); } else glDisable(0x8890);
        break;
    }
    case OP_CONSRASTER: if (a[0] & 1) glEnable(0x9346 /* CONSERVATIVE_RASTERIZATION_NV */); else glDisable(0x9346); break;
    case OP_CONSDILATE: { float v; memcpy(&v, &a[0], 4); if (gl_has(glConservativeRasterParameterfNV, "glConservativeRasterParameterfNV")) glConservativeRasterParameterfNV(0x9379 /* CONSERVATIVE_RASTER_DILATE_NV */, v); break; }
    case OP_SUBPIXEL: if (gl_has(glSubpixelPrecisionBiasNV, "glSubpixelPrecisionBiasNV")) glSubpixelPrecisionBiasNV((GLuint)a[0], (GLuint)a[1]); break;
    case OP_VPSWIZZLE:
        if (!gl_has(glViewportSwizzleNV, "glViewportSwizzleNV")) break;
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) { int v[4]; memcpy(v, blob + i * 16, 16); glViewportSwizzleNV((GLuint)a[0] + (GLuint)i, 0x9350 + (GLenum)(v[0] & 7), 0x9350 + (GLenum)(v[1] & 7), 0x9350 + (GLenum)(v[2] & 7), 0x9350 + (GLenum)(v[3] & 7)); }
        break;
    case OP_TESSLEVELS: if (gl_has(glPatchParameterfv, "glPatchParameterfv")) glPatchParameterfv(a[0] ? 0x8E74 /* PATCH_DEFAULT_OUTER_LEVEL */ : 0x8E73, (const GLfloat *)blob); break;
    case OP_BARRIER:                           /* NVNbarrierBits: 2 / 4 order fragments (rendering to a texture that is being sampled), 8 indirect data, 0x10 texture memory changed */
        if ((a[0] & 6) && gl_has(glTextureBarrier, "glTextureBarrier")) glTextureBarrier();
        if (a[0] & 8) compute_flush();
        if (a[0] & 0x10) g_serial++;
        break;
    case OP_DISCARD: {                         /* the contents of an attachment are no longer needed */
        GLenum att[2] = {GL_DEPTH_ATTACHMENT_, GL_STENCIL_ATTACHMENT_};
        if (drawlog_now()) printf("discard: %s of fbo %u%c", a[0] == ~0ull ? "depth/stencil" : "colour", S.fbo, 10);
        if (!S.have_fbo || !gl_has(glInvalidateFramebuffer, "glInvalidateFramebuffer")) break;
        if (a[0] == ~0ull) glInvalidateFramebuffer(GL_FRAMEBUFFER_, 2, att);
        else {
            static int keep = -1;            /* TEST TOMO_KEEPCOLOR=1: colour discards are not passed on (the driver may drop the texels; the console keeps them) */
            if (keep < 0) keep = getenv("TOMO_KEEPCOLOR") && atoi(getenv("TOMO_KEEPCOLOR"));
            att[0] = GL_COLOR_ATTACHMENT0_ + (GLenum)(a[0] & 7);
            if (S.color[a[0] & 7] && !keep) glInvalidateFramebuffer(GL_FRAMEBUFFER_, 1, att);
        }
        break;
    }
    case OP_CLEARTEX: {                        /* a0 texture, a4 channel mask, a5 0 float / 1 int / 2 uint, a6 whole level; blob: view, region, colour */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        unsigned mask = (unsigned)a[4];
        if (!t || t->dead || !gl_has(glFramebufferTextureLayer, "glFramebufferTextureLayer")) break;
        memcpy(&v, blob, sizeof v); copy_region(blob + 24, r);
        tex_sync(t);
        t->gpu_written = 1; t->uploaded = 1; t->rt_gen++;
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0, layered = t->b.target == 2 || t->b.target == 3 || t->b.target == 4 || t->b.target == 8 || t->b.target == 9;
        if (a[6]) { r[0] = r[1] = r[2] = 0; r[3] = lvl(t->b.width, level); r[4] = lvl(t->b.height, level); r[5] = t->b.target == 2 ? lvl(t->b.depth, level) : 1; }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_fbo_tmp[1]);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_, 0, 0);
        glScissorIndexed(0, rsx(t, r[0]), rsy(t, r[1]), rsx(t, r[0] + r[3]) - rsx(t, r[0]), rsy(t, r[1] + r[4]) - rsy(t, r[1]));
        glColorMaski(0, mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
        for (int z = 0; z < (r[5] > 0 ? r[5] : 1); z++) {
            if (layered) glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, level, (t->b.target == 2 ? 0 : layer) + r[2] + z);
            else glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, level);
            if (a[5] == 1) glClearBufferiv(GL_COLOR, 0, (const GLint *)(blob + 48)); else if (a[5] == 2) glClearBufferuiv(GL_COLOR, 0, (const GLuint *)(blob + 48)); else glClearBufferfv(GL_COLOR, 0, (const GLfloat *)(blob + 48));
        }
        apply_sc(0);
        apply_chanmask();
        fbo_detach(g_fbo_tmp[1]);
        glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
        g_st.clears++;
        break;
    }
    case OP_DOWNSAMPLE_VIEW: {
        Tex *s = P(a[0]), *d = P(a[1]);
        View sv, dv;
        if (!s || !d || s->dead || d->dead) break;
        memcpy(&sv, blob, sizeof sv); memcpy(&dv, blob + 24, sizeof dv);
        int sl = (sv.set & 0x81) == 0x81 ? sv.lvl0 : 0, dl = (dv.set & 0x81) == 0x81 ? dv.lvl0 : 0;
        int sr[6] = {0, 0, 0, lvl(s->b.width, sl), lvl(s->b.height, sl), 1}, dr[6] = {0, 0, 0, lvl(d->b.width, dl), lvl(d->b.height, dl), 1};
        blit_tex(s, &sv, sr, d, &dv, dr, 0);
        break;
    }
    case OP_DRAW_INDIRECT: {                   /* a0 mode, a1 index type, a2 index buffer, a3 indirect data, a4 draws (or the maximum), a5 stride, a6 bit 0 elements, bit 1 count read from memory */
        static const GLenum itype[3] = {GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT};
        unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1, n = (unsigned)a[4], stride = (unsigned)a[5];
        int elements = (int)(a[6] & 1);
        compute_flush();                       /* the indirect data may have been written by a dispatch */
        if (g_rbp_n) rb_land(1);
        if (a[6] & 2) { uint64_t param; memcpy(&param, blob, 8); uint32_t cnt = param ? rd32(param) : 0; if (cnt < n) n = cnt; }
        if (!stride) stride = elements ? 20 : 16;
        if (drawlog_now()) {
            printf("indirect draw: %s, %u draws (max %u%s), stride %u, data %llx:", elements ? "elements" : "arrays", n, (unsigned)a[4], (a[6] & 2) ? ", count from memory" : "", stride, (unsigned long long)a[3]);
            for (unsigned k = 0; k < n && k < 4 && a[3]; k++) { const uint32_t *q = P(a[3] + (uint64_t)k * stride); printf(" [%u %u %u %u %u]", q[0], q[1], q[2], q[3], elements ? q[4] : 0); }
            printf("\n");
        }
        for (unsigned k = 0; k < n && a[3]; k++) {
            uint32_t d[5];                     /* arrays: count, instances, first, base instance; elements: count, instances, first index, base vertex, base instance */
            memcpy(d, P(a[3] + (uint64_t)k * stride), elements ? 20 : 16);
            if (!d[0] || !d[1]) continue;
            feedback_guard();
            if (!draw_prepare()) { g_st.draws_skipped++; g_frame_skipped++; continue; }
            if (elements) {
                if (!a[2]) continue;
                size_t off = arena_put(a[2] + ((uint64_t)d[2] << t), (uint64_t)d[0] << t);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, g_arena_buf);
                glDrawElementsInstancedBaseVertexBaseInstance(nvn_prim((unsigned)a[0]), (GLsizei)d[0], itype[t], (const void *)off, (GLsizei)d[1], (GLint)d[3], d[4]);
            } else glDrawArraysInstancedBaseInstance(nvn_prim((unsigned)a[0]), (GLint)d[2], (GLsizei)d[0], (GLsizei)d[1], d[3]);
            feedback_restore();
            g_st.draws++; g_frame_draws++; count_dummy_samples();
        }
        break;
    }
    case OP_DISPATCH_INDIRECT: {
        uint32_t g[3] = {0, 0, 0};
        compute_flush();
        if (g_rbp_n) rb_land(1);
        if (a[0]) memcpy(g, P(a[0]), 12);
        compute_dispatch(g[0], g[1], g[2]);
        break;
    }
    case OP_RENDER_ENABLE:                     /* a1 = 0: a0 is the switch; otherwise mode + 1 and a2 the address of two counters (16 bytes each) to compare */
        if (!a[1]) S.render_off = !(a[0] & 1);
        else {
            cbuf_flush_if(a[2], 32);
            int eq = a[2] && rd64(a[2]) == rd64(a[2] + 16);
            /* Tomodachi Life clears a render target lazily: SetRenderEnableConditional(not equal) on the target's 32-byte slot, the
             * clear, then a TIMESTAMP_TOP report into the slot. The slot starts {1, 0 | 0, 0} and nothing else writes it, so the
             * console's skip of a second clear in a frame is not visible in the memory here; a slot reported in this frame
             * counts as equal: a target bound again in the frame keeps its contents. Mii creation binds its depth again for
             * the colour pass after the depth pre-pass and tests it for equality: cleared there, no Mii was drawn (2026-10-24).
             * TOMO_CLEAR_ONCE=0: every conditional clear runs. */
            static int once = -1; if (once < 0) once = !getenv("TOMO_CLEAR_ONCE") || atoi(getenv("TOMO_CLEAR_ONCE"));
            int mem_eq = eq;
            if (!eq && once && a[2] && g_tsrep[(a[2] >> 5) & 255].addr == a[2] && g_tsrep[(a[2] >> 5) & 255].frame == (long long)g_st.frames + 1) eq = 1;
            S.render_off = a[1] == 1 ? !eq : eq;
            {   static long long from = -2; static int left;      /* TEST ONLY: TOMO_CONDLOG=<from frame>:<count> */
                if (from == -2) { const char *e = getenv("TOMO_CONDLOG"); from = -1; if (e) { from = atoll(e); left = strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 400; } }
                if (from >= 0 && (long long)g_st.frames + 1 >= from && left > 0) { left--;
                    printf("condexec: frame %lld draw %d mode %d at %llx: %llx | %llx mem_eq %d forced %d -> %s%c", (long long)g_st.frames + 1, g_frame_draw_index, (int)a[1] - 1, (unsigned long long)a[2],
                           (unsigned long long)rd64(a[2]), (unsigned long long)rd64(a[2] + 16), mem_eq, eq && !mem_eq, S.render_off ? "OFF" : "on", 10); } }
        }
        if (drawlog_now()) printf("render enable: %s mode %d counters %llu %llu -> %s\n", a[1] ? "conditional" : "switch", (int)a[1] - 1,
                                  a[1] && a[2] ? (unsigned long long)rd64(a[2]) : 0ull, a[1] && a[2] ? (unsigned long long)rd64(a[2] + 16) : 0ull, S.render_off ? "OFF" : "on");
        break;
    case OP_COUNTER_RESET: counter_stop((int)a[0]); if (a[0] < 17) g_counter[a[0]].acc = 0; counter_start((int)a[0]); break;
    case OP_COUNTER_REPORT: {                  /* (type, address of an NVNcounterData {counter, timestamp}) */
        uint64_t d[2] = {0, gpu_ts()};
        int type = (int)a[0];
        /* type 16 (TIMESTAMP_TOP): a timestamp in the counter field too. Tomodachi Life reports it twice and draws its island
         * objects (palm trees, shrubs) under SetRenderEnableConditional(not equal) of the pair: two zeros culled them all. */
        if (type == 16) { d[0] = d[1]; if (a[1]) { g_tsrep[(a[1] >> 5) & 255].addr = a[1]; g_tsrep[(a[1] >> 5) & 255].frame = (long long)g_st.frames + 1; } }
        if (type > 0 && type < 16 && counter_target(type)) { counter_stop(type); d[0] = g_counter[type].acc; counter_start(type); }
        if (a[1]) { cbuf_flush_if(a[1], 16); memcpy(P(a[1]), d, 16); ww_note(a[1], 16); }
        if (drawlog_now()) printf("counter report: type %d value %llu at %llx (draw %d)\n", type, (unsigned long long)d[0], (unsigned long long)a[1], g_frame_draw_index);
        break;
    }
    case OP_REPORT_VALUE: if (a[1]) { uint64_t d[2] = {(uint32_t)a[0], gpu_ts()}; cbuf_flush_if(a[1], 16); memcpy(P(a[1]), d, 16); ww_note(a[1], 16); } break;
    case OP_FENCE: if (a[0]) fence_now(P(a[0]), (int)a[2]); break;
    case OP_WAITSYNC: { SyncObj *o = P(a[0]); if (o && o->s && !o->dead) glWaitSync(o->s, 0, 0xFFFFFFFFFFFFFFFFull); break; }
    case OP_EVENT_SIGNAL: event_signal_at(a[0], (int)a[1], (uint32_t)a[4]); break;
    case OP_EVENT_WAIT: {                      /* the queue stops until the event's value equals (mode 0) or has reached (mode 1, wrapping) the value */
        static int nlog;
        uint32_t want = (uint32_t)a[2];
        DWORD t0 = GetTickCount();
        while (a[0]) {
            uint32_t v = rd32(a[0]);
            if (a[1] == 0 ? v == want : (int32_t)(v - want) >= 0) break;
            if (GetTickCount() - t0 > 5000) { logf_limited(&nlog, 10, "nvn_gl: WaitEvent: the event at %llx still holds %u after 5 s (waiting for %s %u); continuing\n", (unsigned long long)a[0], v, a[1] ? ">=" : "==", want); break; }
            Sleep(0);
        }
        break;
    }
    case OP_DRAW_TEXTURE: {                    /* nvn_c4.c: glDrawTextureNV with the depth, stencil and depth bounds tests off */
        GLuint tex = 0, smp = 0;
        static int nlog;
        float r[8], ssx = 1.0f, ssy = 1.0f;
        if (!S.have_fbo || S.render_off) break;
        frozen_scene(0);                         /* DrawTexture disables depth even with a depth attachment bound. */
        {   /* the source takes the texels of the texture drawn last over its memory first, as a draw's textures do (view_copies_refresh) */
            uint32_t tid = (uint32_t)a[0] & 0xfffff;
            Tex *st = S.tpool && tid < (uint32_t)S.tpool->count ? S.tpool->e[tid].tex : NULL;
            if (st && !st->dead && st->o.gl && st->alias_from) alias_refresh(st);
            if (st && st->rw) { ssx = (float)st->rw / (float)st->b.width; ssy = (float)st->rh / (float)st->b.height; }
            if (drawlog_now() || rtlog_match(S.color0))
                printf("drawtexture: frame %lld into fbo %u (%dx%d) from id 0x%x %dx%d format 0x%x gl %u mem %p\n", (long long)g_st.frames + 1, S.fbo, S.fbo_w, S.fbo_h, tid,
                       st ? st->b.width : 0, st ? st->b.height : 0, st ? st->b.format : 0, st ? st->o.gl : 0, st ? (void *)st->mem : NULL);
        }
        if (!resolve_names(S.tpool, S.spool, a[0], &tex, &smp)) { logf_limited(&nlog, 20, "nvn_gl: DrawTexture with handle 0x%llx: no such texture in the bound texture pool; nothing to draw\n", (unsigned long long)a[0]); break; }
        nvn_c4_draw_texture_rect((const NvnC4DrawTexture *)blob, r);      /* game pixels -> the GL storage of both textures (resolution scale, widescreen) */
        if (S.prs && S.prsy) {
            /* widescreen: a 16:9 image keeps its shape, centred (never stretched); a widened source (the scene) fills the target */
            float q = ssx != ssy ? 1.0f : S.prsy / S.prs, half = (float)S.fbo_w * 0.5f;
            for (int i = 0; i < 4; i++) {
                if (!(i & 1)) r[i] = (r[i] - half) * q + half;
                r[i] *= i & 1 ? S.prsy : S.prs;
            }
        }
        r[4] *= ssx; r[6] *= ssx; r[5] *= ssy; r[7] *= ssy;
        if (!nvn_c4_draw_texture_gl(tex, smp, (const NvnC4DrawTexture *)blob, r, S.alpha_func, S.alpha_ref)) {
            printf("nvn: stopping: nvnCommandBufferDrawTexture with a texture view that is not 2D or rectangle: what the GPU does is not established\n");
            fflush(stdout);
            report_and_exit(6);
        }
        g_st.draws++; g_frame_draws++;
        break;
    }
    case OP_COVMODTABLE:
        if (!gl_has(glCoverageModulationTableNV, "glCoverageModulationTableNV")) break;
        if (a[0]) { glEnable(0x9331 /* COVERAGE_MODULATION_TABLE_NV */); glCoverageModulationTableNV(16, (const GLfloat *)blob); } else glDisable(0x9331);
        break;
    case OP_DEBUG:
        if (!gl_has(glPushDebugGroup, "glPushDebugGroup")) break;
        if (a[0] == 0) { if (S.debug_depth < 60) { glPushDebugGroup(0x824A /* DEBUG_SOURCE_APPLICATION */, (GLuint)a[1], -1, (const char *)blob); S.debug_depth++; } }
        else if (a[0] == 1) { if (S.debug_depth > 0) { glPopDebugGroup(); S.debug_depth--; } }
        else glDebugMessageInsert(0x824A, 0x8268 /* DEBUG_TYPE_MARKER */, (GLuint)a[1], 0x826B /* DEBUG_SEVERITY_NOTIFICATION */, -1, (const char *)blob);
        break;
    case OP_DISPATCH: { LONG64 pf0 = pf_now(); HS_IN("compute dispatch", 0); compute_dispatch((unsigned)a[0], (unsigned)a[1], (unsigned)a[2]); HS_OUT(); pf_add(PF_DISPATCH, pf0); break; }
    case OP_IMAGES: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2];
        for (int i = 0; i < count && first + i < 8; i++) memcpy(&S.nvn_img[stage][first + i], blob + i * 8, 8);
        break;
    }
    case OP_CALL:
        for (int i = 0; i < (int)a[0]; i++) { uint64_t h; memcpy(&h, blob + i * 8, 8); if (depth < 8) replay(h, depth + 1); }
        break;
    }
    if (timed) gputime_check(m, gt0);
    if (oc0) { LONG64 dt = pf_now() - oc0; InterlockedExchangeAdd64(&g_pf[PF_OTHERCMD], dt); InterlockedIncrement64(&g_pfn[PF_OTHERCMD]); if (m->op < OP_COUNT) { g_optime[m->op] += dt; g_opn[m->op]++; } }
    if (g_debug) {
        static int nlog;
        GLenum e = glGetError();
        if (e) { g_st.gl_errors++; logf_limited(&nlog, 60, "nvn_gl: GL error 0x%x after %s\n", e, g_cur_op); }
    }
    HS_OUT();
}
#ifdef _WIN32
static volatile LONG g_gl_swap_pending;              /* (below: a finished frame kept for its tick) */
static void gl_swap_now(void);
static int swap_midbatch_ok(void);
#endif
static void replay(uint64_t handle, int depth) {
    size_t len = 0;
    uint8_t *data = list_lookup(handle, &len);
    static int nlog;
    if (!data) { g_st.stale++; logf_limited(&nlog, 10, "nvn_gl: command handle %llx is unknown or expired\n", (unsigned long long)handle); return; }
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        if (m->size < sizeof(Cmd)) break;
        exec(m, depth);
#ifdef _WIN32
        /* a frame kept for its tick goes to the screen AT the tick, also while a batch is being replayed: as a job it waited
         * behind the batch (TOMO_PACE_LEAD: the next frame's, a picture a refresh late) */
        if (g_gl_swap_pending && display_present_due() && swap_midbatch_ok()) gl_swap_now();
#endif
        {   static int on = -1, nlog;                   /* TOMO_GLERR_DRAW: a GL error after any command, with its framebuffer */
            if (on < 0) on = getenv("TOMO_GLERR_DRAW") != NULL;
            GLenum e;
            if (on && (e = glGetError()) != 0) {
                if (nlog++ < 40) {
                    GLint fb = 0; glGetIntegerv(0x8CA6, &fb);
                    printf("glerr: GL error 0x%x after %s (frame %lld): framebuffer %d status 0x%x, colour 0 %dx%d format 0x%x gl %u, depth %dx%d format 0x%x gl %u, have_fbo %d\n", e,
                           k_op_name[m->op < OP_COUNT ? m->op : 0], (long long)g_st.frames + 1, fb, glCheckFramebufferStatus(GL_FRAMEBUFFER_),
                           S.color0 ? S.color0->b.width : 0, S.color0 ? S.color0->b.height : 0, S.color0 ? S.color0->b.format : 0, S.color0 ? S.color0->o.gl : 0,
                           S.depth_tex ? S.depth_tex->b.width : 0, S.depth_tex ? S.depth_tex->b.height : 0, S.depth_tex ? S.depth_tex->b.format : 0, S.depth_tex ? S.depth_tex->o.gl : 0, S.have_fbo);
                }
                while (glGetError()) {}
            }
        }
        off += m->size;
    }
}

static void frozen_clear_selftest(void) {
    if (!getenv("SSBU_FROZEN_CLEAR_SELFTEST")) return;
    Tex target = {0}, depth = {0};
    target.b.target = 1; target.b.width = 1280; target.b.height = 720; target.b.depth = 1;
    target.b.format = 0x25; target.b.levels = target.glevels = 1; target.gpu_written = 1;
    glGenTextures(1, &target.o.gl); glBindTexture(GL_TEXTURE_2D, target.o.gl);
    glTexStorage2D(GL_TEXTURE_2D, 1, 0x8058 /* RGBA8 */, 1280, 720);
    const float red[4] = {1, 0, 0, 1};
    glClearTexImage(target.o.gl, 0, GL_RGBA, GL_FLOAT, red);
    S.color0 = S.color[0] = &target; S.depth_tex = &depth;
    g_st.frames = 0; frozen_scene(S.depth_tex != NULL);
    S.depth_tex = NULL; frozen_scene(S.depth_tex != NULL);           /* keep the red 3D picture */
    const float green[4] = {0, 1, 0, 1};
    uint8_t pixel[4] = {0};
    glClearTexImage(target.o.gl, 0, GL_RGBA, GL_FLOAT, green);
    g_st.frames = 1; frozen_scene(S.depth_tex != NULL);              /* unchanged scenes still restore their saved background */
    glGetTextureSubImage(target.o.gl, 0, 640, 360, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof pixel, pixel);
    int restore_bad = pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255;
    glGenFramebuffers(1, &S.fbo); glBindFramebuffer(GL_FRAMEBUFFER_, S.fbo);
    glFramebufferTexture(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, target.o.gl, 0);
    S.have_fbo = 1; S.fbo_w = 1280; S.fbo_h = 720;
    glDisable(GL_SCISSOR_TEST);
    struct { Cmd cmd; float rgba[4]; } clear = {0};
    clear.cmd.op = OP_CLEARCOLOR; clear.cmd.size = sizeof clear; clear.cmd.a[2] = 15;
    clear.rgba[2] = clear.rgba[3] = 1;
    g_st.frames = 2; exec(&clear.cmd, 0);          /* the game replaces it with a blue UI background */
    g_st.frames = 3; frozen_scene(S.depth_tex != NULL);              /* the following UI frame must not resurrect the red scene */
    glGetTextureSubImage(target.o.gl, 0, 640, 360, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof pixel, pixel);
    GLenum error = glGetError();
    int bad = restore_bad || pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 255 || pixel[3] != 255 || error;
    printf("frozen-clear self-test: unchanged background restoration: %s\n", restore_bad ? "FAIL" : "PASS");
    printf("frozen-clear self-test: next frame is %u,%u,%u,%u (expected 0,0,255,255), GL error 0x%x: %s\n",
           pixel[0], pixel[1], pixel[2], pixel[3], error, bad ? "FAIL" : "PASS");
    fflush(stdout); ExitProcess(bad ? 1 : 0);
}
static void frozen_drawtexture_selftest(void) {
    if (!getenv("SSBU_FROZEN_DRAWTEXTURE_SELFTEST")) return;
    Tex target = {0}, source = {0}, depth = {0};
    target.b.target = source.b.target = 1;
    target.b.width = 1280; target.b.height = 720; source.b.width = source.b.height = 4;
    target.b.depth = source.b.depth = target.b.levels = source.b.levels = target.glevels = source.glevels = 1;
    target.b.format = source.b.format = 0x25; target.gpu_written = source.gpu_written = 1;
    glGenTextures(1, &target.o.gl); glBindTexture(GL_TEXTURE_2D, target.o.gl);
    glTexStorage2D(GL_TEXTURE_2D, 1, 0x8058 /* RGBA8 */, 1280, 720);
    glGenTextures(1, &source.o.gl); glBindTexture(GL_TEXTURE_2D, source.o.gl);
    glTexStorage2D(GL_TEXTURE_2D, 1, 0x8058, 4, 4);
    const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    glClearTexImage(target.o.gl, 0, GL_RGBA, GL_FLOAT, red);
    glClearTexImage(source.o.gl, 0, GL_RGBA, GL_FLOAT, blue);
    S.color0 = S.color[0] = &target; S.depth_tex = &depth;
    g_st.frames = 0; frozen_scene(S.depth_tex != NULL);
    S.depth_tex = NULL; frozen_scene(S.depth_tex != NULL);
    glGenFramebuffers(1, &S.fbo); glBindFramebuffer(GL_FRAMEBUFFER_, S.fbo);
    glFramebufferTexture(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, target.o.gl, 0);
    S.have_fbo = 1; S.fbo_w = 1280; S.fbo_h = 720;
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    TexPoolEnt entry = {0}; entry.tex = &source;
    TexPool pool = {0}; pool.count = 1; pool.e = &entry; S.tpool = &pool;
    struct { Cmd cmd; NvnC4DrawTexture rect; } draw = {0};
    draw.cmd.op = OP_DRAW_TEXTURE; draw.cmd.size = sizeof draw; draw.cmd.a[0] = UINT64_C(1) << 32;
    const float dst[4] = {0, 0, 1280, 720}, src[4] = {0, 0, 4, 4};
    nvn_c4_draw_texture_words(dst, src, &draw.rect);
    g_st.frames = 1; S.depth_tex = &depth;
    exec(&draw.cmd, 0);                          /* DrawTexture is 2D even with a depth attachment */
    S.depth_tex = NULL; frozen_scene(S.depth_tex != NULL);          /* the next ordinary UI draw must retain the blue background */
    uint8_t pixel[4] = {0};
    glGetTextureSubImage(target.o.gl, 0, 640, 360, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof pixel, pixel);
    GLenum error = glGetError();
    int bad = pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 255 || pixel[3] != 255 || error;
    printf("frozen-drawtexture self-test: background %u,%u,%u,%u (expected 0,0,255,255), GL error 0x%x: %s\n",
           pixel[0], pixel[1], pixel[2], pixel[3], error, bad ? "FAIL" : "PASS");
    fflush(stdout); ExitProcess(bad ? 1 : 0);
}

/* ---------------------------------------------------------------- render-thread jobs */
/* Bindless texture handles read by shaders (TEX.B: the handle comes from a register, not a bound unit): every handle the game
 * asked for (nvnDeviceGetTextureHandle) is a pair (texture id, sampler id); the render thread resolves the pairs to resident
 * GL handles into a hash table of BL_SLOTS entries {handle, gl lo, gl hi, used} (SSBO binding 39; the GLSL mx_bl probes it
 * the way bl_note does, mx_translate.h MX_BL_BITS), again whenever a pool registration changed anything. A handle not in
 * the table samples the 1x1 dummy (entry BL_SLOTS).
 * The game asks for about 1,000 new pairs a second while it streams textures (measured 2026-10-26: each pair asked for ~20
 * times within a few seconds, then never again), so once the table is 3/4 full it drops the pairs of destroyed textures
 * (bl_evict). Before, nothing was dropped: the 65,536 entries were full after ~2 minutes of play, every later pair scanned the
 * whole table on the job threads (main-thread stalls) and sampled the dummy. TOMO_BL_CAP=<entries> (test: evict sooner),
 * TOMO_BL_MISSLOG=1 (the shaders count handles they did not find: "bindless misses" every 10 s). */
#define BL_SLOTS (1u << MX_BL_BITS)
static struct { uint32_t nh, gen, slot, seen; uint64_t gl; } g_bl[BL_SLOTS / 2];
static uint32_t g_bl_slot[BL_SLOTS];                      /* open addressing on the handle: 0 = free, else index + 1 */
static int g_bl_n, g_bl_cap = BL_SLOTS / 2;              /* at most half the slots used: short probes (GLSL gives up after BL_PROBE) */
#define BL_PROBE 256
static volatile uint32_t g_bl_now;                        /* seconds; bl_update keeps it */
static SRWLOCK g_bl_lock = SRWLOCK_INIT;
static uint32_t g_tgen[1 << 20], g_sgen[1 << 12];       /* bumped by a registration of that texture / sampler id: only its pairs are resolved again */
static volatile LONG g_pool_gen = 1;
static GLuint g_bl_buf;
static uint32_t bl_home(uint32_t nh) { return (nh * 2654435761u) >> (32 - MX_BL_BITS); }
static int bl_find(uint32_t nh) {                         /* index or -1; the lock held */
    for (uint32_t k = bl_home(nh); g_bl_slot[k]; k = (k + 1) & (BL_SLOTS - 1)) if (g_bl[g_bl_slot[k] - 1].nh == nh) return (int)g_bl_slot[k] - 1;
    return -1;
}
static int bl_has(uint32_t nh) {
    AcquireSRWLockShared(&g_bl_lock);
    int i = bl_find(nh);
    ReleaseSRWLockShared(&g_bl_lock);
    return i >= 0;
}
static void bl_note(uint32_t nh) {
    /* shared lock for the usual case, a pair asked for again: the job threads ask ~20,000 times a second */
    AcquireSRWLockShared(&g_bl_lock);
    int i = bl_find(nh);
    if (i >= 0 && g_bl[i].seen != g_bl_now) g_bl[i].seen = g_bl_now;
    ReleaseSRWLockShared(&g_bl_lock);
    if (i >= 0) return;
    AcquireSRWLockExclusive(&g_bl_lock);
    uint32_t k = bl_home(nh);
    int probe = 0;
    for (; g_bl_slot[k] && g_bl[g_bl_slot[k] - 1].nh != nh; k = (k + 1) & (BL_SLOTS - 1)) probe++;
    if (!g_bl_slot[k] && g_bl_n < g_bl_cap && probe < BL_PROBE) {
        g_bl[g_bl_n].nh = nh; g_bl[g_bl_n].gen = 0; g_bl[g_bl_n].slot = k; g_bl[g_bl_n].seen = g_bl_now; g_bl[g_bl_n].gl = 0;
        g_bl_slot[k] = (uint32_t)++g_bl_n;
    } else if (!g_bl_slot[k]) { static int nlog; logf_limited(&nlog, 5, "nvn_gl: bindless table full (%d handles): handle %08x samples the dummy\n", g_bl_n, nh); }
    ReleaseSRWLockExclusive(&g_bl_lock);
}
/* render thread: drop the pairs whose texture was destroyed and that were not asked for in the last minute; a pair of a live
 * texture is kept however old: the game asks for some handles once and samples them for good, also after registering another
 * texture under the same id (it asked for 1bb00759 once while loading; tried 2026-10-26: dropping by age or by
 * re-registration made shaders miss it every frame). Measured: ~87 % of the pairs are of destroyed textures (they sample the
 * dummy anyway). Then the hash is rebuilt and the whole table uploaded. */
static int bl_evict(void) {                                  /* how many were dropped */
    static uint32_t *up;
    if (!up && !(up = malloc((size_t)BL_SLOTS * 16))) return 0;
    AcquireSRWLockShared(&g_pool_lock);
    AcquireSRWLockExclusive(&g_bl_lock);
    int n0 = g_bl_n, j = 0;
    for (int i = 0; i < g_bl_n; i++) {
        uint32_t t = g_bl[i].nh & 0xfffff;
        Tex *x = t < (uint32_t)S.tpool->count ? S.tpool->e[t].tex : NULL;
        if ((x && !x->dead) || g_bl_now - g_bl[i].seen <= 60) g_bl[j++] = g_bl[i];
    }
    ReleaseSRWLockShared(&g_pool_lock);
    g_bl_n = j;
    if (j == n0) { ReleaseSRWLockExclusive(&g_bl_lock); return 0; }
    memset(g_bl_slot, 0, sizeof g_bl_slot);
    memset(up, 0, (size_t)BL_SLOTS * 16);
    for (int i = 0; i < g_bl_n; i++) {
        uint32_t k = bl_home(g_bl[i].nh);
        while (g_bl_slot[k]) k = (k + 1) & (BL_SLOTS - 1);
        g_bl_slot[k] = (uint32_t)i + 1; g_bl[i].slot = k;
        if (g_bl[i].gen) { uint32_t *e = up + (size_t)k * 4; e[0] = g_bl[i].nh; e[1] = (uint32_t)g_bl[i].gl; e[2] = (uint32_t)(g_bl[i].gl >> 32); e[3] = 1; }
    }
    ReleaseSRWLockExclusive(&g_bl_lock);
    glNamedBufferSubData(g_bl_buf, 0, (ptrdiff_t)BL_SLOTS * 16, up);
    static int nlog;
    logf_limited(&nlog, 20, "nvn_gl: bindless table: dropped %d handles of destroyed textures, %d kept\n", n0 - g_bl_n, g_bl_n);
    return n0 - g_bl_n;
}
static void bl_update(void) {                                /* render thread, before a batch runs */
    static int miss_log = -1;
    static ULONGLONG miss_next;
    g_bl_now = (uint32_t)(GetTickCount64() / 1000);
    if (!g_bl_n || !S.tpool) return;
    if (!g_bl_buf) {                                         /* BL_SLOTS entries + the dummy + the miss counter (TOMO_BL_MISSLOG) */
        uint32_t tail[8] = {0, (uint32_t)g_dummy_handle, (uint32_t)(g_dummy_handle >> 32), 1, 0, 0, 0, 0};
        const char *cap = getenv("TOMO_BL_CAP");
        if (cap && atoi(cap) >= 1024 && atoi(cap) < (int)(BL_SLOTS / 2)) g_bl_cap = atoi(cap);
        glCreateBuffers(1, &g_bl_buf); glNamedBufferData(g_bl_buf, (ptrdiff_t)(BL_SLOTS + 2) * 16, NULL, 0x88E8 /* DYNAMIC_DRAW */);
        { void *z = calloc(BL_SLOTS, 16); glNamedBufferSubData(g_bl_buf, 0, (ptrdiff_t)BL_SLOTS * 16, z); free(z); }
        glNamedBufferSubData(g_bl_buf, (ptrdiff_t)BL_SLOTS * 16, 32, tail);
    }
    {   /* from 98,304 pairs (the old table's 3/4) on, again once 32,768 were added since or after 60 s: the walk below and the
         * whole-table upload of an eviction stay as cheap as before; the bigger table (MX_BL_BITS) only takes the pairs that
         * could not be dropped instead of refusing new ones (a full table: those sampled the dummy, objects flickered) */
        static int after; static uint32_t when;
        int from = g_bl_cap < 131072 ? g_bl_cap - g_bl_cap / 4 : 98304, step = g_bl_cap / 8 < 32768 ? g_bl_cap / 8 : 32768;      /* (TOMO_BL_CAP: smaller) */
        if (g_bl_n >= from && (g_bl_n >= after + step || g_bl_now >= when + 60)) { bl_evict(); after = g_bl_n; when = g_bl_now; }
    }
    if (miss_log < 0) miss_log = mx_bl_misslog();
    if (miss_log && GetTickCount64() >= miss_next) {             /* test only: a read-back stall every 10 s */
        uint32_t m[2] = {0, 0}, z[2] = {0, 0};
        glGetNamedBufferSubData(g_bl_buf, (ptrdiff_t)(BL_SLOTS + 1) * 16, 8, m);
        glNamedBufferSubData(g_bl_buf, (ptrdiff_t)(BL_SLOTS + 1) * 16, 8, z);
        if (miss_next) printf("nvn_gl: bindless misses in 10 s: %u (last handle %08x), %d handles in the table\n", m[0], m[1], g_bl_n);
        miss_next = GetTickCount64() + 10000;
    }
    AcquireSRWLockShared(&g_bl_lock);
    int n = g_bl_n;
    ReleaseSRWLockShared(&g_bl_lock);
    {   /* an entry changes only with a pool registration (g_pool_gen) or when it is new: the whole table was walked every batch */
        static LONG last_gen; static int last_n = -1;
        LONG gen_now = g_pool_gen;
        if (n == last_n && gen_now == last_gen) return;
        last_n = n; last_gen = gen_now;
    }
    for (int i = 0; i < n; i++) {                            /* only this thread moves entries (bl_evict); bl_note appends past n */
        uint32_t nh = g_bl[i].nh, dims[4] = {0, 0, 0, 0};
        uint32_t gen = 1 + g_tgen[nh & 0xfffff] + (g_sgen[(nh >> 20) & 0xfff] << 16);
        if (g_bl[i].gen == gen) continue;
        int isint = 0;
        uint64_t h = resolve_handle(S.tpool, S.spool, (uint64_t)nh | 0x100000000ull, dims, &isint);
        uint32_t entry[4] = {nh, (uint32_t)h, (uint32_t)(h >> 32), 1};
        glNamedBufferSubData(g_bl_buf, (ptrdiff_t)g_bl[i].slot * 16, 16, entry);
        g_bl[i].gl = h; g_bl[i].gen = gen;
    }
}
static void bl_bind(void) { if (g_bl_buf) gs_ssbo(39, g_bl_buf, 0, (BL_SLOTS + 2) * 16); }
static int snap_hot(uint64_t addr, uint64_t size) {      /* a page of it written by the game in the last 5 s */
    LONG fr = (LONG)g_st.frames;
    if (fr - g_hot_all < 300) return 1;
    for (uint64_t pg = addr >> 12, last = (addr + size - 1) >> 12; pg <= last; pg++) if (fr - g_hot_page[HOT_BUCKET(pg)] < 300) return 1;
    return 0;
}
static void snap_range(SnapSet *s, uint64_t addr, uint64_t size, uint64_t max) {
    if (!addr || !size || size > max || s->bad || !snap_hot(addr, size)) return;
    if ((s->n + 1) * 2 > s->cap) {
        unsigned oc = s->cap, nc = oc ? oc * 2 : 1024;
        SnapEnt *oe = s->e, *ne = calloc(nc, sizeof *ne);
        if (!ne) { s->bad = 1; return; }
        s->e = ne; s->cap = nc;
        for (unsigned i = 0; i < oc; i++) if (oe[i].addr) *snap_slot(s, oe[i].addr, oe[i].size) = oe[i];
        free(oe);
    }
    SnapEnt *e = snap_slot(s, addr, size);
    if (e->addr) return;
    if (s->len + size > s->dcap) {
        size_t nc = (s->len + (size_t)size) * 2 < 262144 ? 262144 : (s->len + (size_t)size) * 2;
        uint8_t *nd = realloc(s->data, nc);
        if (!nd) { s->bad = 1; return; }
        s->data = nd; s->dcap = nc;
    }
    __try { memcpy(s->data + s->len, P(addr), (size_t)size); } __except (1) { return; }      /* (a binding no draw uses may name memory that is gone) */
    e->addr = addr; e->size = size; e->off = s->len; e->state = 0; s->len += (size_t)size; s->n++;
}
/* The ranges a batch binds: uniform and vertex buffers. Not the arrays of deferred binds (the game may fill those after the
 * submission) nor storage buffers (the GPU writes them). */
static void snap_list(SnapSet *s, uint64_t handle, int depth) {
    size_t len = 0;
    uint8_t *data = list_lookup(handle, &len);
    for (size_t off = 0; data && off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        const uint8_t *blob = BLOB(m);
        uint64_t r[2];
        if (m->size < sizeof(Cmd)) break;
        if (m->op == OP_UBO) for (int i = 0; i < (int)m->a[2] && i < 16; i++) { memcpy(r, blob + i * 16, 16); snap_range(s, r[0], r[1], SNAP_MAX_UBO); }
        else if (m->op == OP_VBUF) for (int i = 0; i < (int)m->a[1] && i < 32; i++) { memcpy(r, blob + i * 16, 16); snap_range(s, r[0], r[1], SNAP_MAX_VBUF); }
        else if (m->op == OP_CALL && depth < 8) for (int i = 0; i < (int)m->a[0]; i++) { uint64_t h; memcpy(&h, blob + i * 8, 8); snap_list(s, h, depth + 1); }
        off += m->size;
    }
}
static struct { uint64_t addr, size, h; } g_sfc[8192];      /* TEST: TOMO_SNAP_FLUSHCHECK=1: copied ranges the game changes before its next flush, sync wait or present */
static int g_sfc_n, g_sfc_on = -1;
static LONG64 g_sfc_changed, g_sfc_checked, g_sfc_points;
void nvn_snap_flushcheck(const char *what) {             /* game thread */
    if (g_sfc_on <= 0 || !g_sfc_n) return;
    int changed = 0;
    for (int i = 0; i < g_sfc_n; i++) if (mem_hash(P(g_sfc[i].addr), (size_t)g_sfc[i].size, 1) != g_sfc[i].h) changed++;
    g_sfc_changed += changed; g_sfc_checked += g_sfc_n; g_sfc_points++;
    if (changed) { static int nlog; if (nlog++ < 40) printf("snapcheck: frame %lld: %d of %d ranges copied at the submissions changed before the %s%c", (long long)g_st.frames, changed, g_sfc_n, what, 10); }
    if (g_sfc_points % 2000 == 0) printf("snapcheck: %lld points, %lld ranges checked, %lld changed between the submission and the flush%c", (long long)g_sfc_points, (long long)g_sfc_checked, (long long)g_sfc_changed, 10);
    g_sfc_n = 0;
}
/* The copies are made after the batch is handed to the render thread (snap_begin before, snap_fill after): it starts the
 * batch (write-watch scan, ASTC textures it binds) meanwhile and waits for them before its first command. Made before the
 * hand-over, the render thread stood idle for them: 0.9-1.0 ms of every island frame (the frame's big batch, 860 ranges). */
static SnapSet *snap_begin(void) {                         /* game thread, at the submission, before the batch is posted */
    AcquireSRWLockExclusive(&g_snap_lock);
    SnapSet *s = g_snap_pool;
    if (s) g_snap_pool = s->next;
    ReleaseSRWLockExclusive(&g_snap_lock);
    if (!s && !(s = calloc(1, sizeof *s))) return NULL;
    s->ready = 0;
    s->emu = g_emu_serial;                                 /* (read before the copies: a write of ours racing them counts as later) */
    return s;
}
static void snap_fill(SnapSet *s, const uint64_t *handles, int count) {      /* game thread, after the batch is posted */
    LONG64 t0 = pf_now();
    for (int i = 0; i < count; i++) snap_list(s, handles[i], 0);
    if (g_sfc_on < 0) g_sfc_on = getenv("TOMO_SNAP_FLUSHCHECK") != NULL;
    if (g_sfc_on > 0) for (unsigned i = 0; i < s->cap && g_sfc_n < 8192; i++) if (s->e[i].addr) { g_sfc[g_sfc_n].addr = s->e[i].addr; g_sfc[g_sfc_n].size = s->e[i].size; g_sfc[g_sfc_n++].h = mem_hash(s->data + s->e[i].off, (size_t)s->e[i].size, 1); }
    InterlockedIncrement64(&g_snap_sets); InterlockedExchangeAdd64(&g_snap_ranges, s->n); InterlockedExchangeAdd64(&g_snap_bytes, (LONG64)s->len);
    InterlockedExchangeAdd64(&g_snap_ticks, pf_now() - t0);
    InterlockedExchange(&s->ready, 1);
}
static void snap_give(SnapSet *s) {                        /* render thread, after the replay */
    if (!s) return;
    if (s->cap) memset(s->e, 0, s->cap * sizeof *s->e);
    s->n = 0; s->len = 0; s->bad = 0;
    if (s->dcap > ((size_t)8 << 20)) { free(s->data); s->data = NULL; s->dcap = 0; }
    AcquireSRWLockExclusive(&g_snap_lock);
    s->next = g_snap_pool; g_snap_pool = s;
    ReleaseSRWLockExclusive(&g_snap_lock);
}
/* The ASTC textures a batch binds, synced before its first command: their GPU decodes then run back to back with one memory
 * barrier after them. Synced at their binds, each decode came between two draws, and the GPU let the draws before it finish
 * and the draw after it wait (the title's first frame: 292 small textures, 97 ms of GPU, a 150 ms frame, 2026-10-31). The
 * binds still sync (nothing is done twice in a batch); the pools as the batch sets them, deferred arrays as they hold now.
 * TOMO_TEX_PRESCAN=0: at the binds only, as before. */
/* A handle the batch's pre-scan resolved already (same pool) is not looked up again: the island's big list binds a few
 * hundred textures in some thousand binds, each a lookup of the pool entry and the texture records. */
static struct { uint64_t h; TexPool *tp; uint32_t ser; } g_ps_seen[4096];
static int prescan_seen(TexPool *tp, uint64_t h) {
    unsigned i = (unsigned)(((h ^ (uint64_t)(uintptr_t)tp) * 0x9E3779B97F4A7C15ULL) >> 52);
    for (int k = 0; k < 8; k++, i = (i + 1) & 4095) {
        if (g_ps_seen[i].ser != (uint32_t)g_serial) { g_ps_seen[i].ser = (uint32_t)g_serial; g_ps_seen[i].h = h; g_ps_seen[i].tp = tp; return 0; }
        if (g_ps_seen[i].h == h && g_ps_seen[i].tp == tp) return 1;
    }
    return 0;
}
static void prescan_list(uint64_t handle, int depth, TexPool **tp, int *n) {
    size_t len = 0;
    uint8_t *data = list_lookup(handle, &len);
    for (size_t off = 0; data && off + sizeof(Cmd) <= len && *n < 4096;) {
        const Cmd *m = (const Cmd *)(data + off);
        const uint8_t *blob = BLOB(m), *hs = NULL;
        int count = 0;
        if (m->size < sizeof(Cmd)) break;
        if (m->op == OP_TEXPOOL) *tp = P(m->a[0]);
        else if (m->op == OP_TEXTURES) { count = (int)m->a[2]; hs = blob; }
        else if (m->op == OP_DEFERRED && m->a[5] == OP_TEXTURES && m->a[3]) { count = (int)m->a[2]; hs = P(m->a[3]); }
        else if (m->op == OP_CALL && depth < 8) for (int i = 0; i < (int)m->a[0]; i++) { uint64_t h; memcpy(&h, blob + i * 8, 8); prescan_list(h, depth + 1, tp, n); }
        for (int i = 0; i < count && i < 32; i++) {
            uint64_t h;
            Tex *t; View v; SamplerParams sp;
            memcpy(&h, hs + i * 8, 8);
            if (prescan_seen(*tp, h)) continue;
            handle_lookup(*tp, NULL, h, &t, &v, &sp);
            if (t && !t->dead && t->b.format >= 0x79 && t->b.format <= 0x94 && t->checked != g_serial) { tex_sync_a(t, 1); (*n)++; }
        }
        off += m->size;
    }
}
static void job_submit(void *arg) {
    SubmitJob *j = arg;
    static int nlog;
    if (g_clip_dirty && InterlockedExchange(&g_clip_dirty, 0)) {
        glClipControl(g_origin_mode ? GL_UPPER_LEFT_ : GL_LOWER_LEFT_, g_depth_mode ? GL_ZERO_TO_ONE_ : GL_NEGATIVE_ONE_TO_ONE_);
        printf("nvn_gl: window origin mode %d, depth mode %d (set by the game)\n", g_origin_mode, g_depth_mode);
    }
    g_serial++;
    g_arena_gen++;                               /* guest memory may have changed since the last batch */
    { HS_IN("batch start: write watch scan (waited for), lists, decoded levels", 0);
    ww_scan_apply(j->ww_seq);                     /* which: the arena keeps what nobody wrote */
    bl_update();
    dec_land();
    HS_OUT(); }
    {   static int on = -1;
        if (on < 0) on = !(getenv("TOMO_TEX_PRESCAN") && getenv("TOMO_TEX_PRESCAN")[0] == '0');
        TexPool *tp = S.tpool;
        int n = 0;
        HS_IN("batch start: ASTC textures it binds (checked, queued)", 0);
        if (on && ga_on()) for (int i = 0; i < j->count; i++) prescan_list(j->handles[i], 0, &tp, &n);
        HS_OUT();
    }
    if (j->snap) {                               /* the copies of this batch's ranges (snap_fill on the game's thread) */
        SnapSet *sn = j->snap;
        HS_IN("batch start: submission copies (waited for)", 0);
        for (int k = 0; !InterlockedCompareExchange(&sn->ready, 0, 0); k++) { if (k < 2000) YieldProcessor(); else SwitchToThread(); }
        HS_OUT();
    }
    LONG64 pf0 = pf_now();
    g_snap_cur = j->snap;
    for (int i = 0; i < j->count; i++) replay(j->handles[i], 0);
    g_snap_cur = NULL; snap_give(j->snap); j->snap = NULL;
    { HS_IN("batch end: compute results (flush)", 0);
    compute_flush_async();                       /* the guest may read what compute wrote once its fence after this batch has passed */
    HS_OUT(); }
    pf_add(PF_SUBMIT, pf0);
    g_cur_op = "";
    GLenum e;
    { HS_IN("batch end: GL error check (glGetError)", 0); e = GL_ERR_SOFT(); HS_OUT(); }
    if (e) { g_st.gl_errors++; logf_limited(&nlog, 30, "nvn_gl: GL error 0x%x in submitted batch %lld\n", e, (long long)g_st.submits); while (glGetError()) {} }
    g_st.submits++;
}

static void job_submit_async(void *arg) { SubmitJob *j = arg; j->handles = (const uint64_t *)(j + 1); job_submit(j); }

static void save_bmp(const char *path, Tex *t) {
    ga_barrier();
    int w = tw(t), h = th(t);
    uint8_t *px = malloc((size_t)w * h * 3), head[54] = {'B', 'M'};
    uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
    static const uint8_t pad[4];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (t->b.depth == -1) {                         /* the alpha channel as grey (SSBU_RTDUMP_ALPHA) */
        glReadPixels(0, 0, w, h, 0x1906 /* GL_ALPHA */, GL_UNSIGNED_BYTE, px);
        for (int i = w * h - 1; i >= 0; i--) px[i * 3] = px[i * 3 + 1] = px[i * 3 + 2] = px[i];
    } else glReadPixels(0, 0, w, h, GL_BGR_, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return; }
    memcpy(head + 2, &size, 4);
    v = 54; memcpy(head + 10, &v, 4);
    v = 40; memcpy(head + 14, &v, 4);
    memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4);
    head[26] = 1; head[28] = 24;
    fwrite(head, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) { fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f); fwrite(pad, 1, row - (uint32_t)w * 3, f); }   /* GL row 0 is the top row */
    fclose(f);
    free(px);
    printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);
    fflush(stdout);
}

/* Frame times (present to present) of the last 300 frames and CPU time per thread, printed with the frame-rate line. */
#include <tlhelp32.h>
#ifndef _WIN32
#include <dirent.h>
#endif
static double g_pf_ft[300];
static struct { DWORD tid; ULONGLONG cpu; } g_pf_thr[512];
static int g_pf_nthr;
static DWORD g_pf_main_tid;
const char *guest_thread_name_of(DWORD tid);                             /* os_threads.c */
static int pf_cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
#ifdef _WIN32
static void pf_threads(double frames) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    struct { DWORD tid; double ms; } top[512];
    int n = 0;
    double total = 0;
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(0x0800 /* THREAD_QUERY_LIMITED_INFORMATION */, FALSE, te.th32ThreadID);
        FILETIME c, e, k, u;
        if (!h) continue;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            ULONGLONG cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime), prev = 0;
            int i;
            for (i = 0; i < g_pf_nthr && g_pf_thr[i].tid != te.th32ThreadID; i++) {}
            if (i < g_pf_nthr) prev = g_pf_thr[i].cpu; else if (g_pf_nthr < 512) { g_pf_thr[g_pf_nthr].tid = te.th32ThreadID; i = g_pf_nthr++; }
            if (i < 512) g_pf_thr[i].cpu = cpu;
            if (n < 512) { top[n].tid = te.th32ThreadID; top[n].ms = (double)(cpu - prev) / 10000.0 / frames; total += top[n].ms; n++; }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    printf("nvn_perf: cpu ms/frame by thread (all %.1f):", total);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) if (top[i].ms > 0.05 && (best < 0 || top[i].ms > top[best].ms)) best = i;
        if (best < 0) break;
        const char *name = top[best].tid == g_render_tid ? "render(host)" : top[best].tid == g_pf_main_tid ? "main" : guest_thread_name_of(top[best].tid);
        printf(" %s[%lu] %.1f", name ? name : "host", (unsigned long)top[best].tid, top[best].ms);
        top[best].ms = 0;
    }
    printf("\n");
}
#else
static void pf_threads(double frames) {                 /* Linux build: /proc/self/task/<tid>/stat, utime + stime */
    struct { DWORD tid; double ms; } top[512];
    int n = 0;
    double total = 0;
    long hz = sysconf(_SC_CLK_TCK);
    DIR *d = opendir("/proc/self/task");
    struct dirent *de;
    if (!d) return;
    while ((de = readdir(d))) {
        DWORD tid = (DWORD)strtoul(de->d_name, NULL, 10);
        char sp[96], buf[512];
        if (!tid) continue;
        snprintf(sp, sizeof sp, "/proc/self/task/%lu/stat", (unsigned long)tid);
        FILE *f = fopen(sp, "r");
        if (!f) continue;
        size_t k = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[k] = 0;
        const char *rp = strrchr(buf, ')');
        unsigned long long ut = 0, st = 0;
        if (!rp || sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        ULONGLONG cpu = (ut + st) * 10000000ull / (ULONGLONG)(hz > 0 ? hz : 100), prev = 0;
        int i;
        for (i = 0; i < g_pf_nthr && g_pf_thr[i].tid != tid; i++) {}
        if (i < g_pf_nthr) prev = g_pf_thr[i].cpu; else if (g_pf_nthr < 512) { g_pf_thr[g_pf_nthr].tid = tid; i = g_pf_nthr++; }
        if (i < 512) g_pf_thr[i].cpu = cpu;
        if (n < 512) { top[n].tid = tid; top[n].ms = (double)(cpu - prev) / 10000.0 / frames; total += top[n].ms; n++; }
    }
    closedir(d);
    printf("nvn_perf: cpu ms/frame by thread (all %.1f):", total);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) if (top[i].ms > 0 && (best < 0 || top[i].ms > top[best].ms)) best = i;
        if (best < 0 || top[best].ms < 0.05) break;
        const char *g = guest_thread_name_of(top[best].tid);
        printf(" %s %.1f", g ? g : top[best].tid == g_render_tid ? "render" : "host", top[best].ms);
        top[best].ms = 0;
    }
    printf("\n");
}
#endif
static DWORD WINAPI pf_threads_thread(LPVOID arg) { for (;;) { WaitForSingleObject((HANDLE)arg, INFINITE); pf_threads(300.0); } }
static const char *const k_pp_name[10] = {"flush", "attr-keys", "stage_glsl", "program-bind", "ssbo+cbuf", "vertex-format", "vertex-buffers", "stencil+rest", "", ""};
static void pf_frame_mark(LONG64 frame) {
    static LONG64 last, freq, snap[PF_N], snapn[PF_N], b0[3];
    LONG64 now = pf_now();
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    if (last) g_pf_ft[frame % 300] = (double)(now - last) * 1000.0 / (double)freq;
    if (last && g_hitch_ms) hitch_frame((long long)frame, (double)(now - last) * 1000.0 / (double)freq, g_frame_dispatch > 0);
    last = now;
    if (frame % 300) return;
    double ft[300], sum = 0;
    memcpy(ft, g_pf_ft, sizeof ft);
    qsort(ft, 300, sizeof ft[0], pf_cmp_d);
    for (int i = 0; i < 300; i++) sum += ft[i];
    printf("nvn_perf: frames %lld..%lld frame time ms: avg %.2f min %.2f median %.2f p99 %.2f max %.2f | fps avg %.1f, 1%% low %.1f\n", (long long)frame - 300, (long long)frame,
           sum / 300, ft[0], ft[150], ft[296], ft[299], sum > 0 ? 300000.0 / sum : 0.0, ft[296] > 0 ? 1000.0 / ft[296] : 0.0);
    if (g_listlog_dbg) {
        printf("nvn_perf: compute read-backs by line:");
        for (int k = 0; k < 32 && g_cfl_site[k].line; k++) { printf(" %d:%lld", g_cfl_site[k].line, (long long)g_cfl_site[k].n); g_cfl_site[k].n = 0; }
        printf("\n");
    }
    if (g_lp_on > 0) {
        static LONG64 p[5];
        LONG64 v[5] = {g_lp_inv_n, g_lp_inv_t, g_lp_inv_wait, g_lp_reg_n, g_lp_reg_wait};
        printf("nvn_perf: list table: %d lists; per frame: %.1f invalidations taking %.2f ms (+ %.2f ms waiting for the lock), %.1f registrations waiting %.2f ms%c", g_list_count,
               (double)(v[0] - p[0]) / 300.0, (double)(v[1] - p[1]) * 1000.0 / (double)freq / 300.0, (double)(v[2] - p[2]) * 1000.0 / (double)freq / 300.0,
               (double)(v[3] - p[3]) / 300.0, (double)(v[4] - p[4]) * 1000.0 / (double)freq / 300.0, 10);
        memcpy(p, v, sizeof p);
    }
    if (g_pp_on > 0) {
        static LONG64 ps[10];
        printf("nvn_perf: draw_prepare parts ms/frame:");
        for (int k = 0; k < 8; k++) { printf(" %s %.2f", k_pp_name[k], (double)(g_pp[k] - ps[k]) * 1000.0 / (double)freq / 300.0); ps[k] = g_pp[k]; }
        printf("\n");
    }
    printf("nvn_perf: ms/frame (calls/frame):");
    for (int k = 0; k < PF_N; k++) {
        LONG64 t = g_pf[k], c = g_pfn[k];
        printf(" %s %.2f (%.1f)", k_pf_name[k], (double)(t - snap[k]) * 1000.0 / (double)freq / 300.0, (double)(c - snapn[k]) / 300.0);
        if (k == PF_N - 1) {                                   /* the command types that cost most (ms per frame, calls per frame) */
            static LONG64 osnap[OP_COUNT], osnapn[OP_COUNT];
            printf("\nnvn_perf: commands:");
            for (int top = 0; top < 8; top++) {
                int b = -1;
                for (int o = 0; o < OP_COUNT; o++) if (g_optime[o] - osnap[o] > 0 && (b < 0 || g_optime[o] - osnap[o] > g_optime[b] - osnap[b])) b = o;
                if (b < 0) break;
                printf(" %s %.2f (%.1f)", k_op_name[b], (double)(g_optime[b] - osnap[b]) * 1000.0 / (double)freq / 300.0, (double)(g_opn[b] - osnapn[b]) / 300.0);
                osnap[b] = g_optime[b]; osnapn[b] = g_opn[b];
            }
            for (int o = 0; o < OP_COUNT; o++) { osnap[o] = g_optime[o]; osnapn[o] = g_opn[o]; }
            static LONG64 wsnap[16], wsnapn[16];
            printf("\nnvn_perf: waits for other jobs:");
            for (int i = 0; i < 16 && g_wj[i].fn; i++) {
                void (*f)(void *) = g_wj[i].fn;
                const char *nm = f == job_sync_wait ? "SyncWait" : f == job_server_wait ? "QueueWaitSync" : f == job_finish ? "Finish" : f == job_flush ? "Flush" : f == job_fence ? "FenceSync" :
                                 f == job_housekeeping ? "ApplyDeferredFinalizes" : f == job_nop ? "barrier" : f == job_write_texels ? "WriteTexels" : f == job_read_texels ? "ReadTexels" : "other";
                if (g_wj[i].n - wsnapn[i]) printf(" %s %.2f (%.1f)", nm, (double)(g_wj[i].ticks - wsnap[i]) * 1000.0 / (double)freq / 300.0, (double)(g_wj[i].n - wsnapn[i]) / 300.0);
                wsnap[i] = g_wj[i].ticks; wsnapn[i] = g_wj[i].n;
            }
            static LONG64 bsnap[16], bsnapn[16];
            printf("\nnvn_perf: alive: %lld GL textures, %lld resident handles, %d dead objects pending; spare render targets %d (%lld reused, %lld kept, %lld old ones freed)", (long long)g_tex_live, (long long)g_handles_live, g_dead_n,
                   g_spare_n, (long long)g_spare_hits, (long long)g_spare_kept, (long long)g_spare_full);
            printf(" (%lld at the same memory, %lld cleared)", (long long)g_spare_same_mem, (long long)g_spare_cleared);
            { int mx = 0; for (int i = 0; i < g_spare_n; i++) { int nh = g_spare[i].o.nh; for (VNode *n = g_spare[i].views; n; n = n->next) nh += n->o.nh; if (nh > mx) mx = nh; }
              printf("; %d samplers, %lld framebuffers made, most handles on one spare %d", g_sampler_count, (long long)g_st.fbo_made, mx); }
            printf("\nnvn_perf: barriers by caller:");
            for (int i = 0; i < 16 && g_bar[i].who; i++) {
                if (g_bar[i].n - bsnapn[i]) printf(" %s %.2f (%.1f)", g_bar[i].who, (double)(g_bar[i].ticks - bsnap[i]) * 1000.0 / (double)freq / 300.0, (double)(g_bar[i].n - bsnapn[i]) / 300.0);
                bsnap[i] = g_bar[i].ticks; bsnapn[i] = g_bar[i].n;
            }
        }
        snap[k] = t; snapn[k] = c;
    }
    {   static LONG64 r0[3];
        printf(" | MB/frame: streamed %.2f (ring %.2f in %.0f copies, %lld part waits), read back %.2f, hashed %.2f\n", (double)(g_pf_arena_bytes - b0[0]) / 300.0 / 1048576.0,
               (double)(g_ring_bytes - r0[0]) / 300.0 / 1048576.0, (double)(g_ring_calls - r0[1]) / 300.0, (long long)(g_ring_waits - r0[2]), (double)(g_pf_cflush_bytes - b0[1]) / 300.0 / 1048576.0,
               (double)(g_pf_hash_bytes - b0[2]) / 300.0 / 1048576.0);
        r0[0] = g_ring_bytes; r0[1] = g_ring_calls; r0[2] = g_ring_waits; }
    b0[0] = g_pf_arena_bytes; b0[1] = g_pf_cflush_bytes; b0[2] = g_pf_hash_bytes;
    {
        static int on = -1;
        static LONG64 z[17];
        if (on < 0) on = getenv("SSBU_GLSTATS") != NULL;
        if (on) {
            LONG64 v[17] = {g_gs_draws, g_gs_prog_same, g_gs_attr_set, g_gs_attr_same, g_gs_vbuf_bind, g_gs_buf_bind, g_st.tex_created, g_st.tex_uploads, g_st.fbo_made, g_st.views,
                            g_st.handles, g_shc_compiled, g_shd_loaded, g_shc_hit, g_pfn[PF_CFLUSH], g_pfn[PF_WAIT_SUBMIT] + g_pfn[PF_WAIT_PRESENT] + g_pfn[PF_WAIT_OTHER], g_gs_gl_binds};
            double d[17];
            for (int k = 0; k < 17; k++) { d[k] = (double)(v[k] - z[k]) / 300.0; z[k] = v[k]; }
            printf("nvn_glstats: per frame: guest waits for the render thread %.1f | compute read-backs (glGetNamedBufferSubData) %.1f | created: textures %.3f, uploads %.3f, framebuffers %.3f, views %.3f, bindless handles %.3f | "
                   "shader stages: built %.3f (from disk %.3f), reused %.3f | draws %.1f: same two programs as the previous draw %.1f; vertex attribute formats set %.1f, unchanged since last set %.1f; vertex buffer binds %.1f; uniform/constant buffer binds %.1f (glBindBufferRange calls %.1f)\n",
                   d[15], d[14], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[0], d[1], d[2], d[3], d[4], d[5], d[16]);
        }
    }
    {   /* P3 round 2: the snapshot of all threads (CreateToolhelp32Snapshot) took 30-70 ms on the render thread: one long
         * frame in every 300 (hitch log, "present" scope). It is taken by a helper thread now. */
        static HANDLE ev;
        if (!ev) { ev = CreateEventA(NULL, FALSE, FALSE, NULL); CloseHandle(CreateThread(NULL, 0, pf_threads_thread, ev, 0, NULL)); }
        SetEvent(ev);
    }
}

static void job_present(void *arg);
static void present_fence(long long frame);
static void job_fence(void *arg);
static void job_sync_wait(void *arg);
static void job_flush(void *arg);
static void job_finish(void *arg);
static void job_housekeeping(void *arg); static void job_write_texels(void *arg); static void job_read_texels(void *arg); static void job_server_wait(void *arg); static void job_fence(void *arg);
static void joblog(void (*fn)(void *), LONG64 t) {
    static LONG64 freq;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    printf("joblog: frame %lld t=%.3f ms thread %lu %s%c", (long long)g_joblog_cur, (double)(t % (freq * 100)) * 1000.0 / (double)freq, (unsigned long)GetCurrentThreadId(),
           fn == job_submit ? "SubmitCommands" : fn == job_present ? "PresentTexture" : fn == job_fence ? "FenceSync" : fn == job_sync_wait ? "SyncWait" : fn == job_flush ? "Flush" :
           fn == job_finish ? "Finish" : fn == job_submit_async ? "SubmitCommands (posted)" : fn == job_read_texels ? "ReadTexels" : fn == job_write_texels ? "WriteTexels" :
           fn == job_nop ? "barrier" : fn == job_housekeeping ? "housekeeping" : fn == job_server_wait ? "QueueWaitSync" : fn == job_rb_land_range ? "rb_land_range" : "other", 10);
}
#ifdef _WIN32
#include "nvn_gl_dxgi.c"                           /* the picture to the screen through a DXGI swap chain */
#include "nvn_gl_menu.c"                           /* the in-game settings menu (menu.cpp builds it) */
/* PRESENT AT THE TICK (2026-10-30, as the Vulkan renderer does since session 14, display.c): the finished frame's picture is
 * blitted into the back buffer at once and the flip (SwapBuffers) waits for the tick of the refresh clock, posted by the
 * vsync thread (nvn_front_post_present_flush -> job_gl_swap_flush). Before, the flip came when the render thread was done,
 * so a frame landed on whichever vblank followed, and on a 240 Hz monitor (4 refreshes a game frame) frames stayed on
 * screen 3, 4 or 5 refreshes as the render time varied. A late frame (queued after its tick) is flipped at once, as before.
 * SSBU_PACE_HOLD=0 or latency = lowest: flipped at once always. */
static volatile LONG g_gl_swap_pending;
static int g_swap_si = 1;                         /* the swap interval (vsync = off: 0), for the DXGI swap chain too */
static void gl_swap_now(void) {
    LONG64 pf1 = pf_now();
    HS_IN("SwapBuffers", 0); if (!dx_present(g_swap_si)) SwapBuffers(g_dc); HS_OUT(); pf_add(PF_SWAP, pf1);
    if (InterlockedExchange(&g_gl_swap_pending, 0)) { InterlockedExchange(&g_display_holding, 0); display_present_flushed(); }
    display_present_done(0);
}
static void job_gl_swap_flush(void *arg) { (void)arg; if (g_gl_swap_pending && display_present_due()) gl_swap_now(); }      /* (not a newer frame kept for a later tick) */
static int swap_midbatch_ok(void) { return g_dx.state != 1 || g_dx.checks >= 3; }      /* (dx_check's read-backs move the read framebuffer: not between commands) */
#endif
/* F12 (display.c): a flicker sample. The next 8 presented pictures go to flicker/f<frame>.bmp beside the exe, then the draw
 * log of the two frames after them (every draw: targets, programs, textures; the frozen-scene restores) to the session log,
 * so a flicker seen on a player's PC can be looked at frame by frame. TOMO_FLICKER_SAMPLE_AT=<frame>: the same in a test. */
static volatile LONG g_flk_req;
static volatile LONG g_rtdump_step;               /* TEST: SSBU_RTDUMP_STEP=<step>: the render targets dumped at that input step's frame */
static volatile LONG64 g_drawshot_at = -1;        /* (declared above) TEST: SSBU_DRAWSHOT_STEP=<step>: the draws of the frame after that step's, as SSBU_DRAWSHOT */
static int step_listed(const char *name, int step) {      /* TEST: <name>=<step>[,<step>...] */
    const char *e = getenv(name);
    char k[16], l[256];
    if (!e) return 0;
    snprintf(l, sizeof l, ",%s,", e); snprintf(k, sizeof k, ",%d,", step);
    return strstr(l, k) != NULL;
}
void nvn_front_flicker_sample(void) { InterlockedExchange(&g_flk_req, 1); }
static void job_present(void *arg) {
    if (g_rbp_n) rb_land(0);
    g_dec_spent = 0;
    pair_off();
    pw_gate();
    PresentJob *j = arg;
    static int shot = -2, every, shot_from, shot_to;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    RECT r;
    LONG64 frame = g_st.frames + 1;
#ifdef _WIN32
    if (g_gl_swap_pending && display_pace_lead()) { display_wait_present_due(50); gl_swap_now(); }      /* TOMO_PACE_LEAD: the kept frame goes at its tick first */
    if (InterlockedExchange(&g_gl_swap_pending, 0)) { InterlockedExchange(&g_display_holding, 0); display_present_flushed(); }      /* a frame kept for its tick is replaced by this newer one */
#endif
    LONG64 pf0 = pf_now();
    drawsig_frame(frame);
    if (frame == 1) { void tomo_timeline(const char *what); tomo_timeline("the game's first picture"); }
    {   static long long lo = -2, hi; static LONG64 f0, v0, b0, t0;      /* TEST: TOMO_RTSTAT=<from>-<to>: what each frame made for its render targets */
        if (lo == -2) { lo = -1; const char *e = getenv("TOMO_RTSTAT"); if (e) sscanf(e, "%lld-%lld", &lo, &hi); }
        if (lo >= 0 && frame >= lo && frame <= hi && (g_st.fbo_made != f0 || g_st.views != v0 || g_bc_made != b0 || g_st.tex_created != t0))
            { LARGE_INTEGER q; QueryPerformanceFrequency(&q);
              printf("rtstat: frame %lld: %lld framebuffers (%lld evicted; %.1f ms making, %.1f of it detaching), %lld views, %lld compressed-target scratches, %lld textures made%c", frame, (long long)(g_st.fbo_made - f0), (long long)g_fbo_evicted,
                     (double)g_fbo_t_make * 1000.0 / (double)q.QuadPart, (double)g_fbo_t_detach * 1000.0 / (double)q.QuadPart, (long long)(g_st.views - v0), (long long)(g_bc_made - b0), (long long)(g_st.tex_created - t0), 10); }
        f0 = g_st.fbo_made; v0 = g_st.views; b0 = g_bc_made; t0 = g_st.tex_created; g_fbo_evicted = 0; g_fbo_t_make = g_fbo_t_detach = 0;
    }
    if (shot == -2) { shot = getenv("SSBU_SHOT") ? atoi(getenv("SSBU_SHOT")) : -1; every = getenv("SSBU_SHOT_EVERY") ? atoi(getenv("SSBU_SHOT_EVERY")) : 0; shot_from = getenv("SSBU_SHOT_FROM") ? atoi(getenv("SSBU_SHOT_FROM")) : 0; shot_to = getenv("SSBU_SHOT_TO") ? atoi(getenv("SSBU_SHOT_TO")) : 0x7fffffff; }
    {   /* debug: TOMO_DBGBUF=<frame>: 64 KB of floats at storage binding 38 for override shaders (TOMO_SHOVR) to write; rows of 80 printed at that frame */
        static int dbf = -2;
        static GLuint dbb;
        if (dbf == -2) dbf = getenv("TOMO_DBGBUF") ? atoi(getenv("TOMO_DBGBUF")) : -1;
        if (dbf >= 0) {
            if (!dbb) { void *z = calloc(1, 65536); glCreateBuffers(1, &dbb); glNamedBufferData(dbb, 65536, z, 0x88E8); free(z); }
            if (frame == dbf) {
                float *v = malloc(65536);
                glMemoryBarrier(0xffffffffu);
                glGetNamedBufferSubData(dbb, 0, 65536, v);
                for (int row = 0; row < 65536 / 4 / 80; row++) {
                    int any = 0;
                    for (int i = 0; i < 80; i++) any |= v[row * 80 + i] != 0.0f;
                    if (!any) continue;
                    printf("dbgbuf %d:", row);
                    for (int i = 0; i < 80; i++) printf(" %g", v[row * 80 + i]);
                    printf("\n");
                }
                fflush(stdout);
                free(v);
            }
            gs_ssbo(38, dbb, 0, 65536);
        }
    }
    GetClientRect(g_hwnd, &r);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB_);
    int wflip = 1;                               /* the window's rows are bottom first; the DXGI picture's top first */
#ifdef _WIN32
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, dx_target((int)r.right, (int)r.bottom, &wflip));
#else
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
#endif
    int ff_skip = 0;                             /* fast-forward (display.c): the monitor shows every ff-th frame */
    {   int display_ff(void); static unsigned ffk; int ff = display_ff(); ff_skip = ff > 1 && (++ffk % (unsigned)ff) != 0; }
    if (t && getenv("TOMO_ALIASLOG")) {              /* diagnosis: textures laid over the presented window texture's memory */
        static int said;
        if (said++ < 3) {
            printf("aliaslog: window texture gl %u mem %p %dx%dx%d format 0x%x target %d flags 0x%x levels %d samples %d\n", t->o.gl, (void *)t->mem,
                   t->b.width, t->b.height, t->b.depth, t->b.format, t->b.target, (unsigned)t->b.flags, t->b.levels, t->b.samples);
            for (Tex *o = g_all_tex; o; o = o->all_next)
                if (o != t && !o->dead && o->mem && o->mem < t->mem + 1920 * 1080 * 4 && o->mem + 16 > t->mem)
                    printf("aliaslog:   gl %u mem %p (%+lld) %dx%d format 0x%x flags 0x%x\n", o->o.gl, (void *)o->mem, (long long)(o->mem - t->mem), o->b.width, o->b.height, o->b.format, (unsigned)o->b.flags);
        }
    }
    if (ff_skip) { present_fence(frame); }
    else if (t && !t->dead && t->o.gl) {
        int sr[4] = {0, 0, t->b.width, t->b.height};
        ga_barrier();
        if (j->win->crop[2] > 0 && j->win->crop[3] > 0) memcpy(sr, j->win->crop, sizeof sr);
        if (t->rw) { int x0 = rsx(t, sr[0]), y0 = rsy(t, sr[1]); sr[2] = rsx(t, sr[0] + sr[2]) - x0; sr[3] = rsy(t, sr[1] + sr[3]) - y0; sr[0] = x0; sr[1] = y0; }      /* resolution scale */
        glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
        glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, 0);
        void display_fit(int sw, int sh, int dw, int dh, int out[4]);      /* PR #24, display.c: the picture keeps its 16:9 shape */
        int d4[4];
        display_fit(sr[2], sr[3], (int)r.right, (int)r.bottom, d4);      /* 16:9, or wider with widescreen */
        if (d4[2] != r.right || d4[3] != r.bottom) { glColorMaski(0, 1, 1, 1, 1); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); }      /* the game's next draw restores its own mask */
        if (wflip) glBlitFramebuffer(sr[0], sr[1], sr[0] + sr[2], sr[1] + sr[3], d4[0], r.bottom - d4[1], d4[0] + d4[2], r.bottom - d4[1] - d4[3], GL_COLOR_BUFFER_BIT, GL_LINEAR);
        else glBlitFramebuffer(sr[0], sr[1], sr[0] + sr[2], sr[1] + sr[3], d4[0], d4[1], d4[0] + d4[2], d4[1] + d4[3], GL_COLOR_BUFFER_BIT, GL_LINEAR);
        if (g_drawlog_frame >= 0 && frame == g_drawlog_frame + 1) save_bmp("frame_drawlog.bmp", t);      /* the frame the draw log describes */
        { int step = nvn_front_take_step_shot(); if (step) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sstep_%03d.bmp", dir ? dir : "", dir ? "/" : "", step - 1); save_bmp(path, t);
            if (getenv("SSBU_DRAWSHOT_SEAM")) printf("  SEAM: step %d shot, frame %lld, gl %u: %d rows\n", step - 1, (long long)frame, t->o.gl, seam_rows(g_fbo_read, tw(t), th(t)));
            if (step_listed("SSBU_RTDUMP_STEP", step - 1)) InterlockedExchange(&g_rtdump_step, 1);
            if (step_listed("SSBU_DRAWSHOT_STEP", step - 1)) InterlockedExchange64(&g_drawshot_at, frame);
            if (step_listed("SSBU_DRAWLOG_STEP", step - 1)) { g_dl_force[0] = frame; g_dl_force[1] = frame; } } }      /* (TEST: and its draw log) */      /* (TEST: the targets dumped at that step's frame too) */
        nvn_front_draw_tag_clear();
        if (j->shot_fight) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sfight_%05d.bmp", dir ? dir : "", dir ? "/" : "", j->shot_fight); save_bmp(path, t); printf("shot: fight frame %d saved at presented frame %lld (OpenGL)\n", j->shot_fight, (long long)frame); }
        {   static int left; static long long at = -2;
            if (at == -2) at = getenv("TOMO_FLICKER_SAMPLE_AT") ? atoll(getenv("TOMO_FLICKER_SAMPLE_AT")) : -1;      /* TEST: as if F12 were pressed there */
            if (frame == at) InterlockedExchange(&g_flk_req, 1);
            if (InterlockedExchange(&g_flk_req, 0) && !left) {
                left = 8;
                CreateDirectoryA("flicker", NULL);
                printf("flicker sample: pictures of frames %lld..%lld in the folder flicker, then the draw log of frames %lld and %lld below (the game pauses a few seconds)%c", frame, frame + 7, frame + 9, frame + 10, 10);
            }
            if (left > 0) {
                char path[64]; snprintf(path, sizeof path, "flicker/f%lld.bmp", frame); save_bmp(path, t);
                if (!--left) { g_dl_force[0] = frame + 1; g_dl_force[1] = frame + 2; }      /* (drawlog_now: the draws of the frames after these) */
            }
        }
        if (frame == shot || (every > 0 && frame >= shot_from && frame <= shot_to && frame % every == 0)) {
            char path[300];
            const char *dir = getenv("SSBU_SHOT_DIR");                /* default: the current directory */
            snprintf(path, sizeof path, "%s%sframe_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)frame);
            save_bmp(path, t);
        }
        {   /* TOMO_LUMLOG=1 (TEST): the presented picture's mean luminance per frame against the real time ("lum: <frame> <ms>
             * <mean>"), read back 3 frames late through a mapped buffer, so the frame rate is not disturbed: compares the game's
             * speed between frame rates (scratch speed.py: the title camera's flight over the island) */
            static int on = -1; static GLuint fb2[2], tex2, pb; static uint8_t *pm; static void *fs[4]; static LONG64 ff[4], ft[4], qf; static int k;
            if (on < 0) { on = getenv("TOMO_LUMLOG") && gl_has(glNamedBufferStorage, "glNamedBufferStorage") && gl_has(glMapNamedBufferRange, "glMapNamedBufferRange"); LARGE_INTEGER f; QueryPerformanceFrequency(&f); qf = f.QuadPart; }
            if (on && t && t->o.gl && t->b.width > 0 && t->b.height > 0) {
                enum { LW = 160, LH = 90, LB = LW * LH * 4 };
                if (!pb) {
                    glGenFramebuffers(2, fb2); glGenTextures(1, &tex2);
                    glBindTexture(GL_TEXTURE_2D, tex2); glTexStorage2D(GL_TEXTURE_2D, 1, 0x8058 /* RGBA8 */, LW, LH);
                    glCreateBuffers(1, &pb); glNamedBufferStorage(pb, 4 * LB, NULL, 0x1 | 0x40 | 0x80 /* READ | PERSISTENT | COHERENT */);
                    pm = glMapNamedBufferRange(pb, 0, 4 * LB, 0x1 | 0x40 | 0x80);
                }
                glBindFramebuffer(GL_READ_FRAMEBUFFER_, fb2[0]); glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, 0);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, fb2[1]); glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, tex2, 0);
                glDisable(GL_SCISSOR_TEST);
                glBlitFramebuffer(0, 0, tw(t), th(t), 0, 0, LW, LH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                glBindFramebuffer(GL_READ_FRAMEBUFFER_, fb2[1]);
                glBindBuffer(0x88EB /* PIXEL_PACK_BUFFER */, pb); glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, LW, LH, 0x1908 /* RGBA */, GL_UNSIGNED_BYTE, (void *)(uintptr_t)(k * LB));
                glBindBuffer(0x88EB, 0);
                glEnable(GL_SCISSOR_TEST);
                glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
                if (fs[k]) glDeleteSync(fs[k]);
                fs[k] = glFenceSync(0x9117 /* SYNC_GPU_COMMANDS_COMPLETE */, 0); ff[k] = frame; ft[k] = pf_now();
                k = (k + 1) & 3;                           /* the oldest of the four: 3 frames ago */
                if (pm && fs[k] && glClientWaitSync(fs[k], 1, 50000000) != 0x911D /* WAIT_FAILED */) {
                    const uint8_t *q = pm + k * LB; unsigned s = 0;
                    for (int i = 0; i < LW * LH; i++) s += (q[i * 4] * 2u + q[i * 4 + 1] * 5u + q[i * 4 + 2]) / 8u;
                    printf("lum: %lld %.1f %.2f%c", (long long)ff[k], (double)ft[k] * 1000.0 / (double)qf, s / (double)(LW * LH), 10);
                }
            }
        }
        {   /* TOMO_FLICKER=1 (diagnosis): one-frame spikes in the presented picture. 48x27 cells of mean luminance; a cell of a frame
             * that differs by more than 12 from both the frame before and the one after, while those two agree, flickered. */
            static int on = -1; static GLuint fb[2], smalltex; static uint8_t hist[3][48 * 27], *px, *ring[3]; static int nh; static LONG64 seen, flick;
            if (on < 0) on = getenv("TOMO_FLICKER") != NULL;
            if (on && t && t->o.gl && t->b.width > 0 && t->b.height > 0) {
                if (!fb[0]) {
                    glGenFramebuffers(2, fb); glGenTextures(1, &smalltex);
                    glBindTexture(GL_TEXTURE_2D, smalltex); glTexStorage2D(GL_TEXTURE_2D, 1, 0x8058 /* RGBA8 */, 480, 270);
                    px = malloc(480 * 270 * 4);
                    for (int k = 0; k < 3; k++) ring[k] = calloc(480 * 270, 4);
                }
                glBindFramebuffer(GL_READ_FRAMEBUFFER_, fb[0]); glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, 0);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, fb[1]); glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, smalltex, 0);
                glDisable(GL_SCISSOR_TEST);
                glBlitFramebuffer(0, 0, tw(t), th(t), 0, 0, 480, 270, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                glBindFramebuffer(GL_READ_FRAMEBUFFER_, fb[1]);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, 480, 270, 0x1908 /* RGBA */, GL_UNSIGNED_BYTE, px);
                glEnable(GL_SCISSOR_TEST);
                glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
                memmove(hist[0], hist[1], sizeof hist[0] * 2);
                { uint8_t *o = ring[0]; ring[0] = ring[1]; ring[1] = ring[2]; ring[2] = o; memcpy(ring[2], px, 480 * 270 * 4); }
                {   static int rl = -1; if (rl < 0) rl = getenv("TOMO_REGIONLOG") != NULL;      /* TEST: TOMO_REGIONLOG=1 (with TOMO_FLICKER): every frame's mean colour
                                                                                                   * (x10) of three bands of the picture (rows of the 480x270 copy, y up) */
                    if (rl) { static const int band[3][2] = {{240, 270}, {120, 150}, {0, 30}}; unsigned m[3][3] = {{0}};
                        for (int bi = 0; bi < 3; bi++) for (int y = band[bi][0]; y < band[bi][1]; y++) for (int x = 0; x < 480; x++) { const uint8_t *q = px + ((size_t)y * 480 + x) * 4; m[bi][0] += q[0]; m[bi][1] += q[1]; m[bi][2] += q[2]; }
                        printf("region: frame %lld top %u %u %u mid %u %u %u bottom %u %u %u%c", (long long)frame - 1, m[0][0] / 1440, m[0][1] / 1440, m[0][2] / 1440, m[1][0] / 1440, m[1][1] / 1440, m[1][2] / 1440, m[2][0] / 1440, m[2][1] / 1440, m[2][2] / 1440, 10); } }
                for (int cy = 0; cy < 27; cy++)
                    for (int cx = 0; cx < 48; cx++) {
                        unsigned sum = 0;
                        for (int y = 0; y < 10; y++) for (int x = 0; x < 10; x++) { const uint8_t *q = px + ((size_t)(cy * 10 + y) * 480 + cx * 10 + x) * 4; sum += (q[0] * 2u + q[1] * 5u + q[2]) / 8u; }
                        hist[2][cy * 48 + cx] = (uint8_t)(sum / 100);
                    }
                if (++nh >= 3) {
                    int n = 0, x0 = 99, y0 = 99, x1 = -1, y1 = -1;
                    for (int k = 0; k < 48 * 27; k++) {
                        int a = hist[0][k], b = hist[1][k], c = hist[2][k];
                        if (abs(a - c) < 4 && abs(b - a) > 12 && abs(b - c) > 12) { n++; if (k % 48 < x0) x0 = k % 48; if (k % 48 > x1) x1 = k % 48; if (k / 48 < y0) y0 = k / 48; if (k / 48 > y1) y1 = k / 48; }
                    }
                    seen++;
                    if (n) { flick++; printf("flicker: frame %lld: %d cells in x %d..%d y %d..%d (of 48x27)\n", (long long)frame - 1, n, x0, x1, y0, y1); }
                    {   static int dumps = -2, dfrom, dcells = 20;   /* TOMO_FLICKER_DUMP=<n>[,<frame>[,<cells>]]: the frames before, of and after the first n flickers of <cells> (20) cells or more (past <frame>), 480x270 */
                        if (dumps == -2) { const char *e = getenv("TOMO_FLICKER_DUMP"), *c2 = e && strchr(e, ',') ? strchr(strchr(e, ',') + 1, ',') : NULL; dumps = e ? atoi(e) : 0; dfrom = e && strchr(e, ',') ? atoi(strchr(e, ',') + 1) : 0; if (c2) dcells = atoi(c2 + 1); }
                        if (dumps > 0 && n >= dcells && frame > dfrom) {
                            dumps--;
                            for (int k = 0; k < 3; k++) {
                                char path[64]; snprintf(path, sizeof path, "flk_%lld_%c.ppm", (long long)frame - 1, 'a' + k);
                                FILE *fp = fopen(path, "wb");
                                if (!fp) continue;
                                fprintf(fp, "P6 480 270 255\n");
                                for (int y = 269; y >= 0; y--) for (int x = 0; x < 480; x++) fwrite(ring[k] + ((size_t)y * 480 + x) * 4, 1, 3, fp);
                                fclose(fp);
                            }
                        }
                    }
                    {   static int period = -2, pfrom;     /* TOMO_FLICKER_PERIOD=<p>[,<frame>]: after the first big flicker past <frame> (3000), the draw log of the frame p later and of the one before */
                        if (period == -2) { const char *e = getenv("TOMO_FLICKER_PERIOD"); period = e ? atoi(e) : -1; pfrom = e && strchr(e, ',') ? atoi(strchr(e, ',') + 1) : 3000; }
                        if (period > 0 && n >= 20 && frame > pfrom && g_dl_force[0] < 0) {
                            g_dl_force[0] = frame - 2 + period - 1; g_dl_force[1] = frame - 2 + period;
                            printf("flicker: draw log scheduled for rendered frames %lld and %lld\n", (long long)g_dl_force[0], (long long)g_dl_force[1]);
                        }
                        if (g_dl_force[0] >= 0 && (frame == g_dl_force[0] + 1 || frame == g_dl_force[1] + 1)) { char path[64]; snprintf(path, sizeof path, "flick_%lld.bmp", (long long)frame); save_bmp(path, t); }
                    }
                    {   static int shots = -2, from = 0, left;     /* TOMO_FLICKER_SHOTS=<n>[,<frame>]: after the first flicker of 100 cells or more past <frame> (2000), the next n frames as flick_<frame>.bmp */
                        if (shots == -2) { const char *e = getenv("TOMO_FLICKER_SHOTS"); shots = e ? atoi(e) : 0; from = e && strchr(e, ',') ? atoi(strchr(e, ',') + 1) : 2000; }
                        if (shots > 0 && !left && n >= 100 && frame > from) { left = shots; shots = 0; printf("flicker: saving the next %d frames\n", left); }
                        if (left > 0) { char path[64]; snprintf(path, sizeof path, "flick_%lld.bmp", (long long)frame); save_bmp(path, t); left--; }
                    }
                    if (seen % 600 == 0) printf("flicker: %lld of %lld frames flickered\n", (long long)flick, (long long)seen);
                }
            }
        }
        {   /* FLICKER RECORDER (2026-11-01; TOMO_FLICKER_LOG=0: off). The user sees Miis and backgrounds flicker for a frame now and
             * then; neither their recording nor the test scenes caught one. Every presented picture goes into a 480x270 copy read
             * back 3 frames late through a mapped buffer (no wait). A frame that differs from the one before and the one after in
             * 4 or more of 48x27 cells of a picture that otherwise stands still (the frames around it agree on 85 % of them: no
             * camera move, no transition; a close Mii's blink changes 2-3) is saved with its neighbours as flicker_log\f<frame>.bmp (before | flicker | after), 60 a run. */
            static int on = -1, k, have, saved;
            static GLuint fbx[2], ttex, pb;
            static uint8_t *pm, *img[3];
            static void *fs[4];
            static long long ff[4], fimg[3], last_hit = -100;
            if (on < 0) { const char *e = getenv("TOMO_FLICKER_LOG"); on = !(e && e[0] == '0') && gl_has(glNamedBufferStorage, "glNamedBufferStorage") && gl_has(glMapNamedBufferRange, "glMapNamedBufferRange"); }
            if (on && t && t->o.gl && t->b.width > 0 && t->b.height > 0 && frame > 300) {
                enum { FW = 480, FH = 270, FB = FW * FH * 4, CW = 48, CH = 27 };
                if (!pb) {
                    glGenFramebuffers(2, fbx); glGenTextures(1, &ttex);
                    glBindTexture(GL_TEXTURE_2D, ttex); glTexStorage2D(GL_TEXTURE_2D, 1, 0x8058 /* RGBA8 */, FW, FH);
                    glCreateBuffers(1, &pb); glNamedBufferStorage(pb, 4 * FB, NULL, 0x1 | 0x40 | 0x80 /* READ | PERSISTENT | COHERENT */);
                    pm = glMapNamedBufferRange(pb, 0, 4 * FB, 0x1 | 0x40 | 0x80);
                    for (int i = 0; i < 3; i++) img[i] = malloc(FB);
                    if (!pm || !img[0] || !img[1] || !img[2]) on = 0;
                }
                if (on) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER_, fbx[0]); glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, 0);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, fbx[1]); glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, ttex, 0);
                    glDisable(GL_SCISSOR_TEST);
                    glBlitFramebuffer(sr[0], sr[1], sr[0] + sr[2], sr[1] + sr[3], 0, 0, FW, FH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER_, fbx[1]);
                    glBindBuffer(0x88EB /* PIXEL_PACK_BUFFER */, pb); glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, FW, FH, 0x1908 /* RGBA */, GL_UNSIGNED_BYTE, (void *)(uintptr_t)((size_t)k * FB));
                    glBindBuffer(0x88EB, 0);
                    glEnable(GL_SCISSOR_TEST);
                    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
                    if (fs[k]) glDeleteSync(fs[k]);
                    fs[k] = glFenceSync(0x9117 /* SYNC_GPU_COMMANDS_COMPLETE */, 0); ff[k] = frame;
                    k = (k + 1) & 3;                           /* the oldest of the four: 3 frames ago */
                    GLenum r = fs[k] ? glClientWaitSync(fs[k], 0, 0) : 0;
                    if (r == 0x911A || r == 0x911C) {          /* ALREADY_SIGNALED / CONDITION_SATISFIED */
                        uint8_t *o = img[0]; img[0] = img[1]; img[1] = img[2]; img[2] = o;
                        fimg[0] = fimg[1]; fimg[1] = fimg[2]; fimg[2] = ff[k];
                        memcpy(img[2], pm + (size_t)k * FB, FB);
                        if (++have >= 3 && fimg[1] == fimg[0] + 1 && fimg[2] == fimg[1] + 1) {
                            static uint8_t cl[3][CW * CH];
                            for (int i = 0; i < 3; i++)       /* each cell's mean luminance, every other pixel of its 10x10 */
                                for (int cy = 0; cy < CH; cy++) for (int cx = 0; cx < CW; cx++) {
                                    unsigned s = 0;
                                    for (int y = 0; y < 10; y += 2) for (int x = 0; x < 10; x += 2) { const uint8_t *q = img[i] + ((size_t)(cy * 10 + y) * FW + cx * 10 + x) * 4; s += (q[0] * 2u + q[1] * 5u + q[2]) / 8u; }
                                    cl[i][cy * CW + cx] = (uint8_t)(s / 25);
                                }
                            int still = 0, hit = 0, x0 = CW, y0 = CH, x1 = -1, y1 = -1;
                            for (int c = 0; c < CW * CH; c++) {
                                int a = cl[0][c], b = cl[1][c], d = cl[2][c];
                                if (abs(a - d) < 4) { still++; if (abs(b - a) > 20 && abs(b - d) > 20) { hit++; if (c % CW < x0) x0 = c % CW; if (c % CW > x1) x1 = c % CW; if (c / CW < y0) y0 = c / CW; if (c / CW > y1) y1 = c / CW; } }
                            }
                            if (hit >= 4 && still >= CW * CH * 85 / 100 && hit <= CW * CH * 40 / 100 && fimg[1] - last_hit > 10 && saved < 60) {
                                char path[64];
                                last_hit = fimg[1]; saved++;
                                CreateDirectoryA("flicker_log", NULL);
                                snprintf(path, sizeof path, "flicker_log/f%lld.bmp", fimg[1]);
                                FILE *fp = fopen(path, "wb");
                                if (fp) {             /* a 24-bit BMP (bottom row first; the picture's rows come top first) */
                                    uint8_t head[54] = {'B', 'M'};
                                    uint32_t row = 3 * FW * 3, size = 54 + row * FH, v;
                                    memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4);
                                    v = 3 * FW; memcpy(head + 18, &v, 4); v = FH; memcpy(head + 22, &v, 4); head[26] = 1; head[28] = 24;
                                    fwrite(head, 1, 54, fp);
                                    uint8_t *line = malloc(row);
                                    for (int y = FH - 1; line && y >= 0; y--) {
                                        for (int i = 0; i < 3; i++) for (int x = 0; x < FW; x++) { const uint8_t *q = img[i] + ((size_t)y * FW + x) * 4; uint8_t *o2 = line + ((size_t)i * FW + x) * 3; o2[0] = q[2]; o2[1] = q[1]; o2[2] = q[0]; }
                                        fwrite(line, 1, row, fp);
                                    }
                                    free(line);
                                    fclose(fp);
                                }
                                printf("flicker log: frame %lld saved as %s (%d of %d cells changed for one frame, x %d..%d y %d..%d of 48x27)%c", fimg[1], path, hit, CW * CH, x0, x1, y0, y1, 10);
                                fflush(stdout);
                            }
                        }
                    }
                }
            }
        }
        if ((getenv("SSBU_RTDUMP") && frame == atoi(getenv("SSBU_RTDUMP"))) || InterlockedExchange(&g_rtdump_step, 0)) {       /* debug: every colour target in use, as rt_<n>.bmp */
            for (int k = 0; k < NFBO; k++) {
                Tex fake;
                char path[64];
                GLint w = 0, h = 0;
                if (!g_fbos[k].used || !g_fbos[k].k.color[0]) continue;
                glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbos[k].fbo);
                glBindTexture(GL_TEXTURE_2D, g_fbos[k].k.color[0]);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
                if (w <= 0 || h <= 0 || (w < 64 && h < 64)) continue;
                {   int fw, fh; const char *e = getenv("SSBU_RTDUMP_SIZE");      /* TEST: <w>x<h>, only targets of that size */
                    if (e && sscanf(e, "%dx%d", &fw, &fh) == 2 && (fw != w || fh != h)) continue; }
                memset(&fake, 0, sizeof fake);
                fake.b.width = w; fake.b.height = h;
                snprintf(path, sizeof path, "rt_%03d_gl%u_f%lld.bmp", k, g_fbos[k].k.color[0], (long long)frame);
                save_bmp(path, &fake);
                if (getenv("SSBU_RTDUMP_ALPHA")) { fake.b.depth = -1; snprintf(path, sizeof path, "rt_%03d_gl%u_a.bmp", k, g_fbos[k].k.color[0]); save_bmp(path, &fake); }
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
        }
    } else {
        glClearColor(0, 0, 0, 1);                 /* nothing has been drawn into the presented texture yet */
        glClear(GL_COLOR_BUFFER_BIT);
    }
#ifdef _WIN32
    if (!ff_skip) menu_gl_draw((int)r.right, (int)r.bottom);      /* the in-game settings menu over the picture (nvn_gl_menu.c) */
    fbo_detach(g_fbo_read);
    if (!ff_skip) present_fence(frame);
    if (ff_skip) { (void)display_present_defer(); }           /* (not shown; the plan's target is consumed) */
    else if (!j->shot_fight && display_present_defer()) {      /* kept in the back buffer for its tick (see gl_swap_now) */
        glFlush();
        InterlockedExchange(&g_display_holding, 1);
        InterlockedExchange(&g_gl_swap_pending, 1);
        if (display_present_due()) gl_swap_now();          /* the tick came meanwhile */
    } else gl_swap_now();
#else
    present_fence(frame);
#endif
    pf_add(PF_PRESENT, pf0);
    pf_frame_mark(frame);
    g_joblog_cur = frame;
    glEnable(GL_FRAMEBUFFER_SRGB_);
    glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
    {   static long long lo = -2, hi;          /* TOMO_FRAMESUM=<from>-<to>: one line per frame with the counts above */
        if (lo == -2) { const char *e = getenv("TOMO_FRAMESUM"); lo = -1; if (e) sscanf(e, "%lld-%lld", &lo, &hi); }
        if (lo >= 0 && frame >= lo && frame <= hi)
            printf("framesum: frame %lld: draws %lld clears %lld | 2048 targets: draws %lld clears %lld | target switches %lld dispatches %lld\n", (long long)frame, (long long)g_frame_draws, (long long)g_frame_clears,
                   (long long)g_fs_big, (long long)g_fs_bigclr, (long long)g_fs_rt, (long long)g_fs_disp);
        g_fs_big = g_fs_bigclr = g_fs_rt = g_fs_disp = 0;
    }
    arena_retire();                              /* all draws using the previous stores have been submitted */
    gpuprof_frame();
    g_st.frames = frame;
    g_gpu_ts_base = now_ns(); g_gpu_ts_k = 0;
    if (frame <= 3 || frame % 600 == 0 || g_frame_draws != 0 && g_logn < 40 && (g_logn++, 1))
        printf("nvn_gl: frame %lld: %lld draws (%lld skipped), %lld clears | totals: %lld lists, %lld textures, %lld uploads (%lld MB), %lld programs ok, %lld rejected, %lld GL errors\n",
               (long long)frame, (long long)g_frame_draws, (long long)g_frame_skipped, (long long)g_frame_clears, (long long)g_st.lists, (long long)g_st.tex_created,
               (long long)g_st.tex_uploads, (long long)(g_st.tex_bytes >> 20), (long long)g_st.prog_ok, (long long)g_st.prog_fail, (long long)g_st.gl_errors);
    fflush(stdout);
    {   /* frame rate over the last 300 presented frames */
        static ULONGLONG t0;
        static LONG64 d0, c0;
        if (frame % 300 == 0) {
            ULONGLONG now = GetTickCount64();
            if (t0 && now > t0) printf("nvn_gl: frames %lld..%lld: %.1f fps, %lld draws and %lld dispatches per frame\n", (long long)frame - 300, (long long)frame, 300000.0 / (double)(now - t0),
                                       (long long)(g_st.draws - d0) / 300, (long long)(g_st.dispatches - c0) / 300);
            t0 = now; d0 = g_st.draws; c0 = g_st.dispatches;
            if (frame % 3000 == 0 || g_dummy_draws) { static long long last = -1; if (g_dummy_draws != last) { last = g_dummy_draws; printf("nvn_gl: dummy texture: %lld of %lld draws reference a unit that holds it (%lld units unbound, %lld units with a dead or unknown handle)\n", g_dummy_draws, (long long)g_st.draws, g_dummy_unbound, g_dummy_dead); } }
        }
    }
    g_prev_frame_draws = g_frame_draws;
    if (g_frame_dispatch >= (g_drawlog_mindisp_n > 0 ? g_drawlog_mindisp_n : 1)) g_disp_frames++;
    g_frame_dispatch = 0; g_ncrange = 0;
    input_frame_draws((int)g_frame_draws);        /* nnhid.c: scripted input can wait for a screen by its draw count */
    g_frame_draws = g_frame_skipped = g_frame_clears = 0;
    g_frame_draw_index = 0;
    list_collect();
}

static void job_write_texels(void *arg) {
    WriteJob *j = arg;
    if (g_rbp_n) rb_land(1);
    Tex *t = j->tex;
    Fmt f;
    static int nlog;
    if (!t->o.gl) tex_realize(t);
    if (!fmt_get(t->b.format, &f)) return;
    int level = j->has_view && (j->view.set & 0x81) == 0x81 ? j->view.lvl0 : 0, layer = j->has_view && (j->view.set & 0x82) == 0x82 ? j->view.lay0 : 0;
    const int *r = j->region;
    while (glGetError()) {}
    if (j->row_stride && f.kind != 4) glPixelStorei(GL_UNPACK_ROW_LENGTH_, (GLint)(j->row_stride / f.bpb));
    if (t->b.target == 4 || t->b.target == 9 || t->b.target == 8) layer += r[2];
    tex_put(t, &f, level, layer, r[0], r[1], r[2], r[3], r[4], r[5] > 0 ? r[5] : 1, j->data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
    GLenum e = glGetError();
    if (e) logf_limited(&nlog, 10, "nvn_gl: WriteTexels on texture %dx%dx%d format 0x%x region %d,%d,%d %dx%dx%d failed: GL error 0x%x\n", t->b.width, t->b.height, t->b.depth, t->b.format,
                        r[0], r[1], r[2], r[3], r[4], r[5], e);
    t->gpu_written = 1; t->uploaded = 1;
    g_st.copies++;
}

/* The render thread's job loop (both back ends). */
static void render_loop(void) {
    for (;;) {
        if (WaitForSingleObject(g_job_event, g_rbp_n || pre_pending() ? 1 : INFINITE) == WAIT_TIMEOUT) {      /* read-backs or binaries pending: look again in 1 ms */
            if (g_rb->idle) g_rb->idle();
            if (pre_pending()) pre_idle();
            continue;
        }
        for (;;) {
            EnterCriticalSection(&g_job_lock);
            Job *j = g_job_head;
            if (j) { g_job_head = j->next; if (!g_job_head) g_job_tail = NULL; }
            LeaveCriticalSection(&g_job_lock);
            if (!j) break;
            HANDLE done = j->done;
            if (g_rb->idle) g_rb->idle();
            g_hitch_rt = j->fn == g_rb->submit || j->fn == g_rb->submit_async ? "submitted commands (outside the named scopes)" : j->fn == g_rb->present ? "present (blit)" : j->fn == g_rb->sync_wait ? "SyncWait job" : "other job (texel read/write, finish, barrier, ...)";
            j->fn(j->arg);                       /* a member of g_rb (seam step S2), or a job of the front end / of the back end itself */
            g_hitch_rt = "idle";
            if (done) SetEvent(done); else { free(j); InterlockedDecrement(&g_async_pending); }      /* gl_post: nobody waits */
        }
    }
}

static DWORD WINAPI render_thread(LPVOID arg) {
    (void)arg;
    pf_job_submit = g_rb->submit; pf_job_present = g_rb->present;
    g_joblog_frame = getenv("SSBU_JOBLOG") ? atoi(getenv("SSBU_JOBLOG")) : 0;
    { void display_perf_host_thread(void); display_perf_host_thread(); }        /* display.c: performance cores the game's threads do not hold */
#ifdef _WIN32
    PIXELFORMATDESCRIPTOR pfd;
#else
    int pfd = 0;
#endif
    g_render_tid = GetCurrentThreadId();
    {   /* Round 17, MEASUREMENT: SSBU_RENDER_PRIO=1 above normal, 2 highest (default: normal, as before). The game's own
         * threads run above normal when the game asked for a priority under 16 (os_threads.c host_priority). */
        const char *e = getenv("SSBU_RENDER_PRIO");
        int p = e ? atoi(e) : 0;
        if (p > 0) { SetThreadPriority(GetCurrentThread(), p >= 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL); printf("nvn: render thread priority %s (SSBU_RENDER_PRIO)\n", p >= 2 ? "highest" : "above normal"); }
    }
    if (g_vk) {                                 /* V1 step S0: no OpenGL context, no shader workers; the jobs go to nvn_vk.c (vk_route) */
        nvn_vk_init(g_hwnd);
        hitch_set_render();
        SetEvent(g_ready_event);
        render_loop();
        return 0;
    }
#ifndef _WIN32
    (void)pfd;
    printf("nvn_gl: the OpenGL renderer is not part of the Linux build\n"); fflush(stdout); ExitProcess(5);
#else
    g_dc = GetDC(g_hwnd);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    SetPixelFormat(g_dc, ChoosePixelFormat(g_dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(g_dc);
    if (!rc || !wglMakeCurrent(g_dc, rc)) { printf("nvn_gl: cannot create an OpenGL context (error %lu)\n", GetLastError()); fflush(stdout); ExitProcess(5); }
    int missing = 0;
#define X(ret, name, args) gl##name = (void *)wglGetProcAddress("gl" #name); if (!gl##name) { printf("nvn_gl: the driver lacks gl" #name "\n"); missing++; }
    GLF(X)
#undef X
#define X(ret, name, args) gl##name = (void *)wglGetProcAddress("gl" #name);
    GLO(X)
#undef X
    printf("nvn_gl: %s | %s | OpenGL %s\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    if (missing) { printf("nvn_gl: %d required OpenGL functions are missing; this backend needs an NVIDIA driver\n", missing); fflush(stdout); ExitProcess(5); }
    BOOL (WINAPI *swap_interval)(int) = (void *)wglGetProcAddress("wglSwapIntervalEXT");
    {   /* 2026-10-25: the flip waits for the vertical blank unless vsync = off. With interval 0 the frame clock's present (half
         * a refresh after a vblank, display.c) flipped at once: unseen while Windows composed the window, a tear across the
         * picture once the window covered the monitor at its real size (DPI aware: flipped directly). TOMO_SWAP_INTERVAL wins. */
        const char *e = getenv("TOMO_SWAP_INTERVAL");
        int si = e ? atoi(e) : gfx_options()->present == 2 ? 0 : 1;
        if (swap_interval) swap_interval(si);
#ifdef _WIN32
        g_swap_si = si;
#endif
        printf("nvn_gl: swap interval %d (%s)\n", si, e ? "TOMO_SWAP_INTERVAL" : si ? "the flip waits for the vertical blank: no tearing" : "vsync = off: tearing allowed");
    }
    g_debug = getenv("SSBU_GLDEBUG") != NULL;
    g_dbg_noscissor = getenv("SSBU_NOSCISSOR") != NULL;
    g_dbg_flipface = getenv("SSBU_FLIPFACE") != NULL;
    g_dbg_nocompute = getenv("SSBU_NOCOMPUTE") != NULL;
    g_drawlog_mindisp_n = getenv("SSBU_DRAWLOG_MINDISP") ? atoi(getenv("SSBU_DRAWLOG_MINDISP")) : 0;
    if (getenv("SSBU_SKIPDRAW")) g_dbg_skipdraw = atoi(getenv("SSBU_SKIPDRAW"));
    glEnable(GL_DEBUG_OUTPUT_);
    /* P3 (hitch log, tests/logs/p3b_hitch0.log against p3b_hitch_dbg.log): without synchronous debug output this driver
     * hands the GL calls of the render thread to a thread of its own, and every call that returns something (texture
     * handles, program status, glGetError, buffer read-back) then waits for everything queued before it: 0.1-2 s frames
     * whenever a new shader or texture arrives (65 s of frames over 25 ms in a 170 s run; 13 s with it on). Synchronous
     * debug output is a debugging state of the context: it changes when the callback is called, not what is drawn; the
     * callback prints errors only unless SSBU_GLDEBUG is set, as before. Since 2026-10-31 only with SSBU_GL_DRIVER_THREAD=0
     * or SSBU_GLDEBUG (gl_driver_thread: steady frames gained more than the uploads lost). */
    if (gl_driver_thread()) g_gl_thr_on = 1; else glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_);
    g_fdlog = getenv("SSBU_FDLOG") != NULL;
    if (g_fdlog && glDebugMessageControl) glDebugMessageControl(0x1100 /* DONT_CARE */, 0x1100, 0x1100, 0, NULL, 1);     /* every severity, low ones too */
    glDebugMessageCallback((void *)debug_cb, NULL);
    glGetIntegerv(GL_MAX_PROGRAM_PARAMETER_BUFFER_BINDINGS_NV_, &g_max_pbuf);
    printf("nvn_gl: parameter buffer bindings per program: %d; window origin mode %d, depth mode %d\n", g_max_pbuf, g_origin_mode, g_depth_mode);
    glClipControl(g_origin_mode ? GL_UPPER_LEFT_ : GL_LOWER_LEFT_, g_depth_mode ? GL_ZERO_TO_ONE_ : GL_NEGATIVE_ONE_TO_ONE_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glEnable(GL_FRAMEBUFFER_SRGB_);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS_);
    glEnable(GL_SCISSOR_TEST);
    for (GLuint i = 0; i < 16; i++) { glScissorIndexed(i, 0, 0, 16384, 16384); S.gsc[i][2] = S.gsc[i][3] = 16384; }
    glGenBuffers(1, &g_arena);
    glBindBuffer(GL_ARRAY_BUFFER_, g_arena);
    glBufferData(GL_ARRAY_BUFFER_, (ptrdiff_t)ARENA_SIZE, NULL, GL_STREAM_DRAW_);
    glGenFramebuffers(1, &g_fbo_read);
    glGenFramebuffers(2, g_fbo_tmp);
    glGenProgramPipelines(1, &g_pipeline);
    static const uint8_t black[4] = {0, 0, 0, 255};
    { void album_movie_frame_source_attach(void); album_movie_frame_source_attach(); }   /* close4: queue_present feeds the movie maker (job_present_movie) */
    glGenTextures(1, &g_dummy_tex);
    glBindTexture(GL_TEXTURE_2D, g_dummy_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, 0x8058, 1, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g_dummy_handle = glGetTextureHandleARB(g_dummy_tex);
    glMakeTextureHandleResidentARB(g_dummy_handle);
    for (int s = 0; s < 6; s++) { for (int i = 4; i < 36; i++) S.gl_tex[s][i] = g_dummy_handle; for (int i = 0; i < 32; i++) { uint32_t d[4] = {1, 1, 1, 1}; memcpy(&S.gl_tex[s][48 + 2 * i], d, 16); } S.tex_dirty[s] = 1; }
    S.chanmask = 0xffffffffu; S.svmask[0] = S.svmask[1] = 0xff; S.swmask[0] = S.swmask[1] = 0xff;
    S.sc0[2] = S.sc0[3] = 16384;
    S.ds = 0x1111118822ULL;                     /* depth func LESS, stencil funcs ALWAYS, ops KEEP */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SwapBuffers(g_dc);
#endif
    fflush(stdout);
    shd_init();
    hitch_set_render();
    arena_selftest();                            /* SSBU_ARENA_SELFTEST: rollover must preserve this draw's inputs */
    bc_cache_selftest();                         /* SSBU_BC_CACHE_SELFTEST: wrap the scratch cache twice and check its copies */
    frozen_clear_selftest();                     /* SSBU_FROZEN_CLEAR_SELFTEST: a new UI must outlive an old snapshot */
    frozen_drawtexture_selftest();               /* SSBU_FROZEN_DRAWTEXTURE_SELFTEST: restore before the first 2D draw */
    nvn_c4_gl_selftest_hook();                  /* SSBU_NVN_GLTEST: OpenGL self-test of nvn_c4.c, then exit */
    SetEvent(g_ready_event);
    render_loop();
    return 0;
}

/* Called by display.c once the window exists. */
void nvn_gl_start(void *hwnd) {
    g_hwnd = hwnd;
    {   /* V1: which back end. SSBU_RENDERER=gl (default) | vk */
        const char *e = getenv("SSBU_RENDERER");
        if (e && e[0] && strcmp(e, "gl") && strcmp(e, "vk")) { printf("nvn: SSBU_RENDERER=%s is not a renderer (gl, vk)\n", e); fflush(stdout); ExitProcess(5); }
        /* Track V session 4: VULKAN IS THE DEFAULT. SSBU_RENDERER=gl|vk wins; else renderer = vulkan | opengl of config\graphics.ini. */
        g_vk = e && e[0] ? !strcmp(e, "vk") : !gfx_options()->renderer;
#ifndef _WIN32
        if (!g_vk) { printf("nvn: the OpenGL renderer is not part of the Linux build: Vulkan instead\n"); g_vk = 1; }
#endif
        g_rb = g_vk ? &rb_vk : &rb_gl;
        printf("nvn: renderer: %s (%s)\n", g_vk ? "Vulkan (nvn_vk.c)" : "OpenGL (nvn_gl.c)", e && e[0] ? "SSBU_RENDERER" : "graphics.ini; the default is Vulkan");
    }
    InitializeCriticalSection(&g_job_lock);
    InitializeCriticalSection(&g_dead_lock);
    g_job_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    { const char *e = getenv("SSBU_STACKS_AT"); void nvn_vk_stacks_at(int seconds); if (e && atoi(e) > 0) nvn_vk_stacks_at(atoi(e)); }      /* session 13, TEST: every thread's call stack once, that many seconds from here, with either renderer (nvn_vk_wd.c) */
    { const char *e = getenv("TOMO_FREEZE_DUMP"); void nvn_wd_freeze_dump(int ms); if (e && atoi(e) > 0) nvn_wd_freeze_dump(atoi(e)); }      /* TEST: the same report when the frames stop that many ms (nvn_vk_wd.c) */
    CreateThread(NULL, 16 << 20, render_thread, NULL, 0, NULL);
    WaitForSingleObject(g_ready_event, INFINITE);
}

void nvn_gl_report(FILE *out) {
    fprintf(out, "\nnvn_gl: recorded command lists kept at the end: %lld MB\n", (long long)(g_list_bytes >> 20));
    fprintf(out, "\nnvn_gl: %lld frames, %lld batches, %lld command lists (%lld unknown handles), %lld draws (+%lld skipped), %lld clears, %lld copies\n", (long long)g_st.frames,
            (long long)g_st.submits, (long long)g_st.lists, (long long)g_st.stale, (long long)g_st.draws, (long long)g_st.draws_skipped, (long long)g_st.clears, (long long)g_st.copies);
    fprintf(out, "nvn_gl: textures %lld (uploads %lld, %lld MB, views %lld, bindless handles %lld, unmapped formats %lld), framebuffers %lld (%lld incomplete)\n", (long long)g_st.tex_created,
            (long long)g_st.tex_uploads, (long long)(g_st.tex_bytes >> 20), (long long)g_st.views, (long long)g_st.handles, (long long)g_st.unknown_fmt, (long long)g_st.fbo_made, (long long)g_st.fbo_bad);
    fprintf(out, "nvn_gl: nvnProgramSetShaders calls %ld, stages compiled %lld, rejected %lld, GL errors %lld, streamed %lld MB\n", (long)g_setshaders_calls, (long long)g_st.prog_ok,
            (long long)g_st.prog_fail, (long long)g_st.gl_errors, (long long)(g_st.arena_bytes >> 20));
    fprintf(out, "nvn_gl: shader cache: %lld stages compiled by the driver, %lld taken from the cache by machine code, %lld by identical GLSL text; texture memory checks: %lld unwritten (write watch), %lld written and hashed, %lld not watched (hashed)\n",
            (long long)g_shc_compiled, (long long)g_shc_hit, (long long)g_shc_src_hit, (long long)g_ww_clean, (long long)g_ww_dirty, (long long)g_ww_unwatched);
    fprintf(out, "nvn_gl: shader binaries on disk: %lld loaded, %lld compiled and saved by the render thread, %lld refused by the driver (compiled again)\n", (long long)g_shd_loaded, (long long)g_shd_saved, (long long)g_shd_refused);
    for (int i = 0; g_fdlog && i < 64 && g_fd_ids[i].n; i++) fprintf(out, "fdlog-id: %u type %x severity %x: %lld messages, first: %s\n", g_fd_ids[i].id, g_fd_ids[i].type, g_fd_ids[i].sev, (long long)g_fd_ids[i].n, g_fd_ids[i].text);
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    ga_report(out);
      fprintf(out, "nvn_gl: shader pairs made ready by the workers (SSBU_PAIR_WARM=%d): %lld from pairs.txt + %lld programs handed over; %lld warm draws, %lld of them over 8 ms (not in the driver's cache), %.1f s in all; %lld known already, %lld without binaries or not translated, %lld stages compiled for it\n",
              pw_mode(), (long long)g_pw_named, (long long)g_pw_progs, (long long)g_pw_warmed, (long long)g_pw_cold_n, (double)g_pw_ticks / (double)f.QuadPart, (long long)g_pw_dup, (long long)g_pw_missing, (long long)g_pw_compiled);
      fprintf(out, "nvn_gl: presents held for shader preparation: %lld times, %.2f s in all\n", (long long)g_pw_gates, (double)g_pw_gate_ticks / (double)f.QuadPart); }
    fprintf(out, "nvn_gl: asynchronous shaders%s: %lld stages handed to the workers during play, %lld draws skipped while they compiled, %lld compiled here after 20 s\n",
            shader_async() ? "" : " (off: SSBU_SHADER_SYNC)", (long long)g_pend_started, (long long)g_pend_skipped, (long long)g_pend_late);
    fprintf(out, "nvn_gl: shader pairs drawn (pairs.txt): %lld known at start, %lld added in this run\n", (long long)g_prec_loaded, (long long)g_prec_added);
    if (pair_mode()) fprintf(out, "nvn_gl: linked pair programs (SSBU_PAIR=%d): %lld loaded from disk, %lld linked here, %lld not linkable, %lld binaries refused, %lld draws used one\n", pair_mode(), (long long)g_pair_loaded, (long long)g_pair_linked, (long long)g_pair_failed, (long long)g_pair_refused, (long long)g_pair_draws);
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fprintf(out, "nvn_gl: first draws of a program pair preceded by a discarded point: %lld (%lld over 4 ms, %.0f ms in all), not done %lld (counter running or vertex stage with side effects)\n", (long long)g_fdw_n, (long long)g_fdw_slow, (double)g_fdw_ticks * 1000.0 / (double)f.QuadPart, (long long)g_fdw_skipped); }
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fprintf(out, "nvn_gl: nvnProgramSetShaders held its caller %lld times, %.1f s in all, while all workers were compiling\n", (long long)g_shw_held, (double)g_shw_held_ticks / (double)f.QuadPart); }
    fprintf(out, "nvn_gl: shader variants (variants.txt): %lld known at start, %lld of them queued for the workers, %lld added in this run\n", (long long)g_shv_loaded, (long long)g_shv_queued, (long long)g_shv_added);
    fprintf(out, "nvn_gl: shader workers (%d): %lld stages queued, %lld done (%lld compiled and saved, %lld already on disk, %lld not translated or not saved)\n", g_shw_n, (long long)g_shw_queued, (long long)g_shw_done,
            (long long)g_shw_compiled, (long long)g_shw_present, (long long)g_shw_failed);
    fprintf(out, "nvn_gl: shaders given with NV assembly text: %ld; with Maxwell machine code only: %ld (translated to GLSL: %lld stages, not translatable yet: %lld; draws through GLSL: %lld)\n", (long)g_with_text,
            (long)g_without_text, (long long)g_st.glsl_ok, (long long)g_st.glsl_fail, (long long)g_st.glsl_draws);
    fprintf(out, "nvn_gl: compute dispatches %lld (run %lld, skipped %lld), %lld MB copied back to guest memory, draws using a vertex buffer written by compute in the same frame: %lld\n",
            (long long)g_st.dispatches, (long long)g_st.dispatch_run, (long long)g_st.dispatch_skipped, (long long)(g_st.compute_bytes >> 20), (long long)g_st.draws_from_compute);
    for (int h = 0; h < 64 && g_chist[h].serial; h++)
        fprintf(out, "nvn_gl:   compute program #%d: %lld dispatches (%lld run), %lld groups\n", g_chist[h].serial, (long long)g_chist[h].calls, (long long)g_chist[h].run, (long long)g_chist[h].groups);
    for (int s = 0; s < 6; s++) {
        int any = 0;
        for (int i = 0; i < 32; i++) any |= g_bufhist[s][i] != 0;
        if (!any) continue;
        fprintf(out, "nvn_gl: program.buffer[n] uses in %s programs (n:count):", k_stage_name[s]);
        for (int i = 0; i < 32; i++) if (g_bufhist[s][i]) fprintf(out, " %d:%ld", i, (long)g_bufhist[s][i]);
        fprintf(out, "\n");
    }
    fprintf(out, "nvn_gl: program.storage[n] uses (n:count):");
    for (int i = 0; i < 32; i++) if (g_storhist[i]) fprintf(out, " %d:%ld", i, (long)g_storhist[i]);
    fprintf(out, "\n");
    hitch_report(out);
    if (g_vk) { nvn_vk_report(out); vk_shutdown_at_exit(); }
}

/* ================================================================ the NVN entry points */
static void n_nop(Ctx *c) { (void)c; }
static void n_true(Ctx *c) { RET(1); }
static void n_zero(Ctx *c) { RET(0); }

/* ---- device */
/* the clip control is set at the GL start-up; a mode the game sets later (Tomodachi Life does, after the device exists) is
 * applied by the render thread before its next batch (job_submit): until then the start-up's modes flipped its winding */
static void dev_origin_mode(Ctx *c) { g_origin_mode = (int)A1; InterlockedExchange(&g_clip_dirty, 1); }
static void dev_depth_mode(Ctx *c) { g_depth_mode = (int)A1; InterlockedExchange(&g_clip_dirty, 1); }
/* The real values (close5; until then this port gave textureID | samplerID << 32 and the bare ID for the other three):
 *   nvnDeviceGetTextureHandle [sdk 0x41bc80]         sign-extended 32-bit (textureID | samplerID << 20), then bit 32 set
 *   nvnDeviceGetTexelFetchHandle [0x41bc70]          sign-extended textureID, bit 32 set
 *   nvnDeviceGetImageHandle [0x41bc00]               sign-extended textureID */
static void dev_texture_handle(Ctx *c) { uint32_t h = (uint32_t)A1 | (uint32_t)A2 << 20; bl_note(h); RET((uint64_t)(int64_t)(int32_t)h | 0x100000000ull); }   /* (device, textureID, samplerID) */
static void dev_texel_fetch_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)(uint32_t)A1 | 0x100000000ull); }
static void dev_image_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)(uint32_t)A1); }

/* ---- programs */
static volatile LONG g_prog_serial;
static void prog_initialize(Ctx *c) {
    Prog *p = hnew(A0, K_PROG, sizeof(Prog));
    p->serial = (int)InterlockedIncrement(&g_prog_serial);
    p->guest = A0;
    RET(1);
}
static void prog_finalize(Ctx *c) {
    gl_barrier();
    Prog *p = hobj(A0, K_PROG);
    if (!p) return;
    InterlockedExchange(&p->dead, 1);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
    defer_free(K_PROG, p);
}
/* Debug aid SSBU_SHADERDUMP_ALL=<dir>: every shader stage handed to nvnProgramSetShaders, whole, as <dir>/s<program>_<i>.mxd for the
 * offline harness (mxtest.exe). File: "MXD1", u32 program serial, u32 index in the call, u32 control bytes, u32 data bytes, then the
 * control section (at least up to +0x720, plus the assembly text if any) and the code section (header + code + constants). */
static void dump_stage_all(int serial, int index, uint64_t control, uint64_t data) {
    static const char *dir;
    static volatile LONG init;
    if (!init) { dir = getenv("SSBU_SHADERDUMP_ALL"); InterlockedExchange(&init, 1); }
    if (!dir || !control) return;
    uint32_t text_off = rd32(control + 0x14), text_len = rd32(control + 0x18), ctl_len = 0x720, data_len = 0;
    if (text_len && text_len < (1u << 20) && text_off < (1u << 20) && text_off + text_len > ctl_len) ctl_len = text_off + text_len;
    { uint32_t tail = rd32(control + 0x20); if (tail && tail < (1u << 20) && tail + 0x400 > ctl_len) ctl_len = tail + 0x400; }      /* the block after the text */
    if (data) {
        uint32_t code_len = rd32(control + 0x6f8), c_size = rd32(control + 0x6fc), c_off = rd32(control + 0x700), total = rd32(control + 0x704);
        if (code_len < (4u << 20)) data_len = 0x30 + code_len;
        if (c_off < (4u << 20) && c_size < (1u << 20) && c_off + c_size > data_len) data_len = c_off + c_size;
        if (total > data_len && total < (8u << 20)) data_len = total;
    }
    char path[300];
    snprintf(path, sizeof path, "%s/s%05d_%d.mxd", dir, serial, index);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t head[5] = {0x3144584du, (uint32_t)serial, (uint32_t)index, ctl_len, data_len};
    fwrite(head, 4, 5, f);
    fwrite(P(control), 1, ctl_len, f);
    if (data_len) fwrite(P(data), 1, data_len, f);
    fclose(f);
}

/* nvnProgramSetShaders, guest thread (P3 round 4): one job that makes the program's vertex and fragment stage ready (binaries
 * compiled when missing) and has the driver do its first-draw work for the pair. Returns 0 when it does not apply (the
 * stages are then handed to the workers one by one, as before). After start-up the caller is held until the workers
 * have nothing left, so a fighter or stage that is loaded is ready when its loading ends. */
static int pw_queue_prog(Prog *p) {
    int m = pw_mode();
    Stage *v = &p->st[0], *f = &p->st[1];
    if (!m || g_shd_mode <= 0 || !g_shw_n || g_shw_ctx_failed || (m < 2 && g_st.frames < 300) || GetCurrentThreadId() == g_render_tid) return 0;
    /* 2026-10-30: with the shader store every stage is translated already (glsl_state != 0), and this returned 0: no pair was
     * warmed at play ("0 programs handed over" in the 2026-10-29 session, where 651 of 850 pairs then stalled their first
     * draw, 26 ms on average, 16.8 s in all: the driver's disk cache no longer held the pairs the preparation had warmed).
     * Now a program handed over is warmed whatever the state of its stages: with a warm driver cache that is 2 ms per
     * pair on a worker, and when the cache is cold the work is loading time instead of a stall mid-frame. */
    if (!(v->code && v->code_len > 0) || p->st[2].state || p->st[3].state || p->st[4].state) return 0;
    int has_f = f->code && f->code_len > 0;
    if (f->state && !has_f) return 0;
    uint32_t vc = v->cdata_copy ? v->ctl[3] : 0, fc = has_f && f->cdata_copy ? f->ctl[3] : 0;
    size_t need = (size_t)v->code_len + vc + (has_f ? (size_t)f->code_len + fc : 0);
    ShJob *j = calloc(1, sizeof *j + need);
    uint8_t *d = j->data;
    j->stage = 3; j->code_len = v->code_len; j->cdata_size = vc; j->key[0] = has_f ? (uint32_t)f->code_len : 0; j->key[1] = fc;
    memcpy(d, v->code, (size_t)v->code_len); d += v->code_len;
    if (vc) { memcpy(d, v->cdata_copy, vc); d += vc; }
    if (has_f) { memcpy(d, f->code, (size_t)f->code_len); d += f->code_len; if (fc) memcpy(d, f->cdata_copy, fc); }
    uint64_t h = sh_hash(0x5041495257ULL ^ (uint64_t)j->code_len ^ ((uint64_t)j->key[0] << 32), j->data, need);
    if (!h) h = 1;
    memcpy(&j->key[2], &h, 8);
    int seen = 0;
    EnterCriticalSection(&g_shw_lock);
    unsigned k = (unsigned)(h >> 20) & 16383;
    for (int probe = 0; probe < 64; probe++, k = (k + 1) & 16383) { if (g_pw_qseen[k] == h) { seen = 1; break; } if (!g_pw_qseen[k]) { g_pw_qseen[k] = h; break; } }
    LeaveCriticalSection(&g_shw_lock);
    if (seen) free(j); else { InterlockedIncrement64(&g_pw_progs); pw_enqueue(j); }
    static int nowait = -1;
    if (nowait < 0) nowait = getenv("SSBU_SHADER_NOWAIT") != NULL;
    if (!nowait && g_st.frames >= 300 && g_shw_queued != g_shw_done) {
        LONG64 t0 = pf_now(), freq = 0;
        { LARGE_INTEGER q; QueryPerformanceFrequency(&q); freq = q.QuadPart; }
        while (g_shw_queued != g_shw_done && pf_now() - t0 < 30 * freq) Sleep(1);
        InterlockedExchangeAdd64(&g_shw_held_ticks, pf_now() - t0); InterlockedIncrement64(&g_shw_held);
    }
    return 1;
}
/* nvnMemoryPoolInitialize (nvn_stub.c), guest thread. The game lays a pool over the code section of each "BNSH" shader
 * container when it sets the container up (loading), long before it makes the first program of it: the Vulkan back end
 * scans the container then (nvn_vk_m4.c). */
void nvn_front_pool_made(uint64_t memory, uint64_t size) { if (g_rb == &rb_vk && memory && size >= 0x40 && (memory >> 28) != 0x710 && rd32(memory) == 0x12345678u) nvn_vk_container_seen(memory, memory); }      /* such a pool begins with the code section of a stage (its magic); a search below every other pool cost 0.2 s each (measured) */
/* The constant block of a stage (ctl[3] bytes at ctl[4] of the shader data: the compiler's constants and BRX jump tables)
 * is copied when the program is set: the game reuses that memory afterwards (program #5401's jump table read as zeros at
 * draw time, and its vertex shader looped for ever: a GPU reset ended the process). */
static void stage_keep_cdata(Stage *st, const uint8_t *base) {        /* base: the shader data section (ctl[4] is relative to it) */
    free(st->cdata_copy);
    st->cdata_copy = NULL;
    if (!st->code || !base || !st->ctl[3] || !st->ctl[4] || st->ctl[3] > (1u << 20)) return;
    st->cdata_copy = malloc(st->ctl[3]);
    if (st->cdata_copy) memcpy(st->cdata_copy, base + st->ctl[4], st->ctl[3]);
}
/* nvnProgramSetShaders(program, count, const NVNshaderData *): { u64 data (GPU address), const void *control }.
 * control: magic 0x98761234, offset of the NV_gpu_program5 text at +0x14, its length at +0x18. */
__declspec(thread) long long nvn_setsh_part[8];
static void prog_set_shaders_(Ctx *c);
static void prog_set_shaders(Ctx *c) {             /* session 8: the call timed part by part; a line for every call over 2 ms (SSBU_VK_SETSH_LOG=<ms> changes the limit, 0 = no lines) */
    static int lim = -1; static LONG nl; static LONG64 fq;
    if (lim < 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fq = f.QuadPart; lim = getenv("SSBU_VK_SETSH_LOG") ? atoi(getenv("SSBU_VK_SETSH_LOG")) : 2; }
    if (!lim || g_rb != &rb_vk) { prog_set_shaders_(c); return; }
    memset(nvn_setsh_part, 0, sizeof nvn_setsh_part);
    uint64_t guest = A0, ra = c->x[30];
    LONG64 t0 = pf_now();
    prog_set_shaders_(c);
    LONG64 t = pf_now() - t0;
    if (t * 1000 > (LONG64)lim * fq && (InterlockedIncrement(&nl) <= 400 || (t * 1000 > 20 * fq && nl <= 4000))) {      /* session 10: a call over 20 ms is logged after the first 400 lines too (Intel: the lines were used up at loading) */
        long long *q = nvn_setsh_part;
#define SETSH_MS(x) ((double)(x) * 1000.0 / (double)fq)
        printf("nvn_setsh: frame %lld, program object %llx (called from %llx): %.1f ms | barrier %.1f | container %.1f (search %.1f, copy or walk %.1f) | program_set %.1f (hash + job copy %.1f, lock %.1f, HELD %.1f) | rest %.1f\n",
               nvn_front_frame_next(), (unsigned long long)guest, (unsigned long long)ra, SETSH_MS(t), SETSH_MS(q[0]), SETSH_MS(q[1]), SETSH_MS(q[2]), SETSH_MS(q[3]), SETSH_MS(q[4]), SETSH_MS(q[5]), SETSH_MS(q[6]), SETSH_MS(q[7]), SETSH_MS(t - q[0] - q[1] - q[4]));
        fflush(stdout);
    }
}
static void prog_set_shaders_(Ctx *c) {
    static int nlog, nlog2;
    Prog *p = hobj(A0, K_PROG);
    /* Only a program set again waits for the commands queued before (they read its stages when they run); one that has no
     * stages yet (just initialized) is drawn by none of them: the game's job threads waited 30-70 ms for the render thread
     * here at each scene load (2026-10-31). */
    int had = 0;
    for (int s = 0; p && s < 6; s++) had |= p->st[s].state != 0;
    if (had) { LONG64 tb = pf_now(); gl_barrier(); nvn_setsh_part[0] += pf_now() - tb; }
    int count = (int)A1;
    uint64_t stride = strstr(hostfn_current_name(), "Ext") ? 24 : 16;      /* the Ext variant has a third pointer per shader */
    InterlockedIncrement(&g_setshaders_calls);
    if (!p) { p = hnew(A0, K_PROG, sizeof(Prog)); p->serial = (int)InterlockedIncrement(&g_prog_serial); p->guest = A0; }
    for (int s = 0; s < 6; s++) if (p->st[s].state == 1 || p->st[s].state == 4) { free(p->st[s].text); p->st[s].text = NULL; p->st[s].state = 0; }
    for (int i = 0; i < count && i < 6 && A2; i++) {
        uint64_t control = rd64(A2 + (uint64_t)i * stride + 8);
        if (!control || rd32(control) != 0x98761234u) { logf_limited(&nlog, 10, "nvn_gl: %s: shader %d of program #%d has no recognisable control section\n", hostfn_current_name(), i, p->serial); continue; }
        uint64_t data = rd64(A2 + (uint64_t)i * stride);
        dump_stage_all(p->serial, i, control, data);
        /* V1 session 6: a program made by FUN_71000382a0 (13.0.4; the caller's return address lies in it) comes from a variation
         * of a "BNSH" container; that function keeps its third argument, the variation's program inside the file, in x20 */
        if (g_rb == &rb_vk && i == 0 && g_game_build != GAME_OTHER && c->x[30] - GA(0x71000382a0ull) < 2884) {   /* SSBU's program set-up function */ LONG64 tb = pf_now(); nvn_vk_container_seen(control, c->x[20]); nvn_setsh_part[1] += pf_now() - tb; }
        {   static int diag = -1; static LONG nd;      /* SSBU_SHADER_SRC_LOG=<n>: where the first n heap-held stages come from (guest return addresses, the memory around the control section) */
            if (diag < 0) diag = getenv("SSBU_SHADER_SRC_LOG") ? atoi(getenv("SSBU_SHADER_SRC_LOG")) : 0;
            if (diag && (control >> 28) != 0x710 && InterlockedIncrement(&nd) <= diag) {
                uint64_t fp = c->x[29], ra1 = fp ? rd64(fp + 8) : 0, fp2 = fp ? rd64(fp) : 0, ra2 = fp2 ? rd64(fp2 + 8) : 0, fp3 = fp2 ? rd64(fp2) : 0, ra3 = fp3 ? rd64(fp3 + 8) : 0, pg = control & ~0xffffull;
                printf("shader-src: frame %lld program #%d stage %d of %d: control %llx data %llx (third pointer %llx) | called from %llx <- %llx <- %llx <- %llx | 64K base: %08x %08x %08x %08x %08x %08x %08x %08x | before control: %016llx %016llx %016llx %016llx | data: %08x %08x %08x %08x\n", nvn_front_frame_next(), p->serial, i, count,
                       (unsigned long long)control, (unsigned long long)data, (unsigned long long)(stride == 24 ? rd64(A2 + (uint64_t)i * stride + 16) : 0), (unsigned long long)c->x[30], (unsigned long long)ra1, (unsigned long long)ra2, (unsigned long long)ra3,
                       rd32(pg), rd32(pg + 4), rd32(pg + 8), rd32(pg + 12), rd32(pg + 16), rd32(pg + 20), rd32(pg + 24), rd32(pg + 28),
                       (unsigned long long)rd64(control - 32), (unsigned long long)rd64(control - 24), (unsigned long long)rd64(control - 16), (unsigned long long)rd64(control - 8), data ? rd32(data) : 0, data ? rd32(data + 4) : 0, data ? rd32(data + 8) : 0, data ? rd32(data + 12) : 0);
                fflush(stdout);
            }
        }
        static const int8_t stage_of_type[16] = {-1, 0, 3, 4, 2, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        if (rd32(control + 0x18)) {
            InterlockedIncrement(&g_with_text);
            prog_add_stage(p, P(control + rd32(control + 0x14)), (int)rd32(control + 0x18), p->serial);
            if (data && rd32(data) == 0x12345678u) {                  /* the machine code is there as well */
                int s2 = stage_of_type[(rd32(data + 0x30) >> 10) & 15], len2 = (int)rd32(control + 0x6f8);
                if (s2 >= 0 && s2 < 2 && p->st[s2].state == 1 && len2 > 0 && len2 < (4 << 20)) { p->st[s2].code = P(data + 0x30); p->st[s2].code_len = len2; p->st[s2].glsl_state = 0; memcpy(p->st[s2].ctl, P(control + 0x6f0), 32); stage_keep_cdata(&p->st[s2], p->st[s2].code - 0x30); }
            }
            /* V1 M3: a compute program with assembly text (the five built into the exe) also carries its machine code; the
             * Vulkan back end runs that (no NV assembly there), OpenGL only with SSBU_COMPUTE_MC=1 (translator check). */
            if (rd32(control + 0x714) == 5 && data && p->st[5].state == 1) {
                static int ncl;
                int len = (int)rd32(control + 0x6f8);
                logf_limited(&ncl, 8, "nvn_gl: program #%d: compute stage with assembly text and %d bytes of machine code: %08x %08x %08x %08x, work group %u x %u x %u\n", p->serial, len,
                             rd32(data), rd32(data + 4), rd32(data + 8), rd32(data + 12), rd32(control + 0x748), rd32(control + 0x74c), rd32(control + 0x750));
                for (int k = 0; k < 3; k++) p->st[5].local[k] = rd32(control + 0x748 + (uint64_t)k * 4);
                p->st[5].smem_bytes = rd32(control + 0x754); p->st[5].lmem_bytes = rd32(control + 0x758);
                if (len > 0 && len < (4 << 20)) { p->st[5].code = P(data + (rd32(data) == 0x12345679u ? (rd32(control + 0x6f0) ? rd32(control + 0x6f0) : 0x100) : 0)); p->st[5].code_len = len; p->st[5].glsl_state = 0; memcpy(p->st[5].ctl, P(control + 0x6f0), 32); stage_keep_cdata(&p->st[5], P(data)); }
            }
            continue;
        }
        /* No assembly text: only Maxwell machine code. data: magic 0x12345678, program header (0x50 bytes) at +0x30, then 64-bit
         * instruction words; control +0x6f8 = size of the instructions in bytes. Header word 0 bits 10..13 = stage (1 vertex,
         * 2 tess control, 3 tess evaluation, 4 geometry, 5 fragment). */
        InterlockedIncrement(&g_without_text);
        if (rd32(control + 0x714) == 5 && data) {                     /* compute stage (control +0x714 = NVN shader stage) */
            static int ncl;
            int len = (int)rd32(control + 0x6f8);
            logf_limited(&ncl, 40, "nvn_gl: program #%d: compute stage as machine code, %d bytes, data %llx: %08x %08x %08x %08x | +0x30: %08x %08x %08x %08x\n", p->serial, len, (unsigned long long)data,
                         rd32(data), rd32(data + 4), rd32(data + 8), rd32(data + 12), rd32(data + 0x30), rd32(data + 0x34), rd32(data + 0x38), rd32(data + 0x3c));
            {   static int ndump;
                if (len < 2048 && ndump++ < 3) {
                    printf("nvn_gl: program #%d compute (%d bytes): control +0x6f0:", p->serial, len);
                    for (int k = 0; k < 16; k++) printf(" %u", rd32(control + 0x6f0 + (uint64_t)k * 4));
                    printf("\n   data +0..+0x60:"); for (int k = 0; k < 0x60; k += 4) printf("%s%08x", k % 0x20 ? " " : "\n   ", rd32(data + k));
                    printf("\n   data +0x100..+0x340:"); for (int k = 0x100; k < 0x340; k += 4) printf("%s%08x", k % 0x20 ? " " : "\n   ", rd32(data + k));
                    printf("\n");
                }
            }
            for (int k = 0; k < 3; k++) p->st[5].local[k] = rd32(control + 0x748 + (uint64_t)k * 4);      /* work group size */
            p->st[5].smem_bytes = rd32(control + 0x754); p->st[5].lmem_bytes = rd32(control + 0x758);
            if (len > 0 && len < (4 << 20)) { p->st[5].code = P(data + (rd32(data) == 0x12345679u ? (rd32(control + 0x6f0) ? rd32(control + 0x6f0) : 0x100) : 0)); p->st[5].code_len = len; p->st[5].state = 4; p->st[5].dumped = 0; p->st[5].glsl_state = 0; memcpy(p->st[5].ctl, P(control + 0x6f0), 32); stage_keep_cdata(&p->st[5], P(data)); }
            continue;
        }
        if (!data || rd32(data) != 0x12345678u) { logf_limited(&nlog2, 10, "nvn_gl: program #%d shader %d: no text and no recognisable code section\n", p->serial, i); continue; }
        int s = stage_of_type[(rd32(data + 0x30) >> 10) & 15], len = (int)rd32(control + 0x6f8);
        if (s < 0 || len <= 0 || len > (4 << 20)) { logf_limited(&nlog2, 10, "nvn_gl: program #%d shader %d: unexpected program header %08x, code size %d\n", p->serial, i, rd32(data + 0x30), len); continue; }
        p->st[s].code = P(data + 0x30); p->st[s].code_len = len; p->st[s].state = 4; p->st[s].dumped = 0; p->st[s].glsl_state = 0; memcpy(p->st[s].ctl, P(control + 0x6f0), 32); stage_keep_cdata(&p->st[s], p->st[s].code - 0x30);
    }
    if (g_rb == &rb_vk) { LONG64 tb = pf_now(); nvn_vk_program_set(p); nvn_setsh_part[4] += pf_now() - tb; RET(1); return; }       /* V1 M4: the stages go to the Vulkan workers at once (nvn_vk_m4.c) */
    if (gputime_ms()) for (int s = 0; s < 2; s++) {      /* TOMO_GPUTIME: where a stage's constant data (jump tables) lies in the shader data */
        Stage *st = &p->st[s];
        static int nlog;
        if (!st->code || st->ctl[3] < 16 || !st->ctl[4] || p->serial < 5380) continue;   /* only stages with a table-sized constant block */
        const uint32_t *d = (const uint32_t *)(st->code - 0x30), *cd = (const uint32_t *)(st->code - 0x30 + st->ctl[4]);
        logf_limited(&nlog, 60, "gputime: program #%d stage %d set: code %p len %d ctl %u %u %u %u %u %u %u %u; data+ctl[4] (%u): %08x %08x %08x %08x %08x %08x; data+0x30+len: %08x %08x %08x %08x %08x %08x\n",
                     p->serial, s, (void *)st->code, st->code_len, st->ctl[0], st->ctl[1], st->ctl[2], st->ctl[3], st->ctl[4], st->ctl[5], st->ctl[6], st->ctl[7], st->ctl[4],
                     cd[0], cd[1], cd[2], cd[3], cd[4], cd[5], d[(0x30 + st->code_len) / 4], d[(0x30 + st->code_len) / 4 + 1], d[(0x30 + st->code_len) / 4 + 2],
                     d[(0x30 + st->code_len) / 4 + 3], d[(0x30 + st->code_len) / 4 + 4], d[(0x30 + st->code_len) / 4 + 5]);
        fflush(stdout);
    }
    if (!pw_queue_prog(p))
    for (int s = 0; s < 2; s++) {              /* P3: have the driver's binary ready before the first draw */
        Stage *st = &p->st[s];
        if (st->code && st->code_len > 0 && st->glsl_state == 0)
            shd_prewarm(s, st->code, st->code_len, st->cdata_copy, st->cdata_copy ? st->ctl[3] : 0);
    }
    RET(1);
}

/* ---- samplers */
static void smpb_defaults(Ctx *c) {
    SamplerParams p;
    memset(&p, 0, sizeof p);
    p.min = 1; p.mag = 1; p.ws = p.wt = p.wr = 7; p.lodmax = 1000.0f; p.aniso = 1.0f; p.cfunc = 2;
    memcpy(P(A0), &p, sizeof p);
}
#define SMP ((SamplerParams *)P(A0))
static void smpb_minmag(Ctx *c) { SMP->min = (uint8_t)A1; SMP->mag = (uint8_t)A2; }
static void smpb_wrap(Ctx *c) { SMP->ws = (uint8_t)A1; SMP->wt = (uint8_t)A2; SMP->wr = (uint8_t)A3; }
static void smpb_lodclamp(Ctx *c) { SMP->lodmin = F0; SMP->lodmax = F1; }
static void smpb_lodbias(Ctx *c) { SMP->bias = F0; }
static void smpb_compare(Ctx *c) { SMP->cmode = (uint8_t)A1; SMP->cfunc = (uint8_t)A2; }
static void smpb_border(Ctx *c) { if (A1) memcpy(SMP->border, P(A1), 16); }
static void smpb_aniso(Ctx *c) { SMP->aniso = F0; }
static void smpb_reduction(Ctx *c) { SMP->reduction = (uint8_t)A1; }
static void smp_initialize(Ctx *c) { memcpy(P(A0), P(A1), sizeof(SamplerParams)); RET(1); }
static void spool_initialize(Ctx *c) {                 /* (pool, memoryPool, offset, numDescriptors) */
    SamplerPool *sp = hnew(A0, K_SPOOL, sizeof *sp);
    sp->count = (int)A3 > 0 ? (int)A3 : 0; sp->mempool = A1; sp->offset = A2;
    sp->e = calloc((size_t)sp->count + 1, sizeof *sp->e);
    sp->used = calloc((size_t)sp->count + 1, 1);
    RET(1);
}
static void spool_register(Ctx *c) {                   /* (pool, id, sampler) or (pool, id, samplerBuilder): same contents */
    InterlockedIncrement(&g_pool_gen); g_sgen[A1 & 0xfff]++;
    if (g_rb == &rb_vk) gl_barrier();                    /* OpenGL: the table is read under its lock when a command runs, as the GPU reads it on the console */
    SamplerPool *sp = hobj(A0, K_SPOOL);
    if (!sp || (uint32_t)A1 >= (uint32_t)sp->count || !A2) return;
    AcquireSRWLockExclusive(&g_pool_lock);
    memcpy(&sp->e[(uint32_t)A1], P(A2), sizeof(SamplerParams));
    sp->used[(uint32_t)A1] = 1;
    ReleaseSRWLockExclusive(&g_pool_lock);
}

/* ---- textures */
#define TB ((TexBuilder *)P(A0))
static void texb_defaults(Ctx *c) {
    TexBuilder b;
    memset(&b, 0, sizeof b);
    b.target = 1; b.levels = 1;                        /* [sdk 0x427610]: target 1, size 0 x 0 x 0, 1 level, format 0, 0 samples, swizzle 2 3 4 5 */
    b.swz[0] = 2; b.swz[1] = 3; b.swz[2] = 4; b.swz[3] = 5;
    memcpy(P(A0), &b, sizeof b);
}
static void texb_flags(Ctx *c) { TB->flags = (int)A1; }
static void texb_target(Ctx *c) { TB->target = (int)A1; }
static void texb_format(Ctx *c) { TB->format = (int)A1; }
static void texb_width(Ctx *c) { TB->width = (int)A1; }
static void texb_height(Ctx *c) { TB->height = (int)A1; }
static void texb_depth(Ctx *c) { TB->depth = (int)A1; }
static void texb_size1d(Ctx *c) { TB->width = (int)A1; TB->height = 1; TB->depth = 1; }     /* [sdk 0x4276a0] */
static void texb_size2d(Ctx *c) { TB->width = (int)A1; TB->height = (int)A2; TB->depth = 1; }     /* [sdk 0x4276b0] */
static void texb_size3d(Ctx *c) { TB->width = (int)A1; TB->height = (int)A2; TB->depth = (int)A3; }
static void texb_levels(Ctx *c) { TB->levels = (int)A1; }
static void texb_samples(Ctx *c) { TB->samples = (int)A1; }
static void texb_swizzle(Ctx *c) { TB->swz[0] = (uint8_t)A1; TB->swz[1] = (uint8_t)A2; TB->swz[2] = (uint8_t)A3; TB->swz[3] = (uint8_t)A4; }
static void texb_dsmode(Ctx *c) { TB->dsmode = (int)A1; }
static void texb_storage(Ctx *c) { TB->pool = A1; TB->offset = A2; }
static void texb_packaged(Ctx *c) { TB->packaged = A1; }
static void texb_stride(Ctx *c) { TB->stride = (int64_t)A1; }
static void texb_layout(Ctx *c) {
    static int nlog;
    if (!A1) return;
    memcpy(TB->layout, P(A1), 8);
    TB->has_layout = 1;
    Fmt f;
    int hb = fmt_get(TB->format, &f) ? (TB->height + f.bh - 1) / f.bh : TB->height;
    logf_limited(&nlog, 40, "nvn_gl: packaged texture layout %02x %02x %02x %02x %02x %02x %02x %02x for %dx%dx%d format 0x%x target %d levels %d (block height by rule: %d)\n", TB->layout[0], TB->layout[1],
                 TB->layout[2], TB->layout[3], TB->layout[4], TB->layout[5], TB->layout[6], TB->layout[7], TB->width, TB->height, TB->depth, TB->format, TB->target, TB->levels, 1 << tex_rule_block_height_log2(hb));
}
/* Storage size and alignment are the SDK's own code (nvn_sdk.c: [sdk 0x428760] and [sdk 0x4278b0] lifted, with the GPU
 * memory-kind choice they end in). The size this file's own layout needs (tex_storage_size) is only compared with it. */
static void texb_sdk(const TexBuilder *b, NvnSdkTexBuilder *r) {
    memset(r, 0, sizeof *r);
    r->flags = (uint32_t)b->flags; r->target = (uint32_t)b->target; r->width = (uint32_t)b->width; r->height = (uint32_t)b->height; r->depth = (uint32_t)b->depth;
    r->levels = (uint32_t)b->levels; r->format = (uint32_t)b->format; r->samples = (uint32_t)b->samples; r->stride = (uint32_t)b->stride; r->glname = b->glname;
    r->has_layout = (uint32_t)b->has_layout; memcpy(&r->layout, b->layout, 4);
}
/* Tomodachi Life (SDK 20, GAME_OTHER): nvn_sdk.c models SSBU's 13.x NVN internals only, so this game gets this file's own
 * layout: its size rounded to 512 bytes, alignment 512, storage class 0, no sparse layout. Not rounded to a 64 KB page:
 * the game carves texture storage out of fixed-size heaps (MiiBodyBaseModel ran out with 64 KB pages). */
#define OWN_LAYOUT (g_game_build == GAME_OTHER)
static uint64_t own_size(const TexBuilder *b) { return ((uint64_t)tex_storage_size(b) + 0x1ff) & ~0x1ffull; }
static void texb_storage_size(Ctx *c) {
    static int nlog;
    TexBuilder b = *TB;
    NvnSdkTexBuilder r;
    if (OWN_LAYOUT) { RET(own_size(&b)); return; }
    texb_sdk(&b, &r);
    uint64_t real = nvn_sdk_texture_storage_size(c, &r);
    size_t own = nvn_sdk_format_known(r.format) && !b.glname ? tex_storage_size(&b) : 0;
    if (own > real) logf_limited(&nlog, 40, "nvn_gl: texture layout of this file needs 0x%llx bytes, the SDK's storage size is 0x%llx (%dx%dx%d format 0x%x target %d levels %d samples %d flags 0x%x)\n",
                                 (unsigned long long)own, (unsigned long long)real, b.width, b.height, b.depth, b.format, b.target, b.levels, b.samples, b.flags);
    RET(real);
}
static void texb_storage_align(Ctx *c) {
    NvnSdkTexBuilder r;
    if (OWN_LAYOUT) { RET(0x200); return; }
    texb_sdk(TB, &r);
    RET(nvn_sdk_texture_storage_alignment(c, &r));
}
static void texb_get_flags(Ctx *c) { RET(TB->flags); }
static void texb_get_target(Ctx *c) { RET(TB->target); }
static void texb_get_format(Ctx *c) { RET(TB->format); }
static void texb_get_width(Ctx *c) { RET(TB->width); }
static void texb_get_height(Ctx *c) { RET(TB->height); }
static void texb_get_depth(Ctx *c) { RET(TB->depth); }
static void texb_get_levels(Ctx *c) { RET(TB->levels); }
static void texb_get_samples(Ctx *c) { RET(TB->samples); }

static void tex_initialize(Ctx *c) {                   /* (texture, builder) */
    Tex *t = hnew(A0, K_TEX, sizeof *t);
    uint64_t mem = 0, size = 0;
    memcpy(&t->b, P(A1), sizeof t->b);
    if (t->b.pool && nvn_pool_range(t->b.pool, &mem, &size) && mem) { t->mem = (uint8_t *)P(mem) + t->b.offset; t->limit = (uint8_t *)P(mem) + size; }
    t->storage = tex_storage_size2(&t->b, &t->data_size);
    {   /* CPU_NO_ACCESS pool: only the GPU writes it, nothing to upload or hash (the game makes its render targets anew each
         * frame). Not a block-compressed format: no GPU renders those, their texels are written by the CPU through a physical
         * pool a virtual one aliases (the texture streaming threads), or by a copy (which marks the texture itself). */
        Fmt f0;
        const TexFormat *tf0 = tex_format(t->b.format);
        int compressed = (fmt_get(t->b.format, &f0) && f0.kind == 4) || (tf0 && tf0->sw_decode);
        uint64_t pf = t->b.pool ? nvn_pool_flags_of(t->b.pool) : 0;
        if ((pf & 1) && !t->b.packaged && !compressed) t->gpu_written = 1;
        /* Not a COMPRESSIBLE pool (render targets live in those) and its memory already holds texels: a texture loaded from a
         * file (the X menu's fluid mask XMenuIn24^d, the layouts' White8_00^d: R8 in pools 0x21). Measured 2026-10-25 on the
         * island: all 47 such textures held data, every render target zeros. Marked rendered, they stayed empty. */
        if (t->gpu_written && !(pf & 0x80) && t->mem && t->limit) {
            size_t n = t->data_size < 65536 ? t->data_size : 65536;
            if (t->mem + n > t->limit) n = 0;
            for (size_t i = 0; i < n; i++) if (t->mem[i]) { t->gpu_written = 0; break; }
        }
    }
    if (!tex_capture_packaged(t)) {                /* PR #41, changed: no copy = the old behaviour (the game's own storage is read) */
        static int said;
        if (said++ < 5) printf("nvn: packaged texture %dx%d format 0x%x: its source %llx + %llu could not be copied: read in place as before\n",
                               t->b.width, t->b.height, t->b.format, (unsigned long long)t->b.packaged, (unsigned long long)t->data_size);
    }
    t->guest = A0;
    t->born = g_st.frames;
    tex_range_add(t);
    do { t->all_next = g_all_tex; } while (InterlockedCompareExchangePointer((void *volatile *)&g_all_tex, t, t->all_next) != t->all_next);
    RET(1);
}
static void tex_finalize(Ctx *c) {
    if (g_rb == &rb_vk) gl_barrier();                    /* OpenGL: the deferred free after the queued commands is enough (see gl_housekeeping) */
    Tex *t = hobj(A0, K_TEX);
    if (!t) return;
    if (!t->dead) tex_range_remove(t);
    t->dead_frame = g_st.frames;
    InterlockedExchange(&t->dead, 1);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
    defer_free(K_TEX, t);
}
#define TEXGET(name, expr) static void name(Ctx *c) { Tex *t = hobj(A0, K_TEX); RET(t ? (expr) : 0); }
TEXGET(tex_get_flags, t->b.flags)
TEXGET(tex_get_target, t->b.target)
TEXGET(tex_get_format, t->b.format)
TEXGET(tex_get_width, t->b.width)
TEXGET(tex_get_height, t->b.height)
TEXGET(tex_get_depth, t->b.depth)
TEXGET(tex_get_levels, t->b.levels)
TEXGET(tex_get_samples, t->b.samples)
TEXGET(tex_get_stride, t->b.stride)
static void tex_get_storage_size(Ctx *c);       /* below: the SDK's value [sdk 0x429720], not this file's own layout size (t->storage) */
TEXGET(tex_get_memory_offset, t->b.offset)
TEXGET(tex_get_address, (uint64_t)(uintptr_t)t->mem)
TEXGET(tex_get_dsmode, t->b.dsmode)
/* Storage class (raw, and 0x100 | GPU memory kind) and sparse tile layout: the SDK's own code, lifted (nvn_sdk.c: [sdk 0x427a10],
 * [0x427b30], [0x428c00], [0x429870], [0x4290b0], [0x429150]). NVNtextureSparseTileLayout = the 16 bytes [sdk 0x411090] writes.
 * The texture forms run on the NVNtexture that nvnTextureInitialize [sdk 0x4287d0] would have built from this texture's builder. */
static void texb_storage_class(Ctx *c) { NvnSdkTexBuilder r; if (OWN_LAYOUT) { RET(0); return; } texb_sdk(TB, &r); RET(nvn_sdk_texture_storage_class(c, &r)); }
static void texb_raw_storage_class(Ctx *c) { NvnSdkTexBuilder r; if (OWN_LAYOUT) { RET(0); return; } texb_sdk(TB, &r); RET(nvn_sdk_texture_raw_storage_class(c, &r)); }
static void texb_sparse_layout(Ctx *c) { uint64_t out = A1; uint32_t v[4]; NvnSdkTexBuilder r; if (OWN_LAYOUT) { if (out) memset(P(out), 0, 16); return; } texb_sdk(TB, &r); nvn_sdk_texture_sparse_layout(c, &r, v); if (out) memcpy(P(out), v, 16); }
static int tex_sdk(Ctx *c, NvnSdkTexBuilder *r, const char *fn) {
    Tex *t = hobj(A0, K_TEX);
    if (t && !t->b.glname) { texb_sdk(&t->b, r); return 1; }
    printf("nvn: stopping: %s on %s: the SDK object cannot be modelled\n", fn, t ? "a texture made from an OpenGL texture name" : "a texture that was never initialized");
    fflush(stdout);
    report_and_exit(6);
    return 0;
}
static void tex_storage_class(Ctx *c) { NvnSdkTexBuilder r; uint32_t v = 0; if (OWN_LAYOUT) { RET(0); return; } if (tex_sdk(c, &r, "nvnTextureGetStorageClass")) nvn_sdk_tex_storage_class(c, &r, &v); RET(v); }
static void tex_raw_storage_class(Ctx *c) { NvnSdkTexBuilder r; uint32_t v = 0; if (OWN_LAYOUT) { RET(0); return; } if (tex_sdk(c, &r, "nvnTextureGetRawStorageClass")) nvn_sdk_tex_raw_storage_class(c, &r, &v); RET(v); }
/* nvnTextureGetStorageSize [sdk 0x429720] = the 32-bit word +0x78 of the NVNtexture = the SDK layout's size (nvn_sdk.c). Until close5
 * this answered t->storage, the size of this file's own layout, which is not rounded to the GPU page as the SDK's is. */
static void tex_get_storage_size(Ctx *c) { NvnSdkTexBuilder r; uint32_t v = 0; if (OWN_LAYOUT) { Tex *t = hobj(A0, K_TEX); RET(t ? own_size(&t->b) : 0); return; } if (tex_sdk(c, &r, "nvnTextureGetStorageSize")) nvn_sdk_tex_storage_size(c, &r, &v); RET(v); }
static void tex_sparse_layout(Ctx *c) { uint64_t out = A1; uint32_t v[4] = {0, 0, 0, 0}; NvnSdkTexBuilder r; if (OWN_LAYOUT) { if (out) memset(P(out), 0, 16); return; } if (tex_sdk(c, &r, "nvnTextureGetSparseTileLayout")) nvn_sdk_tex_sparse_layout(c, &r, v); if (out) memcpy(P(out), v, 16); }

static void tex_write_texels(Ctx *c) {                 /* (texture, view, region, data) [, rowStride, imageStride] */
    Tex *t = hobj(A0, K_TEX);
    WriteJob j;
    if (!t || !A2 || !A3) return;
    memset(&j, 0, sizeof j);
    j.tex = t; j.data = P(A3);
    if (A1) { memcpy(&j.view, P(A1), sizeof j.view); j.has_view = 1; }
    memcpy(j.region, P(A2), 24);
    if (strstr(hostfn_current_name(), "Strided")) j.row_stride = (int64_t)A4;
    gl_run(g_rb->write_texels, &j);
}
static void tex_flush_texels(Ctx *c) { Tex *t = hobj(A0, K_TEX); if (t) t->dirty = 1; }

#define TV ((View *)P(A0))
_Static_assert(sizeof(View) == 16, "View must fit the 16-byte NVNtextureView of NVN 55");
static void view_defaults(Ctx *c) { memset(P(A0), 0, sizeof(View)); TV->set = 0x80; }
static void view_levels(Ctx *c) { TV->lvl0 = (int16_t)A1; TV->nlvl = (int16_t)A2; TV->set |= 0x81; }
static void view_layers(Ctx *c) { TV->lay0 = (int16_t)A1; TV->nlay = (int16_t)A2; TV->set |= 0x82; }
static void view_format(Ctx *c) { TV->format = (uint16_t)A1; TV->set |= 0x84; }
static void view_swizzle(Ctx *c) { TV->swz[0] = (uint8_t)A1; TV->swz[1] = (uint8_t)A2; TV->swz[2] = (uint8_t)A3; TV->swz[3] = (uint8_t)A4; TV->set |= 0x88; }
static void view_dsmode(Ctx *c) { TV->set = (uint8_t)((TV->set & ~0x40) | (A1 ? 0x40 : 0) | 0x90); }
static void view_target(Ctx *c) { TV->target = (uint8_t)A1; TV->set |= 0xa0; }

static void tpool_initialize(Ctx *c) {                 /* (pool, memoryPool, offset, numDescriptors) */
    TexPool *tp = hnew(A0, K_TPOOL, sizeof *tp);
    tp->count = (int)A3 > 0 ? (int)A3 : 0; tp->mempool = A1; tp->offset = A2;
    tp->e = calloc((size_t)tp->count + 1, sizeof *tp->e);
    RET(1);
}
static void tpool_register(Ctx *c) {                   /* (pool, id, texture, view) */
    InterlockedIncrement(&g_pool_gen); g_tgen[A1 & 0xfffff]++;
    if (g_rb == &rb_vk) gl_barrier();
    TexPool *tp = hobj(A0, K_TPOOL);
    TexPoolEnt e;
    if (!tp || (uint32_t)A1 >= (uint32_t)tp->count) return;
    memset(&e, 0, sizeof e);
    e.tex = hobj(A2, K_TEX);
    if (A3) memcpy(&e.view, P(A3), sizeof e.view);
    AcquireSRWLockExclusive(&g_pool_lock);
    Tex *old = tp->e[(uint32_t)A1].tex;
    if (old != e.tex) { if (e.tex) InterlockedIncrement(&e.tex->pool_refs); if (old) InterlockedDecrement(&old->pool_refs); }
    tp->e[(uint32_t)A1] = e;
    ReleaseSRWLockExclusive(&g_pool_lock);
}

/* ---- small state objects: contents live in the guest object */
static void blend_func_set(Ctx *c) { wr8(A0 + 1, (uint8_t)A1); wr8(A0 + 2, (uint8_t)A2); wr8(A0 + 3, (uint8_t)A3); wr8(A0 + 4, (uint8_t)A4); }
static void blend_equation(Ctx *c) { wr8(A0 + 5, (uint8_t)A1); wr8(A0 + 6, (uint8_t)A2); }
static void color_defaults(Ctx *c) { static const uint8_t d[4] = {0, 3, 8, 0}; memcpy(P(A0), d, 4); }
static void color_blend_enable(Ctx *c) { uint8_t v = rd8(A0), bit = (uint8_t)(1u << (A1 & 7)); wr8(A0, (uint8_t)((A2 & 1) ? v | bit : v & ~bit)); }
static void color_logic_op(Ctx *c) { wr8(A0 + 1, (uint8_t)A1); }
static void color_alpha_test(Ctx *c) { wr8(A0 + 2, (uint8_t)A1); }
static void chanmask_defaults(Ctx *c) { wr32(A0, 0xffffffffu); }
static void chanmask_set(Ctx *c) {                     /* (state, index, r, g, b, a) */
    uint32_t v = rd32(A0), sh = ((uint32_t)A1 & 7) * 4;
    v = (v & ~(15u << sh)) | ((uint32_t)((A2 & 1) | (A3 & 1) << 1 | (A4 & 1) << 2 | (A5 & 1) << 3) << sh);
    wr32(A0, v);
}
static void poly_defaults(Ctx *c) { static const uint8_t d[4] = {0, 1, 2, 0}; memcpy(P(A0), d, 4); }
static void poly_cull(Ctx *c) { wr8(A0, (uint8_t)A1); }
static void poly_front(Ctx *c) { wr8(A0 + 1, (uint8_t)A1); }
static void poly_mode(Ctx *c) { wr8(A0 + 2, (uint8_t)A1); }
static void poly_offset(Ctx *c) {
    wr8(A0 + 3, (uint8_t)A1);
    { static int n, on = -1; if (on < 0) on = getenv("TOMO_POLYLOG") != NULL; if (on && A1 && n < 20) { n++; printf("polylog: nvnPolygonStateSetPolygonOffsetEnables(%llx, %llx)\n",(unsigned long long)A0, (unsigned long long)A1); } }
}
static void poly_get_cull(Ctx *c) { RET(rd8(A0)); }
static void poly_get_front(Ctx *c) { RET(rd8(A0 + 1)); }
static void poly_get_mode(Ctx *c) { RET(rd8(A0 + 2)); }
static void poly_get_offset(Ctx *c) { RET(rd8(A0 + 3)); }
/* depth-stencil: bit0 depth test, bit1 depth write, bit2 stencil test, 4..7 depth func, 8..11 / 12..15 stencil func front / back,
 * 16..27 stencil ops front (stencil fail, depth fail, depth pass), 28..39 the same for back */
static void ds_defaults(Ctx *c) { wr64(A0, 0x1111118822ULL); }
static void ds_bits(uint64_t a, int shift, uint64_t mask, uint64_t v) { wr64(a, (rd64(a) & ~(mask << shift)) | ((v & mask) << shift)); }
static void ds_depth_test(Ctx *c) { ds_bits(A0, 0, 1, A1); }
static void ds_depth_write(Ctx *c) { ds_bits(A0, 1, 1, A1); }
static void ds_stencil_test(Ctx *c) { ds_bits(A0, 2, 1, A1); }
static void ds_depth_func(Ctx *c) { ds_bits(A0, 4, 15, A1); }
static void ds_stencil_func(Ctx *c) { if (A1 & 1) ds_bits(A0, 8, 15, A2); if (A1 & 2) ds_bits(A0, 12, 15, A2); }
static void ds_stencil_op(Ctx *c) {                    /* (state, faces, stencilFail, depthFail, depthPass) */
    uint64_t v = (A2 & 15) | (A3 & 15) << 4 | (A4 & 15) << 8;
    if (A1 & 1) ds_bits(A0, 16, 0xfff, v);
    if (A1 & 2) ds_bits(A0, 28, 0xfff, v);
}
static void ds_get_depth_test(Ctx *c) { RET(rd64(A0) & 1); }
static void ds_get_depth_write(Ctx *c) { RET((rd64(A0) >> 1) & 1); }
static void ds_get_stencil_test(Ctx *c) { RET((rd64(A0) >> 2) & 1); }
static void ds_get_depth_func(Ctx *c) { RET((rd64(A0) >> 4) & 15); }
/* vertex attribute: bits 0..7 format (0 = disabled), 8..12 stream, 14..31 offset */
static void vattr_defaults(Ctx *c) { wr32(A0, 0); }
static void vattr_format(Ctx *c) { wr32(A0, (rd32(A0) & 0x1f00u) | ((uint32_t)A1 & 0xff) | ((uint32_t)A2 << 14)); }
static void vattr_stream(Ctx *c) { wr32(A0, (rd32(A0) & ~0x1f00u) | (((uint32_t)A1 & 31) << 8)); }
static void vstream_defaults(Ctx *c) { wr64(A0, 0); }
static void vstream_stride(Ctx *c) { wr32(A0, (uint32_t)A1); }
static void vstream_divisor(Ctx *c) { wr32(A0 + 4, (uint32_t)A1); }

/* ---- command buffers: recording */
static void cmd_initialize(Ctx *c) { cb_new(A0); RET(1); }
static void cmd_finalize(Ctx *c) { CmdBuf *cb = hobj(A0, K_CMD); if (cb) { cb_forget(cb); free(cb->data); free(cb); wr32(A0 + 8, 0); } }
static void cb_range_close(CmdBuf *cb) {         /* the part of the current block the recording took so far: lists recorded there before are gone */
    if (cb->cmd_pool && cb->cmd_used > cb->r_start) {
        list_invalidate(cb->cmd_pool, cb->cmd_off + cb->r_start, cb->cmd_used - cb->r_start);
        if (cb->r_n < 4) { cb->r_pool[cb->r_n] = cb->cmd_pool; cb->r_lo[cb->r_n] = cb->cmd_off + cb->r_start; cb->r_hi[cb->r_n] = cb->cmd_off + cb->cmd_used; cb->r_n++; }
    }
    cb->r_start = cb->cmd_used;
}
static void cmd_add_command_memory(Ctx *c) {          /* (cmdbuf, pool, offset, size): a new block replaces the current one. Not the whole block ends
                                                         * older lists: the game gives a window (0x200 bytes) and keeps only what the recording took of it (GetCommandMemoryUsed) */
    CmdBuf *cb = cb_get(A0);
    if (cb->recording) cb_range_close(cb);
    cb->cmd_pool = A1; cb->cmd_off = A2; cb->cmd_size = A3; cb->cmd_used = 0; cb->r_start = 0;
}
static void cmd_add_control_memory(Ctx *c) { CmdBuf *cb = cb_get(A0); cb->ctrl_size = A2; cb->ctrl_used = 0; }     /* (cmdbuf, memory, size) */
static void cmd_get_command_size(Ctx *c) { RET(cb_get(A0)->cmd_size); }
static void cmd_get_control_size(Ctx *c) { RET(cb_get(A0)->ctrl_size); }
static void cmd_is_recording(Ctx *c) { RET(cb_get(A0)->recording); }
/* L1 round 2, TEST ONLY (SSBU_L1_TRACE=1 with SSBU_L1=1): for the 5 presents after the first press, every command-buffer
 * recording, submission, fence, sync wait, window acquire and present, with the guest thread name and the guest return address. */
#ifdef _WIN32
int display_l1_trace(void); double display_l1_ms(void); const char *guest_thread_name_of(unsigned long tid);
#else
int display_l1_trace(void); double display_l1_ms(void); const char *guest_thread_name_of(DWORD tid);
#endif
#define L1T(...) do { if (display_l1_trace()) { char l1b_[240]; snprintf(l1b_, sizeof l1b_, __VA_ARGS__); printf("l1-trace: %+8.2f %-26s lr %09llx %s%c", display_l1_ms(), guest_thread_name_of(GetCurrentThreadId()) ? guest_thread_name_of(GetCurrentThreadId()) : "?", (unsigned long long)c->x[30], l1b_, 10); } } while (0)
static void cmd_begin(Ctx *c) { L1T("begin recording cb %llx", (unsigned long long)A0); CmdBuf *cb = cb_get(A0); cb->len = 0; cb->recording = 1; cb->rec_cmd = cb->rec_ctrl = 0; cb->dispatches = 0; cb->r_n = 0; cb->r_start = cb->cmd_used; }
static void cmd_end(Ctx *c) {
    CmdBuf *cb = cb_get(A0);
    cb->ctrl_used += 32; cb->rec_ctrl += 32;
    cb_range_close(cb);
    uint64_t h = list_register(cb->data, cb->len, (uint32_t)cb->rec_cmd, (uint32_t)cb->rec_ctrl, (uint32_t)cb->dispatches, cb);
    L1T("end recording cb %llx -> list %llx (%u bytes, %u dispatches)", (unsigned long long)A0, (unsigned long long)h, (unsigned)cb->len, (unsigned)cb->dispatches);
    cb->hint = cb->len < 0xffffffffu ? (uint32_t)cb->len : 0xffffffffu;
    cb->data = NULL; cb->len = cb->cap = 0; cb->recording = 0;
    RET(h);
}
static void cmd_call(Ctx *c) { uint64_t src = A2; int n = (int)A1; Cmd *m = rec(c, OP_CALL, (size_t)n * 8); memcpy(BLOB(m), P(src), (size_t)n * 8); }
static void cmd_texpool(Ctx *c) { rec(c, OP_TEXPOOL, 0)->a[0] = (uint64_t)(uintptr_t)hobj(A1, K_TPOOL); }
static void cmd_smppool(Ctx *c) { rec(c, OP_SMPPOOL, 0)->a[0] = (uint64_t)(uintptr_t)hobj(A1, K_SPOOL); }
static void cmd_program(Ctx *c) { rec(c, OP_PROGRAM, 0)->a[0] = (uint64_t)(uintptr_t)hobj(A1, K_PROG); }
static void rec_ranges(Ctx *c, uint32_t op, int single) {      /* (cmdbuf, stage, index, address, size) or (cmdbuf, stage, first, count, ranges) */
    int n = single ? 1 : (int)A3;
    uint64_t src = A4, addr = A3, size = A4;
    Cmd *m = rec(c, op, (size_t)n * 16);
    if (single) { m->a[2] = 1; memcpy(BLOB(m), &addr, 8); memcpy(BLOB(m) + 8, &size, 8); }
    else if (src) memcpy(BLOB(m), P(src), (size_t)n * 16);
    else memset(BLOB(m), 0, (size_t)n * 16);
}
/* NVN 55 nvnCommandBufferBind{UniformBuffers,StorageBuffers,Textures,Images}Deferred(cmdbuf, stage, first, count, address): like
 * the immediate forms, but the array at `address` is read when the commands execute, so the game can fill it after recording */
static void rec_deferred(Ctx *c, uint32_t op, size_t esz) { Cmd *m = rec(c, OP_DEFERRED, 0); m->a[3] = A4; m->a[5] = op; m->a[6] = esz; }
static void cmd_ubos_deferred(Ctx *c) { rec_deferred(c, OP_UBO, 16); }
static void cmd_ssbos_deferred(Ctx *c) { rec_deferred(c, OP_SSBO, 16); }
static void cmd_textures_deferred(Ctx *c) { rec_deferred(c, OP_TEXTURES, 8); }
static void cmd_images_deferred(Ctx *c) { rec_deferred(c, OP_IMAGES, 8); }
static void cmd_ubo(Ctx *c) { rec_ranges(c, OP_UBO, 1); }
static void cmd_ubos(Ctx *c) { rec_ranges(c, OP_UBO, 0); }
static void cmd_ssbo(Ctx *c) { rec_ranges(c, OP_SSBO, 1); }
static void cmd_ssbos(Ctx *c) { rec_ranges(c, OP_SSBO, 0); }
static void cmd_texture(Ctx *c) { uint64_t h = A3; Cmd *m = rec(c, OP_TEXTURES, 8); m->a[2] = 1; memcpy(BLOB(m), &h, 8); }   /* (cmdbuf, stage, index, handle) */
static void cmd_textures(Ctx *c) {                      /* (cmdbuf, stage, first, count, handles) */
    int n = (int)A3;
    uint64_t src = A4;
    Cmd *m = rec(c, OP_TEXTURES, (size_t)n * 8);
    if (src) memcpy(BLOB(m), P(src), (size_t)n * 8); else memset(BLOB(m), 0, (size_t)n * 8);
}
static void cmd_vbuf(Ctx *c) { uint64_t addr = A2, size = A3; Cmd *m = rec(c, OP_VBUF, 16); m->a[1] = 1; memcpy(BLOB(m), &addr, 8); memcpy(BLOB(m) + 8, &size, 8); }   /* (cmdbuf, index, address, size) */
static void cmd_vbufs(Ctx *c) {                         /* (cmdbuf, first, count, ranges) */
    int n = (int)A2;
    uint64_t src = A3;
    Cmd *m = rec(c, OP_VBUF, (size_t)n * 16);
    if (src) memcpy(BLOB(m), P(src), (size_t)n * 16); else memset(BLOB(m), 0, (size_t)n * 16);
}
static void cmd_vattr(Ctx *c) { int n = (int)A1; uint64_t src = A2; Cmd *m = rec(c, OP_VATTR, (size_t)n * 4); if (n) memcpy(BLOB(m), P(src), (size_t)n * 4); }       /* (cmdbuf, count, states) */
static void cmd_vstream(Ctx *c) { int n = (int)A1; uint64_t src = A2; Cmd *m = rec(c, OP_VSTREAM, (size_t)n * 8); if (n) memcpy(BLOB(m), P(src), (size_t)n * 8); }   /* (cmdbuf, count, states) */
static void rec_state(Ctx *c, uint32_t op, size_t size) { uint64_t src = A1; Cmd *m = rec(c, op, 0); m->a[0] = m->a[1] = m->a[2] = 0; if (src) memcpy(m->a, P(src), size); }
static void cmd_blend(Ctx *c) { rec_state(c, OP_BLEND, 8); }
static void cmd_color(Ctx *c) { rec_state(c, OP_COLOR, 4); }
static void cmd_chanmask(Ctx *c) { rec_state(c, OP_CHANMASK, 4); }
static void cmd_multisample(Ctx *c) { rec_state(c, OP_MULTISAMPLE, 0x18); }
static void cmd_polygon(Ctx *c) { rec_state(c, OP_POLYGON, 4); }
static void cmd_depthstencil(Ctx *c) { rec_state(c, OP_DEPTHSTENCIL, 8); }
static void cmd_viewport(Ctx *c) {                      /* (cmdbuf, x, y, w, h) */
    float v[4] = {(float)(int)A1, (float)(int)A2, (float)(int)A3, (float)(int)A4};
    Cmd *m = rec(c, OP_VIEWPORTS, 16);
    m->a[0] = 0; m->a[1] = 1; memcpy(BLOB(m), v, 16);
}
static void cmd_viewports(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_VIEWPORTS, (size_t)n * 16); memcpy(BLOB(m), P(src), (size_t)n * 16); }   /* (cmdbuf, first, count, float[4] each) */
static void cmd_scissor(Ctx *c) { int v[4] = {(int)A1, (int)A2, (int)A3, (int)A4}; Cmd *m = rec(c, OP_SCISSORS, 16); m->a[0] = 0; m->a[1] = 1; memcpy(BLOB(m), v, 16); }
static void cmd_scissors(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_SCISSORS, (size_t)n * 16); memcpy(BLOB(m), P(src), (size_t)n * 16); }
static void cmd_depth_range(Ctx *c) { float v[2] = {F0, F1}; Cmd *m = rec(c, OP_DEPTHRANGES, 8); m->a[0] = 0; m->a[1] = 1; memcpy(BLOB(m), v, 8); }
static void cmd_depth_ranges(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_DEPTHRANGES, (size_t)n * 8); memcpy(BLOB(m), P(src), (size_t)n * 8); }
static void cmd_targets(Ctx *c) {                       /* (cmdbuf, count, textures[], views[], depth, depthView) */
    int n = (int)A1 < 8 ? (int)A1 : 8;
    uint64_t texs = A2, views = A3, depth = A4, dview = A5;
    TargetRef r[9];
    memset(r, 0, sizeof r);
    for (int i = 0; i < n; i++) {
        uint64_t t = texs ? rd64(texs + (uint64_t)i * 8) : 0, v = views ? rd64(views + (uint64_t)i * 8) : 0;
        r[i].tex = hobj(t, K_TEX);
        if (v) memcpy(&r[i].view, P(v), sizeof(View));
    }
    r[8].tex = hobj(depth, K_TEX);
    if (dview) memcpy(&r[8].view, P(dview), sizeof(View));
    Cmd *m = rec(c, OP_TARGETS, sizeof r);
    m->a[0] = (uint64_t)n;
    memcpy(BLOB(m), r, sizeof r);
}
static void cmd_stencil_ref(Ctx *c) { rec(c, OP_STENCIL_REF, 0); }
static void cmd_stencil_vmask(Ctx *c) { rec(c, OP_STENCIL_VMASK, 0); }
static void cmd_stencil_mask(Ctx *c) { rec(c, OP_STENCIL_MASK, 0); }
static void cmd_blend_color(Ctx *c) { uint64_t src = A1; Cmd *m = rec(c, OP_BLENDCOLOR, 16); memcpy(BLOB(m), P(src), 16); }
static void rec_float(Ctx *c, uint32_t op) { float v[3] = {F0, F1, F2}; Cmd *m = rec(c, op, 0); memcpy(m->a, v, 12); }
static void cmd_alpha_ref(Ctx *c) { rec_float(c, OP_ALPHAREF); }
static void cmd_point_size(Ctx *c) { rec_float(c, OP_POINTSIZE); }
static void cmd_line_width(Ctx *c) { rec_float(c, OP_LINEWIDTH); }
static void cmd_poly_offset(Ctx *c) { rec_float(c, OP_POLYOFFSET); }
static void cmd_rast_discard(Ctx *c) { rec(c, OP_RASTDISCARD, 0); }
static void cmd_depth_clamp(Ctx *c) { rec(c, OP_DEPTHCLAMP, 0); }
static void cmd_prim_restart(Ctx *c) { rec(c, OP_PRIMRESTART, 0); }
static void cmd_patch_size(Ctx *c) { rec(c, OP_PATCHSIZE, 0); }
static void cmd_clear_color(Ctx *c) { uint64_t src = A2; Cmd *m = rec(c, OP_CLEARCOLOR, 16); if (src) memcpy(BLOB(m), P(src), 16); else memset(BLOB(m), 0, 16); }   /* (cmdbuf, index, rgba, mask) */
static void cmd_clear_ds(Ctx *c) { float d = F0; Cmd *m = rec(c, OP_CLEARDS, 0); m->a[3] = 0; memcpy(&m->a[3], &d, 4); }   /* (cmdbuf, depth [v0], depthMask, stencilValue, stencilMask) */
static void cmd_draw_arrays(Ctx *c) { Cmd *m = rec(c, OP_DRAW_ARRAYS, 0); m->a[3] = 0; m->a[4] = 1; }              /* (cmdbuf, mode, first, count) */
static void cmd_draw_arrays_inst(Ctx *c) { rec(c, OP_DRAW_ARRAYS, 0); }                                            /* (cmdbuf, mode, first, count, baseInstance, instanceCount) */
static void cmd_draw_elements(Ctx *c) { Cmd *m = rec(c, OP_DRAW_ELEMENTS, 0); m->a[4] = 0; m->a[5] = 0; m->a[6] = 1; }   /* (cmdbuf, mode, type, count, indexBuffer) */
static void cmd_draw_elements_bv(Ctx *c) { Cmd *m = rec(c, OP_DRAW_ELEMENTS, 0); m->a[5] = 0; m->a[6] = 1; }             /* (.., baseVertex) */
static void cmd_draw_elements_inst(Ctx *c) {            /* (cmdbuf, mode, type, count, indexBuffer, baseVertex, baseInstance, instanceCount) */
    static int nlog;
    logf_limited(&nlog, 6, "nvn_gl: DrawElementsInstanced mode %d type %d count %d index %llx x5=%d x6=%d x7=%d\n", (int)A1, (int)A2, (int)A3, (unsigned long long)A4, (int)A5, (int)A6, (int)A7);
    rec(c, OP_DRAW_ELEMENTS, 0);
}
static void cmd_copy_buf_tex(Ctx *c) {                  /* (cmdbuf, buffer, texture, view, region, flags) */
    uint64_t view = A3, region = A4;
    Tex *t = hobj(A2, K_TEX);
    Cmd *m = rec(c, OP_COPY_BUF_TEX, 48);
    CmdBuf *cb = cb_get(A0);
    /* PR #14 (xtlg511): the copy takes the strides this command buffer has now (SetCopyRowStride / SetCopyImageStride), as
     * NVN does: at replay the running stride state may come from another command buffer's copies (a 320x320 upload read with
     * the 832-texel rows of a stage preview read past its buffer: the crash after a random stage choice) */
    m->a[1] = (uint64_t)(uintptr_t)t; m->a[5] = (uint64_t)cb->row_stride; m->a[6] = (uint64_t)cb->image_stride;
    memset(BLOB(m), 0, 48);
    if (view) memcpy(BLOB(m), P(view), sizeof(View));
    if (region) memcpy(BLOB(m) + 24, P(region), 24);
}
static void cmd_copy_tex_tex(Ctx *c) {                  /* (cmdbuf, src, srcView, srcRegion, dst, dstView, dstRegion, flags) */
    uint64_t sv = A2, sr = A3, dv = A5, dr = A6;
    Tex *s = hobj(A1, K_TEX), *d = hobj(A4, K_TEX);
    Cmd *m = rec(c, OP_COPY_TEX_TEX, 96);
    m->a[0] = (uint64_t)(uintptr_t)s; m->a[3] = (uint64_t)(uintptr_t)d;
    memset(BLOB(m), 0, 96);
    if (sv) memcpy(BLOB(m), P(sv), sizeof(View));
    if (sr) memcpy(BLOB(m) + 24, P(sr), 24);
    if (dv) memcpy(BLOB(m) + 48, P(dv), sizeof(View));
    if (dr) memcpy(BLOB(m) + 72, P(dr), 24);
}
static void cmd_copy_tex_buf(Ctx *c) {                  /* (cmdbuf, texture, view, region, buffer, flags) */
    uint64_t view = A2, region = A3;
    Tex *t = hobj(A1, K_TEX);
    Cmd *m = rec(c, OP_COPY_TEX_BUF, 48);
    CmdBuf *cb = cb_get(A0);
    m->a[0] = (uint64_t)(uintptr_t)t; m->a[3] = c->x[4]; m->a[5] = (uint64_t)cb->row_stride; m->a[6] = (uint64_t)cb->image_stride;   /* PR #14 (xtlg511): as cmd_copy_buf_tex */
    memset(BLOB(m), 0, 48);
    if (view) memcpy(BLOB(m), P(view), sizeof(View));
    if (region) memcpy(BLOB(m) + 24, P(region), 24);
}
static void cmd_sample_mask(Ctx *c) { rec(c, OP_SAMPLEMASK, 0); }                     /* (cmdbuf, mask) */
static void cmd_downsample(Ctx *c) { Tex *s = hobj(A1, K_TEX), *d = hobj(A2, K_TEX); Cmd *m = rec(c, OP_DOWNSAMPLE, 0); m->a[0] = (uint64_t)(uintptr_t)s; m->a[1] = (uint64_t)(uintptr_t)d; }
static void cmd_copy_buf_buf(Ctx *c) { rec(c, OP_COPY_BUF_BUF, 0); }                  /* (cmdbuf, src, dst, size, flags) */
static void cmd_update_ubo(Ctx *c) {                    /* (cmdbuf, buffer, bufferSize, offset, size, data) */
    uint64_t size = A4, src = A5;
    Cmd *m = rec(c, OP_UPDATE_UBO, (size_t)size);
    if (src) memcpy(BLOB(m), P(src), (size_t)size); else m->a[3] = 0;
}
static void cmd_clear_buffer(Ctx *c) { rec(c, OP_CLEAR_BUF, 0); }                     /* (cmdbuf, buffer, size, value) */
static void cmd_row_stride(Ctx *c) { cb_get(A0)->row_stride = (int64_t)A1; rec(c, OP_ROWSTRIDE, 0); }
static void cmd_dispatch(Ctx *c) { cb_get(A0)->dispatches++; rec(c, OP_DISPATCH, 0); }                          /* (cmdbuf, groupsX, groupsY, groupsZ) */
static void cmd_image(Ctx *c) { uint64_t h = A3; Cmd *m = rec(c, OP_IMAGES, 8); m->a[2] = 1; memcpy(BLOB(m), &h, 8); }   /* (cmdbuf, stage, index, imageHandle) */
static void cmd_images(Ctx *c) {                        /* (cmdbuf, stage, first, count, handles) */
    int n = (int)A3;
    uint64_t src = A4;
    Cmd *m = rec(c, OP_IMAGES, (size_t)n * 8);
    if (src) memcpy(BLOB(m), P(src), (size_t)n * 8); else memset(BLOB(m), 0, (size_t)n * 8);
}

/* Round 18, TEST ONLY (SSBU_RACECHK=1): does guest memory that submitted commands NAME (uniform / storage / vertex
 * ranges) change between nvnQueueSubmitCommands on the game's thread and the replay on the render thread? The submit
 * walks the lists and keeps a hash of every range (and the first 512 bytes of a uniform range); the back end calls
 * nvn_front_racechk at the start (phase 1) and at the end (phase 2) of the replay of the same batch, which walks them
 * again and compares. Lines "racechk:". Nothing runs without the variable. */
typedef struct RaceRec { struct RaceRec *next; int n, cap, ncs, capcs; LONG64 frame; uint64_t *h; uint8_t *copy; Range *cs; } RaceRec;      /* cs: every storage range of the batch (what a dispatch may write) */
static int race_is_storage(const RaceRec *r, const Range *g) { for (int i = 0; i < r->ncs; i++) if (g->addr < r->cs[i].addr + r->cs[i].size && r->cs[i].addr < g->addr + g->size) return 1; return 0; }
static RaceRec *g_race_head, *g_race_tail;
static SRWLOCK g_race_lock = SRWLOCK_INIT;
static LONG64 g_race_stat[3][3], g_race_batches, g_race_bad_batches[3];      /* [uniform, storage, vertex][ranges seen, changed before the replay, changed during it] */
static int racechk_env(void) { static int on = -1; if (on < 0) on = getenv("SSBU_RACECHK") != NULL; return on; }
static uint64_t race_fnv(const uint8_t *b, size_t n) { uint64_t h = 0xcbf29ce484222325ULL; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL; return h; }
static void race_walk(uint64_t handle, int depth, RaceRec *r, int phase, int *pos, int *bad) {
    size_t len = 0;
    uint8_t *data = list_lookup(handle, &len);
    if (!data) return;
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        if (m->size < sizeof(Cmd)) break;
        off += m->size;
        if (m->op == OP_CALL) { for (int i = 0; i < (int)m->a[0]; i++) { uint64_t h; memcpy(&h, BLOB(m) + i * 8, 8); if (depth < 8) race_walk(h, depth + 1, r, phase, pos, bad); } continue; }
        if (m->op != OP_UBO && m->op != OP_SSBO && m->op != OP_VBUF) continue;
        int kind = m->op == OP_UBO ? 0 : m->op == OP_SSBO ? 1 : 2, count = m->op == OP_VBUF ? (int)m->a[1] : (int)m->a[2];
        size_t cap = kind == 0 ? 65536 : kind == 1 ? 16384 : 4096;
        for (int i = 0; i < count; i++) {
            Range g;
            memcpy(&g, BLOB(m) + i * 16, 16);
            if (!g.addr || !g.size || g.addr < 0x1000) continue;
            size_t n = g.size > cap ? cap : (size_t)g.size, nc = n > 512 ? 512 : n;
            const uint8_t *p = P(g.addr);
            uint64_t h = race_fnv(p, n);
            if (phase == 0) {
                if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 1024; r->h = realloc(r->h, (size_t)r->cap * 8); r->copy = realloc(r->copy, (size_t)r->cap * 512); }
                r->h[r->n] = h;
                if (kind == 0) memcpy(r->copy + (size_t)r->n * 512, p, nc);
                r->n++;
                if (kind == 1) { if (r->ncs == r->capcs) { r->capcs = r->capcs ? r->capcs * 2 : 256; r->cs = realloc(r->cs, (size_t)r->capcs * sizeof(Range)); } r->cs[r->ncs++] = g; }
                continue;
            }
            int k = (*pos)++;
            if (k >= r->n) continue;
            if (phase == 1) g_race_stat[kind][0]++;
            if (r->h[k] == h) continue;
            r->h[k] = h;                                 /* phase 2 then reports only what changed during the replay */
            g_race_stat[kind][phase]++; bad[kind]++;
            static int shown, shown_cs;
            static LONG64 plain[3];
            int is_cs = kind != 0 && race_is_storage(r, &g);      /* a storage range of this batch: a dispatch's target, written by our own copy-back */
            if (kind == 2 && !is_cs) { plain[phase]++; if (plain[phase] % 200 == 1) printf("racechk: vertex ranges that are NOT a storage range of their batch and changed: %lld before the replay, %lld during it (so far)\n", (long long)plain[1], (long long)plain[2]); }
            if (is_cs ? shown_cs++ < 12 : shown < 400) {
                if (!is_cs) shown++;
                if (is_cs) printf("racechk: (a storage range of this batch) ");
                printf("racechk: frame %lld %s range %d of the batch (%s, stage/first %d, slot %d) guest %llx+%llu CHANGED %s", (long long)g_st.frames, kind == 0 ? "UNIFORM" : kind == 1 ? "storage" : "vertex", k,
                       k_op_name[m->op], (int)m->a[0], (int)(m->op == OP_VBUF ? m->a[0] : m->a[1]) + i, (unsigned long long)g.addr, (unsigned long long)g.size, phase == 1 ? "between the submit and the start of its replay" : "DURING its replay");
                if (kind == 0) {
                    const uint8_t *o = r->copy + (size_t)k * 512;
                    size_t d = 0;
                    while (d < nc && o[d] == p[d]) d++;
                    if (d < nc) { d &= ~(size_t)3; printf("; first difference at +%llu: at submit %08x %08x %08x %08x, now %08x %08x %08x %08x", (unsigned long long)d, *(const uint32_t *)(o + d), d + 8 <= nc ? *(const uint32_t *)(o + d + 4) : 0, d + 12 <= nc ? *(const uint32_t *)(o + d + 8) : 0, d + 16 <= nc ? *(const uint32_t *)(o + d + 12) : 0,
                                                       *(const uint32_t *)(p + d), d + 8 <= nc ? *(const uint32_t *)(p + d + 4) : 0, d + 12 <= nc ? *(const uint32_t *)(p + d + 8) : 0, d + 16 <= nc ? *(const uint32_t *)(p + d + 12) : 0); }
                    else printf("; the difference is after the first 512 bytes");
                    memcpy(r->copy + (size_t)k * 512, p, nc);
                }
                printf("\n");
            }
        }
    }
}
static void racechk_submit(const uint64_t *handles, int count) {      /* the game's thread */
    RaceRec *r = calloc(1, sizeof *r);
    int pos = 0, bad[3] = {0, 0, 0};
    r->frame = g_st.frames;
    for (int i = 0; i < count; i++) race_walk(handles[i], 0, r, 0, &pos, bad);
    AcquireSRWLockExclusive(&g_race_lock);
    if (g_race_tail) g_race_tail->next = r; else g_race_head = r;
    g_race_tail = r;
    ReleaseSRWLockExclusive(&g_race_lock);
}
void nvn_front_racechk(const uint64_t *handles, int count, int phase) {      /* render thread: 1 = before the replay of the oldest recorded batch, 2 = after it */
    if (!racechk_env()) return;
    AcquireSRWLockExclusive(&g_race_lock);
    RaceRec *r = g_race_head;
    if (r && phase == 2) { g_race_head = r->next; if (!g_race_head) g_race_tail = NULL; }
    ReleaseSRWLockExclusive(&g_race_lock);
    if (!r) return;
    int pos = 0, bad[3] = {0, 0, 0};
    for (int i = 0; i < count; i++) race_walk(handles[i], 0, r, phase, &pos, bad);
    for (int k = 0; k < 3; k++) if (bad[k]) g_race_bad_batches[k]++;
    if (phase == 1) g_race_batches++;
    if (phase == 2) {
        static LONG64 last;
        if (g_st.frames - last >= 300) {
            last = g_st.frames;
            printf("racechk: totals at frame %lld: %lld batches | uniform ranges %lld, changed before the replay %lld, during %lld | storage %lld / %lld / %lld | vertex (first 4 KB) %lld / %lld / %lld | batches with a changed uniform / storage / vertex range: %lld / %lld / %lld\n",
                   (long long)g_st.frames, (long long)g_race_batches, (long long)g_race_stat[0][0], (long long)g_race_stat[0][1], (long long)g_race_stat[0][2], (long long)g_race_stat[1][0], (long long)g_race_stat[1][1], (long long)g_race_stat[1][2],
                   (long long)g_race_stat[2][0], (long long)g_race_stat[2][1], (long long)g_race_stat[2][2], (long long)g_race_bad_batches[0], (long long)g_race_bad_batches[1], (long long)g_race_bad_batches[2]);
        }
        free(r->h); free(r->copy); free(r->cs); free(r);
    }
}

/* ---- queue and window */
static void l1_pipe_after_submit(void); static int l1_pipe_wait(void);
static void queue_submit(Ctx *c) {                      /* (queue, count, handles) */
    SubmitJob j = {(int)A1, P(A2), 0};
    L1T("SUBMIT %d lists: %llx %llx %llx %llx", (int)A1, A2 && A1 > 0 ? (unsigned long long)rd64(A2) : 0ull, A2 && A1 > 1 ? (unsigned long long)rd64(A2 + 8) : 0ull, A2 && A1 > 2 ? (unsigned long long)rd64(A2 + 16) : 0ull, A2 && A1 > 3 ? (unsigned long long)rd64(A2 + 24) : 0ull);
    { void display_l1_submit(void); display_l1_submit(); }      /* L1: submits between the game's wait for the window texture and its present */
    if (j.count > 0 && A2 && racechk_env()) racechk_submit(j.handles, j.count);
    { int r1_ra_phase(void), r1_on(void); int ph = r1_ra_phase();      /* R1 round 5 (diagnosis): a submitted handle the host no longer knows, by run-ahead phase */
      if (r1_on()) { static LONG64 bad[3], ok[3]; size_t ln; for (int i = 0; i < j.count; i++) { if (list_lookup(j.handles[i], &ln)) ok[ph]++; else { bad[ph]++; if (bad[ph] <= 5 || !(bad[ph] & (bad[ph] - 1))) { uint32_t si = (uint32_t)j.handles[i] - 1; int have = si < (uint32_t)g_list_count; printf("r1-ra: submitted handle %llx unknown (phase %d: %lld bad / %lld good so far; slot gen %u data %d finalized %d created frame %lld used %lld, frame now %lld)\n", (unsigned long long)j.handles[i], ph, (long long)bad[ph], (long long)ok[ph],
                have ? g_lists[si].gen : 0, have && g_lists[si].data != NULL, have ? g_lists[si].finalized : -1, have ? (long long)g_lists[si].created : -1, have ? (long long)g_lists[si].used : -1, (long long)g_st.frames); } } } } }
    QueueObj *q = hobj(A0, K_QUEUE);
    if (q && A2) {                              /* what the submitted commands took of the queue's memory (model: see rec; a dispatch's launch descriptor is 0x100 bytes) */
        AcquireSRWLockShared(&g_list_lock);
        for (int i = 0; i < j.count; i++) {
            uint64_t h = j.handles[i];
            uint32_t k = (uint32_t)h - 1;
            if (h && k < (uint32_t)g_list_count && g_lists[k].data) { q->cmd_used += g_lists[k].model_cmd; q->control_used += g_lists[k].model_ctrl; q->compute_used += (LONG64)g_lists[k].dispatches * 0x100; }
        }
        ReleaseSRWLockShared(&g_list_lock);
    }
    if (j.count > 0 && A2 && drawtag_env()) {      /* round 17, TEST (SSBU_DRAWTAG): the frame SSBU_SHOT_FIGHT will save: its commands are tagged */
        /* the game polls the pad for frame n AFTER it has submitted the commands whose picture is saved as fight_<n>
         * (submit, next frame's logic and poll, present): the tag is armed by the present that saved fight_<n-1>, so
         * SSBU_SHOT_FIGHT must name n-1 as well (e.g. 699,700 tags the commands of 700). First form (the pending
         * shot at the submit) tagged nothing: run p17_zvk. */
        extern volatile LONG g_drawtag_next;
        int n = (int)InterlockedExchange(&g_drawtag_next, 0);
        if (n) { if (gl_async_on()) gl_post(job_drawtag, &n, sizeof n); else gl_run(job_drawtag, &n); }
    }
    int r1_ra_quiet(void);
    if (j.count > 0 && A2 && r1_ra_quiet()) { static LONG64 n; n++; }      /* R1 round 5, run-ahead: the real pass's submission = the lists the speculative pass already ran on the GPU: not run again */
    else if (j.count > 0 && A2) {
        if (g_rb == &rb_gl) j.ww_seq = ww_scan_request();      /* the game's writes for this batch are done: the scan thread looks now */
        if (gl_async_on() && j.count <= 4096) {
            if (g_rb == &rb_gl && snap_on()) j.snap = snap_begin();
            size_t size = sizeof j + (size_t)j.count * 8;
            SubmitJob *copy = malloc(size);
            *copy = j;
            memcpy(copy + 1, j.handles, (size_t)j.count * 8);
            gl_post(g_rb->submit_async, copy, size);
            free(copy);
            if (j.snap) snap_fill(j.snap, j.handles, j.count);
        } else gl_run(g_rb->submit, &j);
        l1_pipe_after_submit();      /* L1 round 2 (SSBU_L1_PIPE=1): the picture is complete after this submission: present it now, hold the game here */
    }
}
static void wb_set_textures(Ctx *c) {                   /* (builder, count, textures[]): kept in the builder (0x40 bytes) */
    int n = (int)A1 < 6 ? (int)A1 : 6;
    wr8(A0, (uint8_t)n);
    for (int i = 0; i < n; i++) wr64(A0 + 8 + (uint64_t)i * 8, rd64(A2 + (uint64_t)i * 8));
}
/* Track V/O session 9: the textures of every window the game made (for the Vulkan back end's resolution scale: nvn_vk_draw.c wintex_is). */
static Tex *g_window_tex[16];
static volatile int g_window_tex_n;
int nvn_front_tex_is_window(const Tex *t) { for (int i = 0; i < g_window_tex_n; i++) if (g_window_tex[i] == t) return 1; return 0; }
static void win_initialize(Ctx *c) {                    /* (window, builder) */
    Window *w = hnew(A0, K_WIN, sizeof *w);
    w->count = (int)(rd32(A1) & 0xff) <= 6 ? (int)(rd32(A1) & 0xff) : 0;
    w->interval = rd8(A1 + 4); w->nactive = rd8(A1 + 5); w->native = rd64(A1 + 56);
    display_set_interval(w->interval);
    for (int i = 0; i < w->count; i++) w->tex[i] = hobj(rd64(A1 + 8 + (uint64_t)i * 8), K_TEX);
    for (int i = 0; i < w->count; i++) if (g_window_tex_n < 16) g_window_tex[g_window_tex_n++] = w->tex[i];
    printf("nvn_gl: window with %d textures", w->count);
    if (w->count && w->tex[0]) printf(" (%dx%d format 0x%x)", w->tex[0]->b.width, w->tex[0]->b.height, w->tex[0]->b.format);
    printf("\n");
    RET(1);
}
static void win_acquire(Ctx *c) {                       /* (window, sync, int *textureIndex) -> 0 = success */
    static volatile LONG next;
    Window *w = hobj(A0, K_WIN);
    L1T("window acquire (index %d)", (int)((next + 1) % 3));
    if (A2) wr32(A2, (uint32_t)(InterlockedIncrement(&next) % (w && w->count ? (w->nactive ? w->nactive : w->count) : 2)));
    InterlockedExchangePointer(&g_window_sync, A1 ? hobj(A1, K_SYNC) : NULL);        /* P3 round 3: nvnSyncWait on it = wait for the refresh that frees the texture */
    if (w && A2 && A1) { extern Window *g_l1_acq_win; extern volatile LONG g_l1_acq_idx, g_l1_acq_new; g_l1_acq_win = w; g_l1_acq_idx = (LONG)rd32(A2); InterlockedExchange(&g_l1_acq_new, 1); }      /* L1: early present */
    display_clock_sample(A2);
    RET(0);
}
static void queue_acquire(Ctx *c) { c->x[0] = c->x[1]; c->x[1] = 0; win_acquire(c); }      /* nvnQueueAcquireTexture(queue, window, int *textureIndex) */
static void win_set_crop(Ctx *c) { Window *w = hobj(A0, K_WIN); if (w) { w->crop[0] = (int)A1; w->crop[1] = (int)A2; w->crop[2] = (int)A3; w->crop[3] = (int)A4; } }
/* close4: a window built on the movie maker's native window (nn::album::GetMovieMakerNativeWindow, nnalbum.c) is not the screen:
 * what the game presents to it is one picture of the video being recorded. The picture is read back and handed to the encoder;
 * nothing is shown and the display is not paced by it. */
uint64_t album_movie_native_window(void);
int album_movie_push_frame(const uint8_t *rgba, int w, int h, int stride, int bottom_up);
void album_movie_frame_source_attach(void);
static void job_present_movie(void *arg) {
    PresentJob *j = arg;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    if (!t || t->dead || !t->o.gl || t->b.width <= 0 || t->b.height <= 0) return;
    int w = tw(t), h = th(t);
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
    glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);                /* GL row 0 is the top row of an NVN texture (see save_bmp) */
    fbo_detach(g_fbo_read);
    album_movie_push_frame(px, w, h, w * 4, 0);
    free(px);
    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
}
/* Session 13, TEST ONLY: SSBU_SHOT_FIGHT=<n>[,<n>...] (nnhid.c counts the fight frames at the pad poll): the present the game
 * queues next after the poll that reached fight frame n is saved as fight_<n>.bmp in SSBU_SHOT_DIR by whichever back end runs. */
static volatile LONG g_fight_shot;
void nvn_front_shot_fight(int n) { InterlockedExchange(&g_fight_shot, n); }
int nvn_front_shot_pending(void) { return (int)g_fight_shot; }      /* round 17 (SSBU_DRAWTAG): the fight frame the next queued present will be saved as, 0 = none */
unsigned long nvn_front_present_tid(void) { return g_pf_main_tid; }      /* the game thread that queues the presents (the stall log of nvn_vk_wd.c) */
/* Session 14 (display.c, the vsync thread at a tick): the present the Vulkan back end kept for this tick, as a job of the
 * render thread (in order with the other jobs; nobody waits for it). */
void nvn_vk_present_flush_job(void *arg);              /* nvn_vk_draw.c */
void nvn_front_post_present_flush(void) {
#ifdef _WIN32
    if (!g_vk) { gl_post(job_gl_swap_flush, NULL, 0); return; }
#endif
    if (g_vk) gl_post(nvn_vk_present_flush_job, NULL, 0);
}
/* osk.c, the on-screen keyboard: drawn by the Vulkan renderer only (the OpenGL path keeps the old text dialog) */
void nvn_vk_osk_redraw_job(void *arg);                 /* nvn_vk_draw.c */
long long nvn_vk_presents(void);                       /* nvn_vk_draw.c */
int nvn_front_osk_ready(void) { return g_vk != 0; }
void nvn_front_osk_redraw(void) { if (g_vk) gl_post(nvn_vk_osk_redraw_job, NULL, 0); }
long long nvn_front_presents(void) { return g_vk ? nvn_vk_presents() : g_st.frames; }
/* ---- Track L1 (input latency), early present. The game submits the commands of a frame's picture BEFORE it waits for the
 * window texture (nvnSyncWait on the acquire sync = display_wait_refresh), then reads the pad, runs that frame's logic and
 * only then calls nvnQueuePresentTexture for the picture it had finished before the wait (measured: tests\logs\vk13_l1t_*,
 * `l1-submit:` 0 submits between the wait and the present in every fight frame). With SSBU_L1_EARLY=1 (graphics.ini
 * latency = early | lowest) that picture is handed to the window when the wait BEGINS, for the tick the game waits for (the
 * hold of session 14 keeps it until then), instead of one tick later; the game's own present of the same texture then only
 * does the pacing bookkeeping. If the game does not present that texture next, the early present is counted as a miss. */
Window *g_l1_acq_win;
volatile LONG g_l1_acq_idx, g_l1_acq_new;
static Window *volatile g_l1_early_win;
static volatile LONG g_l1_early_idx;
static LONG64 g_l1_early_n, g_l1_early_miss;
static void l1_early_present(void) {
    int display_l1_early(void);
    if (!g_vk || !display_l1_early() || !InterlockedExchange(&g_l1_acq_new, 0)) return;
    Window *w = g_l1_acq_win;
    if (!w || (w->native && w->native == album_movie_native_window())) return;
    if (g_l1_early_win) { g_l1_early_win = NULL; g_l1_early_miss++; }
    PresentJob j = {w, (int)g_l1_acq_idx};
    /* the picture of THIS acquire is the one rendered since the last present: the index the game acquired at the end of its
     * previous frame was the target of the commands it submitted since; present it */
    g_l1_early_win = w; g_l1_early_idx = j.index;
    g_l1_early_n++;
    gl_post(g_rb->present, &j, sizeof j);
}
/* L1 round 2, PIPE (SSBU_L1_PIPE=1): measured (SSBU_L1_TRACE, tests\logs\vk13_l1t_tr1.log): after its present the game
 * acquires the next window texture, waits for it (nvnSyncWait on the acquire sync, 0x710386fce0) and for the previous
 * frame's fence (0x710386fcf0), then ends the command buffers its "render dispatch" thread recorded during the previous
 * frame plus one of its own and submits them in ONE call (0x71037f0b40, list of 6), then runs the next frame (task workers,
 * pad poll, logic) and only then presents that picture (0x710386fc84). The picture is complete after that submission
 * (l1-submit: no other submission before the present). Pipe: the window wait returns at once; right after that submission
 * the picture goes to the render thread for the next tick and the GAME is held there until that tick (display_wait_refresh),
 * i.e. before its next pad poll as before; the game's later present of the same texture only does the bookkeeping.
 * Saves one whole frame of the game's two. */
static int g_l1_pipe = -1;
static volatile LONG g_l1_pipe_pending;
static int l1_pipe_on(void) { if (g_l1_pipe < 0) { const char *e = getenv("SSBU_L1_PIPE"); int display_l1_pipe_default(void); g_l1_pipe = e ? atoi(e) != 0 : display_l1_pipe_default(); printf("nvn: pipe (picture presented right after the game's frame submission, game held there) %s%c", g_l1_pipe ? "ON" : "off", 10); } return g_l1_pipe && g_vk; }
static volatile LONG g_l1_sh_pending;
static int l1_pipe_wait(void) {
    InterlockedExchange(&g_l1_sh_pending, 1);
    if (!l1_pipe_on() || !g_l1_acq_win) return 0;
    InterlockedExchange(&g_l1_pipe_pending, 1);
    return 1;
}
static void l1_nop_job(void *arg) { (void)arg; }
static void l1_pipe_after_submit(void) {
    { static int sh = -1; if (sh < 0) sh = getenv("SSBU_L1_SUBMIT_HOLD") && atoi(getenv("SSBU_L1_SUBMIT_HOLD")); if (sh && !g_l1_pipe && InterlockedExchange(&g_l1_sh_pending, 0)) gl_run(l1_nop_job, NULL); }      /* TEST: old path, but the game waits until the render thread has replayed this submission (is the old picture a race with the game's next frame?) */
    if (!InterlockedExchange(&g_l1_pipe_pending, 0)) return;
    { int r1_ra_quiet(void); if (r1_ra_quiet()) return; }      /* R1 round 5, run-ahead: the real pass's submission repeats the picture the speculative pass presented: not presented, the game not held */
    Window *w = g_l1_acq_win;
    if (!w || (w->native && w->native == album_movie_native_window())) { display_wait_refresh(); return; }
    if (g_l1_early_win) { g_l1_early_win = NULL; g_l1_early_miss++; }
    PresentJob j = {w, (int)g_l1_acq_idx};
    g_l1_early_win = w; g_l1_early_idx = j.index;
    g_l1_early_n++;
    { void display_present_plan(int vulkan); display_present_plan(g_vk); }
    gl_post(g_rb->present, &j, sizeof j);
    display_wait_refresh();
}
void nvn_l1_report(FILE *out) { if (g_l1_early_n || g_l1_early_miss) fprintf(out, "l1: early presents %lld, game presents that did not match an early present %lld%c", (long long)g_l1_early_n, (long long)g_l1_early_miss, 10); }
static void queue_present(Ctx *c) {                     /* (queue, window, textureIndex) */
    PresentJob j = {hobj(A1, K_WIN), (int)A2};
    L1T("PRESENT texture %d", (int)A2);
    { void nvn_snap_flushcheck(const char *); nvn_snap_flushcheck("present"); }
    if (j.win && j.win->native && j.win->native == album_movie_native_window()) { gl_run(g_rb->present_movie, &j); return; }
    g_pf_main_tid = GetCurrentThreadId();
    int ra_mode; { int r1_ra_present_mode(void); ra_mode = g_l1_pipe > 0 ? r1_ra_present_mode() : 0; }      /* R1 round 5, run-ahead: 2 = the real pass's present (nothing), 1 = presented at the speculative submission already */
    if (ra_mode == 2) return;
    if (ra_mode == 1 && g_l1_early_win == j.win) j.index = (int)g_l1_early_idx;      /* (the restore rolled the game's texture index back, the host's acquire count is not: the texture presented is the host's) */
    if (g_l1_early_win && g_l1_early_win == j.win && g_l1_early_idx == j.index) {      /* L1: this picture was presented at the game's wait already: only the pacing bookkeeping */
        g_l1_early_win = NULL;
        if (g_l1_pipe) {      /* round 2: planned and held at the submission already; a picture wanted by SSBU_SHOT_FIGHT is saved by presenting the same texture again (test only) */
            if (g_fight_shot) { void nvn_vk_shot_job(void *arg); j.shot_fight = (int)InterlockedExchange(&g_fight_shot, 0); gl_run(nvn_vk_shot_job, &j); }
        } else {
        display_before_present();
        { void display_present_plan(int vulkan); display_present_plan(g_vk); }
        }
        { void display_l1_early_skip(void); display_l1_early_skip(); }
        display_pace();
        return;
    }
    if (g_l1_early_win) { g_l1_early_win = NULL; g_l1_early_miss++; }
    if (g_fight_shot) j.shot_fight = (int)InterlockedExchange(&g_fight_shot, 0);      /* session 13 (test, SSBU_SHOT_FIGHT): bound here, on the game's thread, so that both renderers save the same game frame */
    if (j.shot_fight && drawtag_env()) InterlockedExchange(&g_drawtag_next, j.shot_fight + 1);      /* round 17 (SSBU_DRAWTAG): the next submission is the next fight frame's */
    display_before_present();
    { void display_present_plan(int vulkan); display_present_plan(g_vk); }      /* session 14: the tick this frame is shown at, decided before the present (display.c) */
    /* OpenGL: the present is queued like the submission before it (NVN's present is a queue operation too). The game's
     * render thread used to stand here while ours replayed the whole frame (8-11 ms on the island), so the frame's busy
     * time was the game's work plus our replay: 14-19 ms, and frame_rate = 60 fell back to 30. Now the replay runs during
     * the wait for the next refresh. The game stays at most a frame ahead: its next frame starts with a SyncWait, which
     * runs after everything queued before it. TOMO_SYNC_PRESENT=1: the present waits, as before. */
    static int sync_present = -1;
    if (sync_present < 0) sync_present = getenv("TOMO_SYNC_PRESENT") != NULL;
    if (!g_vk && gl_async_on() && !sync_present && !j.shot_fight) gl_post(g_rb->present, &j, sizeof j);
    else gl_run(g_rb->present, &j);
    display_pace();
}


/* ================================================================ the rest of the NVN API (plan\phases\G1_nvn_audit.md)
 * Signatures and enum values: the public nvn.h (open-ead/nnheaders, MIT). Every entry point the game obtains has a handler
 * here or in nvn_stub.c; the ones that have no effect on this platform by the API's own meaning say why in nvn_audit_notes.tsv. */

/* ---- debug labels: kept for every object (nvn_label_get), and given to the OpenGL object where there is one */
typedef struct LabelEnt { uint64_t key; char text[96]; struct LabelEnt *next; } LabelEnt;
static LabelEnt *g_labels[1024];
static SRWLOCK g_label_lock = SRWLOCK_INIT;
void nvn_label_set(uint64_t obj, const char *text) {
    unsigned h = (unsigned)((obj >> 4) * 2654435761u) & 1023;
    AcquireSRWLockExclusive(&g_label_lock);
    LabelEnt *e = g_labels[h];
    while (e && e->key != obj) e = e->next;
    if (!e) { e = calloc(1, sizeof *e); e->key = obj; e->next = g_labels[h]; g_labels[h] = e; }
    snprintf(e->text, sizeof e->text, "%s", text ? text : "");
    ReleaseSRWLockExclusive(&g_label_lock);
}
const char *nvn_label_get(uint64_t obj) {
    unsigned h = (unsigned)((obj >> 4) * 2654435761u) & 1023;
    const char *r = "";
    AcquireSRWLockShared(&g_label_lock);
    for (LabelEnt *e = g_labels[h]; e; e = e->next) if (e->key == obj) r = e->text;
    ReleaseSRWLockShared(&g_label_lock);
    return r;
}
void nvn_label_drop(uint64_t obj) { if (nvn_label_get(obj)[0]) nvn_label_set(obj, ""); }
static void any_set_label(Ctx *c) {                     /* nvn<Object>SetDebugLabel(object, const char *label) */
    nvn_label_set(A0, A1 ? (const char *)P(A1) : "");
    Tex *t = hobj(A0, K_TEX);
    Prog *p = hobj(A0, K_PROG);
    if (t) t->label_dirty = 1;                          /* applied by tex_sync on the render thread */
    if (p) p->label_dirty = 1;                          /* applied when its GLSL programs are next used */
}
static void any_debug_id(Ctx *c) { RET(A0); }           /* nvn<Object>GetDebugID: a number unique to the object: its address */

/* ---- device */
static uint64_t g_nvn_device;
static int g_device_flags;
static LARGE_INTEGER g_qpf;
static uint64_t now_ns(void) {
    LARGE_INTEGER t;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / g_qpf.QuadPart) * 1000000000ull + (uint64_t)(t.QuadPart % g_qpf.QuadPart) * 1000000000ull / (uint64_t)g_qpf.QuadPart;
}
static void devb_defaults(Ctx *c) { memset(P(A0), 0, 0x40); }
static void devb_set_flags(Ctx *c) { wr32(A0, (uint32_t)A1); }
static void devb_get_flags(Ctx *c) { RET(rd32(A0)); }
static void dev_initialize(Ctx *c) { g_nvn_device = A0; g_device_flags = A1 ? (int)rd32(A1) : 0; RET(1); }      /* (device, builder) */
static void dev_finalize(Ctx *c) { if (g_nvn_device == A0) g_nvn_device = 0; }
static void dev_get_origin_mode(Ctx *c) { RET(g_origin_mode); }
static void dev_get_depth_mode(Ctx *c) { RET(g_depth_mode); }
static void dev_timestamp_now(Ctx *c) { RET(now_ns()); }
static void dev_timestamp_of(Ctx *c) { RET(A1 ? rd64(A1 + 8) : 0); }      /* (device, const NVNcounterData *): counter timestamps are written in nanoseconds */
static void dev_debug_domain(Ctx *c) { static volatile LONG next; RET(InterlockedIncrement(&next)); }
static void dev_separate_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)(uint32_t)A1 | 0x100000000ull); }   /* nvnDeviceGetSeparateTextureHandle [sdk 0x41bc60] */
static void dev_separate_sampler_handle(Ctx *c) { RET((uint64_t)((int64_t)(int32_t)(uint32_t)A1 << 20)); }                 /* nvnDeviceGetSeparateSamplerHandle [sdk 0x41bc50]: sbfiz x0, x1, #20, #32 */
static void dev_debugger_attached(Ctx *c) { RET(0); }   /* no external NVN debugger exists for this process: the true answer */
/* nvnDeviceInstallDebugCallback: in this SDK build the function is a bare `ret` [sdk 0x41b890] (the entry the name resolves to in
 * the table nvnDeviceGetProcAddress reads, [0x41d6a0] -> table [0xc28398]): nothing is stored, nothing can ever be reported.
 * Bound to n_nop in the table below. */
static void job_housekeeping(void *arg) { (void)arg; gl_housekeeping(); }
static void dev_apply_deferred(Ctx *c) { gl_run(g_rb->housekeeping, NULL); }      /* (device, age): objects finalized so far are released now */
static void any_get_device(Ctx *c) { RET(g_nvn_device); }                       /* nvn<Builder>GetDevice: there is one device */
static void any_set_device(Ctx *c) { if (A1) g_nvn_device = A1; }               /* nvn<Builder>SetDevice(builder, device) */

/* ---- sync objects: OpenGL fences */
static void sync_initialize(Ctx *c) { hnew(A0, K_SYNC, sizeof(SyncObj)); RET(1); }
static void job_sync_drop(void *arg) { SyncObj *o = arg; if (o->s) glDeleteSync(o->s); o->s = NULL; }
static void sync_finalize(Ctx *c) {                     /* the small host record stays: a recorded FenceSync / WaitSync may still name it */
    SyncObj *o = hobj(A0, K_SYNC);
    if (!o) return;
    InterlockedExchange(&o->dead, 1);
    wr32(A0 + 8, 0);
    gl_run(g_rb->sync_drop, o);
}
static void fence_now(SyncObj *o, int flags) {
    if (o->dead) return;
    if (o->s) glDeleteSync(o->s);
    o->s = glFenceSync(0x9117 /* SYNC_GPU_COMMANDS_COMPLETE */, 0);
    o->fence_frame = (long long)g_st.frames + 1;
    if (flags & 1) glFlush();                           /* NVN_SYNC_FLAG_FLUSH_FOR_CPU */
}
static void job_fence(void *arg) { SyncJob *j = arg; fence_now(j->o, j->flags); }
static void queue_fence_sync(Ctx *c) {                  /* (queue, sync, condition, flags) */
    L1T("fence sync %llx", (unsigned long long)A1);
    SyncObj *o = hobj(A1, K_SYNC);
    SyncJob j = {o, (int)A3, 0, 0};
    if (o) { if (gl_async_on()) gl_post(g_rb->fence, &j, sizeof j); else gl_run(g_rb->fence, &j); }
}
/* ONE FRAME IN FLIGHT. The game waits, every frame, for the fence of the frame before (synclog: 6-9 ms at 4K): on the
 * console that frees the memory the GPU read. Here the render thread has already copied all of it (arena, uploads) when it
 * reaches the wait, so the GPU only has to be done with what the game reads back (the buffers the GPU wrote, ~30 KB a
 * frame on the island, landed through their own fences right after the batch that wrote them). So the wait lands those,
 * then waits for the frame before the fenced one: the GPU works on one frame while the game builds the next instead of
 * idling between them (GPU busy 75 % at 4K). TOMO_EARLY_FENCE=0: the full wait. */
#define PF_RING 8
static void *g_pfence[PF_RING];                             /* a fence at the end of each presented frame (job_present) */
static long long g_pf_frame[PF_RING];
static int g_early_fence = -1;
static LONG64 g_early_n, g_full_n;
static void present_fence(long long frame) {
    if (g_early_fence < 0) g_early_fence = !(getenv("TOMO_EARLY_FENCE") && getenv("TOMO_EARLY_FENCE")[0] == '0');
    if (frame % 600 == 0) {
        static LARGE_INTEGER fq; if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        printf("nvn_gl: render-thread waits for the GPU, ms a frame by line (1000xx = compute read-back):");
        for (int k = 0; k < 32 && g_stall[k].line; k++) printf(" %d: %.2f (%.1f)", g_stall[k].line, (double)g_stall[k].ticks * 1000.0 / (double)fq.QuadPart / 600.0, (double)g_stall[k].n / 600.0);
        printf("%c", 10); memset(g_stall, 0, sizeof g_stall); }
    if (!g_early_fence) return;
    int k = (int)(frame % PF_RING);
    if (g_pfence[k]) glDeleteSync(g_pfence[k]);
    g_pfence[k] = glFenceSync(0x9117 /* SYNC_GPU_COMMANDS_COMPLETE */, 0);
    g_pf_frame[k] = frame;
    if (frame % 600 == 0 && (g_frz_rest || g_frz_skip)) printf("nvn_gl: frozen scene so far: %lld restores, %lld of them followed by 3D in the same frame (wrong), %lld left out (frames that start with 2D)%c", (long long)g_frz_rest, (long long)g_frz_wrong, (long long)g_frz_skip, 10);
    if (frame % 600 == 0 && g_snap_sets) { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        printf("nvn_gl: submit copies: %lld batches, %.0f ranges and %.0f KB a batch, %.3f ms a batch on the game's thread; %lld ranges the game rewrote before the replay read them (the copy was used)%c",
               (long long)g_snap_sets, (double)g_snap_ranges / (double)g_snap_sets, (double)g_snap_bytes / 1024.0 / (double)g_snap_sets, (double)g_snap_ticks * 1000.0 / (double)fq.QuadPart / (double)g_snap_sets, (long long)g_snap_used, 10);
        g_snap_sets = g_snap_ranges = g_snap_bytes = g_snap_ticks = g_snap_used = 0; }
    if (frame % 600 == 0) { static LONG64 cb0; printf("nvn_gl: one frame in flight: %lld early waits, %lld of them landed read-backs first; %.2f MB a frame read back from GPU-written buffers%c", (long long)g_early_n, (long long)g_full_n, (double)(g_st.compute_bytes - cb0) / 600.0 / 1048576.0, 10); cb0 = g_st.compute_bytes; }
}
static void job_sync_wait(void *arg) {
    SyncJob *j = arg;
    if (!j->o->s) { j->result = 3; return; }            /* never fenced: NVN_SYNC_WAIT_RESULT_FAILED */
    if (g_early_fence > 0 && j->timeout && j->o->fence_frame >= (long long)g_st.frames) {
        long long f = j->o->fence_frame - 1;            /* the frame presented before the fenced one */
        if (g_rbp_n) { rb_land(1); g_full_n++; }         /* what the GPU wrote for the game lands first */
        int k = (int)(f % PF_RING);
        if (f > 0 && g_pfence[k] && g_pf_frame[k] == f) {
            GLenum r = glClientWaitSync(g_pfence[k], 1, j->timeout);
            if (r == 0x911A || r == 0x911C) { j->result = 1; g_early_n++; return; }
        }
    }
    GLenum r = glClientWaitSync(j->o->s, 1 /* SYNC_FLUSH_COMMANDS_BIT */, j->timeout);
    j->result = r == 0x911A ? 0 : r == 0x911C ? 1 : r == 0x911B ? 2 : 3;       /* already signaled, condition satisfied, timeout expired, failed */
    if (g_rbp_n && j->result < 2) rb_land(0);           /* the game may now read what the GPU wrote before its fence */
}
static void sync_wait(Ctx *c) {                         /* (sync, timeoutNs) -> NVNsyncWaitResult */
    L1T("sync wait %llx%s", (unsigned long long)A0, hobj(A0, K_SYNC) == g_window_sync ? " (WINDOW texture)" : "");
    SyncObj *o = hobj(A0, K_SYNC);
    SyncJob j = {o, 0, A1, 3};
    LONG64 sw0 = pf_now();
    { void nvn_snap_flushcheck(const char *); nvn_snap_flushcheck("sync wait"); }
    if (o) gl_run(g_rb->sync_wait, &j);
    {   static int on = -1, n; if (on < 0) on = getenv("TOMO_SYNCLOG") ? (atoi(getenv("TOMO_SYNCLOG")) > 1 ? atoi(getenv("TOMO_SYNCLOG")) : 1500) : 0;      /* TEST ONLY, TOMO_SYNCLOG=1|<from frame>: which fence the game waits for, and how long */
        if (on && o && g_st.frames > on && n++ < 80) { LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            printf("synclog: frame %lld waits on fence of frame %lld%s: %.2f ms, result %d%c", (long long)g_st.frames + 1, o->fence_frame, o == g_window_sync ? " (window)" : "", (double)(pf_now() - sw0) * 1000.0 / (double)f.QuadPart, j.result, 10); } }
    if (o && o == g_window_sync && A1 && j.result == 3) {      /* P3 round 3: the window texture's sync (never fenced by us): it is free at the refresh that shows the queued frame (display.c) */
        if (l1_pipe_wait()) j.result = 1; else {      /* L1 round 2: pipe mode does not hold here */
        l1_early_present();                                    /* L1 (latency = early / SSBU_L1_EARLY=1): the picture the game finished before this wait goes to the window now (at its tick) */
        if (display_wait_refresh()) j.result = 1;             /* NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED; otherwise as before */
        }
    }
    RET(j.result);
}
static void job_server_wait(void *arg) { SyncJob *j = arg; if (j->o->s) glWaitSync(j->o->s, 0, 0xFFFFFFFFFFFFFFFFull); j->result = j->o->s != NULL; }
static void queue_wait_sync(Ctx *c) {                   /* (queue, sync) -> NVNboolean */
    SyncObj *o = hobj(A1, K_SYNC);
    SyncJob j = {o, 0, 0, 0};
    if (o) gl_run(g_rb->server_wait, &j);
    RET(j.result);
}
static void sync_no_gl_interop(Ctx *c) { RET(0); }

/* ---- events: a 32-bit value in memory pool storage that the CPU and queued commands signal and wait for.
 * NVNeventBuilder: +0 pool, +8 offset. NVNevent: +0 pool, +8 offset, +16 address of the value. */
static void evb_defaults(Ctx *c) { memset(P(A0), 0, 0x20); }
static void evb_storage(Ctx *c) { wr64(A0, A1); wr64(A0 + 8, A2); }
static void evb_get_storage(Ctx *c) { if (A1) wr64(A1, rd64(A0 + 8)); RET(rd64(A0)); }
static void ev_get_pool(Ctx *c) { RET(rd64(A0)); }
static void ev_get_offset(Ctx *c) { RET(rd64(A0 + 8)); }
static void ev_initialize(Ctx *c) {                     /* (event, builder) */
    uint64_t mem = 0, size = 0, pool = rd64(A1), off = rd64(A1 + 8);
    memset(P(A0), 0, 0x40);
    wr64(A0, pool); wr64(A0 + 8, off);
    if (!pool || !nvn_pool_range(pool, &mem, &size) || !mem || off + 4 > size) { RET(0); return; }
    wr64(A0 + 16, mem + off);
    RET(1);
}
static void ev_finalize(Ctx *c) { wr64(A0 + 16, 0); }
static void ev_get_value(Ctx *c) { uint64_t a = rd64(A0 + 16); RET(a ? rd32(a) : 0); }
static void event_signal_at(uint64_t a, int mode, uint32_t value) {
    if (!a) return;
    if (mode == 1) InterlockedExchangeAdd((volatile LONG *)P(a), (LONG)value); else InterlockedExchange((volatile LONG *)P(a), (LONG)value);
}
static void ev_signal(Ctx *c) { event_signal_at(rd64(A0 + 16), (int)A1, (uint32_t)A2); }      /* (event, mode, value) */

/* ---- queue. NVNqueueBuilder (0x40): +0 queue memory, +8 its size, +0x10 command, +0x18 compute, +0x20 control memory size,
 * +0x28 command flush threshold, +0x30 flags, +0x34 priority. NVNqueue: +0 host record. */
static void qb_defaults(Ctx *c) {                       /* sizes: the defaults of the device (nvnDeviceGetInteger 0x3a / 0x3d / 0x3b in this table), priority normal */
    memset(P(A0), 0, 0x40);
    wr64(A0 + 0x10, 65536); wr64(A0 + 0x18, 262144); wr64(A0 + 0x20, 16384); wr64(A0 + 0x28, 0); wr32(A0 + 0x34, 100);
}
static void qb_set_flags(Ctx *c) { wr32(A0 + 0x30, (uint32_t)A1); }
static void qb_set_cmd(Ctx *c) { wr64(A0 + 0x10, A1); }
static void qb_set_compute(Ctx *c) { wr64(A0 + 0x18, A1); }
static void qb_set_control(Ctx *c) { wr64(A0 + 0x20, A1); }
static void qb_set_threshold(Ctx *c) { wr64(A0 + 0x28, A1); }
static void qb_set_priority(Ctx *c) { wr32(A0 + 0x34, (uint32_t)A1); }
static void qb_set_memory(Ctx *c) { wr64(A0, A1); wr64(A0 + 8, A2); }
static void qb_get_flags(Ctx *c) { RET(rd32(A0 + 0x30)); }
static void qb_get_cmd(Ctx *c) { RET(rd64(A0 + 0x10)); }
static void qb_get_compute(Ctx *c) { RET(rd64(A0 + 0x18)); }
static void qb_get_control(Ctx *c) { RET(rd64(A0 + 0x20)); }
static void qb_get_threshold(Ctx *c) { RET(rd64(A0 + 0x28)); }
static void qb_get_priority(Ctx *c) { RET(rd32(A0 + 0x34)); }
static void qb_get_memory(Ctx *c) { RET(rd64(A0)); }
static void qb_get_memory_size(Ctx *c) { RET(rd64(A0 + 8)); }
static void qb_get_queue_memory_size(Ctx *c) { RET((rd64(A0 + 0x10) + rd64(A0 + 0x18) + rd64(A0 + 0x20) + 0xfff) & ~0xfffull); }   /* what a queue with these sizes needs */
static void queue_initialize(Ctx *c) {                  /* (queue, builder) */
    QueueObj *q = hnew(A0, K_QUEUE, sizeof *q);
    q->cmd_size = rd64(A1 + 0x10); q->compute_size = rd64(A1 + 0x18); q->control_size = rd64(A1 + 0x20); q->flush_threshold = rd64(A1 + 0x28);
    q->flags = (int)rd32(A1 + 0x30); q->priority = (int)rd32(A1 + 0x34);
    RET(1);
}
static void queue_finalize(Ctx *c) { gl_barrier(); QueueObj *q = hobj(A0, K_QUEUE); if (q) { wr32(A0 + 8, 0); free(q); } nvn_label_drop(A0); }
static void queue_total_cmd(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); RET(q ? q->cmd_used : 0); }
static void queue_total_control(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); RET(q ? q->control_used : 0); }
static void queue_total_compute(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); RET(q ? q->compute_used : 0); }
static void queue_reset_usage(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); if (q) q->cmd_used = q->control_used = q->compute_used = 0; }
static void job_flush(void *arg) { (void)arg; glFlush(); }
static void job_finish(void *arg) { (void)arg; glFinish(); if (g_rbp_n) rb_land(1); }
static void queue_flush(Ctx *c) { (void)c; { void nvn_snap_flushcheck(const char *); nvn_snap_flushcheck("flush"); } if (gl_async_on()) gl_post(g_rb->flush, NULL, 0); else gl_run(g_rb->flush, NULL); }
static void queue_finish(Ctx *c) { (void)c; gl_run(g_rb->finish, NULL); }
static void job_reset_status(void *arg) { *(int *)arg = glGetGraphicsResetStatus ? (int)glGetGraphicsResetStatus() : 0; }
static void queue_get_error(Ctx *c) {                   /* (queue, NVNqueueErrorInfo *) -> NVNqueueGetErrorResult: the state of the real GPU context */
    int st = 0;
    gl_run(g_rb->reset_status, &st);
    RET(st ? 1 : 0);                                    /* GPU_NO_ERROR, or GPU_ERROR_UNKNOWN after a context reset */
}

/* ---- multisample state (0x18 bytes): +0 multisample enable, +1 alpha to coverage, +2 its dither, +3 bit 0 coverage to colour,
 * bit 1 sample locations, bit 2 sample location grid, bits 3..4 coverage modulation mode, bits 5..7 coverage-to-colour output;
 * +4 samples, +5 raster samples; +8..+23 sixteen sample locations, x in the low and y in the high four bits (sixteenths). */
static void ms_defaults(Ctx *c) { memset(P(A0), 0, 0x18); wr8(A0, 1); wr8(A0 + 2, 1); memset(P(A0 + 8), 0x88, 16); }
static void ms_enable(Ctx *c) { wr8(A0, (uint8_t)(A1 & 1)); }
static void ms_a2c(Ctx *c) { wr8(A0 + 1, (uint8_t)(A1 & 1)); }
static void ms_a2c_dither(Ctx *c) { wr8(A0 + 2, (uint8_t)(A1 & 1)); }
static void ms_samples(Ctx *c) { wr8(A0 + 4, (uint8_t)A1); }
static void ms_raster_samples(Ctx *c) { wr8(A0 + 5, (uint8_t)A1); }
static void ms_flag(uint64_t a, unsigned shift, unsigned mask, uint64_t v) { wr8(a + 3, (uint8_t)((rd8(a + 3) & ~(mask << shift)) | ((v & mask) << shift))); }
static void ms_c2c(Ctx *c) { ms_flag(A0, 0, 1, A1); }
static void ms_locations_on(Ctx *c) { ms_flag(A0, 1, 1, A1); }
static void ms_grid_on(Ctx *c) { ms_flag(A0, 2, 1, A1); }
static void ms_covmod(Ctx *c) { ms_flag(A0, 3, 3, A1); }
static void ms_c2c_output(Ctx *c) { ms_flag(A0, 5, 7, A1); }
static void ms_set_locations(Ctx *c) {                  /* (state, start, count, const float *xy pairs in [0, 1)) */
    for (int i = 0; i < (int)A2 && (int)A1 + i < 16; i++) {
        float x, y;
        memcpy(&x, P(A3 + (uint64_t)i * 8), 4); memcpy(&y, P(A3 + (uint64_t)i * 8 + 4), 4);
        int xi = (int)(x * 16.0f), yi = (int)(y * 16.0f);
        wr8(A0 + 8 + A1 + (uint64_t)i, (uint8_t)((xi < 0 ? 0 : xi > 15 ? 15 : xi) | (yi < 0 ? 0 : yi > 15 ? 15 : yi) << 4));
    }
}
static void ms_get_enable(Ctx *c) { RET(rd8(A0)); }
static void ms_get_a2c(Ctx *c) { RET(rd8(A0 + 1)); }
static void ms_get_a2c_dither(Ctx *c) { RET(rd8(A0 + 2)); }
static void ms_get_samples(Ctx *c) { RET(rd8(A0 + 4)); }
static void ms_get_raster_samples(Ctx *c) { RET(rd8(A0 + 5)); }
static void ms_get_c2c(Ctx *c) { RET(rd8(A0 + 3) & 1); }
static void ms_get_locations_on(Ctx *c) { RET((rd8(A0 + 3) >> 1) & 1); }
static void ms_get_grid_on(Ctx *c) { RET((rd8(A0 + 3) >> 2) & 1); }
static void ms_get_covmod(Ctx *c) { RET((rd8(A0 + 3) >> 3) & 3); }
static void ms_get_c2c_output(Ctx *c) { RET(rd8(A0 + 3) >> 5); }
static void job_grid(void *arg) { GLint *wh = arg; wh[0] = wh[1] = 1; glGetIntegerv(0x933E /* SAMPLE_LOCATION_PIXEL_GRID_WIDTH_NV */, &wh[0]); glGetIntegerv(0x933F, &wh[1]); while (glGetError()) {} }
static void ms_get_grid(Ctx *c) {                       /* (state, int *width, int *height): the grid this GPU repeats sample locations over */
    GLint wh[2] = {1, 1};
    if ((rd8(A0 + 3) >> 2) & 1) gl_run(g_rb->sample_grid, wh);
    if (A1) wr32(A1, (uint32_t)wh[0]);
    if (A2) wr32(A2, (uint32_t)wh[1]);
}

/* ---- the other small state objects: getters of what the setters stored */
/* blend state (8 bytes): +0 bits 0..2 target, 3..4 advanced overlap, 5 premultiplied source, 6 normalised destination;
 * +1..+4 functions, +5 +6 equations, +7 advanced mode */
static void blend_defaults(Ctx *c) { static const uint8_t d[8] = {0x20, 2, 1, 2, 1, 1, 1, 0}; memcpy(P(A0), d, 8); }
static void blend_target(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~7) | (A1 & 7))); }
static void blend_adv_mode(Ctx *c) { wr8(A0 + 7, (uint8_t)A1); }
static void blend_adv_overlap(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~0x18) | ((A1 & 3) << 3))); }
static void blend_adv_premult(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~0x20) | ((A1 & 1) << 5))); }
static void blend_adv_normdst(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~0x40) | ((A1 & 1) << 6))); }
static void blend_get_target(Ctx *c) { RET(rd8(A0) & 7); }
static void blend_get_func(Ctx *c) { for (int i = 0; i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], rd8(A0 + 1 + (uint64_t)i)); }
static void blend_get_equation(Ctx *c) { if (A1) wr32(A1, rd8(A0 + 5)); if (A2) wr32(A2, rd8(A0 + 6)); }
static void blend_get_adv_mode(Ctx *c) { RET(rd8(A0 + 7)); }
static void blend_get_adv_overlap(Ctx *c) { RET((rd8(A0) >> 3) & 3); }
static void blend_get_adv_premult(Ctx *c) { RET((rd8(A0) >> 5) & 1); }
static void blend_get_adv_normdst(Ctx *c) { RET((rd8(A0) >> 6) & 1); }
static void color_get_blend_enable(Ctx *c) { RET((rd8(A0) >> (A1 & 7)) & 1); }
static void color_get_logic_op(Ctx *c) { RET(rd8(A0 + 1)); }
static void color_get_alpha_test(Ctx *c) { RET(rd8(A0 + 2)); }
static void chanmask_get(Ctx *c) {                      /* (state, index, NVNboolean *r, *g, *b, *a) */
    uint32_t m = rd32(A0) >> (((uint32_t)A1 & 7) * 4);
    for (int i = 0; i < 4; i++) if (c->x[2 + i]) wr8(c->x[2 + i], (uint8_t)((m >> i) & 1));
}
static void ds_get_stencil_func(Ctx *c) { RET((rd64(A0) >> (A1 & 2 && !(A1 & 1) ? 12 : 8)) & 15); }       /* (state, face) */
static void ds_get_stencil_op(Ctx *c) {                 /* (state, face, *stencilFail, *depthFail, *depthPass) */
    uint64_t v = rd64(A0) >> (A1 & 2 && !(A1 & 1) ? 28 : 16);
    for (int i = 0; i < 3; i++) if (c->x[2 + i]) wr32(c->x[2 + i], (uint32_t)((v >> (i * 4)) & 15));
}
static void vattr_get_format(Ctx *c) { if (A1) wr32(A1, rd32(A0) & 0xff); if (A2) wr64(A2, rd32(A0) >> 14); }
static void vattr_get_stream(Ctx *c) { RET((rd32(A0) >> 8) & 31); }
static void vstream_get_stride(Ctx *c) { RET(rd32(A0)); }
static void vstream_get_divisor(Ctx *c) { RET(rd32(A0 + 4)); }

/* ---- samplers: NVNsamplerBuilder and NVNsampler both hold a SamplerParams */
static void smpb_border_i(Ctx *c) { if (A1) { memcpy(SMP->border, P(A1), 16); SMP->border_kind = strstr(hostfn_current_name(), "ui") ? 1 : 2; } }
static void smpb_border_f(Ctx *c) { if (A1) { memcpy(SMP->border, P(A1), 16); SMP->border_kind = 0; } }
static void smpb_lod_snap(Ctx *c) { SMP->lod_snap = F0; }
static void smp_get_minmag(Ctx *c) { if (A1) wr32(A1, SMP->min); if (A2) wr32(A2, SMP->mag); }
static void smp_get_wrap(Ctx *c) { if (A1) wr32(A1, SMP->ws); if (A2) wr32(A2, SMP->wt); if (A3) wr32(A3, SMP->wr); }
static void smp_get_lodclamp(Ctx *c) { if (A1) memcpy(P(A1), &SMP->lodmin, 4); if (A2) memcpy(P(A2), &SMP->lodmax, 4); }
static void smp_get_lodbias(Ctx *c) { F0 = SMP->bias; }
static void smp_get_compare(Ctx *c) { if (A1) wr32(A1, SMP->cmode); if (A2) wr32(A2, SMP->cfunc); }
static void smp_get_border(Ctx *c) { if (A1) memcpy(P(A1), SMP->border, 16); }
static void smp_get_aniso(Ctx *c) { F0 = SMP->aniso; }
static void smp_get_reduction(Ctx *c) { RET(SMP->reduction); }
static void smp_get_lod_snap(Ctx *c) { F0 = SMP->lod_snap; }
static void smp_compare(Ctx *c) { RET(A0 && A1 && !memcmp(P(A0), P(A1), sizeof(SamplerParams))); }
static void smp_finalize(Ctx *c) { nvn_label_drop(A0); }      /* the parameters live in the guest object; OpenGL sampler objects are shared by value */
static void spool_finalize(Ctx *c) {
    gl_barrier();
    SamplerPool *sp = hobj(A0, K_SPOOL);
    if (!sp) return;
    AcquireSRWLockExclusive(&g_pool_lock);
    sp->count = 0;                                      /* the record itself stays: recorded SetSamplerPool commands may still name it */
    ReleaseSRWLockExclusive(&g_pool_lock);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
}
static void spool_get_pool(Ctx *c) { SamplerPool *sp = hobj(A0, K_SPOOL); RET(sp ? sp->mempool : 0); }
static void spool_get_offset(Ctx *c) { SamplerPool *sp = hobj(A0, K_SPOOL); RET(sp ? sp->offset : 0); }
static void spool_get_size(Ctx *c) { SamplerPool *sp = hobj(A0, K_SPOOL); RET(sp ? sp->count : 0); }

/* ---- textures */
static void texb_get_swizzle(Ctx *c) { for (int i = 0; i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], TB->swz[i]); }
static void texb_get_dsmode(Ctx *c) { RET(TB->dsmode); }
static void texb_get_pool(Ctx *c) { RET(TB->pool); }
static void texb_get_offset(Ctx *c) { RET(TB->offset); }
static void texb_get_packaged(Ctx *c) { RET(TB->packaged); }
static void texb_get_layout(Ctx *c) { if (A1) memcpy(P(A1), TB->layout, 8); }
static void texb_get_stride(Ctx *c) { RET(TB->stride); }
static void texb_set_glname(Ctx *c) { TB->glname = (uint32_t)A1; }
static void texb_get_glname(Ctx *c) { RET(TB->glname); }
static void tex_get_swizzle(Ctx *c) { Tex *t = hobj(A0, K_TEX); for (int i = 0; t && i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], t->b.swz[i]); }
TEXGET(tex_get_pool, t->b.pool)
static void tex_compare(Ctx *c) {                       /* two textures are the same when they describe the same storage the same way */
    Tex *a = hobj(A0, K_TEX), *b = hobj(A1, K_TEX);
    RET(a && b && (a == b || !memcmp(&a->b, &b->b, sizeof a->b)));
}
static void tex_view_offset(Ctx *c) {                   /* (texture, view): where the view's first level and layer start in the texture's storage */
    Tex *t = hobj(A0, K_TEX);
    View v;
    TexLayout L;
    if (!t || !A1 || (t->b.flags & 0x10)) { RET(0); return; }
    memcpy(&v, P(A1), sizeof v);
    tex_lay(&t->b, &L);
    int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0;
    if (level < 0 || level >= L.levels || layer < 0 || layer >= L.layers) { RET(0); return; }
    RET((uint64_t)(tex_level_ptr(&L, (const uint8_t *)0x1000, level, layer) - (const uint8_t *)0x1000));
}
/* ReadJob: nvn_front.h (V1 session 5: the Vulkan back end reads texels too) */
/* nvnTextureReadTexels: texels of a region as tightly packed rows (or with the given strides). nvnTextureInvalidateTexels: what
 * the GPU rendered or copied into the texture becomes readable in its pool memory, in the texture's own (block-linear) layout. */
static void job_read_texels(void *arg) {
    ReadJob *j = arg;
    if (g_rbp_n) rb_land(1);
    Tex *t = j->tex;
    Fmt f;
    static uint8_t *tmp;
    static size_t tmp_cap;
    static int nlog;
    if (!fmt_get(t->b.format, &f)) return;
    tex_sync(t);
    while (glGetError()) {}
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    int level = j->has_view && (j->view.set & 0x81) == 0x81 ? j->view.lvl0 : 0, layer0 = j->has_view && (j->view.set & 0x82) == 0x82 ? j->view.lay0 : 0;
    GLenum fo = f.kind == 1 ? 0x1902 : f.kind == 2 ? GL_STENCIL_INDEX_ : f.format;
    if (!j->to_pool) {
        const int *r = j->region;
        int d = r[5] > 0 ? r[5] : 1, z = t->b.target == 2 ? r[2] : layer0 + r[2];
        size_t rows = (size_t)(r[4] + f.bh - 1) / f.bh, rowb = j->row_stride ? (size_t)j->row_stride : ((size_t)(r[3] + f.bw - 1) / f.bw) * f.bpb;
        size_t bytes = (j->image_stride ? (size_t)j->image_stride : rowb * rows) * (size_t)d;
        if (j->row_stride && f.kind != 4) glPixelStorei(0x0D02 /* PACK_ROW_LENGTH */, (GLint)(j->row_stride / f.bpb));
        if (j->image_stride && j->row_stride && f.kind != 4) glPixelStorei(0x806C /* PACK_IMAGE_HEIGHT */, (GLint)(j->image_stride / j->row_stride));
        if (f.kind == 4) glGetCompressedTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, (GLsizei)bytes, j->out);
        else if (t->rw && d == 1) rs_get(t, level, r[0], r[1], r[3], r[4], fo, f.type, (GLsizei)bytes, j->out);      /* resolution scale */
        else glGetTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, fo, f.type, (GLsizei)bytes, j->out);
        glPixelStorei(0x0D02, 0); glPixelStorei(0x806C, 0);
    } else if (t->mem && t->gpu_written && !t->b.packaged) {
        const TexFormat *tf = tex_format(t->b.format);
        TexLayout L;
        if (tf && tf->sw_decode) { logf_limited(&nlog, 5, "nvn_gl: InvalidateTexels on a texture of format 0x%x that is kept decoded: its pool memory is not rewritten\n", t->b.format); return; }
        if (t->b.samples > 1) { logf_limited(&nlog, 5, "nvn_gl: InvalidateTexels on a multisample texture: its pool memory is not rewritten\n"); return; }
        if (t->b.flags & 0x10) {
            int64_t stride = t->b.stride ? t->b.stride : (int64_t)t->b.width * f.bpb;
            if (f.kind == 4 || t->mem + stride * t->b.height > t->limit) return;
            glPixelStorei(0x0D02, (GLint)(stride / f.bpb));
            glGetTextureSubImage(t->o.gl, 0, 0, 0, 0, t->b.width, t->b.height, 1, fo, f.type, (GLsizei)(stride * t->b.height), t->mem);
            glPixelStorei(0x0D02, 0);
        } else {
            tex_lay(&t->b, &L);
            if (level >= L.levels || level >= t->glevels) return;
            for (int layer = 0; layer < L.layers; layer++) {
                uint8_t *dst = (uint8_t *)(uintptr_t)tex_level_ptr(&L, t->mem, level, layer);
                size_t lin = L.lvl_linear_size[level];
                if (t->limit && dst + L.lvl_size[level] > t->limit) return;
                if (lin > tmp_cap) { tmp_cap = lin * 2; tmp = realloc(tmp, tmp_cap); }
                int is3d = t->b.target == 2;
                if (f.kind == 4) glGetCompressedTextureSubImage(t->o.gl, level, 0, 0, is3d ? 0 : layer, L.lvl_w[level], L.lvl_h[level], is3d ? L.lvl_d[level] : 1, (GLsizei)lin, tmp);
                else if (t->rw && !is3d) rs_get(t, level, 0, 0, L.lvl_w[level], L.lvl_h[level], fo, f.type, (GLsizei)lin, tmp);      /* resolution scale */
                else glGetTextureSubImage(t->o.gl, level, 0, 0, is3d ? 0 : layer, L.lvl_w[level], L.lvl_h[level], is3d ? L.lvl_d[level] : 1, fo, f.type, (GLsizei)lin, tmp);
                tex_swizzle_level(&L, level, tmp, dst);
            }
        }
        t->hash = mem_hash(t->mem, t->data_size, t->ww_hashed || (t->b.flags & 0x10) != 0);
    }
    GLenum e = glGetError();
    if (e) logf_limited(&nlog, 10, "nvn_gl: reading texels of texture %dx%dx%d format 0x%x failed: GL error 0x%x\n", t->b.width, t->b.height, t->b.depth, t->b.format, e);
}
static void tex_read_texels(Ctx *c) {                   /* (texture, view, region, out) [, rowStride, imageStride] */
    ReadJob j;
    memset(&j, 0, sizeof j);
    j.tex = hobj(A0, K_TEX); j.out = P(A3);
    if (!j.tex || !A2 || !A3) return;
    if (A1) { memcpy(&j.view, P(A1), sizeof j.view); j.has_view = 1; }
    memcpy(j.region, P(A2), 24);
    if (strstr(hostfn_current_name(), "Strided")) { j.row_stride = (int64_t)A4; j.image_stride = (int64_t)A5; }
    gl_run(g_rb->read_texels, &j);
    {   static int copylog = -1; if (copylog < 0) copylog = getenv("TOMO_COPYLOG") != NULL;
        if (copylog) printf("read texels: texture %dx%d format 0x%x region %d,%d %dx%d from %llx: %08x %08x\n", j.tex->b.width, j.tex->b.height, j.tex->b.format,
                            j.region[0], j.region[1], j.region[3], j.region[4], (unsigned long long)c->x[30] - 4, rd32(A3), rd32(A3 + 4));
    }
}
static void tex_invalidate_texels(Ctx *c) {             /* (texture, view, region) */
    ReadJob j;
    memset(&j, 0, sizeof j);
    j.tex = hobj(A0, K_TEX); j.to_pool = 1;
    if (!j.tex) return;
    if (A1) { memcpy(&j.view, P(A1), sizeof j.view); j.has_view = 1; }
    gl_run(g_rb->read_texels, &j);
}
static void view_get_levels(Ctx *c) { if (A1) wr32(A1, (uint32_t)TV->lvl0); if (A2) wr32(A2, (uint32_t)TV->nlvl); RET((TV->set & 0x81) == 0x81); }
static void view_get_layers(Ctx *c) { if (A1) wr32(A1, (uint32_t)TV->lay0); if (A2) wr32(A2, (uint32_t)TV->nlay); RET((TV->set & 0x82) == 0x82); }
static void view_get_format(Ctx *c) { if (A1) wr32(A1, TV->format); RET((TV->set & 0x84) == 0x84); }
static void view_get_swizzle(Ctx *c) { for (int i = 0; i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], TV->swz[i]); RET((TV->set & 0x88) == 0x88); }
static void view_get_dsmode(Ctx *c) { if (A1) wr32(A1, (TV->set >> 6) & 1); RET((TV->set & 0x90) == 0x90); }
static void view_get_target(Ctx *c) { if (A1) wr32(A1, TV->target); RET((TV->set & 0xa0) == 0xa0); }
static void view_compare(Ctx *c) { RET(A0 && A1 && !memcmp(P(A0), P(A1), sizeof(View))); }
static void tpool_finalize(Ctx *c) {
    gl_barrier();
    TexPool *tp = hobj(A0, K_TPOOL);
    if (!tp) return;
    AcquireSRWLockExclusive(&g_pool_lock);
    for (int i = 0; i < tp->count; i++) if (tp->e[i].tex) { InterlockedDecrement(&tp->e[i].tex->pool_refs); tp->e[i].tex = NULL; }
    tp->count = 0;                                      /* the record itself stays: recorded SetTexturePool commands may still name it */
    ReleaseSRWLockExclusive(&g_pool_lock);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
}
static void tpool_get_pool(Ctx *c) { TexPool *tp = hobj(A0, K_TPOOL); RET(tp ? tp->mempool : 0); }
static void tpool_get_offset(Ctx *c) { TexPool *tp = hobj(A0, K_TPOOL); RET(tp ? tp->offset : 0); }
static void tpool_get_size(Ctx *c) { TexPool *tp = hobj(A0, K_TPOOL); RET(tp ? tp->count : 0); }

/* ---- programs */
static void prog_sample_shading(Ctx *c) { Prog *p = hobj(A0, K_PROG); if (p) p->sample_shading = (int)A1; }    /* 0 as the shader says, 1 forced on, 2 forced off */

/* ---- window */
static void wb_defaults(Ctx *c) { memset(P(A0), 0, 0x40); wr8(A0 + 4, 1); }
static void wb_native(Ctx *c) { wr64(A0 + 56, A1); }
static void wb_interval(Ctx *c) { wr8(A0 + 4, (uint8_t)A1); }
static void wb_num_active(Ctx *c) { wr8(A0 + 5, (uint8_t)A1); }
static void wb_get_native(Ctx *c) { RET(rd64(A0 + 56)); }
static void wb_get_interval(Ctx *c) { RET(rd8(A0 + 4)); }
static void wb_get_num_textures(Ctx *c) { RET(rd32(A0) & 0xff); }
static void wb_get_num_active(Ctx *c) { RET(rd8(A0 + 5) ? rd8(A0 + 5) : (rd32(A0) & 0xff)); }
static void wb_get_texture(Ctx *c) { RET((uint32_t)A1 < 6 ? rd64(A0 + 8 + (A1 & 7) * 8) : 0); }
static void win_finalize(Ctx *c) { gl_barrier(); Window *w = hobj(A0, K_WIN); if (w) { wr32(A0 + 8, 0); w->count = 0; } nvn_label_drop(A0); }
static void win_set_interval(Ctx *c) {
    Window *w = hobj(A0, K_WIN);
    void display_rate_site(uint64_t lr);
    display_rate_site(c->x[30]);                     /* display.c: the game's frame-rate controller (frame_rate = 60) */
    if (w) { w->interval = (int)A1; display_set_interval((int)A1); }
}
static void win_get_interval(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? w->interval : 1); }
static void win_get_native(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? w->native : 0); }
static void win_get_num_textures(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? w->count : 0); }
static void win_get_num_active(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? (w->nactive ? w->nactive : w->count) : 0); }
static void win_set_num_active(Ctx *c) { Window *w = hobj(A0, K_WIN); if (w && (int)A1 >= 1 && (int)A1 <= w->count) w->nactive = (int)A1; }
static void win_get_crop(Ctx *c) {                      /* (window, NVNrectangle *): the crop set, or the whole texture */
    Window *w = hobj(A0, K_WIN);
    if (!w || !A1) return;
    if (w->crop[2] > 0 && w->crop[3] > 0) memcpy(P(A1), w->crop, 16);
    else { int r[4] = {0, 0, w->count && w->tex[0] ? w->tex[0]->b.width : 0, w->count && w->tex[0] ? w->tex[0]->b.height : 0}; memcpy(P(A1), r, 16); }
}

/* ---- command buffers: state kept while recording */
static void cmd_set_callback(Ctx *c) { cb_get(A0)->callback = A1; }
static void cmd_set_callback_data(Ctx *c) { cb_get(A0)->callback_data = A1; }
static void cmd_get_callback(Ctx *c) { RET(cb_get(A0)->callback); }
static void cmd_get_callback_data(Ctx *c) { RET(cb_get(A0)->callback_data); }
static void cmd_callback_enabled(Ctx *c) { cb_get(A0)->callback_off = !(A1 & 1); }
static void cmd_get_command_used(Ctx *c) { RET(cb_get(A0)->cmd_used); }
static void cmd_get_control_used(Ctx *c) { RET(cb_get(A0)->ctrl_used); }
static void cmd_get_command_free(Ctx *c) { CmdBuf *cb = cb_get(A0); RET(cb->cmd_size > cb->cmd_used ? cb->cmd_size - cb->cmd_used : 0); }
static void cmd_get_control_free(Ctx *c) { CmdBuf *cb = cb_get(A0); RET(cb->ctrl_size > cb->ctrl_used ? cb->ctrl_size - cb->ctrl_used : 0); }
static void cmd_get_row_stride(Ctx *c) { RET(cb_get(A0)->row_stride); }
static void cmd_get_image_stride(Ctx *c) { RET(cb_get(A0)->image_stride); }
static void cmd_image_stride(Ctx *c) { cb_get(A0)->image_stride = (int64_t)A1; rec(c, OP_IMAGESTRIDE, 0); }
static void cmd_depth_bounds(Ctx *c) { float v[2] = {F0, F1}; Cmd *m = rec(c, OP_DEPTHBOUNDS, 0); memcpy(&m->a[1], v, 8); }       /* (cmdbuf, enable, near [v0], far [v1]) */
static void cmd_cons_raster(Ctx *c) { rec(c, OP_CONSRASTER, 0); }
static void cmd_cons_dilate(Ctx *c) { float v = F0; Cmd *m = rec(c, OP_CONSDILATE, 0); memcpy(&m->a[0], &v, 4); }
static void cmd_subpixel_bias(Ctx *c) { rec(c, OP_SUBPIXEL, 0); }
static void cmd_viewport_swizzles(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_VPSWIZZLE, (size_t)n * 16); if (n > 0 && src) memcpy(BLOB(m), P(src), (size_t)n * 16); }
static void cmd_tess_levels(Ctx *c) {                   /* (cmdbuf, const float *levels): 2 inner or 4 outer */
    int outer = strstr(hostfn_current_name(), "Outer") != NULL;
    uint64_t src = A1;
    Cmd *m = rec(c, OP_TESSLEVELS, 16);
    m->a[0] = (uint64_t)outer;
    memset(BLOB(m), 0, 16);
    if (src) memcpy(BLOB(m), P(src), outer ? 16 : 8);
}
static void cmd_barrier(Ctx *c) { rec(c, OP_BARRIER, 0); }
static void cmd_discard_color(Ctx *c) { rec(c, OP_DISCARD, 0)->a[0] = A1 & 7; }       /* (cmdbuf, colour target index) */
static void cmd_discard_ds(Ctx *c) { rec(c, OP_DISCARD, 0)->a[0] = ~0ull; }
static void cmd_clear_color_typed(Ctx *c) {             /* ClearColori / ClearColorui (cmdbuf, index, const int *, mask) */
    uint64_t src = A2;
    int kind = strstr(hostfn_current_name(), "ui") ? 2 : 1;
    Cmd *m = rec(c, OP_CLEARCOLOR, 16);
    m->a[5] = (uint64_t)kind;
    if (src) memcpy(BLOB(m), P(src), 16); else memset(BLOB(m), 0, 16);
}
static void cmd_clear_texture(Ctx *c) {                 /* (cmdbuf, texture, view, region, color, mask); the i / ui variants pass integers */
    uint64_t view = A2, region = A3, color = A4, mask = A5;
    const char *name = hostfn_current_name();
    int kind = strstr(name, "Textureui") ? 2 : strstr(name, "Texturei") ? 1 : 0;
    Tex *t = hobj(A1, K_TEX);
    Cmd *m = rec(c, OP_CLEARTEX, 64);
    m->a[0] = (uint64_t)(uintptr_t)t; m->a[4] = mask; m->a[5] = (uint64_t)kind;
    memset(BLOB(m), 0, 64);
    if (view) memcpy(BLOB(m), P(view), sizeof(View));
    if (region) memcpy(BLOB(m) + 24, P(region), 24); else m->a[6] = 1;
    if (color) memcpy(BLOB(m) + 48, P(color), 16);
}
static void cmd_downsample_view(Ctx *c) {               /* (cmdbuf, src, srcView, dst, dstView) */
    uint64_t sv = A2, dv = A4;
    Tex *s = hobj(A1, K_TEX), *d = hobj(A3, K_TEX);
    Cmd *m = rec(c, OP_DOWNSAMPLE_VIEW, 48);
    m->a[0] = (uint64_t)(uintptr_t)s; m->a[1] = (uint64_t)(uintptr_t)d;
    memset(BLOB(m), 0, 48);
    if (sv) memcpy(BLOB(m), P(sv), sizeof(View));
    if (dv) memcpy(BLOB(m) + 24, P(dv), sizeof(View));
}
static void cmd_draw_arrays_indirect(Ctx *c) { Cmd *m = rec(c, OP_DRAW_INDIRECT, 0); m->a[6] = 0; m->a[4] = 1; m->a[5] = 0; m->a[3] = m->a[1]; m->a[1] = 0; m->a[2] = 0; }      /* (cmdbuf, mode, indirect) */
static void cmd_draw_elements_indirect(Ctx *c) { Cmd *m = rec(c, OP_DRAW_INDIRECT, 0); m->a[6] = 1; m->a[4] = 1; m->a[5] = 0; }   /* (cmdbuf, mode, type, indexBuffer, indirect) */
static void cmd_multi_draw_arrays_indirect(Ctx *c) {    /* (cmdbuf, mode, indirect, parameter, maxDrawCount, stride) */
    uint64_t indirect = A2, param = A3, maxn = A4, stride = A5;
    Cmd *m = rec(c, OP_DRAW_INDIRECT, 8);
    m->a[1] = 0; m->a[2] = 0; m->a[3] = indirect; m->a[4] = maxn; m->a[5] = stride; m->a[6] = 2;
    memcpy(BLOB(m), &param, 8);
}
static void cmd_multi_draw_elements_indirect(Ctx *c) {  /* (cmdbuf, mode, type, indexBuffer, indirect, parameter, maxDrawCount, stride) */
    uint64_t indirect = A4, param = A5, maxn = A6, stride = A7, index = A3;
    Cmd *m = rec(c, OP_DRAW_INDIRECT, 8);
    m->a[2] = index; m->a[3] = indirect; m->a[4] = maxn; m->a[5] = stride; m->a[6] = 3;
    memcpy(BLOB(m), &param, 8);
}
static void cmd_dispatch_indirect(Ctx *c) { cb_get(A0)->dispatches++; rec(c, OP_DISPATCH_INDIRECT, 0); }       /* (cmdbuf, address of three group counts) */
static void condlog(Ctx *c, const char *what) {         /* TEST ONLY, TOMO_CONDLOG=1: the game code behind conditional rendering and counter reports */
    static int on = -1; static uint64_t seen[64]; static int n;
    if (on < 0) on = getenv("TOMO_CONDLOG") != NULL;
    if (!on) return;
    for (int i = 0; i < n; i++) if (seen[i] == c->x[30]) return;
    if (n < 64) seen[n++] = c->x[30];
    printf("condlog: %s lr %llx args %llx %llx %llx%c", what, (unsigned long long)c->x[30], (unsigned long long)A1, (unsigned long long)c->x[2], (unsigned long long)c->x[3], 10);
}
static void cmd_render_enable(Ctx *c) { condlog(c, "SetRenderEnable"); Cmd *m = rec(c, OP_RENDER_ENABLE, 0); m->a[1] = 0; m->a[2] = 0; }                      /* (cmdbuf, enable) */
static void cmd_render_enable_cond(Ctx *c) { condlog(c, "SetRenderEnableConditional"); Cmd *m = rec(c, OP_RENDER_ENABLE, 0); m->a[2] = m->a[1]; m->a[1] = m->a[0] + 1; }   /* (cmdbuf, mode, counter address) */
static void cmd_reset_counter(Ctx *c) { condlog(c, "ResetCounter"); rec(c, OP_COUNTER_RESET, 0); }
static void cmd_report_counter(Ctx *c) { condlog(c, "ReportCounter"); rec(c, OP_COUNTER_REPORT, 0); }
static void cmd_report_value(Ctx *c) { rec(c, OP_REPORT_VALUE, 0); }
static void cmd_fence_sync(Ctx *c) { SyncObj *o = hobj(A1, K_SYNC); rec(c, OP_FENCE, 0)->a[0] = (uint64_t)(uintptr_t)o; }      /* (cmdbuf, sync, condition, flags) */
static void cmd_wait_sync(Ctx *c) { SyncObj *o = hobj(A1, K_SYNC); rec(c, OP_WAITSYNC, 0)->a[0] = (uint64_t)(uintptr_t)o; }
static void cmd_signal_event(Ctx *c) { uint64_t a = A1 ? rd64(A1 + 16) : 0; rec(c, OP_EVENT_SIGNAL, 0)->a[0] = a; }            /* (cmdbuf, event, mode, location, flags, value) */
static void cmd_wait_event(Ctx *c) { uint64_t a = A1 ? rd64(A1 + 16) : 0; rec(c, OP_EVENT_WAIT, 0)->a[0] = a; }                /* (cmdbuf, event, waitMode, value) */
/* nvnCommandBufferDrawTexture(cmdbuf, textureHandle, dstRegion, srcRegion) [sdk 0x438210]: see nvn_c4.c. The handle is the one
 * nvnDeviceGetTextureHandle gives (texture ID | sampler ID << 20, bit 32 set). An empty destination records nothing, as the
 * real encoder writes nothing [0x43827c]. */
static void cmd_draw_texture(Ctx *c) {
    uint64_t h = A1, dst = A2, src = A3;
    float d[4], s[4];
    NvnC4DrawTexture w;
    memcpy(d, P(dst), 16); memcpy(s, P(src), 16);
    if (!nvn_c4_draw_texture_words(d, s, &w)) return;
    w.src_texture = (uint32_t)h & 0xfffff; w.src_sampler = (uint32_t)h >> 20;   /* [sdk 0x4382f0, 0x4382f4] */
    Cmd *m = rec(c, OP_DRAW_TEXTURE, sizeof w);
    m->a[0] = h;
    memcpy(BLOB(m), &w, sizeof w);
}
static void cmd_covmod_table(Ctx *c) { uint64_t src = A1; Cmd *m = rec(c, OP_COVMODTABLE, 64); m->a[0] = src != 0; if (src) memcpy(BLOB(m), P(src), 64); }
static void rec_debug(Ctx *c, int kind, uint64_t id, uint64_t text) {
    size_t n = text ? strnlen(P(text), 255) : 0;
    Cmd *m = rec(c, OP_DEBUG, n + 1);
    m->a[0] = (uint64_t)kind; m->a[1] = id;
    if (n) memcpy(BLOB(m), P(text), n);
    BLOB(m)[n] = 0;
}
static void cmd_push_group(Ctx *c) { rec_debug(c, 0, A1, A2); }               /* (cmdbuf, domain, description) */
static void cmd_pop_group(Ctx *c) { rec_debug(c, 1, 0, 0); }
static void cmd_marker(Ctx *c) { rec_debug(c, 2, 0, A1); }                    /* (cmdbuf, description) */
static void cmd_marker_id(Ctx *c) { rec_debug(c, 2, A1, A2); }                /* (cmdbuf, domain, description) */
static void cmd_set_label(Ctx *c) { cb_get(A0); any_set_label(c); }

static void job_finalize_handle(void *arg) {
    uint64_t h = *(uint64_t *)arg;
    uint32_t i = (uint32_t)h - 1;
    AcquireSRWLockExclusive(&g_list_lock);
    if (h && i < (uint32_t)g_list_count && g_lists[i].data && (g_lists[i].gen & 0x7fffffff) == (uint32_t)(h >> 32)) g_lists[i].finalized = 1;
    ReleaseSRWLockExclusive(&g_list_lock);
}
static void dev_finalize_command_handle(Ctx *c) {       /* (device, handle): the recorded commands of that handle are released (by the render thread, at the next present) */
    uint64_t h = A1;
    { int r1_ra_phase(void); if (r1_ra_phase() == 1) return; }      /* R1 round 5, run-ahead: a speculative pass's release is undone by the restore (the real pass asks again); its own lists expire unused */
    if (gl_async_on() && g_async_pending) gl_post(job_finalize_handle, &h, sizeof h);     /* after the submits already queued, which may still use it */
    else job_finalize_handle(&h);
}
/* ================================================================ V1 step S0: the render thread's jobs in Vulkan mode
 * (plan\phases\V1_vulkan.md, section 1). Every job the entry points above hand to the render thread arrives here when
 * SSBU_RENDERER=vk. A job nvn_vk.c implements is done there; one it does not implement yet stops the game with the name
 * of the NVN operation (nvn_vk_missing; SSBU_VK_MISSING=warn: reported once, counted, skipped). A job that is not one
 * of this file's (nvn_vk.c's own, through nvn_render_run) runs as it is. Step S2 replaces this table by the RBackend
 * interface. */
void nvn_render_run(void (*fn)(void *), void *arg) { gl_run(fn, arg); }
/* PR #41: pool / buffer finalization may be followed at once by a decommit or reuse of the guest's storage. A queue barrier
 * alone is not enough on Vulkan: kept compute buffers and asynchronous landings outlive the jobs that used them. */
static void job_rb_land_range(void *arg) { Range *r = arg; if (g_rbp_n) rb_land_range(r->addr, r->size); }   /* read-backs must not land in memory the game released */
void nvn_front_memory_retire(uint64_t addr, uint64_t size) {
    if (!addr || !size || !g_render_tid) return;
    if (g_rb == &rb_vk) { Range range = {addr, size}; gl_run(nvn_vk_memory_retire, &range); }
    else { static int on = -1; if (on < 0) { const char *e = getenv("TOMO_RETIRE_BARRIER"); on = e ? atoi(e) : 0; } if (on) gl_barrier();
        int hit = 0;                               /* a hint read from this thread: after the barrier only the render thread changes the list */
        for (int i = 0; i < g_rbp_n && i < 64 && !hit; i++) hit = addr < g_rbp[i].addr + g_rbp[i].size && g_rbp[i].addr < addr + size;
        if (hit) { Range range = {addr, size}; gl_run(job_rb_land_range, &range); } }   /* OpenGL: no barrier by default since 2026-10-23 (TOMO_RETIRE_BARRIER=1 restores it): the game frees storage only after its fences (nvnSyncWait), which pass through the render thread, and read-backs over the freed range land first (above); the barrier made the game wait 18 ms a frame for the render thread (house close-up 20 -> 25 fps). The sea foam that once drew wrong without it looked the same in a side-by-side check. */
}
/* Seam step S2: the OpenGL back end as an RBackend (nvn_front.h); rb_vk is in nvn_vk_draw.c. */
static void rb_gl_idle(void) { gl_housekeeping(); if (g_rbp_n) rb_land(0); dec_land(); }     /* read-backs land as soon as the GPU is done (the game reads some results without a fence: late data made the island's shadows jump) */
const RBackend rb_gl = {"gl", job_submit, job_submit_async, job_present, job_present_movie, job_fence, job_sync_wait, job_server_wait, job_sync_drop, job_flush, job_finish,
                        job_write_texels, job_read_texels, job_housekeeping, job_reset_status, job_grid, rb_gl_idle};

/* ---- front-end services for the Vulkan back end (nvn_front.h). Until step S3 splits this file they are thin doors to
 * the statics above; nothing here calls OpenGL. Render thread only, except where the callee has its own lock. */
int nvn_front_fmt(int nvn_format, Fmt *f) { return fmt_get(nvn_format, f); }
uint8_t *nvn_front_list(uint64_t handle, size_t *len) { return list_lookup(handle, len); }
/* Track V session 4, step-aligned comparison of the two renderers: with SSBU_SHOT_STEPS=1 the scripted input (nnhid.c)
 * asks for a screenshot when a key step of SSBU_SEQ begins; the next presented frame is saved as step_<n>.bmp in
 * SSBU_SHOT_DIR by whichever back end runs. Counted in input polls, so both renderers save the same game frame. */
static volatile LONG g_step_shot;
void nvn_front_shot_step(int step) { static int on = -1; if (on < 0) on = getenv("SSBU_SHOT_STEPS") != NULL; if (on) InterlockedExchange(&g_step_shot, step + 1); }
int nvn_front_take_step_shot(void) { return g_step_shot ? (int)InterlockedExchange(&g_step_shot, 0) : 0; }
void nvn_front_batch_begin(void) { g_serial++; }
void nvn_front_barrier_texmem(void) { g_serial++; }
int nvn_front_tex_layers(const Tex *t) { return tex_layers(&t->b); }
int nvn_front_tex_levels(const Tex *t) {
    const TexBuilder *b = &t->b;
    int w = b->width > 0 ? b->width : 1, h = b->height > 0 ? b->height : 1, d = b->depth > 0 ? b->depth : 1, levels = b->levels > 0 ? b->levels : 1, maxdim = w > h ? w : h, maxl = 1;
    if (b->target == 2 && d > maxdim) maxdim = d;
    while ((maxdim >> maxl) > 0) maxl++;
    return b->samples > 1 ? 1 : levels > maxl ? maxl : levels;
}
/* The decision of tex_sync without its OpenGL calls (the same rule, the same fields of Tex). */
int nvn_front_tex_changed(Tex *t) {
    Fmt f;
    if (t->checked == g_serial) return 0;
    t->checked = g_serial;
    const uint8_t *src = tex_source(t);
    if (t->gpu_written || !src || t->b.samples > 1 || !fmt_get(t->b.format, &f) || (f.kind >= 1 && f.kind <= 3)) return 0;
    if (!t->b.packaged && t->limit && src + t->data_size > t->limit) return 0;
    if (t->b.packaged && t->uploaded) return 0;
    LONG64 pf0 = pf_now();
    uint64_t newest = ww_newest(src, t->data_size);
    if (newest) {
        if (t->uploaded && !t->dirty && t->ww_hashed && newest <= t->ww_seen) { g_ww_clean++; pf_add(PF_TEXHASH, pf0); return 0; }
        t->ww_seen = newest;
        g_ww_dirty++;
    } else g_ww_unwatched++;
    uint64_t h = mem_hash(src, t->data_size, newest != 0 || (t->b.flags & 0x10) != 0);
    int same = t->uploaded && !t->dirty && h == t->hash && (!newest || t->ww_hashed);
    t->ww_hashed = newest != 0;
    pf_add(PF_TEXHASH, pf0); g_pf_hash_bytes += (LONG64)t->data_size;
    if (same) return 0;
    t->hash = h; t->uploaded = 1; t->dirty = 0;
    g_st.tex_uploads++;
    return 1;
}
/* tex_upload_memory with the texels handed to the caller instead of glTexSubImage. */
int nvn_front_tex_read(Tex *t, int decode, nvn_front_put_fn *put) {
    static uint8_t *tmp;
    static size_t tmp_cap;
    Fmt f;
    TexBuilder *b = &t->b;
    const uint8_t *src = tex_source(t);
    if (!src || !fmt_get(b->format, &f)) return 0;
    if (b->flags & 0x10) {                         /* linear: plain rows with a stride */
        int64_t stride = b->stride ? b->stride : (int64_t)b->width * f.bpb;
        if (f.kind == 4) return 0;
        put(t, 0, 0, b->width, b->height, 1, src, (size_t)stride * (size_t)b->height, 0, stride);
        g_st.tex_bytes += stride * b->height;
        tex_release_packaged(t);
        return 1;
    }
    TexLayout L;
    tex_lay(b, &L);
    int levels = nvn_front_tex_levels(t);
    for (int layer = 0; layer < L.layers; layer++) {
        for (int l = 0; l < levels && l < L.levels; l++) {
            const uint8_t *p = tex_level_ptr(&L, src, l, layer);
            size_t lin = L.lvl_linear_size[l], need = lin + (decode ? (size_t)L.lvl_w[l] * L.lvl_h[l] * 4 : 0);
            if (!b->packaged && t->limit && p + L.lvl_size[l] > t->limit) return 0;
            if (need > tmp_cap) { tmp_cap = need * 2; tmp = realloc(tmp, tmp_cap); }
            tex_deswizzle_level(&L, l, p, tmp);
            if (decode) {
                tex_decode_rgba8(b->format, tmp, L.lvl_w[l], L.lvl_h[l], tmp + lin);
                put(t, l, layer, L.lvl_w[l], L.lvl_h[l], 1, tmp + lin, (size_t)L.lvl_w[l] * L.lvl_h[l] * 4, 1, 0);
            } else put(t, l, layer, L.lvl_w[l], L.lvl_h[l], L.lvl_d[l], tmp, lin, 0, 0);
            g_st.tex_bytes += (LONG64)lin;
        }
    }
    tex_release_packaged(t);
    return 1;
}
int nvn_front_handle(TexPool *tp, SamplerPool *sp, uint64_t nh, Tex **t, View *v, SamplerParams *p) {
    uint32_t tid = (uint32_t)nh & 0xfffff, sid = (uint32_t)(nh >> 20) & 0xfff;
    int r = 0;
    *t = NULL;
    if (!nh || !tp) return 0;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)tp->count) { *t = tp->e[tid].tex; *v = tp->e[tid].view; }
    if (sp && sid < (uint32_t)sp->count && sp->used[sid]) { *p = sp->e[sid]; r |= 2; }
    ReleaseSRWLockShared(&g_pool_lock);
    if (*t && !(*t)->dead) r |= 1; else *t = NULL;
    return r;
}
long long nvn_front_frame_next(void) { return (long long)g_st.frames + 1; }
/* V1 session 5: compute outputs of the two back ends compared OFFLINE (tests/vk5_ccmp.py). SSBU_COMPUTE_CMP=<folder>: every
 * dispatch is named by a hash of what it reads from guest memory (machine code, group counts, every bound uniform buffer,
 * every bound storage buffer it does not write; of the written ones the size); when what it wrote has landed in guest memory
 * the written ranges go to <folder>/<hash>_<storage index>.bin (the first SSBU_COMPUTE_CMP_N distinct hashes, default 600).
 * Two runs (vk, gl) that met the same inputs wrote files of the same name. Render thread only. */
static struct { uint64_t h, addr, size; int n; } g_ccmp_pend[512];
static int g_ccmp_npend, g_ccmp_on = -1, g_ccmp_left;
static uint64_t g_ccmp_seen[4096];
static const char *g_ccmp_dir;
static uint64_t ccmp_hash(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ULL; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL;
    return h;
}
void nvn_front_ccmp_dispatch(const Range *ubo, const Range *ssbo, uint32_t written, const uint8_t *code, int code_len, unsigned gx, unsigned gy, unsigned gz) {
    if (g_ccmp_on < 0) { g_ccmp_dir = getenv("SSBU_COMPUTE_CMP"); g_ccmp_on = g_ccmp_dir != NULL; g_ccmp_left = getenv("SSBU_COMPUTE_CMP_N") ? atoi(getenv("SSBU_COMPUTE_CMP_N")) : 600; if (g_ccmp_on) CreateDirectoryA(g_ccmp_dir, NULL); }
    if (!g_ccmp_on || g_ccmp_left <= 0 || !code || code_len <= 0) return;
    uint32_t g[3] = {gx, gy, gz};
    uint64_t h = ccmp_hash(0xcbf29ce484222325ULL, code, (size_t)code_len);
    h = ccmp_hash(h, g, sizeof g);
    for (int n = 0; n < 16; n++) if (ubo[n].addr && ubo[n].size) { h = ccmp_hash(h ^ (uint64_t)(n + 1), P(ubo[n].addr), (size_t)ubo[n].size); }
    for (int n = 0; n < 32; n++) {
        if (!ssbo[n].addr || !ssbo[n].size) continue;
        /* SSBU_COMPUTE_CMP_MODE=1: storage buffers enter by SIZE only, written or not. Needed for skinning program #16: its
         * assembly text (OpenGL) declares storage 2 written, the translated machine code (Vulkan) 0, 1 and 2, so the
         * default naming never matches between the two. A false match (same sizes, other contents) shows as a difference. */
        static int mode = -1;
        if (mode < 0) mode = getenv("SSBU_COMPUTE_CMP_MODE") ? atoi(getenv("SSBU_COMPUTE_CMP_MODE")) : 0;
        if (mode == 1 || (written >> n & 1)) h = ccmp_hash(h ^ (uint64_t)(n + 101), &ssbo[n].size, 8);
        else h = ccmp_hash(h ^ (uint64_t)(n + 201), P(ssbo[n].addr), (size_t)ssbo[n].size);
    }
    unsigned slot = (unsigned)(h >> 17) & 4095;
    if (g_ccmp_seen[slot] == h) return;
    g_ccmp_seen[slot] = h;
    g_ccmp_left--;
    for (int n = 0; n < 32; n++) if ((written >> n & 1) && ssbo[n].addr && ssbo[n].size && g_ccmp_npend < 512) { g_ccmp_pend[g_ccmp_npend].h = h; g_ccmp_pend[g_ccmp_npend].addr = ssbo[n].addr; g_ccmp_pend[g_ccmp_npend].size = ssbo[n].size; g_ccmp_pend[g_ccmp_npend++].n = n; }
}
void nvn_front_ccmp_landed(void) {
    for (int i = 0; i < g_ccmp_npend; i++) {
        char path[400];
        snprintf(path, sizeof path, "%s/%016llx_%d.bin", g_ccmp_dir, (unsigned long long)g_ccmp_pend[i].h, g_ccmp_pend[i].n);
        FILE *fp = fopen(path, "wb");
        if (fp) { fwrite(P(g_ccmp_pend[i].addr), 1, (size_t)g_ccmp_pend[i].size, fp); fclose(fp); }
    }
    g_ccmp_npend = 0;
}
long long nvn_front_frame_end(long long draws, long long skipped, long long clears) {
    LONG64 frame = g_st.frames + 1;
    pf_frame_mark(frame);
    g_joblog_cur = frame;
    g_st.frames = frame;
    if (frame <= 3 || frame % 600 == 0 || draws != 0 && g_logn < 40 && (g_logn++, 1))
        printf("nvn_vk: frame %lld: %lld draws (%lld skipped), %lld clears | totals: %lld lists, %lld textures, %lld uploads (%lld MB)\n", (long long)frame, draws, skipped, clears,
               (long long)g_st.lists, (long long)g_st.tex_created, (long long)g_st.tex_uploads, (long long)(g_st.tex_bytes >> 20));
    {   static ULONGLONG t0;
        static LONG64 d0;
        if (frame % 300 == 0) {
            ULONGLONG now = GetTickCount64();
            if (t0 && now > t0) printf("nvn_vk: frames %lld..%lld: %.1f fps, %lld draws per frame\n", (long long)frame - 300, (long long)frame, 300000.0 / (double)(now - t0), (long long)(g_st.draws - d0) / 300);
            t0 = now; d0 = g_st.draws;
        }
    }
    fflush(stdout);
    g_prev_frame_draws = draws;
    input_frame_draws((int)draws);
    list_collect();
    return (long long)frame;
}
void nvn_front_take_dead(void (*drop)(int kind, void *host)) {
    if (!g_dead_n) return;
    EnterCriticalSection(&g_dead_lock);
    for (int i = 0; i < g_dead_n; i++) {
        drop(g_dead[i].kind, g_dead[i].p);
        if (g_dead[i].kind == K_TEX) tex_release_packaged(g_dead[i].p);
        if (g_dead[i].kind == K_PROG) { Prog *p = g_dead[i].p; for (int s = 0; s < 6; s++) { free(p->st[s].text); memset(&p->st[s], 0, sizeof p->st[s]); } }
    }
    g_dead_n = 0;
    LeaveCriticalSection(&g_dead_lock);
}
void nvn_front_count(int what, long long n) {
    switch (what) { case 0: g_st.draws += n; break; case 1: g_st.draws_skipped += n; break; case 2: g_st.clears += n; break; case 3: g_st.submits += n; break; case 5: g_st.tex_created += n; break; }
}
int nvn_front_clip_modes(void) { return g_origin_mode | g_depth_mode << 8; }
const char *nvn_front_op_name(unsigned op) { return k_op_name[op < OP_COUNT ? op : 0]; }
void *nvn_front_hwnd(void) { return g_hwnd; }
static void job_vk_shutdown(void *arg) { (void)arg; nvn_vk_shutdown(); }
/* At exit (nvn_gl_report): the render thread destroys the Vulkan objects. It is given 3 s: a render thread that is held
 * in a job at that moment is left alone (the process is ending) and that is said. */
static void vk_shutdown_at_exit(void) {
    static Job j;
    if (GetCurrentThreadId() == g_render_tid) { nvn_vk_shutdown(); return; }
    j.fn = job_vk_shutdown; j.arg = NULL; j.next = NULL; j.done = CreateEventA(NULL, FALSE, FALSE, NULL);
    EnterCriticalSection(&g_job_lock);
    if (g_job_tail) g_job_tail->next = &j; else g_job_head = &j;
    g_job_tail = &j;
    LeaveCriticalSection(&g_job_lock);
    SetEvent(g_job_event);
    /* Session 13: the event is set when the job has FINISHED, not when it is taken, and the job writes the pipeline cache
     * (0.3-0.7 s read-out + 1-2 s file for 0.9 GB, more on a busy PC): 3 s was too short and the old line ("did not take
     * the job") was printed while the shutdown was running; the process then left with the cache half written. Now: 60 s. */
    if (WaitForSingleObject(j.done, 3000) != WAIT_OBJECT_0) {
        LONG64 t0 = GetTickCount64();
        printf("nvn_vk: shutdown still running after 3 s (the pipeline cache is being written); waiting for it, at most 60 s\n"); fflush(stdout);
        if (WaitForSingleObject(j.done, 60000) != WAIT_OBJECT_0) printf("nvn_vk: shutdown NOT finished after 63 s: the render thread is held or never took the job\n");
        else printf("nvn_vk: shutdown finished %.1f s after the first 3 s\n", (double)(GetTickCount64() - t0) / 1000.0);
        fflush(stdout);
    }
}

/* nvnBufferFlushMappedRange / nvnMemoryPoolFlushMappedRange (nvn_stub.c): the CPU wrote this memory. Textures stored there are read again. */
Tex *nvn_front_tex_first(void) { return g_all_tex; }
void nvn_gl_memory_written(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t end = tex_range_end(addr, size);
    AcquireSRWLockShared(&g_tex_range_lock);
    if (g_tex_range_fallback) {
        for (Tex *t = g_all_tex; t; t = t->all_next)
            if (!t->dead && t->mem && (uint64_t)(uintptr_t)t->mem < end && addr < tex_range_end((uint64_t)(uintptr_t)t->mem, t->storage)) t->dirty = 1;
    } else {
        size_t lo = 0, hi = g_tex_range_count;
        while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (g_tex_ranges[mid].prefix_end <= addr) lo = mid + 1; else hi = mid; }
        for (size_t i = lo; i < g_tex_range_count && g_tex_ranges[i].begin < end; i++)
            if (g_tex_ranges[i].end > addr) g_tex_ranges[i].tex->dirty = 1;
    }
    ReleaseSRWLockShared(&g_tex_range_lock);
}

/* nvnMemoryPoolInvalidateMappedRange / nvnBufferInvalidateMappedRange(object, offset, size): the CPU is about to read what the GPU
 * wrote there. Pool memory is host memory, but what was rendered into textures stored there lives in GL: it is written back
 * into the pool (as InvalidateTexels does), and storage buffers the GPU wrote go back too. The cursor's GPU pick reads its
 * result this way: as a no-op, nothing on the island map was ever under the cursor (A over a building did nothing). */
typedef struct { uint64_t addr, size; } InvalJob;
static void job_invalidate(void *arg) {
    InvalJob *j = arg;
    uint64_t end = j->addr + j->size;
    int n = 0, walked = 0;
    LONG64 t0 = pf_now(), t1;
    if (g_rbp_n) rb_land(1);
    if (g_cbuf_live) cbuf_flush_if(j->addr, j->size);
    t1 = pf_now();
    /* the live textures over the range, from the range index: walking every record ever made (56,000 on the island)
     * took 2.4 ms, twice a frame for the map's GPU pick */
    static Tex **cand;
    static size_t ccap;
    size_t nc = 0;
    AcquireSRWLockShared(&g_tex_range_lock);
    if (g_tex_range_fallback)
        for (Tex *t = g_all_tex; t; t = t->all_next) { if (nc == ccap) { ccap = ccap ? ccap * 2 : 64; cand = realloc(cand, ccap * sizeof *cand); } cand[nc++] = t; }
    else {
        size_t lo = 0, hi = g_tex_range_count;
        while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (g_tex_ranges[mid].prefix_end <= j->addr) lo = mid + 1; else hi = mid; }
        for (size_t i = lo; i < g_tex_range_count && g_tex_ranges[i].begin < end; i++)
            if (g_tex_ranges[i].end > j->addr) { if (nc == ccap) { ccap = ccap ? ccap * 2 : 64; cand = realloc(cand, ccap * sizeof *cand); } cand[nc++] = g_tex_ranges[i].tex; }
    }
    ReleaseSRWLockShared(&g_tex_range_lock);
    for (size_t k = 0; k < nc; k++, walked++) {         /* records are never freed: the pointers stay good */
        Tex *t = cand[k];
        if (t->dead || !t->mem || !t->gpu_written || t->b.packaged || !t->o.gl) continue;
        uint64_t a = (uint64_t)(uintptr_t)t->mem;
        if (a >= end || a + t->storage <= j->addr) continue;
        ReadJob r;
        memset(&r, 0, sizeof r);
        r.tex = t; r.to_pool = 1;
        job_read_texels(&r);
        n++;
    }
    static int log = -1; if (log < 0) log = getenv("TOMO_COPYLOG") != NULL;
    if (log) printf("invalidate: %llx+%llu, %d texture(s) written back (%d records walked; %.3f ms landing, %.3f ms walk and read)%c", (unsigned long long)j->addr, (unsigned long long)j->size, n, walked,
                    (double)(t1 - t0) / 1e4, (double)(pf_now() - t1) / 1e4, 10);
}
static void obj_invalidate_range(Ctx *c) {
    uint64_t nvn_obj_memory(uint64_t obj), m = A0 ? nvn_obj_memory(A0) : 0;
    if (!m || !A2) return;
    InvalJob j = {m + A1, A2};
    int hit = 0;                                     /* only a range with a rendered texture needs the render thread (buffers land at the batch end) */
    uint64_t end = tex_range_end(j.addr, j.size);
    AcquireSRWLockShared(&g_tex_range_lock);
    if (g_tex_range_fallback) hit = 1;
    else {
        size_t lo = 0, hi = g_tex_range_count;
        while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (g_tex_ranges[mid].prefix_end <= j.addr) lo = mid + 1; else hi = mid; }
        for (size_t i = lo; i < g_tex_range_count && g_tex_ranges[i].begin < end && !hit; i++)
            hit = g_tex_ranges[i].end > j.addr && g_tex_ranges[i].tex->gpu_written;
    }
    ReleaseSRWLockShared(&g_tex_range_lock);
    LONG64 w0 = pf_now();
    if (hit) gl_run(job_invalidate, &j);
    static int log = -1; if (log < 0) log = getenv("TOMO_COPYLOG") != NULL;
    if (log && hit) printf("invalidate: the game waited %.3f ms%c", (double)(pf_now() - w0) / 1e4, 10);
}
static const struct { const char *name; ImportFn fn; } k_nvn[] = {
    {"nvnDeviceSetWindowOriginMode", dev_origin_mode}, {"nvnDeviceSetDepthMode", dev_depth_mode}, {"nvnDeviceGetTextureHandle", dev_texture_handle},
    {"nvnDeviceGetTexelFetchHandle", dev_texel_fetch_handle}, {"nvnDeviceGetImageHandle", dev_image_handle},
    {"nvnProgramInitialize", prog_initialize}, {"nvnProgramFinalize", prog_finalize}, {"nvnProgramSetShaders", prog_set_shaders}, {"nvnProgramSetShadersExt", prog_set_shaders},
    {"nvnSamplerBuilderSetDefaults", smpb_defaults}, {"nvnSamplerBuilderSetMinMagFilter", smpb_minmag}, {"nvnSamplerBuilderSetWrapMode", smpb_wrap},
    {"nvnSamplerBuilderSetLodClamp", smpb_lodclamp}, {"nvnSamplerBuilderSetLodBias", smpb_lodbias}, {"nvnSamplerBuilderSetCompare", smpb_compare},
    {"nvnSamplerBuilderSetMaxAnisotropy", smpb_aniso}, {"nvnSamplerBuilderSetReductionFilter", smpb_reduction},
    {"nvnSamplerInitialize", smp_initialize},     {"nvnSamplerPoolInitialize", spool_initialize}, {"nvnSamplerPoolRegisterSampler", spool_register}, {"nvnSamplerPoolRegisterSamplerBuilder", spool_register},
        {"nvnTextureBuilderSetDefaults", texb_defaults}, {"nvnTextureBuilderSetFlags", texb_flags}, {"nvnTextureBuilderSetTarget", texb_target},
    {"nvnTextureBuilderSetFormat", texb_format}, {"nvnTextureBuilderSetWidth", texb_width}, {"nvnTextureBuilderSetHeight", texb_height}, {"nvnTextureBuilderSetDepth", texb_depth},
    {"nvnTextureBuilderSetSize1D", texb_size1d}, {"nvnTextureBuilderSetSize2D", texb_size2d}, {"nvnTextureBuilderSetSize3D", texb_size3d}, {"nvnTextureBuilderSetLevels", texb_levels},
    {"nvnTextureBuilderSetSamples", texb_samples}, {"nvnTextureBuilderSetSwizzle", texb_swizzle}, {"nvnTextureBuilderSetDepthStencilMode", texb_dsmode},
    {"nvnTextureBuilderSetStorage", texb_storage}, {"nvnTextureBuilderSetPackagedTextureData", texb_packaged}, {"nvnTextureBuilderSetPackagedTextureLayout", texb_layout},
    {"nvnTextureBuilderSetStride", texb_stride}, {"nvnTextureBuilderGetStorageSize", texb_storage_size}, {"nvnTextureBuilderGetStorageAlignment", texb_storage_align},
    {"nvnTextureBuilderGetFlags", texb_get_flags}, {"nvnTextureBuilderGetTarget", texb_get_target}, {"nvnTextureBuilderGetFormat", texb_get_format},
    {"nvnTextureBuilderGetWidth", texb_get_width}, {"nvnTextureBuilderGetHeight", texb_get_height}, {"nvnTextureBuilderGetDepth", texb_get_depth},
    {"nvnTextureBuilderGetLevels", texb_get_levels}, {"nvnTextureBuilderGetSamples", texb_get_samples},
    {"nvnTextureInitialize", tex_initialize}, {"nvnTextureFinalize", tex_finalize}, {"nvnTextureGetFlags", tex_get_flags},
    {"nvnTextureGetTarget", tex_get_target}, {"nvnTextureGetFormat", tex_get_format}, {"nvnTextureGetWidth", tex_get_width}, {"nvnTextureGetHeight", tex_get_height},
    {"nvnTextureGetDepth", tex_get_depth}, {"nvnTextureGetLevels", tex_get_levels}, {"nvnTextureGetSamples", tex_get_samples}, {"nvnTextureGetStride", tex_get_stride},
    {"nvnTextureGetStorageSize", tex_get_storage_size}, {"nvnTextureGetMemoryOffset", tex_get_memory_offset}, {"nvnTextureGetTextureAddress", tex_get_address},
    {"nvnTextureBuilderGetStorageClass", texb_storage_class}, {"nvnTextureBuilderGetRawStorageClass", texb_raw_storage_class}, {"nvnTextureBuilderGetSparseTileLayout", texb_sparse_layout},
    {"nvnTextureGetStorageClass", tex_storage_class}, {"nvnTextureGetRawStorageClass", tex_raw_storage_class}, {"nvnTextureGetSparseTileLayout", tex_sparse_layout},
    {"nvnTextureGetDepthStencilMode", tex_get_dsmode},     {"nvnTextureWriteTexels", tex_write_texels}, {"nvnTextureWriteTexelsStrided", tex_write_texels}, {"nvnTextureFlushTexels", tex_flush_texels},
    {"nvnTextureViewSetDefaults", view_defaults}, {"nvnTextureViewSetLevels", view_levels}, {"nvnTextureViewSetLayers", view_layers}, {"nvnTextureViewSetFormat", view_format},
    {"nvnTextureViewSetSwizzle", view_swizzle}, {"nvnTextureViewSetDepthStencilMode", view_dsmode}, {"nvnTextureViewSetTarget", view_target},
    {"nvnTexturePoolInitialize", tpool_initialize}, {"nvnTexturePoolRegisterTexture", tpool_register},     {"nvnBlendStateSetBlendFunc", blend_func_set}, {"nvnBlendStateSetBlendEquation", blend_equation},
    {"nvnColorStateSetDefaults", color_defaults}, {"nvnColorStateSetBlendEnable", color_blend_enable}, {"nvnColorStateSetLogicOp", color_logic_op}, {"nvnColorStateSetAlphaTest", color_alpha_test},
    {"nvnChannelMaskStateSetDefaults", chanmask_defaults}, {"nvnChannelMaskStateSetChannelMask", chanmask_set},
                        {"nvnPolygonStateSetDefaults", poly_defaults}, {"nvnPolygonStateSetCullFace", poly_cull}, {"nvnPolygonStateSetFrontFace", poly_front}, {"nvnPolygonStateSetPolygonMode", poly_mode},
    {"nvnPolygonStateSetPolygonOffsetEnables", poly_offset}, {"nvnPolygonStateGetCullFace", poly_get_cull}, {"nvnPolygonStateGetFrontFace", poly_get_front},
    {"nvnPolygonStateGetPolygonMode", poly_get_mode}, {"nvnPolygonStateGetPolygonOffsetEnables", poly_get_offset},
    {"nvnDepthStencilStateSetDefaults", ds_defaults}, {"nvnDepthStencilStateSetDepthTestEnable", ds_depth_test}, {"nvnDepthStencilStateSetDepthWriteEnable", ds_depth_write},
    {"nvnDepthStencilStateSetDepthFunc", ds_depth_func}, {"nvnDepthStencilStateSetStencilTestEnable", ds_stencil_test}, {"nvnDepthStencilStateSetStencilFunc", ds_stencil_func},
    {"nvnDepthStencilStateSetStencilOp", ds_stencil_op}, {"nvnDepthStencilStateGetDepthTestEnable", ds_get_depth_test}, {"nvnDepthStencilStateGetDepthWriteEnable", ds_get_depth_write},
    {"nvnDepthStencilStateGetStencilTestEnable", ds_get_stencil_test}, {"nvnDepthStencilStateGetDepthFunc", ds_get_depth_func},
    {"nvnVertexAttribStateSetDefaults", vattr_defaults}, {"nvnVertexAttribStateSetFormat", vattr_format}, {"nvnVertexAttribStateSetStreamIndex", vattr_stream},
    {"nvnVertexStreamStateSetDefaults", vstream_defaults}, {"nvnVertexStreamStateSetStride", vstream_stride}, {"nvnVertexStreamStateSetDivisor", vstream_divisor},
    {"nvnCommandBufferInitialize", cmd_initialize}, {"nvnCommandBufferFinalize", cmd_finalize}, {"nvnCommandBufferAddCommandMemory", cmd_add_command_memory},
    {"nvnCommandBufferAddControlMemory", cmd_add_control_memory}, {"nvnCommandBufferGetCommandMemorySize", cmd_get_command_size}, {"nvnCommandBufferGetControlMemorySize", cmd_get_control_size},
        {"nvnCommandBufferIsRecording", cmd_is_recording},         {"nvnCommandBufferBeginRecording", cmd_begin}, {"nvnCommandBufferEndRecording", cmd_end}, {"nvnCommandBufferCallCommands", cmd_call}, {"nvnCommandBufferCopyCommands", cmd_call},
    {"nvnCommandBufferSetTexturePool", cmd_texpool}, {"nvnCommandBufferSetSamplerPool", cmd_smppool}, {"nvnCommandBufferBindProgram", cmd_program},
    {"nvnCommandBufferBindUniformBuffer", cmd_ubo}, {"nvnCommandBufferBindUniformBuffers", cmd_ubos}, {"nvnCommandBufferBindStorageBuffer", cmd_ssbo},
    {"nvnCommandBufferBindStorageBuffers", cmd_ssbos}, {"nvnCommandBufferBindTexture", cmd_texture}, {"nvnCommandBufferBindTextures", cmd_textures},
    {"nvnCommandBufferBindVertexBuffer", cmd_vbuf}, {"nvnCommandBufferBindVertexBuffers", cmd_vbufs}, {"nvnCommandBufferBindVertexAttribState", cmd_vattr},
    {"nvnCommandBufferBindVertexStreamState", cmd_vstream}, {"nvnCommandBufferBindBlendState", cmd_blend}, {"nvnCommandBufferBindColorState", cmd_color},
    {"nvnCommandBufferBindChannelMaskState", cmd_chanmask}, {"nvnCommandBufferBindMultisampleState", cmd_multisample}, {"nvnCommandBufferBindPolygonState", cmd_polygon},
    {"nvnCommandBufferBindDepthStencilState", cmd_depthstencil}, {"nvnCommandBufferSetViewport", cmd_viewport}, {"nvnCommandBufferSetViewports", cmd_viewports},
    {"nvnCommandBufferSetScissor", cmd_scissor}, {"nvnCommandBufferSetScissors", cmd_scissors}, {"nvnCommandBufferSetDepthRange", cmd_depth_range},
    {"nvnCommandBufferSetDepthRanges", cmd_depth_ranges}, {"nvnCommandBufferSetRenderTargets", cmd_targets}, {"nvnCommandBufferSetStencilRef", cmd_stencil_ref},
    {"nvnCommandBufferSetStencilValueMask", cmd_stencil_vmask}, {"nvnCommandBufferSetStencilMask", cmd_stencil_mask}, {"nvnCommandBufferSetBlendColor", cmd_blend_color},
    {"nvnCommandBufferSetAlphaRef", cmd_alpha_ref}, {"nvnCommandBufferSetPointSize", cmd_point_size}, {"nvnCommandBufferSetLineWidth", cmd_line_width},
    {"nvnCommandBufferSetPolygonOffsetClamp", cmd_poly_offset}, {"nvnCommandBufferSetRasterizerDiscard", cmd_rast_discard}, {"nvnCommandBufferSetDepthClamp", cmd_depth_clamp},
    {"nvnCommandBufferSetPrimitiveRestart", cmd_prim_restart}, {"nvnCommandBufferSetPatchSize", cmd_patch_size}, {"nvnCommandBufferClearColor", cmd_clear_color},
    {"nvnCommandBufferClearDepthStencil", cmd_clear_ds}, {"nvnCommandBufferDrawArrays", cmd_draw_arrays}, {"nvnCommandBufferDrawArraysInstanced", cmd_draw_arrays_inst},
    {"nvnCommandBufferDrawElements", cmd_draw_elements}, {"nvnCommandBufferDrawElementsBaseVertex", cmd_draw_elements_bv}, {"nvnCommandBufferDrawElementsInstanced", cmd_draw_elements_inst},
    {"nvnCommandBufferCopyBufferToTexture", cmd_copy_buf_tex}, {"nvnCommandBufferCopyTextureToTexture", cmd_copy_tex_tex}, {"nvnCommandBufferCopyTextureToBuffer", cmd_copy_tex_buf},
    {"nvnCommandBufferSetSampleMask", cmd_sample_mask}, {"nvnCommandBufferDownsample", cmd_downsample},
    {"nvnCommandBufferCopyBufferToBuffer", cmd_copy_buf_buf}, {"nvnCommandBufferUpdateUniformBuffer", cmd_update_ubo}, {"nvnCommandBufferClearBuffer", cmd_clear_buffer},
    {"nvnCommandBufferSetCopyRowStride", cmd_row_stride},
                {"nvnQueueSubmitCommands", queue_submit}, {"nvnQueuePresentTexture", queue_present},     {"nvnCommandBufferBindImages", cmd_images}, {"nvnCommandBufferBindImage", cmd_image}, {"nvnCommandBufferDispatchCompute", cmd_dispatch},
    {"nvnWindowBuilderSetTextures", wb_set_textures}, {"nvnWindowInitialize", win_initialize}, {"nvnWindowAcquireTexture", win_acquire}, {"nvnWindowSetCrop", win_set_crop},
    /* the rest of the API (see the block above the table) */
    {"nvnDeviceBuilderSetDefaults", devb_defaults}, {"nvnDeviceBuilderSetFlags", devb_set_flags}, {"nvnDeviceBuilderGetFlags", devb_get_flags}, {"nvnDeviceInitialize", dev_initialize},
    {"nvnDeviceFinalize", dev_finalize}, {"nvnDeviceGetWindowOriginMode", dev_get_origin_mode}, {"nvnDeviceGetDepthMode", dev_get_depth_mode},
    {"nvnDeviceGetCurrentTimestampInNanoseconds", dev_timestamp_now}, {"nvnDeviceGetTimestampInNanoseconds", dev_timestamp_of}, {"nvnDeviceGenerateDebugDomainId", dev_debug_domain},
    {"nvnDeviceGetSeparateTextureHandle", dev_separate_handle}, {"nvnDeviceGetSeparateSamplerHandle", dev_separate_sampler_handle}, {"nvnDeviceIsExternalDebuggerAttached", dev_debugger_attached},
    {"nvnDeviceInstallDebugCallback", n_nop}, {"nvnDeviceApplyDeferredFinalizes", dev_apply_deferred}, {"nvnDeviceSetDebugLabel", any_set_label},
    {"nvnDeviceFinalizeCommandHandle", dev_finalize_command_handle}, {"nvnQueueBuilderSetDevice", any_set_device}, {"nvnQueueBuilderGetDevice", any_get_device},
    {"nvnWindowBuilderSetDevice", any_set_device}, {"nvnWindowBuilderGetDevice", any_get_device}, {"nvnTextureBuilderSetDevice", any_set_device},
    {"nvnTextureBuilderGetDevice", any_get_device}, {"nvnSamplerBuilderSetDevice", any_set_device}, {"nvnSamplerBuilderGetDevice", any_get_device}, {"nvnSyncInitialize", sync_initialize},
    {"nvnSyncFinalize", sync_finalize}, {"nvnSyncSetDebugLabel", any_set_label}, {"nvnSyncWait", sync_wait}, {"nvnQueueFenceSync", queue_fence_sync}, {"nvnQueueWaitSync", queue_wait_sync},
    {"nvnSyncCreateGLSync", sync_no_gl_interop}, {"nvnSyncInitializeFromFencedGLSync", sync_no_gl_interop}, {"nvnEventBuilderSetDefaults", evb_defaults},
    {"nvnEventBuilderSetStorage", evb_storage}, {"nvnEventBuilderGetStorage", evb_get_storage}, {"nvnEventBuilderGetMemoryPool", ev_get_pool},
    {"nvnEventBuilderGetMemoryOffset", ev_get_offset}, {"nvnEventInitialize", ev_initialize}, {"nvnEventFinalize", ev_finalize}, {"nvnEventGetValue", ev_get_value},
    {"nvnEventSignal", ev_signal}, {"nvnEventGetMemoryPool", ev_get_pool}, {"nvnEventGetMemoryOffset", ev_get_offset}, {"nvnQueueBuilderSetDefaults", qb_defaults},
    {"nvnQueueBuilderSetFlags", qb_set_flags}, {"nvnQueueBuilderSetCommandMemorySize", qb_set_cmd}, {"nvnQueueBuilderSetComputeMemorySize", qb_set_compute},
    {"nvnQueueBuilderSetControlMemorySize", qb_set_control}, {"nvnQueueBuilderSetCommandFlushThreshold", qb_set_threshold}, {"nvnQueueBuilderSetQueuePriority", qb_set_priority},
    {"nvnQueueBuilderSetQueueMemory", qb_set_memory}, {"nvnQueueBuilderGetFlags", qb_get_flags}, {"nvnQueueBuilderGetCommandMemorySize", qb_get_cmd},
    {"nvnQueueBuilderGetComputeMemorySize", qb_get_compute}, {"nvnQueueBuilderGetControlMemorySize", qb_get_control}, {"nvnQueueBuilderGetCommandFlushThreshold", qb_get_threshold},
    {"nvnQueueBuilderGetQueuePriority", qb_get_priority}, {"nvnQueueBuilderGetMemory", qb_get_memory}, {"nvnQueueBuilderGetMemorySize", qb_get_memory_size},
    {"nvnQueueBuilderGetQueueMemorySize", qb_get_queue_memory_size}, {"nvnQueueInitialize", queue_initialize}, {"nvnQueueFinalize", queue_finalize}, {"nvnQueueSetDebugLabel", any_set_label},
    {"nvnQueueGetTotalCommandMemoryUsed", queue_total_cmd}, {"nvnQueueGetTotalControlMemoryUsed", queue_total_control}, {"nvnQueueGetTotalComputeMemoryUsed", queue_total_compute},
    {"nvnQueueResetMemoryUsageCounts", queue_reset_usage}, {"nvnQueueFlush", queue_flush}, {"nvnQueueFinish", queue_finish}, {"nvnQueueGetError", queue_get_error},
    {"nvnQueueAcquireTexture", queue_acquire}, {"nvnMultisampleStateSetDefaults", ms_defaults}, {"nvnMultisampleStateSetMultisampleEnable", ms_enable},
    {"nvnMultisampleStateSetAlphaToCoverageEnable", ms_a2c}, {"nvnMultisampleStateSetAlphaToCoverageDither", ms_a2c_dither}, {"nvnMultisampleStateSetSamples", ms_samples},
    {"nvnMultisampleStateSetRasterSamples", ms_raster_samples}, {"nvnMultisampleStateSetCoverageToColorEnable", ms_c2c}, {"nvnMultisampleStateSetCoverageToColorOutput", ms_c2c_output},
    {"nvnMultisampleStateSetCoverageModulationMode", ms_covmod}, {"nvnMultisampleStateSetSampleLocationsEnable", ms_locations_on},
    {"nvnMultisampleStateSetSampleLocationsGridEnable", ms_grid_on}, {"nvnMultisampleStateSetSampleLocations", ms_set_locations}, {"nvnMultisampleStateGetMultisampleEnable", ms_get_enable},
    {"nvnMultisampleStateGetAlphaToCoverageEnable", ms_get_a2c}, {"nvnMultisampleStateGetAlphaToCoverageDither", ms_get_a2c_dither}, {"nvnMultisampleStateGetSamples", ms_get_samples},
    {"nvnMultisampleStateGetRasterSamples", ms_get_raster_samples}, {"nvnMultisampleStateGetCoverageToColorEnable", ms_get_c2c},
    {"nvnMultisampleStateGetCoverageToColorOutput", ms_get_c2c_output}, {"nvnMultisampleStateGetCoverageModulationMode", ms_get_covmod},
    {"nvnMultisampleStateGetSampleLocationsEnable", ms_get_locations_on}, {"nvnMultisampleStateGetSampleLocationsGridEnable", ms_get_grid_on},
    {"nvnMultisampleStateGetSampleLocationsGrid", ms_get_grid}, {"nvnBlendStateSetDefaults", blend_defaults}, {"nvnBlendStateSetBlendTarget", blend_target},
    {"nvnBlendStateSetAdvancedMode", blend_adv_mode}, {"nvnBlendStateSetAdvancedOverlap", blend_adv_overlap}, {"nvnBlendStateSetAdvancedPremultipliedSrc", blend_adv_premult},
    {"nvnBlendStateSetAdvancedNormalizedDst", blend_adv_normdst}, {"nvnBlendStateGetBlendTarget", blend_get_target}, {"nvnBlendStateGetBlendFunc", blend_get_func},
    {"nvnBlendStateGetBlendEquation", blend_get_equation}, {"nvnBlendStateGetAdvancedMode", blend_get_adv_mode}, {"nvnBlendStateGetAdvancedOverlap", blend_get_adv_overlap},
    {"nvnBlendStateGetAdvancedPremultipliedSrc", blend_get_adv_premult}, {"nvnBlendStateGetAdvancedNormalizedDst", blend_get_adv_normdst},
    {"nvnColorStateGetBlendEnable", color_get_blend_enable}, {"nvnColorStateGetLogicOp", color_get_logic_op}, {"nvnColorStateGetAlphaTest", color_get_alpha_test},
    {"nvnChannelMaskStateGetChannelMask", chanmask_get}, {"nvnDepthStencilStateGetStencilFunc", ds_get_stencil_func}, {"nvnDepthStencilStateGetStencilOp", ds_get_stencil_op},
    {"nvnVertexAttribStateGetFormat", vattr_get_format}, {"nvnVertexAttribStateGetStreamIndex", vattr_get_stream}, {"nvnVertexStreamStateGetStride", vstream_get_stride},
    {"nvnVertexStreamStateGetDivisor", vstream_get_divisor}, {"nvnSamplerBuilderSetBorderColor", smpb_border_f}, {"nvnSamplerBuilderSetBorderColori", smpb_border_i},
    {"nvnSamplerBuilderSetBorderColorui", smpb_border_i}, {"nvnSamplerBuilderSetLodSnap", smpb_lod_snap}, {"nvnSamplerBuilderGetMinMagFilter", smp_get_minmag},
    {"nvnSamplerBuilderGetWrapMode", smp_get_wrap}, {"nvnSamplerBuilderGetLodClamp", smp_get_lodclamp}, {"nvnSamplerBuilderGetLodBias", smp_get_lodbias},
    {"nvnSamplerBuilderGetCompare", smp_get_compare}, {"nvnSamplerBuilderGetBorderColor", smp_get_border}, {"nvnSamplerBuilderGetBorderColori", smp_get_border},
    {"nvnSamplerBuilderGetBorderColorui", smp_get_border}, {"nvnSamplerBuilderGetMaxAnisotropy", smp_get_aniso}, {"nvnSamplerBuilderGetReductionFilter", smp_get_reduction},
    {"nvnSamplerBuilderGetLodSnap", smp_get_lod_snap}, {"nvnSamplerGetMinMagFilter", smp_get_minmag}, {"nvnSamplerGetWrapMode", smp_get_wrap}, {"nvnSamplerGetLodClamp", smp_get_lodclamp},
    {"nvnSamplerGetLodBias", smp_get_lodbias}, {"nvnSamplerGetCompare", smp_get_compare}, {"nvnSamplerGetBorderColor", smp_get_border}, {"nvnSamplerGetBorderColori", smp_get_border},
    {"nvnSamplerGetBorderColorui", smp_get_border}, {"nvnSamplerGetMaxAnisotropy", smp_get_aniso}, {"nvnSamplerGetReductionFilter", smp_get_reduction}, {"nvnSamplerCompare", smp_compare},
    {"nvnSamplerGetDebugID", any_debug_id}, {"nvnSamplerFinalize", smp_finalize}, {"nvnSamplerSetDebugLabel", any_set_label}, {"nvnSamplerPoolFinalize", spool_finalize},
    {"nvnSamplerPoolGetMemoryPool", spool_get_pool}, {"nvnSamplerPoolGetMemoryOffset", spool_get_offset}, {"nvnSamplerPoolGetSize", spool_get_size},
    {"nvnSamplerPoolSetDebugLabel", any_set_label}, {"nvnTextureBuilderGetSwizzle", texb_get_swizzle}, {"nvnTextureBuilderGetDepthStencilMode", texb_get_dsmode},
    {"nvnTextureBuilderGetMemoryPool", texb_get_pool}, {"nvnTextureBuilderGetMemoryOffset", texb_get_offset}, {"nvnTextureBuilderGetPackagedTextureData", texb_get_packaged},
    {"nvnTextureBuilderGetPackagedTextureLayout", texb_get_layout}, {"nvnTextureBuilderGetStride", texb_get_stride}, {"nvnTextureBuilderSetGLTextureName", texb_set_glname},
    {"nvnTextureBuilderGetGLTextureName", texb_get_glname}, {"nvnTextureGetSwizzle", tex_get_swizzle}, {"nvnTextureGetMemoryPool", tex_get_pool}, {"nvnTextureCompare", tex_compare},
    {"nvnTextureGetDebugID", any_debug_id}, {"nvnTextureGetViewOffset", tex_view_offset}, {"nvnTextureReadTexels", tex_read_texels}, {"nvnTextureReadTexelsStrided", tex_read_texels},
    {"nvnTextureInvalidateTexels", tex_invalidate_texels}, {"nvnTextureSetDebugLabel", any_set_label}, {"nvnTextureViewGetLevels", view_get_levels},
    {"nvnTextureViewGetLayers", view_get_layers}, {"nvnTextureViewGetFormat", view_get_format}, {"nvnTextureViewGetSwizzle", view_get_swizzle},
    {"nvnTextureViewGetDepthStencilMode", view_get_dsmode}, {"nvnTextureViewGetTarget", view_get_target}, {"nvnTextureViewCompare", view_compare}, {"nvnTexturePoolFinalize", tpool_finalize},
    {"nvnTexturePoolGetMemoryPool", tpool_get_pool}, {"nvnTexturePoolGetMemoryOffset", tpool_get_offset}, {"nvnTexturePoolGetSize", tpool_get_size},
    {"nvnTexturePoolRegisterImage", tpool_register}, {"nvnTexturePoolSetDebugLabel", any_set_label}, {"nvnProgramSetDebugLabel", any_set_label},
    {"nvnProgramSetSampleShading", prog_sample_shading}, {"nvnWindowBuilderSetDefaults", wb_defaults}, {"nvnWindowBuilderSetNativeWindow", wb_native},
    {"nvnWindowBuilderSetPresentInterval", wb_interval}, {"nvnWindowBuilderSetNumActiveTextures", wb_num_active}, {"nvnWindowBuilderGetNativeWindow", wb_get_native},
    {"nvnWindowBuilderGetPresentInterval", wb_get_interval}, {"nvnWindowBuilderGetNumTextures", wb_get_num_textures}, {"nvnWindowBuilderGetNumActiveTextures", wb_get_num_active},
    {"nvnWindowBuilderGetTexture", wb_get_texture}, {"nvnWindowFinalize", win_finalize}, {"nvnWindowSetDebugLabel", any_set_label}, {"nvnWindowSetPresentInterval", win_set_interval},
    {"nvnWindowGetPresentInterval", win_get_interval}, {"nvnWindowGetNativeWindow", win_get_native}, {"nvnWindowGetNumTextures", win_get_num_textures},
    {"nvnWindowGetNumActiveTextures", win_get_num_active}, {"nvnWindowSetNumActiveTextures", win_set_num_active}, {"nvnWindowGetCrop", win_get_crop},
    {"nvnCommandBufferSetMemoryCallback", cmd_set_callback}, {"nvnCommandBufferSetMemoryCallbackData", cmd_set_callback_data}, {"nvnCommandBufferGetMemoryCallback", cmd_get_callback},
    {"nvnCommandBufferGetMemoryCallbackData", cmd_get_callback_data}, {"nvnCommandBufferSetCommandMemoryCallbackEnabled", cmd_callback_enabled},
    {"nvnCommandBufferGetCommandMemoryUsed", cmd_get_command_used}, {"nvnCommandBufferGetControlMemoryUsed", cmd_get_control_used},
    {"nvnCommandBufferGetCommandMemoryFree", cmd_get_command_free}, {"nvnCommandBufferGetControlMemoryFree", cmd_get_control_free}, {"nvnCommandBufferGetCopyRowStride", cmd_get_row_stride},
    {"nvnCommandBufferGetCopyImageStride", cmd_get_image_stride}, {"nvnCommandBufferSetCopyImageStride", cmd_image_stride}, {"nvnCommandBufferSetDepthBounds", cmd_depth_bounds},
    {"nvnCommandBufferSetConservativeRasterEnable", cmd_cons_raster}, {"nvnCommandBufferSetConservativeRasterDilate", cmd_cons_dilate},
    {"nvnCommandBufferSetSubpixelPrecisionBias", cmd_subpixel_bias}, {"nvnCommandBufferSetViewportSwizzles", cmd_viewport_swizzles},
    {"nvnCommandBufferSetInnerTessellationLevels", cmd_tess_levels}, {"nvnCommandBufferSetOuterTessellationLevels", cmd_tess_levels}, {"nvnCommandBufferBarrier", cmd_barrier},
    {"nvnCommandBufferDiscardColor", cmd_discard_color}, {"nvnCommandBufferDiscardDepthStencil", cmd_discard_ds}, {"nvnCommandBufferClearColori", cmd_clear_color_typed},
    {"nvnCommandBufferClearColorui", cmd_clear_color_typed}, {"nvnCommandBufferClearTexture", cmd_clear_texture}, {"nvnCommandBufferClearTexturei", cmd_clear_texture},
    {"nvnCommandBufferClearTextureui", cmd_clear_texture}, {"nvnCommandBufferDownsampleTextureView", cmd_downsample_view}, {"nvnCommandBufferTiledDownsample", cmd_downsample},
    {"nvnCommandBufferTiledDownsampleTextureView", cmd_downsample_view}, {"nvnCommandBufferDrawArraysIndirect", cmd_draw_arrays_indirect},
    {"nvnCommandBufferDrawElementsIndirect", cmd_draw_elements_indirect}, {"nvnCommandBufferMultiDrawArraysIndirectCount", cmd_multi_draw_arrays_indirect},
    {"nvnCommandBufferMultiDrawElementsIndirectCount", cmd_multi_draw_elements_indirect}, {"nvnCommandBufferDispatchComputeIndirect", cmd_dispatch_indirect},
    {"nvnCommandBufferSetRenderEnable", cmd_render_enable}, {"nvnCommandBufferSetRenderEnableConditional", cmd_render_enable_cond}, {"nvnCommandBufferResetCounter", cmd_reset_counter},
    {"nvnCommandBufferBindUniformBuffersDeferred", cmd_ubos_deferred}, {"nvnCommandBufferBindStorageBuffersDeferred", cmd_ssbos_deferred},
    {"nvnCommandBufferBindTexturesDeferred", cmd_textures_deferred}, {"nvnCommandBufferBindImagesDeferred", cmd_images_deferred},
    {"nvnCommandBufferReportCounter", cmd_report_counter}, {"nvnCommandBufferReportValue", cmd_report_value}, {"nvnCommandBufferFenceSync", cmd_fence_sync},
    {"nvnCommandBufferWaitSync", cmd_wait_sync}, {"nvnCommandBufferSignalEvent", cmd_signal_event}, {"nvnCommandBufferWaitEvent", cmd_wait_event},
    {"nvnCommandBufferDrawTexture", cmd_draw_texture},
    {"nvnCommandBufferBindCoverageModulationTable", cmd_covmod_table},{"nvnCommandBufferPushDebugGroup", cmd_push_group}, {"nvnCommandBufferPushDebugGroupStatic", cmd_push_group},
    {"nvnCommandBufferPushDebugGroupDynamic", cmd_push_group}, {"nvnCommandBufferPopDebugGroup", cmd_pop_group}, {"nvnCommandBufferPopDebugGroupId", cmd_pop_group},
    {"nvnCommandBufferInsertDebugMarker", cmd_marker}, {"nvnCommandBufferInsertDebugMarkerStatic", cmd_marker_id}, {"nvnCommandBufferInsertDebugMarkerDynamic", cmd_marker_id},
    {"nvnCommandBufferSetDebugLabel", cmd_set_label},
    /* no effect on this platform by the meaning of the call itself: each one is explained in nvn_audit_notes.tsv */
    {"nvnCommandBufferSetShaderScratchMemory", n_nop}, {"nvnSetExternalDebuggerEnabledNVX", n_nop}, {"nvnDeviceSetIntermediateShaderCache", n_nop}, {"nvnDeviceWalkDebugDatabase", n_nop},
    {"nvnCommandBufferSetTiledCacheAction", n_nop}, {"nvnCommandBufferSetTiledCacheTileSize", n_nop}, {"nvnCommandBufferSaveZCullData", n_nop},
    {"nvnCommandBufferRestoreZCullData", n_nop}, {"nvnCommandBufferSetStencilCullCriteria", n_nop}, {"nvnCommandBufferResolveDepthBuffer", n_nop},
    {"nvnCommandBufferSetColorReductionEnable", n_nop}, {"nvnCommandBufferSetColorReductionThresholds", n_nop}, {"nvnBufferInvalidateMappedRange", obj_invalidate_range},
    {"nvnMemoryPoolInvalidateMappedRange", obj_invalidate_range}, {"nvnDeviceRegisterFastClearColor", n_true}, {"nvnDeviceRegisterFastClearColori", n_true},
    {"nvnDeviceRegisterFastClearColorui", n_true}, {"nvnDeviceRegisterFastClearDepth", n_true}, {"nvnTextureBuilderGetZCullStorageSize", n_zero},
    {"nvnTextureGetZCullStorageSize", n_zero},
};

/* The host function for an NVN entry point, or NULL when this backend does not implement it. */
ImportFn nvn_gl_pick(const char *name) {
    for (size_t i = 0; i < sizeof k_nvn / sizeof k_nvn[0]; i++) if (strcmp(name, k_nvn[i].name) == 0) return k_nvn[i].fn;
    return NULL;
}
