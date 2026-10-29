// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/model_loader.h"

#include <chrono>
#include <cstdio>
#include <map>

#include "formats/flver.h"
#include "vfs/bnd.h"
#include "vfs/dcx.h"
#include "vfs/vfs.h"

namespace ember::app {
namespace {

constexpr std::uint32_t kMinTriangles = 1500;
constexpr std::uint32_t kMaxTriangles = 80000;
constexpr std::uint32_t kMaxEntryBytes = 8u << 20;

struct Counters {
    std::size_t entries_read = 0, dcx = 0, flver_files = 0, header_fail = 0, body_fail = 0,
                geom_fail = 0, too_small = 0, too_big = 0, unclean = 0;
    std::string first_problem;
};

std::optional<LoadedModel> TryModel(std::span<const std::byte> bytes, const std::string& where,
                                    Counters& c) {
    ++c.flver_files;
    std::string err;
    const auto header = formats::ParseFlverHeader(bytes, &err);
    if (!header || header->mesh_count == 0) {
        if (!header) {
            ++c.header_fail;
            if (c.first_problem.empty()) c.first_problem = err;
        }
        return std::nullopt;
    }
    const auto body = formats::ParseFlverBody(bytes, *header, &err);
    if (!body) {
        ++c.body_fail;
        if (c.first_problem.empty()) c.first_problem = err;
        return std::nullopt;
    }
    auto geometry = formats::DecodeFlverGeometry(bytes, *header, *body, &err);
    if (!geometry) {
        ++c.geom_fail;
        if (c.first_problem.empty()) c.first_problem = err;
        return std::nullopt;
    }
    const std::size_t triangles = geometry->indices.size() / 3;
    if (triangles < kMinTriangles) { ++c.too_small; return std::nullopt; }
    if (triangles > kMaxTriangles) { ++c.too_big; return std::nullopt; }
    const std::uint64_t components = 3ull * geometry->vertex_count;
    if (geometry->bad_indices != 0 || geometry->non_finite_vertices != 0 ||
        geometry->outside_bounds_components * 100 > components) {
        ++c.unclean;
        return std::nullopt;
    }
    LoadedModel model;
    model.description = where + ": " + std::to_string(geometry->meshes_decoded) + " meshes, " +
                        std::to_string(geometry->vertex_count) + " vertices, " +
                        std::to_string(triangles) + " triangles, face-set entry size " +
                        std::to_string(body->face_set_stride);
    model.positions = std::move(geometry->positions);
    model.indices = std::move(geometry->indices);
    for (int i = 0; i < 3; ++i) {
        model.bounds_min[i] = header->bounds_min[i];
        model.bounds_max[i] = header->bounds_max[i];
    }
    return model;
}

} // namespace

std::optional<LoadedModel> FindFirstModel(const std::vector<vfs::Bhd5Archive>& archives,
                                          double time_budget_seconds, std::string* summary) {
    const auto started = std::chrono::steady_clock::now();
    const auto out_of_time = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >
               time_budget_seconds;
    };
    Counters c;
    std::optional<LoadedModel> found;
    for (const vfs::Bhd5Archive& archive : archives) {
        const std::string archive_name = archive.DataPath().filename().string();
        for (const vfs::Bhd5Entry& entry : archive.Index().entries) {
            if (out_of_time() || found) break;
            if (entry.stored_size < 32 || entry.stored_size > kMaxEntryBytes) continue;
            const auto raw = archive.Read(entry);
            if (!raw) continue;
            ++c.entries_read;
            if (!vfs::IsDcx(*raw)) continue;
            ++c.dcx;
            const auto inflated = vfs::DecompressDcx(*raw, nullptr);
            if (!inflated) continue;
            char hash[16];
            std::snprintf(hash, sizeof(hash), "%08x", entry.name_hash);
            const std::string where = archive_name + " entry " + hash;
            if (formats::IsFlver(*inflated)) {
                found = TryModel(*inflated, where, c);
            } else if (vfs::IsBnd(*inflated)) {
                const auto idx = vfs::ParseBnd(*inflated, nullptr);
                if (!idx) continue;
                for (const vfs::BndEntry& e : idx->entries) {
                    if (e.offset > inflated->size() || e.stored_size > inflated->size() - e.offset ||
                        !vfs::Vfs::Normalize(e.name).ends_with(".flver")) {
                        continue;
                    }
                    found = TryModel(std::span<const std::byte>(*inflated).subspan(
                                         static_cast<std::size_t>(e.offset),
                                         static_cast<std::size_t>(e.stored_size)),
                                     where + " / " + e.name, c);
                    if (found) break;
                }
            }
        }
        if (found || out_of_time()) break;
    }
    if (summary) {
        char buf[360];
        std::snprintf(buf, sizeof(buf),
                      "read %zu entries (%zu DCX), %zu FLVER files: header failures %zu, table failures %zu, "
                      "geometry failures %zu, too small %zu, too big %zu, not clean %zu; %.2f s",
                      c.entries_read, c.dcx, c.flver_files, c.header_fail, c.body_fail, c.geom_fail,
                      c.too_small, c.too_big, c.unclean,
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        *summary = buf;
        if (!c.first_problem.empty()) *summary += "; first problem: " + c.first_problem;
    }
    return found;
}

} // namespace ember::app
