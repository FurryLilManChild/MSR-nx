// MSR-nx: single-screen mode, one of the bottom screen's readouts drawn over the top screen
// (main_nx.c present). Rectangles in output-image pixels (row 0 at the bottom, as the image is
// shown upside down) and in bottom-screen-image texels (row 0 at the 3DS screen's bottom).
#version 460
layout (std140, binding = 0) uniform Rects {
    vec4 dst;   // x, y, w, h in the 1280x720 output image
    vec4 src;   // x, y, w, h in the 320x240 bottom-screen image
};
layout (location = 0) out vec2 v_uv;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));   // a 4-vertex strip
    vec2 p = dst.xy + c * dst.zw;
    gl_Position = vec4(p.x / 640.0 - 1.0, p.y / 360.0 - 1.0, 0.0, 1.0);
    v_uv = (src.xy + c * src.zw) / vec2(320.0, 240.0);
}
