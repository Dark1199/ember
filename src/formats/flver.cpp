// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats/flver.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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
        if (!std::isfinite(h.bounds_min[i]) || !std::isfinite(h.bounds_max[i])) {
            return Fail(error, "bounding box is not finite", bytes);
        }
        // Skeleton-only files (no meshes) carry the "empty bounds" sentinel: min = +FLT_MAX and
        // max = -FLT_MAX, which is inverted by construction. With meshes it is an error.
        if (h.bounds_min[i] > h.bounds_max[i] && h.mesh_count != 0) {
            return Fail(error, "bounding box is inverted", bytes);
        }
    }
    return h;
}

// ------------------------------------------------------------------------------------------
// Body tables and geometry.

namespace {

constexpr std::uint32_t kDummyBytes = 0x40, kMaterialBytes = 0x20, kBoneBytes = 0x80,
                        kMeshBytes = 0x30, kVertexBufferBytes = 0x20, kLayoutBytes = 0x10,
                        kMemberBytes = 0x14, kTextureBytes = 0x20;
constexpr std::uint32_t kFaceSetStrides[] = {0x20, 0x1C, 0x18};

// Reads a zero-terminated string at an absolute offset: UTF-16LE/BE (non-ASCII characters become
// '?') when `unicode`, else single bytes. Fails when unterminated within 512 characters.
bool ReadString(std::span<const std::byte> bytes, bool big_endian, bool unicode, std::uint64_t off,
                std::string& out) {
    out.clear();
    for (int i = 0; i < 512; ++i) {
        if (unicode) {
            if (off + 2ull * i + 2 > bytes.size()) return false;
            const auto lo = static_cast<unsigned>(bytes[off + 2ull * i]);
            const auto hi = static_cast<unsigned>(bytes[off + 2ull * i + 1]);
            const unsigned c = big_endian ? (lo << 8) | hi : (hi << 8) | lo;
            if (c == 0) return true;
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        } else {
            if (off + i >= bytes.size()) return false;
            const auto c = static_cast<char>(bytes[off + i]);
            if (c == 0) return true;
            out.push_back(c);
        }
    }
    return false;
}

bool ReadU32(const io::ByteView& v, std::uint64_t off, std::uint32_t& out) {
    return v.Read(off, out);
}

std::nullopt_t BodyFail(std::string* error, const std::string& why) {
    if (error) *error = "FLVER body: " + why;
    return std::nullopt;
}

// Reads `count` u32 values at an absolute file offset.
bool ReadIndexList(const io::ByteView& v, std::uint64_t off, std::uint32_t count,
                   std::vector<std::uint32_t>& out) {
    out.clear();
    if (count > kMaxCount) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t value = 0;
        if (!v.Read(off + 4ull * i, value)) return false;
        out.push_back(value);
    }
    return true;
}

} // namespace

std::uint64_t FlverTablesEnd(const FlverHeader& h, std::uint32_t face_set_stride) {
    return 0x80ull + 1ull * kDummyBytes * h.dummy_count + 1ull * kMaterialBytes * h.material_count +
           1ull * kBoneBytes * h.bone_count + 1ull * kMeshBytes * h.mesh_count +
           1ull * face_set_stride * h.face_set_count +
           1ull * kVertexBufferBytes * h.vertex_buffer_count +
           1ull * kLayoutBytes * h.buffer_layout_count + 1ull * kTextureBytes * h.texture_count;
}

