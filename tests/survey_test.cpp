// SPDX-License-Identifier: GPL-3.0-or-later
#include <zlib.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vfs/bhd5.h"
#include "vfs/survey.h"

namespace fs = std::filesystem;
using namespace ember::vfs;
using Bytes = std::vector<std::byte>;

namespace {

template <typename T>
void PutLE(Bytes& out, T v) {
    for (size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
void PutBE(Bytes& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::byte>(v >> s));
}
void Ascii(Bytes& out, const std::string& s) {
    for (char c : s) out.push_back(static_cast<std::byte>(c));
}

Bytes Bnd3() { // one named entry "a.txt" = "hello"
    Bytes b;
    Ascii(b, "BND3");
    Ascii(b, std::string("07D7R6\0\0", 8));
    b.push_back(std::byte{0x04});
    b.push_back(std::byte{0});
    b.push_back(std::byte{0});
    b.push_back(std::byte{0});
    PutLE<std::int32_t>(b, 1);
    PutLE<std::int32_t>(b, 0x40);
    PutLE<std::uint64_t>(b, 0);
    b.push_back(std::byte{0x40});
    b.push_back(std::byte{0}); b.push_back(std::byte{0}); b.push_back(std::byte{0});
    PutLE<std::int32_t>(b, 5);
    PutLE<std::uint32_t>(b, 0x40);
    PutLE<std::uint32_t>(b, 0x30);
    Ascii(b, std::string("a.txt\0", 6));
    while (b.size() < 0x40) b.push_back(std::byte{0});
    Ascii(b, "hello");
    return b;
}

Bytes Dcx(const Bytes& content, bool corrupt) {
    uLongf size = compressBound(static_cast<uLong>(content.size()));
    Bytes packed(size);
    const int rc = compress2(reinterpret_cast<Bytef*>(packed.data()), &size,
                             reinterpret_cast<const Bytef*>(content.data()),
                             static_cast<uLong>(content.size()), 9);
    assert(rc == Z_OK);
    packed.resize(size);
    if (corrupt) packed[packed.size() / 2] ^= std::byte{0xFF};
    Bytes out;
    Ascii(out, std::string("DCX\0", 4));
    PutBE(out, 0x10000); PutBE(out, 0x18); PutBE(out, 0x24); PutBE(out, 0x24); PutBE(out, 0x2C);
    Ascii(out, std::string("DCS\0", 4));
    PutBE(out, static_cast<std::uint32_t>(content.size()));
    PutBE(out, static_cast<std::uint32_t>(packed.size()));
    Ascii(out, std::string("DCP\0", 4));
    Ascii(out, "DFLT");
    PutBE(out, 0x20);
    for (int i = 0; i < 5; ++i) PutBE(out, 0x09000000);
    Ascii(out, std::string("DCA\0", 4));
    PutBE(out, 8);
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
}

Bytes Flver() { // minimal plausible header + 0x40 data bytes
    Bytes b;
    Ascii(b, std::string("FLVER\0L\0", 8));
    PutLE<std::int32_t>(b, 0x2000C); PutLE<std::int32_t>(b, 0x5C); PutLE<std::int32_t>(b, 0x40);
    for (std::int32_t c : {1, 2, 3, 2, 2}) PutLE<std::int32_t>(b, c);
    const float box[6] = {-1.f, -2.f, -3.f, 1.f, 2.f, 3.f};
    for (float f : box) { std::uint32_t u; std::memcpy(&u, &f, 4); PutLE<std::uint32_t>(b, u); }
    PutLE<std::int32_t>(b, 100); PutLE<std::int32_t>(b, 120);
    b.push_back(std::byte{16}); b.push_back(std::byte{1}); b.push_back(std::byte{0}); b.push_back(std::byte{0});
    for (std::int32_t c : {0, 1, 1, 1}) PutLE<std::int32_t>(b, c);
    b.resize(0x5C + 0x40, std::byte{0});
    return b;
}

Bytes Bhf3() { // two files in a 0x30-byte BDF3: ids + names + comp, flags 0x74
    Bytes b;
    Ascii(b, "BHF3");
    Ascii(b, std::string("07D7R6\0\0", 8));
    b.push_back(std::byte{0x74}); b.push_back(std::byte{0}); b.push_back(std::byte{0}); b.push_back(std::byte{0});
    PutLE<std::int32_t>(b, 2);
    PutLE<std::uint64_t>(b, 0);
    PutLE<std::uint32_t>(b, 0);
    const std::uint32_t names = 0x20 + 2 * 24;
    const std::uint32_t off[2] = {0x10, 0x20};
    for (int i = 0; i < 2; ++i) {
        b.push_back(std::byte{0x40}); b.push_back(std::byte{0}); b.push_back(std::byte{0}); b.push_back(std::byte{0});
        PutLE<std::uint32_t>(b, 0x10);
        PutLE<std::uint32_t>(b, off[i]);
        PutLE<std::int32_t>(b, i);
        PutLE<std::uint32_t>(b, names + 3 * i);
        PutLE<std::uint32_t>(b, 0x10);
    }
    Ascii(b, std::string("a\0b\0", 4));
    return b;
}

bool Has(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& l : lines) if (l.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "ember_survey_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::vector<Bytes> contents = {
        Dcx(Bnd3(), false), Dcx(Bnd3(), true), Bnd3(), [] { Bytes b; Ascii(b, "FSB5zzzz"); return b; }(),
        Dcx(Flver(), false), Bhf3(), [] { Bytes b; Ascii(b, "BDF3"); Ascii(b, std::string(12, '0')); b.resize(0x30, std::byte{0}); return b; }()};
    Bytes bdt;
    Ascii(bdt, "BDF3");
    Ascii(bdt, std::string("07D7R6\0\0\0\0\0\0", 12));
    struct Placed { std::uint32_t hash; std::int32_t size; std::int64_t offset; };
    std::vector<Placed> placed;
    std::uint32_t hash = 100;
    for (const Bytes& c : contents) {
        const auto offset = static_cast<std::int64_t>(bdt.size());
        bdt.insert(bdt.end(), c.begin(), c.end());
        while (bdt.size() % 16) bdt.push_back(std::byte{0});
        placed.push_back({hash++, static_cast<std::int32_t>(bdt.size() - offset), offset});
    }
    placed.push_back({999, 64, 1 << 20}); // outside the data file: must be counted as rejected

    Bytes bhd;
    Ascii(bhd, "BHD5");
    bhd.push_back(std::byte{0xFF}); bhd.push_back(std::byte{0}); bhd.push_back(std::byte{0}); bhd.push_back(std::byte{0});
    PutLE<std::int32_t>(bhd, 1);
    PutLE<std::int32_t>(bhd, 0); // file size, patched below
    PutLE<std::int32_t>(bhd, 1);
    PutLE<std::int32_t>(bhd, 0x18);
    PutLE<std::int32_t>(bhd, static_cast<std::int32_t>(placed.size()));
    PutLE<std::int32_t>(bhd, 0x20);
    for (const Placed& p : placed) {
        PutLE<std::uint32_t>(bhd, p.hash);
        PutLE<std::int32_t>(bhd, p.size);
        PutLE<std::int64_t>(bhd, p.offset);
    }
    const auto total = static_cast<std::int32_t>(bhd.size());
    std::memcpy(bhd.data() + 0x0C, &total, 4);

    std::ofstream(dir / "t.bdt", std::ios::binary).write(reinterpret_cast<const char*>(bdt.data()), static_cast<std::streamsize>(bdt.size()));
    std::ofstream(dir / "t.bhd5", std::ios::binary).write(reinterpret_cast<const char*>(bhd.data()), static_cast<std::streamsize>(bhd.size()));

    std::string error;
    auto archive = Bhd5Archive::Open(dir / "t.bhd5", dir / "t.bdt", &error);
    assert(archive && error.empty());
    const auto lines = SurveyArchives({std::move(*archive)});
    for (const auto& l : lines) std::puts(l.c_str());

    assert(Has(lines, "survey t.bdt: index little-endian, 7 entries (1 rejected as out of range)"));
    assert(Has(lines, "16-aligned 7/7"));
    assert(Has(lines, "DCX. x3"));
    assert(Has(lines, "BND3 x1"));
    assert(Has(lines, "FSB5 x1"));
    assert(Has(lines, "DCX sample 3: ok 2, failed 1; methods DFLT x3"));
    assert(Has(lines, "decoded content starts with BND3 x1  FLVE x1"));
    assert(Has(lines, "first DCX failure:"));
    assert(Has(lines, "BHF3 sample: parsed 1, failed 0, 2 entries; 1 have a size-matching BDF3 in this archive (1 BDF3 files)"));
    assert(Has(lines, "BHF3 flags 0x74 -> stride 24 ids 1 names 1 comp 1 long 0 x1"));
    assert(Has(lines, "FLVER header sample (direct or inside bundles): ok 1, failed 0; versions 0x2000c x1; byte order little x1; vertex index bits 16 x1; meshes 2..2, materials 2..2, bones 3..3"));
    assert(Has(lines, "BND sample (plain or inside DCX): parsed 2, failed 0, layout-corrected 0, 2 bundle entries"));

    // Full verification: the corrupt DCX entry must be reported, the rest read back cleanly.
    {
        auto again = Bhd5Archive::Open(dir / "t.bhd5", dir / "t.bdt", &error);
        assert(again);
        std::vector<std::string> log;
        std::vector<Bhd5Archive> one;
        one.push_back(std::move(*again));
        const bool ok = VerifyArchives(one, [&](const std::string& l) { log.push_back(l); });
        for (const auto& l : log) std::puts(l.c_str());
        assert(!ok);
        assert(Has(log, "DCX FAILED entry hash"));
        assert(Has(log, "PROBLEMS FOUND: 7 entries"));
        assert(Has(log, "DCX 3 (ok 2, failed 1)"));
        assert(Has(log, "verify: PROBLEMS FOUND"));
    }

    fs::remove_all(dir);
    std::puts("survey_test: ok");
    return 0;
}
