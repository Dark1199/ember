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

// What a candidate entry table must satisfy to be accepted.
struct TableRules {
    std::uint64_t header_end = 0;  // != 0: the table and every name must lie below it
    bool data_in_file = true;      // BND3: entry data follows the table inside the same file
    bool data_size_known = true;   // is `data_size` meaningful?
    std::uint64_t data_size = 0;   // size of the space the data offsets point into
};

constexpr std::uint64_t kBdf3HeaderSize = 0x10; // BDF3 data files start with a 16-byte header

std::optional<std::vector<BndEntry>> ReadBnd3Entries(std::span<const std::byte> bytes,
                                                     const io::ByteView& view,
                                                     std::uint64_t count, const FormatBits& bits,
                                                     const TableRules& rules, std::string* why) {
    const std::uint64_t header_end = rules.header_end;
    const std::uint64_t stride = 8 + (bits.long_offsets ? 8 : 4) + (bits.ids ? 4 : 0) +
                                 (bits.names ? 4 : 0) + (bits.compression ? 4 : 0);
    if (count > (bytes.size() - kBnd3HeaderSize) / stride) {
        *why = "entry table does not fit in the file";
        return std::nullopt;
    }
    const std::uint64_t table_end = kBnd3HeaderSize + count * stride;
    if (header_end != 0 && table_end > header_end) {
        *why = "entry table runs past the declared end of headers";
        return std::nullopt;
    }
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
        if (rules.data_size_known &&
            !(e.offset <= rules.data_size && e.stored_size <= rules.data_size - e.offset)) {
            *why = "entry " + std::to_string(i) + ": data lies outside the file";
            return std::nullopt;
        }
        // Names (and, in a BND3, data) live after the entry table. This is what tells a wrong
        // offset width apart from the right one: with the wrong stride every entry after the
        // first is misaligned and its fields land inside the table.
        if ((rules.data_in_file && e.stored_size > 0 && e.offset < table_end) ||
            (!rules.data_in_file && e.stored_size > 0 && e.offset < kBdf3HeaderSize) ||
            (bits.names && name_offset < table_end)) {
            *why = "entry " + std::to_string(i) + ": points back into the entry table";
            return std::nullopt;
        }
        if (bits.names && header_end != 0 && name_offset >= header_end) {
            *why = "entry " + std::to_string(i) + ": name lies beyond the declared end of headers";
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

struct Bnd3Layout {
    std::vector<BndEntry> entries;
    FormatBits bits{};
    bool corrected = false;           // differs from what the flags say
    bool header_end_consistent = false;
    std::string why_first;            // why the flag-derived layout was rejected
};

// Searches the entry layouts described in bnd.h and returns the first that validates.
std::optional<Bnd3Layout> FindBnd3Layout(std::span<const std::byte> bytes, const io::ByteView& view,
                                         std::uint64_t count, std::uint8_t flags,
                                         TableRules rules) {
    const FormatBits declared = DecodeFormat(flags);
    // Candidate layouts: flag-derived first, then the opposite offset width, then the rest.
    std::vector<FormatBits> candidates;
    const auto add = [&](bool ids, bool comp, bool long_offsets) {
        const FormatBits c{ids, declared.names, comp, long_offsets};
        for (const FormatBits& have : candidates) {
            if (have.ids == c.ids && have.compression == c.compression &&
                have.long_offsets == c.long_offsets) {
                return;
            }
        }
        candidates.push_back(c);
    };
    add(declared.ids, declared.compression, declared.long_offsets);
    add(declared.ids, declared.compression, !declared.long_offsets);
    // The rest, widest entries first: a table whose entries really are wide also validates as a
    // narrower one when it holds a single entry, and the wider reading keeps more information
    // (ids, uncompressed size). Real tables with two or more entries only validate as themselves.
    const std::size_t fixed = candidates.size();
    for (const bool long_offsets : {false, true}) {
        for (const bool comp : {false, true}) {
            for (const bool ids : {false, true}) {
                add(ids, comp, long_offsets);
            }
        }
    }
    const auto width = [](const FormatBits& c) {
        return (c.long_offsets ? 8 : 4) + (c.ids ? 4 : 0) + (c.compression ? 4 : 0);
    };
    std::stable_sort(candidates.begin() + static_cast<std::ptrdiff_t>(fixed), candidates.end(),
                     [&](const FormatBits& a, const FormatBits& b) { return width(a) > width(b); });

    Bnd3Layout result;
    result.bits = declared;
    const std::uint64_t header_end = rules.header_end;
    // Pass 0 insists the names fit the declared end-of-headers; pass 1 drops that requirement
    // in case the field means something else in some files.
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 0 && header_end == 0) {
            continue;
        }
        TableRules pass_rules = rules;
        pass_rules.header_end = pass == 0 ? header_end : 0;
        for (const FormatBits& candidate : candidates) {
            std::string why;
            auto parsed = ReadBnd3Entries(bytes, view, count, candidate, pass_rules, &why);
            if (parsed) {
                result.entries = std::move(*parsed);
                result.bits = candidate;
                result.header_end_consistent = pass == 0;
                result.corrected = candidate.ids != declared.ids ||
                                   candidate.compression != declared.compression ||
                                   candidate.long_offsets != declared.long_offsets;
                return result;
            }
            if (result.why_first.empty()) {
                result.why_first = why;
            }
        }
    }
    return std::nullopt;
}

