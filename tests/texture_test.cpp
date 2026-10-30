// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the DDS decoder and the TPF reader. Fixtures are built by hand from the public
// DDS block formats and the TPF description in formats/tpf.h.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "formats/dds.h"
#include "formats/tpf.h"

using namespace ember::formats;
using Bytes = std::vector<std::byte>;

namespace {

void P32(Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::byte>(v >> (8 * i))); }

Bytes DdsFile(const char* fourcc, std::uint32_t w, std::uint32_t h, std::uint32_t mips, const Bytes& pixels) {
    Bytes b;
    for (char c : std::string("DDS ")) b.push_back(static_cast<std::byte>(c));
    P32(b, 124); P32(b, 0x1007); P32(b, h); P32(b, w); P32(b, 0); P32(b, 0); P32(b, mips);
    for (int i = 0; i < 11; ++i) P32(b, 0);
    P32(b, 32); P32(b, 4);
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::byte>(fourcc[i]));
    for (int i = 0; i < 5; ++i) P32(b, 0);
    P32(b, 0x1000); for (int i = 0; i < 4; ++i) P32(b, 0);
    assert(b.size() == 128);
    b.insert(b.end(), pixels.begin(), pixels.end());
    return b;
}

Bytes Block(std::initializer_list<int> v) { Bytes b; for (int x : v) b.push_back(static_cast<std::byte>(x)); return b; }

} // namespace

