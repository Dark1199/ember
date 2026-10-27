// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vfs/bhd5.h"
#include "vfs/dcx.h"

namespace ember::vfs {

class BndArchive;

/// Read-only virtual file system over user-supplied game directories.
///
/// The original game files are Windows-era: they were authored with case-insensitive paths and
/// backslash separators. Android and Linux file systems are case-sensitive, so every lookup goes
/// through a normalized key (lowercase ASCII, '/' separators). Mounts are searched newest-first,
/// so a later mount (a patch or mod directory) overrides an earlier one, the same way OpenMW
/// orders its data directories.
///
/// The engine never copies or caches game data anywhere except process memory.
class Vfs {
public:
    /// Indexes every regular file below `root`. Returns false (and fills `error`) if the
    /// directory cannot be read.
    bool MountDirectory(const std::filesystem::path& root, std::string* error = nullptr);

    /// Mounts one index/data archive pair. Packed files are looked up by path hash; loose files
    /// always win over packed ones, and later-mounted archives win over earlier ones.
    bool MountArchive(const std::filesystem::path& index_path,
                      const std::filesystem::path& data_path, std::string* error = nullptr);

    /// Mounts every ".bhd5" / ".bhd" file found among the loose files that has a ".bdt" with the
    /// same name next to it (names are matched case-insensitively). Returns how many mounted;
    /// the reason for each failure is appended to `errors`.
    std::size_t MountArchivesFromIndex(std::vector<std::string>* errors = nullptr);

    [[nodiscard]] std::size_t ArchiveCount() const noexcept {
        return archives_.size();
    }
    [[nodiscard]] const std::vector<Bhd5Archive>& Archives() const noexcept {
        return archives_;
    }
    /// Total number of packed files across all mounted archives (listed by hash only).
    [[nodiscard]] std::size_t PackedFileCount() const noexcept;

    [[nodiscard]] bool Exists(std::string_view path) const;
    [[nodiscard]] std::optional<std::uintmax_t> Size(std::string_view path) const;
    [[nodiscard]] std::optional<std::vector<std::byte>> ReadAll(std::string_view path) const;

    /// Like ReadAll, but a file wrapped in a compressed container (DCX) is decompressed first.
    /// Files that are not wrapped come back unchanged. A wrapped file that fails to decompress
    /// yields nullopt and the reason in `error`.
    [[nodiscard]] std::optional<std::vector<std::byte>> ReadAllDecoded(
        std::string_view path, std::string* error = nullptr) const;

    /// Reads `path` (decompressing a DCX wrapper if present) and opens it as a BND3/BND4 bundle.
    /// The returned archive owns the one decoded buffer; entries are viewed in place. Hold on to
    /// it while reading several entries instead of calling this repeatedly.
    [[nodiscard]] std::shared_ptr<const BndArchive> OpenBnd(std::string_view path,
                                                            std::string* error = nullptr) const;

    /// Normalized paths whose key starts with `prefix` (itself normalized), sorted.
    [[nodiscard]] std::vector<std::string> List(std::string_view prefix = {}) const;

    /// Distinct first path components (top-level folders and files), sorted.
    [[nodiscard]] std::vector<std::string> TopLevel() const;

    [[nodiscard]] std::size_t FileCount() const noexcept {
        return index_.size();
    }

    /// Lowercases ASCII, converts '\\' to '/', drops "./", repeated and leading separators.
    [[nodiscard]] static std::string Normalize(std::string_view path);

private:
    struct Entry {
        std::filesystem::path real_path;
        std::uintmax_t size = 0;
    };
    [[nodiscard]] std::pair<const Bhd5Archive*, const Bhd5Entry*> FindPacked(
        std::string_view path) const noexcept;

    std::unordered_map<std::string, Entry> index_;
    std::vector<Bhd5Archive> archives_; ///< mount order; searched newest first
};

} // namespace ember::vfs
