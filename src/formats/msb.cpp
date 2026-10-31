// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats/msb.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <map>

#include "io/bytes.h"

namespace ember::formats {
namespace {

constexpr std::uint32_t kMaxTables = 16;
constexpr std::uint32_t kMaxEntries = 200000;
constexpr std::size_t kMaxString = 512;
constexpr std::size_t kModelHeader = 0x20;
constexpr std::size_t kPartHeader = 0x64;

std::string FirstBytes(std::span<const std::byte> bytes) {
    std::string out;
    char buf[4];
    for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 32); ++i) {
        std::snprintf(buf, sizeof(buf), "%02x ", static_cast<unsigned>(bytes[i]));
        out += buf;
    }
    return out;
}

bool Fail(std::string* error, std::span<const std::byte> bytes, const std::string& why) {
    if (error) {
        *error = "msb: " + why + " (size " + std::to_string(bytes.size()) + ", first bytes: " + FirstBytes(bytes) + ")";
    }
    return false;
}

/// NUL terminated string at an absolute offset; false when it leaves the file or has no end.
bool ReadCString(std::span<const std::byte> bytes, std::uint64_t offset, std::string* out) {
    if (offset >= bytes.size()) return false;
    const std::size_t limit = std::min<std::size_t>(bytes.size() - offset, kMaxString);
    for (std::size_t i = 0; i < limit; ++i) {
        if (bytes[offset + i] == std::byte{0}) {
            out->assign(reinterpret_cast<const char*>(bytes.data() + offset), i);
            return true;
        }
    }
    return false;
}

bool ReadFloat(const io::ByteView& v, std::uint64_t at, float* out) {
    std::uint32_t raw;
    if (!v.Read(at, raw)) return false;
    *out = std::bit_cast<float>(raw);
    return std::isfinite(*out);
}

bool ReadVec3(const io::ByteView& v, std::uint64_t at, float out[3]) {
    return ReadFloat(v, at, &out[0]) && ReadFloat(v, at + 4, &out[1]) && ReadFloat(v, at + 8, &out[2]);
}

/// Name or sib string stored at `entry + relative`. Offsets are signed in the file; negative or
/// out-of-range values are rejected.
bool ReadRelativeString(std::span<const std::byte> bytes, std::uint64_t entry, std::int32_t relative, std::string* out) {
    if (relative < 0) return false;
    return ReadCString(bytes, entry + static_cast<std::uint64_t>(relative), out);
}

bool ParseModel(std::span<const std::byte> bytes, const io::ByteView& v, std::uint64_t at, MsbModel* out,
                std::string* why) {
    if (bytes.size() < kModelHeader || at > bytes.size() - kModelHeader) {
        *why = "model entry runs past the end of the file";
        return false;
    }
    std::int32_t name_off = 0, sib_off = 0;
    std::uint32_t type = 0, id = 0, instances = 0;
    if (!v.Read(at, name_off) || !v.Read(at + 4, type) || !v.Read(at + 8, id) || !v.Read(at + 12, sib_off) ||
        !v.Read(at + 16, instances)) {
        *why = "model entry unreadable";
        return false;
    }
    if (!ReadRelativeString(bytes, at, name_off, &out->name) || out->name.empty()) {
        *why = "model name offset invalid";
        return false;
    }
    // The sib path is informational: tolerate a bad one rather than rejecting the model.
    if (!ReadRelativeString(bytes, at, sib_off, &out->sib_path)) out->sib_path.clear();
    out->type = type;
    out->id = id;
    out->instance_count = instances;
    return true;
}

bool ParsePart(std::span<const std::byte> bytes, const io::ByteView& v, std::uint64_t at, MsbPart* out,
               std::string* why) {
    if (bytes.size() < kPartHeader || at > bytes.size() - kPartHeader) {
        *why = "part entry runs past the end of the file";
        return false;
    }
    std::int32_t name_off = 0, sib_off = 0;
    if (!v.Read(at, name_off) || !v.Read(at + 4, out->type) || !v.Read(at + 8, out->id) ||
        !v.Read(at + 12, out->model_index) || !v.Read(at + 16, sib_off)) {
        *why = "part entry unreadable";
        return false;
    }
    if (!ReadRelativeString(bytes, at, name_off, &out->name) || out->name.empty()) {
        *why = "part name offset invalid";
        return false;
    }
    if (!ReadRelativeString(bytes, at, sib_off, &out->sib_path)) out->sib_path.clear();
    if (!ReadVec3(v, at + 0x14, out->position) || !ReadVec3(v, at + 0x20, out->rotation_degrees) ||
        !ReadVec3(v, at + 0x2C, out->scale)) {
        *why = "part transform is not finite";
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!v.Read(at + 0x38 + 4 * i, out->draw_groups[i]) || !v.Read(at + 0x48 + 4 * i, out->disp_groups[i])) {
            *why = "part groups unreadable";
            return false;
        }
    }
    return true;
}

} // namespace

std::uint32_t MsbFile::TableCount(const std::string& name) const noexcept {
    for (const MsbTable& t : tables) {
        if (t.name == name) return t.entry_count;
    }
    return 0;
}

