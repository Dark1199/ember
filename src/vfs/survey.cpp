// SPDX-License-Identifier: GPL-3.0-or-later
#include "vfs/survey.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>

#include "formats/dds.h"
#include "formats/flver.h"
#include "formats/tpf.h"
#include "vfs/tpf_scan.h"
#include "vfs/bnd.h"
#include "vfs/dcx.h"
#include "vfs/vfs.h"

namespace ember::vfs {
namespace {

constexpr std::size_t kDcxSampleLimit = 24;
constexpr std::size_t kBndSampleLimit = 12;
constexpr std::size_t kBxfSampleLimit = 16;
constexpr std::size_t kFlverSampleLimit = 60;
constexpr std::uint32_t kMaxSampleEntryBytes = 16u << 20;
constexpr std::uint64_t kSampleByteBudget = 64ull << 20;
constexpr std::size_t kTopMagics = 8;

std::string Printable(const unsigned char* p, std::size_t n) {
    std::string out;
    bool text = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] == 0 && i > 0) {
            continue; // trailing NUL pads shown as part of the name below
        }
        if (p[i] < 0x20 || p[i] > 0x7E) {
            text = false;
        }
    }
    char buf[8];
    if (text) {
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(p[i] == 0 ? '.' : static_cast<char>(p[i]));
        }
    } else {
        out = "0x";
        for (std::size_t i = 0; i < n; ++i) {
            std::snprintf(buf, sizeof(buf), "%02x", p[i]);
            out += buf;
        }
    }
    return out;
}

std::string Magic(std::span<const std::byte> bytes) {
    unsigned char m[4] = {};
    const std::size_t n = std::min<std::size_t>(4, bytes.size());
    for (std::size_t i = 0; i < n; ++i) {
        m[i] = static_cast<unsigned char>(bytes[i]);
    }
    return Printable(m, 4);
}

