#version 450
// Layout units (top-left origin, kLayoutHeight tall) to clip space, then the swapchain's
// pre-rotation (a 2x2 matrix) so the picture is upright on a rotated display.
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(push_constant) uniform Push {
    vec4 rotation;  // column 0 = xy, column 1 = zw
    vec2 layout_size;
} pc;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
    vec2 ndc = in_pos / pc.layout_size * 2.0 - 1.0;
    gl_Position = vec4(mat2(pc.rotation.xy, pc.rotation.zw) * ndc, 0.0, 1.0);
    v_uv = in_uv;
    v_color = in_color;
}
