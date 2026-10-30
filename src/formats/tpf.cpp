// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats/tpf.h"

#include <algorithm>
#include <cstdio>

#include "io/bytes.h"

namespace ember::formats {
namespace {

constexpr std::uint32_t kMaxTextures = 4096;
constexpr std::uint32_t kStrides[] = {0x14, 0x18, 0x10, 0x1C};

std::nullopt_t Fail(std::string* error, const std::string& why, std::span<const std::byte> bytes) {
    if (error) {
        std::string hex;
        char buf[4];
        for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 48); ++i) {
            std::snprintf(buf, sizeof(buf), "%02x ", static_cast<unsigned>(bytes[i]));
            hex += buf;
        }
        *error = "TPF: " + why + " (first bytes: " + hex + ")";
    }
    return std::nullopt;
}

bool ReadName(std::span<const std::byte> bytes, std::uint64_t off, bool utf16, std::string& out) {
    out.clear();
    for (int i = 0; i < 260; ++i) {
        if (utf16) {
            if (off + 2ull * i + 2 > bytes.size()) return false;
            const auto lo = static_cast<unsigned>(bytes[off + 2ull * i]);
            const auto hi = static_cast<unsigned>(bytes[off + 2ull * i + 1]);
            const unsigned c = (hi << 8) | lo;
            if (c == 0) return true;
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        } else {
            if (off + i >= bytes.size()) return false;
            const auto c = static_cast<char>(bytes[off + i]);
            if (c == 0) return true;
            out.push_back(c);
        }
    }
    return false;
}

} // namespace

bool IsTpf(std::span<const std::byte> bytes) noexcept {
    return io::ByteView(bytes).Matches(0, "TPF\0", 4);
}

std::optional<TpfIndex> ParseTpf(std::span<const std::byte> bytes, std::uint64_t file_size,
                                 std::string* error) {
    if (!IsTpf(bytes)) return Fail(error, "bad magic", bytes);
    io::ByteView v(bytes);
    std::int32_t data_size = 0, count = 0;
    TpfIndex index;
    if (!v.Read(4, data_size) || !v.Read(8, count) || !v.Read(12, index.platform) ||
        !v.Read(13, index.flag2) || !v.Read(14, index.encoding)) {
        return Fail(error, "truncated header", bytes);
    }
    // Header bytes 12..15 read as individual bytes above; platform lives at 0x0C.
    if (count < 0 || static_cast<std::uint32_t>(count) > kMaxTextures) {
        return Fail(error, "implausible texture count " + std::to_string(count), bytes);
    }
    const bool utf16 = index.encoding == 1;
    std::string last = "no stride tried";
    for (const std::uint32_t stride : kStrides) {
        TpfIndex candidate = index;
        candidate.entry_stride = stride;
        bool ok = true;
        for (std::int32_t i = 0; i < count && ok; ++i) {
            const std::uint64_t at = 16ull + 1ull * stride * i;
            TpfTexture t;
            std::int32_t offset = 0, size = 0, name_off = 0;
            if (!v.Read(at, offset) || !v.Read(at + 4, size) || !v.Read(at + 8, t.format) ||
                !v.Read(at + 9, t.type) || !v.Read(at + 10, t.mipmaps) ||
                (stride >= 0x10 && !v.Read(at + 12, name_off))) {
                last = "stride " + std::to_string(stride) + ": entry table beyond the data given";
                ok = false;
                break;
            }
            if (offset < 0 || size <= 0 || static_cast<std::uint64_t>(offset) + size > file_size ||
                !ReadName(bytes, static_cast<std::uint32_t>(name_off), utf16, t.name) || t.name.empty() ||
                !std::ranges::all_of(t.name, [](char c) { return c >= 32 && c < 127; })) {
                last = "stride " + std::to_string(stride) + ": entry " + std::to_string(i) +
                       " has implausible offset/size/name";
                ok = false;
                break;
            }
            t.offset = static_cast<std::uint32_t>(offset);
            t.size = static_cast<std::uint32_t>(size);
            if (static_cast<std::uint64_t>(offset) + 4 <= bytes.size()) {
                t.starts_with_dds = io::ByteView(bytes).Matches(static_cast<std::uint64_t>(offset), "DDS ", 4);
                if (!t.starts_with_dds) {
                    last = "stride " + std::to_string(stride) + ": entry " + std::to_string(i) +
                           " data does not start with DDS";
                    ok = false;
                    break;
                }
            }
            candidate.textures.push_back(std::move(t));
        }
        if (ok) return candidate;
    }
    return Fail(error, "no entry layout validates (" + last + ")", bytes);
}

} // namespace ember::formats
