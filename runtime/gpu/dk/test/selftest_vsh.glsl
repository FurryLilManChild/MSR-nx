// MSR-nx: vertex shader for the fragment shader self-test (dk_pica.c, dkr_selftest): one triangle
// over the whole target, with every input of pica_fsh.glsl varying across it.
#version 460

layout (location = 0) out vec4 v_color;
layout (location = 1) out vec4 v_uv01;
layout (location = 2) out vec4 v_uv2w;
layout (location = 3) out vec4 v_quat;
layout (location = 4) out vec4 v_view;

void main() {
    vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
    gl_Position = vec4(p, 0.5, 1.0);
    vec2 t = p * 0.5 + 0.5;
    v_color = vec4(t.x, t.y, 1.0 - t.x, 0.25 + 0.5 * t.y);
    v_uv01 = vec4(t.x * 2.0, t.y * 3.0, t.y, 1.0 - t.x);
    v_uv2w = vec4(t.y, t.x, 1.0 + t.x, 0.0);
    v_quat = vec4(0.3 * t.x, 0.2, 0.1 * t.y, 0.9);
    v_view = vec4(t.x - 0.5, t.y - 0.5, -1.0 - t.x, 0.0);
}
