// SPDX-License-Identifier: GPL-3.0-or-later
#include "vfs/tpf_scan.h"

#include <chrono>
#include <fstream>

#include "vfs/bnd.h"
#include "vfs/dcx.h"
#include "vfs/vfs.h"

namespace ember::vfs {
namespace {

std::string Extension(const std::string& name) {
    const std::string n = Vfs::Normalize(name);
    const auto dot = n.rfind('.');
    const auto slash = n.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "(none)";
    return n.substr(dot);
}

} // namespace

TpfScanStats ScanSplitTpfs(const Bhd5Archive& archive, double budget_seconds, std::size_t max_tpfs,
                           std::uint64_t prefix_bytes,
                           const std::function<bool(const TpfProbe&)>& visit) {
    const auto started = std::chrono::steady_clock::now();
    const auto out_of_time = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >
               budget_seconds;
    };
    TpfScanStats stats;
    std::ifstream file(archive.DataPath(), std::ios::binary);
    if (!file) return stats;

    // One pass over the index: classify entries by their first four bytes.
    std::vector<const Bhd5Entry*> headers, data_files;
    for (const Bhd5Entry& e : archive.Index().entries) {
        if (e.stored_size < 16 || e.stored_size > (64u << 20) + (1u << 30)) continue;
        char magic[4] = {};
        file.clear();
        file.seekg(static_cast<std::streamoff>(e.data_offset));
        if (!file.read(magic, 4)) continue;
        if (std::string_view(magic, 4) == "BHF3" && e.stored_size <= (8u << 20)) headers.push_back(&e);
        else if (std::string_view(magic, 4) == "BDF3") data_files.push_back(&e);
    }

    std::size_t visited = 0;
    for (const Bhd5Entry* header_entry : headers) {
        if (out_of_time()) {
            stats.out_of_time = true;
            break;
        }
        const auto header = archive.Read(*header_entry);
        if (!header) continue;
        ++stats.bhf3_headers;
        std::string err;
        const auto index = ParseBhf3(*header, std::nullopt, &err);
        if (!index) {
            ++stats.bhf3_unparsed;
            continue;
        }
        std::vector<const BndEntry*> tpfs;
        for (const BndEntry& e : index->entries) {
            ++stats.entries;
            ++stats.extensions[Extension(e.name)];
            if (stats.first_names.size() < 400) stats.first_names += e.name + " | ";
            const std::string n = Vfs::Normalize(e.name);
            if (n.ends_with(".tpf") || n.ends_with(".tpf.dcx")) tpfs.push_back(&e);
        }
        if (tpfs.empty()) continue;
        // Candidate data files: size within [extent, extent + 64]; the first TPF's magic decides.
        const Bhd5Entry* data_entry = nullptr;
        for (const Bhd5Entry* d : data_files) {
            if (d->stored_size < index->data_extent || d->stored_size > index->data_extent + 64) continue;
            const auto probe = archive.ReadRange(*d, tpfs.front()->offset, 4);
            const std::string_view want = Vfs::Normalize(tpfs.front()->name).ends_with(".dcx")
                                              ? std::string_view("DCX\0", 4) : std::string_view("TPF\0", 4);
            if (probe && probe->size() == 4 && std::string_view(reinterpret_cast<const char*>(probe->data()), 4) == want) {
                data_entry = d;
                break;
            }
        }
        if (!data_entry) {
            ++stats.bhf3_unpaired;
            continue;
        }
        for (const BndEntry* t : tpfs) {
            if (visited >= max_tpfs || out_of_time()) {
                stats.out_of_time = stats.out_of_time || out_of_time();
                return stats;
            }
            ++stats.tpf_entries;
            TpfProbe probe;
            probe.bundle_entry_name = t->name;
            probe.data_entry = data_entry;
            probe.offset = t->offset;
            probe.size = t->stored_size;
            probe.dcx_wrapped = Vfs::Normalize(t->name).ends_with(".dcx");
            std::optional<std::vector<std::byte>> prefix;
            if (probe.dcx_wrapped) {
                // The compressed stream must be read whole; the decoded TPF replaces the prefix.
                if (t->stored_size <= (24u << 20)) {
                    if (const auto raw = archive.ReadRange(*data_entry, t->offset, t->stored_size)) {
                        prefix = DecompressDcx(*raw, &probe.error);
                    }
                }
            } else {
                prefix = archive.ReadRange(*data_entry, t->offset, std::min<std::uint64_t>(prefix_bytes, t->stored_size));
            }
            if (!prefix) {
                if (probe.error.empty()) probe.error = "read failed";
                ++stats.tpf_failed;
            } else {
                probe.prefix = std::move(*prefix);
                probe.index = formats::ParseTpf(probe.prefix, probe.dcx_wrapped ? probe.prefix.size() : t->stored_size, &probe.error);
                (probe.index ? stats.tpf_parsed : stats.tpf_failed) += 1;
            }
            ++visited;
            if (!visit(probe)) return stats;
        }
    }
    return stats;
}

} // namespace ember::vfs