int main() {
    std::string error;
    {   // DXT1: c0 red, c1 blue, all indices 0 -> opaque red 4x4
        const Bytes px = Block({0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0});
        const Bytes file = DdsFile("DXT1", 4, 4, 1, px);
        auto info = ParseDds(file, &error);
        assert(info && info->format == DdsFormat::Dxt1);
        auto img = DecodeDds(file, *info, 8, &error);
        assert(img && img->size() == 1 && (*img)[0].rgba.size() == 64);
        assert((*img)[0].rgba[0] == 255 && (*img)[0].rgba[1] == 0 && (*img)[0].rgba[2] == 0 && (*img)[0].rgba[3] == 255);
    }
    {   // DXT1 with c0 <= c1: index 3 is transparent black; index 2 is the midpoint
        const Bytes px = Block({0x00, 0x00, 0xFF, 0xFF, 0xAA, 0xFF, 0x00, 0x00}); // idx: 2,2,2,2 then 3s
        const Bytes file = DdsFile("DXT1", 4, 4, 1, px);
        auto info = ParseDds(file, &error);
        auto img = DecodeDds(file, *info, 8, &error);
        assert(img);
        assert((*img)[0].rgba[3] == 255);           // pixel 0 uses index 2 (opaque midpoint)
        assert((*img)[0].rgba[4 * 4 + 3] == 0);     // pixel 4 uses index 3 (transparent)
    }
    {   // DXT5: alpha endpoints 255 / 0, all alpha indices 0 -> 255; colour block white
        const Bytes px = Block({0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0});
        const Bytes file = DdsFile("DXT5", 4, 4, 1, px);
        auto info = ParseDds(file, &error);
        assert(info && info->format == DdsFormat::Dxt5);
        auto img = DecodeDds(file, *info, 8, &error);
        assert(img && (*img)[0].rgba[0] == 255 && (*img)[0].rgba[3] == 255);
    }
    {   // DXT3 explicit alpha nibble 0x8 -> 136
        Bytes px = Block({0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0});
        auto img = DecodeDds(DdsFile("DXT3", 4, 4, 1, px), *ParseDds(DdsFile("DXT3", 4, 4, 1, px), &error), 8, &error);
        assert(img && (*img)[0].rgba[3] == 136);
    }
    {   // mip chain: 8x8 DXT1 with 4 declared levels but only 2 present -> trimmed to 2
        Bytes px(8 * 4 + 8, std::byte{0});
        const Bytes file = DdsFile("DXT1", 8, 8, 4, px);
        auto info = ParseDds(file, &error);
        assert(info && info->mip_count == 2);
        auto img = DecodeDds(file, *info, 8, &error);
        assert(img && img->size() == 2 && (*img)[1].width == 4);
    }
    {   // unsupported fourcc is reported by name; truncated and garbage files fail cleanly
        const Bytes file = DdsFile("ATI2", 4, 4, 1, Bytes(16));
        auto info = ParseDds(file, &error);
        assert(info && info->format == DdsFormat::Unsupported && info->format_name == "ATI2");
        assert(!DecodeDds(file, *info, 8, &error));
        assert(!ParseDds(Bytes(10), &error));
        assert(!ParseDds(DdsFile("DXT1", 64, 64, 1, Bytes(8)), &error));
        Bytes bad = DdsFile("DXT1", 4, 4, 1, Bytes(8)); bad[0] = std::byte{'X'};
        assert(!ParseDds(bad, &error));
    }
    {   // TPF with two textures, UTF-16 names, entry stride 0x14, payloads are DDS files
        const Bytes d1 = DdsFile("DXT1", 4, 4, 1, Block({0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0}));
        const Bytes d2 = DdsFile("DXT1", 4, 4, 1, Block({0x1F, 0x00, 0x00, 0xF8, 0, 0, 0, 0}));
        const std::uint32_t entries = 16, names = entries + 2 * 0x14;
        const std::string n1 = "m10_0000_a", n2 = "m10_0000_n";
        const std::uint32_t n1_off = names, n2_off = names + 2 * (n1.size() + 1);
        const std::uint32_t data = (n2_off + 2 * (n2.size() + 1) + 15) & ~15u;
        Bytes b;
        for (char c : std::string("TPF\0", 4)) b.push_back(static_cast<std::byte>(c));
        P32(b, static_cast<std::uint32_t>(d1.size() + d2.size())); P32(b, 2);
        b.push_back(std::byte{0}); b.push_back(std::byte{3}); b.push_back(std::byte{1}); b.push_back(std::byte{0});
        P32(b, data); P32(b, static_cast<std::uint32_t>(d1.size())); b.push_back(std::byte{1}); b.push_back(std::byte{0});
        b.push_back(std::byte{1}); b.push_back(std::byte{0}); P32(b, n1_off); P32(b, 0);
        P32(b, data + static_cast<std::uint32_t>(d1.size())); P32(b, static_cast<std::uint32_t>(d2.size()));
        b.push_back(std::byte{1}); b.push_back(std::byte{0}); b.push_back(std::byte{1}); b.push_back(std::byte{0});
        P32(b, n2_off); P32(b, 0);
        for (const std::string* n : {&n1, &n2}) { for (char c : *n) { b.push_back(static_cast<std::byte>(c)); b.push_back(std::byte{0}); } b.push_back(std::byte{0}); b.push_back(std::byte{0}); }
        b.resize(data, std::byte{0});
        b.insert(b.end(), d1.begin(), d1.end());
        b.insert(b.end(), d2.begin(), d2.end());
        assert(IsTpf(b));
        auto idx = ParseTpf(b, b.size(), &error);
        assert(idx && idx->textures.size() == 2 && idx->entry_stride == 0x14);
        assert(idx->textures[0].name == n1 && idx->textures[1].name == n2 && idx->textures[1].starts_with_dds);
        // header + entries + names only (a prefix): still parses, DDS magic unknown
        auto prefix = ParseTpf(std::span<const std::byte>(b).first(data), b.size(), &error);
        assert(prefix && prefix->textures.size() == 2 && !prefix->textures[0].starts_with_dds);
        // the texture bytes decode
        const auto& t = idx->textures[1];
        auto span = std::span<const std::byte>(b).subspan(t.offset, t.size);
        auto info = ParseDds(span, &error);
        assert(info);
        auto img = DecodeDds(span, *info, 4, &error);
        assert(img && (*img)[0].rgba[2] == 255); // blue
        // damaged: data range past the file, garbage name offset, bad magic
        Bytes cut(b.begin(), b.begin() + data + 10);
        assert(!ParseTpf(cut, cut.size(), &error));
        Bytes bad = b; bad[0] = std::byte{'X'};
        assert(!ParseTpf(bad, bad.size(), &error));
        assert(!ParseTpf(Bytes(8), 8, &error));
    }
    std::puts("texture_test: ok");
    return 0;
}
