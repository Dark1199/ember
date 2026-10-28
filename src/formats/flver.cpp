// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats/flver.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "io/bytes.h"

namespace ember::formats {
namespace {

constexpr std::uint32_t kMaxCount = 1u << 20; // far above any real model, below "garbage"

bool ReadFloat(const io::ByteView& view, std::uint64_t offset, float& out) {
    std::uint32_t bits = 0;
    if (!view.Read(offset, bits)) {
        return false;
    }
    out = std::bit_cast<float>(bits);
    return true;
}

std::nullopt_t Fail(std::string* error, const std::string& why, std::span<const std::byte> bytes) {
    if (error) {
        std::string hex;
        char buf[4];
        for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 32); ++i) {
            std::snprintf(buf, sizeof(buf), "%02x ", static_cast<unsigned>(bytes[i]));
            hex += buf;
        }
        *error = "FLVER: " + why + " (first bytes: " + hex + ")";
    }
    return std::nullopt;
}

} // namespace

bool IsFlver(std::span<const std::byte> bytes) noexcept {
    return io::ByteView(bytes).Matches(0, std::string_view("FLVER\0", 6).data(), 6);
}

std::optional<FlverHeader> ParseFlverHeader(std::span<const std::byte> bytes, std::string* error) {
    if (!IsFlver(bytes)) {
        return Fail(error, "bad magic", bytes);
    }
    if (bytes.size() < kFlverHeaderBytes) {
        return Fail(error, "shorter than the header", bytes);
    }
    io::ByteView view(bytes);
    FlverHeader h;
    const char order = static_cast<char>(bytes[6]);
    if ((order != 'L' && order != 'B') || bytes[7] != std::byte{0}) {
        return Fail(error, "unknown byte-order marker", bytes);
    }
    h.big_endian = order == 'B';
    view.SetBigEndian(h.big_endian);

    std::int32_t version = 0, data_offset = 0, data_size = 0;
    std::int32_t counts[5] = {};
    std::int32_t face_count = 0, total_face_count = 0, face_sets = 0, layouts = 0, textures = 0;
    bool ok = view.Read(0x08, version) && view.Read(0x0C, data_offset) && view.Read(0x10, data_size);
    for (int i = 0; i < 5; ++i) {
        ok = ok && view.Read(0x14 + 4 * i, counts[i]);
    }
    for (int i = 0; i < 3; ++i) {
        ok = ok && ReadFloat(view, 0x28 + 4 * i, h.bounds_min[i]) &&
             ReadFloat(view, 0x34 + 4 * i, h.bounds_max[i]);
    }
    ok = ok && view.Read(0x40, face_count) && view.Read(0x44, total_face_count) &&
         view.Read(0x48, h.vertex_index_bits) && view.Read(0x50, face_sets) &&
         view.Read(0x54, layouts) && view.Read(0x58, textures);
    if (!ok) {
        return Fail(error, "truncated header", bytes);
    }
    std::uint8_t unicode = 0;
    (void)view.Read(0x49, unicode);
    h.unicode = unicode != 0;

    const std::int32_t all[] = {version,    data_offset, data_size, counts[0], counts[1],
                                counts[2],  counts[3],   counts[4], face_count, total_face_count,
                                face_sets,  layouts,     textures};
    for (const std::int32_t v : all) {
        if (v < 0) {
            return Fail(error, "negative field", bytes);
        }
    }
    h.version = static_cast<std::uint32_t>(version);
    h.data_offset = static_cast<std::uint32_t>(data_offset);
    h.data_size = static_cast<std::uint32_t>(data_size);
    h.dummy_count = static_cast<std::uint32_t>(counts[0]);
    h.material_count = static_cast<std::uint32_t>(counts[1]);
    h.bone_count = static_cast<std::uint32_t>(counts[2]);
    h.mesh_count = static_cast<std::uint32_t>(counts[3]);
    h.vertex_buffer_count = static_cast<std::uint32_t>(counts[4]);
    h.face_count = static_cast<std::uint32_t>(face_count);
    h.total_face_count = static_cast<std::uint32_t>(total_face_count);
    h.face_set_count = static_cast<std::uint32_t>(face_sets);
    h.buffer_layout_count = static_cast<std::uint32_t>(layouts);
    h.texture_count = static_cast<std::uint32_t>(textures);

    for (const std::uint32_t c : {h.dummy_count, h.material_count, h.bone_count, h.mesh_count,
                                  h.vertex_buffer_count, h.face_set_count, h.buffer_layout_count,
                                  h.texture_count}) {
        if (c > kMaxCount) {
            return Fail(error, "implausible count", bytes);
        }
    }
    if (static_cast<std::uint64_t>(h.data_offset) + h.data_size > bytes.size() ||
        h.data_offset < kFlverHeaderBytes) {
        return Fail(error, "data range lies outside the file", bytes);
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(h.bounds_min[i]) || !std::isfinite(h.bounds_max[i]) ||
            h.bounds_min[i] > h.bounds_max[i]) {
            return Fail(error, "bounding box is not finite or is inverted", bytes);
        }
    }
    return h;
}

} // namespace ember::formats
