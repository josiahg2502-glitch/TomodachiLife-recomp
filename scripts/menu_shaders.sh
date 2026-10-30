#!/bin/sh
# Precompiles the settings menu's shaders into C headers (SPIR-V words): runtime/menu_gl_spv.h for the OpenGL renderer
# (GL_ARB_gl_spirv) from menu_gl.vert / menu_gl.frag. Run after changing those files; the headers are committed, so a build
# needs no shader compiler. GLSLANG = glslangValidator (default: the Vulkan SDK's, or the one on PATH).
#   sh scripts/menu_shaders.sh
set -e
R=$(cd "$(dirname "$0")/../runtime" && pwd)
G=${GLSLANG:-$(ls /c/VulkanSDK/*/Bin/glslangValidator.exe 2>/dev/null | tail -1)}
G=${G:-glslangValidator}
T=$(mktemp -d)
"$G" -G --vn k_menu_gl_vert_spv -o "$T/v.h" "$R/menu_gl.vert" > /dev/null
"$G" -G --vn k_menu_gl_frag_spv -o "$T/f.h" "$R/menu_gl.frag" > /dev/null
{ echo "/* SPIR-V of menu_gl.vert / menu_gl.frag (OpenGL target), made by scripts/menu_shaders.sh. Do not edit. */"; cat "$T/v.h" "$T/f.h"; } > "$R/menu_gl_spv.h"
rm -rf "$T"
echo "wrote $R/menu_gl_spv.h"
