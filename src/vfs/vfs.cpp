// SPDX-License-Identifier: GPL-3.0-or-later
#include "vfs/vfs.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <system_error>

namespace ember::vfs {

std::string Vfs::Normalize(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (const char raw : path) {
        char c = raw == '\\' ? '/' : raw;
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c == '/' && (out.empty() || out.back() == '/')) {
            continue; // leading or repeated separator
        }
        out.push_back(c);
    }
    // Drop "./" components.
    std::string cleaned;
    cleaned.reserve(out.size());
    std::size_t pos = 0;
    while (pos < out.size()) {
        std::size_t end = out.find('/', pos);
        if (end == std::string::npos) {
            end = out.size();
        }
        const std::string_view part(out.data() + pos, end - pos);
        if (part != ".") {
            if (!cleaned.empty()) {
                cleaned.push_back('/');
            }
            cleaned.append(part);
        }
        pos = end + 1;
    }
    return cleaned;
}

bool Vfs::MountDirectory(const std::filesystem::path& root, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        if (error) {
            *error = "not a readable directory: " + root.string();
        }
        return false;
    }
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (error) {
            *error = "cannot iterate " + root.string() + ": " + ec.message();
        }
        return false;
    }
    for (const std::filesystem::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec) || entry_ec) {
            continue;
        }
        const std::filesystem::path relative = std::filesystem::relative(it->path(), root, entry_ec);
        if (entry_ec) {
            continue;
        }
        const std::uintmax_t size = it->file_size(entry_ec);
        // A later mount replaces earlier entries with the same normalized key.
        index_[Normalize(relative.generic_string())] = Entry{it->path(), entry_ec ? 0 : size};
    }
    return true;
}

bool Vfs::Exists(std::string_view path) const {
    return index_.contains(Normalize(path));
}

std::optional<std::uintmax_t> Vfs::Size(std::string_view path) const {
    const auto it = index_.find(Normalize(path));
    if (it == index_.end()) {
        return std::nullopt;
    }
    return it->second.size;
}

std::optional<std::vector<std::byte>> Vfs::ReadAll(std::string_view path) const {
    const auto it = index_.find(Normalize(path));
    if (it == index_.end()) {
        return std::nullopt;
    }
    std::ifstream file(it->second.real_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamsize size = file.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size)) {
        return std::nullopt;
    }
    return data;
}

std::vector<std::string> Vfs::List(std::string_view prefix) const {
    const std::string key = Normalize(prefix);
    std::vector<std::string> result;
    for (const auto& [path, entry] : index_) {
        if (path.starts_with(key)) {
            result.push_back(path);
        }
    }
    std::ranges::sort(result);
    return result;
}

std::vector<std::string> Vfs::TopLevel() const {
    std::set<std::string> names;
    for (const auto& [path, entry] : index_) {
        names.insert(path.substr(0, path.find('/')));
    }
    return {names.begin(), names.end()};
}

} // namespace ember::vfs
