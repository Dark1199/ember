// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the BHF3 split-bundle header reader and the FLVER header reader. Test data is
// built here from the layouts documented in vfs/bnd.h and formats/flver.h.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "formats/flver.h"
#include "vfs/bnd.h"

using namespace ember;
using Bytes = std::vector<std::byte>;

namespace {

struct Out {
    bool big;
    Bytes b;
    template <typename T>
    void Put(T v) {
        std::byte raw[sizeof(T)];
        std::memcpy(raw, &v, sizeof(T));
        if (big) for (size_t i = 0; i < sizeof(T) / 2; ++i) std::swap(raw[i], raw[sizeof(T) - 1 - i]);
        b.insert(b.end(), raw, raw + sizeof(T));
    }
    void Ascii(const std::string& s, size_t pad = 0) {
        for (char c : s) b.push_back(static_cast<std::byte>(c));
        for (size_t i = s.size(); i < pad; ++i) b.push_back(std::byte{0});
    }
};

struct File { std::string name; std::uint32_t size; std::uint32_t offset; std::int32_t id; };

// BHF3 header file. `with_ids` / `with_comp` choose the table shape; the flag byte is always
// 0x74, as in real files, so the layout has to be found from the data.
Bytes BuildBhf3(const std::vector<File>& files, bool with_ids, bool with_comp, bool big = false) {
    const size_t stride = 8 + 4 + (with_ids ? 4 : 0) + 4 + (with_comp ? 4 : 0);
    size_t name_pos = 0x20 + files.size() * stride;
    Out o{big, {}};
    o.Ascii("BHF3");
    o.Ascii("07D7R6", 8);
    o.b.push_back(std::byte{0x74});
    o.b.push_back(std::byte{static_cast<unsigned char>(big ? 1 : 0)});
    o.b.push_back(std::byte{0});
    o.b.push_back(std::byte{0});
    o.Put<std::int32_t>(static_cast<std::int32_t>(files.size()));
    o.Put<std::uint64_t>(0);
    o.Put<std::uint32_t>(0);
    std::vector<size_t> name_off;
    for (const File& f : files) { name_off.push_back(name_pos); name_pos += f.name.size() + 1; }
    for (size_t i = 0; i < files.size(); ++i) {
        o.b.push_back(std::byte{0x40});
        o.b.push_back(std::byte{0}); o.b.push_back(std::byte{0}); o.b.push_back(std::byte{0});
        o.Put<std::uint32_t>(files[i].size);
        o.Put<std::uint32_t>(files[i].offset);
        if (with_ids) o.Put<std::int32_t>(files[i].id);
        o.Put<std::uint32_t>(static_cast<std::uint32_t>(name_off[i]));
        if (with_comp) o.Put<std::uint32_t>(files[i].size);
    }
    for (const File& f : files) { o.Ascii(f.name); o.b.push_back(std::byte{0}); }
    return o.b;
}

Bytes Flver(bool big, std::int32_t data_offset, std::int32_t data_size, float bb_min0, float bb_max0,
            size_t total_size) {
    Out o{big, {}};
    o.Ascii(std::string("FLVER\0", 6));
    o.Ascii(big ? std::string("B\0", 2) : std::string("L\0", 2));
    o.Put<std::int32_t>(0x2000C);
    o.Put<std::int32_t>(data_offset);
    o.Put<std::int32_t>(data_size);
    for (std::int32_t c : {2, 3, 17, 4, 4}) o.Put<std::int32_t>(c); // dummy,material,bone,mesh,vbuf
    o.Put<float>(bb_min0); o.Put<float>(-2.0f); o.Put<float>(-3.0f);
    o.Put<float>(bb_max0); o.Put<float>(2.0f); o.Put<float>(3.0f);
    o.Put<std::int32_t>(1200); o.Put<std::int32_t>(1500);
    o.b.push_back(std::byte{16}); o.b.push_back(std::byte{1}); o.b.push_back(std::byte{0}); o.b.push_back(std::byte{0});
    o.Put<std::int32_t>(0); o.Put<std::int32_t>(8); o.Put<std::int32_t>(5); o.Put<std::int32_t>(6);
    o.b.resize(total_size, std::byte{0});
    return o.b;
}

} // namespace

