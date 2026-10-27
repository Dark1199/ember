// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the BND3/BND4 bundle reader. The test files are built here from the documented
// layout (see vfs/bnd.h); they exercise the parser, not real game data.
#include <zlib.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vfs/bnd.h"
#include "vfs/dcx.h"
#include "vfs/vfs.h"

namespace fs = std::filesystem;
using namespace ember::vfs;
using Bytes = std::vector<std::byte>;

namespace {

class Writer {
public:
    explicit Writer(bool big_endian) : be_(big_endian) {}
    void U8(std::uint32_t v) { out.push_back(static_cast<std::byte>(v)); }
    void U16(std::uint32_t v) { Put(v, 2); }
    void U32(std::uint64_t v) { Put(v, 4); }
    void U64(std::uint64_t v) { Put(v, 8); }
    void Ascii(const std::string& s, std::size_t pad_to = 0) {
        for (char c : s) U8(static_cast<unsigned char>(c));
        for (std::size_t i = s.size(); i < pad_to; ++i) U8(0);
    }
    void Raw(const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); }
    void AlignTo(std::size_t n) { while (out.size() % n) U8(0); }
    void PatchU32(std::size_t at, std::uint64_t v) {
        for (int i = 0; i < 4; ++i) {
            const int shift = be_ ? (3 - i) * 8 : i * 8;
            out[at + i] = static_cast<std::byte>(v >> shift);
        }
    }
    Bytes out;

private:
    void Put(std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) {
            const int shift = be_ ? (n - 1 - i) * 8 : i * 8;
            out.push_back(static_cast<std::byte>(v >> shift));
        }
    }
    bool be_;
};

struct Item {
    std::string name;
    Bytes data;
    std::int32_t id;
    std::uint8_t flags = 0x40;
};

