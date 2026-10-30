// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/model_loader.h"

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "formats/flver.h"
#include "vfs/bnd.h"
#include "vfs/dcx.h"
#include "vfs/tpf_scan.h"
#include "vfs/vfs.h"

namespace ember::app {

std::string TextureKey(const std::string& path) {
    std::string name = path;
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    const auto dot = name.rfind('.');
    if (dot != std::string::npos) name.erase(dot);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

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
    model.uvs = std::move(geometry->uvs);
    model.indices = std::move(geometry->indices);
    model.ranges = std::move(geometry->ranges);
    for (const formats::FlverMaterial& m : body->materials) {
        std::string key;
        for (std::uint32_t i = 0; i < m.texture_count; ++i) {
            const formats::FlverTexture& t = body->textures[m.texture_index + i];
            std::string type = t.type;
            for (char& ch : type) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (type.find("diffuse") != std::string::npos) {
                key = TextureKey(t.path);
                break;
            }
        }
        model.material_texture_keys.push_back(std::move(key));
    }
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

namespace ember::app {

std::vector<LoadedTexture> FindTextures(const std::vector<vfs::Bhd5Archive>& archives,
                                        const std::vector<std::string>& wanted_keys,
                                        double time_budget_seconds, std::string* summary) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    std::set<std::string> missing;
    for (const std::string& k : wanted_keys) {
        if (!k.empty()) missing.insert(k);
    }
    const std::size_t wanted = missing.size();
    std::vector<LoadedTexture> found;
    std::size_t tpfs = 0, tpf_failed = 0, decode_failed = 0, unsupported = 0;
    std::string first_problem;
    for (const vfs::Bhd5Archive& archive : archives) {
        if (missing.empty() || elapsed() > time_budget_seconds) break;
        vfs::ScanSplitTpfs(archive, time_budget_seconds - elapsed(), SIZE_MAX, 16384,
                           [&](const vfs::TpfProbe& probe) {
            ++tpfs;
            if (!probe.index) {
                ++tpf_failed;
                return true;
            }
            for (const formats::TpfTexture& t : probe.index->textures) {
                const std::string key = TextureKey(t.name);
                const auto it = missing.find(key);
                if (it == missing.end() || t.size > (24u << 20)) continue;
                const auto bytes = archive.ReadRange(*probe.data_entry, probe.offset + t.offset, t.size);
                std::string err;
                const auto info = bytes ? formats::ParseDds(*bytes, &err) : std::nullopt;
                if (!info) {
                    ++decode_failed;
                    if (first_problem.empty()) first_problem = key + ": " + (bytes ? err : "read failed");
                    missing.erase(it); // do not retry a broken copy
                    continue;
                }
                if (info->format == formats::DdsFormat::Unsupported) {
                    ++unsupported;
                    if (first_problem.empty()) first_problem = key + ": pixel format " + info->format_name;
                    missing.erase(it);
                    continue;
                }
                std::uint32_t skip = 0;
                for (std::uint32_t w = info->width, h = info->height;
                     (w > 1024 || h > 1024) && skip + 1 < info->mip_count; w /= 2, h /= 2) {
                    ++skip;
                }
                auto levels = formats::DecodeDds(*bytes, *info, 12, &err, skip);
                if (!levels) {
                    ++decode_failed;
                    if (first_problem.empty()) first_problem = key + ": " + err;
                } else {
                    found.push_back({key, std::move(*levels)});
                }
                missing.erase(it);
            }
            return !missing.empty() && elapsed() < time_budget_seconds;
        });
    }
    if (summary) {
        char buf[300];
        std::snprintf(buf, sizeof(buf),
                      "%zu texture names wanted, %zu found and decoded, %zu not found; scanned %zu TPF files "
                      "(%zu unreadable), %zu decode failures, %zu unsupported formats; %.2f s",
                      wanted, found.size(), missing.size(), tpfs, tpf_failed, decode_failed, unsupported,
                      elapsed());
        *summary = buf;
        if (!first_problem.empty()) *summary += "; first problem: " + first_problem;
        if (!missing.empty()) {
            *summary += "; first missing:";
            int n = 0;
            for (const std::string& k : missing) {
                if (n++ >= 4) break;
                *summary += " " + k;
            }
        }
    }
    return found;
}

} // namespace ember::app