std::optional<FlverBody> ParseFlverBody(std::span<const std::byte> bytes, const FlverHeader& h,
                                        std::string* error) {
    io::ByteView v(bytes, h.big_endian);
    FlverBody body;
    const std::uint64_t data_begin = h.data_offset;
    const std::uint64_t data_end = data_begin + h.data_size;

    const std::uint64_t meshes_at = 0x80ull + 1ull * kDummyBytes * h.dummy_count +
                                    1ull * kMaterialBytes * h.material_count +
                                    1ull * kBoneBytes * h.bone_count;
    const std::uint64_t face_sets_at = meshes_at + 1ull * kMeshBytes * h.mesh_count;

    // Meshes: fixed size, so they can be read before the face-set stride is known.
    for (std::uint32_t i = 0; i < h.mesh_count; ++i) {
        const std::uint64_t at = meshes_at + 1ull * kMeshBytes * i;
        FlverMesh m;
        std::uint8_t dynamic = 0;
        std::uint32_t fs_count = 0, fs_off = 0, vb_count = 0, vb_off = 0;
        if (!v.Read(at, dynamic) || !ReadU32(v, at + 4, m.material_index) ||
            !ReadU32(v, at + 0x10, m.default_bone) || !ReadU32(v, at + 0x20, fs_count) ||
            !ReadU32(v, at + 0x24, fs_off) || !ReadU32(v, at + 0x28, vb_count) ||
            !ReadU32(v, at + 0x2C, vb_off)) {
            return BodyFail(error, "mesh table runs past the file");
        }
        m.dynamic = dynamic != 0;
        if (m.material_index >= h.material_count && h.material_count != 0) {
            return BodyFail(error, "mesh " + std::to_string(i) + " names material " +
                                       std::to_string(m.material_index) + " of " +
                                       std::to_string(h.material_count));
        }
        if (!ReadIndexList(v, fs_off, fs_count, m.face_sets) ||
            !ReadIndexList(v, vb_off, vb_count, m.vertex_buffers)) {
            return BodyFail(error, "mesh " + std::to_string(i) + " index lists lie outside the file");
        }
        for (const std::uint32_t f : m.face_sets) {
            if (f >= h.face_set_count) return BodyFail(error, "face set index out of range");
        }
        for (const std::uint32_t b : m.vertex_buffers) {
            if (b >= h.vertex_buffer_count) return BodyFail(error, "vertex buffer index out of range");
        }
        body.meshes.push_back(std::move(m));
    }

    // The face-set entry size is not certain, so each candidate is tried and the first one whose
    // tables validate against the rest of the file is used (and reported).
    std::string last_problem = "no candidate stride tried";
    for (const std::uint32_t stride : kFaceSetStrides) {
        std::vector<FlverFaceSet> face_sets;
        std::vector<FlverVertexBuffer> buffers;
        std::vector<std::vector<FlverMember>> layouts;
        bool ok = true;
        const auto fail = [&](const std::string& why) {
            last_problem = "stride " + std::to_string(stride) + ": " + why;
            ok = false;
        };
        for (std::uint32_t i = 0; ok && i < h.face_set_count; ++i) {
            const std::uint64_t at = face_sets_at + 1ull * stride * i;
            FlverFaceSet f;
            std::uint8_t strip = 0, cull = 0;
            if (!ReadU32(v, at, f.flags) || !v.Read(at + 4, strip) || !v.Read(at + 5, cull) ||
                !ReadU32(v, at + 8, f.index_count) || !ReadU32(v, at + 12, f.index_offset)) {
                fail("face set table runs past the file");
                break;
            }
            f.triangle_strip = strip == 1;
            f.cull_backfaces = cull == 1;
            const std::uint64_t width = h.vertex_index_bits == 32 ? 4 : 2;
            if (strip > 1 || cull > 1 || f.index_count > kMaxCount * 8ull ||
                data_begin + f.index_offset + width * f.index_count > data_end) {
                fail("face set " + std::to_string(i) + " implausible");
                break;
            }
            face_sets.push_back(f);
        }
        const std::uint64_t buffers_at = face_sets_at + 1ull * stride * h.face_set_count;
        for (std::uint32_t i = 0; ok && i < h.vertex_buffer_count; ++i) {
            const std::uint64_t at = buffers_at + 1ull * kVertexBufferBytes * i;
            FlverVertexBuffer b;
            if (!ReadU32(v, at, b.buffer_index) || !ReadU32(v, at + 4, b.layout_index) ||
                !ReadU32(v, at + 8, b.vertex_size) || !ReadU32(v, at + 12, b.vertex_count) ||
                !ReadU32(v, at + 0x18, b.buffer_length) || !ReadU32(v, at + 0x1C, b.buffer_offset)) {
                fail("vertex buffer table runs past the file");
                break;
            }
            if (b.layout_index >= h.buffer_layout_count || b.vertex_size < 4 || b.vertex_size > 256 ||
                b.vertex_count > (1u << 24) ||
                1ull * b.vertex_size * b.vertex_count > b.buffer_length ||
                data_begin + b.buffer_offset + b.buffer_length > data_end) {
                fail("vertex buffer " + std::to_string(i) + " implausible");
                break;
            }
            buffers.push_back(b);
        }
        const std::uint64_t layouts_at = buffers_at + 1ull * kVertexBufferBytes * h.vertex_buffer_count;
        for (std::uint32_t i = 0; ok && i < h.buffer_layout_count; ++i) {
            const std::uint64_t at = layouts_at + 1ull * kLayoutBytes * i;
            std::uint32_t member_count = 0, member_off = 0;
            if (!ReadU32(v, at, member_count) || !ReadU32(v, at + 12, member_off) ||
                member_count > 64) {
                fail("buffer layout " + std::to_string(i) + " implausible");
                break;
            }
            std::vector<FlverMember> members;
            for (std::uint32_t k = 0; k < member_count; ++k) {
                const std::uint64_t m_at = member_off + 1ull * kMemberBytes * k;
                FlverMember m;
                if (!ReadU32(v, m_at + 4, m.struct_offset) || !ReadU32(v, m_at + 8, m.type) ||
                    !ReadU32(v, m_at + 12, m.semantic) || !ReadU32(v, m_at + 16, m.index)) {
                    fail("layout member lies outside the file");
                    break;
                }
                members.push_back(m);
            }
            if (!ok) break;
            layouts.push_back(std::move(members));
        }
        std::vector<FlverMaterial> materials;
        std::vector<FlverTexture> textures;
        bool strings_ok = true;
        if (ok) {
            const std::uint64_t materials_at = 0x80ull + 1ull * kDummyBytes * h.dummy_count;
            for (std::uint32_t i = 0; i < h.material_count; ++i) {
                const std::uint64_t at = materials_at + 1ull * kMaterialBytes * i;
                FlverMaterial m;
                std::uint32_t name_off = 0, mtd_off = 0;
                if (!ReadU32(v, at, name_off) || !ReadU32(v, at + 4, mtd_off) ||
                    !ReadU32(v, at + 8, m.texture_count) || !ReadU32(v, at + 12, m.texture_index)) {
                    fail("material table runs past the file");
                    break;
                }
                if (m.texture_index > h.texture_count || m.texture_count > h.texture_count - m.texture_index) {
                    fail("material " + std::to_string(i) + " names textures outside the table");
                    break;
                }
                strings_ok = strings_ok && ReadString(bytes, h.big_endian, h.unicode, name_off, m.name) &&
                             ReadString(bytes, h.big_endian, h.unicode, mtd_off, m.mtd);
                materials.push_back(std::move(m));
            }
            const std::uint64_t textures_at = layouts_at + 1ull * kLayoutBytes * h.buffer_layout_count;
            for (std::uint32_t i = 0; ok && i < h.texture_count; ++i) {
                const std::uint64_t at = textures_at + 1ull * kTextureBytes * i;
                FlverTexture t;
                std::uint32_t path_off = 0, type_off = 0;
                if (!ReadU32(v, at, path_off) || !ReadU32(v, at + 4, type_off)) {
                    fail("texture table runs past the file");
                    break;
                }
                strings_ok = strings_ok && ReadString(bytes, h.big_endian, h.unicode, path_off, t.path) &&
                             ReadString(bytes, h.big_endian, h.unicode, type_off, t.type);
                textures.push_back(std::move(t));
            }
        }
        if (ok) {
            body.materials = std::move(materials);
            body.textures = std::move(textures);
            body.strings_ok = strings_ok;
            body.face_set_stride = stride;
            body.face_sets = std::move(face_sets);
            body.vertex_buffers = std::move(buffers);
            body.layouts = std::move(layouts);
            return body;
        }
    }
    return BodyFail(error, "tables do not validate (" + last_problem + ")");
}