std::string TopList(const std::map<std::string, std::size_t>& tally, std::size_t limit) {
    std::vector<std::pair<std::string, std::size_t>> v(tally.begin(), tally.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::string out;
    for (std::size_t i = 0; i < v.size() && i < limit; ++i) {
        out += (i ? "  " : "") + v[i].first + " x" + std::to_string(v[i].second);
    }
    if (v.size() > limit) {
        out += "  (+" + std::to_string(v.size() - limit) + " more kinds)";
    }
    return out.empty() ? "(none)" : out;
}

// DCX compression method tag ("DFLT", ...): four bytes after the "DCP\0" tag whose offset is the
// big-endian u32 at 0x0C. Returns "?" for anything malformed.
std::string DcxMethod(std::span<const std::byte> b) {
    if (b.size() < 0x10) {
        return "?";
    }
    std::uint32_t dcp = 0;
    for (int i = 0; i < 4; ++i) {
        dcp = (dcp << 8) | static_cast<unsigned char>(b[0x0C + i]);
    }
    if (dcp > b.size() || b.size() - dcp < 8) {
        return "?";
    }
    unsigned char m[4];
    for (int i = 0; i < 4; ++i) {
        m[i] = static_cast<unsigned char>(b[dcp + 4 + i]);
    }
    return Printable(m, 4);
}

std::string Size(std::uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

void SurveyOne(const Bhd5Archive& archive, std::vector<std::string>& out) {
    const std::string name = archive.DataPath().filename().string();
    const Bhd5Index& index = archive.Index();
    const std::string tag = "survey " + name + ": ";

    std::ifstream data(archive.DataPath(), std::ios::binary | std::ios::ate);
    if (!data) {
        out.push_back(tag + "cannot reopen the data file");
        return;
    }
    const std::uint64_t data_size = static_cast<std::uint64_t>(data.tellg());
    unsigned char head[16] = {};
    data.seekg(0);
    data.read(reinterpret_cast<char*>(head), sizeof(head));

    std::size_t aligned16 = 0, aligned2048 = 0, shorter_than_magic = 0;
    for (const Bhd5Entry& e : index.entries) {
        aligned16 += (e.data_offset % 16 == 0);
        aligned2048 += (e.data_offset % 2048 == 0);
    }
    const std::size_t n = index.entries.size();
    char line[256];
    std::snprintf(line, sizeof(line),
                  "%sindex %s-endian, %zu entries (%zu rejected as out of range), data %s, "
                  "data header %s, offsets 16-aligned %zu/%zu, 2048-aligned %zu/%zu",
                  tag.c_str(), index.big_endian ? "big" : "little", n, index.invalid_entries,
                  Size(data_size).c_str(), Printable(head, 8).c_str(), aligned16, n, aligned2048, n);
    out.emplace_back(line);

    std::map<std::string, std::size_t> magics;
    std::vector<const Bhd5Entry*> dcx_candidates, bnd_candidates, bhf_candidates;
    std::vector<std::uint32_t> bdf_sizes; // stored sizes of BDF3 data files, for pairing
    for (const Bhd5Entry& e : index.entries) {
        if (e.stored_size < 4) {
            ++shorter_than_magic;
            continue;
        }
        unsigned char m[4] = {};
        data.clear();
        data.seekg(static_cast<std::streamoff>(e.data_offset));
        if (!data.read(reinterpret_cast<char*>(m), 4)) {
            ++magics["<unreadable>"];
            continue;
        }
        const std::string key = Printable(m, 4);
        ++magics[key];
        if (e.stored_size <= kMaxSampleEntryBytes) {
            if (key == "DCX." && dcx_candidates.size() < kDcxSampleLimit) {
                dcx_candidates.push_back(&e);
            } else if ((key == "BND3" || key == "BND4") && bnd_candidates.size() < kBndSampleLimit) {
                bnd_candidates.push_back(&e);
            } else if (key == "BHF3" && bhf_candidates.size() < kBxfSampleLimit) {
                bhf_candidates.push_back(&e);
            }
        }
        if (key == "BDF3") {
            bdf_sizes.push_back(e.stored_size);
        }
    }
    out.push_back(tag + "first 4 bytes of entries: " + TopList(magics, kTopMagics) +
                  (shorter_than_magic ? "  (" + std::to_string(shorter_than_magic) + " shorter than 4 bytes)"
                                      : ""));

    std::uint64_t budget = kSampleByteBudget;
    std::size_t flver_ok = 0, flver_fail = 0;
    std::map<std::string, std::size_t> flver_versions, flver_index_bits, flver_order;
    std::uint32_t flver_min[6], flver_max[6] = {};
    std::fill(std::begin(flver_min), std::end(flver_min), 0xFFFFFFFFu);
    float flver_extent_max = 0.0f;
    std::string first_flver_error, first_flver_dump;
    // Body (tables + geometry) statistics, for models with meshes.
    std::size_t body_models = 0, body_ok = 0, body_fail = 0, tables_fit = 0, geom_ok = 0, geom_fail = 0;
    std::uint64_t g_vertices = 0, g_triangles = 0, g_bad = 0, g_nonfinite = 0, g_outside = 0,
                  g_components = 0;
    std::uint32_t g_strip = 0, g_list = 0;
    std::map<std::string, std::size_t> body_strides, layout_shapes;
    std::string first_body_error, first_body_dump, first_geom_error;
    std::size_t mat_models = 0, mat_strings_bad = 0, mat_total = 0, tex_total = 0, uv_models = 0;
    std::map<std::string, std::size_t> tex_types;
    std::string first_tex_paths;
    float uv_lo = 1e30f, uv_hi = -1e30f;
    const auto check_flver = [&](std::span<const std::byte> bytes) {
        if (flver_ok + flver_fail >= kFlverSampleLimit) {
            return;
        }
        std::string err;
        if (const auto h = formats::ParseFlverHeader(bytes, &err)) {
            ++flver_ok;
            char v[16];
            std::snprintf(v, sizeof(v), "0x%x", h->version);
            ++flver_versions[v];
            ++flver_index_bits[std::to_string(h->vertex_index_bits)];
            ++flver_order[h->big_endian ? "big" : "little"];
            const std::uint32_t values[6] = {h->mesh_count, h->material_count, h->bone_count,
                                             h->face_count, h->vertex_buffer_count, h->texture_count};
            for (int i = 0; i < 6; ++i) {
                flver_min[i] = std::min(flver_min[i], values[i]);
                flver_max[i] = std::max(flver_max[i], values[i]);
            }
            for (int i = 0; i < 3; ++i) {
                flver_extent_max = std::max(flver_extent_max, h->bounds_max[i] - h->bounds_min[i]);
            }
            if (h->mesh_count > 0) {
                ++body_models;
                if (formats::FlverTablesEnd(*h, 0x20) <= h->data_offset) ++tables_fit;
                std::string berr;
                if (const auto body = formats::ParseFlverBody(bytes, *h, &berr)) {
                    ++body_ok;
                    ++body_strides["0x" + [&] { char t[8]; std::snprintf(t, sizeof(t), "%x", body->face_set_stride); return std::string(t); }()];
                    for (const auto& layout : body->layouts) {
                        std::string shape;
                        for (const auto& m : layout) {
                            shape += std::to_string(m.semantic) + ":" + std::to_string(m.type) + "@" +
                                     std::to_string(m.struct_offset) + " ";
                        }
                        ++layout_shapes[shape];
                    }
                    ++mat_models;
                    mat_total += body->materials.size();
                    tex_total += body->textures.size();
                    if (!body->strings_ok) ++mat_strings_bad;
                    for (const auto& t : body->textures) {
                        ++tex_types[t.type];
                        if (first_tex_paths.size() < 330) first_tex_paths += t.type + " = " + t.path + " | ";
                    }
                    std::string gerr;
                    if (const auto g = formats::DecodeFlverGeometry(bytes, *h, *body, &gerr)) {
                        if (g->meshes_with_uvs > 0) {
                            ++uv_models;
                            uv_lo = std::min({uv_lo, g->uv_min[0], g->uv_min[1]});
                            uv_hi = std::max({uv_hi, g->uv_max[0], g->uv_max[1]});
                        }
                        ++geom_ok;
                        g_vertices += g->vertex_count;
                        g_triangles += g->indices.size() / 3;
                        g_bad += g->bad_indices;
                        g_nonfinite += g->non_finite_vertices;
                        g_outside += g->outside_bounds_components;
                        g_components += 3ull * g->vertex_count;
                        g_strip += g->strip_sets;
                        g_list += g->list_sets;
                    } else {
                        ++geom_fail;
                        if (first_geom_error.empty()) first_geom_error = gerr;
                    }
                } else {
                    ++body_fail;
                    if (first_body_error.empty()) {
                        first_body_error = berr;
                        // Table region right after the header: shows the real entry shapes.
                        for (std::size_t i = 0x80; i < std::min<std::size_t>(bytes.size(), 0x80 + 256); ++i) {
                            char t[4];
                            std::snprintf(t, sizeof(t), "%02x ", static_cast<unsigned>(bytes[i]));
                            first_body_dump += t;
                            if ((i - 0x80) % 16 == 15) first_body_dump += "| ";
                        }
                    }
                }
            }
        } else {
            ++flver_fail;
            if (first_flver_error.empty()) {
                first_flver_error = err;
                for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 96); ++i) {
                    char h[4];
                    std::snprintf(h, sizeof(h), "%02x ", static_cast<unsigned>(bytes[i]));
                    first_flver_dump += h;
                    if (i % 16 == 15) first_flver_dump += "| ";
                }
            }
        }
    };
    std::size_t bnd_ok = 0, bnd_fail = 0, bnd_corrected = 0, bnd_entries = 0;
    std::string first_bnd_error, first_bnd_dump;
    std::map<std::string, std::size_t> layouts;
    const auto check_bnd = [&](std::span<const std::byte> bytes) {
        std::string err;
        if (auto idx = ParseBnd(bytes, &err)) {
            ++bnd_ok;
            bnd_corrected += idx->layout_corrected ? 1 : 0;
            bnd_entries += idx->entries.size();
            char key[160];
            std::snprintf(key, sizeof(key),
                          "BND%c flags 0x%02x -> stride %u ids %d names %d comp %d long %d%s",
                          idx->kind == BndKind::Bnd3 ? '3' : '4', idx->format_flags,
                          idx->entry_stride, idx->has_ids, idx->has_names,
                          idx->has_compression_field, idx->long_offsets,
                          idx->kind == BndKind::Bnd3 && !idx->header_end_consistent
                              ? " (names outside declared header end)" : "");
            ++layouts[key];
            for (const BndEntry& e : idx->entries) {
                if (e.name.size() >= 6 && e.stored_size >= 6 && e.offset <= bytes.size() &&
                    e.stored_size <= bytes.size() - e.offset &&
                    Vfs::Normalize(e.name).ends_with(".flver")) {
                    check_flver(bytes.subspan(static_cast<std::size_t>(e.offset),
                                              static_cast<std::size_t>(e.stored_size)));
                }
            }
        } else {
            ++bnd_fail;
            if (first_bnd_error.empty()) {
                first_bnd_error = err;
                // Header plus the first entries, as hex, so the real layout can be read off.
                for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 160); ++i) {
                    char h[4];
                    std::snprintf(h, sizeof(h), "%02x ", static_cast<unsigned>(bytes[i]));
                    first_bnd_dump += h;
                    if (i % 16 == 15) first_bnd_dump += "| ";
                }
            }
        }
    };

    if (!dcx_candidates.empty()) {
        std::size_t ok = 0, fail = 0, sampled = 0;
        std::map<std::string, std::size_t> methods, decoded;
        std::string first_error;
        for (const Bhd5Entry* e : dcx_candidates) {
            if (e->stored_size > budget) {
                break;
            }
            budget -= e->stored_size;
            const auto raw = archive.Read(*e);
            if (!raw) {
                ++fail;
                continue;
            }
            ++sampled;
            ++methods[DcxMethod(*raw)];
            std::string err;
            const auto inflated = DecompressDcx(*raw, &err);
            if (!inflated) {
                ++fail;
                if (first_error.empty()) {
                    first_error = err;
                }
                continue;
            }
            ++ok;
            ++decoded[Magic(*inflated)];
            if (IsBnd(*inflated)) {
                check_bnd(*inflated);
            } else if (formats::IsFlver(*inflated)) {
                check_flver(*inflated);
            }
        }
        out.push_back(tag + "DCX sample " + std::to_string(sampled) + ": ok " + std::to_string(ok) +
                      ", failed " + std::to_string(fail) + "; methods " + TopList(methods, 4) +
                      "; decoded content starts with " + TopList(decoded, 6));
        if (!first_error.empty()) {
            out.push_back(tag + "first DCX failure: " + first_error);
        }
    }
    for (const Bhd5Entry* e : bnd_candidates) {
        if (e->stored_size > budget) {
            break;
        }
        budget -= e->stored_size;
        if (const auto raw = archive.Read(*e)) {
            check_bnd(*raw);
        }
    }
    // Split bundles: BHF3 header files. Their data lives in a BDF3 file whose name is not known
    // here (entries are listed by hash only), so pairing is judged by size: a BDF3 whose stored
    // size is at least the header's furthest entry end, and no more than 64 bytes above it.
    if (!bhf_candidates.empty()) {
        std::size_t ok = 0, fail = 0, paired = 0, entries_total = 0;
        std::map<std::string, std::size_t> bxf_layouts;
        std::string first_error, first_dump;
        for (const Bhd5Entry* e : bhf_candidates) {
            if (e->stored_size > budget) {
                break;
            }
            budget -= e->stored_size;
            const auto raw = archive.Read(*e);
            if (!raw) {
                continue;
            }
            std::string err;
            if (const auto idx = ParseBhf3(*raw, std::nullopt, &err)) {
                ++ok;
                entries_total += idx->entries.size();
                char key[160];
                std::snprintf(key, sizeof(key), "BHF3 flags 0x%02x -> stride %u ids %d names %d comp %d long %d",
                              idx->format_flags, idx->entry_stride, idx->has_ids, idx->has_names,
                              idx->has_compression_field, idx->long_offsets);
                ++bxf_layouts[key];
                for (const std::uint32_t size : bdf_sizes) {
                    if (size >= idx->data_extent && size - idx->data_extent <= 64) {
                        ++paired;
                        break;
                    }
                }
            } else {
                ++fail;
                if (first_error.empty()) {
                    first_error = err;
                    for (std::size_t i = 0; i < std::min<std::size_t>(raw->size(), 160); ++i) {
                        char h[4];
                        std::snprintf(h, sizeof(h), "%02x ", static_cast<unsigned>((*raw)[i]));
                        first_dump += h;
                        if (i % 16 == 15) first_dump += "| ";
                    }
                }
            }
        }
        out.push_back(tag + "BHF3 sample: parsed " + std::to_string(ok) + ", failed " + std::to_string(fail) +
                      ", " + std::to_string(entries_total) + " entries; " + std::to_string(paired) +
                      " have a size-matching BDF3 in this archive (" + std::to_string(bdf_sizes.size()) +
                      " BDF3 files); layouts " + TopList(bxf_layouts, 4));
        if (!first_error.empty()) {
            out.push_back(tag + "first BHF3 failure: " + first_error);
            out.push_back(tag + "first failing BHF3, first 160 bytes (16 per group): " + first_dump);
        }
    }
    if (flver_ok + flver_fail > 0) {
        char range[300];
        std::snprintf(range, sizeof(range),
                      "meshes %u..%u, materials %u..%u, bones %u..%u, faces %u..%u, vertex buffers %u..%u, "
                      "textures %u..%u, largest bounding-box side %.1f",
                      flver_min[0], flver_max[0], flver_min[1], flver_max[1], flver_min[2], flver_max[2],
                      flver_min[3], flver_max[3], flver_min[4], flver_max[4], flver_min[5], flver_max[5],
                      static_cast<double>(flver_extent_max));
        out.push_back(tag + "FLVER header sample (direct or inside bundles): ok " + std::to_string(flver_ok) +
                      ", failed " + std::to_string(flver_fail) + "; versions " + TopList(flver_versions, 4) +
                      "; byte order " + TopList(flver_order, 2) + "; vertex index bits " +
                      TopList(flver_index_bits, 3) + "; " + (flver_ok ? range : "no valid header"));
        if (!first_flver_error.empty()) {
            out.push_back(tag + "first FLVER failure: " + first_flver_error);
            out.push_back(tag + "first failing FLVER, first 96 bytes (16 per group): " + first_flver_dump);
        }
    }
    if (body_models > 0) {
        char geo[400];
        std::snprintf(geo, sizeof(geo),
                      "%zu vertices, %llu triangles, %llu dropped for bad indices, %llu non-finite vertices, "
                      "%llu of %llu position components outside the header box (+1%% slack); "
                      "strip face sets %u, list face sets %u",
                      static_cast<std::size_t>(g_vertices), static_cast<unsigned long long>(g_triangles),
                      static_cast<unsigned long long>(g_bad), static_cast<unsigned long long>(g_nonfinite),
                      static_cast<unsigned long long>(g_outside), static_cast<unsigned long long>(g_components),
                      g_strip, g_list);
        out.push_back(tag + "FLVER body sample (" + std::to_string(body_models) + " models with meshes): table sizes fit before the data block " +
                      std::to_string(tables_fit) + "/" + std::to_string(body_models) + "; tables validated " +
                      std::to_string(body_ok) + ", failed " + std::to_string(body_fail) + "; face-set entry size " +
                      TopList(body_strides, 3) + "; geometry decoded " + std::to_string(geom_ok) + ", failed " +
                      std::to_string(geom_fail) + "; " + geo);
        if (!layout_shapes.empty()) {
            out.push_back(tag + "FLVER vertex layouts (semantic:type@offset): " + TopList(layout_shapes, 6));
        }
        if (!first_body_error.empty()) {
            out.push_back(tag + "first FLVER body failure: " + first_body_error);
            out.push_back(tag + "bytes 0x80..0x180 of that file (16 per group): " + first_body_dump);
        }
        if (!first_geom_error.empty()) {
            out.push_back(tag + "first FLVER geometry failure: " + first_geom_error);
        }
    }
    if (mat_models > 0) {
        char uv[160];
        std::snprintf(uv, sizeof(uv), "%zu models have UVs, range %.2f..%.2f (raw int16 / 1024)", uv_models,
                      static_cast<double>(uv_lo), static_cast<double>(uv_hi));
        out.push_back(tag + "FLVER materials: " + std::to_string(mat_total) + " in " + std::to_string(mat_models) +
                      " models, " + std::to_string(tex_total) + " texture references, " +
                      std::to_string(mat_strings_bad) + " models with unreadable strings; " + uv);
        out.push_back(tag + "FLVER texture slot names: " + TopList(tex_types, 10));
        out.push_back(tag + "FLVER first texture references: " + first_tex_paths);
    }
    {
        // Texture packs inside split bundles: how they are named, and whether the TPF and DDS
        // headers read as described.
        std::map<std::string, std::size_t> tpf_shapes, dds_formats;
        std::string first_tpf_error, first_tpf_dump;
        std::size_t tex_count = 0, dds_ok = 0, dds_bad = 0, big_dim = 0;
        std::uint32_t max_w = 0, max_h = 0, max_mips = 0;
        std::string dds_error;
        const TpfScanStats scan = ScanSplitTpfs(archive, 6.0, 60, 16384, [&](const TpfProbe& probe) {
            if (!probe.index) {
                if (first_tpf_error.empty()) {
                    first_tpf_error = probe.error;
                    for (std::size_t i = 0; i < std::min<std::size_t>(probe.prefix.size(), 96); ++i) {
                        char h[4];
                        std::snprintf(h, sizeof(h), "%02x ", static_cast<unsigned>(probe.prefix[i]));
                        first_tpf_dump += h;
                        if (i % 16 == 15) first_tpf_dump += "| ";
                    }
                }
                return true;
            }
            char shape[80];
            std::snprintf(shape, sizeof(shape), "platform %u flag2 %u encoding %u entry size 0x%x",
                          probe.index->platform, probe.index->flag2, probe.index->encoding,
                          probe.index->entry_stride);
            ++tpf_shapes[shape];
            for (const auto& t : probe.index->textures) {
                ++tex_count;
                std::optional<std::vector<std::byte>> head;
                if (probe.dcx_wrapped) {
                    if (t.offset <= probe.prefix.size()) {
                        head = std::vector<std::byte>(probe.prefix.begin() + t.offset,
                                                      probe.prefix.begin() + std::min<std::size_t>(probe.prefix.size(), t.offset + 128));
                    }
                } else {
                    head = archive.ReadRange(*probe.data_entry, probe.offset + t.offset, 128);
                }
                std::string derr;
                const auto info = head ? formats::ParseDds(*head, &derr, false) : std::nullopt;
                if (!info) {
                    ++dds_bad;
                    if (dds_error.empty()) dds_error = derr.empty() ? "unreadable" : derr;
                    continue;
                }
                ++dds_ok;
                ++dds_formats[info->format_name + (info->format == formats::DdsFormat::Unsupported ? " (unsupported)" : "")];
                max_w = std::max(max_w, info->width);
                max_h = std::max(max_h, info->height);
                max_mips = std::max(max_mips, info->mip_count);
                big_dim += info->width > 1024 || info->height > 1024;
            }
            return true;
        });
        if (scan.bhf3_headers > 0) {
            std::string exts;
            for (const auto& [ext, count] : scan.extensions) exts += ext + " x" + std::to_string(count) + "  ";
            out.push_back(tag + "split bundles: " + std::to_string(scan.bhf3_headers) + " BHF3 read, " +
                          std::to_string(scan.bhf3_unparsed) + " unparsed, " + std::to_string(scan.bhf3_unpaired) +
                          " with TPFs but no data file found, " + std::to_string(scan.entries) + " entries; names end in " + exts);
            out.push_back(tag + "first split-bundle entry names: " + scan.first_names);
            out.push_back(tag + "TPF sample: " + std::to_string(scan.tpf_entries) + " files, parsed " +
                          std::to_string(scan.tpf_parsed) + ", failed " + std::to_string(scan.tpf_failed) +
                          "; shapes " + TopList(tpf_shapes, 4) + "; " + std::to_string(tex_count) + " textures, DDS headers ok " +
                          std::to_string(dds_ok) + ", bad " + std::to_string(dds_bad) + "; pixel formats " +
                          TopList(dds_formats, 6) + "; largest " + std::to_string(max_w) + "x" + std::to_string(max_h) +
                          ", up to " + std::to_string(max_mips) + " mips, " + std::to_string(big_dim) + " above 1024");
            if (!first_tpf_error.empty()) {
                out.push_back(tag + "first TPF failure: " + first_tpf_error);
                out.push_back(tag + "first failing TPF, first 96 bytes: " + first_tpf_dump);
            }
            if (!dds_error.empty()) out.push_back(tag + "first DDS problem: " + dds_error);
        }
    }
    if (bnd_ok + bnd_fail > 0) {
        out.push_back(tag + "BND sample (plain or inside DCX): parsed " + std::to_string(bnd_ok) +
                      ", failed " + std::to_string(bnd_fail) + ", layout-corrected " +
                      std::to_string(bnd_corrected) + ", " + std::to_string(bnd_entries) +
                      " bundle entries in total");
        if (!layouts.empty()) {
            out.push_back(tag + "BND layouts that parsed: " + TopList(layouts, 6));
        }
        if (!first_bnd_error.empty()) {
            out.push_back(tag + "first BND failure: " + first_bnd_error);
            out.push_back(tag + "first failing bundle, first 160 bytes (16 per group): " + first_bnd_dump);
        }
    }
}

} // namespace

