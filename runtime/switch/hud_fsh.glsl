// MSR-nx: a bottom-screen readout over the top screen. The readouts' dark backgrounds are see-through;
// lines, icons and text get more opaque the brighter they are, and bright text is fully opaque.
#version 460
layout (binding = 0) uniform sampler2D bottom;
layout (location = 0) in vec2 v_uv;
layout (location = 0) out vec4 o;
void main() {
    vec3 c = texture(bottom, v_uv).rgb;
    float m = max(c.r, max(c.g, c.b));
    float a = clamp((m - 0.25) / 0.45, 0.0, 1.0);
    o = vec4(c, 0.3 + 0.7 * a);
}
