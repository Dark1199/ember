#version 450
// Flat-shaded look from screen-space derivatives: no normals or textures needed yet.
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 params; // x = lowest y of the model, y = highest y
} pc;
layout(location = 0) in vec3 v_position;
layout(location = 0) out vec4 out_color;
void main() {
    vec3 c = cross(dFdx(v_position), dFdy(v_position));
    vec3 n = c / max(length(c), 1e-12);
    float light = 0.35 + 0.65 * abs(dot(n, normalize(vec3(0.4, 0.8, 0.5))));
    float h = clamp((v_position.y - pc.params.x) / max(pc.params.y - pc.params.x, 1e-4), 0.0, 1.0);
    vec3 base = mix(vec3(0.55, 0.35, 0.22), vec3(0.95, 0.75, 0.50), h);
    out_color = vec4(base * light, 1.0);
}