std::optional<MsbFile> ParseMsb(std::span<const std::byte> bytes, std::string* error) {
    io::ByteView v(bytes, /*big_endian=*/false);
    MsbFile msb;
    bool have_models = false, have_parts = false;
    std::uint64_t table_at = 0;

    for (std::uint32_t t = 0; t < kMaxTables; ++t) {
        std::int32_t version = 0, count = 0, name_off = 0;
        if (!v.Read(table_at, version) || !v.Read(table_at + 4, count) || !v.Read(table_at + 8, name_off)) {
            Fail(error, bytes, "table header at " + std::to_string(table_at) + " runs past the end");
            return std::nullopt;
        }
        if (version != 0) {
            Fail(error, bytes, "table at " + std::to_string(table_at) + " has version " + std::to_string(version));
            return std::nullopt;
        }
        if (count < 1 || static_cast<std::uint32_t>(count) > kMaxEntries + 1) {
            Fail(error, bytes, "implausible offset count " + std::to_string(count));
            return std::nullopt;
        }
        const std::uint32_t entries = static_cast<std::uint32_t>(count) - 1;
        const std::uint64_t list_at = table_at + 12;
        const std::uint64_t list_end = list_at + 4ull * static_cast<std::uint64_t>(count);
        if (list_end > bytes.size()) {
            Fail(error, bytes, "offset list of table at " + std::to_string(table_at) + " runs past the end");
            return std::nullopt;
        }
        std::string table_name;
        if (name_off < 0 || !ReadCString(bytes, static_cast<std::uint64_t>(name_off), &table_name)) {
            Fail(error, bytes, "table name offset invalid");
            return std::nullopt;
        }
        std::int32_t next = 0;
        (void)v.Read(list_at + 4ull * entries, next);
        msb.tables.push_back({table_name, entries});

        std::string why;
        if (table_name == "MODEL_PARAM_ST") {
            msb.models.reserve(entries);
            for (std::uint32_t i = 0; i < entries; ++i) {
                std::int32_t off = 0;
                (void)v.Read(list_at + 4ull * i, off);
                MsbModel m;
                if (off < 0 || !ParseModel(bytes, v, static_cast<std::uint64_t>(off), &m, &why)) {
                    Fail(error, bytes, "model " + std::to_string(i) + ": " + (why.empty() ? "bad offset" : why));
                    return std::nullopt;
                }
                msb.models.push_back(std::move(m));
            }
            have_models = true;
        } else if (table_name == "PARTS_PARAM_ST") {
            msb.parts.reserve(entries);
            for (std::uint32_t i = 0; i < entries; ++i) {
                std::int32_t off = 0;
                (void)v.Read(list_at + 4ull * i, off);
                MsbPart p;
                if (off < 0 || !ParsePart(bytes, v, static_cast<std::uint64_t>(off), &p, &why)) {
                    Fail(error, bytes, "part " + std::to_string(i) + ": " + (why.empty() ? "bad offset" : why));
                    return std::nullopt;
                }
                msb.parts.push_back(std::move(p));
            }
            have_parts = true;
        }
        // Event and point tables are only counted.

        // The chain ends at 0 or at/after the end of the file. Anything else must move forward.
        if (next <= 0 || static_cast<std::uint64_t>(next) >= bytes.size()) break;
        if (static_cast<std::uint64_t>(next) <= table_at) {
            Fail(error, bytes, "table chain does not move forward");
            return std::nullopt;
        }
        table_at = static_cast<std::uint64_t>(next);
    }

    if (!have_models || !have_parts) {
        std::string found;
        for (const MsbTable& t : msb.tables) found += t.name + " ";
        Fail(error, bytes, std::string("missing ") + (!have_models ? "MODEL_PARAM_ST" : "PARTS_PARAM_ST") +
                               " (tables found: " + (found.empty() ? "none" : found) + ")");
        return std::nullopt;
    }
    return msb;
}

std::string SummarizeMsb(const MsbFile& msb) {
    std::map<std::uint32_t, std::uint32_t> by_type;
    std::uint32_t unresolved = 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const MsbPart& p : msb.parts) {
        ++by_type[p.type];
        if (!msb.ModelOf(p)) ++unresolved;
        for (int i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], p.position[i]);
            hi[i] = std::max(hi[i], p.position[i]);
        }
    }
    std::string out = "models " + std::to_string(msb.models.size()) + ", parts " + std::to_string(msb.parts.size()) + " (";
    bool first = true;
    for (const auto& [type, n] : by_type) {
        out += (first ? "" : ", ") + std::string("type ") + std::to_string(type) + ": " + std::to_string(n);
        first = false;
    }
    out += "), events " + std::to_string(msb.TableCount("EVENT_PARAM_ST")) + ", points " +
           std::to_string(msb.TableCount("POINT_PARAM_ST")) + ", unresolved model refs " + std::to_string(unresolved);
    if (!msb.parts.empty()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), ", part bounds (%.0f %.0f %.0f)..(%.0f %.0f %.0f)", lo[0], lo[1], lo[2], hi[0],
                      hi[1], hi[2]);
        out += buf;
    }
    return out;
}

} // namespace ember::formats
