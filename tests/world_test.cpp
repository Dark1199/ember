// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the world loader's pure parts (transforms, instance merging, path candidates) and
// the failure path of a load. Model file naming and the
// rotation order are documented assumptions; the startup log shows what the real data does.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "app/world_loader.h"

using namespace ember::app;

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static LoadedModel Triangle(const std::string& key) {
    LoadedModel m;
    m.positions = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    m.uvs = {0, 0, 1, 0, 0, 1};
    m.indices = {0, 1, 2};
    m.ranges = {{0, 3, 0}};
    m.material_texture_keys = {key};
    return m;
}

int main() {
    assert(MapIdFromMsbPath("/map/MapStudio/M10_01_00_00.msb") == "m10_01_00_00");
    {
        const auto c = ModelPathCandidates("m10_01_00_00", "M0000B0A10");
        assert(c.size() == 2 && c[0] == "/map/m10_01_00_00/m0000b0a10.flver.dcx" && c[1].ends_with(".flver"));
        const auto d = ModelPathCandidates("m10_01_00_00", "m0000B0");
        assert(d.size() == 4 && d[2] == "/map/m10_01_00_00/m0000b0a10.flver.dcx");
    }
    { // transform: scale, then X, Z, Y rotation, then translation
        const float zero[3] = {0, 0, 0}, one[3] = {1, 1, 1}, in[3] = {1, 0, 0};
        float out[3];
        const float ry90[3] = {0, 90, 0};
        TransformPoint(zero, ry90, one, in, out);
        assert(Near(out[0], 0) && Near(out[1], 0) && Near(out[2], -1));
        const float rz90[3] = {0, 0, 90};
        TransformPoint(zero, rz90, one, in, out);
        assert(Near(out[0], 0) && Near(out[1], 1) && Near(out[2], 0));
        const float rx90[3] = {90, 0, 0}, in_y[3] = {0, 1, 0};
        TransformPoint(zero, rx90, one, in_y, out);
        assert(Near(out[0], 0) && Near(out[1], 0) && Near(out[2], 1));
        const float pos[3] = {10, 20, 30}, sc[3] = {2, 3, 4}, none[3] = {0, 0, 0}, p1[3] = {1, 1, 1};
        TransformPoint(pos, none, sc, p1, out);
        assert(Near(out[0], 12) && Near(out[1], 23) && Near(out[2], 34));
    }
    { // merging groups triangles by texture key and offsets indices per instance
        WorldBuilder b;
        const float rot[3] = {0, 0, 0}, one[3] = {1, 1, 1};
        const float p0[3] = {0, 0, 0}, p1[3] = {100, 0, 0}, p2[3] = {0, 50, 0};
        assert(b.AddInstance(Triangle("tex_a"), p0, rot, one) == 1);
        assert(b.AddInstance(Triangle("tex_b"), p1, rot, one) == 1);
        assert(b.AddInstance(Triangle("tex_a"), p2, rot, one) == 1);
        assert(b.Triangles() == 3);
        const auto keys = b.KeysByUse();
        assert(keys.size() == 2 && keys[0] == "tex_a" && keys[1] == "tex_b");
        const LoadedModel m = b.Finish("test");
        assert(m.positions.size() == 27 && m.uvs.size() == 18 && m.indices.size() == 9);
        assert(m.ranges.size() == 2 && m.material_texture_keys.size() == 2);
        assert(m.ranges[0].index_count == 6 && m.ranges[1].first_index == 6 && m.ranges[1].index_count == 3);
        assert(m.material_texture_keys[0] == "tex_a" && m.ranges[1].material_index == 1);
        // tex_a holds instance 0 (vertices 0..2) and instance 2 (vertices 6..8)
        assert(m.indices[0] == 0 && m.indices[3] == 6 && m.indices[6] == 3);
        assert(Near(m.bounds_max[0], 101) && Near(m.bounds_max[1], 51) && Near(m.bounds_min[0], 0));
        for (auto i : m.indices) assert(i < m.positions.size() / 3);
        assert(b.Triangles() == 0); // cleared by Finish
    }
    { // out-of-range indices in a source model are dropped, never copied
        LoadedModel bad = Triangle("x");
        bad.indices = {0, 1, 7};
        WorldBuilder b;
        const float rot[3] = {0, 0, 0}, one[3] = {1, 1, 1}, p[3] = {0, 0, 0};
        assert(b.AddInstance(bad, p, rot, one) == 0);
    }
    { // a missing MSB reports why instead of crashing
        ember::vfs::Vfs vfs;
        std::string summary;
        WorldOptions o;
        assert(!LoadWorld(vfs, "/map/MapStudio/m99_00_00_00.msb", o, &summary));
        assert(summary.find("unreadable") != std::string::npos);
    }
    std::puts("world_test: ok");
    return 0;
}