bool VerifyArchives(const std::vector<Bhd5Archive>& archives,
                    const std::function<void(const std::string&)>& log) {
    constexpr std::uint32_t kMaxWholeDcxBytes = 256u << 20; // larger DCX entries are only read through
    constexpr std::size_t kChunk = 1u << 20;
    constexpr std::size_t kMaxListedFailures = 10;
    bool all_ok = true;
    for (const Bhd5Archive& archive : archives) {
        const std::string tag = "verify " + archive.DataPath().filename().string() + ": ";
        std::ifstream data(archive.DataPath(), std::ios::binary);
        if (!data) {
            log(tag + "cannot open the data file");
            all_ok = false;
            continue;
        }
        const auto& entries = archive.Index().entries;
        std::size_t read_failed = 0, dcx_ok = 0, dcx_failed = 0, dcx_skipped = 0, listed = 0;
        std::uint64_t bytes = 0;
        std::vector<char> chunk(kChunk);
        std::size_t next_report = 1;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const Bhd5Entry& e = entries[i];
            const auto fail = [&](const char* what, const std::string& detail) {
                if (listed++ < kMaxListedFailures) {
                    char hash[16];
                    std::snprintf(hash, sizeof(hash), "%08x", e.name_hash);
                    log(tag + what + " entry hash " + hash + " at offset " +
                        std::to_string(e.data_offset) + ", " + std::to_string(e.stored_size) +
                        " bytes" + (detail.empty() ? "" : ": " + detail));
                }
            };
            unsigned char magic[4] = {};
            data.clear();
            data.seekg(static_cast<std::streamoff>(e.data_offset));
            const bool is_dcx = e.stored_size >= 4 && data.read(reinterpret_cast<char*>(magic), 4) &&
                                magic[0] == 'D' && magic[1] == 'C' && magic[2] == 'X' && magic[3] == 0;
            if (is_dcx && e.stored_size <= kMaxWholeDcxBytes) {
                const auto raw = archive.Read(e);
                std::string err;
                if (!raw) {
                    ++read_failed;
                    fail("READ FAILED", "");
                } else if (DecompressDcx(*raw, &err)) {
                    ++dcx_ok;
                    bytes += e.stored_size;
                } else {
                    ++dcx_failed;
                    fail("DCX FAILED", err);
                }
            } else {
                if (is_dcx) {
                    ++dcx_skipped; // too large to hold in memory at once: checked as a plain read
                }
                data.clear();
                data.seekg(static_cast<std::streamoff>(e.data_offset));
                std::uint64_t left = e.stored_size;
                bool ok = true;
                while (left > 0 && ok) {
                    const auto n = static_cast<std::streamsize>(std::min<std::uint64_t>(left, kChunk));
                    ok = static_cast<bool>(data.read(chunk.data(), n));
                    left -= static_cast<std::uint64_t>(n);
                }
                if (ok) {
                    bytes += e.stored_size;
                } else {
                    ++read_failed;
                    fail("READ FAILED", "");
                }
            }
            if (i + 1 >= next_report * entries.size() / 4 && next_report < 4) {
                log(tag + std::to_string(i + 1) + "/" + std::to_string(entries.size()) + " entries");
                ++next_report;
            }
        }
        const bool ok = read_failed == 0 && dcx_failed == 0;
        all_ok = all_ok && ok;
        log(tag + (ok ? "OK: " : "PROBLEMS FOUND: ") + std::to_string(entries.size()) + " entries, " +
            Size(bytes) + " read; DCX " + std::to_string(dcx_ok + dcx_failed) + " (ok " +
            std::to_string(dcx_ok) + ", failed " + std::to_string(dcx_failed) + ")" +
            (dcx_skipped ? ", " + std::to_string(dcx_skipped) + " oversized DCX read through only" : "") +
            "; read failures " + std::to_string(read_failed) +
            (listed > kMaxListedFailures
                 ? "; " + std::to_string(listed - kMaxListedFailures) + " more failures not listed"
                 : ""));
    }
    log(all_ok ? "verify: every archive entry read back and every DCX stream decompressed cleanly"
               : "verify: PROBLEMS FOUND (see lines above); re-install the game zip");
    return all_ok;
}

std::vector<std::string> SurveyArchives(const std::vector<Bhd5Archive>& archives) {
    std::vector<std::string> lines;
    for (const Bhd5Archive& archive : archives) {
        SurveyOne(archive, lines);
    }
    return lines;
}

} // namespace ember::vfs
