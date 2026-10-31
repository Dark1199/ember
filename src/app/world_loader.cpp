// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/world_loader.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

namespace ember::app {

namespace {
constexpr float kDegToRad = 3.14159265358979f / 180.0f;
constexpr std::uintmax_t kMaxModelBytes = 24u << 20;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

std::string MapIdFromMsbPath(const std::string& path) {
    std::string name = path;
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    const auto dot = name.find('.');
    if (dot != std::string::npos) name.erase(dot);
    return Lower(name);
}

std::vector<std::string> ModelPathCandidates(const std::string& map_id, const std::string& model_name) {
    const std::string dir = "/map/" + map_id + "/";
    const std::string name = Lower(model_name);
    // Map ids look like "m10_01_00_00"; some model files carry the area as a suffix ("a10").
    std::string area;
    if (map_id.size() >= 3 && map_id[0] == 'm') area = "a" + map_id.substr(1, 2);
    std::vector<std::string> out;
    for (const char* ext : {".flver.dcx", ".flver"}) out.push_back(dir + name + ext);
    if (!area.empty() && !name.ends_with(area)) {
        for (const char* ext : {".flver.dcx", ".flver"}) out.push_back(dir + name + area + ext);
    }
    return out;
}

void TransformPoint(const float position[3], const float rotation_degrees[3], const float scale[3],
                    const float in[3], float out[3]) {
    float x = in[0] * scale[0], y = in[1] * scale[1], z = in[2] * scale[2];
    const float rx = rotation_degrees[0] * kDegToRad, ry = rotation_degrees[1] * kDegToRad,
                rz = rotation_degrees[2] * kDegToRad;
    float c = std::cos(rx), s = std::sin(rx);
    float ny = y * c - z * s, nz = y * s + z * c;
    y = ny;
    z = nz;
    c = std::cos(rz);
    s = std::sin(rz);
    float nx = x * c - y * s;
    ny = x * s + y * c;
    x = nx;
    y = ny;
    c = std::cos(ry);
    s = std::sin(ry);
    nx = x * c + z * s;
    nz = -x * s + z * c;
    out[0] = nx + position[0];
    out[1] = y + position[1];
    out[2] = nz + position[2];
}

std::uint32_t WorldBuilder::MaterialFor(const std::string& key) {
    for (std::uint32_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return i;
    }
    keys_.push_back(key);
    material_indices_.emplace_back();
    return static_cast<std::uint32_t>(keys_.size() - 1);
}

std::size_t WorldBuilder::AddInstance(const LoadedModel& model, const float position[3],
                                      const float rotation_degrees[3], const float scale[3]) {
    const std::size_t vertices = model.positions.size() / 3;
    if (vertices == 0 || model.uvs.size() / 2 != vertices) return 0;
    const auto base = static_cast<std::uint32_t>(positions_.size() / 3);
    positions_.reserve(positions_.size() + model.positions.size());
    for (std::size_t v = 0; v < vertices; ++v) {
        float p[3];
        TransformPoint(position, rotation_degrees, scale, &model.positions[3 * v], p);
        for (int i = 0; i < 3; ++i) {
            positions_.push_back(p[i]);
            min_[i] = std::min(min_[i], p[i]);
            max_[i] = std::max(max_[i], p[i]);
        }
    }
    uvs_.insert(uvs_.end(), model.uvs.begin(), model.uvs.end());
    std::size_t added = 0;
    for (const auto& range : model.ranges) {
        const std::string& key = range.material_index < model.material_texture_keys.size()
                                     ? model.material_texture_keys[range.material_index]
                                     : std::string();
        std::vector<std::uint32_t>& dst = material_indices_[MaterialFor(key)];
        const std::size_t end = std::min<std::size_t>(model.indices.size(),
                                                      std::size_t(range.first_index) + range.index_count);
        for (std::size_t i = range.first_index; i + 3 <= end; i += 3) {
            const std::uint32_t a = model.indices[i], b = model.indices[i + 1], c = model.indices[i + 2];
            if (a >= vertices || b >= vertices || c >= vertices) continue; // defensive: never index past the copy
            dst.push_back(base + a);
            dst.push_back(base + b);
            dst.push_back(base + c);
            ++added;
        }
    }
    triangles_ += added;
    return added;
}

LoadedModel WorldBuilder::Finish(const std::string& description) {
    LoadedModel out;
    out.description = description;
    out.positions = std::move(positions_);
    out.uvs = std::move(uvs_);
    for (std::uint32_t m = 0; m < keys_.size(); ++m) {
        if (material_indices_[m].empty()) continue;
        const auto material = static_cast<std::uint32_t>(out.material_texture_keys.size());
        out.material_texture_keys.push_back(keys_[m]);
        out.ranges.push_back({static_cast<std::uint32_t>(out.indices.size()),
                              static_cast<std::uint32_t>(material_indices_[m].size()), material});
        out.indices.insert(out.indices.end(), material_indices_[m].begin(), material_indices_[m].end());
    }
    for (int i = 0; i < 3; ++i) {
        out.bounds_min[i] = triangles_ ? min_[i] : 0.0f;
        out.bounds_max[i] = triangles_ ? max_[i] : 0.0f;
    }
    positions_.clear();
    uvs_.clear();
    material_indices_.clear();
    keys_.clear();
    triangles_ = 0;
    for (int i = 0; i < 3; ++i) {
        min_[i] = 1e30f;
        max_[i] = -1e30f;
    }
    return out;
}

std::vector<std::string> WorldBuilder::KeysByUse() const {
    std::vector<std::uint32_t> order(keys_.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return material_indices_[a].size() > material_indices_[b].size();
    });
    std::vector<std::string> out;
    for (std::uint32_t i : order) {
        if (!keys_[i].empty() && !material_indices_[i].empty()) out.push_back(keys_[i]);
    }
    return out;
}

std::optional<LoadedWorld> LoadWorld(const vfs::Vfs& vfs, const std::string& msb_path,
                                     const WorldOptions& options, std::string* summary) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    const auto say = [&](const std::string& text) {
        if (summary) *summary = text;
    };
    std::string error;
    const auto bytes = vfs.ReadAllDecoded(msb_path, &error);
    if (!bytes) {
        say(msb_path + ": unreadable: " + error);
        return std::nullopt;
    }
    const auto msb = formats::ParseMsb(*bytes, &error);
    if (!msb) {
        say(error);
        return std::nullopt;
    }
    const std::string map_id = MapIdFromMsbPath(msb_path);

