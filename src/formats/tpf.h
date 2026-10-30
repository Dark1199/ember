// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ember::formats {

// ---------------------------------------------------------------------------------------------
// TPF texture packs, as described by public community documentation (NOT yet confirmed on real
// files; the survey reports what it finds). Header (16 bytes, little-endian on PC):
//   0x00 "TPF\0"   0x04 i32 data size   0x08 i32 texture count
//   0x0C u8 platform   0x0D u8 flag2   0x0E u8 encoding (1 = UTF-16 names)   0x0F pad
// then one entry per texture. The entry size is not certain, so 0x14, 0x18, 0x10 and 0x1C are
// tried and the first that validates is used:
//   +0x00 i32 data offset   +0x04 i32 data size   +0x08 u8 format  +0x09 u8 type  +0x0A u8 mipmaps
//   +0x0B u8 flags   +0x0C i32 name offset   (+0x10 i32 further flags)
// A texture's data on PC is a complete DDS file (starts with "DDS ").
// ---------------------------------------------------------------------------------------------

struct TpfTexture {
    std::string name;           ///< as stored (no extension in the files seen so far, if any)
    std::uint32_t offset = 0;   ///< into the TPF
    std::uint32_t size = 0;
    std::uint8_t format = 0;
    std::uint8_t type = 0;
    std::uint8_t mipmaps = 0;
    bool starts_with_dds = false; ///< only known when the data bytes were available
};

struct TpfIndex {
    std::uint32_t entry_stride = 0;
    std::uint8_t platform = 0;
    std::uint8_t flag2 = 0;
    std::uint8_t encoding = 0;
    std::vector<TpfTexture> textures;
};

[[nodiscard]] bool IsTpf(std::span<const std::byte> bytes) noexcept;

/// Parses the header, entry table and names. `bytes` may be just a prefix of the file (header,
/// entries and names are all that is needed); data ranges are checked against `file_size`, and the
/// "DDS " magic only when those bytes are inside `bytes`. Fails with the reason and first bytes.
[[nodiscard]] std::optional<TpfIndex> ParseTpf(std::span<const std::byte> bytes,
                                               std::uint64_t file_size, std::string* error);

} // namespace ember::formats
