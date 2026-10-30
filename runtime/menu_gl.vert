#version 450
// The in-game settings menu on the OpenGL renderer (nvn_gl_menu.c): Dear ImGui's vertices. Precompiled into menu_gl_spv.h
// (scripts/gl_shaders.sh); the driver takes the SPIR-V (GL_ARB_gl_spirv), nothing is compiled from text at run time.
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(location = 0) uniform vec4 u_xf;      // xy: scale, zw: translate (pixels -> clip space, y down)
layout(location = 0) out vec4 v_col;
layout(location = 1) out vec2 v_uv;
void main() {
    v_col = a_col;
    v_uv = a_uv;
    gl_Position = vec4(a_pos * u_xf.xy + u_xf.zw, 0.0, 1.0);
}
