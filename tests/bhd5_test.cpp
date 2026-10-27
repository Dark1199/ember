// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vfs/bhd5.h"
#include "vfs/vfs.h"

namespace fs = std::filesystem;
using ember::vfs::Bhd5Archive;
using ember::vfs::ParseBhd5;
using ember::vfs::PathHash;

namespace {

struct Packed {
    std::string path;
    std::string content;
};

struct Writer {
    bool big = false;
    std::vector<std::byte> out;
    template <typename T>
    void Put(T value) {
        std::byte raw[sizeof(T)];
        std::memcpy(raw, &value, sizeof(T)); // host is little-endian
        if (big) {
            for (size_t i = 0; i < sizeof(T) / 2; ++i) std::swap(raw[i], raw[sizeof(T) - 1 - i]);
        }
        out.insert(out.end(), raw, raw + sizeof(T));
    }
    void Bytes(const char* text, size_t n) {
        for (size_t i = 0; i < n; ++i) out.push_back(static_cast<std::byte>(text[i]));
    }
};

// Builds a data file and an index with two buckets following the documented layout.
void BuildArchive(const std::vector<Packed>& files, bool big, std::vector<std::byte>& bhd,
                  std::vector<std::byte>& bdt, std::int64_t bad_offset = -1) {
    Writer data;
    data.Bytes("BDF3", 4);
    data.Bytes("07D7R6\0\0\0\0\0\0", 12);
    struct Placed { uint32_t hash; int32_t size; int64_t offset; };
    std::vector<Placed> placed;
    for (const Packed& f : files) {
        const int64_t offset = static_cast<int64_t>(data.out.size());
        data.Bytes(f.content.data(), f.content.size());
        while (data.out.size() % 16) data.out.push_back(std::byte{0});
        placed.push_back({PathHash(f.path), static_cast<int32_t>(data.out.size() - offset), offset});
    }
    if (bad_offset >= 0) placed.push_back({0xDEADBEEFu, 16, bad_offset});
    bdt = data.out;

    Writer idx;
    idx.big = big;
    idx.Bytes("BHD5", 4);
    idx.out.push_back(static_cast<std::byte>(big ? 0x00 : 0xFF));
    for (int i = 0; i < 3; ++i) idx.out.push_back(std::byte{0});
    idx.Put<int32_t>(1);          // 0x08
    const size_t size_at = idx.out.size();
    idx.Put<int32_t>(0);          // 0x0C file size, patched below
    idx.Put<int32_t>(2);          // 0x10 bucket count
    idx.Put<int32_t>(24);         // 0x14 bucket table offset
    // Split entries between two buckets.
    const size_t half = placed.size() / 2;
    const int32_t table_end = 24 + 2 * 8;
    idx.Put<int32_t>(static_cast<int32_t>(half));
    idx.Put<int32_t>(table_end);
    idx.Put<int32_t>(static_cast<int32_t>(placed.size() - half));
    idx.Put<int32_t>(table_end + static_cast<int32_t>(half) * 16);
    for (const Placed& p : placed) {
        idx.Put<uint32_t>(p.hash);
        idx.Put<int32_t>(p.size);
        idx.Put<int64_t>(p.offset);
    }
    const int32_t total = static_cast<int32_t>(idx.out.size());
    Writer patch;
    patch.big = big;
    patch.Put<int32_t>(total);
    std::memcpy(idx.out.data() + size_at, patch.out.data(), 4);
    bhd = idx.out;
}

void WriteFile(const fs::path& path, const std::vector<std::byte>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string AsString(const std::vector<std::byte>& v, size_t n) {
    return std::string(reinterpret_cast<const char*>(v.data()), n);
}

} // namespace

int main() {
    // Hash: '/' ensured, lowercase, backslash converted, hash = hash * 37 + c.
    assert(PathHash("A") == 47u * 37u + 'a');
    assert(PathHash("\\a") == PathHash("/a"));
    assert(PathHash("  /MAP/File.BIN ") == PathHash("map\\file.bin"));
    assert(PathHash("/ab") == (47u * 37u + 'a') * 37u + 'b');

    const std::vector<Packed> files = {{"/map/a.bin", "alpha"},
                                       {"/chr/b.bin", "bravo!"},
                                       {"/event/c.bin", "charlie"}};
    for (const bool big : {false, true}) {
        std::vector<std::byte> bhd, bdt;
        BuildArchive(files, big, bhd, bdt);
        std::string error;
        auto index = ParseBhd5(bhd, bdt.size(), &error);
        assert(index && error.empty());
        assert(index->big_endian == big);
        assert(index->entries.size() == 3 && index->invalid_entries == 0);
        for (size_t i = 1; i < index->entries.size(); ++i)
            assert(index->entries[i - 1].name_hash <= index->entries[i].name_hash);
    }

    // An entry pointing outside the data file is skipped, not trusted.
    {
        std::vector<std::byte> bhd, bdt;
        BuildArchive(files, false, bhd, bdt, /*bad_offset=*/1 << 20);
        auto index = ParseBhd5(bhd, bdt.size(), nullptr);
        assert(index && index->entries.size() == 3 && index->invalid_entries == 1);
    }

    // Garbage and truncation fail cleanly, with the first bytes in the message.
    {
        std::vector<std::byte> junk(64, std::byte{0x41});
        std::string error;
        assert(!ParseBhd5(junk, 1000, &error));
        assert(error.find("41 41 41") != std::string::npos);
        std::vector<std::byte> bhd, bdt;
        BuildArchive(files, false, bhd, bdt);
        bhd.resize(30); // cut inside the bucket table
        assert(!ParseBhd5(bhd, bdt.size(), &error));
    }

    // End to end through the VFS: names differ in case, backslashes work, loose files win.
    {
        const fs::path root = fs::temp_directory_path() / "ember_bhd5_test";
        fs::remove_all(root);
        std::vector<std::byte> bhd, bdt;
        BuildArchive(files, false, bhd, bdt);
        WriteFile(root / "DVDBND9.BHD5", bhd);
        WriteFile(root / "dvdbnd9.bdt", bdt);
        WriteFile(root / "orphan.bhd5", bhd); // no .bdt: reported, not fatal
        const std::string loose = "LOOSE";
        WriteFile(root / "map" / "a.bin",
                  std::vector<std::byte>(reinterpret_cast<const std::byte*>(loose.data()),
                                         reinterpret_cast<const std::byte*>(loose.data()) + 5));

        ember::vfs::Vfs vfs;
        std::string error;
        assert(vfs.MountDirectory(root, &error));
        std::vector<std::string> errors;
        assert(vfs.MountArchivesFromIndex(&errors) == 1);
        assert(errors.size() == 1);
        assert(vfs.ArchiveCount() == 1 && vfs.PackedFileCount() == 3);

        const auto bravo = vfs.ReadAll("CHR\\B.BIN");
        assert(bravo && bravo->size() == 16 && AsString(*bravo, 6) == "bravo!");
        assert(vfs.Exists("/event/c.bin") && !vfs.Exists("/event/missing.bin"));
        assert(vfs.Size("event/c.bin") == 16u);
        const auto a = vfs.ReadAll("map/a.bin"); // loose copy overrides the packed one
        assert(a && AsString(*a, 5) == "LOOSE");
        fs::remove_all(root);
    }
    std::puts("bhd5_test: ok");
    return 0;
}
