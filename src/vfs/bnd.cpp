// SPDX-License-Identifier: GPL-3.0-or-later
#include "vfs/bnd.h"

#include <algorithm>
#include <cstdio>

#include "io/bytes.h"
#include "vfs/dcx.h"
#include "vfs/vfs.h"

namespace ember::vfs {
namespace {

constexpr std::size_t kBnd3HeaderSize = 0x20;
constexpr std::size_t kBnd4HeaderSize = 0x40;
constexpr std::size_t kMaxNameBytes = 1024;
constexpr std::int64_t kMaxBnd4Stride = 0x100;

struct FormatBits {
    bool ids;
    bool names;
    bool compression;
    bool long_offsets;
};

FormatBits DecodeFormat(std::uint8_t flags) noexcept {
    return {(flags & 0x02) != 0, (flags & 0x0C) != 0, (flags & 0x20) != 0, (flags & 0x10) != 0};
}

std::string HexPrefix(std::span<const std::byte> bytes, std::size_t count = 32) {
    std::string out;
    char buf[4];
    for (std::size_t i = 0; i < std::min(count, bytes.size()); ++i) {
        std::snprintf(buf, sizeof(buf), "%02x ", static_cast<unsigned>(bytes[i]));
        out += buf;
    }
    return out.empty() ? "(empty)" : out;
}

std::nullopt_t Fail(std::string* error, const std::string& why, std::span<const std::byte> bytes) {
    if (error) {
        *error = why + " (first bytes: " + HexPrefix(bytes) + ")";
    }
    return std::nullopt;
}

std::string TrimmedAscii(std::span<const std::byte> bytes, std::size_t offset, std::size_t length) {
    std::string out;
    for (std::size_t i = 0; i < length && offset + i < bytes.size(); ++i) {
        const char c = static_cast<char>(bytes[offset + i]);
        if (c == '\0') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

// NUL-terminated single-byte string (ASCII / Shift-JIS bytes are kept as stored).
bool ReadCString(std::span<const std::byte> bytes, std::uint64_t offset, std::string& out) {
    out.clear();
    if (offset >= bytes.size()) {
        return false;
    }
    for (std::uint64_t i = offset; i < bytes.size() && i - offset <= kMaxNameBytes; ++i) {
        const char c = static_cast<char>(bytes[i]);
        if (c == '\0') {
            return true;
        }
        out.push_back(c);
    }
    return false; // unterminated or implausibly long
}

void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// NUL-terminated UTF-16 string converted to UTF-8; unpaired surrogates become U+FFFD.
bool ReadUtf16String(const io::ByteView& view, std::uint64_t offset, std::string& out) {
    out.clear();
    for (std::size_t n = 0; n <= kMaxNameBytes; ++n, offset += 2) {
        std::uint16_t unit = 0;
        if (!view.Read(offset, unit)) {
            return false;
        }
        if (unit == 0) {
            return true;
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            std::uint16_t low = 0;
            if (view.Read(offset + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
                AppendUtf8(out, 0x10000 + ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) +
                                    (low - 0xDC00));
                offset += 2;
                ++n;
                continue;
            }
            AppendUtf8(out, 0xFFFD);
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            AppendUtf8(out, 0xFFFD);
        } else {
            AppendUtf8(out, unit);
        }
    }
    return false;
}

// Offsets are 32-bit unsigned or 64-bit signed depending on the layout.
bool ReadOffset(const io::ByteView& view, std::uint64_t at, bool long_offsets, std::uint64_t& out) {
    if (long_offsets) {
        std::int64_t value = 0;
        if (!view.Read(at, value) || value < 0) {
            return false;
        }
        out = static_cast<std::uint64_t>(value);
        return true;
    }
    std::uint32_t value = 0;
    if (!view.Read(at, value)) {
        return false;
    }
    out = value;
    return true;
}

bool InFile(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t size) {
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

std::optional<std::vector<BndEntry>> ReadBnd3Entries(std::span<const std::byte> bytes,
                                                     const io::ByteView& view,
                                                     std::uint64_t count, const FormatBits& bits,
                                                     std::string* why) {
    const std::uint64_t stride = 8 + (bits.long_offsets ? 8 : 4) + (bits.ids ? 4 : 0) +
                                 (bits.names ? 4 : 0) + (bits.compression ? 4 : 0);
    if (count > (bytes.size() - kBnd3HeaderSize) / stride) {
        *why = "entry table does not fit in the file";
        return std::nullopt;
    }
    const std::uint64_t table_end = kBnd3HeaderSize + count * stride;
    std::vector<BndEntry> entries;
    entries.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t at = kBnd3HeaderSize + i * stride;
        BndEntry e;
        std::int32_t stored = 0;
        if (!view.Read(at, e.raw_flags) || !view.Read(at + 4, stored) || stored < 0) {
            *why = "entry " + std::to_string(i) + ": bad flags or size";
            return std::nullopt;
        }
        e.stored_size = static_cast<std::uint64_t>(stored);
        at += 8;
        if (!ReadOffset(view, at, bits.long_offsets, e.offset)) {
            *why = "entry " + std::to_string(i) + ": bad data offset";
            return std::nullopt;
        }
        at += bits.long_offsets ? 8 : 4;
        if (bits.ids) {
            if (!view.Read(at, e.id)) {
                return std::nullopt;
            }
            at += 4;
        }
        std::uint32_t name_offset = 0;
        if (bits.names) {
            if (!view.Read(at, name_offset)) {
                return std::nullopt;
            }
            at += 4;
        }
        if (bits.compression) {
            std::int32_t u = 0;
            if (!view.Read(at, u) || u < 0) {
                *why = "entry " + std::to_string(i) + ": bad uncompressed size";
                return std::nullopt;
            }
            e.uncompressed_size = static_cast<std::uint64_t>(u);
        }
        if (!InFile(bytes, e.offset, e.stored_size)) {
            *why = "entry " + std::to_string(i) + ": data lies outside the file";
            return std::nullopt;
        }
        // Names and data live after the entry table. This is what tells a wrong offset width
        // apart from the right one: with the wrong stride every entry after the first is
        // misaligned and its fields land inside the table.
        if ((e.stored_size > 0 && e.offset < table_end) || (bits.names && name_offset < table_end)) {
            *why = "entry " + std::to_string(i) + ": points back into the entry table";
            return std::nullopt;
        }
        if (bits.names && !ReadCString(bytes, name_offset, e.name)) {
            *why = "entry " + std::to_string(i) + ": bad name";
            return std::nullopt;
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

std::optional<BndIndex> ParseBnd3(std::span<const std::byte> bytes, std::string* error) {
    if (bytes.size() < kBnd3HeaderSize) {
        return Fail(error, "BND3: file shorter than the 32-byte header", bytes);
    }
    io::ByteView view(bytes);
    BndIndex index;
    index.kind = BndKind::Bnd3;
    index.version = TrimmedAscii(bytes, 4, 8);
    std::uint8_t flags = 0;
    std::uint8_t endian = 0;
    std::int32_t count = 0;
    (void)view.Read(0x0C, flags);
    (void)view.Read(0x0D, endian);
    index.format_flags = flags;
    index.big_endian = endian != 0;
    view.SetBigEndian(index.big_endian);
    if (!view.Read(0x10, count) || count < 0) {
        return Fail(error, "BND3: bad entry count", bytes);
    }

    FormatBits bits = DecodeFormat(flags);
    std::string why_first;
    auto entries = ReadBnd3Entries(bytes, view, static_cast<std::uint64_t>(count), bits, &why_first);
    if (!entries) {
        // The stride is not stored in BND3, so a wrong long-offset assumption shows up as an
        // entry table that does not fit. Retry once with the opposite choice.
        FormatBits flipped = bits;
        flipped.long_offsets = !bits.long_offsets;
        std::string why_second;
        entries = ReadBnd3Entries(bytes, view, static_cast<std::uint64_t>(count), flipped,
                                  &why_second);
        if (!entries) {
            return Fail(error, "BND3: " + why_first + "; with the opposite offset width: " + why_second,
                        bytes);
        }
        bits = flipped;
        index.layout_corrected = true;
    }
    index.long_offsets = bits.long_offsets;
    index.entries = std::move(*entries);
    return index;
}

std::optional<BndIndex> ParseBnd4(std::span<const std::byte> bytes, std::string* error) {
    if (bytes.size() < kBnd4HeaderSize) {
        return Fail(error, "BND4: file shorter than the 64-byte header", bytes);
    }
    io::ByteView view(bytes);
    BndIndex index;
    index.kind = BndKind::Bnd4;
    index.version = TrimmedAscii(bytes, 0x18, 8);
    std::uint8_t endian = 0;
    (void)view.Read(0x09, endian);
    index.big_endian = endian != 0;
    view.SetBigEndian(index.big_endian);

    std::int32_t count = 0;
    std::int64_t header_size = 0;
    std::int64_t stride = 0;
    std::int64_t header_end = 0;
    std::uint8_t unicode = 0;
    std::uint8_t flags = 0;
    if (!view.Read(0x0C, count) || !view.Read(0x10, header_size) || !view.Read(0x20, stride) ||
        !view.Read(0x28, header_end) || !view.Read(0x30, unicode) || !view.Read(0x31, flags)) {
        return Fail(error, "BND4: truncated header", bytes);
    }
    if (count < 0 || header_size != static_cast<std::int64_t>(kBnd4HeaderSize)) {
        return Fail(error, "BND4: header fields are implausible (count or header size)", bytes);
    }
    index.unicode_names = unicode != 0;
    index.format_flags = flags;
    const FormatBits bits = DecodeFormat(flags);
    index.long_offsets = bits.long_offsets;

    const std::int64_t needed = 8 + 8 + (bits.compression ? 8 : 0) + (bits.long_offsets ? 8 : 4) +
                                (bits.ids ? 4 : 0) + (bits.names ? 4 : 0);
    if (stride < needed || stride > kMaxBnd4Stride) {
        return Fail(error, "BND4: entry stride " + std::to_string(stride) +
                               " is too small for the format flags (need " +
                               std::to_string(needed) + ") or implausibly large",
                    bytes);
    }
    if (static_cast<std::uint64_t>(count) >
        (bytes.size() - kBnd4HeaderSize) / static_cast<std::uint64_t>(stride)) {
        return Fail(error, "BND4: entry table does not fit in the file", bytes);
    }

    index.entries.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        std::uint64_t at = kBnd4HeaderSize + static_cast<std::uint64_t>(i) * stride;
        BndEntry e;
        std::int64_t stored = 0;
        if (!view.Read(at, e.raw_flags) || !view.Read(at + 8, stored) || stored < 0) {
            return Fail(error, "BND4: entry " + std::to_string(i) + ": bad flags or size", bytes);
        }
        e.stored_size = static_cast<std::uint64_t>(stored);
        at += 16;
        if (bits.compression) {
            std::int64_t u = 0;
            if (!view.Read(at, u) || u < 0) {
                return Fail(error, "BND4: entry " + std::to_string(i) + ": bad uncompressed size",
                            bytes);
            }
            e.uncompressed_size = static_cast<std::uint64_t>(u);
            at += 8;
        }
        if (!ReadOffset(view, at, bits.long_offsets, e.offset)) {
            return Fail(error, "BND4: entry " + std::to_string(i) + ": bad data offset", bytes);
        }
        at += bits.long_offsets ? 8 : 4;
        if (bits.ids) {
            (void)view.Read(at, e.id);
            at += 4;
        }
        std::uint32_t name_offset = 0;
        if (bits.names && !view.Read(at, name_offset)) {
            return Fail(error, "BND4: entry " + std::to_string(i) + ": truncated", bytes);
        }
        if (!InFile(bytes, e.offset, e.stored_size)) {
            return Fail(error, "BND4: entry " + std::to_string(i) + ": data lies outside the file",
                        bytes);
        }
        if (bits.names) {
            const bool ok = index.unicode_names ? ReadUtf16String(view, name_offset, e.name)
                                                : ReadCString(bytes, name_offset, e.name);
            if (!ok) {
                return Fail(error, "BND4: entry " + std::to_string(i) + ": bad name", bytes);
            }
        }
        index.entries.push_back(std::move(e));
    }
    return index;
}

} // namespace

bool IsBnd(std::span<const std::byte> bytes) noexcept {
    const io::ByteView view(bytes);
    return view.Matches(0, "BND3", 4) || view.Matches(0, "BND4", 4);
}

std::optional<BndIndex> ParseBnd(std::span<const std::byte> bytes, std::string* error) {
    const io::ByteView view(bytes);
    if (view.Matches(0, "BND3", 4)) {
        return ParseBnd3(bytes, error);
    }
    if (view.Matches(0, "BND4", 4)) {
        return ParseBnd4(bytes, error);
    }
    return Fail(error, "not a BND3/BND4 bundle (bad magic)", bytes);
}

BndArchive::BndArchive(std::vector<std::byte> data, BndIndex index)
    : data_(std::move(data)), index_(std::move(index)) {
    by_name_.reserve(index_.entries.size());
    for (std::uint32_t i = 0; i < index_.entries.size(); ++i) {
        if (!index_.entries[i].name.empty()) {
            by_name_.emplace(Vfs::Normalize(index_.entries[i].name), i); // first one wins
        }
    }
}

std::optional<BndArchive> BndArchive::Open(std::vector<std::byte> bytes, std::string* error) {
    if (IsDcx(bytes)) {
        auto inflated = DecompressDcx(bytes, error);
        if (!inflated) {
            return std::nullopt;
        }
        bytes = std::move(*inflated);
    }
    auto index = ParseBnd(bytes, error);
    if (!index) {
        return std::nullopt;
    }
    return BndArchive(std::move(bytes), std::move(*index));
}

const BndEntry* BndArchive::Find(std::string_view name) const {
    const std::string key = Vfs::Normalize(name);
    if (key.empty()) {
        return nullptr;
    }
    if (const auto it = by_name_.find(key); it != by_name_.end()) {
        return &index_.entries[it->second];
    }
    // Trailing match on whole path components. Rare path, so normalize on the fly.
    const std::string suffix = "/" + key;
    for (const BndEntry& entry : index_.entries) {
        if (entry.name.empty()) {
            continue;
        }
        const std::string full = Vfs::Normalize(entry.name);
        if (full.size() > suffix.size() &&
            full.compare(full.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

std::span<const std::byte> BndArchive::Raw(const BndEntry& entry) const noexcept {
    if (entry.offset > data_.size() || entry.stored_size > data_.size() - entry.offset) {
        return {};
    }
    return {data_.data() + entry.offset, static_cast<std::size_t>(entry.stored_size)};
}

std::optional<std::vector<std::byte>> BndArchive::Read(const BndEntry& entry,
                                                       std::string* error) const {
    return DecodeIfDcx(Raw(entry), error);
}

} // namespace ember::vfs
