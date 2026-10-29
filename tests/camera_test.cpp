// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cmath>
#include <cstdio>

#include "gfx/camera.h"

using namespace ember::gfx;

static std::array<float, 4> Apply(const Mat4& m, float x, float y, float z) {
    std::array<float, 4> r{};
    const float in[4] = {x, y, z, 1.0f};
    for (int row = 0; row < 4; ++row)
        for (int k = 0; k < 4; ++k) r[row] += m[k * 4 + row] * in[k];
    return r;
}

int main() {
    const float lo[3] = {-1, 0, -1}, hi[3] = {1, 2, 1};
    for (const float t : {0.0f, 1.3f, 5.0f}) {
        for (const float aspect : {2.15f, 0.46f}) {
            const Mat4 mvp = OrbitMvp(lo, hi, t, aspect, 0);
            const auto c = Apply(mvp, 0, 1, 0); // box centre lands in the middle of the screen
            assert(c[3] > 0.0f);
            assert(std::fabs(c[0] / c[3]) < 1e-4f && std::fabs(c[1] / c[3]) < 1e-4f);
            const float depth = c[2] / c[3];
            assert(depth > 0.0f && depth < 1.0f);
            // Every corner of the box is inside the view volume (the sphere fits).
            for (int i = 0; i < 8; ++i) {
                const auto p = Apply(mvp, (i & 1) ? 1.f : -1.f, (i & 2) ? 2.f : 0.f, (i & 4) ? 1.f : -1.f);
                assert(p[3] > 0.0f);
                assert(std::fabs(p[0] / p[3]) <= 1.0f && std::fabs(p[1] / p[3]) <= 1.0f);
                assert(p[2] / p[3] >= 0.0f && p[2] / p[3] <= 1.0f);
            }
        }
    }
    // Up on the model is up on the screen: higher y gives smaller clip y (Vulkan y points down).
    {
        const Mat4 mvp = OrbitMvp(lo, hi, 0.0f, 1.0f, 0);
        assert(Apply(mvp, 0, 2, 0)[1] / Apply(mvp, 0, 2, 0)[3] < Apply(mvp, 0, 0, 0)[1] / Apply(mvp, 0, 0, 0)[3]);
    }
    // Quarter turn: a point right of centre moves to below centre (clockwise on screen).
    {
        const auto p = Apply(PreRotation(1), 1, 0, 0);
        assert(p[0] == 0.0f && p[1] == 1.0f);
        const auto q = Apply(PreRotation(3), 1, 0, 0);
        assert(q[0] == 0.0f && q[1] == -1.0f);
        const auto h = Apply(PreRotation(2), 1, 0.5f, 0);
        assert(h[0] == -1.0f && h[1] == -0.5f);
    }
    std::puts("camera_test: ok");
    return 0;
}
