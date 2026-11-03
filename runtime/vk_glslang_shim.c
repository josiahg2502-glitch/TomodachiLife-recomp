/* ssbu_glslang.dll - GLSL (Vulkan dialect) -> SPIR-V through the glslang library, behind a C function.
 *
 * Why a DLL: the glslang 16.6.0 release libraries (reference\upstream\glslang-16.6.0\lib) are MSVC C++ objects built
 * against the DLL C runtime (/MD); the game exe is C, linked by clang. The DLL keeps glslang and its runtime on its own
 * side; the exe loads it with LoadLibrary (plan\phases\V1_vulkan.md, section 4). Build: build_glslang.bat (MSVC).
 *
 * stage: 0 vertex, 1 fragment, 5 compute (the MX_* numbers of mx_translate.h).
 * Returns 1 and a malloc'ed word array (free it with ssbu_spirv_free), or 0 with the compiler's messages in log.
 * Thread-safe: glslang's process initialisation is done once, shaders and programs are per call. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>                                     /* Linux build: ssbu_glslang.so against the system's glslang */
#define __declspec(x) __attribute__((visibility("default")))
#endif
#include <glslang/Include/glslang_c_interface.h>
#if !defined(__has_include) || __has_include(<glslang/Public/resource_limits_c.h>)
#include <glslang/Public/resource_limits_c.h>
#else
/* glslang before 12 (the Android NDK's 11.11, Ubuntu 22.04's 11.8): the library has no default resource; glslang's own
 * defaults (StandAlone's DefaultTBuiltInResource), the ones the newer library gives on Windows and Linux */
#if defined(__has_include) && __has_include(<glslang/build_info.h>)
#include <glslang/build_info.h>
#endif
#if defined(GLSLANG_VERSION_MAJOR) && (GLSLANG_VERSION_MAJOR < 11 || (GLSLANG_VERSION_MAJOR == 11 && GLSLANG_VERSION_MINOR < 10))
#define SHIM_GLSLANG_BEFORE_11_10 1
#else
#define SHIM_GLSLANG_BEFORE_11_10 0
#endif
static const glslang_resource_t *glslang_default_resource(void) {
    static const glslang_resource_t r = {
        .max_lights = 32, .max_clip_planes = 6, .max_texture_units = 32, .max_texture_coords = 32, .max_vertex_attribs = 64,
        .max_vertex_uniform_components = 4096, .max_varying_floats = 64, .max_vertex_texture_image_units = 32,
        .max_combined_texture_image_units = 80, .max_texture_image_units = 32, .max_fragment_uniform_components = 4096,
        .max_draw_buffers = 32, .max_vertex_uniform_vectors = 128, .max_varying_vectors = 8, .max_fragment_uniform_vectors = 16,
        .max_vertex_output_vectors = 16, .max_fragment_input_vectors = 15, .min_program_texel_offset = -8, .max_program_texel_offset = 7,
        .max_clip_distances = 8, .max_compute_work_group_count_x = 65535, .max_compute_work_group_count_y = 65535,
        .max_compute_work_group_count_z = 65535, .max_compute_work_group_size_x = 1024, .max_compute_work_group_size_y = 1024,
        .max_compute_work_group_size_z = 64, .max_compute_uniform_components = 1024, .max_compute_texture_image_units = 16,
        .max_compute_image_uniforms = 8, .max_compute_atomic_counters = 8, .max_compute_atomic_counter_buffers = 1,
        .max_varying_components = 60, .max_vertex_output_components = 64, .max_geometry_input_components = 64,
        .max_geometry_output_components = 128, .max_fragment_input_components = 128, .max_image_units = 8,
        .max_combined_image_units_and_fragment_outputs = 8, .max_combined_shader_output_resources = 8, .max_image_samples = 0,
        .max_fragment_image_uniforms = 8, .max_combined_image_uniforms = 8, .max_geometry_texture_image_units = 16,
        .max_geometry_output_vertices = 256, .max_geometry_total_output_components = 1024, .max_geometry_uniform_components = 1024,
        .max_geometry_varying_components = 64, .max_tess_control_input_components = 128, .max_tess_control_output_components = 128,
        .max_tess_control_texture_image_units = 16, .max_tess_control_uniform_components = 1024,
        .max_tess_control_total_output_components = 4096, .max_tess_evaluation_input_components = 128,
        .max_tess_evaluation_output_components = 128, .max_tess_evaluation_texture_image_units = 16,
        .max_tess_evaluation_uniform_components = 1024, .max_tess_patch_components = 120, .max_patch_vertices = 32,
        .max_tess_gen_level = 64, .max_viewports = 16, .max_fragment_atomic_counters = 8, .max_combined_atomic_counters = 8,
        .max_atomic_counter_bindings = 1, .max_fragment_atomic_counter_buffers = 1, .max_combined_atomic_counter_buffers = 1,
        .max_atomic_counter_buffer_size = 16384, .max_transform_feedback_buffers = 4, .max_transform_feedback_interleaved_components = 64,
        .max_cull_distances = 8, .max_combined_clip_and_cull_distances = 8, .max_samples = 4,
        .max_mesh_output_vertices_nv = 256, .max_mesh_output_primitives_nv = 512, .max_mesh_work_group_size_x_nv = 32,
        .max_mesh_work_group_size_y_nv = 1, .max_mesh_work_group_size_z_nv = 1, .max_task_work_group_size_x_nv = 32,
        .max_task_work_group_size_y_nv = 1, .max_task_work_group_size_z_nv = 1, .max_mesh_view_count_nv = 4,
#if !SHIM_GLSLANG_BEFORE_11_10                           /* (the EXT mesh limits came with 11.10; Ubuntu 22.04 has 11.8) */
        .max_mesh_output_vertices_ext = 256, .max_mesh_output_primitives_ext = 256, .max_mesh_work_group_size_x_ext = 128,
        .max_mesh_work_group_size_y_ext = 128, .max_mesh_work_group_size_z_ext = 128, .max_task_work_group_size_x_ext = 128,
        .max_task_work_group_size_y_ext = 128, .max_task_work_group_size_z_ext = 128, .max_mesh_view_count_ext = 4,
#endif
        .maxDualSourceDrawBuffersEXT = 1,
        .limits = {.non_inductive_for_loops = 1, .while_loops = 1, .do_while_loops = 1, .general_uniform_indexing = 1,
                   .general_attribute_matrix_vector_indexing = 1, .general_varying_indexing = 1, .general_sampler_indexing = 1,
                   .general_variable_indexing = 1, .general_constant_matrix_vector_indexing = 1}};
    return &r;
}
#endif