int main() {
    const std::vector<File> files = {
        {"N:\\FRPG\\data\\Other\\tex\\a.tpf", 100, 0x10, 1},
        {"b.tpf", 32, 0x80, 2},
        {"C:\\x\\empty.bin", 0, 0xA0, 3},
    };
    // Table shapes with the same 0x74 flag byte; little and big endian; data size known/unknown.
    for (const bool big : {false, true}) {
        for (const bool ids : {false, true}) {
            for (const bool comp : {false, true}) {
                if (!ids && comp) continue; // same width as ids+names: indistinguishable from it
                const Bytes header = BuildBhf3(files, ids, comp, big);
                assert(ember::vfs::IsBhf3(header));
                std::string error;
                for (const auto known : {std::optional<std::uint64_t>(0xA0), std::optional<std::uint64_t>()}) {
                    auto idx = ember::vfs::ParseBhf3(header, known, &error);
                    assert(idx && error.empty());
                    assert(idx->big_endian == big && idx->entries.size() == 3);
                    assert(idx->entries[0].name == "N:\\FRPG\\data\\Other\\tex\\a.tpf");
                    assert(idx->entries[0].offset == 0x10 && idx->entries[0].stored_size == 100);
                    assert(idx->entries[1].offset == 0x80 && idx->entries[2].stored_size == 0);
                    assert(idx->data_extent == 0xA0);
                    assert(idx->has_names && idx->version == "07D7R6");
                    if (ids) assert(idx->entries[1].id == 2);
                }
            }
        }
    }
    // Entries beyond the paired data file are rejected when its size is known.
    {
        const Bytes header = BuildBhf3(files, true, true);
        std::string error;
        assert(!ember::vfs::ParseBhf3(header, std::uint64_t{0x60}, &error));
        assert(error.find("BHF3") != std::string::npos);
    }
    // Not a BHF3 / truncated / corrupt table.
    {
        std::string error;
        Bytes junk(64, std::byte{0x11});
        assert(!ember::vfs::IsBhf3(junk) && !ember::vfs::ParseBhf3(junk, std::nullopt, &error));
        Bytes header = BuildBhf3(files, true, true);
        header.resize(0x30);
        assert(!ember::vfs::ParseBhf3(header, std::nullopt, &error));
        Bytes corrupt = BuildBhf3(files, true, true);
        for (size_t i = 0x20; i < 0x20 + 3 * 24; ++i) corrupt[i] = std::byte{0x01};
        assert(!ember::vfs::ParseBhf3(corrupt, std::nullopt, &error));
    }

    // SplitBundle: header + BDF3 data, lookup, raw views, DCX-free read.
    {
        Bytes data;
        for (char c : std::string("BDF3")) data.push_back(static_cast<std::byte>(c));
        data.resize(0x10, std::byte{0});
        data.resize(0xA0, std::byte{0});
        for (int i = 0; i < 100; ++i) data[0x10 + i] = static_cast<std::byte>('a' + i % 26);
        const Bytes header = BuildBhf3(files, true, true);
        std::string error;
        auto bundle = ember::vfs::SplitBundle::Open(header, data, &error);
        assert(bundle && bundle->Count() == 3);
        const auto* a = bundle->Find("A.TPF");
        assert(a && a->stored_size == 100 && bundle->Raw(*a).size() == 100);
        assert(static_cast<char>(bundle->Raw(*a)[27]) == 'b');
        assert(bundle->Find("n:/frpg/data/other/tex/a.tpf") == a);
        assert(bundle->Find("nothing") == nullptr && bundle->Find("tpf") == nullptr);
        auto read = bundle->Read(*bundle->Find("b.tpf"), &error);
        assert(read && read->size() == 32);
        assert(bundle->Raw(*bundle->Find("empty.bin")).empty());
        // Wrong magic on the data file, and data shorter than the header requires.
        Bytes bad = data; bad[0] = std::byte{'X'};
        assert(!ember::vfs::SplitBundle::Open(header, bad, &error));
        Bytes shorter(data.begin(), data.begin() + 0x60);
        assert(!ember::vfs::SplitBundle::Open(header, shorter, &error));
    }

    // FLVER header.
    {
        std::string error;
        for (const bool big : {false, true}) {
            const Bytes ok = Flver(big, 0x100, 0x200, -1.0f, 1.0f, 0x400);
            assert(formats::IsFlver(ok));
            auto h = formats::ParseFlverHeader(ok, &error);
            assert(h && h->big_endian == big && h->version == 0x2000C);
            assert(h->material_count == 3 && h->bone_count == 17 && h->mesh_count == 4);
            assert(h->vertex_buffer_count == 4 && h->face_count == 1200 && h->total_face_count == 1500);
            assert(h->vertex_index_bits == 16 && h->unicode && h->texture_count == 6);
            assert(h->bounds_min[2] == -3.0f && h->bounds_max[0] == 1.0f);
        }
        assert(!formats::ParseFlverHeader(Flver(false, 0x100, 0x200, -1.0f, 1.0f, 0x2FF), &error)); // data past EOF
        assert(error.find("outside the file") != std::string::npos);
        assert(!formats::ParseFlverHeader(Flver(false, 0x100, 0x200, 2.0f, 1.0f, 0x400), &error));  // inverted box
        assert(!formats::ParseFlverHeader(Flver(false, 0x100, 0x200, NAN, 1.0f, 0x400), &error));
        assert(!formats::ParseFlverHeader(Flver(false, 0x10, 0x20, -1.0f, 1.0f, 0x400), &error));    // data inside header
        Bytes bad = Flver(false, 0x100, 0x200, -1.0f, 1.0f, 0x400);
        bad[6] = std::byte{'X'};
        assert(!formats::ParseFlverHeader(bad, &error));
        bad = Flver(false, 0x100, 0x200, -1.0f, 1.0f, 0x400);
        bad[0] = std::byte{'G'};
        assert(!formats::IsFlver(bad) && !formats::ParseFlverHeader(bad, &error));
        bad.resize(0x40);
        assert(!formats::ParseFlverHeader(Bytes(0x40, std::byte{0}), &error));
    }
    std::puts("split_test: ok");
    return 0;
}
