// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ember::vfs {

// ---------------------------------------------------------------------------------------------
// Split archives: a small index file (.bhd5, sometimes written .bhd) paired with a large data
// file (.bdt) that holds the packed file contents.
//
// Layout implemented here (little-endian unless the endian marker says otherwise), as described
// by public community documentation of the format. It has NOT yet been checked against real
// files by this project: parsing is therefore strict and every failure reports the raw header
// bytes so a wrong assumption is easy to find and correct.
//
//   index file
//     0x00  char[4]  "BHD5"
//     0x04  u8       endian marker: 0xFF little-endian, 0x00 big-endian
//     0x05  u8[3]    zero
//     0x08  i32      expected to be 1
//     0x0C  i32      size of the index file
//     0x10  i32      bucket count
//     0x14  i32      offset of the bucket table
//   bucket table: bucket count x { i32 entry_count; i32 entries_offset }
//   entry (16 bytes): { u32 name_hash; i32 stored_size; i64 data_offset }
//
//   data file
//     0x00  char[4]  "BDF3", then a short version string; entry offsets are absolute file offsets
//
// Entries carry only a 32-bit hash of the path, never the path itself: to read a file the engine
// hashes the path it wants and looks the hash up. The archive therefore cannot be listed by name.
// `stored_size` is the stored (padded) length, which can exceed the real file length; formats
// that know their own length (the common container formats) ignore the padding.
// ---------------------------------------------------------------------------------------------

struct Bhd5Entry {
    std::uint32_t name_hash = 0;
    std::uint32_t stored_size = 0;
    std::uint64_t data_offset = 0;
};

struct Bhd5Index {
    bool big_endian = false;
    std::uint32_t declared_file_size = 0;
    std::size_t invalid_entries = 0; ///< entries skipped because they fall outside the data file
    std::vector<Bhd5Entry> entries;  ///< sorted by name_hash
};

/// 32-bit path hash used by the index: path is trimmed, lowercased, '\\' becomes '/', a leading
/// '/' is ensured, then hash = hash * 37 + character over every character.
[[nodiscard]] std::uint32_t PathHash(std::string_view path) noexcept;

/// Parses an index held in memory. `data_file_size` is used to reject entries that point outside
/// the data file. On failure returns nullopt and fills `error` (including a hex dump of the first
/// bytes).
[[nodiscard]] std::optional<Bhd5Index> ParseBhd5(std::span<const std::byte> index_bytes,
                                                 std::uint64_t data_file_size,
                                                 std::string* error);

/// An opened index/data pair. Data is read from disk on demand, never cached.
class Bhd5Archive {
public:
    [[nodiscard]] static std::optional<Bhd5Archive> Open(const std::filesystem::path& index_path,
                                                         const std::filesystem::path& data_path,
                                                         std::string* error);

    [[nodiscard]] const Bhd5Entry* Find(std::uint32_t name_hash) const noexcept;
    [[nodiscard]] std::optional<std::vector<std::byte>> Read(const Bhd5Entry& entry) const;

    [[nodiscard]] const Bhd5Index& Index() const noexcept {
        return index_;
    }
    [[nodiscard]] const std::filesystem::path& DataPath() const noexcept {
        return data_path_;
    }

private:
    Bhd5Index index_;
    std::filesystem::path data_path_;
};

} // namespace ember::vfs