std::uint32_t StrideOf(const FormatBits& bits) {
    return static_cast<std::uint32_t>(8 + (bits.long_offsets ? 8 : 4) + (bits.ids ? 4 : 0) +
                                      (bits.names ? 4 : 0) + (bits.compression ? 4 : 0));
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
    std::int32_t header_end_field = 0;
    (void)view.Read(0x14, header_end_field);

    TableRules rules;
    rules.header_end = header_end_field > 0 ? static_cast<std::uint64_t>(header_end_field) : 0;
    rules.data_size = bytes.size();
    auto layout = FindBnd3Layout(bytes, view, static_cast<std::uint64_t>(count), flags, rules);
    if (!layout) {
        // Reconstruct the flag-derived failure reason for the message.
        std::string why;
        (void)ReadBnd3Entries(bytes, view, static_cast<std::uint64_t>(count), DecodeFormat(flags),
                              rules, &why);
        return Fail(error, "BND3: no entry layout fits this file (flag-derived layout: " + why +
                               "); count " + std::to_string(count) + ", declared end of headers " +
                               std::to_string(rules.header_end),
                    bytes);
    }
    index.layout_corrected = layout->corrected;
    index.long_offsets = layout->bits.long_offsets;
    index.has_ids = layout->bits.ids;
    index.has_names = layout->bits.names;
    index.has_compression_field = layout->bits.compression;
    index.header_end_consistent = layout->header_end_consistent;
    index.entry_stride = StrideOf(layout->bits);
    index.entries = std::move(layout->entries);
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
    index.has_ids = bits.ids;
    index.has_names = bits.names;
    index.has_compression_field = bits.compression;
    index.entry_stride = static_cast<std::uint32_t>(stride);

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

bool IsBhf3(std::span<const std::byte> bytes) noexcept {
    return io::ByteView(bytes).Matches(0, "BHF3", 4);
}

std::optional<BxfIndex> ParseBhf3(std::span<const std::byte> header,
                                  std::optional<std::uint64_t> data_size, std::string* error) {
    if (!IsBhf3(header)) {
        return Fail(error, "not a BHF3 header file (bad magic)", header);
    }
    if (header.size() < kBnd3HeaderSize) {
        return Fail(error, "BHF3: file shorter than the 32-byte header", header);
    }
    io::ByteView view(header);
    BxfIndex index;
    index.version = TrimmedAscii(header, 4, 8);
    std::uint8_t flags = 0;
    std::uint8_t endian = 0;
    std::int32_t count = 0;
    (void)view.Read(0x0C, flags);
    (void)view.Read(0x0D, endian);
    index.format_flags = flags;
    index.big_endian = endian != 0;
    view.SetBigEndian(index.big_endian);
    if (!view.Read(0x10, count) || count < 0) {
        return Fail(error, "BHF3: bad entry count", header);
    }
    TableRules rules;
    rules.data_in_file = false; // the entry data is in the paired BDF3 file
    // Without the paired file's size, offsets are still capped at 4 GiB: otherwise a table read
    // with an id column folded into 64-bit offsets would validate just as well.
    rules.data_size_known = true;
    rules.data_size = data_size.value_or(0xFFFFFFFFull);
    auto layout = FindBnd3Layout(header, view, static_cast<std::uint64_t>(count), flags, rules);
    if (!layout) {
        std::string why;
        (void)ReadBnd3Entries(header, view, static_cast<std::uint64_t>(count), DecodeFormat(flags),
                              rules, &why);
        return Fail(error, "BHF3: no entry layout fits this file (flag-derived layout: " + why +
                               "); count " + std::to_string(count),
                    header);
    }
    index.layout_corrected = layout->corrected;
    index.has_ids = layout->bits.ids;
    index.has_names = layout->bits.names;
    index.has_compression_field = layout->bits.compression;
    index.long_offsets = layout->bits.long_offsets;
    index.entry_stride = StrideOf(layout->bits);
    index.entries = std::move(layout->entries);
    for (const BndEntry& e : index.entries) {
        index.data_extent = std::max(index.data_extent, e.offset + e.stored_size);
    }
    return index;
}

SplitBundle::SplitBundle(std::vector<std::byte> data, BxfIndex index)
    : data_(std::move(data)), index_(std::move(index)) {
    by_name_.reserve(index_.entries.size());
    for (std::uint32_t i = 0; i < index_.entries.size(); ++i) {
        if (!index_.entries[i].name.empty()) {
            by_name_.emplace(Vfs::Normalize(index_.entries[i].name), i);
        }
    }
}

std::optional<SplitBundle> SplitBundle::Open(std::vector<std::byte> header,
                                             std::vector<std::byte> data, std::string* error) {
    for (std::vector<std::byte>* part : {&header, &data}) {
        if (IsDcx(*part)) {
            auto inflated = DecompressDcx(*part, error);
            if (!inflated) {
                return std::nullopt;
            }
            *part = std::move(*inflated);
        }
    }
    if (!io::ByteView(data).Matches(0, "BDF3", 4)) {
        return Fail(error, "split bundle data file does not start with BDF3", data);
    }
    auto index = ParseBhf3(header, data.size(), error);
    if (!index) {
        return std::nullopt;
    }
    return SplitBundle(std::move(data), std::move(*index));
}

const BndEntry* SplitBundle::Find(std::string_view name) const {
    const std::string key = Vfs::Normalize(name);
    if (key.empty()) {
        return nullptr;
    }
    if (const auto it = by_name_.find(key); it != by_name_.end()) {
        return &index_.entries[it->second];
    }
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

std::span<const std::byte> SplitBundle::Raw(const BndEntry& entry) const noexcept {
    if (entry.offset > data_.size() || entry.stored_size > data_.size() - entry.offset) {
        return {};
    }
    return {data_.data() + entry.offset, static_cast<std::size_t>(entry.stored_size)};
}

std::optional<std::vector<std::byte>> SplitBundle::Read(const BndEntry& entry,
                                                        std::string* error) const {
    return DecodeIfDcx(Raw(entry), error);
}

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
