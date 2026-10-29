#version 450
// Position-only vertex shader: model space in, clip space out. The mesh's own positions are also
// passed on so the fragment shader can derive a flat normal.
layout(location = 0) in vec3 in_position;
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 params; // x = lowest y of the model, y = highest y
} pc;
layout(location = 0) out vec3 v_position;
void main() {
    v_position = in_position;
    gl_Position = pc.mvp * vec4(in_position, 1.0);
}
