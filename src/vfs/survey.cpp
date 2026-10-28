// SPDX-License-Identifier: GPL-3.0-or-later
#include "vfs/survey.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>

#include "vfs/bnd.h"
#include "vfs/dcx.h"

namespace ember::vfs {
namespace {

constexpr std::size_t kDcxSampleLimit = 24;
constexpr std::size_t kBndSampleLimit = 12;
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
    std::vector<const Bhd5Entry*> dcx_candidates, bnd_candidates;
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
            }
        }
    }
    out.push_back(tag + "first 4 bytes of entries: " + TopList(magics, kTopMagics) +
                  (shorter_than_magic ? "  (" + std::to_string(shorter_than_magic) + " shorter than 4 bytes)"
                                      : ""));

    std::uint64_t budget = kSampleByteBudget;
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
