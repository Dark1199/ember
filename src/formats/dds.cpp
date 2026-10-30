// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats/dds.h"

#include <algorithm>
#include <cstring>

#include "io/bytes.h"

namespace ember::formats {
namespace {

constexpr std::uint32_t kMaxDimension = 8192;

std::uint32_t Fourcc(const char (&s)[5]) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[3])) << 24;
}

std::uint64_t LevelBytes(const DdsInfo& info, std::uint32_t w, std::uint32_t h) {
    switch (info.format) {
    case DdsFormat::Dxt1:
        return 8ull * ((w + 3) / 4) * ((h + 3) / 4);
    case DdsFormat::Dxt3:
    case DdsFormat::Dxt5:
        return 16ull * ((w + 3) / 4) * ((h + 3) / 4);
    case DdsFormat::Rgba32:
        return 4ull * w * h;
    default:
        return 0;
    }
}

void Rgb565(std::uint16_t c, std::uint8_t out[3]) {
    const unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
    out[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
}

// Colour half of a DXT block (8 bytes). `allow_alpha_key` enables DXT1's 3-colour + transparent mode.
void DecodeColorBlock(const std::uint8_t* b, bool allow_alpha_key, std::uint8_t out[16][4]) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(b[0] | b[1] << 8);
    const std::uint16_t c1 = static_cast<std::uint16_t>(b[2] | b[3] << 8);
    std::uint8_t pal[4][4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    if (c0 > c1 || !allow_alpha_key) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<std::uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<std::uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        }
    } else {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<std::uint8_t>((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
        pal[3][3] = 0;
    }
    std::uint32_t bits = static_cast<std::uint32_t>(b[4]) | static_cast<std::uint32_t>(b[5]) << 8 |
                         static_cast<std::uint32_t>(b[6]) << 16 | static_cast<std::uint32_t>(b[7]) << 24;
    for (int i = 0; i < 16; ++i, bits >>= 2) {
        std::memcpy(out[i], pal[bits & 3], 4);
    }
}

void DecodeAlphaBlock(const std::uint8_t* b, std::uint8_t alpha[16]) {
    std::uint8_t table[8];
    table[0] = b[0];
    table[1] = b[1];
    if (table[0] > table[1]) {
        for (int i = 1; i <= 6; ++i) {
            table[1 + i] = static_cast<std::uint8_t>(((7 - i) * table[0] + i * table[1]) / 7);
        }
    } else {
        for (int i = 1; i <= 4; ++i) {
            table[1 + i] = static_cast<std::uint8_t>(((5 - i) * table[0] + i * table[1]) / 5);
        }
        table[6] = 0;
        table[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i, bits >>= 3) alpha[i] = table[bits & 7];
}

void DecodeLevel(const DdsInfo& info, const std::uint8_t* src, std::uint32_t w, std::uint32_t h,
                 std::vector<std::uint8_t>& out) {
    out.assign(4ull * w * h, 0);
    if (info.format == DdsFormat::Rgba32) {
        // Masks select which bits form each channel; handle the usual 8-bit-per-channel layouts.
        int shift[4];
        for (int c = 0; c < 4; ++c) {
            shift[c] = info.masks[c] == 0 ? -1 : std::countr_zero(info.masks[c]);
        }
        for (std::uint64_t i = 0; i < 1ull * w * h; ++i) {
            std::uint32_t px;
            std::memcpy(&px, src + 4 * i, 4);
            for (int c = 0; c < 4; ++c) {
                out[4 * i + c] = shift[c] < 0 ? 255 : static_cast<std::uint8_t>((px >> shift[c]) & 0xFF);
            }
        }
        return;
    }
    const std::uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const std::size_t block_bytes = info.format == DdsFormat::Dxt1 ? 8 : 16;
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            const std::uint8_t* block = src + block_bytes * (1ull * by * bw + bx);
            std::uint8_t px[16][4];
            std::uint8_t alpha[16];
            if (info.format == DdsFormat::Dxt1) {
                DecodeColorBlock(block, true, px);
            } else {
                DecodeColorBlock(block + 8, false, px);
                if (info.format == DdsFormat::Dxt3) {
                    for (int i = 0; i < 16; ++i) {
                        const unsigned nib = (block[i / 2] >> (4 * (i & 1))) & 15;
                        alpha[i] = static_cast<std::uint8_t>(nib * 17);
                    }
                } else {
                    DecodeAlphaBlock(block, alpha);
                }
                for (int i = 0; i < 16; ++i) px[i][3] = alpha[i];
            }
            for (int i = 0; i < 16; ++i) {
                const std::uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) std::memcpy(&out[4ull * (1ull * y * w + x)], px[i], 4);
            }
        }
    }
}

} // namespace

