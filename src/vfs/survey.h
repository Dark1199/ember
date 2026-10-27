// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "vfs/bhd5.h"

namespace ember::vfs {

/// Cheap structural audit of mounted archives, meant to run once at startup on the user's real
/// files. It reads small samples only and returns human-readable log lines:
///   - per archive: byte order, entry count, entries rejected as out of range, data-file size and
///     header, how many data offsets are 16-byte / 2048-byte aligned;
///   - the most common first four bytes of the stored entries (what the archive really holds);
///   - a bounded sample of DCX entries: decompress, report the method and what came out;
///   - a bounded sample of bundles (BND3/BND4, plain or inside DCX): parse, report entry counts,
///     layout corrections and the first failure with its leading bytes.
/// Nothing from the game data is retained or written anywhere except these summary counts.
[[nodiscard]] std::vector<std::string> SurveyArchives(const std::vector<Bhd5Archive>& archives);

} // namespace ember::vfs
