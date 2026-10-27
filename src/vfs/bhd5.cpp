// SPDX-License-Identifier: GPL-3.0-or-later
#include "vfs/bhd5.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "io/bytes.h"

namespace ember::vfs {

namespace {

std::string HexPrefix(std::span<const std::byte> bytes, std::size_t count = 32) {
    std::string out;
    const std::size_t n = std::min(count, bytes.size());
    for (std::size_t i = 0; i < n; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x ", static_cast<unsigned>(bytes[i]));
        out += buf;
    }
    return out;
}

bool Fail(std::string* error, std::span<const std::byte> bytes, const std::string& what) {
    if (error) {
        *error = what + " | first bytes: " + HexPrefix(bytes);
    }
    return false;
}

} // namespace

std::uint32_t PathHash(std::string_view path) noexcept {
    // Trim ASCII whitespace.
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) path.remove_prefix(1);
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.remove_suffix(1);

    std::uint32_t hash = 0;
    bool first = true;
    const auto mix = [&hash](char c) {
        hash = hash * 37u + static_cast<std::uint32_t>(static_cast<unsigned char>(c));
    };
    for (const char raw : path) {
        char c = raw == '\\' ? '/' : raw;
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (first && c != '/') {
            mix('/'); // ensure a leading separator
        }
        first = false;
        mix(c);
    }
    if (first) {
        mix('/'); // empty path hashes as "/"
    }
    return hash;
}

std::optional<Bhd5Index> ParseBhd5(std::span<const std::byte> bytes, std::uint64_t data_file_size,
                                   std::string* error) {
    io::ByteView view(bytes);
    if (bytes.size() < 24 || !view.Matches(0, "BHD5", 4)) {
        Fail(error, bytes, "not a BHD5 index (missing magic; the file may be encrypted)");
        return std::nullopt;
    }
    std::uint8_t endian_marker = 0;
    (void)view.Read(4, endian_marker);
    if (endian_marker != 0xFF && endian_marker != 0x00) {
        Fail(error, bytes, "unexpected endian marker");
        return std::nullopt;
    }
    Bhd5Index index;
    index.big_endian = endian_marker == 0x00;
    view.SetBigEndian(index.big_endian);

    std::int32_t version_word = 0;
    std::int32_t file_size = 0;
    std::int32_t bucket_count = 0;
    std::int32_t buckets_offset = 0;
    if (!view.Read(8, version_word) || !view.Read(12, file_size) ||
        !view.Read(16, bucket_count) || !view.Read(20, buckets_offset)) {
        Fail(error, bytes, "truncated header");
        return std::nullopt;
    }
    index.declared_file_size = static_cast<std::uint32_t>(file_size);

    if (bucket_count < 0 || buckets_offset < 0 ||
        static_cast<std::uint64_t>(buckets_offset) + static_cast<std::uint64_t>(bucket_count) * 8 >
            bytes.size()) {
        Fail(error, bytes, "bucket table lies outside the file");
        return std::nullopt;
    }

    for (std::int32_t b = 0; b < bucket_count; ++b) {
        const std::uint64_t bucket_at = static_cast<std::uint64_t>(buckets_offset) + b * 8ull;
        std::int32_t entry_count = 0;
        std::int32_t entries_offset = 0;
        (void)view.Read(bucket_at, entry_count);
        (void)view.Read(bucket_at + 4, entries_offset);
        if (entry_count < 0 || entries_offset < 0 ||
            static_cast<std::uint64_t>(entries_offset) + static_cast<std::uint64_t>(entry_count) *
                                                              16 >
                bytes.size()) {
            Fail(error, bytes, "entry table of bucket " + std::to_string(b) + " is out of range");
            return std::nullopt;
        }
        for (std::int32_t e = 0; e < entry_count; ++e) {
            const std::uint64_t at = static_cast<std::uint64_t>(entries_offset) + e * 16ull;
            Bhd5Entry entry;
            std::int32_t stored_size = 0;
            std::int64_t data_offset = 0;
            (void)view.Read(at, entry.name_hash);
            (void)view.Read(at + 4, stored_size);
            (void)view.Read(at + 8, data_offset);
            if (stored_size < 0 || data_offset < 0 ||
                static_cast<std::uint64_t>(data_offset) + static_cast<std::uint64_t>(stored_size) >
                    data_file_size) {
                ++index.invalid_entries;
                continue;
            }
            entry.stored_size = static_cast<std::uint32_t>(stored_size);
            entry.data_offset = static_cast<std::uint64_t>(data_offset);
            index.entries.push_back(entry);
        }
    }
    if (index.entries.empty() && index.invalid_entries > 0) {
        Fail(error, bytes,
             "every entry points outside the data file (" + std::to_string(index.invalid_entries) +
                 " entries): the layout assumption is probably wrong");
        return std::nullopt;
    }
    std::ranges::stable_sort(index.entries, {}, &Bhd5Entry::name_hash);
    return index;
}

std::optional<Bhd5Archive> Bhd5Archive::Open(const std::filesystem::path& index_path,
                                             const std::filesystem::path& data_path,
                                             std::string* error) {
    std::ifstream index_file(index_path, std::ios::binary | std::ios::ate);
    if (!index_file) {
        if (error) *error = "cannot open " + index_path.string();
        return std::nullopt;
    }
    const std::streamsize index_size = index_file.tellg();
    std::vector<std::byte> index_bytes(static_cast<std::size_t>(std::max<std::streamsize>(index_size, 0)));
    index_file.seekg(0);
    if (index_size > 0 &&
        !index_file.read(reinterpret_cast<char*>(index_bytes.data()), index_size)) {
        if (error) *error = "cannot read " + index_path.string();
        return std::nullopt;
    }

    std::error_code ec;
    const std::uint64_t data_size = std::filesystem::file_size(data_path, ec);
    if (ec) {
        if (error) *error = "cannot stat " + data_path.string() + ": " + ec.message();
        return std::nullopt;
    }
    std::ifstream data_file(data_path, std::ios::binary);
    char magic[4] = {};
    if (!data_file.read(magic, 4) || std::string_view(magic, 4) != "BDF3") {
        if (error) *error = "data file does not start with BDF3: " + data_path.string();
        return std::nullopt;
    }

    std::string parse_error;
    auto index = ParseBhd5(index_bytes, data_size, &parse_error);
    if (!index) {
        if (error) *error = index_path.string() + ": " + parse_error;
        return std::nullopt;
    }
    Bhd5Archive archive;
    archive.index_ = std::move(*index);
    archive.data_path_ = data_path;
    return archive;
}

const Bhd5Entry* Bhd5Archive::Find(std::uint32_t name_hash) const noexcept {
    const auto it = std::ranges::lower_bound(index_.entries, name_hash, {}, &Bhd5Entry::name_hash);
    if (it == index_.entries.end() || it->name_hash != name_hash) {
        return nullptr;
    }
    return &*it;
}

std::optional<std::vector<std::byte>> Bhd5Archive::Read(const Bhd5Entry& entry) const {
    std::ifstream file(data_path_, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    file.seekg(static_cast<std::streamoff>(entry.data_offset));
    std::vector<std::byte> data(entry.stored_size);
    if (entry.stored_size > 0 &&
        !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(entry.stored_size))) {
        return std::nullopt;
    }
    return data;
}

} // namespace ember::vfs
