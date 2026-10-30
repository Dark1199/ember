#version 450
// Diffuse texture times a flat-shaded light term from screen-space derivatives (no normals yet).
layout(set = 0, binding = 0) uniform sampler2D diffuse_map;
layout(location = 0) in vec3 v_position;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
    vec3 c = cross(dFdx(v_position), dFdy(v_position));
    vec3 n = c / max(length(c), 1e-12);
    float light = 0.45 + 0.55 * abs(dot(n, normalize(vec3(0.4, 0.8, 0.5))));
    vec4 texel = texture(diffuse_map, v_uv);
    out_color = vec4(texel.rgb * light, 1.0);
}
