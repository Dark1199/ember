// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the FLVER body reader. The fixture is built here from the layout documented in
// formats/flver.h, so these tests prove the reader follows that description (and rejects damaged
// input); they cannot prove the description matches real files. The startup survey does that.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "formats/flver.h"

using namespace ember::formats;
using Bytes = std::vector<std::byte>;

namespace {

struct Out {
    bool big = false;
    Bytes b;
    template <typename T>
    void Put(T v) {
        std::uint8_t raw[sizeof(T)];
        std::memcpy(raw, &v, sizeof(T));
        if (big) for (size_t i = 0; i < sizeof(T) / 2; ++i) std::swap(raw[i], raw[sizeof(T) - 1 - i]);
        for (auto c : raw) b.push_back(static_cast<std::byte>(c));
    }
    void At(size_t off) { assert(b.size() <= off); b.resize(off, std::byte{0}); }
};

struct Spec {
    bool big = false;
    bool strip = true;
    std::uint32_t stride = 0x1C;
    std::vector<std::uint16_t> indices = {0, 1, 2, 3, 0xFFFF, 0, 2, 3};
    int vertex_count = 4;
    std::uint32_t position_type = 2;
    std::uint32_t vertex_size = 16; // position + 4 spare bytes
    float z_shift = 0.0f;
};

// One mesh, one face set, one vertex buffer, one layout holding a single position member.
Bytes Build(const Spec& s) {
    Out o{s.big, {}};
    const std::uint32_t tables = 0x80 + 0x20 + 0x30 + s.stride + 0x20 + 0x10;
    const std::uint32_t lists = tables;           // two index lists, 4 bytes each
    const std::uint32_t members = lists + 8;      // one member
    const std::uint32_t data = (members + 0x14 + 0xF) & ~0xFu;
    const std::uint32_t vb_bytes = s.vertex_size * s.vertex_count;
    const std::uint32_t idx_off = vb_bytes;       // indices after the vertices, inside the data block
    const std::uint32_t data_size = idx_off + 2 * s.indices.size();

    o.b.insert(o.b.end(), {std::byte{'F'}, std::byte{'L'}, std::byte{'V'}, std::byte{'E'}, std::byte{'R'}, std::byte{0}});
    o.b.push_back(static_cast<std::byte>(s.big ? 'B' : 'L'));
    o.b.push_back(std::byte{0});
    o.Put<std::int32_t>(0x2000C); o.Put<std::int32_t>(data); o.Put<std::int32_t>(data_size);
    for (std::int32_t c : {0, 1, 0, 1, 1}) o.Put<std::int32_t>(c); // dummy, material, bone, mesh, vbuf
    for (float f : {0.0f, 0.0f, 0.0f}) o.Put<float>(f);            // bbox min
    for (float f : {3.0f, 3.0f, 3.0f}) o.Put<float>(f);            // bbox max
    o.Put<std::int32_t>(2); o.Put<std::int32_t>(2);
    o.b.insert(o.b.end(), {std::byte{16}, std::byte{1}, std::byte{0}, std::byte{0}});
    o.Put<std::int32_t>(0); o.Put<std::int32_t>(1); o.Put<std::int32_t>(1); o.Put<std::int32_t>(0);
    o.At(0x80);
    o.At(0x80 + 0x20); // one material (zeros)
    // mesh
    o.Put<std::uint8_t>(0); o.At(0x80 + 0x20 + 4); o.Put<std::uint32_t>(0);
    o.At(0x80 + 0x20 + 0x20); o.Put<std::uint32_t>(1); o.Put<std::uint32_t>(lists);
    o.Put<std::uint32_t>(1); o.Put<std::uint32_t>(lists + 4);
    // face set
    const size_t fs = 0x80 + 0x20 + 0x30;
    o.At(fs); o.Put<std::uint32_t>(0); o.Put<std::uint8_t>(s.strip ? 1 : 0); o.Put<std::uint8_t>(0);
    o.At(fs + 8); o.Put<std::uint32_t>(static_cast<std::uint32_t>(s.indices.size())); o.Put<std::uint32_t>(idx_off);
    // vertex buffer
    const size_t vb = fs + s.stride;
    o.At(vb); o.Put<std::uint32_t>(0); o.Put<std::uint32_t>(0); o.Put<std::uint32_t>(s.vertex_size);
    o.Put<std::uint32_t>(s.vertex_count);
    o.At(vb + 0x18); o.Put<std::uint32_t>(vb_bytes); o.Put<std::uint32_t>(0);
    // layout
    const size_t lay = vb + 0x20;
    o.At(lay); o.Put<std::uint32_t>(1); o.At(lay + 12); o.Put<std::uint32_t>(members);
    o.At(lists); o.Put<std::uint32_t>(0); o.Put<std::uint32_t>(0); // face set 0, vertex buffer 0
    o.At(members); o.Put<std::uint32_t>(0); o.Put<std::uint32_t>(0); o.Put<std::uint32_t>(s.position_type);
    o.Put<std::uint32_t>(0); o.Put<std::uint32_t>(0);
    o.At(data);
    // vertices: a unit square at z = z_shift, scaled by 3 to fit the box
    const float xy[4][2] = {{0, 0}, {3, 0}, {0, 3}, {3, 3}};
    for (int i = 0; i < s.vertex_count; ++i) {
        o.Put<float>(xy[i % 4][0]); o.Put<float>(xy[i % 4][1]); o.Put<float>(s.z_shift);
        o.Put<std::uint32_t>(0xAABBCCDD);
    }
    for (std::uint16_t i : s.indices) o.Put<std::uint16_t>(i);
    return o.b;
}

std::optional<FlverGeometry> Decode(const Bytes& file, std::string* error) {
    auto h = ParseFlverHeader(file, error);
    if (!h) return std::nullopt;
    auto body = ParseFlverBody(file, *h, error);
    if (!body) return std::nullopt;
    return DecodeFlverGeometry(file, *h, *body, error);
}

} // namespace