Bytes Text(const std::string& s) {
    Bytes b;
    for (char c : s) b.push_back(static_cast<std::byte>(c));
    return b;
}
std::string Str(const Bytes& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
std::string Str(std::span<const std::byte> b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// BND3. `flags` are written to the header; `layout_long` is what the entry table really uses,
// which lets a test make the flags lie.
Bytes BuildBnd3(const std::vector<Item>& items, std::uint8_t flags, bool big_endian,
                bool layout_long) {
    const bool ids = flags & 0x02, names = flags & 0x0C, comp = flags & 0x20;
    const std::size_t stride = 8 + (layout_long ? 8 : 4) + (ids ? 4 : 0) + (names ? 4 : 0) +
                               (comp ? 4 : 0);
    const std::size_t names_start = 0x20 + items.size() * stride;
    std::size_t names_end = names_start;
    std::vector<std::size_t> name_off;
    for (const Item& it : items) {
        name_off.push_back(names_end);
        names_end += it.name.size() + 1;
    }
    std::size_t data_pos = (names_end + 15) & ~std::size_t{15};

    Writer w(big_endian);
    w.Ascii("BND3");
    w.Ascii("07D7R6", 8);
    w.U8(flags);
    w.U8(big_endian ? 1 : 0);
    w.U8(0);
    w.U8(0);
    w.U32(items.size());
    w.U32(names_end);
    w.U64(0);
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < items.size(); ++i) {
        offsets.push_back(data_pos);
        w.U8(items[i].flags);
        w.U8(0); w.U8(0); w.U8(0);
        w.U32(items[i].data.size());
        if (layout_long) w.U64(data_pos); else w.U32(data_pos);
        if (ids) w.U32(static_cast<std::uint32_t>(items[i].id));
        if (names) w.U32(name_off[i]);
        if (comp) w.U32(items[i].data.size());
        data_pos += (items[i].data.size() + 15) & ~std::size_t{15};
    }
    for (const Item& it : items) { w.Ascii(it.name); w.U8(0); }
    for (const Item& it : items) { w.AlignTo(16); w.Raw(it.data); }
    return w.out;
}

// BND4. Names are ASCII, or UTF-16 when `unicode`. `extra_stride` pads each entry.
Bytes BuildBnd4(const std::vector<Item>& items, std::uint8_t flags, bool big_endian, bool unicode,
                std::size_t extra_stride = 0) {
    const bool ids = flags & 0x02, names = flags & 0x0C, comp = flags & 0x20,
               long_off = flags & 0x10;
    const std::size_t needed = 16 + (comp ? 8 : 0) + (long_off ? 8 : 4) + (ids ? 4 : 0) +
                               (names ? 4 : 0);
    const std::size_t stride = needed + extra_stride;
    const std::size_t names_start = 0x40 + items.size() * stride;

    // Name bytes (UTF-8 input -> UTF-16 when unicode; the test only uses BMP + one emoji).
    Writer nw(big_endian);
    std::vector<std::size_t> name_off;
    for (const Item& it : items) {
        name_off.push_back(names_start + nw.out.size());
        if (!unicode) {
            nw.Ascii(it.name);
            nw.U8(0);
        } else {
            // Minimal UTF-8 -> UTF-16 for the test strings.
            for (std::size_t i = 0; i < it.name.size();) {
                const unsigned char c = it.name[i];
                std::uint32_t cp;
                int len;
                if (c < 0x80) { cp = c; len = 1; }
                else if (c < 0xE0) { cp = c & 0x1F; len = 2; }
                else if (c < 0xF0) { cp = c & 0x0F; len = 3; }
                else { cp = c & 0x07; len = 4; }
                for (int k = 1; k < len; ++k) cp = (cp << 6) | (it.name[i + k] & 0x3F);
                i += len;
                if (cp >= 0x10000) {
                    cp -= 0x10000;
                    nw.U16(0xD800 + (cp >> 10));
                    nw.U16(0xDC00 + (cp & 0x3FF));
                } else {
                    nw.U16(cp);
                }
            }
            nw.U16(0);
        }
    }
    std::size_t data_pos = (names_start + nw.out.size() + 15) & ~std::size_t{15};

    Writer w(big_endian);
    w.Ascii("BND4");
    w.U8(0); w.U8(0); w.U8(0); w.U8(0); w.U8(0); // 0x04..0x08
    w.U8(big_endian ? 1 : 0);                      // 0x09
    w.U8(0); w.U8(0);                              // 0x0A, 0x0B
    w.U32(items.size());                           // 0x0C
    w.U64(0x40);                                   // 0x10
    w.Ascii("07D7R6", 8);                          // 0x18
    w.U64(stride);                                 // 0x20
    w.U64(names_start + nw.out.size());            // 0x28
    w.U8(unicode ? 1 : 0);                         // 0x30
    w.U8(flags);                                   // 0x31
    w.U8(0); w.U8(0);                              // 0x32, 0x33
    w.U32(0);                                      // 0x34
    w.U64(0);                                      // 0x38
    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::size_t begin = w.out.size();
        w.U8(items[i].flags);
        w.U8(0); w.U8(0); w.U8(0);
        w.U32(0xFFFFFFFF);
        w.U64(items[i].data.size());
        if (comp) w.U64(items[i].data.size());
        if (long_off) w.U64(data_pos); else w.U32(data_pos);
        if (ids) w.U32(static_cast<std::uint32_t>(items[i].id));
        if (names) w.U32(name_off[i]);
        while (w.out.size() - begin < stride) w.U8(0xEE);
        data_pos += (items[i].data.size() + 15) & ~std::size_t{15};
    }
    w.Raw(nw.out);
    for (const Item& it : items) { w.AlignTo(16); w.Raw(it.data); }
    return w.out;
}

Bytes Deflate(const Bytes& in) {
    uLongf size = compressBound(static_cast<uLong>(in.size()));
    Bytes out(size);
    const int rc = compress2(reinterpret_cast<Bytef*>(out.data()), &size,
                             reinterpret_cast<const Bytef*>(in.data()),
                             static_cast<uLong>(in.size()), 9);
    assert(rc == Z_OK);
    (void)rc;
    out.resize(size);
    return out;
}

Bytes MakeDcx(const Bytes& payload) {
    const Bytes packed = Deflate(payload);
    Writer w(true);
    w.Ascii("DCX"); w.U8(0);
    w.U32(0x10000); w.U32(0x18); w.U32(0x24); w.U32(0x24); w.U32(0x2C);
    w.Ascii("DCS"); w.U8(0); w.U32(payload.size()); w.U32(packed.size());
    w.Ascii("DCP"); w.U8(0); w.Ascii("DFLT"); w.U32(0x20);
    for (int i = 0; i < 5; ++i) w.U32(0x09000000);
    w.Ascii("DCA"); w.U8(0); w.U32(8);
    w.Raw(packed);
    return w.out;
}

