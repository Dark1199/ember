// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ember::vfs {

// ---------------------------------------------------------------------------------------------
// Compressed wrapper ("DCX"): a short big-endian header in front of one compressed stream.
//
// Structure as described by public community documentation (not yet verified against real files
// by this project, so every offset is derived from fields in the file rather than hard-coded):
//
//   0x00  "DCX\0"
//   0x04  u32  version word
//   0x08  u32  offset of the "DCS" block
//   0x0C  u32  offset of the "DCP" block
//   0x10  u32  (unused here)   0x14  u32  (unused here)
//   [dcs] "DCS\0"  u32 uncompressed_size  u32 compressed_size
//   [dcp] "DCP\0"  char[4] method  u32 dcp_block_size  ... (method parameters, ignored)
//   [dca] "DCA\0"  u32 dca_block_size      (located at dcp + dcp_block_size)
//   [data] starts at dca + dca_block_size
//
// Only the "DFLT" method (a standard zlib stream) is supported. Other methods are reported by
// name so they can be added deliberately.
// ---------------------------------------------------------------------------------------------

/// True when the bytes start with the "DCX\0" magic.
[[nodiscard]] bool IsDcx(std::span<const std::byte> bytes) noexcept;

/// Hard ceiling on a declared decompressed size, so a corrupt or hostile header cannot make the
/// engine allocate gigabytes.
inline constexpr std::uint64_t kMaxDecompressedSize = 1ull << 30; // 1 GiB

/// Inflates a zlib stream whose exact decompressed size is known. Fails (nullopt + error) on
/// corrupt data, trailing/missing output, or a size above kMaxDecompressedSize.
[[nodiscard]] std::optional<std::vector<std::byte>> InflateZlib(std::span<const std::byte> in,
                                                                std::uint64_t expected_size,
                                                                std::string* error);

/// Returns the bytes unchanged (copied) when they are not DCX, otherwise the decompressed bytes.
/// This is the single decode step shared by Vfs::ReadAllDecoded and container entry reads.
[[nodiscard]] std::optional<std::vector<std::byte>> DecodeIfDcx(std::span<const std::byte> bytes,
                                                                std::string* error);

/// Decompresses a DCX file held in memory.
[[nodiscard]] std::optional<std::vector<std::byte>> DecompressDcx(std::span<const std::byte> bytes,
                                                                  std::string* error);

} // namespace ember::vfs
