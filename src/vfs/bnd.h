// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ember::vfs {

// ---------------------------------------------------------------------------------------------
// Bundle containers ("BND3" and "BND4"): one file that holds many named files.
//
// Layouts as described by public community documentation. They have NOT yet been verified
// against real game files by this project, so the parser is strict, derives every size from
// fields in the file, and reports the first bytes of the file when it rejects something.
//
// BND3 (32-byte header, entry table directly after it)
//   0x00 "BND3"   0x04 char[8] version   0x0C u8 format flags   0x0D u8 big-endian
//   0x10 i32 file count   0x14 i32 end of headers
//   entry (stride depends on the format flags):
//     u8 flags, 3 pad, i32 stored size, u32 (or i64) data offset,
//     [i32 id] [u32 name offset] [i32 uncompressed size]
//   Because BND3 stores no entry stride, the long-offset bit is cross-checked against the file:
//   if the flag-derived layout does not fit the file, the opposite choice is tried.
//
// BND4 (64-byte header, entry stride stored in the header)
//   0x00 "BND4"   0x09 u8 big-endian   0x0C i32 file count   0x10 i64 header size (0x40)
//   0x18 char[8] version   0x20 i64 entry stride   0x28 i64 end of headers
//   0x30 u8 unicode names   0x31 u8 format flags   0x32 u8 extended
//   entry: u8 flags, 3 pad, i32 -1, i64 stored size, [i64 uncompressed size],
//          u32 (or i64) data offset, [i32 id] [u32 name offset]
//
// Format flag bits (both versions): 0x02 ids, 0x04/0x08 names, 0x10 long offsets,
// 0x20 per-entry compression info present.
//
// Names are NUL-terminated: Shift-JIS/ASCII bytes (kept as stored) or UTF-16 (converted to
// UTF-8) when the BND4 unicode flag is set.
// ---------------------------------------------------------------------------------------------

enum class BndKind { Bnd3, Bnd4 };

struct BndEntry {
    std::int32_t id = -1;                // -1 when the bundle stores no ids
    std::uint8_t raw_flags = 0;          // per-entry flag byte, kept verbatim
    std::string name;                    // as stored, e.g. "N:\\FRPG\\data\\chr\\c1000.flver"
    std::uint64_t offset = 0;            // start of the entry's bytes inside the bundle
    std::uint64_t stored_size = 0;       // bytes present in the bundle
    std::uint64_t uncompressed_size = 0; // from the header when present, else 0
};

struct BndIndex {
    BndKind kind = BndKind::Bnd3;
    bool big_endian = false;
    bool unicode_names = false;
    bool long_offsets = false;      // the layout that actually parsed
    bool layout_corrected = false;  // BND3 only: flags disagreed with the file; see above
    std::uint8_t format_flags = 0;
    std::string version;
    std::vector<BndEntry> entries;
};

/// True when the bytes start with "BND3" or "BND4".
[[nodiscard]] bool IsBnd(std::span<const std::byte> bytes) noexcept;

/// Parses and fully validates the entry table (every offset/size lies inside `bytes`, names are
/// terminated). Never reads outside `bytes`. On failure returns nullopt with the reason and the
/// first bytes of the file in `error`.
[[nodiscard]] std::optional<BndIndex> ParseBnd(std::span<const std::byte> bytes,
                                               std::string* error);

/// An opened bundle: owns the single decoded buffer; entries are views into it (no per-entry
/// copies, no per-entry allocation besides the name strings).
class BndArchive {
public:
    /// Takes ownership of `bytes`. A DCX wrapper around the bundle is removed first.
    [[nodiscard]] static std::optional<BndArchive> Open(std::vector<std::byte> bytes,
                                                        std::string* error);

    [[nodiscard]] const BndIndex& Index() const noexcept {
        return index_;
    }
    [[nodiscard]] std::size_t Count() const noexcept {
        return index_.entries.size();
    }

    /// Case-insensitive, separator-agnostic lookup. Tries the whole stored path first, then a
    /// trailing match on whole path components (so "c1000.flver" finds "N:\\...\\c1000.flver").
    /// Returns the first match in table order, or nullptr.
    [[nodiscard]] const BndEntry* Find(std::string_view name) const;

    /// Zero-copy view of an entry's stored bytes (empty for an entry not from this archive).
    [[nodiscard]] std::span<const std::byte> Raw(const BndEntry& entry) const noexcept;

    /// Copy of the entry's content, with a DCX wrapper removed if the entry has one. This is the
    /// same decode step as Vfs::ReadAllDecoded.
    [[nodiscard]] std::optional<std::vector<std::byte>> Read(const BndEntry& entry,
                                                             std::string* error) const;

private:
    BndArchive(std::vector<std::byte> data, BndIndex index);

    std::vector<std::byte> data_;
    BndIndex index_;
    std::unordered_map<std::string, std::uint32_t> by_name_; // normalized full name -> entry
};

} // namespace ember::vfs