namespace {

constexpr std::uint32_t kSemanticPosition = 0;
constexpr std::uint32_t kTypeFloat3 = 2;

// Appends the triangles of one face set to `out` (indices rebased by `base`). Counts indices that
// point past the vertex buffer instead of emitting those triangles.
void AppendTriangles(const std::vector<std::uint32_t>& raw, bool strip, std::uint32_t vertex_count,
                     std::uint32_t restart, std::uint32_t base, std::vector<std::uint32_t>& out,
                     std::uint64_t& bad_indices) {
    const auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count) {
            ++bad_indices;
            return;
        }
        if (a == b || b == c || a == c) return; // degenerate (strip joins)
        out.push_back(base + a);
        out.push_back(base + b);
        out.push_back(base + c);
    };
    if (!strip) {
        for (std::size_t i = 0; i + 2 < raw.size(); i += 3) emit(raw[i], raw[i + 1], raw[i + 2]);
        return;
    }
    std::size_t run = 0; // position inside the current strip, for the winding parity
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == restart) {
            run = 0;
            continue;
        }
        if (run >= 2) {
            const std::uint32_t a = raw[i - 2], b = raw[i - 1], c = raw[i];
            if (run % 2 == 0) emit(a, b, c);
            else emit(b, a, c);
        }
        ++run;
    }
}

} // namespace

