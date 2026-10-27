// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ember::vfs {

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

    [[nodiscard]] bool Exists(std::string_view path) const;
    [[nodiscard]] std::optional<std::uintmax_t> Size(std::string_view path) const;
    [[nodiscard]] std::optional<std::vector<std::byte>> ReadAll(std::string_view path) const;

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
    std::unordered_map<std::string, Entry> index_;
};

} // namespace ember::vfs
