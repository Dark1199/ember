// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "formats/tpf.h"
#include "vfs/bhd5.h"

namespace ember::vfs {

/// One ".tpf" entry found inside a split bundle (BHF3 header + BDF3 data, both stored in the same
/// archive; the pairing is done by size and confirmed by finding "TPF\0" at the entry's offset).
struct TpfProbe {
    std::string bundle_entry_name;     ///< the name stored in the BHF3 entry
    const Bhd5Entry* data_entry = nullptr; ///< the BDF3 file inside the archive
    std::uint64_t offset = 0;          ///< of the TPF inside the BDF3
    std::uint64_t size = 0;
    std::optional<formats::TpfIndex> index; ///< parsed header, entries and names
    std::string error;                 ///< why `index` is empty
    std::vector<std::byte> prefix;     ///< the bytes that were read (header, entries, names, maybe more)
    bool dcx_wrapped = false;          ///< stored as ".tpf.dcx": `prefix` then holds the WHOLE decoded TPF
                                       ///< and texture offsets are relative to it, not to the BDF3
};

struct TpfScanStats {
    std::size_t bhf3_headers = 0;
    std::size_t bhf3_unparsed = 0;
    std::size_t bhf3_unpaired = 0;
    std::size_t entries = 0;       ///< entries in all paired BHF3 headers
    std::size_t tpf_entries = 0;   ///< those named "*.tpf"
    std::size_t tpf_parsed = 0;
    std::size_t tpf_failed = 0;
    std::map<std::string, std::size_t> extensions; ///< entry name extensions, for diagnosis
    std::string first_names;       ///< a few entry names verbatim, for diagnosis
    bool out_of_time = false;
};

/// Visits the TPF files of every paired split bundle in `archive` until `visit` returns false,
/// `max_tpfs` were visited, or `budget_seconds` ran out. Only the first `prefix_bytes` of each TPF
/// are read.
TpfScanStats ScanSplitTpfs(const Bhd5Archive& archive, double budget_seconds, std::size_t max_tpfs,
                           std::uint64_t prefix_bytes,
                           const std::function<bool(const TpfProbe&)>& visit);

} // namespace ember::vfs