std::vector<Item> SampleItems() {
    return {
        {"N:\\FRPG\\data\\INTERROOT_win32\\chr\\c1000\\c1000.flver", Text("flver-bytes-0123456789"), 100},
        {"N:\\FRPG\\data\\INTERROOT_win32\\chr\\c1000\\c1000.hkx", Text("hk"), 200},
        {"readme.txt", Text(""), 300},
    };
}

void CheckSample(const BndIndex& idx, bool has_ids, bool has_names) {
    assert(idx.entries.size() == 3);
    assert(idx.entries[0].stored_size == 22);
    assert(idx.entries[1].stored_size == 2);
    assert(idx.entries[2].stored_size == 0);
    assert(idx.entries[0].id == (has_ids ? 100 : -1));
    assert(idx.entries[1].id == (has_ids ? 200 : -1));
    if (has_names) {
        assert(idx.entries[2].name == "readme.txt");
    } else {
        assert(idx.entries[2].name.empty());
    }
}

} // namespace

int main() {
    const std::vector<Item> items = SampleItems();

    // BND3 little endian, ids + names + compression field, 32-bit offsets (flags 0x2E).
    {
        const Bytes file = BuildBnd3(items, 0x2E, false, false);
        std::string error;
        auto archive = BndArchive::Open(file, &error);
        assert(archive && error.empty());
        const BndIndex& idx = archive->Index();
        assert(idx.kind == BndKind::Bnd3 && !idx.big_endian && !idx.layout_corrected);
        assert(idx.version == "07D7R6" && idx.format_flags == 0x2E);
        CheckSample(idx, true, true);
        assert(idx.entries[0].uncompressed_size == 22);

        // Lookup: full path, other case/separators, trailing components, missing.
        const BndEntry* e = archive->Find("n:/frpg/data/interroot_win32/chr/c1000/c1000.flver");
        assert(e && Str(archive->Raw(*e)) == "flver-bytes-0123456789");
        assert(archive->Find("C1000\\C1000.HKX") == &idx.entries[1]);
        assert(archive->Find("c1000.flver") == &idx.entries[0]);
        assert(archive->Find("flver") == nullptr);          // not a whole path component
        assert(archive->Find("c1000/c1000") == nullptr);
        assert(archive->Find("") == nullptr);
        assert(archive->Find("readme.txt") == &idx.entries[2]);
        assert(archive->Raw(idx.entries[2]).empty());
    }

    // BND3 big endian, names only, long offsets (flags 0x54).
    {
        const Bytes file = BuildBnd3(items, 0x54, true, true);
        auto archive = BndArchive::Open(file, nullptr);
        assert(archive);
        assert(archive->Index().big_endian && archive->Index().long_offsets &&
               !archive->Index().layout_corrected);
        CheckSample(archive->Index(), false, true);
        assert(Str(archive->Raw(archive->Index().entries[1])) == "hk");
    }

    // BND3 where the long-offset flag lies about the table: parsed by the cross-check.
    {
        const Bytes lying_long = BuildBnd3(items, 0x54, false, false); // flag says long, table short
        auto a = BndArchive::Open(lying_long, nullptr);
        assert(a && a->Index().layout_corrected && !a->Index().long_offsets);
        CheckSample(a->Index(), false, true);
        const Bytes lying_short = BuildBnd3(items, 0x44, false, true); // flag says short, table long
        auto b = BndArchive::Open(lying_short, nullptr);
        assert(b && b->Index().layout_corrected && b->Index().long_offsets);
        CheckSample(b->Index(), false, true);
    }

    // BND4 little endian, ASCII names, ids, stride larger than the fields need.
    {
        const Bytes file = BuildBnd4(items, 0x2E, false, false, 8);
        auto archive = BndArchive::Open(file, nullptr);
        assert(archive);
        const BndIndex& idx = archive->Index();
        assert(idx.kind == BndKind::Bnd4 && !idx.unicode_names && !idx.big_endian);
        CheckSample(idx, true, true);
        assert(Str(archive->Raw(idx.entries[0])) == "flver-bytes-0123456789");
        assert(archive->Find("c1000.hkx") == &idx.entries[1]);
    }

    // BND4 big endian, UTF-16 names (non-ASCII + a surrogate pair), long offsets.
    {
        std::vector<Item> uni = {
            {"caf\xC3\xA9/\xF0\x9F\x98\x80.bin", Text("payload"), 7},
            {"plain.txt", Text("x"), 8},
        };
        const Bytes file = BuildBnd4(uni, 0x3E, true, true);
        auto archive = BndArchive::Open(file, nullptr);
        assert(archive);
        const BndIndex& idx = archive->Index();
        assert(idx.big_endian && idx.unicode_names && idx.long_offsets);
        assert(idx.entries[0].name == "caf\xC3\xA9/\xF0\x9F\x98\x80.bin");
        assert(idx.entries[0].id == 7 && idx.entries[1].id == 8);
        assert(Str(archive->Raw(idx.entries[0])) == "payload");
        assert(archive->Find("PLAIN.TXT") == &idx.entries[1]);
    }

    // BND4 with ids only: no names, so nothing can be found by name but entries are readable.
    {
        const Bytes file = BuildBnd4(items, 0x02, false, false);
        auto archive = BndArchive::Open(file, nullptr);
        assert(archive);
        CheckSample(archive->Index(), true, false);
        assert(archive->Find("c1000.hkx") == nullptr);
        assert(Str(archive->Raw(archive->Index().entries[1])) == "hk");
    }

    // Nested compression: a DCX entry inside a bundle, and a bundle inside a DCX.
    {
        const Bytes inner = Text(std::string(5000, 'q') + "tail");
        std::vector<Item> nested = {
            {"data\\packed.bin.dcx", MakeDcx(inner), 1},
            {"data\\plain.bin", Text("not wrapped"), 2},
        };
        const Bytes bundle = BuildBnd4(nested, 0x2E, false, false);
        const Bytes wrapped = MakeDcx(bundle);
        std::string error;
        auto archive = BndArchive::Open(wrapped, &error); // DCX around the bundle
        assert(archive && error.empty());
        const BndEntry* packed = archive->Find("packed.bin.dcx");
        assert(packed);
        assert(IsDcx(archive->Raw(*packed)));
        const auto decoded = archive->Read(*packed, &error);
        assert(decoded && Str(*decoded) == Str(inner));
        const auto plain = archive->Read(*archive->Find("plain.bin"), &error);
        assert(plain && Str(*plain) == "not wrapped");

        // A corrupt nested stream is reported, not crashed on.
        Bytes bad = nested[0].data;
        bad[bad.size() - 6] ^= std::byte{0xFF};
        nested[0].data = bad;
        auto broken = BndArchive::Open(BuildBnd4(nested, 0x2E, false, false), nullptr);
        assert(broken);
        assert(!broken->Read(*broken->Find("packed.bin.dcx"), &error));
        assert(!error.empty());
    }

    // Rejections.
    {
        std::string error;
        assert(!IsBnd(Text("BNDX")) && IsBnd(Text("BND3")) && IsBnd(Text("BND4")));
        assert(!ParseBnd(Text("hello world, not a bundle at all......."), &error));
        assert(error.find("bad magic") != std::string::npos);
        assert(error.find("first bytes") != std::string::npos);
        assert(!ParseBnd(Bytes{}, &error));

        const Bytes bnd3 = BuildBnd3(items, 0x2E, false, false);
        const Bytes bnd4 = BuildBnd4(items, 0x2E, false, false);

        Bytes shortened(bnd3.begin(), bnd3.begin() + 0x10);
        assert(!ParseBnd(shortened, &error) && error.find("header") != std::string::npos);
        shortened.assign(bnd4.begin(), bnd4.begin() + 0x30);
        assert(!ParseBnd(shortened, &error));

        // Huge entry counts must fail before allocating.
        Bytes huge3 = bnd3;
        huge3[0x10] = std::byte{0xFF}; huge3[0x11] = std::byte{0xFF};
        huge3[0x12] = std::byte{0xFF}; huge3[0x13] = std::byte{0x7F};
        assert(!ParseBnd(huge3, &error));
        Bytes huge4 = bnd4;
        huge4[0x0C] = std::byte{0xFF}; huge4[0x0D] = std::byte{0xFF};
        huge4[0x0E] = std::byte{0xFF}; huge4[0x0F] = std::byte{0x7F};
        assert(!ParseBnd(huge4, &error));
        Bytes negative = bnd3;
        negative[0x13] = std::byte{0x80}; // negative count
        assert(!ParseBnd(negative, &error));

        // Entry data past the end of the file.
        Bytes oob = bnd4;
        oob[0x40 + 8] = std::byte{0xFF}; oob[0x40 + 9] = std::byte{0xFF}; // stored size = 0xFFFF
        assert(!ParseBnd(oob, &error) && error.find("outside") != std::string::npos);
        Bytes cut(bnd4.begin(), bnd4.end() - 10); // last entry loses its tail
        assert(!ParseBnd(cut, &error));

        // Stride too small for the flags / header size wrong.
        Bytes small_stride = bnd4;
        small_stride[0x20] = std::byte{12};
        assert(!ParseBnd(small_stride, &error) && error.find("stride") != std::string::npos);
        Bytes bad_header = bnd4;
        bad_header[0x10] = std::byte{0x50};
        assert(!ParseBnd(bad_header, &error));

        // Unterminated name: point the first name at the very end of the file, whose last
        // bytes are non-zero payload.
        std::vector<Item> tail = {{"a", Text("zzzzzzzzzzzzzzzz"), 1}};
        Bytes unterminated = BuildBnd4(tail, 0x2E, false, false);
        const std::size_t name_field = 0x40 + 16 + 8 + 4 + 4;
        const std::uint32_t last = static_cast<std::uint32_t>(unterminated.size() - 1);
        std::memcpy(&unterminated[name_field], &last, 4);
        assert(!ParseBnd(unterminated, &error) && error.find("name") != std::string::npos);
    }

    // Robustness sweep: every truncation and every single-byte corruption of valid files must
    // return cleanly (parse or error), never read out of bounds. Run under ASan in CI-style
    // builds for full value.
    {
        const std::vector<Bytes> files = {
            BuildBnd3(items, 0x2E, false, false), BuildBnd3(items, 0x54, true, true),
            BuildBnd4(items, 0x2E, false, false, 4), BuildBnd4(items, 0x3E, true, true),
        };
        std::size_t parsed = 0;
        for (const Bytes& file : files) {
            for (std::size_t len = 0; len <= file.size(); ++len) {
                const std::span<const std::byte> prefix(file.data(), len);
                parsed += ParseBnd(prefix, nullptr).has_value() ? 1 : 0;
            }
            for (std::size_t i = 0; i < file.size(); ++i) {
                for (const std::uint8_t mask : {0xFF, 0x80, 0x01}) {
                    Bytes mutated = file;
                    mutated[i] ^= static_cast<std::byte>(mask);
                    if (auto idx = ParseBnd(mutated, nullptr)) {
                        for (const BndEntry& e : idx->entries) {
                            assert(e.offset + e.stored_size <= mutated.size());
                        }
                    }
                }
            }
        }
        assert(parsed > 0);
    }

    // Through the VFS: a DCX-wrapped bundle on disk is opened by path.
    {
        const fs::path root = fs::temp_directory_path() / "ember_bnd_test";
        fs::remove_all(root);
        fs::create_directories(root / "chr");
        const Bytes wrapped = MakeDcx(BuildBnd3(items, 0x2E, false, false));
        std::ofstream(root / "chr" / "C1000.CHRBND.DCX", std::ios::binary)
            .write(reinterpret_cast<const char*>(wrapped.data()),
                   static_cast<std::streamsize>(wrapped.size()));
        std::ofstream(root / "chr" / "notabnd.bin", std::ios::binary) << "plain text file";

        Vfs vfs;
        std::string error;
        assert(vfs.MountDirectory(root, &error));
        const auto archive = vfs.OpenBnd("CHR\\c1000.chrbnd.dcx", &error);
        assert(archive && archive->Count() == 3);
        const BndEntry* e = archive->Find("c1000.hkx");
        assert(e && Str(archive->Raw(*e)) == "hk");
        assert(!vfs.OpenBnd("chr/notabnd.bin", &error) && error.find("bad magic") != std::string::npos);
        assert(!vfs.OpenBnd("chr/missing.bnd", &error));
        fs::remove_all(root);
    }

    std::puts("bnd_test: ok");
    return 0;
}
