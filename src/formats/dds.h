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
// DDS texture containers (the public Microsoft format): "DDS " + 124-byte header, then the mip
// chain from the largest level down. Supported pixel formats: DXT1, DXT3, DXT5 (block compressed,
// decoded on the CPU to RGBA8) and uncompressed 32-bit RGBA/BGRA masks. Anything else is
// reported by name and skipped, never guessed.
// ---------------------------------------------------------------------------------------------

enum class DdsFormat { Dxt1, Dxt3, Dxt5, Rgba32, Unsupported };

struct DdsInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_count = 1;
    DdsFormat format = DdsFormat::Unsupported;
    std::string format_name;      ///< fourCC text or "rgb<bits>" for diagnostics
    std::uint32_t data_offset = 0;
    // Rgba32 only: channel masks.
    std::uint32_t masks[4] = {};  ///< r, g, b, a
};

[[nodiscard]] bool IsDds(std::span<const std::byte> bytes) noexcept;

/// Parses the header and (unless `check_pixels` is false, for peeking at just the first 128
/// bytes) checks that the mip chain fits in `bytes`, trimming levels that are not stored.
[[nodiscard]] std::optional<DdsInfo> ParseDds(std::span<const std::byte> bytes, std::string* error,
                                              bool check_pixels = true);

/// One decoded mip level.
struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; ///< width * height * 4
};

/// Decodes levels `first_level` onwards (at most `max_levels` of them) to RGBA8; skipped levels
/// are not decoded, which is how large textures are brought down to a smaller size. Levels are at
/// least 1x1.
[[nodiscard]] std::optional<std::vector<RgbaImage>> DecodeDds(std::span<const std::byte> bytes,
                                                              const DdsInfo& info,
                                                              std::uint32_t max_levels,
                                                              std::string* error,
                                                              std::uint32_t first_level = 0);

} // namespace ember::formats