#ifdef _WIN32
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_once(PINIT_ONCE once, PVOID arg, PVOID *ctx) { (void)once; (void)arg; (void)ctx; return glslang_initialize_process() != 0; }
#else
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_init_ok;
static void init_once_posix(void) { g_init_ok = glslang_initialize_process() != 0; }
#define InitOnceExecuteOnce(o, f, a, c) (pthread_once((o), init_once_posix), g_init_ok)
#endif

__declspec(dllexport) void ssbu_spirv_free(uint32_t *words) { free(words); }

__declspec(dllexport) int ssbu_glsl_to_spirv(int stage, const char *src, uint32_t **words, size_t *count, char *log, size_t log_size) {
    glslang_input_t in;
    int ok = 0;
    if (log && log_size) log[0] = 0;
    *words = NULL; *count = 0;
    if (!InitOnceExecuteOnce(&g_once, init_once, NULL, NULL)) { if (log) snprintf(log, log_size, "glslang_initialize_process failed"); return 0; }
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = stage == 0 ? GLSLANG_STAGE_VERTEX : stage == 1 ? GLSLANG_STAGE_FRAGMENT : stage == 5 ? GLSLANG_STAGE_COMPUTE : GLSLANG_STAGE_COUNT;
    if (in.stage == GLSLANG_STAGE_COUNT) { if (log) snprintf(log, log_size, "stage %d is not vertex (0), fragment (1) or compute (5)", stage); return 0; }
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_3;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_6;
    in.code = src;
    in.default_version = 460;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT | GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT;
    in.resource = glslang_default_resource();
    glslang_shader_t *sh = glslang_shader_create(&in);
    glslang_program_t *pr = NULL;
    if (!sh) { if (log) snprintf(log, log_size, "glslang_shader_create failed"); return 0; }
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        if (log) snprintf(log, log_size, "%s%s", glslang_shader_get_info_log(sh), glslang_shader_get_info_debug_log(sh));
        goto done;
    }
    pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, in.messages)) {
        if (log) snprintf(log, log_size, "%s%s", glslang_program_get_info_log(pr), glslang_program_get_info_debug_log(pr));
        goto done;
    }
    glslang_program_SPIRV_generate(pr, in.stage);
    {
        size_t n = glslang_program_SPIRV_get_size(pr);
        const char *msg = glslang_program_SPIRV_get_messages(pr);
        if (msg && msg[0] && log) snprintf(log, log_size, "%s", msg);
        if (n) {
            *words = malloc(n * sizeof(uint32_t));
            if (*words) { glslang_program_SPIRV_get(pr, *words); *count = n; ok = 1; }
        }
    }
done:
    if (pr) glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return ok;
}
