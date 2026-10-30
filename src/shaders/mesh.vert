#version 450
// Model-space position and first UV set in, clip space out. The position is also passed on so the
// fragment shader can derive a flat normal.
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 params; // x = lowest y of the model, y = highest y
} pc;
layout(location = 0) out vec3 v_position;
layout(location = 1) out vec2 v_uv;
void main() {
    v_position = in_position;
    v_uv = in_uv;
    gl_Position = pc.mvp * vec4(in_position, 1.0);
}