int main() {
    std::string error;
    for (const bool big : {false, true}) {
        for (const std::uint32_t stride : {0x1Cu, 0x18u, 0x20u}) {
            Spec s; s.big = big; s.stride = stride;
            const Bytes file = Build(s);
            auto h = ParseFlverHeader(file, &error);
            assert(h);
            assert(FlverTablesEnd(*h, stride) <= h->data_offset);
            auto g = Decode(file, &error);
            assert(g && error.empty());
            assert(g->vertex_count == 4 && g->positions.size() == 12);
            assert(g->positions[3] == 3.0f && g->positions[4] == 0.0f);
            // strip 0 1 2 3 | restart | 0 2 3 -> (0,1,2) (2,1,3) and the lone triangle (0,2,3)
            assert((g->indices == std::vector<std::uint32_t>{0, 1, 2, 2, 1, 3, 0, 2, 3}));
            assert(g->strip_sets == 1 && g->bad_indices == 0 && g->outside_bounds_components == 0);
            auto body = ParseFlverBody(file, *h, &error);
            assert(body && body->face_set_stride == stride);
        }
    }
    { // triangle list
        Spec s; s.strip = false; s.indices = {0, 1, 2, 1, 3, 2};
        auto g = Decode(Build(s), &error);
        assert(g && g->indices.size() == 6 && g->list_sets == 1);
    }
    { // index past the vertex buffer: that triangle is dropped and counted
        Spec s; s.strip = false; s.indices = {0, 1, 2, 1, 3, 9};
        auto g = Decode(Build(s), &error);
        assert(g && g->indices.size() == 3 && g->bad_indices == 1);
    }
    { // positions outside the header's box are counted
        Spec s; s.z_shift = 50.0f;
        auto g = Decode(Build(s), &error);
        assert(g && g->outside_bounds_components == 4);
    }
    { // unsupported position type is an error that says what it found
        Spec s; s.position_type = 3;
        assert(!Decode(Build(s), &error));
        assert(error.find("type 3") != std::string::npos);
    }
    { // truncated file: tables or data run past the end, never a crash
        const Bytes file = Build(Spec{});
        for (size_t cut : {size_t(0x50), size_t(0x90), size_t(0xE0), file.size() - 1}) {
            Bytes shorter(file.begin(), file.begin() + cut);
            (void)Decode(shorter, &error); // must return, not crash
            assert(!Decode(shorter, &error) || cut == file.size());
        }
    }
    { // vertex size that cannot hold the position
        Spec s; s.vertex_size = 8;
        assert(!Decode(Build(s), &error));
    }
    { // skeleton-only file: empty-bounds sentinel is valid with zero meshes, invalid with meshes
        Bytes file(0x100, std::byte{0});
        const char magic[] = {'F', 'L', 'V', 'E', 'R', 0, 'L', 0};
        std::memcpy(file.data(), magic, 8);
        const auto put32 = [&](size_t at, std::uint32_t v) { std::memcpy(file.data() + at, &v, 4); };
        const auto putf = [&](size_t at, float f) { std::memcpy(file.data() + at, &f, 4); };
        put32(0x08, 0x2000C); put32(0x0C, 0x100); put32(0x10, 0); put32(0x1C, 2);
        for (int i = 0; i < 3; ++i) { putf(0x28 + 4 * i, 3.4028235e38f); putf(0x34 + 4 * i, -3.4028235e38f); }
        file[0x48] = std::byte{16};
        auto h = ParseFlverHeader(file, &error);
        assert(h && h->mesh_count == 0 && h->bone_count == 2);
        put32(0x20, 1); // now claims a mesh: the inverted box is an error again
        assert(!ParseFlverHeader(file, &error));
        put32(0x20, 0);
        putf(0x28, std::nanf(""));
        assert(!ParseFlverHeader(file, &error));
    }
    std::puts("flver_test: ok");
    return 0;
}
