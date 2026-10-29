// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace ember::gfx {

/// Column-major 4x4 matrix (element [column * 4 + row]), the layout GLSL expects.
using Mat4 = std::array<float, 16>;

inline Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = sum;
        }
    return r;
}

/// Right-handed perspective for Vulkan clip space: depth 0..1, +y points down on screen.
inline Mat4 Perspective(float fov_y_radians, float aspect, float near_plane, float far_plane) {
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 m{};
    m[0] = f / aspect;
    m[5] = -f;
    m[10] = far_plane / (near_plane - far_plane);
    m[11] = -1.0f;
    m[14] = near_plane * far_plane / (near_plane - far_plane);
    return m;
}

/// Right-handed look-at with +y up.
inline Mat4 LookAt(const std::array<float, 3>& eye, const std::array<float, 3>& target) {
    const auto sub = [](auto a, auto b) { return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
    const auto cross = [](auto a, auto b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                    a[0] * b[1] - a[1] * b[0]};
    };
    const auto norm = [](std::array<float, 3> a) {
        const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        const float inv = l > 1e-12f ? 1.0f / l : 0.0f;
        return std::array<float, 3>{a[0] * inv, a[1] * inv, a[2] * inv};
    };
    const auto dot = [](auto a, auto b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto f = norm(sub(target, eye));
    const std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    const auto s = norm(cross(f, up));
    const auto u = cross(s, f);
    Mat4 m{};
    m[0] = s[0];  m[4] = s[1];  m[8] = s[2];   m[12] = -dot(s, eye);
    m[1] = u[0];  m[5] = u[1];  m[9] = u[2];   m[13] = -dot(u, eye);
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] = dot(f, eye);
    m[15] = 1.0f;
    return m;
}

/// Clip-space rotation that compensates a swapchain pre-transform, given as quarter turns
/// clockwise (0, 1, 2, 3 for identity, 90, 180, 270): x' = -y, y' = x for one quarter turn.
inline Mat4 PreRotation(int quarter_turns) {
    Mat4 m{};
    m[10] = 1.0f;
    m[15] = 1.0f;
    switch (quarter_turns & 3) {
    case 0: m[0] = 1; m[5] = 1; break;
    case 1: m[1] = 1; m[4] = -1; break;   // x' = -y, y' = x
    case 2: m[0] = -1; m[5] = -1; break;
    default: m[1] = -1; m[4] = 1; break;  // x' = y, y' = -x
    }
    return m;
}

/// Orbit camera around the centre of a box, slowly turning with `seconds`. `aspect` is the
/// displayed aspect ratio (width / height as the user sees it).
inline Mat4 OrbitMvp(const float bounds_min[3], const float bounds_max[3], float seconds,
                     float aspect, int pre_rotation_quarter_turns) {
    const std::array<float, 3> centre{(bounds_min[0] + bounds_max[0]) * 0.5f,
                                      (bounds_min[1] + bounds_max[1]) * 0.5f,
                                      (bounds_min[2] + bounds_max[2]) * 0.5f};
    float radius = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float half = (bounds_max[i] - bounds_min[i]) * 0.5f;
        radius += half * half;
    }
    radius = std::max(std::sqrt(radius), 1e-3f);
    const float fov = 0.9f;
    // Distance at which the bounding sphere fits the narrower view angle, with a little margin.
    const float narrow = aspect >= 1.0f ? fov : 2.0f * std::atan(std::tan(fov * 0.5f) * aspect);
    const float distance = radius / std::sin(narrow * 0.5f) * 1.05f;
    const float angle = seconds * 0.5f;
    const std::array<float, 3> eye{centre[0] + distance * std::sin(angle) * 0.9f,
                                   centre[1] + distance * 0.35f,
                                   centre[2] + distance * std::cos(angle) * 0.9f};
    const Mat4 view = LookAt(eye, centre);
    const Mat4 proj = Perspective(fov, aspect, distance * 0.01f, distance + radius * 2.0f);
    return Multiply(PreRotation(pre_rotation_quarter_turns), Multiply(proj, view));
}

} // namespace ember::gfx