    // Map pieces, nearest the middle of the map first.
    std::vector<const formats::MsbPart*> pieces;
    float centre[3] = {};
    for (const formats::MsbPart& p : msb->parts) {
        if (p.type != static_cast<std::uint32_t>(formats::MsbPartType::MapPiece)) continue;
        pieces.push_back(&p);
        for (int i = 0; i < 3; ++i) centre[i] += p.position[i];
    }
    if (pieces.empty()) {
        say(msb_path + ": no map pieces among " + std::to_string(msb->parts.size()) + " parts");
        return std::nullopt;
    }
    for (float& c : centre) c /= static_cast<float>(pieces.size());
    const auto dist2 = [&](const formats::MsbPart* p) {
        float d = 0;
        for (int i = 0; i < 3; ++i) d += (p->position[i] - centre[i]) * (p->position[i] - centre[i]);
        return d;
    };
    std::stable_sort(pieces.begin(), pieces.end(),
                     [&](const auto* a, const auto* b) { return dist2(a) < dist2(b); });

    struct Cached {
        bool tried = false;
        std::optional<LoadedModel> model;
    };
    std::map<std::uint32_t, Cached> cache;
    std::size_t unresolved_models = 0, decode_failures = 0, dropped_budget = 0, no_model_ref = 0,
                degenerate = 0, out_of_time = 0;
    std::string first_unresolved, first_decode_failure;
    WorldBuilder builder;
    LoadedWorld world;
    float start_sum[3] = {};

    for (const formats::MsbPart* part : pieces) {
        if (elapsed() > options.time_budget_seconds) {
            ++out_of_time;
            continue;
        }
        const formats::MsbModel* ref = msb->ModelOf(*part);
        if (!ref) {
            ++no_model_ref;
            continue;
        }
        Cached& entry = cache[part->model_index];
        if (!entry.tried) {
            entry.tried = true;
            std::string path;
            const auto candidates = ModelPathCandidates(map_id, ref->name);
            for (const std::string& candidate : candidates) {
                if (vfs.Exists(candidate)) {
                    path = candidate;
                    break;
                }
            }
            if (path.empty()) {
                ++unresolved_models;
                if (first_unresolved.empty()) {
                    first_unresolved = ref->name + " (tried";
                    for (const std::string& c : candidates) first_unresolved += " " + c;
                    first_unresolved += ")";
                }
            } else if (const auto size = vfs.Size(path); size && *size > kMaxModelBytes) {
                ++decode_failures;
                if (first_decode_failure.empty()) first_decode_failure = path + ": file too large";
            } else if (const auto file = vfs.ReadAllDecoded(path, &error)) {
                std::string why;
                entry.model = DecodeFlverModel(*file, path, &why);
                if (!entry.model) {
                    ++decode_failures;
                    if (first_decode_failure.empty()) first_decode_failure = path + ": " + why;
                }
            } else {
                ++decode_failures;
                if (first_decode_failure.empty()) first_decode_failure = path + ": " + error;
            }
        }
        if (!entry.model) continue;
        const float* s = part->scale;
        if (std::fabs(s[0]) < 1e-6f || std::fabs(s[1]) < 1e-6f || std::fabs(s[2]) < 1e-6f) {
            ++degenerate;
            continue;
        }
        if (builder.Triangles() + entry.model->indices.size() / 3 > options.max_triangles) {
            ++dropped_budget;
            continue;
        }
        if (builder.AddInstance(*entry.model, part->position, part->rotation_degrees, part->scale) > 0) {
            ++world.instances;
            for (int i = 0; i < 3; ++i) start_sum[i] += part->position[i];
        }
    }

    std::size_t resolved = 0;
    for (const auto& [index, c] : cache) resolved += c.model ? 1 : 0;
    char counters[420];
    std::snprintf(counters, sizeof(counters),
                  "%s: %zu map pieces, %zu distinct models (%zu decoded, %zu unresolved, %zu decode failures), "
                  "%zu bad model refs; placed %zu instances, %zu triangles, %zu skipped over the %zu triangle "
                  "budget, %zu degenerate scale, %zu not reached in %.0f s; %.2f s",
                  map_id.c_str(), pieces.size(), cache.size(), resolved, unresolved_models, decode_failures,
                  no_model_ref, world.instances, builder.Triangles(), dropped_budget, options.max_triangles,
                  degenerate, options.time_budget_seconds, out_of_time, elapsed());
    std::string text = counters;
    if (!first_unresolved.empty()) text += "; first unresolved: " + first_unresolved;
    if (!first_decode_failure.empty()) text += "; first decode failure: " + first_decode_failure;
    say(text);

    if (world.instances == 0) return std::nullopt;
    for (int i = 0; i < 3; ++i) world.start[i] = start_sum[i] / static_cast<float>(world.instances);
    world.hot_textures = builder.KeysByUse();
    world.model = builder.Finish(map_id + ": " + std::to_string(world.instances) + " map pieces");
    return world;
}

} // namespace ember::app