std::optional<FlverGeometry> DecodeFlverGeometry(std::span<const std::byte> bytes,
                                                 const FlverHeader& h, const FlverBody& body,
                                                 std::string* error) {
    io::ByteView v(bytes, h.big_endian);
    FlverGeometry g;
    const bool wide = h.vertex_index_bits == 32;
    if (h.vertex_index_bits != 16 && !wide) {
        return BodyFail(error, "unsupported index size " + std::to_string(h.vertex_index_bits));
    }
    const std::uint32_t restart = wide ? 0xFFFFFFFFu : 0xFFFFu;

    for (std::size_t mi = 0; mi < body.meshes.size(); ++mi) {
        const FlverMesh& mesh = body.meshes[mi];
        const FlverVertexBuffer* vb = nullptr;
        const FlverMember* position = nullptr;
        for (const std::uint32_t bi : mesh.vertex_buffers) {
            const FlverVertexBuffer& cand = body.vertex_buffers[bi];
            for (const FlverMember& m : body.layouts[cand.layout_index]) {
                if (m.semantic == kSemanticPosition) {
                    vb = &cand;
                    position = &m;
                    break;
                }
            }
            if (vb) break;
        }
        if (!vb) {
            ++g.meshes_without_positions;
            continue;
        }
        if (position->type != kTypeFloat3 || position->struct_offset + 12 > vb->vertex_size) {
            return BodyFail(error, "mesh " + std::to_string(mi) + ": position member has type " +
                                       std::to_string(position->type) + " at offset " +
                                       std::to_string(position->struct_offset) + " (vertex size " +
                                       std::to_string(vb->vertex_size) + ")");
        }
        // Pick the main level of detail: flags == 0, else the first face set.
        const FlverFaceSet* fs = nullptr;
        for (const std::uint32_t fi : mesh.face_sets) {
            if (body.face_sets[fi].flags == 0) { fs = &body.face_sets[fi]; break; }
        }
        if (!fs && !mesh.face_sets.empty()) fs = &body.face_sets[mesh.face_sets.front()];
        if (!fs) {
            ++g.meshes_without_faces;
            continue;
        }

        const FlverMember* uv_member = nullptr;
        for (const FlverMember& m : body.layouts[vb->layout_index]) {
            if (m.semantic == 5 && m.index == 0 && (m.type == 21 || m.type == 22) &&
                m.struct_offset + 4 <= vb->vertex_size) {
                uv_member = &m;
                break;
            }
        }
        const auto base = static_cast<std::uint32_t>(g.positions.size() / 3);
        const std::uint64_t first = static_cast<std::uint64_t>(h.data_offset) + vb->buffer_offset;
        for (std::uint32_t i = 0; i < vb->vertex_count; ++i) {
            float p[3];
            for (int k = 0; k < 3; ++k) {
                if (!ReadFloat(v, first + 1ull * i * vb->vertex_size + position->struct_offset + 4 * k,
                               p[k])) {
                    return BodyFail(error, "vertex data runs past the file");
                }
            }
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
                ++g.non_finite_vertices;
                p[0] = p[1] = p[2] = 0.0f;
            }
            float uv[2] = {0.0f, 0.0f};
            if (uv_member) {
                std::int16_t raw[2] = {0, 0};
                const std::uint64_t at = first + 1ull * i * vb->vertex_size + uv_member->struct_offset;
                if (!v.Read(at, raw[0]) || !v.Read(at + 2, raw[1])) {
                    return BodyFail(error, "vertex data runs past the file");
                }
                uv[0] = static_cast<float>(raw[0]) / 1024.0f;
                uv[1] = static_cast<float>(raw[1]) / 1024.0f;
                if (g.meshes_with_uvs == 0 && g.uvs.empty()) {
                    g.uv_min[0] = g.uv_max[0] = uv[0];
                    g.uv_min[1] = g.uv_max[1] = uv[1];
                }
                for (int k = 0; k < 2; ++k) {
                    g.uv_min[k] = std::min(g.uv_min[k], uv[k]);
                    g.uv_max[k] = std::max(g.uv_max[k], uv[k]);
                }
            }
            g.uvs.push_back(uv[0]);
            g.uvs.push_back(uv[1]);
            for (int k = 0; k < 3; ++k) {
                g.positions.push_back(p[k]);
                const float slack = 0.01f * (h.bounds_max[k] - h.bounds_min[k]) + 1e-3f;
                if (p[k] < h.bounds_min[k] - slack || p[k] > h.bounds_max[k] + slack) {
                    ++g.outside_bounds_components;
                }
            }
        }
        std::vector<std::uint32_t> raw;
        raw.reserve(fs->index_count);
        const std::uint64_t idx_at = static_cast<std::uint64_t>(h.data_offset) + fs->index_offset;
        for (std::uint32_t i = 0; i < fs->index_count; ++i) {
            std::uint32_t value = 0;
            bool ok = false;
            if (wide) {
                ok = v.Read(idx_at + 4ull * i, value);
            } else {
                std::uint16_t narrow = 0;
                ok = v.Read(idx_at + 2ull * i, narrow);
                value = narrow;
            }
            if (!ok) return BodyFail(error, "index data runs past the file");
            raw.push_back(value);
        }
        const auto range_first = static_cast<std::uint32_t>(g.indices.size());
        AppendTriangles(raw, fs->triangle_strip, vb->vertex_count, restart, base, g.indices,
                        g.bad_indices);
        g.ranges.push_back({range_first, static_cast<std::uint32_t>(g.indices.size()) - range_first,
                            mesh.material_index});
        if (uv_member) ++g.meshes_with_uvs;
        (fs->triangle_strip ? g.strip_sets : g.list_sets) += 1;
        ++g.meshes_decoded;
    }
    g.vertex_count = static_cast<std::uint32_t>(g.positions.size() / 3);
    return g;
}

} // namespace ember::formats
