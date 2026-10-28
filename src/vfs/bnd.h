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
//   BND3 stores no entry stride, and real files show that the flag bits above do not reliably
//   describe the table (every Dark Souls bundle checked so far carries 0x74, which contradicts
//   them). The parser therefore tries the flag-derived layout first, then every combination of
//   {long offsets, ids, compression field} with the names field kept as the flags say, and takes
//   the first one whose whole table validates: every data range inside the file and after the
//   table, every name inside the file and after the table, and (preferred) inside the declared
//   end-of-headers. `layout_corrected` is set when the choice differs from the flag-derived one.
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
    bool has_ids = false;           // BND3: the entry layout that actually parsed
    bool has_names = false;
    bool has_compression_field = false;
    std::uint32_t entry_stride = 0; // bytes per entry in the table (BND3: derived, BND4: stored)
    bool header_end_consistent = false; // BND3: names lie inside the declared end-of-headers
    std::uint8_t format_flags = 0;
    std::string version;
    std::vector<BndEntry> entries;
};

// ---------------------------------------------------------------------------------------------
// Split bundles ("BHF3" header file + "BDF3" data file). A BHF3 is a BND3 header without the data:
// the same 32-byte header (no end-of-headers field) and the same entry table, but every entry's
// offset and size refer to the paired BDF3 file, which starts with a 16-byte header ("BDF3" and a
// version string) and holds the raw entry bytes. The entry-table layout is inferred exactly as
// for BND3 (see above); the names stay in the header file.
// ---------------------------------------------------------------------------------------------
struct BxfIndex {
    bool big_endian = false;
    bool long_offsets = false;
    bool layout_corrected = false;
    bool has_ids = false;
    bool has_names = false;
    bool has_compression_field = false;
    std::uint8_t format_flags = 0;
    std::uint32_t entry_stride = 0;
    std::string version;
    std::uint64_t data_extent = 0; ///< end of the furthest entry; the BDF3 must be at least this long
    std::vector<BndEntry> entries; ///< offsets/sizes address the BDF3 file
};

[[nodiscard]] bool IsBhf3(std::span<const std::byte> bytes) noexcept;

/// Parses a BHF3 header file. `data_size` is the size of the paired BDF3 when known; every entry
/// must then lie inside it. Without it only the header itself can be validated (offsets are
/// capped at 4 GiB).
[[nodiscard]] std::optional<BxfIndex> ParseBhf3(std::span<const std::byte> header,
                                                std::optional<std::uint64_t> data_size,
                                                std::string* error);

/// An opened split bundle: header index plus the paired BDF3 data, both held in memory. Entries
/// are views into the data buffer; names are looked up exactly as in BndArchive.
class SplitBundle {
public:
    /// Takes ownership of both buffers (a DCX wrapper around either is removed first). Fails if
    /// the data does not start with "BDF3" or is shorter than the header says it must be.
    [[nodiscard]] static std::optional<SplitBundle> Open(std::vector<std::byte> header,
                                                         std::vector<std::byte> data,
                                                         std::string* error);

    [[nodiscard]] const BxfIndex& Index() const noexcept {
        return index_;
    }
    [[nodiscard]] std::size_t Count() const noexcept {
        return index_.entries.size();
    }
    [[nodiscard]] const BndEntry* Find(std::string_view name) const;
    [[nodiscard]] std::span<const std::byte> Raw(const BndEntry& entry) const noexcept;
    [[nodiscard]] std::optional<std::vector<std::byte>> Read(const BndEntry& entry,
                                                             std::string* error) const;

private:
    SplitBundle(std::vector<std::byte> data, BxfIndex index);

    std::vector<std::byte> data_;
    BxfIndex index_;
    std::unordered_map<std::string, std::uint32_t> by_name_;
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