bool IsDds(std::span<const std::byte> bytes) noexcept {
    return io::ByteView(bytes).Matches(0, "DDS ", 4);
}

std::optional<DdsInfo> ParseDds(std::span<const std::byte> bytes, std::string* error,
                                bool check_pixels) {
    const auto fail = [&](const std::string& why) -> std::optional<DdsInfo> {
        if (error) *error = "DDS: " + why;
        return std::nullopt;
    };
    if (!IsDds(bytes)) return fail("bad magic");
    io::ByteView v(bytes);
    std::uint32_t size = 0, height = 0, width = 0, mips = 0, pf_size = 0, pf_flags = 0, fourcc = 0,
                  bit_count = 0;
    DdsInfo info;
    if (!v.Read(4, size) || !v.Read(12, height) || !v.Read(16, width) || !v.Read(28, mips) ||
        !v.Read(76, pf_size) || !v.Read(80, pf_flags) || !v.Read(84, fourcc) || !v.Read(88, bit_count) ||
        !v.Read(92, info.masks[0]) || !v.Read(96, info.masks[1]) || !v.Read(100, info.masks[2]) ||
        !v.Read(104, info.masks[3])) {
        return fail("truncated header");
    }
    if (size != 124 || pf_size != 32) return fail("unexpected header sizes");
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        return fail("implausible dimensions " + std::to_string(width) + "x" + std::to_string(height));
    }
    info.width = width;
    info.height = height;
    info.mip_count = std::max<std::uint32_t>(mips, 1);
    info.data_offset = 128;
    if (pf_flags & 0x4) { // FOURCC
        char text[5] = {static_cast<char>(fourcc & 0xFF), static_cast<char>((fourcc >> 8) & 0xFF),
                        static_cast<char>((fourcc >> 16) & 0xFF), static_cast<char>((fourcc >> 24) & 0xFF), 0};
        for (char& c : text) if (c != 0 && (c < 32 || c > 126)) c = '?';
        info.format_name = text;
        if (fourcc == Fourcc("DXT1")) info.format = DdsFormat::Dxt1;
        else if (fourcc == Fourcc("DXT3")) info.format = DdsFormat::Dxt3;
        else if (fourcc == Fourcc("DXT5")) info.format = DdsFormat::Dxt5;
    } else if ((pf_flags & 0x40) && bit_count == 32) { // RGB, 32 bit
        info.format = DdsFormat::Rgba32;
        info.format_name = "rgb32";
        if (!(pf_flags & 0x1)) info.masks[3] = 0; // no alpha channel
    } else {
        info.format_name = "flags" + std::to_string(pf_flags) + "/bits" + std::to_string(bit_count);
    }
    if (info.format == DdsFormat::Unsupported || !check_pixels) return info;
    std::uint64_t total = 0;
    std::uint32_t w = width, h = height;
    for (std::uint32_t i = 0; i < info.mip_count; ++i) {
        total += LevelBytes(info, w, h);
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    if (info.data_offset + total > bytes.size()) {
        // Some files declare more levels than they store: trim to what is present.
        std::uint32_t fit = 0;
        std::uint64_t used = info.data_offset;
        w = width;
        h = height;
        while (fit < info.mip_count && used + LevelBytes(info, w, h) <= bytes.size()) {
            used += LevelBytes(info, w, h);
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
            ++fit;
        }
        if (fit == 0) return fail("pixel data runs past the end of the file");
        info.mip_count = fit;
    }
    return info;
}

std::optional<std::vector<RgbaImage>> DecodeDds(std::span<const std::byte> bytes, const DdsInfo& info,
                                                std::uint32_t max_levels, std::string* error,
                                                std::uint32_t first_level) {
    if (info.format == DdsFormat::Unsupported) {
        if (error) *error = "DDS: unsupported pixel format " + info.format_name;
        return std::nullopt;
    }
    std::vector<RgbaImage> levels;
    std::uint64_t offset = info.data_offset;
    std::uint32_t w = info.width, h = info.height;
    for (std::uint32_t i = 0; i < info.mip_count && levels.size() < max_levels; ++i) {
        const std::uint64_t n = LevelBytes(info, w, h);
        if (offset + n > bytes.size()) break;
        if (i < first_level) {
            offset += n;
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
            continue;
        }
        RgbaImage image;
        image.width = w;
        image.height = h;
        DecodeLevel(info, reinterpret_cast<const std::uint8_t*>(bytes.data()) + offset, w, h, image.rgba);
        levels.push_back(std::move(image));
        offset += n;
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    if (levels.empty()) {
        if (error) *error = "DDS: no complete mip level";
        return std::nullopt;
    }
    return levels;
}

} // namespace ember::formats
