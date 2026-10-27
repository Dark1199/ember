// SPDX-License-Identifier: GPL-3.0-or-later
#include <zlib.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vfs/dcx.h"
#include "vfs/vfs.h"

namespace fs = std::filesystem;
using namespace ember::vfs;

namespace {

void PutBE(std::vector<std::byte>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::byte>(v >> shift));
}
void PutAscii(std::vector<std::byte>& out, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<std::byte>(s[i]));
}

std::vector<std::byte> Deflate(const std::string& text) {
    uLongf size = compressBound(static_cast<uLong>(text.size()));
    std::vector<std::byte> out(size);
    const int rc = compress2(reinterpret_cast<Bytef*>(out.data()), &size,
                             reinterpret_cast<const Bytef*>(text.data()),
                             static_cast<uLong>(text.size()), 9);
    assert(rc == Z_OK);
    out.resize(size);
    return out;
}

// A DCX file built from the documented structure, with the method name configurable.
std::vector<std::byte> MakeDcx(const std::string& text, const char* method = "DFLT",
                               std::uint32_t declared_uncompressed = 0xFFFFFFFF) {
    const std::vector<std::byte> packed = Deflate(text);
    std::vector<std::byte> out;
    PutAscii(out, "DCX\0", 4);
    PutBE(out, 0x10000);
    PutBE(out, 0x18); // DCS offset
    PutBE(out, 0x24); // DCP offset
    PutBE(out, 0x24);
    PutBE(out, 0x2C);
    PutAscii(out, "DCS\0", 4);
    PutBE(out, declared_uncompressed == 0xFFFFFFFF ? static_cast<std::uint32_t>(text.size())
                                                    : declared_uncompressed);
    PutBE(out, static_cast<std::uint32_t>(packed.size()));
    PutAscii(out, "DCP\0", 4);
    PutAscii(out, method, 4);
    PutBE(out, 0x20); // DCP block size
    for (int i = 0; i < 5; ++i) PutBE(out, 0x09000000); // method parameters (ignored)
    PutAscii(out, "DCA\0", 4);
    PutBE(out, 8); // DCA block size
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
}

std::string Str(const std::vector<std::byte>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

} // namespace

int main() {
    std::string text;
    for (int i = 0; i < 500; ++i) text += "ember payload line " + std::to_string(i) + "\n";

    // Round trip.
    {
        const auto dcx = MakeDcx(text);
        assert(IsDcx(dcx));
        std::string error;
        const auto out = DecompressDcx(dcx, &error);
        assert(out && Str(*out) == text && error.empty());
    }
    // Plain zlib helper.
    {
        const auto packed = Deflate("abc");
        std::string error;
        assert(Str(*InflateZlib(packed, 3, &error)) == "abc");
        assert(!InflateZlib(packed, 2, &error)); // declared size too small
        assert(!InflateZlib(packed, 4, &error)); // declared size too large
    }
    // Failures: unsupported method, size mismatch, size bomb, truncation, bad magic.
    {
        std::string error;
        assert(!DecompressDcx(MakeDcx(text, "KRAK"), &error));
        assert(error.find("KRAK") != std::string::npos);

        assert(!DecompressDcx(MakeDcx(text, "DFLT", static_cast<std::uint32_t>(text.size()) + 5), &error));

        assert(!DecompressDcx(MakeDcx("x", "DFLT", 0xFFFFFFF0u), &error));
        assert(error.find("safety limit") != std::string::npos);

        auto truncated = MakeDcx(text);
        truncated.resize(truncated.size() - 20);
        assert(!DecompressDcx(truncated, &error));

        std::vector<std::byte> junk(100, std::byte{0x7});
        assert(!IsDcx(junk) && !DecompressDcx(junk, &error));

        auto corrupt = MakeDcx(text);
        corrupt[corrupt.size() - 10] ^= std::byte{0xFF};
        assert(!DecompressDcx(corrupt, &error));
    }
    // Through the VFS: wrapped files decode, plain files pass through, missing files fail.
    {
        const fs::path root = fs::temp_directory_path() / "ember_dcx_test";
        fs::remove_all(root);
        fs::create_directories(root / "data");
        const auto dcx = MakeDcx(text);
        std::ofstream(root / "data" / "File.Bin.DCX", std::ios::binary)
            .write(reinterpret_cast<const char*>(dcx.data()), static_cast<std::streamsize>(dcx.size()));
        std::ofstream(root / "data" / "plain.txt", std::ios::binary) << "plain";

        Vfs vfs;
        std::string error;
        assert(vfs.MountDirectory(root, &error));
        const auto decoded = vfs.ReadAllDecoded("DATA\\file.bin.dcx", &error);
        assert(decoded && Str(*decoded) == text);
        assert(Str(*vfs.ReadAllDecoded("data/plain.txt")) == "plain");
        assert(!vfs.ReadAllDecoded("data/nope.bin", &error));
        fs::remove_all(root);
    }
    std::puts("dcx_test: ok");
    return 0;
}
