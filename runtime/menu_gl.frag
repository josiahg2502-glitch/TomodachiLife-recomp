#version 450
// The in-game settings menu on the OpenGL renderer (nvn_gl_menu.c). Precompiled into menu_gl_spv.h (scripts/gl_shaders.sh).
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
void main() { o_col = v_col * texture(u_tex, v_uv); }
