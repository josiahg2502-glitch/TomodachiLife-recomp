#!/bin/sh
# Precompiles the runtime's own OpenGL shaders into C headers (SPIR-V words, taken by the driver through GL_ARB_gl_spirv):
# runtime/menu_gl_spv.h from menu_gl.vert / menu_gl.frag (the settings menu) and runtime/astc_decode_spv.h from
# astc_decode.comp (ASTC textures decoded on the GPU), and runtime/bc_decode_spv.h from bc_decode.comp (Vulkan target: BC
# textures decoded on GPUs without them). Run after changing those files; the headers are committed, so a build needs no
# shader compiler. GLSLANG = glslangValidator (default: the Vulkan SDK's, or the one on PATH).
#   sh scripts/gl_shaders.sh
set -e
R=$(cd "$(dirname "$0")/../runtime" && pwd)
G=${GLSLANG:-$(ls /c/VulkanSDK/*/Bin/glslangValidator.exe 2>/dev/null | tail -1)}
G=${G:-glslangValidator}
T=$(mktemp -d)
"$G" -G --vn k_menu_gl_vert_spv -o "$T/v.h" "$R/menu_gl.vert" > /dev/null
"$G" -G --vn k_menu_gl_frag_spv -o "$T/f.h" "$R/menu_gl.frag" > /dev/null
{ echo "/* SPIR-V of menu_gl.vert / menu_gl.frag (OpenGL target), made by scripts/gl_shaders.sh. Do not edit. */"; cat "$T/v.h" "$T/f.h"; } > "$R/menu_gl_spv.h"
"$G" -G --target-env opengl --vn k_astc_decode_spv -o "$T/c.h" "$R/astc_decode.comp" > /dev/null
{ echo "/* SPIR-V of astc_decode.comp (OpenGL target), made by scripts/gl_shaders.sh. Do not edit. */"; cat "$T/c.h"; } > "$R/astc_decode_spv.h"
"$G" -V --target-env vulkan1.1 --vn k_bc_decode_spv -o "$T/b.h" "$R/bc_decode.comp" > /dev/null
{ echo "/* SPIR-V of bc_decode.comp (Vulkan target), made by scripts/gl_shaders.sh. Do not edit. */"; cat "$T/b.h"; } > "$R/bc_decode_spv.h"
rm -rf "$T"
echo "wrote $R/menu_gl_spv.h, $R/astc_decode_spv.h and $R/bc_decode_spv.h"
